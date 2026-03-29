## FrankenPHP integration

TrueAsync now ships inside FrankenPHP: async workers spin up a PHP coroutine per request via `FrankenPHP\HttpServer::onRequest()`, waking the scheduler through a Go-side notifier. Build PHP with `--enable-async --enable-zts --enable-embed`, compile FrankenPHP with the `trueasync` tag (details in `TRUE_ASYNC.README.md`), and mark workers as `async` in your `Caddyfile` to route traffic into the TrueAsync event loop.
