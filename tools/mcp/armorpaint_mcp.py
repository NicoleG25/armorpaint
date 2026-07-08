# /// script
# requires-python = ">=3.10"
# dependencies = ["mcp>=1.2.0"]
# ///
"""
ArmorPaint MCP bridge.

Exposes ArmorPaint's in-app command server (127.0.0.1:4519, added in the custom
Intel build) as MCP tools so Claude can drive a live ArmorPaint session:
new project from a primitive, import a mesh, add solid PBR fill layers, list
layers, and export textures / mesh / material.

Run standalone for a smoke test:  uv run armorpaint_mcp.py --selftest
"""

import os
import socket
import subprocess
import sys
import time

from mcp.server.fastmcp import FastMCP

HOST = "127.0.0.1"
PORT = int(os.environ.get("ARMORPAINT_MCP_PORT", "4519"))
APP_PATH = os.environ.get("ARMORPAINT_APP", "/Applications/ArmorPaint.app")
CONNECT_TIMEOUT = float(os.environ.get("ARMORPAINT_CONNECT_TIMEOUT", "30"))

mcp = FastMCP("armorpaint")


def _port_open() -> bool:
    try:
        with socket.create_connection((HOST, PORT), timeout=1):
            return True
    except OSError:
        return False


def _ensure_running() -> None:
    """Connect if ArmorPaint is up; otherwise launch it and wait for the port."""
    if _port_open():
        return
    if not os.path.exists(APP_PATH):
        raise RuntimeError(
            f"ArmorPaint not running and app not found at {APP_PATH}. "
            "Set ARMORPAINT_APP to the .app path."
        )
    subprocess.run(["open", APP_PATH], check=True)
    deadline = time.time() + CONNECT_TIMEOUT
    while time.time() < deadline:
        if _port_open():
            return
        time.sleep(0.5)
    raise RuntimeError("Launched ArmorPaint but its command server never opened.")


def _send(line: str, read_timeout: float = 15.0) -> str:
    """Send one tab-delimited command, return the server's single-line reply."""
    _ensure_running()
    with socket.create_connection((HOST, PORT), timeout=5) as s:
        s.sendall((line + "\n").encode())
        s.settimeout(read_timeout)
        chunks = []
        while True:
            try:
                data = s.recv(4096)
            except socket.timeout:
                break
            if not data:
                break
            chunks.append(data)
            if b"\n" in data:
                break
    reply = b"".join(chunks).decode(errors="replace").strip()
    if reply.startswith("ERR\t"):
        raise RuntimeError(reply[4:])
    if reply.startswith("OK\t"):
        return reply[3:]
    return reply or "<no reply>"


# --- tools ------------------------------------------------------------------

@mcp.tool()
def ap_status() -> str:
    """Check the ArmorPaint command server is reachable (launches the app if needed)."""
    return _send("ping")


@mcp.tool()
def ap_new_project(primitive_type: int = 0) -> str:
    """Start a fresh project on a built-in primitive mesh.
    primitive_type indexes ArmorPaint's default mesh list (0 = default box/rounded cube).
    Returns after the new project is created."""
    return _send(f"new_project\t{primitive_type}", read_timeout=30)


@mcp.tool()
def ap_open_project(path: str) -> str:
    """Open an existing .arm ArmorPaint project file by absolute path."""
    return _send(f"open_project\t{path}", read_timeout=30)


@mcp.tool()
def ap_import_mesh(path: str) -> str:
    """Import a mesh (.obj/.fbx/.blend/etc.) to paint on, by absolute path."""
    return _send(f"import_mesh\t{path}", read_timeout=60)


@mcp.tool()
def ap_fill_layer(
    color_hex: str = "ffcccccc",
    roughness: float = 1.0,
    metallic: float = 0.0,
    occlusion: float = 1.0,
) -> str:
    """Add a new solid PBR fill layer.
    color_hex is AARRGGBB (e.g. 'ffcc3020' = opaque red-orange).
    roughness/metallic/occlusion are 0..1. Returns the new layer id."""
    return _send(f"fill_layer\t{color_hex}\t{roughness}\t{metallic}\t{occlusion}")


@mcp.tool()
def ap_list_layers() -> str:
    """List the layer stack as JSON. Each entry: index, id, name, kind
    (layer/mask/group/filter), blend (blend_type_t), opacity, visible, parent
    (index of the group/layer it belongs to, -1 if none), selected."""
    return _send("list_layers")


# --- layer stack: artist-style buildup (base + masked wear/grime) -----------
#
# The realism unlock: instead of one flat graph, stack real layers the way a texture
# artist does — a base material, then wear/grime layers ON TOP, each revealed by a
# mask. Fill a mask with a mesh bake (curvature = edge wear, occlusion = cavity dirt)
# or a procedural grunge graph, so the wear follows the geometry and reads as real.
#
# Typical loop:
#   base:  a recipe / graph  -> ap_material_fill_layer
#   wear:  ap_add_layer -> ap_paint_color(rustcolor, rough=0.9) (or a graph fill)
#          ap_set_layer_blend(2)  # multiply grime, or leave 0 to overlay
#          ap_add_mask -> ap_bake(0)  # curvature into the mask = edge-only wear
#   grime: ap_add_layer -> ... -> ap_add_mask -> ap_bake(10)  # AO into mask = cavities


@mcp.tool()
def ap_add_layer() -> str:
    """Add a new empty paint layer on top of the selected one and select it.
    Returns {index, id}. Build a base first, then stack wear layers with this."""
    return _send("add_layer")


@mcp.tool()
def ap_add_group() -> str:
    """Add a group (folder) at the top and select it. A group's mask/opacity apply
    to all layers inside it. Returns {index, id}."""
    return _send("add_group")


@mcp.tool()
def ap_add_mask() -> str:
    """Add a mask to the SELECTED layer/group and select the mask, so the next
    ap_bake / ap_paint / ap_fill_mask_material fills it. A bright mask reveals the
    layer, black hides it. Returns {index, id}."""
    return _send("add_mask")


@mcp.tool()
def ap_select_layer(index: int) -> str:
    """Select a slot by its index (from ap_list_layers) so subsequent
    bake/paint/blend/opacity ops target it."""
    return _send(f"select_layer\t{index}")


@mcp.tool()
def ap_set_layer_blend(blend_type: int = 0) -> str:
    """Set the SELECTED layer's blend mode. blend_type_t: 0 mix, 1 darken,
    2 multiply, 4 lighten, 5 screen, 6 dodge, 7 add, 8 overlay, 9 soft-light,
    10 linear-light, 12 subtract. Multiply = grime darkening; screen/add = glow."""
    return _send(f"set_layer_blend\t{blend_type}")


@mcp.tool()
def ap_set_layer_opacity(opacity: float = 1.0) -> str:
    """Set the SELECTED layer's opacity (0..1). On a mask, scales its strength —
    lower it to make wear subtle."""
    return _send(f"set_layer_opacity\t{opacity}")


@mcp.tool()
def ap_fill_mask_material() -> str:
    """Fill the SELECTED mask procedurally from the active material graph (build a
    grayscale noise/voronoi grunge graph, set it as the material, then call this).
    The material's base color becomes the mask value."""
    return _send("fill_mask_material", read_timeout=30)


@mcp.tool()
def ap_export_textures(directory: str, texture_type: str = "png", preset: str = "generic") -> str:
    """Export the visible layers as texture maps into directory (created if missing).
    texture_type: png | jpg | exr16 | exr32.
    preset names an export_presets/*.json: generic (base/nor/occ/rough/metal),
    unity (base/nor/mos/height), unreal, base_color, specular, etc."""
    return _send(
        f"export_textures\t{texture_type}\t{preset}\t{directory}", read_timeout=60
    )


@mcp.tool()
def ap_export_mesh(directory: str) -> str:
    """Export the current mesh into directory."""
    return _send(f"export_mesh\t{directory}", read_timeout=60)


@mcp.tool()
def ap_export_material(path: str) -> str:
    """Export the current material to a .arm file at path."""
    return _send(f"export_material\t{path}", read_timeout=60)


# --- material node graphs ---------------------------------------------------

import json  # noqa: E402


@mcp.tool()
def ap_get_material_json() -> str:
    """Return the default material node graph as JSON (a template to edit and send
    back with ap_set_material_json). Note the typed keys, e.g. 'default_value[f32]'
    for float arrays — the encoder requires those exact keys."""
    return _send("get_material_json", read_timeout=30)


@mcp.tool()
def ap_set_material_json(graph_json: str) -> str:
    """Replace the active material's node graph with graph_json (a full canvas:
    {name, nodes, links}) and rebake. graph_json must be compact (no raw newlines)
    and use the typed keys from ap_get_material_json. Returns the node count."""
    compact = json.dumps(json.loads(graph_json), separators=(",", ":"))
    return _send("set_material_json\t" + compact, read_timeout=60)


@mcp.tool()
def ap_material_fill_layer() -> str:
    """Bake the active material's node graph into a new fill layer (so it shows up
    in exports). Call after ap_set_material_json / a recipe."""
    return _send("material_fill_layer", read_timeout=30)


# Minimal RGB -> Material Output canvas. Recipes edit this and apply it.
def _solid_canvas(base_rgba, roughness, metallic, occlusion):
    return {
        "name": "Material",
        "nodes": [
            {
                "id": 1, "name": "Color", "type": "RGB", "x": 122.0, "y": 100.0,
                "color": -5025958, "inputs": [],
                "outputs": [{
                    "id": 0, "node_id": 1, "name": "Color", "type": "RGBA",
                    "color": -3684567, "default_value[f32]": list(base_rgba),
                    "min": 0.0, "max": 1.0, "precision": 100.0, "display": 0,
                }],
                "buttons": [{
                    "name": "default_value", "type": "RGBA", "output": 0,
                    "default_value[f32]": [0.0], "data": None,
                    "min": 0.0, "max": 1.0, "precision": 100.0, "height": 0.0,
                }],
                "width": 0.0, "flags": 0,
            },
            {
                "id": 0, "name": "Material Output", "type": "OUTPUT_MATERIAL_PBR",
                "x": 386.0, "y": 100.0, "color": -5025958,
                "inputs": [
                    _sock(0, 0, "Base Color", "RGBA", list(base_rgba)),
                    _sock(1, 0, "Opacity", "VALUE", [1.0]),
                    _sock(2, 0, "Occlusion", "VALUE", [occlusion]),
                    _sock(3, 0, "Roughness", "VALUE", [roughness]),
                    _sock(4, 0, "Metallic", "VALUE", [metallic]),
                    _sock(5, 0, "Normal Map", "VECTOR", [0.5, 0.5, 1.0]),
                    _sock(6, 0, "Emission", "VALUE", [0.0]),
                    _sock(7, 0, "Height", "VALUE", [0.0]),
                    _sock(8, 0, "Subsurface", "VALUE", [0.0]),
                ],
                "outputs": [], "buttons": [], "width": 0.0, "flags": 0,
            },
        ],
        "links": [{"id": 0, "from_id": 1, "from_socket": 0, "to_id": 0, "to_socket": 0}],
    }


def _sock(sid, node_id, name, typ, value):
    return {
        "id": sid, "node_id": node_id, "name": name, "type": typ,
        "color": -3684567, "default_value[f32]": value,
        "min": 0.0, "max": 1.0, "precision": 100.0, "display": 0,
    }


def _hex_to_rgba(color_hex):
    h = color_hex.lstrip("#")
    if len(h) == 8:  # AARRGGBB
        a, r, g, b = (int(h[i:i + 2], 16) / 255 for i in (0, 2, 4, 6))
    else:            # RRGGBB
        r, g, b = (int(h[i:i + 2], 16) / 255 for i in (0, 2, 4))
        a = 1.0
    return [r, g, b, a]


@mcp.tool()
def ap_material_solid(
    color_hex: str = "ffcccccc",
    roughness: float = 0.5,
    metallic: float = 0.0,
    occlusion: float = 1.0,
    bake: bool = True,
) -> str:
    """Author a solid PBR material via the node graph (base color + roughness +
    metallic + occlusion) and apply it. color_hex is AARRGGBB or RRGGBB. If bake,
    also creates a fill layer so it appears in exports. Unlike ap_fill_layer this
    goes through the material graph, so it composes with future procedural nodes."""
    canvas = _solid_canvas(_hex_to_rgba(color_hex), roughness, metallic, occlusion)
    r = _send("set_material_json\t" + json.dumps(canvas, separators=(",", ":")),
              read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


# --- incremental node-graph building ----------------------------------------


def _floats(values):
    return "\t".join(repr(float(v)) for v in values)


@mcp.tool()
def ap_clear_material() -> str:
    """Reset the active material to just its Material Output node (keeps the output,
    drops everything else and all links). Start procedural graphs from here.
    Returns the output node's id."""
    return _send("clear_material")


@mcp.tool()
def ap_add_node(node_type: str) -> str:
    """Add a material node to the active material graph. Returns {id, inputs, outputs}
    (socket counts). Common types: RGB, TEX_NOISE, TEX_VORONOI, VALTORGB (color ramp),
    MIX_RGB, MAPPING, BUMP, NORMAL_MAP. Output PBR node is OUTPUT_MATERIAL_PBR (id 0).
    Call ap_describe_node(type) first to get socket indices + button enums."""
    return _send(f"add_node\t{node_type}")


# Node schema: socket names by INDEX + button enums, so a graph can be wired without
# guess-and-render. Socket COUNTS were probed live against this ArmorPaint build; names
# follow Blender convention; the indices the recipes rely on (Scale, Fac, blend mode,
# band direction, ramp stops, PBR inputs) are empirically confirmed. Types absent here
# (TEX_MUSGRAVE, SEPARATE_XYZ, INVERT, MAP_RANGE, VECTOR_MATH...) do NOT exist in this
# build — add_node rejects them.
NODE_SCHEMA = {
    "OUTPUT_MATERIAL_PBR": {
        "inputs": ["Base Color", "Opacity", "Occlusion", "Roughness", "Metallic",
                   "Normal", "Emission", "Height", "Subsurface"],
        "outputs": [], "note": "The fixed output node, id 0 (returned by clear_material). Cannot be added."},
    "RGB": {"inputs": [], "outputs": ["Color"],
            "note": "Set color via ap_set_node_output(id,0,[r,g,b,a])."},
    "VALUE": {"inputs": [], "outputs": ["Value"]},
    "TEX_NOISE": {"inputs": ["Vector", "Scale", "Detail", "Roughness", "Lacunarity", "Distortion"],
                  "outputs": ["Fac", "Color"],
                  "note": "Recipes drive from output 0 (grayscale). Input 1 Scale = frequency."},
    "TEX_VORONOI": {"inputs": ["Vector", "Scale", "Smoothness", "Exponent", "Randomness", "W"],
                    "outputs": ["Distance", "Color", "Position"],
                    "note": "Output 0 Distance is dark at cell borders (cracks/scratches)."},
    "TEX_WAVE": {"inputs": ["Vector", "Scale", "Distortion", "Detail", "Detail Scale", "Detail Roughness"],
                 "outputs": ["Color", "Fac"],
                 "buttons": [{"index": 0, "name": "wave_type", "enum": {"0": "bands", "1": "rings"}},
                             {"index": 1, "name": "bands_direction", "enum": {"0": "X/horizontal", "1": "Y/vertical", "2": "Z", "3": "diagonal"}},
                             {"index": 2, "name": "wave_profile", "enum": {"0": "sine (rounded)", "1": "saw", "2": "triangle"}}],
                 "note": "Drive from output 1 Fac. button 1 flips flute direction (confirmed). Distortion 0 = straight."},
    "TEX_GRADIENT": {"inputs": ["Vector"], "outputs": ["Color", "Fac"]},
    "TEX_IMAGE": {"inputs": ["Vector"], "outputs": ["Color", "Alpha"],
                  "buttons": [{"index": 0, "name": "asset_index", "note": "float index from import_texture"},
                              {"index": 1, "name": "color_space", "enum": {"0": "auto", "1": "linear (data maps)", "2": "sRGB", "3": "DirectX normal"}}]},
    "MIX_RGB": {"inputs": ["Fac", "Color1", "Color2"], "outputs": ["Color"],
                "buttons": [{"index": 0, "name": "blend", "enum": {"0": "mix", "1": "darken", "2": "multiply", "4": "lighten", "5": "screen", "6": "dodge", "7": "add", "8": "overlay", "9": "soft-light", "10": "linear-light", "12": "subtract"}}]},
    "VALTORGB": {"inputs": ["Fac"], "outputs": ["Color", "Alpha"],
                 "buttons": [{"index": 0, "name": "color_ramp", "note": "N stops, each [r,g,b,a,position] (5 floats). Remaps 0..1 -> gradient."}],
                 "note": "Use to remap a scalar into a target range; feed a value into Fac."},
    "BUMP": {"inputs": ["Strength", "Distance", "Height", "Normal"], "outputs": ["Normal"],
             "note": "Link a grayscale height into input 2; output 0 -> PBR Normal (index 5)."},
    "NORMAL_MAP": {"inputs": ["Strength", "Color"], "outputs": ["Normal"],
                   "note": "Link a normal-map image Color into input 1; output 0 -> PBR Normal."},
    "MAPPING": {"inputs": ["Vector", "Location", "Rotation", "Scale"], "outputs": ["Vector"],
                "note": "Feed TEX_COORD into 0; Scale (3) tiles/stretches (anisotropy). Rotation is radians. NOTE: rotating coords does NOT reorient TEX_WAVE bands — use its bands_direction button."},
    "TEX_COORD": {"inputs": [], "outputs": ["Generated", "Normal", "UV", "Object", "Camera", "Window"],
                  "note": "Output 2 UV or 0 Generated feed a MAPPING/texture Vector."},
    "NEW_GEOMETRY": {"inputs": [], "outputs": ["Position", "Normal", "Tangent", "True Normal", "Incoming", "Pointiness"],
                     "note": "Pointiness is SCREEN-SPACE (keys on UV seams when baked) — for real edge wear use ap_bake curvature/occlusion, not this."},
    "MATH": {"inputs": ["A", "B"], "outputs": ["Value"],
             "buttons": [{"index": 0, "name": "operation", "enum": {"0": "add", "1": "subtract", "2": "multiply", "3": "divide", "10": "min", "11": "max", "13": "power"}, "note": "enum order may vary; verify"}]},
    "HUE_SAT": {"inputs": ["Hue", "Saturation", "Value", "Fac", "Color"], "outputs": ["Color"]},
    "GAMMA": {"inputs": ["Color", "Gamma"], "outputs": ["Color"]},
    "BRIGHTCONTRAST": {"inputs": ["Color", "Bright", "Contrast"], "outputs": ["Color"]},
    "CLAMP": {"inputs": ["Value", "Min", "Max"], "outputs": ["Result"]},
}


@mcp.tool()
def ap_describe_node(node_type: str = "") -> str:
    """Schema for a material node type: input names by INDEX, output names by index, and
    button names + enum values. Query this BEFORE wiring a node so you use the right
    socket/button indices instead of guess-and-render. Pass "" to list all known types.
    Socket counts are verified against this build; the indices the recipes rely on
    (Scale, Fac, blend, band direction, ramp stops, PBR inputs) are confirmed."""
    if not node_type:
        return json.dumps(sorted(NODE_SCHEMA.keys()))
    t = node_type.upper()
    if t not in NODE_SCHEMA:
        return json.dumps({"error": f"no schema for '{t}'", "known": sorted(NODE_SCHEMA.keys())})
    return json.dumps(NODE_SCHEMA[t], separators=(",", ":"))


@mcp.tool()
def ap_set_node_input(node_id: int, socket_index: int, values: list[float]) -> str:
    """Set an input socket's default value (used when the socket has no link).
    values is 1..4 floats (e.g. [r,g,b,a] for a color, [x] for a scalar)."""
    return _send(f"set_input\t{node_id}\t{socket_index}\t{_floats(values)}")


@mcp.tool()
def ap_set_node_output(node_id: int, socket_index: int, values: list[float]) -> str:
    """Set an output socket's default value (e.g. an RGB node's color)."""
    return _send(f"set_output\t{node_id}\t{socket_index}\t{_floats(values)}")


@mcp.tool()
def ap_link_nodes(from_id: int, from_socket: int, to_id: int, to_socket: int) -> str:
    """Link an output socket to an input socket. Sockets are addressed by INDEX.
    On OUTPUT_MATERIAL_PBR the input indices are: 0 Base Color, 1 Opacity,
    2 Occlusion, 3 Roughness, 4 Metallic, 5 Normal Map, 6 Emission, 7 Height,
    8 Subsurface."""
    return _send(f"link\t{from_id}\t{from_socket}\t{to_id}\t{to_socket}")


@mcp.tool()
def ap_commit_material() -> str:
    """Reparse the material shader and rebake fill layers after graph edits.
    Call once after building/editing a graph, before exporting."""
    return _send("commit_material", read_timeout=60)


@mcp.tool()
def ap_material_brushed_metal(
    color_hex: str = "ffb8b8c0",
    metallic: float = 1.0,
    noise_scale: float = 12.0,
    bake: bool = True,
) -> str:
    """Procedural brushed/worn metal: solid base color + metallic, with a noise
    texture driving roughness variation. Builds the graph from scratch and applies
    it. A concrete example of composing ap_add_node/ap_link_nodes into a recipe."""
    rgba = _hex_to_rgba(color_hex)
    out = json.loads(_send("clear_material"))["output_id"]  # _send already strips OK\t
    _send(f"set_input\t{out}\t0\t{_floats(rgba)}")       # base color
    _send(f"set_input\t{out}\t4\t{_floats([metallic])}")  # metallic
    noise = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"set_input\t{noise}\t1\t{_floats([noise_scale])}")  # noise scale
    _send(f"link\t{noise}\t0\t{out}\t3")                  # noise -> roughness
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


@mcp.tool()
def ap_set_node_button(node_id: int, button_index: int, values: list[float]) -> str:
    """Set a node BUTTON value (parameters that aren't sockets). Notably a VALTORGB
    color ramp: values is N stops of [r,g,b,a,position] (5 floats each), e.g.
    [0.4,0.4,0.4,1, 0.0,  0.7,0.7,0.7,1, 1.0] for a 2-stop grey ramp. Also enum
    selectors and bool toggles."""
    return _send(f"set_button\t{node_id}\t{button_index}\t{_floats(values)}")


# Helper: noise/value -> color ramp -> scalar, to remap 0..1 into a target range.
def _ramp_scalar(out, driver_id, driver_socket, dst_index, lo, hi):
    ramp = json.loads(_send("add_node\tVALTORGB"))["id"]
    _send(f"link\t{driver_id}\t{driver_socket}\t{ramp}\t0")            # driver -> ramp factor
    _send(f"set_button\t{ramp}\t0\t"
          f"{_floats([lo, lo, lo, 1.0, 0.0,  hi, hi, hi, 1.0, 1.0])}")  # 2 grey stops
    _send(f"link\t{ramp}\t0\t{out}\t{dst_index}")                      # ramp color -> dst
    return ramp


@mcp.tool()
def ap_material_worn_metal(
    color_light: str = "ffb8b098",
    color_dark: str = "ff5a5048",
    metallic: float = 1.0,
    detail_scale: float = 7.0,
    variation_scale: float = 2.5,
    bump_strength: float = 0.4,
    bake: bool = True,
) -> str:
    """Rich worn/oxidized metal built from a node graph: two noise textures drive
    (a) base-color variation between color_light and color_dark via a MIX node,
    (b) roughness variation, and (c) surface relief via a BUMP node into the normal.
    Far better than a flat fill. Socket indices: OUTPUT_MATERIAL_PBR 0 base,
    3 rough, 4 metal, 5 normal; MIX_RGB 0 fac/1 col1/2 col2; BUMP 2 height/out 0."""
    light, dark = _hex_to_rgba(color_light), _hex_to_rgba(color_dark)
    out = json.loads(_send("clear_material"))["output_id"]
    n_detail = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"set_input\t{n_detail}\t1\t{_floats([detail_scale])}")
    n_var = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"set_input\t{n_var}\t1\t{_floats([variation_scale])}")
    # base color = mix(light, dark) by broad noise
    mix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{mix}\t1\t{_floats(light)}")
    _send(f"set_input\t{mix}\t2\t{_floats(dark)}")
    _send(f"link\t{n_var}\t0\t{mix}\t0")      # noise -> mix factor
    _send(f"link\t{mix}\t0\t{out}\t0")        # mix -> base color
    # roughness variation
    _send(f"link\t{n_var}\t0\t{out}\t3")      # noise -> roughness
    _send(f"set_input\t{out}\t4\t{_floats([metallic])}")
    # surface relief: fine noise -> bump height -> normal
    bump = json.loads(_send("add_node\tBUMP"))["id"]
    _send(f"set_input\t{bump}\t0\t{_floats([bump_strength])}")  # strength
    _send(f"link\t{n_detail}\t0\t{bump}\t2")  # noise -> bump height
    _send(f"link\t{bump}\t0\t{out}\t5")       # bump normal -> output normal
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


# Helper: a TEX_NOISE whose coordinates are transformed by a MAPPING node, so the
# pattern can be scaled non-uniformly (anisotropic streaks) or rotated. Returns the
# noise node id. coord_out: TEX_COORD output (0 Generated, 2 UV).
def _mapped_noise(scale_xyz=(1.0, 1.0, 1.0), rotation_xyz=(0.0, 0.0, 0.0), coord_out=0):
    coord = json.loads(_send("add_node\tTEX_COORD"))["id"]
    mp = json.loads(_send("add_node\tMAPPING"))["id"]
    _send(f"link\t{coord}\t{coord_out}\t{mp}\t0")          # coord -> mapping Vector
    _send(f"set_input\t{mp}\t2\t{_floats(rotation_xyz)}")  # Rotation
    _send(f"set_input\t{mp}\t3\t{_floats(scale_xyz)}")     # Scale
    noise = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"link\t{mp}\t0\t{noise}\t0")                    # mapping -> noise Vector
    return noise


@mcp.tool()
def ap_material_brushed_steel(
    color_hex: str = "ffc2c4c8",
    streak: float = 30.0,
    metallic: float = 1.0,
    rough_lo: float = 0.2,
    rough_hi: float = 0.5,
    bake: bool = True,
) -> str:
    """Anisotropic brushed steel: a MAPPING node stretches noise along one axis
    (streak controls how much) so roughness and micro-relief read as directional
    brushing. Showcases MAPPING + TEX_COORD. Higher streak = finer/longer streaks."""
    rgba = _hex_to_rgba(color_hex)
    out = json.loads(_send("clear_material"))["output_id"]
    _send(f"set_input\t{out}\t0\t{_floats(rgba)}")
    _send(f"set_input\t{out}\t4\t{_floats([metallic])}")
    n = _mapped_noise(scale_xyz=(2.0, streak, 1.0))  # stretched along Y = brush lines
    _ramp_scalar(out, n, 0, 3, rough_lo, rough_hi)   # roughness streaks
    bump = json.loads(_send("add_node\tBUMP"))["id"]
    _send(f"set_input\t{bump}\t0\t{_floats([0.3])}")
    _send(f"link\t{n}\t0\t{bump}\t2")
    _send(f"link\t{bump}\t0\t{out}\t5")
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


@mcp.tool()
def ap_material_concrete(
    color_light: str = "ff9a9790",
    color_dark: str = "ff5d5a53",
    stain_hex: str = "ff43413b",
    cracks: bool = True,
    bump_strength: float = 0.55,
    bake: bool = True,
) -> str:
    """Realistic concrete, from scratch: multi-octave grey variation (macro stains x
    meso mottle + fine grain), optional VORONOI hairline cracks darkening the surface,
    correlated high roughness + relief, non-metallic. Good for walls/floors/bunkers."""
    light, dark, stain = _hex_to_rgba(color_light), _hex_to_rgba(color_dark), _hex_to_rgba(stain_hex)
    out = json.loads(_send("clear_material"))["output_id"]
    n_macro = json.loads(_send("add_node\tTEX_NOISE"))["id"]; _send(f"set_input\t{n_macro}\t1\t{_floats([2.5])}")
    n_meso = json.loads(_send("add_node\tTEX_NOISE"))["id"]; _send(f"set_input\t{n_meso}\t1\t{_floats([6.0])}")
    n_fine = json.loads(_send("add_node\tTEX_NOISE"))["id"]; _send(f"set_input\t{n_fine}\t1\t{_floats([15.0])}")
    # base tone: mix light<->dark by macro, then darken with meso stains
    base1 = _mix_by(light, dark, n_macro, 0)
    stainmix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{stainmix}\t2\t{_floats(stain)}")
    _send(f"link\t{base1}\t0\t{stainmix}\t1"); _send(f"link\t{n_meso}\t0\t{stainmix}\t0")
    _send(f"set_input\t{stainmix}\t0\t{_floats([0.35])}")  # subtle stains
    base_node, base_socket = stainmix, 0
    if cracks:
        voro = json.loads(_send("add_node\tTEX_VORONOI"))["id"]; _send(f"set_input\t{voro}\t1\t{_floats([9.0])}")
        # voronoi Distance is dark at cell borders -> hairline cracks; multiply darkens
        crackmix = _mix(stainmix, 0, voro, 0, blend=2)  # multiply cracks in
        base_node, base_socket = crackmix, 0
    _send(f"link\t{base_node}\t{base_socket}\t{out}\t0")
    _ramp_scalar(out, n_macro, 0, 3, 0.62, 0.9)   # high, varied roughness
    _send(f"set_input\t{out}\t4\t{_floats([0.0])}")  # non-metal
    bump = json.loads(_send("add_node\tBUMP"))["id"]; _send(f"set_input\t{bump}\t0\t{_floats([bump_strength])}")
    _send(f"link\t{n_fine}\t0\t{bump}\t2"); _send(f"link\t{bump}\t0\t{out}\t5")
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


def _mix_by(color1, color2, factor_node, factor_socket):
    """MIX_RGB(color1, color2) with factor from a node output. Returns mix node id."""
    m = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{m}\t1\t{_floats(color1)}")
    _send(f"set_input\t{m}\t2\t{_floats(color2)}")
    _send(f"link\t{factor_node}\t{factor_socket}\t{m}\t0")
    return m


@mcp.tool()
def ap_material_wood(
    color_light: str = "ff8a5a30",
    color_dark: str = "ff40260f",
    grain_scale: float = 4.0,
    distortion: float = 2.0,
    bake: bool = True,
) -> str:
    """Wood: a TEX_WAVE grain mixes two wood tones for the base, drives roughness
    variation, and adds grain relief via bump. Non-metallic."""
    light, dark = _hex_to_rgba(color_light), _hex_to_rgba(color_dark)
    out = json.loads(_send("clear_material"))["output_id"]
    wave = json.loads(_send("add_node\tTEX_WAVE"))["id"]
    _send(f"set_input\t{wave}\t1\t{_floats([grain_scale])}")    # scale
    _send(f"set_input\t{wave}\t2\t{_floats([distortion])}")     # distortion
    mix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{mix}\t1\t{_floats(light)}")
    _send(f"set_input\t{mix}\t2\t{_floats(dark)}")
    _send(f"link\t{wave}\t1\t{mix}\t0")     # wave Factor -> mix factor
    _send(f"link\t{mix}\t0\t{out}\t0")      # -> base
    _ramp_scalar(out, wave, 1, 3, 0.35, 0.65)  # roughness
    _send(f"set_input\t{out}\t4\t{_floats([0.0])}")
    bump = json.loads(_send("add_node\tBUMP"))["id"]
    _send(f"set_input\t{bump}\t0\t{_floats([0.4])}")
    _send(f"link\t{wave}\t1\t{bump}\t2")
    _send(f"link\t{bump}\t0\t{out}\t5")
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


@mcp.tool()
def ap_material_rubber(
    color_hex: str = "ff181818",
    roughness: float = 0.85,
    bump_strength: float = 0.25,
    bake: bool = True,
) -> str:
    """Matte rubber/plastic: dark solid base, high uniform-ish roughness with a touch
    of noise, non-metallic, faint surface relief. Good for tires, grips, trims."""
    rgba = _hex_to_rgba(color_hex)
    out = json.loads(_send("clear_material"))["output_id"]
    _send(f"set_input\t{out}\t0\t{_floats(rgba)}")
    _send(f"set_input\t{out}\t4\t{_floats([0.0])}")
    n = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"set_input\t{n}\t1\t{_floats([9.0])}")
    _ramp_scalar(out, n, 0, 3, max(0.0, roughness - 0.08), min(1.0, roughness + 0.05))
    bump = json.loads(_send("add_node\tBUMP"))["id"]
    _send(f"set_input\t{bump}\t0\t{_floats([bump_strength])}")
    _send(f"link\t{n}\t0\t{bump}\t2")
    _send(f"link\t{bump}\t0\t{out}\t5")
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


@mcp.tool()
def ap_material_knurled_metal(
    color_hex: str = "ff0e0e11",
    ribs: float = 42.0,
    metallic: float = 1.0,
    bump_strength: float = 0.8,
    rough_valley: float = 0.50,
    rough_crest: float = 0.24,
    horizontal: bool = False,
    bake: bool = True,
) -> str:
    """Knurled / fluted machined metal (e.g. an anodized black loupe or knob cap):
    fine parallel ribs from a TEX_WAVE bands pattern driving a rounded BUMP, dark
    metallic base, satin roughness keyed so rib crests catch light (smoother) and
    valleys stay matte, plus faint machining grain. `ribs` = flute count (wave scale).
    Flutes are vertical by default (wave band-direction button 1 = axis); set
    horizontal=True for the other axis. On the real cap mesh the direction ultimately
    follows the UVs. Great for grips, knobs, lens rings, machined hardware."""
    rgba = _hex_to_rgba(color_hex)
    out = json.loads(_send("clear_material"))["output_id"]
    _send(f"set_input\t{out}\t0\t{_floats(rgba)}")
    _send(f"set_input\t{out}\t4\t{_floats([metallic])}")
    wave = json.loads(_send("add_node\tTEX_WAVE"))["id"]
    _send(f"set_input\t{wave}\t1\t{_floats([ribs])}")   # scale = flute count
    _send(f"set_input\t{wave}\t2\t{_floats([0.0])}")    # no distortion: straight flutes
    # wave BUTTON 1 = bands direction enum; flips flutes between axes (default horizontal)
    _send(f"set_button\t{wave}\t1\t{_floats([0.0 if horizontal else 1.0])}")
    # rounded ribs (raw sine wave Fac) + faint machining grain, into one bump/normal
    fine = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"set_input\t{fine}\t1\t{_floats([85.0])}")
    height = _mix(wave, 1, fine, 0, blend=7, factor=0.04)  # add faint grain to ribs
    bump = json.loads(_send("add_node\tBUMP"))["id"]
    _send(f"set_input\t{bump}\t0\t{_floats([bump_strength])}")
    _send(f"link\t{height}\t0\t{bump}\t2"); _send(f"link\t{bump}\t0\t{out}\t5")
    # roughness keyed to the ribs: valleys matte, crests satin-smooth (descending ramp)
    _ramp_scalar(out, wave, 1, 3, rough_valley, rough_crest)
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


@mcp.tool()
def ap_material_painted_scratched(
    paint_hex: str = "ff203a6a",
    metal_hex: str = "ffb0b0b8",
    scratch_scale: float = 18.0,
    bake: bool = True,
) -> str:
    """Painted metal with scratches: a fine TEX_VORONOI reveals bare metal through the
    paint. Voronoi distance mixes paint<->metal base color, and drives metallic and
    roughness so scratches read as exposed metal. Great for crates/panels/props."""
    paint, metal = _hex_to_rgba(paint_hex), _hex_to_rgba(metal_hex)
    out = json.loads(_send("clear_material"))["output_id"]
    voro = json.loads(_send("add_node\tTEX_VORONOI"))["id"]
    _send(f"set_input\t{voro}\t1\t{_floats([scratch_scale])}")  # scale
    mix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{mix}\t1\t{_floats(paint)}")   # color1 = paint
    _send(f"set_input\t{mix}\t2\t{_floats(metal)}")   # color2 = metal
    _send(f"link\t{voro}\t0\t{mix}\t0")               # voronoi distance -> factor
    _send(f"link\t{mix}\t0\t{out}\t0")                # -> base color
    _send(f"link\t{voro}\t0\t{out}\t4")               # distance -> metallic (scratches metal)
    _ramp_scalar(out, voro, 0, 3, 0.25, 0.6)          # roughness: paint smooth, scratch rough
    bump = json.loads(_send("add_node\tBUMP"))["id"]
    _send(f"set_input\t{bump}\t0\t{_floats([0.6])}")
    _send(f"link\t{voro}\t0\t{bump}\t2")
    _send(f"link\t{bump}\t0\t{out}\t5")
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=30)
    return r


@mcp.tool()
def ap_screenshot(path: str) -> str:
    """Save ArmorPaint's own lit viewport (the material on the mesh, its lighting) to a
    PNG at path. This is the fast feedback loop for judging realism: author a material,
    screenshot, adjust — no external round-trip. Returns the image size."""
    return _send(f"screenshot\t{path}", read_timeout=30)


# --- brush painting (hand-placed detail) ------------------------------------

@mcp.tool()
def ap_paint_color(color_hex: str, roughness: float = 0.5, metallic: float = 0.0) -> str:
    """Set the brush paint color (+ roughness/metallic). The brush stamps the active
    material's output, so this reduces the material to a solid color. color_hex is
    RRGGBB or AARRGGBB. Call before ap_paint_dab/ap_paint_stroke; change it mid-stroke
    to paint multiple colors."""
    h = color_hex.lstrip("#")
    if len(h) == 8:
        h = h[2:]  # drop alpha for RRGGBB
    return _send(f"paint_color\t{h}\t{roughness}\t{metallic}", read_timeout=30)


@mcp.tool()
def ap_bake(bake_type: int = 0) -> str:
    """Bake mesh-derived data into the current layer as a real 3D mask: 0 curvature
    (edges), 3 height, 10 occlusion (AO / cavity dirt). Better than the GEOMETRY node's
    screen-space pointiness. Use the baked layer as a wear/dirt mask."""
    return _send(f"bake\t{bake_type}", read_timeout=60)


@mcp.tool()
def ap_paint_dab(u: float, v: float, radius: float = 0.25, opacity: float = 1.0, mode: int = 0) -> str:
    """Stamp the brush once onto the current paint layer. mode 0 = 3D (u,v are
    normalized screen coords 0..1; the brush raycasts onto the mesh in the viewport),
    mode 1 = 2D (UV space; needs the 2D view). Default brush color is the active
    swatch. Each dab needs a rendered frame, so space calls out slightly."""
    return _send(f"paint_dab\t{u}\t{v}\t{radius}\t{opacity}\t{mode}")


@mcp.tool()
def ap_paint_stroke(
    points: list[list[float]],
    radius: float = 0.15,
    opacity: float = 1.0,
    mode: int = 0,
    colors: list[str] = None,
) -> str:
    """Paint a CONTINUOUS brush stroke through points (a list of [u,v], screen coords
    for mode 0) — real brushing, connected segments, not isolated dots. Moves to the
    first point, then draws a connected segment to each next point (one render frame
    each). Optional colors is a per-point list of hex RRGGBB to blend color along the
    stroke (a gradient); it calls paint_color before each segment."""
    if not points:
        return json.dumps({"segments": 0})
    _send(f"paint_move\t{points[0][0]}\t{points[0][1]}")
    segs = 0
    for i, p in enumerate(points[1:], start=1):
        if colors and i - 1 < len(colors):
            _send(f"paint_color\t{colors[i-1].lstrip('#')[-6:]}\t0.4\t0.0", read_timeout=30)
        _send(f"paint_to\t{p[0]}\t{p[1]}\t{radius}\t{opacity}\t{mode}")
        time.sleep(0.11)  # one segment per rendered frame
        segs += 1
    return json.dumps({"segments": segs})


# --- real scanned textures (the realism path) -------------------------------

# TEX_IMAGE color spaces: 0 Auto, 1 Linear, 2 sRGB, 3 DirectX Normal.
def _image_node(path, color_space=0):
    idx = json.loads(_send(f"import_texture\t{path}", 15.0))["index"]
    nid = json.loads(_send("add_node\tTEX_IMAGE"))["id"]
    _send(f"set_button\t{nid}\t0\t{float(idx)}")     # asset index
    _send(f"set_button\t{nid}\t1\t{float(color_space)}")
    return nid


@mcp.tool()
def ap_import_texture(path: str) -> str:
    """Import an image file into ArmorPaint's project textures so a TEX_IMAGE node can
    reference it. Returns its asset index. Real scanned PBR maps are the path to
    photoreal materials (procedural noise tops out at stylized)."""
    return _send(f"import_texture\t{path}", read_timeout=20)


@mcp.tool()
def ap_add_image_texture(path: str, color_space: int = 0) -> str:
    """Import an image and add a TEX_IMAGE node for it. Returns the node id. Output 0
    is Color, 1 is Alpha. color_space: 0 Auto, 1 Linear (data maps), 2 sRGB, 3 DirectX
    normal. Link its Color into the graph with ap_link_nodes."""
    nid = _image_node(path, color_space)
    return json.dumps({"id": nid})


@mcp.tool()
def ap_material_from_pbr_set(
    albedo: str,
    normal: str = "",
    rough: str = "",
    metal: str = "",
    ao: str = "",
    bake: bool = True,
) -> str:
    """Photoreal material from a set of real scanned PBR map files (absolute paths).
    albedo is required; normal/rough/metal/ao optional. Wires each map to the right
    OUTPUT_MATERIAL_PBR input (normal via a NORMAL_MAP node). This is the realistic
    path — feed it a Poly Haven / scanned texture set."""
    out = json.loads(_send("clear_material"))["output_id"]
    a = _image_node(albedo, 0)          # albedo (auto/sRGB)
    _send(f"link\t{a}\t0\t{out}\t0")
    if rough:
        r = _image_node(rough, 1)
        _send(f"link\t{r}\t0\t{out}\t3")
    if metal:
        m = _image_node(metal, 1)
        _send(f"link\t{m}\t0\t{out}\t4")
    if ao:
        o = _image_node(ao, 1)
        _send(f"link\t{o}\t0\t{out}\t2")
    if normal:
        n = _image_node(normal, 1)
        nm = json.loads(_send("add_node\tNORMAL_MAP"))["id"]
        _send(f"link\t{n}\t0\t{nm}\t1")   # image color -> normal map input
        _send(f"link\t{nm}\t0\t{out}\t5")  # -> output normal
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=40)
    return r


def _mix(node_a, socket_a, node_b, socket_b, blend=0, factor=1.0):
    """Add a MIX_RGB combining two inputs. blend: 0 Mix,2 Multiply,5 Screen,7 Add.
    Returns the mix node id (output 0)."""
    m = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_button\t{m}\t0\t{float(blend)}")     # blend_type enum
    _send(f"set_input\t{m}\t0\t{_floats([factor])}")  # factor
    _send(f"link\t{node_a}\t{socket_a}\t{m}\t1")
    _send(f"link\t{node_b}\t{socket_b}\t{m}\t2")
    return m


def _fbm(base_scale, octaves=3):
    """Fractal (multi-octave) noise: sum TEX_NOISE octaves at rising scale and
    falling amplitude via add-mix. Real surfaces are fractal — single-scale noise
    reads soft and fake; FBM gives correlated macro+micro grit. Returns the top
    node id (grayscale on output 0)."""
    cur = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"set_input\t{cur}\t1\t{_floats([base_scale])}")
    amp, sc = 0.5, base_scale
    for _ in range(octaves - 1):
        sc *= 2.4
        nx = json.loads(_send("add_node\tTEX_NOISE"))["id"]
        _send(f"set_input\t{nx}\t1\t{_floats([sc])}")
        cur = _mix(cur, 0, nx, 0, blend=7, factor=amp)  # add octave
        amp *= 0.5
    return cur


@mcp.tool()
def ap_material_rusted_metal(
    steel_hex: str = "ff3a3d42",
    rust_dark_hex: str = "ff4a2a16",
    rust_light_hex: str = "ff8a4a24",
    rust_coverage: float = 0.5,
    edge_rust: bool = True,
    streaks: bool = True,
    bake: bool = True,
) -> str:
    """Realistic rusted metal, fully procedural (no imported textures). FBM (fractal
    multi-octave) rust mask AND-ed with big macro patches so bare-steel patches survive,
    optional curvature-baked EDGE rust (rust eats exposed edges) and de-streaked vertical
    drips. One shared mask drives base color, roughness, metallic and a gritty FBM bump so
    the channels stay correlated. Steel darkens with the macro and rust varies tonally, so
    it reads as metal THAT IS RUSTING, not a flat brown. From-scratch realism showcase."""
    steel = _hex_to_rgba(steel_hex)
    rust_d, rust_l = _hex_to_rgba(rust_dark_hex), _hex_to_rgba(rust_light_hex)
    steel_dark = [c * 0.72 for c in steel[:3]] + [1.0]  # darkened steel for tonal life

    # Optional: bake curvature edge mask (mesh-derived, still no imports) so rust
    # concentrates on exposed edges.
    edge_png = None
    if edge_rust:
        import tempfile
        _send("new_project\t0", 20); time.sleep(1.5)
        _send("bake\t0", 30); time.sleep(0.8)  # curvature
        ed = tempfile.mkdtemp(prefix="ap_edge_")
        _send(f"export_textures\tpng\tbase_color\t{ed}", 40); time.sleep(0.4)
        p = os.path.join(ed, "untitled_base.png")
        edge_png = p if os.path.exists(p) else None

    _send("new_project\t0", 20); time.sleep(1.5)
    out = json.loads(_send("clear_material"))["output_id"]

    # WHERE rust lives: big macro patches multiplied with FBM meso detail. The multiply
    # (AND) keeps bare-steel patches instead of the whole surface going uniformly rusty.
    n_macro = json.loads(_send("add_node\tTEX_NOISE"))["id"]
    _send(f"set_input\t{n_macro}\t1\t{_floats([2.5])}")
    patch = _fbm(3.0, octaves=3)
    combo = _mix(n_macro, 0, patch, 0, blend=2)  # multiply keeps steel patches

    mask_src, mask_socket = combo, 0
    if streaks:  # vertical drips, BROKEN UP by a cross noise so they don't comb-band
        n_streak = _mapped_noise(scale_xyz=(3.0, 9.0, 1.0))
        n_break = json.loads(_send("add_node\tTEX_NOISE"))["id"]
        _send(f"set_input\t{n_break}\t1\t{_floats([5.0])}")
        drip = _mix(n_streak, 0, n_break, 0, blend=2)            # multiply = irregular runs
        mask_src = _mix(combo, 0, drip, 0, blend=5, factor=0.3)  # screen in gently
        mask_socket = 0
    if edge_png:  # rust concentrates on exposed edges
        eid = _image_node(edge_png, 1)
        mask_src = _mix(mask_src, mask_socket, eid, 0, blend=7)  # add edge rust
        mask_socket = 0

    # coverage ramp -> final rust mask. High window + wide transition: only strong areas
    # rust fully, with a real partial-rust gradient between (FBM add-mix lifts the mean).
    ramp = json.loads(_send("add_node\tVALTORGB"))["id"]
    _send(f"link\t{mask_src}\t{mask_socket}\t{ramp}\t0")
    lo = max(0.0, rust_coverage - 0.08); hi = min(1.0, rust_coverage + 0.30)
    _send(f"set_button\t{ramp}\t0\t{_floats([0,0,0,1, lo,  1,1,1,1, hi])}")

    # rust tone varies with a fine FBM; steel darkens with the macro so neither is flat
    finecol = _fbm(12.0, octaves=2)
    rust_mix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{rust_mix}\t1\t{_floats(rust_d)}"); _send(f"set_input\t{rust_mix}\t2\t{_floats(rust_l)}")
    _send(f"link\t{finecol}\t0\t{rust_mix}\t0")
    steel_var = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{steel_var}\t1\t{_floats(steel)}"); _send(f"set_input\t{steel_var}\t2\t{_floats(steel_dark)}")
    _send(f"link\t{n_macro}\t0\t{steel_var}\t0")

    base_mix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"link\t{steel_var}\t0\t{base_mix}\t1")
    _send(f"link\t{rust_mix}\t0\t{base_mix}\t2"); _send(f"link\t{ramp}\t0\t{base_mix}\t0")
    _send(f"link\t{base_mix}\t0\t{out}\t0")

    # hard-correlated channels: bare steel smooth+metallic, rust matte+dielectric
    _ramp_scalar(out, ramp, 0, 3, 0.30, 0.95)  # roughness
    metal_mix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{metal_mix}\t1\t{_floats([1,1,1,1])}"); _send(f"set_input\t{metal_mix}\t2\t{_floats([0,0,0,1])}")
    _send(f"link\t{ramp}\t0\t{metal_mix}\t0"); _send(f"link\t{metal_mix}\t0\t{out}\t4")

    # gritty pitting relief: FBM micro detail into bump (multi-scale, correlated)
    grit = _fbm(20.0, octaves=3)
    bump = json.loads(_send("add_node\tBUMP"))["id"]; _send(f"set_input\t{bump}\t0\t{_floats([0.7])}")
    _send(f"link\t{grit}\t0\t{bump}\t2"); _send(f"link\t{bump}\t0\t{out}\t5")

    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=40)
    return r


@mcp.tool()
def ap_material_worn_edges(
    clean_hex: str = "ff33363c",
    worn_hex: str = "ffb0aea6",
    clean_rough: float = 0.5,
    worn_rough: float = 0.35,
    metallic: float = 1.0,
    mask_type: int = 0,
    bake: bool = True,
) -> str:
    """Realistic worn metal driven by a baked mesh mask: bakes curvature (mask_type 0)
    or occlusion (10), then mixes clean<->worn base color and roughness by that mask so
    edges show bare/worn metal (curvature) or cavities collect dirt (occlusion). This
    is the grunge payoff that procedural noise alone can't do — it follows the geometry."""
    import tempfile
    clean, worn = _hex_to_rgba(clean_hex), _hex_to_rgba(worn_hex)
    # 1. bake the mask into a layer and export it
    _send("new_project\t0", 20); time.sleep(1.5)
    _send(f"bake\t{mask_type}", 30); time.sleep(1.0)
    maskdir = tempfile.mkdtemp(prefix="ap_mask_")
    _send(f"export_textures\tpng\tbase_color\t{maskdir}", 40); time.sleep(0.5)
    maskpng = os.path.join(maskdir, "untitled_base.png")
    if not os.path.exists(maskpng):
        return "ERR: mask bake/export failed"
    # 2. build the worn material using the mask
    _send("new_project\t0", 20); time.sleep(1.5)
    out = json.loads(_send("clear_material"))["output_id"]
    m = _image_node(maskpng, 1)  # linear mask
    mix = json.loads(_send("add_node\tMIX_RGB"))["id"]
    _send(f"set_input\t{mix}\t1\t{_floats(clean)}")
    _send(f"set_input\t{mix}\t2\t{_floats(worn)}")
    _send(f"link\t{m}\t0\t{mix}\t0")     # mask -> mix factor
    _send(f"link\t{mix}\t0\t{out}\t0")   # -> base color
    _ramp_scalar(out, m, 0, 3, worn_rough, clean_rough)  # roughness by mask
    _send(f"set_input\t{out}\t4\t{_floats([metallic])}")
    r = _send("commit_material", read_timeout=60)
    if bake:
        r += " | " + _send("material_fill_layer", read_timeout=40)
    return r


# --- FAULTLINE Unity integration --------------------------------------------

FAULTLINE_ART = os.environ.get(
    "FAULTLINE_ART",
    "/Users/nicole/Documents/FAULTLINE/My project/Assets/Art/Materials",
)


@mcp.tool()
def ap_export_to_faultline(name: str, texture_type: str = "png") -> str:
    """Export the current material's PBR maps straight into the FAULTLINE Unity
    project at Assets/Art/Materials/<name>/, named <name>_base/_nor/_occ/_rough/
    _metal. Uses the 'generic' preset. After this, build the URP material in Unity
    with the ArmorPaint importer (Faultline menu) so import settings are correct.
    Override the destination root with the FAULTLINE_ART env var."""
    dest = os.path.join(FAULTLINE_ART, name)
    r = _send(f"export_textures\t{texture_type}\tgeneric\t{dest}", read_timeout=60)
    # ArmorPaint names maps from the (untitled) project file; rename to <name>_*.
    renamed = []
    if os.path.isdir(dest):
        for f in os.listdir(dest):
            if f.startswith("untitled_"):
                new = name + "_" + f[len("untitled_"):]
                os.replace(os.path.join(dest, f), os.path.join(dest, new))
                renamed.append(new)
    return f"{r} -> {dest} ({', '.join(sorted(renamed)) or 'no maps renamed'})"


def _selftest() -> int:
    print("port open:", _port_open())
    for line in ("ping", "new_project\t0", "list_layers"):
        try:
            print(f">>> {line!r}\n<<< {_send(line, read_timeout=30)}")
        except Exception as e:  # noqa: BLE001
            print(f">>> {line!r}\n!!! {e}")
            return 1
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(_selftest())
    mcp.run()
