/*
 * FrankenPHP TrueAsync Extension
 *
 * Provides PHP classes for async request handling:
 * - FrankenPHP\HttpServer - register request handler callback
 * - FrankenPHP\Request - HTTP request object
 * - FrankenPHP\Response - HTTP response object
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "zend_smart_str.h"
#include "ext/standard/url.h"
#include "ext/json/php_json.h"
#include "SAPI.h"
#include "frankenphp.h"
#include <pthread.h>

/* PHP 8.6 removed XtOffsetOf from the public headers; fall back to the
 * standard offsetof so the extension keeps compiling across versions. */
#ifndef XtOffsetOf
#define XtOffsetOf(s_type, field) offsetof(s_type, field)
#endif

/* Forward declarations for CGO functions from Go */
extern char *go_async_get_request_method(uintptr_t thread_index, uint64_t request_id);
extern char *go_async_get_request_uri(uintptr_t thread_index, uint64_t request_id);
extern char *go_async_get_request_header(uintptr_t thread_index, uint64_t request_id, const char *header_name);
extern char *go_async_get_request_body(uintptr_t thread_index, uint64_t request_id, size_t *length);
extern char *go_async_get_all_request_headers(uintptr_t thread_index, uint64_t request_id, size_t *length);
extern char *go_async_get_request_host(uintptr_t thread_index, uint64_t request_id);
extern char *go_async_get_request_remote_addr(uintptr_t thread_index, uint64_t request_id);
extern char *go_async_get_request_proto(uintptr_t thread_index, uint64_t request_id);
extern bool go_async_get_request_is_tls(uintptr_t thread_index, uint64_t request_id);
extern char *go_async_get_parsed_body(uintptr_t thread_index, uint64_t request_id, size_t *length);
extern char *go_async_get_uploaded_files(uintptr_t thread_index, uint64_t request_id, size_t *length);
extern void go_async_notify_request_done(uintptr_t thread_index, uint64_t request_id);
extern void go_async_response_write(uintptr_t thread_index, uint64_t request_id, void *data, size_t length);
extern void go_async_response_complete(uintptr_t thread_index, uint64_t request_id, int status_code, void *headers_data, size_t headers_len, void *body_data, size_t body_len);

/* TLS variables from frankenphp.c */
extern __thread uintptr_t thread_index;
extern __thread bool is_async_mode_requested;
extern __thread zval *async_request_callback;

/* ============================================================================
 * Pending Writes Management - REMOVED
 * Now we copy data to Go memory, so no need for pending writes tracking
 * ============================================================================ */

/* Class entry pointers */
static zend_class_entry *frankenphp_httpserver_ce;
static zend_class_entry *frankenphp_request_ce;
static zend_class_entry *frankenphp_response_ce;
static zend_class_entry *frankenphp_uploadedfile_ce;

/* Object handlers */
static zend_object_handlers frankenphp_request_object_handlers;
static zend_object_handlers frankenphp_response_object_handlers;

/* ============================================================================
 * Request Object
 * ============================================================================ */

typedef struct {
    uint64_t request_id;  /* Links to Go's asyncRequestMap */
    zend_object std;
} frankenphp_request_object;

static inline frankenphp_request_object *frankenphp_request_from_obj(zend_object *obj) {
    return (frankenphp_request_object *)((char *)(obj) - XtOffsetOf(frankenphp_request_object, std));
}

static zend_object *frankenphp_request_create_object(zend_class_entry *ce) {
    frankenphp_request_object *intern = zend_object_alloc(sizeof(frankenphp_request_object), ce);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->std.handlers = &frankenphp_request_object_handlers;
    intern->request_id = 0;

    return &intern->std;
}

static void frankenphp_request_free_object(zend_object *object) {
    frankenphp_request_object *intern = frankenphp_request_from_obj(object);

    zend_object_std_dtor(&intern->std);
}

/* ============================================================================
 * Response Object
 * ============================================================================ */

typedef struct {
    uint64_t request_id;
    uint8_t headers_sent;
    int status_code;
    HashTable headers;      /* name => zval(array of zend_string values) */
    zend_string *buffer;
    zend_object std;
} frankenphp_response_object;

static inline frankenphp_response_object *frankenphp_response_from_obj(zend_object *obj) {
    return (frankenphp_response_object *)((char *)(obj) - XtOffsetOf(frankenphp_response_object, std));
}

static zend_object *frankenphp_response_create_object(zend_class_entry *ce) {
    frankenphp_response_object *intern = zend_object_alloc(sizeof(frankenphp_response_object), ce);

    zend_object_std_init(&intern->std, ce);
    object_properties_init(&intern->std, ce);

    intern->std.handlers = &frankenphp_response_object_handlers;
    intern->request_id = 0;
    intern->headers_sent = 0;
    intern->status_code = 200;
    zend_hash_init(&intern->headers, 8, NULL, ZVAL_PTR_DTOR, 0);
    intern->buffer = NULL;

    return &intern->std;
}

static void frankenphp_response_free_object(zend_object *object) {
    frankenphp_response_object *intern = frankenphp_response_from_obj(object);

    zend_hash_destroy(&intern->headers);

    if (intern->buffer) {
        zend_string_release(intern->buffer);
        intern->buffer = NULL;
    }

    zend_object_std_dtor(&intern->std);
}

/* ============================================================================
 * HttpServer Class Methods
 * ============================================================================ */

/* HttpServer::onRequest(callable $callback): bool */
PHP_METHOD(FrankenPHP_HttpServer, onRequest)
{
    zval *callback;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_ZVAL(callback)
    ZEND_PARSE_PARAMETERS_END();

    /* Validate callback is callable */
    if (!zend_is_callable(callback, 0, NULL)) {
        zend_throw_error(NULL, "Argument must be a valid callback");
        RETURN_FALSE;
    }

    /* Free previous callback if exists */
    if (async_request_callback != NULL) {
        zval_ptr_dtor(async_request_callback);
        efree(async_request_callback);
        async_request_callback = NULL;
    }

    /* Store new callback in TLS.
     * Use safe_emalloc() instead of emalloc(sizeof(zval)) to bypass the
     * zend_alloc.h __builtin_constant_p specialization path. On Windows,
     * PHP is built with MSVC (no __builtin_constant_p) so _emalloc_16 etc.
     * are NOT exported from php8ts.lib, but when FrankenPHP is compiled with
     * Clang the macro routes the constant 16-byte allocation through
     * _emalloc_16 and the link fails. _safe_emalloc is a real exported symbol. */
    async_request_callback = safe_emalloc(1, sizeof(zval), 0);
    ZVAL_COPY(async_request_callback, callback);

    /* Mark this thread as async mode requested */
    is_async_mode_requested = true;

    RETURN_TRUE;
}

/* ============================================================================
 * Request Class Methods
 * ============================================================================ */

/* Request::getMethod(): string */
PHP_METHOD(FrankenPHP_Request, getMethod)
{
    frankenphp_request_object *intern;
    char *method;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    /* Get method from Go via CGO */
    method = go_async_get_request_method(thread_index, intern->request_id);
    if (method == NULL) {
        RETURN_STRING("GET");
    }

    RETVAL_STRING(method);
    free(method);
}

/* Request::getUri(): string */
PHP_METHOD(FrankenPHP_Request, getUri)
{
    frankenphp_request_object *intern;
    char *uri;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    /* Get URI from Go via CGO */
    uri = go_async_get_request_uri(thread_index, intern->request_id);
    if (uri == NULL) {
        RETURN_STRING("/");
    }

    RETVAL_STRING(uri);
    free(uri);
}

/* Request::getHeader(string $name): ?string */
PHP_METHOD(FrankenPHP_Request, getHeader)
{
    frankenphp_request_object *intern;
    zend_string *name;
    char *value;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    value = go_async_get_request_header(thread_index, intern->request_id, ZSTR_VAL(name));
    if (value == NULL) {
        RETURN_NULL();
    }

    RETVAL_STRING(value);
    free(value);
}

/* Request::getHeaders(): array */
PHP_METHOD(FrankenPHP_Request, getHeaders)
{
    frankenphp_request_object *intern;
    char *data;
    size_t length = 0;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    array_init(return_value);

    data = go_async_get_all_request_headers(thread_index, intern->request_id, &length);
    if (data == NULL || length == 0) {
        return;
    }

    /* Parse "name\0value\0" pairs into associative array */
    size_t i = 0;
    while (i < length) {
        const char *name = data + i;
        size_t name_len = strlen(name);
        i += name_len + 1;
        if (i >= length) break;

        const char *value = data + i;
        size_t value_len = strlen(value);
        i += value_len + 1;

        /* If key already exists, append with ", " (standard HTTP multi-value) */
        zval *existing = zend_hash_str_find(Z_ARRVAL_P(return_value), name, name_len);
        if (existing) {
            size_t old_len = Z_STRLEN_P(existing);
            size_t new_len = old_len + 2 + value_len;
            zend_string *merged = zend_string_alloc(new_len, 0);
            memcpy(ZSTR_VAL(merged), Z_STRVAL_P(existing), old_len);
            memcpy(ZSTR_VAL(merged) + old_len, ", ", 2);
            memcpy(ZSTR_VAL(merged) + old_len + 2, value, value_len);
            ZSTR_VAL(merged)[new_len] = '\0';
            zval_ptr_dtor(existing);
            ZVAL_STR(existing, merged);
        } else {
            add_assoc_stringl(return_value, name, value, value_len);
        }
    }

    free(data);
}

/* Request::getHost(): string */
PHP_METHOD(FrankenPHP_Request, getHost)
{
    frankenphp_request_object *intern;
    char *host;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    host = go_async_get_request_host(thread_index, intern->request_id);
    if (host == NULL) {
        RETURN_STRING("");
    }
    RETVAL_STRING(host);
    free(host);
}

/* Request::getRemoteAddr(): string */
PHP_METHOD(FrankenPHP_Request, getRemoteAddr)
{
    frankenphp_request_object *intern;
    char *addr;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    addr = go_async_get_request_remote_addr(thread_index, intern->request_id);
    if (addr == NULL) {
        RETURN_STRING("");
    }
    RETVAL_STRING(addr);
    free(addr);
}

/* Request::getProtocolVersion(): string */
PHP_METHOD(FrankenPHP_Request, getProtocolVersion)
{
    frankenphp_request_object *intern;
    char *proto;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    proto = go_async_get_request_proto(thread_index, intern->request_id);
    if (proto == NULL) {
        RETURN_STRING("HTTP/1.1");
    }
    RETVAL_STRING(proto);
    free(proto);
}

/* Request::getScheme(): string */
PHP_METHOD(FrankenPHP_Request, getScheme)
{
    frankenphp_request_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    bool is_tls = go_async_get_request_is_tls(thread_index, intern->request_id);
    RETURN_STRING(is_tls ? "https" : "http");
}

/* Helper: URL-decode a string in-place, returns new length */
static size_t frankenphp_url_decode(char *str, size_t len)
{
    return php_url_decode(str, len);
}

/* Request::getQueryParams(): array */
PHP_METHOD(FrankenPHP_Request, getQueryParams)
{
    frankenphp_request_object *intern;
    char *uri;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    array_init(return_value);

    uri = go_async_get_request_uri(thread_index, intern->request_id);
    if (uri == NULL) {
        return;
    }

    /* Find query string after '?' */
    char *query = strchr(uri, '?');
    if (query == NULL || *(query + 1) == '\0') {
        free(uri);
        return;
    }
    query++; /* skip '?' */

    /* Parse query string: key=value&key2=value2 */
    char *pair, *saveptr;
    char *query_copy = estrdup(query);
    free(uri);

    pair = php_strtok_r(query_copy, "&", &saveptr);
    while (pair != NULL) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            char *key = pair;
            char *val = eq + 1;
            frankenphp_url_decode(key, strlen(key));
            size_t val_len = frankenphp_url_decode(val, strlen(val));
            add_assoc_stringl(return_value, key, val, val_len);
        } else {
            frankenphp_url_decode(pair, strlen(pair));
            add_assoc_string(return_value, pair, "");
        }
        pair = php_strtok_r(NULL, "&", &saveptr);
    }

    efree(query_copy);
}

/* Request::getCookies(): array */
PHP_METHOD(FrankenPHP_Request, getCookies)
{
    frankenphp_request_object *intern;
    char *cookie_header;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    array_init(return_value);

    cookie_header = go_async_get_request_header(thread_index, intern->request_id, "Cookie");
    if (cookie_header == NULL) {
        return;
    }

    /* Parse Cookie header: name=value; name2=value2 */
    char *pair, *saveptr;
    char *copy = estrdup(cookie_header);
    free(cookie_header);

    pair = php_strtok_r(copy, ";", &saveptr);
    while (pair != NULL) {
        /* Skip leading whitespace */
        while (*pair == ' ') pair++;

        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            char *key = pair;
            char *val = eq + 1;
            size_t val_len = frankenphp_url_decode(val, strlen(val));
            add_assoc_stringl(return_value, key, val, val_len);
        }
        pair = php_strtok_r(NULL, ";", &saveptr);
    }

    efree(copy);
}

/* Request::getBody(): string */
PHP_METHOD(FrankenPHP_Request, getBody)
{
    frankenphp_request_object *intern;
    char *body;
    size_t length = 0;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    /* Get body from Go via CGO */
    body = go_async_get_request_body(thread_index, intern->request_id, &length);
    if (body == NULL || length == 0) {
        RETURN_EMPTY_STRING();
    }

    RETVAL_STRINGL(body, length);
    free(body);
}

/* Request::getParsedBody(): array */
PHP_METHOD(FrankenPHP_Request, getParsedBody)
{
    frankenphp_request_object *intern;
    char *data;
    size_t length = 0;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    array_init(return_value);

    data = go_async_get_parsed_body(thread_index, intern->request_id, &length);
    if (data == NULL || length == 0) {
        return;
    }

    /* Parse "name\0value\0" pairs */
    size_t i = 0;
    while (i < length) {
        const char *name = data + i;
        size_t name_len = strlen(name);
        i += name_len + 1;
        if (i >= length) break;

        const char *value = data + i;
        size_t value_len = strlen(value);
        i += value_len + 1;

        add_assoc_stringl(return_value, name, value, value_len);
    }

    free(data);
}

/* Helper: create an UploadedFile object from metadata */
static void frankenphp_create_uploaded_file(zval *return_value,
    const char *field_name, const char *file_name, const char *mime_type,
    const char *tmp_name, zend_long size, zend_long error)
{
    object_init_ex(return_value, frankenphp_uploadedfile_ce);

    zend_update_property_string(frankenphp_uploadedfile_ce, Z_OBJ_P(return_value),
        "name", sizeof("name") - 1, file_name);
    zend_update_property_string(frankenphp_uploadedfile_ce, Z_OBJ_P(return_value),
        "type", sizeof("type") - 1, mime_type);
    zend_update_property_string(frankenphp_uploadedfile_ce, Z_OBJ_P(return_value),
        "tmpName", sizeof("tmpName") - 1, tmp_name);
    zend_update_property_long(frankenphp_uploadedfile_ce, Z_OBJ_P(return_value),
        "size", sizeof("size") - 1, size);
    zend_update_property_long(frankenphp_uploadedfile_ce, Z_OBJ_P(return_value),
        "error", sizeof("error") - 1, error);
}

/* Request::getUploadedFiles(): array */
PHP_METHOD(FrankenPHP_Request, getUploadedFiles)
{
    frankenphp_request_object *intern;
    char *json_data;
    size_t length = 0;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));

    array_init(return_value);

    json_data = go_async_get_uploaded_files(thread_index, intern->request_id, &length);
    if (json_data == NULL || length == 0) {
        return;
    }

    /* Decode JSON array of file metadata */
    zval json_arr;
    php_json_decode(&json_arr, json_data, length, 1, PHP_JSON_PARSER_DEFAULT_DEPTH);
    free(json_data);

    if (Z_TYPE(json_arr) != IS_ARRAY) {
        zval_ptr_dtor(&json_arr);
        return;
    }

    /* Convert JSON objects to UploadedFile objects */
    zval *entry;
    ZEND_HASH_FOREACH_VAL(Z_ARRVAL(json_arr), entry) {
        if (Z_TYPE_P(entry) != IS_ARRAY) continue;

        zval *zfield = zend_hash_str_find(Z_ARRVAL_P(entry), "field_name", sizeof("field_name") - 1);
        zval *zfile  = zend_hash_str_find(Z_ARRVAL_P(entry), "file_name", sizeof("file_name") - 1);
        zval *zmime  = zend_hash_str_find(Z_ARRVAL_P(entry), "mime_type", sizeof("mime_type") - 1);
        zval *ztmp   = zend_hash_str_find(Z_ARRVAL_P(entry), "tmp_name", sizeof("tmp_name") - 1);
        zval *zsize  = zend_hash_str_find(Z_ARRVAL_P(entry), "size", sizeof("size") - 1);
        zval *zerr   = zend_hash_str_find(Z_ARRVAL_P(entry), "error", sizeof("error") - 1);

        if (!zfield || !zfile) continue;

        const char *field_name = Z_TYPE_P(zfield) == IS_STRING ? Z_STRVAL_P(zfield) : "";
        const char *file_name = Z_TYPE_P(zfile) == IS_STRING ? Z_STRVAL_P(zfile) : "";
        const char *mime_type = zmime && Z_TYPE_P(zmime) == IS_STRING ? Z_STRVAL_P(zmime) : "";
        const char *tmp_name = ztmp && Z_TYPE_P(ztmp) == IS_STRING ? Z_STRVAL_P(ztmp) : "";
        zend_long size = zsize ? zval_get_long(zsize) : 0;
        zend_long error = zerr ? zval_get_long(zerr) : 0;

        /* Check if field already has an entry (multiple files for same field) */
        zval *existing = zend_hash_str_find(Z_ARRVAL_P(return_value), field_name, strlen(field_name));
        if (existing) {
            /* Convert to array if not already */
            if (Z_TYPE_P(existing) != IS_ARRAY) {
                zval arr, old;
                ZVAL_COPY(&old, existing);
                array_init(&arr);
                zend_hash_next_index_insert(Z_ARRVAL(arr), &old);
                zval_ptr_dtor(existing);
                ZVAL_COPY_VALUE(existing, &arr);
            }
            zval file_obj;
            frankenphp_create_uploaded_file(&file_obj, field_name, file_name, mime_type, tmp_name, size, error);
            zend_hash_next_index_insert(Z_ARRVAL_P(existing), &file_obj);
        } else {
            zval file_obj;
            frankenphp_create_uploaded_file(&file_obj, field_name, file_name, mime_type, tmp_name, size, error);
            add_assoc_zval(return_value, field_name, &file_obj);
        }
    } ZEND_HASH_FOREACH_END();

    zval_ptr_dtor(&json_arr);
}

/* ============================================================================
 * UploadedFile Class Methods
 * ============================================================================ */

/* UploadedFile::getName(): string */
PHP_METHOD(FrankenPHP_UploadedFile, getName) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval *prop = zend_read_property(frankenphp_uploadedfile_ce, Z_OBJ_P(ZEND_THIS), "name", sizeof("name") - 1, 1, NULL);
    RETURN_COPY(prop);
}

/* UploadedFile::getType(): string */
PHP_METHOD(FrankenPHP_UploadedFile, getType) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval *prop = zend_read_property(frankenphp_uploadedfile_ce, Z_OBJ_P(ZEND_THIS), "type", sizeof("type") - 1, 1, NULL);
    RETURN_COPY(prop);
}

/* UploadedFile::getSize(): int */
PHP_METHOD(FrankenPHP_UploadedFile, getSize) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval *prop = zend_read_property(frankenphp_uploadedfile_ce, Z_OBJ_P(ZEND_THIS), "size", sizeof("size") - 1, 1, NULL);
    RETURN_COPY(prop);
}

/* UploadedFile::getTmpName(): string */
PHP_METHOD(FrankenPHP_UploadedFile, getTmpName) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval *prop = zend_read_property(frankenphp_uploadedfile_ce, Z_OBJ_P(ZEND_THIS), "tmpName", sizeof("tmpName") - 1, 1, NULL);
    RETURN_COPY(prop);
}

/* UploadedFile::getError(): int */
PHP_METHOD(FrankenPHP_UploadedFile, getError) {
    ZEND_PARSE_PARAMETERS_NONE();
    zval *prop = zend_read_property(frankenphp_uploadedfile_ce, Z_OBJ_P(ZEND_THIS), "error", sizeof("error") - 1, 1, NULL);
    RETURN_COPY(prop);
}

/* UploadedFile::moveTo(string $path): bool */
PHP_METHOD(FrankenPHP_UploadedFile, moveTo) {
    zend_string *destination;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(destination)
    ZEND_PARSE_PARAMETERS_END();

    zval *tmp_prop = zend_read_property(frankenphp_uploadedfile_ce, Z_OBJ_P(ZEND_THIS), "tmpName", sizeof("tmpName") - 1, 1, NULL);
    if (Z_TYPE_P(tmp_prop) != IS_STRING || Z_STRLEN_P(tmp_prop) == 0) {
        RETURN_FALSE;
    }

    /* Try rename first (fast, same filesystem), fall back to copy+delete */
    if (rename(Z_STRVAL_P(tmp_prop), ZSTR_VAL(destination)) == 0) {
        RETURN_TRUE;
    }

    /* Cross-filesystem: copy then delete */
    php_stream *src = php_stream_open_wrapper(Z_STRVAL_P(tmp_prop), "rb", REPORT_ERRORS, NULL);
    if (!src) RETURN_FALSE;

    php_stream *dst = php_stream_open_wrapper(ZSTR_VAL(destination), "wb", REPORT_ERRORS, NULL);
    if (!dst) {
        php_stream_close(src);
        RETURN_FALSE;
    }

    php_stream_copy_to_stream_ex(src, dst, PHP_STREAM_COPY_ALL, NULL);
    php_stream_close(src);
    php_stream_close(dst);
    unlink(Z_STRVAL_P(tmp_prop));

    RETURN_TRUE;
}

/* ============================================================================
 * Response Class Methods
 * ============================================================================ */

/* Response::setStatus(int $code): void */
PHP_METHOD(FrankenPHP_Response, setStatus)
{
    frankenphp_response_object *intern;
    zend_long status_code;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(status_code)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));
    intern->status_code = (int)status_code;
}

/* Helper: add a header value to the response object's headers HashTable */
static void frankenphp_response_add_header(frankenphp_response_object *intern, zend_string *name, zend_string *value, bool replace)
{
    zval *existing = zend_hash_find(&intern->headers, name);

    if (existing && replace) {
        zend_hash_clean(Z_ARRVAL_P(existing));
        zval val;
        ZVAL_STR_COPY(&val, value);
        zend_hash_next_index_insert(Z_ARRVAL_P(existing), &val);
    } else if (existing) {
        zval val;
        ZVAL_STR_COPY(&val, value);
        zend_hash_next_index_insert(Z_ARRVAL_P(existing), &val);
    } else {
        zval arr;
        array_init(&arr);
        zval val;
        ZVAL_STR_COPY(&val, value);
        zend_hash_next_index_insert(Z_ARRVAL(arr), &val);
        zend_hash_add(&intern->headers, name, &arr);
    }
}

/* Response::setHeader(string $name, string $value): void */
PHP_METHOD(FrankenPHP_Response, setHeader)
{
    frankenphp_response_object *intern;
    zend_string *name, *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_STR(value)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));
    frankenphp_response_add_header(intern, name, value, true);
}

/* Response::addHeader(string $name, string $value): void */
PHP_METHOD(FrankenPHP_Response, addHeader)
{
    frankenphp_response_object *intern;
    zend_string *name, *value;

    ZEND_PARSE_PARAMETERS_START(2, 2)
        Z_PARAM_STR(name)
        Z_PARAM_STR(value)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));
    frankenphp_response_add_header(intern, name, value, false);
}

/* Response::removeHeader(string $name): void */
PHP_METHOD(FrankenPHP_Response, removeHeader)
{
    frankenphp_response_object *intern;
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));
    zend_hash_del(&intern->headers, name);
}

/* Response::getStatus(): int */
PHP_METHOD(FrankenPHP_Response, getStatus)
{
    frankenphp_response_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));
    RETURN_LONG(intern->status_code);
}

/* Response::getHeader(string $name): ?string */
PHP_METHOD(FrankenPHP_Response, getHeader)
{
    frankenphp_response_object *intern;
    zend_string *name;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(name)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));

    zval *arr = zend_hash_find(&intern->headers, name);
    if (!arr || Z_TYPE_P(arr) != IS_ARRAY) {
        RETURN_NULL();
    }

    /* Return first value */
    zval *first = zend_hash_index_find(Z_ARRVAL_P(arr), 0);
    if (!first || Z_TYPE_P(first) != IS_STRING) {
        RETURN_NULL();
    }

    RETURN_STR_COPY(Z_STR_P(first));
}

/* Response::getHeaders(): array */
PHP_METHOD(FrankenPHP_Response, getHeaders)
{
    frankenphp_response_object *intern;
    zend_string *name;
    zval *arr;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));

    array_init(return_value);

    ZEND_HASH_FOREACH_STR_KEY_VAL(&intern->headers, name, arr) {
        if (!name || Z_TYPE_P(arr) != IS_ARRAY) continue;

        zval copy;
        ZVAL_ARR(&copy, zend_array_dup(Z_ARRVAL_P(arr)));
        zend_hash_add(Z_ARRVAL_P(return_value), name, &copy);
    } ZEND_HASH_FOREACH_END();
}

/* Response::redirect(string $url, int $code = 302): void */
PHP_METHOD(FrankenPHP_Response, redirect)
{
    frankenphp_response_object *intern;
    zend_string *url;
    zend_long code = 302;

    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(url)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(code)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));
    intern->status_code = (int)code;

    zend_string *location = zend_string_init("Location", sizeof("Location") - 1, 0);
    frankenphp_response_add_header(intern, location, url, true);
    zend_string_release(location);
}

/* Response::isHeadersSent(): bool */
PHP_METHOD(FrankenPHP_Response, isHeadersSent)
{
    frankenphp_response_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));
    RETURN_BOOL(intern->headers_sent);
}

/* Response::write(string $data): void */
PHP_METHOD(FrankenPHP_Response, write)
{
    frankenphp_response_object *intern;
    zend_string *data_str;
    size_t old_len, new_len;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data_str)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));

    if (!intern->buffer) {
        intern->buffer = zend_string_copy(data_str);
    } else {
        old_len = ZSTR_LEN(intern->buffer);
        new_len = old_len + ZSTR_LEN(data_str);
        intern->buffer = zend_string_extend(intern->buffer, new_len, 0);
        memcpy(ZSTR_VAL(intern->buffer) + old_len, ZSTR_VAL(data_str), ZSTR_LEN(data_str));
        ZSTR_VAL(intern->buffer)[new_len] = '\0';
    }
}

/* Serialize headers HashTable into "name\0value\0" format */
static zend_string *frankenphp_serialize_headers(HashTable *headers)
{
    smart_str buf = {0};
    zend_string *name;
    zval *arr;

    ZEND_HASH_FOREACH_STR_KEY_VAL(headers, name, arr) {
        if (!name || Z_TYPE_P(arr) != IS_ARRAY) continue;

        zval *val;
        ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(arr), val) {
            if (Z_TYPE_P(val) != IS_STRING) continue;

            smart_str_append(&buf, name);
            smart_str_appendc(&buf, '\0');
            smart_str_append(&buf, Z_STR_P(val));
            smart_str_appendc(&buf, '\0');
        } ZEND_HASH_FOREACH_END();
    } ZEND_HASH_FOREACH_END();

    if (buf.s) {
        return smart_str_extract(&buf);
    }
    return NULL;
}

/* Response::end(): void */
PHP_METHOD(FrankenPHP_Response, end)
{
    frankenphp_response_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));

    if (intern->headers_sent) {
        return;
    }
    intern->headers_sent = 1;

    zend_string *headers_blob = frankenphp_serialize_headers(&intern->headers);
    void *headers_data = headers_blob ? ZSTR_VAL(headers_blob) : NULL;
    size_t headers_len = headers_blob ? ZSTR_LEN(headers_blob) : 0;

    void *body_data = intern->buffer ? ZSTR_VAL(intern->buffer) : NULL;
    size_t body_len = intern->buffer ? ZSTR_LEN(intern->buffer) : 0;

    go_async_response_complete(thread_index, intern->request_id,
                               intern->status_code,
                               headers_data, headers_len,
                               body_data, body_len);

    if (headers_blob) {
        zend_string_release(headers_blob);
    }
}

/* ============================================================================
 * Method Argument Info
 * ============================================================================ */

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_httpserver_onrequest, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_CALLABLE_INFO(0, callback, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getmethod, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_geturi, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getheader, 0, 1, IS_STRING, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getheaders, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_gethost, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getremoteaddr, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getprotocolversion, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getscheme, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getqueryparams, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getcookies, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getbody, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getparsedbody, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getuploadedfiles, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

/* UploadedFile arginfo */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_uploadedfile_getname, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_uploadedfile_gettype, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_uploadedfile_getsize, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_uploadedfile_gettmpname, 0, 0, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_uploadedfile_geterror, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_uploadedfile_moveto, 0, 1, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, path, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_setstatus, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, code, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_setheader, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_addheader, 0, 2, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_removeheader, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_getstatus, 0, 0, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_getheader, 0, 1, IS_STRING, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_getheaders, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_redirect, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, url, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, code, IS_LONG, 0, "302")
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_isheaderssent, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_write, 0, 1, IS_VOID, 0)
    ZEND_ARG_TYPE_INFO(0, data, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_response_end, 0, 0, IS_VOID, 0)
ZEND_END_ARG_INFO()

/* ============================================================================
 * Class Method Tables
 * ============================================================================ */

static const zend_function_entry frankenphp_httpserver_methods[] = {
    PHP_ME(FrankenPHP_HttpServer, onRequest, arginfo_httpserver_onrequest, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
    PHP_FE_END
};

static const zend_function_entry frankenphp_request_methods[] = {
    PHP_ME(FrankenPHP_Request, getMethod, arginfo_request_getmethod, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getUri, arginfo_request_geturi, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getHeader, arginfo_request_getheader, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getHeaders, arginfo_request_getheaders, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getHost, arginfo_request_gethost, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getRemoteAddr, arginfo_request_getremoteaddr, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getProtocolVersion, arginfo_request_getprotocolversion, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getScheme, arginfo_request_getscheme, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getQueryParams, arginfo_request_getqueryparams, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getCookies, arginfo_request_getcookies, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getBody, arginfo_request_getbody, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getParsedBody, arginfo_request_getparsedbody, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getUploadedFiles, arginfo_request_getuploadedfiles, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry frankenphp_response_methods[] = {
    PHP_ME(FrankenPHP_Response, setStatus, arginfo_response_setstatus, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, setHeader, arginfo_response_setheader, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, addHeader, arginfo_response_addheader, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, removeHeader, arginfo_response_removeheader, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, getStatus, arginfo_response_getstatus, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, getHeader, arginfo_response_getheader, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, getHeaders, arginfo_response_getheaders, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, redirect, arginfo_response_redirect, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, isHeadersSent, arginfo_response_isheaderssent, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, write, arginfo_response_write, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, end, arginfo_response_end, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry frankenphp_uploadedfile_methods[] = {
    PHP_ME(FrankenPHP_UploadedFile, getName, arginfo_uploadedfile_getname, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_UploadedFile, getType, arginfo_uploadedfile_gettype, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_UploadedFile, getSize, arginfo_uploadedfile_getsize, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_UploadedFile, getTmpName, arginfo_uploadedfile_gettmpname, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_UploadedFile, getError, arginfo_uploadedfile_geterror, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_UploadedFile, moveTo, arginfo_uploadedfile_moveto, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

/* ============================================================================
 * Module Initialization
 * ============================================================================ */

int frankenphp_extension_init(void)
{
    zend_class_entry ce;

    /* Register FrankenPHP\HttpServer class */
    INIT_CLASS_ENTRY(ce, "FrankenPHP\\HttpServer", frankenphp_httpserver_methods);
    frankenphp_httpserver_ce = zend_register_internal_class(&ce);

    /* Register FrankenPHP\Request class */
    INIT_CLASS_ENTRY(ce, "FrankenPHP\\Request", frankenphp_request_methods);
    frankenphp_request_ce = zend_register_internal_class(&ce);
    frankenphp_request_ce->create_object = frankenphp_request_create_object;

    memcpy(&frankenphp_request_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    frankenphp_request_object_handlers.offset = XtOffsetOf(frankenphp_request_object, std);
    frankenphp_request_object_handlers.free_obj = frankenphp_request_free_object;

    /* Register FrankenPHP\Response class */
    INIT_CLASS_ENTRY(ce, "FrankenPHP\\Response", frankenphp_response_methods);
    frankenphp_response_ce = zend_register_internal_class(&ce);
    frankenphp_response_ce->create_object = frankenphp_response_create_object;

    memcpy(&frankenphp_response_object_handlers, zend_get_std_object_handlers(), sizeof(zend_object_handlers));
    frankenphp_response_object_handlers.offset = XtOffsetOf(frankenphp_response_object, std);
    frankenphp_response_object_handlers.free_obj = frankenphp_response_free_object;

    /* Register FrankenPHP\UploadedFile class */
    INIT_CLASS_ENTRY(ce, "FrankenPHP\\UploadedFile", frankenphp_uploadedfile_methods);
    frankenphp_uploadedfile_ce = zend_register_internal_class(&ce);

    /* Declare properties */
    zval default_str, default_long;
    ZVAL_STRING(&default_str, "");
    ZVAL_LONG(&default_long, 0);

    zend_declare_property(frankenphp_uploadedfile_ce, "name", sizeof("name") - 1, &default_str, ZEND_ACC_PROTECTED);
    zend_declare_property(frankenphp_uploadedfile_ce, "type", sizeof("type") - 1, &default_str, ZEND_ACC_PROTECTED);
    zend_declare_property(frankenphp_uploadedfile_ce, "tmpName", sizeof("tmpName") - 1, &default_str, ZEND_ACC_PROTECTED);
    zend_declare_property(frankenphp_uploadedfile_ce, "size", sizeof("size") - 1, &default_long, ZEND_ACC_PROTECTED);
    zend_declare_property(frankenphp_uploadedfile_ce, "error", sizeof("error") - 1, &default_long, ZEND_ACC_PROTECTED);

    zval_ptr_dtor(&default_str);

    return SUCCESS;
}

/* ============================================================================
 * Helper Functions (for later use in frankenphp_trueasync.c)
 * ============================================================================ */

/* Get the stored request callback */
zval *frankenphp_get_request_callback(void)
{
    return async_request_callback;
}

/* Create a Request object with given request_id */
void frankenphp_create_request_object(zval *return_value, uint64_t request_id)
{
    object_init_ex(return_value, frankenphp_request_ce);

    frankenphp_request_object *intern = frankenphp_request_from_obj(Z_OBJ_P(return_value));
    intern->request_id = request_id;
}

/* Create a Response object with given request_id */
void frankenphp_create_response_object(zval *return_value, uint64_t request_id)
{
    object_init_ex(return_value, frankenphp_response_ce);

    frankenphp_response_object *intern = frankenphp_response_from_obj(Z_OBJ_P(return_value));
    intern->request_id = request_id;
}
