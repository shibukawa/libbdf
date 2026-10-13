package svgdoc

import (
	"errors"
	"math"
	"runtime"
	"sort"
	"strconv"
	"strings"

	"github.com/shibukawa/bdf/image/svg"
)

// exactFloats reports whether the reference computes as the C does: without
// fused multiply-adds, which Go's compiler makes of a product and a sum on
// arm64 (Matrix.Mul, hslToRGB, math.Sincos). The C never fuses, so its
// floats are those of the reference on amd64, to the bit; on arm64 the
// transforms are not compared and the colours to 24 binary places.
var exactFloats = runtime.GOARCH == "amd64"

// byLen is the order the C reader keeps its tables in: by length, then by
// bytes.
func byLen(keys []string) {
	sort.Slice(keys, func(i, j int) bool {
		if len(keys[i]) != len(keys[j]) {
			return len(keys[i]) < len(keys[j])
		}
		return keys[i] < keys[j]
	})
}

type dumper struct {
	b           []byte
	deep, exact bool
}

func (d *dumper) str(s string) { d.b = append(d.b, s...) }

func (d *dumper) int(v int) { d.b = strconv.AppendInt(d.b, int64(v), 10) }

// quoted writes a string with everything but printable ASCII escaped.
func (d *dumper) quoted(s string) {
	const hex = "0123456789abcdef"
	for i := 0; i < len(s); i++ {
		if c := s[i]; c >= 0x20 && c < 0x7f && c != '\\' {
			d.b = append(d.b, c)
		} else {
			d.b = append(d.b, '\\', 'x', hex[c>>4], hex[c&15])
		}
	}
}

func (d *dumper) float(v float64) {
	if v != v {
		d.str("nan")
		return
	}
	s := strconv.FormatUint(math.Float64bits(v), 16)
	d.str("0000000000000000"[len(s):])
	d.str(s)
}

func (d *dumper) near(v float64) {
	if d.exact || !(v >= -4 && v <= 4) {
		d.float(v)
		return
	}
	d.b = strconv.AppendInt(d.b, int64(math.Floor(v*16777216.0+0.5)), 10)
}

func (d *dumper) bool(ok bool) {
	if ok {
		d.str("1")
	} else {
		d.str("0")
	}
}

// values writes what the value functions make of a string.
func (d *dumper) values(v string) {
	f, rest, ok := svg.ParseNumber(v)
	d.str("n\t")
	d.bool(ok)
	d.str("\t")
	d.float(f)
	d.str("\t")
	d.int(len(rest))
	d.str("\nN\t")
	nums := svg.ParseNumbers(v)
	d.int(len(nums))
	for _, x := range nums {
		d.str("\t")
		d.float(x)
	}
	f, ok = svg.ParseLength(v, 100, 16)
	d.str("\nL\t")
	d.bool(ok)
	d.str("\t")
	d.float(f)
	c, ok := svg.ParseColor(v, svg.Color{R: 0.25, G: 0.5, B: 0.75, A: 1})
	d.str("\nC\t")
	d.bool(ok)
	for _, x := range []float64{c.R, c.G, c.B, c.A} {
		d.str("\t")
		d.near(x)
	}
	d.str("\n")
	if d.exact {
		m := svg.ParseTransform(v)
		d.str("M")
		for _, x := range m {
			d.str("\t")
			d.float(x)
		}
		d.str("\n")
	}
}

func (d *dumper) declared(n *svg.Node, name string) {
	v, ok := n.Declared(name)
	d.str("d\t")
	d.quoted(name)
	d.str("\t")
	d.bool(ok)
	d.str("\t")
	d.quoted(v)
	d.str("\n")
}

// fixedNames is the properties and attributes the renderer asks for
// (raster/imagebdf/svg.go).
var fixedNames = []string{"color", "fill", "stroke", "display", "opacity", "overflow", "clip-path", "mask", "stop-color", "stop-opacity", "font-family", "font-size", "font-style", "font-weight", "letter-spacing", "stroke-dasharray", "stroke-dashoffset", "stroke-linecap", "stroke-linejoin", "stroke-miterlimit", "stroke-width", "text-anchor", "visibility", "transform", "style", "class", "id", "href", "x", "d"}

func sortedKeys(m map[string]string) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	byLen(keys)
	return keys
}

// styleNames is the names a style attribute declares, as parseDecls of the
// reference reads them (it is not exported): each once.
func styleNames(style string) []string {
	seen := map[string]bool{}
	var names []string
	for _, piece := range strings.Split(style, ";") {
		k, _, ok := strings.Cut(piece, ":")
		if !ok {
			continue
		}
		k = strings.ToLower(strings.TrimSpace(k))
		if !seen[k] {
			seen[k] = true
			names = append(names, k)
		}
	}
	byLen(names)
	return names
}

func (d *dumper) node(n *svg.Node, depth int) {
	// a run of text is what is named so: an element that a document names
	// "#text" is one with attributes, and maybe children
	isText := n.Name == svg.TextNode
	if !isText && n.Text != "" || n.Attr == nil && (!isText || len(n.Children) != 0) || isText && n.Sheet != nil {
		panic("a node that is neither an element nor a run of text")
	}
	if isText {
		d.str("T\t")
	} else {
		d.str("E\t")
	}
	d.int(depth)
	d.str("\t")
	d.quoted(n.Name)
	d.str("\t")
	d.int(len(n.Children))
	d.str("\t")
	d.quoted(n.Text)
	d.str("\n")
	attrs := sortedKeys(n.Attr)
	for _, k := range attrs {
		d.str("A\t")
		d.quoted(k)
		d.str("\t")
		d.quoted(n.Attr[k])
		d.str("\n")
		if d.deep {
			d.values(n.Attr[k])
		}
	}
	sheet := sortedKeys(n.Sheet)
	if n.Sheet == nil {
		d.str("S\t-\n")
	} else {
		d.str("S\t")
		d.int(len(sheet))
		d.str("\n")
		for _, k := range sheet {
			d.str("D\t")
			d.quoted(k)
			d.str("\t")
			d.quoted(n.Sheet[k])
			d.str("\n")
		}
	}
	if d.deep && (!isText || len(attrs) > 0) {
		for _, k := range attrs {
			d.declared(n, k)
		}
		for _, k := range sheet {
			d.declared(n, k)
		}
		for _, k := range styleNames(n.Attr["style"]) {
			d.declared(n, k)
		}
		for _, k := range fixedNames {
			d.declared(n, k)
		}
	}
}

// tree writes a tree in the order of the document and notes the place of
// each of its nodes in that order.
func (d *dumper) tree(top *svg.Node, place map[*svg.Node]int) {
	type frame struct {
		n     *svg.Node
		depth int
	}
	stack := []frame{{top, 1}}
	for len(stack) > 0 {
		f := stack[len(stack)-1]
		stack = stack[:len(stack)-1]
		place[f.n] = len(place)
		d.node(f.n, f.depth)
		for i := len(f.n.Children) - 1; i >= 0; i-- {
			if f.n.Children[i].Parent != f.n {
				panic("a child of another parent")
			}
			stack = append(stack, frame{f.n.Children[i], f.depth + 1})
		}
	}
}

// goDump is the reference's document of data as text: what svgport_dump
// writes of the C reader's. A panic of the reference, which its renderer
// recovers from and takes the image as unreadable, is "panic".
func goDump(data []byte, opts *svg.Options, deep, exact bool) (out string) {
	doc, err, panicked := func() (doc *svg.Document, err error, panicked bool) {
		defer func() {
			if r := recover(); r != nil {
				panicked = true
			}
		}()
		doc, err = svg.Parse(data, opts)
		return doc, err, false
	}()
	if panicked {
		return "panic\n"
	}
	if err != nil {
		if !errors.Is(err, svg.ErrNotSVG) || doc != nil {
			return "error " + err.Error() + "\n"
		}
		return "notsvg\n"
	}
	d := &dumper{deep: deep, exact: exact}
	d.str("doc\n")
	places := map[*svg.Node]map[*svg.Node]int{doc.Root: {}}
	d.tree(doc.Root, places[doc.Root])
	ids := make([]string, 0, len(doc.IDs))
	for id := range doc.IDs {
		ids = append(ids, id)
	}
	byLen(ids)
	var tops []*svg.Node
	var later dumper
	later.deep, later.exact = deep, exact
	for _, id := range ids {
		n := doc.IDs[id]
		top := n
		for top.Parent != nil {
			top = top.Parent
		}
		t := 0
		if top != doc.Root {
			if places[top] == nil {
				tops = append(tops, top)
				places[top] = map[*svg.Node]int{}
				later.str("O\t")
				later.int(len(tops))
				later.str("\n")
				later.tree(top, places[top])
			}
			for t = 1; tops[t-1] != top; t++ {
			}
		}
		d.str("I\t")
		d.quoted(id)
		d.str("\t")
		d.int(t)
		d.str("\t")
		d.int(places[top][n])
		d.str("\n")
	}
	d.b = append(d.b, later.b...)
	for _, w := range doc.Warnings {
		d.str("W\t")
		d.str(w)
		d.str("\n")
	}
	return string(d.b)
}

// goValues is what the reference's value functions make of s.
func goValues(s string, exact bool) string {
	d := &dumper{exact: exact}
	d.values(s)
	return string(d.b)
}
