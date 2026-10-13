/* What the tests of the port ask the C reader: not a part of the library. */
#ifndef SVGPORT_H
#define SVGPORT_H

#include <stddef.h>
#include <stdint.h>

/* The document as text (see dump.go, which writes the reference's document
 * the same way), malloc'd, or NULL when out of memory. deep adds what the
 * value functions make of every attribute; exact writes the floats that the
 * reference computes with fused multiply-adds on some processors. */
char *svgport_dump(const uint8_t *data, size_t len, int64_t max_depth, int64_t max_nodes,
                   int64_t max_matches, int deep, int exact, size_t *out_len);

/* bdfi_parse_float of count strings, one after the other in s (ends[i] is
 * where string i ends): the bits of each result and what was returned. */
void svgport_floats(const char *s, const uint32_t *ends, size_t count, uint64_t *bits,
                    int8_t *status);

/* The value functions of one string, as the lines of a deep dump. */
char *svgport_values(const char *s, size_t n, int exact, size_t *out_len);

/* bdfi_svg_to_lower and bdfi_svg_is_space of the characters [from, to). */
void svgport_runes(uint32_t from, uint32_t to, uint32_t *lower, uint8_t *space);

/* Go's strings.TrimSpace, strings.ToLower and strings.Fields of a string, as
 * lines. */
char *svgport_strings(const char *s, size_t n, size_t *out_len);

void svgport_mat_mul(const double *m, const double *n, double *out);

/* Reads a document `times` times and frees it; the status of the last. */
int svgport_read(const uint8_t *data, size_t len, int times);

/* bdfi_svg_parse_numbers of a string `times` times; how many numbers. */
size_t svgport_numbers(const char *s, size_t n, int times);

#endif
