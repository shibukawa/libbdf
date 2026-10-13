package cbdf

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"math/rand/v2"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/shibukawa/bdf"
)

var nodeKinds = map[uint8]string{
	NodeParagraph: "paragraph", NodeHeading: "heading", NodeCaption: "caption", NodeList: "list",
	NodeItem: "item", NodeTable: "table", NodeCell: "cell", NodeFigure: "figure",
}

// jsString is s as JSON.stringify writes it.
func jsString(s string) string {
	var b strings.Builder
	enc := json.NewEncoder(&b)
	enc.SetEscapeHTML(false)
	enc.Encode(s)
	out := strings.TrimSuffix(b.String(), "\n")
	// encoding/json escapes the two line separators JSON.stringify leaves alone
	out = strings.ReplaceAll(out, `\u2028`, "\u2028")
	return strings.ReplaceAll(out, `\u2029`, "\u2029")
}

func floatBits(v float32) string {
	if v != v {
		return "nan"
	}
	return fmt.Sprintf("%08x", math.Float32bits(v))
}

// dumpContent writes the structure as tools/structure-oracle.mjs does.
func dumpContent(runs []TextRun, links []TextLink, nodes []TextNode) []string {
	var lines []string
	for i, n := range nodes {
		s := fmt.Sprintf("N %d %s %d", i, nodeKinds[n.Kind], n.Parent)
		if n.Level != 0 {
			s += fmt.Sprintf(" level=%d", n.Level)
		}
		if n.HasCell {
			s += fmt.Sprintf(" cell=%d,%d,%d,%d", n.Row, n.Col, n.Rows, n.Cols)
		}
		switch n.Scope {
		case ScopeCol:
			s += " scope=col"
		case ScopeRow:
			s += " scope=row"
		}
		if n.Kind == NodeFigure {
			s += " alt=" + jsString(n.Alt)
		}
		if n.HasBounds {
			s += " bounds=" + floatBits(n.X) + "," + floatBits(n.Y) + "," + floatBits(n.W) + "," + floatBits(n.H)
		}
		lines = append(lines, s)
	}
	key := func(r TextRun) string {
		if r.Lang == "" {
			return fmt.Sprintf("%d -", r.Node)
		}
		return fmt.Sprintf("%d %s", r.Node, jsString(r.Lang))
	}
	from := 0
	for i := 1; i <= len(runs); i++ {
		if i == len(runs) || key(runs[i]) != key(runs[from]) {
			lines = append(lines, fmt.Sprintf("R %d %d %s", from, i-1, key(runs[from])))
			from = i
		}
	}
	for i, l := range links {
		lines = append(lines, fmt.Sprintf("L %d %d %d", i, l.Node, l.After))
	}
	return lines
}

// TestStructureMatchesTypeScript compares the structure of every page layer
// of the test documents with what the reference's TypeScript reader makes of
// it (testdata/structure.txt.gz, written by tools/structure-oracle.mjs).
func TestStructureMatchesTypeScript(t *testing.T) {
	f, err := os.Open(filepath.Join("testdata", "structure.txt.gz"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		t.Fatal(err)
	}
	type block struct {
		head  []string
		lines []string
	}
	var blocks []*block
	sc := bufio.NewScanner(zr)
	sc.Buffer(make([]byte, 1<<20), 1<<26)
	for sc.Scan() {
		line := sc.Text()
		if strings.HasPrefix(line, "== ") {
			blocks = append(blocks, &block{head: strings.Fields(line)[1:]})
		} else if len(blocks) > 0 {
			blocks[len(blocks)-1].lines = append(blocks[len(blocks)-1].lines, line)
		}
	}
	if err := sc.Err(); err != nil {
		t.Fatal(err)
	}
	dir := testdataDir(t)
	docs := map[string]*Doc{}
	defer func() {
		for _, d := range docs {
			d.Close()
		}
	}()
	objects, nodes, figures, missing := 0, 0, 0, 0
	for _, b := range blocks {
		name, hash := b.head[0], b.head[4]
		where := strings.Join(b.head[:4], " ")
		d := docs[name]
		if d == nil {
			data, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(name)))
			if err != nil {
				missing++ // a document of a newer reference than the pinned one
				continue
			}
			if d, err = OpenMemory(data); err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			if d.Locked() {
				if err := d.Unlock("demo-パスワード"); err != nil {
					t.Fatalf("%s: %v", name, err)
				}
			}
			docs[name] = d
		}
		var h [16]byte
		if _, err := hex.Decode(h[:], []byte(hash)); err != nil {
			t.Fatal(err)
		}
		if !d.Has(h) {
			missing++ // the document changed since
			continue
		}
		runs, links, ns, err := d.Content(h)
		if b.head[5] == "error" {
			if err == nil {
				t.Errorf("%s: libbdf reads what the TypeScript reader refuses", where)
			}
			continue
		}
		if err != nil {
			t.Errorf("%s: %v", where, err)
			continue
		}
		if got := fmt.Sprintf("%d %d %d", len(ns), len(runs), len(links)); got != strings.Join(b.head[5:], " ") {
			t.Errorf("%s: %s nodes, runs and links, the TypeScript reader has %s", where, got, strings.Join(b.head[5:], " "))
			continue
		}
		got := dumpContent(runs, links, ns)
		if d := firstDiff(strings.Join(b.lines, "\n"), strings.Join(got, "\n")); d != "" {
			t.Errorf("%s: %s", where, d)
			continue
		}
		objects++
		nodes += len(ns)
		for _, n := range ns {
			if n.Kind == NodeFigure {
				figures++
			}
		}
	}
	if objects < 300 {
		t.Errorf("only %d objects compared (%d not in this testdata)", objects, missing)
	}
	t.Logf("%d objects, %d nodes, %d figures match; %d not in this testdata", objects, nodes, figures, missing)
}

// ---- Random objects ---------------------------------------------------------

var (
	cellPayloads    = []string{"A1", "B2", "C3:D4", "A1 col", "B1 row", " aa10 ", "A0", "A1:", "1A", "Z99 col ", "A1\u00a0row", "XFD1048576", "A1 colx", "", "B2:A1", "A1:C1 col", "A1 col row", "A1\u3000", "AAAAAAAAAA1", "A99999999999", "a1:b2\trow"}
	headingPayloads = []string{"1", "2", "3", "6", "7", "0", "3.0", " 4 ", "0x5", "5e0", "50e-1", "abc", "", "+2", "-1", "1.5", "0b11", "０１", "0o6", ".3e1", "2.", "1e", "0x", "\u30002\u00a0", "6.000", "0.6e1", "Infinity"}
	figurePayloads  = []string{"", "a figure", "図の説明", "with \"quotes\" and \\", "line\nbreak"}
	langPayloads    = []string{"ja", "en-US", "", "zh-Hant"}
	textPayloads    = []string{"text", "日本語", "😀 emoji", "", "a", "longer run of text here", "é\u0301", "\U0001f468\u200d\U0001f469\u200d\U0001f467", "\ufeffmarked", "bad\xff\xe3\x81bytes"}
)

func pick[T any](r *rand.Rand, from []T) T { return from[r.IntN(len(from))] }

// coordinate is a float a drawing might use: mostly small, at times zero,
// negative zero, or large.
func coordinate(r *rand.Rand) float32 {
	switch r.IntN(20) {
	case 0:
		return 0
	case 1:
		return float32(math.Copysign(0, -1))
	case 2:
		return float32(r.NormFloat64() * 1e6)
	case 3:
		return float32(r.IntN(200) - 100)
	}
	return float32(r.NormFloat64() * 100)
}

// randomObject draws marks, text, shapes and, with child, uses of the part
// 01…, in an order no writer would.
func randomObject(r *rand.Rand, child bool) []byte {
	o := bdf.NewObject()
	font := o.AddFont(bdf.SystemFont("sans-serif", 400, bdf.StyleNormal))
	var paths []bdf.PathRef
	for range 1 + r.IntN(4) {
		p := &bdf.Path{}
		for range 1 + r.IntN(5) {
			c := func() float32 { return coordinate(r) }
			switch r.IntN(9) {
			case 0:
				p.MoveTo(c(), c())
			case 1:
				p.LineTo(c(), c())
			case 2:
				p.QuadTo(c(), c(), c(), c())
			case 3:
				p.CubicTo(c(), c(), c(), c(), c(), c())
			case 4:
				p.Close()
			case 5:
				p.Rect(c(), c(), c(), c())
			case 6:
				p.Ellipse(c(), c(), c(), c(), c(), c(), c(), r.IntN(2) == 0)
			case 7:
				p.ArcTo(c(), c(), c(), c(), c())
			case 8:
				p.RoundRect(c(), c(), c(), c(), c())
			}
		}
		paths = append(paths, o.AddPath(p))
	}
	if r.IntN(4) == 0 {
		paths = append(paths, o.AddPath(&bdf.Path{})) // a path with no points
	}
	if r.IntN(4) == 0 {
		paths = append(paths, o.AddExtPath(bdf.Hash{9}, 0)) // one this object does not hold
	}
	img := o.AddImage(bdf.Hash{7})
	var use bdf.ObjRef
	if child {
		use = o.AddObject(bdf.Hash{1}, bdf.Rect{X: coordinate(r), Y: coordinate(r), W: coordinate(r), H: coordinate(r)})
	}
	// SAVE and RESTORE pair up inside a mask and outside it, never across its
	// ends: the two readers of the reference differ on a stream that does
	// (text.go skips what is in a mask, text.ts restores at its end), and no
	// writer makes one
	depth, masks, maskDepth := 0, 0, 0
	for range 5 + r.IntN(60) {
		c := func() float32 { return coordinate(r) }
		switch r.IntN(30) {
		case 0, 1, 2, 3, 4:
			kind := byte(r.IntN(14))
			payload := pick(r, textPayloads)
			switch kind {
			case bdf.MarkCell:
				payload = pick(r, cellPayloads)
			case bdf.MarkHeading:
				payload = pick(r, headingPayloads)
			case bdf.MarkFigure:
				payload = pick(r, figurePayloads)
			case bdf.MarkLang:
				payload = pick(r, langPayloads)
			}
			o.Mark(kind, payload)
		case 5, 6:
			o.Mark(pick(r, []byte{bdf.MarkList, bdf.MarkListItem, bdf.MarkTable, bdf.MarkCell, bdf.MarkFigure, bdf.MarkEnd, bdf.MarkEnd}), pick(r, cellPayloads))
		case 7, 8, 9:
			adv := float32(0)
			if r.IntN(2) == 0 {
				adv = float32(r.IntN(100))
			}
			if r.IntN(5) == 0 {
				o.StrokeText(pick(r, textPayloads), c(), c(), adv)
			} else {
				o.FillText(pick(r, textPayloads), c(), c(), adv)
			}
		case 10:
			o.Font(font, float32(r.IntN(30)))
		case 11:
			o.TextStyle(byte(r.IntN(5)), 0, 0, 0)
		case 12:
			o.Save()
			depth++
		case 13:
			if depth > maskDepth {
				o.Restore()
				depth--
			}
		case 14:
			o.Transform(float32(r.NormFloat64()), float32(r.NormFloat64()), float32(r.NormFloat64()), float32(r.NormFloat64()), c(), c())
		case 15:
			o.Translate(c(), c())
		case 16:
			o.Scale(float32(r.NormFloat64()*2), float32(r.NormFloat64()*2))
		case 17:
			o.ClipRect(c(), c(), c(), c())
		case 18:
			o.ClipPath(pick(r, paths), bdf.NonZero)
		case 19:
			o.FillRect(c(), c(), c(), c())
		case 20:
			o.StrokeRect(c(), c(), c(), c())
		case 21:
			if r.IntN(2) == 0 {
				o.FillPath(pick(r, paths), bdf.EvenOdd)
			} else {
				o.StrokePath(pick(r, paths))
			}
		case 22:
			o.FillPathAt(pick(r, paths), bdf.NonZero, c(), c())
		case 23:
			var glyphs []bdf.Glyph
			for range r.IntN(4) {
				glyphs = append(glyphs, bdf.Glyph{Path: pick(r, paths), X: c(), Y: c()})
			}
			o.FillPathRun(bdf.NonZero, glyphs)
		case 24:
			if r.IntN(2) == 0 {
				o.Image(img, c(), c(), c(), c())
			} else {
				o.ImageSub(img, c(), c(), c(), c(), c(), c(), c(), c())
			}
		case 25:
			if child {
				if r.IntN(2) == 0 {
					o.Use(use)
				} else {
					o.UseAt(use, c(), c())
				}
			}
		case 26:
			if masks == 0 {
				o.MaskBegin(bdf.MaskAlpha, 0, nil)
				masks++
				maskDepth = depth
			} else {
				for ; depth > maskDepth; depth-- {
					o.Restore()
				}
				o.MaskEnd()
				masks--
				maskDepth = 0
			}
		case 27:
			o.Link(c(), c(), c(), c(), "https://example.com/")
		case 28:
			o.Mark(bdf.MarkAltText, pick(r, textPayloads))
		case 29:
			o.ClearRect(c(), c(), c(), c())
		}
	}
	for ; masks > 0; masks-- {
		for ; depth > maskDepth; depth-- {
			o.Restore()
		}
		o.MaskEnd()
	}
	for ; depth > 0; depth-- {
		o.Restore()
	}
	return o.Encode()
}

// TestStructureRandom walks random objects with libbdf and with the
// reference's TypeScript reader, which it runs with node: set
// LIBBDF_BDF_CHECKOUT to a checkout of bdf with packages/core built. The
// test documents hold what writers make; this holds what they do not.
// LIBBDF_STRUCTURE_CASES and LIBBDF_STRUCTURE_SEED change how many objects
// and which.
func TestStructureRandom(t *testing.T) {
	checkout := os.Getenv("LIBBDF_BDF_CHECKOUT")
	if checkout == "" {
		t.Skip("LIBBDF_BDF_CHECKOUT is not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node is not installed")
	}
	cases, seed := 20000, uint64(7)
	if n, err := strconv.Atoi(os.Getenv("LIBBDF_STRUCTURE_CASES")); err == nil && n > 0 {
		cases = n
	}
	if n, err := strconv.ParseUint(os.Getenv("LIBBDF_STRUCTURE_SEED"), 10, 64); err == nil {
		seed = n
	}
	r := rand.New(rand.NewPCG(20261013, seed))
	type pair struct {
		Top   []byte `json:"top"`
		Child []byte `json:"child,omitempty"`
	}
	pairs := make([]pair, cases)
	var in bytes.Buffer
	enc := json.NewEncoder(&in)
	for i := range pairs {
		withChild := i%3 != 0
		pairs[i].Top = randomObject(r, withChild)
		if withChild {
			pairs[i].Child = randomObject(r, false)
		}
		enc.Encode(pairs[i])
	}
	file := filepath.Join(t.TempDir(), "pairs.jsonl")
	if err := os.WriteFile(file, in.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	script, _ := filepath.Abs(filepath.Join("..", "tools", "structure-oracle.mjs"))
	out, err := exec.Command(node, script, checkout, "--objects", file).Output()
	if err != nil {
		t.Fatalf("node: %v", err)
	}
	blocks := strings.Split(strings.TrimPrefix(string(out), "== "), "\n== ")
	if len(blocks) != cases {
		t.Fatalf("%d answers for %d objects", len(blocks), cases)
	}
	nodes, figures := 0, 0
	for i, b := range blocks {
		head, body, _ := strings.Cut(strings.TrimSuffix(b, "\n"), "\n")
		top, err := DecodeObject(pairs[i].Top)
		if err != nil {
			t.Fatalf("object %d: %v", i, err)
		}
		var child *Object
		if pairs[i].Child != nil {
			if child, err = DecodeObject(pairs[i].Child); err != nil {
				t.Fatalf("child %d: %v", i, err)
			}
		}
		runs, links, ns, err := ExtractContent(top, child)
		top.Close()
		if child != nil {
			child.Close()
		}
		if err != nil {
			t.Fatalf("object %d: %v (the TypeScript reader: %s)", i, err, head)
		}
		if want := fmt.Sprintf("%d %d %d %d", i, len(ns), len(runs), len(links)); head != want {
			t.Fatalf("object %d: nodes, runs and links %q, the TypeScript reader has %q\n%x\n%x", i, want, head, pairs[i].Top, pairs[i].Child)
		}
		if d := firstDiff(body, strings.Join(dumpContent(runs, links, ns), "\n")); d != "" {
			t.Fatalf("object %d: %s\n%x\n%x", i, d, pairs[i].Top, pairs[i].Child)
		}
		nodes += len(ns)
		for _, n := range ns {
			if n.Kind == NodeFigure {
				figures++
			}
		}
	}
	t.Logf("%d objects, %d nodes, %d figures match", cases, nodes, figures)
}
