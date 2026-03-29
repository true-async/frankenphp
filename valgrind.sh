#!/usr/bin/env bash
set -euo pipefail

BIN="./caddy/frankenphp/frankenphp"
CFG="/home/edmond/frankenphp/Caddyfile.async"
LOG_FILE="${LOG_FILE:-valgrind.log}"

echo "Running FrankenPHP under valgrind..."
echo "Config: $CFG"
echo "Log: $LOG_FILE"
echo

# shellcheck disable=SC2086
exec valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
  --num-callers=20 --error-limit=no \
  --suppressions=./valgrind_go_suppressions.supp \
  "$BIN" run --config "$CFG" \
  2>&1 | tee "$LOG_FILE"
