package cbdf

import (
	"bytes"
	"fmt"
	"math"
	"math/rand/v2"
	"sort"

	"github.com/shibukawa/bdf"
	"github.com/shibukawa/bdf/font/woff2"
)

// The random drawings the rendering tests compare libbdf and the reference
// on, and go/genvectors makes the vectors of the C tests from.

func pickOf[T any](r *rand.Rand, from []T) T { return from[r.IntN(len(from))] }

// sortedHashes are the keys of a map in an order: a map has none, and the
// same seed has to make the same drawing.
func sortedHashes(m map[bdf.Hash]bdf.Rect) []bdf.Hash {
	keys := make([]bdf.Hash, 0, len(m))
	for h := range m {
		keys = append(keys, h)
	}
	sort.Slice(keys, func(i, j int) bool { return bytes.Compare(keys[i][:], keys[j][:]) < 0 })
	return keys
}

// drawing makes random drawings: every instruction of the vector core in
// an order no writer would choose.
type drawing struct {
	r      *rand.Rand
	o      *bdf.Object
	paths  []bdf.PathRef
	paints []bdf.PaintRef
	uses   []bdf.ObjRef
	images []bdf.ImageRef
	fonts  []bdf.FontRef
	depth  int
	groups int
	masks  int
}

func (d *drawing) c() float32 { return float32(d.r.IntN(2600)-300) / 10 } // -30 to 230

func (d *drawing) small() float32 { return float32(d.r.IntN(400)) / 10 }

func (d *drawing) color() bdf.Color {
	c := d.r.Uint32()
	switch d.r.IntN(4) {
	case 0:
		c |= 0xff // opaque
	case 1:
		c &^= 0xff // transparent
	}
	return bdf.Color(c)
}

func (d *drawing) path() *bdf.Path {
	p := &bdf.Path{}
	for range 1 + d.r.IntN(6) {
		switch d.r.IntN(12) {
		case 0, 1:
			p.MoveTo(d.c(), d.c())
		case 2, 3, 4:
			p.LineTo(d.c(), d.c())
		case 5:
			p.QuadTo(d.c(), d.c(), d.c(), d.c())
		case 6:
			p.CubicTo(d.c(), d.c(), d.c(), d.c(), d.c(), d.c())
		case 7:
			p.Close()
		case 8:
			p.Rect(d.c(), d.c(), d.small(), d.small())
		case 9:
			p.Ellipse(d.c(), d.c(), d.small(), d.small(), float32(d.r.NormFloat64()), float32(d.r.NormFloat64()*3), float32(d.r.NormFloat64()*3), d.r.IntN(2) == 0)
		case 10:
			p.ArcTo(d.c(), d.c(), d.c(), d.c(), d.small())
		case 11:
			p.RoundRect(d.c(), d.c(), d.small()*2, d.small()*2, d.small()/2)
		}
	}
	return p
}

func (d *drawing) paint() bdf.Paint {
	var stops []bdf.Stop
	for range d.r.IntN(5) {
		off := float32(d.r.IntN(13)-1) / 10 // some out of range, some the same
		stops = append(stops, bdf.Stop{Offset: off, Color: d.color()})
	}
	switch d.r.IntN(3) {
	case 0:
		return bdf.LinearGradient(d.c(), d.c(), d.c(), d.c(), stops...)
	case 1:
		return bdf.RadialGradient(d.c(), d.c(), d.small(), d.c(), d.c(), d.small()*2, stops...)
	}
	return bdf.ConicGradient(float32(d.r.NormFloat64()*2), d.c(), d.c(), stops...)
}

// pattern is a pattern of an image: repeated or not, scaled, turned and
// moved, and now and then with a matrix that maps the plane onto a line.
func (d *drawing) pattern(img bdf.ImageRef) bdf.Paint {
	r := d.r
	p := bdf.Pattern(img, byte(r.IntN(4)))
	switch r.IntN(6) {
	case 0:
		// as it is
	case 1:
		p.Matrix = [6]float32{0, 0, 0, 0, d.c(), d.c()}
	case 2:
		p.Matrix = [6]float32{2, 1, 4, 2, 0, 0}
	default:
		s := pickOf(r, []float32{0.1, 0.5, 1, 1.5, 4, -1})
		a := r.NormFloat64()
		if r.IntN(2) == 0 {
			a = 0
		}
		sin, cos := float32(math.Sin(a)), float32(math.Cos(a))
		p.Matrix = [6]float32{s * cos, s * sin, -s * sin, s * cos * pickOf(r, []float32{1, 1, 0.5, -1}), d.c(), d.c()}
	}
	return p
}

// newDrawing starts a drawing with paths and paints, and uses of the
// objects and images given.
func newDrawing(r *rand.Rand, children map[bdf.Hash]bdf.Rect, images []bdf.Hash, fonts []bdf.Hash) *drawing {
	d := &drawing{r: r, o: bdf.NewObject()}
	for range 2 + r.IntN(4) {
		d.paths = append(d.paths, d.o.AddPath(d.path()))
	}
	for range 1 + r.IntN(3) {
		d.paints = append(d.paints, d.o.AddPaint(d.paint()))
	}
	for _, h := range sortedHashes(children) {
		d.uses = append(d.uses, d.o.AddObject(h, children[h]))
	}
	// the images come last: a drawing without them is the drawing it was
	// before there were any
	if len(images) > 0 {
		for range 1 + r.IntN(3) {
			d.images = append(d.images, d.o.AddImage(pickOf(r, images)))
		}
		for range 1 + r.IntN(2) {
			d.paints = append(d.paints, d.o.AddPaint(d.pattern(pickOf(r, d.images))))
		}
		if r.IntN(8) == 0 {
			d.paints = append(d.paints, d.o.AddPaint(d.pattern(bdf.ImageRef(len(d.images)+r.IntN(3))))) // an image there is not
		}
	}
	// and the fonts after them: nil for a drawing without text, empty for
	// one whose fonts are all named
	if fonts != nil {
		for range 1 + r.IntN(4) {
			f := bdf.SystemFont(pickOf(r, textFamilies), pickOf(r, []uint16{100, 400, 400, 500, 600, 700, 900}), byte(r.IntN(3)))
			if len(fonts) > 0 && r.IntN(2) == 0 {
				f.Kind, f.Hash = bdf.FontEmbedded, pickOf(r, fonts)
			}
			d.fonts = append(d.fonts, d.o.AddFont(f))
		}
	}
	return d
}

// The families a random text names: ones the fonts of the tests have, ones
// they stand in for, generic ones, lists of them, and none at all.
var textFamilies = []string{
	"sans-serif", "serif", "monospace", "cursive", "system-ui, sans-serif", "DejaVu Sans", "dejavu-sans", "M PLUS 1p",
	"Test Sans", "Test Serif", "test mono, Test Sans", "Test Sans Bold",
	"STIX Two Math", "Arial", "Arial Black", "Helvetica Neue", "Times New Roman", "Courier New", "Meiryo", "MS Gothic",
	"\u6e38\u30b4\u30b7\u30c3\u30af", "'DejaVu Sans', serif", "\"M PLUS 1p\", sans-serif", "No Such Font", "No Such Font, monospace",
	"\uff24\uff45\uff4a\uff41\uff36\uff55 \uff33\uff41\uff4e\uff53", "", " , ,", "Sans-Serif", "MONOSPACE", "'quoted \\' name'", "Segoe UI Symbol",
}

// The strings a random text draws: words, other scripts, both directions,
// what a canvas draws as spaces, what no font has, and bytes that are no
// text.
var textStrings = []string{
	"Hello, world", "fi fl AV To", "0123456789", "a", " ", "The quick brown fox", "x\ty\nz", "caf\u00e9 na\u00efve",
	"\u65e5\u672c\u8a9e\u306e\u30c6\u30ad\u30b9\u30c8", "\u3053\u3093\u306b\u3061\u306f world", "\u05e9\u05dc\u05d5\u05dd", "abc \u05e9\u05dc\u05d5\u05dd def",
	"\u0645\u0631\u062d\u0628\u0627 123 abc", "\u05d0 1 2 \u05d1", "\u2211\u222b\u221a x\u00b2", "\U0001d49c\U0001f600", "e\u0301\u0323", "\u200f\u200e",
	"\ufffd", "(\u05d0)", "MM\u00d7WW",
}

// textStep sets a font or the style of text, or draws a string.
func (d *drawing) textStep() {
	r, o := d.r, d.o
	switch r.IntN(8) {
	case 0, 1:
		f := pickOf(r, d.fonts)
		if r.IntN(30) == 0 {
			f = bdf.FontRef(len(d.fonts) + r.IntN(2)) // one there is not
		}
		o.Font(f, pickOf(r, []float32{0, -3, 4, 7.5, 10, 12, 16, 24, 40, 9, 36, 20.25}))
	case 2:
		o.TextStyle(byte(pickOf(r, []int{0, 1, 2, 3, 4, 9})), byte(r.IntN(7)), byte(r.IntN(4)), pickOf(r, []float32{0, 0, 0, 1.5, -0.5, 4}))
	default:
		s := pickOf(r, textStrings)
		if r.IntN(12) == 0 {
			s = string([]byte{'a', 0xff, 0xe3, 0x81, 'b'}) // not UTF-8
		}
		advance := float32(0)
		if r.IntN(3) == 0 {
			advance = float32(5 + r.IntN(150))
		}
		if r.IntN(4) == 0 {
			o.StrokeText(s, d.c(), d.c(), advance)
		} else {
			o.FillText(s, d.c(), d.c(), advance)
		}
	}
}

// imageStep draws an image, or changes how images are drawn.
func (d *drawing) imageStep() {
	r, o := d.r, d.o
	img := pickOf(r, d.images)
	if r.IntN(30) == 0 {
		img = bdf.ImageRef(len(d.images) + r.IntN(2)) // one there is not
	}
	size := func() float32 {
		switch r.IntN(12) {
		case 0:
			return 0
		case 1:
			return -d.small()
		case 2:
			return d.small() / 10 // much smaller than the image: one of its smaller copies
		}
		return d.small() * 2
	}
	switch r.IntN(6) {
	case 0:
		o.Smoothing(r.IntN(2) == 0, byte(r.IntN(3)))
	case 1, 2, 3:
		o.Image(img, d.c(), d.c(), size(), size())
	default:
		// a part of it: inside it, around it, backwards, or none
		src := func() (float32, float32) {
			switch r.IntN(8) {
			case 0:
				return float32(r.IntN(40)) - 10, 0
			case 1:
				return float32(r.IntN(40)), -float32(1 + r.IntN(20))
			case 2:
				return -5, 400
			}
			return float32(r.IntN(20)), float32(1+r.IntN(40)) / pickOf(r, []float32{1, 1, 2, 3})
		}
		sx, sw := src()
		sy, sh := src()
		o.ImageSub(img, sx, sy, sw, sh, d.c(), d.c(), size(), size())
	}
}

func (d *drawing) step() {
	r, o := d.r, d.o
	if len(d.images) > 0 && r.IntN(5) == 0 {
		d.imageStep()
		return
	}
	if len(d.fonts) > 0 && r.IntN(3) == 0 {
		d.textStep()
		return
	}
	switch r.IntN(44) {
	case 0, 1:
		o.FillColor(d.color())
	case 2:
		o.StrokeColor(d.color())
	case 3:
		o.FillPaint(pickOf(r, d.paints))
	case 4:
		o.StrokePaint(pickOf(r, d.paints))
	case 5:
		o.Line(pickOf(r, []float32{0.05, 0.4, 1, 2.5, 7, 22}), byte(r.IntN(3)), byte(r.IntN(3)), pickOf(r, []float32{0.5, 1, 2, 4, 10}))
	case 6:
		var segs []float32
		for range r.IntN(5) {
			segs = append(segs, float32(r.IntN(12)))
		}
		o.Dash(segs, float32(r.IntN(20)-5))
	case 7:
		o.Alpha(float32(r.IntN(11)) / 10)
	case 8:
		o.Blend(byte(r.IntN(26)))
	case 9:
		o.Blend(0)
	case 10:
		o.Shadow(d.color(), float32(r.IntN(8)), float32(r.IntN(13)-6), float32(r.IntN(13)-6))
	case 11:
		o.Shadow(0, 0, 0, 0)
	case 12:
		o.Save()
		d.depth++
	case 13:
		if d.depth > 0 {
			o.Restore()
			d.depth--
		}
	case 14:
		a := r.NormFloat64()
		s := float32(0.5 + r.Float64())
		o.Transform(s*float32(math.Cos(a)), s*float32(math.Sin(a)), -s*float32(math.Sin(a))+float32(r.NormFloat64()*0.1), s*float32(math.Cos(a)), d.c()/4, d.c()/4)
	case 15:
		o.Translate(d.c()/3, d.c()/3)
	case 16:
		o.Scale(float32(0.4+r.Float64()*1.5), float32(0.4+r.Float64()*1.5))
	case 17:
		o.ClipRect(d.c(), d.c(), d.small()*4, d.small()*4)
	case 18:
		o.ClipPath(pickOf(r, d.paths), byte(r.IntN(2)))
	case 19, 20, 21:
		o.FillRect(d.c(), d.c(), d.small()*2, d.small()*2)
	case 22:
		o.StrokeRect(d.c(), d.c(), d.small()*2, d.small()*2)
	case 23:
		o.ClearRect(d.c(), d.c(), d.small(), d.small())
	case 24, 25, 26, 27:
		o.FillPath(pickOf(r, d.paths), byte(r.IntN(2)))
	case 28, 29, 30, 31:
		o.StrokePath(pickOf(r, d.paths))
	case 32:
		o.FillPathAt(pickOf(r, d.paths), byte(r.IntN(2)), d.c()/2, d.c()/2)
	case 33:
		var glyphs []bdf.Glyph
		for range r.IntN(5) {
			glyphs = append(glyphs, bdf.Glyph{Path: pickOf(r, d.paths), X: d.c() / 2, Y: d.c() / 2})
		}
		o.FillPathRun(byte(r.IntN(2)), glyphs)
	case 34, 35:
		o.GroupBegin(float32(r.IntN(11))/10, byte(r.IntN(27)), d.c(), d.c(), d.small()*5, d.small()*5)
		d.groups++
	case 36, 37:
		if d.groups > 0 {
			o.GroupEnd()
			d.groups--
		}
	case 38:
		var transfer []byte
		if r.IntN(2) == 0 {
			transfer = make([]byte, 256)
			for i := range transfer {
				transfer[i] = byte(255 - i)
			}
		}
		o.MaskBegin(byte(r.IntN(2)), d.color(), transfer)
		d.masks++
	case 39:
		if d.masks > 0 {
			o.MaskEnd()
			d.masks--
		}
	case 40, 41:
		if len(d.uses) > 0 {
			if r.IntN(2) == 0 {
				o.Use(pickOf(r, d.uses))
			} else {
				o.UseAt(pickOf(r, d.uses), d.c()/3, d.c()/3)
			}
		}
	case 42:
		o.GroupEnd() // one that may end nothing, or a group of the object that draws this one
	case 43:
		o.MaskEnd()
	}
}

// finish ends the drawing; most are left well-formed, and some with what
// they began still open.
func (d *drawing) finish() *bdf.Object {
	if d.r.IntN(5) > 0 {
		for ; d.masks > 0; d.masks-- {
			d.o.MaskEnd()
		}
		for ; d.groups > 0; d.groups-- {
			d.o.GroupEnd()
		}
		for ; d.depth > 0; d.depth-- {
			d.o.Restore()
		}
	}
	d.o.SetBBox(0, 0, 200, 150)
	return d.o
}

// RandomDrawing is an object of random instructions that uses the objects
// given.
func RandomDrawing(r *rand.Rand, children map[bdf.Hash]bdf.Rect) *bdf.Object {
	return randomDrawing(r, children, nil, nil)
}

// randomDrawing is RandomDrawing with images and text to draw as well.
func randomDrawing(r *rand.Rand, children map[bdf.Hash]bdf.Rect, images []bdf.Hash, fonts []bdf.Hash) *bdf.Object {
	d := newDrawing(r, children, images, fonts)
	for range 4 + r.IntN(40) {
		d.step()
	}
	return d.finish()
}

// RandomDocument makes a document of random drawings: objects that draw
// each other, pages of several layers, a scroll view of strips cut to their
// bodies, and a sheet of tiles with gridlines.
func RandomDocument(r *rand.Rand) *bdf.Document {
	return randomDocument(r, bdf.NewDocument(), nil, nil)
}

// RandomTextDocument is RandomDocument with text in its drawings: in fonts
// that are named, and in the font programs given, which the document
// carries as they are, packed as WOFF2, or damaged.
func RandomTextDocument(r *rand.Rand, programs [][]byte) *bdf.Document {
	doc := bdf.NewDocument()
	fonts := []bdf.Hash{}
	for range r.IntN(4) {
		if len(programs) == 0 {
			break
		}
		data := pickOf(r, programs)
		switch r.IntN(10) {
		case 0:
			if packed, err := woff2.Encode(data); err == nil {
				data = packed
			}
		case 1:
			data = data[:len(data)*(1+r.IntN(9))/10] // cut short
		case 2:
			data = append([]byte(nil), data...)
			for range 1 + r.IntN(8) {
				data[r.IntN(len(data))] ^= 1 << r.IntN(8) // damaged
			}
		case 3:
			data = []byte("wOFF, by its first bytes")
		}
		fonts = append(fonts, doc.AddFont(data))
	}
	return randomDocument(r, doc, nil, fonts)
}

// RandomImageDocument is RandomDocument with images of the formats given
// (see RandomImage) in its drawings: drawn whole and in parts, as patterns,
// smoothed and not.
func RandomImageDocument(r *rand.Rand, formats ...string) *bdf.Document {
	doc := bdf.NewDocument()
	var images []bdf.Hash
	for range 1 + r.IntN(4) {
		images = append(images, doc.AddImage(RandomImage(r, formats...)))
	}
	return randomDocument(r, doc, images, nil)
}

func randomDocument(r *rand.Rand, doc *bdf.Document, images []bdf.Hash, fonts []bdf.Hash) *bdf.Document {
	// objects that draw nothing else, then ones that draw those, then pages
	leaves := map[bdf.Hash]bdf.Rect{}
	for range 1 + r.IntN(3) {
		h, box := doc.AddObject(randomDrawing(r, nil, images, fonts))
		leaves[h] = box
	}
	middle := map[bdf.Hash]bdf.Rect{}
	for range r.IntN(3) {
		h, box := doc.AddObject(randomDrawing(r, leaves, images, fonts))
		middle[h] = box
	}
	for _, h := range sortedHashes(leaves) {
		if r.IntN(2) == 0 {
			middle[h] = leaves[h]
		}
	}
	layer := func() bdf.Layer {
		h, _ := doc.AddObject(randomDrawing(r, middle, images, fonts))
		return bdf.Layer{Role: pickOf(r, []string{bdf.RoleBody, bdf.RoleBody, bdf.RoleBackground, bdf.RoleAnnotation, bdf.RoleHeader}), Obj: h}
	}
	pages := doc.NewView("pages", bdf.ViewFixed, "")
	for range 1 + r.IntN(3) {
		var layers []bdf.Layer
		for range 1 + r.IntN(3) {
			layers = append(layers, layer())
		}
		pages.AddPage(float32(120+r.IntN(120)), float32(90+r.IntN(90)), layers...)
	}
	scroll := doc.NewView("scroll", bdf.ViewScroll, "")
	for range 1 + r.IntN(4) {
		p := scroll.AddPage(200, float32(60+r.IntN(120)), layer(), layer())
		if r.IntN(3) > 0 {
			p.Body = &bdf.RectDef{X: float32(r.IntN(30)), Y: float32(r.IntN(30)), W: float32(100 + r.IntN(80)), H: float32(30 + r.IntN(60))}
		}
	}
	sheet := doc.NewView("sheet", bdf.ViewSheet, "")
	sheet.Tile = pickOf(r, []float32{0, 64, 100, 256})
	sheet.Gridlines = r.IntN(3) > 0
	sheet.Cols = []bdf.Run{{float32(1 + r.IntN(6)), float32(10 + r.IntN(60))}, {float32(r.IntN(3)), 0}, {float32(1 + r.IntN(20)), float32(5+r.IntN(40)) + 0.5}}
	sheet.Rows = []bdf.Run{{float32(1 + r.IntN(30)), float32(8+r.IntN(20)) + 0.25}, {2, 0}, {float32(1 + r.IntN(5)), 33}}
	sheet.Tiles = map[string]string{}
	for range r.IntN(7) {
		h, _ := doc.AddObject(randomDrawing(r, middle, images, fonts))
		sheet.Tiles[fmt.Sprintf("%d,%d", r.IntN(4), r.IntN(3))] = h.String()
	}
	if r.IntN(4) == 0 {
		sheet.Tiles["01,0"] = "not a tile"
		sheet.Tiles["5,5"] = "nor a hash"
	}
	return doc
}
