package svgdoc

// Documents made at random from the things SVG documents are made of: what
// damaged copies of written documents seldom are, style sheets whose rules
// match the elements in many ways at once.

import (
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/shibukawa/bdf/image/svg"
)

var (
	genTags    = []string{"g", "g", "g", "rect", "circle", "path", "text", "tspan", "a", "textPath", "use", "svg", "defs", "symbol", "switch", "linearGradient", "stop", "clipPath", "image", "foreignObject", "p", "br", "img", "hr", "title", "x:g", "svg:rect", "G"}
	genIDs     = []string{"a", "b", "c", "d", "e", "A", "a b", "", "é", "x.y", "x#y"}
	genClasses = []string{"k", "l", "m", "n", "K", "kl", "é", "x.y", "k:l"}
	genProps   = []string{"fill", "stroke", "opacity", "display", "font-size", "stroke-width", "color", "x", "FILL", "Stroke", "É", "clip-path", "stop-color"}
	genValues  = []string{"red", "#abc", "none", "1", "2px", "url(#a)", "inherit", "", " padded ", "a:b", "rgb(1,2,3)", "hsl(120,50%,50%)", "1.5em", "currentColor", "\"q;r\"", "x !important", "!important"}
	genText    = []string{"text", " ", "\n  ", "a &amp; b", "&lt;", "&#65;", "&#x1F600;", "&nbsp;", "&e1;", "&e2;", "&undefined;", "&", "é", "\r\n", "]]>", "<![CDATA[ <c> & ]]>", "<!-- c -->", "<?pi x?>", "日本"}
)

func genSelector(rng *rand.Rand) string {
	var b strings.Builder
	for i, n := 0, 1+rng.Intn(3); i < n; i++ {
		if i > 0 {
			b.WriteString(pick(rng, []string{" ", " ", " > ", ">", "  ", "\t", " "}))
		}
		switch rng.Intn(6) {
		case 0:
			b.WriteString("*")
		case 1, 2:
			b.WriteString(strings.TrimPrefix(pick(rng, genTags), "x:"))
		}
		for rng.Intn(3) == 0 {
			b.WriteString("." + pick(rng, genClasses))
		}
		if rng.Intn(5) == 0 {
			b.WriteString("#" + pick(rng, genIDs))
		}
		if rng.Intn(30) == 0 {
			b.WriteString(pick(rng, []string{":hover", "[x]", "::before", ":not(.k)", "[a=b]"}))
		}
		if b.Len() == 0 {
			b.WriteString(pick(rng, []string{"g", ".k", "#a", "*"}))
		}
	}
	if rng.Intn(40) == 0 {
		return pick(rng, []string{"", " ", ".", "#", "> g", "g >", ">"})
	}
	return b.String()
}

func genDecls(rng *rand.Rand) string {
	var b strings.Builder
	for i, n := 0, rng.Intn(5); i < n; i++ {
		b.WriteString(pick(rng, []string{"", " ", "\n\t"}) + pick(rng, genProps) + pick(rng, []string{":", ": ", " : ", ""}) + pick(rng, genValues) + pick(rng, []string{";", ";", "; ", " !important;", ";;", ""}))
	}
	return b.String()
}

func genSheet(rng *rand.Rand) string {
	var b strings.Builder
	for i, n := 0, rng.Intn(8); i < n; i++ {
		switch rng.Intn(12) {
		case 0:
			b.WriteString(pick(rng, []string{"/* c */", "/* { } */", "/*", "*/", "@import 'x';", "@media print { g { fill: red } }", "@font-face { x: y }", "}", "{", "<!-- -->"}))
			continue
		}
		for j, m := 0, 1+rng.Intn(3); j < m; j++ {
			if j > 0 {
				b.WriteString(pick(rng, []string{",", ", ", " ,\n"}))
			}
			b.WriteString(genSelector(rng))
		}
		b.WriteString(pick(rng, []string{"{", " {", "{ "}) + genDecls(rng) + pick(rng, []string{"}", "}", "}\n", " } "}))
	}
	return b.String()
}

func genAttrs(rng *rand.Rand) string {
	var b strings.Builder
	if rng.Intn(3) == 0 {
		fmt.Fprintf(&b, ` id="%s"`, pick(rng, genIDs))
	}
	if rng.Intn(2) == 0 {
		b.WriteString(` class="`)
		for i, n := 0, rng.Intn(4); i < n; i++ {
			b.WriteString(pick(rng, genClasses) + pick(rng, []string{" ", " ", "  ", "\t", " ", ""}))
		}
		b.WriteString(`"`)
	}
	if rng.Intn(4) == 0 {
		fmt.Fprintf(&b, ` style="%s"`, strings.ReplaceAll(genDecls(rng), `"`, "'"))
	}
	for rng.Intn(3) == 0 {
		name := pick(rng, []string{"fill", "stroke", "transform", "x", "width", "href", "xlink:href", "foo:href", "points", "offset", "xmlns", "xmlns:xlink", "opacity", "d", "viewBox", "font-size", "checked"})
		switch rng.Intn(8) {
		case 0:
			b.WriteString(" " + name)
		case 1:
			b.WriteString(" " + name + "=" + pick(rng, []string{"a", "1", "a-b", "x:y", ""}))
		case 2:
			fmt.Fprintf(&b, " %s='%s'", name, strings.ReplaceAll(valueInput(rng), "'", ""))
		default:
			v := valueInput(rng)
			v = strings.NewReplacer(`"`, "&quot;", "<", "&lt;").Replace(v)
			fmt.Fprintf(&b, ` %s="%s"`, name, v)
		}
	}
	return b.String()
}

func genElement(rng *rand.Rand, b *strings.Builder, depth int) {
	tag := pick(rng, genTags)
	if rng.Intn(15) == 0 {
		tag = "style"
	}
	b.WriteString("<" + tag + genAttrs(rng))
	if rng.Intn(5) == 0 {
		b.WriteString("/>")
		return
	}
	b.WriteString(">")
	if tag == "style" {
		sheet := genSheet(rng)
		if rng.Intn(3) == 0 {
			sheet = "<![CDATA[" + strings.ReplaceAll(sheet, "]]>", "") + "]]>"
		} else {
			sheet = strings.NewReplacer("<", "&lt;", "&", "&amp;").Replace(sheet)
			if rng.Intn(3) == 0 {
				sheet = strings.ReplaceAll(sheet, ">", "&gt;")
			}
		}
		b.WriteString(sheet)
	} else {
		for i, n := 0, rng.Intn(4); i < n && depth < 8; i++ {
			if rng.Intn(3) == 0 {
				b.WriteString(pick(rng, genText))
			} else {
				genElement(rng, b, depth+1)
			}
		}
	}
	switch rng.Intn(25) {
	case 0: // left open
	case 1:
		b.WriteString("</" + pick(rng, genTags) + ">")
	case 2:
		b.WriteString("</" + strings.ToUpper(tag) + ">")
	default:
		b.WriteString("</" + tag + pick(rng, []string{"", "", "", " ", "\n"}) + ">")
	}
}

// genDocument is a document made at random, in one of the encodings the
// readers both read.
func genDocument(rng *rand.Rand) []byte {
	var b strings.Builder
	decl := rng.Intn(3) == 0
	if rng.Intn(4) == 0 {
		b.WriteString(`<!DOCTYPE svg [<!ENTITY e1 "one &amp; &#60;"><!ENTITY e2 '<g class="k"/>'><!ENTITY e1 "again">]>`)
	}
	if rng.Intn(10) == 0 {
		b.WriteString("<!-- before -->")
	}
	root := pick(rng, []string{"svg", "svg", "svg", "svg", "svg:svg", "g", "SVG"})
	b.WriteString("<" + root + ` xmlns="http://www.w3.org/2000/svg"` + genAttrs(rng) + ">")
	if rng.Intn(2) == 0 {
		// a style sheet of its own, with rules enough to meet on an element
		b.WriteString("<style><![CDATA[" + strings.ReplaceAll(genSheet(rng)+genSheet(rng), "]]>", "") + "]]></style>")
	}
	for i, n := 0, 1+rng.Intn(6); i < n; i++ {
		if rng.Intn(4) == 0 {
			b.WriteString(pick(rng, genText))
		} else {
			genElement(rng, &b, 2)
		}
	}
	if rng.Intn(10) != 0 {
		b.WriteString("</" + root + ">")
	}
	if rng.Intn(15) == 0 {
		genElement(rng, &b, 1)
	}
	doc := b.String()
	switch rng.Intn(10) {
	case 0:
		if decl {
			doc = `<?xml version="1.0" encoding="UTF-16"?>` + doc
		}
		return utf16Of(doc, rng.Intn(2) == 0, true)
	case 1:
		be := rng.Intn(2) == 0
		return append([]byte(`<?xml version="1.0" encoding="utf-16`+map[bool]string{false: "le", true: "be"}[be]+`"?>`), utf16Of(doc, be, false)...)
	case 2:
		// Latin-1 where the document allows, and bytes that are none where not
		out := []byte(`<?xml version="1.0" encoding="` + pick(rng, []string{"ISO-8859-1", "windows-1252", "latin1", "us-ascii"}) + `"?>`)
		for _, r := range doc {
			if r < 0x100 {
				out = append(out, byte(r))
			} else {
				out = append(out, 0x80+byte(r%32))
			}
		}
		return out
	case 3:
		return []byte("\xef\xbb\xbf" + doc)
	}
	if decl {
		doc = pick(rng, []string{`<?xml version="1.0" encoding="UTF-8"?>`, `<?xml version="1.0"?>`, `<?xml version="1.0" encoding="utf8"?>`, `<?xml version='1.0' encoding='utf-8' standalone='no'?>` + "\n"}) + doc
	}
	return []byte(doc)
}

// TestGenerated compares the readers on LIBBDF_SVG_GENERATED documents made at
// random (the default is small).
func TestGenerated(t *testing.T) {
	total, seed := envInt("LIBBDF_SVG_GENERATED", 4000), int64(envInt("LIBBDF_SVG_SEED", 1))
	rng := rand.New(rand.NewSource(seed))
	docs, sheets, failed := 0, 0, 0
	for i := 0; i < total; i++ {
		data := genDocument(rng)
		if rng.Intn(5) == 0 {
			data = mutate(rng, data)
		}
		opts := []*svg.Options{nil, nil, smallLimits, {MaxDepth: 5, MaxNodes: 30, MaxStyleMatches: 25}}[rng.Intn(4)]
		if d := compare(data, opts); d != "" {
			if failed++; failed <= 10 {
				t.Errorf("document %d of seed %d: %s\n%q", i, seed, d, data)
			}
			if dir := os.Getenv("LIBBDF_SVG_DUMP"); dir != "" {
				os.MkdirAll(dir, 0o755)
				os.WriteFile(filepath.Join(dir, fmt.Sprintf("gen-%d-%d", seed, i)), data, 0o644)
			}
			continue
		}
		if g := goDump(data, opts, false, false); strings.HasPrefix(g, "doc\n") {
			docs++
			if strings.Contains(g, "\nD\t") {
				sheets++
			}
		}
	}
	t.Logf("seed %d: %d documents made, %d of them SVG the reference reads, %d with style rules that match; %d disagreements", seed, total, docs, sheets, failed)
}
