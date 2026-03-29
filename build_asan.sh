#!/bin/bash
set -e

cd /home/edmond/frankenphp/caddy/frankenphp

# AddressSanitizer flags
export ASAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer -g"

export CGO_CFLAGS="$(php-config --includes) $ASAN_FLAGS"
export CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs) $ASAN_FLAGS"
export CGO_CXXFLAGS="$ASAN_FLAGS"

echo "Building frankenphp with trueasync and ASAN..."
go build -tags "trueasync,nowatcher" -ldflags="-linkmode=external -extldflags=-fsanitize=address"

echo "Build completed: $(file ./frankenphp)"
ls -lh ./frankenphp

echo ""
echo "Run with: ASAN_OPTIONS=detect_leaks=1 ./caddy/frankenphp/frankenphp ..."
