package cbdf

import (
	"encoding/binary"
	"math/rand/v2"
	"sort"
	"unicode/utf16"
)

// Fonts made for the tests: small font programs with outlines of every
// kind a reader has to follow (lines, quadratic curves with implied points,
// composite glyphs; cubic curves and subroutines in CFF), which the tests
// may carry as they are nobody's but theirs.

// FontSeg is a segment of a glyph's outline in font units (y up): 'M' starts a
// contour at (X, Y), 'L' draws a line to it, 'Q' a quadratic curve with
// the control point (X1, Y1), 'C' a cubic one with (X1, Y1) and (X2, Y2).
type FontSeg struct {
	Kind           byte
	X1, Y1, X2, Y2 int
	X, Y           int
}

// FontComponent is a glyph drawn as part of another, moved and scaled.
type FontComponent struct {
	Glyph  int
	DX, DY int
	Scale  float64 // 0: as it is
}

// FontGlyph is a glyph: an outline, or (TrueType only) components.
type FontGlyph struct {
	Advance    int
	Path       []FontSeg
	Components []FontComponent
}

// FontSpec is a font to build. Glyph 0 is the missing glyph.
type FontSpec struct {
	Family, Style         string
	Weight                int
	Italic                bool
	UnitsPerEm            int
	Ascent, Descent       int // hhea: descent is negative
	Glyphs                []FontGlyph
	Cmap                  map[rune]int
	GlobalSubr, LocalSubr bool // CFF: draw through subroutines
}

type be struct{ b []byte }

func (w *be) u8(v ...int) {
	for _, x := range v {
		w.b = append(w.b, byte(x))
	}
}
func (w *be) u16(v ...int) {
	for _, x := range v {
		w.b = binary.BigEndian.AppendUint16(w.b, uint16(x))
	}
}
func (w *be) u32(v ...int) {
	for _, x := range v {
		w.b = binary.BigEndian.AppendUint32(w.b, uint32(x))
	}
}

// sfntFile assembles tables into a font file.
func sfntFile(tag uint32, tables map[string][]byte) []byte {
	var names []string
	for n := range tables {
		names = append(names, n)
	}
	sort.Strings(names)
	var w be
	w.u32(int(tag))
	w.u16(len(names), 0, 0, 0)
	off := 12 + 16*len(names)
	var body []byte
	for _, n := range names {
		t := tables[n]
		w.b = append(w.b, n...)
		w.u32(0, off+len(body), len(t))
		body = append(body, t...)
		for len(body)%4 != 0 {
			body = append(body, 0)
		}
	}
	return append(w.b, body...)
}

// common are the tables both kinds of font have.
func (s FontSpec) common(long bool) map[string][]byte {
	t := map[string][]byte{}
	var head be
	head.u32(0x00010000, 0x00010000, 0, 0x5F0F3CF5)
	head.u16(0, s.UnitsPerEm)
	head.u32(0, 0, 0, 0)
	head.u16(0, 0, 0, 0, 0, 8, 2)
	if long {
		head.u16(1, 0)
	} else {
		head.u16(0, 0)
	}
	t["head"] = head.b
	var hhea be
	hhea.u32(0x00010000)
	hhea.u16(s.Ascent, s.Descent, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, len(s.Glyphs))
	t["hhea"] = hhea.b
	var hmtx be
	for _, g := range s.Glyphs {
		hmtx.u16(g.Advance, 0)
	}
	t["hmtx"] = hmtx.b
	// cmap: format 4 for the BMP, and format 12 when there is more
	var bmp, all []rune
	for r := range s.Cmap {
		all = append(all, r)
		if r < 0xffff {
			bmp = append(bmp, r)
		}
	}
	sort.Slice(all, func(i, j int) bool { return all[i] < all[j] })
	sort.Slice(bmp, func(i, j int) bool { return bmp[i] < bmp[j] })
	var f4 be
	n := len(bmp) + 1
	f4.u16(4, 16+8*n, 0, 2*n, 0, 0, 0)
	for _, r := range bmp {
		f4.u16(int(r))
	}
	f4.u16(0xffff, 0)
	for _, r := range bmp {
		f4.u16(int(r))
	}
	f4.u16(0xffff)
	for _, r := range bmp {
		f4.u16(s.Cmap[r] - int(r))
	}
	f4.u16(1)
	for range n {
		f4.u16(0)
	}
	var cmap be
	if len(all) == len(bmp) {
		cmap.u16(0, 1, 3, 1)
		cmap.u32(12)
		cmap.b = append(cmap.b, f4.b...)
	} else {
		var f12 be
		f12.u16(12, 0)
		f12.u32(16+12*len(all), 0, len(all))
		for _, r := range all {
			f12.u32(int(r), int(r), s.Cmap[r])
		}
		cmap.u16(0, 2, 3, 1)
		cmap.u32(20)
		cmap.u16(3, 10)
		cmap.u32(20 + len(f4.b))
		cmap.b = append(cmap.b, f4.b...)
		cmap.b = append(cmap.b, f12.b...)
	}
	t["cmap"] = cmap.b
	// name: the family and the style, as Windows has them
	var name, strs be
	records := []struct {
		id int
		s  string
	}{{1, s.Family}, {2, s.Style}}
	name.u16(0, len(records), 6+12*len(records))
	for _, r := range records {
		u := utf16.Encode([]rune(r.s))
		name.u16(3, 1, 0x0409, r.id, 2*len(u), len(strs.b))
		for _, c := range u {
			strs.u16(int(c))
		}
	}
	t["name"] = append(name.b, strs.b...)
	os2 := make([]byte, 78)
	binary.BigEndian.PutUint16(os2[4:], uint16(s.Weight))
	if s.Italic {
		binary.BigEndian.PutUint16(os2[62:], 1)
	}
	t["OS/2"] = os2
	var post be
	post.u32(0x00030000, 0, 0, 0, 0, 0, 0, 0)
	t["post"] = post.b
	return t
}

// BuildTTF writes the font with TrueType outlines: lines and quadratic
// curves (a cubic segment is drawn as a line to its end).
func BuildTTF(s FontSpec) []byte {
	t := s.common(true)
	var glyf, loca be
	for _, g := range s.Glyphs {
		loca.u32(len(glyf.b))
		switch {
		case len(g.Components) > 0:
			glyf.u16(0xffff, 0, 0, 0, 0)
			for i, c := range g.Components {
				flags := 0x0001 | 0x0002 // words, an offset
				if c.Scale != 0 {
					flags |= 0x0008
				}
				if i < len(g.Components)-1 {
					flags |= 0x0020
				}
				glyf.u16(flags, c.Glyph, c.DX, c.DY)
				if c.Scale != 0 {
					glyf.u16(int(c.Scale * 16384))
				}
			}
		case len(g.Path) > 0:
			type pt struct {
				x, y int
				on   bool
			}
			var pts []pt
			var ends []int
			for i, sg := range g.Path {
				if sg.Kind == 'M' && i > 0 {
					ends = append(ends, len(pts)-1)
				}
				if sg.Kind == 'Q' {
					pts = append(pts, pt{sg.X1, sg.Y1, false})
				}
				pts = append(pts, pt{sg.X, sg.Y, true})
			}
			ends = append(ends, len(pts)-1)
			glyf.u16(len(ends), 0, 0, 0, 0)
			glyf.u16(ends...)
			glyf.u16(0)
			for _, p := range pts {
				if p.on {
					glyf.u8(1)
				} else {
					glyf.u8(0)
				}
			}
			last := 0
			for _, p := range pts {
				glyf.u16(p.x - last)
				last = p.x
			}
			last = 0
			for _, p := range pts {
				glyf.u16(p.y - last)
				last = p.y
			}
		}
		for len(glyf.b)%4 != 0 {
			glyf.b = append(glyf.b, 0)
		}
	}
	loca.u32(len(glyf.b))
	t["glyf"], t["loca"] = glyf.b, loca.b
	var maxp be
	maxp.u32(0x00010000)
	maxp.u16(len(s.Glyphs), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
	t["maxp"] = maxp.b
	return sfntFile(0x00010000, t)
}

// cffNum is a number of a charstring or a DICT.
func cffNum(v int) []byte {
	switch {
	case v >= -107 && v <= 107:
		return []byte{byte(v + 139)}
	case v >= 108 && v <= 1131:
		v -= 108
		return []byte{byte(247 + v>>8), byte(v)}
	case v >= -1131 && v <= -108:
		v = -v - 108
		return []byte{byte(251 + v>>8), byte(v)}
	}
	return []byte{28, byte(v >> 8), byte(v)}
}

// cffOffset is a number of a DICT that takes five bytes whatever it is.
func cffOffset(v int) []byte { return []byte{29, byte(v >> 24), byte(v >> 16), byte(v >> 8), byte(v)} }

func cffIndex(items [][]byte) []byte {
	var w be
	w.u16(len(items))
	if len(items) == 0 {
		return w.b
	}
	w.u8(4)
	off := 1
	w.u32(off)
	for _, it := range items {
		off += len(it)
		w.u32(off)
	}
	for _, it := range items {
		w.b = append(w.b, it...)
	}
	return w.b
}

// BuildCFF writes the font with CFF outlines: lines and cubic curves (a
// quadratic segment is raised to a cubic one).
func BuildCFF(s FontSpec) []byte {
	// what every glyph ends with goes through subroutines when asked to
	var global, local [][]byte
	end := []byte{14}
	if s.GlobalSubr {
		global = [][]byte{{14}} // endchar
		end = append(cffNum(0-107), 29)
	}
	if s.LocalSubr {
		local = [][]byte{append(append([]byte(nil), end...), 11)}
		end = append(cffNum(0-107), 10)
	}
	var charStrings [][]byte
	for _, g := range s.Glyphs {
		var cs []byte
		x, y := 0, 0
		op := func(code byte, coords ...int) {
			for i := 0; i < len(coords); i += 2 {
				cs = append(cs, cffNum(coords[i]-x)...)
				cs = append(cs, cffNum(coords[i+1]-y)...)
				x, y = coords[i], coords[i+1]
			}
			cs = append(cs, code)
		}
		for _, sg := range g.Path {
			switch sg.Kind {
			case 'M':
				op(21, sg.X, sg.Y)
			case 'L':
				op(5, sg.X, sg.Y)
			case 'Q':
				// the cubic of the same curve, to whole units
				op(8, x+2*(sg.X1-x)/3, y+2*(sg.Y1-y)/3, sg.X+2*(sg.X1-sg.X)/3, sg.Y+2*(sg.Y1-sg.Y)/3, sg.X, sg.Y)
			case 'C':
				op(8, sg.X1, sg.Y1, sg.X2, sg.Y2, sg.X, sg.Y)
			}
		}
		charStrings = append(charStrings, append(cs, end...))
	}
	header := []byte{1, 0, 4, 1}
	names := cffIndex([][]byte{[]byte("Test")})
	strings := cffIndex(nil)
	globals := cffIndex(global)
	chars := cffIndex(charStrings)
	// the Top DICT names the CharStrings and the Private DICT by offsets
	// that depend on its own length: five bytes each, so that it is known
	topLen := 5 + 1 + 5 + 5 + 1
	topIndex := func(top []byte) []byte { return cffIndex([][]byte{top}) }
	before := len(header) + len(names) + len(topIndex(make([]byte, topLen))) + len(strings) + len(globals)
	private := []byte{}
	if len(local) > 0 {
		private = append(cffOffset(6), 19) // Subrs, right after this DICT
	}
	top := append(cffOffset(before), 17)
	top = append(top, cffOffset(len(private))...)
	top = append(top, cffOffset(before+len(chars))...)
	top = append(top, 18)
	cff := append([]byte(nil), header...)
	cff = append(cff, names...)
	cff = append(cff, topIndex(top)...)
	cff = append(cff, strings...)
	cff = append(cff, globals...)
	cff = append(cff, chars...)
	cff = append(cff, private...)
	if len(local) > 0 {
		cff = append(cff, cffIndex(local)...)
	}
	t := s.common(false)
	t["CFF "] = cff
	var maxp be
	maxp.u32(0x00005000)
	maxp.u16(len(s.Glyphs))
	t["maxp"] = maxp.b
	return sfntFile(0x4f54544f, t)
}

// RandomFont is a font of random glyphs for the characters given: boxes
// and blobs within the em, some with holes, some (TrueType) made of other
// glyphs. cubic says whether its curves are cubic, as those of CFF are.
func RandomFont(r *rand.Rand, family string, weight int, italic bool, runes []rune, cubic bool) FontSpec {
	s := FontSpec{Family: family, Style: "Regular", Weight: weight, Italic: italic, UnitsPerEm: pickOf(r, []int{1000, 1000, 2048, 256}),
		Cmap: map[rune]int{}, GlobalSubr: r.IntN(2) == 0, LocalSubr: r.IntN(2) == 0}
	if italic {
		s.Style = "Italic"
	}
	em := s.UnitsPerEm
	s.Ascent, s.Descent = em*8/10, -em*2/10
	if r.IntN(5) == 0 {
		s.Ascent, s.Descent = em, -em/2
	}
	contour := func(x0, y0, x1, y1 int, reverse bool) []FontSeg {
		// a ring of points around the box, some of its sides curved
		n := 3 + r.IntN(4)
		cx, cy := (x0+x1)/2, (y0+y1)/2
		type p struct{ x, y int }
		var ring []p
		for i := range n {
			// around the centre, in the order of the corners of a polygon
			q := [...][2]int{{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}}[i*4/n%4]
			ring = append(ring, p{cx + (q[0]-cx)*(6+r.IntN(5))/10, cy + (q[1]-cy)*(6+r.IntN(5))/10})
		}
		if reverse {
			for i, j := 0, len(ring)-1; i < j; i, j = i+1, j-1 {
				ring[i], ring[j] = ring[j], ring[i]
			}
		}
		segs := []FontSeg{{Kind: 'M', X: ring[0].x, Y: ring[0].y}}
		for i := 1; i <= len(ring); i++ {
			to, from := ring[i%len(ring)], ring[i-1]
			if i == len(ring) && r.IntN(2) == 0 {
				break // the last side is the closing of the contour
			}
			switch r.IntN(3) {
			case 0:
				mx, my := (from.x+to.x)/2+(to.y-from.y)/4, (from.y+to.y)/2-(to.x-from.x)/4
				if cubic {
					segs = append(segs, FontSeg{Kind: 'C', X1: from.x + (mx-from.x)/2, Y1: from.y + (my - from.y), X2: to.x + (mx - to.x), Y2: to.y + (my-to.y)/2, X: to.x, Y: to.y})
				} else {
					segs = append(segs, FontSeg{Kind: 'Q', X1: mx, Y1: my, X: to.x, Y: to.y})
				}
			default:
				segs = append(segs, FontSeg{Kind: 'L', X: to.x, Y: to.y})
			}
		}
		return segs
	}
	s.Glyphs = append(s.Glyphs, FontGlyph{Advance: em / 2, Path: contour(em/10, 0, em*4/10, em*7/10, false)})
	for _, c := range runes {
		g := FontGlyph{Advance: em * (3 + r.IntN(8)) / 10}
		switch {
		case c == ' ':
			// no outline
		case !cubic && len(s.Glyphs) > 2 && r.IntN(5) == 0:
			g.Components = []FontComponent{{Glyph: 1 + r.IntN(len(s.Glyphs)-1)}, {Glyph: 1 + r.IntN(len(s.Glyphs)-1), DX: em / 4, DY: em / 8, Scale: 0.5}}
		default:
			w := g.Advance * 9 / 10
			g.Path = contour(em/20, -em/10*r.IntN(2), w, em*(5+r.IntN(4))/10, false)
			if r.IntN(3) == 0 {
				g.Path = append(g.Path, contour(w/4, em/5, w*3/4, em*2/5, true)...) // a hole
			}
		}
		s.Cmap[c] = len(s.Glyphs)
		s.Glyphs = append(s.Glyphs, g)
	}
	return s
}

// TestFonts are the fonts the tests make, by file name: families of
// several weights and slants with TrueType outlines and with CFF ones, for
// Latin letters, digits, a few Hebrew and Japanese characters and one past
// the BMP. The same seed makes the same fonts.
func TestFonts(seed uint64) map[string][]byte {
	r := rand.New(rand.NewPCG(seed, 77))
	var latin []rune
	for c := rune(' '); c <= '~'; c++ {
		latin = append(latin, c)
	}
	some := func(from, to rune, n int) []rune {
		var out []rune
		for range n {
			out = append(out, from+rune(r.IntN(int(to-from)+1)))
		}
		return out
	}
	hebrew := append(append([]rune(nil), latin...), some(0x5d0, 0x5ea, 12)...)
	wide := append(append([]rune{' ', 0x3053, 0x3093, 0x306b, 0x3061, 0x306f, 0x65e5, 0x672c, 0x8a9e, 0x1d49c}, some(0x3041, 0x3093, 10)...), latin[:40]...)
	return map[string][]byte{
		"a-sans-regular.ttf":     BuildTTF(RandomFont(r, "Test Sans", 400, false, hebrew, false)),
		"a-sans-bold.ttf":        BuildTTF(RandomFont(r, "Test Sans", 700, false, latin, false)),
		"a-sans-italic.ttf":      BuildTTF(RandomFont(r, "Test Sans", 400, true, latin, false)),
		"b-serif-regular.otf":    BuildCFF(RandomFont(r, "Test Serif", 400, false, latin, true)),
		"b-serif-bolditalic.otf": BuildCFF(RandomFont(r, "Test Serif", 800, true, latin[:60], true)),
		"c-mono.otf":             BuildCFF(RandomFont(r, "Test Mono", 500, false, wide, true)),
		"d-wide.ttf":             BuildTTF(RandomFont(r, "Noto Sans CJK JP", 400, false, wide, false)),
	}
}
