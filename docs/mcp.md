# ArmorPaint MCP server (live control from an external process)

This fork adds an in-app command server so an external process (e.g. an AI agent
via MCP) can drive a live ArmorPaint session: start a project from a primitive,
import a mesh, add solid PBR fill layers, list layers, and export textures / mesh /
material.

> **Fork note:** this is an **Intel / x86_64 macOS** fork. `base/tools/make.js`
> emits `ARCHS = x86_64` (upstream targets arm64) and the prebuilt
> `base/tools/bin/macos/amake` is an x86_64 rebuild. Building elsewhere requires
> reverting those.

## Architecture

```
external client ──TCP 127.0.0.1:4519──> ArmorPaint
  (tools/mcp/armorpaint_mcp.py)          paint/sources/mcp_server.c
                                          socket thread → queue →
                                          per-frame sys_notify_on_update →
                                          engine ops on the main thread
```

- **`paint/sources/mcp_server.c`** — background POSIX socket thread on
  `127.0.0.1:4519` (`ARMORPAINT_MCP_PORT` overrides). Reads tab-delimited line
  commands into a mutex-guarded queue. A per-frame `sys_notify_on_update` hook
  drains the queue and calls engine functions on the **main thread**, so all
  GC/engine access stays single-threaded (same discipline as the
  `sys_notify_on_next_frame` path in `args.c`). Included from `main.c`; started by
  `mcp_server_start()` after `base_init()`.
- **`paint/project.js`** sets `flags.idle_sleep = false` — the command loop must
  keep ticking to drain commands; with idle-sleep on, the main loop parks when the
  window is idle and commands never execute.

## Line protocol

Tab-delimited fields, newline-terminated. Reply is one line: `OK\t<json>` or `ERR\t<msg>`.

| Command | Effect |
|---|---|
| `ping` | `OK\tpong` |
| `new_project\t<int>` | New project on default mesh index `<int>` |
| `open_project\t<path>` | Open a `.arm` project |
| `import_mesh\t<path>` | Import a mesh to paint on |
| `fill_layer\t<AARRGGBB>\t<rough>\t<metal>\t<occ>` | Add solid PBR fill layer; returns id |
| `list_layers` | JSON array of `{index,id,name}` |
| `export_textures\t<type>\t<preset>\t<dir>` | Export maps (`type`: png/jpg/exr16/exr32) |
| `export_mesh\t<dir>` | Export current mesh |
| `export_material\t<path>` | Export current material `.arm` |
| `get_material_json` | Dump the default material node graph as JSON (a template) |
| `set_material_json\t<json>` | Replace the active material's node graph and rebake |
| `material_fill_layer` | Bake the active material's node graph into a new fill layer |
| `clear_material` | Keep the Material Output node, drop the rest + all links; returns output id |
| `add_node\t<TYPE>` | Add a node to the material graph; returns `{id,inputs,outputs}` |
| `set_input\t<node_id>\t<index>\t<f..>` | Set an input socket's default value |
| `set_output\t<node_id>\t<index>\t<f..>` | Set an output socket's default value (e.g. RGB color) |
| `set_button\t<node_id>\t<index>\t<f..>` | Set a node BUTTON (enum/bool, or a VALTORGB ramp) |
| `link\t<from_id>\t<from_socket>\t<to_id>\t<to_socket>` | Link output→input (sockets by **index**) |
| `commit_material` | Reparse the shader + rebake fill layers after edits |
| `add_layer` | New empty paint layer on top, selected; returns `{index,id}` |
| `add_group` | New group (folder) at top, selected; returns `{index,id}` |
| `add_mask` | Add a mask to the selected layer/group, select it; returns `{index,id}` |
| `select_layer\t<index>` | Select a slot so bake/paint/blend/opacity target it |
| `set_layer_blend\t<blend_type>` | Selected layer blend (0 mix, 2 multiply, 5 screen, 7 add, 8 overlay, ...) |
| `set_layer_opacity\t<0..1>` | Selected layer opacity (on a mask, its strength) |
| `fill_mask_material` | Fill the selected mask from the active material graph (procedural grunge) |

### Layer stack — artist-style buildup (the realism unlock)

Instead of one flat graph, stack real layers the way a texture artist does: a base
material, then wear/grime layers ON TOP, each revealed by a **mask**. Fill the mask
with a mesh **bake** (`bake 0` curvature = edge wear, `bake 10` occlusion = cavity
dirt) or a procedural grunge graph, so the wear follows the geometry and reads as
real. `list_layers` now returns `kind` (layer/mask/group/filter), `blend`, `opacity`,
`visible`, `parent` (index), `selected`.

A mask has no type field in the engine — it's a slot_layer with `texpaint` but no
`texpaint_nor` (see `slot_layer_is_mask`); it stores just before its owning layer in
the array, with `parent` pointing at that layer. `bake` and paint target
`g_context->layer`, so `add_mask` (which selects the new mask) then `bake` writes the
bake straight into the mask.

Worked loop (verified — base + curvature-masked multiply wear composites in the lit
preview):
```
paint_color 8899aa 0.4 1.0   # base metal
material_fill_layer
add_layer                    # wear layer on top
paint_color 553322 0.9 0.0   # rust color, rough, non-metal
set_layer_blend 2            # multiply = grime darkening
add_mask                     # mask on the wear layer, selected
bake 0                       # curvature into the mask = edge-only wear
```
Bridge: `ap_add_layer`, `ap_add_group`, `ap_add_mask`, `ap_select_layer`,
`ap_set_layer_blend`, `ap_set_layer_opacity`, `ap_fill_mask_material`.

Buttons hold parameters that aren't sockets. A VALTORGB (color ramp) button is N
stops of `[r,g,b,a,position]` (5 floats each) — set it to remap a 0..1 driver
(noise/voronoi) into a target range. Node schemas mapped so far: TEX_NOISE,
TEX_VORONOI (Distance/Color/Position out), TEX_WAVE (Color/Factor out), TEX_GRADIENT,
MIX_RGB (0 fac/1 col1/2 col2), BUMP (2 height in / 0 normal out), VALTORGB (0 factor
in / 0 color, 1 alpha out), MAPPING (0 vector/1 loc/2 rot/3 scale), TEX_COORD
(0 Generated/2 UV out). Insert TEX_COORD→MAPPING before a texture's Vector input to
scale/rotate the pattern (e.g. non-uniform scale = anisotropic brushed streaks).

Bridge recipes: `ap_material_solid`, `ap_material_brushed_metal`,
`ap_material_worn_metal`, `ap_material_brushed_steel` (anisotropic), `ap_material_concrete`,
`ap_material_wood`, `ap_material_rubber`, `ap_material_painted_scratched`, plus
primitives `ap_add_node`/`ap_set_node_input`/`ap_set_node_output`/`ap_set_node_button`/
`ap_link_nodes`/`ap_clear_material`/`ap_commit_material`.

The FAULTLINE importer (ArmorPaintMaterialImporter.Build(name, tiling, maxSize))
takes a UV tiling and a max texture size for texel density / VRAM control.

### Realism from scratch (no imports)

Pure single-noise recipes read as stylized. Realistic *procedural* materials come from
technique, not scanned maps:
- **Layer noise octaves** — a macro noise (patches/blotches) + a fine noise (grain);
  real surfaces have variation at multiple scales.
- **Correlate the PBR channels** — drive base color, roughness, metallic AND bump from
  ONE shared mask so they move together (rust is redder AND rougher AND non-metallic
  AND bumpier; clean metal is darker AND smoother AND reflective). Uncorrelated channels
  are the tell of a fake material.
- **Tame with color ramps** — VALTORGB remaps a 0..1 noise into a controlled coverage /
  contrast range instead of full-swing.
- **Bake-driven wear** — curvature/occlusion bakes are computed from the mesh (still
  from scratch), so use them for edge wear + cavity dirt that follows the geometry.

`ap_material_rusted_metal` is the worked example (macro+fine noise, a ramped rust mask
driving color/rough/metal/bump); `ap_material_worn_edges` adds a baked mask. Same
pattern extends to concrete, wood, fabric, etc. Verified: renders as believable rusted
metal in URP.

### Realistic materials (real scanned textures)

Procedural noise tops out at stylized. Photoreal comes from real scanned PBR maps,
loaded via `import_texture\t<path>` (adds to the project's texture assets, returns the
asset index) + a `TEX_IMAGE` node whose button 0 is that index and button 1 the color
space (0 Auto, 1 Linear for data maps, 2 sRGB, 3 DirectX normal). `TEX_IMAGE` output 0
is Color, 1 is Alpha; a normal map routes through a `NORMAL_MAP` node
(image Color -> NORMAL_MAP input 1 -> output normal 5).

Bridge: `ap_import_texture`, `ap_add_image_texture`, and `ap_material_from_pbr_set(
albedo, normal, rough, metal, ao)` — feed it a scanned/Poly Haven set for a photoreal
material. Verified end to end: a metal-plate PBR set renders as photoreal worn metal in
URP.

Note on edge wear: the GEOMETRY node's Pointiness output is screen-space (ddx/ddy of
the normal), so when baked in UV space it keys on UV seams rather than true 3D edges —
not a reliable wear mask. Real edge wear needs a curvature/AO bake (future work).

### Brush painting (hand-placed detail)

The third creation mode, alongside procedural graphs and scanned textures. `paint_dab
<u> <v> [radius] [opacity] [mode]` stamps the brush onto the current paint layer;
`paint_move <u> <v>` sets a stroke's start. mode 0 = 3D (u,v are normalized screen
coords; the brush raycasts onto the mesh via the rendered gbuffer — works headlessly),
mode 1 = 2D (UV space; needs the 2D view). Each dab consumes one rendered frame
(`pdirty`), so space calls ~one frame apart. Implementation: a `down` flag OR'd into
the brush uniform (uniforms.c) makes the brush paint when `pdirty>0` without a real
mouse. Continuous strokes: the paint shader fills the capsule between `last` and `current`,
so `paint_move <u> <v>` (set start) then repeated `paint_to <u> <v>` (each paints a
connected segment from the previous point) makes a real brush stroke — not isolated
dots. Change `paint_color` between segments to blend color along the stroke (gradient).
Bridge: `ap_paint_dab` (single stamp), `ap_paint_stroke([[u,v],...], colors=[...])`
(continuous, optional per-point colors). Verified: a dense stroke paints a solid
connected line with a red->yellow gradient.

Brush color: the brush stamps the ACTIVE MATERIAL's output color (make_paint.c
out_basecol), not a swatch. `paint_color <hex> [rough] [metal]` reduces the material to
a solid color so painting paints it; change it between dabs to paint multiple colors
(verified blue + red). Bridge `ap_paint_color`. To brush a procedural/textured material,
just set that material, then paint.

### Baking mesh masks

`bake <type>` bakes mesh-derived data into the current layer (a real 3D mask, unlike
the GEOMETRY node's screen-space pointiness): 0 curvature (edges), 3 height, 10
occlusion (AO / cavity dirt); sets tool=BAKE + bake_type, reparses the bake shader, and
paints one frame. Bridge `ap_bake`. Use the baked layer as a wear/dirt mask driving
roughness/mix. (Occlusion/thickness are raytrace bakes.)

### Building graphs incrementally

`clear_material` → `add_node` / `set_input` / `set_output` / `link` → `commit_material`
→ `material_fill_layer` → `export_textures`.

Nodes are built directly onto `g_context->material->canvas` (not the node-editor
view canvas) with deterministic ids. **Sockets are addressed by index** — the
material parser reads `outputs->buffer[from_socket]`, so links use socket indices,
not ids. `OUTPUT_MATERIAL_PBR` input indices: 0 Base Color, 1 Opacity, 2 Occlusion,
3 Roughness, 4 Metallic, 5 Normal Map, 6 Emission, 7 Height, 8 Subsurface.

Verified procedural: a `TEX_NOISE` node linked into Base Color and Roughness bakes a
real noise pattern (exported map stddev ~28–30 vs 0 for a flat fill). The bridge
wraps these as `ap_clear_material`, `ap_add_node`, `ap_set_node_input`,
`ap_set_node_output`, `ap_link_nodes`, `ap_commit_material`, plus an example recipe
`ap_material_brushed_metal(color, metallic, noise_scale)`.

### Material node graphs

A material is a node canvas (`{name, nodes, links}`). `set_material_json` runs the
same `json -> json_encode_to_armpack -> armpack_decode` path the app uses to load
`default_material.arm`, then reparses the paint shader and rebakes fill layers.
`get_material_json` returns the default graph as a starting template.

Array-typed socket values use **typed keys**, e.g. `"default_value[f32]"` — the
encoder requires these exact keys, so edit the template in place rather than adding
plain keys. Base color comes from the `RGB` node's output; roughness/metallic/etc.
are inputs on the `OUTPUT_MATERIAL_PBR` node. After `set_material_json`, call
`material_fill_layer` so the graph shows up in exports.

The bridge wraps these as `ap_get_material_json`, `ap_set_material_json`,
`ap_material_fill_layer`, plus a high-level `ap_material_solid(color, roughness,
metallic, occlusion)` recipe. Procedural recipes (noise/voronoi/color-ramp driven)
are the next additions.

## MCP bridge

`tools/mcp/armorpaint_mcp.py` is a self-contained [uv](https://docs.astral.sh/uv/)
script (PEP 723 inline deps). It auto-launches `/Applications/ArmorPaint.app`
(`ARMORPAINT_APP` overrides) if the port is closed, and exposes MCP tools
`ap_status`, `ap_new_project`, `ap_open_project`, `ap_import_mesh`, `ap_fill_layer`,
`ap_list_layers`, `ap_export_textures`, `ap_export_mesh`, `ap_export_material`.

Smoke test:
```bash
uv run tools/mcp/armorpaint_mcp.py --selftest
```

Register with Claude Code:
```bash
claude mcp add armorpaint --scope user -- uv run /abs/path/tools/mcp/armorpaint_mcp.py
```

## Extending

Add a command branch in `mcp_server.c` calling any function declared in
`paint/sources/functions.h`, then a matching tool in `armorpaint_mcp.py`, and
rebuild. Natural next ops: material node-graph assembly, brush strokes, curvature/AO
baking.
