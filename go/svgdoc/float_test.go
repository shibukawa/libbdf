package svgdoc

// bdfi_parse_float against strconv.ParseFloat.

import (
	"errors"
	"fmt"
	"math"
	"math/big"
	"math/rand"
	"strconv"
	"strings"
	"testing"
)

// goFloat is strconv.ParseFloat(s, 64) as bdfi_parse_float answers: the bits
// and 0, 1 for a number out of range, -1 for what is no number.
func goFloat(s string) (uint64, int8) {
	v, err := strconv.ParseFloat(s, 64)
	switch {
	case err == nil:
		return math.Float64bits(v), 0
	case errors.Is(err, strconv.ErrRange):
		return math.Float64bits(v), 1
	case errors.Is(err, strconv.ErrSyntax):
		return math.Float64bits(v), -1
	}
	panic(err)
}

// checkFloats compares the two on the strings given and returns how many
// differ (the first few are reported).
func checkFloats(t testing.TB, in []string) int {
	bits, status := cFloats(in)
	bad := 0
	for i, s := range in {
		wb, ws := goFloat(s)
		if wb != bits[i] || ws != status[i] {
			if bad++; bad <= 5 {
				if len(s) > 200 {
					s = s[:200] + fmt.Sprintf("… (%d bytes)", len(s))
				}
				t.Errorf("ParseFloat(%q): C %016x (%d), Go %016x (%d)", s, bits[i], status[i], wb, ws)
			}
		}
	}
	return bad
}

func digits(rng *rand.Rand, n int) string {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte('0' + rng.Intn(10))
	}
	return string(b)
}

// decimalOf is the exact decimal expansion of m * 2**e.
func decimalOf(m *big.Int, e int) string {
	r := new(big.Rat).SetInt(m)
	if e >= 0 {
		r.Mul(r, new(big.Rat).SetInt(new(big.Int).Lsh(big.NewInt(1), uint(e))))
		return r.FloatString(0)
	}
	r.Quo(r, new(big.Rat).SetInt(new(big.Int).Lsh(big.NewInt(1), uint(-e))))
	return strings.TrimRight(r.FloatString(-e), "0")
}

// nudge changes a decimal a little: a digit more or less at its end.
func nudge(rng *rand.Rand, s string) string {
	switch rng.Intn(8) {
	case 0:
		return s + "1"
	case 1:
		return s + "0000000000000000000000001"
	case 2:
		return s + digits(rng, 1+rng.Intn(30))
	case 3: // one less in the last place
		b := []byte(s)
		for i := len(b) - 1; i >= 0; i-- {
			if b[i] == '.' {
				continue
			}
			if b[i] > '0' {
				b[i]--
				break
			}
			b[i] = '9'
		}
		return string(b) + "9999999999"
	case 4:
		if len(s) > 2 {
			return s[:len(s)-1-rng.Intn(min(len(s)-1, 20))]
		}
	case 5:
		return s + strings.Repeat("0", rng.Intn(900))
	case 6:
		return s + strings.Repeat("0", rng.Intn(900)) + "1"
	}
	return s
}

// asExponent writes a decimal another way: the point moved and an exponent
// that moves it back.
func asExponent(rng *rand.Rand, s string) string {
	if rng.Intn(2) == 0 {
		return s
	}
	intPart, frac, _ := strings.Cut(s, ".")
	all := intPart + frac
	shift := rng.Intn(len(all) + 1)
	exp := len(intPart) - shift
	out := all[:shift] + "." + all[shift:]
	if rng.Intn(3) == 0 {
		out = strings.Repeat("0", rng.Intn(5)) + out
	}
	e := []string{"e", "E"}[rng.Intn(2)]
	if exp >= 0 && rng.Intn(2) == 0 {
		e += "+"
	}
	return out + e + strconv.Itoa(exp)
}

// halfway is a decimal at, or next to, the middle between two neighbouring
// float64s: where rounding decides.
func halfway(rng *rand.Rand) string {
	var bits uint64
	switch rng.Intn(6) {
	case 0: // subnormals
		bits = uint64(rng.Int63n(1 << 52))
	case 1: // small subnormals
		bits = uint64(rng.Intn(64))
	case 2: // the largest
		bits = 0x7fefffffffffffff - uint64(rng.Intn(4))
	case 3: // around the smallest normal
		bits = 0x0010000000000000 + uint64(rng.Intn(8)) - 4
	case 4: // powers of two, where the spacing changes
		bits = uint64(rng.Intn(2046)+1)<<52 - uint64(rng.Intn(2))
	default:
		bits = uint64(rng.Int63()) & 0x7fffffffffffffff
		if bits >= 0x7ff0000000000000 {
			bits &= 0x7fefffffffffffff
		}
	}
	f := math.Float64frombits(bits)
	mant, exp := math.Frexp(f) // f = mant * 2**exp, mant in [0.5, 1)
	m := big.NewInt(int64(mant * (1 << 53)))
	e := exp - 53
	if bits < 1<<52 { // subnormal: f = bits * 2**-1074
		m, e = new(big.Int).SetUint64(bits), -1074
	}
	// the middle to the next float64 is (2m+1) * 2**(e-1)
	mid := new(big.Int).Lsh(m, 1)
	mid.Add(mid, big.NewInt(1))
	s := decimalOf(mid, e-1)
	if rng.Intn(3) != 0 {
		s = nudge(rng, s)
	}
	s = asExponent(rng, s)
	if rng.Intn(4) == 0 {
		s = "-" + s
	}
	return s
}

var floatWords = []string{"inf", "Inf", "INF", "+inf", "-inf", "infinity", "Infinity", "+Infinity", "-INFINITY", "infinit", "infi", "in", "i", "nan", "NaN", "NAN", "+nan", "-nan", "nann",
	"0x1p0", "0X1P0", "0x1.8p3", "0x.8p1", "0x1p-1074", "0x1p-1075", "0x1.fffffffffffffp1023", "0x1.fffffffffffff8p1023", "0x1p1024", "0x1.00000000000008p0", "0x1.00000000000018p0", "0x10000000000000001p0",
	"0x_1p0", "0x1_0p0", "0x1p0_0", "0x1p_0", "0x1__0p0", "0x1p", "0x1", "0x", "0xp1", "0x.p1", "0x1.p1", "-0x1p-2", "+0x1p+2", "0x1e5", "0x1p5e", "0xgp1", "0x0p0", "-0x0p0", "0x0.0000p99999",
	"1_0", "1_000.5", "_1", "1_", "1e1_0", "1e_10", "", ".", "+", "-", "e", "e5", "1e", "1e+", "1e-", "+.e1", ".e1", "1.e1", ".1e1", "1..2", "1.2.3", "--1", "+-1", "1e--1", "1 ", " 1", "1\x00", "1f", "1d",
	"0", "-0", "+0", "0.0", "-0.0", "00", "000.000", "0e0", "0e999999999", "-0e-999999999", "0.0000e+5", "1", "10", "1e0", "1E0", "1e+0", "1e-0", "1e00000000000000000000001", "1e-00000000000000000000001",
	"1e308", "1.7976931348623157e308", "1.7976931348623158e308", "1.797693134862315807e308", "1.797693134862315808e308", "1.7976931348623159e308", "1e309", "1e310", "1e311", "1e400", "1e999", "1e9999", "1e10000", "1e99999", "1e100000", "1e123456789", "1e2147483648", "1e9223372036854775808", "1e99999999999999999999",
	"4.9e-324", "5e-324", "4.9406564584124654e-324", "2.4703282292062327e-324", "2.4703282292062328e-324", "2.47e-324", "2.5e-324", "1e-323", "1e-324", "1e-325", "1e-344", "1e-345", "1e-346", "1e-400", "1e-9999", "1e-10000", "1e-99999", "1e-123456789",
	"2.2250738585072014e-308", "2.2250738585072011e-308", "2.2250738585072012e-308", "2.225073858507201e-308", "9007199254740992", "9007199254740993", "9007199254740994", "9007199254740995", "18446744073709551615", "18446744073709551616", "9999999999999999999", "99999999999999999999", "0.1", "0.3", "0.30000000000000004", "1.0000000000000002", "1.00000000000000011102230246251565404236316680908203125", "1.00000000000000011102230246251565404236316680908203124", "1.00000000000000011102230246251565404236316680908203126",
	"123456789012345678901234567890", "0.000000000000000000000000000001", "1e22", "1e23", "8.5e15", "9.5e15", "4503599627370496.5", "4503599627370497.5", "\xc4\xb0nf", "infinity\xc4\xb0", "1\xff"}

// floatInput is a string ParseFloat may be asked about, of one of the kinds
// that are hard: digits at random, the middle between two float64s, the
// smallest and the largest numbers, long mantissas, huge exponents, and
// what is no number at all.
func floatInput(rng *rand.Rand) string {
	sign := []string{"", "", "", "-", "+"}[rng.Intn(5)]
	exp := func(lo, hi int) string {
		e := lo + rng.Intn(hi-lo+1)
		s := []string{"e", "E"}[rng.Intn(2)]
		if e >= 0 && rng.Intn(3) == 0 {
			s += "+"
		}
		return s + strconv.Itoa(e)
	}
	switch rng.Intn(16) {
	case 0, 1: // what a drawing has: a few digits and a point
		return sign + strconv.Itoa(rng.Intn(100000)) + "." + digits(rng, rng.Intn(8))
	case 2: // digits at random, with a point somewhere
		d := digits(rng, 1+rng.Intn(25))
		p := rng.Intn(len(d) + 1)
		return sign + d[:p] + "." + d[p:]
	case 3: // and an exponent
		d := digits(rng, 1+rng.Intn(25))
		p := rng.Intn(len(d) + 1)
		return sign + d[:p] + "." + d[p:] + exp(-30, 30)
	case 4, 5, 6: // where rounding decides
		return halfway(rng)
	case 7: // the smallest numbers
		return sign + strconv.Itoa(1+rng.Intn(9)) + "." + digits(rng, rng.Intn(20)) + exp(-345, -300)
	case 8: // the largest
		return sign + strconv.Itoa(1+rng.Intn(9)) + "." + digits(rng, rng.Intn(20)) + exp(290, 312)
	case 9: // 15 to 21 digits: where a mantissa stops fitting
		return sign + digits(rng, 15+rng.Intn(7)) + exp(-40, 40)
	case 10: // long mantissas
		d := digits(rng, 20+rng.Intn(100))
		if rng.Intn(4) == 0 {
			d = digits(rng, 700+rng.Intn(400))
		}
		p := rng.Intn(len(d) + 1)
		s := sign + d[:p] + "." + d[p:]
		if rng.Intn(2) == 0 {
			s += exp(-len(d)-340, 340)
		}
		return s
	case 11: // zeros before and after
		s := sign + strings.Repeat("0", rng.Intn(400)) + "." + strings.Repeat("0", rng.Intn(400)) + digits(rng, 1+rng.Intn(25))
		if rng.Intn(2) == 0 {
			s += exp(-50, 800)
		}
		return s
	case 12: // huge exponents
		e := []string{"400", "999", "9999", "10000", "10001", "12345", "99999", "100000", "123456", "999999", "2147483647", "2147483648", "4294967296", "9223372036854775807", "99999999999999999999", "00000000000000000000400", "000000310", "0345"}[rng.Intn(18)]
		z := strings.Repeat("0", []int{0, 0, 300, 9990, 9999, 10000, 12340, 99990, 100000}[rng.Intn(9)])
		return sign + []string{"1", digits(rng, 1+rng.Intn(20)), "0." + z + digits(rng, 1+rng.Intn(20)), digits(rng, 1+rng.Intn(5)) + z}[rng.Intn(4)] + "e" + []string{"", "+", "-"}[rng.Intn(3)] + e
	case 13: // what is written otherwise, or is no number
		w := floatWords[rng.Intn(len(floatWords))]
		if rng.Intn(4) == 0 {
			w = sign + w
		}
		return w
	case 14: // hexadecimal
		const hex = "0123456789abcdefABCDEF"
		b := []byte("0x")
		for i, n := 0, 1+rng.Intn(20); i < n; i++ {
			b = append(b, hex[rng.Intn(len(hex))])
			if rng.Intn(8) == 0 {
				b = append(b, '.')
			}
			if rng.Intn(20) == 0 {
				b = append(b, '_')
			}
		}
		return sign + string(b) + []string{"p", "P"}[rng.Intn(2)] + []string{"", "+", "-"}[rng.Intn(3)] + strconv.Itoa([]int{rng.Intn(100), 1000 + rng.Intn(100), rng.Intn(2000)}[rng.Intn(3)])
	}
	// a few characters of those numbers are made of
	const alphabet = "0123456789012345678901234567890123456789+-..eE_xXpPinfINFtyaAbBcdf \x00"
	b := make([]byte, rng.Intn(12))
	for i := range b {
		b[i] = alphabet[rng.Intn(len(alphabet))]
	}
	return string(b)
}

// TestParseFloat compares bdfi_parse_float with strconv.ParseFloat on the
// words above and on LIBBDF_SVG_FLOATS inputs of the hard kinds (the default is
// small: a test run takes seconds).
func TestParseFloat(t *testing.T) {
	if bad := checkFloats(t, floatWords); bad != 0 {
		t.Fatalf("%d of the %d words differ", bad, len(floatWords))
	}
	// more digits than the reference keeps, before the point and after it
	var long []string
	for _, n := range []int{799, 800, 801, 802, 850, 1200} {
		for _, d := range []string{"1", "5", "9"} {
			nines := strings.Repeat(d, n)
			long = append(long, nines, nines+".5", nines+"e-"+strconv.Itoa(n-10), nines+"5e-"+strconv.Itoa(n), "0."+nines, "0."+nines+"1", "."+strings.Repeat("0", n)+nines+"e"+strconv.Itoa(n),
				"1"+strings.Repeat("0", n)+"1e-"+strconv.Itoa(n), "9007199254740993"+strings.Repeat("0", n)+"1e-"+strconv.Itoa(n+1), "9007199254740993"+strings.Repeat("0", n)+"e-"+strconv.Itoa(n))
		}
	}
	if bad := checkFloats(t, long); bad != 0 {
		t.Fatalf("%d of the %d long numbers differ", bad, len(long))
	}
	total := envInt("LIBBDF_SVG_FLOATS", 400000)
	rng := rand.New(rand.NewSource(int64(envInt("LIBBDF_SVG_SEED", 1))))
	bad, kinds := 0, map[int8]int{}
	for done := 0; done < total; {
		batch := make([]string, min(20000, total-done))
		for i := range batch {
			batch[i] = floatInput(rng)
		}
		bad += checkFloats(t, batch)
		_, status := cFloats(batch)
		for _, s := range status {
			kinds[s]++
		}
		done += len(batch)
	}
	if bad != 0 {
		t.Errorf("%d of %d differ", bad, total)
	}
	t.Logf("%d inputs: %d numbers, %d out of range, %d no numbers; %d differ", total, kinds[0], kinds[1], kinds[-1], bad)
}

// TestParseFloatSlowPath checks that the inputs reach the part of the
// reference that keeps 800 digits: a mantissa cut at 19 digits whose two
// neighbours round apart.
func TestParseFloatSlowPath(t *testing.T) {
	// 2^53+1 is halfway between two float64s: the digits after it decide,
	// and past 800 digits before the point the reference misplaces the point
	base := "9007199254740993"
	for _, s := range []string{
		base + strings.Repeat("0", 30) + "1",
		base + strings.Repeat("0", 790) + "1e-800",
		base + strings.Repeat("0", 900) + "1e-900",
		base + strings.Repeat("0", 900) + "e-900",
		base + strings.Repeat("0", 900) + ".5e-900",
	} {
		gb, gs := goFloat(s)
		bits, status := cFloats([]string{s})
		if gb != bits[0] || gs != status[0] {
			t.Errorf("%.40q…: C %016x (%d), Go %016x (%d)", s, bits[0], status[0], gb, gs)
		}
		t.Logf("%.20s… (%d digits): %g", s, len(s), math.Float64frombits(gb))
	}
}
