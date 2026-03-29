#!/bin/bash

echo "Starting FrankenPHP with debug logging..."
echo "Logs will show [LEAK DEBUG] messages"
echo ""

./caddy/frankenphp/frankenphp run --config Caddyfile.async 2>&1 | grep --line-buffered -E "\[LEAK DEBUG\]|error|Starting FrankenPHP"
