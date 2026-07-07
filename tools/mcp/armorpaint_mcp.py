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
def ap_export_textures(directory: str, texture_type: str = "png", preset: str = "default") -> str:
    """Export the visible layers as texture maps into directory.
    texture_type: png | jpg | exr16 | exr32. preset names an export_presets/*.json."""
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
