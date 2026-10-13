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

echo "MCP tools: external modification invalidates doc cache"
printf '%s\n' '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":8,"height":8,"filename":"'$t'/m.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp >/dev/null
printf 'local spr = Sprite(16, 16)\nspr:saveAs("%s/m.aseprite")\n' "$t" > "$t/mk.lua"
"$ASEPRITE" -b --script "$t/mk.lua" || fail "fixture regeneration failed"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/m.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
printf '%s' "$resp" | python3 -c 'import sys,json; t=json.load(sys.stdin)["result"]["content"][0]["text"]; assert json.loads(t)["width"] == 16, repr(t)' || fail "stale cache served: $resp"

echo "MCP tools: load error stays pure JSON on stdout"
printf 'garbage, not a sprite\n' > "$t/bad.aseprite"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/bad.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp 2>/dev/null)
printf '%s' "$resp" | python3 -c 'import sys,json; [json.loads(l) for l in sys.stdin if l.strip()]' || fail "load error leaked to stdout: $resp"
printf '%s' "$resp" | python3 -c 'import sys,json; assert json.load(sys.stdin)["result"]["isError"]' || fail "corrupt file should be an error result: $resp"
