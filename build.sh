#!/bin/bash
set -e

cd /home/edmond/frankenphp/caddy/frankenphp

export CGO_CFLAGS="$(php-config --includes)"
export CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs) -L/usr/lib/gcc/x86_64-linux-gnu/13 -lbacktrace"

echo "Building frankenphp with trueasync..."
go build -tags "trueasync,nowatcher" -gcflags="all=-N -l"

echo "Build completed: $(file ./frankenphp)"
ls -lh ./frankenphp
