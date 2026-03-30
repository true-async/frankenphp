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

## Request API

All data is fetched from Go's `http.Request` via CGO — no SAPI globals, safe for concurrent coroutines.

| Method | Return | Description |
|--------|--------|-------------|
| `getMethod()` | `string` | HTTP method (`GET`, `POST`, etc.) |
| `getUri()` | `string` | Full request URI with query string |
| `getHeader(string $name)` | `?string` | Single header value, or `null` |
| `getHeaders()` | `array` | All headers as `name => value` (multi-values joined with `, `) |
| `getBody()` | `string` | Full request body (read once) |
| `getQueryParams()` | `array` | Parsed + URL-decoded query string |
| `getCookies()` | `array` | Parsed + URL-decoded cookies from `Cookie` header |
| `getHost()` | `string` | Host header value |
| `getRemoteAddr()` | `string` | Client address (`ip:port`) |
| `getScheme()` | `string` | `http` or `https` |
| `getProtocolVersion()` | `string` | Protocol (`HTTP/1.1`, `HTTP/2.0`) |
| `getParsedBody()` | `array` | Form fields (urlencoded + multipart) |
| `getUploadedFiles()` | `array` | Uploaded files as `UploadedFile` objects |

## Response API

Headers and status are stored per-object (not in SAPI globals), serialized and sent to Go in a single CGO call at `end()`.

| Method | Return | Description |
|--------|--------|-------------|
| `setStatus(int $code)` | `void` | Set HTTP status code (default 200) |
| `getStatus()` | `int` | Read current status code |
| `setHeader(string $name, string $value)` | `void` | Set header (replaces existing) |
| `addHeader(string $name, string $value)` | `void` | Append header (for `Set-Cookie`, etc.) |
| `removeHeader(string $name)` | `void` | Remove a header |
| `getHeader(string $name)` | `?string` | Read first value of a header, or `null` |
| `getHeaders()` | `array` | All headers as `name => [values...]` |
| `isHeadersSent()` | `bool` | Whether `end()` has been called |
| `redirect(string $url, int $code = 302)` | `void` | Set Location header + status |
| `write(string $data)` | `void` | Buffer response body (multiple calls OK) |
| `end()` | `void` | Send status + headers + body to client. **Must be called.** |

### Example: cookies and redirect

```php
HttpServer::onRequest(function (Request $request, Response $response) {
    // Read cookies from request
    $cookies = $request->getCookies();

    if (!isset($cookies['session'])) {
        // Set multiple cookies
        $response->addHeader('Set-Cookie', 'session=abc123; Path=/; HttpOnly');
        $response->addHeader('Set-Cookie', 'theme=dark; Path=/');
        $response->redirect('/welcome');
        $response->end();
        return;
    }

    // Query params
    $params = $request->getQueryParams();
    $name = $params['name'] ?? 'World';

    $response->setStatus(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->write("Hello, {$name}!");
    $response->end();
});
```

## UploadedFile API

`getUploadedFiles()` returns `FrankenPHP\UploadedFile` objects. Go parses multipart via `http.Request.ParseMultipartForm`, saves files to a temp directory, and passes metadata to PHP.

| Method | Return | Description |
|--------|--------|-------------|
| `getName()` | `string` | Original filename |
| `getType()` | `string` | MIME type |
| `getSize()` | `int` | File size in bytes |
| `getTmpName()` | `string` | Temp file path |
| `getError()` | `int` | Upload error code (`UPLOAD_ERR_OK` = 0) |
| `moveTo(string $path)` | `bool` | Move file to destination (rename or copy+delete) |

Multiple files for the same field are returned as an array of `UploadedFile` objects.

### Example: file upload

```php
HttpServer::onRequest(function (Request $request, Response $response) {
    $files = $request->getUploadedFiles();
    $fields = $request->getParsedBody();

    if (isset($files['avatar'])) {
        $file = $files['avatar'];

        if ($file->getError() === UPLOAD_ERR_OK) {
            $file->moveTo('/uploads/' . $file->getName());
            $response->setStatus(200);
            $response->write("Uploaded: {$file->getName()} ({$file->getSize()} bytes)");
        } else {
            $response->setStatus(400);
            $response->write("Upload error: {$file->getError()}");
        }
    } else {
        $response->setStatus(400);
        $response->write('No file uploaded');
    }

    $response->end();
});
```

## Worker restart (green-blue)

Async workers support graceful restart without losing in-flight requests.
When `RestartWorkers()` is triggered (via admin API, file watcher, or config reload):

1. Old threads are **detached** from the worker — no new requests are routed to them.
2. In-flight requests get a grace period (`drain_timeout`, default 30 s) to finish.
3. Old threads are shut down and their resources (notifier, channels) are released.
4. Fresh threads are booted with the updated PHP code.

During the drain window new requests receive `503 (ErrAllBuffersFull)`.
After the new threads reach `Ready`, traffic resumes normally.

### Caddyfile option

```caddyfile
worker {
    file entrypoint.php
    num 2
    async
    drain_timeout 30s   # grace period for in-flight requests (default 30s)
}
```

### Admin API

Trigger a restart at runtime (all workers, sync and async):

```bash
curl -X POST http://localhost:2019/frankenphp/workers/restart
```

### Go API

```go
frankenphp.WithWorkerDrainTimeout(15 * time.Second)
```

## Execution model notes

- Each async thread uses a buffered channel with 1 slot (default). Set `buffer_size` to increase the per-thread request queue (max 10). If all threads are busy and all buffers are full, the client gets `503 (ErrAllBuffersFull)`.
- Requests wake the PHP scheduler via a notifier (eventfd on Linux, pipe elsewhere) plus a heartbeat fast path to reduce wakeup latency.
- `Response::write()` buffers data in the PHP object. `end()` serializes headers + body and copies them to Go in one CGO call. Always call `end()` even for empty bodies.
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
