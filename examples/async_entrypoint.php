<?php

use FrankenPHP\HttpServer;
use FrankenPHP\Request;
use FrankenPHP\Response;

set_time_limit(0);

echo "Starting FrankenPHP TrueAsync HttpServer...\n";

// Register request handler
HttpServer::onRequest(function (Request $request, Response $response) {

    $method = $request->getMethod();
    $uri = $request->getUri();
    $headers = $request->getHeaders();
    $body = $request->getBody();

    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');

    $responseData = [
        'message' => 'Hello from FrankenPHP TrueAsync!',
        'method' => $method,
        'uri' => $uri,
        'total_coroutines' => count(\Async\get_coroutines()),
        'memory' => round(memory_get_usage(true) / 1024 / 1024, 2),
        'timestamp' => date('Y-m-d H:i:s'),
        'headers_count' => count($headers),
        'body_length' => strlen($body),
    ];

    $response->write(json_encode($responseData, JSON_PRETTY_PRINT));
    $response->end();
});

echo "Request handler registered. Event loop is running.\n";
// Script stays loaded, event loop handles requests
