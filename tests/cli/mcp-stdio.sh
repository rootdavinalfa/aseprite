#! /bin/bash
# Copyright (C) 2026 Igara Studio S.A.

echo "MCP stdio: server/discover"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"supportedVersions":\["2026-07-28"\]' || fail "discover supportedVersions: $resp"
echo "$resp" | grep -q '"resultType":"complete"' || fail "discover resultType: $resp"
echo "$resp" | grep -q '"io.modelcontextprotocol/serverInfo"' || fail "discover serverInfo: $resp"

echo "MCP stdio: tools/list carries the registered tools"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"name":"create_canvas"' || fail "tools list create_canvas: $resp"
echo "$resp" | grep -q '"name":"get_sprite_info"' || fail "tools list get_sprite_info: $resp"
echo "$resp" | grep -q '"name":"run_lua_script"' || fail "tools list run_lua_script: $resp"

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

echo "MCP stdio: legacy initialize echoes supported version"
resp=$(printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"1"}}}' | "$ASEPRITE" --mcp)
echo "$resp" | grep -q '"protocolVersion":"2024-11-05"' || fail "initialize echo: $resp"
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
[ "$(printf '%s\n' "$resp" | grep -c .)" = "1" ] || fail "expected exactly one reply line: $resp"
