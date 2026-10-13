package cbdf

import (
	"encoding/binary"
	"fmt"
	"math/rand"
	randv2 "math/rand/v2"
	"os"
	"path/filepath"
	"testing"

	"github.com/shibukawa/bdf/image/imgconv"
)

// jpegSegments is the start of a JPEG file: the marker of the start of the
// image and the segments given, each a marker and what it holds.
func jpegSegments(segs ...[]byte) []byte {
	b := []byte{0xff, 0xd8}
	for _, s := range segs {
		b = append(b, 0xff, s[0])
		b = binary.BigEndian.AppendUint16(b, uint16(len(s)+1))
		b = append(b, s[1:]...)
	}
	return b
}

// TestSniffAndOrientationMatchReference compares what libbdf takes image
// bytes for, and how it turns them, with the reference: on the images of
// the tests, on EXIF blocks of every kind in JPEG and PNG files, and on
// damaged copies of all of them.
func TestSniffAndOrientationMatchReference(t *testing.T) {
	in := files([]string{goroot() + "/image", ximage(t) + "/testdata"})
	for _, format := range []string{"png", "jpeg", "gif", "bmp", "webp", "avif", "svg", ""} {
		for k, v := range documentImages(t, format) {
			in[k] = v
		}
	}
	png := in[goroot()+"/image/png/testdata/pngsuite/basn2c08.png"]
	if len(png) < 33 {
		t.Fatal("a file of Go's testdata is missing")
	}
	n := 0
	for o := 0; o <= 9; o++ {
		for _, little := range []bool{false, true} {
			e := exif(o, little)
			with := "Exif\x00\x00" + string(e)
			in["png "+string(rune('0'+o))+with[6:8]] = append(append(append([]byte(nil), png[:33]...), pngChunk("eXIf", e)...), png[33:]...)
			// an eXIf chunk that begins as the segment of a JPEG file does
			in["png prefixed "+string(rune('0'+o))+with[6:8]] = append(append(append([]byte(nil), png[:33]...), pngChunk("eXIf", []byte(with))...), png[33:]...)
			in["jpeg "+string(rune('0'+o))+with[6:8]] = jpegSegments(append([]byte{0xe0}, "JFIF\x00"...), append([]byte{0xe1}, with...), []byte{0xda, 0, 1})
			// behind the scan it is not looked for
			in["jpeg late "+string(rune('0'+o))+with[6:8]] = jpegSegments([]byte{0xda, 0, 1}, append([]byte{0xe1}, with...))
			in["jpeg other "+string(rune('0'+o))+with[6:8]] = jpegSegments(append([]byte{0xe1}, "http://ns.adobe.com/xap/1.0/\x00"...), append([]byte{0xe1}, with...))
			n += 5
		}
	}
	turned := 0
	check := func(name string, data []byte) {
		if got, want := Sniff(data), imgconv.Sniff(data); got != want {
			t.Fatalf("%s: libbdf takes it for %q, the reference for %q", name, got, want)
		}
		got, want := Orientation(data), imgconv.Orientation(data)
		if got != want {
			t.Fatalf("%s: orientation %d, the reference has %d\n%x", name, got, want, data[:min(len(data), 200)])
		}
		if want != 1 {
			turned++
		}
	}
	var names []string
	for name, data := range in {
		check(name, data)
		names = append(names, name)
	}
	// a map has no order: the same damage every run
	for i := 1; i < len(names); i++ {
		for j := i; j > 0 && names[j] < names[j-1]; j-- {
			names[j], names[j-1] = names[j-1], names[j]
		}
	}
	rng := rand.New(rand.NewSource(1))
	cases := envInt("LIBBDF_IMAGE_CASES", 2000) * 10
	for i := 0; i < cases; i++ {
		name := names[rng.Intn(len(names))]
		data := in[name]
		// the first bytes are where the format and the EXIF block are
		head := append([]byte(nil), data[:min(len(data), 400)]...)
		check(name+" (damaged)", append(mutate(rng, head), data[len(head):]...))
	}
	if turned < 100 {
		t.Errorf("only %d images are turned", turned)
	}
	t.Logf("%d files, %d of them made here, and %d damaged copies; %d are turned", len(in), n, cases, turned)
}

// TestWriteImageSeeds writes the seeds of the libFuzzer target image_fuzz
// (a byte of options, then an image part) when LIBBDF_IMAGE_SEEDS names a
// directory:
//
//	LIBBDF_IMAGE_SEEDS=$PWD/fuzz/corpus/image go test -C go -run TestWriteImageSeeds .
func TestWriteImageSeeds(t *testing.T) {
	dir := os.Getenv("LIBBDF_IMAGE_SEEDS")
	if dir == "" {
		t.Skip("LIBBDF_IMAGE_SEEDS is not set")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	n := 0
	write := func(data []byte) {
		if len(data) > 1<<15 {
			return // a fuzzer does more with small inputs
		}
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("seed-%03d", n)), append([]byte{byte(n * 37)}, data...), 0o644); err != nil {
			t.Fatal(err)
		}
		n++
	}
	for _, format := range []string{"png", "jpeg", "gif", "bmp", "webp"} {
		for _, data := range documentImages(t, format) {
			write(data)
		}
	}
	r := randv2.New(randv2.NewPCG(3, 4))
	for range 60 {
		write(RandomImage(r))
	}
	t.Logf("%d seeds in %s", n, dir)
}
