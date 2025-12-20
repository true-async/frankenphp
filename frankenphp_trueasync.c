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
#include "frankenphp.h"

#include <fcntl.h>
#include <unistd.h>

/* External TLS variables from frankenphp.c */
extern __thread bool is_async_mode_requested;
extern __thread zval *async_request_callback;

extern void go_async_clear_notification(uintptr_t thread_index);
extern uint64_t go_async_check_new_requests(uintptr_t thread_index);

__thread uintptr_t frankenphp_current_thread_index = 0;

/*
 * Checks if async mode was requested via HttpServer::onRequest()
 * Called from Go after first script execution to determine if worker should activate async mode
 */
bool frankenphp_check_async_mode_requested(uintptr_t thread_index)
{
    return is_async_mode_requested;
}

/*
 * Enters async mode after script execution
 * Called from frankenphp_execute_script when async mode was requested
 */
void frankenphp_enter_async_mode(void)
{
    if (async_request_callback == NULL) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Cannot enter async mode: no callback registered. "
                           "Please use FrankenPHP\\HttpServer::onRequest()");
        return;
    }

    if (!frankenphp_activate_true_async()) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: Failed to activate TrueAsync scheduler");
        return;
    }

    frankenphp_suspend_main_coroutine();
}

/*
 * Activates the TrueAsync scheduler
 */
bool frankenphp_activate_true_async(void)
{
    if (UNEXPECTED(ZEND_ASYNC_SCHEDULER_LAUNCH()) {
        php_error(E_ERROR, "FrankenPHP TrueAsync: The Scheduler was not properly activated");
        return false;
    }

    return ZEND_ASYNC_IS_ACTIVE;
}

/*
 * Callback invoked when AsyncNotifier FD becomes readable (SLOW PATH)
 * This is called by TrueAsync event loop when eventfd signals new request
 */
static void frankenphp_async_check_requests_callback(
    zend_async_event_t *event,
    zend_async_event_callback_t *callback,
    void *result,
    zend_object *exception)
{
    uintptr_t thread_index;
    uint64_t request_id;

    /* Extract thread_index from event extra data */
    thread_index = *(uintptr_t *)((char *)event + event->extra_offset);

    /* Clear the eventfd notification */
    go_async_clear_notification(thread_index);

    /* Check for new requests from Go channel (may return multiple) */
    while ((request_id = go_async_check_new_requests(thread_index)) != 0) {
        /* Create coroutine for this request */
        frankenphp_handle_request_async(request_id, thread_index);
    }
}

/*
 * Registers AsyncNotifier FD with TrueAsync poll events
 * Also registers FAST PATH - direct scheduler callback
 */
bool frankenphp_register_async_notifier_event(int notifier_fd, uintptr_t thread_index)
{
    zend_async_poll_event_t *poll_event;
    uintptr_t *extra_data;

    /* Store thread_index in thread-local storage for heartbeat handler */
    frankenphp_current_thread_index = thread_index;

    if (notifier_fd < 0) {
        php_error(E_ERROR, "Invalid AsyncNotifier FD: %d", notifier_fd);
        return false;
    }

    /* Set FD to non-blocking mode */
    int flags = fcntl(notifier_fd, F_GETFL, 0);
    if (flags == -1 || fcntl(notifier_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        php_error(E_ERROR, "Failed to set AsyncNotifier FD to non-blocking");
        return false;
    }

    /* Create TrueAsync poll event with extra space for thread_index */
    poll_event = ZEND_ASYNC_NEW_POLL_EVENT_EX(
        notifier_fd,
        false,              /* not persistent */
        ASYNC_READABLE,     /* watch for readable events */
        sizeof(uintptr_t)   /* extra space for thread_index */
    );

    if (poll_event == NULL) {
        php_error(E_ERROR, "Failed to create TrueAsync poll event");
        return false;
    }

    /* Store thread_index in extra data */
    extra_data = (uintptr_t *)((char *)poll_event + poll_event->base.extra_offset);
    *extra_data = thread_index;

    /* Register SLOW PATH callback - called when FD is readable */
    poll_event->base.add_callback(
        &poll_event->base,
        ZEND_ASYNC_EVENT_CALLBACK(frankenphp_async_check_requests_callback)
    );

    /* Start polling (integrates with event loop) */
    poll_event->base.start(&poll_event->base);

    /* TODO: Register FAST PATH - scheduler integration
     * When coroutines are active, scheduler should call go_async_check_new_requests()
     * directly without waiting for eventfd to become readable.
     * This reduces latency by avoiding system calls.
     */

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
        php_error(E_WARNING, "No request callback registered");
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
        php_error(E_WARNING, "Failed to call request handler callback");
    }

    /* Cleanup */
    zval_ptr_dtor(&retval);
    zval_ptr_dtor(&request_obj);
    zval_ptr_dtor(&response_obj);
}

/*
 * Creates a coroutine for handling an incoming request
 */
void frankenphp_handle_request_async(uint64_t request_id, uintptr_t thread_index)
{
    /* Create new scope for this coroutine (for coroutine isolation)
     * Note: Superglobals are populated via standard SAPI mechanisms
     */
    zend_async_scope_t *request_scope = ZEND_ASYNC_NEW_SCOPE(ZEND_ASYNC_CURRENT_SCOPE);

    /* Create coroutine within this scope */
    zend_coroutine_t * coroutine = ZEND_ASYNC_NEW_COROUTINE(request_scope);
    coroutine->internal_entry = frankenphp_request_coroutine_entry;
    coroutine->extended_data = (void *)(uintptr_t)request_id;

    /* Enqueue coroutine for execution */
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

    /* Notify all callbacks that event is disposed */
    ZEND_ASYNC_CALLBACKS_NOTIFY(event, NULL, NULL);

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
        return;
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

    return true;
}
