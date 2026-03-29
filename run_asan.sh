#!/bin/bash

# AddressSanitizer options
export ASAN_OPTIONS="detect_leaks=1:log_path=/tmp/asan.log:halt_on_error=0:abort_on_error=0"

# Suppress some known false positives from Go runtime
export LSAN_OPTIONS="suppressions=/home/edmond/frankenphp/asan_suppressions.txt:print_suppressions=0"

echo "Starting FrankenPHP with AddressSanitizer..."
echo "ASAN logs will be written to: /tmp/asan.log.*"
echo ""

# Clear old logs
rm -f /tmp/asan.log.*

# Run FrankenPHP
./caddy/frankenphp/frankenphp run --config Caddyfile.async

echo ""
echo "Check ASAN reports with:"
echo "  cat /tmp/asan.log.*"
