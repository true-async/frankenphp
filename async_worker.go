package frankenphp

// #include "frankenphp.h"
import "C"
import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"
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

	if w := workersByPath[absFileName]; w != nil && allowPathMatching {
		return nil, fmt.Errorf("two workers cannot have the same filename: %q", absFileName)
	}
	if w := workersByName[o.name]; w != nil {
		return nil, fmt.Errorf("two workers cannot have the same name: %q", o.name)
	}

	if o.env == nil {
		o.env = make(PreparedEnv, 1)
	}

	o.env["FRANKENPHP_WORKER\x00"] = "1"

	bufferSize := o.bufferSize
	if bufferSize == 0 {
		bufferSize = 1
	} else if bufferSize > 10 {
		bufferSize = 10
	}

	drainTimeout := o.drainTimeout
	if drainTimeout == 0 {
		drainTimeout = 30 * time.Second
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
		drainTimeout:           drainTimeout,
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
			if thread.asyncNotifier != nil && !thread.notified.Swap(true) {
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
			if globalLogger.Enabled(globalCtx, slog.LevelInfo) {
				globalLogger.LogAttrs(globalCtx, slog.LevelInfo, "async worker thread started", slog.String("worker", h.worker.name), slog.Int("thread", h.thread.threadIndex))
			}
			h.isBootingScript = false
			return h.worker.fileName
		}
		return h.worker.fileName

	case state.ShuttingDown:
		if globalLogger.Enabled(globalCtx, slog.LevelInfo) {
			globalLogger.LogAttrs(globalCtx, slog.LevelInfo, "async worker thread stopping", slog.String("worker", h.worker.name), slog.Int("thread", h.thread.threadIndex))
		}
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

// isDrained returns true if there are no buffered or in-flight requests on this thread.
func (h *asyncWorkerThread) isDrained() bool {
	if len(h.thread.requestChan) > 0 {
		return false
	}
	drained := true
	h.requestMap.Range(func(_, _ any) bool {
		drained = false
		return false
	})
	return drained
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

func (h *asyncWorkerThread) drain() {}

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
	const shutdownRequestID = ^uint64(0)

	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return 0
	}

	select {
	case ch := <-thread.requestChan:
		// sentinel for shutdown: empty context signals async loop to stop
		if ch.frankenPHPContext == nil {
			return C.uint64_t(shutdownRequestID)
		}

		requestID := handler.requestCounter.Add(1)
		handler.requestMap.Store(requestID, ch)
		return C.uint64_t(requestID)
	default:
		// Channel is empty — reset notified flag so next send will Notify
		thread.notified.Store(false)
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

	handler.requestMap.Delete(uint64(requestID))

	ch := val.(contextHolder)
	if ch.frankenPHPContext != nil {
		ch.frankenPHPContext.closeContext()
	}
}

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

// go_async_get_request_host returns the Host header value.
//
//export go_async_get_request_host
func go_async_get_request_host(threadIndex C.uintptr_t, requestID C.uint64_t) *C.char {
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
	host := ch.frankenPHPContext.request.Host
	if host == "" {
		return nil
	}
	return C.CString(host)
}

// go_async_get_request_remote_addr returns the client remote address (IP:port).
//
//export go_async_get_request_remote_addr
func go_async_get_request_remote_addr(threadIndex C.uintptr_t, requestID C.uint64_t) *C.char {
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
	addr := ch.frankenPHPContext.request.RemoteAddr
	if addr == "" {
		return nil
	}
	return C.CString(addr)
}

// go_async_get_request_proto returns the HTTP protocol version (e.g. "HTTP/1.1").
//
//export go_async_get_request_proto
func go_async_get_request_proto(threadIndex C.uintptr_t, requestID C.uint64_t) *C.char {
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
	return C.CString(ch.frankenPHPContext.request.Proto)
}

// go_async_get_request_is_tls returns whether the request uses TLS.
//
//export go_async_get_request_is_tls
func go_async_get_request_is_tls(threadIndex C.uintptr_t, requestID C.uint64_t) C.bool {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		return C.bool(false)
	}
	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		return C.bool(false)
	}
	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil {
		return C.bool(false)
	}
	return C.bool(ch.frankenPHPContext.request.TLS != nil)
}

// go_async_get_parsed_body returns form fields as "name\0value\0" pairs.
// Handles both application/x-www-form-urlencoded and multipart/form-data.
//
//export go_async_get_parsed_body
func go_async_get_parsed_body(threadIndex C.uintptr_t, requestID C.uint64_t, length *C.size_t) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		*length = 0
		return nil
	}
	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		*length = 0
		return nil
	}
	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil {
		*length = 0
		return nil
	}

	r := ch.frankenPHPContext.request
	ct := r.Header.Get("Content-Type")

	if strings.HasPrefix(ct, "multipart/form-data") {
		if err := r.ParseMultipartForm(32 << 20); err != nil {
			*length = 0
			return nil
		}
	} else {
		if err := r.ParseForm(); err != nil {
			*length = 0
			return nil
		}
	}

	var buf []byte
	if r.PostForm != nil {
		for name, values := range r.PostForm {
			for _, v := range values {
				buf = append(buf, name...)
				buf = append(buf, 0)
				buf = append(buf, v...)
				buf = append(buf, 0)
			}
		}
	}

	if len(buf) == 0 {
		*length = 0
		return nil
	}

	*length = C.size_t(len(buf))
	return (*C.char)(C.CBytes(buf))
}

// uploadedFileInfo represents metadata for a single uploaded file.
type uploadedFileInfo struct {
	FieldName string `json:"field_name"`
	FileName  string `json:"file_name"`
	Size      int64  `json:"size"`
	MimeType  string `json:"mime_type"`
	TmpName   string `json:"tmp_name"`
	Error     int    `json:"error"`
}

// go_async_get_uploaded_files parses multipart uploads, saves files to temp dir,
// returns JSON array of file metadata.
//
//export go_async_get_uploaded_files
func go_async_get_uploaded_files(threadIndex C.uintptr_t, requestID C.uint64_t, length *C.size_t) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		*length = 0
		return nil
	}
	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		*length = 0
		return nil
	}
	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil {
		*length = 0
		return nil
	}

	r := ch.frankenPHPContext.request
	ct := r.Header.Get("Content-Type")
	if !strings.HasPrefix(ct, "multipart/form-data") {
		*length = 0
		return nil
	}

	if err := r.ParseMultipartForm(32 << 20); err != nil {
		*length = 0
		return nil
	}

	if r.MultipartForm == nil || len(r.MultipartForm.File) == 0 {
		*length = 0
		return nil
	}

	var files []uploadedFileInfo
	for fieldName, fileHeaders := range r.MultipartForm.File {
		for _, fh := range fileHeaders {
			info := uploadedFileInfo{
				FieldName: fieldName,
				FileName:  fh.Filename,
				Size:      fh.Size,
				MimeType:  fh.Header.Get("Content-Type"),
				Error:     0, // UPLOAD_ERR_OK
			}

			// Save to temp file
			src, err := fh.Open()
			if err != nil {
				info.Error = 6 // UPLOAD_ERR_NO_TMP_DIR
				files = append(files, info)
				continue
			}

			tmpFile, err := os.CreateTemp("", "frankenphp_upload_*")
			if err != nil {
				src.Close()
				info.Error = 6
				files = append(files, info)
				continue
			}

			written, err := io.Copy(tmpFile, src)
			src.Close()
			tmpFile.Close()

			if err != nil {
				os.Remove(tmpFile.Name())
				info.Error = 3 // UPLOAD_ERR_PARTIAL
				files = append(files, info)
				continue
			}

			info.TmpName = tmpFile.Name()
			info.Size = written
			files = append(files, info)
		}
	}

	if len(files) == 0 {
		*length = 0
		return nil
	}

	jsonData, err := json.Marshal(files)
	if err != nil {
		*length = 0
		return nil
	}

	*length = C.size_t(len(jsonData))
	return (*C.char)(C.CBytes(jsonData))
}

// go_async_get_all_request_headers returns all request headers serialized as "name\0value\0" pairs.
// Returns a malloc'd buffer that must be freed by the caller.
//
//export go_async_get_all_request_headers
func go_async_get_all_request_headers(threadIndex C.uintptr_t, requestID C.uint64_t, length *C.size_t) *C.char {
	thread := phpThreads[threadIndex]
	handler, ok := thread.handler.(*asyncWorkerThread)
	if !ok {
		*length = 0
		return nil
	}

	val, ok := handler.requestMap.Load(uint64(requestID))
	if !ok {
		*length = 0
		return nil
	}

	ch := val.(contextHolder)
	if ch.frankenPHPContext == nil || ch.frankenPHPContext.request == nil {
		*length = 0
		return nil
	}

	var buf []byte
	for name, values := range ch.frankenPHPContext.request.Header {
		for _, v := range values {
			buf = append(buf, name...)
			buf = append(buf, 0)
			buf = append(buf, v...)
			buf = append(buf, 0)
		}
	}

	if len(buf) == 0 {
		*length = 0
		return nil
	}

	*length = C.size_t(len(buf))
	cBuf := C.CBytes(buf)
	return (*C.char)(cBuf)
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
// For complete responses (statusCode > 0), it sets headers and status before writing body.
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

	rw := ch.frankenPHPContext.responseWriter

	if write.statusCode > 0 {
		parseSerializedHeaders(rw, write.headers)
		rw.WriteHeader(write.statusCode)
	}

	if len(write.data) > 0 {
		rw.Write(write.data)
	}

	go_async_worker_request_done(C.uintptr_t(t.threadIndex), C.uint64_t(write.requestID))
}

// parseSerializedHeaders parses null-separated "name\0value\0" pairs and sets them on the ResponseWriter.
func parseSerializedHeaders(rw http.ResponseWriter, data []byte) {
	if len(data) == 0 {
		return
	}

	h := rw.Header()
	i := 0
	for i < len(data) {
		nameEnd := i
		for nameEnd < len(data) && data[nameEnd] != 0 {
			nameEnd++
		}
		if nameEnd >= len(data) {
			break
		}
		name := string(data[i:nameEnd])

		valueStart := nameEnd + 1
		valueEnd := valueStart
		for valueEnd < len(data) && data[valueEnd] != 0 {
			valueEnd++
		}
		value := string(data[valueStart:valueEnd])

		h.Add(name, value)
		i = valueEnd + 1
	}
}

// go_async_response_complete queues a full response (status + headers + body) for asynchronous writing.
// Headers are serialized as null-separated "name\0value\0" pairs.
//
//export go_async_response_complete
func go_async_response_complete(threadIndex C.uintptr_t, requestID C.uint64_t, statusCode C.int, headersData unsafe.Pointer, headersLen C.size_t, bodyData unsafe.Pointer, bodyLen C.size_t) {
	thread := phpThreads[threadIndex]

	var bodyCopy []byte
	if int(bodyLen) > 0 {
		bodyCopy = make([]byte, int(bodyLen))
		copy(bodyCopy, unsafe.Slice((*byte)(bodyData), int(bodyLen)))
	}

	var headersCopy []byte
	if int(headersLen) > 0 {
		headersCopy = make([]byte, int(headersLen))
		copy(headersCopy, unsafe.Slice((*byte)(headersData), int(headersLen)))
	}

	thread.responseChan <- responseWrite{
		requestID:  uint64(requestID),
		statusCode: int(statusCode),
		headers:    headersCopy,
		data:       bodyCopy,
	}
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

	dataCopy := make([]byte, int(length))
	copy(dataCopy, unsafe.Slice((*byte)(data), int(length)))

	thread.responseChan <- responseWrite{
		requestID: uint64(requestID),
		data:      dataCopy,
	}
}
