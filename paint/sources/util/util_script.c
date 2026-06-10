
#include "../global.h"

void script_set_stage(char *name) {
	if (g_project->stages == NULL) {
		return;
	}
	for (i32 i = 0; i < g_project->stages->length; ++i) {
		stage_t *s = g_project->stages->buffer[i];
		if (string_equals(s->name, name)) {
			tab_stages_selected = i;
			tab_stages_apply(s);
			return;
		}
	}
}

void script_set_tilesheet_anim(object_t *o, char *anim) {
	mesh_object_t *mo = (mesh_object_t *)o->ext;

	// Locate the material slot
	i32 slot_index = tab_meshes_get_override(mo);
	if (slot_index < 0) {
		for (i32 i = 0; i < g_project->_->materials->length; ++i) {
			if (g_project->_->materials->buffer[i]->data == mo->material) {
				slot_index = i;
				break;
			}
		}
	}

	slot_material_t *slot = g_project->_->materials->buffer[slot_index];

	// Locate the tilesheet animation node
	for (i32 i = 0; i < slot->canvas->nodes->length; ++i) {
		ui_node_t *node = slot->canvas->nodes->buffer[i];
		if (!string_equals(node->type, "TILESHEET_ANIM")) {
			continue;
		}

		ui_node_button_t *enum_but = node->buttons->buffer[4];
		string_array_t   *names    = string_split(u8_array_to_string(enum_but->data), "\n");
		for (i32 j = 0; j < (i32)names->length; ++j) {
			if (!string_equals(names->buffer[j], anim)) {
				continue;
			}

			enum_but->default_value->buffer[0] = (f32)j;
			make_material_parse_paint_material(true);

			// Material override
			for (i32 k = 0; k < g_project->_->paint_objects->length; ++k) {
				mesh_object_t *po = g_project->_->paint_objects->buffer[k];
				if (tab_meshes_get_override(po) == slot_index) {
					tab_meshes_set_override(po, slot_index);
					g_context->ddirty = 2;
				}
			}
			return;
		}
	}
}
