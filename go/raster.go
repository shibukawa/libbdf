package cbdf

/*
#include <stdlib.h>
#include "raster_internal.h"
*/
import "C"

import "unsafe"

// The bindings of this file reach the internals of the rasterizer, which
// the tests check against the reference's raster/imagebdf and against Go's
// math package.

// Math is the functions of src/rmath.c by name.
var Math = map[string]func(float64) float64{
	"sin":  func(x float64) float64 { return float64(C.bdfi_sin(C.double(x))) },
	"cos":  func(x float64) float64 { return float64(C.bdfi_cos(C.double(x))) },
	"tan":  func(x float64) float64 { return float64(C.bdfi_tan(C.double(x))) },
	"atan": func(x float64) float64 { return float64(C.bdfi_atan(C.double(x))) },
	"asin": func(x float64) float64 { return float64(C.bdfi_asin(C.double(x))) },
	"acos": func(x float64) float64 { return float64(C.bdfi_acos(C.double(x))) },
}

// Math2 is the functions of two arguments.
var Math2 = map[string]func(float64, float64) float64{
	"atan2": func(y, x float64) float64 { return float64(C.bdfi_atan2(C.double(y), C.double(x))) },
	"hypot": func(p, q float64) float64 { return float64(C.bdfi_hypot(C.double(p), C.double(q))) },
	"max":   func(x, y float64) float64 { return float64(C.bdfi_fmax(C.double(x), C.double(y))) },
	"min":   func(x, y float64) float64 { return float64(C.bdfi_fmin(C.double(x), C.double(y))) },
	"mod":   func(x, y float64) float64 { return float64(C.bdfi_mod(C.double(x), C.double(y))) },
}

// Sincos is bdfi_sincos.
func Sincos(x float64) (sin, cos float64) {
	var s, c C.double
	C.bdfi_sincos(C.double(x), &s, &c)
	return float64(s), float64(c)
}

// MulAdd is a*b + c as the rasterizer's sources compute it: in two
// operations, whatever the processor offers.
func MulAdd(a, b, c float64) float64 {
	return float64(C.bdfi_r_muladd(C.double(a), C.double(b), C.double(c)))
}

// Image is an image a renderer drew: premultiplied RGBA, 8 bits a channel.
type Image struct {
	W, H, X, Y int
	Pix        []byte
}

func imageOf(img *C.bdf_image) *Image {
	out := &Image{W: int(img.width), H: int(img.height), X: int(img.x), Y: int(img.y)}
	out.Pix = C.GoBytes(unsafe.Pointer(img.pixels), C.int(4*out.W*out.H))
	C.bdf_image_free(img)
	return out
}

// Renderer is a bdf_renderer of a document, which must stay open.
type Renderer struct{ p *C.bdf_renderer }

// NewRenderer makes a renderer; background is 0xRRGGBBAA, used when
// hasBackground.
func (d *Doc) NewRenderer(hasBackground bool, background uint32) (*Renderer, error) {
	var opts C.bdf_render_options
	if hasBackground {
		opts.has_background = 1
		opts.background = C.uint32_t(background)
	}
	var r *C.bdf_renderer
	var e C.bdf_error
	if C.bdf_renderer_new(d.p, &opts, &r, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	return &Renderer{r}, nil
}

// Close frees the renderer.
func (r *Renderer) Close() {
	C.bdf_renderer_free(r.p)
	r.p = nil
}

// Page is bdf_render_page.
func (r *Renderer) Page(view, page int, scale float64) (*Image, error) {
	var img C.bdf_image
	var e C.bdf_error
	if C.bdf_render_page(r.p, C.size_t(view), C.size_t(page), C.double(scale), &img, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	return imageOf(&img), nil
}

// Region is bdf_render_region.
func (r *Renderer) Region(view, page int, region [4]float32, w, h int) (*Image, error) {
	var img C.bdf_image
	var e C.bdf_error
	if C.bdf_render_region(r.p, C.size_t(view), C.size_t(page), (*C.float)(&region[0]), C.int(w), C.int(h), &img, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	return imageOf(&img), nil
}

// Warnings are the warnings of the renderer so far.
func (r *Renderer) Warnings() []string {
	out := make([]string, int(C.bdf_renderer_warning_count(r.p)))
	for i := range out {
		out[i] = C.GoString(C.bdf_renderer_warning(r.p, C.size_t(i)))
	}
	return out
}

// RenderObject is bdf_render_object of an object part.
func RenderObject(data []byte, scale float64, hasBackground bool, background uint32) (*Image, error) {
	o, err := DecodeObject(data)
	if err != nil {
		return nil, err
	}
	defer o.Close()
	var opts C.bdf_render_options
	if hasBackground {
		opts.has_background = 1
		opts.background = C.uint32_t(background)
	}
	var img C.bdf_image
	var e C.bdf_error
	if C.bdf_render_object((*C.bdf_object)(unsafe.Pointer(o.p)), C.double(scale), &opts, &img, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	return imageOf(&img), nil
}
