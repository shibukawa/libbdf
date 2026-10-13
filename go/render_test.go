package cbdf

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"maps"
	"math/rand/v2"
	"os"
	"path/filepath"
	"slices"
	"sort"
	"strconv"
	"strings"
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
	compareDocuments(t, nil, nil, RandomDocument)
}

// fontPrograms are the bytes of the font files of the tests, for documents
// to carry.
func fontPrograms(t testing.TB) [][]byte {
	t.Helper()
	var out [][]byte
	for _, dir := range fontDirs(t) {
		m, _ := filepath.Glob(filepath.Join(dir, "*"))
		sort.Strings(m)
		for _, p := range m {
			switch strings.ToLower(filepath.Ext(p)) {
			case ".ttf", ".otf", ".woff2":
				b, err := os.ReadFile(p)
				if err != nil {
					t.Fatal(err)
				}
				out = append(out, b)
			}
		}
	}
	return out
}

// TestRenderMadeFontsMatchReference draws text in fonts made for the tests
// (see TestFonts): the outlines of TrueType and of CFF with everything a
// reader has to follow in them, carried by the documents and named.
func TestRenderMadeFontsMatchReference(t *testing.T) {
	dir := t.TempDir()
	var programs [][]byte
	made := TestFonts(1)
	var names []string
	for name := range made {
		names = append(names, name)
	}
	sort.Strings(names)
	for _, name := range names {
		if err := os.WriteFile(filepath.Join(dir, name), made[name], 0o644); err != nil {
			t.Fatal(err)
		}
		programs = append(programs, made[name])
	}
	fonts := fontsOf(t, []string{dir})
	defer fonts.Close()
	if fonts.Count() != len(made) {
		t.Fatalf("libbdf reads %d of the %d fonts made", fonts.Count(), len(made))
	}
	compareDocuments(t, []string{dir}, fonts, func(r *rand.Rand) *bdf.Document { return RandomTextDocument(r, programs) })
}

// TestRenderTextMatchesReference does the same with documents that draw
// text: filled and stroked, in fonts they carry (some damaged) and in fonts
// they name, in both directions, aligned and fitted to an advance.
func TestRenderTextMatchesReference(t *testing.T) {
	dirs := fontDirs(t)
	fonts := fontsOf(t, dirs)
	defer fonts.Close()
	programs := fontPrograms(t)
	compareDocuments(t, dirs, fonts, func(r *rand.Rand) *bdf.Document { return RandomTextDocument(r, programs) })
}

// TestRenderSVGMatchesReference draws documents with SVG images (shapes,
// gradients, clip paths, text, images inside them) with both renderers.
func TestRenderSVGMatchesReference(t *testing.T) {
	dirs := fontDirs(t)
	fonts := fontsOf(t, dirs)
	defer fonts.Close()
	compareDocuments(t, dirs, fonts, RandomSVGDocument)
}

// TestRenderImagesMatchReference does the same with documents that draw
// images: whole and in parts, as patterns, smoothed and not, some turned
// by their EXIF orientation, and some that cannot be decoded.
func TestRenderImagesMatchReference(t *testing.T) {
	compareDocuments(t, nil, nil, func(r *rand.Rand) *bdf.Document { return RandomImageDocument(r) })
}

// dumpDocument writes a document the renderers disagree on to the file
// LIBBDF_RENDER_DUMP names (with .bdf after it), when it names one.
func dumpDocument(file []byte) {
	if name := os.Getenv("LIBBDF_RENDER_DUMP"); name != "" {
		os.WriteFile(name+".bdf", file, 0o644)
	}
}

// compareDocuments draws the pages, strips and sheets of random documents
// with libbdf and with the reference.
func compareDocuments(t *testing.T, fontDirs []string, fonts *Fonts, random func(r *rand.Rand) *bdf.Document) {
	cases, r := renderSeed(t)
	cases = max(cases/6, 10)
	images, pixels, apart := 0, 0, 0
	warned := map[string]int{}
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
		opts := &imagebdf.Options{FontDirs: fontDirs, NoSystemFonts: true}
		if transparent {
			opts.Background = color.Transparent
		}
		ref := imagebdf.New(read, opts)
		cr, err := cdoc.NewRendererWith(fonts, transparent, 0)
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
				dumpDocument(buf.Bytes())
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
		// what the two could not draw is the same, in the order they met it
		if want, got := ref.Warnings(), cr.Warnings(); !slices.Equal(want, got) {
			dumpDocument(buf.Bytes())
			t.Fatalf("document %d: the reference warns\n  %s\nlibbdf\n  %s", i, strings.Join(want, "\n  "), strings.Join(got, "\n  "))
		}
		for _, w := range cr.Warnings() {
			warned[w]++
		}
		cr.Close()
		cdoc.Close()
	}
	if os.Getenv("LIBBDF_RENDER_WARNINGS") != "" {
		for _, w := range slices.Sorted(maps.Keys(warned)) {
			t.Logf("%4d  %s", warned[w], w)
		}
	}
	if apart*100 > images+apart {
		t.Errorf("%d of %d images are not close to the reference's", apart, images+apart)
	}
	t.Logf("%d documents, %d images, %d pixels match (%s)", cases, images, pixels, map[bool]string{true: "to the byte", false: fmt.Sprintf("closely; %d came out apart", apart)}[exactFloats])
}

// fontDirs are directories of fonts in the pinned bdf module: what its own
// tests draw text with. The reference is given them in place of the fonts
// of the system, and libbdf their files.
func fontDirs(t testing.TB) []string {
	t.Helper()
	root := filepath.Dir(testdataDir(t))
	var dirs []string
	for _, d := range []string{"fixture/testdata/fonts", "converter/docx/testdata/fonts", "converter/font/testdata", "image/formula/fonts"} {
		dir := filepath.Join(root, filepath.FromSlash(d))
		if _, err := os.Stat(dir); err != nil {
			t.Fatalf("the fonts of the bdf module: %v", err)
		}
		dirs = append(dirs, dir)
	}
	return dirs
}

// fontsOf are the font files of directories as the reference finds them:
// directory by directory, and in each by path.
func fontsOf(t testing.TB, dirs []string) *Fonts {
	t.Helper()
	fonts := NewFonts()
	for _, dir := range dirs {
		var paths []string
		filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
			if err == nil && !d.IsDir() {
				switch strings.ToLower(filepath.Ext(p)) {
				case ".ttf", ".otf", ".ttc", ".otc":
					paths = append(paths, p)
				}
			}
			return nil
		})
		sort.Strings(paths)
		for _, p := range paths {
			fonts.AddFile(p) // a file without a face is passed over, as there
		}
	}
	if fonts.Count() == 0 {
		t.Fatal("no fonts")
	}
	return fonts
}

// TestRenderTestdataMatchesReference draws the pages of the test documents
// with libbdf and with the reference: their text in the fonts they carry,
// and what names a font in the fonts of the bdf module's own tests.
func TestRenderTestdataMatchesReference(t *testing.T) {
	docs, pages, pixels, apart := 0, 0, 0, 0
	dirs := fontDirs(t)
	fonts := fontsOf(t, dirs)
	defer fonts.Close()
	for _, f := range documents(t) {
		doc, cdoc, ok := openBoth(t, f)
		if !ok {
			continue
		}
		ref := imagebdf.New(doc, &imagebdf.Options{FontDirs: dirs, NoSystemFonts: true})
		cr, err := cdoc.NewRendererWith(fonts, false, 0)
		if err != nil {
			t.Fatal(err)
		}
		drawn := 0
		for vi, v := range doc.Views {
			if v.Kind == bdf.ViewSheet {
				continue
			}
			for pi, p := range v.Pages {
				if pi >= 6 {
					break // the first pages of a long document say what the rest would
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
	t.Logf("%d pages of %d documents, %d pixels match (%s)", pages, docs, pixels, map[bool]string{true: "to the byte", false: fmt.Sprintf("closely; %d came out apart", apart)}[exactFloats])
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

// TestMadeFontsDraw checks that the fonts the tests make are fonts to the
// reference and to libbdf: text in each of them, carried by a document and
// named, is drawn (there is ink), without a warning, and alike.
func TestMadeFontsDraw(t *testing.T) {
	dir := t.TempDir()
	made := TestFonts(1)
	for name, data := range made {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	fonts := fontsOf(t, []string{dir})
	defer fonts.Close()
	families := map[string]string{"a-sans-regular.ttf": "Test Sans", "a-sans-bold.ttf": "Test Sans", "a-sans-italic.ttf": "Test Sans",
		"b-serif-regular.otf": "Test Serif", "b-serif-bolditalic.otf": "Test Serif", "c-mono.otf": "Test Mono", "d-wide.ttf": "Noto Sans CJK JP"}
	for name, data := range made {
		doc := bdf.NewDocument()
		o := bdf.NewObject()
		carried := o.AddFont(bdf.EmbeddedFont(doc.AddFont(data), 400, bdf.StyleNormal))
		weight, style := uint16(400), byte(bdf.StyleNormal)
		if strings.Contains(name, "bold") {
			weight = 700
		}
		if strings.Contains(name, "italic") {
			style = bdf.StyleItalic
		}
		named := o.AddFont(bdf.SystemFont(families[name], weight, style))
		o.Font(carried, 30).FillText("Hamburg 0123 efg", 5, 40, 0)
		o.Font(named, 30).FillText("Hamburg 0123 efg", 5, 90, 0).StrokeText("abc", 5, 130, 0)
		o.SetBBox(0, 0, 300, 150)
		h, _ := doc.AddObject(o)
		doc.NewView("p", bdf.ViewFixed, "").AddPage(300, 150, bdf.Layer{Role: bdf.RoleBody, Obj: h})
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
			t.Fatal(err)
		}
		ref := imagebdf.New(read, &imagebdf.Options{FontDirs: []string{dir}, NoSystemFonts: true})
		cr, err := cdoc.NewRendererWith(fonts, false, 0)
		if err != nil {
			t.Fatal(err)
		}
		want, werr := ref.Page(read.Views[0], 0, 1)
		got, gerr := cr.Page(0, 0, 1)
		if werr != nil || gerr != nil {
			t.Fatalf("%s: the reference %v, libbdf %v", name, werr, gerr)
		}
		ink := [3]int{}
		for y := 0; y < want.Rect.Dy(); y++ {
			for x := 0; x < want.Rect.Dx(); x++ {
				if want.Pix[y*want.Stride+4*x] < 128 {
					ink[y/50]++
				}
			}
		}
		if ink[0] < 500 || ink[1] < 500 || ink[2] < 50 {
			t.Errorf("%s: the reference draws little with it: %v dark pixels in the three lines", name, ink)
		}
		if w := ref.Warnings(); len(w) > 0 {
			t.Errorf("%s: the reference warns: %v", name, w)
		}
		if w := cr.Warnings(); len(w) > 0 {
			t.Errorf("%s: libbdf warns: %v", name, w)
		}
		if d := comparePixels(want, got); d != "" && exactFloats {
			t.Errorf("%s: %s", name, d)
		}
		cr.Close()
		cdoc.Close()
	}
}

// TestWriteFontSeeds writes the seeds of the libFuzzer target font_fuzz (a
// byte of options, then a font program) when LIBBDF_FONT_SEEDS names a
// directory:
//
//	LIBBDF_FONT_SEEDS=$PWD/fuzz/corpus/font go test -C go -run TestWriteFontSeeds .
func TestWriteFontSeeds(t *testing.T) {
	dir := os.Getenv("LIBBDF_FONT_SEEDS")
	if dir == "" {
		t.Skip("LIBBDF_FONT_SEEDS is not set")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	n := 0
	write := func(data []byte) {
		if len(data) > 1<<16 {
			return // a fuzzer does more with small inputs
		}
		for _, opts := range []byte{0x00, 0x5b, 0xa7} {
			if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("seed-%03d", n)), append([]byte{opts}, data...), 0o644); err != nil {
				t.Fatal(err)
			}
			n++
		}
	}
	for _, data := range TestFonts(3) {
		write(data)
	}
	for _, data := range fontPrograms(t) {
		write(data)
	}
	t.Logf("%d seeds in %s", n, dir)
}

// fontPage is a document of one page that draws text in a font program it
// carries: filled and stroked, made bold or slanted, in both directions.
func fontPage(opts byte, program []byte) []byte {
	doc := bdf.NewDocument()
	o := bdf.NewObject()
	weight, style := uint16(400), byte(bdf.StyleNormal)
	if opts&1 != 0 {
		weight = 700
	}
	if opts&2 != 0 {
		style = bdf.StyleItalic
	}
	carried := o.AddFont(bdf.EmbeddedFont(doc.AddFont(program), weight, style))
	texts := []string{"Hamburgefonstiv 0123", "a\tb שלום 12", "日本語 \U0001d49c", "A"}
	dir := byte(bdf.DirLTR)
	if opts&8 != 0 {
		dir = bdf.DirRTL
	}
	o.Font(carried, 8+float32(opts>>6)*9).TextStyle(opts>>3&3, opts>>5&3, dir, 0.5)
	advance := float32(0)
	if opts&16 != 0 {
		advance = 60
	}
	o.FillText(texts[opts>>4&3], 4, 20, advance).StrokeText(texts[opts>>2&3], 4, 44, 0)
	o.SetBBox(0, 0, 96, 72)
	h, _ := doc.AddObject(o)
	doc.NewView("p", bdf.ViewFixed, "").AddPage(96, 72, bdf.Layer{Role: bdf.RoleBody, Obj: h})
	var buf bytes.Buffer
	if err := doc.WriteSingle(&buf); err != nil {
		panic(err)
	}
	return buf.Bytes()
}

// FuzzFont draws text in whatever a fuzzer makes of a font program, with
// libbdf and with the reference. Where the reference gives the page up (its
// font reader panics on some damaged tables, and the image fails with it)
// libbdf draws the page without the font: that is the one difference let
// stand.
func FuzzFont(f *testing.F) {
	for _, data := range TestFonts(5) {
		f.Add(byte(0), data)
		f.Add(byte(0x5b), data)
	}
	for _, data := range fontPrograms(f) {
		if len(data) <= 1<<15 {
			f.Add(byte(0xa7), data)
		}
	}
	f.Fuzz(func(t *testing.T, opts byte, program []byte) {
		file := fontPage(opts, program)
		rd, err := bdf.OpenSingle(bytes.NewReader(file), int64(len(file)))
		if err != nil {
			t.Fatal(err)
		}
		read, err := rd.ToDocument()
		if err != nil {
			t.Fatal(err)
		}
		cdoc, err := OpenMemory(file)
		if err != nil {
			t.Fatal(err)
		}
		defer cdoc.Close()
		ref := imagebdf.New(read, &imagebdf.Options{NoSystemFonts: true})
		cr, err := cdoc.NewRenderer(false, 0)
		if err != nil {
			t.Fatal(err)
		}
		defer cr.Close()
		want, werr := ref.Page(read.Views[0], 0, 1)
		got, gerr := cr.Page(0, 0, 1)
		if gerr != nil {
			t.Fatalf("libbdf does not draw the page: %v", gerr)
		}
		if werr != nil {
			return
		}
		if d := comparePixels(want, got); d != "" {
			t.Fatal(d)
		}
	})
}
