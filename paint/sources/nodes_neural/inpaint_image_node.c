
#include "../global.h"

f32  inpaint_image_node_strength;
bool inpaint_image_node_tiled;

static string_array_t *inpaint_image_node_flux_klein_args(char *dir) {
	string_array_t *argv = any_array_create_from_raw(
	    (void *[]){
	        string("%s/%s", dir, neural_node_iris_bin()),
	        "-d",
	        string("%s", dir),
	    },
	    3);
	return argv;
}

void inpaint_image_node_button_on_next_frame(ui_node_t *node) {
	ui_node_t     *from_node = neural_from_node(node->inputs->buffer[0], 0);
	ui_node_t     *mask_node = neural_from_node(node->inputs->buffer[1], 1);
	gpu_texture_t *input     = ui_nodes_get_node_preview_image(from_node);
	gpu_texture_t *mask      = ui_nodes_get_node_preview_image(mask_node);

	if (input != NULL && mask != NULL) {
		char        *node_name = parser_material_node_name(node, NULL);
		ui_handle_t *h         = ui_handle(node_name);
		i32          model     = ui_nest(h, 0)->i;

		char *dir = neural_node_dir();

		if (model == 0) {
#ifdef IRON_BGRA
			buffer_t *input_buf = buffer_bgra_swap(gpu_get_texture_pixels(input)); // Vulkan non-rt textures need a flip
#else
			buffer_t *input_buf = gpu_get_texture_pixels(input);
#endif
			iron_write_png(string("%s%sinput.png", dir, PATH_SEP), input_buf, input->width, input->height, 0);

			buffer_t *mask_buf = gpu_get_texture_pixels(mask);
			for (uint32_t i = 0; i < mask_buf->length / 4; ++i) {
				if (mask_buf->buffer[i * 4] < 200 || mask_buf->buffer[i * 4 + 1] < 200 || mask_buf->buffer[i * 4 + 2] < 200) {
					mask_buf->buffer[i * 4]     = 255;
					mask_buf->buffer[i * 4 + 1] = 255;
					mask_buf->buffer[i * 4 + 2] = 255;
				}
				else {
					mask_buf->buffer[i * 4]     = 0;
					mask_buf->buffer[i * 4 + 1] = 0;
					mask_buf->buffer[i * 4 + 2] = 0;
				}
			}
			iron_write_png(string("%s%smask.png", dir, PATH_SEP), mask_buf, mask->width, mask->height, 0);
		}

		string_array_t *argv;
		if (model == 0) {
			argv = edit_image_node_flux_klein_args(dir);
			string_array_push(argv, "-p");
			string_array_push(argv, "");
			string_array_push(argv, "-i");
			string_array_push(argv, string("%s/input.png", dir));
			string_array_push(argv, "--mask");
			string_array_push(argv, string("%s/mask.png", dir));
		}

		if (g_config->neural_res >= 2048) {
			string_array_push(argv, "--vae-tiling");
		}
		// string_array_push(argv, string("%f", inpaint_image_node_strength));
		string_array_push(argv, "--seed");
		string_array_push(argv, "-1");
		string_array_push(argv, "-W");
		string_array_push(argv, string("%d", g_config->neural_res));
		string_array_push(argv, "-H");
		string_array_push(argv, string("%d", g_config->neural_res));
		string_array_push(argv, "-o");
		string_array_push(argv, string("%s/output.png", dir));
		if (inpaint_image_node_tiled) {
			string_array_push(argv, "--tileable");
		}
		string_array_push(argv, NULL);

		iron_exec_async(argv->buffer[0], argv->buffer);
		sys_notify_on_update(neural_node_check_result, node);
	}
}

void inpaint_image_node_button(i32 node_id) {
	ui_node_canvas_t *canvas    = ui_nodes_get_canvas(true);
	ui_node_t        *node      = ui_get_node(canvas->nodes, node_id);
	char             *node_name = parser_material_node_name(node, NULL);
	ui_handle_t      *h         = ui_handle(node_name);

	string_array_t *models = any_array_create_from_raw(
	    (void *[]){
	        "FLUX 2 klein",
	    },
	    1);
	i32 model = ui_combo(ui_nest(h, 0), models, tr("Model"), false, UI_ALIGN_LEFT, true);

	ui_handle_t *hs = ui_nest(h, 1);
	if (hs->init) {
		hs->f = 1.0;
	}
	inpaint_image_node_strength = ui_slider(hs, tr("Strength"), 0.0, 1.0, true, 100.0, true, UI_ALIGN_LEFT, true);

	inpaint_image_node_tiled = ui_check(ui_nest(h, 2), tr("Tiled"), "");

	if (neural_node_button(node, models->buffer[model])) {
		sys_notify_on_next_frame(&inpaint_image_node_button_on_next_frame, node);
	}
}

void inpaint_image_node_init() {

	ui_node_t *inpaint_image_node_def =
	    GC_ALLOC_INIT(ui_node_t, {.id     = 0,
	                              .name   = _tr("Inpaint Image"),
	                              .type   = "NEURAL_INPAINT_IMAGE",
	                              .x      = 0,
	                              .y      = 0,
	                              .color  = 0xff4982a0,
	                              .inputs = any_array_create_from_raw(
	                                  (void *[]){
	                                      GC_ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                                       .node_id       = 0,
	                                                                       .name          = _tr("Color"),
	                                                                       .type          = "RGBA",
	                                                                       .color         = 0xffc7c729,
	                                                                       .default_value = f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0),
	                                                                       .min           = 0.0,
	                                                                       .max           = 1.0,
	                                                                       .precision     = 100,
	                                                                       .display       = 0}),
	                                      GC_ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                                       .node_id       = 0,
	                                                                       .name          = _tr("Mask"),
	                                                                       .type          = "VALUE",
	                                                                       .color         = 0xffa1a1a1,
	                                                                       .default_value = f32_array_create_x(1.0),
	                                                                       .min           = 0.0,
	                                                                       .max           = 1.0,
	                                                                       .precision     = 100,
	                                                                       .display       = 0}),
	                                  },
	                                  2),
	                              .outputs = any_array_create_from_raw(
	                                  (void *[]){
	                                      GC_ALLOC_INIT(ui_node_socket_t, {.id            = 0,
	                                                                       .node_id       = 0,
	                                                                       .name          = _tr("Color"),
	                                                                       .type          = "RGBA",
	                                                                       .color         = 0xffc7c729,
	                                                                       .default_value = f32_array_create_xyzw(0.0, 0.0, 0.0, 1.0),
	                                                                       .min           = 0.0,
	                                                                       .max           = 1.0,
	                                                                       .precision     = 100,
	                                                                       .display       = 0}),
	                                  },
	                                  1),
	                              .buttons = any_array_create_from_raw(
	                                  (void *[]){
	                                      GC_ALLOC_INIT(ui_node_button_t, {.name          = "inpaint_image_node_button",
	                                                                       .type          = "CUSTOM",
	                                                                       .output        = -1,
	                                                                       .default_value = f32_array_create_x(0),
	                                                                       .data          = NULL,
	                                                                       .min           = 0.0,
	                                                                       .max           = 1.0,
	                                                                       .precision     = 100,
	                                                                       .height        = 4}),
	                                  },
	                                  1),
	                              .width = 0,
	                              .flags = 0});

	any_array_push(nodes_material_neural, inpaint_image_node_def);
	any_map_set(parser_material_node_vectors, "NEURAL_INPAINT_IMAGE", neural_node_vector);
	any_map_set(ui_nodes_custom_buttons, "inpaint_image_node_button", inpaint_image_node_button);
}
