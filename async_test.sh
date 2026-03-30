#!/bin/bash
# Async worker integration tests
# Usage: ./async_test.sh [base_url]
set -euo pipefail

BASE="${1:-http://localhost:8085}"
PASS=0
FAIL=0

assert_status() {
    local name="$1" url="$2" expected="$3" method="${4:-GET}" body="${5:-}"
    local args=(-s -o /dev/null -w '%{http_code}' -X "$method")
    if [ -n "$body" ]; then args+=(-d "$body"); fi
    local got
    got=$(curl "${args[@]}" "$url")
    if [ "$got" = "$expected" ]; then
        echo "  PASS: $name (status=$got)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected=$expected got=$got)"
        FAIL=$((FAIL + 1))
    fi
}

assert_body() {
    local name="$1" url="$2" expected="$3" method="${4:-GET}" body="${5:-}"
    local args=(-s -X "$method")
    if [ -n "$body" ]; then args+=(-d "$body"); fi
    local got
    got=$(curl "${args[@]}" "$url") || got=""
    if [ "$got" = "$expected" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    expected: $expected"
        echo "    got:      $got"
        FAIL=$((FAIL + 1))
    fi
}

assert_header() {
    local name="$1" url="$2" header="$3" expected="$4"
    local args=(-sD - -o /dev/null)
    local headers
    headers=$(curl "${args[@]}" "$url")
    if echo "$headers" | grep -qi "^${header}: ${expected}"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (header '$header: $expected' not found)"
        echo "    headers: $(echo "$headers" | tr '\r' ' ')"
        FAIL=$((FAIL + 1))
    fi
}

assert_header_count() {
    local name="$1" url="$2" header="$3" expected_count="$4"
    local headers count
    headers=$(curl -sD - -o /dev/null "$url")
    count=$(echo "$headers" | grep -ci "^${header}:" || true)
    if [ "$count" = "$expected_count" ]; then
        echo "  PASS: $name ($count occurrences)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected $expected_count occurrences, got $count)"
        FAIL=$((FAIL + 1))
    fi
}

assert_json_field() {
    local name="$1" url="$2" field="$3" expected="$4"
    local extra_args="${5:-}"
    local args=(-sf)
    if [ -n "$extra_args" ]; then eval "args+=($extra_args)"; fi
    local got
    got=$(curl "${args[@]}" "$url" | php -r "echo json_decode(file_get_contents('php://stdin'), true)$field ?? 'NULL';")
    if [ "$got" = "$expected" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name (expected=$expected got=$got)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Async Worker Integration Tests ==="
echo ""

echo "[Response: basic]"
assert_status "200 OK" "$BASE/test/basic" "200"
assert_body "body=ok" "$BASE/test/basic" "ok"

echo "[Response: status codes]"
assert_status "201 Created" "$BASE/test/status" "201"
assert_body "body=created" "$BASE/test/status" "created"

echo "[Response: 404]"
assert_status "404 Not Found" "$BASE/test/unknown-path" "404"
assert_body "body=not found" "$BASE/test/unknown-path" "not found"

echo "[Response: setHeader]"
assert_header "Content-Type" "$BASE/test/headers" "Content-Type" "text/plain"
assert_header "X-Custom-One" "$BASE/test/headers" "X-Custom-One" "value1"
assert_header "X-Custom-Two" "$BASE/test/headers" "X-Custom-Two" "value2"
assert_header "setHeader replaces" "$BASE/test/headers" "X-Replace-Test" "second"

echo "[Response: addHeader / cookies]"
assert_header_count "two Set-Cookie headers" "$BASE/test/cookies" "Set-Cookie" "2"
assert_header_count "two X-Multi headers" "$BASE/test/cookies" "X-Multi" "2"

echo "[Request: getMethod]"
assert_body "GET method" "$BASE/test/request-method" "GET"
assert_body "POST method" "$BASE/test/request-method" "POST" "POST"
assert_body "PUT method" "$BASE/test/request-method" "PUT" "PUT"
assert_body "DELETE method" "$BASE/test/request-method" "DELETE" "DELETE"

echo "[Request: getHeader / getHeaders]"
assert_json_field "getHeader returns value" "$BASE/test/request-headers" "['custom']" "test-value-123" "-H 'X-Test-Header: test-value-123'"
assert_json_field "getHeader returns NULL for missing" "$BASE/test/request-headers" "['missing']" "NULL"

echo "[Request: getBody]"
assert_body "POST body echo" "$BASE/test/request-body" "hello world" "POST" "hello world"

echo "[Request: getQueryParams]"
assert_json_field "query param foo" "$BASE/test/query-params?foo=bar&baz=123" "['foo']" "bar"
assert_json_field "query param baz" "$BASE/test/query-params?foo=bar&baz=123" "['baz']" "123"
assert_json_field "query url-decode value" "$BASE/test/query-params?msg=hello+world&x=a%26b" "['msg']" "hello world"
assert_json_field "query url-decode special" "$BASE/test/query-params?msg=hello+world&x=a%26b" "['x']" "a&b"

echo "[Request: getCookies]"
assert_json_field "cookie session" "$BASE/test/request-cookies" "['session']" "abc" "-H 'Cookie: session=abc; theme=dark'"
assert_json_field "cookie theme" "$BASE/test/request-cookies" "['theme']" "dark" "-H 'Cookie: session=abc; theme=dark'"
assert_json_field "cookie url-decode" "$BASE/test/request-cookies" "['name']" "hello world" "-H 'Cookie: name=hello%20world'"

echo "[Request: getHost/getRemoteAddr/getScheme/getProtocolVersion]"
assert_json_field "host" "$BASE/test/request-info" "['host']" "localhost:8085"
assert_json_field "scheme is http" "$BASE/test/request-info" "['scheme']" "http"
assert_json_field "protocol" "$BASE/test/request-info" "['protocol']" "HTTP/1.1"

echo "[Response: getters]"
assert_json_field "getStatus returns 201" "$BASE/test/response-getters" "['status']" "201"
assert_json_field "getHeader returns value" "$BASE/test/response-getters" "['header']" "hello"
assert_json_field "getHeader returns NULL for missing" "$BASE/test/response-getters" "['missing_header']" "NULL"
assert_json_field "isHeadersSent false before end" "$BASE/test/response-getters" "['headers_sent']" ""

echo "[Response: redirect]"
assert_status "301 redirect" "$BASE/test/redirect" "301"
assert_header "Location header" "$BASE/test/redirect" "Location" "https://example.com/target"

echo "[Request: getParsedBody]"
assert_json_field "form field name" "$BASE/test/parsed-body" "['name']" "John" "-X POST -d 'name=John&age=30'"
assert_json_field "form field age" "$BASE/test/parsed-body" "['age']" "30" "-X POST -d 'name=John&age=30'"

echo "[Request: getUploadedFiles]"
UPLOAD_TMP=$(mktemp)
echo -n "hello file content" > "$UPLOAD_TMP"
UPLOAD_JSON=$(curl -s -F "file=@${UPLOAD_TMP};filename=test.txt" "$BASE/test/upload")
rm -f "$UPLOAD_TMP"
# Validate upload response fields
for check in "['file']['name']|test.txt" "['file']['content']|hello file content" "['file']['error']|0" "['file']['size']|18"; do
    field="${check%%|*}"
    expected="${check#*|}"
    got=$(echo "$UPLOAD_JSON" | php -r "echo json_decode(file_get_contents('php://stdin'), true)$field ?? 'NULL';")
    if [ "$got" = "$expected" ]; then
        echo "  PASS: upload $field"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: upload $field (expected=$expected got=$got)"
        FAIL=$((FAIL + 1))
    fi
done

echo "[Request: echo]"
assert_json_field "echo method" "$BASE/test/echo" "['method']" "POST" "-X POST -d test"
assert_json_field "echo body" "$BASE/test/echo" "['body']" "test" "-X POST -d test"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ] || exit 1
