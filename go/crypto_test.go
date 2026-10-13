package cbdf

import (
	"bytes"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/hkdf"
	"crypto/hmac"
	"crypto/pbkdf2"
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"math/rand/v2"
	"strings"
	"testing"

	"golang.org/x/text/unicode/norm"
)

func rng() *rand.Rand { return rand.New(rand.NewPCG(20261012, 7)) }

func randBytes(r *rand.Rand, n int) []byte {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(r.Uint32())
	}
	return b
}

func TestSHA256AndHMACMatchGo(t *testing.T) {
	r := rng()
	for n := 0; n < 400; n++ {
		data := randBytes(r, n)
		if got, want := SHA256(data), sha256.Sum256(data); got != want {
			t.Fatalf("SHA-256 of %d bytes: %x, want %x", n, got, want)
		}
		key := randBytes(r, r.IntN(200))
		m := hmac.New(sha256.New, key)
		m.Write(data)
		if got := HMACSHA256(key, data); !bytes.Equal(got[:], m.Sum(nil)) {
			t.Fatalf("HMAC with a key of %d bytes over %d bytes differs", len(key), n)
		}
	}
	big := randBytes(r, 1<<20+13)
	if got, want := SHA256(big), sha256.Sum256(big); got != want {
		t.Fatal("SHA-256 of a megabyte differs")
	}
	// FIPS 180-4, the example of one block
	if got := SHA256([]byte("abc")); hex.EncodeToString(got[:]) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" {
		t.Fatalf("SHA-256(abc) = %x", got)
	}
}

func TestKeyDerivationMatchesGo(t *testing.T) {
	r := rng()
	for _, iter := range []int{1, 2, 3, 100, 4096} {
		for range 6 {
			password, salt := randBytes(r, r.IntN(100)), randBytes(r, r.IntN(40))
			want, err := pbkdf2.Key(sha256.New, string(password), salt, iter, 32)
			if err != nil {
				t.Fatal(err)
			}
			if got := PBKDF2(password, salt, uint32(iter)); !bytes.Equal(got[:], want) {
				t.Fatalf("PBKDF2 with %d iterations differs", iter)
			}
		}
	}
	// RFC 7914 §11, PBKDF2-HMAC-SHA-256 (the first 32 bytes of the 64 it lists)
	if got := PBKDF2([]byte("passwd"), []byte("salt"), 1); hex.EncodeToString(got[:]) != "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc" {
		t.Fatalf("PBKDF2(passwd, salt, 1) = %x", got)
	}
	for range 200 {
		ikm, salt, info := randBytes(r, r.IntN(80)), randBytes(r, r.IntN(80)), randBytes(r, r.IntN(80))
		want, err := hkdf.Key(sha256.New, ikm, salt, string(info), 32)
		if err != nil {
			t.Fatal(err)
		}
		if got := HKDF(ikm, salt, info); !bytes.Equal(got[:], want) {
			t.Fatalf("HKDF differs for a salt of %d bytes", len(salt))
		}
	}
}

// wrap is RFC 3394 key wrap, as the reference writes key slots.
func wrap(kek, key []byte) []byte {
	b, _ := aes.NewCipher(kek)
	n := len(key) / 8
	out := make([]byte, 8+len(key))
	copy(out, []byte{0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6, 0xa6})
	copy(out[8:], key)
	var blk [16]byte
	for j := range 6 {
		for i := 1; i <= n; i++ {
			copy(blk[:8], out[:8])
			copy(blk[8:], out[8*i:8*i+8])
			b.Encrypt(blk[:], blk[:])
			binary.BigEndian.PutUint64(out[:8], binary.BigEndian.Uint64(blk[:8])^uint64(n*j+i))
			copy(out[8*i:], blk[8:])
		}
	}
	return out
}

func TestAESMatchesGo(t *testing.T) {
	r := rng()
	for range 500 {
		var key [32]byte
		var block [16]byte
		copy(key[:], randBytes(r, 32))
		copy(block[:], randBytes(r, 16))
		c, _ := aes.NewCipher(key[:])
		var enc, dec [16]byte
		c.Encrypt(enc[:], block[:])
		c.Decrypt(dec[:], block[:])
		if got := AESEncrypt(key, block); got != enc {
			t.Fatalf("AES-256 encrypt: %x, want %x", got, enc)
		}
		if got := AESDecrypt(key, block); got != dec {
			t.Fatalf("AES-256 decrypt: %x, want %x", got, dec)
		}
		// key wrap: keys of 16 to 64 bytes
		plain := randBytes(r, 8*(2+r.IntN(7)))
		wrapped := wrap(key[:], plain)
		if got, ok := KWUnwrap(key, wrapped); !ok || !bytes.Equal(got, plain) {
			t.Fatalf("a key of %d bytes does not unwrap", len(plain))
		}
		wrapped[r.IntN(len(wrapped))] ^= 1 << r.IntN(8)
		if _, ok := KWUnwrap(key, wrapped); ok {
			t.Fatal("a damaged wrapped key unwraps")
		}
	}
	// FIPS 197 appendix C.3
	var key [32]byte
	var block [16]byte
	for i := range key {
		key[i] = byte(i)
	}
	for i := range block {
		block[i] = byte(i * 0x11)
	}
	if got := AESEncrypt(key, block); hex.EncodeToString(got[:]) != "8ea2b7ca516745bfeafc49904b496089" {
		t.Fatalf("FIPS 197 C.3: %x", got)
	}
	// RFC 3394 §4.6: 256 bits of key data with a 256-bit KEK
	data, _ := hex.DecodeString("00112233445566778899AABBCCDDEEFF000102030405060708090A0B0C0D0E0F")
	wrapped, _ := hex.DecodeString("28C9F404C4B810F4CBCCB35CFB87F8263F5786E2D80ED326CBC7F0E71A99F43BFB988B9B7A02DD21")
	if got, ok := KWUnwrap(key, wrapped); !ok || !bytes.Equal(got, data) {
		t.Fatalf("RFC 3394 §4.6: %x %v", got, ok)
	}
	for _, n := range []int{0, 8, 16, 23, 41, 80} {
		if _, ok := KWUnwrap(key, make([]byte, n)); ok {
			t.Errorf("%d bytes unwrap", n)
		}
	}
}

func TestGCMMatchesGo(t *testing.T) {
	r := rng()
	for i := range 600 {
		var key [32]byte
		var nonce [12]byte
		copy(key[:], randBytes(r, 32))
		copy(nonce[:], randBytes(r, 12))
		plain := randBytes(r, []int{0, 1, 15, 16, 17, 31, 32, 33, 255, 1000}[i%10]+r.IntN(3))
		aad := randBytes(r, []int{0, 1, 16, 20, 100}[i%5])
		b, _ := aes.NewCipher(key[:])
		a, _ := cipher.NewGCM(b)
		sealed := a.Seal(nil, nonce[:], plain, aad)
		ct := sealed[:len(sealed)-16]
		var tag [16]byte
		copy(tag[:], sealed[len(sealed)-16:])
		got, ok := GCMOpen(key, nonce, aad, ct, tag)
		if !ok || !bytes.Equal(got, plain) {
			t.Fatalf("%d bytes with %d of additional data do not open", len(plain), len(aad))
		}
		// one bit off anywhere fails
		bad := append([]byte(nil), ct...)
		badTag, badAAD, badNonce := tag, append([]byte(nil), aad...), nonce
		switch which := r.IntN(4); {
		case which == 0 && len(bad) > 0:
			bad[r.IntN(len(bad))] ^= 1 << r.IntN(8)
		case which == 1 && len(badAAD) > 0:
			badAAD[r.IntN(len(badAAD))] ^= 1 << r.IntN(8)
		case which == 2:
			badNonce[r.IntN(12)] ^= 1 << r.IntN(8)
		default:
			badTag[r.IntN(16)] ^= 1 << r.IntN(8)
		}
		if _, ok := GCMOpen(key, badNonce, badAAD, bad, badTag); ok {
			t.Fatal("damaged data opens")
		}
	}
}

func TestP256MatchesGo(t *testing.T) {
	r := rng()
	curve := ecdh.P256()
	key := func() (*ecdh.PrivateKey, [32]byte) {
		for {
			var k [32]byte
			copy(k[:], randBytes(r, 32))
			if p, err := curve.NewPrivateKey(k[:]); err == nil {
				return p, k
			}
		}
	}
	for range 300 {
		a, ak := key()
		b, _ := key()
		pub, ok := P256Public(ak)
		if !ok || !bytes.Equal(pub[:], a.PublicKey().Bytes()) {
			t.Fatalf("public key of %x: %x, want %x", ak, pub, a.PublicKey().Bytes())
		}
		var peer [65]byte
		copy(peer[:], b.PublicKey().Bytes())
		want, err := a.ECDH(b.PublicKey())
		if err != nil {
			t.Fatal(err)
		}
		if got, rc := P256ECDH(ak, peer); rc != 0 || !bytes.Equal(got[:], want) {
			t.Fatalf("ECDH: %x (%d), want %x", got, rc, want)
		}
		// a point off the curve, as the reference tells
		bad := peer
		bad[1+r.IntN(64)] ^= 1 << r.IntN(8)
		_, gerr := curve.NewPublicKey(bad[:])
		if _, rc := P256ECDH(ak, bad); (rc == 0) != (gerr == nil) {
			t.Fatalf("a changed point: libbdf %d, reference %v", rc, gerr)
		}
	}
	// the ends of the range of private keys
	n, _ := hex.DecodeString("ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551")
	var k [32]byte
	if _, ok := P256Public(k); ok {
		t.Error("the private key 0 has a public key")
	}
	copy(k[:], n)
	if _, ok := P256Public(k); ok {
		t.Error("the private key n has a public key")
	}
	k[31]-- // n − 1
	want, err := curve.NewPrivateKey(k[:])
	if err != nil {
		t.Fatal(err)
	}
	if pub, ok := P256Public(k); !ok || !bytes.Equal(pub[:], want.PublicKey().Bytes()) {
		t.Errorf("the public key of n − 1: %x", pub)
	}
	k = [32]byte{31: 1}
	one, _ := curve.NewPrivateKey(k[:])
	if pub, ok := P256Public(k); !ok || !bytes.Equal(pub[:], one.PublicKey().Bytes()) {
		t.Errorf("the public key of 1 is not the base point: %x", pub)
	}
	// encodings that are not uncompressed points
	var peer [65]byte
	copy(peer[:], one.PublicKey().Bytes())
	for _, first := range []byte{0, 2, 3, 5} {
		peer[0] = first
		if _, rc := P256ECDH(k, peer); rc != -2 {
			t.Errorf("a point that starts with %d: %d", first, rc)
		}
	}
}

// goInsertedJoiner reports whether x/text put a combining grapheme joiner
// into its output: it breaks runs of more than 30 combining marks with one
// (the stream-safe format), which is not NFC and which libbdf does not do.
func goInsertedJoiner(in, out string) bool {
	return strings.Count(out, "͏") != strings.Count(in, "͏")
}

// nfcAlphabet holds characters that normalisation does something with.
var nfcAlphabet = []rune("aeAEoOuUnNcC éÅṩǕ" + // precomposed Latin
	"̸ְִุཱིུ̧̨̛̣̀́̂̃̈̊ͅ" + // marks of many classes
	"かハヴ゚パガ" + // kana and their marks
	"ᄀ하ᅵᆨᇂ가힣각" + // Hangul
	"豈﨎יִשׁÅῺ́̓̈́ʹ;ά 〈" + // singletons and exclusions
	"\U0001d15e\U0001d1bb\U0002f800\U000110ab\U00011099\U000110ba\U0001f600")

func TestNFCMatchesXText(t *testing.T) {
	r := rng()
	n, skipped := 0, 0
	for range 200000 {
		var sb strings.Builder
		for range 1 + r.IntN(12) {
			if r.IntN(8) == 0 {
				sb.WriteRune(rune(r.IntN(0x3000)))
			} else {
				sb.WriteRune(nfcAlphabet[r.IntN(len(nfcAlphabet))])
			}
		}
		s := sb.String()
		want := norm.NFC.String(s)
		if goInsertedJoiner(s, want) {
			skipped++
			continue
		}
		if got := NFC(s); got != want {
			t.Fatalf("NFC(%+q) = %+q, want %+q", s, got, want)
		}
		n++
	}
	// every character on its own
	for c := rune(0); c <= 0x10FFFF; c++ {
		if c >= 0xD800 && c <= 0xDFFF {
			continue
		}
		s := string(c)
		if got, want := NFC(s), norm.NFC.String(s); got != want {
			t.Fatalf("NFC(%+q) = %+q, want %+q", s, got, want)
		}
	}
	// bytes that are not UTF-8 stay, and do not stop what is around them
	for _, s := range []string{"\xff", "é\xc3", "\xe3\x81é", "a\x80́", "\xed\xa0\x80が"} {
		if got, want := NFC(s), norm.NFC.String(s); got != want {
			t.Errorf("NFC(%+q) = %+q, want %+q", s, got, want)
		}
	}
	// a long run of marks out of order is put in order in time in proportion to it
	long := "a" + strings.Repeat("̣́", 200000)
	// (the first dot below composes with the letter: three bytes for three)
	if got := NFC(long); !strings.HasPrefix(got, "\u1ea1\u0323") || !strings.HasSuffix(got, "\u0301\u0301") || len(got) != len(long) {
		t.Errorf("a long run of marks: %d bytes, starting %+q", len(got), got[:6])
	}
	t.Logf("%d strings compared, %d skipped where x/text inserts a joiner", n, skipped)
}

func FuzzNFC(f *testing.F) {
	for _, s := range []string{"", "abc", "é", "パスワード", "각", "\xff́", "ạ̧́"} {
		f.Add(s)
	}
	f.Fuzz(func(t *testing.T, s string) {
		want := norm.NFC.String(s)
		if goInsertedJoiner(s, want) {
			return
		}
		if got := NFC(s); got != want {
			t.Fatalf("NFC(%+q) = %+q, want %+q", s, got, want)
		}
	})
}

func FuzzGCM(f *testing.F) {
	f.Add(make([]byte, 32), make([]byte, 12), []byte("manifest"), []byte("plain text"))
	f.Fuzz(func(t *testing.T, k, n, aad, plain []byte) {
		var key [32]byte
		var nonce [12]byte
		copy(key[:], k)
		copy(nonce[:], n)
		b, _ := aes.NewCipher(key[:])
		a, _ := cipher.NewGCM(b)
		sealed := a.Seal(nil, nonce[:], plain, aad)
		var tag [16]byte
		copy(tag[:], sealed[len(sealed)-16:])
		got, ok := GCMOpen(key, nonce, aad, sealed[:len(sealed)-16], tag)
		if !ok || !bytes.Equal(got, plain) {
			t.Fatalf("%d bytes with %d of additional data do not open", len(plain), len(aad))
		}
	})
}

func FuzzP256(f *testing.F) {
	f.Add(bytes.Repeat([]byte{1}, 32), bytes.Repeat([]byte{2}, 65))
	g, _ := ecdh.P256().NewPrivateKey(append(make([]byte, 31), 1))
	f.Add(bytes.Repeat([]byte{0xfe}, 32), g.PublicKey().Bytes())
	f.Fuzz(func(t *testing.T, k, p []byte) {
		var key [32]byte
		var peer [65]byte
		copy(key[:], k)
		copy(peer[:], p)
		priv, kerr := ecdh.P256().NewPrivateKey(key[:])
		pub, ok := P256Public(key)
		if ok != (kerr == nil) {
			t.Fatalf("private key %x: libbdf %v, reference %v", key, ok, kerr)
		}
		if kerr != nil {
			return
		}
		if !bytes.Equal(pub[:], priv.PublicKey().Bytes()) {
			t.Fatalf("public key of %x differs", key)
		}
		other, perr := ecdh.P256().NewPublicKey(peer[:])
		got, rc := P256ECDH(key, peer)
		if (rc == 0) != (perr == nil) {
			t.Fatalf("peer %x: libbdf %d, reference %v", peer, rc, perr)
		}
		if perr == nil {
			if want, _ := priv.ECDH(other); !bytes.Equal(got[:], want) {
				t.Fatalf("ECDH differs: %x, want %x", got, want)
			}
		}
	})
}
