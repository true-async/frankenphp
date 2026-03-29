#!/bin/bash
set -e

FRANKENPHP="./caddy/frankenphp/frankenphp"
ENTRYPOINT="./async_entrypoint.php"
URL="http://localhost:8085/api/ping"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

ok()   { echo -e "${GREEN}[OK]${NC} $1"; }
fail() { echo -e "${RED}[FAIL]${NC} $1"; exit 1; }
info() { echo -e "${YELLOW}[INFO]${NC} $1"; }

CADDYFILE=$(mktemp /tmp/Caddyfile.reload-test.XXXXXX)
cat > "$CADDYFILE" <<EOF
{
	admin localhost:2019
	frankenphp
}

:8085 {
	root * .
	php_server {
		index off
		file_server off
		worker {
			file ${ENTRYPOINT}
			num 1
			async
			buffer_size 20
			match /*
		}
	}
}
EOF

cleanup() {
    info "Stopping FrankenPHP..."
    kill $SERVER_PID 2>/dev/null || true
    wait $SERVER_PID 2>/dev/null || true
    if [ -f "${ENTRYPOINT}.bak" ]; then
        cp "${ENTRYPOINT}.bak" "$ENTRYPOINT"
        rm "${ENTRYPOINT}.bak"
    fi
    rm -f "$CADDYFILE"
    info "Cleanup done."
}

trap cleanup EXIT

# --- Step 1: Start server ---
info "Starting FrankenPHP with async worker..."
$FRANKENPHP run --config "$CADDYFILE" 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 $SERVER_PID 2>/dev/null; then
    fail "Server failed to start"
fi

# --- Step 2: Test initial requests ---
info "Testing initial requests..."
RESP=$(curl -s -o /dev/null -w "%{http_code}" "$URL")
if [ "$RESP" = "200" ]; then
    ok "Initial request: 200"
else
    fail "Initial request: $RESP (expected 200)"
fi

PING_BEFORE=$(curl -s "$URL")
info "Before: $PING_BEFORE"

INFO_BEFORE=$(curl -s "http://localhost:8085/api/info")
UPTIME_BEFORE=$(echo "$INFO_BEFORE" | grep -o '"uptime_s":[^,}]*' | cut -d: -f2 | tr -d ' ')
info "Uptime before: ${UPTIME_BEFORE}s"

# --- Step 3: Modify PHP script ---
info "Modifying PHP entrypoint..."
cp "$ENTRYPOINT" "${ENTRYPOINT}.bak"
sed -i "s/'pong' => true/'pong' => true, 'reloaded' => true/" "$ENTRYPOINT"

# --- Step 4: Trigger RestartWorkers() via admin API ---
info "Calling POST /workers/restart (triggers RestartWorkers directly)..."
RESTART_RESP=$(curl -s -w "\n%{http_code}" -X POST http://localhost:2019/frankenphp/workers/restart)
RESTART_CODE=$(echo "$RESTART_RESP" | tail -1)
RESTART_BODY=$(echo "$RESTART_RESP" | head -n -1)
info "Admin API response: $RESTART_CODE - $RESTART_BODY"

if [ "$RESTART_CODE" = "200" ]; then
    ok "RestartWorkers() called successfully"
else
    fail "RestartWorkers() failed: HTTP $RESTART_CODE - $RESTART_BODY"
fi

# --- Step 5: Wait and test ---
info "Waiting for new async worker..."
sleep 2

FOUND=false
for i in $(seq 1 10); do
    RESP=$(curl -s "$URL" 2>/dev/null || echo "")
    if echo "$RESP" | grep -q '"reloaded"'; then
        FOUND=true
        ok "New worker serving (reload marker found)"
        break
    fi
    sleep 0.5
done

if [ "$FOUND" = false ]; then
    fail "Old worker still serving. Response: $RESP"
fi

# --- Step 6: Verify ---
RESP_CODE=$(curl -s -o /dev/null -w "%{http_code}" "$URL")
if [ "$RESP_CODE" = "200" ]; then
    ok "Post-reload: 200"
else
    fail "Post-reload: $RESP_CODE"
fi

PING_AFTER=$(curl -s "$URL")
info "After: $PING_AFTER"

INFO_AFTER=$(curl -s "http://localhost:8085/api/info")
UPTIME_AFTER=$(echo "$INFO_AFTER" | grep -o '"uptime_s":[^,}]*' | cut -d: -f2 | tr -d ' ')
info "Uptime after: ${UPTIME_AFTER}s (was ${UPTIME_BEFORE}s) — should be lower (fresh worker)"

echo ""
echo -e "${GREEN}=== ALL TESTS PASSED ===${NC}"
