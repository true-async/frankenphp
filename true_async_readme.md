# TrueAsync Build Guide

## PHP Requirements

Build PHP with these flags:

```bash
cd /path/to/php-src
./configure --prefix=/usr/local --enable-async --enable-zts --enable-embed --with-openssl --with-curl
make -j$(nproc) && sudo make install
```

**Critical:** Use `--enable-embed` WITHOUT `=shared` or `=static`

## Verify PHP Build

```bash
# Check flags
php-config --configure-options | grep -E "(embed|zts|async)"

# Verify symbols are embedded (should show "T", not "U")
nm /usr/local/lib/libphp.so | grep " T tsrm_mutex_lock"
```

## Build FrankenPHP

### Main HTTP Server

```bash
cd /home/edmond/frankenphp
export CGO_CFLAGS="$(php-config --includes)"
export CGO_LDFLAGS="-L/usr/local/lib -lphp"

cd caddy/frankenphp
go build -tags nowatcher -o ../../frankenphp
```

### Test Program (TrueAsync)

```bash
cd /home/edmond/frankenphp/testcmd
go build -tags nowatcher -o testcmd
./testcmd
```

## TrueAsync Usage

### Go API

```go
frankenphp.Init(
    frankenphp.WithAsyncMode("/path/to/entrypoint.php", 1)
)
```

### Entrypoint PHP

```php
<?php
use FrankenPHP\HttpServer;
use FrankenPHP\Request;
use FrankenPHP\Response;

HttpServer::onRequest(function (Request $req, Response $res) {
    $res->setStatus(200);
    $res->write("Hello from TrueAsync!");
    $res->end();
});
```

## Debugging with Delve

### Problem: DWARF v5 Compatibility

Go 1.25+ generates debug info using DWARF v5, but Delve needs to be built with Go 1.25.0+ to support it. If you see:

```
To debug executables using DWARFv5 or later Delve must be built with Go version 1.25.0 or later
```

### Solution: Build with DWARF v4

```bash
# In testcmd directory
GOEXPERIMENT=nodwarf5 bash build.sh

# Or for one-off builds
GOEXPERIMENT=nodwarf5 go build -tags nowatcher -o testcmd
```

### Install Delve

```bash
# Install delve
go install github.com/go-delve/delve/cmd/dlv@latest

# Add to PATH (add to ~/.bashrc for persistence)
export PATH=$PATH:~/go/bin
```

### Using Delve

```bash
cd /home/edmond/frankenphp/testcmd

# Interactive debugging
dlv exec ./testcmd

# Common commands:
# break main.main    - set breakpoint
# continue (c)       - run program
# next (n)           - next line
# step (s)           - step into function
# print var (p var)  - print variable
# locals             - show local variables
# quit (q)           - exit
```

## Common Issues

**undefined reference to tsrm_*** - PHP built with `=shared`, rebuild without it

**multiple definition** - Remove `#include "*.c"` from .go files

**xdebug warning** - Comment out xdebug in `/usr/local/lib/php.ini`

**DWARF v5 error in Delve** - Build with `GOEXPERIMENT=nodwarf5`
