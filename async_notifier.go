package frankenphp

// #include <unistd.h>
// #include <fcntl.h>
// #ifdef __linux__
// #include <sys/eventfd.h>
// #endif
import "C"
import (
	"fmt"
	"runtime"
	"syscall"
	"unsafe"
)

// AsyncNotifier provides a way to wake up C event loop from Go goroutines
// Uses eventfd on Linux and pipe on other platforms
type AsyncNotifier struct {
	readFD  int
	writeFD int
}

// NewAsyncNotifier creates a new notifier
// On Linux: uses eventfd (most efficient)
// On macOS/BSD: uses pipe
func NewAsyncNotifier() (*AsyncNotifier, error) {
	var readFD, writeFD int

	if runtime.GOOS == "linux" {
		// Try to use eventfd on Linux
		fd, err := createEventFD()
		if err == nil {
			readFD = fd
			writeFD = fd // eventfd uses same FD for read/write
			return &AsyncNotifier{readFD: readFD, writeFD: writeFD}, nil
		}
		// Fall back to pipe if eventfd fails
	}

	// Use pipe for non-Linux or if eventfd failed
	var fds [2]int
	if err := syscall.Pipe(fds[:]); err != nil {
		return nil, fmt.Errorf("failed to create pipe: %w", err)
	}

	readFD = fds[0]
	writeFD = fds[1]

	// Set non-blocking mode on both FDs
	if err := setNonBlocking(readFD); err != nil {
		syscall.Close(readFD)
		syscall.Close(writeFD)
		return nil, err
	}
	if err := setNonBlocking(writeFD); err != nil {
		syscall.Close(readFD)
		syscall.Close(writeFD)
		return nil, err
	}

	return &AsyncNotifier{readFD: readFD, writeFD: writeFD}, nil
}

// createEventFD creates an eventfd on Linux
func createEventFD() (int, error) {
	if runtime.GOOS != "linux" {
		return -1, fmt.Errorf("eventfd not available on %s", runtime.GOOS)
	}

	// EFD_NONBLOCK = 04000 (O_NONBLOCK)
	// EFD_CLOEXEC = 02000000 (O_CLOEXEC)
	const EFD_NONBLOCK = 0x800
	const EFD_CLOEXEC = 0x80000

	fd := C.eventfd(0, C.int(EFD_NONBLOCK|EFD_CLOEXEC))
	if fd < 0 {
		return -1, fmt.Errorf("eventfd() failed")
	}

	return int(fd), nil
}

// setNonBlocking sets O_NONBLOCK flag on a file descriptor
func setNonBlocking(fd int) error {
	flags, err := syscall.FcntlInt(uintptr(fd), syscall.F_GETFL, 0)
	if err != nil {
		return err
	}

	_, err = syscall.FcntlInt(uintptr(fd), syscall.F_SETFL, flags|syscall.O_NONBLOCK)
	return err
}

// GetReadFD returns the file descriptor for reading (used by C event loop)
func (n *AsyncNotifier) GetReadFD() int {
	return n.readFD
}

// Notify wakes up the event loop
// Called from Go goroutines when new request arrives
func (n *AsyncNotifier) Notify() error {
	var buf [8]byte

	if runtime.GOOS == "linux" && n.readFD == n.writeFD {
		// eventfd: write 8-byte integer
		buf[0] = 1
		_, err := syscall.Write(n.writeFD, buf[:8])
		if err != nil && err != syscall.EAGAIN {
			return err
		}
	} else {
		// pipe: write 1 byte
		buf[0] = 1
		_, err := syscall.Write(n.writeFD, buf[:1])
		if err != nil && err != syscall.EAGAIN {
			return err
		}
	}

	return nil
}

// Clear reads and clears the notification
// Called from C event loop after wakeup
func (n *AsyncNotifier) Clear() error {
	var buf [8]byte

	if runtime.GOOS == "linux" && n.readFD == n.writeFD {
		// eventfd: read 8 bytes
		_, err := syscall.Read(n.readFD, buf[:8])
		if err != nil && err != syscall.EAGAIN {
			return err
		}
	} else {
		// pipe: read all available bytes
		for {
			_, err := syscall.Read(n.readFD, buf[:])
			if err == syscall.EAGAIN {
				break
			}
			if err != nil {
				return err
			}
		}
	}

	return nil
}

// Close closes the file descriptors
func (n *AsyncNotifier) Close() error {
	var err error

	if n.readFD == n.writeFD {
		// eventfd: close once
		err = syscall.Close(n.readFD)
	} else {
		// pipe: close both
		err1 := syscall.Close(n.readFD)
		err2 := syscall.Close(n.writeFD)
		if err1 != nil {
			err = err1
		} else if err2 != nil {
			err = err2
		}
	}

	return err
}

//export go_async_clear_notification
func go_async_clear_notification(threadIndex C.uintptr_t) {
	thread := phpThreads[threadIndex]
	if thread.asyncNotifier != nil {
		thread.asyncNotifier.Clear()
	}
}
