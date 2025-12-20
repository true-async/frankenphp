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

type asyncWorker struct {
	*worker

	bufferSize int
	rrIndex    atomic.Uint32
}

type asyncWorkerThread struct {
	state  *state.ThreadState
	thread *phpThread
	worker *asyncWorker

	requestMap     sync.Map
	requestCounter atomic.Uint64

	dummyFrankenPHPContext *frankenPHPContext
	dummyContext           context.Context
	isBootingScript        bool

	failureCount int
}

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

func (aw *asyncWorker) handleRequestAsync(ch contextHolder) error {
	metrics.StartWorkerRequest(aw.name)

	start := aw.rrIndex.Add(1) % uint32(len(aw.threads))

	for i := 0; i < len(aw.threads); i++ {
		idx := (start + uint32(i)) % uint32(len(aw.threads))
		thread := aw.threads[idx]

		select {
		case thread.requestChan <- ch:
			if len(thread.requestChan) == 1 && thread.asyncNotifier != nil {
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
	if h.isBootingScript {
		return h.dummyFrankenPHPContext
	}
	return nil
}

func (h *asyncWorkerThread) context() context.Context {
	if h.isBootingScript {
		return h.dummyContext
	}
	return globalCtx
}

func (h *asyncWorkerThread) name() string {
	return "Async Worker Thread: " + h.worker.name
}

//export go_async_worker_get_notification_fd
func go_async_worker_get_notification_fd(threadIndex C.uintptr_t) C.int {
	thread := phpThreads[threadIndex]
	if thread.asyncNotifier == nil {
		return -1
	}
	return C.int(thread.asyncNotifier.GetReadFD())
}

//export go_async_worker_clear_notification
func go_async_worker_clear_notification(threadIndex C.uintptr_t) {
	thread := phpThreads[threadIndex]
	if thread.asyncNotifier != nil {
		thread.asyncNotifier.Clear()
	}
}

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
