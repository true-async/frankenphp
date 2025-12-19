#!/bin/bash
set -e

export CGO_CFLAGS="$(php-config --includes)"
export CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs)"

echo "Building FrankenPHP with:"
echo "CGO_CFLAGS=$CGO_CFLAGS"
echo "CGO_LDFLAGS=$CGO_LDFLAGS"

go build -tags "trueasync,nowatcher" -o frankenphp
