package cbdf

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"testing"
)

// pngSpec is a PNG image to be written: any size, colour type, bit depth
// and interlacing, with rows of random bytes behind random filters, which
// is as good an image as any to a decoder.
type pngSpec struct {
	w, h      int
	depth     int
	colorType int
	interlace bool
	palette   int // entries of PLTE (colour type 3, or a suggestion for 2 and 6)
	trns      int // bytes of tRNS, -1 for none
	level     int // of zlib
	split     int // the most bytes of an IDAT chunk, 0 for one chunk
	extra     []string
}

func (s pngSpec) String() string {
	return fmt.Sprintf("%dx%d depth %d type %d interlace %v palette %d trns %d level %d split %d %v",
		s.w, s.h, s.depth, s.colorType, s.interlace, s.palette, s.trns, s.level, s.split, s.extra)
}

var pngPasses = [7][4]int{{8, 8, 0, 0}, {8, 8, 4, 0}, {4, 8, 0, 4}, {4, 4, 2, 0}, {2, 4, 0, 2}, {2, 2, 1, 0}, {1, 2, 0, 1}}

// raw returns the filtered rows of the image: random bytes, with the
// colours a tRNS chunk names now and then, and sometimes a filter type
// there is not.
func (s pngSpec) raw(rng *rand.Rand, trns []byte) []byte {
	channels := map[int]int{0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[s.colorType]
	if channels == 0 {
		channels = 1 // a colour type there is not: any rows will do
	}
	bits := channels * max(s.depth, 1)
	var out []byte
	row := func(w int) {
		f := byte(rng.Intn(5))
		if rng.Intn(4000) == 0 {
			f = byte(5 + rng.Intn(250))
		}
		n := (bits*w + 7) / 8
		b := make([]byte, n)
		switch rng.Intn(4) {
		case 0:
			// smooth: the filters then make something of their neighbours
			v := byte(rng.Intn(256))
			for i := range b {
				v += byte(rng.Intn(5) - 2)
				b[i] = v
			}
		case 1:
			for i := range b {
				b[i] = []byte{0, 0xff, 0x80, 1}[rng.Intn(4)]
			}
		default:
			rng.Read(b)
		}
		// the transparent colour, where there is one
		if len(trns) > 0 && s.colorType != 3 && s.depth >= 8 && rng.Intn(3) == 0 {
			px := bits / 8
			for i := 0; i+px <= n; i += px {
				if rng.Intn(3) != 0 {
					continue
				}
				for c := 0; c < channels && 2*c+1 < len(trns); c++ {
					if s.depth == 16 {
						b[i+2*c], b[i+2*c+1] = trns[2*c], trns[2*c+1]
					} else {
						b[i+c] = trns[2*c+1]
					}
				}
			}
		}
		out = append(out, f)
		out = append(out, b...)
	}
	if !s.interlace {
		for y := 0; y < s.h; y++ {
			row(s.w)
		}
		return out
	}
	for _, p := range pngPasses {
		w, h := (s.w-p[2]+p[0]-1)/p[0], (s.h-p[3]+p[1]-1)/p[1]
		if w == 0 || h == 0 {
			continue
		}
		for y := 0; y < h; y++ {
			row(w)
		}
	}
	return out
}

// encode writes the image. tweak, when it is not nil, may change the raw
// rows and the compressed stream on their way.
func (s pngSpec) encode(rng *rand.Rand, tweak func(what string, b []byte) []byte) []byte {
	if tweak == nil {
		tweak = func(_ string, b []byte) []byte { return b }
	}
	out := []byte("\x89PNG\r\n\x1a\n")
	ihdr := binary.BigEndian.AppendUint32(nil, uint32(s.w))
	ihdr = binary.BigEndian.AppendUint32(ihdr, uint32(s.h))
	il := byte(0)
	if s.interlace {
		il = 1
	}
	ihdr = append(ihdr, byte(s.depth), byte(s.colorType), 0, 0, il)
	out = append(out, pngChunk("IHDR", ihdr)...)
	for _, e := range s.extra {
		if e == "before-plte" {
			out = append(out, pngChunk("gAMA", []byte{0, 0, 0xb1, 0x8f})...)
		}
	}
	if s.palette > 0 {
		pal := make([]byte, 3*s.palette)
		rng.Read(pal)
		out = append(out, pngChunk("PLTE", pal)...)
	}
	var trns []byte
	if s.trns >= 0 {
		trns = make([]byte, s.trns)
		rng.Read(trns)
		if s.colorType != 3 && s.depth < 16 {
			for i := 0; i+1 < len(trns); i += 2 {
				trns[i] = 0
				if s.depth < 8 {
					trns[i+1] &= 1<<s.depth - 1
				}
			}
		}
		out = append(out, pngChunk("tRNS", trns)...)
	}
	for _, e := range s.extra {
		if e == "text" {
			out = append(out, pngChunk("tEXt", []byte("Comment\x00made for a test"))...)
		}
	}
	raw := tweak("raw", s.raw(rng, trns))
	var z bytes.Buffer
	zw, _ := zlib.NewWriterLevel(&z, s.level)
	// several writes with flushes between them make several blocks, and
	// stored ones at level 0
	for len(raw) > 0 {
		n := len(raw)
		if rng.Intn(3) == 0 {
			n = 1 + rng.Intn(len(raw))
		}
		zw.Write(raw[:n])
		raw = raw[n:]
		if rng.Intn(2) == 0 {
			zw.Flush()
		}
	}
	zw.Close()
	data := tweak("zlib", z.Bytes())
	if s.split <= 0 {
		out = append(out, pngChunk("IDAT", data)...)
	} else {
		for first := true; first || len(data) > 0; first = false {
			n := min(len(data), 1+rng.Intn(s.split))
			if rng.Intn(20) == 0 {
				n = 0 // an empty chunk among them
			}
			out = append(out, pngChunk("IDAT", data[:n])...)
			data = data[n:]
		}
	}
	for _, e := range s.extra {
		switch e {
		case "idat-after":
			out = append(out, pngChunk("IDAT", []byte("left over"))...)
		case "text-after":
			out = append(out, pngChunk("tIME", []byte{7, 0xe8, 1, 2, 3, 4, 5})...)
		}
	}
	return append(out, pngChunk("IEND", nil)...)
}

// randomPNG picks an image: mostly one a decoder takes, sometimes one with
// a combination it has to refuse.
func randomPNG(rng *rand.Rand) pngSpec {
	valid := [][2]int{{0, 1}, {0, 2}, {0, 4}, {0, 8}, {0, 16}, {2, 8}, {2, 16}, {3, 1}, {3, 2}, {3, 4}, {3, 8}, {4, 8}, {4, 16}, {6, 8}, {6, 16}}
	cd := valid[rng.Intn(len(valid))]
	s := pngSpec{colorType: cd[0], depth: cd[1], trns: -1}
	sizes := []int{1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 33, 64, 100}
	s.w, s.h = sizes[rng.Intn(len(sizes))], sizes[rng.Intn(len(sizes))]
	if rng.Intn(30) == 0 {
		s.w = 200 + rng.Intn(1200) // rows longer than a buffer
	}
	if rng.Intn(30) == 0 {
		s.h = 200 + rng.Intn(600)
	}
	s.interlace = rng.Intn(3) == 0
	s.level = []int{zlib.NoCompression, zlib.BestSpeed, zlib.DefaultCompression, zlib.BestCompression, zlib.HuffmanOnly}[rng.Intn(5)]
	if rng.Intn(2) == 0 {
		s.split = []int{1, 2, 7, 100, 4095, 4096, 4097, 5000, 40000}[rng.Intn(9)]
	}
	switch s.colorType {
	case 3:
		s.palette = 1 + rng.Intn(1<<s.depth)
		if rng.Intn(3) == 0 {
			s.trns = rng.Intn(s.palette + 1)
			if rng.Intn(6) == 0 {
				s.trns = rng.Intn(257) // more of them than colours
			}
		}
	case 0:
		if rng.Intn(3) == 0 {
			s.trns = 2
		}
	case 2:
		if rng.Intn(3) == 0 {
			s.trns = 6
		}
		if rng.Intn(8) == 0 {
			s.palette = 1 + rng.Intn(256)
		}
	case 6:
		if rng.Intn(8) == 0 {
			s.palette = 1 + rng.Intn(256)
		}
	}
	for _, e := range []string{"before-plte", "text", "idat-after", "text-after"} {
		if rng.Intn(6) == 0 {
			s.extra = append(s.extra, e)
		}
	}
	// what has to be refused
	switch rng.Intn(40) {
	case 0:
		s.depth = []int{0, 3, 5, 8, 16, 32}[rng.Intn(6)]
	case 1:
		s.colorType = []int{1, 5, 7}[rng.Intn(3)]
	case 2:
		s.trns = rng.Intn(9)
	case 3:
		s.palette = rng.Intn(300)
	}
	return s
}

// generatedPNGs are images of every kind, by seed.
func generatedPNGs(seed int64, n int) map[string][]byte {
	rng := rand.New(rand.NewSource(seed))
	out := map[string][]byte{}
	for i := 0; i < n; i++ {
		s := randomPNG(rng)
		var tweak func(string, []byte) []byte
		switch rng.Intn(12) {
		case 0:
			// bytes after the end of the compressed stream, in its chunk
			extra := make([]byte, 1+rng.Intn(9000))
			tweak = func(what string, b []byte) []byte {
				if what == "zlib" {
					return append(append([]byte(nil), b...), extra...)
				}
				return b
			}
		case 1:
			// a row too many or too few
			d := rng.Intn(40) - 20
			tweak = func(what string, b []byte) []byte {
				if what != "raw" {
					return b
				}
				if d < 0 {
					return b[:max(0, len(b)+d)]
				}
				return append(append([]byte(nil), b...), make([]byte, d)...)
			}
		case 2:
			// a damaged checksum of the stream
			tweak = func(what string, b []byte) []byte {
				if what == "zlib" && len(b) > 0 {
					b = append([]byte(nil), b...)
					b[len(b)-1-rng.Intn(min(len(b), 4))] ^= 1 << rng.Intn(8)
				}
				return b
			}
		}
		out[fmt.Sprintf("generated %d/%d: %s", seed, i, s)] = s.encode(rng, tweak)
	}
	return out
}

// pngFiles are the PNG images of the test documents and of the testdata of
// Go's image packages.
func pngFiles(t testing.TB) map[string][]byte {
	in := files([]string{goroot() + "/image/png/testdata", goroot() + "/image/testdata", ximage(t) + "/testdata"}, ".png")
	for k, v := range documentImages(t, "png") {
		in[k] = v
	}
	return in
}

func TestPNGFiles(t *testing.T) {
	checkFiles(t, "png", pngFiles(t))
}

func TestPNGGenerated(t *testing.T) {
	n := envInt("LIBBDF_IMAGE_CASES", 2000)
	checkFiles(t, "png", generatedPNGs(int64(envInt("LIBBDF_IMAGE_SEED", 1)), n))
}

func TestPNGMutations(t *testing.T) {
	in := pngFiles(t)
	for k, v := range generatedPNGs(7, 300) {
		in[k] = v
	}
	checkMutations(t, "png", in)
}

// pngFixChecksums recomputes the checksum of every chunk as far as the
// chunks can be followed, so that damage inside a chunk is read rather
// than refused at its checksum. Where the image data is still a stream of
// rows it also fixes the checksum of the stream.
func pngFixChecksums(b []byte) []byte {
	b = append([]byte(nil), b...)
	for pos := 8; pos+12 <= len(b); {
		n := int(binary.BigEndian.Uint32(b[pos:]))
		if n < 0 || pos+12+n > len(b) {
			break
		}
		binary.BigEndian.PutUint32(b[pos+8+n:], crc32.ChecksumIEEE(b[pos+4:pos+8+n]))
		pos += 12 + n
	}
	return b
}

// TestPNGDamagedInside damages images and repairs the checksums of their
// chunks, which otherwise refuse nearly every damaged file before any of
// it is read.
func TestPNGDamagedInside(t *testing.T) {
	in := pngFiles(t)
	for k, v := range generatedPNGs(13, 400) {
		in[k] = v
	}
	var names []string
	for name := range in {
		names = append(names, name)
	}
	sort.Strings(names)
	cases, seed := envInt("LIBBDF_IMAGE_CASES", 3000), int64(envInt("LIBBDF_IMAGE_SEED", 1))
	rng := rand.New(rand.NewSource(seed))
	decoded, failed := 0, 0
	for i := 0; i < cases; i++ {
		name := names[rng.Intn(len(names))]
		data := mutate(rng, in[name])
		for rng.Intn(3) == 0 {
			data = mutate(rng, data)
		}
		data = pngFixChecksums(data)
		if _, _, _, s := reference("png", data); s == statusOK {
			decoded++
		}
		if d := compare("png", data); d != "" {
			failed++
			if failed <= 20 {
				t.Errorf("case %d of seed %d (%s): %s", i, seed, name, d)
			}
			if dir := os.Getenv("LIBBDF_IMAGE_DUMP"); dir != "" {
				os.MkdirAll(dir, 0o755)
				os.WriteFile(filepath.Join(dir, fmt.Sprintf("png-inside-%d-%d.png", seed, i)), data, 0o644)
			}
		}
	}
	t.Logf("%d damaged files, %d of them still images the reference decodes, %d disagreements", cases, decoded, failed)
}

func FuzzPNG(f *testing.F) {
	in := pngFiles(f)
	for k, v := range generatedPNGs(11, 200) {
		in[k] = v
	}
	fuzzDecode(f, "png", in)
}
