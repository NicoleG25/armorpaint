
#include "../global.h"

ui_text_coloring_t *tab_scripts_text_coloring = NULL;
i32                 tab_scripts_selected      = 0;

void tab_scripts_prepare() {
	if (g_project->script_datas == NULL) {
		g_project->script_datas = string_array_create(0);
		g_project->script_names = string_array_create(0);
	}
	if (g_project->script_datas->length == 0) {
		string_array_push(g_project->script_datas, "void main() {\n    \n}\n");
		string_array_push(g_project->script_names, "main.c");
	}
	// A script may have been removed
	if (tab_scripts_selected >= g_project->script_datas->length) {
		tab_scripts_selected = g_project->script_datas->length - 1;
	}
}

char *tab_scripts_get() {
	tab_scripts_prepare();
	return g_project->script_datas->buffer[tab_scripts_selected];
}

void tab_scripts_set(char *s) {
	tab_scripts_prepare();
	g_project->script_datas->buffer[tab_scripts_selected] = string_copy(s);
}

void tab_scripts_create(char *name) {
	tab_scripts_prepare();
	i32 i = string_array_index_of(g_project->script_names, name);
	if (i < 0) {
		string_array_push(g_project->script_names, string_copy(name));
		string_array_push(g_project->script_datas, "");
		tab_scripts_selected = g_project->script_datas->length - 1;
	}
}

void tab_scripts_draw_export(char *path) {
	char *str = tab_scripts_get();
	char *f   = ui_files_filename;
	if (string_equals(f, "")) {
		f = string_copy(tr("untitled"));
	}
	path = string("%s%s%s", path, PATH_SEP, f);
	if (!ends_with(path, ".c")) {
		path = string("%s.c", path);
	}
	iron_file_save_bytes(path, sys_string_to_buffer(str), 0);
}

void tab_scripts_draw_import(char *path) {
	buffer_t *b = data_get_blob(path);
	tab_scripts_set(sys_buffer_to_string(b));
	data_delete_blob(path);
}

void tab_scripts_draw_edit() {
	tab_scripts_prepare();

	if (ui_menu_button(tr("Clear"), "", ICON_ERASE)) {
		tab_scripts_set("");
	}
	ui->enabled = !string_equals(g_project->script_names->buffer[tab_scripts_selected], "main.c");
	if (ui_menu_button(tr("Delete"), "", ICON_DELETE)) {
		array_splice((any_array_t *)g_project->script_datas, tab_scripts_selected, 1);
		array_splice((any_array_t *)g_project->script_names, tab_scripts_selected, 1);
		tab_scripts_selected = 0;
	}
	ui->enabled = true;
	if (ui_menu_button(tr("Import"), "", ICON_IMPORT)) {
		ui_files_show("c", false, false, &tab_scripts_draw_import);
	}
	if (ui_menu_button(tr("Export"), "", ICON_EXPORT)) {
		ui_files_show("c", true, false, &tab_scripts_draw_export);
	}
	if (ui_menu_sub_button(ui_handle(__ID__), tr("Templates"))) {
		ui_menu_sub_begin(2);
		if (ui_menu_button("hello.c", "", ICON_DRAFT)) {
			tab_scripts_set("\
void main() {\n\
	printf(\"Hello, world!\\n\");\n\
}\n\
");
		}
		if (ui_menu_button("rotate.c", "", ICON_DRAFT)) {
			tab_scripts_set("\
void on_update() {\n\
	mesh_object_t *o = context_main_object();\n\
	transform_rotate(o->base->transform, vec4_z_axis(), 0.005);\n\
	context_t *c = script_get_context();\n\
	c->ddirty = 2;\n\
}\n\
void main() {\n\
	script_notify_on_update(on_update);\n\
}\n\
");
		}
		ui_menu_sub_end();
	}
}

void tab_scripts_draw(ui_handle_t *htab) {
	if (ui_tab(htab, tr("Scripts"), false, -1, false)) {

		ui_begin_sticky();
		f32_array_t *row = f32_array_create_from_raw(
		    (f32[]){
		        -70,
		        -70,
		        -140,
		    },
		    3);

		// #ifndef NDEBUG
		// 		if (g_config->experimental) {
		// 			f32_array_push(row, -90);
		// 		}
		// #endif

		ui_row(row);

		if (ui_icon_button(tr("Run"), ICON_PLAY, UI_ALIGN_CENTER)) {
			minic_ctx_t *ctx = minic_eval(tab_scripts_hscript->text);
			// minic_ctx_free(ctx);
		}

		if (ui_icon_button(tr("Edit"), ICON_EDIT, UI_ALIGN_CENTER)) {
			ui_menu_draw(&tab_scripts_draw_edit, -1, -1);
		}

		tab_scripts_prepare();
		ui_handle_t *file_handle = ui_handle(__ID__);
		file_handle->i           = tab_scripts_selected;
		tab_scripts_selected     = ui_combo(file_handle, g_project->script_names, tr("File"), false, UI_ALIGN_LEFT, true);

		// #ifndef NDEBUG
		// 		if (g_config->experimental && ui_icon_button("Run Tests", ICON_PLAY, UI_ALIGN_CENTER)) {
		// 			minic_tests();
		// 		}
		// #endif

		ui_end_sticky();

		draw_font_t *_font      = ui->ops->font;
		i32          _font_size = ui->font_size;
		draw_font_t *f          = data_get_font("font_mono.ttf");
		ui_set_font(ui, f);
		ui->font_size                = math_floor(15 * UI_SCALE());
		ui_text_area_line_numbers    = true;
		ui_text_area_scroll_past_end = true;
		gc_unroot(ui_text_area_coloring);
		ui_text_area_coloring = tab_scripts_get_text_coloring();
		gc_root(ui_text_area_coloring);

		tab_scripts_prepare();

		tab_scripts_hscript->text = g_project->script_datas->buffer[tab_scripts_selected];
		ui_text_area(tab_scripts_hscript, UI_ALIGN_LEFT, true, "", false);
		g_project->script_datas->buffer[tab_scripts_selected] = tab_scripts_hscript->text;

		ui_text_area_line_numbers    = false;
		ui_text_area_scroll_past_end = false;
		gc_unroot(ui_text_area_coloring);
		ui_text_area_coloring = NULL;
		ui_set_font(ui, _font);
		ui->font_size = _font_size;
	}
}

ui_text_coloring_t *tab_scripts_get_text_coloring() {
	if (tab_scripts_text_coloring == NULL) {
		buffer_t *blob = data_get_blob("text_coloring.json");
		gc_unroot(tab_scripts_text_coloring);
		tab_scripts_text_coloring = json_parse(sys_buffer_to_string(blob));
		gc_root(tab_scripts_text_coloring);
	}
	return tab_scripts_text_coloring;
}
