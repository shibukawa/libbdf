// Package svgdoc checks the C reader of SVG documents (svg_*.c) against the
// Go package it is a port of, github.com/shibukawa/bdf/image/svg. The C
// sources of the reader are compiled into the package (links to ../../src),
// with reader.c (the allocation helpers) and rmath.c (Go's math) of libbdf.
package svgdoc

/*
#cgo CFLAGS: -I${SRCDIR}/../../include -I${SRCDIR}/../../src -std=c11 -Wall -Wextra
#cgo LDFLAGS: -lm
#include <stdlib.h>
#include "svgport.h"
*/
import "C"

import (
	"unsafe"

	"github.com/shibukawa/bdf/image/svg"
)

func bytesPtr(b []byte) *C.uint8_t {
	if len(b) == 0 {
		return nil
	}
	return (*C.uint8_t)(unsafe.Pointer(&b[0]))
}

func b2i(b bool) C.int {
	if b {
		return 1
	}
	return 0
}

// cDump is the C reader's document of data as text (svgport_dump).
func cDump(data []byte, opts *svg.Options, deep, exact bool) string {
	var o svg.Options
	if opts != nil {
		o = *opts
	}
	var n C.size_t
	p := C.svgport_dump(bytesPtr(data), C.size_t(len(data)), C.int64_t(o.MaxDepth), C.int64_t(o.MaxNodes), C.int64_t(o.MaxStyleMatches), b2i(deep), b2i(exact), &n)
	if p == nil {
		panic("the C reader ran out of memory")
	}
	defer C.free(unsafe.Pointer(p))
	return C.GoStringN(p, C.int(n))
}

// cValues is what the C value functions make of s, as the lines of a deep
// dump.
func cValues(s string, exact bool) string {
	b := []byte(s)
	var n C.size_t
	p := C.svgport_values((*C.char)(unsafe.Pointer(bytesPtr(b))), C.size_t(len(b)), b2i(exact), &n)
	if p == nil {
		panic("the C reader ran out of memory")
	}
	defer C.free(unsafe.Pointer(p))
	return C.GoStringN(p, C.int(n))
}

// cStrings is the C ports of strings.TrimSpace, ToLower and Fields on s.
func cStrings(s string) string {
	b := []byte(s)
	var n C.size_t
	p := C.svgport_strings((*C.char)(unsafe.Pointer(bytesPtr(b))), C.size_t(len(b)), &n)
	if p == nil {
		panic("the C reader ran out of memory")
	}
	defer C.free(unsafe.Pointer(p))
	return C.GoStringN(p, C.int(n))
}

// cFloats is bdfi_parse_float of every string: the bits of the results and
// what was returned (0, 1 for a number out of range, -1 for no number).
func cFloats(in []string) ([]uint64, []int8) {
	var buf []byte
	ends := make([]uint32, len(in))
	for i, s := range in {
		buf = append(buf, s...)
		ends[i] = uint32(len(buf))
	}
	bits, status := make([]uint64, len(in)), make([]int8, len(in))
	if len(in) == 0 {
		return bits, status
	}
	buf = append(buf, 0) // so that there is a first byte
	C.svgport_floats((*C.char)(unsafe.Pointer(&buf[0])), (*C.uint32_t)(unsafe.Pointer(&ends[0])), C.size_t(len(in)),
		(*C.uint64_t)(unsafe.Pointer(&bits[0])), (*C.int8_t)(unsafe.Pointer(&status[0])))
	return bits, status
}

// cRunes is bdfi_svg_to_lower and bdfi_svg_is_space of the characters
// [from, to).
func cRunes(from, to rune) ([]uint32, []uint8) {
	lower, space := make([]uint32, to-from), make([]uint8, to-from)
	C.svgport_runes(C.uint32_t(from), C.uint32_t(to), (*C.uint32_t)(unsafe.Pointer(&lower[0])), (*C.uint8_t)(unsafe.Pointer(&space[0])))
	return lower, space
}

// cMatMul is bdfi_svg_mat_mul.
func cMatMul(m, n svg.Matrix) svg.Matrix {
	var out svg.Matrix
	C.svgport_mat_mul((*C.double)(unsafe.Pointer(&m[0])), (*C.double)(unsafe.Pointer(&n[0])), (*C.double)(unsafe.Pointer(&out[0])))
	return out
}

// cRead reads a document a number of times with the C reader and frees it.
func cRead(data []byte, times int) int {
	return int(C.svgport_read(bytesPtr(data), C.size_t(len(data)), C.int(times)))
}

// cNumbers is bdfi_svg_parse_numbers of s a number of times.
func cNumbers(s string, times int) int {
	b := []byte(s)
	return int(C.svgport_numbers((*C.char)(unsafe.Pointer(bytesPtr(b))), C.size_t(len(b)), C.int(times)))
}
