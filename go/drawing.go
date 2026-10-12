package cbdf

import (
	"fmt"
	"math"
	"math/rand/v2"

	"github.com/shibukawa/bdf"
)

// The random drawings the rendering tests compare libbdf and the reference
// on, and go/genvectors makes the vectors of the C tests from.

func pickOf[T any](r *rand.Rand, from []T) T { return from[r.IntN(len(from))] }

// drawing makes random drawings: every instruction of the vector core in
// an order no writer would choose.
type drawing struct {
	r      *rand.Rand
	o      *bdf.Object
	paths  []bdf.PathRef
	paints []bdf.PaintRef
	uses   []bdf.ObjRef
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

// newDrawing starts a drawing with paths and paints, and uses of the
// objects given.
func newDrawing(r *rand.Rand, children map[bdf.Hash]bdf.Rect) *drawing {
	d := &drawing{r: r, o: bdf.NewObject()}
	for range 2 + r.IntN(4) {
		d.paths = append(d.paths, d.o.AddPath(d.path()))
	}
	for range 1 + r.IntN(3) {
		d.paints = append(d.paints, d.o.AddPaint(d.paint()))
	}
	for h, box := range children {
		d.uses = append(d.uses, d.o.AddObject(h, box))
	}
	return d
}

func (d *drawing) step() {
	r, o := d.r, d.o
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
	d := newDrawing(r, children)
	for range 4 + r.IntN(40) {
		d.step()
	}
	return d.finish()
}

// RandomDocument makes a document of random drawings: objects that draw
// each other, pages of several layers, a scroll view of strips cut to their
// bodies, and a sheet of tiles with gridlines.
func RandomDocument(r *rand.Rand) *bdf.Document {
	doc := bdf.NewDocument()
	// objects that draw nothing else, then ones that draw those, then pages
	leaves := map[bdf.Hash]bdf.Rect{}
	for range 1 + r.IntN(3) {
		h, box := doc.AddObject(RandomDrawing(r, nil))
		leaves[h] = box
	}
	middle := map[bdf.Hash]bdf.Rect{}
	for range r.IntN(3) {
		h, box := doc.AddObject(RandomDrawing(r, leaves))
		middle[h] = box
	}
	for h, box := range leaves {
		if r.IntN(2) == 0 {
			middle[h] = box
		}
	}
	layer := func() bdf.Layer {
		h, _ := doc.AddObject(RandomDrawing(r, middle))
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
		h, _ := doc.AddObject(RandomDrawing(r, middle))
		sheet.Tiles[fmt.Sprintf("%d,%d", r.IntN(4), r.IntN(3))] = h.String()
	}
	if r.IntN(4) == 0 {
		sheet.Tiles["01,0"] = "not a tile"
		sheet.Tiles["5,5"] = "nor a hash"
	}
	return doc
}
