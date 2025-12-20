package frankenphp

// #include "frankenphp.h"
import "C"
import (
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/dunglas/frankenphp/internal/fastabs"
	"github.com/dunglas/frankenphp/internal/state"
)

var (
	ErrAllBuffersFull = ErrRejected{"all async worker buffers are full", 503}
)

// asyncWorker extends worker with async request handling capabilities.
// Uses buffered channels and round-robin dispatch to distribute requests across threads.
type asyncWorker struct {
	*worker

	bufferSize int
	rrIndex    atomic.Uint32
}

// asyncWorkerThread handles multiple concurrent requests on a single PHP thread.
// Requests are tracked per-thread using sync.Map for zero-contention architecture.
type asyncWorkerThread struct {
	state  *state.ThreadState
	thread *phpThread
	worker *asyncWorker

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

	baseWorker := &worker{
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
	}

	baseWorker.configureMercure(&o)

	baseWorker.requestOptions = append(
		baseWorker.requestOptions,
		WithRequestDocumentRoot(filepath.Dir(o.fileName), false),
		WithRequestPreparedEnv(o.env),
	)

	if o.extensionWorkers != nil {
		o.extensionWorkers.internalWorker = baseWorker
	}

	asyncW := &asyncWorker{
		worker:     baseWorker,
		bufferSize: bufferSize,
	}

	return asyncW, nil
}

// initThreads initializes all threads for this async worker.
// Called during worker startup to prepare thread pool.
func (aw *asyncWorker) initThreads(workersReady *sync.WaitGroup) {
	for i := 0; i < aw.num; i++ {
		thread := getInactivePHPThread()

		thread.requestChan = make(chan contextHolder, aw.bufferSize)
		thread.asyncNotifier = NewAsyncNotifier()
		thread.asyncMode = true
		thread.handler = &asyncWorkerThread{
			state:           thread.state,
			thread:          thread,
			worker:          aw,
			isBootingScript: true,
		}

		aw.attachThread(thread)

		workersReady.Go(func() {
			thread.state.Set(state.BootRequested)
			thread.boot()
			thread.state.WaitFor(state.Ready, state.ShuttingDown, state.Done)
		})
	}
}

// handleRequestAsync dispatches requests using round-robin across worker threads.
// Returns ErrAllBuffersFull if all thread buffers are full.
func (aw *asyncWorker) handleRequestAsync(ch contextHolder) error {
	metrics.StartWorkerRequest(aw.name)

    // A simple Round Robin algorithm for testing
	start := aw.rrIndex.Add(1) % uint32(len(aw.threads))

	for i := 0; i < len(aw.threads); i++ {
		idx := (start + uint32(i)) % uint32(len(aw.threads))
		thread := aw.threads[idx]

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

	metrics.StopWorkerRequest(aw.name, 0)
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
func go_async_worker_request_done(threadIndex C.uintptr_t, requestID C.uint64_t) {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		return
	}

	ch := val.(contextHolder)

	if ch.frankenPHPContext != nil {
		ch.frankenPHPContext.closeContext()
	}

	handler.requestMap.Delete(uint64(requestID))
}
