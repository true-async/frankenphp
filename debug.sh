#!/bin/bash
set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
CFG="$ROOT_DIR/Caddyfile.async"
BIN="$ROOT_DIR/caddy/frankenphp/frankenphp"
CACHE_DIR="$ROOT_DIR/.cache/go-build"

ulimit -c unlimited || true

echo "Building frankenphp with debug flags..."
cd "$ROOT_DIR/caddy/frankenphp"
mkdir -p "$CACHE_DIR"
CGO_CFLAGS="$(php-config --includes) -g -O0" \
CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs) -g" \
GOEXPERIMENT=cgocheck2 \
GOCACHE="$CACHE_DIR" \
  go build -tags "trueasync,nowatcher" -gcflags "all=-N -l" -o "$BIN"

cd "$ROOT_DIR"
#echo "Starting frankenphp with GOTRACEBACK=crash using config $CFG"
#GOTRACEBACK=crash "$BIN" run --config "$CFG"
