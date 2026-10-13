/*
 * GIF images: a port of Go's image/gif, reader.go, and of the reader of
 * compress/lzw that it decompresses with (BSD license, see
 * third_party/go-image/LICENSE), so that an image decodes to the bytes the
 * reference renderer draws and the files gif.Decode refuses are refused.
 *
 * gif.Decode returns the first frame alone, an *image.Paletted whose bounds
 * are the frame's and not the logical screen's, and stops reading there:
 * what follows the first frame is never looked at. The decoding follows
 * decoder.decode step by step, with lzw.Reader's buffer and the 4096 bytes
 * it decodes at a time. (That amount does not show in the result: after
 * the last pixel Go asks the reader for one byte more, and the reader
 * decodes on until it has one, which is "too much", or its data ends.)
 *
 * The frame is not built: each pixel is converted as it leaves the LZW
 * reader, to what the reference's draw.Draw makes of the *image.Paletted.
 */
#include <stdlib.h>
#include <string.h>

#include "image_internal.h"

/* The constants of compress/lzw. */
#define BDFI_GIF_LZW_MAX_WIDTH    12
#define BDFI_GIF_LZW_INVALID_CODE 0xffff
#define BDFI_GIF_LZW_FLUSH_BUFFER 4096

/* The values the err fields of blockReader and lzw.Reader take. */
#define BDFI_GIF_ERR_NONE           0
#define BDFI_GIF_ERR_EOF            1 /* io.EOF */
#define BDFI_GIF_ERR_UNEXPECTED_EOF 2 /* io.ErrUnexpectedEOF */
#define BDFI_GIF_ERR_INVALID_CODE   3 /* "lzw: invalid code" */

/*
 * The most pixels a byte of LZW data can stand for: codes are at least three
 * bits wide, so a byte holds fewer than three, and a code expands to fewer
 * than 4096 pixels.
 */
#define BDFI_GIF_MAX_EXPANSION 12288

/* The header, the logical screen descriptor and the global colour table. */
typedef struct bdfi_gif_screen {
  int32_t width, height;
  const uint8_t *colors; /* the global colour table, three bytes a colour, or NULL */
  size_t n_colors;
  size_t end; /* the offset of what follows them */
} bdfi_gif_screen;

/* The interlaceScan passes of image/gif. */
static const uint8_t bdfi_gif_pass_skip[4] = {8, 8, 4, 2};
static const uint8_t bdfi_gif_pass_start[4] = {0, 4, 2, 1};

/* The state of decoding the first frame: decoder.r, the blockReader, the lzw.Reader and the
 * image the pixels go to. */
typedef struct bdfi_gif_decoder {
  /* decoder.r: the file and the position in it */
  const uint8_t *data;
  size_t len, pos;

  /* blockReader. Go copies each sub-block into decoder.tmp; here block points at it in the
   * file, so that block[i] to block[j] are the buffered bytes. */
  const uint8_t *block;
  uint8_t block_i, block_j;
  int block_err;

  /* lzw.Reader */
  uint32_t bits;
  uint32_t n_bits;
  uint32_t width;
  uint32_t lit_width;
  int err;
  uint16_t clear, eof, hi, overflow, last;
  uint8_t suffix[1 << BDFI_GIF_LZW_MAX_WIDTH];
  uint16_t prefix[1 << BDFI_GIF_LZW_MAX_WIDTH];
  uint8_t output[2 * (1 << BDFI_GIF_LZW_MAX_WIDTH)];
  size_t o;
  const uint8_t *to_read; /* the bytes Read has still to return */
  size_t to_read_len;

  /* The palette of the frame as the bytes each index is drawn with, and how many of its
   * indexes Go's palette has. */
  uint8_t palette[256 * 4];
  size_t n_palette;

  /* The frame: its size, where the next pixel goes, and whether an index was outside the
   * palette. */
  uint8_t *pix;
  size_t frame_w, frame_h;
  size_t x, y;
  int interlaced;
  uint32_t pass;
  int bad_pixel;
} bdfi_gif_decoder;

/*
 * decoder.readHeaderAndScreenDescriptor and readColorTable: 1, or 0 where
 * they return an error. Both read with io.ReadFull, so a file that ends
 * inside the header or the table is an error.
 */
static int bdfi_gif_read_screen(const uint8_t *data, size_t len, bdfi_gif_screen *s) {
  memset(s, 0, sizeof *s);
  if (len < 13) {
    return 0;
  }
  if (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0) {
    return 0;
  }
  s->width = (int32_t)data[6] + ((int32_t)data[7] << 8);
  s->height = (int32_t)data[8] + ((int32_t)data[9] << 8);
  s->end = 13;
  uint8_t fields = data[10];
  if ((fields & 0x80) != 0) {
    size_t n = (size_t)1 << (1 + (fields & 7));
    if (len - 13 < 3 * n) {
      return 0;
    }
    s->colors = data + 13;
    s->n_colors = n;
    s->end = 13 + 3 * n;
  }
  return 1;
}

/*
 * decoder.readExtension and readGraphicControl, from after the 0x21: 1 and
 * the position after the extension, or 0 where they return an error. Of what
 * the extensions say only the transparent index matters to the first frame.
 */
static int bdfi_gif_read_extension(const uint8_t *data, size_t len, size_t *at,
                                   int *has_transparent, uint8_t *transparent) {
  size_t pos = *at;
  if (pos >= len) {
    return 0;
  }
  uint8_t extension = data[pos++];
  size_t size = 0;
  switch (extension) {
  case 0x01: /* plain text */
    size = 13;
    break;
  case 0xf9: /* graphic control */
    if (len - pos < 6) {
      return 0;
    }
    if (data[pos] != 4) {
      return 0;
    }
    /* A flag that is not set leaves the index of an earlier graphic control standing. */
    if ((data[pos + 1] & 1) != 0) {
      *transparent = data[pos + 4];
      *has_transparent = 1;
    }
    if (data[pos + 5] != 0) {
      return 0;
    }
    *at = pos + 6;
    return 1;
  case 0xfe: /* comment */
    break;
  case 0xff: /* application: the size is taken as it is (11 by the specification) */
    if (pos >= len) {
      return 0;
    }
    size = data[pos++];
    break;
  default:
    return 0;
  }
  if (len - pos < size) {
    return 0;
  }
  pos += size;
  /* decoder.readBlock until an empty block. The loop count of a NETSCAPE2.0 extension is read
   * from the first of them, which changes nothing in what is read. */
  for (;;) {
    if (pos >= len) {
      return 0;
    }
    size_t n = data[pos++];
    if (n == 0) {
      break;
    }
    if (len - pos < n) {
      return 0;
    }
    pos += n;
  }
  *at = pos;
  return 1;
}

/* blockReader.fill: the next sub-block of the image data. */
static void bdfi_gif_block_fill(bdfi_gif_decoder *d) {
  if (d->block_err != BDFI_GIF_ERR_NONE) {
    return;
  }
  if (d->pos >= d->len) {
    d->block_j = 0;
    d->block_err = BDFI_GIF_ERR_UNEXPECTED_EOF;
    return;
  }
  d->block_j = d->data[d->pos++];
  if (d->block_j == 0) {
    /* the block terminator */
    d->block_err = BDFI_GIF_ERR_EOF;
    return;
  }
  d->block_i = 0;
  if (d->len - d->pos < d->block_j) {
    d->pos = d->len;
    d->block_j = 0;
    d->block_err = BDFI_GIF_ERR_UNEXPECTED_EOF;
    return;
  }
  d->block = d->data + d->pos;
  d->pos += d->block_j;
}

/*
 * blockReader.ReadByte: the next byte of the image data, or -1 with
 * block_err set. Go looks at the error only after a fill; lzw.Reader does not
 * read again after an error, so looking at it first, which keeps a stale
 * block from being read, is the same.
 */
static int bdfi_gif_block_byte(bdfi_gif_decoder *d) {
  if (d->block_err != BDFI_GIF_ERR_NONE) {
    return -1;
  }
  if (d->block_i == d->block_j) {
    bdfi_gif_block_fill(d);
    if (d->block_err != BDFI_GIF_ERR_NONE) {
      return -1;
    }
  }
  return d->block[d->block_i++];
}

/*
 * blockReader.close: 1 where it returns nil. After the image data Go takes
 * the rest of the sub-block the LZW data ended in, or, if the data filled
 * its sub-block, one more sub-block of a single byte, and then wants the
 * block terminator.
 */
static int bdfi_gif_block_close(bdfi_gif_decoder *d) {
  if (d->block_err == BDFI_GIF_ERR_EOF) {
    return 1;
  }
  if (d->block_err != BDFI_GIF_ERR_NONE) {
    return 0;
  }
  if (d->block_i == d->block_j) {
    bdfi_gif_block_fill(d);
    if (d->block_err == BDFI_GIF_ERR_EOF) {
      return 1;
    }
    if (d->block_err != BDFI_GIF_ERR_NONE) {
      return 0;
    }
    if (d->block_j > 1) {
      return 0; /* errTooMuch */
    }
  }
  bdfi_gif_block_fill(d);
  return d->block_err == BDFI_GIF_ERR_EOF;
}

/* Reader.readLSB of compress/lzw: the next code, or -1 when the image data has no more. */
static int bdfi_gif_lzw_code(bdfi_gif_decoder *d) {
  while (d->n_bits < d->width) {
    int x = bdfi_gif_block_byte(d);
    if (x < 0) {
      return -1;
    }
    d->bits |= (uint32_t)x << d->n_bits;
    d->n_bits += 8;
  }
  uint32_t code = d->bits & (((uint32_t)1 << d->width) - 1);
  d->bits >>= d->width;
  d->n_bits -= d->width;
  return (int)code;
}

/*
 * Reader.decode of compress/lzw: decompresses until 4096 bytes or more are
 * in output, or the data ends, and leaves them in to_read.
 *
 * The indexes are in bounds for the reasons they are in Go. hi is below
 * overflow, which is at most 4096, and last is a code that was read, at most
 * hi. A code that is expanded lies between eof and hi and was entered since
 * the last clear code with a prefix below itself, so that a chain is shorter
 * than 4096 and ends at a literal; the expansion is written downwards from
 * the end of output and stays above the 4096 bytes that o is below.
 */
static void bdfi_gif_lzw_decode(bdfi_gif_decoder *d) {
  for (;;) {
    int read = bdfi_gif_lzw_code(d);
    if (read < 0) {
      /* the end of the sub-blocks (io.EOF) and the end of the file alike */
      d->err = BDFI_GIF_ERR_UNEXPECTED_EOF;
      break;
    }
    uint16_t code = (uint16_t)read;
    if (code < d->clear) {
      /* a literal code */
      d->output[d->o] = (uint8_t)code;
      d->o++;
      if (d->last != BDFI_GIF_LZW_INVALID_CODE) {
        /* save what the hi code expands to */
        d->suffix[d->hi] = (uint8_t)code;
        d->prefix[d->hi] = d->last;
      }
    } else if (code == d->clear) {
      d->width = 1 + d->lit_width;
      d->hi = d->eof;
      d->overflow = (uint16_t)((uint32_t)1 << d->width);
      d->last = BDFI_GIF_LZW_INVALID_CODE;
      continue;
    } else if (code == d->eof) {
      d->err = BDFI_GIF_ERR_EOF;
      break;
    } else if (code <= d->hi) {
      uint16_t c = code;
      size_t i = sizeof d->output - 1;
      if (code == d->hi && d->last != BDFI_GIF_LZW_INVALID_CODE) {
        /* code == hi expands to the last expansion followed by its head: walk the prefix
         * chain to the literal it starts with */
        c = d->last;
        while (c >= d->clear) {
          c = d->prefix[c];
        }
        d->output[i] = (uint8_t)c;
        i--;
        c = d->last;
      }
      /* copy the suffix chain into output, from the end */
      while (c >= d->clear) {
        d->output[i] = d->suffix[c];
        i--;
        c = d->prefix[c];
      }
      d->output[i] = (uint8_t)c;
      size_t n = sizeof d->output - i;
      memmove(d->output + d->o, d->output + i, n);
      d->o += n;
      if (d->last != BDFI_GIF_LZW_INVALID_CODE) {
        /* save what the hi code expands to */
        d->suffix[d->hi] = (uint8_t)c;
        d->prefix[d->hi] = d->last;
      }
    } else {
      d->err = BDFI_GIF_ERR_INVALID_CODE;
      break;
    }
    d->last = code;
    d->hi++;
    if (d->hi >= d->overflow) {
      if (d->width == BDFI_GIF_LZW_MAX_WIDTH) {
        /* the table is full: nothing is entered until a clear code */
        d->last = BDFI_GIF_LZW_INVALID_CODE;
        d->hi--;
      } else {
        d->width++;
        d->overflow = (uint16_t)((uint32_t)1 << d->width);
      }
    }
    if (d->o >= BDFI_GIF_LZW_FLUSH_BUFFER) {
      break;
    }
  }
  /* flush pending output */
  d->to_read = d->output;
  d->to_read_len = d->o;
  d->o = 0;
}

/*
 * Takes n pixels of the frame, in the order of the file, as the reference
 * draws them. The *image.Paletted that gif.Decode returns goes through
 * drawRGBA of image/draw, the general case, which reads Paletted.RGBA64At:
 * the palette's color.RGBA as 16 bits a channel, of which the high byte is
 * kept. That is r, g, b, 0xff for a colour of the table and four zeros for
 * the transparent index. Go checks afterwards that every index is inside
 * the palette (errBadPixel) and then reorders the rows of an interlaced
 * frame; here the check and the reordering happen as the pixels arrive.
 */
static void bdfi_gif_put_pixels(bdfi_gif_decoder *d, const uint8_t *index, size_t n) {
  while (n > 0) {
    size_t run = d->frame_w - d->x;
    if (run > n) {
      run = n;
    }
    uint8_t *p = d->pix + (d->y * d->frame_w + d->x) * 4;
    for (size_t i = 0; i < run; i++) {
      size_t c = index[i];
      if (c >= d->n_palette) {
        d->bad_pixel = 1;
      }
      memcpy(p, d->palette + 4 * c, 4);
      p += 4;
    }
    index += run;
    n -= run;
    d->x += run;
    if (d->x == d->frame_w) {
      /* the next row: of the same pass of uninterlace, or the first of the next pass that
       * has rows */
      d->x = 0;
      if (!d->interlaced) {
        d->y++;
      } else {
        d->y += bdfi_gif_pass_skip[d->pass];
        while (d->y >= d->frame_h && d->pass < 3) {
          d->pass++;
          d->y = bdfi_gif_pass_start[d->pass];
        }
      }
    }
  }
}

/*
 * The image data of decoder.readImageDescriptor, from lzw.NewReader on: 1
 * where Go goes on to return the frame, 0 where it returns an error.
 */
static int bdfi_gif_read_pixels(bdfi_gif_decoder *d) {
  /* readFull(lzwr, m.Pix): Reader.Read hands out what a decode left, and decodes again when
   * that is used up */
  size_t total = d->frame_w * d->frame_h;
  size_t have = 0;
  while (have < total) {
    if (d->to_read_len > 0) {
      size_t n = d->to_read_len;
      if (n > total - have) {
        n = total - have;
      }
      bdfi_gif_put_pixels(d, d->to_read, n);
      d->to_read += n;
      d->to_read_len -= n;
      have += n;
      continue;
    }
    if (d->err != BDFI_GIF_ERR_NONE) {
      return 0; /* errNotEnough, or an invalid code */
    }
    bdfi_gif_lzw_decode(d);
  }
  /* lzwr.Read of one more byte: there must be none (errTooMuch), and the LZW data must have
   * ended with its end code or with the sub-blocks, which giflib's files do */
  for (;;) {
    if (d->to_read_len > 0) {
      return 0;
    }
    if (d->err != BDFI_GIF_ERR_NONE) {
      break;
    }
    bdfi_gif_lzw_decode(d);
  }
  if (d->err != BDFI_GIF_ERR_EOF && d->err != BDFI_GIF_ERR_UNEXPECTED_EOF) {
    return 0;
  }
  if (!bdfi_gif_block_close(d)) {
    return 0;
  }
  /* errBadPixel */
  return !d->bad_pixel;
}

/*
 * decoder.readImageDescriptor, from after the 0x2c, for the first frame,
 * with the conversion of the frame to premultiplied RGBA.
 */
static bdf_status bdfi_gif_read_image(const uint8_t *data, size_t len, size_t pos,
                                      const bdfi_gif_screen *s, int has_transparent,
                                      uint8_t transparent, bdfi_bitmap *out) {
  /* decoder.newImageFromDescriptor */
  if (len - pos < 9) {
    return BDF_ERR_FORMAT;
  }
  int32_t left = (int32_t)data[pos] + ((int32_t)data[pos + 1] << 8);
  int32_t top = (int32_t)data[pos + 2] + ((int32_t)data[pos + 3] << 8);
  int32_t width = (int32_t)data[pos + 4] + ((int32_t)data[pos + 5] << 8);
  int32_t height = (int32_t)data[pos + 6] + ((int32_t)data[pos + 7] << 8);
  uint8_t fields = data[pos + 8];
  pos += 9;
  /* the frame has to lie inside the logical screen */
  if (left + width > s->width || top + height > s->height) {
    return BDF_ERR_FORMAT;
  }
  /* Go decodes a frame without pixels as any other, and the image it returns for one has
   * empty bounds, which the reference takes for an error as it does a failure to decode. */
  if (width == 0 || height == 0) {
    return BDF_ERR_FORMAT;
  }
  const uint8_t *colors = s->colors;
  size_t n_colors = s->n_colors;
  if ((fields & 0x80) != 0) {
    /* a local colour table: readColorTable */
    n_colors = (size_t)1 << (1 + (fields & 7));
    if (len - pos < 3 * n_colors) {
      return BDF_ERR_FORMAT;
    }
    colors = data + pos;
    pos += 3 * n_colors;
  } else if (!colors) {
    return BDF_ERR_FORMAT; /* "gif: no color table" */
  }
  if (pos >= len) {
    return BDF_ERR_FORMAT;
  }
  uint8_t lit_width = data[pos++];
  if (lit_width < 2 || lit_width > 8) {
    return BDF_ERR_FORMAT;
  }
  /* The frame is inside the screen, which was found small enough, and so is small enough
   * itself. A guard of this decoder's own: image data too short to expand to the frame, which
   * Go finds out by decoding it (errNotEnough), is refused before the frame is allocated. */
  size_t frame_w = (size_t)width, frame_h = (size_t)height;
  if (frame_w * frame_h / BDFI_GIF_MAX_EXPANSION > len - pos) {
    return BDF_ERR_FORMAT;
  }
  bdfi_gif_decoder *d = bdfi_calloc_array(1, sizeof *d);
  if (!d) {
    return BDF_ERR_NOMEM;
  }
  d->pix = bdfi_malloc_array(frame_w * frame_h, 4);
  if (!d->pix) {
    free(d);
    return BDF_ERR_NOMEM;
  }
  d->data = data;
  d->len = len;
  d->pos = pos;
  d->frame_w = frame_w;
  d->frame_h = frame_h;
  d->interlaced = (fields & 0x40) != 0;

  /* The palette: the colours of the table, opaque. With a transparent index that entry is
   * color.RGBA{}; an index past the table makes Go lengthen the palette to it with
   * color.RGBA{}, and the indexes in between are then pixels like any other. The entries
   * past the table are zero here from the allocation. */
  for (size_t i = 0; i < n_colors; i++) {
    d->palette[4 * i + 0] = colors[3 * i + 0];
    d->palette[4 * i + 1] = colors[3 * i + 1];
    d->palette[4 * i + 2] = colors[3 * i + 2];
    d->palette[4 * i + 3] = 0xff;
  }
  d->n_palette = n_colors;
  if (has_transparent) {
    memset(d->palette + 4 * (size_t)transparent, 0, 4);
    if ((size_t)transparent >= d->n_palette) {
      d->n_palette = (size_t)transparent + 1;
    }
  }

  /* Reader.init of compress/lzw */
  d->lit_width = lit_width;
  d->width = 1 + (uint32_t)lit_width;
  d->clear = (uint16_t)((uint32_t)1 << lit_width);
  d->eof = (uint16_t)(d->clear + 1);
  d->hi = d->eof;
  d->overflow = (uint16_t)((uint32_t)1 << d->width);
  d->last = BDFI_GIF_LZW_INVALID_CODE;

  if (!bdfi_gif_read_pixels(d)) {
    free(d->pix);
    free(d);
    return BDF_ERR_FORMAT;
  }
  out->w = width;
  out->h = height;
  out->pix = d->pix;
  free(d);
  return BDF_OK;
}

/* gif.DecodeConfig: the size of the logical screen, which need not be that of the frame. */
BDF_INTERNAL int bdfi_gif_config(const uint8_t *data, size_t len, int32_t *w, int32_t *h) {
  bdfi_gif_screen s;
  *w = 0;
  *h = 0;
  if (!bdfi_gif_read_screen(data, len, &s)) {
    return 0;
  }
  *w = s.width;
  *h = s.height;
  return 1;
}

/* gif.Decode, after the size check of imgconv.Decode, and the conversion to premultiplied
 * RGBA. */
BDF_INTERNAL bdf_status bdfi_gif_decode(const uint8_t *data, size_t len, bdfi_bitmap *out) {
  memset(out, 0, sizeof *out);
  bdfi_gif_screen s;
  if (!bdfi_gif_read_screen(data, len, &s)) {
    return BDF_ERR_FORMAT;
  }
  /* imgconv.Decode refuses these sizes on what DecodeConfig says, the logical screen, before
   * it decodes */
  if (s.width <= 0 || s.height <= 0 || s.width > BDFI_MAX_DECODE_PIXELS / s.height) {
    return BDF_ERR_TOO_LARGE;
  }
  /* decoder.decode: the blocks up to the first image descriptor */
  size_t pos = s.end;
  int has_transparent = 0;
  uint8_t transparent = 0;
  for (;;) {
    if (pos >= len) {
      return BDF_ERR_FORMAT;
    }
    uint8_t c = data[pos++];
    if (c == 0x2c) {
      break;
    }
    /* anything but an extension is the trailer before any image, or an unknown block */
    if (c != 0x21 || !bdfi_gif_read_extension(data, len, &pos, &has_transparent, &transparent)) {
      return BDF_ERR_FORMAT;
    }
  }
  return bdfi_gif_read_image(data, len, pos, &s, has_transparent, transparent, out);
}
