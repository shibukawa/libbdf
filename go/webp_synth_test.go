package cbdf

// WebP files made by code for the tests of the WebP decoder: what cwebp
// does not write (several partitions, loop filter deltas, every predictor
// mode, groups of Huffman trees that are renumbered, raw alpha with every
// filter, ...) and containers that are wrong in a known way. The writers
// here follow the specifications (RFC 6386 for VP8, the WebP Lossless
// Bitstream Specification for VP8L); they choose at random among what a
// stream may say and do not care what the picture looks like.

import (
	"encoding/binary"
	"fmt"
	"math/rand"
	"sort"
)

// ---- the container ---------------------------------------------------------

func webpChunk(id string, data []byte) []byte {
	b := append([]byte(id), 0, 0, 0, 0)
	binary.LittleEndian.PutUint32(b[4:], uint32(len(data)))
	b = append(b, data...)
	if len(data)&1 == 1 {
		b = append(b, 0)
	}
	return b
}

func webpRIFF(chunks ...[]byte) []byte {
	body := []byte("WEBP")
	for _, c := range chunks {
		body = append(body, c...)
	}
	b := append([]byte("RIFF"), 0, 0, 0, 0)
	binary.LittleEndian.PutUint32(b[4:], uint32(len(body)))
	return append(b, body...)
}

const webpAlphaFlag = 1 << 4

func webpVP8X(flags byte, w, h int) []byte {
	w--
	h--
	return webpChunk("VP8X", []byte{flags, 0, 0, 0, byte(w), byte(w >> 8), byte(w >> 16), byte(h), byte(h >> 8), byte(h >> 16)})
}

// ---- VP8 -------------------------------------------------------------------

// webpBoolEncoder is the arithmetic coder a VP8 partition is written with
// (RFC 6386, section 7.3).
type webpBoolEncoder struct {
	out      []byte
	rng      uint32
	bottom   uint32
	bitCount int
}

func newWebpBoolEncoder() *webpBoolEncoder { return &webpBoolEncoder{rng: 255, bitCount: 24} }

func (e *webpBoolEncoder) addOne() {
	for i := len(e.out) - 1; i >= 0; i-- {
		if e.out[i] == 255 {
			e.out[i] = 0
		} else {
			e.out[i]++
			break
		}
	}
}

func (e *webpBoolEncoder) put(prob uint8, bit bool) {
	split := 1 + (((e.rng - 1) * uint32(prob)) >> 8)
	if bit {
		e.bottom += split
		e.rng -= split
	} else {
		e.rng = split
	}
	for e.rng < 128 {
		e.rng <<= 1
		if e.bottom&(1<<31) != 0 {
			e.addOne()
		}
		e.bottom <<= 1
		e.bitCount--
		if e.bitCount == 0 {
			e.out = append(e.out, byte(e.bottom>>24))
			e.bottom &= 1<<24 - 1
			e.bitCount = 8
		}
	}
}

func (e *webpBoolEncoder) flush() []byte {
	c := e.bitCount
	v := e.bottom
	if v&(1<<uint(32-c)) != 0 {
		e.addOne()
	}
	v <<= uint(c & 7)
	for c >>= 3; c > 0; c-- {
		v <<= 8
	}
	for c = 4; c > 0; c-- {
		e.out = append(e.out, byte(v>>24))
		v <<= 8
	}
	return e.out
}

// webpRandBytes returns n bytes of one of several distributions: uniform
// ones decode to dense coefficients, skewed ones to sparse ones.
func webpRandBytes(rng *rand.Rand, n int) []byte {
	return webpBytesOf(rng, n, rng.Intn(5))
}

// webpAllOnes is the kind of bytes that an arithmetic decoder reads as 1
// whatever the probability: every coefficient there is, each as large as a
// coefficient gets.
const webpAllOnes = 5

// webpBytesOf returns n bytes of the kind given.
func webpBytesOf(rng *rand.Rand, n, kind int) []byte {
	b := make([]byte, n)
	switch kind {
	case 0:
		rng.Read(b)
	case 1: // mostly zero
		for i := range b {
			if rng.Intn(8) == 0 {
				b[i] = byte(rng.Intn(256))
			}
		}
	case 2: // mostly 0xff
		for i := range b {
			b[i] = 0xff
			if rng.Intn(8) == 0 {
				b[i] = byte(rng.Intn(256))
			}
		}
	case 3: // low values
		for i := range b {
			b[i] = byte(rng.Intn(1 + rng.Intn(64)))
		}
	case 4: // runs
		for i := 0; i < n; {
			v, l := byte(rng.Intn(256)), 1+rng.Intn(12)
			for ; l > 0 && i < n; l, i = l-1, i+1 {
				b[i] = v
			}
		}
	default: // webpAllOnes
		for i := range b {
			b[i] = 0xff
		}
	}
	return b
}

// webpVP8Head is what the headers of a VP8 key frame say, as far as the
// generator chooses it (RFC 6386, sections 9.2 to 9.6).
type webpVP8Head struct {
	useSegment, updateMap, updateData, absolute bool
	quantizer, strength                         [4]int // of the segments
	segProb                                     [3]int // of the segment map; -1: not sent
	simple                                      bool   // the simple loop filter
	level, sharpness                            int
	useLFDelta, updateLFDelta                   bool
	lfDelta                                     [8]int // four by reference frame, four by mode
	log2nop                                     int    // 1, 2, 4 or 8 token partitions
	baseQ                                       int
	dq                                          [5]int // Y1 DC, Y2 DC, Y2 AC, UV DC, UV AC
	ones                                        int    // of 100 arbitrary bits after the headers, how many are 1
	tokens                                      int    // the kind of bytes of the token partitions (webpBytesOf)
}

// webpRandVP8Head chooses headers at random.
func webpRandVP8Head(rng *rand.Rand) webpVP8Head {
	coin := func(n int) bool { return rng.Intn(n) == 0 }
	signed := func(max int) int {
		if coin(3) {
			return 0
		}
		if coin(2) {
			return rng.Intn(2*max+1) - max
		}
		return rng.Intn(9) - 4
	}
	var hd webpVP8Head
	hd.useSegment = coin(2)
	hd.updateMap = !coin(4)
	hd.updateData = !coin(4)
	hd.absolute = coin(2)
	for i := 0; i < 4; i++ {
		hd.quantizer[i] = signed(127)
		hd.strength[i] = signed(63)
	}
	for i := range hd.segProb {
		hd.segProb[i] = rng.Intn(256)
		if coin(3) {
			hd.segProb[i] = -1
		}
	}
	hd.simple = coin(2)
	hd.level = rng.Intn(64)
	if coin(4) {
		hd.level = 0
	}
	hd.sharpness = rng.Intn(8)
	hd.useLFDelta = coin(2)
	hd.updateLFDelta = !coin(4)
	for i := range hd.lfDelta {
		hd.lfDelta[i] = signed(63)
	}
	hd.log2nop = rng.Intn(4)
	if coin(2) {
		hd.log2nop = 0
	}
	hd.baseQ = rng.Intn(128)
	for i := range hd.dq {
		hd.dq[i] = signed(15)
	}
	hd.ones = []int{2, 2, 2, 10, 50, 90, 98}[rng.Intn(7)]
	hd.tokens = rng.Intn(5)
	if coin(12) {
		hd.tokens = webpAllOnes
	}
	return hd
}

// webpVP8Frame returns a VP8 key frame of w x h pixels: its headers say what
// the generator chooses (segments, the loop filter and its deltas, the number
// of partitions, the quantizers) and everything after them (the token
// probabilities, the modes and the coefficients of the macroblocks) is
// arbitrary bytes, which an arithmetic decoder reads as something. short
// makes the partitions too short for the picture now and then.
func webpVP8Frame(rng *rand.Rand, w, h int, short bool) []byte {
	return webpVP8FrameOf(rng, w, h, short, webpRandVP8Head(rng))
}

// webpVP8FrameOf is webpVP8Frame with the headers given.
func webpVP8FrameOf(rng *rand.Rand, w, h int, short bool, hd webpVP8Head) []byte {
	e := newWebpBoolEncoder()
	bit := func(b bool) { e.put(128, b) }
	coin := func(n int) bool { return rng.Intn(n) == 0 }
	uintN := func(v uint32, n int) {
		for n > 0 {
			n--
			bit(v>>uint(n)&1 == 1)
		}
	}
	optInt := func(v, n int) {
		if v == 0 && coin(2) {
			bit(false)
			return
		}
		bit(true)
		if v < 0 {
			uintN(uint32(-v), n)
			bit(true)
		} else {
			uintN(uint32(v), n)
			bit(v == 0 && coin(4)) // a negative zero now and then
		}
	}

	bit(coin(2)) // the color space
	bit(coin(2)) // the clamping
	// the segment header
	bit(hd.useSegment)
	if hd.useSegment {
		bit(hd.updateMap)
		bit(hd.updateData)
		if hd.updateData {
			bit(hd.absolute)
			for _, v := range hd.quantizer {
				optInt(v, 7)
			}
			for _, v := range hd.strength {
				optInt(v, 6)
			}
		}
		if hd.updateMap {
			for _, v := range hd.segProb {
				if v < 0 {
					bit(false)
				} else {
					bit(true)
					uintN(uint32(v), 8)
				}
			}
		}
	}
	// the filter header
	bit(hd.simple)
	uintN(uint32(hd.level), 6)
	uintN(uint32(hd.sharpness), 3)
	bit(hd.useLFDelta)
	if hd.useLFDelta {
		bit(hd.updateLFDelta)
		if hd.updateLFDelta {
			for _, v := range hd.lfDelta {
				optInt(v, 6)
			}
		}
	}
	// the partitions
	uintN(uint32(hd.log2nop), 2)
	nop := 1 << uint(hd.log2nop)
	// the quantizers
	uintN(uint32(hd.baseQ), 7)
	for _, v := range hd.dq {
		optInt(v, 4)
	}
	bit(coin(2)) // refresh the last frame
	// what follows is read as the updates of the token probabilities, the
	// skip probability and the modes of the macroblocks
	mbw, mbh := (w+15)/16, (h+15)/16
	mbs := mbw * mbh
	for i, n := 0, 1300+mbs*(180+rng.Intn(100)); i < n; i++ {
		e.put(uint8(1+rng.Intn(255)), rng.Intn(100) < hd.ones)
	}
	first := e.flush()
	first = append(first, webpRandBytes(rng, 16+mbs*24+rng.Intn(64))...)
	if short && coin(3) {
		first = first[:rng.Intn(len(first)+1)]
	}

	// the other partitions: the lengths of all but the last, then their
	// bytes. A partition has the coefficients of every nop-th row of
	// macroblocks; a macroblock of arbitrary bytes reads a kilobyte of
	// them at the most, one of bytes that are all ones four.
	perMB := 1000
	if hd.tokens == webpAllOnes {
		perMB = 5000
	}
	var lens, parts []byte
	for i := 0; i < nop; i++ {
		n := (mbh+nop-1)/nop*mbw*perMB + 32 + rng.Intn(32)
		if short && coin(4) {
			n = rng.Intn(1 + n/(1+rng.Intn(200)))
		}
		if i < nop-1 {
			lens = append(lens, byte(n), byte(n>>8), byte(n>>16))
		}
		parts = append(parts, webpBytesOf(rng, n, hd.tokens)...)
	}

	tag := uint32(len(first))<<5 | uint32(rng.Intn(2))<<4 | uint32(rng.Intn(4))<<1
	b := []byte{byte(tag), byte(tag >> 8), byte(tag >> 16), 0x9d, 0x01, 0x2a}
	xs, ys := rng.Intn(4), rng.Intn(4) // the scales: not used by a decoder
	b = append(b, byte(w), byte(w>>8)|byte(xs<<6), byte(h), byte(h>>8)|byte(ys<<6))
	b = append(b, first...)
	b = append(b, lens...)
	return append(b, parts...)
}

// ---- VP8L ------------------------------------------------------------------

// webpBitWriter writes bits from the low bit of a byte up, as VP8L reads.
type webpBitWriter struct {
	b    []byte
	nbit uint
}

// put writes the low n bits of v, the lowest first.
func (w *webpBitWriter) put(v uint32, n uint) {
	for i := uint(0); i < n; i++ {
		if w.nbit%8 == 0 {
			w.b = append(w.b, 0)
		}
		if v>>i&1 != 0 {
			w.b[len(w.b)-1] |= 1 << (w.nbit % 8)
		}
		w.nbit++
	}
}

// code writes a Huffman code of n bits, the highest first.
func (w *webpBitWriter) code(c uint32, n int) {
	for i := n; i > 0; i-- {
		w.put(c>>uint(i-1)&1, 1)
	}
}

// webpHuff is a prefix code: the length and the canonical code of every
// symbol of an alphabet. A code of one symbol takes no bits.
type webpHuff struct {
	lengths []int
	codes   []uint32
	single  bool
}

func webpNewHuff(lengths []int) *webpHuff {
	h := &webpHuff{lengths: lengths, codes: make([]uint32, len(lengths))}
	var count [16]int
	n := 0
	for _, l := range lengths {
		if l > 0 {
			count[l]++
			n++
		}
	}
	h.single = n == 1
	var next [16]uint32
	code := uint32(0)
	for l := 1; l < 16; l++ {
		code = (code + uint32(count[l-1])) << 1
		next[l] = code
	}
	for s, l := range lengths {
		if l > 0 {
			h.codes[s] = next[l]
			next[l]++
		}
	}
	return h
}

func (h *webpHuff) put(w *webpBitWriter, symbol int) {
	if h.lengths[symbol] == 0 {
		panic(fmt.Sprintf("symbol %d has no code", symbol))
	}
	if !h.single {
		w.code(h.codes[symbol], h.lengths[symbol])
	}
}

// webpRandLengths returns code lengths of a complete prefix code over the
// given symbols of an alphabet of n: the depths of the leaves of a binary
// tree grown at random, no deeper than max.
func webpRandLengths(rng *rand.Rand, n int, symbols []int, max int) []int {
	lengths := make([]int, n)
	if len(symbols) == 1 {
		lengths[symbols[0]] = 1 + rng.Intn(max)
		return lengths
	}
	depths := []int{1, 1}
	for len(depths) < len(symbols) {
		i := rng.Intn(len(depths))
		for depths[i] >= max {
			i = (i + 1) % len(depths)
		}
		depths[i]++
		depths = append(depths, depths[i])
	}
	rng.Shuffle(len(depths), func(i, j int) { depths[i], depths[j] = depths[j], depths[i] })
	for i, s := range symbols {
		lengths[s] = depths[i]
	}
	return lengths
}

var webpCodeLengthCodeOrder = [19]int{17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}

// webpLGen writes a VP8L stream. One choice in bad breaks a rule of the
// format (bad 0: none does), so that some streams are refused, or decode
// past what an encoder would write.
type webpLGen struct {
	rng *rand.Rand
	w   webpBitWriter
	bad int
	// For the tests of the groups of trees: with groupLimit the image has
	// groups chosen by tiles of 1<<groupBits pixels, the indices of the
	// groups are below groupLimit and the largest is in use; with groupsAll
	// the stream is written to its end even where a decoder has to refuse
	// the number of groups.
	groupBits, groupLimit int
	groupsAll             bool
}

func (g *webpLGen) pick(n int) int  { return g.rng.Intn(n) }
func (g *webpLGen) coin(n int) bool { return g.rng.Intn(n) == 0 }
func (g *webpLGen) isBad() bool     { return g.bad > 0 && g.rng.Intn(g.bad) == 0 }

// subset returns k distinct numbers below n (all of them if k is more).
func (g *webpLGen) subset(n, k int) []int {
	if k > n {
		k = n
	}
	s := g.rng.Perm(n)[:k]
	sort.Ints(s)
	return s
}

// normalTree writes code lengths as a "normal" Huffman code: the code of
// the code lengths, then the lengths with it, run-length coded at random.
func (g *webpLGen) normalTree(lengths []int) {
	type token struct {
		sym   int
		extra uint32
		bits  uint
	}
	var tokens []token
	n := len(lengths)
	useLength := g.coin(3)
	if useLength {
		// the lengths after the last that is read are zero
		for n > 0 && lengths[n-1] == 0 {
			n--
		}
	}
	prev := 8
	for i := 0; i < n; {
		v, run := lengths[i], 1
		for i+run < n && lengths[i+run] == v {
			run++
		}
		switch {
		case v == 0 && run >= 3 && !g.coin(4):
			if run >= 11 && !g.coin(3) {
				r := 11 + g.pick(min(run, 138)-10)
				tokens = append(tokens, token{18, uint32(r - 11), 7})
				i += r
			} else {
				r := 3 + g.pick(min(run, 10)-2)
				tokens = append(tokens, token{17, uint32(r - 3), 3})
				i += r
			}
		case v != 0 && v == prev && run >= 3 && !g.coin(4):
			r := 3 + g.pick(min(run, 6)-2)
			tokens = append(tokens, token{16, uint32(r - 3), 2})
			i += r
		default:
			tokens = append(tokens, token{sym: v})
			if v != 0 {
				prev = v
			}
			i++
		}
	}
	if useLength {
		for len(tokens) < 2 {
			tokens = append(tokens, token{sym: 0})
		}
	}

	used := map[int]bool{}
	for _, t := range tokens {
		used[t.sym] = true
	}
	for g.coin(2) {
		used[g.pick(19)] = true
	}
	var symbols []int
	for s := range used {
		symbols = append(symbols, s)
	}
	sort.Ints(symbols)
	cl := webpNewHuff(webpRandLengths(g.rng, 19, symbols, 7))
	if g.isBad() {
		cl.lengths[g.pick(19)] = g.pick(8)
	}

	g.w.put(0, 1) // not a simple code
	nCodes := 4
	for i, s := range webpCodeLengthCodeOrder {
		if cl.lengths[s] != 0 && i+1 > nCodes {
			nCodes = i + 1
		}
	}
	nCodes += g.pick(19 - nCodes + 1)
	g.w.put(uint32(nCodes-4), 4)
	for i := 0; i < nCodes; i++ {
		g.w.put(uint32(cl.lengths[webpCodeLengthCodeOrder[i]]), 3)
	}
	if useLength {
		g.w.put(1, 1)
		ms := len(tokens) - 2
		if g.isBad() {
			ms += g.pick(400)
		}
		k := 0
		for ms >= 1<<uint(2+2*k) {
			k++
		}
		k += g.pick(8 - k)
		g.w.put(uint32(k), 3)
		g.w.put(uint32(ms), uint(2+2*k))
	} else {
		g.w.put(0, 1)
	}
	for _, t := range tokens {
		if cl.lengths[t.sym] == 0 {
			break // a broken code of the code lengths: the rest is whatever follows
		}
		cl.put(&g.w, t.sym)
		g.w.put(t.extra, t.bits)
	}
}

// tree writes a Huffman code of the given symbols of an alphabet, and
// returns it.
func (g *webpLGen) tree(alphabet int, symbols []int) *webpHuff {
	simple := len(symbols) <= 2 && symbols[len(symbols)-1] < 256 && !g.coin(3)
	if simple {
		g.w.put(1, 1)
		g.w.put(uint32(len(symbols)-1), 1)
		if symbols[0] < 2 && g.coin(2) {
			g.w.put(0, 1)
			g.w.put(uint32(symbols[0]), 1)
		} else {
			g.w.put(1, 1)
			g.w.put(uint32(symbols[0]), 8)
		}
		lengths := make([]int, alphabet)
		lengths[symbols[0]] = 1
		if len(symbols) == 2 {
			g.w.put(uint32(symbols[1]), 8)
			lengths[symbols[1]] = 1
		}
		return webpNewHuff(lengths)
	}
	lengths := webpRandLengths(g.rng, alphabet, symbols, 15)
	written := lengths
	if g.isBad() {
		// a code that is not complete, or says too much
		written = append([]int(nil), lengths...)
		for i := g.pick(3); i >= 0; i-- {
			written[g.pick(alphabet)] = g.pick(16)
		}
	}
	g.normalTree(written)
	return webpNewHuff(lengths)
}

// webpLGroup is a group of five codes and the symbols the generator may use
// of each.
type webpLGroup struct {
	huff                          [5]*webpHuff
	lits, lens, caches            []int
	reds, blues, alphas, distSyms []int
}

func webpTiles(size, bits int) int { return (size + 1<<uint(bits) - 1) >> uint(bits) }

// lz77 returns what a length or distance symbol stands for, with extra bits
// chosen at random.
func (g *webpLGen) lz77(symbol int) (value int, extra uint32, bits uint) {
	if symbol < 4 {
		return symbol + 1, 0, 0
	}
	bits = uint(symbol-2) >> 1
	offset := (2 + symbol&1) << bits
	extra = uint32(g.pick(1 << bits))
	return offset + int(extra) + 1, extra, bits
}

var webpDistanceMap = [120]uint8{
	0x18, 0x07, 0x17, 0x19, 0x28, 0x06, 0x27, 0x29, 0x16, 0x1a,
	0x26, 0x2a, 0x38, 0x05, 0x37, 0x39, 0x15, 0x1b, 0x36, 0x3a,
	0x25, 0x2b, 0x48, 0x04, 0x47, 0x49, 0x14, 0x1c, 0x35, 0x3b,
	0x46, 0x4a, 0x24, 0x2c, 0x58, 0x45, 0x4b, 0x34, 0x3c, 0x03,
	0x57, 0x59, 0x13, 0x1d, 0x56, 0x5a, 0x23, 0x2d, 0x44, 0x4c,
	0x55, 0x5b, 0x33, 0x3d, 0x68, 0x02, 0x67, 0x69, 0x12, 0x1e,
	0x66, 0x6a, 0x22, 0x2e, 0x54, 0x5c, 0x43, 0x4d, 0x65, 0x6b,
	0x32, 0x3e, 0x78, 0x01, 0x77, 0x79, 0x53, 0x5d, 0x11, 0x1f,
	0x64, 0x6c, 0x42, 0x4e, 0x76, 0x7a, 0x21, 0x2f, 0x75, 0x7b,
	0x31, 0x3f, 0x63, 0x6d, 0x52, 0x5e, 0x00, 0x74, 0x7c, 0x41,
	0x4f, 0x10, 0x20, 0x62, 0x6e, 0x30, 0x73, 0x7d, 0x51, 0x5f,
	0x40, 0x72, 0x7e, 0x61, 0x6f, 0x50, 0x71, 0x7f, 0x60, 0x70,
}

// webpDistance is the distance in pixels of a distance code (section 4.2.2
// of the specification).
func webpDistance(w, code int) int {
	if code > 120 {
		return code - 120
	}
	c := int(webpDistanceMap[code-1])
	if d := (c>>4)*w + 8 - c&0xf; d >= 1 {
		return d
	}
	return 1
}

// group writes the five codes of a group. pixels, if there are any, are the
// values the codes have to have; else the symbols are chosen at random.
// With drop the decoder reads the group and does not keep it, so it may say
// anything.
func (g *webpLGen) group(ccBits int, pixels [][4]byte, drop bool) *webpLGroup {
	gr := &webpLGroup{}
	some := func(n int) []int { return g.subset(n, []int{1, 1, 2, 3, 5, 16, 256}[g.pick(7)]) }
	if pixels != nil {
		var seen [4][256]bool
		for _, p := range pixels {
			for c := 0; c < 4; c++ {
				seen[c][p[c]] = true
			}
		}
		list := func(c int) (s []int) {
			for v, ok := range seen[c] {
				if ok || g.coin(40) {
					s = append(s, v)
				}
			}
			return s
		}
		gr.reds, gr.lits, gr.blues, gr.alphas = list(0), list(1), list(2), list(3)
		gr.distSyms = g.subset(40, 1+g.pick(2))
	} else {
		gr.lits, gr.reds, gr.blues, gr.alphas = some(256), some(256), some(256), some(256)
		if g.coin(2) {
			gr.lens = g.subset(24, 1+g.pick(4))
			if g.coin(2) {
				gr.lens = g.subset(8, 1+g.pick(4)) // short ones
			}
		}
		if ccBits > 0 && !g.coin(3) {
			gr.caches = g.subset(1<<uint(ccBits), 1+g.pick(6))
		}
		gr.distSyms = g.subset([]int{4, 12, 40}[g.pick(3)], 1+g.pick(4))
	}
	green := append([]int(nil), gr.lits...)
	for _, s := range gr.lens {
		green = append(green, 256+s)
	}
	for _, s := range gr.caches {
		green = append(green, 280+s)
	}
	sizes := [5]int{280, 256, 256, 256, 40}
	if ccBits > 0 {
		sizes[0] += 1 << uint(ccBits)
	}
	for j, symbols := range [5][]int{green, gr.reds, gr.blues, gr.alphas, gr.distSyms} {
		if drop && g.coin(2) {
			// a simple code of symbols that the alphabet may not have
			g.w.put(1, 1)
			g.w.put(1, 1)
			g.w.put(1, 1)
			g.w.put(uint32(g.pick(256)), 8)
			g.w.put(uint32(g.pick(256)), 8)
			continue
		}
		gr.huff[j] = g.tree(sizes[j], symbols)
	}
	return gr
}

// image writes an image of w x h pixels as decodePix of the Go decoder
// reads one: the color cache, the groups of codes (several, chosen by a
// smaller image, only for top) and the pixels. pixels, if there are any,
// are the values to write (R, G, B, A); else what is written is literals,
// backward references and cache lookups at random.
func (g *webpLGen) image(w, h int, top bool, pixels [][4]byte) {
	ccBits := 0
	if g.coin(3) {
		ccBits = 1 + g.pick(11)
		if g.isBad() {
			ccBits = []int{0, 12, 15}[g.pick(3)]
		}
		g.w.put(1, 1)
		g.w.put(uint32(ccBits), 4)
		if ccBits < 1 || ccBits > 11 {
			return
		}
	} else {
		g.w.put(0, 1)
	}

	hBits, tilesPerRow := 0, 0
	groupOf := []int{0}
	nGroups, mapped := 1, false
	if top {
		if pixels == nil && (g.coin(3) || g.groupLimit > 0) {
			g.w.put(1, 1)
			hBits = 2 + g.pick(3)
			if g.coin(8) {
				hBits = 2 + g.pick(8)
			}
			if g.groupLimit > 0 {
				hBits = g.groupBits
			}
			g.w.put(uint32(hBits-2), 3)
			tilesPerRow = webpTiles(w, hBits)
			nTiles := tilesPerRow * webpTiles(h, hBits)
			limit := 1 + g.pick(4)
			switch {
			case g.coin(12):
				limit = 1000 + g.pick(1600) // renumbered, and 2600 or more is refused
			case g.coin(6):
				limit = 1 + g.pick(40)
			}
			if g.isBad() {
				limit = 2590 + g.pick(64000)
			}
			if g.groupLimit > 0 {
				limit = g.groupLimit
			}
			meta := make([][4]byte, nTiles)
			groupOf = make([]int, nTiles)
			maxIndex := 0
			for i := range meta {
				idx := g.pick(limit)
				if i == 0 && (g.coin(2) || g.groupLimit > 0) {
					idx = limit - 1
				}
				groupOf[i] = idx
				maxIndex = max(maxIndex, idx)
				meta[i] = [4]byte{byte(idx >> 8), byte(idx), byte(g.pick(256)), byte(g.pick(256))}
			}
			g.image(tilesPerRow, webpTiles(h, hBits), false, meta)
			if maxIndex >= 2600 && !g.groupsAll {
				return
			}
			nGroups = maxIndex + 1
			mapped = maxIndex >= 1000 || maxIndex >= nTiles
		} else {
			g.w.put(0, 1)
		}
	}
	used := make([]bool, nGroups)
	for _, i := range groupOf {
		used[i] = true
	}
	groups := make([]*webpLGroup, nGroups)
	for i := range groups {
		groups[i] = g.group(ccBits, pixels, mapped && !used[i])
	}

	total := w * h
	pos, x, y := 0, 0, 0
	hMask := 0
	if hBits != 0 {
		hMask = 1<<uint(hBits) - 1
	}
	gr, lookup := groups[0], hMask != 0
	for pos < total {
		if lookup {
			gr = groups[groupOf[tilesPerRow*(y>>uint(hBits))+x>>uint(hBits)]]
		}
		if len(gr.lens) > 0 && g.coin(3) {
			// a backward reference, if one fits here
			ls, ds := gr.lens[g.pick(len(gr.lens))], gr.distSyms[g.pick(len(gr.distSyms))]
			length, lextra, lbits := g.lz77(ls)
			code, dextra, dbits := g.lz77(ds)
			dist := webpDistance(w, code)
			if (length <= total-pos && dist <= pos) || g.isBad() {
				gr.huff[0].put(&g.w, 256+ls)
				g.w.put(lextra, lbits)
				gr.huff[4].put(&g.w, ds)
				g.w.put(dextra, dbits)
				if length > total-pos || dist > pos {
					return // refused
				}
				pos += length
				x += length
				for x >= w {
					x, y = x-w, y+1
				}
				lookup = hMask != 0
				continue
			}
		}
		if len(gr.caches) > 0 && pixels == nil && g.coin(3) {
			gr.huff[0].put(&g.w, 280+gr.caches[g.pick(len(gr.caches))])
		} else {
			p := [4]byte{}
			if pixels != nil {
				p = pixels[pos]
			} else {
				p = [4]byte{byte(gr.reds[g.pick(len(gr.reds))]), byte(gr.lits[g.pick(len(gr.lits))]),
					byte(gr.blues[g.pick(len(gr.blues))]), byte(gr.alphas[g.pick(len(gr.alphas))])}
			}
			gr.huff[0].put(&g.w, int(p[1]))
			gr.huff[1].put(&g.w, int(p[0]))
			gr.huff[2].put(&g.w, int(p[2]))
			gr.huff[3].put(&g.w, int(p[3]))
		}
		pos++
		x++
		if x == w {
			x, y = 0, y+1
		}
		lookup = hMask != 0 && x&hMask == 0
	}
}

// randPixels returns n pixels of arbitrary values, from few or many values.
func (g *webpLGen) randPixels(n int) [][4]byte {
	p := make([][4]byte, n)
	k := []int{1, 2, 4, 16, 256}[g.pick(5)]
	var vals [4][]int
	for c := range vals {
		vals[c] = g.subset(256, k)
	}
	for i := range p {
		for c := 0; c < 4; c++ {
			p[i][c] = byte(vals[c][g.pick(len(vals[c]))])
		}
	}
	return p
}

// body writes what follows the header of a VP8L image of w x h pixels: the
// transforms, in any order, and the image.
func (g *webpLGen) body(w, h int) {
	types := g.rng.Perm(4)[:g.pick(5)]
	if g.coin(3) {
		types = nil
	}
	if g.isBad() && len(types) > 0 {
		types = append(types, types[g.pick(len(types))]) // a transform twice
	}
	cw := w
	for _, t := range types {
		g.w.put(1, 1)
		g.w.put(uint32(t), 2)
		switch t {
		case 0, 1: // predictor, cross-color: an image of the tiles
			bits := 2 + g.pick(3)
			if g.coin(8) {
				bits = 2 + g.pick(8)
			}
			g.w.put(uint32(bits-2), 3)
			tw, th := webpTiles(cw, bits), webpTiles(h, bits)
			if g.coin(2) {
				g.image(tw, th, false, g.randPixels(tw*th))
			} else {
				g.image(tw, th, false, nil)
			}
		case 3: // color indexing: the palette
			n := []int{1, 2, 3, 4, 5, 16, 17, 255, 256, 1 + g.pick(256)}[g.pick(10)]
			g.w.put(uint32(n-1), 8)
			bits := 0
			switch {
			case n <= 2:
				bits = 3
			case n <= 4:
				bits = 2
			case n <= 16:
				bits = 1
			}
			cw = webpTiles(cw, bits)
			if g.coin(2) {
				g.image(n, 1, false, g.randPixels(n))
			} else {
				g.image(n, 1, false, nil)
			}
		}
	}
	g.w.put(0, 1)
	g.image(cw, h, true, nil)
}

// webpVP8LBody returns a VP8L image of w x h pixels without its header: what
// an ALPH chunk holds after its first byte.
func webpVP8LBody(rng *rand.Rand, w, h, bad int) []byte {
	g := &webpLGen{rng: rng, bad: bad}
	g.body(w, h)
	b := g.w.b
	if g.coin(4) {
		b = append(b, webpRandBytes(rng, rng.Intn(16))...)
	}
	if bad > 0 && g.coin(8) {
		b = b[:rng.Intn(len(b)+1)]
	}
	return b
}

// webpVP8LHeader returns the 5 bytes a VP8L image starts with.
func webpVP8LHeader(w, h int, alpha bool, version int) []byte {
	v := uint32(w-1) | uint32(h-1)<<14 | uint32(version)<<29
	if alpha {
		v |= 1 << 28
	}
	return []byte{0x2f, byte(v), byte(v >> 8), byte(v >> 16), byte(v >> 24)}
}

// webpGreenBody returns a VP8L image without its header whose green values
// are the given ones: an alpha plane as an ALPH chunk stores it.
func webpGreenBody(rng *rand.Rand, w, h int, green []byte) []byte {
	g := &webpLGen{rng: rng}
	pixels := make([][4]byte, len(green))
	for i, v := range green {
		pixels[i] = [4]byte{0, v, 0, 0xff}
	}
	g.w.put(0, 1) // no transform
	g.image(w, h, true, pixels)
	return g.w.b
}

// ---- files -----------------------------------------------------------------

// webpSize is a size of a small image; the sizes at the edges of macroblocks
// and of tiles come often.
func webpSize(rng *rand.Rand) int {
	switch rng.Intn(4) {
	case 0:
		return []int{1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 48, 49, 64, 65}[rng.Intn(19)]
	case 1:
		return 1 + rng.Intn(8)
	}
	return 1 + rng.Intn(72)
}

// webpSynthetic returns one file made by code: a lossy or a lossless image
// in one of the containers the format has. bad as for webpLGen.
func webpSynthetic(rng *rand.Rand, bad int) []byte {
	w, h := webpSize(rng), webpSize(rng)
	coin := func(n int) bool { return rng.Intn(n) == 0 }
	isBad := func() bool { return bad > 0 && rng.Intn(bad) == 0 }
	junk := func() []byte {
		return webpChunk([]string{"EXIF", "ICCP", "XMP ", "ABCD", "ANIM"}[rng.Intn(5)], webpRandBytes(rng, rng.Intn(20)))
	}
	var chunks [][]byte
	if coin(3) {
		// lossless
		data := append(webpVP8LHeader(w, h, coin(2), 0), webpVP8LBody(rng, w, h, bad)...)
		if coin(2) {
			flags := byte(rng.Intn(256)) &^ 2
			if isBad() {
				chunks = append(chunks, webpVP8X(flags, w+rng.Intn(2), h+1-rng.Intn(2)))
			} else {
				chunks = append(chunks, webpVP8X(flags, w, h))
			}
		}
		if coin(6) {
			chunks = append(chunks, junk())
		}
		chunks = append(chunks, webpChunk("VP8L", data))
	} else {
		frame := webpVP8Frame(rng, w, h, bad > 0)
		switch rng.Intn(3) {
		case 0: // the frame alone
		case 1: // with a VP8X chunk and no alpha
			flags := byte(rng.Intn(256)) &^ (2 | webpAlphaFlag)
			chunks = append(chunks, webpVP8X(flags, w, h))
		default: // with alpha
			chunks = append(chunks, webpVP8X(webpAlphaFlag, w, h))
			filter := byte(rng.Intn(4))
			pre := byte(rng.Intn(4)) // the pre-processing: not used by a decoder
			var a []byte
			if coin(2) {
				a = append([]byte{pre<<4 | filter<<2}, webpRandBytes(rng, w*h)...)
				if isBad() {
					a = a[:rng.Intn(len(a)+1)]
				}
			} else if coin(2) {
				a = append([]byte{pre<<4 | filter<<2 | 1}, webpVP8LBody(rng, w, h, bad)...)
			} else {
				a = append([]byte{pre<<4 | filter<<2 | 1}, webpGreenBody(rng, w, h, webpRandBytes(rng, w*h))...)
			}
			if len(a) > 0 && isBad() {
				a[0] |= 2 // a compression there is none of
			}
			if coin(6) {
				chunks = append(chunks, junk())
			}
			chunks = append(chunks, webpChunk("ALPH", a))
		}
		if coin(6) {
			chunks = append(chunks, junk())
		}
		chunks = append(chunks, webpChunk("VP8 ", frame))
	}
	if coin(6) {
		chunks = append(chunks, junk())
	}
	return webpRIFF(chunks...)
}

// webpSynthetics returns n files made by code from a seed, by name. One
// choice in bad breaks a rule (0: none).
func webpSynthetics(seed int64, n, bad int) map[string][]byte {
	out := map[string][]byte{}
	for i := 0; i < n; i++ {
		rng := rand.New(rand.NewSource(seed*1000003 + int64(i)))
		out[fmt.Sprintf("synthetic-%d-%d-%d", seed, bad, i)] = webpSynthetic(rng, bad)
	}
	return out
}

// webpGroupsImage returns a lossless image of w x h pixels whose trees are
// in groups chosen by tiles of 1<<bits pixels, with indices below limit and
// the largest of them in use. A decoder has to refuse 2600 groups or more;
// the stream is complete all the same.
func webpGroupsImage(rng *rand.Rand, w, h, bits, limit int) []byte {
	g := &webpLGen{rng: rng, groupBits: bits, groupLimit: limit, groupsAll: true}
	g.w.put(0, 1) // no transform
	g.image(w, h, true, nil)
	return append(webpVP8LHeader(w, h, false, 0), append(g.w.b, 0, 0)...)
}

// oneSymbol writes a tree of the one symbol given (below 256).
func (g *webpLGen) oneSymbol(symbol int) {
	g.w.put(1, 1) // a simple code
	g.w.put(0, 1) // of one symbol
	g.w.put(1, 1) // of 8 bits
	g.w.put(uint32(symbol), 8)
}

// webpGreenCodes returns a lossless image of w x 1 pixels of red 1, blue 2
// and alpha 255 whose green tree is written by tree (with a color cache of
// ccBits bits if that is not 0) and whose pixels are the bits given, which
// are codes of that tree.
func webpGreenCodes(w, ccBits int, tree func(g *webpLGen), bits string) []byte {
	g := &webpLGen{rng: rand.New(rand.NewSource(1))}
	g.w.put(0, 1) // no transform
	if ccBits > 0 {
		g.w.put(1, 1)
		g.w.put(uint32(ccBits), 4)
	} else {
		g.w.put(0, 1)
	}
	g.w.put(0, 1) // one group of trees
	tree(g)
	g.oneSymbol(1)   // red
	g.oneSymbol(2)   // blue
	g.oneSymbol(255) // alpha
	g.oneSymbol(0)   // distance
	for _, c := range bits {
		if c == '0' || c == '1' {
			g.w.put(uint32(c-'0'), 1)
		}
	}
	return append(webpVP8LHeader(w, 1, true, 0), append(g.w.b, 0, 0)...)
}
