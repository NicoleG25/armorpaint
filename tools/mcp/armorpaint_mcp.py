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
    """List the current project's layers as JSON (index, id, name)."""
    return _send("list_layers")


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
