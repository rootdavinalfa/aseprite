# Native MCP Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the shipped Aseprite binary itself an MCP server (stdio + local HTTP, dual-era protocol, full 116-tool diivi parity) with an in-app setup dialog — no Python, no uv.

**Architecture:** A new `src/app/mcp/` subsystem. Transport-agnostic protocol core (`mcp_protocol`) speaks both modern (`2026-07-28`, stateless `_meta`) and legacy (`initialize` handshake) MCP. Tool handlers are Lua snippets executed in-process through `script::Engine` (the same API diivi drives via subprocess), with native C++ only for fonts, image content, and the preview server. Two transports share one dispatcher: `--mcp` stdio loop (headless) and `ix::HttpServer` POST `/mcp` (GUI toggle or `--mcp-http` headless).

**Tech Stack:** C++17, json11 (bundled), IXWebSocket `ix::HttpServer` (bundled), Aseprite Lua engine (`script::Engine`), FreeType via `laf/ft`, existing `os::Event` callback queue, `base::encode_base64`, `clip` text clipboard.

**Spec:** `docs/superpowers/specs/2026-10-13-aseprite-native-mcp-design.md`

## Global Constraints

- C++17; Aseprite house style (2-space indent, `#ifdef HAVE_CONFIG_H #include "config.h"`, EULA copyright header block, include guards `APP_MCP_<NAME>_H_INCLUDED`, `namespace app { namespace mcp {`).
- All MCP sources compile only under `ENABLE_SCRIPTING`; HTTP code (`mcp_server.cpp` HTTP parts, preview) additionally under `ENABLE_WEBSOCKET`.
- No new third-party dependencies. Allowed: json11, ixwebsocket (already vendored/linked under `ENABLE_WEBSOCKET`), `base::encode_base64` (`laf/base/base64.h`), `clip::set_text` (`laf/clip/clip.h`), `ft::Lib` (`laf/ft/lib.h`).
- Protocol constants: modern version `"2026-07-28"`; legacy supported list `{"2024-11-05","2025-03-26","2025-06-18","2025-11-25"}`; legacy fallback reply `"2025-06-18"`; `ttlMs` `86400000`; `cacheScope` `"public"`; `serverInfo` `{"name":"aseprite","version":get_app_version()}` (`ver/info.h`).
- Error codes: `-32700` parse, `-32600` invalid request, `-32601` unknown method, `-32602` unknown tool/invalid params, `-32603` internal, `-32020` HeaderMismatch, `-32022` UnsupportedProtocolVersion.
- Tool semantics: text-first results; `isError:true` when Lua fails, throws, or output starts with `ERROR:`; image content only where the tool produced a `.png`.
- Tool reference: diivi/aseprite-mcp pinned at `90d1696a7e41edff89bbd0823ae6a5f86c114bcc` (`aseprite_mcp/tools/*.py`, 116 `@mcp.tool` registrations). Mirror names + input schemas; write descriptions and handlers fresh (no verbatim copying).
- Preferences: `mcp.enabled` (bool, default `false`), `mcp.port` (int, default `3777`).
- Tests: bash scripts in `tests/cli/*.sh`, `source`d by `tests/run-tests.sh`; use `$ASEPRITE`, `$t`, `fail`; `python3` allowed for JSON assertions; json11 serializes object keys sorted (deterministic output).
- Build: `cmake --build build -j8` from repo root; binary at `build/bin/aseprite`.
- Commits: one per task minimum, imperative subject ≤ 72 chars.

## Review Focus

1. **Stray stdout writes corrupting stdio framing** — any `print`/warning during a tool call must not reach stdout. Test pinned in Task 1 (junk input, all non-empty stdout lines parse as JSON) and Task 3 (a tool that prints).
2. **Tricky string arguments** (quotes, backslashes, newlines, non-ASCII) must survive JSON → Lua literal → output round-trip. Test in Task 3.
3. **Concurrent HTTP requests** must not corrupt engine/document state. Test in Task 11 (two parallel curls).
4. **Port already in use** must produce a clear error and non-zero exit, not a crash or hang. Test in Task 11.
5. **Externally modified file vs. document cache** must reload (never serve stale metadata). Test in Task 3.

---

### Task 1: `--mcp` stdio mode, protocol core, empty registry

**Files:**
- Create: `src/app/mcp/mcp_protocol.h`, `src/app/mcp/mcp_protocol.cpp`
- Create: `src/app/mcp/mcp_server.h`, `src/app/mcp/mcp_server.cpp`
- Create: `src/app/mcp/mcp_tools_registry.h`, `src/app/mcp/mcp_tools_registry.cpp`
- Create: `tests/cli/mcp-stdio.sh`
- Modify: `src/app/cli/app_options.h` (add `Option& m_mcp;`, `bool m_startMcp;`, `bool startMcp() const`)
- Modify: `src/app/cli/app_options.cpp` (define `--mcp`; parse; extend the no-UI condition at the existing `if (m_startShell || m_showHelp || m_showVersion || m_po.enabled(m_batch))` to include `m_startMcp`)
- Modify: `src/app/app.h` (add `bool m_isMcp;` next to `m_isShell`)
- Modify: `src/app/app.cpp` (`App::initialize`: `m_isMcp = options.startMcp();`; `App::run()`: before the `m_isShell` branch, `if (m_isMcp) { mcp::McpServer::instance()->runStdio(*m_engine, *context()); }`)
- Modify: `src/app/CMakeLists.txt` (add the six files to the `target_sources(app-lib PRIVATE ...)` list inside the `if(ENABLE_SCRIPTING)` block)

**Interfaces:**
- Produces (used by every later task):
  - `app::mcp::protocol`: `enum class Era { Modern, Legacy };`, `extern const char kModernVersion[];`, `extern const char* const kLegacyVersions[];`, `extern const char kDefaultLegacyVersion[];`, `Era detectEra(const json11::Json& request);`, `bool isNotification(const json11::Json& request);`, `json11::Json handle(const json11::Json& request, Era era);` (returns `json11::Json()` for notifications), `json11::Json makeError(const json11::Json& id, int code, const std::string& message, const json11::Json& data = json11::Json());`
  - `app::mcp::McpServer::instance() -> McpServer*`, `void runStdio(script::Engine& engine, Context& context);`, private `json11::Json handleMessage(const json11::Json& request);` (era detection + `protocol::handle`; Task 11 reuses it from the HTTP path)
  - `app::mcp::ToolResult { bool isError = false; std::string text; std::string imageMime; std::string imageBase64; };`
  - `app::mcp::McpTool { std::string name; std::string description; json11::Json inputSchema; std::function<ToolResult(const json11::Json& args)> run; };`
  - `const std::vector<McpTool>& mcp_tools();` (static, deterministic order)

- [ ] **Step 1: Write the failing test** — `tests/cli/mcp-stdio.sh`

```bash
echo "MCP stdio: server/discover"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"supportedVersions":\["2026-07-28"\]' || fail "discover supportedVersions: $resp"
echo "$resp" | grep -q '"resultType":"complete"' || fail "discover resultType: $resp"
echo "$resp" | grep -q '"io.modelcontextprotocol/serverInfo"' || fail "discover serverInfo: $resp"

echo "MCP stdio: tools/list is empty"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"tools":\[\]' || fail "empty tools list: $resp"

echo "MCP stdio: junk line -> -32700"
resp=$(printf '%s\n' 'not json' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '\-32700' || fail "parse error: $resp"

echo "MCP stdio: unknown method -> -32601"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"nope"}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '\-32601' || fail "method not found: $resp"

echo "MCP stdio: stdout is pure JSON, exit 0 on EOF"
printf '%s\n' '{"jsonrpc":"2.0","id":4,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' \
  | "$ASEPRITE" --mcp \
  | python3 -c 'import sys,json; [json.loads(l) for l in sys.stdin if l.strip()]' \
  || fail "stdout was not pure JSON"
printf '' | "$ASEPRITE" --mcp || fail "EOF shutdown returned non-zero"
```

- [ ] **Step 2: Run test to verify it fails**

From `tests/`: `ASEPRITE="$PWD/../build/bin/aseprite" bash run-tests.sh mcp-stdio`
Expected: FAIL (`--mcp` unknown option).

- [ ] **Step 3: Implement.** `mcp_protocol.cpp`: `detectEra` returns `Modern` when `request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"]` is a string, else `Legacy`. `isNotification` true when `request["id"]` is missing or null. `handle` dispatches modern methods `server/discover` (result: `resultType:"complete"`, `supportedVersions` = {`kModernVersion`}, `capabilities: {"tools":{"listChanged":false}}`, `instructions` = "Aseprite pixel-art server. Tools mirror diivi/aseprite-mcp: create/load a .aseprite file, draw with the drawing tools, inspect with export_frame at 8-10x scale, iterate. run_lua_script is the escape hatch.", `ttlMs`, `cacheScope`, `_meta` serverInfo), `tools/list` (from `mcp_tools()`, adds `resultType`/`ttlMs`/`cacheScope`), `tools/call` (empty registry → `-32602` "Unknown tool: X"), unknown → `-32601`, `initialize` → `-32601` for now (Task 2 adds it). `mcp_tools()` returns an empty static vector for now. `mcp_server.cpp` `runStdio`: swap `std::cout` streambuf to `std::cerr` for the duration (safety net; restore after), then `std::getline(std::cin, line)` loop → skip empty lines → `json11::Json::parse` (on error emit `-32700` with id null) → `protocol::handle` → if not null write `json11::dump()` + `'\n'` and flush. Return at EOF. `app_options.cpp` option: `m_po.add("mcp").description("Start the MCP server on stdio")`.

- [ ] **Step 4: Run test to verify it passes**

From `tests/`: `ASEPRITE="$PWD/../build/bin/aseprite" bash run-tests.sh mcp-stdio`
Expected: `Done` with no FAILED.

- [ ] **Step 5: Commit**

```bash
git add src/app/mcp src/app/cli/app_options.h src/app/cli/app_options.cpp src/app/app.h src/app/app.cpp src/app/CMakeLists.txt tests/cli/mcp-stdio.sh
git commit -m "Add native MCP protocol core and --mcp stdio mode"
```

---

### Task 2: Legacy era support (`initialize`, `ping`, legacy shapes)

**Files:**
- Modify: `src/app/mcp/mcp_protocol.cpp`
- Modify: `tests/cli/mcp-stdio.sh`

**Interfaces:**
- Consumes: Task 1 `protocol` API.
- Produces: legacy behavior — `initialize` reply `{protocolVersion, capabilities:{"tools":{"listChanged":false}}, serverInfo}` with `protocolVersion` echoed when requested value is in `kLegacyVersions`, else `kDefaultLegacyVersion`; `ping` → `{}` under legacy; `notifications/initialized` accepted (no reply); legacy `tools/list` / `tools/call` results omit `resultType`/`ttlMs`/`cacheScope`; legacy results also omit `_meta.serverInfo`.

- [ ] **Step 1: Extend the failing test** — append to `tests/cli/mcp-stdio.sh`

```bash
echo "MCP stdio: legacy initialize echoes supported version"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"protocolVersion":"2025-06-18"' || fail "initialize echo: $resp"
echo "$resp" | grep -q '"resultType"' && fail "legacy reply must omit resultType: $resp"

echo "MCP stdio: unknown legacy version gets fallback version"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"1900-01-01"}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"protocolVersion":"2025-06-18"' || fail "fallback version: $resp"

echo "MCP stdio: legacy ping is {}"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"ping"}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"result":{}' || fail "ping: $resp"

echo "MCP stdio: notifications produce no reply"
resp=$(printf '%s\n%s\n%s\n' '{"jsonrpc":"2.0","method":"notifications/initialized"}' '{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":1}}' '{"jsonrpc":"2.0","id":9,"method":"ping"}' | "$ASEPRITE" --mcp)
[ "$(printf '%s\n' "$resp" | grep -c '"id":9')" = "1" ] || fail "expected exactly one reply: $resp"
```

- [ ] **Step 2: Run test to verify it fails.** Expected: initialize/ping assertions FAIL (`-32601`).

- [ ] **Step 3: Implement.** In `mcp_protocol.cpp`: on legacy era, `initialize` returns the legacy shape (echo requested version when in `kLegacyVersions`); `ping` returns `{}`; `handle` returns null Json for `notifications/initialized` and `notifications/cancelled` (accepted; a `ponytail:` comment notes cancellation is only honored at message boundaries because tools are short); legacy `tools/list`/`tools/call` build results without modern-only fields (branch on `era`).

- [ ] **Step 4: Run test to verify it passes.** Expected: `Done`.

- [ ] **Step 5: Commit**

```bash
git add src/app/mcp/mcp_protocol.cpp tests/cli/mcp-stdio.sh
git commit -m "Add legacy MCP era support to protocol core"
```

---

### Task 3: Tool context, Lua tool infrastructure, first three tools

**Files:**
- Create: `src/app/mcp/mcp_tool_context.h`, `src/app/mcp/mcp_tool_context.cpp`
- Create: `src/app/mcp/mcp_tools_canvas_draw.cpp`, `src/app/mcp/mcp_tools_layers.cpp`, `src/app/mcp/mcp_tools_animation.cpp`, `src/app/mcp/mcp_tools_palette_transform.cpp`, `src/app/mcp/mcp_tools_io.cpp`, `src/app/mcp/mcp_tools_misc.cpp` (later category files start with empty register functions)
- Modify: `src/app/mcp/mcp_tools_registry.cpp` (assemble from the six register functions, in order: canvas_draw, layers, animation, palette_transform, io, misc)
- Modify: `src/app/mcp/mcp_server.cpp` (wire `ToolContext::instance().setEngine/setContext` in `runStdio`)
- Modify: `src/app/CMakeLists.txt` (new files)
- Create: `tests/cli/mcp-tools.sh`

**Interfaces:**
- Consumes: Task 1 registry/protocol/server.
- Produces:
  - `app::mcp::ToolContext::instance()`, `void setEngine(script::Engine*)`, `void setContext(Context*)`, `bool openDocument(const std::string& filename, std::string& error)`, `ToolResult runLua(const std::string& code)`
  - `std::string luaString(const std::string& value)` (escape for a Lua double-quoted literal: `\` `"` newline `\r` `\0`)
  - Factories in `mcp_tools_registry.h`: `McpTool LuaTool(std::string name, std::string description, json11::Json inputSchema, bool openFilename, std::function<std::string(const json11::Json&)> buildScript);`, `McpTool NativeTool(std::string name, std::string description, json11::Json inputSchema, std::function<ToolResult(const json11::Json&)> fn);`
  - Arg helpers: `std::string argString(const json11::Json&, const char* name, const std::string& def = {});`, `int argInt(const json11::Json&, const char* name, int def = 0);`, `bool argBool(const json11::Json&, const char* name, bool def = false);`
  - Register functions: `register_canvas_draw_tools`, `register_layers_tools`, `register_animation_tools`, `register_palette_transform_tools`, `register_io_tools`, `register_misc_tools` (all `void(std::vector<McpTool>&)`)
  - Tools registered by this task (3): `create_canvas(width:int, height:int, filename:str="canvas.aseprite")` (Lua, `openFilename=false`), `get_sprite_info(filename:str)` (Lua, `openFilename=true`), `run_lua_script(script:str, filename:str="")` (Lua, `openFilename=true`; skip open when filename empty)

**Behavior pinned:**
- `openDocument`: normalize to absolute path; if `Context::documents()` already holds a doc with that filename → `setActiveDocument` and return. Else if cached doc path matches and `!doc->isModified()` and `base::get_modification_time` equals recorded time → `setActiveDocument`. Else if file exists → execute `CommandId::OpenFile()` with param `filename` (synchronous) and cache the resulting active doc + mtime. Missing file → `error = "File <name> not found"`.
- `runLua`: lock an internal mutex; set a `ScopedEngineDelegate` capturing `onConsolePrint`/`onConsoleError` into a text buffer; `engine->evalCode(code, "mcp:tool")`; on `false` → `ToolResult{isError=true, text=<captured errors>}`; on success, if captured text starts with `ERROR:` → `isError=true` with the prefix stripped; else the captured text is the result.

- [ ] **Step 1: Write the failing test** — `tests/cli/mcp-tools.sh`

```bash
echo "MCP tools: create_canvas writes a file"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
[ -f "$t/c.aseprite" ] || fail "create_canvas did not create file: $resp"

echo "MCP tools: get_sprite_info reads size"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '8' || fail "sprite info: $resp"

echo "MCP tools: unknown tool -> -32602"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"nope","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '\-32602' || fail "unknown tool: $resp"

echo "MCP tools: tricky strings round-trip through run_lua_script"
# Lua source that will run:  print("l1\nl2") print("a\"b\\c é")
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"run_lua_script","arguments":{"script":"print(\"l1\\nl2\") print(\"a\\\"b\\\\c é\")"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert "l1\nl2" in t, repr(t); assert "a\"b\\c é" in t, repr(t)' || fail "string round-trip: $resp"

echo "MCP tools: exported stdout stays pure JSON"
printf '%s\n' '{"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"run_lua_script","arguments":{"script":"print(\"noise\")"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' \
  | "$ASEPRITE" --mcp \
  | python3 -c 'import sys,json; [json.loads(l) for l in sys.stdin if l.strip()]' \
  || fail "tool output leaked to stdout"

echo "MCP tools: external modification invalidates doc cache"
printf '%s\n' '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/m.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp >/dev/null
printf 'local spr = Sprite(16, 16)\nspr:saveAs("%s/m.aseprite")\n' "$t" > "$t/mk.lua"
"$ASEPRITE" -b --script "$t/mk.lua" || fail "fixture regeneration failed"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/m.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '16' || fail "stale cache served: $resp"
```

Note: the tricky-string test pins real-newline output (must stay one JSON line), escaped quotes/backslashes, and raw UTF-8 (é).

- [ ] **Step 2: Run test to verify it fails**

From `tests/`: `ASEPRITE="$PWD/../build/bin/aseprite" bash run-tests.sh mcp-tools`
Expected: FAIL (unknown tools).

- [ ] **Step 3: Implement.** Port diivi's Lua logic for the three tools from the pinned reference (`aseprite_mcp/tools/canvas.py:create_canvas`, `animation.py:get_sprite_info`, `script.py:run_lua_script`) as fresh snippets: `create_canvas` builds `local spr = Sprite(w, h)` + `spr:saveAs(...)` and prints the path; `get_sprite_info` prints a JSON string built in Lua (width, height, colorMode, frames, layers, tags); `run_lua_script` evals the given code with the file open when `filename` is non-empty. `mcp_tools()` becomes non-empty; `mcp-tools.sh` fixture regeneration uses a relative-safe absolute path.

- [ ] **Step 4: Run test to verify it passes.** Expected: `Done` (both `mcp-stdio` and `mcp-tools` filters pass).

- [ ] **Step 5: Commit**

```bash
git add src/app/mcp tests/cli/mcp-tools.sh src/app/CMakeLists.txt
git commit -m "Add MCP tool context and first Lua-backed tools"
```

---

### Task 4: Canvas, drawing, and pixel-read tools (24 tools)

**Files:**
- Modify: `src/app/mcp/mcp_tools_canvas_draw.cpp`
- Modify: `src/app/mcp/mcp_tools_io.cpp` (pixel read tools live with inspection)
- Modify: `tests/cli/mcp-tools.sh`

**Interfaces:**
- Consumes: Task 3 factories (`LuaTool`, `luaString`, arg helpers) and `ToolContext::openDocument`.
- Produces (registered names, schemas mirrored from the pinned reference):
  - canvas (6 remaining): `add_layer`, `add_group`, `add_frame`, `set_frame`, `set_frame_duration`, `set_layer`
  - drawing (14): `draw_pixels`, `draw_line`, `draw_rectangle`, `fill_area`, `draw_circle`, `draw_pixels_at`, `draw_line_at`, `draw_rectangle_at`, `draw_circle_at`, `fill_area_at`, `draw_polygon`, `draw_path`, `apply_gradient_rect`, `draw_ellipse_at`
  - pixel read (4): `get_pixel_color`, `get_pixels_rect`, `get_composite_pixel`, `get_composite_rect`

- [ ] **Step 1: Extend the failing test** — append smoke coverage: `add_layer` on `$t/c.aseprite`; `draw_rectangle` filled `#FF0000`; then `get_pixel_color` returns `#FF0000`; then `get_composite_pixel` matches.

```bash
echo "MCP tools: draw and read back a pixel"
run_tool() { printf '%s\n' "$1" | "$ASEPRITE" --mcp; }
run_tool '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"add_layer","arguments":{"filename":"'$t'/c.aseprite","name":"ink"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"draw_rectangle","arguments":{"filename":"'$t'/c.aseprite","x":1,"y":1,"width":4,"height":4,"color":"#FF0000","fill":true},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/c.aseprite","x":2,"y":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
echo "$resp" | grep -qi 'FF0000' || fail "pixel read: $resp"
```

(Adjust argument names to the reference schemas during implementation; the reference is authoritative.)

- [ ] **Step 2: Run test to verify it fails.** Expected: FAIL (unknown tool `add_layer`).

- [ ] **Step 3: Implement.** For each tool, mirror the input schema from the pinned `canvas.py` / `drawing.py` / `pixel_read.py`, and write the Lua snippet fresh (diivi's snippets show the required API calls and edge handling — replicate behavior, not text). Prefer whole-number coordinates; keep diivi's validation behaviors (e.g. positive canvas size, traversal rejection is unnecessary here because MakeAbsolute-style normalization happens in `openDocument`).

- [ ] **Step 4: Run test to verify it passes.** Expected: `Done`.

- [ ] **Step 5: Commit**

```bash
git add src/app/mcp/mcp_tools_canvas_draw.cpp src/app/mcp/mcp_tools_io.cpp tests/cli/mcp-tools.sh
git commit -m "Add canvas, drawing, and pixel-read MCP tools"
```

---

### Task 5: Layers, selection, effects, transform tools (20)

**Files:**
- Modify: `src/app/mcp/mcp_tools_layers.cpp` (layers, selection, effects — per the spec file map), `src/app/mcp/mcp_tools_palette_transform.cpp` (transform), `tests/cli/mcp-tools.sh`

**Interfaces:**
- Produces: layers (7): `delete_layer`, `rename_layer`, `duplicate_layer`, `reorder_layer`, `set_layer_blend_mode`, `merge_layer_down`, `flatten_sprite`; selection (4): `move_region`, `copy_region`, `erase_region`, `erase_color`; effects (5): `outline_cel`, `replace_color`, `adjust_hsl`, `apply_dither_gradient`, `apply_dither_pattern`; transform (4): `flip_layer`, `rotate_layer`, `resize_canvas`, `crop_canvas`.

- [ ] **Step 1: Extend the failing test.** Smoke: `rename_layer`; `flip_layer`; verify via `get_sprite_info` (layer list) and a pixel read.
- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement** mirroring the pinned `layers.py`, `selection.py`, `fx.py`, `transform.py` schemas.
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `Add layer, selection, effects, and transform MCP tools`

---

### Task 6: Animation and quality tools (27)

**Files:**
- Modify: `src/app/mcp/mcp_tools_animation.cpp`, `tests/cli/mcp-tools.sh`

**Interfaces:**
- Produces: animation (23 remaining): `add_frames`, `set_frame_duration_all`, `set_layer_visibility`, `set_layer_opacity`, `duplicate_frame_range`, `set_cel_position`, `tween_cel_positions`, `offset_cel_positions`, `create_cel`, `clear_cel`, `copy_cel`, `copy_frame`, `propagate_frame_to_range`, `set_tag`, `set_onion_skin`, `propagate_cels`, `tween_cel_positions_eased`, `oscillate_cel_positions`, `tween_cel_opacity_eased`, `tween_cel_scale_eased`, `delete_frame`, `delete_tag`, `set_cel_opacity`; quality (4): `ensure_layers_present`, `validate_scene`, `audit_animation`, `animation_sanitize`.

- [ ] **Step 1: Extend the failing test.** Smoke: `add_frames` ×2 with duration; `set_tag`; `validate_scene` returns JSON without error; `get_sprite_info` shows 3 frames and the tag.
- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement** mirroring pinned `animation.py` and `quality.py`.
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `Add animation and quality MCP tools`

---

### Task 7: Palette, slices, tilemap, scene tools (19)

**Files:**
- Modify: `src/app/mcp/mcp_tools_palette_transform.cpp`, `src/app/mcp/mcp_tools_misc.cpp`, `tests/cli/mcp-tools.sh`

**Interfaces:**
- Produces: palette (8): `get_palette`, `set_palette`, `remap_colors_in_cel_range`, `list_palette_presets`, `apply_palette_preset`, `generate_color_ramp`, `quantize_to_palette`, `set_color_mode`; slices (5): `create_slice`, `set_slice_center`, `set_slice_pivot`, `list_slices`, `delete_slice`; tilemap (5): `create_tilemap_layer`, `draw_on_tile`, `set_tiles`, `get_tile_at`, `get_tilemap_info`; scene (1): `copy_layers_between_sprites`.

- [ ] **Step 1: Extend the failing test.** Smoke: `set_palette` then `get_palette` returns the same hex list; `create_slice` + `list_slices`; `create_tilemap_layer` + `get_tilemap_info`; `copy_layers_between_sprites` between `$t/c.aseprite` and a second file.
- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement** mirroring pinned `palette.py`, `slices.py`, `tilemap.py`, `scene.py`.
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `Add palette, slices, tilemap, and scene MCP tools`

---

### Task 8: Export/import, analysis, and image content (10 tools)

**Files:**
- Modify: `src/app/mcp/mcp_tools_io.cpp`, `tests/cli/mcp-tools.sh`

**Interfaces:**
- Produces: export (7): `export_sprite`, `copy_sprite`, `export_frame`, `export_spritesheet`, `export_layers`, `export_tag`, `import_image_as_layer`; analysis (3): `render_onion_skin`, `compare_frames`, `get_color_stats`.
- Image content rule: after a Lua handler writes a `.png` (output paths used by `export_frame`, `export_sprite`, `export_spritesheet`), attach `imageMime="image/png"` and `imageBase64=base::encode_base64(fileContents)` to the `ToolResult`. A small free helper in `mcp_tools_io.cpp`: `bool attachPng(const std::string& path, ToolResult& result);` (reads the file; ignores failure silently).

- [ ] **Step 1: Extend the failing test.** Smoke: `export_frame` on `$t/c.aseprite` → `$t/f.png`; assert response contains `"type":"image"` and `"mimeType":"image/png"`, decode base64 with python3, assert decoded bytes start with the PNG magic `\x89PNG`.

```bash
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"export_frame","arguments":{"filename":"'$t'/c.aseprite","frame_index":1,"output_filename":"'$t'/f.png","scale":8},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
printf '%s' "$resp" | python3 -c 'import sys,json,base64; r=json.load(sys.stdin); img=[c for c in r["result"]["content"] if c["type"]=="image"][0]; assert base64.b64decode(img["data"])[:4]==b"\x89PNG"' || fail "png image content: $resp"
```

- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement** mirroring pinned `export.py` and `analysis.py`; route export tools through a `LuaTool` variant that post-processes results (extend `LuaTool` with an optional `std::function<void(const json11::Json& args, ToolResult&)> after;` parameter — `attachPng` is passed for the three export tools).
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `Add export, analysis, and image-content MCP tools`

---

### Task 9: Native effects and the workflow guide (8 tools)

**Files:**
- Modify: `src/app/mcp/mcp_tools_io.cpp` (native_fx group), `src/app/mcp/mcp_tools_misc.cpp` (guide), `tests/cli/mcp-tools.sh`

**Interfaces:**
- Produces: native_fx (7): `outline_native`, `adjust_hsl_native`, `adjust_brightness_contrast`, `invert_colors`, `apply_convolution`, `list_convolution_matrices`, `extract_palette`; guide (1): `animation_workflow_guide`.

- [ ] **Step 1: Extend the failing test.** Smoke: `invert_colors` then pixel read; `list_convolution_matrices` returns non-empty; `animation_workflow_guide` returns non-empty text.
- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement.** These are still Lua handlers: mirror pinned `native_fx.py` (the snippets use `app.command.*` native filters with a target-activation wrapper — replicate `core/native.py`'s transaction/activate pattern in a shared snippet helper `%NATIVE_TARGET%` inside `mcp_tools_io.cpp`). The guide is a static text `NativeTool` whose content is adapted from the spec's workflow (plan → palette → layers → coarse-to-fine → export-check loop).
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `Add native effects and workflow guide MCP tools`

---

### Task 10: Fonts and text tools (3 tools)

**Files:**
- Create: `src/app/mcp/mcp_fonts.h`, `src/app/mcp/mcp_fonts.cpp`
- Modify: `src/app/mcp/mcp_tools_canvas_draw.cpp` (`register_text_tools` is called from `register_canvas_draw_tools`), `tests/cli/mcp-tools.sh`
- Modify: `src/app/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `struct FontInfo { std::string name; enum class Kind { Bitmap, TrueType } kind; std::string path; };` — `std::vector<FontInfo> listFonts();` (scans `ASEPRITE_MCP_FONTS` env override, else `~/.aseprite-mcp/fonts`: directories containing `font.json` are bitmap fonts; `.ttf`/`.otf` files are TrueType. macOS also enumerates system TTFs best-effort; other platforms return the directory scan only.)
  - `struct TextRenderOptions { int size = 1; int letterSpacing = 0; bool bold = false; bool antialias = false; };`
  - `struct TextMetrics { int width = 0; int height = 0; int advanceWidth = 0; int aboveBaseline = 0; int belowBaseline = 0; };`
  - `bool measureText(const FontInfo& font, const std::string& text, const TextRenderOptions& options, TextMetrics& metrics, std::string& error);`
  - `bool drawTextInImage(const FontInfo& font, const std::string& text, doc::Image* image, int x, int y, doc::color_t color, const TextRenderOptions& options, TextMetrics* metricsOrNull, std::string& error);` (renders glyphs; the tool composes outline/shadow/anchor from metrics)
  - Tools: `list_text_fonts()`, `measure_text(text, font, size=1, letter_spacing=0, bold=0, antialias=False)`, `draw_text(filename, text, x, y, font, size=1, color="#FFFFFF", layer_name=None, frame_index=1, anchor="topleft", letter_spacing=0, bold=0, outline_color=None, outline_width=1, outline_diagonal=True, shadow_color=None, ...)` — schemas mirrored from pinned `text.py`.
- Bitmap sheets: `font.json` (`sheets[].file/cell_w/cell_h/ascent/chars`, `letter_gap`, `space_width`, `overrides`, `origin`, `ink_rule`, `advance`) parsed with json11; glyphs composited into the target `doc::Image` via `doc::put_pixel`-style access.
- TrueType: `ft::Lib` (`laf/ft/lib.h`) `open()` → `FT_Face`; render glyphs without antialiasing by default; `measureText` uses the same path so metrics match `draw_text`.

- [ ] **Step 1: Extend the failing test.** Fixture: write `$t/fonts/tiny/font.json` (2 glyphs, 8×8 cell, ascent 7, sheet `tiny.png`) via heredoc; generate `tiny.png` with an `"$ASEPRITE" -b --script` helper that draws two solid glyph boxes and exports PNG. Then with `ASEPRITE_MCP_FONTS=$t/fonts`: `list_text_fonts` contains `tiny`; `measure_text` returns width > 0; `draw_text` with color `#FF00FF` then `get_pixel_color` at a glyph pixel returns `FF00FF`.
- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement** `mcp_fonts.*` and the three tools (mirroring `text.py` behavior: anchors, whole-number scaling for bitmap fonts, no antialiasing by default).
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `Add native text rendering and font MCP tools`

---

### Task 11: HTTP transport (`--mcp-http`, in-app server core)

**Files:**
- Modify: `src/app/mcp/mcp_server.h`, `src/app/mcp/mcp_server.cpp` (HTTP parts under `#ifdef ENABLE_WEBSOCKET`)
- Modify: `src/app/cli/app_options.h/.cpp` (`--mcp-http` int option; mutual-exclusion error for `--shell` / `--mcp` combinations; implies no UI)
- Modify: `src/app/app.h/.cpp` (`m_isMcpHttp`, `m_mcpHttpPort`; `App::run()` non-GUI branch: `mcp::McpServer::instance()->runHttpOnly(port)`)
- Create: `tests/cli/mcp-http.sh`

Note: no CMake linking change is needed for HTTP — `app-lib` already links `ixwebsocket` inside the scripting block (`src/app/CMakeLists.txt:186`, under `if(ENABLE_WEBSOCKET)`), and the HTTP code is compiled under the same macro.

**Interfaces:**
- Consumes: Task 1–10 dispatcher and tool layer.
- Produces:
  - `bool McpServer::startHttp(int port, std::string& error);` (creates `ix::HttpServer` on `127.0.0.1`, `setOnConnectionCallback`, `listen()`; error from the returned pair), `void stopHttp();`, `bool isHttpRunning() const;`, `void runHttpOnly(int port);` (start; on failure print to stderr + exit 1; then block forever — termination via default SIGINT/SIGTERM disposition)
  - HTTP behavior: single endpoint `POST /mcp`; anything else 404. Validation order: `Origin` present and not `http(s)://localhost*`/`http://127.0.0.1*` → 403; body must parse as one JSON-RPC message; modern (header `MCP-Protocol-Version` and/or body `_meta`): required headers `MCP-Protocol-Version` (must equal body `_meta` value), `Mcp-Method` (must equal `method`), `Mcp-Name` for `tools/call` (must equal `params.name`, base64 sentinel `=?base64?…?=` decoded before compare) → any mismatch/missing → 400 + `-32020`; `MCP-Protocol-Version` not in supported list → 400 + `-32022` with `supported`; unknown method → 404 + `-32601`; notifications → 202 empty; `GET`/`DELETE` → 405; responses `application/json`.
  - Execution: HTTP worker queues an `os::Event` (`setType(os::Event::Callback)`, `setCallback(...)`) on `os::System::instance()->eventQueue()`, waits on a mutex/condvar (120 s timeout) for the main thread to run `handleMessage` and fill the response; implemented as a small local request-state struct in `mcp_server.cpp`.
  - `runHttpOnly` (headless) executes directly on the calling ix worker thread (`ToolContext` mutex serializes Lua).

- [ ] **Step 1: Write the failing test** — `tests/cli/mcp-http.sh`

```bash
PORT=18099
"$ASEPRITE" --mcp-http=$PORT &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
for i in $(seq 1 50); do
  curl -s -o /dev/null "http://127.0.0.1:$PORT/mcp" -X POST && break
  sleep 0.1
done

echo "MCP http: modern discover over HTTP"
resp=$(curl -s -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
echo "$resp" | grep -q '"supportedVersions"' || fail "http discover: $resp"

echo "MCP http: header mismatch -> 400 and -32020"
body='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"create_canvas","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}'
code=$(curl -s -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' -H 'Mcp-Name: other' -d "$body")
[ "$code" = "400" ] || fail "expected 400, got $code"
grep -q '\-32020' "$t/out" || fail "expected HeaderMismatch"

echo "MCP http: unsupported version -> 400 and -32022"
code=$(curl -s -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 1900-01-01' \
  -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":3,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1900-01-01"}}}')
[ "$code" = "400" ] || fail "expected 400, got $code"
grep -q '\-32022' "$t/out" || fail "expected UnsupportedProtocolVersion"

echo "MCP http: GET -> 405; unknown method -> 404; notification -> 202"
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/mcp")
[ "$code" = "405" ] || fail "GET should be 405, got $code"
code=$(curl -s -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: nope' -d '{"jsonrpc":"2.0","id":4,"method":"nope","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ "$code" = "404" ] || fail "unknown method should be 404, got $code"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: notifications/initialized' -d '{"jsonrpc":"2.0","method":"notifications/initialized"}')
[ "$code" = "202" ] || fail "notification should be 202, got $code"

echo "MCP http: legacy initialize over HTTP"
resp=$(curl -s -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":5,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}')
echo "$resp" | grep -q '"protocolVersion":"2025-06-18"' || fail "legacy http initialize: $resp"

echo "MCP http: bad Origin -> 403"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Origin: http://evil.example' -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":6,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ "$code" = "403" ] || fail "bad Origin should be 403, got $code"

echo "MCP http: concurrent requests both succeed"
# Ensure the fixture used by the parallel calls exists
curl -s -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: create_canvas' \
  -d '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
body='{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}'
curl -s -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: get_sprite_info' -d "$body" > "$t/r1" &
curl -s -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: get_sprite_info' -d "$body" > "$t/r2" &
wait
grep -q '"result"' "$t/r1" || fail "parallel request 1: $(cat $t/r1)"
grep -q '"result"' "$t/r2" || fail "parallel request 2: $(cat $t/r2)"

echo "MCP http: port already in use exits non-zero"
"$ASEPRITE" --mcp-http=$PORT >"$t/out2" 2>&1 && fail "expected non-zero exit"
grep -qi 'port\|listen\|address' "$t/out2" || fail "expected port error, got: $(cat $t/out2)"
```

- [ ] **Step 2: Run test to verify it fails.** Expected: FAIL (`--mcp-http` unknown option).

- [ ] **Step 3: Implement.** Follow the interfaces above. Parse the option value with `strtol` (pattern of `--texture-width` in `cli_processor.cpp`). In `runHttpOnly`, block until the process is terminated (e.g. `ix::SocketServer::wait()` after a successful `listen()`; default SIGINT/SIGTERM disposition ends the process); on listen failure print `"Cannot start MCP HTTP server on port <n>: <error>"` to stderr and `exit(1)`. HTTP handler marshals via `os::Event` callback; `runHttpOnly` path calls `handleMessage` directly.

- [ ] **Step 4: Run test to verify it passes.** Expected: `Done`.

- [ ] **Step 5: Commit**

```bash
git add src/app/mcp/mcp_server.h src/app/mcp/mcp_server.cpp src/app/cli/app_options.h src/app/cli/app_options.cpp src/app/app.h src/app/app.cpp src/app/CMakeLists.txt tests/cli/mcp-http.sh
git commit -m "Add MCP HTTP transport and --mcp-http headless mode"
```

---

### Task 12: Preview server tools (2 tools)

**Files:**
- Modify: `src/app/mcp/mcp_tools_misc.cpp`, `tests/cli/mcp-tools.sh`

**Interfaces:**
- Produces: `start_preview_server(directory: str, port: int = 8000)`, `stop_preview_server(port: int = 8000)`. Implementation: a file-local `PreviewServer` class wrapping `ix::HttpServer` (under `#ifdef ENABLE_WEBSOCKET`) that serves files from `directory` with basic content types (`html`, `png`, `gif`, `json`, default `application/octet-stream`); `start` returns an error string when the directory is missing or the port is taken; `stop` stops and cleans up. A `static std::map<int, PreviewServer>` inside `mcp_tools_misc.cpp` tracks running servers.

- [ ] **Step 1: Extend the failing test.** Start on port `18100` for `$t`; `curl` an exported PNG; stop; assert 200 and clean stop.
- [ ] **Step 2: Run test to verify it fails.**
- [ ] **Step 3: Implement.** Mirrors pinned `preview.py` behavior (directory check, port default 8000).
- [ ] **Step 4: Run test to verify it passes.**
- [ ] **Step 5: Commit** `Add preview server MCP tools`

---

### Task 13: GUI — preferences, menu, setup dialog with how-to

**Files:**
- Modify: `data/pref.xml` (new section `mcp`: `enabled` bool default false, `port` int default 3777)
- Modify: `data/gui.xml` (in `edit_menu`, right after line 837 `<item command="Options" text="@.edit_preferences" />`, add `<item command="OpenMcpServer" text="@.mcp_server" />`)
- Modify: `data/strings/en.ini` (keys: `mcp_server`, `mcp_status_running`, `mcp_status_stopped`, `mcp_port`, `mcp_start`, `mcp_stop`, `mcp_stdio_howto`, `mcp_http_howto`, `mcp_howto_title`, `mcp_copy`, `mcp_copied`, `mcp_client_example` — multi-line how-to text uses `\n` escapes)
- Modify: `src/app/commands/commands_list.h` (add `FOR_EACH_COMMAND(OpenMcpServer)` near `OpenScriptFolder`)
- Create: `src/app/commands/cmd_mcp_server.h`, `src/app/commands/cmd_mcp_server.cpp` (pattern of `cmd_undo_history.cpp`: file-local `static McpServerWindow* g_window;` → `new McpServerWindow()` on first use, toggle `openWindow()`/`closeWindow(nullptr)`; factory `CommandFactory::createOpenMcpServerCommand()`)
- Create: `data/widgets/mcp_server.xml` (labels, status line, Start/Stop `ui::Button`s, port `ui::Entry`, two snippet areas with copy buttons, how-to label; `window` root id `mcp_server`)
- Create: `src/app/ui/mcp_server_window.h`, `src/app/ui/mcp_server_window.cpp` (`class McpServerWindow : public app::gen::McpServerWindow` — status refresh on open; Start/Stop call `McpServer::startHttp/stopHttp` and persist `Preferences::instance().mcp.enabled(bool)`; port entry commits to `mcp.port`; copy buttons `clip::set_text(...)`; stdio snippet embeds `base::get_app_path()`; HTTP snippet embeds `http://127.0.0.1:<port>/mcp`)
- Modify: `src/app/app.cpp` (GUI branch, near `checkUpdate.launch()`: start HTTP when `Preferences::instance().mcp.enabled()`, log failure; after `manager->run()` returns: `McpServer::instance()->stopHttp()`)
- Modify: `src/app/CMakeLists.txt` (new command + window sources in the `ENABLE_SCRIPTING` list)

**Interfaces:**
- Consumes: `McpServer::startHttp/stopHttp/isHttpRunning` (Task 11), generated `Preferences` (`Preferences::instance().mcp.enabled()/.port()`), generated widget class `app::gen::McpServerWindow` from `data/widgets/mcp_server.xml`.
- Produces: `CommandId::OpenMcpServer()`; menu item; dialog contents: status (`Running on http://127.0.0.1:<port>/mcp` / `Stopped`), Start/Stop, port, stdio card (command line `"<app path>" --mcp` + copyable JSON snippet `{"mcpServers":{"aseprite":{"command":"<app path>","args":["--mcp"]}}}`), HTTP card (URL + copyable snippet `{"mcpServers":{"aseprite":{"url":"http://127.0.0.1:<port>/mcp"}}}`), numbered how-to (enable → paste snippet into AI client MCP config → ask the assistant to draw), one concrete client example from the ini.

- [ ] **Step 1: Verify the current build passes** (baseline before UI work): `cmake --build build -j8` → success; from `tests/`: `ASEPRITE="$PWD/../build/bin/aseprite" bash run-tests.sh mcp` → `Done`.
- [ ] **Step 2: Implement** all files above. `pref.xml` and widget XML are auto-generated (no CMake change for XML; `file(GLOB widget_files ...)`).
- [ ] **Step 3: Build to verify** `cmake --build build -j8` → success (regenerates `pref.xml.h`/`mcp_server.xml.h`).
- [ ] **Step 4: Manual verification (GUI)** — launch `build/bin/aseprite`, open `Edit > MCP Server…`; start the server on 3777; `curl` the discover request (as in Task 11); verify status label flips to Running; copy the stdio snippet; quit and relaunch → server auto-starts (pref persisted). Record each result in the commit message.
- [ ] **Step 5: Commit**

```bash
git add data/pref.xml data/gui.xml data/strings/en.ini data/widgets/mcp_server.xml src/app/commands/commands_list.h src/app/commands/cmd_mcp_server.h src/app/commands/cmd_mcp_server.cpp src/app/ui/mcp_server_window.h src/app/ui/mcp_server_window.cpp src/app/app.cpp src/app/CMakeLists.txt
git commit -m "Add MCP server preferences, menu item, and setup dialog"
```

---

### Task 14: Full verification and acceptance

**Files:**
- Modify: `tests/cli/mcp-tools.sh` (final tool-count assertion: `tools/list` returns 116 entries and is stable across two calls)

**Interfaces:**
- Consumes: everything.
- Produces: whole-suite green run.

- [ ] **Step 1: Add the count assertion.**

```bash
echo "MCP tools: 116 tools, deterministic order"
run_list() { printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp; }
a=$(run_list); b=$(run_list)
[ "$a" = "$b" ] || fail "tools/list is not deterministic"
printf '%s' "$a" | python3 -c 'import sys,json; r=json.load(sys.stdin); assert len(r["result"]["tools"])==116, len(r["result"]["tools"])' || fail "expected 116 tools"
```

- [ ] **Step 2: Run all MCP tests.** From `tests/`: `ASEPRITE="$PWD/../build/bin/aseprite" bash run-tests.sh mcp` → `Done`.
- [ ] **Step 3: Run the complete existing suite.** From `tests/`: `ASEPRITE="$PWD/../build/bin/aseprite" bash run-tests.sh` → `Done` (existing CLI + Lua tests unaffected).
- [ ] **Step 4: Real-client acceptance (manual).** Configure an MCP client with `{"command":"<repo>/build/bin/aseprite","args":["--mcp"]}`; run: create canvas → draw rectangle → `export_frame` at 8× → inspect the returned image; then repeat over the HTTP URL from the dialog against the running GUI. Record outcomes in the commit message.
- [ ] **Step 5: Commit**

```bash
git add tests/cli/mcp-tools.sh
git commit -m "Verify full MCP tool surface and suite"
```

---

## Self-Review Notes

- Spec coverage: goals 1–4 → Tasks 1–2 (stdio/dual-era), 3–12 (tools/parity), 11 (HTTP), 13 (dialog/how-to); non-goals honored (no resources/SSE/auth); security items (loopback bind, Origin 403, header validation, `run_lua_script` warning) → Task 11 + Task 3 descriptions.
- Sequencing follows the spec §12; each category task is independent once Task 3's interfaces exist.
- Counts pinned: 116 = 3 (T3) + 24 (T4) + 20 (T5) + 27 (T6) + 19 (T7) + 10 (T8) + 8 (T9) + 3 (T10) + 2 (T12).
