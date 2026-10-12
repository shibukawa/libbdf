package cbdf

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"testing"

	"github.com/klauspost/compress/flate"
	"github.com/shibukawa/bdf"
)

// testdataDir is the testdata directory of the pinned bdf module: the
// documents every converter writes, which seed and check everything here.
// BDF_TESTDATA points elsewhere, for a checkout of bdf.
func testdataDir(t testing.TB) string {
	t.Helper()
	if d := os.Getenv("BDF_TESTDATA"); d != "" {
		return d
	}
	out, err := exec.Command("go", "list", "-m", "-f", "{{.Dir}}", "github.com/shibukawa/bdf").Output()
	if err != nil {
		t.Fatalf("locating the bdf module: %v", err)
	}
	return filepath.Join(strings.TrimSpace(string(out)), "testdata")
}

func documents(t testing.TB) []string {
	t.Helper()
	dir := testdataDir(t)
	var files []string
	for _, pattern := range []string{"*.bdf", "*/*.bdf"} {
		m, _ := filepath.Glob(filepath.Join(dir, pattern))
		files = append(files, m...)
	}
	if len(files) == 0 {
		t.Fatalf("no documents under %s", dir)
	}
	return files
}

type part struct {
	doc, hash, typ string
	data           []byte
}

// parts returns the object and path collection parts of every document.
func parts(t testing.TB) []part {
	t.Helper()
	var out []part
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
			if p.Type == bdf.PartObject || p.Type == bdf.PartPath {
				out = append(out, part{filepath.Base(f), p.Hash.String(), p.Type, p.Data})
			}
		}
	}
	return out
}

// ---- A dump both decoders are rendered into -------------------------------

var numbered = regexp.MustCompile(`^(unsupported opset|unsupported object flags|unknown opcode) .*$`)

// errText renders a decoder error as offset and message. The reference puts
// the offending number into three of its messages; libbdf's are static.
func errText(err error) string {
	if err == nil {
		return "ok"
	}
	var fe *bdf.FormatError
	if errors.As(err, &fe) {
		return fmt.Sprintf("error %d %s", fe.Offset, numbered.ReplaceAllString(fe.Msg, "$1"))
	}
	var ce *Error
	if errors.As(err, &ce) {
		return fmt.Sprintf("error %d %s", ce.Offset, ce.Message)
	}
	return "error ? " + err.Error()
}

func f32(v float32) string { return strconv.FormatFloat(float64(v), 'g', -1, 32) }

func f32s(v []float32) string {
	s := make([]string, len(v))
	for i, f := range v {
		s[i] = f32(f)
	}
	return "[" + strings.Join(s, " ") + "]"
}

// arg renders an operand. Path and image references are compared as 32-bit
// values because the reference narrows them to its PathRef and ImageRef.
func arg(a any) string {
	switch v := a.(type) {
	case float32:
		return f32(v)
	case uint64:
		return strconv.FormatUint(v, 10)
	case string:
		return strconv.Quote(v)
	case []byte:
		return fmt.Sprintf("bytes:%x", v)
	case []float32:
		return f32s(v)
	case []bdf.Glyph:
		s := make([]string, len(v))
		for i, g := range v {
			s[i] = fmt.Sprintf("{%d %s %s}", uint32(g.Path), f32(g.X), f32(g.Y))
		}
		return "[" + strings.Join(s, " ") + "]"
	case []Glyph:
		s := make([]string, len(v))
		for i, g := range v {
			s[i] = fmt.Sprintf("{%d %s %s}", uint32(g.Path), f32(g.X), f32(g.Y))
		}
		return "[" + strings.Join(s, " ") + "]"
	}
	return fmt.Sprintf("?%T", a)
}

func pathText(external bool, verbs []byte, args []float32, hash [16]byte, index uint32) string {
	if external {
		return fmt.Sprintf("ext %x#%d", hash, index)
	}
	return fmt.Sprintf("inline %x %s", verbs, f32s(args))
}

func paintText(kind uint8, coords []float32, stops [][2]string, image uint64, repeat uint8, matrix [6]float32) string {
	if kind == bdf.PaintPattern {
		return fmt.Sprintf("pattern img=%d repeat=%d %s", uint32(image), repeat, f32s(matrix[:]))
	}
	s := make([]string, len(stops))
	for i, st := range stops {
		s[i] = st[0] + ":" + st[1]
	}
	return fmt.Sprintf("kind=%d %s stops=[%s]", kind, f32s(coords), strings.Join(s, " "))
}

func fontText(kind uint8, hash [16]byte, family string, weight uint16, style uint8) string {
	return fmt.Sprintf("kind=%d %x %q w%d s%d", kind, hash, family, weight, style)
}

func instrText(op byte, args []any) string {
	s := make([]string, len(args))
	for i, a := range args {
		s[i] = arg(a)
	}
	return bdf.OpName(op) + " " + strings.Join(s, " ")
}

// dumpGo renders an object as the reference decodes it.
func dumpGo(data []byte) string {
	var sb strings.Builder
	o, err := bdf.DecodeObject(data)
	if err != nil {
		return "decode " + errText(err) + "\n"
	}
	fmt.Fprintf(&sb, "opset %d bbox %s\n", o.Opset, f32s([]float32{o.BBox.X, o.BBox.Y, o.BBox.W, o.BBox.H}))
	for _, s := range o.Strings {
		fmt.Fprintf(&sb, "str %q\n", s)
	}
	for _, p := range o.Paths {
		if p.Inline != nil {
			fmt.Fprintln(&sb, "path", pathText(false, p.Inline.Verbs, p.Inline.Args, [16]byte{}, 0))
		} else {
			fmt.Fprintln(&sb, "path", pathText(true, nil, nil, p.Hash, p.Index))
		}
	}
	for _, p := range o.Paints {
		var stops [][2]string
		for _, st := range p.Stops {
			stops = append(stops, [2]string{f32(st.Offset), strconv.FormatUint(uint64(st.Color), 10)})
		}
		fmt.Fprintln(&sb, "paint", paintText(p.Kind, p.Coords, stops, uint64(p.Image), p.Repeat, p.Matrix))
	}
	for _, f := range o.Fonts {
		fmt.Fprintln(&sb, "font", fontText(f.Kind, f.Hash, f.Family, f.Weight, f.Style))
	}
	for _, h := range o.Images {
		fmt.Fprintf(&sb, "image %x\n", h[:])
	}
	for _, h := range o.Objects {
		fmt.Fprintf(&sb, "object %x\n", h[:])
	}
	fmt.Fprintf(&sb, "ops %d\n", len(o.Ops))
	ins, err := o.Instructions()
	for _, in := range ins {
		fmt.Fprintln(&sb, instrText(in.Op, in.Args))
	}
	fmt.Fprintln(&sb, "walk", errText(err))
	return sb.String()
}

// dumpC renders an object as libbdf decodes it, in the same form.
func dumpC(data []byte) string {
	var sb strings.Builder
	o, err := DecodeObject(data)
	if err != nil {
		return "decode " + errText(err) + "\n"
	}
	defer o.Close()
	b := o.BBox()
	fmt.Fprintf(&sb, "opset %d bbox %s\n", o.Opset(), f32s(b[:]))
	for _, s := range o.Strings() {
		fmt.Fprintf(&sb, "str %q\n", s)
	}
	for _, p := range o.Paths() {
		fmt.Fprintln(&sb, "path", pathText(p.External, p.Verbs, p.Args, p.Hash, p.Index))
	}
	for _, p := range o.Paints() {
		var stops [][2]string
		for _, st := range p.Stops {
			stops = append(stops, [2]string{f32(st.Offset), strconv.FormatUint(uint64(st.Color), 10)})
		}
		fmt.Fprintln(&sb, "paint", paintText(p.Kind, p.Coords, stops, p.Image, p.Repeat, p.Matrix))
	}
	for _, f := range o.Fonts() {
		fmt.Fprintln(&sb, "font", fontText(f.Kind, f.Hash, f.Family, f.Weight, f.Style))
	}
	for _, h := range o.Images() {
		fmt.Fprintf(&sb, "image %x\n", h[:])
	}
	for _, h := range o.Objects() {
		fmt.Fprintf(&sb, "object %x\n", h[:])
	}
	fmt.Fprintf(&sb, "ops %d\n", len(o.Ops()))
	ins, err := o.Instructions()
	for _, in := range ins {
		fmt.Fprintln(&sb, instrText(in.Op, in.Args))
	}
	fmt.Fprintln(&sb, "walk", errText(err))
	return sb.String()
}

func dumpPathsGo(data []byte) string {
	ps, err := bdf.DecodePathCollection(data)
	if err != nil {
		return "decode " + errText(err) + "\n"
	}
	var sb strings.Builder
	for _, p := range ps {
		fmt.Fprintln(&sb, pathText(false, p.Verbs, p.Args, [16]byte{}, 0))
	}
	return sb.String()
}

func dumpPathsC(data []byte) string {
	ps, err := DecodePaths(data)
	if err != nil {
		return "decode " + errText(err) + "\n"
	}
	var sb strings.Builder
	for _, p := range ps {
		fmt.Fprintln(&sb, pathText(false, p.Verbs, p.Args, [16]byte{}, 0))
	}
	return sb.String()
}

// ---- Manifests ----------------------------------------------------------------

func runsText(runs [][2]float32) string {
	s := make([]string, len(runs))
	for i, r := range runs {
		s[i] = f32s(r[:])
	}
	return "[" + strings.Join(s, " ") + "]"
}

func runsOf(runs []bdf.Run) [][2]float32 {
	out := make([][2]float32, len(runs))
	for i, r := range runs {
		out[i] = [2]float32(r)
	}
	return out
}

// dumpManifestGo renders the manifest the reference reader holds.
func dumpManifestGo(m *bdf.Manifest) string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "bdf %d opset %d unit %q\n", m.BDF, m.Opset, m.Unit)
	fmt.Fprintf(&sb, "meta source %q generator %q\n", m.Meta.Source, m.Meta.Generator)
	dc := reflect.ValueOf(m.Meta.DC)
	for i, term := range bdf.DCTerms {
		if vals, ok := dc.Field(i).Interface().(bdf.DCValues); ok && len(vals) > 0 {
			fmt.Fprintf(&sb, "dc %s %q\n", term, []string(vals))
		}
	}
	if s := m.Segment; s != nil {
		fmt.Fprintf(&sb, "segment %q %d %d\n", s.View, s.From, s.To)
	}
	for _, v := range m.Views {
		fmt.Fprintf(&sb, "view id %q kind %q title %q direction %q textIndex %q", v.ID, v.Kind, v.Title, v.Direction, v.TextIndex)
		if v.Continuous != nil {
			fmt.Fprintf(&sb, " gap %s", f32(v.Continuous.Gap))
		}
		fmt.Fprintf(&sb, " tile %s cols %s rows %s", f32(v.Tile), runsText(runsOf(v.Cols)), runsText(runsOf(v.Rows)))
		if v.Freeze != nil {
			fmt.Fprintf(&sb, " freeze %d %d", v.Freeze.Cols, v.Freeze.Rows)
		}
		fmt.Fprintf(&sb, " gridlines %v", v.Gridlines)
		keys := make([]string, 0, len(v.Tiles))
		for k := range v.Tiles {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			fmt.Fprintf(&sb, " tile %q=%q", k, v.Tiles[k])
		}
		fmt.Fprintf(&sb, " tilesRef %q", v.TilesRef)
		if p := v.Play; p != nil {
			fmt.Fprintf(&sb, " play %q %q %q %q", p.Seq, p.Audio, p.Type, p.Cues)
		}
		sb.WriteByte('\n')
		for _, p := range v.Pages {
			fmt.Fprintf(&sb, "  page %s %s", f32(p.W), f32(p.H))
			if p.Body != nil {
				fmt.Fprintf(&sb, " body %s", f32s([]float32{p.Body.X, p.Body.Y, p.Body.W, p.Body.H}))
			}
			for _, l := range p.Layers {
				fmt.Fprintf(&sb, " %s=%x", l.Role, l.Obj[:])
			}
			sb.WriteByte('\n')
		}
	}
	for _, p := range m.Parts {
		fmt.Fprintf(&sb, "part %x %q %q %d %d %d", p.H[:], p.T, p.Enc, p.Len, p.Size, p.Off)
		if p.Sealed != (bdf.Hash{}) {
			fmt.Fprintf(&sb, " sealed %x", p.Sealed[:])
		}
		sb.WriteByte('\n')
	}
	if e := m.Encryption; e != nil {
		fmt.Fprintf(&sb, "encryption %q manifest %x %q\n", e.Cipher, e.Manifest.Part[:], e.Manifest.Enc)
		for _, k := range e.Keys {
			fmt.Fprintf(&sb, "  key %q %q %d %x %q %x %x\n", k.Type, k.KDF, k.Iter, k.Salt, k.Crv, k.EPK, k.Key)
		}
	}
	return sb.String()
}

// dumpManifestC renders the manifest libbdf holds, in the same form.
func dumpManifestC(d *Doc) string {
	var sb strings.Builder
	source, generator := d.Meta()
	fmt.Fprintf(&sb, "bdf %d opset %d unit %q\n", 1, d.Opset(), d.Unit())
	fmt.Fprintf(&sb, "meta source %q generator %q\n", source, generator)
	for _, term := range bdf.DCTerms {
		if vals := d.DC(term); len(vals) > 0 {
			fmt.Fprintf(&sb, "dc %s %q\n", term, vals)
		}
	}
	if s, ok := d.Segment(); ok {
		fmt.Fprintf(&sb, "segment %q %d %d\n", s.View, s.From, s.To)
	}
	for _, v := range d.Views() {
		fmt.Fprintf(&sb, "view id %q kind %q title %q direction %q textIndex %q", v.ID, v.Kind, v.Title, v.Direction, v.TextIndex)
		if v.HasContinuous {
			fmt.Fprintf(&sb, " gap %s", f32(v.Gap))
		}
		fmt.Fprintf(&sb, " tile %s cols %s rows %s", f32(v.TileStated), runsText(v.Cols), runsText(v.Rows))
		if v.HasFreeze {
			fmt.Fprintf(&sb, " freeze %d %d", v.FreezeCols, v.FreezeRows)
		}
		fmt.Fprintf(&sb, " gridlines %v", v.Gridlines)
		tiles := append([][2]string(nil), v.Tiles...)
		sort.Slice(tiles, func(i, j int) bool { return tiles[i][0] < tiles[j][0] })
		for _, t := range tiles {
			fmt.Fprintf(&sb, " tile %q=%q", t[0], t[1])
		}
		fmt.Fprintf(&sb, " tilesRef %q", v.TilesRef)
		if v.HasPlay {
			fmt.Fprintf(&sb, " play %q %q %q %q", v.Seq, v.Audio, v.AudioType, v.Cues)
		}
		sb.WriteByte('\n')
		for _, p := range v.Pages {
			fmt.Fprintf(&sb, "  page %s %s", f32(p.W), f32(p.H))
			if p.Body != nil {
				fmt.Fprintf(&sb, " body %s", f32s(p.Body[:]))
			}
			for _, l := range p.Layers {
				fmt.Fprintf(&sb, " %s=%x", l.Role, l.Obj[:])
			}
			sb.WriteByte('\n')
		}
	}
	for _, p := range d.Parts() {
		fmt.Fprintf(&sb, "part %x %q %q %d %d %d", p.Hash[:], p.Type, p.Enc, p.Len, p.Size, p.Off)
		if p.Sealed != nil && *p.Sealed != ([16]byte{}) {
			fmt.Fprintf(&sb, " sealed %x", p.Sealed[:])
		}
		sb.WriteByte('\n')
	}
	if d.Encrypted() {
		h, enc, _ := d.SealedManifest()
		fmt.Fprintf(&sb, "encryption %q manifest %x %q\n", d.Cipher(), h[:], enc)
		for _, k := range d.Keys() {
			fmt.Fprintf(&sb, "  key %q %q %d %x %q %x %x\n", k.Type, k.KDF, k.Iter, k.Salt, k.Crv, k.EPK, k.Key)
		}
	}
	return sb.String()
}

// firstDiff reports the first line two dumps differ at.
func firstDiff(a, b string) string {
	al, bl := strings.Split(a, "\n"), strings.Split(b, "\n")
	for i := 0; i < len(al) || i < len(bl); i++ {
		var x, y string
		if i < len(al) {
			x = al[i]
		}
		if i < len(bl) {
			y = bl[i]
		}
		if x != y {
			return fmt.Sprintf("line %d:\n  go: %s\n  c:  %s", i+1, x, y)
		}
	}
	return ""
}

// goInflate is the reference's inflate (reader.go): at most limit bytes, and
// data that would give more is an error.
func goInflate(b []byte, limit int) ([]byte, error) {
	rd := flate.NewReader(bytes.NewReader(b))
	defer rd.Close()
	var out bytes.Buffer
	n, err := io.Copy(&out, io.LimitReader(rd, int64(limit)))
	if err != nil {
		return nil, err
	}
	if n == int64(limit) {
		var extra [1]byte
		if _, err := io.ReadFull(rd, extra[:]); err == nil {
			return nil, errors.New("data inflates to more than its stated size")
		} else if err != io.EOF {
			return nil, err
		}
	}
	return out.Bytes(), nil
}

// ---- Tests ------------------------------------------------------------------

func TestVersion(t *testing.T) {
	if Version() == "" {
		t.Fatal("empty version")
	}
}

func TestObjectsMatchReference(t *testing.T) {
	n := 0
	for _, p := range parts(t) {
		if p.typ != bdf.PartObject {
			continue
		}
		n++
		if d := firstDiff(dumpGo(p.data), dumpC(p.data)); d != "" {
			t.Errorf("%s %s: %s", p.doc, p.hash, d)
		}
	}
	t.Logf("%d objects compared", n)
}

func TestPathCollectionsMatchReference(t *testing.T) {
	n := 0
	for _, p := range parts(t) {
		if p.typ != bdf.PartPath {
			continue
		}
		n++
		if d := firstDiff(dumpPathsGo(p.data), dumpPathsC(p.data)); d != "" {
			t.Errorf("%s %s: %s", p.doc, p.hash, d)
		}
	}
	t.Logf("%d path collections compared", n)
}

func TestHeadersParse(t *testing.T) {
	for _, f := range documents(t) {
		data, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		h, err := ParseHeader(data[:bdf.HeaderSize], uint64(len(data)))
		if err != nil {
			t.Errorf("%s: %v", f, err)
			continue
		}
		if h.Version != 1 || h.ManifestOff != bdf.HeaderSize {
			t.Errorf("%s: header %+v", f, h)
		}
		if _, err := ParseHeader(data[:bdf.HeaderSize], h.ManifestOff+h.ManifestLen-1); err == nil {
			t.Errorf("%s: a file shorter than its manifest parsed", f)
		}
	}
}

// TestDocumentsMatchReference opens every document with both readers and
// compares the manifests and the decoded bytes of every part.
func TestDocumentsMatchReference(t *testing.T) {
	docs, partsRead := 0, 0
	for _, f := range documents(t) {
		data, err := os.ReadFile(f)
		if err != nil {
			t.Fatal(err)
		}
		r, err := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		if err != nil {
			t.Fatalf("%s: reference: %v", f, err)
		}
		d, err := OpenMemory(data)
		if err != nil {
			t.Errorf("%s: libbdf: %v", f, err)
			continue
		}
		docs++
		if diff := firstDiff(dumpManifestGo(r.Manifest), dumpManifestC(d)); diff != "" {
			t.Errorf("%s: manifest: %s", filepath.Base(f), diff)
		}
		if r.Locked() != d.Locked() {
			t.Errorf("%s: locked %v vs %v", f, r.Locked(), d.Locked())
		}
		for _, e := range r.Manifest.Parts {
			want, werr := r.Part(e.H)
			got, gerr := d.Part(e.H)
			if (werr == nil) != (gerr == nil) {
				t.Errorf("%s %s: reference %v, libbdf %v", filepath.Base(f), e.H, werr, gerr)
			} else if !bytes.Equal(want, got) {
				t.Errorf("%s %s: %d bytes differ from %d", filepath.Base(f), e.H, len(got), len(want))
			}
			partsRead++
		}
		d.Close()
	}
	t.Logf("%d documents, %d parts compared", docs, partsRead)
}

// TestTruncatedObjectsMatchReference cuts every object part at a spread of
// lengths: the two decoders must fail at the same offset with the same message.
func TestTruncatedObjectsMatchReference(t *testing.T) {
	for _, p := range parts(t) {
		if p.typ != bdf.PartObject {
			continue
		}
		step := len(p.data)/64 + 1
		for n := 0; n < len(p.data); n += step {
			cut := p.data[:n]
			if d := firstDiff(dumpGo(cut), dumpC(cut)); d != "" {
				t.Errorf("%s %s cut at %d: %s", p.doc, p.hash, n, d)
				break
			}
		}
	}
}

// TestInflateMatchesReference recompresses every part at every level and
// inflates it back with both decoders, with the limit at, above and below
// the size.
func TestInflateMatchesReference(t *testing.T) {
	n := 0
	for _, p := range parts(t) {
		for _, level := range []int{flate.NoCompression, flate.BestSpeed, flate.DefaultCompression, flate.BestCompression, flate.HuffmanOnly} {
			var buf bytes.Buffer
			w, _ := flate.NewWriter(&buf, level)
			w.Write(p.data)
			w.Close()
			for _, limit := range []int{len(p.data), len(p.data) + 1, len(p.data) - 1, 1 << 30} {
				if limit < 0 {
					continue
				}
				want, werr := goInflate(buf.Bytes(), limit)
				got, gerr := Inflate(buf.Bytes(), limit)
				if (werr == nil) != (gerr == nil) || !bytes.Equal(want, got) {
					t.Fatalf("%s %s level %d limit %d: reference %d bytes %v, libbdf %d bytes %v", p.doc, p.hash, level, limit, len(want), werr, len(got), gerr)
				}
				n++
			}
		}
	}
	t.Logf("%d inflations compared", n)
}

// FuzzObject feeds the same bytes to both decoders and requires the same
// acceptance, the same tables, the same instructions and the same error.
func FuzzObject(f *testing.F) {
	for _, p := range parts(f) {
		f.Add(p.data)
	}
	f.Add([]byte("BOBJ"))
	f.Add(append([]byte("BOBJ\x01\x00\x00\x00"), bytes.Repeat([]byte{0}, 24)...))
	f.Fuzz(func(t *testing.T, data []byte) {
		if d := firstDiff(dumpGo(data), dumpC(data)); d != "" {
			t.Fatalf("object: %s", d)
		}
		if d := firstDiff(dumpPathsGo(data), dumpPathsC(data)); d != "" {
			t.Fatalf("path collection: %s", d)
		}
	})
}

// FuzzInflate requires both decoders to accept the same streams with the
// same output, with the limit varying with the input.
func FuzzInflate(f *testing.F) {
	for i, p := range parts(f) {
		if i%7 != 0 {
			continue
		}
		var buf bytes.Buffer
		w, _ := flate.NewWriter(&buf, flate.BestCompression)
		w.Write(p.data)
		w.Close()
		f.Add(buf.Bytes())
	}
	f.Add([]byte{0x01, 0x00, 0x00, 0xff, 0xff})
	f.Fuzz(func(t *testing.T, data []byte) {
		limit := 1 << 16
		if len(data)%5 == 0 {
			limit = 32
		}
		want, werr := goInflate(data, limit)
		got, gerr := Inflate(data, limit)
		if (werr == nil) != (gerr == nil) {
			t.Fatalf("reference %v, libbdf %v", werr, gerr)
		}
		if werr == nil && !bytes.Equal(want, got) {
			t.Fatalf("outputs differ: %d and %d bytes", len(want), len(got))
		}
	})
}

// FuzzDoc opens the same bytes with both readers. libbdf must never accept a
// document the reference rejects, and when both open it the parts must read
// the same. The reference is the more lenient of the two in places the spec
// leaves to encoding/json (case-folded member names, base64 with line
// breaks), so the other direction is not required.
func FuzzDoc(f *testing.F) {
	for _, name := range documents(f) {
		if data, err := os.ReadFile(name); err == nil && len(data) < 200<<10 {
			f.Add(data)
		}
	}
	f.Fuzz(func(t *testing.T, data []byte) {
		d, cerr := OpenMemory(data)
		r, gerr := bdf.OpenSingle(bytes.NewReader(data), int64(len(data)))
		if cerr != nil {
			return
		}
		defer d.Close()
		if gerr != nil {
			t.Fatalf("libbdf opened what the reference rejects: %v", gerr)
		}
		if r.Locked() != d.Locked() {
			t.Fatalf("locked %v vs %v", r.Locked(), d.Locked())
		}
		for i, e := range d.Parts() {
			if i == 64 {
				break
			}
			want, werr := r.Part(bdf.Hash(e.Hash))
			got, gerr := d.Part(e.Hash)
			if (werr == nil) != (gerr == nil) {
				t.Fatalf("part %x: reference %v, libbdf %v", e.Hash, werr, gerr)
			}
			if werr == nil && !bytes.Equal(want, got) {
				t.Fatalf("part %x: %d bytes differ from %d", e.Hash, len(got), len(want))
			}
		}
	})
}
