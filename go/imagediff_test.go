package cbdf

// What the tests of the image decoders share: the comparison with the
// reference's decoding, the files to compare on, and the damage done to
// them. The tests of a decoder are in <format>_test.go.

import (
	"bytes"
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"testing"

	"github.com/shibukawa/bdf"
	"github.com/shibukawa/bdf/image/imgconv"
)

// compare decodes data with the C decoder of a format and with the
// reference and reports how they differ, or "" when they agree: the same
// answer about the size, the same status, and for a decoded image the same
// size and the same bytes.
func compare(format string, data []byte) string {
	gw, gh, gok := refConfig(format, data)
	cw, ch, cok := cConfig(format, data)
	if gok != cok || gok && (gw != cw || gh != ch) {
		return fmt.Sprintf("config: reference %dx%d %v, C %dx%d %v", gw, gh, gok, cw, ch, cok)
	}
	gp, gw, gh, gs := reference(format, data)
	cp, cw, ch, cs := cDecode(format, data)
	if gs != cs {
		return fmt.Sprintf("decode: reference status %d (%dx%d), C status %d (%dx%d)", gs, gw, gh, cs, cw, ch)
	}
	if gs != statusOK {
		return ""
	}
	if gw != cw || gh != ch {
		return fmt.Sprintf("decode: reference %dx%d, C %dx%d", gw, gh, cw, ch)
	}
	if !bytes.Equal(gp, cp) {
		n, first := 0, -1
		for i := range gp {
			if gp[i] != cp[i] {
				if first < 0 {
					first = i
				}
				n++
			}
		}
		return fmt.Sprintf("decode: %d of %d bytes differ, the first at pixel (%d, %d) channel %d: reference %d, C %d",
			n, len(gp), first/4%gw, first/4/gw, first%4, gp[first], cp[first])
	}
	return ""
}

// files returns the regular files under the given directories whose names
// end in one of the extensions (all of them when there are none), by path.
// A directory that does not exist has none.
func files(dirs []string, exts ...string) map[string][]byte {
	out := map[string][]byte{}
	for _, d := range dirs {
		filepath.Walk(d, func(p string, info os.FileInfo, err error) error {
			if err != nil || !info.Mode().IsRegular() {
				return nil
			}
			ok := len(exts) == 0
			for _, e := range exts {
				ok = ok || strings.HasSuffix(strings.ToLower(p), e)
			}
			if ok {
				if b, err := os.ReadFile(p); err == nil {
					out[p] = b
				}
			}
			return nil
		})
	}
	return out
}

// documentImages returns the image parts of the test documents that Sniff
// of the reference takes for the format, by document and part.
func documentImages(t testing.TB, format string) map[string][]byte {
	t.Helper()
	out := map[string][]byte{}
	for _, f := range documents(t) {
		r, err := bdf.OpenSingleFile(f)
		if err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		if r.Locked() && r.Unlock("demo-パスワード") != nil {
			r.Close()
			continue
		}
		doc, err := r.ToDocument()
		r.Close()
		if err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		for _, p := range doc.Parts() {
			if p.Type == bdf.PartImage && imgconv.Sniff(p.Data) == format {
				out[filepath.Base(f)+" "+p.Hash.String()] = p.Data
			}
		}
	}
	return out
}

// withDocumentImages are the files of the given directories that end in
// the format's name, and the image parts of the test documents that are of
// the format.
func withDocumentImages(t testing.TB, format string, dirs []string, exts ...string) map[string][]byte {
	t.Helper()
	out := files(dirs, exts...)
	for k, v := range documentImages(t, format) {
		out[k] = v
	}
	return out
}

// goroot is the source tree of the Go that runs the tests: its image
// packages have testdata.
func goroot() string { return filepath.Join(runtime.GOROOT(), "src") }

// ximage is the directory of the golang.org/x/image module the reference
// decoders come from: it has testdata too.
func ximage(t testing.TB) string {
	t.Helper()
	out, err := exec.Command("go", "list", "-m", "-f", "{{.Dir}}", "golang.org/x/image").Output()
	if err != nil {
		t.Fatalf("locating golang.org/x/image: %v", err)
	}
	return strings.TrimSpace(string(out))
}

// mutate damages an image the ways files get damaged: flipped bits, bytes
// overwritten, a cut, a stretch removed or repeated.
func mutate(rng *rand.Rand, src []byte) []byte {
	b := append([]byte(nil), src...)
	if len(b) == 0 {
		return b
	}
	switch rng.Intn(7) {
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
		// the header is where a decoder decides the most
		b[rng.Intn(min(len(b), 64))] = byte(rng.Intn(256))
	case 6:
		// the extremes of a field
		v := []byte{0, 1, 0x7f, 0x80, 0xff}[rng.Intn(5)]
		i := rng.Intn(len(b))
		for j := i; j < len(b) && j < i+1+rng.Intn(4); j++ {
			b[j] = v
		}
	}
	return b
}

func envInt(name string, def int) int {
	if v, err := strconv.Atoi(os.Getenv(name)); err == nil {
		return v
	}
	return def
}

// checkFiles compares the decoders on every file given.
func checkFiles(t *testing.T, format string, in map[string][]byte) {
	t.Helper()
	decoded := 0
	for name, data := range in {
		if d := compare(format, data); d != "" {
			t.Errorf("%s: %s", name, d)
		}
		if _, _, _, s := reference(format, data); s == statusOK {
			decoded++
		}
	}
	t.Logf("%d files, %d of them images the reference decodes", len(in), decoded)
}

// checkMutations compares the decoders on damaged copies of the files
// given. LIBBDF_IMAGE_CASES sets how many (the default is small: a test run
// takes seconds), LIBBDF_IMAGE_SEED the seed, and LIBBDF_IMAGE_DUMP names a directory
// the files they disagree on are written to.
func checkMutations(t *testing.T, format string, in map[string][]byte) {
	t.Helper()
	var names []string
	for name := range in {
		names = append(names, name)
	}
	if len(names) == 0 {
		t.Skip("no files to damage")
	}
	// a map has no order: the same seed has to damage the same files
	for i := 1; i < len(names); i++ {
		for j := i; j > 0 && names[j] < names[j-1]; j-- {
			names[j], names[j-1] = names[j-1], names[j]
		}
	}
	cases, seed := envInt("LIBBDF_IMAGE_CASES", 2000), int64(envInt("LIBBDF_IMAGE_SEED", 1))
	rng := rand.New(rand.NewSource(seed))
	decoded, failed := 0, 0
	for i := 0; i < cases; i++ {
		name := names[rng.Intn(len(names))]
		data := mutate(rng, in[name])
		for rng.Intn(4) == 0 {
			data = mutate(rng, data)
		}
		if _, _, _, s := reference(format, data); s == statusOK {
			decoded++
		}
		if d := compare(format, data); d != "" {
			failed++
			if failed <= 20 {
				t.Errorf("case %d of seed %d (%s): %s", i, seed, filepath.Base(name), d)
			}
			if dir := os.Getenv("LIBBDF_IMAGE_DUMP"); dir != "" {
				os.MkdirAll(dir, 0o755)
				os.WriteFile(filepath.Join(dir, fmt.Sprintf("%s-%d-%d", format, seed, i)), data, 0o644)
			}
		}
	}
	t.Logf("%d damaged files, %d of them still images the reference decodes, %d disagreements", cases, decoded, failed)
}

// fuzzDecode is the body of a decoder's fuzz test: the seeds are the files
// given, and every input has to decode alike.
func fuzzDecode(f *testing.F, format string, in map[string][]byte) {
	for _, data := range in {
		if len(data) <= 1<<16 {
			f.Add(data)
		}
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		// an image of many megapixels says nothing a small one does not
		if w, h, ok := refConfig(format, data); ok && (w > 4096 || h > 4096 || w*h > 1<<22) {
			t.Skip()
		}
		if d := compare(format, data); d != "" {
			t.Fatal(d)
		}
	})
}
