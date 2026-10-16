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

echo "MCP tools: animation and quality tools"
run_tool '{"jsonrpc":"2.0","id":40,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/anim.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":41,"method":"tools/call","params":{"name":"add_frames","arguments":{"filename":"'$t'/anim.aseprite","count":1,"duration_ms":120},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "add_frames: $resp"
run_tool '{"jsonrpc":"2.0","id":42,"method":"tools/call","params":{"name":"add_frames","arguments":{"filename":"'$t'/anim.aseprite","count":1,"duration_ms":200},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":43,"method":"tools/call","params":{"name":"set_tag","arguments":{"filename":"'$t'/anim.aseprite","name":"walk","from_frame":1,"to_frame":3,"direction":"pingpong"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":44,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/anim.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; s=json.loads(t); assert s["frames"] == 3, repr(t); assert s["durations_ms"][1] == 120 and s["durations_ms"][2] == 200, repr(t); tags={x["name"]: x for x in s["tags"]}; assert tags["walk"] == {"name":"walk","from":1,"to":3,"direction":"pingpong"}, repr(t)' || fail "animation frames/tag: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":45,"method":"tools/call","params":{"name":"validate_scene","arguments":{"filename":"'$t'/anim.aseprite","required_layers":["Layer 1"]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; r=json.loads(t); assert r["frames"] == 3, repr(t); assert r["range"] == {"start":1,"end":3}, repr(t); assert r["missing_layers"] == [] and r["missing_cels"] == [], repr(t)' || fail "validate_scene: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":46,"method":"tools/call","params":{"name":"add_frames","arguments":{"filename":"'$t'/anim.aseprite","count":0},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Count must be >= 1" in r["content"][0]["text"], repr(r)' || fail "add_frames count validation: $resp"

echo "MCP tools: out-of-range source_frame_index is an error"
resp=$(run_tool '{"jsonrpc":"2.0","id":47,"method":"tools/call","params":{"name":"set_cel_position","arguments":{"filename":"'$t'/anim.aseprite","layer_name":"Layer 1","frame_index":1,"x":0,"y":0,"source_frame_index":99},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Source frame out of range" in r["content"][0]["text"], repr(r)' || fail "set_cel_position out-of-range source frame: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":48,"method":"tools/call","params":{"name":"tween_cel_positions","arguments":{"filename":"'$t'/anim.aseprite","layer_name":"Layer 1","start_frame":1,"end_frame":3,"start_x":0,"start_y":0,"end_x":2,"end_y":2,"source_frame_index":99},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Source frame out of range" in r["content"][0]["text"], repr(r)' || fail "tween out-of-range source frame: $resp"

echo "MCP tools: palette tools"
run_tool '{"jsonrpc":"2.0","id":50,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/pal.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":51,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"'$t'/pal.aseprite","layer_name":"Layer 1","frame_index":1,"pixels":[{"x":1,"y":1,"color":"#123456"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":52,"method":"tools/call","params":{"name":"set_palette","arguments":{"filename":"'$t'/pal.aseprite","colors":["#000000","#FFFFFF"]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "set_palette: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":53,"method":"tools/call","params":{"name":"get_palette","arguments":{"filename":"'$t'/pal.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t) == ["#000000","#FFFFFF"], repr(t)' || fail "get_palette round-trip: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":54,"method":"tools/call","params":{"name":"quantize_to_palette","arguments":{"filename":"'$t'/pal.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "quantize_to_palette: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":55,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/pal.aseprite","x":1,"y":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#000000"), repr(t)' || fail "quantized pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":56,"method":"tools/call","params":{"name":"remap_colors_in_cel_range","arguments":{"filename":"'$t'/pal.aseprite","layer_name":"Layer 1","start_frame":1,"end_frame":1,"mappings":[{"from":"#000000","to":"#FF00FF"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "remap_colors_in_cel_range: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":57,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/pal.aseprite","x":1,"y":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff00ff"), repr(t)' || fail "remapped pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":58,"method":"tools/call","params":{"name":"apply_palette_preset","arguments":{"filename":"'$t'/pal.aseprite","preset":"gameboy"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "apply_palette_preset: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":59,"method":"tools/call","params":{"name":"get_palette","arguments":{"filename":"'$t'/pal.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t) == ["#0F380F","#306230","#8BAC0F","#9BBC0F"], repr(t)' || fail "palette preset colors: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":60,"method":"tools/call","params":{"name":"list_palette_presets","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; p=json.loads(t); assert len(p["gameboy"]) == 4 and "dawnbringer16" in p, repr(t)' || fail "list_palette_presets: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":61,"method":"tools/call","params":{"name":"generate_color_ramp","arguments":{"base_color":"#D04648","steps":5},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; r=json.loads(t); assert len(r) == 5 and all(re.fullmatch(r"#[0-9A-F]{6}", c) for c in r), repr(t)' || fail "generate_color_ramp: $resp"
# Python round() is half-to-even; these entries are exactly .5 channel values.
resp=$(run_tool '{"jsonrpc":"2.0","id":90,"method":"tools/call","params":{"name":"generate_color_ramp","arguments":{"base_color":"#FF004D","steps":16},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t) == ["#800011","#900017","#A2001D","#B20024","#C3002C","#D40034","#E6003E","#F60048","#FE0956","#FD1B67","#FC2D77","#FC3F87","#FB5195","#FB62A3","#FA73B0","#FA84BC"], repr(t)' || fail "ramp half-to-even rounding: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":91,"method":"tools/call","params":{"name":"generate_color_ramp","arguments":{"base_color":"#29ADFF","steps":16},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t)[4] == "#007FEC", repr(t)' || fail "ramp half-to-even rounding #29ADFF: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":62,"method":"tools/call","params":{"name":"set_color_mode","arguments":{"filename":"'$t'/pal.aseprite","mode":"indexed"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "set_color_mode indexed: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":63,"method":"tools/call","params":{"name":"set_color_mode","arguments":{"filename":"'$t'/pal.aseprite","mode":"rgb"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "set_color_mode rgb: $resp"

echo "MCP tools: slice tools"
resp=$(run_tool '{"jsonrpc":"2.0","id":64,"method":"tools/call","params":{"name":"create_slice","arguments":{"filename":"'$t'/pal.aseprite","name":"button","x":1,"y":2,"width":4,"height":3},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "create_slice: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":65,"method":"tools/call","params":{"name":"set_slice_center","arguments":{"filename":"'$t'/pal.aseprite","name":"button","x":1,"y":1,"width":2,"height":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "set_slice_center: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":66,"method":"tools/call","params":{"name":"set_slice_pivot","arguments":{"filename":"'$t'/pal.aseprite","name":"button","x":3,"y":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "set_slice_pivot: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":67,"method":"tools/call","params":{"name":"list_slices","arguments":{"filename":"'$t'/pal.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; s=json.loads(t); assert len(s) == 1, repr(t); assert s[0] == {"name":"button","x":1,"y":2,"width":4,"height":3,"center":{"x":1,"y":1,"width":2,"height":1},"pivot":{"x":3,"y":2}}, repr(t)' || fail "list_slices: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":68,"method":"tools/call","params":{"name":"delete_slice","arguments":{"filename":"'$t'/pal.aseprite","name":"button"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "delete_slice: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":69,"method":"tools/call","params":{"name":"list_slices","arguments":{"filename":"'$t'/pal.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t) == [], repr(t)' || fail "deleted slice list: $resp"

echo "MCP tools: tilemap tools"
run_tool '{"jsonrpc":"2.0","id":70,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/tile.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":71,"method":"tools/call","params":{"name":"create_tilemap_layer","arguments":{"filename":"'$t'/tile.aseprite","layer_name":"terrain","tile_width":4,"tile_height":4},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "create_tilemap_layer: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":72,"method":"tools/call","params":{"name":"get_tilemap_info","arguments":{"filename":"'$t'/tile.aseprite","layer_name":"terrain"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t) == {"tile_width":4,"tile_height":4,"tile_count":0,"map_cols":2,"map_rows":2}, repr(t)' || fail "get_tilemap_info: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":73,"method":"tools/call","params":{"name":"draw_on_tile","arguments":{"filename":"'$t'/tile.aseprite","layer_name":"terrain","tile_index":1,"pixels":[{"x":0,"y":0,"color":"#FF0000"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "draw_on_tile: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":74,"method":"tools/call","params":{"name":"get_tilemap_info","arguments":{"filename":"'$t'/tile.aseprite","layer_name":"terrain"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t)["tile_count"] == 1, repr(t)' || fail "tile appended: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":75,"method":"tools/call","params":{"name":"set_tiles","arguments":{"filename":"'$t'/tile.aseprite","layer_name":"terrain","frame_index":1,"tiles":[{"col":0,"row":0,"tile_index":1},{"col":1,"row":0,"tile_index":0}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "set_tiles: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":76,"method":"tools/call","params":{"name":"get_tile_at","arguments":{"filename":"'$t'/tile.aseprite","layer_name":"terrain","frame_index":1,"col":0,"row":0},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t) == {"col":0,"row":0,"tile_index":1}, repr(t)' || fail "get_tile_at: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":77,"method":"tools/call","params":{"name":"get_tile_at","arguments":{"filename":"'$t'/tile.aseprite","layer_name":"terrain","frame_index":1,"col":1,"row":0},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t)["tile_index"] == 0, repr(t)' || fail "get_tile_at empty cell: $resp"

echo "MCP tools: copy layers between sprites"
run_tool '{"jsonrpc":"2.0","id":78,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/scene.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":79,"method":"tools/call","params":{"name":"copy_layers_between_sprites","arguments":{"source_filename":"'$t'/c.aseprite","target_filename":"'$t'/scene.aseprite","layer_names":["paint"]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "copy_layers_between_sprites: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":80,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/scene.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; names=[l["name"] for l in json.loads(t)["layers"]]; assert "paint" in names, repr(t)' || fail "copied layer list: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":81,"method":"tools/call","params":{"name":"copy_layers_between_sprites","arguments":{"source_filename":"'$t'/c.aseprite","target_filename":"'$t'/scene.aseprite","layer_names":["paint","NOPE"]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; t=r["content"][0]["text"]; assert not r["isError"], repr(r); assert "NOPE" in t, repr(t)' || fail "copy reports skipped layers: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":82,"method":"tools/call","params":{"name":"copy_layers_between_sprites","arguments":{"source_filename":"'$t'/c.aseprite","target_filename":"'$t'/c.aseprite","layer_names":["paint"]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "must be different files" in r["content"][0]["text"], repr(r)' || fail "same-file copy must be rejected: $resp"

echo "MCP tools: export_frame returns the PNG as image content"
resp=$(run_tool '{"jsonrpc":"2.0","id":95,"method":"tools/call","params":{"name":"export_frame","arguments":{"filename":"'$t'/c.aseprite","frame_index":1,"output_filename":"'$t'/f.png","scale":8},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"type":"image"' || fail "export_frame has no image content: $resp"
printf '%s' "$resp" | grep -q '"mimeType":"image/png"' || fail "export_frame mime type: $resp"
printf '%s' "$resp" | python3 -c 'import sys,json,base64,struct; r=json.load(sys.stdin); img=[c for c in r["result"]["content"] if c["type"]=="image"][0]; assert base64.b64decode(img["data"])[:4]==b"\x89PNG"; assert struct.unpack(">II", base64.b64decode(img["data"])[16:24])==(64,64)' || fail "png image content: $resp"

echo "MCP tools: export_sprite and copy_sprite write files"
resp=$(run_tool '{"jsonrpc":"2.0","id":96,"method":"tools/call","params":{"name":"export_sprite","arguments":{"filename":"'$t'/scene.aseprite","output_filename":"'$t'/sprite.png","format":"png"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "export_sprite: $resp"
[ -f "$t/sprite1.png" ] || fail "export_sprite numbered siblings: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":119,"method":"tools/call","params":{"name":"export_sprite","arguments":{"filename":"'$t'/pal.aseprite","output_filename":"'$t'/single.png","format":"png"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"type":"image"' || fail "export_sprite has no image content: $resp"
[ -f "$t/single.png" ] || fail "export_sprite did not write the file: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":97,"method":"tools/call","params":{"name":"export_sprite","arguments":{"filename":"'$t'/anim.aseprite","output_filename":"'$t'/anim-noext","format":"GIF"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ -f "$t/anim-noext.gif" ] || fail "export_sprite gif extension: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":98,"method":"tools/call","params":{"name":"copy_sprite","arguments":{"filename":"'$t'/scene.aseprite","output_filename":"'$t'/copy"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ -f "$t/copy.aseprite" ] || fail "copy_sprite did not write the copy: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":99,"method":"tools/call","params":{"name":"copy_sprite","arguments":{"filename":"'$t'/scene.aseprite","output_filename":"'$t'/copy.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "already exists" in r["content"][0]["text"], repr(r)' || fail "copy_sprite overwrite guard: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":100,"method":"tools/call","params":{"name":"copy_sprite","arguments":{"filename":"'$t'/scene.aseprite","output_filename":"'$t'/copy.aseprite","overwrite":true},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "copy_sprite overwrite: $resp"

echo "MCP tools: export_spritesheet layouts, data file, and tag filter"
resp=$(run_tool '{"jsonrpc":"2.0","id":101,"method":"tools/call","params":{"name":"export_spritesheet","arguments":{"filename":"'$t'/anim.aseprite","output_filename":"'$t'/sheet.png","data_filename":"'$t'/sheet.json","scale":2,"padding":0},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"type":"image"' || fail "export_spritesheet has no image content: $resp"
printf '%s' "$resp" | python3 -c 'import sys,json,base64,struct; r=json.load(sys.stdin); t=r["result"]["content"][0]["text"]; assert "Sprite sheet exported" in t, repr(t); img=[c for c in r["result"]["content"] if c["type"]=="image"][0]; assert struct.unpack(">II", base64.b64decode(img["data"])[16:24])==(48,16)' || fail "spritesheet size: $resp"
python3 -c 'import json; d=json.load(open("'$t'/sheet.json")); assert d["meta"]["size"]=={"w":48,"h":16}, d' || fail "spritesheet data file"
run_tool '{"jsonrpc":"2.0","id":102,"method":"tools/call","params":{"name":"set_tag","arguments":{"filename":"'$t'/anim.aseprite","name":"clip","from_frame":1,"to_frame":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":103,"method":"tools/call","params":{"name":"export_spritesheet","arguments":{"filename":"'$t'/anim.aseprite","output_filename":"'$t'/sheet-clip.png","data_filename":"'$t'/sheet-clip.json","tag_name":"clip"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,base64,struct; r=json.load(sys.stdin); img=[c for c in r["result"]["content"] if c["type"]=="image"][0]; assert struct.unpack(">II", base64.b64decode(img["data"])[16:24])==(16,8)' || fail "spritesheet tag filter: $resp"
python3 -c 'import json; d=json.load(open("'$t'/sheet-clip.json")); assert len(d["frames"])==2, d' || fail "spritesheet tag data file"
resp=$(run_tool '{"jsonrpc":"2.0","id":104,"method":"tools/call","params":{"name":"export_spritesheet","arguments":{"filename":"'$t'/anim.aseprite","output_filename":"'$t'/nope.png","tag_name":"NOPE"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Tag not found" in r["content"][0]["text"], repr(r)' || fail "spritesheet missing tag: $resp"

echo "MCP tools: export_layers and export_tag"
resp=$(run_tool '{"jsonrpc":"2.0","id":105,"method":"tools/call","params":{"name":"export_layers","arguments":{"filename":"'$t'/c.aseprite","output_directory":"'$t'/layers"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert "paint1.png" in t and "paint2.png" in t, repr(t)' || fail "export_layers: $resp"
[ -f "$t/layers/paint1.png" ] || fail "export_layers wrote no paint1.png: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":106,"method":"tools/call","params":{"name":"export_tag","arguments":{"filename":"'$t'/anim.aseprite","tag_name":"clip","output_filename":"'$t'/clip.gif","scale":4},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert "clip.gif" in t, repr(t)' || fail "export_tag: $resp"
[ -s "$t/clip.gif" ] || fail "export_tag wrote no gif: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":107,"method":"tools/call","params":{"name":"export_tag","arguments":{"filename":"'$t'/anim.aseprite","tag_name":"NOPE","output_filename":"'$t'/nope.gif"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Tag not found" in r["content"][0]["text"], repr(r)' || fail "export_tag missing tag: $resp"

echo "MCP tools: import_image_as_layer imports a PNG"
run_tool '{"jsonrpc":"2.0","id":120,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/importsrc.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":121,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"'$t'/importsrc.aseprite","layer_name":"Layer 1","frame_index":1,"pixels":[{"x":2,"y":2,"color":"#ff0000"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":122,"method":"tools/call","params":{"name":"export_frame","arguments":{"filename":"'$t'/importsrc.aseprite","frame_index":1,"output_filename":"'$t'/impsrc.png"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "import source export: $resp"
run_tool '{"jsonrpc":"2.0","id":123,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/import.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":124,"method":"tools/call","params":{"name":"import_image_as_layer","arguments":{"filename":"'$t'/import.aseprite","image_path":"'$t'/impsrc.png","layer_name":"ref"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert not r["isError"] and r["content"][0]["text"]=="OK", repr(r)' || fail "import_image_as_layer: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":125,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/import.aseprite","x":2,"y":2,"layer_name":"ref"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff0000"), repr(t)' || fail "imported pixel: $resp"

echo "MCP tools: render_onion_skin and compare_frames and get_color_stats"
resp=$(run_tool '{"jsonrpc":"2.0","id":112,"method":"tools/call","params":{"name":"render_onion_skin","arguments":{"filename":"'$t'/anim.aseprite","frame_index":2,"output_filename":"'$t'/onion.png","scale":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
python3 -c 'import struct; d=open("'$t'/onion.png","rb").read(24); assert struct.unpack(">II", d[16:24])==(16,16)' || fail "onion skin size: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":113,"method":"tools/call","params":{"name":"render_onion_skin","arguments":{"filename":"'$t'/anim.aseprite","frame_index":2,"output_filename":"'$t'/onion.png","ghost_opacity":300},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "ghost_opacity" in r["content"][0]["text"], repr(r)' || fail "onion skin validation: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":114,"method":"tools/call","params":{"name":"compare_frames","arguments":{"filename":"'$t'/anim.aseprite","frame_a":1,"frame_b":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; d=json.loads(t); assert d=={"frame_a":1,"frame_b":1,"changed_pixels":0,"total_pixels":64,"percent_changed":0.0}, repr(t)' || fail "compare identical frames: $resp"
run_tool '{"jsonrpc":"2.0","id":115,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"'$t'/anim.aseprite","layer_name":"Layer 1","frame_index":2,"pixels":[{"x":5,"y":5,"color":"#ff0000"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":116,"method":"tools/call","params":{"name":"compare_frames","arguments":{"filename":"'$t'/anim.aseprite","frame_a":1,"frame_b":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; d=json.loads(t); assert d=={"frame_a":1,"frame_b":2,"changed_pixels":1,"total_pixels":64,"percent_changed":1.56,"changed_bounds":{"x":5,"y":5,"width":1,"height":1}}, repr(t)' || fail "compare differing frames: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":117,"method":"tools/call","params":{"name":"get_color_stats","arguments":{"filename":"'$t'/anim.aseprite","frame_index":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; d=json.loads(t); assert d=={"frame":2,"unique_colors":1,"opaque_pixels":1,"top_colors":[{"color":"#FF0000","count":1}]}, repr(t)' || fail "color stats: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":118,"method":"tools/call","params":{"name":"get_color_stats","arguments":{"filename":"'$t'/anim.aseprite","top":0},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "top must be >= 1" in r["content"][0]["text"], repr(r)' || fail "color stats validation: $resp"

echo "MCP tools: scaled export_spritesheet is non-destructive in one session"
# One server process serves all three requests: an edit made before the
# export must survive the export's temporary resize (and its undo).
run_tool '{"jsonrpc":"2.0","id":130,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/undo.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null

mkfifo "$t/mcp-undo-in"
"$ASEPRITE" --mcp < "$t/mcp-undo-in" > "$t/mcp-undo-out" 2>/dev/null &
mcp_server=$!
# Watchdog: if the test stalls, kill the server after 15s so the suite
# can never hang on it. Disowned so it doesn't hold the test's stdout.
( sleep 15; kill "$mcp_server" 2>/dev/null ) >/dev/null 2>&1 &
mcp_watchdog=$!
disown
exec 3<> "$t/mcp-undo-in"

printf '{"jsonrpc":"2.0","id":131,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"%s/undo.aseprite","layer_name":"Layer 1","frame_index":1,"pixels":[{"x":3,"y":3,"color":"#ff0000"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":131,' "$t/mcp-undo-out" && break
    sleep 0.1
done
grep -q '"id":131,' "$t/mcp-undo-out" || fail "no reply to draw: $(cat "$t/mcp-undo-out")"

printf '{"jsonrpc":"2.0","id":132,"method":"tools/call","params":{"name":"export_spritesheet","arguments":{"filename":"%s/undo.aseprite","output_filename":"%s/undo-sheet.png","scale":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":132,' "$t/mcp-undo-out" && break
    sleep 0.1
done
grep -q '"id":132,' "$t/mcp-undo-out" || fail "no reply to scaled export: $(cat "$t/mcp-undo-out")"

printf '{"jsonrpc":"2.0","id":133,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"%s/undo.aseprite","x":3,"y":3,"layer_name":"Layer 1","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":133,' "$t/mcp-undo-out" && break
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
        r = m["result"]
        if m["id"] == 132:
            assert not r["isError"], r
        if m["id"] == 133:
            w["pixel"] = r["content"][0]["text"]
assert w.get("pixel", "").startswith("#ff0000"), w' "$t/mcp-undo-out" || fail "scaled export reverted the drawing: $(cat "$t/mcp-undo-out")"

echo "MCP tools: scaled export_spritesheet fails cleanly on a read-only document"
# Aseprite marks a document read-only when it loads a file with an
# unsupported chunk, so SpriteSize is disabled: the scaled export must
# report the resize failure and write nothing.
run_tool '{"jsonrpc":"2.0","id":134,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/ro.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
python3 - "$t/ro.aseprite" "$t/ro-bad.aseprite" <<'PY'
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
data = bytearray(open(src, "rb").read())
assert struct.unpack_from("<H", data, 4)[0] == 0xA5E0, "not an .aseprite file"
off = 128  # first frame header, after the 128-byte file header
frame_bytes, magic, old_count, duration = struct.unpack_from("<IHHH", data, off)
nchunks = struct.unpack_from("<I", data, off + 12)[0]
# Inject an unknown chunk (type 0x9999) before the frame's first chunk.
data[off + 16:off + 16] = struct.pack("<IH", 6, 0x9999)
struct.pack_into("<I", data, off, frame_bytes + 6)
struct.pack_into("<I", data, off + 12, nchunks + 1)
struct.pack_into("<I", data, 0, len(data))
open(dst, "wb").write(data)
PY
resp=$(run_tool '{"jsonrpc":"2.0","id":135,"method":"tools/call","params":{"name":"export_spritesheet","arguments":{"filename":"'$t'/ro-bad.aseprite","output_filename":"'$t'/ro-sheet.png","scale":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 2>/dev/null)
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "failed to resize sprite for export" in r["content"][0]["text"], repr(r)' || fail "read-only scaled export: $resp"
[ ! -f "$t/ro-sheet.png" ] || fail "read-only scaled export wrote a sheet: $resp"

echo "MCP tools: native filter commands"
run_tool '{"jsonrpc":"2.0","id":140,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/fx.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":141,"method":"tools/call","params":{"name":"draw_pixels_at","arguments":{"filename":"'$t'/fx.aseprite","layer_name":"Layer 1","frame_index":1,"pixels":[{"x":1,"y":1,"color":"#ff0000"},{"x":2,"y":2,"color":"#ff0000"}]},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":142,"method":"tools/call","params":{"name":"invert_colors","arguments":{"filename":"'$t'/fx.aseprite","layer_name":"Layer 1","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "invert_colors: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":143,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/fx.aseprite","x":1,"y":1,"layer_name":"Layer 1","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#00ffff (r=0, g=255, b=255, a=255)"), repr(t)' || fail "inverted pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":144,"method":"tools/call","params":{"name":"outline_native","arguments":{"filename":"'$t'/fx.aseprite","layer_name":"Layer 1","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "outline_native: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":145,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/fx.aseprite","x":0,"y":1,"layer_name":"Layer 1","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#000000 (r=0, g=0, b=0, a=255)"), repr(t)' || fail "outlined pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":146,"method":"tools/call","params":{"name":"adjust_hsl_native","arguments":{"filename":"'$t'/fx.aseprite","layer_name":"Layer 1","frame_index":1,"hue":45,"saturation":10,"lightness":-5},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "adjust_hsl_native: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":147,"method":"tools/call","params":{"name":"adjust_brightness_contrast","arguments":{"filename":"'$t'/fx.aseprite","layer_name":"Layer 1","frame_index":1,"brightness":10,"contrast":-5},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "adjust_brightness_contrast: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":148,"method":"tools/call","params":{"name":"apply_convolution","arguments":{"filename":"'$t'/fx.aseprite","matrix":"blur-3x3","layer_name":"Layer 1","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "apply_convolution: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":149,"method":"tools/call","params":{"name":"apply_convolution","arguments":{"filename":"'$t'/fx.aseprite","matrix":"nope","layer_name":"Layer 1","frame_index":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Unknown matrix" in r["content"][0]["text"], repr(r)' || fail "apply_convolution unknown matrix: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":150,"method":"tools/call","params":{"name":"extract_palette","arguments":{"filename":"'$t'/fx.aseprite","max_colors":4},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; d=json.loads(t); assert d["count"] == len(d["colors"]) and 1 <= d["count"] <= 4, repr(t); assert all(re.fullmatch(r"#[0-9A-F]{6}", c) for c in d["colors"]), repr(t)' || fail "extract_palette: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":151,"method":"tools/call","params":{"name":"list_convolution_matrices","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; m=json.loads(t); assert len(m) == 39, repr(t); assert "blur-3x3" in m and "misc-emboss" in m, repr(t)' || fail "list_convolution_matrices: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":152,"method":"tools/call","params":{"name":"animation_workflow_guide","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.strip(), repr(t)' || fail "animation_workflow_guide: $resp"

echo "MCP tools: text tools draw a bitmap font"
# A two-glyph sheet font ("A" and "B" are solid 6x7 boxes in 8x8 cells)
# generated headless with Aseprite itself, so the fixture needs no PNG encoder.
mkdir -p "$t/fonts/tiny"
cat > "$t/fonts/tiny/font.json" <<'JSON'
{
  "name": "tiny",
  "letter_gap": 1,
  "space_width": 2,
  "sheets": [{
    "file": "tiny.png",
    "cell_w": 8,
    "cell_h": 8,
    "ascent": 7,
    "chars": ["AB"]
  }]
}
JSON
cat > "$t/tiny_sheet.lua" <<LUA
local spr = Sprite(16, 8)
local img = Image(16, 8, ColorMode.RGB)
for y = 1, 7 do
  for x = 1, 6 do
    img:putPixel(x, y, Color(255, 255, 255, 255))
    img:putPixel(x + 8, y, Color(255, 255, 255, 255))
  end
end
spr:newCel(spr.layers[1], spr.frames[1], img, Point(0, 0))
spr:saveAs("$t/fonts/tiny/tiny.png")
LUA
"$ASEPRITE" -b --script "$t/tiny_sheet.lua" || fail "tiny font sheet generation failed"
[ -f "$t/fonts/tiny/tiny.png" ] || fail "tiny font sheet was not written"
export ASEPRITE_MCP_FONTS="$t/fonts"

resp=$(run_tool '{"jsonrpc":"2.0","id":160,"method":"tools/call","params":{"name":"list_text_fonts","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; f=json.loads(t); assert any(x["name"]=="tiny" and x["kind"]=="bitmap" for x in f), repr(t)' || fail "list_text_fonts: $resp"

resp=$(run_tool '{"jsonrpc":"2.0","id":161,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"tiny"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; m=re.search(r"width=(\d+)", t); assert m and int(m.group(1)) > 0, repr(t)' || fail "measure_text width: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":162,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"nope"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "not found" in r["content"][0]["text"], repr(r)' || fail "measure_text unknown font: $resp"

run_tool '{"jsonrpc":"2.0","id":163,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/text.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":164,"method":"tools/call","params":{"name":"draw_text","arguments":{"filename":"'$t'/text.aseprite","text":"A","x":2,"y":3,"font":"tiny","color":"#FF00FF","outline_color":"#00FF00"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "draw_text: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":165,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text.aseprite","x":2,"y":3},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff00ff"), repr(t)' || fail "draw_text pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":166,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text.aseprite","x":1,"y":3},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#00ff00"), repr(t)' || fail "draw_text outline pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":167,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text.aseprite","x":1,"y":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#00ff00"), repr(t)' || fail "draw_text diagonal outline pixel: $resp"

echo "MCP tools: text anchors, drop shadow, bitmap scaling and 4-way outline"
run_tool '{"jsonrpc":"2.0","id":168,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/text-anchor.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":169,"method":"tools/call","params":{"name":"draw_text","arguments":{"filename":"'$t'/text-anchor.aseprite","text":"A","x":8,"y":8,"font":"tiny","anchor":"center","color":"#FF00FF","shadow_color":"#0000FF","shadow_dx":2,"shadow_dy":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "draw_text center anchor: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":170,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-anchor.aseprite","x":5,"y":5},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff00ff"), repr(t)' || fail "center-anchored ink pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":171,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-anchor.aseprite","x":12,"y":7},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#0000ff"), repr(t)' || fail "shadow pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":172,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-anchor.aseprite","x":11,"y":6},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; m=re.search(r"a=(\d+)", t); assert m and m.group(1)=="0", repr(t)' || fail "pixel between ink and shadow: $resp"
run_tool '{"jsonrpc":"2.0","id":173,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/text-scale.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":174,"method":"tools/call","params":{"name":"draw_text","arguments":{"filename":"'$t'/text-scale.aseprite","text":"A","x":2,"y":1,"font":"tiny","size":2,"color":"#FF00FF"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "draw_text scale 2: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":175,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-scale.aseprite","x":4,"y":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff00ff"), repr(t)' || fail "scaled glyph pixel: $resp"
run_tool '{"jsonrpc":"2.0","id":176,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/text-4way.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
resp=$(run_tool '{"jsonrpc":"2.0","id":177,"method":"tools/call","params":{"name":"draw_text","arguments":{"filename":"'$t'/text-4way.aseprite","text":"A","x":2,"y":3,"font":"tiny","color":"#FF00FF","outline_color":"#00FF00","outline_diagonal":false},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | grep -q '"isError":false' || fail "draw_text 4-way outline: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":178,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-4way.aseprite","x":8,"y":9},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#00ff00"), repr(t)' || fail "4-way outline straight pixel: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":179,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-4way.aseprite","x":1,"y":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; m=re.search(r"a=(\d+)", t); assert m and m.group(1)=="0", repr(t)' || fail "4-way outline diagonal pixel: $resp"

echo "MCP tools: text bold passes, rasterization bounds and malformed fonts"
resp=$(run_tool '{"jsonrpc":"2.0","id":180,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"tiny","bold":1},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
resp2=$(run_tool '{"jsonrpc":"2.0","id":181,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"tiny","bold":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
python3 - "$resp" "$resp2" <<'PY' || fail "bold passes: $resp / $resp2"
import json, re, sys
def metrics(response):
    text = json.loads(response)["result"]["content"][0]["text"]
    m = re.search(r"width=(\d+) height=\d+ advance_width=(\d+)", text)
    return int(m.group(1)), int(m.group(2))
w1, a1 = metrics(sys.argv[1])
w2, a2 = metrics(sys.argv[2])
assert w2 > w1 and a2 > a1, (w1, a1, w2, a2)
PY
resp=$(run_tool '{"jsonrpc":"2.0","id":182,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"tiny","size":1024},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r)' || fail "measure_text huge size must fail: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":183,"method":"tools/call","params":{"name":"draw_text","arguments":{"filename":"'$t'/text.aseprite","text":"A","x":2,"y":3,"font":"tiny","size":1024,"color":"#FF00FF"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r)' || fail "draw_text huge size must fail: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":184,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"tiny","bold":99},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r)' || fail "measure_text huge bold must fail: $resp"

# Malformed descriptors: a negative origin and a cell grid running past
# the texture must be rejected cleanly when the font is loaded.
mkdir -p "$t/fonts/bad-origin" "$t/fonts/bad-cell"
cp "$t/fonts/tiny/tiny.png" "$t/fonts/bad-origin/tiny.png"
cp "$t/fonts/tiny/tiny.png" "$t/fonts/bad-cell/tiny.png"
cat > "$t/fonts/bad-origin/font.json" <<'JSON'
{ "name": "bad-origin", "letter_gap": 1, "space_width": 2,
  "sheets": [{ "file": "tiny.png", "cell_w": 8, "cell_h": 8, "ascent": 7, "origin": [-1, 0], "chars": ["AB"] }] }
JSON
cat > "$t/fonts/bad-cell/font.json" <<'JSON'
{ "name": "bad-cell", "letter_gap": 1, "space_width": 2,
  "sheets": [{ "file": "tiny.png", "cell_w": 64, "cell_h": 8, "ascent": 7, "chars": ["AB"] }] }
JSON
resp=$(run_tool '{"jsonrpc":"2.0","id":185,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"bad-origin"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Bad sheet" in r["content"][0]["text"], repr(r)' || fail "negative origin: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":186,"method":"tools/call","params":{"name":"measure_text","arguments":{"text":"A","font":"bad-cell"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r); assert "Bad sheet" in r["content"][0]["text"], repr(r)' || fail "cell past the sheet: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":187,"method":"tools/call","params":{"name":"draw_text","arguments":{"filename":"'$t'/text.aseprite","text":"A","x":2,"y":3,"font":"bad-origin","color":"#FF00FF"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; r=json.load(sys.stdin)["result"]; assert r["isError"], repr(r)' || fail "draw_text with malformed font: $resp"
# list_text_fonts only discovers fonts (it never loads them, like the
# reference), so the malformed descriptors cannot break the listing.
resp=$(run_tool '{"jsonrpc":"2.0","id":188,"method":"tools/call","params":{"name":"list_text_fonts","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; f=json.loads(json.load(sys.stdin)["result"]["content"][0]["text"]); names=[x["name"] for x in f]; assert "bad-origin" in names and "tiny" in names, repr(f)' || fail "list_text_fonts with malformed fonts: $resp"

echo "MCP tools: draw_text defaults to the active layer"
run_tool '{"jsonrpc":"2.0","id":189,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/text-active.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
run_tool '{"jsonrpc":"2.0","id":190,"method":"tools/call","params":{"name":"add_layer","arguments":{"filename":"'$t'/text-active.aseprite","layer_name":"top"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null

mkfifo "$t/mcp-text-in"
"$ASEPRITE" --mcp < "$t/mcp-text-in" > "$t/mcp-text-out" 2>/dev/null &
mcp_server=$!
# Watchdog: if the test stalls, kill the server after 15s so the suite
# can never hang on it. Disowned so it doesn't hold the test's stdout.
( sleep 15; kill "$mcp_server" 2>/dev/null ) >/dev/null 2>&1 &
mcp_watchdog=$!
disown
exec 3<> "$t/mcp-text-in"

# set_layer saves the file, which makes the next call reload the
# document; the reload must keep the active layer chosen here.
printf '{"jsonrpc":"2.0","id":191,"method":"tools/call","params":{"name":"set_layer","arguments":{"filename":"%s/text-active.aseprite","layer_name":"top"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":191,' "$t/mcp-text-out" && break
    sleep 0.1
done
grep -q '"id":191,' "$t/mcp-text-out" || fail "no reply to set_layer: $(cat "$t/mcp-text-out")"

printf '{"jsonrpc":"2.0","id":192,"method":"tools/call","params":{"name":"draw_text","arguments":{"filename":"%s/text-active.aseprite","text":"A","x":2,"y":3,"font":"tiny","color":"#FF00FF"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":192,' "$t/mcp-text-out" && break
    sleep 0.1
done

exec 3>&-
kill "$mcp_server" 2>/dev/null
wait "$mcp_server" 2>/dev/null
kill "$mcp_watchdog" 2>/dev/null

python3 -c 'import json,sys
for line in open(sys.argv[1]):
    line = line.strip()
    if not line:
        continue
    m = json.loads(line)
    if m["id"] in (191, 192):
        assert not m["result"]["isError"], m' "$t/mcp-text-out" || fail "set_layer/draw_text failed: $(cat "$t/mcp-text-out")"

resp=$(run_tool '{"jsonrpc":"2.0","id":193,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-active.aseprite","x":2,"y":3,"layer_name":"top"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert t.startswith("#ff00ff"), repr(t)' || fail "text must land on the active layer: $resp"
resp=$(run_tool '{"jsonrpc":"2.0","id":194,"method":"tools/call","params":{"name":"get_pixel_color","arguments":{"filename":"'$t'/text-active.aseprite","x":2,"y":3,"layer_name":"Layer 1"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
printf '%s' "$resp" | python3 -c 'import sys,json,re; t=json.load(sys.stdin)["result"]["content"][0]["text"]; m=re.search(r"a=(\d+)", t); assert m and m.group(1)=="0", repr(t)' || fail "text must not land on the bottom layer: $resp"

echo "MCP tools: preview server serves exported files over HTTP"
# The preview server runs inside the MCP process, so this session has to
# stay alive (FIFO, like the cache test) while curl talks to the server.
run_tool '{"jsonrpc":"2.0","id":200,"method":"tools/call","params":{"name":"export_frame","arguments":{"filename":"'$t'/c.aseprite","frame_index":1,"output_filename":"'$t'/preview.png"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
[ -f "$t/preview.png" ] || fail "preview fixture was not exported"

mkfifo "$t/mcp-preview-in"
"$ASEPRITE" --mcp < "$t/mcp-preview-in" > "$t/mcp-preview-out" 2>/dev/null &
mcp_server=$!
# Watchdog: if the test stalls, kill the server after 15s so the suite
# can never hang on it. Disowned so it doesn't hold the test's stdout.
( sleep 15; kill "$mcp_server" 2>/dev/null ) >/dev/null 2>&1 &
mcp_watchdog=$!
disown
exec 3<> "$t/mcp-preview-in"

# Sends one tool call to the session and waits for its reply.
preview_send() { # id, tool, arguments-json
    printf '{"jsonrpc":"2.0","id":%s,"method":"tools/call","params":{"name":"%s","arguments":%s,"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$1" "$2" "$3" >&3
    for i in $(seq 1 100); do
        grep -q '"id":'"$1"',' "$t/mcp-preview-out" && return 0
        sleep 0.1
    done
    return 1
}

preview_send 201 start_preview_server '{"directory":"'$t'","port":18100}' || fail "no reply to start_preview_server"
grep -q '"id":201,"jsonrpc":"2.0","result"' "$t/mcp-preview-out" || fail "start_preview_server: $(cat "$t/mcp-preview-out")"

served=$(curl -s --max-time 5 -o "$t/served.png" -w '%{http_code} %{content_type}' "http://127.0.0.1:18100/preview.png") || fail "curl could not reach the preview server"
[ "$served" = "200 image/png" ] || fail "preview server served PNG: $served"
python3 -c 'assert open("'$t'/served.png","rb").read(4) == b"\x89PNG"' || fail "served file is not a PNG"
code=$(curl -s --max-time 5 --path-as-is -o /dev/null -w '%{http_code}' "http://127.0.0.1:18100/../../etc/hosts")
[ "$code" = "403" ] || fail "preview path traversal should be 403, got $code"
code=$(curl -s --max-time 5 -o /dev/null -w '%{http_code}' "http://127.0.0.1:18100/nope.png")
[ "$code" = "404" ] || fail "missing preview file should be 404, got $code"

preview_send 202 start_preview_server '{"directory":"'$t'","port":18100}' || fail "no reply to the second start_preview_server"
preview_send 203 start_preview_server '{"directory":"'$t'/nope","port":18101}' || fail "no reply to the missing-directory start"
preview_send 204 start_preview_server '{"directory":"'$t'","port":99999}' || fail "no reply to the invalid-port start"
preview_send 205 stop_preview_server '{"port":18100}' || fail "no reply to stop_preview_server"

# The port must be closed after the stop.
curl -s --max-time 2 -o /dev/null "http://127.0.0.1:18100/preview.png" && fail "preview server still serving after stop"

preview_send 206 stop_preview_server '{"port":18100}' || fail "no reply to the second stop_preview_server"

exec 3>&-
kill "$mcp_server" 2>/dev/null
wait "$mcp_server" 2>/dev/null
kill "$mcp_watchdog" 2>/dev/null

python3 - "$t/mcp-preview-out" <<'PY' || fail "preview server replies: $(cat "$t/mcp-preview-out")"
import json
import sys

replies = {}
for line in open(sys.argv[1]):
    line = line.strip()
    if line:
        m = json.loads(line)
        replies[m["id"]] = m["result"]

def text(i):
    return replies[i]["content"][0]["text"]

assert not replies[201]["isError"], replies[201]
assert "18100" in text(201), text(201)
assert replies[202]["isError"] and "already running" in text(202), text(202)
assert replies[203]["isError"] and "not found" in text(203), text(203)
assert replies[204]["isError"] and "Port" in text(204), text(204)
assert not replies[205]["isError"] and "18100" in text(205), text(205)
assert replies[206]["isError"] and "No preview server running" in text(206), text(206)
PY

echo "MCP tools: 116 tools, deterministic order"
run_list() { printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp; }
a=$(run_list)
b=$(run_list)
[ "$a" = "$b" ] || fail "tools/list is not deterministic"
printf '%s' "$a" | python3 -c 'import sys,json; r=json.load(sys.stdin); assert len(r["result"]["tools"])==116, len(r["result"]["tools"])' || fail "expected 116 tools"

echo "MCP tools: undo history survives a saving tool's own save"
# One server process serves all three requests: add_layer saves the file
# itself, so without a post-run stat refresh the next call would reload
# the document and app.undo() would have nothing to undo. (The drawing
# tools write cel images directly, which Aseprite's undo history does
# not capture, so the observable undoable change here is a new layer.)
run_tool '{"jsonrpc":"2.0","id":208,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/undo-keep.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null

mkfifo "$t/mcp-undo-keep-in"
"$ASEPRITE" --mcp < "$t/mcp-undo-keep-in" > "$t/mcp-undo-keep-out" 2>/dev/null &
mcp_server=$!
# Watchdog: if the test stalls, kill the server after 15s so the suite
# can never hang on it. Disowned so it doesn't hold the test's stdout.
( sleep 15; kill "$mcp_server" 2>/dev/null ) >/dev/null 2>&1 &
mcp_watchdog=$!
disown
exec 3<> "$t/mcp-undo-keep-in"

printf '{"jsonrpc":"2.0","id":210,"method":"tools/call","params":{"name":"add_layer","arguments":{"filename":"%s/undo-keep.aseprite","layer_name":"ink"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":210,' "$t/mcp-undo-keep-out" && break
    sleep 0.1
done
grep -q '"id":210,' "$t/mcp-undo-keep-out" || fail "no reply to add_layer: $(cat "$t/mcp-undo-keep-out")"

printf '{"jsonrpc":"2.0","id":211,"method":"tools/call","params":{"name":"run_lua_script","arguments":{"filename":"%s/undo-keep.aseprite","script":"app.undo()"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":211,' "$t/mcp-undo-keep-out" && break
    sleep 0.1
done
grep -q '"id":211,' "$t/mcp-undo-keep-out" || fail "no reply to undo: $(cat "$t/mcp-undo-keep-out")"

printf '{"jsonrpc":"2.0","id":212,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"%s/undo-keep.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
for i in $(seq 1 100); do
    grep -q '"id":212,' "$t/mcp-undo-keep-out" && break
    sleep 0.1
done
exec 3>&-
kill "$mcp_server" 2>/dev/null
wait "$mcp_server" 2>/dev/null
kill "$mcp_watchdog" 2>/dev/null

python3 -c 'import json,sys
replies = {}
for line in open(sys.argv[1]):
    line = line.strip()
    if line:
        m = json.loads(line)
        replies[m["id"]] = m["result"]
assert not replies[210]["isError"], replies[210]
assert not replies[211]["isError"], replies[211]
assert not replies[212]["isError"], replies[212]
info = json.loads(replies[212]["content"][0]["text"])
names = [l["name"] for l in info["layers"]]
assert "ink" not in names, names' "$t/mcp-undo-keep-out" || fail "undo did not remove the saving tool's layer (document was reloaded): $(cat "$t/mcp-undo-keep-out")"

echo "MCP tools: symlink spellings of one file share the document"
if [[ "$(uname)" =~ "MINGW" ]] || [[ "$(uname)" =~ "MSYS" ]] ; then
    echo "Ignore symlink-spelling test on Windows"
else
    run_tool '{"jsonrpc":"2.0","id":219,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/canon.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
    if ln -s "$t" "$t/alias" 2>/dev/null && [ -L "$t/alias" ]; then
        mkfifo "$t/mcp-canon-in"
        "$ASEPRITE" --mcp < "$t/mcp-canon-in" > "$t/mcp-canon-out" 2>/dev/null &
        mcp_server=$!
        # Watchdog: if the test stalls, kill the server after 15s so the
        # suite can never hang on it. Disowned so it doesn't hold the
        # test's stdout.
        ( sleep 15; kill "$mcp_server" 2>/dev/null ) >/dev/null 2>&1 &
        mcp_watchdog=$!
        disown
        exec 3<> "$t/mcp-canon-in"

        printf '{"jsonrpc":"2.0","id":220,"method":"tools/call","params":{"name":"add_layer","arguments":{"filename":"%s/canon.aseprite","layer_name":"ink"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
        for i in $(seq 1 100); do
            grep -q '"id":220,' "$t/mcp-canon-out" && break
            sleep 0.1
        done
        grep -q '"id":220,' "$t/mcp-canon-out" || fail "no reply to canonical add_layer: $(cat "$t/mcp-canon-out")"

        # The undo goes through the symlinked spelling; it can only
        # undo the layer when both spellings are the same document.
        printf '{"jsonrpc":"2.0","id":221,"method":"tools/call","params":{"name":"run_lua_script","arguments":{"filename":"%s/alias/canon.aseprite","script":"app.undo()"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
        for i in $(seq 1 100); do
            grep -q '"id":221,' "$t/mcp-canon-out" && break
            sleep 0.1
        done
        grep -q '"id":221,' "$t/mcp-canon-out" || fail "no reply to aliased undo: $(cat "$t/mcp-canon-out")"

        printf '{"jsonrpc":"2.0","id":222,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"%s/alias/canon.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}\n' "$t" >&3
        for i in $(seq 1 100); do
            grep -q '"id":222,' "$t/mcp-canon-out" && break
            sleep 0.1
        done
        exec 3>&-
        kill "$mcp_server" 2>/dev/null
        wait "$mcp_server" 2>/dev/null
        kill "$mcp_watchdog" 2>/dev/null

        python3 -c 'import json,sys
replies = {}
for line in open(sys.argv[1]):
    line = line.strip()
    if line:
        m = json.loads(line)
        replies[m["id"]] = m["result"]
assert not replies[220]["isError"], replies[220]
assert not replies[221]["isError"], replies[221]
assert not replies[222]["isError"], replies[222]
info = json.loads(replies[222]["content"][0]["text"])
names = [l["name"] for l in info["layers"]]
assert "ink" not in names, names' "$t/mcp-canon-out" || fail "symlink spelling opened a second document: $(cat "$t/mcp-canon-out")"
    else
        echo "Ignore symlink-spelling test (cannot create symlinks)"
    fi
fi

echo "MCP tools: repeated create/edit calls keep exactly one document"

mkfifo "$t/mcp-tabs-in"
"$ASEPRITE" --mcp < "$t/mcp-tabs-in" > "$t/mcp-tabs-out" 2>/dev/null &
mcp_server=$!
# Watchdog: if the test stalls, kill the server after 15s so the suite
# can never hang on it. Disowned so it doesn't hold the test's stdout.
( sleep 15; kill "$mcp_server" 2>/dev/null ) >/dev/null 2>&1 &
mcp_watchdog=$!
disown
exec 3<> "$t/mcp-tabs-in"

send_tabs() {
    printf '%s\n' "$1" >&3
    for i in $(seq 1 100); do
        grep -q "\"id\":$2," "$t/mcp-tabs-out" && break
        sleep 0.1
    done
    grep -q "\"id\":$2," "$t/mcp-tabs-out" || fail "no reply to id $2: $(cat "$t/mcp-tabs-out")"
}

send_tabs '{"jsonrpc":"2.0","id":230,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/tabs.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 230
send_tabs '{"jsonrpc":"2.0","id":231,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/tabs.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 231
send_tabs '{"jsonrpc":"2.0","id":232,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/tabs.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 232
send_tabs '{"jsonrpc":"2.0","id":233,"method":"tools/call","params":{"name":"draw_rectangle","arguments":{"filename":"'$t'/tabs.aseprite","x":1,"y":1,"width":4,"height":4,"color":"#FF0000","fill":true},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 233
send_tabs '{"jsonrpc":"2.0","id":234,"method":"tools/call","params":{"name":"run_lua_script","arguments":{"script":"print(\"COUNT=\"..#app.sprites)"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 234
# An export creates a temporary clone sprite; it must not leave a
# document behind either.
send_tabs '{"jsonrpc":"2.0","id":235,"method":"tools/call","params":{"name":"export_frame","arguments":{"filename":"'$t'/tabs.aseprite","frame_index":1,"output_filename":"'$t'/tabs-exp.png","scale":2},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 235
send_tabs '{"jsonrpc":"2.0","id":236,"method":"tools/call","params":{"name":"run_lua_script","arguments":{"script":"print(\"COUNT=\"..#app.sprites)"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' 236

exec 3>&-
kill "$mcp_server" 2>/dev/null
wait "$mcp_server" 2>/dev/null
kill "$mcp_watchdog" 2>/dev/null

python3 -c 'import json,sys
replies = {}
for line in open(sys.argv[1]):
    line = line.strip()
    if line:
        m = json.loads(line)
        replies[m["id"]] = m["result"]
for k in (230, 231, 232, 233, 234, 235, 236):
    assert not replies[k]["isError"], (k, replies[k])
count_text = replies[234]["content"][0]["text"]
assert "COUNT=1" in count_text, count_text
count_text = replies[236]["content"][0]["text"]
assert "COUNT=1" in count_text, count_text' "$t/mcp-tabs-out" || fail "repeated calls left extra documents: $(cat "$t/mcp-tabs-out")"
