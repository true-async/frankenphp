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
#include "SAPI.h"
#include "frankenphp.h"
#include <pthread.h>

/* Forward declarations for CGO functions from Go */
extern char *go_async_get_request_method(uintptr_t thread_index, uint64_t request_id);
extern char *go_async_get_request_uri(uintptr_t thread_index, uint64_t request_id);
extern char *go_async_get_request_header(uintptr_t thread_index, uint64_t request_id, const char *header_name);
extern char *go_async_get_request_body(uintptr_t thread_index, uint64_t request_id, size_t *length);
extern char *go_async_get_all_request_headers(uintptr_t thread_index, uint64_t request_id, size_t *length);
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

    /* Store new callback in TLS */
    async_request_callback = emalloc(sizeof(zval));
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

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_request_getbody, 0, 0, IS_STRING, 0)
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
    PHP_ME(FrankenPHP_Request, getBody, arginfo_request_getbody, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry frankenphp_response_methods[] = {
    PHP_ME(FrankenPHP_Response, setStatus, arginfo_response_setstatus, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, setHeader, arginfo_response_setheader, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, addHeader, arginfo_response_addheader, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, write, arginfo_response_write, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, end, arginfo_response_end, ZEND_ACC_PUBLIC)
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
