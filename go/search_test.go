package cbdf

import (
	"bufio"
	"bytes"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math/rand/v2"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"unicode"
	"unicode/utf8"

	"github.com/shibukawa/bdf"
	"golang.org/x/text/unicode/norm"
)

// The limit and the context tools/search-oracle.mjs asks with.
const (
	oracleLimit   = 4
	oracleContext = 12
)

// dumpSearch writes the hits of the queries as tools/search-oracle.mjs does.
func dumpSearch(plain string, queries []string, hits [][]Hit, caseSensitive bool) ([]string, error) {
	sum := sha256.Sum256([]byte(plain))
	astral := 0
	for _, r := range plain {
		if r > 0xffff {
			astral = 1
			break
		}
	}
	lines := []string{fmt.Sprintf("T %s %d", hex.EncodeToString(sum[:8]), astral)}
	for qi, q := range queries {
		norm, err := NormalizeQuery(q, caseSensitive)
		if err != nil {
			return nil, err
		}
		lines = append(lines, fmt.Sprintf("Q %s %s %d", jsString(q), jsString(norm), len(hits[qi])))
		for _, h := range hits[qi] {
			context := "-"
			if astral == 0 {
				context = jsString(h.Context)
			}
			lines = append(lines, fmt.Sprintf("H %s %s", jsString(h.Text), context))
			for _, s := range h.Segments {
				lines = append(lines, fmt.Sprintf("S %d %d %d %d %d %d", s.Run, s.A, s.B, s.Ordinal, s.Start, s.End))
			}
		}
	}
	return lines, nil
}

// oracleQueries reads the queries back out of the oracle's lines.
func oracleQueries(t testing.TB, lines []string) []string {
	var queries []string
	for _, l := range lines {
		if !strings.HasPrefix(l, "Q ") {
			continue
		}
		dec := json.NewDecoder(strings.NewReader(l[2:]))
		var q string
		if err := dec.Decode(&q); err != nil {
			t.Fatalf("%q: %v", l, err)
		}
		queries = append(queries, q)
	}
	return queries
}

// TestSearchMatchesTypeScript searches the text index parts of the test
// documents and compares the hits with those of the reference's TypeScript
// reader (testdata/search.txt.gz, written by tools/search-oracle.mjs).
func TestSearchMatchesTypeScript(t *testing.T) {
	f, err := os.Open(filepath.Join("testdata", "search.txt.gz"))
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
	indexes, queries, found, missing := 0, 0, 0, 0
	for _, b := range blocks {
		name, hash, caseSensitive := b.head[0], b.head[2], b.head[3] == "1"
		where := strings.Join(b.head[:2], " ")
		data, err := os.ReadFile(filepath.Join(dir, filepath.FromSlash(name)))
		if err != nil {
			missing++
			continue
		}
		d, err := OpenMemory(data)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if d.Locked() {
			if err := d.Unlock("demo-パスワード"); err != nil {
				t.Fatalf("%s: %v", name, err)
			}
		}
		var h [16]byte
		hex.Decode(h[:], []byte(hash))
		part, err := d.Part(h)
		d.Close()
		if err != nil {
			missing++
			continue
		}
		qs := oracleQueries(t, b.lines)
		plain, hits, err := SearchIndexPart(part, caseSensitive, qs, oracleLimit, oracleContext)
		if err != nil {
			t.Errorf("%s: %v", where, err)
			continue
		}
		got, err := dumpSearch(plain, qs, hits, caseSensitive)
		if err != nil {
			t.Fatal(err)
		}
		if d := firstDiff(strings.Join(b.lines, "\n"), strings.Join(got, "\n")); d != "" {
			t.Errorf("%s: %s", where, d)
			continue
		}
		indexes++
		queries += len(qs)
		for _, hs := range hits {
			found += len(hs)
		}
	}
	if indexes < 150 {
		t.Errorf("only %d indexes compared (%d not in this testdata)", indexes, missing)
	}
	t.Logf("%d indexes, %d queries, %d hits match; %d not in this testdata", indexes, queries, found, missing)
}

// ---- Random indexes ---------------------------------------------------------

// Characters a search folds, some it does not, and those that change with
// the ones before them: marks, and the letters of Hangul syllables.
var searchAlphabet = []string{
	"\u0301", "\u0301", "\u0327", "\u0323", "\u0308", "\u0344", "\u0345", "\u3099", "\u309a", "\uff9f", "\u1161", "\u11a8",
	"\u1100", "\uac00", "\u0e33", "\u0f73", "\u0958", "\u09cb", "\u09c7", "\u09be", "\u1b35", "\u1b05", "\U0001d157", "\U0001d165",
	"\U00011099", "\U000110ba", "\u2126\u0301", "o\u0302\u0303", "\u1e69", "s\u0323\u0307", "s\u0307\u0323", "\u01d5",
	"\U00016d63", "\U00016d67", "\U00016d68", "\U00016d69", "\u0b47", "\u0b3e", "\u0cc6", "\u0cd5", "\u0dd9", "\u0dcf", "\u1025", "\u102e",
	"\uac00", "\u11a8", "\u1161", "\u1100", "\u65e5", "\u672c", "\u3042", "\u30a2", "\uf900", "\U0002f800", "\u0e01", "\u0e33",
	"a", "b", "c", "A", "B", "Z", "\u0020", "\u0020", "\u0009", "\u00a0", "\u3000", "1",
	"\uff11", "\uff21", "\uff41", "\uff76", "\uff9e", "\u30ab", "\u30ac", "\u304b", "\u304c", "\u30fc", "\u00e9", "e\u0301",
	"\u00c9", "\u00df", "\u1e9e", "\u0130", "I", "\u0131", "\u03a3", "\u03c3", "\u03c2", "\ufb01", "\ufb03", "\u3314",
	"\u337f", "\u2460", "\u2168", "\u2178", "\u00bd", "\u00b2", "\u2212", "-", "\u2010", "\U0001f600", "\U0001d400", "\U0001d49c",
	"\ud55c", "\u1112", "\u314e", "\uffa1", "\ufdfa", "\u2026", "\u000a", "\u2028", "\u01c4", "\u01c5", "\u01c6", "\u2126",
	"\u03a9", "\u212a", "\u212b", "\u6f22", "\u0000", "\u309e", "\u30fe", "\u30f6", "\u30f7", "\uff70", "\u1680", "\ufeff",
	"\ua66e", "\ua66c", "\u03a3\u03c3", "a\u03a3",
}

// Marks that compose with a letter, some of them only when another is not in
// between.
var searchMarks = []string{"\u0301", "\u0323", "\u0307", "\u0308", "\u0327", "\u0302", "\u3099", "\u0345"}

func randomText(r *rand.Rand, most int) string {
	var b strings.Builder
	for range r.IntN(most + 1) {
		b.WriteString(pick(r, searchAlphabet))
	}
	// more marks in a row than a piece of a run takes
	if r.IntN(40) == 0 {
		for range 20 + r.IntN(60) {
			b.WriteString(pick(r, searchMarks))
		}
		b.WriteString(pick(r, searchAlphabet))
	}
	return b.String()
}

// TestSearchRandom searches random text with libbdf and with the reference's
// TypeScript reader, which it runs with node: set LIBBDF_BDF_CHECKOUT to a
// checkout of bdf with packages/core built. LIBBDF_SEARCH_CASES and
// LIBBDF_SEARCH_SEED change how many indexes and which.
func TestSearchRandom(t *testing.T) {
	if os.Getenv("LIBBDF_BDF_CHECKOUT") == "" {
		t.Skip("LIBBDF_BDF_CHECKOUT is not set")
	}
	cases, seed := 10000, uint64(7)
	if n, err := strconv.Atoi(os.Getenv("LIBBDF_SEARCH_CASES")); err == nil && n > 0 {
		cases = n
	}
	if n, err := strconv.ParseUint(os.Getenv("LIBBDF_SEARCH_SEED"), 10, 64); err == nil {
		seed = n
	}
	r := rand.New(rand.NewPCG(20261013, seed))
	inputs := make([]searchInput, cases)
	for i := range inputs {
		var runs []bdf.IndexRun
		var all []string
		for k := range 1 + r.IntN(12) {
			run := bdf.IndexRun{A: uint32(r.IntN(3)), B: uint32(r.IntN(2)), Ordinal: uint32(k), Sep: byte(r.IntN(3)), Text: randomText(r, 12)}
			runs = append(runs, run)
			all = append(all, run.Text)
		}
		joined := []rune(strings.Join(all, ""))
		queries := []string{randomText(r, 3), randomText(r, 2), " ", ""}
		for range 4 {
			if len(joined) == 0 {
				break
			}
			from := r.IntN(len(joined))
			to := min(len(joined), from+1+r.IntN(5))
			queries = append(queries, string(joined[from:to]))
		}
		inputs[i] = searchInput{Index: bdf.EncodeTextIndex(runs), Queries: queries, CaseSensitive: r.IntN(3) == 0}
	}
	t.Logf("%d indexes, %d hits match", cases, searchWithNode(t, inputs))
}

// TestSearchEveryCharacter searches for every character where it may or may
// not be one with what is before it: after a letter, after a Hangul
// syllable that takes a last letter, and after itself. The hits say where
// the pieces of a run are. It runs the reference's TypeScript reader as
// TestSearchRandom does.
func TestSearchEveryCharacter(t *testing.T) {
	if os.Getenv("LIBBDF_BDF_CHECKOUT") == "" {
		t.Skip("LIBBDF_BDF_CHECKOUT is not set")
	}
	var inputs []searchInput
	var runs []bdf.IndexRun
	var queries []string
	flush := func() {
		if len(runs) > 0 {
			inputs = append(inputs, searchInput{Index: bdf.EncodeTextIndex(runs), Queries: queries, CaseSensitive: len(inputs)%2 == 1})
		}
		runs, queries = nil, nil
	}
	for c := rune(1); c <= unicode.MaxRune; c++ {
		if c >= 0xD800 && c <= 0xDFFF {
			continue
		}
		s := string(c)
		for _, text := range []string{"a" + s + "b", "\uac00" + s, s + s} {
			runs = append(runs, bdf.IndexRun{Ordinal: uint32(len(runs)), Sep: 2, Text: text})
		}
		queries = append(queries, s)
		if len(queries) == 8 {
			flush()
		}
	}
	flush()
	t.Logf("%d indexes, %d hits match", len(inputs), searchWithNode(t, inputs))
}

// An index and what is asked of it, as tools/search-oracle.mjs reads them.
type searchInput struct {
	Index         []byte   `json:"index"`
	Queries       []string `json:"queries"`
	CaseSensitive bool     `json:"caseSensitive"`
}

// searchWithNode searches the indexes with libbdf and with the reference's
// TypeScript reader, and returns how many hits the two agree on.
func searchWithNode(t *testing.T, inputs []searchInput) int {
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node is not installed")
	}
	cases := len(inputs)
	var in bytes.Buffer
	enc := json.NewEncoder(&in)
	enc.SetEscapeHTML(false)
	for i := range inputs {
		enc.Encode(inputs[i])
	}
	file := filepath.Join(t.TempDir(), "indexes.jsonl")
	if err := os.WriteFile(file, in.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	script, _ := filepath.Abs(filepath.Join("..", "tools", "search-oracle.mjs"))
	out, err := exec.Command(node, script, os.Getenv("LIBBDF_BDF_CHECKOUT"), "--indexes", file).Output()
	if err != nil {
		t.Fatalf("node: %v", err)
	}
	blocks := strings.Split(strings.TrimPrefix(string(out), "== "), "\n== ")
	if len(blocks) != cases {
		t.Fatalf("%d answers for %d indexes", len(blocks), cases)
	}
	found := 0
	for i, b := range blocks {
		head, body, _ := strings.Cut(strings.TrimSuffix(b, "\n"), "\n")
		c := inputs[i]
		for _, q := range c.Queries {
			if !utf8.ValidString(q) {
				t.Fatalf("index %d: a query that is not UTF-8", i)
			}
		}
		plain, hits, err := SearchIndexPart(c.Index, c.CaseSensitive, c.Queries, oracleLimit, oracleContext)
		if err != nil {
			t.Fatalf("index %d: %v (the TypeScript reader: %s)", i, err, head)
		}
		got, err := dumpSearch(plain, c.Queries, hits, c.CaseSensitive)
		if err != nil {
			t.Fatal(err)
		}
		if d := firstDiff(body, strings.Join(got, "\n")); d != "" {
			t.Fatalf("index %d (case-sensitive %v): %s\n%x", i, c.CaseSensitive, d, c.Index)
		}
		for _, hs := range hits {
			found += len(hs)
		}
	}
	return found
}

// ---- Normalisation -----------------------------------------------------------

// foldSensitive is fold of the reference's search.ts for a case-sensitive
// search: the minus sign as a hyphen, katakana as hiragana.
func foldSensitive(s string) string {
	return strings.Map(func(c rune) rune {
		switch {
		case c == 0x2212:
			return '-'
		case c >= 0x30a1 && c <= 0x30f6:
			return c - 0x60
		}
		return c
	}, s)
}

// TestSearchNormalizeMatchesXText compares the form KC a search makes of
// text with that of x/text: of every character, and of random strings of
// those that compose and reorder. (Lower case is the reference's TypeScript
// reader's: TestSearchNormalizeAll asks that.)
func TestSearchNormalizeMatchesXText(t *testing.T) {
	check := func(s string) bool {
		want := norm.NFKC.String(s)
		if goInsertedJoiner(s, want) {
			return false
		}
		got, err := NormalizeQuery(s, true)
		if err != nil {
			t.Fatal(err)
		}
		if want = foldSensitive(want); got != want {
			t.Fatalf("%+q is %+q, want %+q", s, got, want)
		}
		return true
	}
	for c := rune(0); c <= unicode.MaxRune; c++ {
		if c < 0xD800 || c > 0xDFFF {
			check(string(c))
		}
	}
	r := rng()
	n, skipped := 0, 0
	for range 200000 {
		var sb strings.Builder
		for range 1 + r.IntN(12) {
			switch r.IntN(8) {
			case 0:
				sb.WriteRune(rune(r.IntN(0x3400)))
			case 1:
				sb.WriteString(pick(r, searchAlphabet))
			default:
				sb.WriteRune(normAlphabet[r.IntN(len(normAlphabet))])
			}
		}
		if check(sb.String()) {
			n++
		} else {
			skipped++
		}
	}
	t.Logf("every character and %d strings compared, %d skipped where x/text inserts a joiner", n, skipped)
}

// TestSearchNormalizeAll compares what a search makes of text with what the
// reference's TypeScript reader makes of it (normalizeQuery, with and
// without case): every character, alone and next to those it may compose
// with. It runs that reader with node: set LIBBDF_BDF_CHECKOUT as for
// TestSearchRandom.
func TestSearchNormalizeAll(t *testing.T) {
	checkout := os.Getenv("LIBBDF_BDF_CHECKOUT")
	if checkout == "" {
		t.Skip("LIBBDF_BDF_CHECKOUT is not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node is not installed")
	}
	var texts []string
	for c := rune(0); c <= unicode.MaxRune; c++ {
		if c >= 0xD800 && c <= 0xDFFF {
			continue
		}
		s := string(c)
		texts = append(texts, s)
		d, kd := norm.NFD.String(s), norm.NFKD.String(s)
		// a character that normalisation changes, a mark, or one of a script
		// whose letters compose
		if d == s && kd == s && !unicode.Is(unicode.M, c) && c >= 0x3100 && (c < 0xA000 || c > 0xABFF) && (c < 0x11000 || c > 0x11FFF) {
			continue
		}
		texts = append(texts, "a"+s, "e"+s+"\u0301", s+"\u0301", s+"\u0323\u0301", s+"\u0301\u0323", s+"\u3099", s+"\u309a",
			"\u1100"+s, "\u1100\u1161"+s, s+"\u1161", s+"\u11a8", s+s, s+"\u0345", "\u03b1"+s+"\u0345", s+"\u0327\u0301", "A"+s+"a")
		if d != s {
			texts = append(texts, d, d+"\u0301", "x"+d+"\u0323")
		}
		if kd != d {
			texts = append(texts, kd, kd+"\u3099")
		}
	}
	var in bytes.Buffer
	enc := json.NewEncoder(&in)
	enc.SetEscapeHTML(false)
	for _, s := range texts {
		enc.Encode(s)
	}
	file := filepath.Join(t.TempDir(), "texts.jsonl")
	if err := os.WriteFile(file, in.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	script, _ := filepath.Abs(filepath.Join("..", "tools", "search-oracle.mjs"))
	out, err := exec.Command(node, script, checkout, "--normalize", file).Output()
	if err != nil {
		t.Fatalf("node: %v", err)
	}
	// (the answers are JSON, which has no line break in it)
	lines := strings.Split(strings.TrimSuffix(string(out), "\n"), "\n")
	if len(lines) != len(texts) {
		t.Fatalf("%d answers for %d texts", len(lines), len(texts))
	}
	for i, s := range texts {
		folded, err := NormalizeQuery(s, false)
		if err != nil {
			t.Fatal(err)
		}
		sensitive, err := NormalizeQuery(s, true)
		if err != nil {
			t.Fatal(err)
		}
		if got := jsString(folded) + " " + jsString(sensitive); got != lines[i] {
			t.Fatalf("%+q is %s, the TypeScript reader has %s", s, got, lines[i])
		}
	}
	t.Logf("%d texts match", len(texts))
}
