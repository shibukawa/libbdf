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
	"unicode/utf8"

	"github.com/shibukawa/bdf"
)

// The limit and the context tools/search-oracle.mjs asks with.
const (
	oracleLimit   = 4
	oracleContext = 12
)

// dumpSearch writes the hits of the queries as tools/search-oracle.mjs does.
// The oracle counts the offsets in a run from after a byte order mark at its
// start, which its decoder drops; libbdf counts the bytes of the run.
func dumpSearch(index []byte, plain string, queries []string, hits [][]Hit, caseSensitive bool) ([]string, error) {
	runs, err := bdf.DecodeTextIndex(index)
	if err != nil {
		return nil, err
	}
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
				mark := 0
				if strings.HasPrefix(runs[s.Run].Text, "\ufeff") {
					mark = 3
				}
				lines = append(lines, fmt.Sprintf("S %d %d %d %d %d %d", s.Run, s.A, s.B, s.Ordinal, s.Start-mark, s.End-mark))
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
		got, err := dumpSearch(part, plain, qs, hits, caseSensitive)
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

// Characters a search folds, and some it does not.
var searchAlphabet = []string{
	"a", "b", "c", "A", "B", "Z", "\u0020", "\u0020", "\u0009", "\u00a0", "\u3000", "1",
	"\uff11", "\uff21", "\uff41", "\uff76", "\uff9e", "\u30ab", "\u30ac", "\u304b", "\u304c", "\u30fc", "\u00e9", "e\u0301",
	"\u00c9", "\u00df", "\u1e9e", "\u0130", "I", "\u0131", "\u03a3", "\u03c3", "\u03c2", "\ufb01", "\ufb03", "\u3314",
	"\u337f", "\u2460", "\u2168", "\u2178", "\u00bd", "\u00b2", "\u2212", "-", "\u2010", "\U0001f600", "\U0001d400", "\U0001d49c",
	"\ud55c", "\u1112", "\u314e", "\uffa1", "\ufdfa", "\u2026", "\u000a", "\u2028", "\u01c4", "\u01c5", "\u01c6", "\u2126",
	"\u03a9", "\u212a", "\u212b", "\u6f22", "\u0000", "\u309e", "\u30fe", "\u30f6", "\u30f7", "\uff70", "\u1680", "\ufeff",
	"\ua66e", "\ua66c", "\u03a3\u03c3", "a\u03a3",
}

func randomText(r *rand.Rand, most int) string {
	var b strings.Builder
	for range r.IntN(most + 1) {
		b.WriteString(pick(r, searchAlphabet))
	}
	return b.String()
}

// TestSearchRandom searches random text with libbdf and with the reference's
// TypeScript reader, which it runs with node: set LIBBDF_BDF_CHECKOUT to a
// checkout of bdf with packages/core built. LIBBDF_SEARCH_CASES and
// LIBBDF_SEARCH_SEED change how many indexes and which.
func TestSearchRandom(t *testing.T) {
	checkout := os.Getenv("LIBBDF_BDF_CHECKOUT")
	if checkout == "" {
		t.Skip("LIBBDF_BDF_CHECKOUT is not set")
	}
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node is not installed")
	}
	cases, seed := 10000, uint64(7)
	if n, err := strconv.Atoi(os.Getenv("LIBBDF_SEARCH_CASES")); err == nil && n > 0 {
		cases = n
	}
	if n, err := strconv.ParseUint(os.Getenv("LIBBDF_SEARCH_SEED"), 10, 64); err == nil {
		seed = n
	}
	r := rand.New(rand.NewPCG(20261013, seed))
	type input struct {
		Index         []byte   `json:"index"`
		Queries       []string `json:"queries"`
		CaseSensitive bool     `json:"caseSensitive"`
	}
	inputs := make([]input, cases)
	var in bytes.Buffer
	enc := json.NewEncoder(&in)
	enc.SetEscapeHTML(false)
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
		inputs[i] = input{Index: bdf.EncodeTextIndex(runs), Queries: queries, CaseSensitive: r.IntN(3) == 0}
		enc.Encode(inputs[i])
	}
	file := filepath.Join(t.TempDir(), "indexes.jsonl")
	if err := os.WriteFile(file, in.Bytes(), 0o644); err != nil {
		t.Fatal(err)
	}
	script, _ := filepath.Abs(filepath.Join("..", "tools", "search-oracle.mjs"))
	out, err := exec.Command(node, script, checkout, "--indexes", file).Output()
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
		got, err := dumpSearch(c.Index, plain, c.Queries, hits, c.CaseSensitive)
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
	t.Logf("%d indexes, %d hits match", cases, found)
}
