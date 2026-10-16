#! /bin/bash
# Copyright (C) 2026 Igara Studio S.A.

PORT=18099
"$ASEPRITE" --mcp-http=$PORT >"$t/mcp-http.out" 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT

ready=0
for i in $(seq 1 50); do
  if curl -s --max-time 2 -o /dev/null "http://127.0.0.1:$PORT/mcp" -X POST; then
    ready=1
    break
  fi
  sleep 0.1
done
[ "$ready" = 1 ] || fail "MCP http server did not start: $(cat "$t/mcp-http.out")"

echo "MCP http: modern discover over HTTP"
resp=$(curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":1,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
echo "$resp" | grep -q '"supportedVersions"' || fail "http discover: $resp"

echo "MCP http: header-only modern request is dispatched as modern"
resp=$(curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":8,"method":"server/discover","params":{}}')
echo "$resp" | grep -q '"supportedVersions"' || fail "header-only discover: $resp"

echo "MCP http: header mismatch -> 400 and -32020"
body='{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"create_canvas","arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}'
code=$(curl -s --max-time 10 -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' -H 'Mcp-Name: other' -d "$body")
[ "$code" = "400" ] || fail "expected 400, got $code"
grep -q '\-32020' "$t/out" || fail "expected HeaderMismatch"

echo "MCP http: tools/call without Mcp-Name and params.name -> 400 and -32020"
code=$(curl -s --max-time 10 -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' \
  -d '{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"arguments":{},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ "$code" = "400" ] || fail "expected 400, got $code"
grep -q '\-32020' "$t/out" || fail "expected HeaderMismatch"

echo "MCP http: base64 sentinel Mcp-Name accepted"
code=$(curl -s --max-time 10 -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: tools/call' -H 'Mcp-Name: =?base64?Y3JlYXRlX2NhbnZhcw==?=' \
  -d '{"jsonrpc":"2.0","id":10,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":4,"height":4,"filename":"'$t'/b64.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ "$code" = "200" ] || fail "base64 Mcp-Name should be accepted, got $code: $(cat $t/out)"
[ -f "$t/b64.aseprite" ] || fail "base64 Mcp-Name call did not run: $(cat $t/out)"

echo "MCP http: unsupported version -> 400 and -32022"
code=$(curl -s --max-time 10 -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 1900-01-01' \
  -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":3,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"1900-01-01"}}}')
[ "$code" = "400" ] || fail "expected 400, got $code"
grep -q '\-32022' "$t/out" || fail "expected UnsupportedProtocolVersion"
grep -q '"supported":\["2026-07-28"\]' "$t/out" || fail "expected supported list"

echo "MCP http: non-JSON Content-Type -> 415"
code=$(curl -s --max-time 10 -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: text/plain' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":11,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ "$code" = "415" ] || fail "non-JSON Content-Type should be 415, got $code"
grep -q '\-32600' "$t/out" || fail "expected invalid-request error body: $(cat "$t/out")"

echo "MCP http: GET/DELETE -> 405; unknown path -> 404; unknown method -> 404; notification -> 202"
code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/mcp")
[ "$code" = "405" ] || fail "GET should be 405, got $code"
code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' -X DELETE "http://127.0.0.1:$PORT/mcp")
[ "$code" = "405" ] || fail "DELETE should be 405, got $code"
code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/other")
[ "$code" = "404" ] || fail "unknown path should be 404, got $code"
code=$(curl -s --max-time 10 -o "$t/out" -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: nope' -d '{"jsonrpc":"2.0","id":4,"method":"nope","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ "$code" = "404" ] || fail "unknown method should be 404, got $code"
grep -q '\-32601' "$t/out" || fail "expected MethodNotFound"
code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Content-Type: application/json' -H 'MCP-Protocol-Version: 2026-07-28' \
  -H 'Mcp-Method: notifications/initialized' -d '{"jsonrpc":"2.0","method":"notifications/initialized"}')
[ "$code" = "202" ] || fail "notification should be 202, got $code"

echo "MCP http: legacy initialize over HTTP"
resp=$(curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":5,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}')
echo "$resp" | grep -q '"protocolVersion":"2025-06-18"' || fail "legacy http initialize: $resp"

echo "MCP http: legacy initialize WITH its version header (real 2025-11-25 client)"
resp=$(curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":51,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"opencode","version":"2.0.29"}}}')
echo "$resp" | grep -q '"protocolVersion":"2025-11-25"' || fail "legacy init with version header: $resp"

echo "MCP http: legacy tools/list WITH its version header"
resp=$(curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2025-11-25' \
  -d '{"jsonrpc":"2.0","id":52,"method":"tools/list"}')
echo "$resp" | grep -q '"tools":\[' || fail "legacy tools/list with version header: $resp"

echo "MCP http: modern body _meta without the version header is served"
resp=$(curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":53,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
echo "$resp" | grep -q '"supportedVersions"' || fail "meta-only discover: $resp"

echo "MCP http: bad Origin -> 403"
code=$(curl -s --max-time 10 -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$PORT/mcp" \
  -H 'Origin: http://evil.example' -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: server/discover' \
  -d '{"jsonrpc":"2.0","id":6,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}')
[ "$code" = "403" ] || fail "bad Origin should be 403, got $code"

echo "MCP http: concurrent requests both succeed"
# Ensure the fixture used by the parallel calls exists
curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: create_canvas' \
  -d '{"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"create_canvas","arguments":{"width":16,"height":16,"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}' >/dev/null
body='{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"get_sprite_info","arguments":{"filename":"'$t'/c.aseprite"},"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}}'
curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: get_sprite_info' -d "$body" > "$t/r1" &
C1=$!
curl -s --max-time 10 -X POST "http://127.0.0.1:$PORT/mcp" -H 'Content-Type: application/json' \
  -H 'MCP-Protocol-Version: 2026-07-28' -H 'Mcp-Method: tools/call' -H 'Mcp-Name: get_sprite_info' -d "$body" > "$t/r2" &
C2=$!
wait "$C1" "$C2"
python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); assert r["result"]["isError"] is False, r; i=json.loads(r["result"]["content"][0]["text"]); assert i["width"]==16 and i["height"]==16, i' "$t/r1" || fail "parallel request 1: $(cat $t/r1)"
python3 -c 'import json,sys; r=json.load(open(sys.argv[1])); assert r["result"]["isError"] is False, r; i=json.loads(r["result"]["content"][0]["text"]); assert i["width"]==16 and i["height"]==16, i' "$t/r2" || fail "parallel request 2: $(cat $t/r2)"

echo "MCP http: port already in use exits non-zero"
"$ASEPRITE" --mcp-http=$PORT >"$t/out2" 2>&1 &
BUSY=$!
busyDone=0
for i in $(seq 1 100); do
  if ! kill -0 "$BUSY" 2>/dev/null; then
    busyDone=1
    break
  fi
  sleep 0.1
done
if [ "$busyDone" != 1 ]; then
  # Windows SO_REUSEADDR lets a second socket share the port.
  kill "$BUSY" 2>/dev/null
  wait "$BUSY" 2>/dev/null
  if [[ "$(uname)" =~ "MINGW" ]] || [[ "$(uname)" =~ "MSYS" ]] ; then
    echo "Ignore port-in-use test on Windows"
  else
    fail "second --mcp-http kept running on a busy port"
  fi
else
  wait "$BUSY"
  [ "$?" != 0 ] || fail "expected non-zero exit"
  grep -qi 'port\|listen\|address' "$t/out2" || fail "expected port error, got: $(cat $t/out2)"
fi

echo "MCP http: invalid ports are rejected"
"$ASEPRITE" --mcp-http=99999 >"$t/out3" 2>&1 && fail "expected non-zero exit for port 99999"
grep -qi 'port' "$t/out3" || fail "expected port error, got: $(cat $t/out3)"
"$ASEPRITE" --mcp-http=abc >"$t/out3" 2>&1 && fail "expected non-zero exit for port abc"
"$ASEPRITE" --mcp-http=0 >"$t/out3" 2>&1 && fail "expected non-zero exit for port 0"
"$ASEPRITE" --mcp-http=12x >"$t/out3" 2>&1 && fail "expected non-zero exit for port 12x"

kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null
trap - EXIT
