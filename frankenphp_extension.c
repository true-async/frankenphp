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
#include "SAPI.h"
#include "frankenphp.h"

/* Global callback storage for HttpServer::onRequest() */
static zval *frankenphp_request_callback = NULL;

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
    uint64_t request_id;  /* Links to Go's asyncRequestMap */
    uint8_t headers_sent;
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

    return &intern->std;
}

static void frankenphp_response_free_object(zend_object *object) {
    frankenphp_response_object *intern = frankenphp_response_from_obj(object);

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
    if (frankenphp_request_callback != NULL) {
        zval_ptr_dtor(frankenphp_request_callback);
        efree(frankenphp_request_callback);
    }

    /* Store new callback */
    frankenphp_request_callback = emalloc(sizeof(zval));
    ZVAL_COPY(frankenphp_request_callback, callback);

    RETURN_TRUE;
}

/* ============================================================================
 * Request Class Methods
 * ============================================================================ */

/* Request::getMethod(): string */
PHP_METHOD(FrankenPHP_Request, getMethod)
{
    frankenphp_request_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    (void)intern; /* Unused until TODO implemented */

    /* TODO: Get method from Go via CGO */
    RETURN_STRING("GET");
}

/* Request::getUri(): string */
PHP_METHOD(FrankenPHP_Request, getUri)
{
    frankenphp_request_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    (void)intern; /* Unused until TODO implemented */

    /* TODO: Get URI from Go via CGO */
    RETURN_STRING("/");
}

/* Request::getHeaders(): array */
PHP_METHOD(FrankenPHP_Request, getHeaders)
{
    frankenphp_request_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    (void)intern; /* Unused until TODO implemented */

    /* TODO: Get headers from Go via CGO */
    array_init(return_value);
}

/* Request::getBody(): string */
PHP_METHOD(FrankenPHP_Request, getBody)
{
    frankenphp_request_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_request_from_obj(Z_OBJ_P(ZEND_THIS));
    (void)intern; /* Unused until TODO implemented */

    /* TODO: Get body from Go via CGO */
    RETURN_EMPTY_STRING();
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
    (void)intern; /* Unused until TODO implemented */

    /* TODO: Set status via Go CGO */
    SG(sapi_headers).http_response_code = (int)status_code;
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
    (void)intern; /* Unused until TODO implemented */

    /* TODO: Set header via Go CGO */
    sapi_header_line ctr = {0};
    char *header_line = NULL;
    ctr.line_len = spprintf(&header_line, 0, "%s: %s", ZSTR_VAL(name), ZSTR_VAL(value));
    ctr.line = header_line;
    ctr.response_code = 0;
    sapi_header_op(SAPI_HEADER_REPLACE, &ctr);
    efree(header_line);
}

/* Response::write(string $data): void */
PHP_METHOD(FrankenPHP_Response, write)
{
    frankenphp_response_object *intern;
    zend_string *data_str;

    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(data_str)
    ZEND_PARSE_PARAMETERS_END();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));

    /* Send headers if not sent */
    if (!intern->headers_sent) {
        /* TODO: Send headers via Go CGO */
        sapi_send_headers();
        intern->headers_sent = 1;
    }

    /* MVP: Use blocking write via SAPI (will integrate with go_ub_write later) */
    PHPWRITE(ZSTR_VAL(data_str), ZSTR_LEN(data_str));
}

/* Response::end(): void */
PHP_METHOD(FrankenPHP_Response, end)
{
    frankenphp_response_object *intern;

    ZEND_PARSE_PARAMETERS_NONE();

    intern = frankenphp_response_from_obj(Z_OBJ_P(ZEND_THIS));

    /* Flush any remaining output */
    if (!intern->headers_sent) {
        sapi_send_headers();
        intern->headers_sent = 1;
    }

    /* TODO: Call go_async_notify_request_done(intern->request_id) */
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
    PHP_ME(FrankenPHP_Request, getHeaders, arginfo_request_getheaders, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Request, getBody, arginfo_request_getbody, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

static const zend_function_entry frankenphp_response_methods[] = {
    PHP_ME(FrankenPHP_Response, setStatus, arginfo_response_setstatus, ZEND_ACC_PUBLIC)
    PHP_ME(FrankenPHP_Response, setHeader, arginfo_response_setheader, ZEND_ACC_PUBLIC)
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
    return frankenphp_request_callback;
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
