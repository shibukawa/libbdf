package cbdf

/*
#include <stdlib.h>
#include "image_internal.h"
#include "raster_internal.h"
*/
import "C"

import "unsafe"

// The bindings of this file reach the image decoders of the rasterizer,
// which the tests check against the decoders of the reference: Go's
// image/png, image/jpeg and image/gif and golang.org/x/image's bmp and webp.

// The statuses a decoder returns.
const (
	statusOK       = int(C.BDF_OK)
	statusFormat   = int(C.BDF_ERR_FORMAT)
	statusNoMem    = int(C.BDF_ERR_NOMEM)
	statusTooLarge = int(C.BDF_ERR_TOO_LARGE)
)

// cConfig reads the size of an image with the decoder of a format.
func cConfig(format string, data []byte) (w, h int, ok bool) {
	var p *C.uint8_t
	if len(data) > 0 {
		p = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	var cw, ch C.int32_t
	var r C.int
	switch format {
	case "png":
		r = C.bdfi_png_config(p, C.size_t(len(data)), &cw, &ch)
	case "jpeg":
		r = C.bdfi_jpeg_config(p, C.size_t(len(data)), &cw, &ch)
	case "gif":
		r = C.bdfi_gif_config(p, C.size_t(len(data)), &cw, &ch)
	case "bmp":
		r = C.bdfi_bmp_config(p, C.size_t(len(data)), &cw, &ch)
	case "webp":
		r = C.bdfi_webp_config(p, C.size_t(len(data)), &cw, &ch)
	default:
		panic("no decoder for " + format)
	}
	if r == 0 {
		return 0, 0, false
	}
	return int(cw), int(ch), true
}

// cDecode decodes an image with the decoder of a format: premultiplied RGBA
// pixels, 4*w bytes a row, or a status that is not statusOK.
func cDecode(format string, data []byte) (pix []byte, w, h int, status int) {
	var p *C.uint8_t
	if len(data) > 0 {
		p = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	var bm C.bdfi_bitmap
	var st C.bdf_status
	switch format {
	case "png":
		st = C.bdfi_png_decode(p, C.size_t(len(data)), &bm)
	case "jpeg":
		st = C.bdfi_jpeg_decode(p, C.size_t(len(data)), &bm)
	case "gif":
		st = C.bdfi_gif_decode(p, C.size_t(len(data)), &bm)
	case "bmp":
		st = C.bdfi_bmp_decode(p, C.size_t(len(data)), &bm)
	case "webp":
		st = C.bdfi_webp_decode(p, C.size_t(len(data)), &bm)
	default:
		panic("no decoder for " + format)
	}
	if st != C.BDF_OK {
		if bm.pix != nil || bm.w != 0 || bm.h != 0 {
			panic("a failed decode left a bitmap")
		}
		return nil, 0, 0, int(st)
	}
	defer C.free(unsafe.Pointer(bm.pix))
	w, h = int(bm.w), int(bm.h)
	if w <= 0 || h <= 0 || bm.pix == nil {
		panic("a decode without an error and without an image")
	}
	return C.GoBytes(unsafe.Pointer(bm.pix), C.int(4*w*h)), w, h, statusOK
}

// Orientation is the EXIF orientation libbdf reads from an image.
func Orientation(data []byte) int {
	var p *C.uint8_t
	if len(data) > 0 {
		p = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	return int(C.bdfi_image_orientation(p, C.size_t(len(data))))
}

// Sniff is the format libbdf takes image bytes for, by the reference's name.
func Sniff(data []byte) string {
	var p *C.uint8_t
	if len(data) > 0 {
		p = (*C.uint8_t)(unsafe.Pointer(&data[0]))
	}
	return C.GoString(C.bdfi_image_name(C.bdfi_image_sniff(p, C.size_t(len(data)))))
}
