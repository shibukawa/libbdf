package cbdf

import (
	"encoding/base64"
	"fmt"
	"math/rand/v2"
	"net/url"
	"strings"

	"github.com/shibukawa/bdf"
)

// svgGen writes a random SVG document: what the renderer of SVG images
// draws (shapes and paths, fills and strokes, gradients, transforms, use
// and symbol, nested svg, clip paths, group opacity, text, embedded images),
// what it leaves out with a warning, and values written in the many ways
// SVG allows, and in some it does not.
type svgGen struct {
	r     *rand.Rand
	b     strings.Builder
	ids   []string // the ids of the elements written so far, and of some to come
	depth int
	// inside is how many SVG images this one is in (as a data: URL)
	inside int
}

func (g *svgGen) pick(from ...string) string { return from[g.r.IntN(len(from))] }

// num is a number as SVG writes numbers: plain, signed, with an exponent,
// without a leading zero.
func (g *svgGen) num(lo, hi int) string {
	v := float64(lo) + g.r.Float64()*float64(hi-lo)
	switch g.r.IntN(12) {
	case 0:
		return fmt.Sprintf("%d", int(v))
	case 1:
		return fmt.Sprintf("%.3e", v)
	case 2:
		return strings.TrimPrefix(fmt.Sprintf("%.2f", v/float64(max(hi, 1))), "0")
	case 3:
		return fmt.Sprintf("+%.1f", v)
	}
	return fmt.Sprintf("%.2f", v)
}

// length is a number with a unit now and then, a percentage, or not a
// length at all.
func (g *svgGen) length(lo, hi int) string {
	switch g.r.IntN(16) {
	case 0:
		return g.num(lo, hi) + g.pick("px", "pt", "mm", "PX", "pc", " px")
	case 1:
		return g.num(0, 100) + "%"
	case 2:
		return g.num(0, 4) + g.pick("em", "ex", "cm", "in", "Q")
	case 3:
		return g.pick("", "auto", "none", "12 px x", "1e400", "-", "NaN", ".")
	}
	return g.num(lo, hi)
}

func (g *svgGen) color() string {
	switch g.r.IntN(12) {
	case 0:
		return g.pick("none", "transparent", "currentColor", "inherit", "", "no such colour")
	case 1:
		return fmt.Sprintf("#%03x", g.r.IntN(0x1000))
	case 2:
		return fmt.Sprintf("rgb(%d, %d, %d)", g.r.IntN(256), g.r.IntN(256), g.r.IntN(256))
	case 3:
		return fmt.Sprintf("rgba(%d,%d,%d,%.2f)", g.r.IntN(256), g.r.IntN(256), g.r.IntN(256), g.r.Float64())
	case 4:
		return fmt.Sprintf("rgb(%d%%, %d%%, %d%%)", g.r.IntN(101), g.r.IntN(101), g.r.IntN(101))
	case 5:
		return fmt.Sprintf("hsl(%d, %d%%, %d%%)", g.r.IntN(360), g.r.IntN(101), g.r.IntN(101))
	case 6:
		return g.pick("red", "Green", "steelblue", "GOLD", "rebeccapurple", "black", "white", "grey")
	case 7:
		return fmt.Sprintf("#%08x", g.r.Uint32())
	}
	return fmt.Sprintf("#%06x", g.r.IntN(0x1000000))
}

// ref is a reference to an element: one that is there, mostly.
func (g *svgGen) ref() string {
	if len(g.ids) == 0 || g.r.IntN(12) == 0 {
		return g.pick("#nothing", "", "#", "url(#nothing)", "http://example.com/x.svg#a")
	}
	return "#" + g.ids[g.r.IntN(len(g.ids))]
}

func (g *svgGen) urlRef() string {
	s := "url(" + g.pick("", " ", "'", "\"") + g.ref()
	if g.r.IntN(8) == 0 {
		return s // not closed
	}
	return s + g.pick(")", ")", ") red", ") none", "') #0af", "\")")
}

// paint is a fill or stroke value.
func (g *svgGen) paint() string {
	if g.r.IntN(4) == 0 {
		return g.urlRef()
	}
	return g.color()
}

func (g *svgGen) transform() string {
	var parts []string
	for range 1 + g.r.IntN(3) {
		switch g.r.IntN(8) {
		case 0:
			parts = append(parts, fmt.Sprintf("translate(%s)", g.num(-20, 40)))
		case 1:
			parts = append(parts, fmt.Sprintf("scale(%s %s)", g.num(0, 2), g.num(0, 2)))
		case 2:
			parts = append(parts, fmt.Sprintf("rotate(%s, %s, %s)", g.num(-180, 180), g.num(0, 100), g.num(0, 100)))
		case 3:
			parts = append(parts, fmt.Sprintf("skewX(%s)", g.num(-60, 60)))
		case 4:
			parts = append(parts, fmt.Sprintf("matrix(%s %s %s %s %s %s)", g.num(0, 2), g.num(-1, 1), g.num(-1, 1), g.num(0, 2), g.num(-20, 20), g.num(-20, 20)))
		case 5:
			parts = append(parts, fmt.Sprintf("rotate(%s)", g.num(-90, 90)), g.pick("skewY(10)", "scale(-1,1)", "nothing(3)", "scale(0)", "translate(1e30)"))
		default:
			parts = append(parts, fmt.Sprintf("translate(%s,%s)", g.num(-20, 40), g.num(-20, 40)))
		}
	}
	return strings.Join(parts, g.pick(" ", ", ", ""))
}

// pathData is path data: every command, absolute and relative, repeated
// without its letter, flags written against what follows, and errors that
// end the path where they are. One thing is left out: anything but a
// command after a Z, at which the reference does not stop (it closes the
// path until it has no memory left).
func (g *svgGen) pathData() string {
	var b strings.Builder
	sep := func() string { return g.pick(" ", ",", " , ", "", "\n") }
	n := func() string { return g.num(-10, 110) }
	fmt.Fprintf(&b, "%s%s %s", g.pick("M", "M", "m", " M"), n(), n())
	if g.r.IntN(6) == 0 {
		fmt.Fprintf(&b, " %s %s %s %s", n(), n(), n(), n()) // line-tos without a letter
	}
	closed := false
	for range 1 + g.r.IntN(8) {
		if closed {
			// a letter must follow a Z
			fmt.Fprintf(&b, "%s%s %s", g.pick("M", "m", "L", "l"), n(), n())
			closed = false
		}
		switch g.r.IntN(14) {
		case 0:
			fmt.Fprintf(&b, "%s%s%s%s", g.pick("L", "l"), n(), sep(), n())
		case 1:
			fmt.Fprintf(&b, "%s%s", g.pick("H", "h", "V", "v"), n())
		case 2:
			fmt.Fprintf(&b, "%s%s %s %s %s %s %s", g.pick("C", "c"), n(), n(), n(), n(), n(), n())
		case 3:
			fmt.Fprintf(&b, "%s%s %s %s %s", g.pick("S", "s"), n(), n(), n(), n())
		case 4:
			fmt.Fprintf(&b, "%s%s %s %s %s", g.pick("Q", "q"), n(), n(), n(), n())
		case 5:
			fmt.Fprintf(&b, "%s%s %s", g.pick("T", "t"), n(), n())
		case 6, 7:
			// flags may touch what follows
			fmt.Fprintf(&b, "%s%s %s %s %s%s%s%s %s", g.pick("A", "a"), g.num(0, 60), g.num(0, 60), g.num(-90, 90),
				g.pick("0", "1"), g.pick("", " ", ","), g.pick("0", "1"), g.pick("", " "), n()+" "+n())
		case 8:
			b.WriteString(g.pick("Z", "z"))
			closed = true
		case 9:
			// more arguments without a letter
			fmt.Fprintf(&b, "L%s %s %s %s", n(), n(), n(), n())
		case 10:
			// an error: the path ends here
			b.WriteString(g.pick(" X 1 2", " L 5", " A 1 1 0 2 0 5 5", " C 1 2 3", " 1e", " L 1 . 2"))
			if g.r.IntN(2) == 0 {
				return b.String()
			}
			// the letter of what follows, so that nothing follows a Z bare
			fmt.Fprintf(&b, " M%s %s", n(), n())
		default:
			fmt.Fprintf(&b, " l %s %s", n(), n())
		}
		b.WriteString(sep())
	}
	if g.r.IntN(3) == 0 {
		b.WriteString("Z")
	}
	return b.String()
}

// The families and strings of the text: those of the documents' text.
func (g *svgGen) text() string {
	s := textStrings[g.r.IntN(len(textStrings))]
	r := strings.NewReplacer("&", "&amp;", "<", "&lt;", ">", "&gt;")
	if g.r.IntN(6) == 0 {
		s = "  " + s + " \n\t " + s + " "
	}
	return r.Replace(s)
}

// attr writes an attribute with its value escaped.
func (g *svgGen) attr(name, value string) {
	r := strings.NewReplacer("&", "&amp;", "<", "&lt;", "\"", "&quot;")
	fmt.Fprintf(&g.b, " %s=\"%s\"", name, r.Replace(value))
}

// presentation writes some presentation attributes, as attributes or in a
// style attribute.
func (g *svgGen) presentation() {
	type prop struct{ name, value string }
	var props []prop
	add := func(name, value string) { props = append(props, prop{name, value}) }
	for range g.r.IntN(5) {
		switch g.r.IntN(22) {
		case 0, 1, 2:
			add("fill", g.paint())
		case 3, 4:
			add("stroke", g.paint())
		case 5:
			add("stroke-width", g.length(0, 8))
		case 6:
			add("opacity", g.pick(g.num(0, 1), "50%", "0", "1", "2", "-1", "x", ".5"))
		case 7:
			add("fill-opacity", g.pick(g.num(0, 1), "30%", "nothing"))
		case 8:
			add("stroke-opacity", g.num(0, 1))
		case 9:
			add("fill-rule", g.pick("evenodd", "nonzero", "inherit", "other"))
		case 10:
			add("stroke-linecap", g.pick("round", "square", "butt", "x"))
			add("stroke-linejoin", g.pick("round", "bevel", "miter", "arcs"))
		case 11:
			add("stroke-dasharray", g.pick("4 2", "5,3,1", "none", "1", "2%", "0 0", ", ,", "3 x", "6  ,  2"))
			add("stroke-dashoffset", g.length(0, 6))
		case 12:
			add("stroke-miterlimit", g.pick("1", "4", "10", "0.5", "x"))
		case 13:
			add("color", g.color())
		case 14:
			add("display", g.pick("none", "inline", "block", "", "inline", "inherit"))
		case 15:
			add("visibility", g.pick("hidden", "visible", "collapse", "inherit", "visible", "visible"))
		case 16:
			add("clip-path", g.pick(g.urlRef(), "none", g.urlRef()))
		case 17:
			add("font-size", g.pick(g.length(4, 30), "small", "x-large", "150%", "2em", "-3", "medium"))
			add("font-family", textFamilies[g.r.IntN(len(textFamilies))])
		case 18:
			add("font-weight", g.pick("bold", "normal", "bolder", "lighter", "700", "150", "+600", "-5", "99999999999999999999", "x"))
			add("font-style", g.pick("italic", "oblique 10deg", "normal"))
		case 19:
			add("text-anchor", g.pick("start", "middle", "end", "other"))
			add("letter-spacing", g.pick("0", "1.5", "-0.5", "0.1em", "normal"))
		case 20:
			add("mask", g.pick(g.urlRef(), "none"))
			add("clip-rule", g.pick("evenodd", "nonzero"))
		default:
			add("transform", g.transform())
		}
	}
	if len(props) == 0 {
		return
	}
	inStyle := g.r.IntN(4) == 0
	var style []string
	for _, p := range props {
		// transform is an attribute only
		if inStyle && p.name != "transform" && g.r.IntN(4) > 0 {
			style = append(style, p.name+g.pick(":", ": ", " : ")+p.value)
			continue
		}
		g.attr(p.name, p.value)
	}
	if len(style) > 0 {
		g.attr("style", strings.Join(style, g.pick(";", "; ", " ;"))+g.pick("", ";", ";;", "; bad"))
	}
	if g.r.IntN(6) == 0 {
		g.attr("class", g.pick("a", "b", "a b", "c"))
	}
}

// id gives the element being written an id, now and then.
func (g *svgGen) id(prefix string) {
	if g.r.IntN(3) > 0 {
		id := fmt.Sprintf("%s%d", prefix, len(g.ids))
		g.ids = append(g.ids, id)
		g.attr("id", id)
	}
}

func (g *svgGen) shape() {
	switch g.r.IntN(9) {
	case 0, 1:
		g.b.WriteString("<rect")
		g.attr("x", g.length(-10, 100))
		g.attr("y", g.length(-10, 100))
		g.attr("width", g.length(0, 90))
		g.attr("height", g.length(0, 90))
		if g.r.IntN(3) == 0 {
			g.attr("rx", g.length(0, 30))
		}
		if g.r.IntN(4) == 0 {
			g.attr("ry", g.length(0, 30))
		}
	case 2:
		g.b.WriteString("<circle")
		g.attr("cx", g.length(0, 100))
		g.attr("cy", g.length(0, 100))
		g.attr("r", g.length(0, 50))
	case 3:
		g.b.WriteString("<ellipse")
		g.attr("cx", g.length(0, 100))
		g.attr("cy", g.length(0, 100))
		if g.r.IntN(5) > 0 {
			g.attr("rx", g.pick(g.length(0, 50), g.length(0, 50), "auto"))
		}
		if g.r.IntN(5) > 0 {
			g.attr("ry", g.length(0, 50))
		}
	case 4:
		g.b.WriteString("<line")
		g.attr("x1", g.length(0, 100))
		g.attr("y1", g.length(0, 100))
		g.attr("x2", g.length(0, 100))
		g.attr("y2", g.length(0, 100))
		g.attr("stroke", g.color())
	case 5:
		g.b.WriteString(g.pick("<polyline", "<polygon"))
		var pts []string
		for range g.r.IntN(8) {
			pts = append(pts, g.num(0, 100)+g.pick(",", " ", " , ")+g.num(0, 100))
		}
		if g.r.IntN(8) == 0 {
			pts = append(pts, g.pick("7", "x", "1 2 3"))
		}
		g.attr("points", strings.Join(pts, " "))
	default:
		g.b.WriteString("<path")
		g.attr("d", g.pathData())
	}
	g.id("s")
	if g.r.IntN(3) > 0 {
		g.attr("fill", g.paint())
	}
	if g.r.IntN(3) == 0 {
		g.attr("stroke", g.paint())
		g.attr("stroke-width", g.num(0, 6))
	}
	g.presentation()
	g.b.WriteString("/>")
}

// tspans writes the content of a text element.
func (g *svgGen) tspans(depth int) {
	for range 1 + g.r.IntN(3) {
		switch g.r.IntN(5) {
		case 0, 1:
			g.b.WriteString(g.text())
		case 2:
			if depth > 2 {
				continue
			}
			g.b.WriteString(g.pick("<tspan", "<tspan", "<a", "<textPath"))
			if g.r.IntN(2) == 0 {
				g.attr("x", g.pick(g.num(0, 100), g.num(0, 100)+" "+g.num(0, 100), ""))
			}
			if g.r.IntN(3) == 0 {
				g.attr("dy", g.num(-10, 20))
			}
			if g.r.IntN(4) == 0 {
				g.attr("y", g.num(0, 100))
				g.attr("dx", g.num(-5, 5))
			}
			g.presentation()
			g.b.WriteString(">")
			g.tspans(depth + 1)
			g.b.WriteString(g.pick("</tspan>", "</tspan>", "</a>", "</textPath>"))
		case 3:
			g.b.WriteString("<title>not drawn</title>")
		default:
			g.b.WriteString(" ")
		}
	}
}

// image writes an image element: a bitmap or an SVG document in a data:
// URL written in one of the ways there are, or what is no image.
func (g *svgGen) image() {
	g.b.WriteString("<image")
	g.attr("x", g.length(0, 80))
	g.attr("y", g.length(0, 80))
	if g.r.IntN(4) > 0 {
		g.attr("width", g.length(0, 80))
	}
	if g.r.IntN(4) > 0 {
		g.attr("height", g.length(0, 80))
	}
	if g.r.IntN(3) == 0 {
		g.attr("preserveAspectRatio", g.aspect())
	}
	var href string
	switch g.r.IntN(12) {
	case 0:
		href = g.pick("other.png", "http://example.com/a.png", "", "data:image/png;base64", "data:,", "data:text/plain,hello")
	case 1, 2, 3:
		// an SVG document inside, escaped or in base64
		in := &svgGen{r: g.r, inside: g.inside + 1}
		svg := in.document()
		if g.r.IntN(2) == 0 {
			href = "data:image/svg+xml," + url.PathEscape(svg)
		} else {
			href = "data:image/svg+xml;base64," + base64.StdEncoding.EncodeToString([]byte(svg))
		}
	default:
		data := RandomImage(g.r)
		enc := base64.StdEncoding.EncodeToString(data)
		switch g.r.IntN(8) {
		case 0:
			enc = strings.TrimRight(enc, "=") // without its padding
		case 1:
			// broken into lines
			var b strings.Builder
			for i := 0; i < len(enc); i += 40 {
				b.WriteString(enc[i:min(i+40, len(enc))])
				b.WriteString(g.pick("\n", " ", "\r\n", "\t"))
			}
			enc = b.String()
		case 2:
			enc = enc[:len(enc)/2] + "!" + enc[len(enc)/2:] // not base64
		case 3:
			enc += "=" // one sign too many
		}
		href = "data:" + g.pick("image/png", "", "image/jpeg", "application/octet-stream") + ";base64," + enc
		if g.r.IntN(10) == 0 {
			// the bytes escaped instead
			href = "data:image/png," + url.PathEscape(string(data))
		}
	}
	g.attr(g.pick("href", "href", "xlink:href"), g.pick("", " ")+href)
	g.presentation()
	g.b.WriteString("/>")
}

func (g *svgGen) aspect() string {
	return g.pick("none", "xMidYMid", "xMinYMin meet", "xMaxYMax slice", "xMidYMax", "defer xMinYMid slice", "xMaxYMin  meet", "", "nothing", "slice")
}

// viewport writes the attributes of an svg or symbol element.
func (g *svgGen) viewport(sized bool) {
	if sized || g.r.IntN(2) == 0 {
		g.attr("width", g.pick(g.length(10, 200), g.length(10, 200), "100%"))
	}
	if sized || g.r.IntN(2) == 0 {
		g.attr("height", g.length(10, 200))
	}
	if g.r.IntN(3) > 0 {
		g.attr("viewBox", g.pick(
			fmt.Sprintf("%s %s %s %s", g.num(-10, 10), g.num(-10, 10), g.num(1, 200), g.num(1, 200)),
			fmt.Sprintf("0,0,%s,%s", g.num(1, 200), g.num(1, 200)), "0 0 100 100", "0 0 0 10", "0 0 100", "0 0 -5 5", "x"))
	}
	if g.r.IntN(3) == 0 {
		g.attr("preserveAspectRatio", g.aspect())
	}
	if g.r.IntN(5) == 0 {
		g.attr("overflow", g.pick("visible", "hidden", "auto", "scroll"))
	}
}

func (g *svgGen) gradient() {
	linear := g.r.IntN(2) == 0
	name := "radialGradient"
	if linear {
		name = "linearGradient"
	}
	g.b.WriteString("<" + name)
	g.id("g")
	coord := func() string {
		return g.pick(g.num(0, 100), g.num(0, 100)+"%", g.num(0, 1), "50%", "", "x")
	}
	if linear {
		for _, a := range []string{"x1", "y1", "x2", "y2"} {
			if g.r.IntN(3) > 0 {
				g.attr(a, coord())
			}
		}
	} else {
		for _, a := range []string{"cx", "cy", "r", "fx", "fy", "fr"} {
			if g.r.IntN(3) == 0 {
				g.attr(a, coord())
			}
		}
	}
	if g.r.IntN(3) == 0 {
		g.attr("gradientUnits", g.pick("userSpaceOnUse", "objectBoundingBox", "other"))
	}
	if g.r.IntN(4) == 0 {
		g.attr("gradientTransform", g.transform())
	}
	if g.r.IntN(4) == 0 {
		g.attr(g.pick("href", "xlink:href"), g.ref())
	}
	g.b.WriteString(">")
	for range g.r.IntN(5) {
		g.b.WriteString("<stop")
		g.attr("offset", g.pick(g.num(0, 1), g.num(0, 100)+"%", "0", "1", "2", "-1", "x", ""))
		if g.r.IntN(5) > 0 {
			g.attr("stop-color", g.color())
		}
		if g.r.IntN(3) == 0 {
			g.attr("stop-opacity", g.pick(g.num(0, 1), "50%", "x"))
		}
		if g.r.IntN(6) == 0 {
			g.attr("style", "stop-color:"+g.color()+";color:"+g.color())
		}
		g.b.WriteString("/>")
	}
	if g.r.IntN(8) == 0 {
		g.b.WriteString("<rect width=\"5\" height=\"5\"/>")
	}
	g.b.WriteString("</" + name + ">")
}

func (g *svgGen) clipPath() {
	g.b.WriteString("<clipPath")
	g.id("c")
	if g.r.IntN(3) == 0 {
		g.attr("clipPathUnits", g.pick("objectBoundingBox", "userSpaceOnUse"))
	}
	if g.r.IntN(4) == 0 {
		g.attr("transform", g.transform())
	}
	g.b.WriteString(">")
	for range 1 + g.r.IntN(3) {
		switch g.r.IntN(6) {
		case 0:
			g.b.WriteString("<use")
			g.attr("href", g.ref())
			g.attr("x", g.num(0, 20))
			g.b.WriteString("/>")
		case 1:
			g.b.WriteString("text in a clip path")
		default:
			g.shape()
		}
	}
	g.b.WriteString("</clipPath>")
}

// element writes an element that draws, or holds ones that do.
func (g *svgGen) element() {
	g.depth++
	defer func() { g.depth-- }()
	deep := g.depth > 4
	switch k := g.r.IntN(30); {
	case k < 12 || deep && k < 24:
		g.shape()
	case k < 15:
		g.b.WriteString(g.pick("<g", "<g", "<a", "<switch"))
		g.id("n")
		g.presentation()
		if g.r.IntN(8) == 0 {
			g.attr(g.pick("systemLanguage", "requiredExtensions"), g.pick("en", ""))
		}
		g.b.WriteString(">")
		for range g.r.IntN(4) {
			g.element()
		}
		g.b.WriteString(g.pick("</g>", "</g>", "</a>", "</switch>"))
	case k < 17:
		g.b.WriteString("<use")
		g.attr(g.pick("href", "xlink:href"), g.ref())
		if g.r.IntN(2) == 0 {
			g.attr("x", g.length(-20, 60))
			g.attr("y", g.length(-20, 60))
		}
		if g.r.IntN(3) == 0 {
			g.attr("width", g.length(5, 80))
			g.attr("height", g.length(5, 80))
		}
		g.id("u")
		g.presentation()
		g.b.WriteString("/>")
	case k < 19:
		g.b.WriteString("<svg")
		g.attr("x", g.length(0, 60))
		g.attr("y", g.length(0, 60))
		g.viewport(false)
		g.id("v")
		g.presentation()
		g.b.WriteString(">")
		for range g.r.IntN(4) {
			g.element()
		}
		g.b.WriteString("</svg>")
	case k < 22:
		g.b.WriteString("<text")
		g.attr("x", g.pick(g.num(0, 60), g.num(0, 60), g.num(0, 50)+","+g.num(0, 50), ""))
		g.attr("y", g.num(10, 100))
		if g.r.IntN(3) > 0 {
			g.attr("font-size", g.num(6, 28))
			g.attr("font-family", textFamilies[g.r.IntN(len(textFamilies))])
		}
		if g.r.IntN(5) == 0 {
			g.attr("dx", g.num(-5, 5))
			g.attr("dy", g.num(-5, 5))
		}
		g.id("t")
		g.presentation()
		g.b.WriteString(">")
		g.tspans(0)
		g.b.WriteString("</text>")
	case k < 24:
		if g.inside < 5 {
			g.image()
		}
	case k < 26:
		g.b.WriteString("<defs>")
		for range 1 + g.r.IntN(3) {
			g.definition()
		}
		g.b.WriteString("</defs>")
	case k < 27:
		g.definition()
	case k < 28:
		g.b.WriteString(g.pick("<title>a title</title>", "<desc>described</desc>", "<metadata><x/></metadata>", "<!-- a comment -->", "<script>1 &lt; 2</script>",
			"<foreignObject width=\"10\" height=\"10\"><p xmlns=\"http://www.w3.org/1999/xhtml\">html</p></foreignObject>", "<unknown x=\"1\"><rect width=\"9\" height=\"9\"/></unknown>", "text outside text"))
	default:
		g.style()
	}
}

// definition writes what is drawn where it is referred to.
func (g *svgGen) definition() {
	switch g.r.IntN(8) {
	case 0, 1, 2:
		g.gradient()
	case 3, 4:
		g.clipPath()
	case 5:
		g.b.WriteString("<symbol")
		g.id("y")
		g.viewport(false)
		g.presentation()
		g.b.WriteString(">")
		for range 1 + g.r.IntN(3) {
			g.element()
		}
		g.b.WriteString("</symbol>")
	case 6:
		// what is not drawn, and warned of when it is used
		g.b.WriteString(g.pick("<pattern", "<mask", "<marker", "<filter"))
		g.id("p")
		g.b.WriteString(g.pick(" width=\"10\" height=\"10\"><rect width=\"5\" height=\"5\"/></pattern>", "><circle r=\"5\"/></mask>", "/>"))
		// (the end tag may not be that of the start: the reader takes it)
	default:
		g.shape()
	}
}

// style writes a style sheet.
func (g *svgGen) style() {
	g.b.WriteString("<style>")
	if g.r.IntN(4) == 0 {
		g.b.WriteString("<![CDATA[")
		defer g.b.WriteString("]]>")
	}
	for range 1 + g.r.IntN(4) {
		sel := g.pick("rect", "circle", "path", "g", "text", "*", ".a", ".b", "g rect", "g > path", "rect.a", "#s1", "#n2 circle", "svg", "tspan", "rect, circle", "a:hover", "@media print")
		if g.r.IntN(4) == 0 && len(g.ids) > 0 {
			sel = "#" + g.ids[g.r.IntN(len(g.ids))]
		}
		fmt.Fprintf(&g.b, "%s { %s: %s%s }\n", sel, g.pick("fill", "stroke", "opacity", "stroke-width", "font-size", "display", "fill-opacity", "color"),
			g.pick(g.color(), g.num(0, 1), g.length(0, 20), "none"), g.pick("", ";", " !important", "; stroke: "+g.color()))
	}
	if g.r.IntN(8) == 0 {
		g.b.WriteString("/* a comment */ } rect { fill: red ")
	}
	g.b.WriteString("</style>")
}

// document writes a whole document.
func (g *svgGen) document() string {
	if g.r.IntN(6) == 0 {
		g.b.WriteString(g.pick("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "\ufeff", "<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n", "\n  "))
	}
	g.b.WriteString("<svg")
	if g.r.IntN(8) > 0 {
		g.attr("xmlns", "http://www.w3.org/2000/svg")
	}
	if g.r.IntN(3) == 0 {
		g.attr("xmlns:xlink", "http://www.w3.org/1999/xlink")
	}
	if g.r.IntN(4) > 0 {
		// a viewport of a hundred units or so, where the content is
		g.attr("width", g.num(60, 200))
		g.attr("height", g.num(60, 200))
		if g.r.IntN(2) == 0 {
			g.attr("viewBox", fmt.Sprintf("%s %s %s %s", g.num(-5, 10), g.num(-5, 10), g.num(80, 140), g.num(80, 140)))
		}
		if g.r.IntN(4) == 0 {
			g.attr("preserveAspectRatio", g.aspect())
		}
	} else {
		g.viewport(g.r.IntN(3) > 0)
		g.presentation()
	}
	g.b.WriteString(">")
	// some ids referred to before they are there
	for range g.r.IntN(3) {
		g.ids = append(g.ids, fmt.Sprintf("%s%d", g.pick("s", "g", "c", "n", "y"), len(g.ids)+1+g.r.IntN(6)))
	}
	n := 3 + g.r.IntN(10)
	if g.inside > 0 {
		n = 1 + g.r.IntN(4)
	}
	for range n {
		g.element()
	}
	g.b.WriteString("</svg>")
	if g.r.IntN(12) == 0 {
		g.b.WriteString("<rect id=\"after\" width=\"3\" height=\"3\"/>")
	}
	return g.b.String()
}

// RandomSVG is an SVG document for the rasterizer to draw (see svgGen), now
// and then cut short, or with a byte of it changed where that cannot make
// path data of what was none.
func RandomSVG(r *rand.Rand) []byte {
	g := &svgGen{r: r}
	s := g.document()
	if r.IntN(20) == 0 {
		// cut short at a tag, where no path data is
		if i := strings.LastIndex(s[:len(s)*(1+r.IntN(9))/10+1], "<"); i > 4 {
			s = s[:i]
		}
	}
	return []byte(s)
}

// RandomSVGDocument is RandomDocument with SVG images (and a bitmap or two)
// in its drawings: drawn whole and in parts, at every scale, and as
// patterns.
func RandomSVGDocument(r *rand.Rand) *bdf.Document {
	doc := bdf.NewDocument()
	var images []bdf.Hash
	for range 1 + r.IntN(3) {
		images = append(images, doc.AddImage(RandomSVG(r)))
	}
	if r.IntN(3) == 0 {
		images = append(images, doc.AddImage(RandomImage(r)))
	}
	return randomDocument(r, doc, images, nil)
}
