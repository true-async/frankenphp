/*
 * FrankenPHP TrueAsync Integration
 *
 * Integrates FrankenPHP with TrueAsync event loop:
 * - Load entrypoint script once and keep it resident
 * - Register AsyncNotifier FD with TrueAsync poll events
 * - Create coroutines for each incoming request
 * - Suspend main coroutine to let event loop run
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "php_main.h"
#include "SAPI.h"
#include "frankenphp.h"

#include <fcntl.h>
#include <unistd.h>

/* External TLS variables from frankenphp.c */
extern __thread bool is_async_mode_requested;
extern __thread zval *async_request_callback;

/* TrueAsync headers - will be available when compiled with -tags trueasync */
#include "Zend/zend_async_API.h"

/* Forward declarations for CGO functions from Go */
extern void go_async_clear_notification(uintptr_t thread_index);
extern uint64_t go_async_check_new_requests(uintptr_t thread_index);

/* Thread-local storage for current thread index (needed by heartbeat handler) */
__thread uintptr_t frankenphp_current_thread_index = 0;

/* ============================================================================
 * 1. Auto-detection and Activation of Async Mode
 * ============================================================================ */

/*
 * Checks if async mode was requested via HttpServer::onRequest()
 * Called from Go after first script execution to determine if worker should activate async mode
 */
bool frankenphp_check_async_mode_requested(uintptr_t thread_index)
{
    /* Simply return the TLS flag value set by HttpServer::onRequest() */
    return is_async_mode_requested;
}

/*
 * Activates async mode for this worker thread
 * Called from Go when worker script uses HttpServer::onRequest()
 * Transitions from blocking worker mode to async event loop mode
 */
void frankenphp_activate_async_mode(uintptr_t thread_index)
{
    /* Store thread_index in TLS for heartbeat handler */
    frankenphp_current_thread_index = thread_index;

    /* Verify that callback was registered */
    if (async_request_callback == NULL) {
        php_error(E_ERROR, "Cannot activate async mode: no callback registered");
        return;
    }

    /* Activate TrueAsync scheduler */
    if (!frankenphp_activate_true_async()) {
        php_error(E_ERROR, "Failed to activate TrueAsync scheduler");
        return;
    }

    /* TODO: Register AsyncNotifier FD with event loop
     * We need to get the eventfd from Go via CGO call
     * int notifier_fd = go_async_get_notifier_fd(thread_index);
     * frankenphp_register_async_notifier_event(notifier_fd, thread_index);
     */

    /* Suspend main coroutine - event loop takes over */
    frankenphp_suspend_main_coroutine();

    /* We never return from here - event loop runs until shutdown */
}

/* ============================================================================
 * 2. Load Entrypoint Script
 * ============================================================================ */

/*
 * Loads the entrypoint.php script
 * This script calls HttpServer::onRequest($callback) to register the handler
 */
int frankenphp_async_load_entrypoint(char *entrypoint_path)
{
    zend_file_handle file_handle;
    int ret;

    if (!entrypoint_path || strlen(entrypoint_path) == 0) {
        php_error(E_ERROR, "TrueAsync entrypoint path is empty");
        return FAILURE;
    }

    /* Initialize file handle */
    zend_stream_init_filename(&file_handle, entrypoint_path);

    /* Start request to initialize superglobals */
    if (php_request_startup() == FAILURE) {
        zend_destroy_file_handle(&file_handle);
        return FAILURE;
    }

    /* Execute entrypoint script */
    ret = php_execute_script(&file_handle);

    /* Note: We do NOT call php_request_shutdown() here!
     * The entrypoint stays loaded and the event loop takes over
     */

    if (ret == SUCCESS) {
        /* Verify that callback was registered */
        zval *callback = frankenphp_get_request_callback();
        if (callback == NULL) {
            php_error(E_WARNING, "TrueAsync entrypoint did not register HttpServer::onRequest() callback");
            return FAILURE;
        }
    }

    return ret;
}

/* ============================================================================
 * 2. Activate TrueAsync Scheduler
 * ============================================================================ */

/*
 * Activates the TrueAsync scheduler
 */
bool frankenphp_activate_true_async(void)
{
    /* Check if TrueAsync extension is loaded */
    if (!ZEND_ASYNC_IS_READY) {
        php_error(E_ERROR, "TrueAsync extension is not loaded");
        return false;
    }

    /* Launch the scheduler */
    ZEND_ASYNC_SCHEDULER_LAUNCH();

    return ZEND_ASYNC_IS_ACTIVE;
}

/* ============================================================================
 * 3. Register AsyncNotifier with TrueAsync Event Loop
 * ============================================================================ */

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

/* ============================================================================
 * 4. Request Handling with Coroutines
 * ============================================================================ */

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
    zend_async_scope_t *request_scope;
    zend_coroutine_t *coroutine;

    /* Create new scope for this coroutine (for coroutine isolation)
     * Note: Superglobals are populated via standard SAPI mechanisms
     */
    request_scope = ZEND_ASYNC_NEW_SCOPE(ZEND_ASYNC_CURRENT_SCOPE);

    /* Create coroutine within this scope */
    coroutine = ZEND_ASYNC_NEW_COROUTINE(request_scope);
    coroutine->internal_entry = frankenphp_request_coroutine_entry;
    coroutine->extended_data = (void *)(uintptr_t)request_id;

    /* Enqueue coroutine for execution */
    ZEND_ASYNC_ENQUEUE_COROUTINE(coroutine);
}

/* ============================================================================
 * 5. Suspend Main Coroutine
 * ============================================================================ */

/*
 * Event methods for the "wait event" that never fires
 * This keeps the main coroutine suspended indefinitely
 */
static bool frankenphp_server_wait_event_start(zend_async_event_t *event) {
    /* Do nothing - event never becomes ready */
    return false;
}

static bool frankenphp_server_wait_event_stop(zend_async_event_t *event) {
    /* Do nothing */
    return false;
}

static bool frankenphp_server_wait_event_dispose(zend_async_event_t *event) {
    efree(event);
    return true;
}

/*
 * Suspends the main coroutine indefinitely
 * This allows the event loop to take over and handle requests
 */
bool frankenphp_suspend_main_coroutine(void)
{
    zend_coroutine_t *coroutine;
    zend_async_event_t *event;

    coroutine = ZEND_ASYNC_CURRENT_COROUTINE;

    /* Create a wait event that never becomes ready */
    event = emalloc(sizeof(zend_async_event_t));
    memset(event, 0, sizeof(zend_async_event_t));

    /* Set event methods (all are stubs that do nothing) */
    event->start = frankenphp_server_wait_event_start;
    event->stop = frankenphp_server_wait_event_stop;
    event->dispose = frankenphp_server_wait_event_dispose;

    /* Create waker and suspend coroutine indefinitely */
    zend_async_waker_new(coroutine);
    zend_async_resume_when(coroutine, event, true, zend_async_waker_callback_resolve, NULL);

    /* This suspends the coroutine - control passes to event loop */
    ZEND_ASYNC_SUSPEND();

    /* We never return from here - event loop runs until shutdown */
    return true;
}
