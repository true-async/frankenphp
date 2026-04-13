//go:build windows

package frankenphp

import (
	"fmt"
	"net"
	"syscall"
	"time"
)

// AsyncNotifier provides a way to wake up C event loop from Go goroutines.
// On Windows, uses a TCP loopback socket pair since uv_poll_init (fd-based)
// is not supported — only uv_poll_init_socket works on Windows.
type AsyncNotifier struct {
	readFD   int
	listener net.Listener
	readConn net.Conn
	writeConn net.Conn
	readSocket syscall.Handle
}

// NewAsyncNotifier creates a TCP loopback socket pair for notification.
// The read socket is registered with libuv via uv_poll_init_socket on the C side.
func NewAsyncNotifier() (*AsyncNotifier, error) {
	// Listen on a random loopback port
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, fmt.Errorf("failed to create listener: %w", err)
	}

	addr := listener.Addr().String()

	// Connect write side
	writeConn, err := net.Dial("tcp", addr)
	if err != nil {
		listener.Close()
		return nil, fmt.Errorf("failed to connect write socket: %w", err)
	}

	// Accept read side
	readConn, err := listener.Accept()
	if err != nil {
		writeConn.Close()
		listener.Close()
		return nil, fmt.Errorf("failed to accept read socket: %w", err)
	}

	// Extract the raw socket handle for the read side
	rawConn, err := readConn.(*net.TCPConn).SyscallConn()
	if err != nil {
		readConn.Close()
		writeConn.Close()
		listener.Close()
		return nil, fmt.Errorf("failed to get raw conn: %w", err)
	}

	var readSocket syscall.Handle
	var controlErr error
	err = rawConn.Control(func(fd uintptr) {
		readSocket = syscall.Handle(fd)
	})
	if err != nil {
		controlErr = err
	}
	if controlErr != nil {
		readConn.Close()
		writeConn.Close()
		listener.Close()
		return nil, fmt.Errorf("failed to extract socket handle: %w", controlErr)
	}

	// Return the socket handle as int — this will be passed to C as the socket
	// parameter of ZEND_ASYNC_NEW_POLL_EVENT_EX(0, socket, ASYNC_READABLE, ...)
	return &AsyncNotifier{
		readFD:     int(readSocket),
		listener:   listener,
		readConn:   readConn,
		writeConn:  writeConn,
		readSocket: readSocket,
	}, nil
}

// GetReadFD returns the socket handle for polling (used by C event loop).
// On Windows this is a SOCKET handle passed to uv_poll_init_socket.
func (n *AsyncNotifier) GetReadFD() int {
	return n.readFD
}

// Notify wakes up the event loop by writing a byte to the write socket.
func (n *AsyncNotifier) Notify() error {
	_, err := n.writeConn.Write([]byte{1})
	if err != nil {
		return fmt.Errorf("notify failed: %w", err)
	}
	return nil
}

// Clear drains all pending notification bytes from the read socket.
func (n *AsyncNotifier) Clear() error {
	buf := make([]byte, 64)
	for {
		_ = n.readConn.(*net.TCPConn).SetReadDeadline(immediateDeadline())
		_, err := n.readConn.Read(buf)
		if err != nil {
			// All data drained or timeout — both are fine
			break
		}
	}
	// Reset deadline
	_ = n.readConn.(*net.TCPConn).SetReadDeadline(noDeadline())
	return nil
}

// Close closes all sockets and the listener.
func (n *AsyncNotifier) Close() error {
	var firstErr error
	if n.writeConn != nil {
		if err := n.writeConn.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	if n.readConn != nil {
		if err := n.readConn.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	if n.listener != nil {
		if err := n.listener.Close(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

func immediateDeadline() time.Time {
	return time.Now().Add(1 * time.Millisecond)
}

func noDeadline() time.Time {
	return time.Time{}
}
