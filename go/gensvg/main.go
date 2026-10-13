// Command gensvg writes the tables of the C sources from the Go packages the
// reference reads with, between the marks "BEGIN GENERATED: name" and "END
// GENERATED: name" of the files given (which have clang-format off around
// them):
//
//	go run -C go ./gensvg ../src/svg_float.c ../src/svg_str.c ../src/svg_xml.c ../src/svg_value.c
//
// The tests of go/svgdoc compare the tables with their sources again
// (TestRunes, TestLabels, TestValues, TestParseFloat).
package main

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"unicode"

	"golang.org/x/text/encoding/charmap"
	"golang.org/x/text/encoding/htmlindex"
)

func moduleDir(module string) string {
	out, err := exec.Command("go", "list", "-m", "-f", "{{.Dir}}", module).Output()
	if err != nil {
		panic(fmt.Sprintf("locating %s: %v", module, err))
	}
	return strings.TrimSpace(string(out))
}

func read(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		panic(err)
	}
	return string(b)
}

// pow10 is pow10Tab of Go's strconv for the powers a float64 is parsed
// with: p of -345 to 310 (atof64 answers the others before it scales).
func pow10() string {
	src := read(filepath.Join(runtime.GOROOT(), "src/internal/strconv/pow10tab.go"))
	if !strings.Contains(src, "pow10Min = -348") || !strings.Contains(src, "pow10Max = 347") {
		panic("pow10tab.go has other bounds")
	}
	rows := regexp.MustCompile(`\{(0x[0-9a-f]{16}), (0x[0-9a-f]{16})\}, // (1e-?[0-9]+)`).FindAllStringSubmatch(src, -1)
	if len(rows) != 347+348+1 {
		panic(fmt.Sprintf("%d powers of ten", len(rows)))
	}
	var b strings.Builder
	for p := -345; p <= 310; p++ {
		r := rows[p+348]
		if r[3] != fmt.Sprintf("1e%d", p) {
			panic("pow10tab.go is not in order: " + r[3])
		}
		fmt.Fprintf(&b, "    {%sULL, %sULL}, /* %s */\n", r[1], r[2], r[3])
	}
	return b.String()
}

// lower is the ranges of unicode.CaseRanges that have a lower case, past
// ASCII: {first, last, what to add}, or BDFI_SVG_LOWER_PAIRS for a range of
// upper and lower case letters in turn.
func lower() string {
	var b strings.Builder
	n := 0
	for _, cr := range unicode.CaseRanges {
		d := cr.Delta[unicode.LowerCase]
		if d == 0 || cr.Hi < 0x80 {
			continue
		}
		if cr.Lo < 0x80 {
			panic("a case range across the end of ASCII")
		}
		if d == unicode.UpperLower {
			fmt.Fprintf(&b, "    {0x%04X, 0x%04X, BDFI_SVG_LOWER_PAIRS},\n", cr.Lo, cr.Hi)
		} else {
			fmt.Fprintf(&b, "    {0x%04X, 0x%04X, %d},\n", cr.Lo, cr.Hi, d)
		}
		n++
	}
	return fmt.Sprintf("    /* Unicode %s: %d ranges */\n", unicode.Version, n) + b.String()
}

func cString(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		if c := s[i]; c < 0x20 || c >= 0x7f || c == '"' || c == '\\' {
			fmt.Fprintf(&b, "\\x%02x", c)
			// a hex digit after it would be taken into the escape
			if i+1 < len(s) && strings.IndexByte("0123456789abcdefABCDEF", s[i+1]) >= 0 {
				b.WriteString("\" \"")
			}
		} else {
			b.WriteByte(c)
		}
	}
	b.WriteByte('"')
	return b.String()
}

// entities is encoding/xml.HTMLEntity, which htmlentity.Lookup of
// tinygodriver is generated from, by name.
func entities() string {
	names := make([]string, 0, len(xml.HTMLEntity))
	for n := range xml.HTMLEntity {
		names = append(names, n)
	}
	sort.Strings(names)
	var b strings.Builder
	for _, n := range names {
		if len(n) > 8 || len(xml.HTMLEntity[n]) > 3 {
			panic("an entity longer than the table holds: " + n)
		}
		fmt.Fprintf(&b, "    {%s, %s},\n", cString(n), cString(xml.HTMLEntity[n]))
	}
	return b.String()
}

// asciiKind says what the decoder of an encoding makes of ASCII: itself
// ("ASCII"), itself but for the escape character ("ASCII_NO_ESC"), or
// something else ("OTHER"). It asks the decoder about every byte alone and
// about every two of them, in both orders.
func asciiKind(label string) string {
	e, err := htmlindex.Get(label)
	if err != nil {
		panic(err)
	}
	same := func(in []byte) bool {
		out, err := e.NewDecoder().Bytes(in)
		return err == nil && bytes.Equal(in, out)
	}
	all, noEsc := true, true
	for a := 0; a < 128; a++ {
		for b := 0; b < 128; b++ {
			if !same([]byte{byte(a)}) || !same([]byte{byte(a), byte(b), 'x', byte(b), byte(a)}) {
				all = false
				if a != 0x1b && b != 0x1b {
					noEsc = false
				}
			}
		}
	}
	switch {
	case all:
		return "BDFI_XML_ENC_ASCII"
	case noEsc:
		return "BDFI_XML_ENC_ASCII_NO_ESC"
	}
	return "BDFI_XML_ENC_OTHER"
}

// labels is nameMap of golang.org/x/text/encoding/htmlindex: the labels
// charset.NewReaderLabel knows, with the encodings this library reads, and
// for the others what their decoders make of ASCII.
func labels() string {
	src := read(filepath.Join(moduleDir("golang.org/x/text"), "encoding/htmlindex/tables.go"))
	i := strings.Index(src, "var nameMap = map[string]htmlEncoding{")
	if i < 0 {
		panic("no nameMap")
	}
	src = src[i:]
	src = src[:strings.Index(src, "\n}")]
	rows := regexp.MustCompile(`"([^"]+)":\s+([A-Za-z0-9_]+),`).FindAllStringSubmatch(src, -1)
	sort.Slice(rows, func(i, j int) bool { return rows[i][1] < rows[j][1] })
	kinds := map[string]string{"utf8": "BDFI_XML_ENC_UTF8", "utf16be": "BDFI_XML_ENC_UTF16BE", "utf16le": "BDFI_XML_ENC_UTF16LE", "windows1252": "BDFI_XML_ENC_WINDOWS1252"}
	var b strings.Builder
	for _, r := range rows {
		if len(r[1]) > 19 {
			panic("a label longer than the table holds: " + r[1])
		}
		if kinds[r[2]] == "" {
			kinds[r[2]] = asciiKind(r[1])
		}
		fmt.Fprintf(&b, "    {%s, %s},\n", cString(r[1]), kinds[r[2]])
	}
	return fmt.Sprintf("    /* %d labels */\n", len(rows)) + b.String()
}

// cp1252 is what charmap.Windows1252 decodes the bytes 0x80 to 0x9F to; the
// bytes from 0xA0 are the characters of the same number.
func cp1252() string {
	dec := charmap.Windows1252.NewDecoder()
	var b strings.Builder
	for c := 0x80; c < 0x100; c++ {
		out, err := dec.Bytes([]byte{byte(c)})
		if err != nil {
			panic(err)
		}
		r := []rune(string(out))
		if len(r) != 1 {
			panic("a byte of windows-1252 that is not one character")
		}
		if c >= 0xA0 {
			if r[0] != rune(c) {
				panic("windows-1252 is not Latin-1 from 0xA0")
			}
			continue
		}
		if c%8 == 0 {
			b.WriteString("   ")
		}
		fmt.Fprintf(&b, " 0x%04X,", r[0])
		if c%8 == 7 {
			b.WriteString("\n")
		}
	}
	return b.String()
}

// colors is namedColors of image/svg, by name.
func colors() string {
	src := read(filepath.Join(moduleDir("github.com/shibukawa/bdf"), "image/svg/colornames.go"))
	rows := regexp.MustCompile(`"([a-z]+)":\s+0x([0-9a-f]{6}),`).FindAllStringSubmatch(src, -1)
	if len(rows) != 148 {
		panic(fmt.Sprintf("%d colour names", len(rows)))
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i][1] < rows[j][1] })
	var b strings.Builder
	for _, r := range rows {
		if len(r[1]) > 20 {
			panic("a colour name longer than the table holds: " + r[1])
		}
		fmt.Fprintf(&b, "    {%s, 0x%s},\n", cString(r[1]), r[2])
	}
	return b.String()
}

func main() {
	tables := map[string]func() string{"pow10": pow10, "lower": lower, "entities": entities, "labels": labels, "cp1252": cp1252, "colors": colors}
	for _, path := range os.Args[1:] {
		src, err := os.ReadFile(path)
		if err != nil {
			panic(err)
		}
		out := src
		for name, table := range tables {
			begin, end := []byte("/* BEGIN GENERATED: "+name+" */\n"), []byte("/* END GENERATED: "+name+" */")
			i := bytes.Index(out, begin)
			if i < 0 {
				continue
			}
			j := bytes.Index(out[i:], end)
			if j < 0 {
				panic(path + ": no end of " + name)
			}
			// the end mark keeps its line, whatever it is indented by
			j = bytes.LastIndexByte(out[:i+j], '\n') + 1
			out = append(append(append([]byte(nil), out[:i+len(begin)]...), table()...), out[j:]...)
			fmt.Fprintf(os.Stderr, "%s: %s\n", path, name)
		}
		if !bytes.Equal(out, src) {
			if err := os.WriteFile(path, out, 0o644); err != nil {
				panic(err)
			}
		}
	}
}
