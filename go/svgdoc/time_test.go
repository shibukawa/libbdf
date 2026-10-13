package svgdoc

import (
	"math/rand"
	"os"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/shibukawa/bdf/image/svg"
)

// TestTimes says how long the two readers take to read the files of the
// tests (LIBBDF_SVG_TIMES=1), and which of the documents of the tests take
// long in one of them.
func TestTimes(t *testing.T) {
	if os.Getenv("LIBBDF_SVG_TIMES") == "" {
		t.Skip("LIBBDF_SVG_TIMES is not set")
	}
	files := svgFiles(t)
	var goAll, cAll time.Duration
	bytes := 0
	const times = 200
	for _, name := range sortedNames(files) {
		data := files[name]
		t0 := time.Now()
		for i := 0; i < times; i++ {
			svg.Parse(data, nil)
		}
		t1 := time.Now()
		cRead(data, times)
		t2 := time.Now()
		goAll += t1.Sub(t0)
		cAll += t2.Sub(t1)
		bytes += len(data)
	}
	t.Logf("%d files, %d bytes, read %d times each: the reference %v, C %v (%.1f times as fast; %.0f MB/s)", len(files), bytes, times, goAll, cAll,
		float64(goAll)/float64(cAll), float64(bytes)*times/cAll.Seconds()/1e6)
	for name, data := range builtDocs() {
		t0 := time.Now()
		func() {
			defer func() { recover() }()
			svg.Parse(data, nil)
		}()
		t1 := time.Now()
		cRead(data, 1)
		t2 := time.Now()
		if t2.Sub(t0) > 50*time.Millisecond {
			t.Logf("%s (%d bytes): the reference %v, C %v", name, len(data), t1.Sub(t0), t2.Sub(t1))
		}
	}
	// numbers, as path data and points have them
	rng := rand.New(rand.NewSource(1))
	var short, long strings.Builder
	for i := 0; i < 20000; i++ {
		short.WriteString(strconv.FormatFloat(float64(rng.Intn(200000)-100000)/100, 'f', -1, 64) + " ")
		long.WriteString(strconv.FormatFloat(rng.NormFloat64()*100, 'g', 17, 64) + ",")
	}
	for name, s := range map[string]string{"short": short.String(), "of 17 digits": long.String()} {
		t0 := time.Now()
		n := 0
		for i := 0; i < 50; i++ {
			n = len(svg.ParseNumbers(s))
		}
		t1 := time.Now()
		if got := cNumbers(s, 50); got != n {
			t.Fatalf("%d numbers, the reference has %d", got, n)
		}
		t2 := time.Now()
		t.Logf("%d numbers (%s): the reference %.0f ns each, C %.0f ns each", n, name, float64(t1.Sub(t0))/50/float64(n), float64(t2.Sub(t1))/50/float64(n))
	}
}
