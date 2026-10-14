/*
 * The reader of SVG documents: a port of the package image/svg of the
 * reference implementation (github.com/shibukawa/bdf), and of what it reads
 * XML with, so that a renderer that is a port of raster/imagebdf/svg.go
 * walks the same tree and gets the same numbers out of it.
 *
 * It reads the tree of the elements of a document with the style sheets of
 * the document applied, and the values their attributes hold: colours,
 * numbers, lengths and transforms. It draws nothing. Path data is not read
 * here: the d attribute of a path is the string of the document.
 *
 * Strings are UTF-8 where the document is, with their lengths: a value may
 * hold a NUL byte (&#0;), and what a document that declares one encoding
 * and is in another leaves in them. They are not NUL-terminated.
 */
#ifndef BDF_SVG_INTERNAL_H
#define BDF_SVG_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "raster_internal.h" /* bdfi_mat, Go's math, and no fused multiply-adds */

/* ---- Limits --------------------------------------------------------------- */

/* The limits of a document when the options name no others (DefaultMaxDepth,
 * DefaultMaxNodes, DefaultMaxStyleMatches of svg.go). */
#define BDFI_SVG_MAX_DEPTH         256
#define BDFI_SVG_MAX_NODES         (1 << 20)
#define BDFI_SVG_MAX_STYLE_MATCHES (1 << 22)

/* xmltree.MaxEntityText: the text the entities of a DOCTYPE stand for, all
 * their references together. */
#define BDFI_XML_MAX_ENTITY_TEXT (1 << 20)

/*
 * Limits of this library, which the reference does not have (it takes the
 * memory and the time): a document of more bytes, or whose style sheets
 * declare more for its elements, is BDF_ERR_TOO_LARGE.
 */
#define BDFI_SVG_MAX_INPUT       ((size_t)1 << 27)   /* bytes of a document */
#define BDFI_SVG_MAX_SHEET_DECLS ((size_t)1 << 22)   /* declarations kept for elements, together */
#define BDFI_SVG_MAX_SHEET_WORK  ((uint64_t)1 << 27) /* declarations gone through to find them */

/* ---- Strings -------------------------------------------------------------- */

/* A string: n bytes at p, which is never NULL (an empty string points at
 * something). */
typedef struct bdfi_svg_str {
  const char *p;
  size_t n;
} bdfi_svg_str;

/* An attribute of an element, or a declaration of a style ("fill: red"). */
typedef struct bdfi_svg_decl {
  bdfi_svg_str name, value;
} bdfi_svg_decl;

/* ---- The document (svg.go) ------------------------------------------------ */

/* TextNode: the name of a node that is a run of text. */
#define BDFI_SVG_TEXT_NODE "#text"

/*
 * Node: an element of a document, or a run of text.
 *
 * The children of an element are a list: first_child, and next from one to
 * the one after it (Node.Children in their order).
 */
typedef struct bdfi_svg_node bdfi_svg_node;
struct bdfi_svg_node {
  /* Node.Name: the local name of the element (rect, linearGradient),
   * whatever prefix the document gives its namespace; "#text" for text. */
  bdfi_svg_str name;
  /* Name == TextNode: a run of text. That is how the reference tells one,
   * so an element that a document names "#text" is one too, to its
   * renderer and to its style sheets (no rule is matched against it or
   * what is inside it): it has no text, and may have attributes and
   * children. */
  int is_text;
  /* Node.Parent, and Node.Children */
  bdfi_svg_node *parent, *first_child, *last_child, *next;
  size_t nchildren;
  /* Node.Attr: the attributes by their local names, in the order of
   * bdfi_svg_str_cmp (bdfi_svg_attr finds one). The declarations of
   * namespaces are left out, and xlink:href is href. */
  const bdfi_svg_decl *attrs;
  size_t nattrs;
  /* The declarations of the style attribute as Node.Declared reads them,
   * the first of a name, in the same order. */
  const bdfi_svg_decl *style;
  size_t nstyle;
  /* Node.Sheet: the declarations of the rules of the style sheets that
   * match the element, the more specific and the later ones having won;
   * has_sheet is 0 when no rule matches (a nil map), and nsheet may be 0
   * when one without declarations does. */
  const bdfi_svg_decl *const *sheet;
  size_t nsheet;
  int has_sheet;
  /* Node.Text: the text of a run of text. */
  bdfi_svg_str text;
  /* The number of the node among all the nodes of the document, in the order
   * they were made (that of the document): 0 to doc->nnodes - 1, each node
   * its own, also those of the elements after the root. It is the key of
   * what a renderer keeps by node. The nodes of one tree have the numbers
   * that follow its top's, in document order. */
  uint32_t index;
};

/* An entry of Document.IDs. */
typedef struct bdfi_svg_id {
  bdfi_svg_str id;
  bdfi_svg_node *node;
} bdfi_svg_id;

/* Document: an SVG document that was read. */
typedef struct bdfi_svg_doc {
  bdfi_svg_node *root; /* Document.Root: the svg element */
  /* Document.IDs: the elements by their id attributes, the first of those
   * that share one, in the order of bdfi_svg_str_cmp (bdfi_svg_by_id finds
   * one). An element with an id is not always inside the root: a document
   * may have elements after it. */
  const bdfi_svg_id *ids;
  size_t nids;
  /* Document.Warnings: what was left out of a document past the limits, as
   * C strings. */
  const char *const *warnings;
  size_t nwarnings;
  size_t nnodes; /* the elements and runs of text of the document: the indexes are below it */
  bdfi_arena arena;
} bdfi_svg_doc;

/* Options: the limits of reading a document. A zero or negative limit is
 * the default one. */
typedef struct bdfi_svg_options {
  int64_t max_depth;         /* Options.MaxDepth */
  int64_t max_nodes;         /* Options.MaxNodes */
  int64_t max_style_matches; /* Options.MaxStyleMatches */
} bdfi_svg_options;

/*
 * svg.Parse: reads an SVG document; opts may be NULL. It keeps what it read
 * of a document with an error or past a limit, and returns
 *
 *   BDF_OK             and the document (bdfi_svg_free frees it),
 *   BDF_ERR_FORMAT     for a document without an svg root element
 *                      (ErrNotSVG), and for one that declares an encoding
 *                      this library does not read and is not ASCII,
 *   BDF_ERR_TOO_LARGE  past the limits of this library (above),
 *   BDF_ERR_NOMEM.
 *
 * err may be NULL; its message says which of the reasons it was.
 */
BDF_INTERNAL bdf_status bdfi_svg_parse(const uint8_t *data, size_t len,
                                       const bdfi_svg_options *opts, bdfi_svg_doc **out,
                                       bdf_error *err);
BDF_INTERNAL void bdfi_svg_free(bdfi_svg_doc *doc);

/* The messages of bdfi_svg_parse (static strings, compared by address). */
BDF_INTERNAL const char *bdfi_svg_msg_not_svg(void);  /* ErrNotSVG */
BDF_INTERNAL const char *bdfi_svg_msg_encoding(void); /* an encoding that is not read */
BDF_INTERNAL const char *bdfi_svg_msg_sheets(void);   /* more declarations than are kept */
BDF_INTERNAL const char *bdfi_svg_msg_input(void);    /* more bytes than are read */

/* n.Attr[name]: 1 and the value, or 0 and "" for an element without the
 * attribute (and for a run of text). */
BDF_INTERNAL int bdfi_svg_attr(const bdfi_svg_node *n, const char *name, size_t len,
                               bdfi_svg_str *out);
/* n.Sheet[name]: 1 and the value, or 0 and "". */
BDF_INTERNAL int bdfi_svg_sheet(const bdfi_svg_node *n, const char *name, size_t len,
                                bdfi_svg_str *out);
/* Node.Declared: the value an element declares for a property, in its style
 * attribute, then in the style sheets of the document, then as a
 * presentation attribute (without the white space around it): 1 and the
 * value, or 0 and "". Properties are not inherited here. */
BDF_INTERNAL int bdfi_svg_declared(const bdfi_svg_node *n, const char *name, size_t len,
                                   bdfi_svg_str *out);
/* doc.IDs[id]: the element, or NULL. */
BDF_INTERNAL const bdfi_svg_node *bdfi_svg_by_id(const bdfi_svg_doc *doc, const char *id,
                                                 size_t len);

/* ---- Values (value.go, color.go) ------------------------------------------ */

/*
 * strconv.ParseFloat(s, 64) of Go 1.27: 0 and the nearest number; 1 for a
 * number out of range (ErrRange), with the infinity it rounds to; -1 for
 * what is no number (ErrSyntax), with 0. Decimal and hexadecimal numbers,
 * "inf", "infinity" and "nan", as Go reads them. It does not depend on the
 * locale.
 */
BDF_INTERNAL int bdfi_parse_float(const char *s, size_t n, double *out);

/* ParseNumber: reads a number at the start of s, after white space and
 * commas: 1, its value and in *rest how many bytes of s were read; 0 when s
 * does not start with a number (*rest is then past the white space, and *v
 * is 0, or an infinity when the number is out of range, with *rest past
 * it). */
BDF_INTERNAL int bdfi_svg_parse_number(const char *s, size_t n, double *v, size_t *rest);
/* ParseNumbers: reads a list of numbers separated by white space or commas,
 * up to what is not a number. It returns how many there are and writes the
 * first cap of them to out (which may be NULL when cap is 0). */
BDF_INTERNAL size_t bdfi_svg_parse_numbers(const char *s, size_t n, double *out, size_t cap);
/* ParseLength: reads a length in CSS px: a number alone or with a unit, a
 * percentage of ref, or em and ex of the font size em: 1 and the length, or
 * 0 for what is no length. */
BDF_INTERNAL int bdfi_svg_parse_length(const char *s, size_t n, double ref, double em, double *out);

/* Matrix is bdfi_mat, Identity is bdfi_mat_identity(), and Matrix.Mul is
 * this (the arithmetic of bdfi_mat_mul2): m times n, n applies first. */
BDF_INTERNAL bdfi_mat bdfi_svg_mat_mul(bdfi_mat m, bdfi_mat n);
/* ParseTransform: reads a transform list into one matrix, up to what is no
 * transform. */
BDF_INTERNAL bdfi_mat bdfi_svg_parse_transform(const char *s, size_t n);

/* Color: red, green, blue and alpha from 0 to 1, not premultiplied. */
typedef struct bdfi_svg_color {
  double r, g, b, a;
} bdfi_svg_color;

/* ParseColor: reads a CSS colour, or currentColor, which is current: 1 and
 * the colour, or 0 (and a colour of zeros) for what is none. */
BDF_INTERNAL int bdfi_svg_parse_color(const char *s, size_t n, bdfi_svg_color current,
                                      bdfi_svg_color *out);

/* ---- Go's strings, for the files of the reader ---------------------------- */

/* utf8.DecodeRune of s[i:n] (i < n): the character and in *len its bytes;
 * U+FFFD and 1 for a byte that starts none. */
BDF_INTERNAL uint32_t bdfi_svg_rune(const char *s, size_t n, size_t i, size_t *len);
/* utf8.DecodeLastRune of s[0:n] (n > 0). */
BDF_INTERNAL uint32_t bdfi_svg_last_rune(const char *s, size_t n, size_t *len);
/* utf8.AppendRune: 1 to 4 bytes; U+FFFD for what is no character. */
BDF_INTERNAL size_t bdfi_svg_put_rune(uint32_t r, char *out);
BDF_INTERNAL int bdfi_svg_is_space(uint32_t r);      /* unicode.IsSpace */
BDF_INTERNAL uint32_t bdfi_svg_to_lower(uint32_t r); /* unicode.ToLower */
BDF_INTERNAL bdfi_svg_str bdfi_svg_str_of(const char *p, size_t n);
BDF_INTERNAL bdfi_svg_str bdfi_svg_trim_space(bdfi_svg_str s); /* strings.TrimSpace */
/* strings.Fields, a field at a time: 1 and the field that starts at or
 * after s[*pos], with *pos moved past it; 0 after the last. */
BDF_INTERNAL int bdfi_svg_next_field(bdfi_svg_str s, size_t *pos, bdfi_svg_str *field);
/* strings.ToLower: the length of the result, which is written to out when
 * out is not NULL (it needs up to 3 n bytes). */
BDF_INTERNAL size_t bdfi_svg_lower(const char *s, size_t n, char *out);
/* strings.ToLower(s) when it is ASCII and at most cap bytes: its length and
 * the bytes in out, or SIZE_MAX. */
BDF_INTERNAL size_t bdfi_svg_lower_ascii(bdfi_svg_str s, char *out, size_t cap);
/* The order of the tables: by length, then by bytes. */
BDF_INTERNAL int bdfi_svg_str_cmp(bdfi_svg_str a, bdfi_svg_str b);
BDF_INTERNAL int bdfi_svg_str_eq(bdfi_svg_str a, const char *p, size_t n);
/* bdfi_svg_find returns the declaration of a name among n in the order of
 * bdfi_svg_str_cmp, or NULL. */
BDF_INTERNAL const bdfi_svg_decl *bdfi_svg_find(const bdfi_svg_decl *d, size_t n, const char *name,
                                                size_t len);
/* A stable sort of n items of size bytes (a merge sort: n log n whatever
 * the items): 0, or -1 when out of memory. */
BDF_INTERNAL int bdfi_svg_sort(void *base, size_t n, size_t size,
                               int (*cmp)(const void *, const void *));

/* ---- Memory of a document ------------------------------------------------- */

/* What a document is made of comes from its arena; the bytes of strings are
 * packed into blocks of it. */
typedef struct bdfi_svg_pool {
  bdfi_arena *arena;
  char *p;
  size_t left;
} bdfi_svg_pool;

/* bdfi_svg_pool_dup copies n bytes: 0, or -1 when out of memory. */
BDF_INTERNAL int bdfi_svg_pool_dup(bdfi_svg_pool *pool, const char *s, size_t n, bdfi_svg_str *out);
/* bdfi_svg_pool_bytes returns n bytes to write a string to, or NULL. */
BDF_INTERNAL char *bdfi_svg_pool_bytes(bdfi_svg_pool *pool, size_t n);
/* bdfi_svg_pool_array returns n zeroed items of size bytes, aligned for any
 * type, or NULL. */
BDF_INTERNAL void *bdfi_svg_pool_array(bdfi_svg_pool *pool, size_t n, size_t size);

/* ---- Style sheets (css.go) ------------------------------------------------ */

/* The declarations of a style attribute as Node.Declared goes through them
 * (parseDecls): in the order of bdfi_svg_str_cmp, the first of a name kept.
 * Their strings are in s or in the pool. 0, or -1 when out of memory. */
BDF_INTERNAL int bdfi_svg_style_decls(bdfi_svg_pool *pool, bdfi_svg_str s,
                                      const bdfi_svg_decl **out, size_t *count);
/*
 * parseCSS, newStyleSheet and styleSheet.apply: reads the style sheets of a
 * document (src) and stores on each element of the tree the declarations of
 * the rules that match it, with at most `most` rules matched against
 * elements. *over is set when that was not enough (sheet.matches >
 * sheet.most). It returns BDF_OK, BDF_ERR_TOO_LARGE or BDF_ERR_NOMEM.
 */
BDF_INTERNAL bdf_status bdfi_svg_apply_css(bdfi_svg_pool *pool, bdfi_svg_str src,
                                           bdfi_svg_node *root, int64_t most, int *over,
                                           const char **msg);

/* ---- The XML reader (xmltree.Open and xmlro.Reader) ----------------------- */

/* The tokens of a document (xmlro.Kind). */
enum {
  BDFI_XML_NONE,
  BDFI_XML_START,
  BDFI_XML_END,
  BDFI_XML_TEXT,
  BDFI_XML_CDATA,
  BDFI_XML_COMMENT,
  BDFI_XML_PROCINST,
  BDFI_XML_DIRECTIVE,
  BDFI_XML_EOF
};

/* What bdfi_xml_next returns. */
enum {
  BDFI_XML_OK = 0,
  BDFI_XML_STOP,     /* an error of the reference's reader: the document ends here */
  BDFI_XML_NOMEM,    /* out of memory */
  BDFI_XML_ENCODING  /* an encoding the reference reads and this library does not */
};

/* A growable run of bytes. */
typedef struct bdfi_xml_buf {
  uint8_t *p;
  size_t n, cap;
} bdfi_xml_buf;

/* An attribute of a start tag: where its name and its value are in the
 * document, or the value in the reader's buffer of replaced entities. */
typedef struct bdfi_xml_attr {
  size_t name_off, name_len;
  size_t val_off, val_len;
  int in_alt;
} bdfi_xml_attr;

/* An entity a DOCTYPE declared, with its value decoded (docEntity). */
typedef struct bdfi_xml_entity {
  bdfi_svg_str name, value;
} bdfi_xml_entity;

/* The reader of a document that is in memory: xmlro.Reader as svg.Parse
 * configures it (Lenient, the entities and the void elements of HTML, a
 * DOCTYPE allowed), over what xmltree.Open makes of the document. */
typedef struct bdfi_xml_reader {
  const uint8_t *buf; /* the document in UTF-8, or what is left of it after its declaration */
  size_t r, w;        /* buf[r..w) is unread */
  int switched;       /* an encoding was switched to: buf is not the start of the document */
  int marked;         /* the document had a UTF-16 byte order mark */
  int prolog;         /* no element yet: a DOCTYPE or an XML declaration may still come */
  int decl_seen;      /* the one XML declaration has been read */
  int doctype_seen;   /* the one DOCTYPE has been read */
  uint8_t *owned;     /* what buf points into, when it is not the caller's */
  int kind;
  int pending_end; /* a start tag that ends its element: the end is the next token */
  int unwinding;   /* an end tag is ending the elements it leaves open */
  int charset;     /* the declaration named an encoding to switch to */
  int err;         /* what the reader stopped with, returned from then on */
  size_t depth, max_depth;
  uint32_t *stack; /* the hashes of the names of the open elements */
  size_t stack_cap;
  uint32_t close_hash; /* of the end tag that is unwinding */
  size_t close_advance;
  size_t name_off, name_len, text_off, text_len;
  int text_in_alt;
  bdfi_xml_attr *attrs;
  size_t nattrs, attrs_cap, attr_pos;
  bdfi_xml_buf alt;   /* text and values with the entities of the tables replaced */
  bdfi_xml_buf label; /* the encoding the declaration named */
  bdfi_xml_entity
      *ents; /* the entities of the DOCTYPE, in their order: the first of a name counts */
  size_t nents, ents_cap;
  bdfi_arena ent_arena; /* their names and values */
  uint64_t grown;       /* bytes entity substitution has added to the document, bounded by max_buffer */
  size_t max_buffer;    /* Options.MaxBufferBytes as xmltree.Open sets it */
} bdfi_xml_reader;

/* xmltree.Open with the options of svg.Parse: BDFI_XML_OK or
 * BDFI_XML_NOMEM. The reader is closed with bdfi_xml_close either way.
 * data is read, not kept past bdfi_xml_close, and not changed. */
BDF_INTERNAL int bdfi_xml_open(bdfi_xml_reader *r, const uint8_t *data, size_t len,
                               size_t max_depth);
BDF_INTERNAL void bdfi_xml_close(bdfi_xml_reader *r);
/* Reader.Next: advances to the next token and gives its kind. */
BDF_INTERNAL int bdfi_xml_next(bdfi_xml_reader *r, int *kind);
/* Reader.LocalName of the start tag the reader is on. */
BDF_INTERNAL bdfi_svg_str bdfi_xml_local_name(const bdfi_xml_reader *r);
/* Reader.NextAttr: 1 and the next attribute of the start tag, its name and
 * its value as written (bdfi_xml_unescape decodes it), or 0 after the last. */
BDF_INTERNAL int bdfi_xml_next_attr(bdfi_xml_reader *r, bdfi_svg_str *name, bdfi_svg_str *value);
/* Reader.Text of a Text or CData token, as written. */
BDF_INTERNAL bdfi_svg_str bdfi_xml_text(const bdfi_xml_reader *r);
/* xmlro.Unescape: appends src with the five predefined entities and the
 * character references decoded and the line ends made "\n": 0, or -1 when
 * out of memory. */
BDF_INTERNAL int bdfi_xml_unescape(bdfi_xml_buf *dst, bdfi_svg_str src);
/* xmltree.AppendText: the character data of the Text or CData token the
 * reader is on, appended. */
BDF_INTERNAL int bdfi_xml_append_text(bdfi_xml_buf *dst, const bdfi_xml_reader *r);
BDF_INTERNAL int bdfi_xml_buf_append(bdfi_xml_buf *b, const void *p, size_t n);
BDF_INTERNAL void bdfi_xml_buf_free(bdfi_xml_buf *b);
/* xmltree.Local: the part of a qualified name after its prefix. */
BDF_INTERNAL bdfi_svg_str bdfi_xml_local(bdfi_svg_str name);

#endif /* BDF_SVG_INTERNAL_H */
