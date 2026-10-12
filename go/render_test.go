package cbdf

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"math/rand/v2"
	"os"
	"strconv"
	"testing"

	"github.com/shibukawa/bdf"
	"github.com/shibukawa/bdf/raster/imagebdf"
)

// comparePixels compares an image of libbdf with one of the reference: to
// the byte where the reference computes as libbdf does (amd64). Where Go
// fuses multiply-adds (arm64) its own pictures are not those of amd64: a
// coverage that differs in its last bit moves a channel by a step, and a
// crossing that falls on the other side of a scanline moves a pixel of a
// sliver by anything. There the two only have to be close: a pixel in a
// thousand may differ by any amount, or one in fifty by little. (And once
// in some thousand drawings a decision tips the other way there, whether
// lines are parallel or a corner is round, and a whole shape differs: see
// apart.)
func comparePixels(want *image.RGBA, got *Image) string {
	b := want.Bounds()
	if b.Dx() != got.W || b.Dy() != got.H {
		return fmt.Sprintf("size %d×%d, the reference has %d×%d", got.W, got.H, b.Dx(), b.Dy())
	}
	if bytes.Equal(want.Pix, got.Pix) {
		return ""
	}
	differ, worst, at := 0, 0, 0
	for i := range want.Pix {
		d := int(want.Pix[i]) - int(got.Pix[i])
		if d < 0 {
			d = -d
		}
		if d > 0 {
			differ++
		}
		if d > worst {
			worst, at = d, i
		}
	}
	if !exactFloats && (differ*1000 <= len(want.Pix) || worst <= 32 && differ*50 <= len(want.Pix)) {
		return ""
	}
	x, y := at/4%got.W, at/4/got.W
	return fmt.Sprintf("%d of %d bytes differ; the most at (%d, %d): %v, the reference has %v", differ, len(want.Pix), x, y, got.Pix[at/4*4:at/4*4+4], want.Pix[at/4*4:at/4*4+4])
}

func renderSeed(t *testing.T) (cases int, r *rand.Rand) {
	cases, seed := 300, uint64(7)
	if n, err := strconv.Atoi(os.Getenv("LIBBDF_RENDER_CASES")); err == nil && n > 0 {
		cases = n
	} else if testing.Short() {
		cases /= 4 // (under AddressSanitizer in the CI)
	}
	if n, err := strconv.ParseUint(os.Getenv("LIBBDF_RENDER_SEED"), 10, 64); err == nil {
		seed = n
	}
	return cases, rand.New(rand.NewPCG(20261013, seed))
}

// TestRenderObjectsMatchReference draws random objects with libbdf and with
// the reference's raster/imagebdf. LIBBDF_RENDER_CASES and
// LIBBDF_RENDER_SEED change how many and which.
func TestRenderObjectsMatchReference(t *testing.T) {
	cases, r := renderSeed(t)
	backgrounds := []struct {
		c    color.Color
		has  bool
		rgba uint32
	}{{nil, false, 0}, {color.White, true, 0xffffffff}, {color.NRGBA{0x20, 0x40, 0x80, 0xff}, true, 0x204080ff}, {color.Transparent, true, 0}}
	pixels, apart := 0, 0
	for i := range cases {
		o := RandomDrawing(r, nil)
		scale := pick(r, []float64{0.5, 1, 1, 1.5, 2.25})
		bg := backgrounds[i%len(backgrounds)]
		want, werr := imagebdf.Object(o, scale, &imagebdf.Options{Background: bg.c})
		data := o.Encode()
		if dump := os.Getenv("LIBBDF_RENDER_DUMP"); dump != "" {
			// the object being drawn, for what does not come back from drawing it
			os.WriteFile(dump, data, 0o644)
		}
		got, gerr := RenderObject(data, scale, bg.has, bg.rgba)
		if (werr == nil) != (gerr == nil) {
			t.Fatalf("object %d: the reference %v, libbdf %v\n%x", i, werr, gerr, data)
		}
		if werr != nil {
			continue
		}
		if want.Rect.Min.X != got.X || want.Rect.Min.Y != got.Y {
			t.Fatalf("object %d: at (%d, %d), the reference has %v", i, got.X, got.Y, want.Rect.Min)
		}
		if d := comparePixels(want, got); d != "" {
			if !exactFloats {
				apart++
				continue
			}
			t.Fatalf("object %d at scale %g: %s\n%x", i, scale, d, data)
		}
		pixels += got.W * got.H
	}
	if apart*200 > cases {
		t.Errorf("%d of %d objects are not close to the reference's", apart, cases)
	}
	t.Logf("%d objects, %d pixels match (%s)", cases-apart, pixels, map[bool]string{true: "to the byte", false: fmt.Sprintf("closely: Go fuses multiply-adds here; %d drawings came out apart", apart)}[exactFloats])
}

// TestRenderDocumentsMatchReference draws the pages, strips and sheets of
// random documents with libbdf and with the reference.
func TestRenderDocumentsMatchReference(t *testing.T) {
	compareDocuments(t, RandomDocument)
}

// TestRenderImagesMatchReference does the same with documents that draw
// images: whole and in parts, as patterns, smoothed and not, some turned
// by their EXIF orientation, and some that cannot be decoded.
func TestRenderImagesMatchReference(t *testing.T) {
	compareDocuments(t, func(r *rand.Rand) *bdf.Document { return RandomImageDocument(r) })
}

// compareDocuments draws the pages, strips and sheets of random documents
// with libbdf and with the reference.
func compareDocuments(t *testing.T, random func(r *rand.Rand) *bdf.Document) {
	cases, r := renderSeed(t)
	cases = max(cases/6, 10)
	images, pixels, apart := 0, 0, 0
	for i := range cases {
		doc := random(r)
		var buf bytes.Buffer
		if err := doc.WriteSingle(&buf); err != nil {
			t.Fatal(err)
		}
		rd, err := bdf.OpenSingle(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
		if err != nil {
			t.Fatal(err)
		}
		read, err := rd.ToDocument()
		if err != nil {
			t.Fatal(err)
		}
		cdoc, err := OpenMemory(buf.Bytes())
		if err != nil {
			t.Fatalf("document %d: %v", i, err)
		}
		transparent := i%3 == 0
		opts := &imagebdf.Options{}
		if transparent {
			opts.Background = color.Transparent
		}
		ref := imagebdf.New(read, opts)
		cr, err := cdoc.NewRenderer(transparent, 0)
		if err != nil {
			t.Fatal(err)
		}
		check := func(what string, want *image.RGBA, werr error, got *Image, gerr error) {
			t.Helper()
			if (werr == nil) != (gerr == nil) {
				t.Fatalf("document %d %s: the reference %v, libbdf %v", i, what, werr, gerr)
			}
			if werr != nil {
				return
			}
			if d := comparePixels(want, got); d != "" {
				if !exactFloats {
					apart++
					return
				}
				os.WriteFile(os.Getenv("LIBBDF_RENDER_DUMP")+".bdf", buf.Bytes(), 0o644)
				t.Fatalf("document %d %s: %s", i, what, d)
			}
			images++
			pixels += got.W * got.H
		}
		for vi, v := range read.Views {
			switch v.Kind {
			case bdf.ViewSheet:
				sw, sh := imagebdf.SheetSize(v)
				for range 3 {
					region := bdf.Rect{X: float32(r.IntN(200)) - 20, Y: float32(r.IntN(150)) - 10, W: float32(40 + r.IntN(300)), H: float32(30 + r.IntN(200))}
					w, h := 40+r.IntN(260), 30+r.IntN(200)
					want, werr := ref.Region(v, 0, region, w, h)
					got, gerr := cr.Region(vi, 0, [4]float32{region.X, region.Y, region.W, region.H}, w, h)
					check(fmt.Sprintf("sheet %v into %d×%d (the sheet is %g×%g)", region, w, h, sw, sh), want, werr, got, gerr)
				}
			case bdf.ViewScroll:
				for range 3 {
					region := bdf.Rect{X: float32(r.IntN(60)) - 10, Y: float32(r.IntN(200)) - 10, W: float32(60 + r.IntN(200)), H: float32(40 + r.IntN(300))}
					w, h := 40+r.IntN(260), 30+r.IntN(200)
					want, werr := ref.Region(v, 0, region, w, h)
					got, gerr := cr.Region(vi, 0, [4]float32{region.X, region.Y, region.W, region.H}, w, h)
					check(fmt.Sprintf("strips %v into %d×%d", region, w, h), want, werr, got, gerr)
				}
				fallthrough // and its strips as pages
			default:
				for pi := range v.Pages {
					scale := pick(r, []float64{0.4, 1, 1.25, 2})
					want, werr := ref.Page(v, pi, scale)
					got, gerr := cr.Page(vi, pi, scale)
					check(fmt.Sprintf("view %d page %d at %g", vi, pi, scale), want, werr, got, gerr)
					region := bdf.Rect{X: float32(r.IntN(80)) - 10, Y: float32(r.IntN(60)) - 10, W: float32(30 + r.IntN(150)), H: float32(20 + r.IntN(120))}
					w, h := 30+r.IntN(200), 20+r.IntN(160)
					want, werr = ref.Region(v, pi, region, w, h)
					got, gerr = cr.Region(vi, pi, [4]float32{region.X, region.Y, region.W, region.H}, w, h)
					check(fmt.Sprintf("view %d page %d %v into %d×%d", vi, pi, region, w, h), want, werr, got, gerr)
				}
				// a page that is not there
				_, werr := ref.Page(v, len(v.Pages), 1)
				_, gerr := cr.Page(vi, len(v.Pages), 1)
				if werr == nil || gerr == nil {
					t.Fatalf("document %d: a page past the last: the reference %v, libbdf %v", i, werr, gerr)
				}
			}
		}
		cr.Close()
		cdoc.Close()
	}
	if apart*100 > images+apart {
		t.Errorf("%d of %d images are not close to the reference's", apart, images+apart)
	}
	t.Logf("%d documents, %d images, %d pixels match (%s)", cases, images, pixels, map[bool]string{true: "to the byte", false: fmt.Sprintf("closely; %d came out apart", apart)}[exactFloats])
}

// isSVG is isSVG of the reference: SVG markup rather than an encoded bitmap.
func isSVG(b []byte) bool {
	if len(b) >= 2 && (b[0] == 0xff && b[1] == 0xfe || b[0] == 0xfe && b[1] == 0xff) {
		return true
	}
	b = bytes.TrimPrefix(b, []byte("\xef\xbb\xbf"))
	b = bytes.TrimLeft(b, " \t\r\n")
	return len(b) > 0 && b[0] == '<'
}

// drawsWhatIsDrawn reports whether an object and the objects it draws draw
// nothing the rasterizer leaves out yet: no text and no SVG images.
func drawsWhatIsDrawn(doc *bdf.Document, h bdf.Hash, seen map[bdf.Hash]bool) bool {
	if ok, done := seen[h]; done {
		return ok
	}
	seen[h] = true // an object that draws itself is not what decides
	part := doc.Part(h)
	if part == nil {
		return true
	}
	o, err := bdf.DecodeObject(part.Data)
	if err != nil {
		return true
	}
	ok := true
	for _, ih := range o.Images {
		if p := doc.Part(ih); p != nil {
			ok = ok && !isSVG(p.Data)
		}
	}
	o.Walk(func(in bdf.Instr) {
		switch in.Op {
		case bdf.OpFillText, bdf.OpStrokeText:
			ok = false
		}
	})
	for _, c := range o.Objects {
		ok = ok && drawsWhatIsDrawn(doc, c, seen)
	}
	seen[h] = ok
	return ok
}

// TestRenderTestdataMatchesReference draws the pages of the test documents
// that draw no text (those of PDFs draw their text as outlines) with libbdf
// and with the reference.
func TestRenderTestdataMatchesReference(t *testing.T) {
	docs, pages, pixels, apart, left := 0, 0, 0, 0, 0
	for _, f := range documents(t) {
		doc, cdoc, ok := openBoth(t, f)
		if !ok {
			continue
		}
		ref := imagebdf.New(doc, nil)
		cr, err := cdoc.NewRenderer(false, 0)
		if err != nil {
			t.Fatal(err)
		}
		seen := map[bdf.Hash]bool{}
		drawn := 0
		for vi, v := range doc.Views {
			if v.Kind == bdf.ViewSheet {
				continue
			}
			for pi, p := range v.Pages {
				if pi >= 6 {
					break // the first pages of a long document say what the rest would
				}
				shapes := true
				for _, l := range p.Layers {
					shapes = shapes && drawsWhatIsDrawn(doc, l.Obj, seen)
				}
				if !shapes {
					left++
					continue
				}
				scale := 1.0
				if float64(p.W)*float64(p.H) > 1200*1200 {
					scale = 0.5
				}
				want, werr := ref.Page(v, pi, scale)
				got, gerr := cr.Page(vi, pi, scale)
				if (werr == nil) != (gerr == nil) {
					t.Fatalf("%s view %d page %d: the reference %v, libbdf %v", f, vi, pi, werr, gerr)
				}
				if werr != nil {
					continue
				}
				if d := comparePixels(want, got); d != "" {
					if !exactFloats {
						apart++
						continue
					}
					t.Errorf("%s view %d page %d: %s", f, vi, pi, d)
					continue
				}
				drawn++
				pixels += got.W * got.H
			}
		}
		if drawn > 0 {
			docs++
			pages += drawn
		}
		cr.Close()
		cdoc.Close()
	}
	if apart*50 > pages+apart {
		t.Errorf("%d of %d pages are not close to the reference's", apart, pages+apart)
	}
	if pages < 20 {
		t.Errorf("only %d pages compared", pages)
	}
	t.Logf("%d pages of %d documents, %d pixels match (%s); %d pages draw text or SVG images, which are not drawn yet", pages, docs, pixels, map[bool]string{true: "to the byte", false: fmt.Sprintf("closely; %d came out apart", apart)}[exactFloats], left)
}

// TestWriteRenderSeeds writes the seeds of the libFuzzer target render_fuzz
// (a byte of options, then an object or a document) when
// LIBBDF_RENDER_SEEDS names a directory:
//
//	LIBBDF_RENDER_SEEDS=$PWD/fuzz/corpus/render go test -C go -run TestWriteRenderSeeds .
func TestWriteRenderSeeds(t *testing.T) {
	dir := os.Getenv("LIBBDF_RENDER_SEEDS")
	if dir == "" {
		t.Skip("LIBBDF_RENDER_SEEDS is not set")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	r := rand.New(rand.NewPCG(20261013, 5))
	n := 0
	write := func(data []byte) {
		if err := os.WriteFile(fmt.Sprintf("%s/seed-%03d", dir, n), append([]byte{byte(r.IntN(256))}, data...), 0o644); err != nil {
			t.Fatal(err)
		}
		n++
	}
	for range 60 {
		write(RandomDrawing(r, nil).Encode())
	}
	for range 12 {
		var buf bytes.Buffer
		if err := RandomDocument(r).WriteSingle(&buf); err != nil {
			t.Fatal(err)
		}
		write(buf.Bytes())
	}
	t.Logf("%d seeds written to %s", n, dir)
}
