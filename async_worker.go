package frankenphp

// #include "frankenphp.h"
import "C"
import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"unsafe"

	"github.com/dunglas/frankenphp/internal/fastabs"
	"github.com/dunglas/frankenphp/internal/state"
)

var (
	ErrAllBuffersFull = ErrRejected{"all async worker buffers are full", 503}
)

// asyncWorkerThread handles multiple concurrent requests on a single PHP thread.
// Requests are tracked per-thread using sync.Map for zero-contention architecture.
type asyncWorkerThread struct {
	state  *state.ThreadState
	thread *phpThread
	worker *worker

	requestMap     sync.Map
	requestCounter atomic.Uint64

	isBootingScript bool
	failureCount    int
}

// newAsyncWorker creates a new async worker with buffered request channels.
// Returns *worker (not *asyncWorker) for compatibility with worker registry.
func newAsyncWorker(o workerOpt) (*worker, error) {
	absFileName, err := fastabs.FastAbs(o.fileName)
	if err != nil {
		return nil, fmt.Errorf("worker filename is invalid %q: %w", o.fileName, err)
	}

	if _, err := os.Stat(absFileName); err != nil {
		return nil, fmt.Errorf("worker file not found %q: %w", absFileName, err)
	}

	if o.name == "" {
		o.name = absFileName
	}

	allowPathMatching := !strings.HasPrefix(o.name, "m#")

	if w := getWorkerByPath(absFileName); w != nil && allowPathMatching {
		return nil, fmt.Errorf("two workers cannot have the same filename: %q", absFileName)
	}
	if w := getWorkerByName(o.name); w != nil {
		return nil, fmt.Errorf("two workers cannot have the same name: %q", o.name)
	}

	if o.env == nil {
		o.env = make(PreparedEnv, 1)
	}

	o.env["FRANKENPHP_WORKER\x00"] = "1"

	bufferSize := o.bufferSize
	if bufferSize == 0 {
		bufferSize = 20
	}

	w := &worker{
		name:                   o.name,
		fileName:               absFileName,
		requestOptions:         o.requestOptions,
		num:                    o.num,
		maxThreads:             o.maxThreads,
		requestChan:            make(chan contextHolder),
		threads:                make([]*phpThread, 0, o.num),
		allowPathMatching:      allowPathMatching,
		maxConsecutiveFailures: o.maxConsecutiveFailures,
		onThreadReady:          o.onThreadReady,
		onThreadShutdown:       o.onThreadShutdown,
		isAsync:                true,
		bufferSize:             bufferSize,
	}

	w.configureMercure(&o)

	w.requestOptions = append(
		w.requestOptions,
		WithRequestDocumentRoot(filepath.Dir(o.fileName), false),
		WithRequestPreparedEnv(o.env),
	)

	if o.extensionWorkers != nil {
		o.extensionWorkers.internalWorker = w
	}

	return w, nil
}

// convertToAsyncWorkerThread prepares an inactive thread to run an async worker.
// It wires channels/notifier, marks async mode, attaches it to the worker,
// and switches the handler via setHandler so the handler isn't lost during boot.
func convertToAsyncWorkerThread(thread *phpThread, w *worker) {
	thread.requestChan = make(chan contextHolder, w.bufferSize)
	thread.responseChan = make(chan responseWrite, 100)

	notifier, err := NewAsyncNotifier()
	if err != nil {
		panic(fmt.Sprintf("failed to create AsyncNotifier: %v", err))
	}
	thread.asyncNotifier = notifier
	thread.asyncMode = true

	thread.setHandler(&asyncWorkerThread{
		state:           thread.state,
		thread:          thread,
		worker:          w,
		isBootingScript: true,
	})

	w.attachThread(thread)

	go thread.responseDispatcher()
}

// handleRequestAsync dispatches requests using round-robin across worker threads.
// Returns ErrAllBuffersFull if all thread buffers are full.
func (w *worker) handleRequestAsync(ch contextHolder) error {
	metrics.StartWorkerRequest(w.name)

	// A simple Round Robin algorithm for testing
	start := w.rrIndex.Add(1) % uint32(len(w.threads))

	for i := 0; i < len(w.threads); i++ {
		idx := (start + uint32(i)) % uint32(len(w.threads))
		thread := w.threads[idx]

		select {
		case thread.requestChan <- ch:
			if len(thread.requestChan) == 1 && thread.asyncNotifier != nil {
				// If the channel was empty, the thread may be waiting for new messages,
				// paused in the EventLoop, so we must write a new message to the asyncNotifier.
				thread.asyncNotifier.Notify()
			}
			return nil
		default:
			continue
		}
	}

	metrics.StopWorkerRequest(w.name, 0)
	return ErrAllBuffersFull
}

func (h *asyncWorkerThread) beforeScriptExecution() string {
	switch h.state.Get() {
	case state.TransitionRequested:
		h.worker.detachThread(h.thread)
		return h.thread.transitionToNewHandler()

	case state.TransitionComplete:
		h.thread.updateContext(true)
		h.state.Set(state.Ready)
		return h.beforeScriptExecution()

	case state.Ready:
		if h.isBootingScript {
			h.isBootingScript = false
			return h.worker.fileName
		}
		return h.worker.fileName

	case state.ShuttingDown:
		h.worker.detachThread(h.thread)
		return ""
	}

	panic("unexpected state: " + h.state.Name())
}

func (h *asyncWorkerThread) afterScriptExecution(exitStatus int) {
	if exitStatus != 0 {
		h.failureCount++

		maxFailures := h.worker.maxConsecutiveFailures
		if maxFailures == 0 {
			maxFailures = 6
		}

		if maxFailures > 0 && h.failureCount >= maxFailures {
			panic(fmt.Sprintf("async worker %q: too many consecutive failures (%d)", h.worker.name, h.failureCount))
		}
	} else {
		h.failureCount = 0
	}
}

func (h *asyncWorkerThread) frankenPHPContext() *frankenPHPContext {
	return nil
}

func (h *asyncWorkerThread) context() context.Context {
	return globalCtx
}

func (h *asyncWorkerThread) name() string {
	return "Async Worker Thread: " + h.worker.name
}

// go_async_worker_get_notification_fd returns the file descriptor for event loop integration.
// Called from C to get the FD to poll for new request notifications.
//
//export go_async_worker_get_notification_fd
func go_async_worker_get_notification_fd(threadIndex C.uintptr_t) C.int {
	thread := phpThreads[threadIndex]
	if thread.asyncNotifier == nil {
		return -1
	}
	return C.int(thread.asyncNotifier.GetReadFD())
}

// go_async_worker_clear_notification clears the notification after event loop wakeup.
// Called from C after the event loop processes the notification.
//
//export go_async_worker_clear_notification
func go_async_worker_clear_notification(threadIndex C.uintptr_t) {
	thread := phpThreads[threadIndex]
	if thread.asyncNotifier != nil {
		thread.asyncNotifier.Clear()
	}
}

// go_async_worker_check_requests checks for new requests in the thread's channel.
// Returns request ID if a request is available, 0 otherwise (non-blocking).
//
//export go_async_worker_check_requests
func go_async_worker_check_requests(threadIndex C.uintptr_t) C.uint64_t {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return 0
	}

	select {
	case ch := <-thread.requestChan:
		requestID := handler.requestCounter.Add(1)
		handler.requestMap.Store(requestID, ch)
		return C.uint64_t(requestID)
	default:
		return 0
	}
}

// go_async_worker_get_script returns the script filename for a given request ID.
// Returns NULL if request ID is invalid.
//
//export go_async_worker_get_script
func go_async_worker_get_script(threadIndex C.uintptr_t, requestID C.uint64_t) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return nil
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil {
		return nil
	}

	return thread.pinCString(ch.frankenPHPContext.scriptFilename)
}

// go_async_worker_get_context validates and prepares context for a request.
// Returns true if context is valid, false otherwise.
//
//export go_async_worker_get_context
func go_async_worker_get_context(threadIndex C.uintptr_t, requestID C.uint64_t) C.bool {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return C.bool(false)
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		return C.bool(false)
	}

	_ = val.(contextHolder)

	// TODO: Set active context for other CGo callbacks (go_ub_write, go_read_post, etc.)

	return C.bool(true)
}

// go_async_worker_request_done marks a request as complete and cleans up resources.
// Called from C when request processing is finished.
//
//export go_async_worker_request_done
func go_async_worker_request_done(threadIndex C.uintptr_t, requestID C.uint64_t) {}

// go_async_get_request_method returns the HTTP method for a request.
//
//export go_async_get_request_method
func go_async_get_request_method(threadIndex C.uintptr_t, requestID C.uint64_t) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return nil
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil {
		return nil
	}

	return C.CString(ch.frankenPHPContext.request.Method)
}

// go_async_get_request_uri returns the request URI.
//
//export go_async_get_request_uri
func go_async_get_request_uri(threadIndex C.uintptr_t, requestID C.uint64_t) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return nil
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil {
		return nil
	}

	return C.CString(ch.frankenPHPContext.request.RequestURI)
}

// go_async_get_request_header returns a specific request header value.
//
//export go_async_get_request_header
func go_async_get_request_header(threadIndex C.uintptr_t, requestID C.uint64_t, headerName *C.char) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return nil
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil {
		return nil
	}

	name := C.GoString(headerName)
	value := ch.frankenPHPContext.request.Header.Get(name)
	if value != "" {
		return C.CString(value)
	}

	return nil
}

// go_async_get_request_body returns the request body.
// Returns a malloc'd C string that must be freed by the caller.
//
//export go_async_get_request_body
func go_async_get_request_body(threadIndex C.uintptr_t, requestID C.uint64_t, length *C.size_t) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		if length != nil {
			*length = 0
		}
		return C.CString("")
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		if length != nil {
			*length = 0
		}
		return C.CString("")
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil || ch.frankenPHPContext.request.Body == nil {
		if length != nil {
			*length = 0
		}
		return C.CString("")
	}

	body, err := io.ReadAll(ch.frankenPHPContext.request.Body)
	if err != nil {
		if length != nil {
			*length = 0
		}
		return C.CString("")
	}

	if length != nil {
		*length = C.size_t(len(body))
	}
	return C.CString(string(body))
}

// go_async_notify_request_done is an alias for go_async_worker_request_done
// for backward compatibility with frankenphp_extension.c
//
//export go_async_notify_request_done
func go_async_notify_request_done(threadIndex C.uintptr_t, requestID C.uint64_t) {
	go_async_worker_request_done(threadIndex, requestID)
}

// responseDispatcher runs as a permanent goroutine per thread, reading from responseChan
// and spawning a new goroutine for each write operation to prevent blocking the PHP thread.
func (t *phpThread) responseDispatcher() {
	for write := range t.responseChan {
		go t.handleWrite(write)
	}
}

// handleWrite executes the blocking HTTP write in an isolated goroutine.
// The data pointer references a PHP zend_string buffer whose lifetime is managed
// by the C-side pending writes system. Calls back to C via frankenphp_async_write_done
// when the write completes so the zend_string reference can be released.
func (t *phpThread) handleWrite(write responseWrite) {
	handler, ok := t.handler.(*asyncWorkerThread)
	if !ok {
		return
	}

	val, ok := handler.requestMap.Load(write.requestID)
	if !ok {
		return
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.responseWriter == nil {
		return
	}

	ch.frankenPHPContext.responseWriter.Write(
		unsafe.Slice((*byte)(write.data), write.length))

	C.frankenphp_async_write_done(C.uintptr_t(t.threadIndex), C.uint64_t(write.requestID))

	if ch.frankenPHPContext != nil {
		ch.frankenPHPContext.closeContext()
	}
	handler.requestMap.Delete(write.requestID)
}

// go_async_response_write queues response data for asynchronous writing.
// The data pointer references a PHP zend_string buffer whose lifetime is managed
// by the C-side pending writes system. This function returns immediately without
// blocking, allowing PHP coroutines to continue execution while the actual HTTP
// write happens in a separate goroutine.
//
//export go_async_response_write
func go_async_response_write(threadIndex C.uintptr_t, requestID C.uint64_t, data unsafe.Pointer, length C.size_t) {
	thread := phpThreads[threadIndex]

	thread.responseChan <- responseWrite{
		requestID: uint64(requestID),
		data:      data,
		length:    int(length),
	}

}
