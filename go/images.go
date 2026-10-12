package cbdf

import (
	"bytes"
	"embed"
	"encoding/binary"
	"hash/crc32"
	"image"
	"image/color"
	"image/gif"
	"image/jpeg"
	"image/png"
	"math/rand/v2"

	"golang.org/x/image/bmp"
)

// The random images the rendering tests draw: encoded as documents hold
// them, in every format the rasterizer decodes and in some it does not.

// randomPicture is a picture with something to interpolate in it and
// something to see through: a gradient, noise, and a transparent corner.
func randomPicture(r *rand.Rand, w, h int) *image.NRGBA {
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	base := [3]int{r.IntN(256), r.IntN(256), r.IntN(256)}
	noise := pickOf(r, []int{0, 0, 8, 64, 256})
	alpha := r.IntN(4) // 0: opaque; 1: a ramp; 2: a hole; 3: noise
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			c := [3]int{base[0] + 255*x/max(w-1, 1), base[1] + 255*y/max(h-1, 1), base[2] + 128*(x^y)/max(w|h, 1)}
			a := 255
			switch alpha {
			case 1:
				a = 255 * (x + y) / max(w+h-2, 1)
			case 2:
				if 2*x < w && 2*y < h {
					a = 0
				}
			case 3:
				a = r.IntN(256)
			}
			var p [3]uint8
			for i := range c {
				if noise > 0 {
					c[i] += r.IntN(noise) - noise/2
				}
				p[i] = uint8(min(max(c[i]%256, 0), 255))
			}
			img.SetNRGBA(x, y, color.NRGBA{p[0], p[1], p[2], uint8(a)})
		}
	}
	return img
}

// asKind converts a picture to one of the image types an encoder writes
// differently: with and without alpha, gray, 16 bits, a palette.
func asKind(r *rand.Rand, src *image.NRGBA) image.Image {
	b := src.Bounds()
	switch r.IntN(7) {
	case 0:
		dst := image.NewRGBA(b)
		for y := b.Min.Y; y < b.Max.Y; y++ {
			for x := b.Min.X; x < b.Max.X; x++ {
				c := src.NRGBAAt(x, y)
				dst.SetRGBA(x, y, color.RGBA{c.R, c.G, c.B, 255})
			}
		}
		return dst
	case 1:
		dst := image.NewGray(b)
		for y := b.Min.Y; y < b.Max.Y; y++ {
			for x := b.Min.X; x < b.Max.X; x++ {
				dst.SetGray(x, y, color.Gray{src.NRGBAAt(x, y).G})
			}
		}
		return dst
	case 2:
		dst := image.NewNRGBA64(b)
		for y := b.Min.Y; y < b.Max.Y; y++ {
			for x := b.Min.X; x < b.Max.X; x++ {
				c := src.NRGBAAt(x, y)
				dst.SetNRGBA64(x, y, color.NRGBA64{uint16(c.R)<<8 | uint16(c.B), uint16(c.G)<<8 | uint16(c.R), uint16(c.B)<<8 | uint16(c.G), uint16(c.A)<<8 | uint16(c.A>>1)})
			}
		}
		return dst
	case 3:
		dst := image.NewGray16(b)
		for y := b.Min.Y; y < b.Max.Y; y++ {
			for x := b.Min.X; x < b.Max.X; x++ {
				c := src.NRGBAAt(x, y)
				dst.SetGray16(x, y, color.Gray16{uint16(c.R)<<8 | uint16(c.G)})
			}
		}
		return dst
	case 4:
		pal := make(color.Palette, 2+r.IntN(30))
		for i := range pal {
			pal[i] = color.NRGBA{uint8(r.IntN(256)), uint8(r.IntN(256)), uint8(r.IntN(256)), uint8(pickOf(r, []int{255, 255, 255, 0, 128}))}
		}
		dst := image.NewPaletted(b, pal)
		for y := b.Min.Y; y < b.Max.Y; y++ {
			for x := b.Min.X; x < b.Max.X; x++ {
				c := src.NRGBAAt(x, y)
				dst.SetColorIndex(x, y, uint8((int(c.R)+int(c.G)/8)%len(pal)))
			}
		}
		return dst
	}
	return src
}

// pngChunk is a chunk of a PNG file.
func pngChunk(typ string, data []byte) []byte {
	b := binary.BigEndian.AppendUint32(nil, uint32(len(data)))
	b = append(b, typ...)
	b = append(b, data...)
	return binary.BigEndian.AppendUint32(b, crc32.ChecksumIEEE(b[4:]))
}

// exif is an EXIF block that says how an image is to be turned.
func exif(orientation int, little bool) []byte {
	var order binary.AppendByteOrder = binary.BigEndian
	b := []byte("MM\x00*")
	if little {
		order, b = binary.LittleEndian, []byte("II*\x00")
	}
	b = order.AppendUint32(b, 8)
	b = order.AppendUint16(b, 2)
	b = order.AppendUint16(b, 0x0100) // a tag before the one looked for
	b = order.AppendUint16(b, 3)
	b = order.AppendUint32(b, 1)
	b = order.AppendUint32(b, 0)
	b = order.AppendUint16(b, 0x0112)
	b = order.AppendUint16(b, 3)
	b = order.AppendUint32(b, 1)
	b = order.AppendUint16(b, uint16(orientation))
	b = order.AppendUint16(b, 0)
	return order.AppendUint32(b, 0)
}

func encodePNG(r *rand.Rand, img image.Image) []byte {
	var buf bytes.Buffer
	enc := png.Encoder{CompressionLevel: pickOf(r, []png.CompressionLevel{png.DefaultCompression, png.NoCompression, png.BestSpeed})}
	if err := enc.Encode(&buf, img); err != nil {
		panic(err)
	}
	b := buf.Bytes()
	if r.IntN(3) == 0 {
		// an eXIf chunk after the header, with an orientation or with one
		// there is not
		e := pngChunk("eXIf", exif(pickOf(r, []int{1, 2, 3, 4, 5, 6, 7, 8, 0, 9}), r.IntN(2) == 0))
		b = append(append(append([]byte(nil), b[:33]...), e...), b[33:]...)
	}
	return b
}

func encodeGIF(r *rand.Rand, img image.Image) []byte {
	var buf bytes.Buffer
	if err := gif.Encode(&buf, img, &gif.Options{NumColors: pickOf(r, []int{2, 5, 16, 64, 256})}); err != nil {
		panic(err)
	}
	return buf.Bytes()
}

func encodeBMP(_ *rand.Rand, img image.Image) []byte {
	var buf bytes.Buffer
	if err := bmp.Encode(&buf, img); err != nil {
		panic(err)
	}
	return buf.Bytes()
}

func encodeJPEG(r *rand.Rand, img image.Image) []byte {
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: pickOf(r, []int{10, 50, 75, 90, 100})}); err != nil {
		panic(err)
	}
	b := buf.Bytes()
	if r.IntN(3) == 0 {
		// an EXIF segment after the start of the image, with an orientation
		// or with one there is not
		e := append([]byte("Exif\x00\x00"), exif(pickOf(r, []int{1, 2, 3, 4, 5, 6, 7, 8, 0, 9}), r.IntN(2) == 0)...)
		seg := binary.BigEndian.AppendUint16([]byte{0xff, 0xe1}, uint16(len(e)+2))
		b = append(append(append([]byte(nil), b[:2]...), append(seg, e...)...), b[2:]...)
	}
	return b
}

// webpImages are the WebP files of the tests (testdata/images, written by
// cwebp from made-up pictures).
//
//go:embed testdata/images/*.webp
var webpImages embed.FS

// encodeWebP gives a WebP file. Go has no encoder of the format, so it is
// one of the files of the tests whatever the picture is: lossy and
// lossless, with alpha and without, and the few that are animations, which
// have a size and no image.
func encodeWebP(r *rand.Rand, _ image.Image) []byte {
	entries, err := webpImages.ReadDir("testdata/images")
	if err != nil || len(entries) == 0 {
		panic("no WebP files in testdata/images")
	}
	// ReadDir gives them sorted by name
	b, err := webpImages.ReadFile("testdata/images/" + entries[r.IntN(len(entries))].Name())
	if err != nil {
		panic(err)
	}
	return b
}

// imageEncoders are the encoders of RandomImage, by the format they write.
var imageEncoders = map[string]func(r *rand.Rand, img image.Image) []byte{
	"png":  encodePNG,
	"jpeg": encodeJPEG,
	"gif":  encodeGIF,
	"bmp":  encodeBMP,
	"webp": encodeWebP,
}

// ImageFormats are the formats RandomImage writes, in order.
func ImageFormats() []string {
	var formats []string
	for f := range imageEncoders {
		formats = append(formats, f)
	}
	// a map has no order
	for i := 1; i < len(formats); i++ {
		for j := i; j > 0 && formats[j] < formats[j-1]; j-- {
			formats[j], formats[j-1] = formats[j-1], formats[j]
		}
	}
	return formats
}

// RandomImage is an image part: mostly an image of one of the formats
// given (all the rasterizer decodes when there are none), small or large
// enough to be drawn from a smaller copy, and now and then bytes that are
// no image, or an image nobody decodes.
func RandomImage(r *rand.Rand, formats ...string) []byte {
	if len(formats) == 0 {
		formats = ImageFormats()
	}
	w, h := 1+r.IntN(40), 1+r.IntN(40)
	switch r.IntN(8) {
	case 0:
		w, h = 100+r.IntN(240), 60+r.IntN(200)
	case 1:
		w, h = pickOf(r, []int{1, 2, 3}), pickOf(r, []int{1, 2, 300})
	}
	b := imageEncoders[pickOf(r, formats)](r, asKind(r, randomPicture(r, w, h)))
	switch r.IntN(24) {
	case 0:
		return b[:len(b)/2] // cut short
	case 1:
		b[len(b)/2] ^= 0x10 // damaged
	case 2:
		return []byte("\x00\x00\x00\x1cftypavif\x00\x00\x00\x00avifmif1miaf")
	case 3:
		return []byte("neither an image nor anything else")
	case 4:
		return nil
	}
	return b
}
