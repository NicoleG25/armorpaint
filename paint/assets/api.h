
// Begin API

#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef int32_t  i32;
typedef uint32_t u32;
typedef int16_t  i16;
typedef uint16_t u16;
typedef int8_t   i8;
typedef uint8_t  u8;
typedef float    f32;

// Opaque types
typedef struct gpu_texture gpu_texture_t;
typedef struct raw_mesh    raw_mesh_t;

// Forward declarations for circular refs
typedef struct object    object_t;
typedef struct transform transform_t;

// --- Array types ---
typedef struct {
	i8 *buffer;
	int length;
	int capacity;
} i8_array_t;
typedef struct {
	u8 *buffer;
	int length;
	int capacity;
} u8_array_t;
typedef struct {
	i16 *buffer;
	int  length;
	int  capacity;
} i16_array_t;
typedef struct {
	u16 *buffer;
	int  length;
	int  capacity;
} u16_array_t;
typedef struct {
	i32 *buffer;
	int  length;
	int  capacity;
} i32_array_t;
typedef struct {
	u32 *buffer;
	int  length;
	int  capacity;
} u32_array_t;
typedef struct {
	f32 *buffer;
	int  length;
	int  capacity;
} f32_array_t;
typedef struct {
	void **buffer;
	int    length;
	int    capacity;
} any_array_t;
typedef struct {
	char **buffer;
	int    length;
	int    capacity;
} string_array_t;
typedef struct {
	u8 *buffer;
	int length;
	int capacity;
} buffer_t;

typedef any_array_t obj_t_array_t;
typedef any_array_t vertex_array_t_array_t;
typedef any_array_t frustum_plane_array_t;
typedef any_array_t bind_const_t_array_t;
typedef any_array_t bind_tex_t_array_t;
typedef any_array_t material_context_t_array_t;
typedef any_array_t shader_const_t_array_t;
typedef any_array_t tex_unit_t_array_t;
typedef any_array_t vertex_element_t_array_t;

// --- Math types ---
typedef struct {
	float x, y;
} vec2_t;
typedef struct {
	float x, y, z;
} vec3_t;
typedef struct {
	float x, y, z, w;
} vec4_t;
typedef struct {
	float x, y, z, w;
} quat_t;
typedef struct {
	float m00, m01, m02, m10, m11, m12, m20, m21, m22;
} mat3_t;
typedef struct {
	float m00, m01, m02, m03, m10, m11, m12, m13, m20, m21, m22, m23, m30, m31, m32, m33;
} mat4_t;

// --- Enums ---
typedef enum {
	UI_LAYOUT_VERTICAL,
	UI_LAYOUT_HORIZONTAL
} ui_layout_t;
typedef enum {
	UI_ALIGN_LEFT,
	UI_ALIGN_CENTER,
	UI_ALIGN_RIGHT
} ui_align_t;
typedef enum {
	UI_STATE_IDLE,
	UI_STATE_STARTED,
	UI_STATE_DOWN,
	UI_STATE_RELEASED,
	UI_STATE_HOVERED
} ui_state_t;
typedef enum {
	GPU_TEXTURE_FORMAT_RGBA32,
	GPU_TEXTURE_FORMAT_RGBA64,
	GPU_TEXTURE_FORMAT_RGBA128,
	GPU_TEXTURE_FORMAT_R8,
	GPU_TEXTURE_FORMAT_R16,
	GPU_TEXTURE_FORMAT_R32,
	GPU_TEXTURE_FORMAT_D32,
	GPU_TEXTURE_FORMAT_RGBA32_BC7,
} gpu_texture_format_t;

// --- UI structs ---
typedef struct {
	int         i;
	float       f;
	int         b;
	int         layout;
	float       scroll_offset;
	int         color;
	int         redraws;
	char       *text;
	int         scroll_enabled;
	int         drag_enabled;
	int         changed;
	int         init;
	any_array_t children;
} ui_handle_t;

typedef struct {
	int         id;
	int         node_id;
	char       *name;
	char       *type;
	int         color;
	f32_array_t default_value;
	float       min;
	float       max;
	float       precision;
	int         display;
} ui_node_socket_t;

typedef struct {
	char       *name;
	char       *type;
	int         output;
	f32_array_t default_value;
	u8_array_t  data;
	float       min;
	float       max;
	float       precision;
	float       height;
} ui_node_button_t;

typedef struct {
	int id;
	int from_id;
	int from_socket;
	int to_id;
	int to_socket;
} ui_node_link_t;

typedef struct {
	int         id;
	char       *name;
	char       *type;
	float       x;
	float       y;
	int         color;
	any_array_t inputs;
	any_array_t outputs;
	any_array_t buttons;
	float       width;
	int         flags;
} ui_node_t;

// --- Engine data structs ---
typedef struct {
	char         *name;
	char         *type;
	char         *data_ref;
	f32_array_t   transform;
	f32_array_t   dimensions;
	int           visible;
	int           spawn;
	void         *anim;
	char         *material_ref;
	obj_t_array_t children;
	void         *_;
} obj_t;

typedef struct {
	char       *attrib;
	char       *data;
	i16_array_t values;
} vertex_array_t;

typedef struct {
	char                  *name;
	float                  scale_pos;
	float                  scale_tex;
	vertex_array_t_array_t vertex_arrays;
	u32_array_t            index_array;
	void                  *_;
} mesh_data_t;

typedef struct {
	char       *name;
	float       near_plane;
	float       far_plane;
	float       fov;
	float       aspect;
	int         frustum_culling;
	f32_array_t ortho;
} camera_data_t;

typedef struct {
	char *name;
	int   color;
	float strength;
	char *irradiance;
	char *radiance;
	int   radiance_mipmaps;
	char *envmap;
	void *_;
} world_data_t;

typedef struct {
	char *name;
	char *data;
} vertex_element_t;

typedef struct {
	char *name;
	char *type;
	char *link;
} shader_const_t;

typedef struct {
	char *name;
	char *link;
} tex_unit_t;

typedef struct {
	char                    *name;
	int                      depth_write;
	char                    *compare_mode;
	char                    *cull_mode;
	char                    *vertex_shader;
	char                    *fragment_shader;
	int                      shader_from_source;
	char                    *blend_source;
	char                    *blend_destination;
	char                    *alpha_blend_source;
	char                    *alpha_blend_destination;
	string_array_t           color_attachments;
	char                    *depth_attachment;
	vertex_element_t_array_t vertex_elements;
	shader_const_t_array_t   constants;
	tex_unit_t_array_t       texture_units;
} shader_context_t;

typedef struct {
	char       *name;
	any_array_t contexts;
} shader_data_t;

typedef struct {
	char       *name;
	f32_array_t vec;
} bind_const_t;

typedef struct {
	char *name;
	char *file;
} bind_tex_t;

typedef struct {
	char                *name;
	bind_const_t_array_t bind_constants;
	bind_tex_t_array_t   bind_textures;
	void                *_;
} material_context_t;

typedef struct {
	char                      *name;
	char                      *shader;
	material_context_t_array_t contexts;
	void                      *_;
} material_data_t;

typedef struct {
	char          *name;
	int            width;
	int            height;
	char          *format;
	float          scale;
	gpu_texture_t *_image;
} render_target_t;

// --- Scene object structs ---
struct transform {
	vec4_t    loc;
	float     scale_world;
	int       dirty;
	object_t *object;
	float     radius;
};

struct object {
	int         uid;
	float       urandom;
	obj_t       raw;
	char       *name;
	transform_t transform;
	object_t   *parent;
	any_array_t children;
	int         visible;
	int         culled;
	int         is_empty;
	void       *ext;
	char       *ext_type;
};

typedef struct {
	object_t        base;
	mesh_data_t     data;
	material_data_t material;
	float           camera_dist;
	int             frustum_culling;
	char           *skip_context;
	char           *force_context;
} mesh_object_t;

typedef struct {
	object_t              base;
	camera_data_t         data;
	int                   frame;
	frustum_plane_array_t frustum_planes;
} camera_object_t;

// --- Paint structs ---
typedef struct {
	int            window_w;
	int            window_h;
	float          window_scale;
	float          rp_supersample;
	string_array_t recent_projects;
	string_array_t plugins;
	char          *keymap;
	char          *theme;
	int            undo_steps;
	float          camera_fov;
	int            layer_res;
	int            brush_live;
	int            node_previews;
	int            material_live;
	int            workspace;
	int            workflow;
} config_t;

typedef struct {
	mesh_object_t paint_object;
	int           ddirty;
	int           pdirty;
	int           rdirty;
	void         *material;
	void         *layer;
	void         *brush;
	int           tool;
	float         brush_radius;
	float         brush_opacity;
	float         brush_hardness;
	float         brush_scale;
	float         brush_angle;
	int           brush_blending;
	int           viewport_mode;
	int           xray;
	bool          capturing_screenshot;
} context_t;

typedef struct {
	char          *version;
	string_array_t assets;
	int            is_bgra;
	char          *envmap;
	float          envmap_strength;
	float          envmap_angle;
	float          camera_fov;
	f32_array_t    camera_world;
	f32_array_t    camera_origin;
	void          *swatches;
	void          *brush_nodes;
	void          *material_nodes;
	string_array_t font_assets;
	void          *layer_datas;
	void          *mesh_datas;
	string_array_t script_datas;
} project_t;

// --- Math ---
float   vec2_len(vec2_t *v);
vec2_t *vec2_set_len(vec2_t *v, float len);
vec2_t *vec2_mult(vec2_t *v, float f);
vec2_t *vec2_add(vec2_t *a, vec2_t *b);
vec2_t *vec2_sub(vec2_t *a, vec2_t *b);
float   vec2_cross(vec2_t *a, vec2_t *b);
vec2_t *vec2_norm(vec2_t *v);
float   vec2_dot(vec2_t *a, vec2_t *b);
vec2_t *vec2_nan(void);
int     vec2_isnan(vec2_t *v);
vec4_t *vec4_cross(vec4_t *a, vec4_t *b);
vec4_t *vec4_add(vec4_t *a, vec4_t *b);
vec4_t *vec4_fadd(vec4_t *v, float x, float y, float z, float w);
vec4_t *vec4_norm(vec4_t *v);
vec4_t *vec4_mult(vec4_t *v, float f);
float   vec4_dot(vec4_t *a, vec4_t *b);
vec4_t *vec4_apply_proj(vec4_t *v, mat4_t *m);
vec4_t *vec4_apply_mat4(vec4_t *v, mat4_t *m);
vec4_t *vec4_apply_axis_angle(vec4_t *v, vec4_t *axis, float angle);
vec4_t *vec4_apply_quat(vec4_t *v, quat_t *q);
int     vec4_equals(vec4_t *a, vec4_t *b);
int     vec4_almost_equals(vec4_t *a, vec4_t *b, float eps);
float   vec4_len(vec4_t *v);
vec4_t *vec4_sub(vec4_t *a, vec4_t *b);
float   vec4_dist(vec4_t *a, vec4_t *b);
vec4_t *vec4_reflect(vec4_t *v, vec4_t *n);
vec4_t *vec4_clamp(vec4_t *v, float min, float max);
vec4_t *vec4_x_axis(void);
vec4_t *vec4_y_axis(void);
vec4_t *vec4_z_axis(void);
vec4_t *vec4_nan(void);
int     vec4_isnan(vec4_t *v);
quat_t *quat_from_axis_angle(vec4_t *axis, float angle);
quat_t *quat_from_mat(mat4_t *m);
quat_t *quat_from_rot_mat(mat4_t *m);
quat_t *quat_mult(quat_t *a, quat_t *b);
quat_t *quat_norm(quat_t *q);
vec4_t *quat_get_euler(quat_t *q);
quat_t *quat_from_euler(float x, float y, float z);
float   quat_dot(quat_t *a, quat_t *b);
quat_t *quat_from_to(vec4_t *from, vec4_t *to);
quat_t *quat_inv(quat_t *q);
mat3_t *mat3_identity(void);
mat3_t *mat3_translation(float x, float y);
mat3_t *mat3_rotation(float angle);
mat3_t *mat3_scale(mat3_t *m, vec4_t *s);
mat3_t *mat3_set_from4(mat4_t *m);
mat3_t *mat3_multmat(mat3_t *a, mat3_t *b);
mat3_t *mat3_transpose(mat3_t *m);
mat3_t *mat3_nan(void);
int     mat3_isnan(mat3_t *m);
mat4_t *mat4_identity(void);
mat4_t *mat4_persp(float fov, float aspect, float near, float far);
mat4_t *mat4_ortho(float left, float right, float bottom, float top, float near, float far);
mat4_t *mat4_rot_z(float angle);
mat4_t *mat4_compose(vec4_t *loc, quat_t *rot, vec4_t *scale);
mat4_t *mat4_set_loc(mat4_t *m, vec4_t *loc);
mat4_t *mat4_from_quat(quat_t *q);
mat4_t *mat4_translate(mat4_t *m, float x, float y, float z);
mat4_t *mat4_scale(mat4_t *m, vec4_t *s);
mat4_t *mat4_mult_mat3x4(mat4_t *a, mat4_t *b);
mat4_t *mat4_mult_mat(mat4_t *a, mat4_t *b);
mat4_t *mat4_inv(mat4_t *m);
mat4_t *mat4_transpose(mat4_t *m);
mat4_t *mat4_transpose3(mat4_t *m);
vec4_t *mat4_get_loc(mat4_t *m);
vec4_t *mat4_get_scale(mat4_t *m);
mat4_t *mat4_mult(mat4_t *m, float f);
mat4_t *mat4_to_rot(mat4_t *m);
vec4_t *mat4_right(mat4_t *m);
vec4_t *mat4_look(mat4_t *m);
vec4_t *mat4_up(mat4_t *m);
void   *mat4_to_f32_array(mat4_t *m);
float   mat4_determinant(mat4_t *m);
mat4_t *mat4_nan(void);
int     mat4_isnan(mat4_t *m);
void    transform_set_matrix(transform_t *t, mat4_t *m);
void    transform_rotate(transform_t *t, vec4_t *axis, float angle);
void    transform_move(transform_t *t, vec4_t *dir, float dist);
vec4_t *transform_look(transform_t *t);
vec4_t *transform_right(transform_t *t);
vec4_t *transform_up(transform_t *t);
vec4_t *raycast_aabb(object_t *obj);
void    line_draw_render(mat4_t *m);
void    line_draw_bounds(mat4_t *m, vec4_t *bounds);
void    shape_draw_sphere(mat4_t *m);
void    draw_set_transform(mat3_t *m);
int     iron_random_get(void);
int     iron_random_get_max(int max);
int     iron_random_get_in(int min, int max);
float   vec4_fdist(float x0, float y0, float z0, float x1, float y1, float z1);
float   mat4_cofactor(float a0, float a1, float a2, float b0, float b1, float b2, float c0, float c1, float c2);
float   cosf(float x);
float   sinf(float x);

// --- Object ---
object_t *object_create(int is_empty);
void      object_set_parent(object_t *obj, object_t *parent);
void      object_remove(object_t *obj);
object_t *object_get_child(object_t *obj, char *name);

// --- Transform ---
transform_t *transform_create(object_t *obj);
void         transform_reset(transform_t *t);
void         transform_update(transform_t *t);
void         transform_build_matrix(transform_t *t);
void         transform_decompose(transform_t *t);
float        transform_world_x(transform_t *t);
float        transform_world_y(transform_t *t);
float        transform_world_z(transform_t *t);

// --- Camera object ---
camera_object_t *camera_object_create(camera_data_t *data);
void             camera_object_build_proj(camera_object_t *cam, float aspect);
void             camera_object_remove(camera_object_t *cam);
void             camera_object_build_mat(camera_object_t *cam);

// --- World data ---
world_data_t *world_data_parse(void *raw, char *name);
void          world_data_load_envmap(world_data_t *data);

// --- Material data ---
material_data_t    *material_data_create(void *raw, char *name);
material_data_t    *material_data_parse(void *raw, char *name);
material_context_t *material_data_get_context(material_data_t *data, char *name);
void                material_context_load(material_context_t *ctx);

// --- Shader data ---
shader_data_t    *shader_data_create(void *raw);
shader_data_t    *shader_data_parse(void *raw, char *name);
void              shader_data_delete(shader_data_t *data);
shader_context_t *shader_data_get_context(shader_data_t *data, char *name);
void              shader_context_load(shader_context_t *ctx);
void              shader_context_compile(shader_context_t *ctx);
void              shader_context_finish_compile(shader_context_t *ctx);
void              shader_context_delete(shader_context_t *ctx);
void              shader_context_add_const(shader_context_t *ctx, int i);
void              shader_context_add_tex(shader_context_t *ctx, int i);

// --- Mesh data ---
mesh_data_t    *mesh_data_parse(void *raw, char *name);
mesh_data_t    *mesh_data_create(void *raw);
int             mesh_data_get_vertex_size(mesh_data_t *data);
void            mesh_data_build_vertices(mesh_data_t *data, void *buf);
void            mesh_data_build_indices(mesh_data_t *data, void *buf);
vertex_array_t *mesh_data_get_vertex_array(mesh_data_t *data, char *attrib);
void            mesh_data_build(mesh_data_t *data);
void            mesh_data_delete(mesh_data_t *data);

// --- Mesh object ---
mesh_object_t *mesh_object_create(mesh_data_t *data, material_data_t *mat);
void           mesh_object_set_data(mesh_object_t *obj, mesh_data_t *data);
void           mesh_object_remove(mesh_object_t *obj);
void           mesh_object_render(mesh_object_t *obj, material_data_t *mat, void *ctx);

// --- Data ---
mesh_data_t     *data_get_mesh(void *pack, char *name);
camera_data_t   *data_get_camera(void *pack, char *name);
material_data_t *data_get_material(void *pack, char *name);
world_data_t    *data_get_world(void *pack, char *name);
shader_data_t   *data_get_shader(void *pack, char *name);
void            *data_get_scene_raw(char *name);
gpu_texture_t   *data_get_image(char *name);
buffer_t        *data_get_blob(char *name);
void            *data_get_video(char *name);
void            *data_get_font(char *name);
void             data_delete_mesh(char *name);
void             data_delete_blob(char *name);
void             data_delete_image(char *name);
void             data_delete_video(char *name);
void             data_delete_font(char *name);
bool             data_is_abs(char *path);
char            *data_path(void);

// --- Scene ---
void            *scene_create(char *name);
void             scene_remove(void);
void            *scene_set_active(void *scene);
object_t        *scene_add_object(char *name);
object_t        *scene_get_child(char *name);
mesh_object_t   *scene_add_mesh_object(char *mesh, char *mat, char *name);
camera_object_t *scene_add_camera_object(char *data, char *name);
void            *scene_add_scene(char *name, object_t *parent);
object_t        *scene_spawn_object(void *scene, object_t *parent, int anim);
object_t        *scene_get_raw_object_by_name(void *scene, char *name);
object_t        *scene_create_object(void *scene, char *name, object_t *parent);
mesh_object_t   *scene_create_mesh_object(void *pack, char *name, mesh_data_t *data, material_data_t *mat);
void             scene_gen_transform(object_t *obj, void *scene);

// --- Render path ---
void             render_path_set_target(char *name, void *additional_targets, void *clear_color, int flags, int color_bits, float depth_bits);
void             render_path_end(void);
void             render_path_draw_meshes(char *context);
void             render_path_draw_skydome(char *name);
void             render_path_bind_target(char *name, char *uniform);
void             render_path_draw_shader(char *name);
void             render_path_load_shader(char *name);
void             render_path_resize(void);
render_target_t *render_path_create_render_target(render_target_t *rt);
render_target_t *render_target_create(void);

// --- UI ---
void         ui_begin(void *g);
void         ui_begin_sticky(void);
void         ui_end_sticky(void);
void         ui_begin_region(void *g, int x, int y, int w);
void         ui_end_region(void);
bool         ui_window(ui_handle_t *handle, int x, int y, int w, int h, int drag);
bool         ui_button(char *text, int align, char *icon);
int          ui_text(char *text, int align, int bg);
bool         ui_tab(ui_handle_t *handle, char *text, int vertical, int color, int notab);
bool         ui_panel(ui_handle_t *handle, char *text, int type, int filled, int pack);
int          ui_sub_image(gpu_texture_t *img, int color, int sx, int sy, int sw, int sh, int h);
int          ui_image(gpu_texture_t *img, int color, int h);
char        *ui_text_input(ui_handle_t *handle, char *label, int align, int multiline, int password);
bool         ui_check(ui_handle_t *handle, char *text, char *label);
bool         ui_radio(ui_handle_t *handle, int index, char *text, char *label);
int          ui_combo(ui_handle_t *handle, void *texts, char *label, int init_max, int search_bar, int label_align);
float        ui_slider(ui_handle_t *handle, char *text, float from, float to, int filled, float precision, int display_value, int align, int no_bg);
void         ui_row(f32_array_t *splits);
void         ui_row2(void);
void         ui_row3(void);
void         ui_row4(void);
void         ui_row5(void);
void         ui_row6(void);
void         ui_row7(void);
void         ui_separator(int h, int color);
void         ui_tooltip(char *text);
void         ui_tooltip_image(gpu_texture_t *img, int max_size);
void         ui_end(void);
void         ui_end_window(void);
void         ui_mouse_down(void *g, int button, int x, int y);
void         ui_mouse_move(void *g, int x, int y, int mx, int my);
void         ui_mouse_up(void *g, int button, int x, int y);
void         ui_mouse_wheel(void *g, float delta);
void         ui_key_down(void *g, int key);
void         ui_key_up(void *g, int key);
void         ui_key_press(void *g, int key);
ui_handle_t *ui_handle_create(void);
ui_handle_t *ui_nest(ui_handle_t *handle, int i);
void         ui_set_scale(float scale);
bool         ui_get_hover(float h);
bool         ui_get_released(float h);
bool         ui_input_in_rect(float x, float y, float w, float h);
void         ui_fill(float x, float y, float w, float h, int color);
void         ui_rect(float x, float y, float w, float h, int color, float strength);
bool         ui_is_visible(float h);
void         ui_end_element(void);
void         ui_end_element_of_size(float h);
void         ui_fade_color(float amount);
void         ui_draw_string(char *text, float x, float y, int align, int truncate);
void         ui_draw_shadow(float x, float y, float w, float h);
void         ui_draw_rect(int fill, int shadows, float x, float y, float w, float h);
void         ui_start_text_edit(ui_handle_t *handle, int align);
float        UI_SCALE(void);
float        UI_ELEMENT_W(void);
float        UI_ELEMENT_H(void);
float        UI_ELEMENT_OFFSET(void);
float        UI_ARROW_SIZE(void);
float        UI_BUTTON_H(void);
float        UI_CHECK_SIZE(void);
float        UI_CHECK_SELECT_SIZE(void);
float        UI_FONT_SIZE(void);
float        UI_SCROLL_W(void);
float        UI_TEXT_OFFSET(void);
float        UI_TAB_W(void);
float        UI_HEADER_DRAG_H(void);
float        UI_TOOLTIP_DELAY(void);
float        ui_float_input(ui_handle_t *handle, char *text, int align, float precision);
int          ui_inline_radio(ui_handle_t *handle, void *texts, int align);
int          ui_color_wheel(ui_handle_t *handle, int alpha, float w, float h, int button, char *picker, char *label);
char        *ui_text_area(ui_handle_t *handle, int align, int editable, char *label, int word_wrap);
void         ui_begin_menu(void);
void         ui_end_menu(void);
bool         ui_menubar_button(char *text);
int          ui_color_r(int color);
int          ui_color_g(int color);
int          ui_color_b(int color);
int          ui_color_a(int color);
int          ui_color(int r, int g, int b, int a);

// --- UI nodes ---
void            ui_nodes_init(void *canvas);
void            ui_node_canvas(void *g, void *canvas);
void            ui_nodes_rgba_popup(ui_handle_t *handle, ui_node_socket_t *socket, int x, int y);
void            ui_remove_node(ui_node_t *node, void *canvas);
float           UI_NODES_SCALE(void);
float           UI_NODES_PAN_X(void);
float           UI_NODES_PAN_Y(void);
float           UI_NODE_X(ui_node_t *node);
float           UI_NODE_Y(ui_node_t *node);
float           UI_NODE_W(ui_node_t *node);
float           UI_NODE_H(ui_node_t *node, void *canvas);
float           UI_OUTPUT_Y(ui_node_t *node, int i);
float           UI_INPUT_Y(ui_node_t *node, void *canvas, int i);
float           UI_OUTPUTS_H(ui_node_t *node, int i);
float           UI_BUTTONS_H(ui_node_t *node);
float           UI_LINE_H(void);
int             ui_get_socket_id(void *canvas);
ui_node_link_t *ui_get_link(void *canvas, int i);
int             ui_next_link_id(void *canvas);
ui_node_t      *ui_get_node(void *canvas, int i);
int             ui_next_node_id(void *canvas);

// --- System ---
float     sys_time(void);
float     sys_delta(void);
float     sys_real_delta(void);
int       sys_w(void);
int       sys_h(void);
int       sys_x(void);
int       sys_y(void);
char     *sys_title(void);
void      sys_title_set(char *title);
void     *sys_get_shader(char *name);
char     *sys_buffer_to_string(buffer_t *buf);
buffer_t *sys_string_to_buffer(char *s);

// --- Shape ---
void line_draw_init(void);
void line_draw_lineb(int x0, int y0, int z0, int x1, int y1, int z1);
void line_draw_line(float x0, float y0, float z0, float x1, float y1, float z1);
void line_draw_begin(void);
void line_draw_end(void);

// --- Draw ---
void  draw_begin(gpu_texture_t *img, int clear, int color);
void  draw_scaled_sub_image(gpu_texture_t *img, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh);
void  draw_scaled_image(gpu_texture_t *img, float x, float y, float w, float h);
void  draw_sub_image(gpu_texture_t *img, float x, float y, float sx, float sy, float sw, float sh);
void  draw_image(gpu_texture_t *img, float x, float y);
void  draw_filled_triangle(float x0, float y0, float x1, float y1, float x2, float y2);
void  draw_filled_rect(float x, float y, float w, float h);
void  draw_rect(float x, float y, float w, float h, float strength);
void  draw_line(float x0, float y0, float x1, float y1, float strength);
void  draw_line_aa(float x0, float y0, float x1, float y1, float strength);
void  draw_string(char *text, float x, float y);
void  draw_end(void);
void  draw_flush(void);
void  draw_set_color(int color);
int   draw_get_color(void);
void  draw_set_pipeline(void *pipeline);
bool  draw_set_font(void *font, int size);
float draw_sub_string_width(void *font, int size, char *text, int start, int end);
int   draw_string_width(void *font, int size, char *text);
void  draw_filled_circle(float x, float y, float r, int segments);
void  draw_circle(float x, float y, float r, int segments, float strength);
void  draw_cubic_bezier(f32_array_t *x, f32_array_t *y, int segments, float strength);

// --- String ---
char           *string_alloc(int size);
char           *string_copy(char *s);
int             string_length(char *s);
bool            string_equals(char *a, char *b);
char           *i32_to_string(int i);
char           *i32_to_string_hex(int i);
char           *i64_to_string(int i);
char           *u64_to_string(int i);
char           *f32_to_string(float f);
char           *f32_to_string_with_zeros(float f);
void            string_strip_trailing_zeros(char *s);
int             string_index_of(char *s, char *sub);
int             string_index_of_pos(char *s, char *sub, int pos);
int             string_last_index_of(char *s, char *sub);
string_array_t *string_split(char *s, char *sep);
char           *string_array_join(string_array_t *arr, char *sep);
char           *string_replace_all(char *s, char *from, char *to);
char           *substring(char *s, int from, int to);
char           *string_from_char_code(int code);
int             char_code_at(char *s, int i);
char           *char_at(char *s, int i);
bool            starts_with(char *s, char *prefix);
bool            ends_with(char *s, char *suffix);
char           *trim_end(char *s);
int             string_utf8_decode(char *s, void *out);

// --- File ---
bool  iron_file_reader_open(void *reader, char *path, int size);
bool  iron_file_reader_close(void *reader);
int   iron_file_reader_read(void *reader, void *buf, int size);
int   iron_file_reader_size(void *reader);
int   iron_file_reader_pos(void *reader);
bool  iron_file_reader_seek(void *reader, int pos);
bool  iron_file_writer_open(void *writer, char *path);
void  iron_file_writer_write(void *writer, void *buf, int size);
void  iron_file_writer_close(void *writer);
void *iron_read_directory(char *path);
void  iron_create_directory(char *path);
bool  iron_is_directory(char *path);
bool  iron_file_exists(char *path);
void  iron_delete_file(char *path);
void  iron_file_save_bytes(char *path, void *buf, int size);
void  iron_file_download(char *url, void *buf, int size, char *path);
void *file_read_directory(char *path);
void  file_copy(char *from, char *to);
void  file_start(char *path);
void  file_download_to(char *url, char *path, void *cb, int size);

// --- GC ---
void *gc_alloc(int size);
void  gc_leaf(void *p);
void  gc_root(void *p);
void  gc_unroot(void *p);
void *gc_realloc(void *p, int size);
void  gc_free(void *p);
void  gc_pause(void);
void  gc_resume(void);
void  gc_run(void);
void  gc_start(void *heap);
void  gc_stop(void);

// --- Map ---
void            i32_map_set(void *map, char *key, int val);
void            f32_map_set(void *map, char *key, float val);
void            any_map_set(void *map, char *key, void *val);
int             i32_map_get(void *map, char *key);
float           f32_map_get(void *map, char *key);
void           *any_map_get(void *map, char *key);
void            map_delete(void *map, char *key);
string_array_t *map_keys(void *map);
void           *i32_map_create(void);
void           *any_map_create(void);
void            i32_imap_set(void *map, int key, int val);
void            any_imap_set(void *map, int key, void *val);
int             i32_imap_get(void *map, int key);
void           *any_imap_get(void *map, int key);
void            imap_delete(void *map, int key);
i32_array_t    *imap_keys(void *map);
void           *any_imap_create(void);

// --- Array ---
void            array_free(void *arr);
void            i8_array_push(i8_array_t *arr, int val);
void            u8_array_push(u8_array_t *arr, int val);
void            i16_array_push(i16_array_t *arr, int val);
void            u16_array_push(u16_array_t *arr, int val);
void            i32_array_push(i32_array_t *arr, int val);
void            u32_array_push(u32_array_t *arr, int val);
void            f32_array_push(f32_array_t *arr, float val);
void            any_array_push(any_array_t *arr, void *val);
void            string_array_push(string_array_t *arr, char *val);
void            i8_array_resize(i8_array_t *arr, int size);
void            u8_array_resize(u8_array_t *arr, int size);
void            i16_array_resize(i16_array_t *arr, int size);
void            u16_array_resize(u16_array_t *arr, int size);
void            i32_array_resize(i32_array_t *arr, int size);
void            u32_array_resize(u32_array_t *arr, int size);
void            f32_array_resize(f32_array_t *arr, int size);
void            any_array_resize(any_array_t *arr, int size);
void            string_array_resize(string_array_t *arr, int size);
void            buffer_resize(buffer_t *arr, int size);
void            array_sort(void *arr, void *fn);
void            i32_array_sort(i32_array_t *arr, void *fn);
void           *array_pop(any_array_t *arr);
int             i32_array_pop(i32_array_t *arr);
void           *array_shift(any_array_t *arr);
void            array_splice(any_array_t *arr, int start, int count);
void            i32_array_splice(i32_array_t *arr, int start, int count);
any_array_t    *array_concat(any_array_t *a, any_array_t *b);
any_array_t    *array_slice(any_array_t *arr, int from, int to);
void            array_insert(any_array_t *arr, int i, void *val);
void            array_remove(any_array_t *arr, void *val);
void            string_array_remove(string_array_t *arr, char *val);
void            i32_array_remove(i32_array_t *arr, int val);
int             array_index_of(any_array_t *arr, void *val);
int             string_array_index_of(string_array_t *arr, char *val);
int             i32_array_index_of(i32_array_t *arr, int val);
void            array_reverse(any_array_t *arr);
buffer_t       *buffer_slice(buffer_t *buf, int from, int to);
int             buffer_get_u8(buffer_t *buf, int pos);
int             buffer_get_i8(buffer_t *buf, int pos);
int             buffer_get_u16(buffer_t *buf, int pos);
int             buffer_get_i16(buffer_t *buf, int pos);
float           buffer_get_f16(buffer_t *buf, int pos);
int             buffer_get_u32(buffer_t *buf, int pos);
int             buffer_get_i32(buffer_t *buf, int pos);
float           buffer_get_f32(buffer_t *buf, int pos);
float           buffer_get_f64(buffer_t *buf, int pos);
int             buffer_get_i64(buffer_t *buf, int pos);
void            buffer_set_u8(buffer_t *buf, int pos, int val);
void            buffer_set_i8(buffer_t *buf, int pos, int val);
void            buffer_set_u16(buffer_t *buf, int pos, int val);
void            buffer_set_i16(buffer_t *buf, int pos, int val);
void            buffer_set_u32(buffer_t *buf, int pos, int val);
void            buffer_set_i32(buffer_t *buf, int pos, int val);
void            buffer_set_f32(buffer_t *buf, int pos, float val);
buffer_t       *buffer_create(int size);
buffer_t       *buffer_create_from_raw(void *p, int size);
f32_array_t    *f32_array_create(int size);
f32_array_t    *f32_array_create_from_buffer(buffer_t *buf);
f32_array_t    *f32_array_create_from_array(f32_array_t *arr);
f32_array_t    *f32_array_create_from_raw(void *p, int size);
f32_array_t    *f32_array_create_x(float x);
f32_array_t    *f32_array_create_xy(float x, float y);
f32_array_t    *f32_array_create_xyz(float x, float y, float z);
f32_array_t    *f32_array_create_xyzw(float x, float y, float z, float w);
f32_array_t    *f32_array_create_xyzwv(float x, float y, float z, float w, float v);
u32_array_t    *u32_array_create(int size);
u32_array_t    *u32_array_create_from_array(u32_array_t *arr);
u32_array_t    *u32_array_create_from_raw(void *p, int size);
i32_array_t    *i32_array_create(int size);
i32_array_t    *i32_array_create_from_array(i32_array_t *arr);
i32_array_t    *i32_array_create_from_raw(void *p, int size);
u16_array_t    *u16_array_create(int size);
u16_array_t    *u16_array_create_from_raw(void *p, int size);
i16_array_t    *i16_array_create(int size);
i16_array_t    *i16_array_create_from_array(i16_array_t *arr);
i16_array_t    *i16_array_create_from_raw(void *p, int size);
u8_array_t     *u8_array_create(int size);
u8_array_t     *u8_array_create_from_array(u8_array_t *arr);
u8_array_t     *u8_array_create_from_raw(void *p, int size);
u8_array_t     *u8_array_create_from_string(char *s);
char           *u8_array_to_string(u8_array_t *arr);
i8_array_t     *i8_array_create(int size);
i8_array_t     *i8_array_create_from_raw(void *p, int size);
any_array_t    *any_array_create(int size);
any_array_t    *any_array_create_from_raw(void *p, int size);
string_array_t *string_array_create(int size);

// --- Input ---
bool  mouse_down(char *button);
bool  mouse_down_any(void);
bool  mouse_started(char *button);
bool  mouse_started_any(void);
bool  mouse_released(char *button);
float mouse_view_x(void);
float mouse_view_y(void);
bool  keyboard_down(char *key);
bool  keyboard_started(char *key);
bool  keyboard_started_any(void);
bool  keyboard_released(char *key);
bool  keyboard_repeat(char *key);
char *keyboard_key_code(int code);

// --- Paint ---
void          *plugin_create(void);
void           plugin_notify_on_ui(void *plugin, void *fn);
void           plugin_notify_on_update(void *plugin, void *fn);
void           plugin_notify_on_delete(void *plugin, void *fn);
void           script_notify_on_update(void *fn);
void           script_notify_on_next_frame(void *fn);
void           console_info(char *s);
void           console_error(char *s);
void           console_log(char *s);
void           ui_box_show_message(char *title, char *text, bool copyable);
void           ui_files_show2(char *filters, bool is_save, bool open_multiple, void *files_done);
void           project_save(bool save_and_quit);
char          *project_filepath_get(void);
void           project_filepath_set(char *path);
context_t     *script_get_context(void);
config_t      *script_get_config(void);
project_t     *script_get_project(void);
object_t      *script_get_object(char *name);
void           context_set_viewport_shader(void *viewport_shader);
void           context_set_viewport_mode(int mode);
void           context_set_camera_controls(int i);
void           node_shader_write_frag(void *raw, char *s);
void           plugin_register_texture(char *format, void *fn);
void           plugin_unregister_texture(char *format);
void           plugin_register_mesh(char *format, void *fn);
void           plugin_unregister_mesh(char *format);
raw_mesh_t    *plugin_make_raw_mesh(char *name, i16_array_t *posa, i16_array_t *nora, u32_array_t *inda, float scale_pos);
void           plugin_material_category_add(char *category_name, any_array_t *node_list);
void           plugin_brush_category_add(char *category_name, any_array_t *node_list);
void           plugin_material_category_remove(char *category_name);
void           plugin_brush_category_remove(char *category_name);
void           plugin_material_custom_nodes_set(char *node_type, void *fn);
void           plugin_brush_custom_nodes_set(char *node_type, void *fn);
void           plugin_material_custom_nodes_remove(char *node_type);
void           plugin_brush_custom_nodes_remove(char *node_type);
void          *plugin_material_kong_get(void);
char          *parser_material_parse_value_input(ui_node_socket_t *inp, bool vector_as_grayscale);
mesh_object_t *context_main_object(void);
void           export_texture_run(char *path, bool bake_material);
void           context_select_tool(int i);
gpu_texture_t *gpu_create_render_target(int width, int height, int format);
void           viewport_capture_screenshot_to(gpu_texture_t *target, float x, float y, float w, float h);
void           viewport_save_texture(gpu_texture_t *screenshot);
void           project_reimport_mesh_skinned(int frame);
void           iron_delay_idle_sleep(void);

// --- Printf / string formatting ---
int   printf(char *fmt, ...);
char *string(char *fmt, ...);

// End API
