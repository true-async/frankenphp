# TrueAsync Guide

This branch integrates **FrankenPHP** with PHP's **TrueAsync** scheduler. 
Async workers launch a PHP coroutine per request, 
using `FrankenPHP\HttpServer::onRequest()` as the entrypoint and a Go-side notifier to wake the PHP event loop.

## PHP build requirements

Build PHP with async + ZTS + embed (no `=shared`):

```bash
cd /path/to/php-src
./configure --prefix=/usr/local --enable-async --enable-zts --enable-embed --with-openssl --with-curl
make -j$(nproc) && sudo make install
```

Verification:

```bash
php-config --configure-options | grep -E "(embed|zts|async)"
nm /usr/local/lib/libphp.so | grep " T tsrm_mutex_lock"   # symbols must be embedded
```

## Build FrankenPHP with TrueAsync

### Quick build

```bash
./build.sh
```

`build.sh` sets `CGO_CFLAGS`/`CGO_LDFLAGS` from `php-config` and builds with `trueasync,nowatcher` tags (binary lands in `caddy/frankenphp/frankenphp`).

### Manual build

```bash
export CGO_CFLAGS="$(php-config --includes)"
export CGO_LDFLAGS="$(php-config --ldflags) $(php-config --libs)"
go build -tags "trueasync,nowatcher" -o frankenphp
```

## Running with Caddy

Mark workers as async to activate the **TrueAsync** pipeline (per-thread request buffers, eventfd/pipe notifier, coroutine dispatch). 
Default buffer size: 20 requests per thread; 503 is returned when all buffers are full.

Example `Caddyfile`:

```caddyfile
{
	frankenphp {
		num_threads 2
		worker {
			file examples/async_entrypoint.php
			num 1
			async              # enable TrueAsync worker threads
			buffer_size 20     # per-thread request queue (1..1000)
			match *            # route everything to the async worker
		}
	}
}

:8080 {
	root * examples
	route {
		# keep original path for routing inside the async entrypoint
		rewrite * /async_entrypoint.php?uri={http.request.uri.path}
		php_server {
			index off
			file_server off
		}
	}
}
```

Run:

```bash
./frankenphp run --config Caddyfile.async
curl http://localhost:8080/
```

## PHP entrypoint (TrueAsync API)

The worker script is executed once per thread; when it registers `HttpServer::onRequest()`, 
the thread switches into the **TrueAsync** loop after startup and keeps handling requests via coroutines.

```php
<?php
use FrankenPHP\HttpServer;
use FrankenPHP\Request;
use FrankenPHP\Response;

set_time_limit(0);

HttpServer::onRequest(function (Request $request, Response $response) {
    $uri = $request->getUri();

    if ($uri === '/') {
        $response->setStatus(200);
        $response->setHeader('Content-Type', 'application/json');
        $response->write(json_encode([
            'message' => 'Hello from TrueAsync!',
            'timestamp' => date('Y-m-d H:i:s'),
        ]));
        $response->end();
        return;
    }

    $response->setStatus(404);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode(['error' => 'Not Found', 'uri' => $uri]));
    $response->end();
});
```

API surface:
- `Request::getMethod()`, `getUri()`, `getHeaders()`, `getBody()` (body is fully read once).
- `Response::setStatus()`, `setHeader()`, `write()`, `end()` (call `end()` to flush and release the pending write reference; multiple `write()` calls are buffered).

## Execution model notes

- Each async thread owns a request queue (`buffer_size`, default 20). If all queues are full you get `ErrAllBuffersFull` -> 503.
- Requests wake the PHP scheduler via a notifier (eventfd on Linux, pipe elsewhere) plus a heartbeat fast path to reduce wakeup latency.
- `Response::write()` hands the PHP buffer to Go without copies; the Go side streams it and then calls back to free the pending write. Always call `end()` even for empty bodies.
- Shutdown sends a sentinel through the queue; the PHP loop frees pending writes and restores the heartbeat handler.

## Debugging with Delve

Go 1.25+ emits DWARF v5. If Delve complains, rebuild with DWARF v4:

```bash
GOEXPERIMENT=nodwarf5 go build -tags nowatcher -o testcmd
```

Install/run Delve:

```bash
go install github.com/go-delve/delve/cmd/dlv@latest
export PATH=$PATH:~/go/bin
dlv exec ./testcmd
```

## Common issues

- `undefined reference to tsrm_*`: PHP built with `--enable-embed=shared`; rebuild without `=shared`.
- `multiple definition`: remove `#include "*.c"` from Go files.
- Requests never arrive: ensure the worker has `async` enabled and that your Caddy matcher routes traffic to the entrypoint (e.g., `match *` + rewrite).
- DWARF v5 error in Delve: rebuild with `GOEXPERIMENT=nodwarf5`.
