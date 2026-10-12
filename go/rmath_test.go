package cbdf

import (
	"math"
	"math/rand/v2"
	"testing"
)

// sameFloat reports whether two results are the same: to the bit where the
// reference computes without fused multiply-adds (amd64). Where Go fuses
// them (arm64) its own results are other ones, by more than the last place
// where a difference is taken (1 - x*x near 1), so there the two only have
// to be close. A NaN is any NaN.
func sameFloat(want, got float64) bool {
	if want != want || got != got {
		return want != want && got != got
	}
	if math.Float64bits(want) == math.Float64bits(got) {
		return true
	}
	if exactFloats {
		return false
	}
	return math.Abs(want-got) <= 1e-7*math.Max(math.Abs(want), 1e-300) || math.Abs(want-got) < 1e-12
}

// argument is a number a drawing might hand a function, and at times one it
// would not: zeros, infinities, a NaN, huge and tiny ones.
func argument(r *rand.Rand) float64 {
	switch r.IntN(40) {
	case 0:
		return 0
	case 1:
		return math.Copysign(0, -1)
	case 2:
		return math.Inf(1)
	case 3:
		return math.Inf(-1)
	case 4:
		return math.NaN()
	case 5:
		return math.Float64frombits(r.Uint64()) // any float at all
	case 6:
		return r.NormFloat64() * 1e12 // past the threshold of the reduction of large arguments
	case 7:
		return r.NormFloat64() * 1e-12
	case 8:
		return float64(r.IntN(17)-8) * math.Pi / 4 // where the octants meet
	case 9:
		return float64(r.IntN(5) - 2)
	case 10:
		return r.Float64()*2 - 1 // the domain of the arcsine
	}
	return r.NormFloat64() * 10
}

// TestMathMatchesGo checks the functions of src/rmath.c against Go's.
func TestMathMatchesGo(t *testing.T) {
	r := rand.New(rand.NewPCG(1, 2))
	one := map[string]func(float64) float64{"sin": math.Sin, "cos": math.Cos, "tan": math.Tan, "atan": math.Atan, "asin": math.Asin, "acos": math.Acos, "log2": math.Log2}
	two := map[string]func(float64, float64) float64{"atan2": math.Atan2, "hypot": math.Hypot, "max": math.Max, "min": math.Min, "mod": math.Mod}
	const n = 400000
	for name, f := range one {
		for range n {
			x := argument(r)
			if want, got := f(x), Math[name](x); !sameFloat(want, got) {
				t.Fatalf("%s(%v [%016x]) = %v [%016x], Go has %v [%016x]", name, x, math.Float64bits(x), got, math.Float64bits(got), want, math.Float64bits(want))
			}
		}
	}
	for name, f := range two {
		for range n {
			x, y := argument(r), argument(r)
			if name == "max" || name == "min" {
				// Go's own two disagree on a NaN beside an infinity (the
				// assembly of some platforms has the NaN win): libbdf does
				// what the package documents
				if (x != x || y != y) && (math.IsInf(x, 0) || math.IsInf(y, 0)) {
					continue
				}
			}
			if want, got := f(x, y), Math2[name](x, y); !sameFloat(want, got) {
				t.Fatalf("%s(%v, %v) = %v [%016x], Go has %v [%016x]", name, x, y, got, math.Float64bits(got), want, math.Float64bits(want))
			}
		}
	}
	for range n {
		x := argument(r)
		ws, wc := math.Sincos(x)
		gs, gc := Sincos(x)
		if !sameFloat(ws, gs) || !sameFloat(wc, gc) {
			t.Fatalf("sincos(%v) = %v, %v; Go has %v, %v", x, gs, gc, ws, wc)
		}
	}
	// the documented corners of max and min
	if Math2["max"](math.NaN(), math.Inf(1)) != math.Inf(1) || Math2["min"](math.Inf(-1), math.NaN()) != math.Inf(-1) {
		t.Error("an infinity does not win over a NaN")
	}
	if !math.IsNaN(Math2["max"](math.NaN(), math.Inf(-1))) || !math.IsNaN(Math2["min"](1, math.NaN())) {
		t.Error("a NaN does not win")
	}
}

// TestRasterizerDoesNotFuse checks that the sources of the rasterizer are
// compiled without fused multiply-adds, on which its sameness on every
// platform rests.
func TestRasterizerDoesNotFuse(t *testing.T) {
	// a*b is 1 - 2^-60 exactly, which rounds to 1; fused with -1 it stays
	a, b := 1+math.Pow(2, -30), 1-math.Pow(2, -30)
	if got := MulAdd(a, b, -1); got != 0 {
		t.Fatalf("a*b + c was fused: %g", got)
	}
}
