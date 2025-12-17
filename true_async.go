//go:build trueasync

package frankenphp

// #cgo trueasync CFLAGS: -DFRANKENPHP_TRUEASYNC -I/usr/local/include/php -I/usr/local/include/php/main -I/usr/local/include/php/TSRM -I/usr/local/include/php/Zend -I/usr/local/include/php/ext
// #cgo trueasync LDFLAGS: -L/usr/local/lib -lphp
// #include "frankenphp.h"
// #include "frankenphp_extension.c"
// #include "frankenphp_trueasync.c"
// #include "Zend/zend_async_API.h"
//
// // FAST PATH: Heartbeat handler called on every coroutine switch
// // Must be SUPER FAST - just non-blocking check of Go channel
// extern __thread uintptr_t frankenphp_current_thread_index;
//
// void frankenphp_async_heartbeat_handler(void) {
//     uint64_t request_id = go_async_check_new_requests(frankenphp_current_thread_index);
//     if (request_id != 0) {
//         frankenphp_handle_request_async(request_id, frankenphp_current_thread_index);
//     }
// }
import "C"
import (
	"context"
	"unsafe"

	"github.com/dunglas/frankenphp/internal/state"
)

// asyncThread represents a PHP thread running in TrueAsync mode
// One thread handles multiple concurrent requests using PHP coroutines
type asyncThread struct {
	thread         *phpThread
	state          *state.ThreadState
	entrypoint     string
	currentRequest *contextHolder // temporary storage for active request during callbacks
}

func convertToAsyncThread(thread *phpThread, entrypoint string) error {
	// Create async notifier (eventfd/pipe)
	notifier, err := NewAsyncNotifier()
	if err != nil {
		return err
	}

	thread.asyncNotifier = notifier
	thread.asyncMode = true

	thread.setHandler(&asyncThread{
		thread:     thread,
		state:      thread.state,
		entrypoint: entrypoint,
	})

	return nil
}

// beforeScriptExecution returns the entrypoint script path
// In async mode, this is called once and the script stays loaded
func (handler *asyncThread) beforeScriptExecution() string {
	switch handler.state.Get() {
	case state.TransitionRequested:
		// Transitioning to another handler
		return handler.thread.transitionToNewHandler()

	case state.TransitionComplete:
		handler.thread.updateContext(false)
		handler.state.Set(state.Ready)
		// Return entrypoint to load
		return handler.entrypoint

	case state.Ready:
		// Entrypoint loaded, now setup TrueAsync and suspend
		return handler.setupAsyncMode()

	case state.ShuttingDown:
		// Signal shutdown
		return ""
	}

	panic("unexpected state: " + handler.state.Name())
}

// setupAsyncMode initializes TrueAsync integration
// Called after entrypoint.php is loaded
func (handler *asyncThread) setupAsyncMode() string {
	threadIndex := C.uintptr_t(handler.thread.threadIndex)

	// 1. Load entrypoint script (registers HttpServer::onRequest callback)
	entrypointPath := C.CString(handler.entrypoint)
	defer C.free(unsafe.Pointer(entrypointPath))

	if C.frankenphp_async_load_entrypoint(entrypointPath) != C.SUCCESS {
		panic("Failed to load TrueAsync entrypoint: " + handler.entrypoint)
	}

	// 2. Register FAST PATH: heartbeat handler for scheduler integration
	//    This is called on EVERY coroutine switch - checks Go channel directly
	C.zend_async_set_heartbeat_handler(C.zend_async_heartbeat_handler_t(C.frankenphp_async_heartbeat_handler))

	// 3. Register SLOW PATH: AsyncNotifier FD with TrueAsync poll events
	//    This wakes event loop when it's idle (no active coroutines)
	notifierFD := C.int(handler.thread.asyncNotifier.GetReadFD())
	if !C.frankenphp_register_async_notifier_event(notifierFD, threadIndex) {
		panic("Failed to register AsyncNotifier with TrueAsync")
	}

	// 4. Activate TrueAsync scheduler
	if !C.frankenphp_activate_true_async() {
		panic("Failed to activate TrueAsync scheduler")
	}

	// 5. Suspend main coroutine indefinitely
	//    Control passes to event loop - never returns
	C.frankenphp_suspend_main_coroutine()

	// Never reached - event loop runs until shutdown
	return ""
}

// afterScriptExecution is called after the entrypoint finishes
// In async mode, this should not normally happen as the event loop runs indefinitely
func (handler *asyncThread) afterScriptExecution(exitStatus int) {
	// In async mode, script should not exit normally
	// If it does, we can restart it or handle the error
}

func (handler *asyncThread) frankenPHPContext() *frankenPHPContext {
	if handler.currentRequest != nil {
		return handler.currentRequest.frankenPHPContext
	}
	return nil
}

func (handler *asyncThread) context() context.Context {
	if handler.currentRequest != nil {
		return handler.currentRequest.ctx
	}
	return globalCtx
}

func (handler *asyncThread) name() string {
	return "TrueAsync PHP Thread"
}
