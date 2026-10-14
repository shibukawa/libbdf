package cbdf

import (
	"bytes"
	"image"
	"image/draw"
	"image/gif"
	"image/jpeg"
	"image/png"
	"io"

	"github.com/shibukawa/bdf/image/imgconv"
	"golang.org/x/image/bmp"
	"golang.org/x/image/webp"
)

// What the image decoders are compared with: the decoding of the reference.

// maxDecodePixels is imgconv.MaxDecodePixels of the reference.
const maxDecodePixels = imgconv.MaxDecodePixels

type codec struct {
	decode func(io.Reader) (image.Image, error)
	config func(io.Reader) (image.Config, error)
}

// codecs are the decoders imgconv.Decode of the reference uses.
var codecs = map[string]codec{
	"png":  {png.Decode, png.DecodeConfig},
	"jpeg": {jpeg.Decode, jpeg.DecodeConfig},
	"gif":  {gif.Decode, gif.DecodeConfig},
	"bmp":  {bmp.Decode, bmp.DecodeConfig},
	"webp": {webp.Decode, webp.DecodeConfig},
}

// refConfig is what bdfi_<fmt>_config has to return: the size DecodeConfig
// reads, and false where it returns an error or panics.
func refConfig(format string, data []byte) (w, h int, ok bool) {
	defer func() {
		if recover() != nil {
			w, h, ok = 0, 0, false
		}
	}()
	c, err := codecs[format].config(bytes.NewReader(data))
	if err != nil {
		return 0, 0, false
	}
	return c.Width, c.Height, true
}

// reference is what bdfi_<fmt>_decode has to return: the reference
// renderer's decoding of an image part. It is imgconv.Decode (the size
// check on what DecodeConfig reads, then Decode, a panic of either being an
// error) and the conversion of decodePicture in raster/imagebdf/image.go,
// without the EXIF orientation, which is applied afterwards.
func reference(format string, data []byte) (pix []byte, w, h int, status int) {
	defer func() {
		if recover() != nil {
			pix, w, h, status = nil, 0, 0, statusFormat
		}
	}()
	d := codecs[format]
	if format == "jpeg" {
		// a JPEG image is read as far as its first scans
		data, _ = imgconv.LimitJPEGScans(data)
	}
	if c, err := d.config(bytes.NewReader(data)); err == nil {
		if c.Width <= 0 || c.Height <= 0 || c.Width > maxDecodePixels/c.Height {
			return nil, 0, 0, statusTooLarge
		}
	}
	img, err := d.decode(bytes.NewReader(data))
	if err != nil {
		return nil, 0, 0, statusFormat
	}
	b := img.Bounds()
	rgba, ok := img.(*image.RGBA)
	if !ok || b.Min != (image.Point{}) {
		rgba = image.NewRGBA(image.Rect(0, 0, b.Dx(), b.Dy()))
		draw.Draw(rgba, rgba.Bounds(), img, b.Min, draw.Src)
	}
	if rgba.Bounds().Empty() {
		return nil, 0, 0, statusFormat
	}
	w, h = b.Dx(), b.Dy()
	pix = make([]byte, 0, 4*w*h)
	for y := 0; y < h; y++ {
		pix = append(pix, rgba.Pix[y*rgba.Stride:y*rgba.Stride+4*w]...)
	}
	return pix, w, h, statusOK
}
