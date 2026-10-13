package cbdf

import (
	"bytes"
	"image"
	"image/png"
	"math/rand/v2"
	"testing"
)

// The speed of the decoders against the reference's, on images of a few
// megapixels: go test -C go -run '^$' -bench Decode .

func benchPicture(noise bool) image.Image {
	r := rand.New(rand.NewPCG(5, 6))
	img := randomPicture(r, 1600, 1200)
	if !noise {
		for i := range img.Pix {
			img.Pix[i] &^= 7
		}
	}
	return img
}

func benchDecode(b *testing.B, format string, data []byte) {
	b.Run("reference", func(b *testing.B) {
		b.SetBytes(1600 * 1200 * 4)
		for b.Loop() {
			if _, _, _, s := reference(format, data); s != statusOK {
				b.Fatal("the reference does not decode the image")
			}
		}
	})
	b.Run("libbdf", func(b *testing.B) {
		b.SetBytes(1600 * 1200 * 4)
		for b.Loop() {
			if _, _, _, s := cDecode(format, data); s != statusOK {
				b.Fatal("libbdf does not decode the image")
			}
		}
	})
}

func BenchmarkDecodePNG(b *testing.B) {
	var buf bytes.Buffer
	if err := png.Encode(&buf, benchPicture(false)); err != nil {
		b.Fatal(err)
	}
	benchDecode(b, "png", buf.Bytes())
}
