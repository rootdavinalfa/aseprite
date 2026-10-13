#! /bin/bash
# Copyright (C) 2026 Igara Studio S.A.

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
