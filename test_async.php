<?php

use FrankenPHP\HttpServer;
use FrankenPHP\Request;
use FrankenPHP\Response;

echo "[STARTUP] Registering TrueAsync handler...\n";

HttpServer::onRequest(function (Request $request, Response $response) {
    echo "[REQUEST] Got request: " . $request->getMethod() . " " . $request->getUri() . "\n";

    $response->setStatus(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->write("Hello from TrueAsync!\n");
    $response->end();

    echo "[REQUEST] Request completed\n";
});

echo "[STARTUP] Handler registered. Event loop starting...\n";
