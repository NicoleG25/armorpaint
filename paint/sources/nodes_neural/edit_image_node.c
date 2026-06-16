
#include "../global.h"

static string_array_t *edit_image_node_flux_klein_args(char *dir) {
	string_array_t *argv = any_array_create_from_raw(
	    (void *[]){
	        string("%s/%s", dir, neural_node_sd_bin()),
	        "--diffusion-model",
	        string("%s/flux-2-klein-4b-Q8_0.gguf", dir),
	        "--taesd",
	        string("%s/taef2.safetensors", dir),
	        "--llm",
	        string("%s/Qwen3-4B-Q8_0.gguf", dir),
	        "--steps",
	        "4",
	    },
	    9);
	return argv;
}

static string_array_t *edit_image_node_qwen_args(char *dir) {
	string_array_t *argv = any_array_create_from_raw(
	    (void *[]){
	        string("%s/%s", dir, neural_node_sd_bin()),
	        "--diffusion-model",
	        string("%s/qwen-image-edit-2511-Q4_K_S.gguf", dir),
	        "--vae",
	        string("%s/Qwen_Image-VAE.safetensors", dir),
	        "--llm",
	        string("%s/Qwen2.5-VL-7B-Instruct-Q4_K_S.gguf", dir),
	        "--llm_vision",
	        string("%s/mmproj-F16.gguf", dir),
	        "--qwen-image-zero-cond-t",
	        "--steps",
	        "30",
	    },
	    12);
	return argv;
}

void edit_image_node_button(i32 node_id) {
	ui_node_canvas_t *canvas    = ui_nodes_get_canvas(true);
	ui_node_t        *node      = ui_get_node(canvas->nodes, node_id);
	char             *node_name = parser_material_node_name(node, NULL);
	ui_handle_t      *h         = ui_handle(node_name);

	string_array_t *models           = any_array_create_from_raw((void *[]){"FLUX 2 klein", "Qwen Image Edit"}, 2);
	i32             model            = ui_combo(ui_nest(h, 0), models, tr("Model"), false, UI_ALIGN_LEFT, true);
	char           *prompt           = ui_text_area(ui_nest(h, 1), UI_ALIGN_LEFT, true, tr("prompt"), true);
	node->buttons->buffer[0]->height = string_split(prompt, "\n")->length + 4;

	ui_handle_t *hs = ui_nest(h, 2);
	if (hs->init) {
		hs->f = 1.0;
	}
	f32 strength = ui_slider(hs, tr("Strength"), 0.0, 1.0, true, 100.0, true, UI_ALIGN_LEFT, true);

	bool tiled = ui_check(ui_nest(h, 3), tr("Tiled"), "");

	if (neural_node_button(node, models->buffer[model])) {
		ui_node_t     *from_node = neural_from_node(node->inputs->buffer[0], 0);
		gpu_texture_t *input     = ui_nodes_get_node_preview_image(from_node);
		if (input != NULL) {
#ifdef IRON_BGRA
			buffer_t *input_buf = buffer_bgra_swap(gpu_get_texture_pixels(input)); // Vulkan non-rt textures need a flip
#else
			buffer_t *input_buf = gpu_get_texture_pixels(input);
#endif

			char *dir = neural_node_dir();
			iron_write_png(string("%s%sinput.png", dir, PATH_SEP), input_buf, input->width, input->height, 0);

			string_array_t *argv;
			if (model == 0) {
				argv = edit_image_node_flux_klein_args(dir);
			}
			else {
				argv = edit_image_node_qwen_args(dir);
			}

			string_array_push(argv, "--cfg-scale");
			string_array_push(argv, "1.0");
			string_array_push(argv, "--diffusion-fa");
			string_array_push(argv, "--offload-to-cpu");
			string_array_push(argv, "--strength");
			string_array_push(argv, string("%f", strength));
			string_array_push(argv, "-s");
			string_array_push(argv, "-1");
			string_array_push(argv, "-W");
			string_array_push(argv, string("%d", g_config->neural_res));
			string_array_push(argv, "-H");
			string_array_push(argv, string("%d", g_config->neural_res));
			string_array_push(argv, "-p");
			string_array_push(argv, prompt);
			string_array_push(argv, "-r");
			string_array_push(argv, string("%s/input.png", dir));
			string_array_push(argv, "-o");
			string_array_push(argv, string("%s/output.png", dir));
			if (tiled) {
				string_array_push(argv, "--circular");
			}
			string_array_push(argv, NULL);

			iron_exec_async(argv->buffer[0], argv->buffer);
			sys_notify_on_update(neural_node_check_result, node);
		}
	}
}

void edit_image_node_init() {

	ui_node_t *edit_image_node_def =
	    GC_ALLOC_INIT(ui_node_t, {.id     = 0,
	                              .name   = _tr("Edit Image"),
	                              .type   = "NEURAL_EDIT_IMAGE",
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
	                                                                       .default_value = f32_array_create_xyzw(1.0, 1.0, 1.0, 1.0),
	                                                                       .min           = 0.0,
	                                                                       .max           = 1.0,
	                                                                       .precision     = 100,
	                                                                       .display       = 0}),
	                                  },
	                                  1),
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
	                                      GC_ALLOC_INIT(ui_node_button_t, {.name          = "edit_image_node_button",
	                                                                       .type          = "CUSTOM",
	                                                                       .output        = -1,
	                                                                       .default_value = f32_array_create_x(0),
	                                                                       .data          = NULL,
	                                                                       .min           = 0.0,
	                                                                       .max           = 1.0,
	                                                                       .precision     = 100,
	                                                                       .height        = 0}),
	                                  },
	                                  1),
	                              .width = 0,
	                              .flags = 0});

	any_array_push(nodes_material_neural, edit_image_node_def);
	any_map_set(parser_material_node_vectors, "NEURAL_EDIT_IMAGE", neural_node_vector);
	any_map_set(ui_nodes_custom_buttons, "edit_image_node_button", edit_image_node_button);
}
