package svgdoc

// Go's fuzzer with the differential comparisons as its bodies. It is guided
// by the coverage of the Go code: it finds the inputs where the reference
// takes a path of its own.
//
//	GOFLAGS=-mod=mod GOPROXY=off go test -run '^$' -fuzz '^FuzzSVG$' -fuzztime 5m -parallel 2 -fuzzminimizetime 20x .

import (
	"math/rand"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/shibukawa/bdf/image/svg"
)

// FuzzSVG reads every input with both readers, with the default limits and
// with small ones, and compares the documents and the values of all their
// attributes.
func FuzzSVG(f *testing.F) {
	for _, data := range allDocs(f) {
		if len(data) <= 1<<16 {
			f.Add(data)
		}
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		if len(data) > 1<<18 {
			t.Skip()
		}
		for _, opts := range []*svg.Options{nil, smallLimits} {
			if d := compare(data, opts); d != "" {
				t.Fatalf("limits %v: %s", opts != nil, d)
			}
		}
	})
}

// FuzzValues asks the value functions of both about every input.
func FuzzValues(f *testing.F) {
	rng := rand.New(rand.NewSource(3))
	for i := 0; i < 300; i++ {
		f.Add(valueInput(rng))
	}
	for _, s := range []string{"hsl(120deg 50% 50% / 25%)", "rgb(\u0130nf,1e999,nan,-inf)", "translate(1,2) rotate(30 4 5) skewX(6) matrix(1 0 0 1 7 8) scale(-1e3)", "#abcd", "1.5\u0130N", "currentColor", "1,2 3-4.5.5e3"} {
		f.Add(s)
	}
	f.Fuzz(func(t *testing.T, s string) {
		if g, c := goValues(s, exactFloats), cValues(s, exactFloats); g != c {
			t.Fatalf("the values of %q:\n--- reference\n%s--- C\n%s", s, g, c)
		}
	})
}

// FuzzFloat compares bdfi_parse_float with strconv.ParseFloat.
func FuzzFloat(f *testing.F) {
	rng := rand.New(rand.NewSource(4))
	for i := 0; i < 400; i++ {
		if s := floatInput(rng); len(s) < 2000 {
			f.Add(s)
		}
	}
	for _, s := range floatWords {
		f.Add(s)
	}
	f.Fuzz(func(t *testing.T, s string) {
		gb, gs := goFloat(s)
		bits, status := cFloats([]string{s})
		if gb != bits[0] || gs != status[0] {
			t.Fatalf("ParseFloat(%q): C %016x (%d), Go %016x (%d)", s, bits[0], status[0], gb, gs)
		}
	})
}

// FuzzStrings compares the ports of strings.TrimSpace, ToLower and Fields.
func FuzzStrings(f *testing.F) {
	rng := rand.New(rand.NewSource(5))
	for i := 0; i < 200; i++ {
		f.Add(stringInput(rng))
	}
	f.Fuzz(func(t *testing.T, s string) {
		if g, c := goStrings(s), cStrings(s); g != c {
			t.Fatalf("%q:\n--- Go\n%s--- C\n%s", s, g, c)
		}
	})
}

// TestWriteCorpus writes the documents of the tests to the directory
// LIBBDF_SVG_CORPUS names: the seeds of the libFuzzer target.
func TestWriteCorpus(t *testing.T) {
	dir := os.Getenv("LIBBDF_SVG_CORPUS")
	if dir == "" {
		t.Skip("LIBBDF_SVG_CORPUS names no directory")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	n, docs := 0, allDocs(t)
	for _, name := range sortedNames(docs) {
		data := docs[name]
		if len(data) > 1<<16 {
			continue
		}
		if err := os.WriteFile(filepath.Join(dir, "seed-"+filepath.Base(filepath.Dir(name))+"-"+filepath.Base(name)), data, 0o644); err != nil {
			t.Fatal(err)
		}
		n++
	}
	t.Logf("%d seeds in %s", n, dir)
}

// readFuzzInput reads the bytes of an input Go's fuzzer wrote (a file of
// testdata/fuzz, with one []byte or string in it).
func readFuzzInput(t testing.TB, path string) []byte {
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	lines := strings.SplitN(string(b), "\n", 3)
	if len(lines) < 2 || lines[0] != "go test fuzz v1" {
		return b
	}
	arg := lines[1]
	arg = strings.TrimSuffix(strings.TrimPrefix(strings.TrimPrefix(arg, "[]byte("), "string("), ")")
	s, err := strconv.Unquote(arg)
	if err != nil {
		t.Fatalf("%s: %v", path, err)
	}
	return []byte(s)
}

// TestWriteReplay writes LIBBDF_SVG_REPLAY_CASES damaged and made documents to
// the directory LIBBDF_SVG_REPLAY names, a thousand to a file of the form
// replay.sh reads: what the C reader is run on under the sanitizers.
func TestWriteReplay(t *testing.T) {
	dir := os.Getenv("LIBBDF_SVG_REPLAY")
	if dir == "" {
		t.Skip("LIBBDF_SVG_REPLAY names no directory")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	docs := allDocs(t)
	names := sortedNames(docs)
	rng := rand.New(rand.NewSource(int64(envInt("LIBBDF_SVG_SEED", 1))))
	total := envInt("LIBBDF_SVG_REPLAY_CASES", 20000)
	var pack []byte
	files := 0
	for i := 0; i < total; i++ {
		var data []byte
		if i%2 == 0 {
			data = mutate(rng, docs[names[rng.Intn(len(names))]])
			for rng.Intn(3) == 0 {
				data = mutate(rng, data)
			}
		} else {
			data = genDocument(rng)
		}
		// each document after its length, four bytes
		pack = append(pack, byte(len(data)), byte(len(data)>>8), byte(len(data)>>16), byte(len(data)>>24))
		pack = append(pack, data...)
		if i%1000 == 999 || i == total-1 {
			if err := os.WriteFile(filepath.Join(dir, "pack-"+strconv.Itoa(files)), pack, 0o644); err != nil {
				t.Fatal(err)
			}
			pack = pack[:0]
			files++
		}
	}
	t.Logf("%d documents in %d files of %s", total, files, dir)
}
