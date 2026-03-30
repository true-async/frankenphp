<?php

use FrankenPHP\HttpServer;
use FrankenPHP\Request;
use FrankenPHP\Response;

echo "[STARTUP] Registering TrueAsync handler...\n";

HttpServer::onRequest(function (Request $request, Response $response) {
    $uri = $request->getUri();
    echo "[REQUEST] Got request: " . $request->getMethod() . " " . $uri . "\n";

    if ($uri === '/cookies') {
        $response->setStatus(200);
        $response->setHeader('Content-Type', 'text/plain');
        $response->addHeader('Set-Cookie', 'session=abc123; Path=/; HttpOnly');
        $response->addHeader('Set-Cookie', 'theme=dark; Path=/');
        $response->addHeader('X-Custom', 'value1');
        $response->addHeader('X-Custom', 'value2');
        $response->write("Check response headers for cookies!\n");
        $response->end();
    } elseif ($uri === '/headers') {
        $response->setStatus(200);
        $response->setHeader('Content-Type', 'application/json');

        $data = [
            'all_headers' => $request->getHeaders(),
            'user_agent' => $request->getHeader('User-Agent'),
            'x_custom' => $request->getHeader('X-Custom-Test'),
            'missing' => $request->getHeader('X-Does-Not-Exist'),
        ];

        $response->write(json_encode($data, JSON_PRETTY_PRINT) . "\n");
        $response->end();
    } else {
        $response->setStatus(200);
        $response->setHeader('Content-Type', 'text/plain');
        $response->write("Hello from TrueAsync!\n");
        $response->end();
    }

    echo "[REQUEST] Request completed\n";
});

echo "[STARTUP] Handler registered. Event loop starting...\n";
