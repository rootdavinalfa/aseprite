#! /bin/bash
# Copyright (C) 2026 Igara Studio S.A.

echo "MCP tools: create_canvas writes a file"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
[ -f "$t/c.aseprite" ] || fail "create_canvas did not create file: $resp"

echo "MCP tools: get_sprite_info reads size"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t)["width"] == 8, repr(t)' || fail "sprite info: $resp"

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

echo "MCP tools: draw and read back a pixel"
run_tool() { printf '%s\n' "$1" | "$ASEPRITE" --mcp; }
run_tool '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"add_layer","arguments":{"filename":"'$t'/c.aseprite","layer_name":"ink"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"draw_rectangle","arguments":{"filename":"'$t'/c.aseprite","x":1,"y":1,"width":4,"height":4,"color":"#FF0000","fill":true},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/c.aseprite","x":2,"y":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -qi '#ff0000 (r=255, g=0, b=0, a=255)' || fail "pixel read: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":12,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; names=[l["name"] for l in json.loads(t)["layers"]]; assert "ink" in names, repr(t)' || fail "add_layer: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":13,"method":"tools/call","params":{"name":"get_composite_pixel","arguments":{"filename":"'$t'/c.aseprite","x":2,"y":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -qi '#ff0000' || fail "composite pixel read: $resp"

echo "MCP tools: pixel rect read returns JSON pixel rows"
resp=$(run_tool '{"jsonrpc":"2.0","id":14,"method":"tools/call","params":{"name":"get_pixels_rect","arguments":{"filename":"'$t'/c.aseprite","x":2,"y":2,"width":2,"height":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; p=json.loads(t); assert len(p) == 4, repr(t); assert all(r["hex"] == "#ff0000" and r["r"] == 255 and r["a"] == 255 for r in p), repr(t); assert {(r["x"], r["y"]) for r in p} == {(2,2),(3,2),(2,3),(3,3)}, repr(t)' || fail "pixel rect: $resp"

echo "MCP tools: add_frame and set_frame_duration add an animation frame"
run_tool '{"jsonrpc":"2.0","id":15,"method":"tools/call","params":{"name":"add_frame","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":16,"method":"tools/call","params":{"name":"set_frame_duration","arguments":{"filename":"'$t'/c.aseprite","frame_index":2,"duration_ms":150},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":17,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; s=json.loads(t); assert s["frames"] == 2, repr(t); assert s["durations_ms"][1] == 150, repr(t)' || fail "frame tools: $resp"

echo "MCP tools: missing required argument is a tool execution error"
resp=$(run_tool '{"jsonrpc":"2.0","id":18,"method":"tools/call","params":{"name":"draw_pixels","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Missing required argument: pixels" in r["content"][0]["text"], repr(r)' || fail "missing required argument: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":19,"method":"tools/call","params":{"name":"draw_pixels","arguments":{"filename":"'$t'/c.aseprite","pixels":null},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Missing required argument: pixels" in r["content"][0]["text"], repr(r)' || fail "null required argument: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":20,"method":"tools/call","params":{"name":"draw_pixels","arguments":{"filename":"'$t'/c.aseprite","pixels":[{"x":7,"y":7,"color":"#123456"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "present arguments should still succeed: $resp"

echo "MCP tools: rename_layer and duplicate_layer change the layer list"
run_tool '{"jsonrpc":"2.0","id":21,"method":"tools/call","params":{"name":"rename_layer","arguments":{"filename":"'$t'/c.aseprite","layer_name":"ink","new_name":"paint"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":22,"method":"tools/call","params":{"name":"duplicate_layer","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":23,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; names=[l["name"] for l in json.loads(t)["layers"]]; assert "paint" in names, repr(t); assert "paint copy" in names, repr(t); assert "ink" not in names, repr(t)' || fail "layer rename/duplicate: $resp"

echo "MCP tools: erase_region clears a rectangle"
run_tool '{"jsonrpc":"2.0","id":24,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint","frame_index":1,"pixels":[{"x":1,"y":1,"color":"#ff0000"},{"x":2,"y":2,"color":"#ff0000"},{"x":3,"y":3,"color":"#ff0000"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":25,"method":"tools/call","params":{"name":"erase_region","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint","frame_index":1,"x":1,"y":1,"width":2,"height":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "erase_region: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":26,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/c.aseprite","x":1,"y":1,"layer_name":"paint","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; m=re.search(r"a=(\d+)", t); assert m and m.group(1) == "0", repr(t)' || fail "erased pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":27,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/c.aseprite","x":3,"y":3,"layer_name":"paint","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff0000"), repr(t)' || fail "pixel outside erase rect: $resp"

echo "MCP tools: replace_color swaps a color in place"
run_tool '{"jsonrpc":"2.0","id":28,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint","frame_index":1,"pixels":[{"x":5,"y":5,"color":"#0000ff"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":29,"method":"tools/call","params":{"name":"replace_color","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint","frame_index":1,"from_color":"#0000ff","to_color":"#ff00ff"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "replace_color: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":30,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/c.aseprite","x":5,"y":5,"layer_name":"paint","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff00ff"), repr(t)' || fail "replaced pixel: $resp"

echo "MCP tools: flip_layer mirrors the cel horizontally"
run_tool '{"jsonrpc":"2.0","id":31,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint","frame_index":1,"pixels":[{"x":0,"y":5,"color":"#00ff00"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":32,"method":"tools/call","params":{"name":"flip_layer","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "flip_layer: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":33,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/c.aseprite","x":0,"y":5,"layer_name":"paint","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; m=re.search(r"a=(\d+)", t); assert m and m.group(1) == "0", repr(t)' || fail "flipped-away pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":34,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/c.aseprite","x":7,"y":5,"layer_name":"paint","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#00ff00"), repr(t)' || fail "flipped pixel: $resp"

echo "MCP tools: move_region without dest_y is a missing-argument error"
resp=$(run_tool '{"jsonrpc":"2.0","id":35,"method":"tools/call","params":{"name":"move_region","arguments":{"filename":"'$t'/c.aseprite","layer_name":"paint","frame_index":1,"x":0,"y":0,"width":1,"height":1,"dest_x":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Missing required argument: dest_y" in r["content"][0]["text"], repr(r)' || fail "move_region schema: $resp"

echo "MCP tools: external modification invalidates doc cache"
# One server process serves both reads; between them the file is
# regenerated at a different size, so the cached doc must be dropped.
# (A fresh process per request would pass even without cache logic.)
printf 'local spr = Sprite(8, 8)\nspr:saveAs("%s/cache.aseprite")\n' "$t" > "$t/mk.lua"
"$ASEPRITE" -b --script "$t/mk.lua" || fail "cache fixture creation failed"

mkfifo "$t/mcp-in"
"$ASEPRITE" --mcp < "$t/mcp-in" > "$t/mcp-out" 2>/dev/null &
mcp_server=$!
# Watchdog: if the test stalls, kill the server after 15s so the suite
# can never hang on it. Disowned so it doesn't hold the test's stdout.
( sleep 15; kill "$mcp_server" 2>/dev/null ) >/dev/null 2>&1 &
mcp_watchdog=$!
disown
exec 3<> "$t/mcp-in"

printf '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"%s/cache.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":6,' "$t/mcp-out" && break
    sleep 0.1
done
grep -q '"id":6,' "$t/mcp-out" || fail "no reply to first read: $(cat "$t/mcp-out")"

printf 'local spr = Sprite(16, 16)\nspr:saveAs("%s/cache.aseprite")\n' "$t" > "$t/mk.lua"
"$ASEPRITE" -b --script "$t/mk.lua" || fail "fixture regeneration failed"

printf '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"%s/cache.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":7,' "$t/mcp-out" && break
    sleep 0.1
done
exec 3>&-
kill "$mcp_server" 2>/dev/null
wait "$mcp_server" 2>/dev/null
kill "$mcp_watchdog" 2>/dev/null

python3 -c 'import json,sys
w = {}
for line in open(sys.argv[1]):
    line = line.strip()
    if line:
        m = json.loads(line)
        w[m["id"]] = json.loads(m["result"]["content"][0]["text"])["width"]
assert w.get(6) == 8, w
assert w.get(7) == 16, w' "$t/mcp-out" || fail "stale cache served in-process: $(cat "$t/mcp-out")"

echo "MCP tools: load error stays pure JSON on stdout"
printf 'garbage, not a sprite\n' > "$t/bad.aseprite"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/bad.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp 2>/dev/null)
printf '%s' "$resp" | python3 -c 'import sys,json; [json.loads(l) for l in sys.stdin if l.strip()]' || fail "load error leaked to stdout: $resp"
printf '%s' "$resp" | python3 -c 'import sys,json; assert json.load(sys.stdin)["result"]["isError"]' || fail "corrupt file should be an error result: $resp"
