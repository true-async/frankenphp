#!/bin/bash
set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="$ROOT_DIR/caddy/frankenphp/frankenphp"
CORE="${1:-/mnt/wslg/dumps/core.php-1}"
RUNTIME_GDB="$HOME/go/pkg/mod/golang.org/toolchain@v0.0.1-go1.25.4.linux-amd64/src/runtime/runtime-gdb.py"
GDB_HOME="$ROOT_DIR/.gdb-home"
GDB_INIT="$GDB_HOME/.gdbinit"

if [ ! -f "$CORE" ]; then
  echo "Core file not found: $CORE" >&2
  exit 1
fi

# Prepare isolated gdb HOME with safe-path settings
mkdir -p "$GDB_HOME"
cat > "$GDB_INIT" <<EOF
set auto-load safe-path /
add-auto-load-safe-path /usr/local/lib
add-auto-load-safe-path $RUNTIME_GDB
set pagination off
EOF

export HOME="$GDB_HOME"
gdb -q "$BIN" "$CORE" \
  -ex "source $RUNTIME_GDB" \
  -ex "set solib-search-path /usr/local/lib" \
  -ex "info goroutines" \
  -ex "goroutine 1 bt" \
  -ex "thread apply all bt" \
  -ex "quit"
