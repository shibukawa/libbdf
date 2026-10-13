package svgdoc

// The value functions, the tables and the ports of Go's strings against the
// reference and the Go packages.

import (
	"bytes"
	"math"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
	"unicode"

	"github.com/shibukawa/bdf/image/svg"
	"golang.org/x/net/html/charset"
)

// spaces and letters that Go's strings package treats specially, and bytes
// that are no UTF-8.
var oddRunes = []string{" ", "\t", "\n", "\v", "\f", "\r", "\u0085", "\u00a0", "\u1680", "\u2000", "\u200a", "\u2028", "\u2029", "\u202f", "\u205f", "\u3000", "\ufeff", "\u200b",
	"\u0130", "\u212a", "\u212b", "É", "é", "Σ", "ς", "ǅ", "ß", "ẞ", "Ⱥ", "ⱥ", "İ", "ı", "Ａ", "𐐀", "𝔄", "日", "\ufffd", "\x00",
	"\xff", "\xfe", "\x80", "\xbf", "\xc0", "\xc2", "\xc3", "\xe0\x80", "\xe2\x80", "\xe2", "\xed\xa0\x80", "\xf0\x9f\x98", "\xf4\x90\x80\x80", "\xc2\x20", "\xe2\x80\x20"}

func pick(rng *rand.Rand, s []string) string { return s[rng.Intn(len(s))] }

func number(rng *rand.Rand) string {
	switch rng.Intn(12) {
	case 0:
		return floatInput(rng)
	case 1:
		return pick(rng, []string{"0", "1", "-1", ".5", "-.5", "1.", "+1", "1e3", "1E-3", "1e", "1e+", ".", "-", "+", "", "1.2.3", "1-2", "1e5e5", "0x10", "1e999", "-1e999", "1e-999", "inf", "nan", "1_0", "١", "１"})
	}
	return strconv.FormatFloat(float64(rng.Intn(20001)-10000)/float64([]int{1, 2, 4, 10, 100, 3}[rng.Intn(6)]), 'g', -1, 64)
}

func sep(rng *rand.Rand) string {
	return pick(rng, []string{" ", ",", ", ", " ,", "\t", "\n", "\r", "", " ", ";", "\u00a0", "/"})
}

// valueInput is a string an attribute might hold: a number, a list, a
// length, a transform, a colour, or something next to one.
func valueInput(rng *rand.Rand) string {
	var b strings.Builder
	switch rng.Intn(12) {
	case 0: // a list of numbers
		for i, n := 0, rng.Intn(8); i < n; i++ {
			b.WriteString(sep(rng) + number(rng))
		}
		b.WriteString(sep(rng))
	case 1, 2: // a length
		b.WriteString(pick(rng, []string{"", " ", ",", "\t\n"}) + number(rng) + pick(rng, []string{"", " ", "\u00a0", "\u3000"}) +
			pick(rng, []string{"", "px", "pt", "pc", "in", "cm", "mm", "q", "Q", "%", "em", "ex", "EM", "Px", "PT", "\u0130n", "\u0130N", "i\u0130", "p\u212a", "m", "e", "rem", "deg", "vh", "px;", "p x", "%%", "\xff", "px\xff", "in\u00a0", "\u00a0cm", "c\x00m"}) +
			pick(rng, []string{"", "", " ", "\u2003", "x", "\xc2"}))
	case 3, 4, 5: // a transform list
		for i, n := 0, 1+rng.Intn(4); i < n; i++ {
			b.WriteString(pick(rng, []string{"", " ", ",", "\n", "\u00a0", ", "}))
			b.WriteString(pick(rng, []string{"matrix", "translate", "scale", "rotate", "skewX", "skewY", "skewx", "Rotate", "translateX", "", "rotate ", " scale", "\u00a0scale\u2003", "ma\xfftrix", "none"}))
			b.WriteString(pick(rng, []string{"(", "(", "(", " (", "", "(("}))
			for j, m := 0, rng.Intn(8); j < m; j++ {
				b.WriteString(number(rng) + sep(rng))
			}
			b.WriteString(pick(rng, []string{")", ")", ")", "", "))", " ) "}))
		}
	case 6: // a colour by name or in hexadecimal
		const hex = "0123456789abcdefABCDEFg "
		switch rng.Intn(4) {
		case 0:
			b.WriteString("#")
			for i, n := 0, rng.Intn(10); i < n; i++ {
				b.WriteByte(hex[rng.Intn(len(hex))])
			}
		case 1:
			b.WriteString(pick(rng, colorNames))
		case 2:
			name := []byte(pick(rng, colorNames))
			for i := range name {
				if rng.Intn(3) == 0 {
					name[i] = byte(unicode.ToUpper(rune(name[i])))
				}
			}
			s := string(name)
			if rng.Intn(3) == 0 {
				s = strings.NewReplacer("i", "\u0130", "k", "\u212a", "I", "\u0130", "K", "\u212a").Replace(s)
			}
			b.WriteString(s)
		default:
			b.WriteString(pick(rng, []string{"currentColor", "CURRENTCOLOR", "currentcolor", "transparent", "Transparent", "none", "inherit", "url(#a)", "", "#", "red;", "re d", "redd", "transparen", "currentcolo", "\u212a", "lightgoldenrodyellow", "lightgoldenrodyellowx", "light\x00"}))
		}
	default: // rgb() and hsl()
		b.WriteString(pick(rng, []string{"rgb", "rgba", "hsl", "hsla", "RGB", "Hsl", "rgbx", "hslrgb", "rgb ", "hs"}))
		b.WriteString(pick(rng, []string{"(", "(", "(", " (", "", "(("}))
		for i, n := 0, rng.Intn(6); i < n; i++ {
			switch rng.Intn(8) {
			case 0:
				b.WriteString(pick(rng, []string{"inf", "-inf", "nan", "Infinity", "\u0130nf", "\u0130nf\u0130n\u0130ty", "+\u0130NF", "infinity\u0130", "0x1p-1", "0X10P0", "1e999", "-1e999", "1_0", "", "x", "\xff", "é", "1e", "none"}))
			case 1:
				b.WriteString(strconv.Itoa(rng.Intn(600) - 150))
			default:
				b.WriteString(number(rng))
			}
			b.WriteString(pick(rng, []string{"", "", "", "%", "%", "deg", "DEG", "Deg", "rad", "%%", "deg%", "%deg", "turn"}))
			b.WriteString(pick(rng, []string{",", ", ", " ", " / ", "/", "  ", ",,", "\t", ";"}))
		}
		b.WriteString(pick(rng, []string{")", ")", ")", "", "))", ") x", ")\u00a0"}))
	}
	s := b.String()
	// white space around it, a character put in, a capital letter
	if rng.Intn(4) == 0 {
		s = pick(rng, oddRunes) + s
	}
	if rng.Intn(4) == 0 {
		s += pick(rng, oddRunes)
	}
	if rng.Intn(6) == 0 && len(s) > 0 {
		i := rng.Intn(len(s) + 1)
		s = s[:i] + pick(rng, oddRunes) + s[i:]
	}
	if rng.Intn(8) == 0 {
		s = strings.ToUpper(s)
	}
	return s
}

var colorNames = func() []string {
	// the names of colornames.go, which the package does not export
	out, _ := filepath.Glob(filepath.Join(bdfModule(), "image/svg/colornames.go"))
	var names []string
	for _, f := range out {
		b, _ := os.ReadFile(f)
		for _, m := range regexp.MustCompile(`"([a-z]+)":\s+0x`).FindAllSubmatch(b, -1) {
			names = append(names, string(m[1]))
		}
	}
	return names
}()

func checkValue(t testing.TB, s string) bool {
	g, c := goValues(s, exactFloats), cValues(s, exactFloats)
	if g != c {
		t.Errorf("the values of %q:\n--- reference\n%s--- C\n%s", s, g, c)
		return false
	}
	return true
}

// TestValues compares the value functions on the values of the names the
// package knows and on LIBBDF_SVG_VALUES strings made for them.
func TestValues(t *testing.T) {
	if len(colorNames) != 148 {
		t.Fatalf("%d colour names", len(colorNames))
	}
	fixed := append([]string{}, colorNames...)
	for _, name := range colorNames {
		fixed = append(fixed, strings.ToUpper(name), " "+name+"\n", name+"x", name[:len(name)-1], "\u00a0"+name+"\u3000")
	}
	for _, u := range []string{"", "px", "pt", "pc", "in", "cm", "mm", "q", "%", "em", "ex"} {
		for _, n := range []string{"1", "-2.5", "1e2", ".5", "3.7795275590551181", "1e308", "1e999", "0"} {
			fixed = append(fixed, n+u, n+" "+u, n+strings.ToUpper(u), " "+n+u+" ", n+u+"x")
		}
	}
	fixed = append(fixed, "#abc", "#ABCD", "#a1b2c3", "#A1B2C3D4", "#ab", "#abcde", "#abcdefg", "#abcdefabc", "#ggg", "#", "# abc", "#abc ", " #abc", "#+ab", "#-12", "#0x1", "#1_2", "#00000000000000000000",
		"rgb(255,0,0)", "rgb(100%, 50%, 0%)", "rgba(1,2,3,.5)", "rgb(1 2 3 / 50%)", "rgb(300,-5,1e3)", "rgb(1,2)", "rgb()", "rgb", "rgb(", "rgb)", "rgb)(", ")rgb(1,2,3", "rgb(1,2,3", "rgb(1,2,3))", "rgb((1,2,3)",
		"hsl(120,100%,50%)", "hsl(120deg 50% 50% / 25%)", "hsl(-120, 50, 50)", "hsl(480,100%,25%)", "hsl(0,0%,50%)", "hsl(nan,50%,50%)", "hsl(inf,50%,50%)", "hsl(120,nan,50%)", "hsl(120,50%,nan)", "hsl(1e999,50%,50%)", "hsla(240,100%,50%,nan)", "hsl(360,100%,50%)", "hsl(359.99999999999994,100%,50%)", "hsl(60,100%,50%)", "hsl(180,100%,75%)", "hsl(300,25%,100%)",
		"translate(1,2)", "translate(1)", "translate()", "scale(2)", "scale(2,3)", "scale()", "rotate(90)", "rotate(30,1,2)", "rotate(1e10)", "rotate(inf)", "rotate(nan 1 2)", "skewX(45)", "skewY(90)", "skewX(1e300)",
		"matrix(1,2,3,4,5,6)", "matrix(1,2,3,4,5)", "matrix(1,2,3,4,5,6,7)", "matrix(1e308,1e308,1e308,1e308,1e308,1e308) scale(10)", "translate(1,2) rotate(30 4 5) skewX(6) matrix(1 0 0 1 7 8) scale(-1e3)",
		"translate(1,2)scale(3)", "translate(1,2),scale(3)", "translate (1,2)", " translate(1,2) ", "translate(1,2) x scale(3)", "translate(1,2) scale 3", "scale(1e999)", "scale(1e999) scale(0)", "rotate(45) rotate(-45)", "translate(1 2", "(1)", "()", ")(", "translate)1(", "TRANSLATE(1,2)")
	bad := 0
	for _, s := range fixed {
		if !checkValue(t, s) {
			bad++
		}
	}
	total := envInt("LIBBDF_SVG_VALUES", 200000)
	rng := rand.New(rand.NewSource(int64(envInt("LIBBDF_SVG_SEED", 1))))
	colours, lengths, lists := 0, 0, 0
	for i := 0; i < total && bad < 10; i++ {
		s := valueInput(rng)
		if !checkValue(t, s) {
			bad++
		}
		if _, ok := svg.ParseColor(s, svg.Color{}); ok {
			colours++
		}
		if _, ok := svg.ParseLength(s, 1, 1); ok {
			lengths++
		}
		if len(svg.ParseNumbers(s)) > 1 {
			lists++
		}
	}
	t.Logf("%d fixed and %d made values (%d colours, %d lengths, %d lists of numbers); the floats that the reference may fuse are compared to the bit: %v", len(fixed), total, colours, lengths, lists, exactFloats)
}

// TestMatrixMul compares Matrix.Mul to the bit where the reference does not
// fuse its products and sums.
func TestMatrixMul(t *testing.T) {
	rng := rand.New(rand.NewSource(7))
	arg := func() float64 {
		switch rng.Intn(12) {
		case 0:
			return 0
		case 1:
			return math.Inf(rng.Intn(2)*2 - 1)
		case 2:
			return math.NaN()
		case 3:
			return math.Float64frombits(rng.Uint64())
		case 4:
			return 1
		}
		return rng.NormFloat64() * math.Pow(10, float64(rng.Intn(9)-4))
	}
	n := 0
	for i := 0; i < 300000; i++ {
		var a, b svg.Matrix
		for k := range a {
			a[k], b[k] = arg(), arg()
		}
		want, got := a.Mul(b), cMatMul(a, b)
		for k := range want {
			if want[k] != want[k] && got[k] != got[k] {
				continue
			}
			same := math.Float64bits(want[k]) == math.Float64bits(got[k])
			if !exactFloats {
				// the reference fused: the same within what a rounding is
				scale := 0.0
				for j := range a {
					scale = math.Max(scale, math.Abs(a[j])*math.Abs(b[j%6]))
				}
				same = same || math.Abs(want[k]-got[k]) <= 1e-9*math.Max(math.Abs(want[k]), 1) || math.IsInf(want[k], 0) || math.IsInf(got[k], 0) || scale > 1e290
				if !same {
					continue // not to the bit: nothing to say here
				}
			}
			if !same {
				t.Fatalf("%v x %v: entry %d is %v [%016x], the reference has %v [%016x]", a, b, k, got[k], math.Float64bits(got[k]), want[k], math.Float64bits(want[k]))
			}
		}
		n++
	}
	if got := cMatMul(svg.Identity, svg.Matrix{1, 2, 3, 4, 5, 6}); got != (svg.Matrix{1, 2, 3, 4, 5, 6}) {
		t.Errorf("the identity times a matrix is %v", got)
	}
	t.Logf("%d products (to the bit: %v)", n, exactFloats)
}

// TestDoesNotFuse checks that the C sources are compiled without fused
// multiply-adds, on which the sameness of the floats on every platform
// rests.
func TestDoesNotFuse(t *testing.T) {
	// a*b is 1 - 2^-60 exactly, which rounds to 1; fused with -1 it stays
	a, b := 1+math.Pow(2, -30), 1-math.Pow(2, -30)
	for _, m := range [][2]svg.Matrix{
		{{a, 0, -1, 0, 0, 0}, {0, 0, 0, 0, b, 1}},
		{{-1, 0, a, 0, 0, 0}, {0, 0, 0, 0, 1, b}},
		{{a, 0, 0, 0, -1, 0}, {0, 0, 0, 0, b, 0}},
	} {
		if got := cMatMul(m[0], m[1]); got[4] != 0 {
			t.Fatalf("a*b + c was fused: %g", got[4])
		}
	}
}

// TestRunes compares the ports of unicode.ToLower and unicode.IsSpace on
// every character.
func TestRunes(t *testing.T) {
	lower, space := cRunes(0, unicode.MaxRune+0x1000)
	toASCII := 0
	for r := rune(0); r < rune(len(lower)); r++ {
		if want := unicode.ToLower(r); rune(lower[r]) != want {
			t.Fatalf("the lower case of %U is %U, Go has %U", r, lower[r], want)
		}
		if want := unicode.IsSpace(r); (space[r] != 0) != want {
			t.Fatalf("%U is white space: %v, Go has %v", r, space[r] != 0, want)
		}
		if r >= 0x80 && lower[r] < 0x80 {
			toASCII++
			if r != 0x130 && r != 0x212a {
				t.Errorf("%U has an ASCII lower case, which the colours and units are not read for", r)
			}
		}
	}
	if toASCII != 2 {
		t.Errorf("%d characters past ASCII have an ASCII lower case", toASCII)
	}
}

func goStrings(s string) string {
	d := &dumper{}
	d.str("trim\t")
	d.quoted(strings.TrimSpace(s))
	d.str("\nlower\t")
	d.quoted(strings.ToLower(s))
	d.str("\nfields")
	for _, f := range strings.Fields(s) {
		d.str("\t")
		d.quoted(f)
	}
	d.str("\n")
	return string(d.b)
}

func stringInput(rng *rand.Rand) string {
	var b strings.Builder
	for i, n := 0, rng.Intn(12); i < n; i++ {
		switch rng.Intn(6) {
		case 0, 1:
			b.WriteString(pick(rng, oddRunes))
		case 2:
			b.WriteRune(rune(rng.Intn(0x3000)))
		case 3:
			b.WriteByte(byte(rng.Intn(256)))
		case 4:
			b.WriteRune(rune(rng.Intn(unicode.MaxRune + 1)))
		default:
			b.WriteString(pick(rng, []string{"a", "B", "z", "Z", "0", " ", "  ", "ab", "CD"}))
		}
	}
	return b.String()
}

// TestStrings compares the ports of strings.TrimSpace, ToLower and Fields
// on strings of the characters they treat specially and of bytes that are
// no UTF-8.
func TestStrings(t *testing.T) {
	rng := rand.New(rand.NewSource(int64(envInt("LIBBDF_SVG_SEED", 1))))
	total := envInt("LIBBDF_SVG_STRINGS", 300000)
	for i := 0; i < total; i++ {
		s := stringInput(rng)
		if g, c := goStrings(s), cStrings(s); g != c {
			t.Fatalf("%q:\n--- Go\n%s--- C\n%s", s, g, c)
		}
	}
	// every character alone, between spaces and between letters
	for r := rune(0); r <= unicode.MaxRune; r++ {
		if r >= 0x3100 && r%97 != 0 && unicode.ToLower(r) == r {
			continue
		}
		for _, s := range []string{string(r), " " + string(r) + " ", "A" + string(r) + "B"} {
			if g, c := goStrings(s), cStrings(s); g != c {
				t.Fatalf("%q:\n--- Go\n%s--- C\n%s", s, g, c)
			}
		}
	}
}

// TestLabels compares what the readers make of every label of an encoding
// that golang.org/x/net/html/charset knows, as written and in other cases
// and with white space: read alike, or refused by the C reader for an
// encoding it does not read.
func TestLabels(t *testing.T) {
	src, err := os.ReadFile(filepath.Join(moduleDir(t, "golang.org/x/text"), "encoding/htmlindex/tables.go"))
	if err != nil {
		t.Fatal(err)
	}
	src = src[bytes.Index(src, []byte("var nameMap")):]
	src = src[:bytes.Index(src, []byte("\n}"))]
	var labels []string
	for _, m := range regexp.MustCompile(`"([^"]+)":`).FindAllSubmatch(src, -1) {
		labels = append(labels, string(m[1]))
	}
	if len(labels) != 228 {
		t.Fatalf("%d labels", len(labels))
	}
	body := "<svg a=\"\x80\x93\xe9\xff\"><text>caf\xe9 \xc3\xa9 \x00\xd8\x00\xdc</text></svg>"
	read, refused, unknown := 0, 0, 0
	for _, label := range labels {
		for _, l := range []string{label, strings.ToUpper(label), " " + label + "\t", "\u00a0" + label + "\u3000", label + "x", "x" + label, strings.ReplaceAll(label, "-", "_"), strings.ReplaceAll(label, "i", "\u0130"), strings.ReplaceAll(label, "k", "\u212a"), label[:len(label)-1]} {
			_, name := charset.Lookup(l)
			doc := []byte(`<?xml version="1.0" encoding="` + l + `"?>` + body)
			if strings.HasPrefix(name, "utf-16") {
				// a document that is in the encoding, with halves of pairs and a byte left over
				doc = append([]byte(`<?xml version="1.0" encoding="`+l+`"?>`), utf16Of("<svg a=\"é€𝄞\"><text>café</text>", name == "utf-16be", false)...)
				doc = append(doc, "\x00\xd8\x00\xdc\xd8\x00\xdc\x00"+string(utf16Of("</svg>", name == "utf-16be", false))+"x"...)
			}
			c := cDump(doc, nil, false, false)
			switch name {
			case "":
				unknown++
				if c != "notsvg\n" && !strings.EqualFold(l, "utf-8") {
					t.Errorf("the label %q is none, and the C reader says %.20q", l, c)
				}
			case "utf-8", "utf-16be", "utf-16le", "windows-1252":
				read++
				if !strings.HasPrefix(c, "doc\n") {
					t.Errorf("the label %q is %s, and the C reader says %.20q", l, name, c)
				}
			default:
				// refused for what is not ASCII in it; what is ASCII is the
				// same bytes, and read alike (below)
				refused++
				if c != "encoding\n" {
					t.Errorf("the label %q is %s, and the C reader says %.20q", l, name, c)
				}
				doc = []byte(`<?xml version="1.0" encoding="` + l + `"?><svg a="&#233;"><text>cafe \x00\x7f` + "\t" + `</text></svg>`)
				if c := cDump(doc, nil, false, false); !strings.HasPrefix(c, "doc\n") && name != "replacement" {
					t.Errorf("the label %q is %s, and the C reader says %.20q of a document in ASCII", l, name, c)
				}
				esc := []byte(`<?xml version="1.0" encoding="` + l + `"?><svg a="` + "\x1b$B" + `"/>`)
				if c := cDump(esc, nil, false, false); (c == "encoding\n") != (name == "replacement" || name == "iso-2022-jp") {
					t.Errorf("the label %q is %s, and the C reader says %.20q of a document with an escape", l, name, c)
				}
				if d := compare(esc, nil); d != "" {
					t.Errorf("the label %q, with an escape: %s", l, d)
				}
			}
			if d := compare(doc, nil); d != "" {
				t.Errorf("the label %q: %s", l, d)
			}
		}
	}
	t.Logf("%d labels and their variants: %d read, %d of encodings that are not read, %d no labels", len(labels), read, refused, unknown)
}

// bdfModule is the directory of the bdf module the tests are pinned to.
func bdfModule() string {
	out, err := exec.Command("go", "list", "-m", "-f", "{{.Dir}}", "github.com/shibukawa/bdf").Output()
	if err != nil {
		panic(err)
	}
	return strings.TrimSpace(string(out))
}
