package cbdf

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"math/rand"
	"os"
	"sort"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

// jpegDiskFiles are the files that exist: the image parts of the reference's
// documents, the testdata of Go's image packages, and what gen.sh made with
// ImageMagick and ffmpeg.
func jpegDiskFiles(t testing.TB) map[string][]byte {
	return withDocumentImages(t, "jpeg", []string{goroot() + "/image/testdata", "testdata/images"}, ".jpeg", ".jpg")
}

var jpegAll struct {
	once  sync.Once
	files map[string][]byte
}

// jpegFiles are the files every test of the decoder runs on: those on disk,
// rewritten copies of them (jpegVariants), images Go's encoder writes
// (jpegEncoded) and files written symbol by symbol (jpegSynthetic).
func jpegFiles(t testing.TB) map[string][]byte {
	jpegAll.once.Do(func() {
		out := jpegDiskFiles(t)
		for name, data := range jpegDiskFiles(t) {
			if len(data) > 8<<10 {
				continue
			}
			for v, d := range jpegVariants(data) {
				out[name+"#"+v] = d
			}
		}
		for name, data := range jpegEncoded() {
			out[name] = data
		}
		for name, data := range jpegSynthetic(1, 600) {
			out[name] = data
		}
		for name, data := range jpegHandmade() {
			out[name] = data
		}
		jpegAll.files = out
	})
	return jpegAll.files
}

func TestJpegFiles(t *testing.T) {
	checkFiles(t, "jpeg", jpegFiles(t))
}

func TestJpegMutations(t *testing.T) {
	checkMutations(t, "jpeg", jpegFiles(t))
}

func FuzzJpeg(f *testing.F) {
	fuzzDecode(f, "jpeg", jpegFiles(f))
}

// TestJpegSynthetic checks the writer of synthetic files itself: nearly all
// it writes has to be an image for the reference, or it tests little. And
// it compares on more of them than the other tests carry around.
func TestJpegSynthetic(t *testing.T) {
	in := jpegSynthetic(int64(envInt("LIBBDF_IMAGE_SEED", 1))+1000, envInt("LIBBDF_IMAGE_SYNTH", 3000))
	decoded := 0
	kinds := map[string]int{}
	for name, data := range in {
		if d := compare("jpeg", data); d != "" {
			t.Errorf("%s: %s", name, d)
			if dir := os.Getenv("LIBBDF_IMAGE_DUMP"); dir != "" {
				os.MkdirAll(dir, 0o755)
				os.WriteFile(dir+"/"+name, data, 0o644)
			}
		}
		if _, _, _, s := reference("jpeg", data); s == statusOK {
			decoded++
			img, _ := jpeg.Decode(bytes.NewReader(data))
			kind := fmt.Sprintf("%T", img)
			if m, ok := img.(*image.YCbCr); ok {
				kind += " " + m.SubsampleRatio.String()
			}
			kinds[kind]++
		}
	}
	t.Logf("%d synthetic files, %d of them images the reference decodes: %v", len(in), decoded, kinds)
	if decoded < len(in)*8/10 {
		t.Errorf("only %d of %d synthetic files are images: the writer is broken", decoded, len(in))
	}
}

// TestJpegKinds says what the files are for Go's decoder, so that a kind of
// image that no file has is seen.
func TestJpegKinds(t *testing.T) {
	kinds := map[string]int{}
	for _, data := range jpegFiles(t) {
		img, err := jpeg.Decode(bytes.NewReader(data))
		if err != nil {
			kinds["error: "+err.Error()]++
			continue
		}
		kind := fmt.Sprintf("%T", img)
		if m, ok := img.(*image.YCbCr); ok {
			kind += " " + m.SubsampleRatio.String()
		}
		kinds[kind]++
	}
	var names []string
	for k := range kinds {
		names = append(names, k)
	}
	sort.Strings(names)
	for _, k := range names {
		t.Logf("%5d  %s", kinds[k], k)
	}
	for _, want := range []string{"*image.Gray", "*image.RGBA", "*image.CMYK",
		"*image.YCbCr YCbCrSubsampleRatio444", "*image.YCbCr YCbCrSubsampleRatio422",
		"*image.YCbCr YCbCrSubsampleRatio420", "*image.YCbCr YCbCrSubsampleRatio440",
		"*image.YCbCr YCbCrSubsampleRatio411", "*image.YCbCr YCbCrSubsampleRatio410"} {
		if kinds[want] == 0 {
			t.Errorf("no file decodes to %s", want)
		}
	}
}

// TestJpegGoErrors says what Go's decoder makes of the damaged files of
// TestJpegMutations: every error it has, and whether it panics anywhere. It
// does not (see NOTES.md): the C has no check that stands in for a panic,
// and if this test ever reports one, it needs one.
func TestJpegGoErrors(t *testing.T) {
	in := jpegFiles(t)
	var names []string
	for name := range in {
		names = append(names, name)
	}
	sort.Strings(names)
	rng := rand.New(rand.NewSource(int64(envInt("LIBBDF_IMAGE_SEED", 1))))
	kinds := map[string]int{}
	for i := 0; i < envInt("LIBBDF_IMAGE_CASES", 2000); i++ {
		data := mutate(rng, in[names[rng.Intn(len(names))]])
		for rng.Intn(4) == 0 {
			data = mutate(rng, data)
		}
		func() {
			defer func() {
				if r := recover(); r != nil {
					kinds[fmt.Sprintf("panic: %v", r)]++
					t.Errorf("Go's decoder panics: %v", r)
				}
			}()
			_, cerr := jpeg.DecodeConfig(bytes.NewReader(data))
			_, err := jpeg.Decode(bytes.NewReader(data))
			switch {
			case err != nil:
				kinds[err.Error()]++
			case cerr != nil:
				// bdfi_jpeg_decode relies on this not happening
				t.Errorf("DecodeConfig fails (%v) where Decode does not", cerr)
			default:
				kinds["decoded"]++
			}
		}()
	}
	var ks []string
	for k := range kinds {
		ks = append(ks, k)
	}
	sort.Strings(ks)
	for _, k := range ks {
		t.Logf("%7d  %s", kinds[k], k)
	}
}

// TestJpegBuffer moves the data of scans across the ends of the 4096-byte
// buffer Go's decoder reads through (the first ends at byte 4096, the
// following ones 4094 bytes later each), a byte at a time, with a comment
// segment of every length in front: stuffed 0xff, restart markers and the
// end of a scan each come to lie on both sides of a buffer's end. The C has
// no buffer; that Go's does not show is what its reader was ported on.
func TestJpegBuffer(t *testing.T) {
	in := map[string][]byte{}
	for name, data := range jpegDiskFiles(t) {
		for _, pick := range []string{"m-rst1-wrap", "m-rst1-1x1", "m-rst2-prog-1x1", "m-gray-prog-rst2", "m-q100.", "m-ycck-rst2"} {
			if strings.Contains(name, pick) {
				in[pick] = data
				// cut in the data, and with a marker where data should be
				in[pick+"-cut"] = data[:len(data)*3/4]
				bad := append([]byte(nil), data...)
				copy(bad[len(bad)*3/4:], []byte{0xff, 0xff, 0xc5, 0xff})
				in[pick+"-marker"] = bad
			}
		}
	}
	// and synthetic files with restart intervals, dense in 0xff
	rng := rand.New(rand.NewSource(5))
	for i := 0; i < 6; i++ {
		spec := jpegSynthSpec{w: 40 + 8*i, h: 24, sof: []byte{0xc0, 0xc2}[i%2], ri: 1 + i%3, plain: true}
		in[fmt.Sprintf("synth-%d", i)] = jpegSynthFile(rng, &spec)
	}
	if len(in) < 20 {
		t.Fatalf("only %d files", len(in))
	}
	n, decoded := 0, 0
	for name, data := range in {
		// every place of the file across the first end of a buffer; with
		// LIBBDF_IMAGE_BUFFER_ALL=1 across the second one too, and everything else
		from, to := max(0, 4096-len(data)-8), 4100
		if os.Getenv("LIBBDF_IMAGE_BUFFER_ALL") != "" {
			from, to = 0, 8400
		}
		for pad := from; pad <= to; pad++ {
			padded := append([]byte{0xff, 0xd8, 0xff, 0xfe, byte((pad + 2) >> 8), byte(pad + 2)}, make([]byte, pad)...)
			padded = append(padded, data[2:]...)
			if d := compare("jpeg", padded); d != "" {
				t.Fatalf("%s with %d bytes in front: %s", name, pad, d)
			}
			n++
			if pad%512 == 0 {
				if _, _, _, s := reference("jpeg", padded); s == statusOK {
					decoded++
				}
			}
		}
	}
	t.Logf("%d files compared; of a sample, %d are images", n, decoded)
}

// TestJpegWrap compares on the two sums of the decoder that take many
// blocks or many scans to overflow an int32: the DC predictor (32769 blocks
// with the largest difference there is) a coefficient that refinement
// scans add to (65539 scans, starting from the largest coefficient a scan
// can set), and the DC coefficient shifted left by the scan's Al. Go's
// arithmetic wraps there, and so must the C's.
func TestJpegWrap(t *testing.T) {
	if testing.Short() {
		t.Skip("large files")
	}
	in := map[string][]byte{}
	// a DC difference of 16 bits is "0" and 16 bits: all ones is +65535,
	// all zeroes -65535; "0" ends the block
	sixteen := jpegTable{counts: []byte{1}, vals: []byte{16}}
	eob := jpegTable{counts: []byte{1}, vals: []byte{0}}
	for name, bits := range map[string]string{"dc-up": "0 1111111111111111 0 ", "dc-down": "0 0000000000000000 0 "} {
		x := jpegSplit(jpegTiny(8, 0xc0, sixteen, eob, strings.Repeat(bits, 33000)))
		w, h := 264*8, 125*8
		copy(x[1].payload[1:5], []byte{byte(h >> 8), byte(h), byte(w >> 8), byte(w)})
		x[0].payload = append([]byte{0}, bytes.Repeat([]byte{1}, 64)...)
		in[name] = jpegJoin(x)
	}
	// 130 blocks of a progressive image whose DC scan shifts by 8 bits: the
	// predictor passes 1 << 23
	{
		x := jpegSplit(jpegTiny(8, 0xc2, sixteen, eob, strings.Repeat("0 1111111111111111 ", 130)))
		x[1].payload[3], x[1].payload[4] = 1040>>8, 1040&0xff
		x[0].payload = append([]byte{0}, bytes.Repeat([]byte{1}, 64)...)
		x[3].payload = []byte{1, 1, 0, 0, 0, 0x08}
		in["dc-shift"] = jpegJoin(x)
	}
	// one block; a scan sets the first AC coefficient to (+-32767 << 15), and
	// every refinement scan (an end of band, and a bit that says "add") adds
	// (1 << 14) to it, away from zero
	ac := jpegTable{counts: []byte{2}, vals: []byte{0x00, 0x0f}}
	for name, first := range map[string]string{"refine-up": "1 111111111111111", "refine-down": "1 000000000000000"} {
		x := jpegSplit(jpegTiny(8, 0xc2, sixteen, ac, first))
		x[0].payload = append([]byte{0}, bytes.Repeat([]byte{1}, 64)...)
		x[3].payload = []byte{1, 1, 0, 1, 1, 0x0f}
		refine := jpegSegment{marker: 0xda, payload: []byte{1, 1, 0, 1, 1, 0xfe}, data: []byte{0x7f}}
		segs := []jpegSegment{x[0], x[1], x[2], x[3]}
		// the 65538th scan reaches 1 << 31 (or the 65539th passes -1 << 31);
		// one more, and the sum is not one whose low bits the transform drops
		for i := 0; i < 65539; i++ {
			segs = append(segs, refine)
		}
		in[name] = jpegJoin(append(segs, x[4]))
	}
	for name, data := range in {
		if _, _, _, s := reference("jpeg", data); s != statusOK {
			t.Errorf("%s: the reference does not decode it", name)
		}
		if d := compare("jpeg", data); d != "" {
			t.Errorf("%s (%d bytes): %s", name, len(data), d)
		}
	}
}

// TestJpegSizes compares on frame headers at the limits of the size: the
// most pixels the reference decodes, one row more, the widest and the
// tallest image, and none. The files have no image data: what is compared
// is which of them is too large, which is refused, and (with
// LIBBDF_IMAGE_HEAVY=1) that the largest images are allocated and then refused
// for the data they lack.
func TestJpegSizes(t *testing.T) {
	for _, c := range []struct {
		w, h   int
		status int
	}{
		{10240, 10240, statusFormat}, // exactly the limit: decoded, and then the data is missing
		{10241, 10240, statusTooLarge},
		{10240, 10241, statusTooLarge},
		{65535, 1600, statusFormat},
		{65535, 1601, statusTooLarge},
		{1600, 65535, statusFormat},
		{1601, 65535, statusTooLarge},
		{65535, 65535, statusTooLarge},
		{0, 8, statusTooLarge},
		{8, 0, statusTooLarge},
		{0, 0, statusTooLarge},
		{1, 1, statusFormat},
	} {
		for _, nComp := range []int{1, 3, 4} {
			for _, sof := range []byte{0xc0, 0xc2} {
				for _, tail := range []string{"eoi", "sos", "nothing", "bad-dht", "jfif-eoi", "jfif-nothing", "jfif-sos"} {
					if os.Getenv("LIBBDF_IMAGE_HEAVY") == "" && c.status == statusFormat && c.w > 1 && strings.HasSuffix(tail, "sos") {
						// Both decoders allocate the image here, and for a
						// progressive one its coefficients: up to 1.7 GB each.
						// Only with LIBBDF_IMAGE_HEAVY=1.
						continue
					}
					data := jpegHeaderOnly(c.w, c.h, nComp, sof, tail)
					name := fmt.Sprintf("%dx%d of %d components, SOF%d, %s", c.w, c.h, nComp, sof-0xc0, tail)
					if d := compare("jpeg", data); d != "" {
						t.Errorf("%s: %s", name, d)
					}
					// DecodeConfig reads a size where the frame header of a JFIF
					// file ends or a scan begins, and only then is a size too large
					want := c.status
					if tail == "eoi" || tail == "nothing" {
						want = statusFormat
					}
					if _, _, _, s := reference("jpeg", data); s != want {
						t.Errorf("%s: the reference returns status %d, the test expects %d", name, s, want)
					}
				}
			}
		}
	}
}

// TestJpegLarge compares on images with more rows and columns than the
// other tests have: offsets beyond 16 bits each way and some megapixels.
func TestJpegLarge(t *testing.T) {
	if testing.Short() {
		t.Skip("large images")
	}
	rng := rand.New(rand.NewSource(7))
	for _, c := range []struct {
		w, h int
		gray bool
	}{{65535, 9, false}, {9, 65535, false}, {65535, 17, true}, {3001, 2003, false}, {2003, 3001, true}} {
		var img image.Image
		if c.gray {
			m := image.NewGray(image.Rect(0, 0, c.w, c.h))
			for i := range m.Pix {
				m.Pix[i] = byte(i/7) ^ byte(rng.Intn(16))
			}
			img = m
		} else {
			m := image.NewRGBA(image.Rect(0, 0, c.w, c.h))
			for i := 0; i < len(m.Pix); i += 4 {
				x, y := i/4%c.w, i/4/c.w
				m.Pix[i], m.Pix[i+1], m.Pix[i+2], m.Pix[i+3] = byte(x), byte(y), byte(x+y)^byte(rng.Intn(8)), 255
			}
			img = m
		}
		var buf bytes.Buffer
		if err := jpeg.Encode(&buf, img, &jpeg.Options{Quality: 60}); err != nil {
			t.Fatal(err)
		}
		if d := compare("jpeg", buf.Bytes()); d != "" {
			t.Errorf("%dx%d: %s", c.w, c.h, d)
		}
	}
	// and progressive, in flex mode and as CMYK, from the writer of
	// synthetic files
	for i, s := range []jpegSynthSpec{
		{w: 1500, h: 1100, nComp: 3, sof: 0xc2, hv: [][2]int{{2, 1}, {1, 2}, {1, 1}}},
		{w: 1100, h: 1500, nComp: 4, sof: 0xc0, hv: [][2]int{{2, 2}, {1, 1}, {1, 1}, {2, 2}}},
		{w: 2100, h: 700, nComp: 3, sof: 0xc1, hv: [][2]int{{4, 2}, {1, 1}, {1, 1}}, ri: 37},
	} {
		s := s
		data := jpegSynthFile(rand.New(rand.NewSource(int64(100+i))), &s)
		if _, _, _, st := reference("jpeg", data); st != statusOK {
			t.Errorf("large synthetic file %d: the reference does not decode it", i)
		}
		if d := compare("jpeg", data); d != "" {
			t.Errorf("large synthetic file %d: %s", i, d)
		}
	}
}

// jpegCPU is the processor time the process has used: on a busy machine
// the only time that says anything.
func jpegCPU() time.Duration {
	var ru syscall.Rusage
	syscall.Getrusage(syscall.RUSAGE_SELF, &ru)
	return time.Duration(ru.Utime.Nano() + ru.Stime.Nano())
}

// BenchmarkJpeg decodes a few files with the reference and with the C
// (which here includes copying the pixels into a Go slice), and reports the
// processor time of a decoding next to the elapsed one.
func BenchmarkJpeg(b *testing.B) {
	type file struct {
		name string
		data []byte
	}
	var in []file
	disk := jpegDiskFiles(b)
	for _, name := range []string{"video-001.jpeg", "video-001.progressive.jpeg", "video-005.gray.jpeg", "video-001.cmyk.jpeg", "video-001.q50.410.jpeg", "video-001.q50.211211.jpeg", "video-001.rgb.jpeg"} {
		data := disk[goroot()+"/image/testdata/"+name]
		if data == nil {
			b.Fatalf("no %s", name)
		}
		in = append(in, file{name, data})
	}
	// an image of the size of a photograph from Go's encoder
	m := image.NewRGBA(image.Rect(0, 0, 2048, 1536))
	rng := rand.New(rand.NewSource(3))
	for i := 0; i < len(m.Pix); i += 4 {
		x, y := i/4%2048, i/4/2048
		m.Pix[i], m.Pix[i+1], m.Pix[i+2], m.Pix[i+3] = byte(x/3), byte(y/2), byte((x+y)/5)^byte(rng.Intn(8)), 255
	}
	var buf bytes.Buffer
	jpeg.Encode(&buf, m, &jpeg.Options{Quality: 85})
	in = append(in, file{"2048x1536", buf.Bytes()})
	// and the slowest kind of file: scans that are one end-of-band run each
	in = append(in, file{"100-scans-1024x1024", jpegManyScans(1024, 1024, 100)})

	for _, f := range in {
		for _, who := range []string{"go", "c"} {
			b.Run(who+"/"+f.name, func(b *testing.B) {
				b.SetBytes(int64(len(f.data)))
				t0 := jpegCPU()
				for i := 0; i < b.N; i++ {
					if who == "go" {
						reference("jpeg", f.data)
					} else {
						cDecode("jpeg", f.data)
					}
				}
				b.ReportMetric(float64(jpegCPU()-t0)/float64(b.N)/1e6, "cpu-ms/op")
			})
		}
	}
}

// jpegManyScans is a progressive gray image whose scans, as many as asked
// for, are one end-of-band run over all its blocks each: a first scan of
// the AC band and refinements that find nothing to refine. Every scan costs
// a visit of every block and a dozen bytes of the file; Go's decoder has
// no limit on that, nor has the port (see NOTES.md).
func jpegManyScans(w, h, scans int) []byte {
	// the AC table: one symbol, a run of 16384 blocks and 14 bits more
	dht := append([]byte{0x10, 1}, append(make([]byte, 15), 0xe0)...)
	segs := []jpegSegment{
		{marker: 0xc2, payload: []byte{8, byte(h >> 8), byte(h), byte(w >> 8), byte(w), 1, 1, 0x11, 0}},
		{marker: 0xc4, payload: dht},
	}
	var bw jpegBitWriter
	for left := ((w + 7) / 8) * ((h + 7) / 8); left > 0; left -= 32767 {
		bw.put(0, 1)
		bw.put(uint32(max(min(left, 32767)-16384, 0)), 14)
	}
	bw.pad()
	segs = append(segs, jpegSegment{marker: 0xda, payload: []byte{1, 1, 0, 1, 63, 0x01}, data: bw.out})
	for i := 1; i < scans; i++ {
		segs = append(segs, jpegSegment{marker: 0xda, payload: []byte{1, 1, 0, 1, 63, 0x10}, data: bw.out})
	}
	return jpegJoin(append(segs, jpegSegment{marker: 0xd9}))
}

// ---- Images from Go's encoder ---------------------------------------------

// jpegEncoded are images Go's encoder writes: gray (one component) and
// colour (4:2:0, the only layout it has), at many qualities and at sizes
// around the multiples of 8 and 16.
func jpegEncoded() map[string][]byte {
	out := map[string][]byte{}
	rng := rand.New(rand.NewSource(11))
	sizes := [][2]int{{1, 1}, {1, 2}, {2, 1}, {3, 5}, {7, 7}, {8, 8}, {9, 9}, {15, 15}, {16, 16}, {17, 17},
		{16, 17}, {17, 16}, {23, 24}, {24, 23}, {31, 33}, {32, 32}, {33, 31}, {47, 64}, {65, 48}, {1, 40}, {40, 1}, {100, 3}}
	qualities := []int{1, 2, 5, 10, 25, 50, 75, 90, 95, 99, 100}
	patterns := []string{"gradient", "noise", "checker", "flat", "edges"}
	n := 0
	for _, sz := range sizes {
		for pi, p := range patterns {
			w, h := sz[0], sz[1]
			pix := func(x, y int) (r, g, b byte) {
				switch p {
				case "gradient":
					return byte(255 * x / w), byte(255 * y / h), byte(255 * (x + y) / (w + h))
				case "noise":
					return byte(rng.Intn(256)), byte(rng.Intn(256)), byte(rng.Intn(256))
				case "checker":
					if (x/3+y/3)%2 == 0 {
						return 250, 20, 40
					}
					return 10, 200, 230
				case "flat":
					return 200, 120, 30
				}
				if x%5 == 0 || y%7 == 0 {
					return 255, 255, 255
				}
				return 0, 0, byte(40 * (x % 4))
			}
			rgba := image.NewRGBA(image.Rect(0, 0, w, h))
			gray := image.NewGray(image.Rect(0, 0, w, h))
			for y := 0; y < h; y++ {
				for x := 0; x < w; x++ {
					r, g, b := pix(x, y)
					rgba.SetRGBA(x, y, color.RGBA{r, g, b, 255})
					gray.SetGray(x, y, color.Gray{byte((int(r) + int(g) + int(b)) / 3)})
				}
			}
			for k := 0; k < 2; k++ {
				q := qualities[(n+pi+5*k)%len(qualities)]
				n++
				var a, b bytes.Buffer
				jpeg.Encode(&a, rgba, &jpeg.Options{Quality: q})
				jpeg.Encode(&b, gray, &jpeg.Options{Quality: q})
				out[fmt.Sprintf("go/colour-%dx%d-%s-q%d", w, h, p, q)] = a.Bytes()
				out[fmt.Sprintf("go/gray-%dx%d-%s-q%d", w, h, p, q)] = b.Bytes()
			}
		}
	}
	return out
}

// ---- Rewritten copies of files --------------------------------------------

// A jpegSegment is a marker segment of a file: the marker, what its length
// covers, and for a scan (and for the bytes after the last marker) the data
// that follows up to the next marker.
type jpegSegment struct {
	marker  byte
	payload []byte
	data    []byte
}

// jpegSplit takes a file apart into its segments (nil if it cannot: the
// files it is used on are well-formed), jpegJoin puts them together.
func jpegSplit(b []byte) []jpegSegment {
	if len(b) < 4 || b[0] != 0xff || b[1] != 0xd8 {
		return nil
	}
	var segs []jpegSegment
	i := 2
	for i+1 < len(b) {
		if b[i] != 0xff {
			return nil
		}
		m := b[i+1]
		i += 2
		if m == 0xd9 {
			segs = append(segs, jpegSegment{marker: m, data: b[i:]})
			return segs
		}
		if i+2 > len(b) {
			return nil
		}
		n := int(b[i])<<8 | int(b[i+1])
		if n < 2 || i+n > len(b) {
			return nil
		}
		s := jpegSegment{marker: m, payload: b[i+2 : i+n]}
		i += n
		if m == 0xda {
			j := i
			for j < len(b) {
				if b[j] == 0xff && j+1 < len(b) && b[j+1] != 0 && (b[j+1] < 0xd0 || b[j+1] > 0xd7) {
					break
				}
				j++
			}
			s.data = b[i:j]
			i = j
		}
		segs = append(segs, s)
	}
	return nil
}

func jpegJoin(segs []jpegSegment) []byte {
	out := []byte{0xff, 0xd8}
	for _, s := range segs {
		out = append(out, 0xff, s.marker)
		if s.marker != 0xd9 {
			n := len(s.payload) + 2
			out = append(out, byte(n>>8), byte(n))
			out = append(out, s.payload...)
		}
		out = append(out, s.data...)
	}
	return out
}

func jpegCloneSegments(segs []jpegSegment) []jpegSegment {
	out := make([]jpegSegment, len(segs))
	for i, s := range segs {
		out[i] = jpegSegment{s.marker, append([]byte(nil), s.payload...), append([]byte(nil), s.data...)}
	}
	return out
}

// jpegAdobe is Adobe's APP14 segment with the colour transform given.
func jpegAdobe(transform byte) jpegSegment {
	return jpegSegment{marker: 0xee, payload: []byte{'A', 'd', 'o', 'b', 'e', 0, 100, 0, 0, 0, 0, transform}}
}

// jpegVariants are copies of a well-formed file with what a decoder has to
// take in its stride changed: the colour model the segments state (JFIF,
// Adobe, the component identifiers), fill bytes, bytes between segments,
// data after the end, no end.
func jpegVariants(data []byte) map[string][]byte {
	segs := jpegSplit(data)
	if segs == nil {
		return nil
	}
	out := map[string][]byte{}
	edit := func(name string, f func(s []jpegSegment) []jpegSegment) {
		out[name] = jpegJoin(f(jpegCloneSegments(segs)))
	}
	without := func(s []jpegSegment, markers ...byte) []jpegSegment {
		var o []jpegSegment
		for _, x := range s {
			drop := false
			for _, m := range markers {
				drop = drop || x.marker == m
			}
			if !drop {
				o = append(o, x)
			}
		}
		return o
	}
	nComp := 0
	for _, s := range segs {
		if s.marker >= 0xc0 && s.marker <= 0xc2 && len(s.payload) >= 6 {
			nComp = int(s.payload[5])
		}
	}

	// the end of the file
	out["no-eoi"] = data[:len(data)-2]
	out["after-eoi"] = append(append([]byte(nil), data...), "\xff\xd8\xff\xdb trailing bytes"...)
	out["no-eoi-garbage"] = append(append([]byte(nil), data[:len(data)-2]...), 0, 1, 2, 3)
	out["two-eoi"] = append(append([]byte(nil), data...), 0xff, 0xd9)
	edit("rst-before-eoi", func(s []jpegSegment) []jpegSegment {
		s[len(s)-2].data = append(s[len(s)-2].data, 0xff, 0xd3)
		return s
	})
	edit("stuffed-ff-before-eoi", func(s []jpegSegment) []jpegSegment {
		s[len(s)-2].data = append(s[len(s)-2].data, 0xff, 0x00, 0xff, 0x00)
		return s
	})
	edit("bytes-before-eoi", func(s []jpegSegment) []jpegSegment {
		s[len(s)-2].data = append(s[len(s)-2].data, 0x12, 0x34, 0x00)
		return s
	})

	// fill bytes before every marker, and bytes that are no segment between
	// the segments
	fill := []byte{0xff, 0xd8}
	junk := []byte{0xff, 0xd8}
	for _, s := range segs {
		one := jpegJoin([]jpegSegment{s})[2:]
		fill = append(append(fill, 0xff, 0xff, 0xff), one...)
		junk = append(append(junk, 0x00, 0x41, 0xff, 0x00, 0x42), one...)
	}
	out["fill-bytes"] = fill
	out["between-segments"] = junk

	// the colour model
	edit("no-app", func(s []jpegSegment) []jpegSegment { return without(s, 0xe0, 0xee) })
	for tr := byte(0); tr < 4; tr++ {
		tr := tr
		edit(fmt.Sprintf("adobe%d", tr), func(s []jpegSegment) []jpegSegment {
			return append([]jpegSegment{jpegAdobe(tr)}, without(s, 0xe0, 0xee)...)
		})
		edit(fmt.Sprintf("jfif-adobe%d", tr), func(s []jpegSegment) []jpegSegment {
			jfif := jpegSegment{marker: 0xe0, payload: []byte("JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")}
			return append([]jpegSegment{jfif, jpegAdobe(tr)}, without(s, 0xe0, 0xee)...)
		})
	}
	edit("adobe-last", func(s []jpegSegment) []jpegSegment {
		s = without(s, 0xe0, 0xee)
		eoi := s[len(s)-1]
		// after the scans: Go takes it into account all the same
		return append(append(s[:len(s)-1:len(s)-1], jpegAdobe(0)), eoi)
	})
	edit("jfif-then-jfxx", func(s []jpegSegment) []jpegSegment {
		jfif := jpegSegment{marker: 0xe0, payload: []byte("JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")}
		jfxx := jpegSegment{marker: 0xe0, payload: []byte("JFXX\x00\x10")}
		return append([]jpegSegment{jfif, jfxx}, without(s, 0xe0, 0xee)...)
	})
	edit("short-apps", func(s []jpegSegment) []jpegSegment {
		// too short to say anything: passed over
		return append([]jpegSegment{{marker: 0xe0, payload: []byte("JFIF")}, {marker: 0xee, payload: []byte("Adobe\x00\x64\x00\x00\x00\x00")},
			{marker: 0xe0}, {marker: 0xfe}, {marker: 0xef, payload: []byte{1}}}, s...)
	})
	edit("jfif-short-app0-adobe0", func(s []jpegSegment) []jpegSegment {
		// an APP0 segment too short to say JFIF does not take it back
		jfif := jpegSegment{marker: 0xe0, payload: []byte("JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")}
		return append([]jpegSegment{jfif, {marker: 0xe0, payload: []byte("JF")}, jpegAdobe(0)}, without(s, 0xe0, 0xee)...)
	})
	edit("adobe0-other-app14", func(s []jpegSegment) []jpegSegment {
		// nor an APP14 segment of somebody else what Adobe's said
		other := jpegSegment{marker: 0xee, payload: []byte("NotAdobe\x00\x00\x00\x02\x02")}
		return append([]jpegSegment{jpegAdobe(0), other}, without(s, 0xe0, 0xee)...)
	})

	// restart markers that are not where they should be: the decoder looks
	// for the one it expects past bytes, fill bytes and stuffed 0xff, and
	// gives up at another marker
	for i, x := range segs {
		k := bytes.Index(x.data, []byte{0xff, 0xd0})
		if x.marker != 0xda || k < 0 {
			continue
		}
		rst := func(name string, f func(data []byte) []byte) {
			edit(name, func(s []jpegSegment) []jpegSegment {
				s[i].data = f(s[i].data)
				return s
			})
		}
		insert := func(b ...byte) func(data []byte) []byte {
			return func(data []byte) []byte {
				return append(append(append([]byte(nil), data[:k]...), b...), data[k:]...)
			}
		}
		rst("rst-after-bytes", insert(0x12, 0x34, 0x56))
		rst("rst-after-byte", insert(0x00))
		rst("rst-after-fill", insert(0xff, 0xff, 0xff))
		rst("rst-after-ff00", insert(0xff, 0x00))
		rst("rst-after-mix", insert(0x01, 0xff, 0x00, 0x02, 0xff, 0xff))
		rst("rst-wrong", func(data []byte) []byte { data[k+1] = 0xd5; return data })
		rst("rst-missing", func(data []byte) []byte { return append(data[:k], data[k+2:]...) })
		rst("rst-twice", insert(0xff, 0xd0))
		break
	}

	if nComp == 3 {
		ids := func(name string, keepJFIF bool, id [3]byte) {
			edit(name, func(s []jpegSegment) []jpegSegment {
				if !keepJFIF {
					s = without(s, 0xe0, 0xee)
				}
				var old [3]byte
				for _, x := range s {
					if x.marker >= 0xc0 && x.marker <= 0xc2 {
						for i := 0; i < 3; i++ {
							old[i] = x.payload[6+3*i]
							x.payload[6+3*i] = id[i]
						}
					}
					if x.marker == 0xda {
						for i := 0; i < int(x.payload[0]); i++ {
							for j := 0; j < 3; j++ {
								if x.payload[1+2*i] == old[j] {
									x.payload[1+2*i] = id[j]
									break
								}
							}
						}
					}
				}
				return s
			})
		}
		ids("ids-rgb", false, [3]byte{'R', 'G', 'B'})
		ids("ids-rgb-jfif", true, [3]byte{'R', 'G', 'B'})
		ids("ids-odd", false, [3]byte{0, 255, 'R'})
		ids("ids-gbr", false, [3]byte{'G', 'B', 'R'})
	}
	return out
}

// ---- Files written by hand -------------------------------------------------

// jpegHeaderOnly is a file with a frame header of the given size and what
// the tail says after it.
func jpegHeaderOnly(w, h, nComp int, sof byte, tail string) []byte {
	out := []byte{0xff, 0xd8}
	if nComp == 4 {
		out = append(out, jpegJoin([]jpegSegment{jpegAdobe(2)})[2:]...)
	}
	if strings.HasPrefix(tail, "jfif-") {
		tail = tail[5:]
		out = append(out, jpegJoin([]jpegSegment{{marker: 0xe0, payload: []byte("JFIF\x00\x01\x01\x00\x00\x01\x00\x01\x00\x00")}})[2:]...)
	}
	p := []byte{8, byte(h >> 8), byte(h), byte(w >> 8), byte(w), byte(nComp)}
	for i := 0; i < nComp; i++ {
		p = append(p, byte(i+1), 0x11, 0)
	}
	segs := []jpegSegment{{marker: sof, payload: p}}
	switch tail {
	case "eoi":
		segs = append(segs, jpegSegment{marker: 0xd9})
	case "sos":
		// a scan of the first component without tables or data
		segs = append(segs, jpegSegment{marker: 0xda, payload: []byte{1, 1, 0, 0, 0, 0}}, jpegSegment{marker: 0xd9})
	case "bad-dht":
		// a table segment Decode refuses and DecodeConfig passes over
		segs = append(segs, jpegSegment{marker: 0xc4, payload: []byte{0x77}},
			jpegSegment{marker: 0xda, payload: []byte{1, 1, 0, 0, 0, 0}}, jpegSegment{marker: 0xd9})
	}
	return append(out, jpegJoin(segs)[2:]...)
}

// jpegHandmade are small files for single decisions of the decoder that
// the others reach by chance at best.
func jpegHandmade() map[string][]byte {
	out := map[string][]byte{
		"hand/empty":         {},
		"hand/ff":            {0xff},
		"hand/soi":           {0xff, 0xd8},
		"hand/soi-eoi":       {0xff, 0xd8, 0xff, 0xd9},
		"hand/soi-ff":        {0xff, 0xd8, 0xff},
		"hand/soi-ffff":      {0xff, 0xd8, 0xff, 0xff, 0xff, 0xff},
		"hand/not-soi":       {0xff, 0xd9, 0xff, 0xd9},
		"hand/short-length":  {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x01, 0xff, 0xd9},
		"hand/zero-length":   {0xff, 0xd8, 0xff, 0xe1, 0x00, 0x02, 0xff, 0xd9},
		"hand/unknown-01":    {0xff, 0xd8, 0xff, 0x01, 0x00, 0x02, 0xff, 0xd9},
		"hand/unknown-c8":    {0xff, 0xd8, 0xff, 0xc8, 0x00, 0x02, 0xff, 0xd9},
		"hand/unknown-dc":    {0xff, 0xd8, 0xff, 0xdc, 0x00, 0x04, 0x00, 0x08, 0xff, 0xd9},
		"hand/sos-first":     {0xff, 0xd8, 0xff, 0xda, 0x00, 0x08, 1, 1, 0, 0, 63, 0, 0xff, 0xd9},
		"hand/dri-long":      {0xff, 0xd8, 0xff, 0xdd, 0x00, 0x05, 0, 1, 2, 0xff, 0xd9},
		"hand/dqt-bad-pq":    {0xff, 0xd8, 0xff, 0xdb, 0x00, 0x03, 0x20, 0xff, 0xd9},
		"hand/dqt-bad-tq":    {0xff, 0xd8, 0xff, 0xdb, 0x00, 0x03, 0x04, 0xff, 0xd9},
		"hand/dqt-short":     {0xff, 0xd8, 0xff, 0xdb, 0x00, 0x05, 0x00, 1, 2, 0xff, 0xd9},
		"hand/dht-empty":     append([]byte{0xff, 0xd8, 0xff, 0xc4, 0x00, 0x13, 0x00}, append(make([]byte, 16), 0xff, 0xd9)...),
		"hand/dht-too-many":  append([]byte{0xff, 0xd8, 0xff, 0xc4, 0x00, 0x13, 0x00, 255, 255}, append(make([]byte, 14), 0xff, 0xd9)...),
		"hand/dht-bad-class": append([]byte{0xff, 0xd8, 0xff, 0xc4, 0x00, 0x13, 0x20}, append(make([]byte, 16), 0xff, 0xd9)...),
	}
	for _, tail := range []string{"eoi", "sos", "nothing", "bad-dht", "jfif-eoi", "jfif-nothing", "jfif-sos"} {
		for _, nComp := range []int{1, 3, 4} {
			out[fmt.Sprintf("hand/header-%d-%s", nComp, tail)] = jpegHeaderOnly(16, 16, nComp, 0xc0, tail)
			out[fmt.Sprintf("hand/header-prog-%d-%s", nComp, tail)] = jpegHeaderOnly(9, 20, nComp, 0xc2, tail)
		}
	}
	// many scans of little data each
	out["hand/many-scans"] = jpegManyScans(40, 24, 300)
	out["hand/many-scans-wide"] = jpegManyScans(2048, 8, 40)
	// frame headers Go refuses, or takes in its own way
	frame := func(name string, payload []byte) {
		out["hand/sof-"+name] = jpegJoin([]jpegSegment{{marker: 0xc0, payload: payload},
			{marker: 0xda, payload: []byte{1, payload[6], 0, 0, 63, 0}}, {marker: 0xd9}})
	}
	frame("precision-12", []byte{12, 0, 8, 0, 8, 1, 1, 0x11, 0})
	frame("count-mismatch", []byte{8, 0, 8, 0, 8, 3, 1, 0x11, 0})
	frame("two-components", []byte{8, 0, 8, 0, 8, 2, 1, 0x11, 0, 2, 0x11, 0})
	frame("same-id", []byte{8, 0, 8, 0, 8, 3, 1, 0x11, 0, 1, 0x11, 0, 3, 0x11, 0})
	frame("bad-tq", []byte{8, 0, 8, 0, 8, 1, 1, 0x11, 4})
	frame("h0", []byte{8, 0, 8, 0, 8, 1, 1, 0x01, 0})
	frame("h3", []byte{8, 0, 8, 0, 8, 1, 1, 0x31, 0})
	frame("v5", []byte{8, 0, 8, 0, 8, 1, 1, 0x15, 0})
	frame("gray-4x4", []byte{8, 0, 8, 0, 8, 1, 1, 0x44, 0})
	frame("3-not-dividing", []byte{8, 0, 8, 0, 8, 3, 1, 0x41, 0, 2, 0x31, 0, 3, 0x11, 0})
	frame("3-4x4", []byte{8, 0, 8, 0, 8, 3, 1, 0x44, 0, 2, 0x11, 0, 3, 0x11, 0})
	frame("4-21", []byte{8, 0, 8, 0, 8, 4, 1, 0x21, 0, 2, 0x11, 0, 3, 0x11, 0, 4, 0x21, 0})
	frame("4-k-differs", []byte{8, 0, 8, 0, 8, 4, 1, 0x22, 0, 2, 0x11, 0, 3, 0x11, 0, 4, 0x11, 0})
	frame("4-chroma-22", []byte{8, 0, 8, 0, 8, 4, 1, 0x22, 0, 2, 0x22, 0, 3, 0x11, 0, 4, 0x22, 0})

	// Huffman tables no encoder writes, on images of one or two blocks.
	// More codes of a length than there are: the look-up table for 8 bits
	// is written over (the third code of one bit is "0" again) while the
	// bit-by-bit decoding of the last bits of a scan still finds the first.
	three := jpegTable{counts: []byte{3}, vals: []byte{0, 1, 2}}
	eob := jpegTable{counts: []byte{1}, vals: []byte{0}}
	out["hand/huff-oversubscribed"] = jpegTiny(8, 0xc0, three, eob, "0 11 0")
	out["hand/huff-oversubscribed-tail"] = jpegTiny(16, 0xc0, three, eob, "0 11 0  0 0")
	out["hand/huff-oversubscribed-2"] = jpegTiny(16, 0xc0, three, eob, "0 11 0  1 0 0")
	// 255 codes of 8 bits after a code of one bit: they wrap around the table
	wrap := jpegTable{counts: []byte{1, 0, 0, 0, 0, 0, 0, 255}}
	for i := 0; i < 256; i++ {
		wrap.vals = append(wrap.vals, byte(i%4))
	}
	out["hand/huff-wrapped"] = jpegTiny(8, 0xc0, wrap, eob, "00000101 10 0")
	// the entry for "01111111" is the one of the code of one bit still
	both := jpegTable{counts: []byte{2}, vals: []byte{0, 0}}
	out["hand/huff-wrapped-2"] = jpegTiny(24, 0xc0, wrap, both, "11111110 101 1  0 1  11111111 1")
	// only codes longer than the look-up table, and one of 16 bits
	long := jpegTable{counts: []byte{0, 0, 0, 0, 0, 0, 0, 0, 1, 1}, vals: []byte{1, 3}}
	eob16 := jpegTable{counts: make([]byte, 16), vals: []byte{0, 0x11}}
	eob16.counts[15] = 2
	out["hand/huff-long"] = jpegTiny(8, 0xc0, long, eob16, "000000000 1 0000000000000000")
	out["hand/huff-long-2"] = jpegTiny(16, 0xc1, long, eob16, "0000000010 101 0000000000000001 0 0000000000000000  000000000 0 0000000000000000")
	// bits that are no code, and a scan that ends inside a code
	out["hand/huff-no-code"] = jpegTiny(8, 0xc0, eob, eob, "1111111111111111 1111")
	out["hand/huff-cut"] = jpegTiny(8, 0xc0, long, eob16, "0000000")
	out["hand/huff-nothing"] = jpegTiny(8, 0xc0, three, eob, "")
	// a DC difference of 16 bits (Go takes it), and of 17
	sixteen := jpegTable{counts: []byte{2}, vals: []byte{16, 17}}
	out["hand/dc-16-bits"] = jpegTiny(8, 0xc0, sixteen, eob, "0 1000000000000001 0")
	out["hand/dc-16-bits-negative"] = jpegTiny(16, 0xc0, sixteen, eob, "0 0000000000000000 0  0 0111111111111111 0")
	out["hand/dc-17-bits"] = jpegTiny(8, 0xc0, sixteen, eob, "1 10000000000000001 0")
	// a table for the third slot before a baseline frame header says there
	// are two (Go takes it), and after it
	base := jpegTiny(8, 0xc0, three, eob, "0 11 0")
	extra := jpegJoin([]jpegSegment{{marker: 0xc4, payload: append([]byte{0x13, 1}, append(make([]byte, 15), 0)...)}})[2:]
	sofAt := bytes.Index(base, []byte{0xff, 0xc0})
	out["hand/dht-3-before-sof0"] = append(append(append([]byte(nil), base[:sofAt]...), extra...), base[sofAt:]...)
	sosAt := bytes.Index(base, []byte{0xff, 0xda})
	out["hand/dht-3-after-sof0"] = append(append(append([]byte(nil), base[:sosAt]...), extra...), base[sosAt:]...)

	// tables that are not there: a scan that needs one is refused, whatever
	// its data, and a scan that needs none is not
	noDHT := func(b []byte) []byte {
		var o []jpegSegment
		for _, x := range jpegSplit(b) {
			if x.marker != 0xc4 {
				o = append(o, x)
			}
		}
		return jpegJoin(o)
	}
	out["hand/huff-undefined"] = noDHT(jpegTiny(8, 0xc0, three, eob, "0000 0000 0000 0000"))
	onlyDC := jpegSplit(jpegTiny(8, 0xc0, three, eob, "1 0 0 0000 0000 0000"))
	onlyDC[2].payload = onlyDC[2].payload[:17+3]
	out["hand/huff-undefined-ac"] = jpegJoin(onlyDC)
	// progressive with the DC coefficient only, its first scan and two
	// refinements: no AC table is asked for
	prog := jpegSplit(jpegTiny(16, 0xc2, three, eob, "0 11  1 0"))
	prog[2].payload = prog[2].payload[:17+3]
	prog[3].payload = []byte{1, 1, 0x03, 0, 0, 0x02}
	one := jpegSegment{marker: 0xda, payload: []byte{1, 1, 0x33, 0, 0, 0x21}, data: []byte{0xbf}}
	two := jpegSegment{marker: 0xda, payload: []byte{1, 1, 0x00, 0, 0, 0x10}, data: []byte{0x7f}}
	out["hand/prog-dc-only"] = jpegJoin([]jpegSegment{prog[0], prog[1], prog[2], prog[3], one, two, prog[4]})
	// tables in the third slot, defined before the frame header, and a scan
	// that asks for them: refused in a baseline image, taken in an extended one
	for _, sof := range []byte{0xc0, 0xc1} {
		for _, sel := range []byte{0x22, 0x20, 0x02} {
			x := jpegSplit(jpegTiny(8, sof, three, eob, "0 11 0"))
			x[2].payload[0], x[2].payload[20] = sel>>4, 0x10|sel&0x0f
			x[3].payload[2] = sel
			out[fmt.Sprintf("hand/table-%02x-sof%d", sel, sof-0xc0)] = jpegJoin([]jpegSegment{x[0], x[2], x[1], x[3], x[4]})
		}
	}

	// segments that are longer than what they hold, or shorter: what is
	// left would pass for bytes between segments
	tiny := func() []jpegSegment { return jpegSplit(jpegTiny(8, 0xc0, three, eob, "0 11 0")) }
	x := tiny()
	x[0].payload = append(x[0].payload, 0, 0, 0)
	out["hand/dqt-3-more"] = jpegJoin(x)
	x = tiny()
	x[0].payload = append(x[0].payload, 0x01)
	out["hand/dqt-1-more"] = jpegJoin(x)
	x = tiny()
	out["hand/dri-4"] = jpegJoin([]jpegSegment{x[0], x[1], x[2], {marker: 0xdd, payload: []byte{0, 0, 0, 0}}, x[3], x[4]})
	out["hand/dri-0"] = jpegJoin([]jpegSegment{x[0], x[1], x[2], {marker: 0xdd, payload: []byte{0, 0}}, x[3], x[4]})
	x = tiny()
	out["hand/sof-twice"] = jpegJoin([]jpegSegment{x[0], x[1], x[1], x[2], x[3], x[4]})
	out["hand/sof-after-scan"] = jpegJoin([]jpegSegment{x[0], x[1], x[2], x[3], x[1], x[4]})
	x = tiny()
	x[2].payload[20+1] = 2 // two AC codes, one value: the other would be the byte after the segment
	x[2].data = []byte{0x00}
	out["hand/dht-value-missing"] = jpegJoin(x)
	x = tiny()
	x[2].payload = append(x[2].payload, 0x11, 0, 0) // the start of a third table
	out["hand/dht-cut"] = jpegJoin(x)

	// scan headers Go refuses, with data a decoder that took them could read
	x = tiny()
	x[3].payload = []byte{1, 9, 0, 0, 63, 0}
	out["hand/sos-unknown-component"] = jpegJoin(x)
	x[3].payload = []byte{1, 1, 0, 0, 63}
	out["hand/sos-5-bytes"] = jpegJoin(x)
	x[3].payload = []byte{1, 1, 0, 0, 63, 0, 0, 0}
	out["hand/sos-8-bytes-gray"] = jpegJoin(x)
	x[3].payload = []byte{0, 0, 63, 0}
	out["hand/sos-no-component"] = jpegJoin(x)
	// (with a DC table of one code, "0" for a difference of two bits)
	dc2 := jpegTable{counts: []byte{1}, vals: []byte{2}}
	one3 := "0 11 0"
	out["hand/3-one-scan"] = jpegSmall(0xc0, dc2, eob, jpegScan{[]byte{3, 1, 0, 2, 0, 3, 0, 0, 63, 0}, one3 + one3 + one3})
	out["hand/3-three-scans"] = jpegSmall(0xc0, dc2, eob, jpegScan{[]byte{1, 3, 0, 0, 63, 0}, one3},
		jpegScan{[]byte{1, 1, 0, 0, 63, 0}, one3}, jpegScan{[]byte{1, 2, 0, 0, 63, 0}, one3})
	out["hand/3-one-missing"] = jpegSmall(0xc0, dc2, eob, jpegScan{[]byte{2, 3, 0, 1, 0, 0, 63, 0}, one3 + one3})
	out["hand/sos-8-bytes-for-one"] = jpegSmall(0xc0, dc2, eob, jpegScan{[]byte{1, 1, 0, 0, 63, 0, 0, 0}, one3})
	out["hand/sos-12-bytes"] = jpegSmall(0xc0, dc2, eob, jpegScan{[]byte{3, 1, 0, 2, 0, 3, 0, 0, 63, 0, 0, 0}, one3 + one3 + one3})
	out["hand/sos-component-twice"] = jpegSmall(0xc0, dc2, eob, jpegScan{[]byte{2, 1, 0, 1, 0, 0, 63, 0}, one3 + one3})
	out["hand/sos-table-4"] = jpegSmall(0xc1, dc2, eob, jpegScan{[]byte{1, 1, 0x40, 0, 63, 0}, one3})
	// the same of a progressive image: a band that starts at the DC
	// coefficient and goes on, ends before it starts or after the block,
	// has AC coefficients of two components, or bits that do not follow on
	// the scan before
	out["hand/prog-0-63"] = jpegTiny(8, 0xc2, three, eob, one3)
	for name, band := range map[string][]byte{"5-3": {5, 3, 0}, "1-64": {1, 64, 0}, "0-0-ah2": {0, 0, 0x20}, "0-0-ah1-al1": {0, 0, 0x11}, "2-2-ah3-al1": {2, 2, 0x31}} {
		x = jpegSplit(jpegTiny(8, 0xc2, three, eob, "0 11 0"))
		x[3].payload = append([]byte{1, 1, 0}, band...)
		out["hand/prog-"+name] = jpegJoin(x)
	}
	out["hand/prog-ac-of-two"] = jpegSmall(0xc2, dc2, eob, jpegScan{[]byte{2, 1, 0, 2, 0, 1, 63, 0}, "0 0"})
	out["hand/prog-dc-of-three"] = jpegSmall(0xc2, dc2, eob, jpegScan{[]byte{3, 1, 0, 2, 0, 3, 0, 0, 0, 0x01}, "0 11 0 11 0 11"},
		jpegScan{[]byte{2, 3, 0, 1, 0, 0, 0, 0x10}, "1 0"}, jpegScan{[]byte{1, 2, 0, 5, 63, 0}, "0"})
	// refinements of a band of five coefficients: a new coefficient, then
	// the end of the band; a code for two bits; a run past the band
	four := jpegTable{counts: []byte{0, 4}, vals: []byte{0x00, 0x02, 0x51, 0x01}}
	// (and "again": a second refinement of the same bit, which corrects
	// the coefficient the first one made)
	for name, bits := range map[string]string{"ok": "11 1 00", "two-bits": "01 00", "run-past-band": "10 1 00", "two-new": "11 0 11 1 00", "again": "11 1 00 / 00 1 / 11 0 1 00 0"} {
		x = jpegSplit(jpegTiny(8, 0xc2, three, four, "00"))
		x[3].payload = []byte{1, 1, 0, 1, 5, 0x01}
		segs := []jpegSegment{x[0], x[1], x[2], x[3]}
		for _, scan := range strings.Split(bits, "/") {
			var bw jpegBitWriter
			for _, c := range scan {
				if c != ' ' {
					bw.put(uint32(c-'0'), 1)
				}
			}
			bw.pad()
			segs = append(segs, jpegSegment{marker: 0xda, payload: []byte{1, 1, 0, 1, 5, 0x10}, data: bw.out})
		}
		out["hand/refine-"+name] = jpegJoin(append(segs, x[4]))
	}
	// a component of 16 blocks an MCU: no scan may have it, not even alone
	rng := rand.New(rand.NewSource(21))
	out["hand/4x4-alone"] = jpegSynthFile(rng, &jpegSynthSpec{w: 40, h: 40, nComp: 3, sof: 0xc1, hv: [][2]int{{4, 4}, {1, 1}, {1, 1}}, plain: true})
	out["hand/4x4-alone-prog"] = jpegSynthFile(rng, &jpegSynthSpec{w: 40, h: 40, nComp: 3, sof: 0xc2, hv: [][2]int{{1, 1}, {4, 4}, {2, 2}}, plain: true})
	out["hand/4x4-gray"] = jpegSynthFile(rng, &jpegSynthSpec{w: 40, h: 40, nComp: 1, sof: 0xc0, hv: [][2]int{{4, 4}}, plain: true})
	return out
}

// A jpegScan is the payload of an SOS segment and the bits that follow it.
type jpegScan struct {
	payload []byte
	bits    string
}

// jpegSmall is an image of one block of three components (1, 2 and 3, not
// subsampled) with one table for the DC and one for the AC coefficients,
// and the scans given.
func jpegSmall(sof byte, dc, ac jpegTable, scans ...jpegScan) []byte {
	x := jpegSplit(jpegTiny(8, sof, dc, ac, ""))
	x[1].payload = []byte{8, 0, 8, 0, 8, 3, 1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0}
	segs := []jpegSegment{x[0], x[1], x[2]}
	for _, sc := range scans {
		var bw jpegBitWriter
		for _, c := range sc.bits {
			if c == '0' || c == '1' {
				bw.put(uint32(c-'0'), 1)
			}
		}
		bw.pad()
		segs = append(segs, jpegSegment{marker: 0xda, payload: sc.payload, data: bw.out})
	}
	return jpegJoin(append(segs, x[4]))
}

// A jpegTable is a Huffman table as a file states it: how many codes there
// are of 1, 2, ... bits, and their values.
type jpegTable struct {
	counts []byte
	vals   []byte
}

// jpegTiny is a gray image 8 pixels high of blocks of flat colour, with
// the tables given for the DC and the AC coefficients and the bits given as
// the data of its scan ('0' and '1'; the last byte is filled with ones).
func jpegTiny(w int, sof byte, dc, ac jpegTable, bits string) []byte {
	var bw jpegBitWriter
	for _, c := range bits {
		if c == '0' || c == '1' {
			bw.put(uint32(c-'0'), 1)
		}
	}
	bw.pad()
	dht := func(class byte, t jpegTable) []byte {
		p := append([]byte{class << 4}, append(append([]byte(nil), t.counts...), make([]byte, 16-len(t.counts))...)...)
		return append(p, t.vals...)
	}
	dqt := append([]byte{0}, bytes.Repeat([]byte{40}, 64)...)
	return jpegJoin([]jpegSegment{
		{marker: 0xdb, payload: dqt},
		{marker: sof, payload: []byte{8, 0, 8, 0, byte(w), 1, 1, 0x11, 0}},
		{marker: 0xc4, payload: append(dht(0, dc), dht(1, ac)...)},
		{marker: 0xda, payload: []byte{1, 1, 0, 0, 63, 0}, data: bw.out},
		{marker: 0xd9},
	})
}

// ---- A writer of synthetic files ------------------------------------------
//
// jpegSynthFile writes a JPEG file symbol by symbol: any frame Go's decoder
// takes (one, three or four components with any sampling factors it
// accepts, sequential or progressive), random Huffman tables (codes of up
// to 16 bits), random quantization tables of 8 or 16 bits, a random scan
// script (components together or apart, left out, scanned twice; spectral
// selection and successive approximation with end-of-band runs), restart
// intervals, and the segments that decide the colour model. The data of a
// scan is random symbols, written in the order Go's decoder reads them (its
// own for scans of one component and for restart intervals in them), with
// the coefficients the decoder holds tracked where the symbols depend on
// them (the refinement scans). So the files are images for Go whatever
// another decoder makes of them, and they reach what an encoder of
// pictures never writes.

type jpegBitWriter struct {
	out  []byte
	acc  uint32
	nacc uint
}

func (w *jpegBitWriter) put(v uint32, n uint) {
	for i := int(n) - 1; i >= 0; i-- {
		w.acc = w.acc<<1 | v>>uint(i)&1
		w.nacc++
		if w.nacc == 8 {
			w.out = append(w.out, byte(w.acc))
			if byte(w.acc) == 0xff {
				w.out = append(w.out, 0)
			}
			w.acc, w.nacc = 0, 0
		}
	}
}

// pad fills the last byte with ones, as an encoder does before a marker.
func (w *jpegBitWriter) pad() {
	for w.nacc != 0 {
		w.put(1, 1)
	}
}

type jpegHuffTable struct {
	counts [16]byte
	vals   []byte
	code   [256]uint32
	size   [256]uint
}

// jpegRandHuff makes a prefix code for the symbols: the leaves of a random
// binary tree, the codes assigned in the canonical order a decoder derives
// from the lengths.
func jpegRandHuff(rng *rand.Rand, syms []byte, deep bool) *jpegHuffTable {
	depths := []int{1, 1}
	if len(syms) == 1 {
		depths = []int{1 + rng.Intn(3)}
	}
	for len(depths) < len(syms) {
		i := rng.Intn(len(depths))
		if depths[i] >= 16 || (!deep && depths[i] >= 9 && rng.Intn(8) != 0) {
			continue
		}
		depths[i]++
		depths = append(depths, depths[i])
	}
	sort.Ints(depths)
	perm := rng.Perm(len(syms))
	h := &jpegHuffTable{}
	code, prev := uint32(0), depths[0]
	for i, d := range depths {
		code <<= uint(d - prev)
		prev = d
		s := syms[perm[i]]
		h.counts[d-1]++
		h.vals = append(h.vals, s)
		h.code[s], h.size[s] = code, uint(d)
		code++
	}
	return h
}

func (h *jpegHuffTable) segment(class, id int) []byte {
	out := append([]byte{byte(class<<4 | id)}, h.counts[:]...)
	return append(out, h.vals...)
}

// A jpegSynthSpec fixes what jpegSynthFile would otherwise choose.
type jpegSynthSpec struct {
	w, h  int
	nComp int
	sof   byte     // 0xc0, 0xc1, 0xc2
	hv    [][2]int // the sampling factors of the components
	ri    int      // the restart interval; -1 for none
	plain bool     // no oddities around the segments
}

type jpegSynth struct {
	rng         *rand.Rand
	w, h        int
	hv          [][2]int // as Go takes them: 1x1 for one component
	mxx, myy    int
	progressive bool
	ri          int
	coef        [][][64]int32 // the decoder's progCoeffs
	eobRun      uint16        // the decoder's
	bw          jpegBitWriter
	dc, ac      [4]*jpegHuffTable
	big         bool // large coefficients now and then
}

var jpegUnzig = [64]int{
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
}

func (s *jpegSynth) sym(h *jpegHuffTable, v byte) {
	if h.size[v] == 0 {
		panic(fmt.Sprintf("no code for symbol %#x", v))
	}
	s.bw.put(h.code[v], h.size[v])
}

// value writes t random bits and returns what receiveExtend makes of them.
func (s *jpegSynth) value(t uint) int32 {
	if t == 0 {
		return 0
	}
	x := uint32(s.rng.Intn(1 << t))
	s.bw.put(x, t)
	v, lim := int32(x), int32(1)<<t
	if v < lim>>1 {
		v += 1 - lim
	}
	return v
}

// size picks the number of bits of a coefficient: small as a rule.
func (s *jpegSynth) size(max int) uint {
	r := s.rng.Intn(100)
	n := 1 + s.rng.Intn(3)
	if r >= 70 {
		n = 1 + s.rng.Intn(6)
	}
	if s.big && r >= 95 {
		n = 1 + s.rng.Intn(max)
	}
	if n > max {
		n = max
	}
	return uint(n)
}

// eob writes an end-of-band symbol and returns the run it starts: this
// block and, in a progressive image as a rule, more.
func (s *jpegSynth) eob(h *jpegHuffTable) uint16 {
	n := uint(0)
	switch r := s.rng.Intn(100); {
	case r < 15 && s.progressive:
		n = 1 + uint(s.rng.Intn(3))
	case r < 18 && s.progressive:
		n = 1 + uint(s.rng.Intn(14))
	case r < 1:
		n = 1 + uint(s.rng.Intn(2))
	}
	s.sym(h, byte(n<<4))
	run := uint16(1) << n
	if n != 0 {
		bits := uint32(s.rng.Intn(1 << n))
		if n > 3 && s.rng.Intn(2) == 0 {
			bits = uint32(s.rng.Intn(4)) // not as long as it could be
		}
		s.bw.put(bits, n)
		run |= uint16(bits)
	}
	return run
}

// refineNonZeroes is the decoder's, with the bits it reads written.
func (s *jpegSynth) refineNonZeroes(b *[64]int32, zig, zigEnd, nz, delta int32) int32 {
	for ; zig <= zigEnd; zig++ {
		u := jpegUnzig[zig]
		if b[u] == 0 {
			if nz == 0 {
				break
			}
			nz--
			continue
		}
		bit := s.rng.Intn(2)
		s.bw.put(uint32(bit), 1)
		if bit == 0 {
			continue
		}
		if b[u] >= 0 {
			b[u] += delta
		} else {
			b[u] -= delta
		}
	}
	return zig
}

// refine writes a block of a refinement scan the way the decoder's refine
// reads it.
func (s *jpegSynth) refine(b *[64]int32, h *jpegHuffTable, zigStart, zigEnd, delta int32) {
	if zigStart == 0 {
		bit := s.rng.Intn(2)
		s.bw.put(uint32(bit), 1)
		if bit != 0 {
			b[0] |= delta
		}
		return
	}
	zig := zigStart
	if s.eobRun == 0 {
		for zig <= zigEnd {
			zeros := int32(0)
			for z := zig; z <= zigEnd; z++ {
				if b[jpegUnzig[z]] == 0 {
					zeros++
				}
			}
			r := s.rng.Intn(100)
			if zeros == 0 || r < 30 {
				s.eobRun = s.eob(h)
				break
			}
			if zeros >= 16 && r < 40 {
				// sixteen zeroes passed over
				s.sym(h, 0xf0)
				zig = s.refineNonZeroes(b, zig, zigEnd, 15, delta) + 1
				continue
			}
			run := int32(s.rng.Intn(int(min(zeros, 16))))
			if run > 3 && s.rng.Intn(2) == 0 {
				run = int32(s.rng.Intn(3))
			}
			z := delta
			s.sym(h, byte(run<<4|1))
			if s.rng.Intn(2) == 0 {
				s.bw.put(0, 1)
				z = -z
			} else {
				s.bw.put(1, 1)
			}
			zig = s.refineNonZeroes(b, zig, zigEnd, run, delta)
			b[jpegUnzig[zig]] = z
			zig++
		}
	}
	if s.eobRun > 0 {
		s.eobRun--
		s.refineNonZeroes(b, zig, zigEnd, -1, delta)
	}
}

// scan writes the data of a scan in the order of the decoder's processSOS.
func (s *jpegSynth) scan(comps []int, td, ta []int, zigStart, zigEnd int32, ah, al uint32) []byte {
	s.bw = jpegBitWriter{}
	var out []byte
	mcu, expectedRST := 0, byte(0xd0)
	var dc [4]int32
	blockCount := 0
	for my := 0; my < s.myy; my++ {
		for mx := 0; mx < s.mxx; mx++ {
			for i, ci := range comps {
				hi, vi := s.hv[ci][0], s.hv[ci][1]
				for j := 0; j < hi*vi; j++ {
					var bx, by int
					if len(comps) != 1 {
						bx, by = hi*mx+j%hi, vi*my+j/hi
					} else {
						q := s.mxx * hi
						bx, by = blockCount%q, blockCount/q
						blockCount++
						if bx*8 >= s.w || by*8 >= s.h {
							continue
						}
					}
					b := new([64]int32)
					if s.progressive {
						b = &s.coef[ci][by*s.mxx*hi+bx]
					}
					if ah != 0 {
						s.refine(b, s.ac[ta[i]], zigStart, zigEnd, 1<<al)
						continue
					}
					zig := zigStart
					if zig == 0 {
						zig++
						t := uint(s.rng.Intn(4))
						if r := s.rng.Intn(100); r >= 97 && s.big {
							t = uint(s.rng.Intn(17))
						} else if r >= 80 {
							t = uint(s.rng.Intn(8))
						}
						s.sym(s.dc[td[i]], byte(t))
						dc[ci] += s.value(t)
						b[0] = dc[ci] << al
					}
					if zig <= zigEnd && s.eobRun > 0 {
						s.eobRun--
						continue
					}
					h := s.ac[ta[i]]
					for zig <= zigEnd {
						r := s.rng.Intn(100)
						if r < 25 {
							s.eobRun = s.eob(h) - 1
							break
						}
						if r < 28 {
							// sixteen coefficients passed over, wherever the band ends
							s.sym(h, 0xf0)
							zig += 16
							continue
						}
						run := int32(s.rng.Intn(16))
						if r < 80 {
							run = int32(s.rng.Intn(3))
						}
						t := s.size(15)
						if zig+run > zigEnd {
							if r >= 90 {
								// a coefficient beyond the band ends the block, its bits unread
								s.sym(h, byte(run<<4)|byte(t))
								break
							}
							run = zigEnd - zig
						}
						s.sym(h, byte(run<<4)|byte(t))
						zig += run
						b[jpegUnzig[zig]] = s.value(t) << al
						zig++
					}
				}
			}
			mcu++
			if s.ri > 0 && mcu%s.ri == 0 && mcu < s.mxx*s.myy {
				s.bw.pad()
				out = append(append(out, s.bw.out...), 0xff, expectedRST)
				s.bw.out = nil
				expectedRST++
				if expectedRST == 0xd8 {
					expectedRST = 0xd0
				}
				dc = [4]int32{}
				s.eobRun = 0
			}
		}
	}
	s.bw.pad()
	return append(out, s.bw.out...)
}

// jpegAllSymbols are the symbols a table of a class needs: the bit counts
// of a DC difference, and every run and size of an AC coefficient.
func jpegAllSymbols(class int) []byte {
	var syms []byte
	if class == 0 {
		for i := 0; i <= 16; i++ {
			syms = append(syms, byte(i))
		}
		return syms
	}
	for i := 0; i < 256; i++ {
		syms = append(syms, byte(i))
	}
	return syms
}

type jpegSynthScan struct {
	comps          []int
	ss, se, ah, al int
}

func jpegSynthFile(rng *rand.Rand, spec *jpegSynthSpec) []byte {
	if spec == nil {
		spec = &jpegSynthSpec{}
	}
	pick := func(n int) bool { return !spec.plain && rng.Intn(n) == 0 }
	s := &jpegSynth{rng: rng, big: rng.Intn(4) == 0}

	// the frame
	s.w, s.h = 1+rng.Intn(40), 1+rng.Intn(40)
	if rng.Intn(5) == 0 {
		s.w, s.h = 1+rng.Intn(100), 1+rng.Intn(12)
	}
	if spec.w != 0 {
		s.w, s.h = spec.w, spec.h
	}
	nComp := []int{1, 3, 3, 3, 3, 4}[rng.Intn(6)]
	if spec.nComp != 0 {
		nComp = spec.nComp
	}
	sof := []byte{0xc0, 0xc1, 0xc2, 0xc2}[rng.Intn(4)]
	if spec.sof != 0 {
		sof = spec.sof
	}
	s.progressive = sof == 0xc2
	factors := []int{1, 1, 1, 2, 2, 4}
	declared := make([][2]int, nComp)
	for {
		for i := range declared {
			declared[i] = [2]int{factors[rng.Intn(6)], factors[rng.Intn(6)]}
		}
		switch nComp {
		case 3:
			switch rng.Intn(4) {
			case 0: // Cb and Cr alike, Y at least as much: what image.YCbCr has ratios for
				declared[2] = declared[1]
				declared[0] = [2]int{max(declared[0][0], declared[1][0]), max(declared[0][1], declared[1][1])}
			case 1:
				declared[1], declared[2] = [2]int{1, 1}, [2]int{1, 1}
			}
		case 4:
			declared[0], declared[1], declared[2], declared[3] = [2]int{1, 1}, [2]int{1, 1}, [2]int{1, 1}, [2]int{1, 1}
			if rng.Intn(2) == 0 {
				declared[0], declared[3] = [2]int{2, 2}, [2]int{2, 2}
			}
		}
		ok := true
		for _, d := range declared {
			// a component of 16 blocks an MCU fits no scan of Go's
			ok = ok && (nComp == 1 || d[0]*d[1] <= 10)
		}
		if ok {
			break
		}
	}
	if spec.hv != nil {
		declared = spec.hv
	}
	s.hv = make([][2]int, nComp)
	maxH, maxV := 1, 1
	for i, d := range declared {
		s.hv[i] = d
		if nComp == 1 {
			s.hv[i] = [2]int{1, 1}
		}
		maxH, maxV = max(maxH, s.hv[i][0]), max(maxV, s.hv[i][1])
	}
	s.mxx, s.myy = (s.w+8*maxH-1)/(8*maxH), (s.h+8*maxV-1)/(8*maxV)
	s.coef = make([][][64]int32, nComp)
	for i := range s.coef {
		s.coef[i] = make([][64]int32, s.mxx*s.myy*s.hv[i][0]*s.hv[i][1])
	}
	ids := []byte{1, 2, 3, 4}
	switch rng.Intn(8) {
	case 0:
		ids = []byte{'R', 'G', 'B', 'K'}
	case 1:
		ids = []byte{0, 1, 2, 3}
	case 2:
		p := rng.Perm(256)
		ids = []byte{byte(p[0]), byte(p[1]), byte(p[2]), byte(p[3])}
	}
	tq := make([]byte, nComp)
	for i := range tq {
		tq[i] = byte(rng.Intn(4))
		if rng.Intn(2) == 0 {
			tq[i] = byte(min(i, 1))
		}
	}

	var segs []jpegSegment
	add := func(marker byte, payload []byte) { segs = append(segs, jpegSegment{marker: marker, payload: payload}) }

	// the segments that decide the colour model
	if rng.Intn(3) == 0 {
		add(0xe0, []byte("JFIF\x00\x01\x02\x00\x00\x01\x00\x01\x00\x00"))
	}
	if nComp == 4 && rng.Intn(20) != 0 || nComp != 4 && rng.Intn(4) == 0 {
		add(0xee, jpegAdobe([]byte{0, 0, 1, 2, 2, 7}[rng.Intn(6)]).payload)
	}
	if pick(4) {
		add(0xfe, []byte("synthetic"))
	}
	if pick(6) {
		add(0xe1, append([]byte("Exif\x00\x00MM\x00\x2a"), make([]byte, rng.Intn(40))...))
	}

	// quantization tables: all four as a rule (one that is not defined is all
	// zeroes), as one segment or several, of 8 or 16 bits, before the frame
	// header or after it
	var dqt []byte
	var dqtSegs []jpegSegment
	missing := -1
	if pick(12) {
		missing = rng.Intn(4)
	}
	for i := 0; i < 4; i++ {
		if i == missing {
			continue
		}
		wide := rng.Intn(5) == 0
		scale := []int{1, 1, 2, 4, 16, 255}[rng.Intn(6)]
		if wide {
			dqt = append(dqt, byte(0x10|i))
		} else {
			dqt = append(dqt, byte(i))
		}
		for k := 0; k < 64; k++ {
			q := 1 + rng.Intn(scale)
			if wide {
				if rng.Intn(50) == 0 {
					q = rng.Intn(65536)
				}
				dqt = append(dqt, byte(q>>8), byte(q))
			} else {
				dqt = append(dqt, byte(q))
			}
		}
		if rng.Intn(2) == 0 || i == 3 {
			dqtSegs = append(dqtSegs, jpegSegment{marker: 0xdb, payload: dqt})
			dqt = nil
		}
	}
	if len(dqt) > 0 {
		dqtSegs = append(dqtSegs, jpegSegment{marker: 0xdb, payload: dqt})
	}
	dqtLate := pick(4)
	if !dqtLate {
		segs = append(segs, dqtSegs...)
	}

	sofPayload := []byte{8, byte(s.h >> 8), byte(s.h), byte(s.w >> 8), byte(s.w), byte(nComp)}
	for i := 0; i < nComp; i++ {
		sofPayload = append(sofPayload, ids[i], byte(declared[i][0]<<4|declared[i][1]), tq[i])
	}
	add(sof, sofPayload)
	if dqtLate {
		segs = append(segs, dqtSegs...)
	}

	s.ri = 0
	if rng.Intn(3) == 0 {
		s.ri = 1 + rng.Intn(2*s.mxx+2)
	}
	if spec.ri > 0 {
		s.ri = spec.ri
	} else if spec.ri < 0 {
		s.ri = 0
	}
	if s.ri > 0 {
		add(0xdd, []byte{byte(s.ri >> 8), byte(s.ri)})
	}

	// the scan script
	var scans []jpegSynthScan
	groups := func() [][]int {
		// the components in scans of one or more, of 10 blocks an MCU at most
		order := rng.Perm(nComp)
		var out [][]int
		for len(order) > 0 {
			n := 1 + rng.Intn(len(order))
			if rng.Intn(2) == 0 {
				n = len(order)
			}
			for {
				total := 0
				for _, c := range order[:n] {
					total += s.hv[c][0] * s.hv[c][1]
				}
				if total <= 10 || n == 1 {
					break
				}
				n--
			}
			g := append([]int(nil), order[:n]...)
			if rng.Intn(4) != 0 {
				sort.Ints(g)
			}
			out = append(out, g)
			order = order[n:]
		}
		return out
	}
	if !s.progressive {
		for _, g := range groups() {
			scans = append(scans, jpegSynthScan{comps: g, se: 63})
		}
		if pick(10) {
			// a component scanned again
			scans = append(scans, jpegSynthScan{comps: []int{rng.Intn(nComp)}, se: 63})
		}
		if pick(10) && len(scans) > 1 {
			// a component left out
			scans = scans[:len(scans)-1]
		}
	} else {
		// DC: a first scan and its refinements
		al := rng.Intn(3)
		for _, g := range groups() {
			scans = append(scans, jpegSynthScan{comps: g, al: al})
		}
		var later []jpegSynthScan
		for a := al - 1; a >= 0; a-- {
			for _, g := range groups() {
				later = append(later, jpegSynthScan{comps: g, ah: a + 1, al: a})
			}
		}
		// AC: bands of every component, each with its refinements
		perComp := make([][]jpegSynthScan, nComp)
		for c := 0; c < nComp; c++ {
			ss := 1
			for ss <= 63 {
				se := min(63, ss+rng.Intn(64))
				if rng.Intn(3) == 0 {
					se = 63
				}
				a := rng.Intn(3)
				if rng.Intn(10) == 0 {
					a = rng.Intn(14)
				}
				for ah := 0; a >= 0; a-- {
					perComp[c] = append(perComp[c], jpegSynthScan{comps: []int{c}, ss: ss, se: se, ah: ah, al: a})
					ah = a
					if pick(12) {
						break // the last bits never come
					}
				}
				ss = se + 1
				if pick(15) {
					break // nor the last bands
				}
			}
		}
		if pick(12) {
			perComp[rng.Intn(nComp)] = nil
		}
		if pick(15) && nComp > 1 {
			// a component without any scan
			gone := rng.Intn(nComp)
			perComp[gone] = nil
			keep := func(in []jpegSynthScan) []jpegSynthScan {
				var o []jpegSynthScan
				for _, sc := range in {
					var c []int
					for _, x := range sc.comps {
						if x != gone {
							c = append(c, x)
						}
					}
					if sc.comps = c; len(c) > 0 {
						o = append(o, sc)
					}
				}
				return o
			}
			scans, later = keep(scans), keep(later)
		}
		// interleave the lists, each in its order
		lists := append([][]jpegSynthScan{later}, perComp...)
		for {
			var live []int
			for i, l := range lists {
				if len(l) > 0 {
					live = append(live, i)
				}
			}
			if len(live) == 0 {
				break
			}
			i := live[rng.Intn(len(live))]
			if rng.Intn(3) != 0 {
				i = live[0]
			}
			scans = append(scans, lists[i][0])
			lists[i] = lists[i][1:]
		}
		if pick(15) {
			// a scan twice
			i := rng.Intn(len(scans))
			scans = append(scans, scans[i])
		}
	}

	maxTable := 4
	if sof == 0xc0 {
		maxTable = 2
	}
	var body []byte // what follows the segments so far: scans with their data, tables in between
	flush := func() {
		body = append(body, jpegJoin(segs)[2:]...)
		segs = nil
	}
	for si, sc := range scans {
		// tables: new ones for the first scan, and now and then later
		var dht []byte
		for id := 0; id < maxTable; id++ {
			if s.dc[id] == nil || rng.Intn(6) == 0 {
				s.dc[id] = jpegRandHuff(rng, jpegAllSymbols(0), rng.Intn(3) == 0)
				dht = append(dht, s.dc[id].segment(0, id)...)
			}
			if s.ac[id] == nil || rng.Intn(6) == 0 {
				s.ac[id] = jpegRandHuff(rng, jpegAllSymbols(1), rng.Intn(3) == 0)
				dht = append(dht, s.ac[id].segment(1, id)...)
			}
			if len(dht) > 0 && rng.Intn(2) == 0 {
				add(0xc4, dht)
				dht = nil
			}
		}
		if len(dht) > 0 {
			add(0xc4, dht)
		}
		if si > 0 && pick(12) {
			// another restart interval from here on
			s.ri = rng.Intn(4)
			add(0xdd, []byte{byte(s.ri >> 8), byte(s.ri)})
		}
		if si > 0 && pick(12) {
			add(0xfe, []byte("between scans"))
		}
		td, ta := make([]int, len(sc.comps)), make([]int, len(sc.comps))
		p := []byte{byte(len(sc.comps))}
		for i, c := range sc.comps {
			td[i], ta[i] = rng.Intn(maxTable), rng.Intn(maxTable)
			p = append(p, ids[c], byte(td[i]<<4|ta[i]))
		}
		if s.progressive {
			p = append(p, byte(sc.ss), byte(sc.se), byte(sc.ah<<4|sc.al))
		} else {
			// what a sequential scan says here is not looked at
			p = append(p, 0, 63, 0)
			if pick(8) {
				p[len(p)-3], p[len(p)-2], p[len(p)-1] = byte(rng.Intn(256)), byte(rng.Intn(256)), byte(rng.Intn(256))
			}
		}
		add(0xda, p)
		segs[len(segs)-1].data = s.scan(sc.comps, td, ta, int32(sc.ss), int32(sc.se), uint32(sc.ah), uint32(sc.al))
		if pick(15) {
			// what a decoder passes over after the data of a scan
			tail := [][]byte{{0xff, 0xd0 + byte(rng.Intn(8))}, {0xff, 0x00}, {0x00}, {0x55, 0xaa, 0x01}, {0xff, 0xff}}[rng.Intn(5)]
			segs[len(segs)-1].data = append(segs[len(segs)-1].data, tail...)
		}
		flush()
	}
	if pick(10) {
		add(0xee, jpegAdobe(byte(rng.Intn(3))).payload)
	}
	if pick(8) {
		// a quantization table after the scans: a progressive image is
		// dequantized with it, a sequential one was dequantized before
		q := []byte{byte(rng.Intn(4))}
		for k := 0; k < 64; k++ {
			q = append(q, byte(1+rng.Intn(3)))
		}
		add(0xdb, q)
	}
	add(0xd9, nil)
	flush()
	return append([]byte{0xff, 0xd8}, body...)
}

// jpegSynthetic are n synthetic files of a seed.
func jpegSynthetic(seed int64, n int) map[string][]byte {
	out := map[string][]byte{}
	rng := rand.New(rand.NewSource(seed))
	for i := 0; i < n; i++ {
		out[fmt.Sprintf("synth/%d-%04d", seed, i)] = jpegSynthFile(rng, nil)
	}
	return out
}
