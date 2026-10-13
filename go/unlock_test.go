package cbdf

import (
	"bytes"
	"compress/flate"
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/pbkdf2"
	"crypto/sha256"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"reflect"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/shibukawa/bdf"
)

func status(err error) int {
	var ce *Error
	if errors.As(err, &ce) {
		return ce.Status
	}
	return -1
}

// compareOpened compares the manifest and every part of a document both
// readers have open.
func compareOpened(r *bdf.Reader, d *Doc) string {
	if diff := firstDiff(dumpManifestGo(r.Manifest), dumpManifestC(d)); diff != "" {
		return "manifest: " + diff
	}
	for _, e := range r.Manifest.Parts {
		want, werr := r.Part(e.H)
		got, gerr := d.Part(e.H)
		if (werr == nil) != (gerr == nil) {
			return fmt.Sprintf("part %s: reference %v, libbdf %v", e.H, werr, gerr)
		}
		if !bytes.Equal(want, got) {
			return fmt.Sprintf("part %s: %d bytes differ from %d", e.H, len(got), len(want))
		}
	}
	return ""
}

// TestUnlockPasswordMatchesReference encrypts every test document with a
// password and opens it with both readers.
func TestUnlockPasswordMatchesReference(t *testing.T) {
	// the same password as the keyboard composes it and as it decomposes
	passwords := [][2]string{{"pass", "pass"}, {"パスワードガ", "パスワードガ"}, {"é à ñ", "é à ñ"}, {"\U0001f511 key", "\U0001f511 key"}}
	docs, partsRead := 0, 0
	for i, f := range documents(t) {
		doc, cdoc, ok := openBoth(t, f)
		if !ok {
			continue
		}
		cdoc.Close()
		pw := passwords[i%len(passwords)]
		lock, err := bdf.NewPasswordLock(pw[0], 1000+i)
		if err != nil {
			t.Fatal(err)
		}
		doc.Lock = lock
		var buf bytes.Buffer
		if err := doc.WriteSingle(&buf); err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		r, err := bdf.OpenSingle(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
		if err != nil {
			t.Fatal(err)
		}
		d, err := OpenMemory(buf.Bytes())
		if err != nil {
			t.Fatalf("%s: %v", f, err)
		}
		name := filepath.Base(f)
		if !d.Locked() || !d.Encrypted() || len(d.Views()) != 0 {
			t.Errorf("%s: an encrypted document opens unlocked", name)
		}
		if diff := compareOpened(r, d); diff != "" { // locked: the stored manifest and the sealed parts
			t.Errorf("%s locked: %s", name, diff)
		}
		if err := d.Unlock("wrong " + pw[0]); status(err) != ErrPassword {
			t.Errorf("%s: a wrong password: %v", name, err)
		}
		if err := d.UnlockECDH([32]byte{31: 1}); status(err) != ErrPassword {
			t.Errorf("%s: a key for a document that has no slot for one: %v", name, err)
		}
		if !d.Locked() {
			t.Errorf("%s: unlocked by a wrong password", name)
		}
		if err := r.Unlock(pw[1]); err != nil {
			t.Fatalf("%s: reference: %v", name, err)
		}
		if err := d.Unlock(pw[1]); err != nil {
			t.Errorf("%s: %v", name, err)
			d.Close()
			continue
		}
		if d.Locked() || !d.Encrypted() {
			t.Errorf("%s: locked %v encrypted %v after the unlock", name, d.Locked(), d.Encrypted())
		}
		if err := d.Unlock(pw[1]); status(err) != ErrArg {
			t.Errorf("%s: a second unlock: %v", name, err)
		}
		if diff := compareOpened(r, d); diff != "" {
			t.Errorf("%s unlocked: %s", name, diff)
		}
		docs++
		partsRead += len(r.Manifest.Parts)
		d.Close()
	}
	t.Logf("%d documents encrypted and opened, %d parts compared", docs, partsRead)
}

// TestUnlockDemoEncrypted opens the encrypted test document of the reference
// (600,000 iterations) and reads its text.
func TestUnlockDemoEncrypted(t *testing.T) {
	file := filepath.Join(testdataDir(t), "demo-encrypted.bdf")
	data, err := os.ReadFile(file)
	if err != nil {
		t.Fatal(err)
	}
	r, _ := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
	d, err := OpenMemory(data)
	if err != nil {
		t.Fatal(err)
	}
	defer d.Close()
	if err := r.Unlock("demo-パスワード"); err != nil {
		t.Fatal(err)
	}
	if err := d.Unlock("demo-パスワード"); err != nil {
		t.Fatal(err)
	}
	if diff := compareOpened(r, d); diff != "" {
		t.Error(diff)
	}
	doc, err := r.ToDocument()
	if err != nil {
		t.Fatal(err)
	}
	want, err := doc.SearchText()
	if err != nil {
		t.Fatal(err)
	}
	got, err := d.SearchText()
	if err != nil {
		t.Fatal(err)
	}
	if len(want.Views) != len(got) || len(got) == 0 {
		t.Fatalf("%d views of text, reference %d", len(got), len(want.Views))
	}
	for i, v := range want.Views {
		for k, p := range v.Pages {
			if got[i].Pages[k].Text != p.Text {
				t.Errorf("view %s page %d: text differs", v.ID, p.Page)
			}
		}
	}
}

// The key pair the test segments of the reference were sealed for
// (packages/core/test/segments.test.mjs: made for those tests only).
const readerD = "_loj2CexGXt2g1YkkUeTui4c9IeJ-BtcOYRzVL81Zi4"

func readerKey(t testing.TB) (*ecdh.PrivateKey, [32]byte) {
	t.Helper()
	b, err := base64.RawURLEncoding.DecodeString(readerD)
	if err != nil {
		t.Fatal(err)
	}
	priv, err := ecdh.P256().NewPrivateKey(b)
	if err != nil {
		t.Fatal(err)
	}
	var k [32]byte
	copy(k[:], b)
	return priv, k
}

// checkMerged compares the pages of a document assembled from segments with
// those of the whole document: the same layers, and every part they need.
func checkMerged(t *testing.T, name string, d *Doc, whole *bdf.Reader, viewID string, from, to int) {
	t.Helper()
	views := d.Views()
	for vi, v := range whole.Manifest.Views {
		if views[vi].ID != v.ID || len(views[vi].Pages) != len(v.Pages) {
			t.Fatalf("%s: view %d is %q with %d pages", name, vi, views[vi].ID, len(views[vi].Pages))
		}
		for pi, p := range v.Pages {
			got := views[vi].Pages[pi]
			held := v.ID == viewID && pi >= from && pi < to
			if !held {
				if len(got.Layers) != 0 {
					t.Errorf("%s: page %d of %s has layers before its segment came", name, pi, v.ID)
				}
				continue
			}
			if len(got.Layers) != len(p.Layers) || got.W != p.W || got.H != p.H {
				t.Errorf("%s: page %d of %s: %d layers, the document has %d", name, pi, v.ID, len(got.Layers), len(p.Layers))
				continue
			}
			for li, l := range p.Layers {
				if got.Layers[li].Role != l.Role || bdf.Hash(got.Layers[li].Obj) != l.Obj {
					t.Errorf("%s: page %d of %s layer %d differs", name, pi, v.ID, li)
				}
				// the object and all it draws read from the segments as from the document
				var walk func(h bdf.Hash)
				seen := map[bdf.Hash]bool{}
				walk = func(h bdf.Hash) {
					if seen[h] {
						return
					}
					seen[h] = true
					want, werr := whole.Part(h)
					gotPart, gerr := d.Part(h)
					if werr != nil || gerr != nil || !bytes.Equal(want, gotPart) {
						t.Errorf("%s: part %s of page %d: reference %v, libbdf %v", name, h, pi, werr, gerr)
						return
					}
					if e, _ := whole.Entry(h); e.T == bdf.PartObject {
						if o, err := bdf.DecodeObject(want); err == nil {
							for _, dep := range o.Deps() {
								walk(dep)
							}
						}
					}
				}
				walk(l.Obj)
			}
		}
	}
}

// TestSegmentsOfReference opens the three test segments of the reference
// with the key they were sealed for and puts them together.
func TestSegmentsOfReference(t *testing.T) {
	dir := testdataDir(t)
	priv, key := readerKey(t)
	whole, err := bdf.OpenSingleFile(filepath.Join(dir, "epub", "basic.bdf"))
	if err != nil {
		t.Fatal(err)
	}
	defer whole.Close()
	var base *Doc
	held := 0
	for _, name := range []string{"basic-1.bdf", "basic-4.bdf", "basic-7.bdf"} {
		data, err := os.ReadFile(filepath.Join(dir, "segments", name))
		if err != nil {
			t.Fatal(err)
		}
		r, _ := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		d, err := OpenMemory(data)
		if err != nil {
			t.Fatal(err)
		}
		if err := d.Unlock("x"); status(err) != ErrPassword {
			t.Errorf("%s: a password for a document sealed for a key: %v", name, err)
		}
		if err := d.UnlockECDH([32]byte{31: 7}); status(err) != ErrPassword {
			t.Errorf("%s: another key: %v", name, err)
		}
		if err := r.UnlockECDH(priv); err != nil {
			t.Fatal(err)
		}
		if err := d.UnlockECDH(key); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if diff := compareOpened(r, d); diff != "" {
			t.Errorf("%s: %s", name, diff)
		}
		seg, ok := d.Segment()
		if !ok || int(seg.From) != held || seg.To <= seg.From || seg.To > seg.From+3 {
			t.Fatalf("%s: segment %+v, %d pages held", name, seg, held)
		}
		if base == nil {
			base = d
		} else if err := base.AddSegment(d); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		held = int(seg.To)
		checkMerged(t, name, base, whole, seg.View, 0, held)
	}
	defer base.Close()
	// the text of the pages that came is that of the document's pages
	doc, err := whole.ToDocument()
	if err != nil {
		t.Fatal(err)
	}
	resolve := resolver(doc)
	for pi := 0; pi < held; pi++ {
		for _, l := range doc.Views[0].Pages[pi].Layers {
			want, _ := bdf.ExtractText(resolve(l.Obj), resolve)
			got, _, err := base.Text(l.Obj)
			if err != nil {
				t.Fatalf("page %d: %v", pi, err)
			}
			if d := compareRuns(want, got, testdataMode()); d != "" {
				t.Errorf("page %d: %s", pi, d)
			}
		}
	}
}

// TestSegmentsMatchReference cuts documents into segments sealed for a fresh
// key each, as a server answers requests, and puts them together.
func TestSegmentsMatchReference(t *testing.T) {
	r0 := rng()
	docs, segments := 0, 0
	for i, f := range documents(t) {
		if i%4 != 0 {
			continue
		}
		whole, err := bdf.OpenSingleFile(f)
		if err != nil {
			t.Fatal(err)
		}
		if whole.Encrypted() {
			whole.Close()
			continue
		}
		pages := true
		for _, v := range whole.Manifest.Views {
			pages = pages && v.Kind != bdf.ViewSheet && v.Play == nil
		}
		if !pages || len(whole.Manifest.Views) == 0 || len(whole.Manifest.Views[0].Pages) == 0 {
			whole.Close()
			continue
		}
		name := filepath.Base(f)
		v := whole.Manifest.Views[0]
		size := 1 + i%3
		var base *Doc
		var have []bdf.Segment
		// the segments come out of order: the last first
		var order []int
		for from := 0; from < len(v.Pages); from += size {
			order = append([]int{from}, order...)
		}
		for _, from := range order {
			var k [32]byte
			var priv *ecdh.PrivateKey
			for priv == nil {
				copy(k[:], randBytes(r0, 32))
				priv, _ = ecdh.P256().NewPrivateKey(k[:])
			}
			lock, err := bdf.NewECDHLock(priv.PublicKey())
			if err != nil {
				t.Fatal(err)
			}
			seg := bdf.SegmentAt(v, from, size)
			var buf bytes.Buffer
			if err := whole.WriteSegment(&buf, bdf.SegmentOptions{Segment: seg, Have: have, Lock: lock}); err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			d, err := OpenMemory(buf.Bytes())
			if err != nil {
				t.Fatalf("%s: %v", name, err)
			}
			if pub, ok := PublicKey(k); !ok || !bytes.Equal(pub[:], priv.PublicKey().Bytes()) {
				t.Fatalf("%s: public key differs", name)
			}
			if err := d.UnlockECDH(k); err != nil {
				t.Fatalf("%s pages %d–%d: %v", name, seg.From, seg.To, err)
			}
			if base == nil {
				base = d
			} else if err := base.AddSegment(d); err != nil {
				t.Fatalf("%s pages %d–%d: %v", name, seg.From, seg.To, err)
			}
			have = append(have, seg)
			segments++
		}
		checkMerged(t, name, base, whole, v.ID, 0, len(v.Pages))
		base.Close()
		whole.Close()
		docs++
	}
	t.Logf("%d documents put together from %d segments", docs, segments)
}

// outerDoc writes a single-file document of a manifest as it is, with the
// encrypted flag, and parts after it.
func outerDoc(manifest string, parts []byte) []byte {
	b := append([]byte("bdf\x00"), 1, 0, 1, 0)
	b = binary.LittleEndian.AppendUint64(b, 32)
	b = binary.LittleEndian.AppendUint64(b, uint64(len(manifest)))
	b = append(b, make([]byte, 8)...)
	b = append(b, manifest...)
	return append(b, parts...)
}

// TestUnlockRefusals gives both readers key slots and sealed manifests a
// reader refuses: the two must refuse the same ones, as malformed and not as
// a wrong password.
func TestUnlockRefusals(t *testing.T) {
	salt := base64.StdEncoding.EncodeToString(make([]byte, 16))
	key := base64.StdEncoding.EncodeToString(make([]byte, 40))
	slot := func(extra string) string {
		return `{"type":"password","kdf":"PBKDF2-SHA256","iter":1,"salt":"` + salt + `","key":"` + key + `"` + extra + `}`
	}
	g, _ := ecdh.P256().NewPrivateKey(append(make([]byte, 31), 1))
	epk := base64.StdEncoding.EncodeToString(g.PublicKey().Bytes())
	ecdhSlot := `{"type":"ecdh","kdf":"HKDF-SHA256","crv":"P-256","epk":"` + epk + `","key":"` + key + `"}`
	wrap := func(cipher, keys string) string {
		return `{"bdf":1,"encryption":{"cipher":"` + cipher + `","keys":[` + keys + `],"manifest":{"part":"abababababababababababababababab","enc":"identity"}},"parts":[]}`
	}
	many := ""
	for i := range 17 {
		if i > 0 {
			many += ","
		}
		many += ecdhSlot
	}
	for _, c := range []struct {
		name, manifest string
		password, ecdh int // the status each unlock must give
	}{
		{"wrong password", wrap("A256GCM", slot("")), ErrPassword, ErrPassword},
		{"unknown cipher", wrap("A128GCM", slot("")), ErrFormat, ErrFormat},
		{"unknown kdf", wrap("A256GCM", `{"type":"password","kdf":"scrypt","iter":1,"salt":"`+salt+`","key":"`+key+`"}`), ErrFormat, ErrPassword},
		{"no iterations", wrap("A256GCM", `{"type":"password","kdf":"PBKDF2-SHA256","iter":0,"salt":"`+salt+`","key":"`+key+`"}`), ErrFormat, ErrPassword},
		{"too many iterations", wrap("A256GCM", `{"type":"password","kdf":"PBKDF2-SHA256","iter":10000001,"salt":"`+salt+`","key":"`+key+`"}`), ErrFormat, ErrPassword},
		{"slots of too many iterations together", wrap("A256GCM", slot("")+`,{"type":"password","kdf":"PBKDF2-SHA256","iter":10000000,"salt":"`+salt+`","key":"`+key+`"}`), ErrFormat, ErrPassword},
		{"a key of another length", wrap("A256GCM", `{"type":"password","kdf":"PBKDF2-SHA256","iter":1,"salt":"`+salt+`","key":"`+base64.StdEncoding.EncodeToString(make([]byte, 24))+`"}`), ErrPassword, ErrPassword},
		{"no slots", wrap("A256GCM", ""), ErrPassword, ErrPassword},
		{"an ecdh slot", wrap("A256GCM", ecdhSlot), ErrPassword, ErrPassword},
		{"unknown curve", wrap("A256GCM", `{"type":"ecdh","kdf":"HKDF-SHA256","crv":"P-384","epk":"`+epk+`","key":"`+key+`"}`), ErrPassword, ErrFormat},
		{"unknown ecdh kdf", wrap("A256GCM", `{"type":"ecdh","kdf":"X963","crv":"P-256","epk":"`+epk+`","key":"`+key+`"}`), ErrPassword, ErrFormat},
		{"a point off the curve", wrap("A256GCM", `{"type":"ecdh","kdf":"HKDF-SHA256","crv":"P-256","epk":"`+base64.StdEncoding.EncodeToString(append([]byte{4}, make([]byte, 64)...))+`","key":"`+key+`"}`), ErrPassword, ErrFormat},
		{"a short point", wrap("A256GCM", `{"type":"ecdh","kdf":"HKDF-SHA256","crv":"P-256","epk":"BAAA","key":"`+key+`"}`), ErrPassword, ErrFormat},
		{"17 ecdh slots", wrap("A256GCM", many), ErrPassword, ErrFormat},
	} {
		data := outerDoc(c.manifest, nil)
		for _, how := range []string{"password", "ecdh"} {
			r, err := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
			if err != nil {
				t.Fatalf("%s: reference: %v", c.name, err)
			}
			d, err := OpenMemory(data)
			if err != nil {
				t.Fatalf("%s: %v", c.name, err)
			}
			var werr, gerr error
			want := c.password
			if how == "password" {
				werr, gerr = r.Unlock("password"), d.Unlock("password")
			} else {
				priv, k := readerKey(t)
				werr, gerr = r.UnlockECDH(priv), d.UnlockECDH(k)
				want = c.ecdh
			}
			var fe *bdf.FormatError
			ref := ErrPassword
			if errors.As(werr, &fe) {
				ref = ErrFormat
			} else if !errors.Is(werr, bdf.ErrWrongPassword) && !errors.Is(werr, bdf.ErrWrongKey) {
				t.Errorf("%s by %s: reference %v", c.name, how, werr)
			}
			if status(gerr) != want || ref != want {
				t.Errorf("%s by %s: libbdf %v, reference %v", c.name, how, gerr, werr)
			}
			d.Close()
		}
	}
}

// TestSealedDamage changes one byte of an encrypted document at a time: the
// two readers must agree on what still opens and reads.
func TestSealedDamage(t *testing.T) {
	doc, small, ok := openBoth(t, filepath.Join(testdataDir(t), "pdf", "cairo-cff.bdf"))
	if !ok {
		t.Fatal("the document is encrypted")
	}
	small.Close()
	lock, _ := bdf.NewPasswordLock("pass", 10)
	doc.Lock = lock
	var buf bytes.Buffer
	if err := doc.WriteSingle(&buf); err != nil {
		t.Fatal(err)
	}
	clean := buf.Bytes()
	changed, refused := 0, 0
	for at := 32; at < len(clean); at += 1 + len(clean)/700 {
		data := append([]byte(nil), clean...)
		data[at] ^= 0x20
		r, werr := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		d, gerr := OpenMemory(data)
		// a change that makes "H" of "h" leaves a member libbdf does not
		// see and encoding/json does (see foldedMembers): the two then
		// read different documents, and may differ
		folded := foldedMembers(data)
		if gerr == nil && werr != nil {
			if folded {
				d.Close()
				refused++
				continue
			}
			t.Fatalf("byte %d: libbdf opens what the reference refuses: %v", at, werr)
		}
		if gerr != nil {
			if werr == nil {
				// the stored manifest was changed into JSON only encoding/json takes
				var m map[string]any
				if json.Unmarshal(data[32:], &m) == nil {
					t.Logf("byte %d: libbdf refuses a manifest the reference takes: %v", at, gerr)
				}
			}
			refused++
			continue
		}
		werr, gerr = r.Unlock("pass"), d.Unlock("pass")
		if (werr == nil) != (gerr == nil) {
			if folded && gerr != nil {
				d.Close()
				refused++
				continue
			}
			t.Fatalf("byte %d: unlock: reference %v, libbdf %v", at, werr, gerr)
		}
		if werr == nil {
			if diff := compareOpened(r, d); diff != "" {
				t.Fatalf("byte %d: %s", at, diff)
			}
		} else {
			refused++
		}
		changed++
		d.Close()
	}
	t.Logf("%d changes tried, %d refused by both", changed, refused)
}

// ---- Fuzzing ----------------------------------------------------------------

// slow writes an input a fuzz function takes more than two seconds over to
// the directory LIBBDF_FUZZ_SLOW_DIR names, when it is set: a fuzzer that
// stops making progress is stuck on one such input, and says nothing of it.
// Call the function it returns when the input is done.
func slow(data ...[]byte) func() {
	dir := os.Getenv("LIBBDF_FUZZ_SLOW_DIR")
	if dir == "" {
		return func() {}
	}
	timer := time.AfterFunc(2*time.Second, func() {
		sum := sha256.Sum256(bytes.Join(data, nil))
		for i, d := range data {
			os.WriteFile(filepath.Join(dir, fmt.Sprintf("%x-%d", sum[:6], i)), d, 0o644)
		}
	})
	return func() { timer.Stop() }
}

// unlockSeeds are small documents of the reference encrypted with the
// password "pass" or sealed for the test key, whole and as segments.
func unlockSeeds(t testing.TB) [][]byte {
	t.Helper()
	priv, _ := readerKey(t)
	var out [][]byte
	for i, f := range documents(t) {
		data, err := os.ReadFile(f)
		if err != nil || len(data) > 24<<10 {
			continue
		}
		r, err := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		if err != nil || r.Encrypted() {
			continue
		}
		doc, err := r.ToDocument()
		if err != nil {
			continue
		}
		var lock *bdf.Lock
		if i%2 == 0 {
			lock, err = bdf.NewPasswordLock("pass", 1+i%5)
		} else {
			lock, err = bdf.NewECDHLock(priv.PublicKey())
		}
		if err != nil {
			t.Fatal(err)
		}
		doc.Lock = lock
		var buf bytes.Buffer
		if err := doc.WriteSingle(&buf); err != nil {
			t.Fatal(err)
		}
		out = append(out, buf.Bytes())
		if v := r.Manifest.Views; len(v) > 0 && len(v[0].Pages) > 1 && v[0].Kind != bdf.ViewSheet && v[0].Play == nil {
			lock, _ := bdf.NewECDHLock(priv.PublicKey())
			var seg bytes.Buffer
			if err := r.WriteSegment(&seg, bdf.SegmentOptions{Segment: bdf.SegmentAt(v[0], 1, 1), Lock: lock}); err == nil {
				out = append(out, seg.Bytes())
			}
		}
	}
	return out
}

// TestWriteUnlockSeeds writes the seeds of the libFuzzer target unlock_fuzz
// (two documents after the length of the first) when LIBBDF_UNLOCK_SEEDS
// names a directory:
//
//	LIBBDF_UNLOCK_SEEDS=$PWD/fuzz/corpus/unlock go test -C go -run TestWriteUnlockSeeds .
func TestWriteUnlockSeeds(t *testing.T) {
	dir := os.Getenv("LIBBDF_UNLOCK_SEEDS")
	if dir == "" {
		t.Skip("LIBBDF_UNLOCK_SEEDS is not set")
	}
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	seeds := unlockSeeds(t)
	pair := func(a, b []byte) []byte {
		return append(append(binary.LittleEndian.AppendUint32(nil, uint32(len(a))), a...), b...)
	}
	n := 0
	write := func(data []byte) {
		if err := os.WriteFile(filepath.Join(dir, fmt.Sprintf("seed-%03d", n)), data, 0o644); err != nil {
			t.Fatal(err)
		}
		n++
	}
	for _, s := range seeds {
		write(pair(s, nil))
	}
	// segments of one document, one after the other
	priv, _ := readerKey(t)
	whole, err := bdf.OpenSingleFile(filepath.Join(testdataDir(t), "epub", "basic.bdf"))
	if err != nil {
		t.Fatal(err)
	}
	defer whole.Close()
	v := whole.Manifest.Views[0]
	var segs [][]byte
	for from := 0; from < len(v.Pages); from += 2 {
		lock, _ := bdf.NewECDHLock(priv.PublicKey())
		var b bytes.Buffer
		if err := whole.WriteSegment(&b, bdf.SegmentOptions{Segment: bdf.SegmentAt(v, from, 2), Lock: lock}); err != nil {
			t.Fatal(err)
		}
		segs = append(segs, b.Bytes())
	}
	for i := 1; i < len(segs); i++ {
		write(pair(segs[0], segs[i]))
	}
	t.Logf("%d seeds written to %s", n, dir)
}

// unlockClass sorts what an unlock gave: 0 opened, 1 no slot opens (a wrong
// password or key), 2 malformed (the reference says so with a FormatError,
// or with the error of what it read the sealed manifest with).
func unlockClass(t *testing.T, err error) int {
	var ce *Error
	switch {
	case err == nil:
		return 0
	case errors.Is(err, bdf.ErrWrongPassword), errors.Is(err, bdf.ErrWrongKey):
		return 1
	case errors.As(err, &ce):
		switch ce.Status {
		case ErrPassword:
			return 1
		case ErrFormat:
			return 2
		}
		t.Fatalf("unlock: %v", err)
	}
	return 2
}

// memberNames are the names of the members of a manifest, as written.
var memberNames = func() map[string]bool {
	names := map[string]bool{}
	seen := map[reflect.Type]bool{}
	var walk func(t reflect.Type)
	walk = func(t reflect.Type) {
		for t.Kind() == reflect.Pointer || t.Kind() == reflect.Slice || t.Kind() == reflect.Map || t.Kind() == reflect.Array {
			t = t.Elem()
		}
		if t.Kind() != reflect.Struct || seen[t] {
			return
		}
		seen[t] = true
		for i := range t.NumField() {
			f := t.Field(i)
			if name, _, _ := strings.Cut(f.Tag.Get("json"), ","); name != "" && name != "-" {
				names[name] = true
			}
			walk(f.Type)
		}
	}
	walk(reflect.TypeFor[bdf.Manifest]())
	return names
}()

var memberPattern = regexp.MustCompile(`"([A-Za-z]+)"\s*:`)

// foldedMembers reports whether the stored manifest of a document has a
// member under a name encoding/json takes for another: it matches names
// without regard to case ("H" for "h"), which neither the format nor libbdf
// does. Such a document means different things to the two readers.
func foldedMembers(data []byte) bool {
	if len(data) < 32 {
		return false
	}
	off, n := binary.LittleEndian.Uint64(data[8:]), binary.LittleEndian.Uint64(data[16:])
	if off > uint64(len(data)) || n > uint64(len(data))-off {
		return false
	}
	m := data[off : off+n]
	if data[24] == 1 {
		m, _ = io.ReadAll(io.LimitReader(flate.NewReader(bytes.NewReader(m)), 1<<24))
	}
	for _, g := range memberPattern.FindAllSubmatch(m, -1) {
		if name := string(g[1]); !memberNames[name] {
			for known := range memberNames {
				if strings.EqualFold(known, name) {
					return true
				}
			}
		}
	}
	return false
}

// compareUnlock unlocks both readers with the password "pass" and then with
// the test key, and compares what they come to. data is the document.
func compareUnlock(t *testing.T, data []byte, r *bdf.Reader, d *Doc) {
	priv, key := readerKey(t)
	for _, s := range r.Manifest.Encryption.Keys {
		if s.Iter > 4096 { // seconds of key derivation teach a fuzzer nothing
			return
		}
	}
	if len(r.Manifest.Encryption.Keys) > 64 {
		return
	}
	// a slot or a part under a folded name is there for the reference alone
	folded := foldedMembers(data)
	werr, gerr := r.Unlock("pass"), d.Unlock("pass")
	if w, g := unlockClass(t, werr), unlockClass(t, gerr); w != g {
		if folded && g != 0 {
			return
		}
		t.Fatalf("password: reference %v, libbdf %v", werr, gerr)
	}
	if werr != nil {
		werr, gerr = r.UnlockECDH(priv), d.UnlockECDH(key)
		if w, g := unlockClass(t, werr), unlockClass(t, gerr); w != g {
			if folded && g != 0 {
				return
			}
			t.Fatalf("key: reference %v, libbdf %v", werr, gerr)
		}
	}
	if werr != nil {
		if !d.Locked() {
			t.Fatal("unlocked by a failure")
		}
		return
	}
	if d.Locked() {
		t.Fatal("still locked")
	}
	if diff := firstDiff(dumpManifestGo(r.Manifest), dumpManifestC(d)); diff != "" {
		t.Fatalf("manifest: %s", diff)
	}
	for i, e := range r.Manifest.Parts {
		if i == 64 {
			break
		}
		want, werr := r.Part(e.H)
		got, gerr := d.Part(e.H)
		if (werr == nil) != (gerr == nil) {
			t.Fatalf("part %s: reference %v, libbdf %v", e.H, werr, gerr)
		}
		if werr == nil && !bytes.Equal(want, got) {
			t.Fatalf("part %s: %d bytes differ from %d", e.H, len(got), len(want))
		}
	}
}

// FuzzUnlock opens and unlocks the same bytes with both readers: the key
// slots and the stored manifest are what a fuzzer reaches here (what is
// sealed fails its tag when changed; see FuzzSealed for that).
func FuzzUnlock(f *testing.F) {
	for _, s := range unlockSeeds(f) {
		f.Add(s)
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		defer slow(data)()
		d, cerr := OpenMemory(data)
		if cerr != nil {
			return
		}
		defer d.Close()
		r, gerr := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		if gerr != nil {
			if foldedMembers(data) { // a member libbdf does not see
				return
			}
			t.Fatalf("libbdf opened what the reference rejects: %v", gerr)
		}
		if r.Locked() != d.Locked() {
			t.Fatalf("locked %v vs %v", r.Locked(), d.Locked())
		}
		if !r.Locked() {
			return
		}
		compareUnlock(t, data, r, d)
	})
}

// kwWrap is AES key wrap (RFC 3394), which the standard library lacks.
func kwWrap(kek, key []byte) []byte {
	block, _ := aes.NewCipher(kek)
	n := len(key) / 8
	a := bytes.Repeat([]byte{0xa6}, 8)
	r := append([]byte(nil), key...)
	var b [16]byte
	for j := 0; j < 6; j++ {
		for i := 0; i < n; i++ {
			copy(b[:8], a)
			copy(b[8:], r[8*i:8*i+8])
			block.Encrypt(b[:], b[:])
			copy(a, b[:8])
			binary.BigEndian.PutUint64(a, binary.BigEndian.Uint64(a)^uint64(n*j+i+1))
			copy(r[8*i:], b[8:])
		}
	}
	return append(a, r...)
}

// sealedDoc writes an encrypted document around a sealed manifest as it is
// given (not as a writer would make it) and two sealed parts, which the
// manifest may name as "sealed": the parts 01.. and 02.. of the document,
// sealed as the outer parts a1.. and a2... The password is "pass".
func sealedDoc(inner, part1, part2 []byte, deflated bool) []byte {
	key := bytes.Repeat([]byte{0x42}, 32)
	salt := bytes.Repeat([]byte{7}, 16)
	kek, _ := pbkdf2.Key(sha256.New, "pass", salt, 1, 32)
	block, _ := aes.NewCipher(key)
	aead, _ := cipher.NewGCM(block)
	nonce := make([]byte, 12)
	seal := func(plain, aad []byte) []byte {
		nonce[11]++
		return aead.Seal(append([]byte(nil), nonce...), nonce, plain, aad)
	}
	enc := "identity"
	if deflated {
		enc = "deflate-raw"
		var buf bytes.Buffer
		zw, _ := flate.NewWriter(&buf, flate.BestSpeed)
		zw.Write(inner)
		zw.Close()
		inner = buf.Bytes()
	}
	name := func(b byte) []byte { return append([]byte{b}, make([]byte, 15)...) }
	sealed := [][]byte{seal(inner, []byte("manifest")), seal(part1, name(1)), seal(part2, name(2))}
	names := []string{"a0", "a1", "a2"}
	var parts []byte
	entries := ""
	for i, s := range sealed {
		if i > 0 {
			entries += ","
		}
		entries += fmt.Sprintf(`{"h":"%s%030d","t":"sealed","enc":"identity","len":%d,"size":%d,"off":%d}`, names[i], 0, len(s), len(s), len(parts))
		parts = append(parts, s...)
	}
	outer := fmt.Sprintf(`{"bdf":1,"encryption":{"cipher":"A256GCM","keys":[{"type":"password","kdf":"PBKDF2-SHA256","iter":1,"salt":"%s","key":"%s"}],"manifest":{"part":"a0%030d","enc":"%s"}},"parts":[%s]}`,
		base64.StdEncoding.EncodeToString(salt), base64.StdEncoding.EncodeToString(kwWrap(kek, key)), 0, enc, entries)
	return outerDoc(outer, parts)
}

// innerSeeds are sealed manifests for sealedDoc.
func innerSeeds() []string {
	obj := `{"h":"01000000000000000000000000000000","t":"obj","enc":"identity","size":4,"sealed":"a1000000000000000000000000000000"}`
	return []string{
		`{"bdf":1,"opset":1,"views":[{"id":"p","kind":"fixed","pages":[{"w":10,"h":10,"layers":[{"role":"body","obj":"01000000000000000000000000000000"}]}]}],"parts":[` + obj + `]}`,
		`{"bdf":1,"parts":[` + obj + `,{"h":"02000000000000000000000000000000","t":"img","enc":"deflate-raw","size":64,"sealed":"a2000000000000000000000000000000"}]}`,
		`{"bdf":1,"segment":{"view":"p","from":0,"to":1},"views":[{"id":"p","kind":"flow","pages":[{"w":1,"h":1,"layers":[]}]}],"parts":[]}`,
		`{"bdf":1,"parts":[{"h":"01000000000000000000000000000000","t":"obj","enc":"identity","size":4,"sealed":"a3000000000000000000000000000000"}]}`,
		`{"bdf":1,"encryption":{"cipher":"A256GCM","keys":[],"manifest":{"part":"a0000000000000000000000000000000","enc":"identity"}},"parts":[]}`,
		`{"bdf":1,"meta":{"dc":{"title":["t"]}},"parts":[{"h":"01000000000000000000000000000000","t":"obj","size":4}]}`,
	}
}

// TestSealedManifests gives both readers sealed manifests no writer makes.
func TestSealedManifests(t *testing.T) {
	part := []byte("BOBJ")
	for i, inner := range innerSeeds() {
		for _, deflated := range []bool{false, true} {
			data := sealedDoc([]byte(inner), part, part, deflated)
			r, err := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
			if err != nil {
				t.Fatalf("seed %d: reference: %v", i, err)
			}
			d, err := OpenMemory(data)
			if err != nil {
				t.Fatalf("seed %d: %v", i, err)
			}
			compareUnlock(t, data, r, d)
			if i < 3 && d.Locked() {
				t.Errorf("seed %d does not open", i)
			}
			if i >= 3 && i < 5 && !d.Locked() {
				t.Errorf("seed %d opens", i)
			}
			d.Close()
		}
	}
}

// FuzzSealed seals what the fuzzer makes, a manifest and two parts, into an
// encrypted document both readers unlock: the sealed manifest and the parts
// behind the seal, which FuzzUnlock cannot reach.
func FuzzSealed(f *testing.F) {
	for _, s := range innerSeeds() {
		f.Add([]byte(s), []byte("BOBJ"), []byte{0x01, 0x00, 0x00, 0xff, 0xff}, false)
		f.Add([]byte(s), []byte("BOBJ"), []byte{0x01, 0x00, 0x00, 0xff, 0xff}, true)
	}
	f.Fuzz(func(t *testing.T, inner, part1, part2 []byte, deflated bool) {
		defer slow(inner, part1, part2)()
		data := sealedDoc(inner, part1, part2, deflated)
		d, cerr := OpenMemory(data)
		if cerr != nil {
			t.Fatalf("the document around it does not open: %v", cerr)
		}
		defer d.Close()
		r, gerr := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		if gerr != nil {
			t.Fatal(gerr)
		}
		// libbdf may refuse a manifest the reference takes (see FuzzDoc), not
		// the other way round; what both take must read the same
		werr, cerr := r.Unlock("pass"), d.Unlock("pass")
		if cerr != nil {
			if unlockClass(t, cerr) != 2 {
				t.Fatalf("libbdf: %v (reference %v)", cerr, werr)
			}
			return
		}
		if werr != nil {
			t.Fatalf("libbdf unlocked what the reference rejects: %v", werr)
		}
		if diff := firstDiff(dumpManifestGo(r.Manifest), dumpManifestC(d)); diff != "" {
			t.Fatalf("manifest: %s", diff)
		}
		for i, e := range r.Manifest.Parts {
			if i == 64 {
				break
			}
			want, werr := r.Part(e.H)
			got, gerr := d.Part(e.H)
			if (werr == nil) != (gerr == nil) {
				t.Fatalf("part %s: reference %v, libbdf %v", e.H, werr, gerr)
			}
			if werr == nil && !bytes.Equal(want, got) {
				t.Fatalf("part %s: %d bytes differ from %d", e.H, len(got), len(want))
			}
		}
	})
}
