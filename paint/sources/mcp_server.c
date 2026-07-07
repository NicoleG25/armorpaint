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
	char   acc[MCP_LINE_MAX];
	size_t acc_len = 0;
	char   rbuf[2048];

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
			else if (acc_len < sizeof(acc) - 1) {
				acc[acc_len++] = c;
			}
		}
	}

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
