package cbdf

// The BMP decoder (img_bmp.c) against golang.org/x/image/bmp.

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"testing"
	"time"

	"golang.org/x/image/bmp"
)

// ---- Building BMP files by hand ------------------------------------------

// bmpMasks are the masks of BI_BITFIELDS that say what no compression
// says: red, green, blue, alpha.
var bmpMasks = [4]uint32{0xff0000, 0xff00, 0xff, 0xff000000}

// bmpFile is a BMP, field by field.
type bmpFile struct {
	magic       string
	infoLen     uint32 // as the header states it; that many bytes of header are written, at least 4 and at most 200
	w, h        int32  // as the header states them: a negative height is rows from the top
	planes      uint16
	bpp         uint16
	compression uint32
	colorUsed   uint32
	masks       [4]uint32 // at offset 54: red, green, blue, alpha in the headers longer than 40 bytes
	palette     []byte    // four bytes a colour: blue, green, red and padding
	offset      int64     // of the pixels, as the header states it: where they are when negative
	pix         []byte    // the rows as the file has them
}

func (f bmpFile) bytes() []byte {
	n := min(max(int(f.infoLen), 4), 200)
	info := make([]byte, 200)
	le := binary.LittleEndian
	le.PutUint32(info[0:], f.infoLen)
	le.PutUint32(info[4:], uint32(f.w))
	le.PutUint32(info[8:], uint32(f.h))
	le.PutUint16(info[12:], f.planes)
	le.PutUint16(info[14:], f.bpp)
	le.PutUint32(info[16:], f.compression)
	le.PutUint32(info[20:], uint32(len(f.pix)))
	le.PutUint32(info[24:], 2835)
	le.PutUint32(info[28:], 2835)
	le.PutUint32(info[32:], f.colorUsed)
	for i, m := range f.masks {
		le.PutUint32(info[40+4*i:], m)
	}
	copy(info[56:], "BGRs") // the colour space of the longer headers
	info = info[:n]
	offset := f.offset
	if offset < 0 {
		offset = int64(14 + n + len(f.palette))
	}
	b := append([]byte(f.magic), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
	le.PutUint32(b[2:], uint32(14+n+len(f.palette)+len(f.pix)))
	le.PutUint32(b[10:], uint32(offset))
	b = append(b, info...)
	b = append(b, f.palette...)
	return append(b, f.pix...)
}

// bmpRowBytes is the length of a row of w pixels in the file.
func bmpRowBytes(w, bpp int) int { return (w*bpp + 31) / 32 * 4 }

// bmpRows is h rows of w pixels of bpp bits each: anything for 24 and 32
// bits and the depths that are not decoded, indexes below limit for the
// paletted ones. The padding of the rows is not zero: it is not to be read.
func bmpRows(w, h, bpp, limit int, seed int64) []byte {
	rng := rand.New(rand.NewSource(seed))
	row := bmpRowBytes(w, bpp)
	b := make([]byte, row*h)
	rng.Read(b)
	if bpp != 1 && bpp != 2 && bpp != 4 && bpp != 8 {
		return b
	}
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			shift := 8 - bpp - x*bpp%8
			p := &b[y*row+x*bpp/8]
			*p = *p&^byte((1<<bpp-1)<<shift) | byte(rng.Intn(limit))<<shift
		}
	}
	return b
}

// bmpPalette is a colour table of n colours, the padding bytes not zero.
func bmpPalette(n int, seed int64) []byte {
	b := make([]byte, 4*n)
	rand.New(rand.NewSource(seed)).Read(b)
	return b
}

// bmpNew is a w×h image (h negative: rows from the top) with a header of
// the given length, as the decoder accepts it for the depths it decodes.
func bmpNew(infoLen uint32, w, h int32, bpp int, seed int64) bmpFile {
	f := bmpFile{magic: "BM", infoLen: infoLen, w: w, h: h, planes: 1, bpp: uint16(bpp), offset: -1}
	if infoLen > 40 {
		f.masks = bmpMasks
	}
	rows := int(h)
	if rows < 0 {
		rows = -rows
	}
	colors := 0
	if bpp == 1 || bpp == 2 || bpp == 4 || bpp == 8 {
		colors = 1 << bpp
		f.palette = bmpPalette(colors, seed)
	}
	f.pix = bmpRows(int(w), rows, bpp, max(colors, 1), seed+1)
	return f
}

// ---- The files -----------------------------------------------------------

// bmpEncoded are images written by x/image/bmp.
func bmpEncoded(add func(name string, data []byte)) {
	encode := func(name string, m image.Image) {
		var buf bytes.Buffer
		if err := bmp.Encode(&buf, m); err != nil {
			panic(name + ": " + err.Error())
		}
		add("enc-"+name, buf.Bytes())
	}
	rng := rand.New(rand.NewSource(1))
	for _, s := range [][2]int{{1, 1}, {2, 2}, {3, 3}, {4, 4}, {5, 3}, {7, 9}, {32, 8}, {33, 7}} {
		w, h := s[0], s[1]
		r := image.Rect(0, 0, w, h)
		size := fmt.Sprintf("-%dx%d", w, h)
		gray := image.NewGray(r)
		rng.Read(gray.Pix)
		encode("gray"+size, gray)
		for _, n := range []int{2, 16, 255, 256} {
			p := make(color.Palette, n)
			for i := range p {
				p[i] = color.RGBA{byte(rng.Intn(256)), byte(rng.Intn(256)), byte(rng.Intn(256)), 0xff}
			}
			m := image.NewPaletted(r, p)
			for i := range m.Pix {
				m.Pix[i] = byte(rng.Intn(n))
			}
			encode(fmt.Sprintf("paletted-%d%s", n, size), m)
		}
		// opaque images are written with 24 bits, the others with 32 and a header of 40
		// bytes, whose alpha the decoder does not take
		nrgba := image.NewNRGBA(r)
		rng.Read(nrgba.Pix)
		encode("nrgba"+size, nrgba)
		rgba := image.NewRGBA(r)
		for i := 0; i < len(rgba.Pix); i += 4 {
			a := rng.Intn(256)
			rgba.Pix[i], rgba.Pix[i+1], rgba.Pix[i+2], rgba.Pix[i+3] = byte(rng.Intn(a+1)), byte(rng.Intn(a+1)), byte(rng.Intn(a+1)), byte(a)
		}
		encode("rgba"+size, rgba)
		for i := 3; i < len(nrgba.Pix); i += 4 {
			nrgba.Pix[i], rgba.Pix[i] = 0xff, 0xff
		}
		encode("nrgba-opaque"+size, nrgba)
		encode("rgba-opaque"+size, rgba)
		ycbcr := image.NewYCbCr(r, image.YCbCrSubsampleRatio420)
		rng.Read(ycbcr.Y)
		rng.Read(ycbcr.Cb)
		rng.Read(ycbcr.Cr)
		encode("ycbcr"+size, ycbcr)
	}
	encode("empty", image.NewRGBA(image.Rectangle{}))
	encode("empty-gray", image.NewGray(image.Rectangle{}))
}

// bmpBuilt are the headers the encoder does not write: every depth and
// header length the decoder accepts and the ones next to them that it
// refuses, rows from the top and from the bottom, with and without alpha,
// colour tables of every size.
func bmpBuilt(add func(name string, data []byte)) {
	put := func(name string, f bmpFile) { add("hand-"+name, f.bytes()) }
	dir := func(h int32) string {
		if h < 0 {
			return "down"
		}
		return "up"
	}

	// header lengths and depths
	depths := []int{0, 1, 2, 3, 4, 5, 7, 8, 9, 12, 15, 16, 17, 23, 24, 25, 31, 32, 33, 48, 64, 0x108, 0x118}
	for _, infoLen := range []uint32{0, 4, 12, 16, 39, 40, 41, 52, 56, 64, 107, 108, 109, 120, 123, 124, 125, 128, 0x10028, 0x28000000, 0xffffffff} {
		for _, bpp := range depths {
			if infoLen != 40 && infoLen != 108 && infoLen != 124 && bpp != 8 && bpp != 24 && bpp != 32 {
				continue
			}
			for _, h := range []int32{3, -3} {
				put(fmt.Sprintf("header%d-depth%d-%s", infoLen, bpp, dir(h)), bmpNew(infoLen, 5, h, bpp, int64(bpp)))
			}
		}
	}

	// widths, for the padding of the rows and the pixels that share a byte
	for _, bpp := range []int{1, 2, 4, 8, 24, 32} {
		for i, w := range []int32{1, 2, 3, 4, 5, 6, 7, 8, 9, 15, 16, 17, 31, 32, 33, 65} {
			h := int32(1 + i%4)
			if i%2 == 1 {
				h = -h
			}
			put(fmt.Sprintf("width%d-depth%d", w, bpp), bmpNew([]uint32{40, 108, 124}[i%3], w, h, bpp, int64(w)))
		}
	}

	// colour tables: the number of colours the header states, and pixels inside and past them
	for _, bpp := range []int{1, 2, 4, 8} {
		full := 1 << bpp
		seen := map[int]bool{}
		for _, used := range []int{0, 1, 2, 3, full - 1, full, full + 1, 2 * full, 255, 256, 257, 1000, 1 << 30, 1<<32 - 1} {
			if seen[used] {
				continue
			}
			seen[used] = true
			colors := used
			if used == 0 {
				colors = full
			}
			f := bmpNew(40, 9, 4, bpp, int64(used))
			f.colorUsed = uint32(used)
			f.palette = bmpPalette(min(colors, 300), 5)
			f.pix = bmpRows(9, 4, bpp, min(colors, full), 6)
			put(fmt.Sprintf("colors%d-depth%d", used, bpp), f)
			if colors < full {
				// one pixel is the first index past the table
				g := f
				g.pix = append([]byte(nil), f.pix...)
				shift := 8 - bpp
				g.pix[0] = g.pix[0]&^byte((full-1)<<shift) | byte(colors)<<shift
				put(fmt.Sprintf("colors%d-depth%d-past", used, bpp), g)
				g.pix = append([]byte(nil), f.pix...)
				last := len(g.pix) - bmpRowBytes(9, bpp) + 8*bpp/8 // the ninth pixel of the last row of the file
				shift = 8 - bpp - 8*bpp%8
				g.pix[last] = g.pix[last]&^byte((full-1)<<shift) | byte(full-1)<<shift
				put(fmt.Sprintf("colors%d-depth%d-past-last", used, bpp), g)
			}
			// the table of the file shorter or longer than the header says
			g := f
			g.offset = -1
			g.palette = bmpPalette(min(colors, 300)+1, 5)
			put(fmt.Sprintf("colors%d-depth%d-table-longer", used, bpp), g)
			g.palette = bmpPalette(max(min(colors, 300)-1, 0), 5)
			put(fmt.Sprintf("colors%d-depth%d-table-shorter", used, bpp), g)
		}
	}

	// the offset of the pixels has to be where the header and the table end
	for _, bpp := range []int{8, 24, 32} {
		f := bmpNew(40, 6, 3, bpp, 7)
		right := int64(14 + 40 + len(f.palette))
		for _, off := range []int64{0, 14, 54, right - 1, right + 1, right + 4, 1 << 31, 1<<32 - 1} {
			if off == right {
				continue
			}
			g := f
			g.offset = off
			if off == right+4 {
				// with the pixels where the offset says
				g.pix = append(make([]byte, 4), f.pix...)
			}
			put(fmt.Sprintf("offset%d-depth%d", off, bpp), g)
		}
	}

	// planes and compression
	for _, planes := range []uint16{0, 2, 256, 257} {
		f := bmpNew(40, 6, 3, 24, 8)
		f.planes = planes
		put(fmt.Sprintf("planes%d", planes), f)
	}
	for _, infoLen := range []uint32{40, 108, 124} {
		for _, bpp := range []int{1, 8, 16, 24, 32} {
			for _, compression := range []uint32{1, 2, 3, 4, 5, 6, 0x100, 1 << 31} {
				f := bmpNew(infoLen, 6, 3, bpp, 9)
				f.compression = compression
				put(fmt.Sprintf("compression%d-header%d-depth%d", compression, infoLen, bpp), f)
				if compression != 3 {
					continue
				}
				// BI_BITFIELDS is taken with the masks of BGRA alone
				for i, masks := range [][4]uint32{
					{0xff, 0xff00, 0xff0000, 0xff000000}, {0xff0000, 0xff00, 0xff, 0}, {0xf800, 0x7e0, 0x1f, 0}, {0, 0, 0, 0},
					{0xff0001, 0xff00, 0xff, 0xff000000}, {0xff0000, 0xff01, 0xff, 0xff000000}, {0xff0000, 0xff00, 0x1ff, 0xff000000}, {0xff0000, 0xff00, 0xff, 0xff000001},
				} {
					f.masks = masks
					put(fmt.Sprintf("compression3-masks%d-header%d-depth%d", i, infoLen, bpp), f)
				}
			}
		}
	}
	// a header of 40 bytes followed by the masks, as BI_BITFIELDS has them there
	{
		f := bmpNew(40, 6, 3, 32, 10)
		f.compression = 3
		f.palette = binary.LittleEndian.AppendUint32(binary.LittleEndian.AppendUint32(binary.LittleEndian.AppendUint32(nil, 0xff0000), 0xff00), 0xff)
		put("compression3-header40-masks-after", f)
	}
	// and followed by all four, which is where the longer headers have them: still refused
	for _, bpp := range []int{8, 24, 32} {
		f := bmpNew(40, 2, 3, bpp, 10)
		f.compression = 3
		var masks []byte
		for _, m := range bmpMasks {
			masks = binary.LittleEndian.AppendUint32(masks, m)
		}
		if bpp == 8 {
			copy(f.palette, masks)
		} else {
			f.pix = append(masks, f.pix...)
		}
		put(fmt.Sprintf("compression3-header40-masks-all-depth%d", bpp), f)
		f.compression = 0
		put(fmt.Sprintf("compression0-header40-masks-all-depth%d", bpp), f)
	}
	// masks that say something else, without BI_BITFIELDS: not looked at
	for _, bpp := range []int{8, 24, 32} {
		f := bmpNew(124, 6, 3, bpp, 11)
		f.masks = [4]uint32{0xf800, 0x7e0, 0x1f, 0}
		put(fmt.Sprintf("masks-unused-depth%d", bpp), f)
	}

	// alpha: taken from the headers longer than 40 bytes, ignored in that one
	for _, infoLen := range []uint32{40, 108, 124} {
		for _, h := range []int32{4, -4} {
			f := bmpNew(infoLen, 16, h, 32, 12)
			// the first pixels: alpha at its ends and next to them, with every colour at its
			for i, a := range []byte{0, 1, 2, 127, 128, 129, 254, 255} {
				copy(f.pix[4*i:], []byte{0xff, 0x80, 0x01, a})
				copy(f.pix[4*(i+8):], []byte{0x00, 0x7f, 0xfe, a})
			}
			put(fmt.Sprintf("alpha-header%d-%s", infoLen, dir(h)), f)
		}
	}

	// the sizes: none, one of them none, negative, and what does not fit
	size := func(name string, infoLen uint32, w, h int32, bpp int, pix int) {
		f := bmpNew(infoLen, 1, 1, bpp, 13)
		f.w, f.h = w, h
		f.pix = make([]byte, pix)
		put("size-"+name, f)
	}
	for _, bpp := range []int{1, 8, 24, 32} {
		d := fmt.Sprintf("-depth%d", bpp)
		size("0x0"+d, 40, 0, 0, bpp, 0)
		size("0x0-data"+d, 124, 0, 0, bpp, 16)
		size("0x5"+d, 40, 0, 5, bpp, 16)
		size("5x0"+d, 40, 5, 0, bpp, 16)
		size("0x-5"+d, 40, 0, -5, bpp, 16)
		size("-5x5"+d, 40, -5, 5, bpp, 100)
		size("-5x-5"+d, 40, -5, -5, bpp, 100)
		size("-1x1"+d, 40, -1, 1, bpp, 16)
		size("min-x1"+d, 40, math.MinInt32, 1, bpp, 16)
		size("1x-1"+d, 40, 1, -1, bpp, 4)
		// more pixels than the reference decodes: refused as too large, whatever the file has
		size("20000x20000"+d, 40, 20000, 20000, bpp, 16)
		size("10241x10240"+d, 40, 10241, 10240, bpp, 16)
		size("10240x-10241"+d, 40, 10240, -10241, bpp, 16)
		size("104857601x1"+d, 40, 104857601, 1, bpp, 16)
		size("1x104857601"+d, 40, 1, 104857601, bpp, 16)
		size("max-x1"+d, 40, math.MaxInt32, 1, bpp, 16)
		size("1xmax"+d, 40, 1, math.MaxInt32, bpp, 16)
		size("1x-max"+d, 40, 1, -math.MaxInt32, bpp, 16)
		size("65536x65536"+d, 40, 65536, 65536, bpp, 16)
		// as many as it decodes, without the rows: a failure to decode
		size("10240x10240"+d, 40, 10240, 10240, bpp, 16)
		size("104857600x1"+d, 108, 104857600, 1, bpp, 16)
		size("1x-104857600"+d, 124, 1, -104857600, bpp, 16)
		// four times the pixels does not fit Go's int: DecodeConfig fails
		size("max-x-max"+d, 40, math.MaxInt32, math.MaxInt32, bpp, 16)
		size("max-x-neg-max"+d, 40, math.MaxInt32, -math.MaxInt32, bpp, 16)
		// the last sizes that fit, and the first that do not
		size("2^30x(2^31-1)"+d, 40, 1<<30, math.MaxInt32, bpp, 16)
		size("(2^30+1)x(2^31-1)"+d, 40, 1<<30+1, math.MaxInt32, bpp, 16)
		size("(2^31-1)x2^30"+d, 40, math.MaxInt32, 1<<30, bpp, 16)
		size("(2^31-1)x(2^30+1)"+d, 40, math.MaxInt32, 1<<30+1, bpp, 16)
		// a height of -2^31 is 2^31 in Go: with a width of 2^30 four times the pixels are 2^63
		size("2^30x-2^31"+d, 40, 1<<30, math.MinInt32, bpp, 16)
		size("max-x-2^31"+d, 40, math.MaxInt32, math.MinInt32, bpp, 16)
		size("0x-2^31"+d, 40, 0, math.MinInt32, bpp, 16)
		size("-1x-2^31"+d, 40, -1, math.MinInt32, bpp, 16)
	}

	// the rows: one byte short, a row short, none, and more than the rows
	for _, bpp := range []int{1, 4, 8, 24, 32} {
		for _, h := range []int32{5, -5} {
			f := bmpNew(124, 7, h, bpp, 14)
			name := fmt.Sprintf("-depth%d-%s", bpp, dir(h))
			g := f
			g.pix = f.pix[:len(f.pix)-1]
			put("rows-byte-short"+name, g)
			g.pix = f.pix[:len(f.pix)-bmpRowBytes(7, bpp)]
			put("rows-row-short"+name, g)
			g.pix = f.pix[:1]
			put("rows-one-byte"+name, g)
			g.pix = nil
			put("rows-none"+name, g)
			g.pix = append(append([]byte(nil), f.pix...), bmpPalette(25, 15)...)
			put("rows-more"+name, g)
		}
	}

	// the signature
	for _, magic := range []string{"BA", "bm", "MB", "CI", "B\x00", "BMM"} {
		f := bmpNew(40, 6, 3, 24, 16)
		f.magic = magic
		put(fmt.Sprintf("magic-%x", magic), f)
	}
}

// bmpTallest are the files whose height is -2^31 and which DecodeConfig
// takes: their height is 2^31 in Go, which the int32_t of bdfi_bmp_config
// does not hold. See bmpCompare.
func bmpTallest() map[string][]byte {
	out := map[string][]byte{}
	for _, infoLen := range []uint32{40, 108, 124} {
		for _, bpp := range []int{1, 2, 4, 8, 24, 32} {
			for _, w := range []int32{1, 7, 1<<30 - 1} {
				f := bmpNew(infoLen, 1, 1, bpp, 17)
				f.w, f.h = w, math.MinInt32
				out[fmt.Sprintf("hand-tallest-%dx-2^31-header%d-depth%d", w, infoLen, bpp)] = f.bytes()
			}
		}
	}
	return out
}

// bmpMade are the files the test makes itself.
func bmpMade() map[string][]byte {
	out := map[string][]byte{}
	add := func(name string, data []byte) {
		if _, dup := out[name]; dup {
			panic("two files are named " + name)
		}
		out[name] = data
	}
	bmpEncoded(add)
	bmpBuilt(add)
	return out
}

// bmpFiles are the files the decoders are compared on: the image parts of
// the reference's documents, the testdata of x/image, what another encoder
// wrote (testdata/gen, see gen.sh) and what the test makes.
func bmpFiles(t testing.TB) map[string][]byte {
	out := withDocumentImages(t, "bmp", []string{ximage(t) + "/testdata", "testdata/images"}, ".bmp")
	if len(out) < 25 {
		t.Fatalf("only %d BMP files were found on disk", len(out))
	}
	for name, data := range bmpMade() {
		out[name] = data
	}
	return out
}

// bmpSeeds are the files of bmpFiles that are worth damaging: the images
// the reference decodes, and of the many it refuses one in eight, which a
// damage can turn into an image.
func bmpSeeds(t testing.TB) map[string][]byte {
	out := map[string][]byte{}
	refused := 0
	all := bmpFiles(t)
	for _, name := range bmpSortedNames(all) {
		data := all[name]
		if len(data) > 1<<17 {
			continue
		}
		if _, _, _, s := reference("bmp", data); s != statusOK {
			refused++
			if refused%8 != 0 {
				continue
			}
		}
		out[name] = data
	}
	for name, data := range bmpTallest() {
		if refused++; refused%8 == 0 {
			out[name] = data
		}
	}
	return out
}

func bmpSortedNames(in map[string][]byte) []string {
	var names []string
	for name := range in {
		names = append(names, name)
	}
	for i := 1; i < len(names); i++ {
		for j := i; j > 0 && names[j] < names[j-1]; j-- {
			names[j], names[j-1] = names[j-1], names[j]
		}
	}
	return names
}

// bmpCompare is compare for BMP, except on the one answer of the reference
// that the C interface cannot give. A file whose height is -2^31 has, for
// Go, a height of 2^31, and DecodeConfig returns it where four times the
// pixels still fit an int; bdfi_bmp_config returns its sizes in int32_t. It
// is to return the largest int32_t for that height, and the decoding is to
// be refused as too large by both.
func bmpCompare(data []byte) string {
	gw, gh, gok := refConfig("bmp", data)
	if !gok || gh <= math.MaxInt32 {
		return compare("bmp", data)
	}
	if gh != 1<<31 {
		return fmt.Sprintf("config: the reference returns a height of %d", gh)
	}
	cw, ch, cok := cConfig("bmp", data)
	if !cok || cw != gw || ch != math.MaxInt32 {
		return fmt.Sprintf("config: reference %dx%d, C %dx%d %v, and not the reference's width and the largest int32", gw, gh, cw, ch, cok)
	}
	_, _, _, gs := reference("bmp", data)
	_, _, _, cs := cDecode("bmp", data)
	if gs != statusTooLarge || cs != statusTooLarge {
		return fmt.Sprintf("decode: reference status %d, C status %d, and not too large for both", gs, cs)
	}
	return ""
}

// ---- The tests -----------------------------------------------------------

func TestBmpFiles(t *testing.T) {
	checkFiles(t, "bmp", bmpFiles(t))
}

// TestBmpTallest is the height that the interface cannot return.
func TestBmpTallest(t *testing.T) {
	for name, data := range bmpTallest() {
		if _, h, ok := refConfig("bmp", data); !ok || h != 1<<31 {
			t.Errorf("%s: the reference returns the height %d (%v), the file was made for 2^31", name, h, ok)
		}
		if d := bmpCompare(data); d != "" {
			t.Errorf("%s: %s", name, d)
		}
		// and compare says what the difference is
		if d := compare("bmp", data); d == "" {
			t.Errorf("%s: compare finds no difference", name)
		}
	}
}

// TestBmpMutations is checkMutations with bmpCompare for compare.
func TestBmpMutations(t *testing.T) {
	in := bmpSeeds(t)
	names := bmpSortedNames(in)
	cases, seed := envInt("LIBBDF_IMAGE_CASES", 2000), int64(envInt("LIBBDF_IMAGE_SEED", 1))
	rng := rand.New(rand.NewSource(seed))
	decoded, failed, tallest := 0, 0, 0
	for i := 0; i < cases; i++ {
		name := names[rng.Intn(len(names))]
		data := mutate(rng, in[name])
		for rng.Intn(4) == 0 {
			data = mutate(rng, data)
		}
		if _, _, _, s := reference("bmp", data); s == statusOK {
			decoded++
		}
		if _, h, ok := refConfig("bmp", data); ok && h > math.MaxInt32 {
			tallest++
		}
		if d := bmpCompare(data); d != "" {
			failed++
			if failed <= 20 {
				t.Errorf("case %d of seed %d (%s): %s", i, seed, filepath.Base(name), d)
			}
			if dir := os.Getenv("LIBBDF_IMAGE_DUMP"); dir != "" {
				os.MkdirAll(dir, 0o755)
				os.WriteFile(filepath.Join(dir, fmt.Sprintf("bmp-%d-%d", seed, i)), data, 0o644)
			}
		}
	}
	t.Logf("%d damaged files of %d, %d of them still images the reference decodes, %d with a height of 2^31, %d disagreements",
		cases, len(names), decoded, tallest, failed)
}

// FuzzBmp is fuzzDecode with bmpCompare for compare.
func FuzzBmp(f *testing.F) {
	for _, data := range bmpSeeds(f) {
		if len(data) <= 1<<16 {
			f.Add(data)
		}
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		// an image of many megapixels says nothing a small one does not
		if w, h, ok := refConfig("bmp", data); ok && (w > 4096 || h > 4096 || w*h > 1<<22) && h <= math.MaxInt32 {
			t.Skip()
		}
		if d := bmpCompare(data); d != "" {
			t.Fatal(d)
		}
	})
}

// TestBmpExpected checks the files of this test against what they were
// made to be, so that the comparison of the decoders is not one of two
// refusals where an image was meant.
func TestBmpExpected(t *testing.T) {
	made := bmpMade()
	for name, want := range map[string]int{
		"enc-gray-5x3": statusOK, "enc-paletted-2-1x1": statusOK, "enc-paletted-256-33x7": statusOK, "enc-nrgba-7x9": statusOK,
		"enc-rgba-7x9": statusOK, "enc-rgba-opaque-33x7": statusOK, "enc-ycbcr-4x4": statusOK, "enc-empty": statusTooLarge, "enc-empty-gray": statusTooLarge,
		"hand-header40-depth1-up": statusOK, "hand-header40-depth2-down": statusOK, "hand-header40-depth4-up": statusOK, "hand-header40-depth8-down": statusOK,
		"hand-header40-depth24-up": statusOK, "hand-header40-depth32-down": statusOK, "hand-header108-depth1-down": statusOK, "hand-header108-depth32-up": statusOK,
		"hand-header124-depth2-up": statusOK, "hand-header124-depth24-down": statusOK, "hand-header124-depth32-down": statusOK,
		"hand-header40-depth0-up": statusFormat, "hand-header40-depth3-up": statusFormat, "hand-header40-depth16-up": statusFormat, "hand-header124-depth16-down": statusFormat,
		"hand-header40-depth23-up": statusFormat, "hand-header40-depth25-up": statusFormat, "hand-header40-depth33-up": statusFormat, "hand-header40-depth64-up": statusFormat,
		"hand-header40-depth264-up": statusFormat, "hand-header40-depth280-up": statusFormat,
		"hand-header12-depth24-up": statusFormat, "hand-header39-depth24-up": statusFormat, "hand-header41-depth24-up": statusFormat, "hand-header52-depth32-up": statusFormat,
		"hand-header56-depth32-up": statusFormat, "hand-header64-depth8-up": statusFormat, "hand-header107-depth24-up": statusFormat, "hand-header109-depth24-up": statusFormat,
		"hand-header123-depth24-up": statusFormat, "hand-header125-depth24-up": statusFormat, "hand-header65576-depth24-up": statusFormat, "hand-header4294967295-depth24-up": statusFormat,
		"hand-width1-depth1": statusOK, "hand-width9-depth1": statusOK, "hand-width33-depth1": statusOK, "hand-width5-depth2": statusOK, "hand-width7-depth4": statusOK,
		"hand-width3-depth8": statusOK, "hand-width5-depth24": statusOK, "hand-width65-depth32": statusOK,
		"hand-colors0-depth1": statusOK, "hand-colors1-depth1": statusOK, "hand-colors1-depth1-past": statusFormat, "hand-colors2-depth1": statusOK, "hand-colors3-depth1": statusFormat,
		"hand-colors3-depth2": statusOK, "hand-colors3-depth2-past": statusFormat, "hand-colors3-depth2-past-last": statusFormat, "hand-colors4-depth2": statusOK, "hand-colors5-depth2": statusFormat,
		"hand-colors15-depth4": statusOK, "hand-colors15-depth4-past": statusFormat, "hand-colors16-depth4": statusOK, "hand-colors17-depth4": statusFormat,
		"hand-colors1-depth8": statusOK, "hand-colors1-depth8-past": statusFormat, "hand-colors255-depth8": statusOK, "hand-colors255-depth8-past": statusFormat,
		"hand-colors255-depth8-past-last": statusFormat, "hand-colors256-depth8": statusOK, "hand-colors257-depth8": statusFormat, "hand-colors4294967295-depth8": statusFormat,
		"hand-colors16-depth4-table-longer": statusFormat, "hand-colors16-depth4-table-shorter": statusFormat,
		"hand-offset0-depth24": statusFormat, "hand-offset53-depth24": statusFormat, "hand-offset55-depth24": statusFormat, "hand-offset58-depth24": statusFormat,
		"hand-offset54-depth8": statusFormat, "hand-offset1077-depth8": statusFormat, "hand-offset1079-depth8": statusFormat, "hand-offset4294967295-depth32": statusFormat,
		"hand-planes0": statusFormat, "hand-planes2": statusFormat, "hand-planes257": statusFormat,
		"hand-compression1-header40-depth8": statusFormat, "hand-compression3-header40-depth32": statusFormat, "hand-compression3-header108-depth32": statusOK,
		"hand-compression3-header124-depth32": statusOK, "hand-compression3-header124-depth24": statusOK, "hand-compression3-header108-depth8": statusOK,
		"hand-compression3-header108-depth1": statusOK, "hand-compression3-header124-depth16": statusFormat,
		"hand-compression3-masks0-header124-depth32": statusFormat, "hand-compression3-masks1-header124-depth32": statusFormat, "hand-compression3-masks2-header108-depth16": statusFormat,
		"hand-compression3-masks4-header124-depth32": statusFormat, "hand-compression3-masks7-header124-depth32": statusFormat,
		"hand-compression3-header40-masks-after": statusFormat, "hand-compression3-header40-masks-all-depth8": statusFormat,
		"hand-compression3-header40-masks-all-depth24": statusFormat, "hand-compression3-header40-masks-all-depth32": statusFormat,
		"hand-compression0-header40-masks-all-depth8": statusOK, "hand-compression0-header40-masks-all-depth24": statusOK,
		"hand-compression0-header40-masks-all-depth32": statusOK, "hand-compression4-header124-depth24": statusFormat, "hand-compression2147483648-header124-depth24": statusFormat,
		"hand-masks-unused-depth8": statusOK, "hand-masks-unused-depth32": statusOK,
		"hand-alpha-header40-up": statusOK, "hand-alpha-header108-down": statusOK, "hand-alpha-header124-up": statusOK,
		"hand-size-0x0-depth24": statusTooLarge, "hand-size-0x0-data-depth8": statusTooLarge, "hand-size-0x5-depth24": statusFormat, "hand-size-5x0-depth1": statusFormat,
		"hand-size-0x-5-depth32": statusFormat, "hand-size--5x5-depth24": statusFormat, "hand-size-min-x1-depth24": statusFormat, "hand-size-1x-1-depth24": statusOK, "hand-size-1x-1-depth1": statusOK,
		"hand-size-20000x20000-depth24": statusTooLarge, "hand-size-10241x10240-depth8": statusTooLarge, "hand-size-10240x-10241-depth32": statusTooLarge,
		"hand-size-104857601x1-depth1": statusTooLarge, "hand-size-1x104857601-depth24": statusTooLarge, "hand-size-max-x1-depth24": statusTooLarge, "hand-size-1x-max-depth8": statusTooLarge,
		"hand-size-65536x65536-depth24": statusTooLarge, "hand-size-10240x10240-depth24": statusFormat, "hand-size-10240x10240-depth1": statusFormat,
		"hand-size-104857600x1-depth32": statusFormat, "hand-size-1x-104857600-depth8": statusFormat,
		"hand-size-max-x-max-depth24": statusFormat, "hand-size-max-x-neg-max-depth8": statusFormat,
		"hand-size-2^30x(2^31-1)-depth24": statusTooLarge, "hand-size-(2^30+1)x(2^31-1)-depth24": statusFormat, "hand-size-(2^31-1)x2^30-depth1": statusTooLarge, "hand-size-(2^31-1)x(2^30+1)-depth32": statusFormat,
		"hand-size-2^30x-2^31-depth24": statusFormat, "hand-size-max-x-2^31-depth8": statusFormat, "hand-size-0x-2^31-depth24": statusFormat, "hand-size--1x-2^31-depth24": statusFormat,
		"hand-rows-byte-short-depth1-up": statusFormat, "hand-rows-byte-short-depth32-down": statusFormat, "hand-rows-row-short-depth24-up": statusFormat,
		"hand-rows-none-depth8-down": statusFormat, "hand-rows-more-depth4-up": statusOK, "hand-rows-more-depth32-down": statusOK,
		"hand-magic-4241": statusFormat, "hand-magic-626d": statusFormat, "hand-magic-424d4d": statusFormat,
	} {
		data, ok := made[name]
		if !ok {
			t.Errorf("no file named %s", name)
			continue
		}
		if _, _, _, got := reference("bmp", data); got != want {
			t.Errorf("%s: the reference returns status %d, the file was made for %d", name, got, want)
		}
	}
	// the pixels of some: the first two of the first row of the picture, which is the last of
	// the file unless the height is negative
	first := func(name string, want ...[4]byte) {
		t.Helper()
		pix, _, _, s := cDecode("bmp", made[name])
		if s != statusOK {
			t.Errorf("%s: status %d", name, s)
			return
		}
		for i, w := range want {
			if got := [4]byte(pix[4*i:]); got != w {
				t.Errorf("%s: pixel %d is %v, not %v", name, i, got, w)
			}
		}
	}
	// blue ff, green 80, red 01 with alpha 0 and 1: ignored, and premultiplied
	first("hand-alpha-header40-down", [4]byte{1, 0x80, 0xff, 0xff}, [4]byte{1, 0x80, 0xff, 0xff})
	first("hand-alpha-header108-down", [4]byte{0, 0, 0, 0}, [4]byte{0, 0, 1, 1})
	first("hand-alpha-header124-down", [4]byte{0, 0, 0, 0}, [4]byte{0, 0, 1, 1})
}

// TestBmpGenerated checks that the files of gen.sh are there and are what
// they were made to be: images of a size, or files the decoder refuses.
func TestBmpGenerated(t *testing.T) {
	for name, want := range map[string][2]int{
		"plasma-24.bmp": {48, 40}, "gradient-24.bmp": {33, 17}, "plasma-8.bmp": {48, 40}, "plasma-4.bmp": {47, 37}, "checkerboard-1.bmp": {45, 33},
		"gradient-gray-8.bmp": {48, 40}, "plasma-8-v5.bmp": {31, 23}, "alpha-v5.bmp": {40, 30}, "alpha-plasma-v5.bmp": {40, 30},
		"alpha-dropped-v3.bmp": {40, 30}, "plasma-v5.bmp": {31, 23},
		"plasma-v2.bmp": {}, "plasma-rle8.bmp": {}, "plasma-565.bmp": {}, "plasma-1555.bmp": {},
	} {
		data, err := os.ReadFile(filepath.Join("testdata/images", name))
		if err != nil {
			t.Error(err)
			continue
		}
		_, w, h, s := reference("bmp", data)
		if want == [2]int{} {
			if s != statusFormat {
				t.Errorf("%s: the reference returns status %d, the file was made to be refused", name, s)
			}
		} else if s != statusOK || w != want[0] || h != want[1] {
			t.Errorf("%s: the reference returns status %d and %dx%d, the file was made a %dx%d image", name, s, w, h, want[0], want[1])
		}
	}
}

// TestBmpAlpha compares the decoders on every colour value with every
// alpha, in the headers that have alpha and in the one that has none.
func TestBmpAlpha(t *testing.T) {
	for _, infoLen := range []uint32{40, 108, 124} {
		f := bmpNew(infoLen, 256, -256, 32, 1)
		for a := 0; a < 256; a++ {
			for c := 0; c < 256; c++ {
				copy(f.pix[4*(a*256+c):], []byte{byte(c), byte(255 - c), byte(c ^ a), byte(a)})
			}
		}
		data := f.bytes()
		if _, _, _, s := reference("bmp", data); s != statusOK {
			t.Fatalf("the reference does not decode the image with a header of %d bytes", infoLen)
		}
		if d := compare("bmp", data); d != "" {
			t.Errorf("header of %d bytes: %s", infoLen, d)
		}
	}
}

// TestBmpTruncated compares the decoders on every beginning of some files.
func TestBmpTruncated(t *testing.T) {
	made := bmpMade()
	n := 0
	for _, name := range []string{"enc-gray-5x3", "enc-nrgba-5x3", "enc-rgba-opaque-5x3", "hand-header40-depth1-up", "hand-header108-depth2-down",
		"hand-header124-depth4-up", "hand-header124-depth32-down", "hand-colors3-depth2", "hand-compression3-header108-depth32", "hand-width33-depth1"} {
		data, ok := made[name]
		if !ok {
			t.Fatalf("no file named %s", name)
		}
		if _, _, _, s := reference("bmp", data); s != statusOK {
			t.Fatalf("%s is not an image", name)
		}
		for i := 0; i <= len(data); i++ {
			if d := compare("bmp", data[:i]); d != "" {
				t.Errorf("%s cut to %d bytes: %s", name, i, d)
				break
			}
			n++
		}
	}
	t.Logf("%d beginnings of files", n)
}

// TestBmpRandom compares the decoders on headers whose every field is
// drawn at random among the values that matter: LIBBDF_IMAGE_CASES of them.
func TestBmpRandom(t *testing.T) {
	cases, seed := envInt("LIBBDF_IMAGE_CASES", 2000), int64(envInt("LIBBDF_IMAGE_SEED", 1))
	rng := rand.New(rand.NewSource(seed))
	decoded, failed, tallest := 0, 0, 0
	for i := 0; i < cases; i++ {
		data := bmpRandom(rng)
		if _, _, _, s := reference("bmp", data); s == statusOK {
			decoded++
		}
		if _, h, ok := refConfig("bmp", data); ok && h > math.MaxInt32 {
			tallest++
		}
		if d := bmpCompare(data); d != "" {
			failed++
			if failed <= 20 {
				t.Errorf("case %d of seed %d: %s", i, seed, d)
			}
			if dir := os.Getenv("LIBBDF_IMAGE_DUMP"); dir != "" {
				os.MkdirAll(dir, 0o755)
				os.WriteFile(filepath.Join(dir, fmt.Sprintf("bmp-random-%d-%d", seed, i)), data, 0o644)
			}
		}
	}
	t.Logf("%d files, %d of them images the reference decodes, %d with a height of 2^31, %d disagreements", cases, decoded, tallest, failed)
}

// bmpRandom is a BMP whose every field is drawn at random.
func bmpRandom(rng *rand.Rand) []byte {
	pick := func(n int) bool { return rng.Intn(n) == 0 }
	oneOf := func(v ...int64) int64 { return v[rng.Intn(len(v))] }
	infoLen := uint32(oneOf(40, 40, 108, 124))
	if pick(15) {
		infoLen = uint32(oneOf(0, 12, 39, 41, 52, 56, 64, 107, 109, 123, 125, 200, 1<<32-1))
	}
	bpp := int(oneOf(1, 2, 4, 8, 24, 32))
	if pick(15) {
		bpp = int(oneOf(0, 3, 5, 16, 23, 25, 31, 33, 64, 1<<16-1))
	}
	w, h := int32(1+rng.Intn(40)), int32(1+rng.Intn(12))
	rows := int(h)
	if pick(2) {
		h = -h
	}
	f := bmpNew(infoLen, w, h, bpp, rng.Int63())
	colors := len(f.palette) / 4
	if colors > 0 && pick(2) {
		// fewer colours than the depth has, and pixels within them or not
		f.colorUsed = uint32(1 + rng.Intn(colors))
		if pick(10) {
			f.colorUsed = uint32(oneOf(int64(colors)+1, 0, 257, 1<<32-1))
		}
		n := int(f.colorUsed)
		if n == 0 || n > colors {
			n = colors
		}
		f.palette = f.palette[:4*n]
		if !pick(6) {
			f.pix = bmpRows(int(w), rows, bpp, n, rng.Int63())
		}
	}
	if pick(12) {
		f.w = int32(oneOf(0, -1, int64(w)+1, int64(w)-1, 1<<16, 1<<30-1, 1<<30, math.MaxInt32, math.MinInt32, 10240, 104857600))
	}
	if pick(12) {
		f.h = int32(oneOf(0, int64(h)+1, int64(h)-1, -int64(h), 1<<16, 1<<30, math.MaxInt32, -math.MaxInt32, math.MinInt32, math.MinInt32, 10240, -10241))
	}
	if pick(20) {
		f.planes = uint16(oneOf(0, 2, 256))
	}
	if pick(10) {
		f.compression = uint32(oneOf(1, 2, 3, 3, 3, 4, 1<<31))
		if pick(4) {
			f.masks[rng.Intn(4)] ^= 1 << rng.Intn(32)
		}
	}
	if pick(20) {
		f.offset = int64(14+min(max(int(infoLen), 4), 200)+len(f.palette)) + oneOf(-4, -1, 1, 4, 1<<32-1)
	}
	switch rng.Intn(15) {
	case 0:
		f.pix = f.pix[:rng.Intn(len(f.pix)+1)]
	case 1:
		f.pix = append(f.pix, make([]byte, rng.Intn(9))...)
	case 2:
		f.magic = string([]byte{byte(rng.Intn(256)), 'M'})
	}
	return f.bytes()
}

// TestBmpSpeed times the two decoders on a few files, when LIBBDF_IMAGE_SPEED
// is set, and compares them on those files: the made ones are larger than
// any other file of the tests.
func TestBmpSpeed(t *testing.T) {
	if os.Getenv("LIBBDF_IMAGE_SPEED") == "" {
		t.Skip("LIBBDF_IMAGE_SPEED is not set")
	}
	in := bmpFiles(t)
	for _, bpp := range []int{1, 4, 8, 24, 32} {
		in[fmt.Sprintf("speed-1000x800-depth%d", bpp)] = bmpNew(124, 1000, 800, bpp, 1).bytes()
	}
	in["speed-1000x800-depth32-no-alpha"] = bmpNew(40, 1000, 800, 32, 1).bytes()
	for _, name := range []string{ximage(t) + "/testdata/video-001.bmp", ximage(t) + "/testdata/yellow_rose-small.bmp", ximage(t) + "/testdata/yellow_rose-small-v5.bmp",
		ximage(t) + "/testdata/bmp_1bpp.bmp", "speed-1000x800-depth1", "speed-1000x800-depth4", "speed-1000x800-depth8", "speed-1000x800-depth24",
		"speed-1000x800-depth32", "speed-1000x800-depth32-no-alpha"} {
		data, ok := in[name]
		if !ok {
			t.Fatalf("no file named %s", name)
		}
		timeOf := func(decode func()) time.Duration {
			best := time.Duration(1 << 62)
			for round := 0; round < 5; round++ {
				n := 0
				start := time.Now()
				for time.Since(start) < 100*time.Millisecond {
					decode()
					n++
				}
				best = min(best, time.Since(start)/time.Duration(n))
			}
			return best
		}
		_, w, h, s := reference("bmp", data)
		if s != statusOK {
			t.Fatalf("%s does not decode", name)
		}
		if d := compare("bmp", data); d != "" {
			t.Errorf("%s: %s", name, d)
		}
		g := timeOf(func() { reference("bmp", data) })
		c := timeOf(func() { cDecode("bmp", data) })
		t.Logf("%-40s %4dx%-4d  Go %9v  C %9v  (C is %.1fx as fast)", filepath.Base(name), w, h, g, c, float64(g)/float64(c))
	}
}
