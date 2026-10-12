package cbdf

import (
	"bytes"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	"github.com/shibukawa/bdf"
)

// exactFloats reports whether the reference computes positions as libbdf
// does. Go may fuse a multiplication and the addition after it into one
// operation that rounds once, and does on arm64, ppc64, s390x and riscv64;
// libbdf never does, so that positions are the same on every platform. Where
// Go does not fuse (amd64) the two must agree to the bit; elsewhere the last
// bits of a rotated or skewed position may differ, and with them a separator
// that was guessed from positions right at its threshold.
var exactFloats = runtime.GOARCH == "amd64"

// How positions are compared.
const (
	bitsEqual = iota // to the bit
	nearly           // within a tolerance: the test documents, where Go fuses
	notAtAll         // fuzzed numbers, where Go fuses: fusion changes them without bound
)

func sameF32(a, b float32) bool {
	return math.Float32bits(a) == math.Float32bits(b) || (a != a && b != b)
}

func positionsAgree(a, b float32, mode int) bool {
	switch {
	case sameF32(a, b), mode == notAtAll:
		return true
	case mode == bitsEqual:
		return false
	}
	d := math.Abs(float64(a) - float64(b))
	return d <= 1e-3*(1+math.Max(math.Abs(float64(a)), math.Abs(float64(b))))
}

// compareRuns reports the first difference between the runs of the
// reference and those of libbdf.
func compareRuns(want []bdf.TextRun, got []TextRun, mode int) string {
	if len(want) != len(got) {
		return fmt.Sprintf("%d runs, reference has %d", len(got), len(want))
	}
	moved := false // the position of the run before differs in bits
	for i, w := range want {
		g := got[i]
		switch {
		case w.Text != g.Text:
			return fmt.Sprintf("run %d: text %q, reference %q", i, g.Text, w.Text)
		case w.AltText != g.AltText, uint32(w.Ordinal) != g.Ordinal, w.Align != g.Align:
			return fmt.Sprintf("run %d: alt %v ordinal %d align %d, reference %v %d %d", i, g.AltText, g.Ordinal, g.Align, w.AltText, w.Ordinal, w.Align)
		case !sameF32(w.Size, g.Size), !sameF32(w.Advance, g.Advance):
			return fmt.Sprintf("run %d: size %g advance %g, reference %g %g", i, g.Size, g.Advance, w.Size, w.Advance)
		case (w.Font != nil) != g.HasFont:
			return fmt.Sprintf("run %d: font %v, reference %v", i, g.HasFont, w.Font != nil)
		}
		if w.Font != nil && (w.Font.Kind != g.Font.Kind || w.Font.Hash != bdf.Hash(g.Font.Hash) || w.Font.Family != g.Font.Family || w.Font.Weight != g.Font.Weight || w.Font.Style != g.Font.Style) {
			return fmt.Sprintf("run %d: font %+v, reference %+v", i, g.Font, *w.Font)
		}
		if !positionsAgree(w.X, g.X, mode) || !positionsAgree(w.Y, g.Y, mode) {
			return fmt.Sprintf("run %d (%q): at %g, %g, reference %g, %g", i, g.Text, g.X, g.Y, w.X, w.Y)
		}
		for k := range w.Matrix {
			if !positionsAgree(w.Matrix[k], g.Matrix[k], mode) {
				return fmt.Sprintf("run %d: matrix %v, reference %v", i, g.Matrix, w.Matrix)
			}
		}
		here := !sameF32(w.X, g.X) || !sameF32(w.Y, g.Y)
		if w.Sep != g.Sep && (mode == bitsEqual || !(here || moved)) {
			return fmt.Sprintf("run %d (%q): separator %d, reference %d", i, g.Text, g.Sep, w.Sep)
		}
		moved = here
	}
	return ""
}

func compareIndex(want []bdf.IndexRun, got []IndexRun) string {
	if len(want) != len(got) {
		return fmt.Sprintf("%d runs, reference has %d", len(got), len(want))
	}
	for i, w := range want {
		g := got[i]
		if w.A != g.A || w.B != g.B || w.Ordinal != g.Ordinal || w.Sep != g.Sep || w.Text != g.Text {
			return fmt.Sprintf("run %d: %+v, reference %+v", i, g, w)
		}
	}
	return ""
}

// openBoth opens a test document with both readers; ok is false for one
// that is encrypted (libbdf does not unlock yet).
func openBoth(t testing.TB, file string) (doc *bdf.Document, cdoc *Doc, ok bool) {
	t.Helper()
	data, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	r, err := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
	if err != nil {
		t.Fatalf("%s: %v", file, err)
	}
	if r.Encrypted() {
		return nil, nil, false
	}
	if doc, err = r.ToDocument(); err != nil {
		t.Fatalf("%s: %v", file, err)
	}
	if cdoc, err = OpenMemory(data); err != nil {
		t.Fatalf("%s: libbdf: %v", file, err)
	}
	return doc, cdoc, true
}

// resolver decodes the objects of a document as they are asked for.
func resolver(doc *bdf.Document) func(bdf.Hash) *bdf.ObjectPart {
	decoded := map[bdf.Hash]*bdf.ObjectPart{}
	return func(h bdf.Hash) *bdf.ObjectPart {
		if o, ok := decoded[h]; ok {
			return o
		}
		p := doc.Part(h)
		if p == nil {
			return nil
		}
		o, err := bdf.DecodeObject(p.Data)
		if err != nil {
			return nil
		}
		decoded[h] = o
		return o
	}
}

// topLevel lists the objects a view draws: the layers of its pages and its
// inline tiles.
func topLevel(v *bdf.View) []bdf.Hash {
	var out []bdf.Hash
	seen := map[bdf.Hash]bool{}
	add := func(h bdf.Hash) {
		if !seen[h] {
			seen[h] = true
			out = append(out, h)
		}
	}
	for _, p := range v.Pages {
		for _, l := range p.Layers {
			add(l.Obj)
		}
	}
	for _, s := range v.Tiles {
		if h, err := bdf.ParseHash(s); err == nil {
			add(h)
		}
	}
	return out
}

func testdataMode() int {
	if exactFloats {
		return bitsEqual
	}
	return nearly
}

// TestTextMatchesReference extracts the text of every page layer and tile
// of the test documents with both implementations.
func TestTextMatchesReference(t *testing.T) {
	objects, runs, links := 0, 0, 0
	for _, f := range documents(t) {
		doc, cdoc, ok := openBoth(t, f)
		if !ok {
			continue
		}
		resolve := resolver(doc)
		for _, v := range doc.Views {
			for _, h := range topLevel(v) {
				o := resolve(h)
				if o == nil {
					t.Fatalf("%s: object %s does not decode", f, h)
				}
				want, werr := bdf.ExtractText(o, resolve)
				got, gotLinks, gerr := cdoc.Text(h)
				if (werr == nil) != (gerr == nil) {
					t.Errorf("%s %s: reference %v, libbdf %v", filepath.Base(f), h, werr, gerr)
					continue
				}
				if d := compareRuns(want, got, testdataMode()); d != "" {
					t.Errorf("%s %s: %s", filepath.Base(f), h, d)
				}
				for _, l := range gotLinks {
					if l.After < -1 || l.After >= int64(len(got)) || !(l.W >= 0 && l.H >= 0) {
						t.Errorf("%s %s: link %+v among %d runs", filepath.Base(f), h, l, len(got))
					}
					if ParseLink(l.URL).Kind == 0 {
						t.Errorf("%s %s: a converter wrote a link no reader follows: %q", filepath.Base(f), h, l.URL)
					}
				}
				objects++
				runs += len(got)
				links += len(gotLinks)
			}
		}
		cdoc.Close()
	}
	t.Logf("%d objects, %d runs compared; %d links read (positions %s)", objects, runs, links, map[bool]string{true: "to the bit", false: "within a tolerance: Go fuses multiply-adds here"}[exactFloats])
}

// TestViewTextMatchesReference compares the text of every view: the stored
// text index, and the one extracted from the objects.
func TestViewTextMatchesReference(t *testing.T) {
	stored, extracted := 0, 0
	for _, f := range documents(t) {
		doc, cdoc, ok := openBoth(t, f)
		if !ok {
			continue
		}
		for vi, v := range doc.Views {
			if v.TextIndex != "" {
				h, err := bdf.ParseHash(v.TextIndex)
				if err != nil || doc.Part(h) == nil {
					t.Fatalf("%s: view %s names a text index that is not there", f, v.ID)
				}
				want, werr := bdf.DecodeTextIndex(doc.Part(h).Data)
				got, gerr := cdoc.ViewText(vi, false)
				if (werr == nil) != (gerr == nil) {
					t.Errorf("%s view %s: stored index: reference %v, libbdf %v", filepath.Base(f), v.ID, werr, gerr)
				} else if d := compareIndex(want, got); d != "" {
					t.Errorf("%s view %s: stored index: %s", filepath.Base(f), v.ID, d)
				}
				stored++
			}
			// BuildTextIndex extracts whatever the view names, and names the result
			h, werr := doc.BuildTextIndex(v)
			got, gerr := cdoc.ViewText(vi, true)
			if (werr == nil) != (gerr == nil) {
				t.Errorf("%s view %s: extraction: reference %v, libbdf %v", filepath.Base(f), v.ID, werr, gerr)
				continue
			}
			if werr != nil {
				continue
			}
			want, err := bdf.DecodeTextIndex(doc.Part(h).Data)
			if err != nil {
				t.Fatal(err)
			}
			if d := compareIndex(want, got); d != "" {
				t.Errorf("%s view %s: extraction: %s", filepath.Base(f), v.ID, d)
			}
			extracted++
		}
		cdoc.Close()
	}
	t.Logf("%d stored indexes and %d extracted ones compared", stored, extracted)
}

// TestSearchTextMatchesReference compares the text for a search index.
func TestSearchTextMatchesReference(t *testing.T) {
	docs, pages := 0, 0
	for _, f := range documents(t) {
		doc, cdoc, ok := openBoth(t, f)
		if !ok {
			continue
		}
		want, werr := doc.SearchText()
		got, gerr := cdoc.SearchText()
		cdoc.Close()
		if (werr == nil) != (gerr == nil) {
			t.Errorf("%s: reference %v, libbdf %v", filepath.Base(f), werr, gerr)
			continue
		}
		if werr != nil {
			continue
		}
		docs++
		if len(want.Views) != len(got) {
			t.Errorf("%s: %d views, reference has %d", filepath.Base(f), len(got), len(want.Views))
			continue
		}
		for i, wv := range want.Views {
			gv := got[i]
			if wv.ID != gv.ID || wv.Kind != gv.Kind || wv.Title != gv.Title || len(wv.Pages) != len(gv.Pages) {
				t.Errorf("%s view %d: %q %q %q with %d pages, reference %q %q %q with %d", filepath.Base(f), i, gv.ID, gv.Kind, gv.Title, len(gv.Pages), wv.ID, wv.Kind, wv.Title, len(wv.Pages))
				continue
			}
			for k, wp := range wv.Pages {
				if int64(wp.Page) != gv.Pages[k].Page || wp.Text != gv.Pages[k].Text {
					t.Errorf("%s view %s page %d: text differs (%d and %d bytes)", filepath.Base(f), wv.ID, wp.Page, len(gv.Pages[k].Text), len(wp.Text))
				}
				pages++
			}
		}
	}
	t.Logf("%d documents, %d pages of text compared", docs, pages)
}

func compareIndexPart(data []byte) string {
	want, werr := bdf.DecodeTextIndex(data)
	got, plain, gerr := DecodeTextIndex(data)
	if (werr == nil) != (gerr == nil) {
		return fmt.Sprintf("text index: reference %v, libbdf %v", werr, gerr)
	}
	if werr == nil {
		if d := compareIndex(want, got); d != "" {
			return "text index: " + d
		}
		if p := bdf.PlainText(want); p != plain {
			return fmt.Sprintf("plain text: %d bytes, reference %d", len(plain), len(p))
		}
	}
	wantCues, werr := bdf.DecodeCues(data)
	systems, cues, gerr := DecodeCues(data)
	if (werr == nil) != (gerr == nil) {
		return fmt.Sprintf("cues: reference %v, libbdf %v", werr, gerr)
	}
	if werr != nil {
		return ""
	}
	if len(wantCues.Systems) != len(systems) || len(wantCues.Cues) != len(cues) {
		return fmt.Sprintf("cues: %d systems and %d cues, reference %d and %d", len(systems), len(cues), len(wantCues.Systems), len(wantCues.Cues))
	}
	for i, w := range wantCues.Systems {
		g := systems[i]
		if w.Page != g.Page || !sameF32(w.X, g.X) || !sameF32(w.Y, g.Y) || !sameF32(w.W, g.W) || !sameF32(w.H, g.H) {
			return fmt.Sprintf("cues: system %d: %+v, reference %+v", i, g, w)
		}
	}
	for i, w := range wantCues.Cues {
		g := cues[i]
		if w.Tick != g.Tick || w.System != g.System || !sameF32(w.X, g.X) {
			return fmt.Sprintf("cues: cue %d: %+v, reference %+v", i, g, w)
		}
	}
	return ""
}

// indexParts returns the index parts of every document: text indexes, cue
// indexes and tile indexes.
func indexParts(t testing.TB) []part {
	t.Helper()
	var out []part
	for _, f := range documents(t) {
		r, err := bdf.OpenSingleFile(f)
		if err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		if !r.Encrypted() {
			for _, e := range r.Manifest.Parts {
				if e.T == bdf.PartIndex {
					data, err := r.Part(e.H)
					if err != nil {
						t.Fatal(err)
					}
					out = append(out, part{filepath.Base(f), e.H.String(), e.T, data})
				}
			}
		}
		r.Close()
	}
	return out
}

// TestIndexPartsMatchReference decodes every index part as a text index and
// as a cue index with both implementations.
func TestIndexPartsMatchReference(t *testing.T) {
	text, cues := 0, 0
	for _, p := range indexParts(t) {
		if d := compareIndexPart(p.data); d != "" {
			t.Errorf("%s %s: %s", p.doc, p.hash, d)
		}
		if bytes.HasPrefix(p.data, []byte("BTXT")) {
			text++
		} else if bytes.HasPrefix(p.data, []byte("BCUE")) {
			cues++
		}
	}
	t.Logf("%d text indexes and %d cue indexes compared", text, cues)
}

// seedObjects builds a top-level object and a child that exercise the walk:
// marks, an ALT_TEXT for a path, a child drawn through transforms, a link,
// and a soft mask.
func seedObjects() (top, child []byte) {
	c := bdf.NewObject()
	cf := c.AddFont(bdf.SystemFont("serif", 400, bdf.StyleNormal))
	c.Font(cf, 10).FillText("child", 0, 0, 20)
	c.Mark(bdf.MarkLine, "").FillText("second", 0, 12, 30)
	c.Link(0, 0, 20, 10, "#page=2")

	o := bdf.NewObject()
	f := o.AddFont(bdf.SystemFont("sans-serif", 700, bdf.StyleItalic))
	sq := o.AddPath((&bdf.Path{}).Rect(0, 0, 1, 1))
	ref := o.AddObject(bdf.Hash{1, 2, 3}, bdf.Rect{X: 2, Y: 3, W: 20, H: 10})
	o.Font(f, 12).TextStyle(bdf.AlignCenter, bdf.BaselineAlphabetic, bdf.DirLTR, 0)
	o.Mark(bdf.MarkHeading, "1").FillText("Title", 10, 20, 30).FillText("more", 45, 20, 20)
	o.Mark(bdf.MarkAltText, "outlined").FillPathAt(sq, bdf.NonZero, 5, 45)
	o.Mark(bdf.MarkAltText, "run").FillPathRun(bdf.NonZero, []bdf.Glyph{{Path: sq, X: 7, Y: 8}})
	o.Save().Transform(0.8, 0.6, -0.6, 0.8, 100, 50).Scale(2, 3).Use(ref).Restore()
	o.Mark(bdf.MarkAltText, "by a child").UseAt(ref, 50, 150)
	o.Translate(3, 4).UseAt(ref, 1, 2)
	o.Link(10, 10, 100, 12, "https://example.com/")
	o.MaskBegin(bdf.MaskAlpha, 0, nil).FillText("masked", 0, 0, 10).MaskEnd()
	o.Mark(bdf.MarkAltText, "left over")
	return o.Encode(), c.Encode()
}

// usesItself reports whether a child object can draw itself more than once
// from one walk: the walks then multiply until the limit of reused
// instructions stops them, which takes seconds for each input.
func usesItself(o *bdf.ObjectPart) bool {
	self := false
	for _, h := range o.Objects {
		self = self || h[0] == 1
	}
	if !self {
		return false
	}
	uses := 0
	_ = o.Walk(func(in bdf.Instr) {
		if in.Op == bdf.OpUse || in.Op == bdf.OpUseAt {
			uses++
		}
	})
	return uses > 1
}

// FuzzText walks the same two objects with both implementations: the first
// is the top-level one, and the second is what every part whose name starts
// with the byte 1 resolves to.
func FuzzText(f *testing.F) {
	top, child := seedObjects()
	f.Add(top, child)
	f.Add(top, []byte{})
	f.Add(child, top)
	for i, p := range parts(f) {
		if p.typ == bdf.PartObject && i%9 == 0 {
			f.Add(p.data, child)
		}
	}
	mode := notAtAll
	if exactFloats {
		mode = bitsEqual
	}
	f.Fuzz(func(t *testing.T, top, child []byte) {
		goTop, werr := bdf.DecodeObject(top)
		cTop, gerr := DecodeObject(top)
		if (werr == nil) != (gerr == nil) {
			t.Fatalf("decode: reference %v, libbdf %v", werr, gerr)
		}
		if werr != nil {
			return
		}
		defer cTop.Close()
		goChild, _ := bdf.DecodeObject(child)
		cChild, _ := DecodeObject(child)
		if (goChild == nil) != (cChild == nil) {
			t.Fatal("the child decodes with one implementation only")
		}
		if cChild != nil {
			defer cChild.Close()
			if usesItself(goChild) {
				return
			}
		}
		want, werr := bdf.ExtractText(goTop, func(h bdf.Hash) *bdf.ObjectPart {
			if h[0] == 1 {
				return goChild
			}
			return nil
		})
		got, _, gerr := ExtractText(cTop, cChild)
		if (werr == nil) != (gerr == nil) {
			t.Fatalf("reference %v, libbdf %v", werr, gerr)
		}
		if werr != nil {
			return
		}
		if d := compareRuns(want, got, mode); d != "" {
			t.Fatal(d)
		}
	})
}

// FuzzIndexPart decodes the same bytes as a text index and as a cue index
// with both implementations.
func FuzzIndexPart(f *testing.F) {
	for _, p := range indexParts(f) {
		if len(p.data) < 64<<10 {
			f.Add(p.data)
		}
	}
	f.Add(bdf.EncodeTextIndex([]bdf.IndexRun{{A: 1, B: 2, Ordinal: 3, Sep: bdf.SepBreak, Text: "text"}, {Sep: bdf.SepSpace, Text: "more"}}))
	f.Add(bdf.EncodeCues(&bdf.Cues{Systems: []bdf.CueSystem{{Page: 1, X: 2, Y: 3, W: 4, H: 5}}, Cues: []bdf.Cue{{Tick: 10, System: 0, X: 6}, {Tick: 480, System: 0, X: 7}}}))
	f.Fuzz(func(t *testing.T, data []byte) {
		if d := compareIndexPart(data); d != "" {
			t.Fatal(d)
		}
	})
}
