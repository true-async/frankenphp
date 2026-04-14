//go:build windows && zend_debug

package frankenphp

// Debug build defines for Windows. Activated via `go build -tags zend_debug`.
// Mirrors what PHP itself sets when configure.bat is invoked with
// --enable-debug (see php-src/win32/build/confutils.js,
// toolset_setup_build_mode -> /D ZEND_DEBUG=1 /U NDebug /U NDEBUG).
//
// ZEND_DEBUG=1 changes the signatures of zend_alloc.h functions
// (_emalloc/_efree/_estrdup/etc. gain ZEND_FILE_LINE_DC arguments) so the
// import library php8ts_debug.lib only resolves when this define is set.
//
// #cgo CFLAGS: -D_WINDOWS -DWINDOWS=1 -DZEND_WIN32=1 -DPHP_WIN32=1 -DWIN32 -D_MBCS -D_USE_MATH_DEFINES -UNDebug -UNDEBUG -DZEND_DEBUG=1 -D_DEBUG -DZTS=1 -DFD_SETSIZE=256
// #cgo LDFLAGS: -lpthreadVC3
import "C"
