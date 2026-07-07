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
