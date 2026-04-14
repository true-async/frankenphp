//go:build windows && !zend_debug

package frankenphp

// Release build defines for Windows. Mirrors what PHP itself sets when
// configure.bat is invoked without --enable-debug (see
// php-src/win32/build/confutils.js, toolset_setup_build_mode).
//
// #cgo CFLAGS: -D_WINDOWS -DWINDOWS=1 -DZEND_WIN32=1 -DPHP_WIN32=1 -DWIN32 -D_MBCS -D_USE_MATH_DEFINES -DNDebug -DNDEBUG -DZEND_DEBUG=0 -DZTS=1 -DFD_SETSIZE=256
// #cgo LDFLAGS: -lpthreadVC3
import "C"
