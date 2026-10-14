package svgdoc

// What the tests share: the comparison of the two readers on a document,
// the files they are compared on, and the ways documents are damaged.

import (
	"bytes"
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"

	"github.com/shibukawa/bdf"
	"github.com/shibukawa/bdf/image/svg"
	"golang.org/x/net/html/charset"
)

// smallLimits are limits a small document reaches (those of the reference's
// own fuzz test).
var smallLimits = &svg.Options{MaxDepth: 40, MaxNodes: 2000, MaxStyleMatches: 20000}

// known counts the documents the C reader refuses on purpose where the
// reference reads them.
var known struct{ encoding int }

var declaredLabel = regexp.MustCompile(`^\s*=\s*(?:"([^"]*)"|'([^']*)')`)

// declaresOtherEncoding reports whether a document has, somewhere, what may
// be the declaration of an encoding the reference reads and the C reader
// does not: a label of one of those (or a label with a reference in it,
// which is not decoded here), and after it a byte that is not ASCII (the C
// reader reads what is ASCII in most of them: it is the same bytes).
func declaresOtherEncoding(data []byte) bool {
	for at := 0; ; {
		i := bytes.Index(data[at:], []byte("encoding"))
		if i < 0 {
			return false
		}
		at += i + len("encoding")
		m := declaredLabel.FindSubmatch(data[at:])
		if m == nil {
			continue
		}
		label, rest := string(m[1])+string(m[2]), data[at+len(m[0]):]
		if strings.Contains(label, "&") {
			return true
		}
		_, name := charset.Lookup(label)
		switch name {
		case "", "utf-8", "utf-16be", "utf-16le", "windows-1252":
			continue
		case "replacement":
			return true
		}
		for _, c := range rest {
			if c >= 0x80 || c == 0x1b && name == "iso-2022-jp" {
				return true
			}
		}
	}
}

// compare reads a document with both readers and reports how what they read
// differs, or "" when it is the same: the same status, the same tree with
// the same attributes, style declarations and text, the same elements by
// id, the same warnings, and the same values out of every attribute.
func compare(data []byte, opts *svg.Options) string {
	g := goDump(data, opts, true, exactFloats)
	c := cDump(data, opts, true, exactFloats)
	if g == c {
		return ""
	}
	if c == "encoding\n" && declaresOtherEncoding(data) {
		known.encoding++
		return ""
	}
	return difference(g, c)
}

// difference is the first lines two dumps differ in.
func difference(g, c string) string {
	gl, cl := strings.Split(g, "\n"), strings.Split(c, "\n")
	i := 0
	for i < len(gl) && i < len(cl) && gl[i] == cl[i] {
		i++
	}
	clip := func(l []string) string {
		s := strings.Join(l[max(0, i-2):min(len(l), i+3)], "\n")
		if len(s) > 1200 {
			s = s[:1200] + "…"
		}
		return s
	}
	return fmt.Sprintf("line %d of %d (reference) and %d (C):\n--- reference\n%s\n--- C\n%s", i+1, len(gl), len(cl), clip(gl), clip(cl))
}

// files returns the regular files under the given directories whose names
// end in ext, by path. A directory that does not exist has none.
func files(dirs []string, ext string) map[string][]byte {
	out := map[string][]byte{}
	for _, d := range dirs {
		filepath.Walk(d, func(p string, info os.FileInfo, err error) error {
			if err != nil || info.IsDir() || !strings.HasSuffix(strings.ToLower(p), ext) {
				return nil
			}
			// a link to a file is the file (what a package manager installs often is one)
			if st, err := os.Stat(p); err != nil || !st.Mode().IsRegular() {
				return nil
			}
			if b, err := os.ReadFile(p); err == nil {
				out[p] = b
			}
			return nil
		})
	}
	return out
}

// moduleDir is the directory of a module of the build in the module cache.
func moduleDir(t testing.TB, module string) string {
	t.Helper()
	out, err := exec.Command("go", "list", "-m", "-f", "{{.Dir}}", module).Output()
	if err != nil {
		t.Fatalf("locating %s: %v", module, err)
	}
	return strings.TrimSpace(string(out))
}

// svgFiles is the SVG files the readers are compared on, read where they
// are: the image parts of the reference's test documents, the testdata of
// the reference's packages, and the files under the directories that
// LIBBDF_SVG_FILES lists (the SVG files of a system, say).
func svgFiles(t testing.TB) map[string][]byte {
	dirs := []string{moduleDir(t, "github.com/shibukawa/bdf")}
	// more of them, from the directories LIBBDF_SVG_FILES lists
	dirs = append(dirs, filepath.SplitList(os.Getenv("LIBBDF_SVG_FILES"))...)
	out := files(dirs, ".svg")
	for name, data := range documentSVGs(t) {
		out[name] = data
	}
	return out
}

// tokens is what documents are made of: a damaged copy may get one.
var tokens = []string{
	"<", ">", "</", "/>", "<!--", "-->", "<![CDATA[", "]]>", "<?", "?>", "<!DOCTYPE svg [", "]>", "<!ENTITY a \"b\">", "&a;", "&amp;", "&lt;",
	"&#65;", "&#x41;", "&nbsp;", "&", ";", "\"", "'", "=", " ", "\r\n", "\t", ":", "xlink:href=\"#a\"", "href=\"#b\"", "xmlns=\"x\"",
	"<br>", "<img>", "</br>", "<svg>", "</svg>", "<style>", "</style>", "<text>", "</text>", "<tspan>", "<a>", "<g>", "</g>",
	"{", "}", "/*", "*/", ".a", "#b", ",", "> ", "*", "[x]", ":hover", "@media", "!important", "fill:red;", "style=\"", "class=\"a b\"", "id=\"b\"",
	"<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?>", "<?xml version=\"1.0\" encoding=\"utf-16le\"?>", "<?xml encoding='utf8'?>", "encoding=\"x\"",
	"\xef\xbb\xbf", "\xff\xfe", "\xfe\xff", "\x00", "\xc3\xa9", "\xe9", "\xc2\xa0", "\xe2\x80\x80", "\xc4\xb0", "\xe2\x84\xaa", "\xff", "\xed\xa0\x80", "\xc3",
	"rotate(30 1 2)", "matrix(1 2 3 4 5 6)", "hsl(120,50%,50%)", "rgb(1,2,3)", "#abc", "1e5", "-.5", "em", "%",
}

// mutate damages a document the ways files get damaged: flipped bits, bytes
// overwritten, a cut, a stretch removed or repeated, a piece of markup put
// in.
func mutate(rng *rand.Rand, src []byte) []byte {
	b := append([]byte(nil), src...)
	if len(b) == 0 {
		return append(b, tokens[rng.Intn(len(tokens))]...)
	}
	switch rng.Intn(9) {
	case 0:
		for i, n := 0, 1+rng.Intn(3); i < n; i++ {
			b[rng.Intn(len(b))] ^= 1 << rng.Intn(8)
		}
	case 1:
		for i, n := 0, 1+rng.Intn(4); i < n; i++ {
			b[rng.Intn(len(b))] = byte(rng.Intn(256))
		}
	case 2:
		b = b[:rng.Intn(len(b))]
	case 3:
		i := rng.Intn(len(b))
		j := i + rng.Intn(min(len(b)-i, 16)+1)
		b = append(b[:i], b[j:]...)
	case 4:
		i := rng.Intn(len(b))
		j := i + rng.Intn(min(len(b)-i, 16)+1)
		b = append(b[:j], append(append([]byte(nil), b[i:j]...), b[j:]...)...)
	case 5:
		// the start is where a reader decides the most
		b[rng.Intn(min(len(b), 64))] = byte(rng.Intn(256))
	case 6:
		// the extremes of a byte
		v := []byte{0, 1, 0x7f, 0x80, 0xff}[rng.Intn(5)]
		i := rng.Intn(len(b))
		for j := i; j < len(b) && j < i+1+rng.Intn(4); j++ {
			b[j] = v
		}
	case 7, 8:
		// a piece of markup, somewhere
		i := rng.Intn(len(b) + 1)
		tok := tokens[rng.Intn(len(tokens))]
		b = append(b[:i], append([]byte(tok), b[i:]...)...)
	}
	return b
}

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

func sortedNames(in map[string][]byte) []string {
	names := make([]string, 0, len(in))
	for name := range in {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

// checkDocs compares the readers on every document given, with the default
// limits and with small ones.
func checkDocs(t *testing.T, in map[string][]byte) {
	t.Helper()
	docs := 0
	for _, name := range sortedNames(in) {
		data := in[name]
		for _, opts := range []*svg.Options{nil, smallLimits} {
			if d := compare(data, opts); d != "" {
				t.Errorf("%s (limits %v): %s", name, opts != nil, d)
			}
		}
		if strings.HasPrefix(goDump(data, nil, false, false), "doc\n") {
			docs++
		}
	}
	t.Logf("%d documents, %d of them SVG the reference reads", len(in), docs)
}

// checkMutations compares the readers on damaged copies of the documents
// given. LIBBDF_SVG_CASES sets how many (the default is small: a test run takes
// seconds), LIBBDF_SVG_SEED the seed, and LIBBDF_SVG_DUMP names a directory the
// documents they disagree on are written to.
func checkMutations(t *testing.T, in map[string][]byte) {
	t.Helper()
	names := sortedNames(in)
	if len(names) == 0 {
		t.Skip("no documents to damage")
	}
	cases, seed := envInt("LIBBDF_SVG_CASES", 3000), int64(envInt("LIBBDF_SVG_SEED", 1))
	rng := rand.New(rand.NewSource(seed))
	docs, failed := 0, 0
	before := known
	for i := 0; i < cases; i++ {
		name := names[rng.Intn(len(names))]
		data := mutate(rng, in[name])
		for rng.Intn(3) == 0 {
			data = mutate(rng, data)
		}
		opts := smallLimits
		if rng.Intn(2) == 0 {
			opts = nil
		}
		d := compare(data, opts)
		if d == "" {
			if strings.HasPrefix(goDump(data, opts, false, false), "doc\n") {
				docs++
			}
			continue
		}
		failed++
		if failed <= 10 {
			t.Errorf("case %d of seed %d (%s): %s", i, seed, filepath.Base(name), d)
		}
		if dir := os.Getenv("LIBBDF_SVG_DUMP"); dir != "" {
			os.MkdirAll(dir, 0o755)
			os.WriteFile(filepath.Join(dir, fmt.Sprintf("svg-%d-%d", seed, i)), data, 0o644)
		}
	}
	t.Logf("seed %d: %d damaged documents, %d of them still SVG the reference reads, %d disagreements; refused by the C reader alone for their encoding: %d",
		seed, cases, docs, failed, known.encoding-before.encoding)
}

// documentSVGs is the SVG image parts of the reference's test documents.
func documentSVGs(t testing.TB) map[string][]byte {
	t.Helper()
	out := map[string][]byte{}
	root := filepath.Join(moduleDir(t, "github.com/shibukawa/bdf"), "testdata")
	filepath.Walk(root, func(p string, info os.FileInfo, err error) error {
		if err != nil || !info.Mode().IsRegular() || !strings.HasSuffix(p, ".bdf") {
			return nil
		}
		data, err := os.ReadFile(p)
		if err != nil {
			return nil
		}
		rd, err := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		if err != nil {
			return nil
		}
		doc, err := rd.ToDocument()
		if err != nil {
			return nil
		}
		for _, part := range doc.Parts() {
			// SVG markup rather than an encoded bitmap, as the renderer tells
			b := bytes.TrimLeft(bytes.TrimPrefix(part.Data, []byte("\xef\xbb\xbf")), " \t\r\n")
			if part.Type == bdf.PartImage && len(b) > 0 && b[0] == '<' {
				out[p+"#"+part.Hash.String()] = part.Data
			}
		}
		return nil
	})
	if len(out) == 0 {
		t.Fatal("no SVG image in the reference's test documents")
	}
	return out
}
