package cbdf

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"testing"
)

// webpFiles are the files the decoders are compared on: the image parts of
// the reference's test documents, the test data of golang.org/x/image, what
// gen.sh made with cwebp, the inputs a fuzzer found, and files made by code
// (webp_synth_test.go), most of them right and some wrong.
func webpFiles(t testing.TB) map[string][]byte {
	in := withDocumentImages(t, "webp", []string{ximage(t) + "/testdata", "testdata/images"}, ".webp")
	for name, data := range webpSynthetics(1, 150, 0) {
		in[name] = data
	}
	for name, data := range webpSynthetics(2, 50, 10) {
		in[name] = data
	}
	return in
}

// webpCompare is compare for the tests of this file. With LIBBDF_IMAGE_WRITE set
// to a directory it also writes the input there, under a name made of its
// hash: that is how fuzz/corpus/webp is seeded, and how the inputs of the
// tests are run through the C decoder alone (work/cover.sh).
func webpCompare(data []byte) string {
	webpWrite(data)
	return compare("webp", data)
}

func webpWrite(data []byte) {
	if dir := os.Getenv("LIBBDF_IMAGE_WRITE"); dir != "" {
		sum := sha256.Sum256(data)
		os.MkdirAll(dir, 0o755)
		os.WriteFile(filepath.Join(dir, fmt.Sprintf("%x", sum[:10])), data, 0o644)
	}
}

func TestWebpFiles(t *testing.T) {
	in := webpFiles(t)
	for _, data := range in {
		webpWrite(data)
	}
	checkFiles(t, "webp", in)
}

func TestWebpMutations(t *testing.T) {
	checkMutations(t, "webp", webpFiles(t))
}

func FuzzWebp(f *testing.F) {
	fuzzDecode(f, "webp", webpFiles(f))
}

// TestWebpSynthetic compares the decoders on files made by code:
// LIBBDF_IMAGE_SYNTH of them without a fault and as many with faults, from the
// seed LIBBDF_IMAGE_SEED.
func TestWebpSynthetic(t *testing.T) {
	n, seed := envInt("LIBBDF_IMAGE_SYNTH", 500), int64(envInt("LIBBDF_IMAGE_SEED", 1))
	for _, bad := range []int{0, 12, 3} {
		in := webpSynthetics(seed+100, n, bad)
		decoded, failed := 0, 0
		for name, data := range in {
			if _, _, _, s := reference("webp", data); s == statusOK {
				decoded++
			}
			if d := webpCompare(data); d != "" {
				failed++
				if failed <= 10 {
					t.Errorf("%s: %s", name, d)
				}
				if dir := os.Getenv("LIBBDF_IMAGE_DUMP"); dir != "" {
					os.MkdirAll(dir, 0o755)
					os.WriteFile(filepath.Join(dir, name+".webp"), data, 0o644)
				}
			}
		}
		t.Logf("faults 1 in %d: %d files, %d of them images the reference decodes, %d disagreements", bad, len(in), decoded, failed)
		if bad == 0 && decoded < len(in)*9/10 {
			t.Errorf("only %d of %d files without a fault decode: the generator is wrong", decoded, len(in))
		}
	}
}

// webpWant compares the decoders on a file and checks that the reference
// answers what the test expects of it: the test would be worth nothing if
// the file were refused for another reason than the one it is made for.
func webpWant(t *testing.T, name string, data []byte, config bool, status int) {
	t.Helper()
	if d := webpCompare(data); d != "" {
		t.Errorf("%s: %s", name, d)
	}
	_, _, ok := refConfig("webp", data)
	_, _, _, s := reference("webp", data)
	if ok != config || s != status {
		t.Errorf("%s: the reference reads a size: %v, decodes with status %d; the test expects %v and %d", name, ok, s, config, status)
	}
}

// TestWebpContainers compares the decoders on containers that are right
// and wrong in known ways.
func TestWebpContainers(t *testing.T) {
	// a lossy frame and a lossless image of the same size that decode
	rng := rand.New(rand.NewSource(7))
	const w, h = 21, 19
	var frame, lossless []byte
	for {
		frame = webpVP8Frame(rng, w, h, false)
		if _, _, _, s := reference("webp", webpRIFF(webpChunk("VP8 ", frame))); s == statusOK {
			break
		}
	}
	for {
		lossless = append(webpVP8LHeader(w, h, true, 0), webpVP8LBody(rng, w, h, 0)...)
		if _, _, _, s := reference("webp", webpRIFF(webpChunk("VP8L", lossless))); s == statusOK {
			break
		}
	}
	vp8, vp8l := webpChunk("VP8 ", frame), webpChunk("VP8L", lossless)
	rawAlpha := func(filter byte) []byte {
		return webpChunk("ALPH", append([]byte{filter << 2}, webpRandBytes(rng, w*h)...))
	}
	green := make([]byte, w*h)
	for i := range green {
		green[i] = byte(i * 7)
	}
	packedAlpha := func(filter byte) []byte {
		return webpChunk("ALPH", append([]byte{filter<<2 | 1}, webpGreenBody(rng, w, h, green)...))
	}
	alphaX, plainX := webpVP8X(webpAlphaFlag, w, h), webpVP8X(0, w, h)
	resize := func(b []byte, delta int) []byte {
		b = append([]byte(nil), b...)
		binary.LittleEndian.PutUint32(b[4:], uint32(int(binary.LittleEndian.Uint32(b[4:]))+delta))
		return b
	}
	frameOf := func(fw, fh int, key bool) []byte {
		b := []byte{0, 0, 0, 0x9d, 0x01, 0x2a, byte(fw), byte(fw >> 8), byte(fh), byte(fh >> 8)}
		if !key {
			b[0] = 1
		}
		return webpChunk("VP8 ", b)
	}
	ok, bad, large := statusOK, statusFormat, statusTooLarge

	webpWant(t, "lossy", webpRIFF(vp8), true, ok)
	webpWant(t, "lossless", webpRIFF(vp8l), true, ok)
	webpWant(t, "lossy with VP8X", webpRIFF(plainX, vp8), true, ok)
	webpWant(t, "lossless with VP8X", webpRIFF(plainX, vp8l), true, ok)
	webpWant(t, "lossless with VP8X that says alpha", webpRIFF(alphaX, vp8l), true, ok)
	for f := byte(0); f < 4; f++ {
		webpWant(t, fmt.Sprintf("raw alpha, filter %d", f), webpRIFF(alphaX, rawAlpha(f), vp8), true, ok)
		webpWant(t, fmt.Sprintf("packed alpha, filter %d", f), webpRIFF(alphaX, packedAlpha(f), vp8), true, ok)
	}
	webpWant(t, "alpha of a compression 2", webpRIFF(alphaX, webpChunk("ALPH", append([]byte{2}, green...)), vp8), true, bad)
	webpWant(t, "alpha of a compression 3", webpRIFF(alphaX, webpChunk("ALPH", append([]byte{3}, green...)), vp8), true, bad)
	webpWant(t, "raw alpha one byte short", webpRIFF(alphaX, webpChunk("ALPH", make([]byte, w*h)), vp8), true, bad)
	webpWant(t, "raw alpha with bytes to spare", webpRIFF(alphaX, webpChunk("ALPH", make([]byte, w*h+9)), vp8), true, ok)
	webpWant(t, "an empty ALPH chunk", webpRIFF(alphaX, webpChunk("ALPH", nil), vp8), true, bad)
	webpWant(t, "alpha announced and missing", webpRIFF(alphaX, vp8), true, bad)
	webpWant(t, "alpha not announced", webpRIFF(plainX, rawAlpha(0), vp8), true, bad)
	webpWant(t, "alpha before VP8X", webpRIFF(rawAlpha(0), alphaX, vp8), false, bad)
	webpWant(t, "alpha twice", webpRIFF(alphaX, rawAlpha(0), rawAlpha(1), vp8), true, bad)
	webpWant(t, "alpha and a lossless image", webpRIFF(alphaX, rawAlpha(0), vp8l), true, bad)
	webpWant(t, "alpha and no image", webpRIFF(alphaX, rawAlpha(0)), true, bad)
	webpWant(t, "VP8X twice", webpRIFF(plainX, plainX, vp8), true, bad)
	webpWant(t, "VP8X of another width (lossy)", webpRIFF(webpVP8X(0, w+1, h), vp8), true, bad)
	webpWant(t, "VP8X of another height (lossy)", webpRIFF(webpVP8X(0, w, h-1), vp8), true, bad)
	webpWant(t, "VP8X of another width (lossless)", webpRIFF(webpVP8X(0, w-1, h), vp8l), true, bad)
	webpWant(t, "VP8X of another height (lossless)", webpRIFF(webpVP8X(0, w, h+1), vp8l), true, bad)
	webpWant(t, "VP8X of 9 bytes", webpRIFF(webpChunk("VP8X", make([]byte, 9)), vp8), false, bad)
	webpWant(t, "VP8X of 11 bytes", webpRIFF(webpChunk("VP8X", make([]byte, 11)), vp8), false, bad)
	webpWant(t, "VP8X alone", webpRIFF(plainX), true, bad)
	webpWant(t, "VP8X of an animation", webpRIFF(webpVP8X(2, w, h), webpChunk("ANIM", make([]byte, 6)), webpChunk("ANMF", append(make([]byte, 16), vp8...))), true, bad)
	webpWant(t, "VP8X of 1<<24 x 127 pixels", webpRIFF(webpVP8X(0, 1<<24, 127), vp8), true, large)
	webpWant(t, "VP8X of 1<<24 x 128 pixels", webpRIFF(webpVP8X(0, 1<<24, 128), vp8), false, bad)
	webpWant(t, "VP8X of 1<<16 x 1<<16 pixels", webpRIFF(webpVP8X(0, 1<<16, 1<<16), vp8), false, bad)
	webpWant(t, "VP8X of 10240 x 10240 pixels", webpRIFF(webpVP8X(0, 10240, 10240), vp8), true, bad)
	webpWant(t, "VP8X of 10241 x 10240 pixels", webpRIFF(webpVP8X(0, 10241, 10240), vp8), true, large)
	webpWant(t, "raw alpha of 20000 x 5000 pixels", webpRIFF(webpVP8X(webpAlphaFlag, 20000, 5000), rawAlpha(0), vp8), true, bad)
	webpWant(t, "packed alpha of 20000 x 5000 pixels", webpRIFF(webpVP8X(webpAlphaFlag, 20000, 5000), packedAlpha(0), vp8), true, bad)
	webpWant(t, "packed alpha of 16384 x 6400 pixels", webpRIFF(webpVP8X(webpAlphaFlag, 16384, 6400), webpChunk("ALPH", []byte{1}), vp8), true, bad)
	webpWant(t, "packed alpha of 16385 x 2 pixels", webpRIFF(webpVP8X(webpAlphaFlag, 16385, 2), packedAlpha(0), vp8), true, bad)

	webpWant(t, "a frame that is not a key frame", webpRIFF(frameOf(w, h, false)), true, large)
	webpWant(t, "a frame that is not a key frame, with VP8X", webpRIFF(plainX, frameOf(w, h, false)), true, bad)
	webpWant(t, "a frame of no width", webpRIFF(frameOf(0, h, true)), true, large)
	webpWant(t, "a frame of no height", webpRIFF(frameOf(w, 0, true)), true, large)
	webpWant(t, "a frame of 16383 x 16383 pixels", webpRIFF(frameOf(16383, 16383, true)), true, large)
	webpWant(t, "a frame of 10240 x 10240 pixels", webpRIFF(frameOf(10240, 10240, true)), true, bad)
	webpWant(t, "a frame of 10241 x 10240 pixels", webpRIFF(frameOf(10241, 10240, true)), true, large)
	webpWant(t, "a frame without its sync code", webpRIFF(webpChunk("VP8 ", []byte{0, 0, 0, 0x9d, 0x01, 0x2b, w, 0, h, 0})), false, bad)
	webpWant(t, "a frame of 9 bytes", webpRIFF(webpChunk("VP8 ", frame[:9])), false, bad)
	webpWant(t, "a frame of 2 bytes", webpRIFF(webpChunk("VP8 ", frame[:2])), false, bad)
	webpWant(t, "a frame of its header", webpRIFF(webpChunk("VP8 ", frame[:10])), true, bad)
	webpWant(t, "a lossless image of 16384 x 16384 pixels", webpRIFF(webpChunk("VP8L", webpVP8LHeader(16384, 16384, false, 0))), true, large)
	webpWant(t, "a lossless image of 10240 x 10240 pixels", webpRIFF(webpChunk("VP8L", webpVP8LHeader(10240, 10240, false, 0))), true, bad)
	webpWant(t, "a lossless image of 10241 x 10240 pixels", webpRIFF(webpChunk("VP8L", webpVP8LHeader(10241, 10240, false, 0))), true, large)
	webpWant(t, "a lossless image of version 1", webpRIFF(webpChunk("VP8L", append(webpVP8LHeader(w, h, false, 1), lossless[5:]...))), false, bad)
	webpWant(t, "a lossless image of another magic number", webpRIFF(webpChunk("VP8L", append([]byte{0x2e}, lossless[1:]...))), false, bad)
	webpWant(t, "a lossless image of 4 bytes", webpRIFF(webpChunk("VP8L", lossless[:4])), false, bad)
	webpWant(t, "a lossless image of 4 bytes, with VP8X", webpRIFF(plainX, webpChunk("VP8L", lossless[:4])), true, bad)
	webpWant(t, "a lossless image of its header", webpRIFF(webpChunk("VP8L", lossless[:5])), true, bad)

	webpWant(t, "nothing", nil, false, bad)
	webpWant(t, "7 bytes", []byte("RIFF\x04\x00\x00"), false, bad)
	webpWant(t, "a RIFF chunk of 3 bytes", []byte("RIFF\x03\x00\x00\x00WEBP"), false, bad)
	webpWant(t, "a RIFF chunk without its form", []byte("RIFF\x04\x00\x00\x00WEB"), false, bad)
	webpWant(t, "a RIFF chunk without chunks", []byte("RIFF\x04\x00\x00\x00WEBP"), false, bad)
	webpWant(t, "not RIFF", append([]byte("RIFX"), webpRIFF(vp8)[4:]...), false, bad)
	webpWant(t, "not WEBP", bytes.Replace(webpRIFF(vp8), []byte("WEBP"), []byte("WAVE"), 1), false, bad)
	webpWant(t, "a RIFF length 1 short", resize(webpRIFF(vp8), -1), false, bad)
	webpWant(t, "a RIFF length that ends in the chunk header", resize(webpRIFF(vp8), -len(vp8)+5), false, bad)
	webpWant(t, "a RIFF length that ends before the chunk", resize(webpRIFF(vp8), -len(vp8)), false, bad)
	webpWant(t, "a RIFF length 1 long", resize(webpRIFF(vp8), 1), true, ok)
	webpWant(t, "a RIFF length of 4 GiB", resize(webpRIFF(vp8l), -len(vp8l)-5), true, ok)
	webpWant(t, "bytes after the image", append(webpRIFF(vp8), 1, 2, 3), true, ok)
	for _, n := range []int{0, 1, 2, 7, 8} {
		junk := webpChunk("JUNK", make([]byte, n))
		webpWant(t, fmt.Sprintf("a chunk of %d bytes to skip", n), webpRIFF(junk, vp8), true, ok)
		webpWant(t, fmt.Sprintf("a chunk of %d bytes to skip, lossless", n), webpRIFF(junk, junk, vp8l), true, ok)
	}
	odd := webpChunk("JUNK", make([]byte, 3))
	webpWant(t, "an odd chunk without its padding", webpRIFF(odd[:len(odd)-1], vp8), false, bad)
	webpWant(t, "an odd chunk at the end of the RIFF chunk", resize(webpRIFF(odd), -1), false, bad)
	webpWant(t, "an odd chunk at the end of the file", webpRIFF(odd)[:12+8+3], false, bad)
	webpWant(t, "a chunk that is cut", webpRIFF(odd)[:12+8+2], false, bad)
	webpWant(t, "a chunk header that is cut", webpRIFF(odd, vp8)[:12+len(odd)+5], false, bad)
	webpWant(t, "a chunk longer than the RIFF chunk", webpRIFF(resize(odd, 100), vp8), false, bad)
	webpWant(t, "a lossy chunk longer than the file", resize(webpRIFF(resize(vp8, 4)), 4), true, bad)
	webpWant(t, "a lossy chunk of 2 GiB", resize(webpRIFF(resize(vp8, 1<<31-len(frame))), 1<<31), false, bad)
	webpWant(t, "a lossless chunk longer than the file", resize(webpRIFF(resize(vp8l, 4)), 4), true, ok)
	webpWant(t, "a lossy chunk of its frame's header", webpRIFF(resize(vp8, 10-len(frame))), true, bad)
	webpWant(t, "a lossy chunk one byte shorter than its frame", webpRIFF(resize(vp8, -1)), true, reference2(webpRIFF(resize(vp8, -1))))
	// Go refuses a last partition of 16 MiB by the length the chunk states, before it reads it
	webpWant(t, "a lossy chunk of 16 MiB that is not there", resize(webpRIFF(resize(vp8, 1<<24)), 1<<24), true, bad)
	webpWant(t, "a lossy chunk of 15 MiB that is not there", resize(webpRIFF(resize(vp8, 15<<20)), 15<<20), true, bad)
	// Go refuses a last partition of 16 MiB or more, also when the file has it.
	var one []byte
	for {
		hd := webpRandVP8Head(rng)
		hd.log2nop, hd.tokens = 0, 1
		one = webpVP8FrameOf(rng, 16, 16, false, hd)
		if reference2(webpRIFF(webpChunk("VP8 ", one))) == statusOK {
			break
		}
	}
	tokens := len(one) - 10 - (int(one[0])>>5 | int(one[1])<<3 | int(one[2])<<11)
	padded := func(n int) []byte { return append(append([]byte(nil), one...), make([]byte, n-tokens)...) }
	webpWant(t, "a last partition of 16 MiB less a byte", webpRIFF(webpChunk("VP8 ", padded(1<<24-1))), true, ok)
	webpWant(t, "a last partition of 16 MiB", webpRIFF(webpChunk("VP8 ", padded(1<<24))), true, bad)
	webpWant(t, "an odd lossy chunk as the last", webpRIFF(webpChunk("VP8 ", append(frame, make([]byte, 1-len(frame)%2)...))[:8+len(frame)+1-len(frame)%2]), true, ok)
}

// TestWebpLossyExtremes compares the decoders on lossy frames whose headers
// and coefficients are at the ends of their ranges, where the integers of
// the Go decoder wrap.
func TestWebpLossyExtremes(t *testing.T) {
	rng := rand.New(rand.NewSource(21))
	decoded, n := 0, 0
	check := func(name string, hd webpVP8Head, w, h int) {
		data := webpRIFF(webpChunk("VP8 ", webpVP8FrameOf(rng, w, h, false, hd)))
		n++
		if reference2(data) == statusOK {
			decoded++
		}
		if d := webpCompare(data); d != "" {
			t.Errorf("%s: %s", name, d)
		}
	}
	// The level of the loop filter is an int8 in Go: the strength of a
	// segment, the level of the frame and the two deltas add up past 127
	// (which turns the filter off) and below -128 (which turns it on).
	for _, strength := range []int{63, 62, 1, -63} {
		for _, level := range []int{63, 62, 2} {
			for _, ref := range []int{63, 3, -63} {
				for _, mode := range []int{63, 0, -63} {
					for _, simple := range []bool{false, true} {
						hd := webpRandVP8Head(rng)
						hd.useSegment, hd.updateData, hd.absolute = true, true, false
						hd.strength = [4]int{strength, strength, strength, strength}
						hd.level, hd.simple = level, simple
						hd.useLFDelta, hd.updateLFDelta = true, true
						hd.lfDelta[0], hd.lfDelta[4] = ref, mode
						hd.tokens = rng.Intn(5)
						check(fmt.Sprintf("filter strength %d, level %d, deltas %d and %d", strength, level, ref, mode), hd, 40, 24)
					}
				}
			}
		}
	}
	// Every coefficient there is, each as large as one gets, for each
	// quantizer: a coefficient wraps as an int16, the products of the
	// inverse DCT as int32, and what the inverse WHT gives as int16 again.
	for q := 0; q < 128; q++ {
		hd := webpRandVP8Head(rng)
		hd.useSegment = false
		hd.baseQ = q
		hd.dq = [5]int{0, rng.Intn(31) - 15, rng.Intn(31) - 15, 0, 0}
		hd.tokens = webpAllOnes
		check(fmt.Sprintf("the largest coefficients, quantizer %d", q), hd, 32, 32)
	}
	t.Logf("%d frames, %d of them images the reference decodes", n, decoded)
	if decoded < n*9/10 {
		t.Errorf("only %d of %d frames decode: the generator is wrong", decoded, n)
	}
}

// TestWebpTwiceGivenCodes compares the decoders on lossless images whose
// code lengths give a code to two symbols. Go's decoder takes that where all
// the codes have one length: the later symbol has the code.
func TestWebpTwiceGivenCodes(t *testing.T) {
	ok := func(name string, data []byte) {
		t.Helper()
		webpWant(t, name, webpRIFF(webpChunk("VP8L", data)), true, statusOK)
	}
	lengthsOf := func(n int, length int, symbols ...int) func(*webpLGen) {
		return func(g *webpLGen) {
			l := make([]int, n)
			for _, s := range symbols {
				l[s] = length
			}
			g.normalTree(l)
		}
	}
	ok("three symbols of 1 bit", webpGreenCodes(4, 0, lengthsOf(280, 1, 10, 20, 30), "0 1 0 1"))
	ok("five symbols of 2 bits", webpGreenCodes(6, 0, lengthsOf(280, 2, 1, 2, 3, 4, 5), "00 01 10 11 00 01"))
	ok("nine symbols of 3 bits", webpGreenCodes(4, 0, lengthsOf(280, 3, 1, 2, 3, 4, 5, 6, 7, 8, 9), "000 001 111 000"))
	// 256 literals of 8 bits and a lookup in the color cache, which gets the code of literal 0
	all := make([]int, 257)
	for i := range all {
		all[i] = i
	}
	all[256] = 280
	ok("257 symbols of 8 bits", webpGreenCodes(4, 1, lengthsOf(282, 8, all...), "00000001 00000000 00000010 00000000"))
	// in the code of the code lengths: 0, 1 and 8 have 1 bit each, so a 0 bit
	// is a length of 8, and there is no length of 0 any more
	ok("three code lengths of 1 bit", webpGreenCodes(3, 0, func(g *webpLGen) {
		g.w.put(0, 1)    // not a simple code
		g.w.put(12-4, 4) // 12 lengths of code lengths: up to the one of 8
		for _, s := range webpCodeLengthCodeOrder[:12] {
			if s == 0 || s == 1 || s == 8 {
				g.w.put(1, 3)
			} else {
				g.w.put(0, 3)
			}
		}
		g.w.put(1, 1)   // only so many codes are read:
		g.w.put(3, 3)   // a count of 8 bits,
		g.w.put(254, 8) // 256 of them
		for i := 0; i < 256; i++ {
			g.w.put(0, 1)
		}
	}, "00000101 11111111 00000000"))
}

// TestWebpGroups compares the decoders on lossless images with many groups
// of Huffman trees: Go keeps only the groups in use when the largest index
// is 1000 or more, or not below the number of tiles (and then the trees of
// the others need not be trees), and refuses an index of 2600.
func TestWebpGroups(t *testing.T) {
	rng := rand.New(rand.NewSource(31))
	image := func(w, h, groups int) []byte {
		return webpRIFF(webpChunk("VP8L", webpGroupsImage(rng, w, h, 2, groups)))
	}
	// 33 x 33 tiles: more tiles than groups
	webpWant(t, "1000 groups", image(132, 132, 1000), true, statusOK)
	webpWant(t, "1001 groups", image(132, 132, 1001), true, statusOK)
	webpWant(t, "2600 groups", image(132, 132, 2600), true, statusOK)
	webpWant(t, "2601 groups", image(132, 132, 2601), true, statusFormat)
	// fewer tiles than groups
	webpWant(t, "7 groups for 4 tiles", image(8, 8, 7), true, statusOK)
	webpWant(t, "4 groups for 4 tiles", image(8, 8, 4), true, statusOK)
	webpWant(t, "5 groups for 4 tiles", image(8, 8, 5), true, statusOK)
}

// reference2 is the status the reference decodes a file with, where a test
// has no expectation of its own.
func reference2(data []byte) int {
	_, _, _, s := reference("webp", data)
	return s
}

// TestWebpCuts compares the decoders on every prefix of small files (a
// decoder has to stop alike wherever a file ends), and on some of them with
// each byte of their beginning set to each value.
func TestWebpCuts(t *testing.T) {
	rng := rand.New(rand.NewSource(11))
	cuts, sweeps := map[string][]byte{}, map[string][]byte{}
	for len(cuts) < 100 {
		data := webpSynthetic(rng, 0)
		if _, _, _, s := reference("webp", data); s == statusOK && len(data) < 500 {
			name := fmt.Sprintf("synthetic %d", len(cuts))
			cuts[name] = data
			if len(cuts)%25 == 0 {
				sweeps[name] = data
			}
		}
	}
	for name, data := range files([]string{"testdata/images"}, ".webp") {
		if len(data) < 700 {
			cuts[name] = data
		}
		switch filepath.Base(name) {
		case "lossy-size-dot1.webp", "alpha-half.webp", "lossless-dot1.webp", "animated-lossy.webp", "meta-xmp-alpha.webp", "lossless-c2-z0.webp":
			sweeps[name] = data
		}
	}
	cases := 0
	for name, data := range cuts {
		for n := 0; n < len(data); n++ {
			cases++
			if d := webpCompare(data[:n]); d != "" {
				t.Errorf("%s cut to %d bytes of %d: %s", name, n, len(data), d)
				break
			}
		}
	}
	for name, data := range sweeps {
		b := append([]byte(nil), data...)
		for i := 0; i < len(b) && i < 48; i++ {
			for v := 0; v < 256; v += 1 + i/24*4 {
				cases++
				b[i] = byte(v)
				if d := webpCompare(b); d != "" {
					t.Errorf("%s with byte %d set to %#x: %s", name, i, v, d)
				}
			}
			b[i] = data[i]
		}
	}
	t.Logf("%d files cut, %d with bytes set, %d cases", len(cuts), len(sweeps), cases)
}

// BenchmarkWebp decodes some of the test data of golang.org/x/image with
// the reference and with the C decoder, each to premultiplied RGBA.
func BenchmarkWebp(b *testing.B) {
	dir := ximage(b) + "/testdata/"
	for _, name := range []string{
		"blue-purple-pink-large.normal-filter.lossy.webp",
		"blue-purple-pink-large.simple-filter.lossy.webp",
		"video-001.lossy.webp",
		"yellow_rose.lossy-with-alpha.webp",
		"yellow_rose.lossless.webp",
		"blue-purple-pink-large.lossless.webp",
		"gopher-doc.8bpp.lossless.webp",
		"tux.lossless.webp",
	} {
		data, err := os.ReadFile(dir + name)
		if err != nil {
			b.Fatal(err)
		}
		b.Run("go/"+name, func(b *testing.B) {
			b.SetBytes(int64(len(data)))
			for i := 0; i < b.N; i++ {
				reference("webp", data)
			}
		})
		b.Run("c/"+name, func(b *testing.B) {
			b.SetBytes(int64(len(data)))
			for i := 0; i < b.N; i++ {
				cDecode("webp", data)
			}
		})
	}
}
