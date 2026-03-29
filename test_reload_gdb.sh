#!/bin/bash
# Run reload test under GDB to catch SIGSEGV
set -e

FRANKENPHP="./caddy/frankenphp/frankenphp"
ENTRYPOINT="./async_entrypoint.php"

CADDYFILE_V1=$(mktemp /tmp/Caddyfile.v1.XXXXXX)
CADDYFILE_V2=$(mktemp /tmp/Caddyfile.v2.XXXXXX)

cat > "$CADDYFILE_V1" <<EOF
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

cat > "$CADDYFILE_V2" <<EOF
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
			buffer_size 19
			match /*
		}
	}
}
EOF

cleanup() {
    if [ -f "${ENTRYPOINT}.bak" ]; then
        cp "${ENTRYPOINT}.bak" "$ENTRYPOINT"
        rm "${ENTRYPOINT}.bak"
    fi
    rm -f "$CADDYFILE_V1" "$CADDYFILE_V2"
}
trap cleanup EXIT

# Start under GDB
cat > /tmp/gdb_commands.txt <<'GDBEOF'
set pagination off
set confirm off
handle SIGPIPE nostop noprint pass
run
GDBEOF

echo "=== Starting FrankenPHP under GDB ==="
gdb -batch -x /tmp/gdb_commands.txt --args $FRANKENPHP run --config "$CADDYFILE_V1" &
GDB_PID=$!

sleep 3

echo "=== Testing initial request ==="
curl -s http://localhost:8085/api/ping
echo ""

echo "=== Modifying PHP entrypoint ==="
cp "$ENTRYPOINT" "${ENTRYPOINT}.bak"
sed -i "s/'pong' => true/'pong' => true, 'reloaded' => true/" "$ENTRYPOINT"

echo "=== Triggering reload ==="
CONFIG_JSON=$($FRANKENPHP adapt --config "$CADDYFILE_V2" 2>/dev/null)
curl -s -X POST -H "Content-Type: application/json" -d "$CONFIG_JSON" http://localhost:2019/load
echo ""

sleep 3

echo "=== Testing after reload ==="
curl -s http://localhost:8085/api/ping
echo ""

echo "=== Sending SIGTERM ==="
# Send to the actual frankenphp process, not gdb
FRANKENPHP_PID=$(pgrep -f "frankenphp run --config")
if [ -n "$FRANKENPHP_PID" ]; then
    kill -TERM $FRANKENPHP_PID
fi

wait $GDB_PID 2>&1
echo "=== GDB exited with: $? ==="
