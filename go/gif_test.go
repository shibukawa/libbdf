package cbdf

// The GIF decoder (img_gif.c) against image/gif: the first frame of every
// file, as gif.Decode reads it.

import (
	"bytes"
	"compress/lzw"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/gif"
	"math/rand"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// ---- Building GIF files by hand ------------------------------------------

// gifColors is a colour table of n colours, three bytes a colour.
func gifColors(n int, seed int64) []byte {
	b := make([]byte, 3*n)
	rand.New(rand.NewSource(seed)).Read(b)
	return b
}

// gifIndexes is w*h pixels below n, in runs, so that LZW finds something
// to repeat, and with every index among them where there is room for that
// (a transparent index that no pixel has would test nothing).
func gifIndexes(w, h, n int, seed int64) []byte {
	rng := rand.New(rand.NewSource(seed))
	p := make([]byte, w*h)
	for i := 0; i < len(p); {
		c := byte(rng.Intn(n))
		for run := 1 + rng.Intn(6); run > 0 && i < len(p); run-- {
			p[i] = c
			i++
		}
	}
	if len(p) >= 2*n {
		for i := 0; i < n; i++ {
			p[(2*i+1)*len(p)/(2*n)] = byte(i)
		}
	}
	return p
}

// gifCompress is the LZW stream compress/lzw writes for the pixels: a clear
// code first, an end code last.
func gifCompress(pix []byte, lit int) []byte {
	var buf bytes.Buffer
	w := lzw.NewWriter(&buf, lzw.LSB, lit)
	if _, err := w.Write(pix); err != nil {
		panic(err)
	}
	w.Close()
	return buf.Bytes()
}

// gifBlocks cuts image data into sub-blocks of the given sizes in turn (of
// 255 bytes when none is given) and ends them with the block terminator.
func gifBlocks(data []byte, sizes ...int) []byte {
	if len(sizes) == 0 {
		sizes = []int{255}
	}
	var out []byte
	for i := 0; len(data) > 0; i++ {
		n := min(sizes[i%len(sizes)], len(data))
		out = append(out, byte(n))
		out = append(out, data[:n]...)
		data = data[n:]
	}
	return append(out, 0)
}

// gifInterlace puts the rows of an image into the order an interlaced GIF
// stores them in.
func gifInterlace(pix []byte, w, h int) []byte {
	var out []byte
	for _, pass := range [][2]int{{8, 0}, {8, 4}, {4, 2}, {2, 1}} {
		for y := pass[1]; y < h; y += pass[0] {
			out = append(out, pix[y*w:(y+1)*w]...)
		}
	}
	return out
}

// gifCoder writes LZW codes one by one, each in the width the decoder reads
// it with: it follows what lzw.Reader does to its table on every code, and
// knows how many pixels the codes written so far expand to.
type gifCoder struct {
	lit      int
	width    uint
	hi, over int
	last     int // the code before, or -1 where the decoder has none
	length   [4096]int
	pixels   int // what the codes expand to
	bits     uint32
	nBits    uint
	out      []byte
}

func newGifCoder(lit int) *gifCoder {
	c := &gifCoder{lit: lit}
	c.reset()
	return c
}

func (c *gifCoder) clearCode() int { return 1 << c.lit }
func (c *gifCoder) endCode() int   { return 1<<c.lit + 1 }

func (c *gifCoder) reset() {
	c.width, c.hi, c.over, c.last = uint(c.lit)+1, c.endCode(), 1<<(c.lit+1), -1
}

// lengthOf is how many pixels a code of the table, or a literal, expands to.
func (c *gifCoder) lengthOf(code int) int {
	if code < c.clearCode() {
		return 1
	}
	return c.length[code]
}

// raw writes a code in a given width, whatever the decoder expects.
func (c *gifCoder) raw(code int, width uint) {
	c.bits |= uint32(code&(1<<width-1)) << c.nBits
	c.nBits += width
	for c.nBits >= 8 {
		c.out = append(c.out, byte(c.bits))
		c.bits >>= 8
		c.nBits -= 8
	}
}

// code writes codes as the decoder reads them.
func (c *gifCoder) code(codes ...int) *gifCoder {
	for _, code := range codes {
		c.raw(code, c.width)
		switch {
		case code < c.clearCode():
			c.pixels++
			if c.last >= 0 {
				c.length[c.hi] = c.lengthOf(c.last) + 1
			}
		case code == c.clearCode():
			c.reset()
			continue
		case code == c.endCode():
			continue
		case code <= c.hi:
			if code == c.hi && c.last >= 0 {
				c.pixels += c.lengthOf(c.last) + 1
			} else {
				c.pixels += c.length[code]
			}
			if c.last >= 0 {
				c.length[c.hi] = c.lengthOf(c.last) + 1
			}
		default:
			continue // an invalid code: the decoder stops
		}
		c.last = code
		c.hi++
		if c.hi >= c.over {
			if c.width == 12 {
				c.last = -1
				c.hi--
			} else {
				c.width++
				c.over = 1 << c.width
			}
		}
	}
	return c
}

// expands is how many pixels a code would expand to now, or 0 for a code
// the decoder would not take.
func (c *gifCoder) expands(code int) int {
	switch {
	case code < c.clearCode():
		return 1
	case code <= c.endCode() || code > c.hi:
		return 0
	case code == c.hi && c.last >= 0:
		return c.lengthOf(c.last) + 1
	}
	return c.length[code]
}

// end writes the end code.
func (c *gifCoder) end() *gifCoder { return c.code(c.endCode()) }

// invalid writes a code the decoder refuses, one past the table: directly
// when the width has room for one, after a clear code otherwise.
func (c *gifCoder) invalid() *gifCoder {
	if c.hi+1 >= c.over {
		c.code(c.clearCode())
	}
	return c.code(c.hi + 1)
}

// align writes clear codes until the stream ends with a whole byte, so
// that the padding of the last byte is not read as a code. (A stream ends
// with its end code otherwise, after which nothing is read.)
func (c *gifCoder) align() *gifCoder {
	for i := 0; c.nBits != 0; i++ {
		if i > 16 {
			panic("clear codes of this width do not get to the end of a byte")
		}
		c.code(c.clearCode())
	}
	return c
}

// literals writes the pixels as literal codes, with a clear code before
// every clearEvery of them (never when it is 0).
func (c *gifCoder) literals(pix []byte, clearEvery int) *gifCoder {
	for i, p := range pix {
		if clearEvery > 0 && i%clearEvery == 0 {
			c.code(c.clearCode())
		}
		c.code(int(p))
	}
	return c
}

// random writes codes drawn at random among those the decoder takes until
// they expand to exactly total pixels: literals, codes of the table, the
// code that is about to be entered, and a clear code once in clearOneIn
// codes (never when that is below 2).
func (c *gifCoder) random(rng *rand.Rand, total int, clearOneIn int) *gifCoder {
	for c.pixels < total {
		if clearOneIn > 1 && rng.Intn(clearOneIn) == 0 {
			c.code(c.clearCode())
			continue
		}
		code := rng.Intn(c.clearCode())
		switch rng.Intn(4) {
		case 0:
			code = c.hi
		case 1, 2:
			code = c.endCode() + 1 + rng.Intn(max(c.hi-c.endCode(), 1))
		}
		if n := c.expands(code); n == 0 || c.pixels+n > total {
			code = rng.Intn(c.clearCode())
		}
		c.code(code)
	}
	return c
}

// bytes is the stream so far, the last byte padded with zero bits.
func (c *gifCoder) bytes() []byte {
	out := append([]byte(nil), c.out...)
	if c.nBits > 0 {
		out = append(out, byte(c.bits))
	}
	return out
}

// gifFile is a GIF with one image, part by part.
type gifFile struct {
	version    string // "GIF89a" when empty
	sw, sh     int    // the logical screen
	global     []byte // the global colour table, or nil
	before     []byte // what stands between the screen and the image descriptor
	x, y, w, h int    // the frame
	local      []byte // the local colour table, or nil
	interlace  bool
	lit        int    // the LZW minimum code size
	data       []byte // the image data, in sub-blocks
	after      []byte // what follows the image data: the trailer when nil
}

// gifTableBits is the size field of a colour table.
func gifTableBits(table []byte) byte {
	bits := byte(0)
	for 2<<bits < len(table)/3 {
		bits++
	}
	return bits
}

func (g gifFile) bytes() []byte {
	b := []byte(g.version)
	if g.version == "" {
		b = []byte("GIF89a")
	}
	fields := byte(0)
	if g.global != nil {
		fields = 0x80 | gifTableBits(g.global)
	}
	b = append(b, byte(g.sw), byte(g.sw>>8), byte(g.sh), byte(g.sh>>8), fields, 0, 0)
	b = append(b, g.global...)
	b = append(b, g.before...)
	fields = 0
	if g.local != nil {
		fields = 0x80 | gifTableBits(g.local)
	}
	if g.interlace {
		fields |= 0x40
	}
	b = append(b, 0x2c, byte(g.x), byte(g.x>>8), byte(g.y), byte(g.y>>8), byte(g.w), byte(g.w>>8), byte(g.h), byte(g.h>>8), fields)
	b = append(b, g.local...)
	b = append(b, byte(g.lit))
	b = append(b, g.data...)
	if g.after == nil {
		return append(b, 0x3b)
	}
	return append(b, g.after...)
}

// gifPlain is a w×h image of n colours (a power of two) in a global colour
// table, as an ordinary encoder writes it.
func gifPlain(w, h, n int, seed int64) gifFile {
	lit := max(int(gifTableBits(make([]byte, 3*n)))+1, 2)
	return gifFile{sw: w, sh: h, global: gifColors(n, seed), w: w, h: h, lit: lit,
		data: gifBlocks(gifCompress(gifIndexes(w, h, n, seed+1), lit))}
}

// The extensions.
func gifGraphicControl(flags byte, index byte) []byte {
	return []byte{0x21, 0xf9, 4, flags, 10, 0, index, 0}
}

func gifComment(text string, sizes ...int) []byte {
	return append([]byte{0x21, 0xfe}, gifBlocks([]byte(text), sizes...)...)
}

func gifApplication(id string, blocks []byte) []byte {
	b := append([]byte{0x21, 0xff, byte(len(id))}, id...)
	return append(b, blocks...)
}

func gifPlainText(text string) []byte {
	b := []byte{0x21, 0x01, 12, 0, 0, 0, 0, 8, 0, 8, 0, 8, 8, 1, 0}
	return append(b, gifBlocks([]byte(text))...)
}

// ---- The files -----------------------------------------------------------

// gifEncoded are images written by image/gif.
func gifEncoded(add func(name string, data []byte)) {
	encode := func(name string, g *gif.GIF) {
		var buf bytes.Buffer
		if err := gif.EncodeAll(&buf, g); err != nil {
			panic(name + ": " + err.Error())
		}
		add("enc-"+name, buf.Bytes())
	}
	palette := func(n int, seed int64) color.Palette {
		t := gifColors(n, seed)
		p := make(color.Palette, n)
		for i := range p {
			p[i] = color.RGBA{t[3*i], t[3*i+1], t[3*i+2], 0xff}
		}
		return p
	}
	paletted := func(r image.Rectangle, p color.Palette, seed int64) *image.Paletted {
		m := image.NewPaletted(r, p)
		copy(m.Pix, gifIndexes(r.Dx(), r.Dy(), len(p), seed))
		return m
	}
	one := func(m *image.Paletted) *gif.GIF {
		return &gif.GIF{Image: []*image.Paletted{m}, Delay: []int{0},
			Config: image.Config{ColorModel: m.Palette, Width: m.Rect.Max.X, Height: m.Rect.Max.Y}}
	}
	// palettes of 2 to 256 colours
	for _, n := range []int{2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 64, 65, 100, 127, 128, 129, 200, 255, 256} {
		encode(fmt.Sprintf("palette-%d", n), one(paletted(image.Rect(0, 0, 23, 17), palette(n, int64(n)), int64(n))))
	}
	// sizes
	for _, s := range [][2]int{{1, 1}, {1, 2}, {2, 1}, {1, 9}, {9, 1}, {2, 2}, {3, 5}, {8, 8}, {64, 64}, {65, 63}, {257, 3}, {3, 257}, {120, 90}} {
		encode(fmt.Sprintf("size-%dx%d", s[0], s[1]), one(paletted(image.Rect(0, 0, s[0], s[1]), palette(64, 7), int64(s[0]*1000+s[1]))))
	}
	// what compresses to long codes: one colour, and stripes
	flat := image.NewPaletted(image.Rect(0, 0, 200, 150), palette(4, 3))
	encode("flat", one(flat))
	stripes := image.NewPaletted(image.Rect(0, 0, 150, 120), palette(8, 3))
	for i := range stripes.Pix {
		stripes.Pix[i] = byte(i % 150 / 19)
	}
	encode("stripes", one(stripes))
	// noise, which does not compress: the table fills up and is cleared
	noise := image.NewPaletted(image.Rect(0, 0, 130, 110), palette(256, 5))
	rand.New(rand.NewSource(11)).Read(noise.Pix)
	encode("noise", one(noise))
	// a transparent colour: the encoder writes a graphic control for it
	for _, at := range []int{0, 1, 7, 15} {
		p := palette(16, 21)
		p[at] = color.RGBA{}
		encode(fmt.Sprintf("transparent-%d", at), one(paletted(image.Rect(0, 0, 19, 13), p, 22)))
	}
	// a frame smaller than the screen, and away from its corner
	for i, r := range []image.Rectangle{image.Rect(0, 0, 10, 8), image.Rect(5, 3, 20, 12), image.Rect(11, 0, 31, 9), image.Rect(0, 7, 6, 23), image.Rect(30, 22, 31, 23)} {
		m := paletted(r, palette(32, 31), int64(40+i))
		encode(fmt.Sprintf("offset-%d", i), &gif.GIF{Image: []*image.Paletted{m}, Delay: []int{0},
			Config: image.Config{ColorModel: m.Palette, Width: 31, Height: 23}})
	}
	// a global table and a local one, a local one alone, the global one alone
	global, local := palette(16, 51), palette(8, 52)
	encode("local-and-global", &gif.GIF{Image: []*image.Paletted{paletted(image.Rect(0, 0, 14, 11), local, 53)}, Delay: []int{0},
		Config: image.Config{ColorModel: global, Width: 14, Height: 11}})
	encode("local-only", &gif.GIF{Image: []*image.Paletted{paletted(image.Rect(0, 0, 14, 11), local, 54)}, Delay: []int{0},
		Config: image.Config{Width: 14, Height: 11}})
	// several frames, with delays, disposal and a loop count; the first decides
	frames := &gif.GIF{LoopCount: 3, Config: image.Config{ColorModel: global, Width: 40, Height: 30}}
	for i, r := range []image.Rectangle{image.Rect(4, 2, 36, 28), image.Rect(0, 0, 40, 30), image.Rect(10, 10, 20, 20)} {
		p := global
		if i == 1 {
			p = local
		}
		frames.Image = append(frames.Image, paletted(r, p, int64(60+i)))
		frames.Delay = append(frames.Delay, 5*i+1)
		frames.Disposal = append(frames.Disposal, byte(i+1))
	}
	encode("frames", frames)
	// gif.Encode of an image that is not paletted: quantized and dithered
	rgba := image.NewRGBA(image.Rect(0, 0, 33, 21))
	rand.New(rand.NewSource(71)).Read(rgba.Pix)
	for i := 3; i < len(rgba.Pix); i += 4 {
		rgba.Pix[i] = 0xff
	}
	for _, n := range []int{2, 6, 256} {
		var buf bytes.Buffer
		if err := gif.Encode(&buf, rgba, &gif.Options{NumColors: n, Drawer: draw.FloydSteinberg}); err != nil {
			panic(err)
		}
		add(fmt.Sprintf("enc-quantized-%d", n), buf.Bytes())
	}
}

// gifBuilt are files no encoder here writes, built byte by byte: the
// layouts gif.Decode accepts and the ones next to them that it refuses.
func gifBuilt(add func(name string, data []byte)) {
	put := func(name string, g gifFile) { add("hand-"+name, g.bytes()) }

	// colour tables of every size: global, local, both, none
	for bits := 1; bits <= 8; bits++ {
		n := 1 << bits
		g := gifPlain(13, 9, n, int64(bits))
		put(fmt.Sprintf("global-%d", n), g)
		l := g
		l.global, l.local = nil, g.global
		put(fmt.Sprintf("local-%d", n), l)
		both := g
		both.global, both.local = gifColors(256, 99), g.global
		put(fmt.Sprintf("both-%d", n), both)
	}
	none := gifPlain(13, 9, 4, 1)
	none.global = nil
	put("no-table", none)

	// files that end in the header, after it, and in the global colour table
	{
		whole := gifPlain(13, 9, 16, 1).bytes()
		for _, n := range []int{0, 5, 6, 12, 13, 14, 13 + 47, 13 + 48, 13 + 49} {
			add(fmt.Sprintf("hand-cut-%d", n), whole[:n])
		}
		// without a global table the header alone has a size
		for _, n := range []int{12, 13, 14} {
			add(fmt.Sprintf("hand-cut-%d-no-table", n), none.bytes()[:n])
		}
	}

	// the versions
	for _, v := range []string{"GIF87a", "GIF88a", "GIF89b", "gif89a", "GIF89\x00"} {
		g := gifPlain(6, 5, 8, 2)
		g.version = v
		put("version-"+fmt.Sprintf("%x", v), g)
	}

	// a pixel past the palette: the palette is shorter than the code size allows
	{
		pix := gifIndexes(9, 7, 4, 3)
		g := gifFile{sw: 9, sh: 7, global: gifColors(4, 3), w: 9, h: 7, lit: 8}
		g.data = gifBlocks(gifCompress(pix, 8))
		put("lit8-palette4", g)
		bad := append([]byte(nil), pix...)
		bad[20] = 200
		g.data = gifBlocks(gifCompress(bad, 8))
		put("pixel-past-palette", g)
		bad[20] = 4
		g.data = gifBlocks(gifCompress(bad, 8))
		put("pixel-just-past-palette", g)
		// a transparent index past the palette lengthens it: the pixels up to it count
		bad[20] = 200
		g.data = gifBlocks(gifCompress(bad, 8))
		for _, ti := range []byte{4, 199, 200, 201, 255} {
			g.before = gifGraphicControl(1, ti)
			put(fmt.Sprintf("pixel-past-palette-transparent-%d", ti), g)
		}
		// and the local table decides, not the global one
		g.before, g.global, g.local = nil, gifColors(256, 4), gifColors(4, 3)
		put("pixel-past-local-palette", g)
	}

	// extensions of every kind before the image
	ext := func(name string, before ...[]byte) {
		g := gifPlain(8, 6, 16, 5)
		g.before = bytes.Join(before, nil)
		put("ext-"+name, g)
	}
	netscape := gifApplication("NETSCAPE2.0", []byte{3, 1, 5, 0, 0})
	ext("comment", gifComment("a comment"))
	ext("comment-empty", gifComment(""))
	ext("comment-blocks", gifComment("a comment that goes over several sub-blocks", 1, 2, 3))
	ext("comment-long", gifComment(string(bytes.Repeat([]byte("0123456789"), 60))))
	ext("netscape", netscape)
	ext("netscape-empty", gifApplication("NETSCAPE2.0", []byte{0}))
	ext("netscape-other", gifApplication("NETSCAPE2.0", []byte{2, 1, 5, 4, 1, 2, 3, 4, 0}))
	ext("netscape-short", gifApplication("NETSCAPE2.", []byte{3, 1, 5, 0, 0}))
	ext("application-xmp", gifApplication("XMP DataXMP", gifBlocks(bytes.Repeat([]byte("x"), 300))))
	ext("application-size0", gifApplication("", gifBlocks([]byte("data"))))
	ext("application-size255", gifApplication(string(bytes.Repeat([]byte("i"), 255)), []byte{0}))
	ext("plain-text", gifPlainText("hello"))
	ext("graphic-control", gifGraphicControl(0, 0))
	ext("graphic-control-disposal", gifGraphicControl(3<<2, 9))
	ext("transparent-0", gifGraphicControl(1, 0))
	ext("transparent-15", gifGraphicControl(1, 15))
	ext("transparent-16", gifGraphicControl(1, 16))
	ext("transparent-255", gifGraphicControl(1, 255))
	// the transparent index of a graphic control stays when the next has none
	ext("transparent-then-not", gifGraphicControl(1, 3), gifGraphicControl(0, 9))
	ext("transparent-twice", gifGraphicControl(1, 3), gifGraphicControl(1, 9))
	ext("not-then-transparent", gifGraphicControl(0, 3), gifGraphicControl(9, 2))
	ext("graphic-control-size5", []byte{0x21, 0xf9, 5, 1, 0, 0, 3, 0, 0})
	ext("graphic-control-size3", []byte{0x21, 0xf9, 3, 1, 0, 0, 0})
	ext("graphic-control-terminator", []byte{0x21, 0xf9, 4, 1, 0, 0, 3, 1})
	ext("unknown-label", []byte{0x21, 0x02, 0})
	ext("unknown-block", []byte{0x00})
	ext("trailer-first", []byte{0x3b})
	ext("all", gifComment("one"), netscape, gifGraphicControl(1, 2), gifPlainText("text"), gifApplication("ICCRGBG1012", gifBlocks(make([]byte, 600))), gifComment("two"))
	// a local table with a transparent index: the table is not cloned, the entry is cleared
	{
		g := gifPlain(8, 6, 16, 5)
		g.global, g.local, g.before = nil, g.global, gifGraphicControl(1, 5)
		put("ext-transparent-local", g)
		g.before = gifGraphicControl(1, 77)
		put("ext-transparent-past-local", g)
	}

	// the frame and the logical screen
	frame := func(name string, sw, sh, x, y, w, h int) {
		pix := gifIndexes(w, h, 8, 6)
		put("frame-"+name, gifFile{sw: sw, sh: sh, global: gifColors(8, 6), x: x, y: y, w: w, h: h, lit: 3,
			data: gifBlocks(gifCompress(pix, 3))})
	}
	frame("smaller", 20, 15, 0, 0, 7, 5)
	frame("offset", 20, 15, 6, 4, 7, 5)
	frame("corner", 20, 15, 13, 10, 7, 5)
	frame("right-of-screen", 20, 15, 14, 10, 7, 5)
	frame("below-screen", 20, 15, 13, 11, 7, 5)
	frame("wider-than-screen", 6, 15, 0, 0, 7, 5)
	frame("outside", 20, 15, 30, 30, 7, 5)
	frame("empty", 20, 15, 3, 3, 0, 0)
	frame("no-width", 20, 15, 3, 3, 0, 5)
	frame("no-height", 20, 15, 3, 3, 7, 0)
	frame("empty-outside", 20, 15, 30, 30, 0, 0)
	frame("screen-0x0", 0, 0, 0, 0, 0, 0)
	frame("screen-0x0-frame", 0, 0, 0, 0, 7, 5)
	frame("screen-0xN", 0, 15, 0, 0, 0, 5)
	frame("screen-Nx0", 20, 0, 0, 0, 7, 0)
	// the largest screens the reference decodes in, and the ones past them
	frame("screen-10240x10240", 10240, 10240, 9000, 10000, 7, 5)
	frame("screen-10241x10240", 10241, 10240, 9000, 10000, 7, 5)
	frame("screen-65535x1600", 65535, 1600, 65528, 1595, 7, 5)
	frame("screen-65535x1601", 65535, 1601, 65528, 1595, 7, 5)
	frame("screen-1x65535", 1, 65535, 0, 65530, 1, 5)
	frame("screen-65535x65535", 65535, 65535, 0, 0, 7, 5)
	// a frame of many pixels with next to no data: refused without decoding
	put("frame-large-no-data", gifFile{sw: 10000, sh: 10000, global: gifColors(4, 1), w: 10000, h: 10000, lit: 2,
		data: gifBlocks(newGifCoder(2).literals([]byte{1, 2, 3}, 0).end().bytes())})
	put("frame-large-little-data", gifFile{sw: 3000, sh: 3000, global: gifColors(4, 1), w: 3000, h: 3000, lit: 2,
		data: gifBlocks(gifCompress(make([]byte, 100000), 2))})

	// what follows the image
	tail := func(name string, after []byte) {
		g := gifPlain(8, 6, 16, 7)
		g.after = after
		put("after-"+name, g)
	}
	tail("nothing", []byte{})
	tail("garbage", []byte{1, 2, 3, 4, 5})
	tail("unknown-block", []byte{0x55, 0x3b})
	second := gifPlain(8, 6, 16, 8).bytes()
	tail("second-frame", second[13+48:])
	tail("second-frame-cut", second[13+48:len(second)-9])
	tail("bad-extension", []byte{0x21, 0x77, 0x3b})

	// the layout of the sub-blocks
	pix := gifIndexes(20, 15, 32, 9)
	stream := gifCompress(pix, 5)
	if len(stream) < 120 || len(stream) > 240 {
		panic(fmt.Sprintf("the stream of the layout tests is %d bytes: it has to fill several sub-blocks of 50 and fit one with padding", len(stream)))
	}
	layout := func(name string, data []byte, trailer bool) {
		g := gifFile{sw: 20, sh: 15, global: gifColors(32, 9), w: 20, h: 15, lit: 5, data: data}
		if !trailer {
			g.after = []byte{}
		}
		put("blocks-"+name, g)
	}
	layout("255", gifBlocks(stream), true)
	layout("50", gifBlocks(stream, 50), true)
	layout("1", gifBlocks(stream, 1), true)
	layout("2", gifBlocks(stream, 2), true)
	layout("mixed", gifBlocks(stream, 1, 7, 100, 2, 31), true)
	// the stream in sub-blocks that end exactly with it, then what is given
	whole := func(extra ...byte) []byte {
		b := gifBlocks(stream, 97)
		return append(b[:len(b)-1], extra...)
	}
	// after data that fills its sub-block Go takes one more sub-block of one byte
	layout("exact-then-terminator", whole(0), true)
	layout("exact-then-1", whole(1, 0xaa, 0), true)
	layout("exact-then-2", whole(2, 0xaa, 0xbb, 0), true)
	layout("exact-then-1-1", whole(1, 0xaa, 1, 0xbb, 0), true)
	layout("exact-then-1-cut", whole(1, 0xaa), false)
	layout("exact-then-1-no-terminator", whole(1, 0xaa), true)
	layout("exact-then-cut", whole(), false)
	layout("no-terminator", whole(), true)
	// data that leaves a part of its sub-block unread: the rest is skipped
	padded := func(n int, extra ...byte) []byte {
		b := gifBlocks(append(append([]byte(nil), stream...), make([]byte, n)...), len(stream)+n)
		return append(b[:len(b)-1], extra...)
	}
	layout("rest-1", padded(1, 0), true)
	layout("rest-9", padded(9, 0), true)
	layout("rest-then-1", padded(3, 1, 0xaa, 0), true)
	layout("rest-then-2", padded(3, 2, 0xaa, 0xbb, 0), true)
	layout("rest-no-terminator", padded(3), true)
	layout("rest-cut", padded(3), false)
	// a sub-block that says more than the file has
	layout("short-block", append(gifBlocks(stream[:50], 50)[:51], 200, 1, 2, 3), false)
	layout("no-data", []byte{0}, true)
	layout("no-data-no-terminator", []byte{}, false)

	// LZW streams
	lzwFile := func(name string, w, h, lit int, stream []byte, sizes ...int) {
		put("lzw-"+name, gifFile{sw: w, sh: h, global: gifColors(256, 10), w: w, h: h, lit: lit, data: gifBlocks(stream, sizes...)})
	}
	small := gifIndexes(12, 10, 4, 11) // 120 pixels of 4 colours
	c := func() *gifCoder { return newGifCoder(2) }
	clr := 4
	lzwFile("literals", 12, 10, 2, c().code(clr).literals(small, 0).end().bytes())
	lzwFile("no-clear-first", 12, 10, 2, c().literals(small, 0).end().bytes())
	lzwFile("clear-every-1", 12, 10, 2, c().literals(small, 1).end().bytes())
	lzwFile("clear-every-2", 12, 10, 2, c().literals(small, 2).end().bytes())
	lzwFile("clear-every-3", 12, 10, 2, c().literals(small, 3).end().bytes())
	lzwFile("clear-every-7", 12, 10, 2, c().literals(small, 7).end().bytes())
	lzwFile("clear-twice", 12, 10, 2, c().code(clr, clr, clr).literals(small, 0).end().bytes())
	lzwFile("clear-before-end", 12, 10, 2, c().literals(small, 0).code(clr).end().bytes())
	lzwFile("clears-before-end", 12, 10, 2, c().literals(small, 0).code(clr, clr, clr).end().bytes())
	// without an end code: the sub-blocks end, which Go accepts. A clear code and a literal
	// for every pixel are six bits, so that the stream ends with a whole byte and a literal.
	if s := c().literals(small, 1); s.nBits != 0 {
		panic("the stream without an end code does not end with a whole byte")
	}
	lzwFile("no-end", 12, 10, 2, c().literals(small, 1).bytes())
	lzwFile("no-end-clears", 12, 10, 2, c().literals(small, 0).align().bytes())
	lzwFile("end-twice", 12, 10, 2, c().literals(small, 0).end().end().bytes())
	lzwFile("end-then-codes", 12, 10, 2, c().literals(small, 0).end().code(1, 2, 3, 7).bytes())
	lzwFile("end-first", 12, 10, 2, c().end().literals(small, 0).bytes())
	lzwFile("clear-end-first", 12, 10, 2, c().code(clr).end().literals(small, 0).bytes())
	// too little and too much
	lzwFile("one-short", 12, 10, 2, c().literals(small[:119], 0).end().bytes())
	lzwFile("one-short-no-end", 12, 10, 2, c().literals(small[:119], 1).bytes())
	lzwFile("row-short", 12, 10, 2, c().literals(small[:108], 0).end().bytes())
	lzwFile("one-pixel", 12, 10, 2, c().literals(small[:1], 0).end().bytes())
	lzwFile("one-more", 12, 10, 2, c().literals(small, 0).code(1).end().bytes())
	lzwFile("one-more-no-end", 12, 10, 2, c().literals(small, 1).code(clr, 1).align().bytes())
	lzwFile("many-more", 12, 10, 2, c().literals(append(append([]byte(nil), small...), gifIndexes(100, 90, 4, 12)...), 0).end().bytes())
	// invalid codes: past the table, at every place
	lzwFile("invalid-first", 12, 10, 2, c().code(6).literals(small, 0).end().bytes())
	lzwFile("invalid-after-clear", 12, 10, 2, c().code(clr, 6).literals(small, 0).end().bytes())
	lzwFile("invalid-second", 12, 10, 2, c().code(clr, 1, 7).literals(small, 0).end().bytes())
	lzwFile("invalid-middle", 12, 10, 2, c().literals(small[:60], 0).invalid().literals(small[60:], 0).end().bytes())
	lzwFile("invalid-after-pixels", 12, 10, 2, c().literals(small, 0).invalid().bytes())
	lzwFile("invalid-after-pixels-and-clear", 12, 10, 2, c().literals(small, 0).code(clr, 6).bytes())
	lzwFile("invalid-after-end", 12, 10, 2, c().literals(small, 0).end().invalid().bytes())
	// the code that is about to be entered: the expansion before it and its first pixel
	// again, here 1 pixel and then 2, 3, ... 15 of them
	{
		hi := c().code(clr, 1)
		for code := 6; code <= 19; code++ {
			hi.code(code)
		}
		if hi.pixels != 120 {
			panic(fmt.Sprintf("the codes of lzw-hi expand to %d pixels", hi.pixels))
		}
		lzwFile("hi", 12, 10, 2, hi.end().bytes())
	}
	lzwFile("hi-first", 12, 10, 2, c().code(clr, 5).bytes()) // hi is the end code then
	lzwFile("hi-mixed", 12, 10, 2, c().random(rand.New(rand.NewSource(13)), 120, 0).end().bytes())
	// the wrong minimum code size
	for _, lit := range []int{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12, 255} {
		lzwFile(fmt.Sprintf("size-%d-for-5", lit), 20, 15, lit, stream)
		lzwFile(fmt.Sprintf("size-%d-for-2", lit), 12, 10, lit, gifCompress(small, 2))
		lzwFile(fmt.Sprintf("size-%d-for-8", lit), 12, 10, lit, gifCompress(small, 8))
	}
	// every code size, with codes drawn at random and clear codes now and then
	for lit := 2; lit <= 8; lit++ {
		for seed := int64(0); seed < 3; seed++ {
			rng := rand.New(rand.NewSource(100*int64(lit) + seed))
			lzwFile(fmt.Sprintf("random-%d-%d", lit, seed), 31, 29, lit, newGifCoder(lit).random(rng, 31*29, int(seed)*40).end().bytes(), 1+rng.Intn(255), 1+rng.Intn(255))
		}
	}
	// 4096 pixels and the ones next to it: lzw.Reader hands its output on every 4096 bytes and
	// goes on decoding when asked for the byte that must not be there
	for _, n := range []int{4095, 4096, 4097} {
		w, h := n, 1
		if n == 4096 {
			w, h = 64, 64
		}
		many := gifIndexes(w, h, 4, int64(n))
		chunk := func(name string, rest func(c *gifCoder)) {
			s := c().literals(many, 50)
			rest(s)
			lzwFile(fmt.Sprintf("chunk-%d-%s", n, name), w, h, 2, s.bytes())
		}
		chunk("end", func(c *gifCoder) { c.end() })
		chunk("no-end", func(c *gifCoder) { c.align() })
		chunk("clear-end", func(c *gifCoder) { c.code(clr).end() })
		chunk("invalid", func(c *gifCoder) { c.invalid() })
		chunk("one-more", func(c *gifCoder) { c.code(1).end() })
		chunk("clear-one-more", func(c *gifCoder) { c.code(clr, 1).end() })
		chunk("clear-invalid", func(c *gifCoder) { c.code(clr, 7) })
		chunk("cut", func(c *gifCoder) { c.out = c.out[:1000] })
		lzwFile(fmt.Sprintf("chunk-%d-long-codes", n), w, h, 2, gifCompress(make([]byte, n), 2))
		lzwFile(fmt.Sprintf("chunk-%d-long-codes-more", n), w, h, 2, gifCompress(make([]byte, n+1), 2))
	}
	// a table that fills up and is not cleared: nothing more is entered, every code stays
	for lit := 2; lit <= 8; lit += 3 {
		rng := rand.New(rand.NewSource(int64(lit)))
		full := newGifCoder(lit)
		full.literals(gifIndexes(4200, 1, 1<<lit, 14), 0) // more literals than the table has room for
		if full.hi != 4095 || full.last != -1 {
			panic("the table of lzw-full is not full")
		}
		full.code(4095, 4094, 1, 4095, 1<<lit+2, 4095)
		full.random(rng, 100*90, 0)
		lzwFile(fmt.Sprintf("full-%d", lit), 100, 90, lit, full.end().bytes())
		// and cleared after it was full
		full = newGifCoder(lit)
		full.literals(gifIndexes(4200, 1, 1<<lit, 15), 0).code(4095, 1<<lit)
		full.random(rng, 100*90, 3000)
		lzwFile(fmt.Sprintf("full-cleared-%d", lit), 100, 90, lit, full.end().bytes())
	}

	// the code that is about to be entered when it is the last the table has room for
	for lit := 2; lit <= 8; lit += 3 {
		rng := rand.New(rand.NewSource(int64(lit)))
		last := newGifCoder(lit)
		for _, p := range gifIndexes(4200, 1, 1<<lit, 18) {
			if last.hi == 4095 {
				break
			}
			last.code(int(p))
		}
		if last.hi != 4095 || last.last < 0 {
			panic("the table of lzw-full-hi-last has not one entry left")
		}
		last.code(4095)
		if last.last != -1 {
			panic("the table of lzw-full-hi-last is not full")
		}
		last.code(4095, 4095)
		last.random(rng, 100*90, 0)
		lzwFile(fmt.Sprintf("full-hi-last-%d", lit), 100, 90, lit, last.end().bytes())
	}

	// interlaced frames of every height up to 17, and some wider ones
	interlaced := func(w, h int) {
		p := gifIndexes(w, h, 16, int64(w*100+h))
		g := gifFile{sw: w, sh: h, global: gifColors(16, 16), w: w, h: h, lit: 4, interlace: true,
			data: gifBlocks(gifCompress(gifInterlace(p, w, h), 4))}
		put(fmt.Sprintf("interlaced-%dx%d", w, h), g)
	}
	for h := 1; h <= 17; h++ {
		interlaced(5, h)
	}
	interlaced(1, 1)
	interlaced(33, 32)
	interlaced(70, 61)
	interlaced(1, 40)
	{
		// interlaced, offset in a larger screen, with a transparent index and a local table
		p := gifIndexes(21, 19, 8, 17)
		put("interlaced-offset-transparent", gifFile{sw: 50, sh: 40, global: gifColors(4, 1), before: gifGraphicControl(1, 3),
			x: 9, y: 11, w: 21, h: 19, local: gifColors(8, 17), lit: 3, interlace: true,
			data: gifBlocks(gifCompress(gifInterlace(p, 21, 19), 3), 33)})
		// interlaced and short of data
		put("interlaced-short", gifFile{sw: 21, sh: 19, global: gifColors(8, 17), w: 21, h: 19, lit: 3, interlace: true,
			data: gifBlocks(gifCompress(p[:300], 3))})
	}
}

// gifMade are the files the test makes itself.
func gifMade() map[string][]byte {
	out := map[string][]byte{}
	add := func(name string, data []byte) {
		if _, dup := out[name]; dup {
			panic("two files are named " + name)
		}
		out[name] = data
	}
	gifEncoded(add)
	gifBuilt(add)
	return out
}

// gifFiles are all the files the decoders are compared on: the image parts
// of the reference's documents, the testdata of the Go tree, what another
// encoder wrote (testdata/gen, see gen.sh) and what the test makes.
func gifFiles(t testing.TB) map[string][]byte {
	out := withDocumentImages(t, "gif", []string{goroot() + "/image/testdata", "testdata/images"}, ".gif")
	if len(out) < 20 {
		t.Fatalf("only %d GIF files were found on disk", len(out))
	}
	for name, data := range gifMade() {
		out[name] = data
	}
	return out
}

// ---- The tests -----------------------------------------------------------

func TestGifFiles(t *testing.T) {
	checkFiles(t, "gif", gifFiles(t))
}

func TestGifMutations(t *testing.T) {
	checkMutations(t, "gif", gifFiles(t))
}

func FuzzGif(f *testing.F) {
	fuzzDecode(f, "gif", gifFiles(f))
}

// TestGifExpected checks the files of this test against what they were
// made to be: a file meant to decode does, and to the pixels it was built
// from, so that the comparison of the decoders is not one of two refusals.
func TestGifExpected(t *testing.T) {
	made := gifMade()
	status := func(name string) int {
		data, ok := made[name]
		if !ok {
			t.Fatalf("no file named %s", name)
		}
		_, _, _, s := reference("gif", data)
		return s
	}
	for name, want := range map[string]int{
		"hand-global-2": statusOK, "hand-local-256": statusOK, "hand-both-16": statusOK, "hand-no-table": statusFormat,
		"hand-version-474946383761": statusOK, "hand-version-474946383861": statusFormat,
		"hand-lit8-palette4": statusOK, "hand-pixel-past-palette": statusFormat, "hand-pixel-just-past-palette": statusFormat,
		"hand-pixel-past-palette-transparent-199": statusFormat, "hand-pixel-past-palette-transparent-200": statusOK,
		"hand-pixel-past-palette-transparent-255": statusOK, "hand-pixel-past-local-palette": statusFormat,
		"hand-ext-comment": statusOK, "hand-ext-comment-empty": statusOK, "hand-ext-netscape": statusOK, "hand-ext-netscape-empty": statusOK,
		"hand-ext-application-size0": statusOK, "hand-ext-application-size255": statusOK, "hand-ext-plain-text": statusOK,
		"hand-ext-transparent-255": statusOK, "hand-ext-transparent-then-not": statusOK, "hand-ext-graphic-control-size5": statusFormat,
		"hand-ext-graphic-control-terminator": statusFormat, "hand-ext-unknown-label": statusFormat, "hand-ext-unknown-block": statusFormat,
		"hand-ext-trailer-first": statusFormat, "hand-ext-all": statusOK, "hand-ext-transparent-past-local": statusOK,
		"hand-frame-smaller": statusOK, "hand-frame-corner": statusOK, "hand-frame-right-of-screen": statusFormat,
		"hand-frame-below-screen": statusFormat, "hand-frame-empty": statusFormat, "hand-frame-no-width": statusFormat,
		"hand-frame-screen-0x0": statusTooLarge, "hand-frame-screen-0xN": statusTooLarge, "hand-frame-screen-Nx0": statusTooLarge,
		"hand-frame-screen-10240x10240": statusOK, "hand-frame-screen-10241x10240": statusTooLarge,
		"hand-frame-screen-65535x1600": statusOK, "hand-frame-screen-65535x1601": statusTooLarge, "hand-frame-screen-1x65535": statusOK,
		"hand-frame-screen-65535x65535": statusTooLarge, "hand-frame-large-no-data": statusFormat, "hand-frame-large-little-data": statusFormat,
		"hand-after-nothing": statusOK, "hand-after-garbage": statusOK, "hand-after-second-frame-cut": statusOK, "hand-after-bad-extension": statusOK,
		"hand-blocks-1": statusOK, "hand-blocks-mixed": statusOK, "hand-blocks-exact-then-terminator": statusOK,
		"hand-blocks-exact-then-1": statusOK, "hand-blocks-exact-then-2": statusFormat, "hand-blocks-exact-then-1-1": statusFormat,
		"hand-blocks-exact-then-1-cut": statusFormat, "hand-blocks-exact-then-cut": statusFormat, "hand-blocks-no-terminator": statusFormat,
		"hand-blocks-rest-1": statusOK, "hand-blocks-rest-9": statusOK, "hand-blocks-rest-then-1": statusFormat, "hand-blocks-rest-cut": statusFormat,
		"hand-blocks-short-block": statusFormat, "hand-blocks-no-data": statusFormat,
		"hand-lzw-literals": statusOK, "hand-lzw-no-clear-first": statusOK, "hand-lzw-clear-every-1": statusOK, "hand-lzw-clear-twice": statusOK,
		"hand-lzw-clear-before-end": statusOK, "hand-lzw-clears-before-end": statusOK, "hand-lzw-no-end-clears": statusOK, "hand-lzw-no-end": statusOK,
		"hand-lzw-end-twice": statusOK, "hand-lzw-end-then-codes": statusOK, "hand-lzw-end-first": statusFormat,
		"hand-lzw-one-short": statusFormat, "hand-lzw-one-short-no-end": statusFormat, "hand-lzw-one-more": statusFormat,
		"hand-lzw-one-more-no-end": statusFormat, "hand-lzw-many-more": statusFormat,
		"hand-lzw-invalid-first": statusFormat, "hand-lzw-invalid-middle": statusFormat, "hand-lzw-invalid-after-pixels": statusFormat,
		"hand-lzw-invalid-after-pixels-and-clear": statusFormat, "hand-lzw-invalid-after-end": statusOK,
		"hand-lzw-hi": statusOK, "hand-lzw-hi-first": statusFormat, "hand-lzw-hi-mixed": statusOK,
		"hand-lzw-size-5-for-5": statusOK, "hand-lzw-size-1-for-5": statusFormat, "hand-lzw-size-9-for-5": statusFormat,
		"hand-lzw-random-2-0": statusOK, "hand-lzw-random-5-1": statusOK, "hand-lzw-random-8-2": statusOK,
		"hand-blocks-50": statusOK, "hand-lzw-chunk-4095-end": statusOK, "hand-lzw-chunk-4096-end": statusOK, "hand-lzw-chunk-4097-end": statusOK,
		"hand-lzw-chunk-4096-no-end": statusOK, "hand-lzw-chunk-4096-clear-end": statusOK, "hand-lzw-chunk-4096-invalid": statusFormat,
		"hand-lzw-chunk-4095-invalid": statusFormat, "hand-lzw-chunk-4096-one-more": statusFormat, "hand-lzw-chunk-4096-clear-one-more": statusFormat,
		"hand-lzw-chunk-4096-clear-invalid": statusFormat, "hand-lzw-chunk-4096-long-codes": statusOK, "hand-lzw-chunk-4096-long-codes-more": statusFormat,
		"hand-cut-12": statusFormat, "hand-cut-13": statusFormat, "hand-cut-60": statusFormat, "hand-cut-61": statusFormat, "hand-cut-13-no-table": statusFormat,
		"hand-lzw-full-hi-last-2": statusOK, "hand-lzw-full-hi-last-5": statusOK, "hand-lzw-full-hi-last-8": statusOK,
		"hand-lzw-full-2": statusOK, "hand-lzw-full-5": statusOK, "hand-lzw-full-8": statusOK, "hand-lzw-full-cleared-2": statusOK, "hand-lzw-full-cleared-8": statusOK,
		"hand-interlaced-5x1": statusOK, "hand-interlaced-5x17": statusOK, "hand-interlaced-70x61": statusOK,
		"hand-interlaced-offset-transparent": statusOK, "hand-interlaced-short": statusFormat,
	} {
		if got := status(name); got != want {
			t.Errorf("%s: the reference returns status %d, the file was made for %d", name, got, want)
		}
	}
	// the pixels of a frame are the ones it was built from
	check := func(name string, w, h int, want func(x, y int) [4]byte) {
		t.Helper()
		pix, gw, gh, s := cDecode("gif", made[name])
		if s != statusOK || gw != w || gh != h {
			t.Errorf("%s: status %d, %dx%d, not a %dx%d image", name, s, gw, gh, w, h)
			return
		}
		for y := 0; y < h; y++ {
			for x := 0; x < w; x++ {
				if got := [4]byte(pix[4*(y*w+x):]); got != want(x, y) {
					t.Errorf("%s: pixel (%d, %d) is %v, not %v", name, x, y, got, want(x, y))
					return
				}
			}
		}
	}
	lookup := func(table []byte, index []byte, w int, transparent int) func(x, y int) [4]byte {
		return func(x, y int) [4]byte {
			i := int(index[y*w+x])
			if i == transparent || 3*i >= len(table) {
				return [4]byte{}
			}
			return [4]byte{table[3*i], table[3*i+1], table[3*i+2], 0xff}
		}
	}
	check("hand-interlaced-5x17", 5, 17, lookup(gifColors(16, 16), gifIndexes(5, 17, 16, 517), 5, -1))
	check("hand-interlaced-70x61", 70, 61, lookup(gifColors(16, 16), gifIndexes(70, 61, 16, 7061), 70, -1))
	check("hand-interlaced-offset-transparent", 21, 19, lookup(gifColors(8, 17), gifIndexes(21, 19, 8, 17), 21, 3))
	check("hand-frame-corner", 7, 5, lookup(gifColors(8, 6), gifIndexes(7, 5, 8, 6), 7, -1))
	check("hand-ext-transparent-then-not", 8, 6, lookup(gifColors(16, 5), gifIndexes(8, 6, 16, 6), 8, 3))
	check("hand-ext-transparent-twice", 8, 6, lookup(gifColors(16, 5), gifIndexes(8, 6, 16, 6), 8, 9))
	bad := gifIndexes(9, 7, 4, 3)
	bad[20] = 200
	check("hand-pixel-past-palette-transparent-255", 9, 7, lookup(gifColors(4, 3), bad, 9, 255))
	check("hand-lzw-chunk-4096-end", 64, 64, lookup(gifColors(256, 10), gifIndexes(64, 64, 4, 4096), 64, -1))

	// DecodeConfig has a size where the header is whole, and the global table if there is one
	for name, want := range map[string]bool{"hand-cut-12": false, "hand-cut-13": false, "hand-cut-60": false, "hand-cut-61": true,
		"hand-cut-12-no-table": false, "hand-cut-13-no-table": true, "hand-cut-14-no-table": true} {
		if _, _, ok := refConfig("gif", made[name]); ok != want {
			t.Errorf("%s: DecodeConfig of the reference succeeds: %v, the file was made for %v", name, ok, want)
		}
	}
	// a transparent index has pixels where the file was made to have them, and none where
	// the index is past the colours the pixels use
	transparent := func(name string, data []byte, want bool) {
		t.Helper()
		pix, _, _, s := cDecode("gif", data)
		if s != statusOK {
			t.Errorf("%s: status %d", name, s)
			return
		}
		n := 0
		for i := 3; i < len(pix); i += 4 {
			if pix[i] == 0 {
				n++
			}
		}
		if (n > 0) != want {
			t.Errorf("%s: %d transparent pixels", name, n)
		}
	}
	for name, want := range map[string]bool{"hand-ext-transparent-0": true, "hand-ext-transparent-15": true, "hand-ext-transparent-16": false,
		"hand-ext-transparent-255": false, "hand-ext-transparent-then-not": true, "hand-ext-transparent-twice": true,
		"hand-ext-not-then-transparent": true, "hand-ext-graphic-control": false, "hand-ext-graphic-control-disposal": false,
		"hand-ext-transparent-local": true, "hand-ext-transparent-past-local": false, "hand-ext-all": true,
		"enc-transparent-0": true, "enc-transparent-7": true, "enc-transparent-15": true, "enc-palette-16": false,
		"hand-pixel-past-palette-transparent-200": true, "hand-interlaced-offset-transparent": true} {
		transparent(name, made[name], want)
	}
	if data, err := os.ReadFile("testdata/images/transparent.gif"); err != nil {
		t.Error(err)
	} else {
		transparent("testdata/images/transparent.gif", data, true)
	}
}

// TestGifGenerated checks that the files of gen.sh are there and are the
// images they were made to be: the size is that of the first frame.
func TestGifGenerated(t *testing.T) {
	for name, want := range map[string][2]int{
		"plasma-256.gif": {48, 40}, "plasma-16.gif": {48, 40}, "plasma-5.gif": {47, 37}, "checkerboard.gif": {32, 32}, "gradient.gif": {48, 40},
		"red-1x1.gif": {1, 1}, "plasma-interlaced.gif": {47, 37}, "plasma-interlaced-3.gif": {21, 3}, "plasma-87a.gif": {30, 20},
		"transparent.gif": {40, 30}, "comment.gif": {40, 30}, "offset.gif": {20, 10}, "frames.gif": {40, 30}, "frames-optimized.gif": {40, 30},
	} {
		data, err := os.ReadFile(filepath.Join("testdata/images", name))
		if err != nil {
			t.Error(err)
			continue
		}
		if _, w, h, s := reference("gif", data); s != statusOK || w != want[0] || h != want[1] {
			t.Errorf("%s: the reference returns status %d and %dx%d, the file was made a %dx%d image", name, s, w, h, want[0], want[1])
		}
	}
}

// TestGifTruncated compares the decoders on every beginning of some files:
// the end of the data at each place the decoder can be in.
func TestGifTruncated(t *testing.T) {
	made := gifMade()
	n := 0
	for _, name := range []string{"enc-palette-16", "enc-transparent-7", "enc-frames", "enc-local-and-global", "hand-ext-all",
		"hand-interlaced-offset-transparent", "hand-blocks-1", "hand-blocks-exact-then-1", "hand-blocks-rest-9", "hand-lzw-no-end",
		"hand-lzw-clear-every-2", "hand-lzw-hi", "hand-lzw-chunk-4096-end", "hand-lzw-chunk-4095-no-end", "hand-lzw-full-cleared-2"} {
		data, ok := made[name]
		if !ok {
			t.Fatalf("no file named %s", name)
		}
		for i := 0; i <= len(data); i++ {
			if d := compare("gif", data[:i]); d != "" {
				t.Errorf("%s cut to %d bytes: %s", name, i, d)
				break
			}
			n++
		}
	}
	t.Logf("%d beginnings of files", n)
}

// TestGifRandom compares the decoders on files put together at random from
// parts that are each valid or nearly so: LIBBDF_IMAGE_CASES of them.
func TestGifRandom(t *testing.T) {
	cases, seed := envInt("LIBBDF_IMAGE_CASES", 2000), int64(envInt("LIBBDF_IMAGE_SEED", 1))
	rng := rand.New(rand.NewSource(seed))
	decoded, failed := 0, 0
	for i := 0; i < cases; i++ {
		data := gifRandom(rng)
		if _, _, _, s := reference("gif", data); s == statusOK {
			decoded++
		}
		if d := compare("gif", data); d != "" {
			failed++
			if failed <= 20 {
				t.Errorf("case %d of seed %d: %s", i, seed, d)
			}
			if dir := os.Getenv("LIBBDF_IMAGE_DUMP"); dir != "" {
				os.MkdirAll(dir, 0o755)
				os.WriteFile(filepath.Join(dir, fmt.Sprintf("gif-random-%d-%d", seed, i)), data, 0o644)
			}
		}
	}
	t.Logf("%d files, %d of them images the reference decodes, %d disagreements", cases, decoded, failed)
}

// gifRandom is a GIF whose every part is drawn at random.
func gifRandom(rng *rand.Rand) []byte {
	pick := func(n int) bool { return rng.Intn(n) == 0 }
	w, h := 1+rng.Intn(40), 1+rng.Intn(40)
	if pick(10) {
		w, h = 1+rng.Intn(300), 1+rng.Intn(60)
	}
	g := gifFile{sw: w + rng.Intn(4)*rng.Intn(10), sh: h + rng.Intn(4)*rng.Intn(10), w: w, h: h}
	g.x, g.y = rng.Intn(g.sw-w+1), rng.Intn(g.sh-h+1)
	if pick(30) {
		g.x += rng.Intn(3)
		g.y += rng.Intn(3)
	}
	if pick(20) {
		g.version = "GIF87a"
	}
	bits := 1 + rng.Intn(8)
	colors := 1 << bits
	switch rng.Intn(4) {
	case 0:
		g.local = gifColors(colors, rng.Int63())
		if pick(2) {
			g.global = gifColors(1<<(1+rng.Intn(8)), rng.Int63())
		}
	default:
		g.global = gifColors(colors, rng.Int63())
	}
	for !pick(2) {
		switch rng.Intn(8) {
		case 0:
			g.before = append(g.before, gifComment("comment", 1+rng.Intn(9))...)
		case 1:
			g.before = append(g.before, gifApplication("NETSCAPE2.0", []byte{3, 1, byte(rng.Intn(3)), 0, 0})...)
		case 2:
			g.before = append(g.before, gifPlainText("text")...)
		case 3:
			g.before = append(g.before, gifApplication(string(make([]byte, rng.Intn(20))), gifBlocks(make([]byte, rng.Intn(600)), 1+rng.Intn(255)))...)
		case 4, 5:
			g.before = append(g.before, gifGraphicControl(byte(rng.Intn(2)|rng.Intn(8)<<2), byte(rng.Intn(colors)))...)
		case 6:
			g.before = append(g.before, gifGraphicControl(1, byte(rng.Intn(256)))...)
		case 7:
			g.before = append(g.before, gifGraphicControl(byte(rng.Intn(256)), byte(rng.Intn(256)))...)
		}
	}
	g.lit = max(bits, 2)
	if pick(10) {
		g.lit = 2 + rng.Intn(7)
	}
	// the pixels: below the number of colours, or (now and then) below what the code size allows
	total := w * h
	if pick(15) {
		total += rng.Intn(5) - 2
	}
	if pick(40) {
		total += rng.Intn(9000) - 4500
	}
	total = max(total, 0)
	limit := min(colors, 1<<g.lit)
	if pick(15) {
		limit = 1 << g.lit
	}
	pix := make([]byte, total)
	for i := 0; i < total; {
		c := byte(rng.Intn(limit))
		for run := 1 + rng.Intn(1+rng.Intn(40)); run > 0 && i < total; run-- {
			pix[i] = c
			i++
		}
	}
	if pick(3) {
		g.interlace = true
		if total == w*h && pick(2) {
			pix = gifInterlace(pix, w, h)
		}
	}
	var stream []byte
	switch rng.Intn(4) {
	case 0:
		stream = gifCompress(pix, g.lit)
	case 1:
		c := newGifCoder(g.lit).literals(pix, rng.Intn(3)*rng.Intn(300))
		if !pick(4) {
			c.end()
		}
		stream = c.bytes()
	default:
		c := newGifCoder(g.lit)
		if pick(2) {
			c.code(c.clearCode())
		}
		c.random(rng, total, rng.Intn(3)*rng.Intn(500))
		for pick(8) {
			c.code(c.clearCode())
		}
		switch rng.Intn(12) {
		case 0:
		case 1:
			c.code(rng.Intn(1 << c.width))
		case 2:
			c.invalid()
		case 3:
			c.end().code(rng.Intn(8))
		default:
			c.end()
		}
		stream = c.bytes()
	}
	sizes := []int{255}
	if pick(2) {
		sizes = []int{1 + rng.Intn(255), 1 + rng.Intn(255), 1 + rng.Intn(4)}
	}
	g.data = gifBlocks(stream, sizes...)
	switch rng.Intn(12) {
	case 0:
		g.data = g.data[:len(g.data)-1]
	case 1:
		g.data = append(g.data[:len(g.data)-1], 1, 0x55, 0)
	case 2:
		g.data = append(g.data[:len(g.data)-1], 2, 0x55, 0x66, 0)
	case 3:
		g.data = append(g.data[:len(g.data)-1], 1, 0x55, 1, 0x66, 0)
	case 4:
		// some bytes more in the last sub-block
		n := rng.Intn(4) + 1
		if last := len(stream) % sizes[0]; len(sizes) == 1 && last > 0 && last+n < 256 {
			g.data = append(g.data[:len(g.data)-1], make([]byte, n)...)
			g.data[len(g.data)-n-last-1] += byte(n)
			g.data = append(g.data, 0)
		}
	}
	switch rng.Intn(6) {
	case 0:
		g.after = []byte{}
	case 1:
		g.after = []byte{0x2c, 0, 0}
	}
	return g.bytes()
}

// TestGifSpeed times the two decoders on a few files, when LIBBDF_IMAGE_SPEED
// is set, and compares them on those files: two of them are larger than any
// other file of the tests.
func TestGifSpeed(t *testing.T) {
	if os.Getenv("LIBBDF_IMAGE_SPEED") == "" {
		t.Skip("LIBBDF_IMAGE_SPEED is not set")
	}
	in := gifFiles(t)
	noise := make([]byte, 1000*800)
	rand.New(rand.NewSource(1)).Read(noise)
	in["speed-noise-1000x800"] = gifFile{sw: 1000, sh: 800, global: gifColors(256, 1), w: 1000, h: 800, lit: 8, data: gifBlocks(gifCompress(noise, 8))}.bytes()
	in["speed-runs-1000x800"] = gifFile{sw: 1000, sh: 800, global: gifColors(256, 1), w: 1000, h: 800, lit: 8, data: gifBlocks(gifCompress(gifIndexes(1000, 800, 256, 2), 8))}.bytes()
	in["speed-interlaced-1000x801"] = gifFile{sw: 1000, sh: 801, global: gifColors(256, 1), w: 1000, h: 801, lit: 8, interlace: true,
		data: gifBlocks(gifCompress(gifInterlace(gifIndexes(1000, 801, 256, 3), 1000, 801), 8))}.bytes()
	for _, name := range []string{"speed-interlaced-1000x801", goroot() + "/image/testdata/video-001.gif", goroot() + "/image/testdata/video-001.interlaced.gif",
		goroot() + "/image/testdata/video-005.gray.gif", "enc-noise", "enc-flat", "speed-noise-1000x800", "speed-runs-1000x800"} {
		data, ok := in[name]
		if !ok {
			t.Fatalf("no file named %s", name)
		}
		gifSpeedLine(t, "gif", name, data)
	}
}

// gifSpeedLine times the reference and the C decoder of a format on a file.
func gifSpeedLine(t *testing.T, format, name string, data []byte) {
	t.Helper()
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
	_, w, h, s := reference(format, data)
	if s != statusOK {
		t.Fatalf("%s does not decode", name)
	}
	if d := compare(format, data); d != "" {
		t.Errorf("%s: %s", name, d)
	}
	g := timeOf(func() { reference(format, data) })
	c := timeOf(func() { cDecode(format, data) })
	t.Logf("%-40s %4dx%-4d  Go %9v  C %9v  (C is %.1fx as fast)", filepath.Base(name), w, h, g, c, float64(g)/float64(c))
}
