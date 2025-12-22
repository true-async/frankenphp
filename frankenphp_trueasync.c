/*
 * FrankenPHP TrueAsync Integration
 *
 * Integrates FrankenPHP with TrueAsync event loop:
 *
 * Author: Edmond Dantes
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_main.h"
#include "SAPI.h"
#include "Zend/zend_async_API.h"
#include "Zend/zend_exceptions.h"
#include "frankenphp.h"

#include <fcntl.h>
#include <unistd.h>

extern __thread uintptr_t thread_index;
extern __thread zval *async_request_callback;

extern int go_async_worker_get_notification_fd(uintptr_t thread_index);
extern void go_async_worker_clear_notification(uintptr_t thread_index);
extern uint64_t go_async_worker_check_requests(uintptr_t thread_index);

__thread zend_async_heartbeat_handler_t old_heartbeat_handler = NULL;
__thread zend_async_poll_event_t *request_event = NULL;

static void close_request_event(void)
{
    zend_async_poll_event_t *event = request_event;
    request_event = NULL;

    if (event != NULL) {
        event->base.dispose(&event->base);
    }
}

/*
 * Scheduler tick handler - FAST PATH for request checking
 * Called on each scheduler tick to poll for new requests without eventfd overhead
 */
void frankenphp_scheudler_tick_handler(void)
{
    uint64_t request_id;

    while ((request_id = go_async_worker_check_requests(thread_index)) != 0) {
        if (request_id == UINT64_MAX) {
            //close_request_event();
            ZEND_ASYNC_SHUTDOWN();
            return;
        }
        frankenphp_handle_request_async(request_id);
    }

    if (old_heartbeat_handler) {
        old_heartbeat_handler();
    }
}

/*
 * Enters async mode after script execution
 * Called from frankenphp_execute_script when async mode was requested
 */
void frankenphp_enter_async_mode(void)
{
    int notifier_fd;

    if (async_request_callback == NULL) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Cannot enter async mode: no callback registered. "
                           "Please use FrankenPHP\\HttpServer::onRequest()");
        return;
    }

    notifier_fd = go_async_worker_get_notification_fd(thread_index);
    if (notifier_fd < 0) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to get AsyncNotifier FD from Go thread");
        return;
    }

    if (!frankenphp_register_request_notifier(notifier_fd, thread_index)) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to register AsyncNotifier FD");
        return;
    }

    if (!frankenphp_activate_true_async()) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to activate TrueAsync scheduler");
        return;
    }

    old_heartbeat_handler = zend_async_set_heartbeat_handler(frankenphp_scheudler_tick_handler);

    frankenphp_suspend_main_coroutine();

    /* Async loop is stopping: free TLS pending write table */
    frankenphp_async_pending_writes_destroy();

    zend_async_set_heartbeat_handler(old_heartbeat_handler);
    old_heartbeat_handler = NULL;

    close_request_event();
    if (async_request_callback != NULL) {
        zval_ptr_dtor(async_request_callback);
        efree(async_request_callback);
        async_request_callback = NULL;
    }
}

/*
 * Activates the TrueAsync scheduler
 */
bool frankenphp_activate_true_async(void)
{
    if (UNEXPECTED(!ZEND_ASYNC_SCHEDULER_LAUNCH())) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: The Scheduler was not properly activated");
        return false;
    }

    return ZEND_ASYNC_IS_ACTIVE;
}

/*
 * AsyncNotifier FD callback - SLOW PATH
 * Invoked by TrueAsync event loop when eventfd/pipe becomes readable
 */
static void frankenphp_async_check_requests_callback(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception)
{
    uintptr_t thread_idx;
    uint64_t request_id;

    thread_idx = *(uintptr_t *)((char *)event + event->extra_offset);
    go_async_worker_clear_notification(thread_idx);

    while ((request_id = go_async_worker_check_requests(thread_idx)) != 0) {
        if (request_id == UINT64_MAX) {
            event->stop(event);
            ZEND_ASYNC_SHUTDOWN();
            return;
        }
        frankenphp_handle_request_async(request_id);

        if (UNEXPECTED(EG(exception))) {
            break;
        }
    }
}

/*
 * Registers AsyncNotifier FD with TrueAsync event loop
 */
bool frankenphp_register_request_notifier(int notifier_fd, uintptr_t thread_index)
{
    int flags;
    uintptr_t *extra_data;

    if (notifier_fd < 0) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Invalid AsyncNotifier FD: %d", notifier_fd);
        return false;
    }

    flags = fcntl(notifier_fd, F_GETFL, 0);
    if (flags == -1 || fcntl(notifier_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to set AsyncNotifier FD to non-blocking");
        return false;
    }

    request_event = ZEND_ASYNC_NEW_POLL_EVENT_EX(
        (zend_file_descriptor_t) notifier_fd, 0, ASYNC_READABLE, sizeof(uintptr_t)
    );

    if (UNEXPECTED(request_event == NULL)) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to create TrueAsync poll event");
        return false;
    }

    extra_data = (uintptr_t *)((char *)request_event + request_event->base.extra_offset);
    *extra_data = thread_index;

    /* Register SLOW PATH callback - called when FD is readable */
    request_event->base.add_callback(&request_event->base,
        ZEND_ASYNC_EVENT_CALLBACK(frankenphp_async_check_requests_callback)
    );

    request_event->base.start(&request_event->base);

    return true;
}

/*
 * Coroutine entry point - invokes user callback
 */
void frankenphp_request_coroutine_entry(void)
{
    zend_coroutine_t *coroutine;
    uint64_t request_id;
    zval request_obj, response_obj, params[2], retval;
    zval *callback;

    /* Get current coroutine and extract request_id */
    coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    request_id = (uint64_t)(uintptr_t)coroutine->extended_data;

    /* Get the user callback */
    callback = frankenphp_get_request_callback();
    if (callback == NULL) {
        php_error(E_WARNING, "FrankenPHP TrueAsync: No request callback registered");
        return;
    }

    /* Create Request and Response objects */
    frankenphp_create_request_object(&request_obj, request_id);
    frankenphp_create_response_object(&response_obj, request_id);

    /* Prepare callback parameters */
    params[0] = request_obj;
    params[1] = response_obj;

    /* Call user callback: callback($request, $response) */
    ZVAL_UNDEF(&retval);
    if (call_user_function(NULL, NULL, callback, &retval, 2, params) == FAILURE) {
        php_error(E_WARNING, "FrankenPHP TrueAsync: Failed to call request handler callback");
    }

    /* Cleanup */
    zval_ptr_dtor(&retval);
    zval_ptr_dtor(&request_obj);
    zval_ptr_dtor(&response_obj);
}

/*
 * Creates a coroutine for handling an incoming request
 */
void frankenphp_handle_request_async(uint64_t request_id)
{
    // Create new scope for this coroutine (for coroutine isolation)
    zend_async_scope_t *request_scope = ZEND_ASYNC_NEW_SCOPE(ZEND_ASYNC_CURRENT_SCOPE);
    if (UNEXPECTED(request_scope == NULL)) {
        return;
    }

    /* Create coroutine within this scope */
    zend_coroutine_t * coroutine = ZEND_ASYNC_NEW_COROUTINE(request_scope);
    if (UNEXPECTED(coroutine == NULL)) {
        return;
    }

    coroutine->internal_entry = frankenphp_request_coroutine_entry;
    coroutine->extended_data = (void *)(uintptr_t)request_id;

    ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine);
}

/* Server wait event methods */
static bool frankenphp_server_wait_event_start(zend_async_event_t *event)
{
    /* No action needed - event waits indefinitely */
    return true;
}

static bool frankenphp_server_wait_event_stop(zend_async_event_t *event)
{
    /* No action needed */
    return true;
}

static bool
frankenphp_server_wait_event_add_callback(zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_push(event, callback);
}

static bool
frankenphp_server_wait_event_del_callback(zend_async_event_t *event, zend_async_event_callback_t *callback)
{
    return zend_async_callbacks_remove(event, callback);
}

static bool
frankenphp_server_wait_event_replay(zend_async_event_t *event, zend_async_event_callback_t *callback, zval *result, zend_object **exception)
{
    /* Event never resolves - cannot replay */
    return false;
}

static zend_string *frankenphp_server_wait_event_info(zend_async_event_t *event)
{
    return zend_string_init("Frankenphp server waiting", sizeof("Frankenphp server waiting") - 1, 0);
}

static bool
frankenphp_server_wait_event_dispose(zend_async_event_t *event)
{
    if (ZEND_ASYNC_EVENT_REFCOUNT(event) > 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
        return true;
    }

    if (ZEND_ASYNC_EVENT_REFCOUNT(event) == 1) {
        ZEND_ASYNC_EVENT_DEL_REF(event);
    }

    zend_async_callbacks_free(event);
    /* Free the event */
    efree(event);

    return true;
}

/*
 * Suspends the main coroutine indefinitely
 * This allows the event loop to take over and handle requests
 */
bool frankenphp_suspend_main_coroutine(void)
{
    zend_coroutine_t *coroutine = ZEND_ASYNC_CURRENT_COROUTINE;
    zend_async_event_t *event = ecalloc(1, sizeof(zend_async_event_t));

    event->ref_count = 1;
    event->start = frankenphp_server_wait_event_start;
    event->stop = frankenphp_server_wait_event_stop;
    event->add_callback = frankenphp_server_wait_event_add_callback;
    event->del_callback = frankenphp_server_wait_event_del_callback;
    event->replay = frankenphp_server_wait_event_replay;
    event->info = frankenphp_server_wait_event_info;
    event->dispose = frankenphp_server_wait_event_dispose;

    /* Create waker for coroutine */
    if (UNEXPECTED(zend_async_waker_new(coroutine) == NULL)) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to create waker");
        event->dispose(event);
        return false;
    }

    /* Attach coroutine to wait event - it will suspend until GRACEFUL_SHUTDOWN */
    zend_async_resume_when(coroutine, event, true, zend_async_waker_callback_resolve, NULL);

    if (UNEXPECTED(EG(exception) != NULL)) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to attach coroutine to wait event");
        zend_async_waker_clean(coroutine);
        event->dispose(event);
        return false;
    }

    ZEND_ASYNC_SUSPEND();
    zend_async_waker_clean(coroutine);

    if (UNEXPECTED(EG(exception) != NULL)) {
        const zend_class_entry *cancel_ce = ZEND_ASYNC_GET_EXCEPTION_CE(ZEND_ASYNC_EXCEPTION_CANCELLATION);

        if (EG(exception)->ce != cancel_ce &&
            !instanceof_function(EG(exception)->ce, cancel_ce)) {
            php_error(E_WARNING, "FrankenPHP TrueAsync: clearing lingering exception (%s)",
                      EG(exception)->ce && EG(exception)->ce->name ? ZSTR_VAL(EG(exception)->ce->name) : "unknown");
        }

        zend_clear_exception();
    }

    return true;
}
