package frankenphp

// #cgo darwin pkg-config: libxml-2.0
// #cgo unix CFLAGS: -Wall -Werror
// #cgo linux CFLAGS: -D_GNU_SOURCE
// #cgo unix LDFLAGS: -lphp -lm -lutil
// #cgo linux LDFLAGS: -ldl -lresolv
// #cgo darwin LDFLAGS: -Wl,-rpath,/usr/local/lib -liconv -ldl
//
// Windows CFLAGS/LDFLAGS live in cgo_windows.go (release defaults) and
// cgo_windows_debug.go (build tag `zend_debug`). They are split so that
// when linking against a debug PHP devel pack we can flip ZEND_DEBUG=1,
// which changes the signatures of zend_alloc.h functions.
import "C"
