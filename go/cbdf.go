// Package cbdf binds libbdf for the differential tests against the Go
// reference implementation (github.com/shibukawa/bdf). It compiles the C
// sources of this repository into the package, so it is a test harness and
// a development binding, not a package to import from elsewhere.
package cbdf

/*
#cgo CFLAGS: -I${SRCDIR}/../include -I${SRCDIR}/../src -I${SRCDIR}/../third_party/brotli/include -I${SRCDIR}/../third_party/brotli/dec -I${SRCDIR}/../third_party/brotli/common -std=c11 -Wall -Wextra
#include <stdlib.h>
#include "bdf/bdf.h"

// The resolver of the differential tests: every part whose name starts with
// the byte 1 is the one child object, and no other part exists.
static const bdf_object *cbdf_one_child(void *ctx, const uint8_t hash[16]) {
  return hash[0] == 1 ? (const bdf_object *)ctx : NULL;
}

static bdf_status cbdf_extract(const bdf_object *top, const bdf_object *child, bdf_text **out, bdf_error *err) {
  return bdf_text_extract(top, cbdf_one_child, (void *)child, out, err);
}
*/
import "C"

import (
	"errors"
	"fmt"
	"unsafe"
)

// Error is a bdf_error: where a decoder stopped.
type Error struct {
	Status  int
	Offset  int
	Message string
}

func (e *Error) Error() string { return fmt.Sprintf("libbdf: %s at %d", e.Message, e.Offset) }

func errorOf(e *C.bdf_error) error {
	if e.status == C.BDF_OK {
		return nil
	}
	return &Error{Status: int(e.status), Offset: int(e.offset), Message: C.GoString(e.message)}
}

// Version returns bdf_version().
func Version() string { return C.GoString(C.bdf_version()) }

func ptr(b []byte) *C.uint8_t {
	if len(b) == 0 {
		return nil
	}
	return (*C.uint8_t)(unsafe.Pointer(&b[0]))
}

func goString(p *C.char, n C.size_t) string {
	if p == nil || n == 0 {
		return ""
	}
	return C.GoStringN(p, C.int(n))
}

func goBytes(p *C.uint8_t, n C.size_t) []byte {
	if p == nil || n == 0 {
		return nil
	}
	return C.GoBytes(unsafe.Pointer(p), C.int(n))
}

// Header is a parsed single-file header.
type Header struct {
	Version, Flags           uint16
	ManifestOff, ManifestLen uint64
	ManifestEnc              uint8
}

// ParseHeader checks the header of a single-file BDF of fileSize bytes.
func ParseHeader(data []byte, fileSize uint64) (Header, error) {
	var h C.bdf_header
	var e C.bdf_error
	if C.bdf_header_parse(ptr(data), C.size_t(len(data)), C.uint64_t(fileSize), &h, &e) != C.BDF_OK {
		return Header{}, errorOf(&e)
	}
	return Header{
		Version: uint16(h.version), Flags: uint16(h.flags),
		ManifestOff: uint64(h.manifest_off), ManifestLen: uint64(h.manifest_len),
		ManifestEnc: uint8(h.manifest_enc),
	}, nil
}

// Inflate inflates raw deflate data with bdf_inflate, bounded by limit.
func Inflate(data []byte, limit int) ([]byte, error) {
	var out *C.uint8_t
	var n C.size_t
	var e C.bdf_error
	if C.bdf_inflate(ptr(data), C.size_t(len(data)), C.size_t(limit), &out, &n, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	defer C.bdf_free(unsafe.Pointer(out))
	b := goBytes(out, n)
	if b == nil {
		b = []byte{}
	}
	return b, nil
}

// Path is an entry of an object's path table, or a path of a collection.
type Path struct {
	External bool
	Verbs    []byte
	Args     []float32
	Hash     [16]byte
	Index    uint32
}

// Stop is a gradient stop.
type Stop struct {
	Offset float32
	Color  uint32
}

// Paint is a gradient or a pattern.
type Paint struct {
	Kind   uint8
	Coords []float32
	Stops  []Stop
	Image  uint64
	Repeat uint8
	Matrix [6]float32
}

// Font is a font reference.
type Font struct {
	Kind   uint8
	Hash   [16]byte
	Family string
	Weight uint16
	Style  uint8
}

// Glyph is a placed path of a FILL_PATH_RUN.
type Glyph struct {
	Path uint64
	X, Y float32
}

// Instr is one decoded instruction. Operands are in reading order: f32 as
// float32, u8/u32/varuint as uint64, strings as string, bytes as []byte,
// DASH segments as []float32 and glyph runs as []Glyph, like bdf.Instr.
type Instr struct {
	Op     byte
	Offset int
	Args   []any
}

// Object is a decoded Object part. Close it when done.
type Object struct {
	p *C.bdf_object
}

// DecodeObject parses an Object part with bdf_object_decode.
func DecodeObject(data []byte) (*Object, error) {
	var p *C.bdf_object
	var e C.bdf_error
	if C.bdf_object_decode(ptr(data), C.size_t(len(data)), &p, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	return &Object{p: p}, nil
}

// Close frees the object.
func (o *Object) Close() {
	if o.p != nil {
		C.bdf_object_free(o.p)
		o.p = nil
	}
}

func (o *Object) Opset() uint16 { return uint16(C.bdf_object_opset(o.p)) }

func (o *Object) BBox() [4]float32 {
	var b [4]C.float
	C.bdf_object_bbox(o.p, &b[0])
	return [4]float32{float32(b[0]), float32(b[1]), float32(b[2]), float32(b[3])}
}

func (o *Object) Strings() []string {
	n := int(C.bdf_object_string_count(o.p))
	out := make([]string, n)
	for i := range out {
		var l C.size_t
		s := C.bdf_object_string(o.p, C.size_t(i), &l)
		out[i] = goString(s, l)
	}
	return out
}

func floats(p *C.float, n C.size_t) []float32 {
	if n == 0 {
		return nil
	}
	return append([]float32(nil), unsafe.Slice((*float32)(unsafe.Pointer(p)), int(n))...)
}

func hash16(p *C.uint8_t) (h [16]byte) {
	if p != nil {
		copy(h[:], unsafe.Slice((*byte)(unsafe.Pointer(p)), 16))
	}
	return h
}

func pathOf(cp *C.bdf_path) Path {
	p := Path{External: cp.external != 0, Index: uint32(cp.index), Hash: hash16(cp.hash)}
	if cp.nverbs > 0 {
		p.Verbs = C.GoBytes(unsafe.Pointer(cp.verbs), C.int(cp.nverbs))
	}
	p.Args = floats(cp.args, cp.nargs)
	return p
}

func (o *Object) Paths() []Path {
	n := int(C.bdf_object_path_count(o.p))
	out := make([]Path, n)
	for i := range out {
		var cp C.bdf_path
		C.bdf_object_path(o.p, C.size_t(i), &cp)
		out[i] = pathOf(&cp)
	}
	return out
}

func (o *Object) Paints() []Paint {
	n := int(C.bdf_object_paint_count(o.p))
	out := make([]Paint, n)
	for i := range out {
		var cp C.bdf_paint
		C.bdf_object_paint(o.p, C.size_t(i), &cp)
		p := Paint{Kind: uint8(cp.kind), Image: uint64(cp.image), Repeat: uint8(cp.repeat)}
		p.Coords = floats(cp.coords, cp.ncoords)
		for _, s := range unsafe.Slice(cp.stops, int(cp.nstops)) {
			p.Stops = append(p.Stops, Stop{Offset: float32(s.offset), Color: uint32(s.color)})
		}
		for k := range p.Matrix {
			p.Matrix[k] = float32(cp.matrix[k])
		}
		out[i] = p
	}
	return out
}

func (o *Object) Fonts() []Font {
	n := int(C.bdf_object_font_count(o.p))
	out := make([]Font, n)
	for i := range out {
		var cf C.bdf_font
		C.bdf_object_font(o.p, C.size_t(i), &cf)
		out[i] = Font{
			Kind: uint8(cf.kind), Hash: hash16(cf.hash),
			Family: goString(cf.family, cf.family_len),
			Weight: uint16(cf.weight), Style: uint8(cf.style),
		}
	}
	return out
}

func (o *Object) Images() [][16]byte {
	n := int(C.bdf_object_image_count(o.p))
	out := make([][16]byte, n)
	for i := range out {
		out[i] = hash16(C.bdf_object_image(o.p, C.size_t(i)))
	}
	return out
}

func (o *Object) Objects() [][16]byte {
	n := int(C.bdf_object_object_count(o.p))
	out := make([][16]byte, n)
	for i := range out {
		out[i] = hash16(C.bdf_object_object(o.p, C.size_t(i)))
	}
	return out
}

func (o *Object) Ops() []byte {
	var l C.size_t
	p := C.bdf_object_ops(o.p, &l)
	return goBytes(p, l)
}

// Instructions walks the op stream. On an error it returns the instructions
// before it and the error, like bdf.ObjectPart.Instructions.
func (o *Object) Instructions() ([]Instr, error) {
	it := C.bdf_instr_new(o.p)
	if it == nil {
		return nil, errors.New("libbdf: out of memory")
	}
	defer C.bdf_instr_free(it)
	var out []Instr
	for {
		var e C.bdf_error
		rc := C.bdf_instr_next(it, &e)
		if rc == 0 {
			return out, nil
		}
		if rc < 0 {
			return out, errorOf(&e)
		}
		in := Instr{Op: byte(C.bdf_instr_op(it)), Offset: int(C.bdf_instr_offset(it))}
		n := int(C.bdf_instr_arg_count(it))
		in.Args = make([]any, 0, n)
		for i := 0; i < n; i++ {
			ci := C.size_t(i)
			var l C.size_t
			switch C.bdf_instr_arg_kind_at(it, ci) {
			case C.BDF_ARG_F32:
				in.Args = append(in.Args, float32(C.bdf_instr_f32(it, ci)))
			case C.BDF_ARG_U8, C.BDF_ARG_U32, C.BDF_ARG_VARUINT:
				in.Args = append(in.Args, uint64(C.bdf_instr_u64(it, ci)))
			case C.BDF_ARG_STRING:
				s := C.bdf_instr_string(it, ci, &l)
				in.Args = append(in.Args, goString(s, l))
			case C.BDF_ARG_BYTES:
				b := goBytes(C.bdf_instr_bytes(it, ci, &l), l)
				if b == nil {
					b = []byte{}
				}
				in.Args = append(in.Args, b)
			case C.BDF_ARG_F32S:
				segs := floats(C.bdf_instr_f32s(it, ci, &l), l)
				if segs == nil {
					segs = []float32{}
				}
				in.Args = append(in.Args, segs)
			case C.BDF_ARG_GLYPHS:
				g := C.bdf_instr_glyphs(it, ci, &l)
				glyphs := make([]Glyph, int(l))
				for k, cg := range unsafe.Slice(g, int(l)) {
					glyphs[k] = Glyph{Path: uint64(cg.path), X: float32(cg.x), Y: float32(cg.y)}
				}
				in.Args = append(in.Args, glyphs)
			default:
				return out, fmt.Errorf("libbdf: operand %d of %s has no kind", i, OpName(in.Op))
			}
		}
		out = append(out, in)
	}
}

// OpName returns bdf_op_name(op).
func OpName(op byte) string { return C.GoString(C.bdf_op_name(C.uint8_t(op))) }

// DecodePaths parses a path collection part.
func DecodePaths(data []byte) ([]Path, error) {
	var ps *C.bdf_paths
	var e C.bdf_error
	if C.bdf_paths_decode(ptr(data), C.size_t(len(data)), &ps, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	defer C.bdf_paths_free(ps)
	n := int(C.bdf_paths_count(ps))
	out := make([]Path, n)
	for i := range out {
		var cp C.bdf_path
		C.bdf_paths_get(ps, C.size_t(i), &cp)
		out[i] = pathOf(&cp)
	}
	return out, nil
}

// ---- Documents --------------------------------------------------------------

// Doc is an open document. Close it when done.
type Doc struct {
	p     *C.bdf_doc
	cdata unsafe.Pointer
	// the bytes of the segments that were added, which the document reads from
	extra []unsafe.Pointer
}

// OpenMemory opens the single-file form from bytes (copied into C memory, as
// the document keeps reading from them).
func OpenMemory(data []byte) (*Doc, error) {
	var cdata unsafe.Pointer
	if len(data) > 0 {
		cdata = C.CBytes(data)
	}
	var p *C.bdf_doc
	var e C.bdf_error
	if C.bdf_doc_open_memory((*C.uint8_t)(cdata), C.size_t(len(data)), &p, &e) != C.BDF_OK {
		C.free(cdata)
		return nil, errorOf(&e)
	}
	return &Doc{p: p, cdata: cdata}, nil
}

// Close frees the document.
func (d *Doc) Close() {
	if d.p != nil {
		C.bdf_doc_free(d.p)
		C.free(d.cdata)
		for _, p := range d.extra {
			C.free(p)
		}
		d.p = nil
	}
}

// Unlock opens an encrypted document with its password.
func (d *Doc) Unlock(password string) error {
	b := []byte(password)
	var e C.bdf_error
	if C.bdf_doc_unlock(d.p, (*C.char)(unsafe.Pointer(ptr(b))), C.size_t(len(b)), &e) != C.BDF_OK {
		return errorOf(&e)
	}
	return nil
}

// UnlockECDH opens a document sealed for the private key (a P-256 scalar).
func (d *Doc) UnlockECDH(private [32]byte) error {
	var e C.bdf_error
	if C.bdf_doc_unlock_ecdh(d.p, (*C.uint8_t)(&private[0]), &e) != C.BDF_OK {
		return errorOf(&e)
	}
	return nil
}

// AddSegment puts the pages of a segment document into d, which owns it
// from then on: seg must not be used or closed after a success.
func (d *Doc) AddSegment(seg *Doc) error {
	var e C.bdf_error
	if C.bdf_doc_add_segment(d.p, seg.p, &e) != C.BDF_OK {
		return errorOf(&e)
	}
	d.extra = append(d.extra, seg.cdata)
	d.extra = append(d.extra, seg.extra...)
	seg.p, seg.cdata, seg.extra = nil, nil, nil
	return nil
}

// PublicKey is bdf_p256_public_key.
func PublicKey(private [32]byte) (out [65]byte, ok bool) {
	return out, C.bdf_p256_public_key((*C.uint8_t)(&private[0]), (*C.uint8_t)(&out[0])) == C.BDF_OK
}

// Error statuses.
const (
	ErrFormat   = int(C.BDF_ERR_FORMAT)
	ErrArg      = int(C.BDF_ERR_ARG)
	ErrPassword = int(C.BDF_ERR_PASSWORD)
)

func (d *Doc) Encrypted() bool { return C.bdf_doc_encrypted(d.p) != 0 }
func (d *Doc) Locked() bool    { return C.bdf_doc_locked(d.p) != 0 }
func (d *Doc) Opset() int64    { return int64(C.bdf_doc_opset(d.p)) }

func (d *Doc) Unit() string {
	var l C.size_t
	s := C.bdf_doc_unit(d.p, &l)
	return goString(s, l)
}

// Meta returns meta.source and meta.generator.
func (d *Doc) Meta() (source, generator string) {
	var m C.bdf_meta
	C.bdf_doc_meta(d.p, &m)
	return goString(m.source, m.source_len), goString(m.generator, m.generator_len)
}

// DC returns the values of a Dublin Core element.
func (d *Doc) DC(element string) []string {
	ce := C.CString(element)
	defer C.free(unsafe.Pointer(ce))
	n := int(C.bdf_doc_dc_count(d.p, ce))
	out := make([]string, n)
	for i := range out {
		var l C.size_t
		s := C.bdf_doc_dc(d.p, ce, C.size_t(i), &l)
		out[i] = goString(s, l)
	}
	return out
}

// Segment is the segment block of a segment document.
type Segment struct {
	View     string
	From, To int64
}

func (d *Doc) Segment() (Segment, bool) {
	var s C.bdf_segment
	if C.bdf_doc_segment(d.p, &s) == 0 {
		return Segment{}, false
	}
	return Segment{View: goString(s.view, s.view_len), From: int64(s.from), To: int64(s.to)}, true
}

// Layer is a layer of a page.
type Layer struct {
	Role string
	Obj  [16]byte
}

// Page is a page of a view.
type Page struct {
	W, H   float32
	Body   *[4]float32
	Layers []Layer
}

// View is a view of the manifest.
type View struct {
	ID, Kind, Title, Direction, TextIndex string
	KindCode                              int
	Pages                                 []Page
	HasContinuous                         bool
	Gap                                   float32
	TileStated                            float32
	Cols, Rows                            [][2]float32
	HasFreeze                             bool
	FreezeCols, FreezeRows                int64
	Gridlines                             bool
	Tiles                                 [][2]string
	TilesRef                              string
	HasPlay                               bool
	Seq, Audio, AudioType, Cues           string
}

func (d *Doc) Views() []View {
	n := int(C.bdf_doc_view_count(d.p))
	out := make([]View, n)
	for vi := range out {
		var cv C.bdf_view
		C.bdf_doc_view(d.p, C.size_t(vi), &cv)
		v := View{
			ID: goString(cv.id, cv.id_len), Kind: goString(cv.kind, cv.kind_len),
			Title: goString(cv.title, cv.title_len), Direction: goString(cv.direction, cv.direction_len),
			TextIndex: goString(cv.text_index, cv.text_index_len), KindCode: int(cv.kind_code),
			HasContinuous: cv.has_continuous != 0, Gap: float32(cv.gap), TileStated: float32(cv.tile_stated),
			HasFreeze: cv.has_freeze != 0, FreezeCols: int64(cv.freeze_cols), FreezeRows: int64(cv.freeze_rows),
			Gridlines: cv.gridlines != 0, TilesRef: goString(cv.tiles_ref, cv.tiles_ref_len),
			HasPlay: cv.has_play != 0, Seq: goString(cv.seq, cv.seq_len), Audio: goString(cv.audio, cv.audio_len),
			AudioType: goString(cv.audio_type, cv.audio_type_len), Cues: goString(cv.cues, cv.cues_len),
		}
		for pi := 0; pi < int(cv.npages); pi++ {
			var cp C.bdf_page
			C.bdf_doc_page(d.p, C.size_t(vi), C.size_t(pi), &cp)
			p := Page{W: float32(cp.w), H: float32(cp.h)}
			if cp.has_body != 0 {
				p.Body = &[4]float32{float32(cp.body[0]), float32(cp.body[1]), float32(cp.body[2]), float32(cp.body[3])}
			}
			for li := 0; li < int(cp.nlayers); li++ {
				var cl C.bdf_layer
				C.bdf_doc_layer(d.p, C.size_t(vi), C.size_t(pi), C.size_t(li), &cl)
				p.Layers = append(p.Layers, Layer{Role: goString(cl.role, cl.role_len), Obj: hash16(cl.obj)})
			}
			v.Pages = append(v.Pages, p)
		}
		for i := 0; i < int(cv.ncols); i++ {
			var run [2]C.float
			C.bdf_doc_view_run(d.p, C.size_t(vi), 0, C.size_t(i), &run[0])
			v.Cols = append(v.Cols, [2]float32{float32(run[0]), float32(run[1])})
		}
		for i := 0; i < int(cv.nrows); i++ {
			var run [2]C.float
			C.bdf_doc_view_run(d.p, C.size_t(vi), 1, C.size_t(i), &run[0])
			v.Rows = append(v.Rows, [2]float32{float32(run[0]), float32(run[1])})
		}
		for i := 0; i < int(cv.ntiles); i++ {
			var ct C.bdf_tile
			C.bdf_doc_tile(d.p, C.size_t(vi), C.size_t(i), &ct)
			v.Tiles = append(v.Tiles, [2]string{goString(ct.key, ct.key_len), goString(ct.obj, ct.obj_len)})
		}
		out[vi] = v
	}
	return out
}

// PartEntry is an entry of the parts table.
type PartEntry struct {
	Hash           [16]byte
	Type, Enc      string
	Len, Size, Off int64
	Sealed         *[16]byte
}

func (d *Doc) Parts() []PartEntry {
	n := int(C.bdf_doc_part_count(d.p))
	out := make([]PartEntry, n)
	for i := range out {
		var ce C.bdf_part_entry
		C.bdf_doc_part_entry(d.p, C.size_t(i), &ce)
		e := PartEntry{
			Hash: hash16(ce.hash), Type: goString(ce._type, ce.type_len), Enc: goString(ce.enc_name, ce.enc_name_len),
			Len: int64(ce.len), Size: int64(ce.size), Off: int64(ce.off),
		}
		if ce.sealed != nil {
			h := hash16(ce.sealed)
			e.Sealed = &h
		}
		out[i] = e
	}
	return out
}

// Part reads and decodes a part.
func (d *Doc) Part(hash [16]byte) ([]byte, error) {
	var out *C.uint8_t
	var n C.size_t
	var e C.bdf_error
	if C.bdf_doc_part(d.p, (*C.uint8_t)(unsafe.Pointer(&hash[0])), &out, &n, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	defer C.bdf_free(unsafe.Pointer(out))
	b := goBytes(out, n)
	if b == nil {
		b = []byte{}
	}
	return b, nil
}

// KeySlot is a key slot of the encryption block.
type KeySlot struct {
	Type, KDF, Crv string
	Iter           int64
	Salt, EPK, Key []byte
}

func (d *Doc) Cipher() string {
	var l C.size_t
	return goString(C.bdf_doc_cipher(d.p, &l), l)
}

func (d *Doc) Keys() []KeySlot {
	n := int(C.bdf_doc_key_count(d.p))
	out := make([]KeySlot, n)
	for i := range out {
		var ck C.bdf_key_slot
		C.bdf_doc_key(d.p, C.size_t(i), &ck)
		out[i] = KeySlot{
			Type: goString(ck._type, ck.type_len), KDF: goString(ck.kdf, ck.kdf_len), Crv: goString(ck.crv, ck.crv_len),
			Iter: int64(ck.iter), Salt: goBytes(ck.salt, ck.salt_len), EPK: goBytes(ck.epk, ck.epk_len),
			Key: goBytes(ck.key, ck.key_len),
		}
	}
	return out
}

// SealedManifest returns the sealed manifest's part and encoding.
func (d *Doc) SealedManifest() (hash [16]byte, enc string, ok bool) {
	var cenc *C.char
	var l C.size_t
	p := C.bdf_doc_sealed_manifest(d.p, &cenc, &l)
	if p == nil {
		return hash, "", false
	}
	return hash16(p), goString(cenc, l), true
}

// ---- Fonts ------------------------------------------------------------------

// Woff2Available reports bdf_woff2_available().
func Woff2Available() bool { return C.bdf_woff2_available() != 0 }

// Woff2Decode unpacks a WOFF2 font with bdf_woff2_decode.
func Woff2Decode(data []byte) ([]byte, error) {
	var out *C.uint8_t
	var n C.size_t
	var e C.bdf_error
	if C.bdf_woff2_decode(ptr(data), C.size_t(len(data)), &out, &n, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	defer C.bdf_free(unsafe.Pointer(out))
	b := goBytes(out, n)
	if b == nil {
		b = []byte{}
	}
	return b, nil
}

// FontSFNT returns a font part as an sfnt font with bdf_font_sfnt.
func FontSFNT(data []byte) ([]byte, error) {
	var out *C.uint8_t
	var n C.size_t
	var e C.bdf_error
	if C.bdf_font_sfnt(ptr(data), C.size_t(len(data)), &out, &n, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	defer C.bdf_free(unsafe.Pointer(out))
	b := goBytes(out, n)
	if b == nil {
		b = []byte{}
	}
	return b, nil
}

// ---- Text -------------------------------------------------------------------

// TextRun is a run of a text.
type TextRun struct {
	Text          string
	X, Y          float32
	Advance, Size float32
	Matrix        [6]float32
	HasFont       bool
	Font          Font
	Align, Sep    uint8
	AltText       bool
	Ordinal       uint32
	Node          int64
	Lang          string
}

// TextLink is a LINK area of a text.
type TextLink struct {
	X, Y, W, H float32
	URL        string
	After      int64
	Node       int64
}

// TextNode is a structure node of a text.
type TextNode struct {
	Kind                 uint8
	Parent               int64
	Level                uint8
	HasCell              bool
	Row, Col, Rows, Cols uint32
	Scope                uint8
	Alt                  string
	HasBounds            bool
	X, Y, W, H           float32
}

// Node kinds and scopes.
const (
	NodeParagraph = uint8(C.BDF_NODE_PARAGRAPH)
	NodeHeading   = uint8(C.BDF_NODE_HEADING)
	NodeCaption   = uint8(C.BDF_NODE_CAPTION)
	NodeList      = uint8(C.BDF_NODE_LIST)
	NodeItem      = uint8(C.BDF_NODE_ITEM)
	NodeTable     = uint8(C.BDF_NODE_TABLE)
	NodeCell      = uint8(C.BDF_NODE_CELL)
	NodeFigure    = uint8(C.BDF_NODE_FIGURE)
	ScopeCol      = uint8(C.BDF_SCOPE_COL)
	ScopeRow      = uint8(C.BDF_SCOPE_ROW)
)

func nodesOf(t *C.bdf_text) []TextNode {
	nodes := make([]TextNode, int(C.bdf_text_node_count(t)))
	for i := range nodes {
		var cn C.bdf_text_node
		C.bdf_text_node_at(t, C.size_t(i), &cn)
		nodes[i] = TextNode{
			Kind: uint8(cn.kind), Parent: int64(cn.parent), Level: uint8(cn.level), HasCell: cn.has_cell != 0,
			Row: uint32(cn.row), Col: uint32(cn.col), Rows: uint32(cn.rows), Cols: uint32(cn.cols),
			Scope: uint8(cn.scope), Alt: goString(cn.alt, cn.alt_len), HasBounds: cn.has_bounds != 0,
			X: float32(cn.x), Y: float32(cn.y), W: float32(cn.w), H: float32(cn.h),
		}
	}
	return nodes
}

// Has reports whether the document has a part (bdf_doc_part_find).
func (d *Doc) Has(hash [16]byte) bool {
	return C.bdf_doc_part_find(d.p, (*C.uint8_t)(unsafe.Pointer(&hash[0])), nil) != 0
}

// Content is the text of a top-level object of the document with its
// structure (bdf_doc_text).
func (d *Doc) Content(hash [16]byte) ([]TextRun, []TextLink, []TextNode, error) {
	var t *C.bdf_text
	var e C.bdf_error
	if C.bdf_doc_text(d.p, (*C.uint8_t)(unsafe.Pointer(&hash[0])), &t, &e) != C.BDF_OK {
		return nil, nil, nil, errorOf(&e)
	}
	defer C.bdf_text_free(t)
	runs, links := textOf(t)
	return runs, links, nodesOf(t), nil
}

// ExtractContent walks an object with bdf_text_extract, as ExtractText, and
// returns the structure too.
func ExtractContent(top, child *Object) ([]TextRun, []TextLink, []TextNode, error) {
	var t *C.bdf_text
	var e C.bdf_error
	var cchild *C.bdf_object
	if child != nil {
		cchild = child.p
	}
	if C.cbdf_extract(top.p, cchild, &t, &e) != C.BDF_OK {
		return nil, nil, nil, errorOf(&e)
	}
	defer C.bdf_text_free(t)
	runs, links := textOf(t)
	return runs, links, nodesOf(t), nil
}

func textOf(t *C.bdf_text) ([]TextRun, []TextLink) {
	runs := make([]TextRun, int(C.bdf_text_run_count(t)))
	for i := range runs {
		var cr C.bdf_text_run
		C.bdf_text_run_at(t, C.size_t(i), &cr)
		r := TextRun{
			Text: goString(cr.text, cr.text_len), X: float32(cr.x), Y: float32(cr.y),
			Advance: float32(cr.advance), Size: float32(cr.size), HasFont: cr.has_font != 0,
			Align: uint8(cr.align), Sep: uint8(cr.sep), AltText: cr.alt_text != 0, Ordinal: uint32(cr.ordinal),
			Node: int64(cr.node), Lang: goString(cr.lang, cr.lang_len),
		}
		for k := range r.Matrix {
			r.Matrix[k] = float32(cr.matrix[k])
		}
		if r.HasFont {
			r.Font = Font{
				Kind: uint8(cr.font.kind), Hash: hash16(cr.font.hash),
				Family: goString(cr.font.family, cr.font.family_len),
				Weight: uint16(cr.font.weight), Style: uint8(cr.font.style),
			}
		}
		runs[i] = r
	}
	links := make([]TextLink, int(C.bdf_text_link_count(t)))
	for i := range links {
		var cl C.bdf_text_link
		C.bdf_text_link_at(t, C.size_t(i), &cl)
		links[i] = TextLink{
			X: float32(cl.x), Y: float32(cl.y), W: float32(cl.w), H: float32(cl.h),
			URL: goString(cl.url, cl.url_len), After: int64(cl.after), Node: int64(cl.node),
		}
	}
	return runs, links
}

// ExtractText walks an object with bdf_text_extract. A part whose name
// starts with the byte 1 resolves to child (nil: to nothing).
func ExtractText(top, child *Object) ([]TextRun, []TextLink, error) {
	var t *C.bdf_text
	var e C.bdf_error
	var cchild *C.bdf_object
	if child != nil {
		cchild = child.p
	}
	if C.cbdf_extract(top.p, cchild, &t, &e) != C.BDF_OK {
		return nil, nil, errorOf(&e)
	}
	defer C.bdf_text_free(t)
	runs, links := textOf(t)
	return runs, links, nil
}

// Text is the text of a top-level object of the document (bdf_doc_text).
func (d *Doc) Text(hash [16]byte) ([]TextRun, []TextLink, error) {
	var t *C.bdf_text
	var e C.bdf_error
	if C.bdf_doc_text(d.p, (*C.uint8_t)(unsafe.Pointer(&hash[0])), &t, &e) != C.BDF_OK {
		return nil, nil, errorOf(&e)
	}
	defer C.bdf_text_free(t)
	runs, links := textOf(t)
	return runs, links, nil
}

// IndexRun is a run of a text index.
type IndexRun struct {
	A, B, Ordinal uint32
	Sep           uint8
	Text          string
}

func indexOf(idx *C.bdf_text_index) []IndexRun {
	out := make([]IndexRun, int(C.bdf_text_index_count(idx)))
	for i := range out {
		var cr C.bdf_index_run
		C.bdf_text_index_run(idx, C.size_t(i), &cr)
		out[i] = IndexRun{A: uint32(cr.a), B: uint32(cr.b), Ordinal: uint32(cr.ordinal), Sep: uint8(cr.sep), Text: goString(cr.text, cr.text_len)}
	}
	return out
}

func plainOf(idx *C.bdf_text_index) (string, error) {
	var out *C.char
	var n C.size_t
	var e C.bdf_error
	if C.bdf_text_index_plain(idx, &out, &n, &e) != C.BDF_OK {
		return "", errorOf(&e)
	}
	defer C.bdf_free(unsafe.Pointer(out))
	return goString(out, n), nil
}

// DecodeTextIndex parses a text index part and joins it into plain text.
func DecodeTextIndex(data []byte) ([]IndexRun, string, error) {
	var idx *C.bdf_text_index
	var e C.bdf_error
	if C.bdf_text_index_decode(ptr(data), C.size_t(len(data)), &idx, &e) != C.BDF_OK {
		return nil, "", errorOf(&e)
	}
	defer C.bdf_text_index_free(idx)
	plain, err := plainOf(idx)
	return indexOf(idx), plain, err
}

// Hit is a hit of a search.
type Hit struct {
	Text, Context string
	Segments      []HitSegment
}

// HitSegment is the part of a hit inside one run.
type HitSegment struct {
	Run           int
	A, B, Ordinal uint32
	Start, End    int
}

// SearchIndexPart decodes a text index part and searches it for each query
// (bdf_text_search_new, bdf_text_search_find): the searchable text and the
// hits of each query.
func SearchIndexPart(data []byte, caseSensitive bool, queries []string, limit, context int) (string, [][]Hit, error) {
	var idx *C.bdf_text_index
	var e C.bdf_error
	if C.bdf_text_index_decode(ptr(data), C.size_t(len(data)), &idx, &e) != C.BDF_OK {
		return "", nil, errorOf(&e)
	}
	defer C.bdf_text_index_free(idx)
	var s *C.bdf_text_search
	cs := C.int(0)
	if caseSensitive {
		cs = 1
	}
	if C.bdf_text_search_new(idx, cs, &s, &e) != C.BDF_OK {
		return "", nil, errorOf(&e)
	}
	defer C.bdf_text_search_free(s)
	var plain *C.char
	var n C.size_t
	if C.bdf_text_search_plain(s, &plain, &n, &e) != C.BDF_OK {
		return "", nil, errorOf(&e)
	}
	text := goString(plain, n)
	C.bdf_free(unsafe.Pointer(plain))
	out := make([][]Hit, len(queries))
	for qi, q := range queries {
		b := []byte(q)
		var hits *C.bdf_text_hits
		if C.bdf_text_search_find(s, (*C.char)(unsafe.Pointer(ptr(b))), C.size_t(len(b)), C.size_t(limit), C.size_t(context), &hits, &e) != C.BDF_OK {
			return "", nil, errorOf(&e)
		}
		out[qi] = make([]Hit, int(C.bdf_text_hits_count(hits)))
		for i := range out[qi] {
			var h C.bdf_text_hit
			C.bdf_text_hits_at(hits, C.size_t(i), &h)
			hit := Hit{Text: goString(h.text, h.text_len), Context: goString(h.context, h.context_len)}
			for k := 0; k < int(h.nsegments); k++ {
				var seg C.bdf_text_hit_segment
				C.bdf_text_hits_segment(hits, C.size_t(i), C.size_t(k), &seg)
				hit.Segments = append(hit.Segments, HitSegment{
					Run: int(seg.run), A: uint32(seg.a), B: uint32(seg.b), Ordinal: uint32(seg.ordinal),
					Start: int(seg.start), End: int(seg.end),
				})
			}
			out[qi][i] = hit
		}
		C.bdf_text_hits_free(hits)
	}
	return text, out, nil
}

// NormalizeQuery is text as a search has it (bdf_text_search_normalize).
func NormalizeQuery(s string, caseSensitive bool) (string, error) {
	b := []byte(s)
	var out *C.char
	var n C.size_t
	var e C.bdf_error
	cs := C.int(0)
	if caseSensitive {
		cs = 1
	}
	if C.bdf_text_search_normalize((*C.char)(unsafe.Pointer(ptr(b))), C.size_t(len(b)), cs, &out, &n, &e) != C.BDF_OK {
		return "", errorOf(&e)
	}
	defer C.bdf_free(unsafe.Pointer(out))
	return goString(out, n), nil
}

// ViewText is the text of a view (bdf_doc_view_text).
func (d *Doc) ViewText(view int, extract bool) ([]IndexRun, error) {
	var idx *C.bdf_text_index
	var e C.bdf_error
	x := C.int(0)
	if extract {
		x = 1
	}
	if C.bdf_doc_view_text(d.p, C.size_t(view), x, &idx, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	defer C.bdf_text_index_free(idx)
	return indexOf(idx), nil
}

// SearchPage is the text of a page; SearchView that of a view.
type SearchPage struct {
	Page int64
	Text string
}

type SearchView struct {
	ID, Kind, Title string
	Pages           []SearchPage
}

// SearchText is the text of the document for a search index.
func (d *Doc) SearchText() ([]SearchView, error) {
	var st *C.bdf_search_text
	var e C.bdf_error
	if C.bdf_doc_search_text(d.p, &st, &e) != C.BDF_OK {
		return nil, errorOf(&e)
	}
	defer C.bdf_search_text_free(st)
	out := make([]SearchView, int(C.bdf_search_text_view_count(st)))
	for i := range out {
		var cv C.bdf_search_view
		C.bdf_search_text_view(st, C.size_t(i), &cv)
		v := SearchView{ID: goString(cv.id, cv.id_len), Kind: goString(cv.kind, cv.kind_len), Title: goString(cv.title, cv.title_len), Pages: []SearchPage{}}
		for k := 0; k < int(cv.npages); k++ {
			var cp C.bdf_search_page
			C.bdf_search_text_page(st, C.size_t(i), C.size_t(k), &cp)
			v.Pages = append(v.Pages, SearchPage{Page: int64(cp.page), Text: goString(cp.text, cp.text_len)})
		}
		out[i] = v
	}
	return out, nil
}

// CueSystem and Cue are the entries of a cue index part.
type CueSystem struct {
	Page       uint32
	X, Y, W, H float32
}

type Cue struct {
	Tick, System uint32
	X            float32
}

// DecodeCues parses a cue index part.
func DecodeCues(data []byte) ([]CueSystem, []Cue, error) {
	var c *C.bdf_cues
	var e C.bdf_error
	if C.bdf_cues_decode(ptr(data), C.size_t(len(data)), &c, &e) != C.BDF_OK {
		return nil, nil, errorOf(&e)
	}
	defer C.bdf_cues_free(c)
	systems := make([]CueSystem, int(C.bdf_cues_system_count(c)))
	for i := range systems {
		var cs C.bdf_cue_system
		C.bdf_cues_system(c, C.size_t(i), &cs)
		systems[i] = CueSystem{Page: uint32(cs.page), X: float32(cs.x), Y: float32(cs.y), W: float32(cs.w), H: float32(cs.h)}
	}
	cues := make([]Cue, int(C.bdf_cues_count(c)))
	for i := range cues {
		var cq C.bdf_cue
		C.bdf_cues_cue(c, C.size_t(i), &cq)
		cues[i] = Cue{Tick: uint32(cq.tick), System: uint32(cq.system), X: float32(cq.x)}
	}
	return systems, cues, nil
}

// LinkTarget is what a LINK url leads to (bdf_link_parse); View is decoded.
type LinkTarget struct {
	Kind int
	Page int64
	View string
}

// ParseLink classifies a LINK url.
func ParseLink(url string) LinkTarget {
	cs := C.CString(url)
	defer C.free(unsafe.Pointer(cs))
	var t C.bdf_link_target
	kind := C.bdf_link_parse(cs, C.size_t(len(url)), &t)
	out := LinkTarget{Kind: int(kind), Page: int64(t.page)}
	if kind == C.BDF_LINK_VIEW {
		buf := make([]byte, len(url)+1)
		if n := C.bdf_link_view_id(&t, (*C.char)(unsafe.Pointer(&buf[0])), C.size_t(len(buf))); n >= 0 {
			out.View = string(buf[:n])
		}
	}
	return out
}
