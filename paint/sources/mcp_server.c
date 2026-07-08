// MCP command server: a localhost socket that lets an external process drive
// ArmorPaint live (open/new project, add fill layers, export). Added for the
// Claude MCP bridge. Socket IO runs on a background thread; every command is
// executed on the main thread via a per-frame update hook, so all engine/GC
// calls stay single-threaded, exactly like the sys_notify_on_next_frame path
// that args.c uses.

#include "global.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define MCP_DEFAULT_PORT 4519
#define MCP_MAX_QUEUE 256
#define MCP_LINE_MAX 8192

// --- shared state between socket thread and main thread ---------------------

// When true, the brush "down" uniform follows pdirty instead of the real mouse, so
// paint_dab can stamp the brush headlessly. Read by uniforms.c.
bool mcp_paint_active = false;

static pthread_mutex_t mcp_mutex = PTHREAD_MUTEX_INITIALIZER;
static char           *mcp_inbox[MCP_MAX_QUEUE];
static int             mcp_inbox_head = 0;
static int             mcp_inbox_tail = 0;
static volatile int    mcp_client_fd  = -1;
static volatile bool   mcp_running    = false;

static void mcp_inbox_push(const char *line) {
	pthread_mutex_lock(&mcp_mutex);
	int next = (mcp_inbox_tail + 1) % MCP_MAX_QUEUE;
	if (next != mcp_inbox_head) { // drop if full
		mcp_inbox[mcp_inbox_tail] = strdup(line);
		mcp_inbox_tail            = next;
	}
	pthread_mutex_unlock(&mcp_mutex);
}

static char *mcp_inbox_pop(void) {
	char *line = NULL;
	pthread_mutex_lock(&mcp_mutex);
	if (mcp_inbox_head != mcp_inbox_tail) {
		line           = mcp_inbox[mcp_inbox_head];
		mcp_inbox_head = (mcp_inbox_head + 1) % MCP_MAX_QUEUE;
	}
	pthread_mutex_unlock(&mcp_mutex);
	return line;
}

static void mcp_reply(const char *msg) {
	int fd = mcp_client_fd;
	if (fd < 0) {
		return;
	}
	send(fd, msg, strlen(msg), 0);
}

// --- command dispatch (runs on the MAIN thread only) ------------------------

// Split a tab-delimited line in place; returns token count, fills argv.
static int mcp_split(char *line, char **argv, int max) {
	int n     = 0;
	argv[n++] = line;
	for (char *p = line; *p && n < max; ++p) {
		if (*p == '\t') {
			*p        = 0;
			argv[n++] = p + 1;
		}
	}
	return n;
}

static void mcp_cmd_list_layers(void) {
	char buf[MCP_LINE_MAX];
	int  off = snprintf(buf, sizeof(buf), "OK\t[");
	if (g_project != NULL && g_project->_ != NULL && g_project->_->layers != NULL) {
		slot_layer_t_array_t *layers = g_project->_->layers;
		for (int i = 0; i < layers->length; ++i) {
			slot_layer_t *l    = layers->buffer[i];
			const char   *name = (l != NULL && l->name != NULL) ? l->name : "";
			off += snprintf(buf + off, sizeof(buf) - off, "%s{\"index\":%d,\"id\":%d,\"name\":\"%s\"}",
			                i == 0 ? "" : ",", i, l != NULL ? l->id : -1, name);
			if (off > (int)sizeof(buf) - 128) {
				break;
			}
		}
	}
	snprintf(buf + off, sizeof(buf) - off, "]\n");
	mcp_reply(buf);
}

static void mcp_cmd_fill_layer(int argc, char **argv) {
	// fill_layer <colorhex AARRGGBB> <rough> <metal> <occ>
	i32 base_color = (argc > 1) ? (i32)strtoul(argv[1], NULL, 16) : (i32)0xffcccccc;
	f32 rough      = (argc > 2) ? (f32)atof(argv[2]) : 1.0f;
	f32 metal      = (argc > 3) ? (f32)atof(argv[3]) : 0.0f;
	f32 occ        = (argc > 4) ? (f32)atof(argv[4]) : 1.0f;

	slot_layer_t *l = layers_new_layer(true, -1, NULL);
	if (l == NULL) {
		mcp_reply("ERR\tlayer create failed\n");
		return;
	}
	context_set_layer(l);
	slot_layer_clear(l, base_color, NULL, occ, rough, metal);
	slot_layer_to_fill_layer(l);

	char buf[128];
	snprintf(buf, sizeof(buf), "OK\t{\"id\":%d}\n", l->id);
	mcp_reply(buf);
}

static void mcp_apply_texture_format(const char *type) {
	if (strcmp(type, "png") == 0) {
		base_bits_handle->i    = TEXTURE_BITS_BITS8;
		g_context->format_type = TEXTURE_LDR_FORMAT_PNG;
	}
	else if (strcmp(type, "jpg") == 0) {
		base_bits_handle->i    = TEXTURE_BITS_BITS8;
		g_context->format_type = TEXTURE_LDR_FORMAT_JPG;
	}
	else if (strcmp(type, "exr16") == 0) {
		base_bits_handle->i = TEXTURE_BITS_BITS16;
	}
	else if (strcmp(type, "exr32") == 0) {
		base_bits_handle->i = TEXTURE_BITS_BITS32;
	}
}

// Load a named export preset (export_presets/<name>.json) into box_export_preset.
// Mirrors the preset lookup in args.c: scans the preset dir, matches by name, and
// falls back to the first preset if the name is unknown, so we never blob a
// nonexistent file.
static void mcp_load_export_preset(const char *preset) {
	gc_unroot(box_export_files);
	box_export_files = file_read_directory(string("%s%sexport_presets", path_data(), PATH_SEP));
	gc_root(box_export_files);
	for (i32 i = 0; i < box_export_files->length; ++i) {
		char *s                     = box_export_files->buffer[i];
		box_export_files->buffer[i] = substring(s, 0, string_length(s) - 5); // strip .json
	}
	if (box_export_files->length == 0) {
		return;
	}
	char *file = string("export_presets/%s.json", box_export_files->buffer[0]);
	for (i32 i = 0; i < box_export_files->length; ++i) {
		if (string_equals(box_export_files->buffer[i], (char *)preset)) {
			file = string("export_presets/%s.json", box_export_files->buffer[i]);
		}
	}
	buffer_t *blob = data_get_blob(file);
	gc_unroot(box_export_preset);
	box_export_preset = json_parse(sys_buffer_to_string(blob));
	gc_root(box_export_preset);
	data_delete_blob(string("export_presets/%s", file));
}

static void mcp_cmd_export_textures(int argc, char **argv) {
	// export_textures <type> <preset> <dir>
	if (argc < 4) {
		mcp_reply("ERR\tusage: export_textures <type> <preset> <dir>\n");
		return;
	}
	const char *type   = argv[1];
	const char *preset = argv[2];
	char       *dir    = string_copy(argv[3]);
	iron_create_directory(dir); // export writes into an existing folder
	mcp_apply_texture_format(type);
	mcp_load_export_preset(preset);
	g_context->layers_export = EXPORT_MODE_VISIBLE;
	export_texture_run(dir, false);
	mcp_reply("OK\t{\"exported\":true}\n");
}

// Dump the default material node graph as compact JSON, a template a client can
// edit and send back via set_material_json. (default_material.arm is an armpack
// encoding of a ui_node_canvas.)
static void mcp_cmd_get_material_json(void) {
	buffer_t *blob = data_get_blob("default_material.arm");
	if (blob == NULL) {
		mcp_reply("ERR\tdefault_material.arm not found\n");
		return;
	}
	char *json = armpack_decode_to_json(blob);
	data_delete_blob("default_material.arm");
	// Strip raw newlines/CR so the reply stays a single protocol line. JSON string
	// contents escape control chars, so no real payload is altered.
	for (char *p = json; *p; ++p) {
		if (*p == '\n' || *p == '\r') {
			*p = ' ';
		}
	}
	mcp_reply(string("OK\t%s\n", json));
}

// Replace the active material's node graph from a JSON canvas and rebake.
// Uses the same json -> armpack -> armpack_decode path the app uses to load
// default_material.arm, so the decoded struct matches the engine's expectations.
static void mcp_cmd_set_material_json(int argc, char **argv) {
	if (argc < 2) {
		mcp_reply("ERR\tusage: set_material_json <json>\n");
		return;
	}
	if (g_context->material == NULL) {
		mcp_reply("ERR\tno active material (create a project first)\n");
		return;
	}
	buffer_t *b = json_encode_to_armpack(argv[1]);
	if (b == NULL) {
		mcp_reply("ERR\tjson_encode_to_armpack failed\n");
		return;
	}
	ui_node_canvas_t *canvas = armpack_decode(b);
	if (canvas == NULL || canvas->nodes == NULL) {
		mcp_reply("ERR\tcanvas decode failed\n");
		return;
	}
	g_context->material->canvas = canvas;
	make_material_parse_paint_material(true); // rebuild the paint shader from nodes
	util_render_make_material_preview();      // refresh material thumbnail
	layers_update_fill_layers();              // rebake fill layers using this material
	mcp_reply(string("OK\t{\"nodes\":%d}\n", canvas->nodes->length));
}

// --- incremental material graph building ------------------------------------

// Defined later in the unity build (nodes_material.c) and not in functions.h.
ui_node_t *nodes_material_get_node_t(char *node_type);

// Instantiate a material node template directly onto the active material's canvas.
// nodes_material_create_node() targets ui_nodes_get_canvas() (the node-editor view,
// which is a different/empty canvas here), so nodes never reached the material and
// their ids collided. Building onto g_context->material->canvas fixes both.
static ui_node_t *mcp_add_node_to_material(char *type) {
	ui_node_t *tmpl = nodes_material_get_node_t(type);
	if (tmpl == NULL) {
		return NULL;
	}
	ui_node_canvas_t *canvas = g_context->material->canvas;
	ui_node_t        *node   = ui_nodes_make_node(tmpl, g_context->material->nodes, canvas);

	// Assign a deterministic id (max existing + 1) instead of trusting the static
	// ui_node_id counter, which is not reliably synced to this canvas. Keep the
	// node's socket node_id fields in agreement so links resolve.
	int newid = 0;
	for (int i = 0; i < canvas->nodes->length; ++i) {
		if (canvas->nodes->buffer[i]->id >= newid) {
			newid = canvas->nodes->buffer[i]->id + 1;
		}
	}
	node->id = newid;
	for (int i = 0; node->inputs != NULL && i < node->inputs->length; ++i) {
		node->inputs->buffer[i]->node_id = newid;
	}
	for (int i = 0; node->outputs != NULL && i < node->outputs->length; ++i) {
		node->outputs->buffer[i]->node_id = newid;
	}
	any_array_push((any_array_t *)canvas->nodes, node);
	return node;
}

static ui_node_t *mcp_find_node(int id) {
	if (g_context->material == NULL || g_context->material->canvas == NULL) {
		return NULL;
	}
	ui_node_array_t *nodes = g_context->material->canvas->nodes;
	for (int i = 0; i < nodes->length; ++i) {
		if (nodes->buffer[i]->id == id) {
			return nodes->buffer[i];
		}
	}
	return NULL;
}

// Set a socket's default_value from tab args starting at argv[start].
static void mcp_set_socket_values(ui_node_socket_t *sock, int argc, char **argv, int start) {
	int          n   = argc - start;
	f32_array_t *arr = f32_array_create(n > 0 ? n : 1);
	for (int i = 0; i < n; ++i) {
		arr->buffer[i] = (float)atof(argv[start + i]);
	}
	arr->length          = n > 0 ? n : 1;
	sock->default_value  = arr;
}

// Import an image file into the project's texture assets so a TEX_IMAGE node can
// reference it (button[0] indexes g_project->_->assets). Returns the asset index —
// the realism path: use real scanned PBR maps instead of only procedural noise.
// The brush stamps the ACTIVE MATERIAL's output color (make_paint.c out_basecol), not
// a swatch — so set a solid-color material and painting paints that color. Reduces the
// material to its output node with the given base/roughness/metallic, then reparses.
static void mcp_cmd_paint_color(int argc, char **argv) {
	// paint_color <hexRRGGBB|AARRGGBB> [rough] [metal]
	if (argc < 2) {
		mcp_reply("ERR\tusage: paint_color <hexRRGGBB> [rough] [metal]\n");
		return;
	}
	if (g_context->material == NULL || g_context->material->canvas == NULL) {
		mcp_reply("ERR\tno active material\n");
		return;
	}
	ui_node_canvas_t *c   = g_context->material->canvas;
	ui_node_t        *out = NULL;
	for (int i = 0; i < c->nodes->length; ++i) {
		if (string_equals(c->nodes->buffer[i]->type, "OUTPUT_MATERIAL_PBR")) {
			out = c->nodes->buffer[i];
			break;
		}
	}
	if (out == NULL) {
		mcp_reply("ERR\tno output node\n");
		return;
	}
	c->nodes->buffer[0] = out; // reduce to a solid material (output only, no links)
	c->nodes->length    = 1;
	c->links->length    = 0;

	uint32_t hex = (uint32_t)strtoul(argv[1], NULL, 16);
	f32      r   = ((hex >> 16) & 0xff) / 255.0f;
	f32      g   = ((hex >> 8) & 0xff) / 255.0f;
	f32      b   = (hex & 0xff) / 255.0f;
	if (out->inputs != NULL && out->inputs->length > 4) {
		f32_array_t *base = f32_array_create(4);
		base->buffer[0] = r; base->buffer[1] = g; base->buffer[2] = b; base->buffer[3] = 1.0f;
		base->length    = 4;
		out->inputs->buffer[0]->default_value = base;
		if (argc > 2) {
			f32_array_t *rough = f32_array_create(1);
			rough->buffer[0] = (f32)atof(argv[2]); rough->length = 1;
			out->inputs->buffer[3]->default_value = rough;
		}
		if (argc > 3) {
			f32_array_t *metal = f32_array_create(1);
			metal->buffer[0] = (f32)atof(argv[3]); metal->length = 1;
			out->inputs->buffer[4]->default_value = metal;
		}
	}
	make_material_parse_paint_material(true);
	mcp_reply("OK\t{\"color\":true}\n");
}

// Bake mesh-derived data (curvature, occlusion, etc.) into the current layer — a real
// 3D wear/dirt mask (unlike the material GEOMETRY node's screen-space pointiness).
// bake_type: 0 curvature, 3 height, 10 occlusion (see BAKE_TYPE_*).
static void mcp_cmd_bake(int argc, char **argv) {
	if (argc < 2) {
		mcp_reply("ERR\tusage: bake <type: 0 curvature, 10 occlusion, ...>\n");
		return;
	}
	g_context->tool      = TOOL_TYPE_BAKE;
	g_context->bake_type = atoi(argv[1]);
	make_material_parse_paint_material(false); // regen shader as the bake shader
	g_context->pdirty = 2;
	mcp_paint_active  = true;
	mcp_reply(string("OK\t{\"bake\":%d}\n", g_context->bake_type));
}

// Stamp the brush at a UV coordinate on the current paint layer. The per-frame
// render_path_paint_draw() paints whenever pdirty>0, so we set the coords + brush +
// pdirty and mcp_paint_active (which makes the brush "down"); the next rendered frame
// applies it. paint2d = paint straight onto the texture in UV space (no raycast).
static void mcp_cmd_paint_dab(int argc, char **argv) {
	// paint_dab <u> <v> [radius] [opacity]
	if (argc < 3) {
		mcp_reply("ERR\tusage: paint_dab <u> <v> [radius] [opacity]\n");
		return;
	}
	f32 u = (f32)atof(argv[1]);
	f32 v = (f32)atof(argv[2]);
	if (argc > 3) {
		g_context->brush_radius = (f32)atof(argv[3]);
	}
	if (argc > 4) {
		g_context->brush_opacity = (f32)atof(argv[4]);
	}
	// mode: 0 = 3D (screen coords, raycast onto the mesh via the rendered gbuffer;
	// works headlessly since the 3D viewport renders each frame). 1 = 2D (UV space;
	// needs the 2D view render pass). Default 3D.
	g_context->paint2d = (argc > 5) ? (atoi(argv[5]) == 1) : false;
	g_context->tool    = TOOL_TYPE_BRUSH;
	// A single dab: start == end so the stroke segment is a point.
	g_context->paint_vec.x      = u;
	g_context->paint_vec.y      = v;
	g_context->last_paint_vec_x = u;
	g_context->last_paint_vec_y = v;
	g_context->last_paint_x     = u;
	g_context->last_paint_y     = v;
	g_context->pdirty           = 2;
	mcp_paint_active            = true;
	mcp_reply("OK\t{\"dab\":true}\n");
}

// Set the start point of a stroke without painting. The next paint_to draws a
// connected segment from here to the new point (real brushing, not a lone dot).
static void mcp_cmd_paint_move(int argc, char **argv) {
	if (argc < 3) {
		mcp_reply("ERR\tusage: paint_move <u> <v>\n");
		return;
	}
	f32 u                       = (f32)atof(argv[1]);
	f32 v                       = (f32)atof(argv[2]);
	g_context->paint_vec.x      = u; // current pen position (stroke anchor)
	g_context->paint_vec.y      = v;
	g_context->last_paint_vec_x = u;
	g_context->last_paint_vec_y = v;
	g_context->last_paint_x     = u;
	g_context->last_paint_y     = v;
	mcp_reply("OK\t{\"moved\":true}\n");
}

// Continue a stroke: paint a connected segment from the previous pen position to
// (u,v). The paint shader fills the capsule between last and current, so chaining
// paint_to calls makes a continuous brush stroke (change paint_color between them to
// blend colors along the stroke). Call paint_move first to set the start.
static void mcp_cmd_paint_to(int argc, char **argv) {
	// paint_to <u> <v> [radius] [opacity] [mode]
	if (argc < 3) {
		mcp_reply("ERR\tusage: paint_to <u> <v> [radius] [opacity] [mode]\n");
		return;
	}
	if (argc > 3) {
		g_context->brush_radius = (f32)atof(argv[3]);
	}
	if (argc > 4) {
		g_context->brush_opacity = (f32)atof(argv[4]);
	}
	g_context->paint2d = (argc > 5) ? (atoi(argv[5]) == 1) : false;
	g_context->tool    = TOOL_TYPE_BRUSH;
	// previous pen position becomes the segment start
	g_context->last_paint_vec_x = g_context->paint_vec.x;
	g_context->last_paint_vec_y = g_context->paint_vec.y;
	g_context->last_paint_x     = g_context->paint_vec.x;
	g_context->last_paint_y     = g_context->paint_vec.y;
	g_context->paint_vec.x      = (f32)atof(argv[1]); // new pen position
	g_context->paint_vec.y      = (f32)atof(argv[2]);
	g_context->pdirty           = 2;
	mcp_paint_active            = true;
	mcp_reply("OK\t{\"to\":true}\n");
}

// Save ArmorPaint's own lit viewport (the "last" render target = the final beauty
// image on the mesh) to a PNG. This is the fast feedback loop: author a material, grab
// the preview, adjust — no external round-trip. Reads the previous rendered frame.
static void mcp_cmd_screenshot(int argc, char **argv) {
	if (argc < 2) {
		mcp_reply("ERR\tusage: screenshot <path>\n");
		return;
	}
	render_target_t *rt = any_map_get(render_path_render_targets, "last");
	if (rt == NULL || rt->_image == NULL) {
		mcp_reply("ERR\tno viewport render target\n");
		return;
	}
	gpu_texture_t *tex = rt->_image;
	// Blit the viewport into a fresh RGBA32 target (GPU handles the BGRA/format
	// conversion) exactly like viewport_capture_screenshot, then read that.
	gpu_texture_t *shot = gpu_create_render_target(tex->width, tex->height, GPU_TEXTURE_FORMAT_RGBA32);
	draw_begin(shot, false, 0);
	draw_image(tex, 0, 0);
	draw_end();
	buffer_t *pixels = gpu_get_texture_pixels(shot);
	iron_write_png(string_copy(argv[1]), pixels, shot->width, shot->height, 0);
	mcp_reply(string("OK\t{\"w\":%d,\"h\":%d}\n", shot->width, shot->height));
}

static void mcp_cmd_import_texture(int argc, char **argv) {
	if (argc < 2) {
		mcp_reply("ERR\tusage: import_texture <path>\n");
		return;
	}
	import_texture_run(string_copy(argv[1]), false);
	int idx = -1;
	for (i32 i = 0; i < g_project->_->assets->length; ++i) {
		if (string_equals(g_project->_->assets->buffer[i]->file, argv[1])) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		mcp_reply("ERR\timport failed or unsupported format\n");
		return;
	}
	mcp_reply(string("OK\t{\"index\":%d,\"count\":%d}\n", idx, g_project->_->assets->length));
}

static void mcp_cmd_add_node(int argc, char **argv) {
	if (argc < 2) {
		mcp_reply("ERR\tusage: add_node <TYPE>\n");
		return;
	}
	if (g_context->material == NULL) {
		mcp_reply("ERR\tno active material\n");
		return;
	}
	ui_node_t *n = mcp_add_node_to_material(argv[1]);
	if (n == NULL) {
		mcp_reply("ERR\tunknown node type\n");
		return;
	}
	mcp_reply(string("OK\t{\"id\":%d,\"inputs\":%d,\"outputs\":%d}\n", n->id,
	                 n->inputs != NULL ? n->inputs->length : 0,
	                 n->outputs != NULL ? n->outputs->length : 0));
}

static void mcp_cmd_set_socket(int argc, char **argv, bool is_input) {
	// set_input/set_output <node_id> <socket_index> <f0> [f1 ..]
	if (argc < 4) {
		mcp_reply("ERR\tusage: set_input|set_output <node_id> <index> <f..>\n");
		return;
	}
	ui_node_t *n = mcp_find_node(atoi(argv[1]));
	if (n == NULL) {
		mcp_reply("ERR\tnode not found\n");
		return;
	}
	ui_node_socket_array_t *sockets = is_input ? n->inputs : n->outputs;
	int                     idx     = atoi(argv[2]);
	if (sockets == NULL || idx < 0 || idx >= sockets->length) {
		mcp_reply("ERR\tsocket index out of range\n");
		return;
	}
	mcp_set_socket_values(sockets->buffer[idx], argc, argv, 3);
	mcp_reply("OK\t{\"set\":true}\n");
}

static void mcp_cmd_set_button(int argc, char **argv) {
	// set_button <node_id> <button_index> <f0> [f1 ..]
	// Buttons hold node parameters that aren't sockets: enum selections (blend
	// type), bools (invert), and CUSTOM data like a VALTORGB color ramp, whose
	// default_value is N stops of [r,g,b,a,position] (5 floats each).
	if (argc < 4) {
		mcp_reply("ERR\tusage: set_button <node_id> <index> <f..>\n");
		return;
	}
	ui_node_t *n = mcp_find_node(atoi(argv[1]));
	if (n == NULL) {
		mcp_reply("ERR\tnode not found\n");
		return;
	}
	int idx = atoi(argv[2]);
	if (n->buttons == NULL || idx < 0 || idx >= n->buttons->length) {
		mcp_reply("ERR\tbutton index out of range\n");
		return;
	}
	int          cnt = argc - 3;
	f32_array_t *arr = f32_array_create(cnt);
	for (int i = 0; i < cnt; ++i) {
		arr->buffer[i] = (float)atof(argv[3 + i]);
	}
	arr->length                        = cnt;
	n->buttons->buffer[idx]->default_value = arr;
	mcp_reply("OK\t{\"set\":true}\n");
}

static void mcp_cmd_link(int argc, char **argv) {
	// link <from_id> <from_socket> <to_id> <to_socket>
	if (argc < 5) {
		mcp_reply("ERR\tusage: link <from_id> <from_socket> <to_id> <to_socket>\n");
		return;
	}
	if (g_context->material == NULL || g_context->material->canvas == NULL) {
		mcp_reply("ERR\tno active material\n");
		return;
	}
	// The material parser addresses sockets by INDEX into a node's inputs/outputs
	// arrays (parser_material.c: node->outputs->buffer[l->from_socket]), not by
	// socket id, so from_socket/to_socket are the socket indices the client passes.
	ui_node_t *from = mcp_find_node(atoi(argv[1]));
	ui_node_t *to   = mcp_find_node(atoi(argv[3]));
	int        fi   = atoi(argv[2]);
	int        ti   = atoi(argv[4]);
	if (from == NULL || to == NULL || from->outputs == NULL || to->inputs == NULL ||
	    fi < 0 || fi >= from->outputs->length || ti < 0 || ti >= to->inputs->length) {
		mcp_reply("ERR\tbad link node/socket\n");
		return;
	}
	ui_node_link_array_t *links = g_context->material->canvas->links;
	ui_node_link_t       *l     = GC_ALLOC_INIT(ui_node_link_t, {0});
	l->id          = ui_next_link_id(links);
	l->from_id     = from->id;
	l->from_socket = fi;
	l->to_id       = to->id;
	l->to_socket   = ti;
	any_array_push((any_array_t *)links, l);
	mcp_reply(string("OK\t{\"link\":%d}\n", l->id));
}

static void mcp_cmd_clear_material(void) {
	if (g_context->material == NULL || g_context->material->canvas == NULL) {
		mcp_reply("ERR\tno active material\n");
		return;
	}
	// Keep the material output node (it is not an addable template), drop the rest,
	// and clear all links. Caller rebuilds and links into the surviving output.
	ui_node_canvas_t *c   = g_context->material->canvas;
	ui_node_t        *out = NULL;
	for (int i = 0; i < c->nodes->length; ++i) {
		if (string_equals(c->nodes->buffer[i]->type, "OUTPUT_MATERIAL_PBR")) {
			out = c->nodes->buffer[i];
			break;
		}
	}
	c->links->length = 0;
	if (out != NULL) {
		c->nodes->buffer[0] = out;
		c->nodes->length    = 1;
		mcp_reply(string("OK\t{\"cleared\":true,\"output_id\":%d}\n", out->id));
	}
	else {
		c->nodes->length = 0;
		mcp_reply("ERR\tno output node found in material\n");
	}
}

static void mcp_cmd_commit_material(void) {
	make_material_parse_paint_material(true);
	util_render_make_material_preview();
	layers_update_fill_layers();
	mcp_reply("OK\t{\"committed\":true}\n");
}

static void mcp_dispatch(char *line) {
	// strip trailing CR/LF
	size_t len = strlen(line);
	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
		line[--len] = 0;
	}
	if (len == 0) {
		return;
	}

	char *argv[16];
	int   argc = mcp_split(line, argv, 16);
	char *cmd  = argv[0];

	if (strcmp(cmd, "ping") == 0) {
		mcp_reply("OK\tpong\n");
	}
	else if (strcmp(cmd, "new_project") == 0) {
		if (argc > 1) {
			g_context->project_type = atoi(argv[1]);
		}
		project_new(true);
		mcp_reply("OK\t{\"new_project\":true}\n");
	}
	else if (strcmp(cmd, "open_project") == 0 && argc > 1) {
		import_arm_run_project(string_copy(argv[1]));
		mcp_reply("OK\t{\"opened\":true}\n");
	}
	else if (strcmp(cmd, "import_mesh") == 0 && argc > 1) {
		import_asset_run(string_copy(argv[1]), -1, -1, false, false, NULL);
		mcp_reply("OK\t{\"imported\":true}\n");
	}
	else if (strcmp(cmd, "fill_layer") == 0) {
		mcp_cmd_fill_layer(argc, argv);
	}
	else if (strcmp(cmd, "list_layers") == 0) {
		mcp_cmd_list_layers();
	}
	else if (strcmp(cmd, "get_material_json") == 0) {
		mcp_cmd_get_material_json();
	}
	else if (strcmp(cmd, "set_material_json") == 0) {
		mcp_cmd_set_material_json(argc, argv);
	}
	else if (strcmp(cmd, "material_fill_layer") == 0) {
		// Bake the active material's node graph into a new fill layer.
		layers_create_fill_layer(UV_TYPE_UVMAP, mat4_nan(), -1);
		mcp_reply("OK\t{\"fill_layer\":true}\n");
	}
	else if (strcmp(cmd, "screenshot") == 0) {
		mcp_cmd_screenshot(argc, argv);
	}
	else if (strcmp(cmd, "import_texture") == 0) {
		mcp_cmd_import_texture(argc, argv);
	}
	else if (strcmp(cmd, "paint_color") == 0) {
		mcp_cmd_paint_color(argc, argv);
	}
	else if (strcmp(cmd, "bake") == 0) {
		mcp_cmd_bake(argc, argv);
	}
	else if (strcmp(cmd, "paint_dab") == 0) {
		mcp_cmd_paint_dab(argc, argv);
	}
	else if (strcmp(cmd, "paint_move") == 0) {
		mcp_cmd_paint_move(argc, argv);
	}
	else if (strcmp(cmd, "paint_to") == 0) {
		mcp_cmd_paint_to(argc, argv);
	}
	else if (strcmp(cmd, "add_node") == 0) {
		mcp_cmd_add_node(argc, argv);
	}
	else if (strcmp(cmd, "set_input") == 0) {
		mcp_cmd_set_socket(argc, argv, true);
	}
	else if (strcmp(cmd, "set_output") == 0) {
		mcp_cmd_set_socket(argc, argv, false);
	}
	else if (strcmp(cmd, "set_button") == 0) {
		mcp_cmd_set_button(argc, argv);
	}
	else if (strcmp(cmd, "link") == 0) {
		mcp_cmd_link(argc, argv);
	}
	else if (strcmp(cmd, "clear_material") == 0) {
		mcp_cmd_clear_material();
	}
	else if (strcmp(cmd, "commit_material") == 0) {
		mcp_cmd_commit_material();
	}
	else if (strcmp(cmd, "export_textures") == 0) {
		mcp_cmd_export_textures(argc, argv);
	}
	else if (strcmp(cmd, "export_mesh") == 0 && argc > 1) {
		export_mesh_run(string_copy(argv[1]), NULL, true);
		mcp_reply("OK\t{\"exported\":true}\n");
	}
	else if (strcmp(cmd, "export_material") == 0 && argc > 1) {
		export_arm_run_material(string_copy(argv[1]));
		mcp_reply("OK\t{\"exported\":true}\n");
	}
	else {
		mcp_reply("ERR\tunknown command\n");
	}
}

// Per-frame hook: drain queued commands and execute on the main thread.
static void mcp_server_update(void *data) {
	char *line;
	while ((line = mcp_inbox_pop()) != NULL) {
		mcp_dispatch(line);
		free(line);
	}
}

// --- socket thread ----------------------------------------------------------

static void mcp_handle_client(int fd) {
	mcp_client_fd = fd;

	// Growable line accumulator: a single request line (e.g. a material node graph
	// as JSON) can be far larger than any fixed buffer.
	size_t cap     = 8192;
	char  *acc     = malloc(cap);
	size_t acc_len = 0;
	char   rbuf[4096];

	for (;;) {
		ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
		if (n <= 0) {
			break;
		}
		for (ssize_t i = 0; i < n; ++i) {
			char c = rbuf[i];
			if (c == '\n') {
				acc[acc_len] = 0;
				mcp_inbox_push(acc);
				acc_len = 0;
			}
			else {
				if (acc_len + 1 >= cap) {
					cap *= 2;
					acc = realloc(acc, cap);
				}
				acc[acc_len++] = c;
			}
		}
	}

	free(acc);
	mcp_client_fd = -1;
	close(fd);
}

static void *mcp_server_thread(void *arg) {
	int port = MCP_DEFAULT_PORT;
	const char *env = getenv("ARMORPAINT_MCP_PORT");
	if (env != NULL) {
		port = atoi(env);
	}

	int server_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (server_fd < 0) {
		return NULL;
	}
	int opt = 1;
	setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
	addr.sin_port        = htons(port);

	if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(server_fd);
		return NULL;
	}
	if (listen(server_fd, 1) < 0) {
		close(server_fd);
		return NULL;
	}

	iron_log("MCP server listening on 127.0.0.1:%d", port);

	for (;;) {
		int client = accept(server_fd, NULL, NULL);
		if (client < 0) {
			continue;
		}
		int one = 1;
		setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
		mcp_handle_client(client); // one client at a time
	}

	close(server_fd);
	return NULL;
}

// --- public entry -----------------------------------------------------------

void mcp_server_start(void) {
	if (mcp_running) {
		return;
	}
	mcp_running = true;

	sys_notify_on_update(&mcp_server_update, NULL);

	pthread_t thread;
	pthread_create(&thread, NULL, mcp_server_thread, NULL);
	pthread_detach(thread);
}
