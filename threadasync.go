package frankenphp

import (
	"context"

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

// setupAsyncMode initializes TrueAsync integration and suspends main coroutine
func (handler *asyncThread) setupAsyncMode() string {
	// This will be called from C via CGO
	// Implementation will be in frankenphp_trueasync.go with build tags
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
