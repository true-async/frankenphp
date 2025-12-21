<?php

use FrankenPHP\HttpServer;
use FrankenPHP\Request;
use FrankenPHP\Response;

set_time_limit(0);

echo "Starting FrankenPHP TrueAsync HttpServer...\n";

// Register request handler
HttpServer::onRequest(function (Request $request, Response $response) {
    $method = $request->getMethod();
    // Восстанавливаем исходный путь, проброшенный через rewrite
    $uri = $_GET['uri'] ?? $request->getUri();
    $headers = $request->getHeaders();
    $body = $request->getBody();

    // Simple routing
    if ($uri === '/') {
        $response->setStatus(200);
        $response->setHeader('Content-Type', 'application/json');

        $responseData = [
            'message' => 'Hello from FrankenPHP TrueAsync!',
            'method' => $method,
            'uri' => $uri,
            'timestamp' => date('Y-m-d H:i:s'),
            'headers_count' => count($headers),
            'body_length' => strlen($body),
        ];

        $response->write(json_encode($responseData, JSON_PRETTY_PRINT));
        $response->end();
    } elseif ($uri === '/echo') {
        $response->setStatus(200);
        $response->setHeader('Content-Type', 'text/plain');

        $echo = "Method: $method\n";
        $echo .= "URI: $uri\n";
        $echo .= "Headers:\n";
        foreach ($headers as $name => $value) {
            $echo .= "  $name: $value\n";
        }
        if (!empty($body)) {
            $echo .= "\nBody:\n$body\n";
        }

        $response->write($echo);
        $response->end();
    } elseif ($uri === '/health') {
        $response->setStatus(200);
        $response->setHeader('Content-Type', 'text/plain');
        $response->write("OK\n");
        $response->end();
    } else {
        $response->setStatus(404);
        $response->setHeader('Content-Type', 'application/json');
        $response->write(json_encode(['error' => 'Not Found', 'uri' => $uri]));
        $response->end();
    }
});

echo "Request handler registered. Event loop is running.\n";
// Script stays loaded, event loop handles requests
