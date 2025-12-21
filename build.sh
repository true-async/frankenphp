#!/bin/bash
set -e

cd /home/edmond/frankenphp/caddy/frankenphp

export CGO_CFLAGS="$(php-config --includes)"
export CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs)"

echo "Building frankenphp with trueasync..."
go build -tags "trueasync,nowatcher"

echo "Build completed: $(file ./frankenphp)"
ls -lh ./frankenphp
