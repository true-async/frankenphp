package frankenphp

// #include <stdint.h>
// #include <stdlib.h>
import "C"
import (
	"sync"
	"sync/atomic"
)

// Request tracking for async mode
var (
	asyncRequestCounter atomic.Uint64
	asyncRequestMap     sync.Map // map[uint64]contextHolder
)

// CGo exports for C event loop integration

//export go_async_get_notification_fd
func go_async_get_notification_fd(threadIndex C.uintptr_t) C.int {
	thread := phpThreads[threadIndex]
	if thread.asyncNotifier == nil {
		return -1
	}
	return C.int(thread.asyncNotifier.GetReadFD())
}

//export go_async_check_new_requests
func go_async_check_new_requests(threadIndex C.uintptr_t) C.uint64_t {
	thread := phpThreads[threadIndex]

	// Non-blocking read from channel
	select {
	case ch := <-thread.requestChan:
		// Got a new request!
		requestID := asyncRequestCounter.Add(1)

		// Store contextHolder for later retrieval
		asyncRequestMap.Store(requestID, ch)

		return C.uint64_t(requestID)
	default:
		// No requests available
		return 0
	}
}

//export go_async_get_request_script
func go_async_get_request_script(requestID C.uint64_t) *C.char {
	val, ok := asyncRequestMap.Load(uint64(requestID))
	if !ok {
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil {
		return nil
	}

	// Return script filename as C string
	return C.CString(ch.frankenPHPContext.scriptFilename)
}

//export go_async_notify_request_done
func go_async_notify_request_done(requestID C.uint64_t) {
	val, ok := asyncRequestMap.Load(uint64(requestID))
	if !ok {
		return
	}

	ch := val.(contextHolder)

	// Close the done channel to notify Go that request is complete
	if ch.frankenPHPContext != nil {
		ch.frankenPHPContext.closeContext()
	}

	// Remove from map
	asyncRequestMap.Delete(uint64(requestID))
}

//export go_async_get_context_holder
func go_async_get_context_holder(requestID C.uint64_t, threadIndex C.uintptr_t) bool {
	val, ok := asyncRequestMap.Load(uint64(requestID))
	if !ok {
		return false
	}

	ch := val.(contextHolder)
	thread := phpThreads[threadIndex]

	// Temporarily set the context on the thread handler for callbacks
	// This allows go_ub_write and other callbacks to work
	thread.handlerMu.Lock()
	if handler, ok := thread.handler.(*asyncThread); ok {
		handler.currentRequest = &ch
	}
	thread.handlerMu.Unlock()

	return true
}

//export go_async_clear_context_holder
func go_async_clear_context_holder(threadIndex C.uintptr_t) {
	thread := phpThreads[threadIndex]

	thread.handlerMu.Lock()
	if handler, ok := thread.handler.(*asyncThread); ok {
		handler.currentRequest = nil
	}
	thread.handlerMu.Unlock()
}

//export go_async_get_request_method
func go_async_get_request_method(requestID C.uint64_t) *C.char {
	val, ok := asyncRequestMap.Load(uint64(requestID))
	if !ok {
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.env == nil {
		return nil
	}

	method := ch.frankenPHPContext.env["REQUEST_METHOD"]
	if method == "" {
		return C.CString("GET")
	}
	return C.CString(method)
}

//export go_async_get_request_uri
func go_async_get_request_uri(requestID C.uint64_t) *C.char {
	val, ok := asyncRequestMap.Load(uint64(requestID))
	if !ok {
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.env == nil {
		return nil
	}

	uri := ch.frankenPHPContext.env["REQUEST_URI"]
	if uri == "" {
		return C.CString("/")
	}
	return C.CString(uri)
}
