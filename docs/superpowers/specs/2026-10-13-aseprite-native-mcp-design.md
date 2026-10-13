# Native MCP Server for Aseprite — Design

- **Date:** 2026-10-13
- **Status:** Approved (design phase), pending written-spec review
- **Repo:** local Aseprite C++ tree (`aseprite/aseprite` sources), built at `build/bin/aseprite`
- **Reference implementation:** [diivi/aseprite-mcp](https://github.com/diivi/aseprite-mcp) (external Python MCP server)

## 1. Goal

Make the shipped Aseprite binary itself an MCP server — no Python, no `uv`, no per-call subprocess. Two transports (stdio and local HTTP), a tool set matching diivi/aseprite-mcp, and an in-app dialog for setup with copy-paste client configurations and a how-to.

Success criteria:

1. An MCP client configured with `{ "command": "<aseprite>", "args": ["--mcp"] }` can discover and call tools without any external runtime.
2. A running Aseprite with the HTTP server enabled answers MCP requests at `http://127.0.0.1:<port>/mcp`, with edits visible live in the app.
3. The full diivi tool set is available (the 104 registered tools across 17 categories).
4. The dialog lets a user enable/configure both transports and copy working client snippets.

## 2. Background

- diivi/aseprite-mcp is an *external* Python server. Each tool call writes a Lua script and spawns `aseprite --batch [file] --script tmp.lua`, then parses `OK` / `ERROR:` lines from stdout. It requires no Aseprite changes but depends on Python and spawns a process per call.
- This design internalizes that model: the same Lua-execution approach, executed **in-process** via Aseprite's embedded `script::Engine`, exposing the same tool vocabulary natively.
- MCP protocol eras:
  - **Modern** (`2026-07-28`): stateless; every request carries version/identity/capabilities in `_meta.io.modelcontextprotocol/*`; `server/discover` is mandatory; results carry `resultType`; list results carry `ttlMs`/`cacheScope`; Streamable HTTP requires header/body validation.
  - **Legacy** (`2024-11-05` … `2025-11-25`): `initialize` handshake, `ping`, no per-request metadata.
  - This server is **dual-era** (spec-supported) so any deployed client version can connect.

## 3. Scope

### In scope

- Native dual-era MCP protocol core (tools capability only).
- stdio transport: `aseprite --mcp` (headless process mode).
- Streamable HTTP transport: POST `/mcp` endpoint; GUI mode (toggleable) and headless mode (`--mcp-http=PORT`).
- Full diivi tool parity: all 104 registered tools, diivi-compatible names and argument shapes.
- Native font stack for `list_text_fonts` / `measure_text` / `draw_text`.
- Image content results for export tools (visual feedback loop).
- Preview file-serving tools.
- Setup dialog (Edit menu) with status, Start/Stop, port, copyable stdio/HTTP snippets, and a short how-to.
- Preferences persistence (`mcp.enabled`, `mcp.port`).
- Automated tests: stdio + HTTP end-to-end scripts riding the existing `tests/` harness.

### Non-goals (v1)

- Resources, prompts, `subscriptions/listen`, list-changed notifications.
- SSE stream responses (plain JSON responses are spec-compliant; SSE adds stream lifecycle complexity with no product need here).
- Sampling, elicitation, MRTR (`InputRequiredResult`), tasks extension.
- Authentication tokens (loopback binding + Origin validation only; token support is a documented follow-up).
- GUI-automation tools (menus/dialogs driven by tools).
- Upstream contribution to aseprite/aseprite; this is a feature of this fork's shipped build.

## 4. Architecture

New subsystem `src/app/mcp/`, guarded by `#ifdef ENABLE_SCRIPTING` (same gate as `--shell`; `ENABLE_SCRIPTING` defaults ON).

### Components

| File | Responsibility |
|---|---|
| `mcp_protocol.h/.cpp` | Pure, I/O-free protocol core. Era detection, method dispatch, validation helpers, response builders (`resultType`, `_meta.serverInfo`, `ttlMs`/`cacheScope`), protocol error objects. No transport knowledge → unit-testable and shared by both transports. |
| `mcp_server.h/.cpp` | `McpServer` singleton. Tool registry ownership, `runStdio()`, `startHttp(port)` / `stopHttp()` / `isHttpRunning()`, HTTP request validation (Origin, headers vs body), main-thread marshaling for GUI mode. |
| `mcp_tool_context.h/.cpp` | Engine wiring and document management: `ScopedEngineDelegate` output capture, Lua string escaping, per-path document cache, save-after-mutation, engine access mutex. |
| `mcp_tools_registry.h/.cpp` | Tool description type (`McpTool`: name, title, description, `inputSchema` as `json11::Json`, handler), registry construction, `LuaTool()` / `NativeTool()` factories, shared Lua snippet helpers. |
| `mcp_tools_canvas_draw.cpp` | Canvas, drawing, text tools. |
| `mcp_tools_layers.cpp` | Layers, selection, effects tools. |
| `mcp_tools_animation.cpp` | Animation, frames, cels, quality tools. |
| `mcp_tools_palette_transform.cpp` | Palette, transform, slices, tilemap tools. |
| `mcp_tools_io.cpp` | Export/import, inspection, analysis tools. |
| `mcp_tools_misc.cpp` | Scene, preview, scripting (`run_lua_script`), workflow guide tools. |
| `mcp_fonts.h/.cpp` | Native text stack: bitmap `font.json` sheets, TrueType/OpenType rendering via bundled FreeType, font discovery. |
| `src/app/ui/mcp_server_window.h/.cpp` | Setup dialog class (includes generated `mcp_server.xml.h`). |
| `src/app/commands/cmd_mcp_server.h/.cpp` | `OpenMcpServer` command; opens the dialog. |
| `data/widgets/mcp_server.xml` | Dialog layout (auto-globbed by CMake). |
| `data/gui.xml` | Edit menu item "MCP Server…". |
| `data/pref.xml` | `mcp` section: `enabled` (bool, false), `port` (int, 3777). |
| `data/strings/en.ini` | New strings (`@.mcp_server` etc.). |
| `tests/cli/mcp-stdio.sh`, `tests/cli/mcp-http.sh` | End-to-end transport tests (sourced by `tests/run-tests.sh`). |

### Dialog pattern

Follows the `data/widgets/aseprite_update.xml` pattern: widget XML → generated `mcp_server.xml.h` → C++ window class. CMake globs `data/widgets/*.xml`, so no CMake change is needed for the dialog XML; C++ sources are added to the explicit list in `src/app/CMakeLists.txt` under the `ENABLE_SCRIPTING` block.

### Preferences

Preferences are generated from `data/pref.xml` into `pref.xml.{h,cpp}`. Adding the `mcp` section yields `Preferences::instance().mcp.enabled()` / `.mcp.port()` with automatic serialization. No manual preferences plumbing.

### Setup dialog ("MCP Server…")

- Opened from the Edit menu (string `@.mcp_server`) through the `OpenMcpServer` command; implemented as a window class following the widget-XML dialog pattern.
- Contents:
  - Status indicator: HTTP server running/stopped, bound URL, and the last error if starting failed (e.g. port busy).
  - Start/Stop control and a port field (writes `mcp.port`; restarting the server applies the change). Start/Stop also writes `mcp.enabled` so the running state persists across launches.
  - "Connect with stdio" card: the exact command line for this binary plus a copyable client-config JSON snippet.
  - "Connect with HTTP" card: the endpoint URL plus a copyable snippet.
  - Short numbered how-to on the dialog: enable → paste the snippet into the AI client's MCP configuration → ask the assistant to draw or edit; includes one concrete client example.
- Copy buttons use the app's existing text clipboard facility.

## 5. Integration points

### CLI (`AppOptions`)

- `--mcp` — start the stdio MCP server (headless; implies no UI, like `--batch`). `AppOptions::startMcp()`.
- `--mcp-http=PORT` — start only the HTTP MCP server, headless. `AppOptions::startMcpHttp()` + port accessor.
- Mode exclusivity: `--mcp` together with `--shell`, `--mcp` with `--mcp-http`, or `--mcp-http` with `--shell` → error message + exit 1. `--batch` alongside either MCP flag is allowed (redundant).
- Both flags set `m_startUI = false` in the existing no-UI condition in `app_options.cpp`.

### App lifecycle (`app.cpp`, `main.cpp`)

- `App::initialize`: `m_isMcp = options.startMcp()`, `m_isMcpHttp = options.startMcpHttp()`, `m_mcpPort`.
- `App::run()` non-GUI branch:
  - `m_isMcp` → `McpServer::instance()->runStdio()`, where the `Shell` branch runs today. Returns when stdin reaches EOF (clean shutdown, exit code 0).
  - `m_isMcpHttp` → start HTTP server on the requested port; block until SIGINT/SIGTERM; then stop server and exit.
- GUI branch: if `Preferences::instance().mcp.enabled()`, start the HTTP server after GUI init; stop it during shutdown (`App::~App` / `ExitGui` path).
- **stdout hygiene (`--mcp` only):** stdout must carry nothing but MCP messages. A safety net redirects `std::cout`'s streambuf to stderr for the lifetime of the mode; diagnostics/LOG already go to stderr in CLI mode.

## 6. Protocol behavior

### Era model

Era is detected per request:

- **Modern** — request body carries `_meta.io.modelcontextprotocol/protocolVersion` (HTTP: also the `MCP-Protocol-Version` header). Modern version: `2026-07-28`.
- **Legacy** — `initialize` request, or any request without modern metadata (permissive fallback, matching dual-era guidance).
- Legacy versions accepted in `initialize`: `2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25`. The requested version is echoed when in the list; otherwise the server replies `2025-06-18` (client may disconnect if unsupported).
- Era state on stdio is fixed by the first meaningful request and kept for the process lifetime. On HTTP the server is stateless: era is derived from each request; a legacy `initialize` is answered without minting sessions, and `Mcp-Session-Id` headers are ignored, never minted (per modern-revision guidance for older-era traffic).

### Methods

| Method | Era | Behavior |
|---|---|---|
| `server/discover` | modern | Result: `supportedVersions: ["2026-07-28"]`, `capabilities: {"tools": {"listChanged": false}}`, `_meta.io.modelcontextprotocol/serverInfo {name: "aseprite", version: <app version>}`, `instructions` (short workflow guidance), `resultType: "complete"`, `ttlMs: 86400000`, `cacheScope: "public"`. |
| `tools/list` | both | Registered tools in deterministic registration order. Modern results add `resultType`, `ttlMs`, `cacheScope`. Optional `cursor` is accepted and ignored (single page, no `nextCursor`). |
| `tools/call` | both | Dispatch by `params.name`; unknown name → protocol error `-32602` `"Unknown tool: <name>"`. |
| `initialize` / `notifications/initialized` | legacy | Handshake; `notifications/initialized` produces no reply. |
| `ping` | legacy | Reply `{}`. Under modern semantics `ping` does not exist → `-32601`. |
| `notifications/cancelled` | both (stdio) | Sets a cancellation flag for the referenced id; no reply; no further messages are sent for that id. |

Modern results include `_meta.io.modelcontextprotocol/serverInfo`. Legacy results omit modern-only fields (`resultType`, `ttlMs`, `cacheScope`).

### Tool results

- Success: `content` array (text, and/or image), `isError: false`.
- Execution failure (Lua error, exception, `ERROR:` protocol, invalid argument values): `content` with an actionable message, `isError: true`.
- `export_frame` / `export_sprite` / export-style tools return a text item with the output path plus an `image` content item (`image/png`, base64) at the requested scale — this is the draw → look → iterate loop.

### Error codes

| Situation | Code / response |
|---|---|
| JSON parse failure | `-32700` |
| Invalid request object | `-32600` |
| Unknown method | `-32601` (HTTP modern: also status 404) |
| Unknown tool / invalid params | `-32602` |
| Internal/marshaling timeout | `-32603` |
| HTTP header/body mismatch | status 400 + `-32020` (`HeaderMismatch`) |
| Unsupported modern version | status 400 (HTTP) / `-32022` (stdio) with `supported` list |

## 7. Transports

### stdio (`--mcp`)

- Newline-delimited JSON-RPC on stdin/stdout; each message one line. `json11::dump` emits compact single-line JSON with escaped strings, satisfying the no-embedded-newline rule. `json11` serializes object keys in sorted order — deterministic output.
- Read loop mirrors `Shell::run`: `getline` → parse → dispatch → write response line. Notifications get no response.
- EOF on stdin → clean shutdown (primary graceful-shutdown signal per spec).
- Logging and stray output go to stderr only.

### Streamable HTTP

- `ix::HttpServer` bound to `127.0.0.1`, single endpoint `POST /mcp`. Other paths → 404.
- Request validation order:
  1. `Origin` header present and not loopback (`http://localhost[:port]`, `http://127.0.0.1[:port]`) → 403 (DNS-rebinding defense).
  2. Body must be a single JSON-RPC message; `Content-Type: application/json`.
  3. Modern request (header `MCP-Protocol-Version` and/or body `_meta` present): required headers `MCP-Protocol-Version`, `Mcp-Method` (and `Mcp-Name` for `tools/call`) must match the body values (header names case-insensitive, values case-sensitive, `Mcp-Name` base64 sentinel `=?base64?…?=` decoded before comparison). Any mismatch or missing required header → 400 + `-32020`.
  4. Unsupported `MCP-Protocol-Version` value → 400 + `-32022` with `supported` list.
  5. Unknown method → 404 + `-32601`.
  6. Legacy request: no header requirements; served with legacy semantics.
- Notifications (JSON-RPC message without `id`) → `202 Accepted`, empty body.
- `GET` / `DELETE` on `/mcp` → `405` (modern shape; legacy clients on 2025-xx revisions do not require a GET stream from us — sessions are never assigned).
- Responses are always `application/json` (the spec-permitted alternative to an SSE stream).
- `Accept` header content is not enforced beyond JSON parsing.
- Server start failure (port busy): GUI shows the error in the dialog status and logs to stderr; CLI exits with a message.

### Execution and threading

- All Lua execution is serialized through one mutex in `McpToolContext` (the engine is not thread-safe).
- **stdio**: requests execute on the main thread (the read loop); no marshaling needed.
- **GUI HTTP**: an ix worker thread queues an `os::Event` (type `Callback`) with a closure onto `os::System::instance()->eventQueue()`; the UI main loop executes it and fills the shared response; the worker waits (120 s timeout) and serializes the result. The `EventQueue` is mutex-guarded, so cross-thread posting is safe.
- **Headless HTTP (`--mcp-http`)**: no UI loop exists, so requests execute directly on the calling ix worker thread under the engine mutex.

## 8. Tool layer

### Registry and handler kinds

- `McpTool` entries are registered in deterministic order. Names and argument schemas mirror diivi (snake_case) so existing prompts/guides transfer.
- **Lua handlers** (majority): build an Aseprite Lua snippet from arguments (with string escaping), run it via `script::Engine::evalCode()` under a `ScopedEngineDelegate` capturing console output/errors, and map results to text content. diivi's `ERROR:` convention and Lua exceptions both map to `isError: true` with the captured message.
- **Native handlers**: font tools (`list_text_fonts`, `measure_text`, `draw_text`), export image content, preview server management, and `run_lua_script` bookkeeping.

### Tool extraction step (first task of the implementation plan)

Enumerate every tool registered by diivi's server (its `tools/*.py` modules), capturing name, description intent, and input schema. Produce a mapping checklist: tool → handler kind → target file. Schemas are mirrored for compatibility; handler code and descriptions are written fresh against Aseprite's documented Lua API — no verbatim copying (diivi is MIT, this build ships under the Aseprite EULA; license hygiene).

### Document management

- Tools take `filename` arguments (diivi parity).
- `McpToolContext` keeps documents open in memory across calls, keyed by normalized absolute path. Before reuse, the file's mtime is compared; an externally modified file is reloaded (cache invalidation).
- Mutating tools save after each call (parity: diivi snippets end with save).
- GUI mode: the document is opened through the app context and appears as a **visible tab** — tool edits are visible live. Closing the tab externally removes it from the cache (path lookup falls back to reload).
- Headless modes: documents are never attached to the UI.

### Fonts (`mcp_fonts`)

- Font discovery directory: `~/.aseprite-mcp/fonts` (drop-in compatible with diivi setups).
  - Bitmap fonts: a directory with `font.json` + sheet PNGs (same format: sheets, per-sheet ascent, origin, ink rule, advance mode, overrides).
  - TrueType/OpenType: bare `.ttf`/`.otf` files, rendered with the bundled FreeType, no antialiasing by default.
- `list_text_fonts` also enumerates system font files; v1 uses CoreText on macOS; other platforms list the fonts directory only (documented limitation in the tool description).
- `measure_text` returns the same metrics `draw_text` uses (width, height, advance, baseline extents) so layouts are computed in one pass.
- `draw_text` composes glyphs into the target cel image natively (anchor, scaling by whole numbers for bitmap fonts, faux bold, outline, drop shadow, letter spacing).

### Preview tools

`start_preview_server` / `stop_preview_server` serve the export directory over a local HTTP server (a second `ix::HttpServer` instance); schemas mirror diivi's.

## 9. Security

- HTTP binds `127.0.0.1` only.
- `Origin` validation on every request (403 when present and not loopback).
- Header/body validation per the modern spec, rejecting mismatches with `-32020`.
- `run_lua_script` executes arbitrary Lua in-process; its description carries the same warning as diivi's tool (trusted input only).
- No authentication token in v1: accepted risk on a localhost-bound, Origin-checked server; a bearer-token preference is the documented follow-up.

## 10. Testing

Ride the existing harness: `tests/run-tests.sh` sources every `tests/cli/*.sh` (with `$ASEPRITE`, temp dir `$t`, and `expect`/`fail` helpers). Assertions use grep on the deterministic compact JSON output; structural checks may use `python3 -m json.tool` (present in CI images).

`tests/cli/mcp-stdio.sh`:

- `server/discover` → `resultType`, `supportedVersions`, `serverInfo` present.
- `tools/list` → tool count equals the registry count; order deterministic across two calls.
- `tools/call` round trip: `create_canvas`-style tool creates a real `.aseprite` file; `export_frame` returns image content.
- Error paths: unknown tool → `-32602`; junk line → `-32700`.
- Legacy: `initialize` echoed version; `ping` → `{}`; `tools/list` without metadata works.
- stdout purity: every stdout line parses as JSON.
- EOF shutdown: process exits 0 after stdin closes.

`tests/cli/mcp-http.sh` (uses `--mcp-http`):

- POST with correct modern headers → 200 JSON result.
- Header mismatch (`Mcp-Name` ≠ body) → 400 + `-32020`.
- Unsupported version → 400 + `-32022`.
- `GET` → 405; unknown path → 404; notification POST → 202.
- Legacy `initialize` over HTTP → 200 with echoed version.
- Bad `Origin` → 403.

Manual acceptance: configure a real MCP client against `aseprite --mcp`, exercise a draw → `export_frame` → inspect loop, then repeat against the HTTP URL shown in the dialog.

## 11. Risks and mitigations

| Risk | Mitigation |
|---|---|
| `draw_text` parity (fonts) is the deepest native work. | Scope native text first as its own workstream; bitmap-sheet path first, TTF second. |
| `ix::HttpServer` is vendored but unused in-app so far. | Validate early with the headless `--mcp-http` mode and its tests; fallback is a thin HTTP loop over `IXSocket`. |
| 104 tool handlers are bulk work. | The extraction checklist makes it mechanical; category files are independent workstreams. |
| Client era variance in the wild. | Dual-era support; manual verification with at least one real legacy-era and one modern-era client if available. |
| Stale document cache vs files edited outside Aseprite. | mtime check + reload before each call. |
| Port collision / server lifecycle bugs in GUI mode. | Explicit start/stop with status surfaced in the dialog; error path tested via occupied port. |

## 12. Implementation sequencing (plan expands this)

1. Protocol core + registry + stdio mode + a first small tool group + `mcp-stdio.sh`; verify with a real client.
2. Tool extraction pass; implement all Lua-backed tool categories.
3. Native extras: fonts (`draw_text` family), image content for exports, preview tools.
4. HTTP transport + headless `--mcp-http` + `mcp-http.sh`; GUI lifecycle wiring.
5. GUI: preferences, menu item, setup dialog with copyable snippets and how-to; manual end-to-end acceptance.

_Design decisions and their rationale were reviewed and approved section-by-section during the brainstorming session on 2026-10-13._
