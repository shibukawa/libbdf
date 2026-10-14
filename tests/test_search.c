/* Tests of the search of a text index: what a character is to a search, and
 * where a hit lies in the runs. The cases follow the reference's TypeScript
 * reader (packages/core/src/search.ts and its tests); go/search_test.go
 * compares the two on the test documents and on random text. */
#include "testutil.h"

/* The characters of the tests, as UTF-8. */
#define FULL_ABC  "\xEF\xBC\xA1\xEF\xBC\xA2\xEF\xBC\xA3"             /* fullwidth ABC */
#define HALF_KANA "\xEF\xBD\xB6\xEF\xBE\x80\xEF\xBD\xB6\xEF\xBE\x85" /* halfwidth katakana "katakana" */
#define HIRA_KANA "\xE3\x81\x8B\xE3\x81\x9F\xE3\x81\x8B\xE3\x81\xAA" /* hiragana "katakana" */
#define KATA_KANA "\xE3\x82\xAB\xE3\x82\xBF\xE3\x82\xAB\xE3\x83\x8A" /* katakana "katakana" */
#define ROMAN_IX  "\xE2\x85\xA8"                                     /* U+2168 */
#define MINUS     "\xE2\x88\x92"                                     /* U+2212 */
#define KIRO      "\xE3\x8C\x94"                                     /* U+3314, the square "kiro" */
#define HIRA_RO   "\xE3\x82\x8D"
#define HIRA_KI   "\xE3\x81\x8D"
#define DOTTED_I  "\xC4\xB0"         /* U+0130 */
#define FI        "\xEF\xAC\x81"     /* U+FB01, the ligature */
#define BREAK     " \xE2\x8F\x8E "   /* how a paragraph break shows in a context */
#define MARK      "\xEF\xBB\xBF"     /* a byte order mark */
#define EMOJI     "\xF0\x9F\x98\x80" /* U+1F600 */
#define ACUTE     "\xCC\x81"         /* U+0301, combining */
#define DOT_BELOW "\xCC\xA3"         /* U+0323, combining */
#define E_ACUTE   "\xC3\xA9"         /* U+00E9 */
#define SCRIPT_A  "\xF0\x9D\x92\x9C" /* U+1D49C, a mathematical capital A */
#define HALF_GUIDE "\xEF\xBD\xB6\xEF\xBE\x9E\xEF\xBD\xB2\xEF\xBE\x84\xEF\xBE\x9E" /* halfwidth "gaido" */
#define JAMO_GAG  "\xE1\x84\x80\xE1\x85\xA1\xE1\x86\xA8" /* U+1100 U+1161 U+11A8 */
#define HANGUL_GAG "\xEA\xB0\x81"                           /* U+AC01, the syllable they are */

static void normalize_is(const char *in, int case_sensitive, const char *want, int line) {
  char *out = NULL;
  size_t n = 0;
  bdf_error err;
  if (bdf_text_search_normalize(in, strlen(in), case_sensitive, &out, &n, &err) != BDF_OK ||
      n != strlen(want) || memcmp(out, want, n) != 0 || out[n] != 0) {
    failures++;
    fprintf(stderr, "line %d: \"%s\" is \"%s\", want \"%s\"\n", line, in, out ? out : "(null)", want);
  }
  bdf_free(out);
}

static void test_normalize(void) {
  /* "normalization folds width, case and kana" of the reference's tests */
  normalize_is(FULL_ABC " " HALF_KANA " " ROMAN_IX, 0, "abc " HIRA_KANA " ix", __LINE__);
  normalize_is("ABC", 1, "ABC", __LINE__);
  normalize_is("b^2" MINUS "4ac", 0, "b^2-4ac", __LINE__);
  normalize_is("", 0, "", __LINE__);
  normalize_is(KATA_KANA, 1, HIRA_KANA, __LINE__); /* kana fold in a case-sensitive search too */
  normalize_is(FULL_ABC, 1, "ABC", __LINE__);
  normalize_is(KIRO, 0, HIRA_KI HIRA_RO, __LINE__); /* one character becomes several */
  normalize_is(FI, 0, "fi", __LINE__);
  normalize_is(DOTTED_I, 0, "i\xCC\x87", __LINE__); /* i and a combining dot */
  normalize_is(DOTTED_I, 1, DOTTED_I, __LINE__);
  /* "normalization is of the string, not of its characters one by one" of
   * the reference's tests: a letter and the mark after it are the letter
   * they compose */
  normalize_is("cafe" ACUTE, 0, "caf" E_ACUTE, __LINE__);
  normalize_is("CAFE" ACUTE, 1, "CAF\xC3\x89", __LINE__);
  /* halfwidth katakana with its voiced marks, and a kana with a combining one */
  normalize_is(HALF_GUIDE, 0, "\xE3\x81\x8C\xE3\x81\x84\xE3\x81\xA9", __LINE__);
  normalize_is("\xEF\xBE\x8A\xEF\xBE\x9F", 0, "\xE3\x81\xB1", __LINE__);
  normalize_is("\xE3\x81\x8B\xE3\x82\x99", 0, "\xE3\x81\x8C", __LINE__);
  /* the letters of a Hangul syllable */
  normalize_is(JAMO_GAG, 0, HANGUL_GAG, __LINE__);
  /* marks in another order than the canonical one */
  normalize_is("a" ACUTE DOT_BELOW, 0, "\xE1\xBA\xA1" ACUTE, __LINE__);
  normalize_is("a" DOT_BELOW ACUTE, 0, "\xE1\xBA\xA1" ACUTE, __LINE__);
  /* a minus sign that normalization makes of another character */
  normalize_is("10\xE2\x81\xBB\xC2\xB3", 0, "10-3", __LINE__);
  normalize_is("\xC3\x89", 0, "\xC3\xA9", __LINE__); /* É as é */
  normalize_is("\xCE\xA3", 0, "\xCF\x83", __LINE__); /* a capital sigma as the one inside a word */
  normalize_is(EMOJI, 0, EMOJI, __LINE__);
  /* bytes that are no UTF-8 are what a decoder makes of them */
  normalize_is("a\xFF" "b\xE3\x81", 0, "a\xEF\xBF\xBD" "b\xEF\xBF\xBD", __LINE__);

  char *out;
  size_t n;
  bdf_error err;
  CHECK(bdf_text_search_normalize(NULL, 1, 0, &out, &n, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_normalize("a", 1, 0, NULL, &n, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_normalize(NULL, 0, 0, &out, &n, &err) == BDF_OK && n == 0);
  bdf_free(out);
}

/* An index and a search of it. */
typedef struct fixture {
  wr part;
  bdf_text_index *index;
  bdf_text_search *search;
} fixture;

static void fixture_begin(fixture *f, size_t runs) {
  memset(f, 0, sizeof *f);
  w_bytes(&f->part, "BTXT", 4);
  w_u16(&f->part, 1);
  w_varuint(&f->part, runs);
}

static void fixture_open(fixture *f, int case_sensitive) {
  bdf_error err;
  if (bdf_text_index_decode(f->part.b, f->part.n, &f->index, &err) != BDF_OK ||
      bdf_text_search_new(f->index, case_sensitive, &f->search, &err) != BDF_OK) {
    fprintf(stderr, "fixture: %s\n", err.message);
    exit(1);
  }
}

static void fixture_end(fixture *f) {
  bdf_text_search_free(f->search);
  bdf_text_index_free(f->index);
  free(f->part.b);
}

/* expect_hits checks the hits of a query against entries joined by '|': the
 * text, then for each segment " run:start-end". */
static void expect_hits(const fixture *f, const char *query, size_t limit, const char *want, int line) {
  bdf_text_hits *hits = NULL;
  bdf_error err;
  char got[2048] = "";
  size_t k = 0;
  if (bdf_text_search_find(f->search, query, strlen(query), limit, 6, &hits, &err) != BDF_OK) {
    failures++;
    fprintf(stderr, "line %d: %s\n", line, err.message);
    return;
  }
  for (size_t i = 0; i < bdf_text_hits_count(hits) && k < sizeof got - 256; i++) {
    bdf_text_hit h;
    bdf_text_hits_at(hits, i, &h);
    k += (size_t)snprintf(got + k, sizeof got - k, "%s%s", i ? "|" : "", h.text);
    if (strlen(h.text) != h.text_len || strlen(h.context) != h.context_len) {
      failures++;
      fprintf(stderr, "line %d: hit %zu: lengths\n", line, i);
    }
    for (size_t s = 0; s < h.nsegments; s++) {
      bdf_text_hit_segment seg;
      bdf_index_run run;
      bdf_text_hits_segment(hits, i, s, &seg);
      k += (size_t)snprintf(got + k, sizeof got - k, " %zu:%zu-%zu", seg.run, seg.start, seg.end);
      /* a segment says what its run says */
      if (bdf_text_index_run(f->index, seg.run, &run) != BDF_OK || run.a != seg.a ||
          run.b != seg.b || run.ordinal != seg.ordinal || seg.end > run.text_len) {
        failures++;
        fprintf(stderr, "line %d: hit %zu segment %zu does not fit its run\n", line, i, s);
      }
    }
  }
  if (strcmp(got, want) != 0) {
    failures++;
    fprintf(stderr, "line %d: hits of \"%s\"\n  %s\nwant\n  %s\n", line, query, got, want);
  }
  bdf_text_hits_free(hits);
}

static void context_is(const fixture *f, const char *query, size_t context, const char *want, int line) {
  bdf_text_hits *hits = NULL;
  bdf_text_hit h;
  bdf_error err;
  if (bdf_text_search_find(f->search, query, strlen(query), 1, context, &hits, &err) != BDF_OK ||
      bdf_text_hits_at(hits, 0, &h) != BDF_OK || strcmp(h.context, want) != 0) {
    failures++;
    fprintf(stderr, "line %d: the context of \"%s\" is \"%s\", want \"%s\"\n", line, query,
            hits && bdf_text_hits_count(hits) ? h.context : "(no hit)", want);
  }
  bdf_text_hits_free(hits);
}

static void test_runs(void) {
  /* a line that goes on in the next run, a paragraph break, a run that joins
   * the one before it */
  fixture f;
  fixture_begin(&f, 5);
  w_index_run(&f.part, 0, 1, 0, BDF_SEP_BREAK, "Each page is a list of");
  w_index_run(&f.part, 0, 1, 1, BDF_SEP_SPACE, "objects, small.");
  w_index_run(&f.part, 0, 1, 2, BDF_SEP_BREAK, "Repeated list");
  w_index_run(&f.part, 2, 0, 7, BDF_SEP_NONE, "ings");
  w_index_run(&f.part, 2, 0, 9, 9, " end"); /* a separator no reader knows is a space */
  fixture_open(&f, 0);

  char *plain = NULL;
  size_t n = 0;
  bdf_error err;
  CHECK(bdf_text_search_plain(f.search, &plain, &n, &err) == BDF_OK);
  CHECK(plain && strcmp(plain, "each page is a list of objects, small.\nrepeated listings  end") == 0 &&
        n == strlen(plain));
  bdf_free(plain);

  /* across the runs of a line, whatever the case and the spaces of the query */
  expect_hits(&f, "LIST OF OBJECTS", 10, "list of objects 0:15-22 1:0-7", __LINE__);
  expect_hits(&f, "list \t of\n\nobjects", 10, "list of objects 0:15-22 1:0-7", __LINE__);
  /* never across a paragraph break */
  expect_hits(&f, "small. Repeated", 10, "", __LINE__);
  expect_hits(&f, "small.\nRepeated", 10, "", __LINE__);
  /* across runs that join, as the document writes them */
  expect_hits(&f, "listings", 10, "listings 2:9-13 3:0-4", __LINE__);
  /* every place, in order, up to the limit */
  expect_hits(&f, "list", 10, "list 0:15-19|list 2:9-13", __LINE__);
  expect_hits(&f, "list", 1, "list 0:15-19", __LINE__);
  expect_hits(&f, "list", 0, "", __LINE__);
  expect_hits(&f, "e", 3, "E 0:0-1|e 0:8-9|e 1:3-4", __LINE__);
  /* nothing to find */
  expect_hits(&f, "", 10, "", __LINE__);
  expect_hits(&f, "zzz", 10, "", __LINE__);
  expect_hits(&f, "a much longer query than the whole of the text that there is to search in", 10, "",
              __LINE__);
  /* a space of the query finds a space of a run, and the one between runs
   * only with what is around it: alone it lies in no run */
  expect_hits(&f, " ", 2, "  0:4-5|  0:9-10", __LINE__);
  expect_hits(&f, "of objects", 10, "of objects 0:20-22 1:0-7", __LINE__);
  /* the white space of a query is joined and that of the text is not: two
   * spaces in a row, here the one between the runs and the one a run starts
   * with, are found by no query */
  expect_hits(&f, "s  end", 10, "", __LINE__);
  expect_hits(&f, "s end", 10, "", __LINE__);
  expect_hits(&f, "listings", 10, "listings 2:9-13 3:0-4", __LINE__);

  /* the context: the searchable text around the hit, a break made visible */
  context_is(&f, "objects", 3, "of objects, s", __LINE__);
  context_is(&f, "small", 8, "bjects, small." BREAK "repeat", __LINE__);
  context_is(&f, "each", 4, "each pag", __LINE__);
  context_is(&f, "end", 100, "each page is a list of objects, small." BREAK "repeated listings  end", __LINE__);
  context_is(&f, "page", 0, "page", __LINE__);
  fixture_end(&f);

  /* a case-sensitive search of the same */
  fixture_begin(&f, 2);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, "List list LIST");
  w_index_run(&f.part, 0, 0, 1, BDF_SEP_SPACE, FULL_ABC);
  fixture_open(&f, 1);
  expect_hits(&f, "list", 10, "list 0:5-9", __LINE__);
  expect_hits(&f, "LIST ABC", 10, "LIST " FULL_ABC " 0:10-14 1:0-9", __LINE__); /* width still folds */
  expect_hits(&f, "abc", 10, "", __LINE__);
  fixture_end(&f);

  /* hits may overlap */
  fixture_begin(&f, 1);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, "aaaa");
  fixture_open(&f, 0);
  expect_hits(&f, "aa", 10, "aa 0:0-2|aa 0:1-3|aa 0:2-4", __LINE__);
  fixture_end(&f);

  /* no runs, and runs without text */
  fixture_begin(&f, 0);
  fixture_open(&f, 0);
  expect_hits(&f, "a", 10, "", __LINE__);
  CHECK(bdf_text_search_plain(f.search, &plain, &n, &err) == BDF_OK && n == 0 && plain[0] == 0);
  bdf_free(plain);
  fixture_end(&f);
  fixture_begin(&f, 3);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, "");
  w_index_run(&f.part, 0, 0, 1, BDF_SEP_SPACE, "");
  w_index_run(&f.part, 0, 0, 2, BDF_SEP_BREAK, "x");
  fixture_open(&f, 0);
  expect_hits(&f, " ", 10, "", __LINE__); /* the space between two runs is in neither */
  expect_hits(&f, "x", 10, "x 2:0-1", __LINE__);
  fixture_end(&f);
}

static void test_characters(void) {
  fixture f;
  fixture_begin(&f, 6);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, KATA_KANA " 10" KIRO "!");           /* 0 */
  w_index_run(&f.part, 0, 0, 1, BDF_SEP_BREAK, "x" MINUS "1 " FI "sh " DOTTED_I);    /* 1 */
  w_index_run(&f.part, 0, 0, 2, BDF_SEP_BREAK, MARK "marked");                       /* 2 */
  w_index_run(&f.part, 0, 0, 3, BDF_SEP_NONE, MARK " up " EMOJI "!");                /* 3 */
  w_index_run(&f.part, 0, 0, 4, BDF_SEP_BREAK, "bad\xFF" "bytes\xE3\x81");           /* 4 */
  w_index_run(&f.part, 0, 0, 5, BDF_SEP_BREAK, "caf\xC3\xA9 cafe\xCC\x81");          /* 5 */
  fixture_open(&f, 0);

  /* kana of either kind and width find each other */
  expect_hits(&f, HIRA_KANA, 10, KATA_KANA " 0:0-12", __LINE__);
  expect_hits(&f, HALF_KANA, 10, KATA_KANA " 0:0-12", __LINE__);
  /* a character that becomes several is found by any of them, whole */
  expect_hits(&f, "10" HIRA_KI, 10, "10" KIRO " 0:13-18", __LINE__);
  expect_hits(&f, HIRA_RO "!", 10, KIRO "! 0:15-19", __LINE__);
  expect_hits(&f, "x-1", 10, "x" MINUS "1 1:0-5", __LINE__);
  expect_hits(&f, "fish", 10, FI "sh 1:6-11", __LINE__);
  expect_hits(&f, "FISH I", 10, FI "sh " DOTTED_I " 1:6-14", __LINE__);
  /* a byte order mark at the start of a string is not read: the run is the
   * text after it */
  expect_hits(&f, "marked up", 10, "marked up 2:0-6 3:0-3", __LINE__);
  expect_hits(&f, EMOJI, 10, EMOJI " 3:4-8", __LINE__);
  bdf_index_run marked;
  CHECK(bdf_text_index_run(f.index, 2, &marked) == BDF_OK && marked.text_len == 6 &&
        memcmp(marked.text, "marked", 6) == 0);
  /* what is no UTF-8 is one unknown character for each stretch of it */
  expect_hits(&f, "bad\xEF\xBF\xBD" "bytes\xEF\xBF\xBD", 10, "bad\xFF" "bytes\xE3\x81 4:0-11", __LINE__);
  expect_hits(&f, "bytes", 10, "bytes 4:4-9", __LINE__);
  /* a letter and its accent are the letter with the accent, however the
   * document and the query write it; without the accent it is another */
  expect_hits(&f, "caf" E_ACUTE, 10, "caf" E_ACUTE " 5:0-5|cafe" ACUTE " 5:6-12", __LINE__);
  expect_hits(&f, "CAFE" ACUTE, 10, "caf" E_ACUTE " 5:0-5|cafe" ACUTE " 5:6-12", __LINE__);
  expect_hits(&f, "cafe", 10, "", __LINE__);
  /* the context counts characters, whatever their width in bytes */
  context_is(&f, "up", 3, "ed up " EMOJI "!", __LINE__);
  fixture_end(&f);
}

/* "a hit is located in the text it was normalized from" of the reference's
 * tests. */
static void test_pieces(void) {
  fixture f;
  fixture_begin(&f, 5);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, HALF_GUIDE);                           /* 0 */
  w_index_run(&f.part, 0, 0, 1, BDF_SEP_BREAK, SCRIPT_A " a" ACUTE DOT_BELOW "b");   /* 1 */
  w_index_run(&f.part, 0, 0, 2, BDF_SEP_BREAK, SCRIPT_A "e" ACUTE);                  /* 2 */
  w_index_run(&f.part, 0, 0, 3, BDF_SEP_BREAK, KIRO);                                /* 3 */
  w_index_run(&f.part, 0, 0, 4, BDF_SEP_BREAK, "x" JAMO_GAG "y");                    /* 4 */
  fixture_open(&f, 0);
  /* halfwidth katakana: a letter and its voiced mark are located together */
  expect_hits(&f, "\xE3\x82\xAC\xE3\x82\xA4\xE3\x83\x89", 10, HALF_GUIDE " 0:0-15", __LINE__);
  expect_hits(&f, "\xE3\x81\x84", 10, "\xEF\xBD\xB2 0:6-9", __LINE__);
  expect_hits(&f, "\xE3\x83\x89", 10, "\xEF\xBE\x84\xEF\xBE\x9E 0:9-15", __LINE__);
  expect_hits(&f, "\xE3\x83\x88", 10, "", __LINE__); /* "to" is not in "do" */
  /* marks out of order, and a character outside the BMP before them */
  expect_hits(&f, "a" DOT_BELOW ACUTE, 10, "a" ACUTE DOT_BELOW " 1:5-10", __LINE__);
  expect_hits(&f, "a" E_ACUTE, 10, SCRIPT_A "e" ACUTE " 2:0-7", __LINE__);
  /* a character that normalizes to several is located whole */
  expect_hits(&f, HIRA_RO, 10, KIRO " 3:0-3", __LINE__);
  /* the letters of a syllable */
  expect_hits(&f, HANGUL_GAG, 10, JAMO_GAG " 4:1-10", __LINE__);
  expect_hits(&f, "\xEA\xB0\x80", 10, "", __LINE__); /* not the syllable without its last letter */
  fixture_end(&f);

  /* a letter with more marks than a piece holds: the first 31 are with it,
   * the rest by themselves, and the search is built in time in proportion to
   * the text */
  enum { MARKS = 200000 };
  char *text = malloc(1 + 2 * MARKS + 2);
  if (!text) {
    exit(1);
  }
  text[0] = 'e';
  for (size_t i = 0; i < MARKS; i++) {
    memcpy(text + 1 + 2 * i, ACUTE, 2);
  }
  memcpy(text + 1 + 2 * MARKS, "x", 2);
  fixture_begin(&f, 1);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, text);
  fixture_open(&f, 0);
  free(text);
  bdf_text_hits *hits = NULL;
  bdf_text_hit_segment seg;
  bdf_error err;
  CHECK(bdf_text_search_find(f.search, E_ACUTE, 2, 10, 0, &hits, &err) == BDF_OK);
  CHECK(bdf_text_hits_count(hits) == 1 && bdf_text_hits_segment(hits, 0, 0, &seg) == BDF_OK &&
        seg.start == 0 && seg.end == 1 + 2 * 31);
  bdf_text_hits_free(hits);
  hits = NULL;
  CHECK(bdf_text_search_find(f.search, "x", 1, 10, 0, &hits, &err) == BDF_OK);
  CHECK(bdf_text_hits_count(hits) == 1 && bdf_text_hits_segment(hits, 0, 0, &seg) == BDF_OK &&
        seg.start == 1 + 2 * MARKS && seg.end == 2 + 2 * MARKS);
  bdf_text_hits_free(hits);
  fixture_end(&f);

  /* a run is normalized by itself: a mark at the start of the next is not
   * joined to the letter */
  fixture_begin(&f, 2);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, "e");
  w_index_run(&f.part, 0, 0, 1, BDF_SEP_NONE, ACUTE "x");
  fixture_open(&f, 0);
  expect_hits(&f, E_ACUTE, 10, "", __LINE__);
  expect_hits(&f, "e" ACUTE "x", 10, "", __LINE__);
  char *plain = NULL;
  size_t n = 0;
  CHECK(bdf_text_search_plain(f.search, &plain, &n, &err) == BDF_OK && n == 4 &&
        memcmp(plain, "e" ACUTE "x", 4) == 0);
  bdf_free(plain);
  fixture_end(&f);
}

static void test_arguments(void) {
  fixture f;
  fixture_begin(&f, 1);
  w_index_run(&f.part, 0, 0, 0, BDF_SEP_NONE, "text");
  fixture_open(&f, 0);
  bdf_text_search *s = NULL;
  bdf_text_hits *hits = NULL;
  bdf_text_hit h;
  bdf_text_hit_segment seg;
  bdf_error err;
  char *plain;
  size_t n;
  CHECK(bdf_text_search_new(NULL, 0, &s, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_new(f.index, 0, NULL, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_plain(NULL, &plain, &n, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_find(NULL, "t", 1, 1, 1, &hits, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_find(f.search, NULL, 1, 1, 1, &hits, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_find(f.search, "t", 1, 1, 1, NULL, &err) == BDF_ERR_ARG);
  CHECK(bdf_text_search_find(f.search, NULL, 0, 1, 1, &hits, &err) == BDF_OK);
  CHECK(bdf_text_hits_count(hits) == 0 && bdf_text_hits_at(hits, 0, &h) == BDF_ERR_ARG);
  bdf_text_hits_free(hits);
  CHECK(bdf_text_search_find(f.search, "t", 1, BDF_SEARCH_LIMIT, BDF_SEARCH_CONTEXT, &hits, &err) == BDF_OK);
  CHECK(bdf_text_hits_count(hits) == 2);
  CHECK(bdf_text_hits_at(hits, 1, &h) == BDF_OK && h.nsegments == 1);
  CHECK(bdf_text_hits_segment(hits, 1, 0, &seg) == BDF_OK && seg.start == 3 && seg.end == 4);
  CHECK(bdf_text_hits_segment(hits, 1, 1, &seg) == BDF_ERR_ARG);
  CHECK(bdf_text_hits_segment(hits, 2, 0, &seg) == BDF_ERR_ARG);
  CHECK(bdf_text_hits_at(hits, 0, NULL) == BDF_ERR_ARG);
  CHECK(bdf_text_hits_count(NULL) == 0);
  bdf_text_hits_free(hits);
  bdf_text_hits_free(NULL);
  bdf_text_search_free(NULL);
  fixture_end(&f);
}

int main(void) {
  test_normalize();
  test_runs();
  test_characters();
  test_pieces();
  test_arguments();
  return finish();
}
