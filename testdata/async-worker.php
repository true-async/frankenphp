<?php
/**
 * Async worker test script for CI.
 * Exercises Request/Response API and returns JSON results.
 */

use FrankenPHP\HttpServer;
use FrankenPHP\Request;
use FrankenPHP\Response;

HttpServer::onRequest(function (Request $request, Response $response) {
    $uri = $request->getUri();
    $path = parse_url($uri, PHP_URL_PATH);

    match ($path) {
        '/test/basic' => handleBasic($request, $response),
        '/test/status' => handleStatus($request, $response),
        '/test/headers' => handleHeaders($request, $response),
        '/test/cookies' => handleCookies($request, $response),
        '/test/request-headers' => handleRequestHeaders($request, $response),
        '/test/request-method' => handleRequestMethod($request, $response),
        '/test/request-body' => handleRequestBody($request, $response),
        '/test/query-params' => handleQueryParams($request, $response),
        '/test/request-cookies' => handleRequestCookies($request, $response),
        '/test/request-info' => handleRequestInfo($request, $response),
        '/test/response-getters' => handleResponseGetters($request, $response),
        '/test/redirect' => handleRedirect($request, $response),
        '/test/parsed-body' => handleParsedBody($request, $response),
        '/test/upload' => handleUpload($request, $response),
        '/test/echo' => handleEcho($request, $response),
        default => handleNotFound($request, $response),
    };
});

function handleBasic(Request $request, Response $response): void {
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->write('ok');
    $response->end();
}

function handleStatus(Request $request, Response $response): void {
    $response->setStatus(201);
    $response->setHeader('Content-Type', 'text/plain');
    $response->write('created');
    $response->end();
}

function handleHeaders(Request $request, Response $response): void {
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->setHeader('X-Custom-One', 'value1');
    $response->setHeader('X-Custom-Two', 'value2');
    // setHeader replaces — verify last value wins
    $response->setHeader('X-Replace-Test', 'first');
    $response->setHeader('X-Replace-Test', 'second');
    $response->write('headers-ok');
    $response->end();
}

function handleCookies(Request $request, Response $response): void {
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->addHeader('Set-Cookie', 'session=abc123; Path=/; HttpOnly');
    $response->addHeader('Set-Cookie', 'theme=dark; Path=/');
    // addHeader appends — both X-Multi values must be present
    $response->addHeader('X-Multi', 'a');
    $response->addHeader('X-Multi', 'b');
    $response->write('cookies-ok');
    $response->end();
}

function handleRequestHeaders(Request $request, Response $response): void {
    $allHeaders = $request->getHeaders();
    $custom = $request->getHeader('X-Test-Header');
    $missing = $request->getHeader('X-Does-Not-Exist');

    $result = [
        'all_headers' => $allHeaders,
        'custom' => $custom,
        'missing' => $missing,
    ];

    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($result));
    $response->end();
}

function handleRequestMethod(Request $request, Response $response): void {
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->write($request->getMethod());
    $response->end();
}

function handleRequestBody(Request $request, Response $response): void {
    $body = $request->getBody();
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'text/plain');
    $response->write($body);
    $response->end();
}

function handleEcho(Request $request, Response $response): void {
    $result = [
        'method' => $request->getMethod(),
        'uri' => $request->getUri(),
        'headers' => $request->getHeaders(),
        'body' => $request->getBody(),
    ];

    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($result));
    $response->end();
}

function handleQueryParams(Request $request, Response $response): void {
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($request->getQueryParams()));
    $response->end();
}

function handleRequestCookies(Request $request, Response $response): void {
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($request->getCookies()));
    $response->end();
}

function handleRequestInfo(Request $request, Response $response): void {
    $result = [
        'host' => $request->getHost(),
        'remote_addr' => $request->getRemoteAddr(),
        'protocol' => $request->getProtocolVersion(),
        'scheme' => $request->getScheme(),
    ];
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($result));
    $response->end();
}

function handleResponseGetters(Request $request, Response $response): void {
    $response->setStatus(201);
    $response->setHeader('X-Test', 'hello');
    $response->addHeader('X-Multi', 'a');
    $response->addHeader('X-Multi', 'b');

    $result = [
        'status' => $response->getStatus(),
        'header' => $response->getHeader('X-Test'),
        'missing_header' => $response->getHeader('X-Nope'),
        'headers_sent' => $response->isHeadersSent(),
        'headers' => $response->getHeaders(),
    ];

    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($result));
    $response->end();
}

function handleRedirect(Request $request, Response $response): void {
    $response->redirect('https://example.com/target', 301);
    $response->end();
}

function handleParsedBody(Request $request, Response $response): void {
    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($request->getParsedBody()));
    $response->end();
}

function handleUpload(Request $request, Response $response): void {
    $files = $request->getUploadedFiles();
    $result = [];

    foreach ($files as $field => $file) {
        if (is_array($file)) {
            $result[$field] = [];
            foreach ($file as $f) {
                $result[$field][] = [
                    'name' => $f->getName(),
                    'type' => $f->getType(),
                    'size' => $f->getSize(),
                    'error' => $f->getError(),
                    'has_tmp' => !empty($f->getTmpName()),
                    'content' => file_get_contents($f->getTmpName()),
                ];
            }
        } else {
            $result[$field] = [
                'name' => $file->getName(),
                'type' => $file->getType(),
                'size' => $file->getSize(),
                'error' => $file->getError(),
                'has_tmp' => !empty($file->getTmpName()),
                'content' => file_get_contents($file->getTmpName()),
            ];
        }
    }

    $response->setStatus(200);
    $response->setHeader('Content-Type', 'application/json');
    $response->write(json_encode($result));
    $response->end();
}

function handleNotFound(Request $request, Response $response): void {
    $response->setStatus(404);
    $response->setHeader('Content-Type', 'text/plain');
    $response->write('not found');
    $response->end();
}
