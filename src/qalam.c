/*
 * qalam - an Arabic-first document compiler that writes PDFs whose text
 * copies and searches correctly.
 *
 * Why Arabic PDFs break: a PDF stores *glyphs* at positions, not text.
 * Shaping turns letters into contextual forms and ligatures, bidi turns
 * logical order into visual order, and extractors must guess the text back.
 *
 * What qalam does about it:
 *   - shapes with HarfBuzz, reorders with FriBidi
 *   - builds ToUnicode from the shaping result (forms -> base letters,
 *     lam-alef -> lam + alef, mark glyphs -> harakat); a glyph that means
 *     different text in different places gets its own CID (CIDToGIDMap)
 *   - wraps each line in /Span <</ActualText (logical text)>> BDC ... EMC
 *   - draws glyphs in visual order, one TJ per word
 *
 * Document features (v0.2): several fonts, numbered headings, table of
 * contents with links, PDF bookmarks, [@key] citations with a linked
 * bibliography, a small TikZ-like drawing language, tables, quotes, lists,
 * justified paragraphs, running header and page numbers.
 * See README.md for the input format.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>
#include <hb.h>
#include <hb-ot.h>
#include <hb-subset.h>
#include <fribidi.h>
#include <zlib.h>
#include "gram.h"
#include "qalam_grammar.h"   /* rule ids and the grammar text, made by gramgen from src/qalam.gram */

/* ------------------------------------------------------------------ util */

typedef struct { char *p; size_t n, cap; } Buf;

static void buf_put(Buf *b, const void *d, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n >= (int)sizeof tmp) {
        char *big = malloc(n + 1);
        va_start(ap, fmt);
        vsnprintf(big, n + 1, fmt, ap);
        va_end(ap);
        buf_put(b, big, n);
        free(big);
    } else {
        buf_put(b, tmp, n);
    }
}

static void die(const char *msg) { fprintf(stderr, "qalam: %s\n", msg); exit(1); }

/* Append a UTF-32 string as a PDF hex string in UTF-16BE with BOM. */
static void put_utf16_hex(Buf *b, const uint32_t *s, int n, int bom) {
    buf_put(b, "<", 1);
    if (bom) buf_put(b, "FEFF", 4);
    for (int i = 0; i < n; i++) {
        uint32_t c = s[i];
        if (c >= 0x10000) {
            c -= 0x10000;
            buf_printf(b, "%04X%04X", 0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF));
        } else {
            buf_printf(b, "%04X", c);
        }
    }
    buf_put(b, ">", 1);
}

/* ----------------------------------------------------------------- font */

typedef struct {
    hb_blob_t *blob;
    hb_face_t *face;
    hb_font_t *font;
    unsigned upem;
    unsigned nglyphs;
    unsigned char *used;      /* glyph ids that appear in the document      */
    /* CIDs: CID == GID for the first nglyphs; extra CIDs point at an existing
     * glyph but carry a different ToUnicode string. That lets one glyph mean
     * different text in different places (a mirrored bracket, a shared form). */
    int ncid, capcid;
    unsigned *cid_gid;
    uint32_t **uni;           /* cid -> unicode string (ToUnicode), or 0     */
    int *uni_len;
    int *cid_w;               /* cid -> advance width in 1/1000 em (PDF /W)  */
    int *next_same;           /* chain of extra CIDs sharing a glyph         */
    int *first_extra;         /* gid -> first extra CID, or -1               */
    char name[64];
} Font;

static void font_load(Font *f, const char *path) {
    f->blob = hb_blob_create_from_file_or_fail(path);
    if (!f->blob) die("cannot open font file");
    f->face = hb_face_create(f->blob, 0);
    f->font = hb_font_create(f->face);
    f->upem = hb_face_get_upem(f->face);
    hb_font_set_scale(f->font, f->upem, f->upem);
    f->nglyphs = hb_face_get_glyph_count(f->face);
    f->used = calloc(f->nglyphs, 1);
    f->ncid = f->nglyphs;
    f->capcid = f->nglyphs + 256;
    f->cid_gid = malloc(f->capcid * sizeof *f->cid_gid);
    for (unsigned i = 0; i < f->nglyphs; i++) f->cid_gid[i] = i;
    f->uni = calloc(f->capcid, sizeof *f->uni);
    f->uni_len = calloc(f->capcid, sizeof *f->uni_len);
    f->cid_w = malloc(f->capcid * sizeof *f->cid_w);
    f->next_same = malloc(f->capcid * sizeof *f->next_same);
    f->first_extra = malloc(f->nglyphs * sizeof *f->first_extra);
    for (unsigned i = 0; i < f->nglyphs; i++) {
        f->cid_w[i] = (int)lround(hb_font_get_glyph_h_advance(f->font, i) * 1000.0 / f->upem);
        f->first_extra[i] = -1;
    }
    hb_blob_t *glyf = hb_face_reference_table(f->face, HB_TAG('g','l','y','f'));
    if (hb_blob_get_length(glyf) == 0) die("font has no glyf table (CFF fonts not supported yet)");
    hb_blob_destroy(glyf);
    /* PostScript-ish name for BaseFont: keep only safe characters */
    char nm[64] = "QalamFont";
    unsigned len = sizeof nm;
    hb_ot_name_get_utf8(f->face, HB_OT_NAME_ID_POSTSCRIPT_NAME, HB_LANGUAGE_INVALID, &len, nm);
    int j = 0;
    for (int i = 0; nm[i] && j < 60; i++)
        if ((nm[i] >= 'A' && nm[i] <= 'Z') || (nm[i] >= 'a' && nm[i] <= 'z') ||
            (nm[i] >= '0' && nm[i] <= '9') || nm[i] == '-') f->name[j++] = nm[i];
    f->name[j] = 0;
    if (!j) strcpy(f->name, "QalamFont");
}

static int same_text(const uint32_t *a, int na, const uint32_t *b, int nb) {
    return na == nb && !memcmp(a, b, na * sizeof *a);
}

/* Extra CIDs: same glyph, different ToUnicode text and/or different width.
 * Giving each occurrence the width it is actually set at means a line can be
 * one continuous TJ with no positioning jumps, which extractors read as spaces. */
static int new_cid(Font *f, unsigned gid, const uint32_t *s, int n, int has_text, int w) {
    if (f->ncid >= 0xFFFF) return -1;                 /* out of 2-byte codes */
    if (f->ncid == f->capcid) {
        f->capcid *= 2;
        f->cid_gid = realloc(f->cid_gid, f->capcid * sizeof *f->cid_gid);
        f->uni = realloc(f->uni, f->capcid * sizeof *f->uni);
        f->uni_len = realloc(f->uni_len, f->capcid * sizeof *f->uni_len);
        f->cid_w = realloc(f->cid_w, f->capcid * sizeof *f->cid_w);
        f->next_same = realloc(f->next_same, f->capcid * sizeof *f->next_same);
    }
    int c = f->ncid++;
    f->cid_gid[c] = gid;
    f->uni[c] = NULL;
    f->uni_len[c] = 0;
    if (has_text) {
        f->uni[c] = malloc((n ? n : 1) * sizeof *s);
        if (n) memcpy(f->uni[c], s, n * sizeof *s);
        f->uni_len[c] = n;
    }
    f->cid_w[c] = w;
    f->next_same[c] = f->first_extra[gid];
    f->first_extra[gid] = c;
    return c;
}

static int same_cid_text(Font *f, int a, int b) {
    if (!f->uni[a] || !f->uni[b]) return f->uni[a] == f->uni[b];
    return same_text(f->uni[a], f->uni_len[a], f->uni[b], f->uni_len[b]);
}

/* CID that draws glyph gid and extracts as text s[0..n) (n == 0: no text). */
static unsigned font_cid_text(Font *f, unsigned gid, const uint32_t *s, int n) {
    if (gid == 0) return gid;
    int w = f->cid_w[gid];
    if (n > 0 && !f->uni[gid]) {
        f->uni[gid] = malloc(n * sizeof *s);
        memcpy(f->uni[gid], s, n * sizeof *s);
        f->uni_len[gid] = n;
        return gid;
    }
    if (n > 0 && same_text(f->uni[gid], f->uni_len[gid], s, n)) return gid;
    for (int c = f->first_extra[gid]; c >= 0; c = f->next_same[c])
        if (f->cid_w[c] == w && f->uni[c] && same_text(f->uni[c], f->uni_len[c], s, n)) return c;
    int c = new_cid(f, gid, s, n, 1, w);
    return c < 0 ? gid : (unsigned)c;
}
static unsigned font_cid(Font *f, unsigned gid, const uint32_t *s, int n) {
    return n <= 0 ? gid : font_cid_text(f, gid, s, n);
}
static unsigned font_cid_empty(Font *f, unsigned gid) { return font_cid_text(f, gid, NULL, 0); }
static int cid_is_empty(Font *f, unsigned c) { return f->uni[c] && f->uni_len[c] == 0; }

/* the CID with the same glyph and text as cid, but advance width w */
static unsigned font_cid_width(Font *f, unsigned cid, int w) {
    if (f->cid_w[cid] == w) return cid;
    unsigned gid = f->cid_gid[cid];
    if (gid != cid && f->cid_w[gid] == w && same_cid_text(f, gid, cid)) return gid;
    for (int c = f->first_extra[gid]; c >= 0; c = f->next_same[c])
        if (f->cid_w[c] == w && same_cid_text(f, c, cid)) return c;
    int c = new_cid(f, gid, f->uni[cid], f->uni_len[cid], f->uni[cid] != NULL, w);
    return c < 0 ? cid : (unsigned)c;
}

static void font_map(Font *f, unsigned gid, const uint32_t *s, int n) {
    if (gid == 0 || gid >= f->nglyphs || f->uni[gid] || n <= 0) return;
    f->uni[gid] = malloc(n * sizeof(uint32_t));
    memcpy(f->uni[gid], s, n * sizeof(uint32_t));
    f->uni_len[gid] = n;
}

/* -------------------------------------------------------------- shaping */

typedef struct { int start, len, level; } Run;

static int is_space(uint32_t c) { return c == ' ' || c == '\t' || c == 0x00A0; }

static hb_buffer_t *shape(Font *f, const uint32_t *para, int plen, int start, int len, int rtl) {
    hb_buffer_t *b = hb_buffer_create();
    hb_buffer_add_utf32(b, para, plen, start, len);
    hb_buffer_set_direction(b, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(b);
    hb_shape(f->font, b, NULL, 0);
    return b;
}

static double shaped_width(Font *f, const uint32_t *para, int plen, int start, int len,
                           int rtl, double size) {
    hb_buffer_t *b = shape(f, para, plen, start, len, rtl);
    unsigned n;
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(b, &n);
    double w = 0;
    for (unsigned i = 0; i < n; i++) w += pos[i].x_advance;
    hb_buffer_destroy(b);
    return w * size / f->upem;
}
static int is_mark_char(uint32_t c) {
    hb_unicode_general_category_t gc = hb_unicode_general_category(hb_unicode_funcs_get_default(), c);
    return gc == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
           gc == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK ||
           gc == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}

/* Give every shaped glyph a CID whose ToUnicode text is what it stands for,
 * and replace the glyph id in the buffer with that CID.
 * - a cluster drawn by one glyph: that glyph stands for all its characters
 *   (contextual form -> base letter, lam-alef ligature -> lam + alef,
 *    mirrored bracket -> the bracket as typed)
 * - a cluster of base glyph + mark glyphs: base glyph -> base letters,
 *   each mark glyph -> one mark character, in order                      */
static void assign_cids(Font *f, const uint32_t *para, int run_end, hb_buffer_t *b) {
    unsigned n;
    hb_glyph_info_t *inf = hb_buffer_get_glyph_infos(b, &n);
    unsigned *cid = malloc(n * sizeof *cid);
    for (unsigned i = 0; i < n; i++) { f->used[inf[i].codepoint] = 1; cid[i] = inf[i].codepoint; }
    for (unsigned i = 0; i < n; i++) {
        unsigned c = inf[i].cluster;
        unsigned first = n;  /* only process each cluster once */
        for (unsigned k = 0; k < n; k++) if (inf[k].cluster == c) { first = k; break; }
        if (first != i) continue;
        unsigned next = run_end;
        for (unsigned k = 0; k < n; k++)
            if (inf[k].cluster > c && inf[k].cluster < next) next = inf[k].cluster;
        unsigned idx[32]; int ng = 0;
        for (unsigned k = 0; k < n && ng < 32; k++) if (inf[k].cluster == c) idx[ng++] = k;
        if (ng == 1) { cid[idx[0]] = font_cid(f, inf[idx[0]].codepoint, para + c, next - c); continue; }
        uint32_t base[32], marks[32]; int nb = 0, nm = 0;
        for (unsigned t = c; t < next && nb < 32 && nm < 32; t++) {
            if (is_mark_char(para[t])) marks[nm++] = para[t]; else base[nb++] = para[t];
        }
        unsigned ib[32], im[32]; int ngb = 0, ngm = 0;
        for (int k = 0; k < ng; k++) {
            if (hb_ot_layout_get_glyph_class(f->face, inf[idx[k]].codepoint) == HB_OT_LAYOUT_GLYPH_CLASS_MARK) im[ngm++] = idx[k];
            else ib[ngb++] = idx[k];
        }
        /* RTL buffers list a cluster's glyphs in reverse */
        int rtl = HB_DIRECTION_IS_BACKWARD(hb_buffer_get_direction(b));
        if (ngb == 1 && nb > 0) cid[ib[0]] = font_cid(f, inf[ib[0]].codepoint, base, nb);
        else if (ngb > 1 && nb > 0) {
            /* a letter drawn from several pieces (e.g. a kaf body plus its stroke):
             * the logically first piece carries the text, the rest extract as nothing */
            int lead = rtl ? ngb - 1 : 0;
            for (int k = 0; k < ngb; k++)
                cid[ib[k]] = k == lead ? font_cid(f, inf[ib[k]].codepoint, base, nb)
                                       : font_cid_empty(f, inf[ib[k]].codepoint);
        }
        if (ngm == nm)
            for (int k = 0; k < nm; k++) {
                unsigned gi = im[rtl ? nm - 1 - k : k];
                cid[gi] = font_cid(f, inf[gi].codepoint, &marks[k], 1);
            }
    }
    for (unsigned i = 0; i < n; i++) inf[i].codepoint = cid[i];
    free(cid);
}

typedef struct { FILE *fp; long *off; int nobj, cap; } Pdf;

static int pdf_begin_obj(Pdf *p, int id) {
    if (id >= p->cap) {
        int old = p->cap;
        p->cap = id * 2 + 16;
        p->off = realloc(p->off, p->cap * sizeof(long));
        memset(p->off + old, 0, (p->cap - old) * sizeof(long));
    }
    p->off[id] = ftell(p->fp);
    if (id > p->nobj) p->nobj = id;
    fprintf(p->fp, "%d 0 obj\n", id);
    return id;
}

static void pdf_stream(Pdf *p, int id, const char *dict_extra, const void *data, size_t n, int compress) {
    pdf_begin_obj(p, id);
    if (compress) {
        uLongf zn = compressBound(n);
        unsigned char *z = malloc(zn);
        compress2(z, &zn, data, n, 9);
        fprintf(p->fp, "<< /Length %lu /Filter /FlateDecode %s >>\nstream\n", (unsigned long)zn, dict_extra ? dict_extra : "");
        fwrite(z, 1, zn, p->fp);
        free(z);
    } else {
        fprintf(p->fp, "<< /Length %zu %s >>\nstream\n", n, dict_extra ? dict_extra : "");
        fwrite(data, 1, n, p->fp);
    }
    fprintf(p->fp, "\nendstream\nendobj\n");
}
/* ============================================================== document */

#define PAGE_W 595.0
#define PAGE_H 842.0
#define MARGIN 64.0
#define TOP (PAGE_H - 84.0)
#define BOTTOM 70.0
#define TEXT_W (PAGE_W - 2 * MARGIN)
#define MAXFONTS 16

typedef struct { double r, g, b; } Color;
static const Color INK   = { 0.13, 0.13, 0.15 };
static const Color GREY  = { 0.45, 0.45, 0.48 };
static const Color ACCENT = { 0.05, 0.33, 0.36 };
static const Color WHITE = { 1, 1, 1 };

typedef struct { char key[32]; Font f; } FontSlot;
static FontSlot fonts[MAXFONTS];
static int nfonts;

typedef struct { double x0, y0, x1, y1; char target[48]; } Annot;
typedef struct { Buf c; Annot *an; int nan, capan; int chrome; } Page;
typedef struct { int level, numbered; char num[32]; char text[512]; int page; double y; } Heading;
typedef struct { char key[64]; char *text; int num; int page; double y; } BibEntry;

static struct {
    Page *pages; int npages;
    double y;                       /* top of free space on the current page */
    int actualtext;                 /* 0 none, 1 per word, 2 per line */
    Heading heads[256]; int nheads;
    BibEntry bib[128]; int nbib, ncited;
    int toc_page;
    char title[512], subtitle[512], author[512], date[256];
    int h1, h2, nfig, ntab, last_block;   /* last_block: 1 figure, 2 table */
    double body_size;
} D = { .actualtext = 2, .toc_page = -1, .body_size = 13 };

enum { FIGURE = 1, TABLE = 2 };

static int font_find(const char *key) {
    for (int i = 0; i < nfonts; i++) if (!strcmp(fonts[i].key, key)) return i;
    return -1;
}
static int font_need(const char *key) {
    int i = font_find(key);
    if (i < 0) i = font_find("body");
    if (i < 0) die("no '@font body <file>' defined");
    return i;
}

static Page *page_at(int i) { return &D.pages[i]; }
static Page *cur(void) { return &D.pages[D.npages - 1]; }

static void new_page(int chrome) {
    D.pages = realloc(D.pages, (D.npages + 1) * sizeof(Page));
    memset(&D.pages[D.npages], 0, sizeof(Page));
    D.pages[D.npages].chrome = chrome;
    D.npages++;
    D.y = TOP;
}

/* make sure h points of vertical space are left, else start a new page */
static void ensure(double h) { if (D.y - h < BOTTOM) new_page(1); }

static void add_annot(Page *p, double x0, double y0, double x1, double y1, const char *target) {
    if (p->nan == p->capan) { p->capan = p->capan ? p->capan * 2 : 8; p->an = realloc(p->an, p->capan * sizeof(Annot)); }
    Annot *a = &p->an[p->nan++];
    a->x0 = x0; a->y0 = y0; a->x1 = x1; a->y1 = y1;
    snprintf(a->target, sizeof a->target, "%s", target);
}

/* number in Arabic-Indic digits, UTF-8 */
static void ar_num(int n, char *out) {
    char tmp[16];
    snprintf(tmp, sizeof tmp, "%d", n);
    char *o = out;
    for (char *t = tmp; *t; t++) { *o++ = (char)0xD9; *o++ = (char)(0xA0 + (*t - '0')); }
    *o = 0;
}

static Color parse_color(const char *s, Color dflt) {
    if (!s) return dflt;
    if (!strcmp(s, "accent")) return ACCENT;
    if (!strcmp(s, "ink")) return INK;
    if (!strcmp(s, "grey") || !strcmp(s, "gray")) return GREY;
    if (!strcmp(s, "white")) return WHITE;
    if (!strcmp(s, "none")) return (Color){ -1, -1, -1 };
    unsigned v;
    if (*s == '#' && sscanf(s + 1, "%6x", &v) == 1)
        return (Color){ ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0 };
    fprintf(stderr, "qalam: bad colour '%s'\n", s);
    return dflt;
}
static int no_color(Color c) { return c.r < 0; }

static void fill_circle(Buf *c, double cx, double cy, double r, Color col) {
    double m = r * 0.5523;
    buf_printf(c, "q %.3f %.3f %.3f rg %.2f %.2f m %.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f %.2f %.2f %.2f %.2f c "
                  "%.2f %.2f %.2f %.2f %.2f %.2f c %.2f %.2f %.2f %.2f %.2f %.2f c f Q\n",
               col.r, col.g, col.b,
               cx + r, cy, cx + r, cy + m, cx + m, cy + r, cx, cy + r, cx - m, cy + r, cx - r, cy + m, cx - r, cy,
               cx - r, cy - m, cx - m, cy - r, cx, cy - r, cx + m, cy - r, cx + r, cy - m, cx + r, cy);
}

/* ------------------------------------------------------------- paragraphs */

typedef struct {
    int fid; double size;
    uint32_t *t; int n;
    FriBidiLevel *levels;
    int *word_of;
    int rtl;
} Para;

static void para_init(Para *P, const char *utf8, int fid, double size) {
    int blen = strlen(utf8);
    P->fid = fid; P->size = size;
    P->t = malloc((blen + 1) * sizeof(uint32_t));
    P->n = fribidi_charset_to_unicode(FRIBIDI_CHAR_SET_UTF8, utf8, blen, P->t);
    P->levels = malloc((P->n + 1) * sizeof *P->levels);
    P->word_of = malloc((P->n + 1) * sizeof *P->word_of);
    P->rtl = 0;
    if (P->n == 0) return;
    FriBidiCharType *types = malloc(P->n * sizeof *types);
    FriBidiBracketType *br = malloc(P->n * sizeof *br);
    fribidi_get_bidi_types(P->t, P->n, types);
    fribidi_get_bracket_types(P->t, P->n, types, br);
    FriBidiParType base = FRIBIDI_PAR_ON;
    if (!fribidi_get_par_embedding_levels_ex(types, br, P->n, &base, P->levels)) die("bidi failed");
    P->rtl = FRIBIDI_IS_RTL(base);
    free(types); free(br);
    int wid = 0;
    for (int i = 0; i < P->n; i++) {
        if (is_space(P->t[i])) { P->word_of[i] = -(i + 1); wid++; }
        else P->word_of[i] = (i > 0 && !is_space(P->t[i - 1])) ? P->word_of[i - 1] : wid++;
    }
}
static void para_free(Para *P) { free(P->t); free(P->levels); free(P->word_of); }

static int text_is_rtl(const char *utf8) {
    Para P; para_init(&P, utf8, 0, 1);
    int r = P.rtl; para_free(&P);
    return r;
}

/* ------------------------------------------------------------------ lines */

typedef struct { int lstart, llen, g0, g1, run, space; double x, w; } Group;
typedef struct {
    Para *P; int s, e;
    int nr; Run runs[256]; hb_buffer_t *bufs[256];
    Group *gr; int ng;
    double width; int nspaces;
} Line;

static void line_layout(Line *L, Para *P, int s, int e) {
    Font *f = &fonts[P->fid].f;
    double k = P->size / f->upem;
    L->P = P; L->s = s; L->e = e; L->nr = 0; L->ng = 0; L->width = 0; L->nspaces = 0;
    for (int i = s; i < e;) {
        int j = i;
        while (j < e && P->levels[j] == P->levels[i]) j++;
        if (L->nr < 256) L->runs[L->nr++] = (Run){ i, j - i, P->levels[i] };
        i = j;
    }
    /* UAX #9 rule L2: reverse runs from the highest level down to the lowest odd one */
    int maxl = 0, minodd = 255;
    for (int i = 0; i < L->nr; i++) {
        if (L->runs[i].level > maxl) maxl = L->runs[i].level;
        if ((L->runs[i].level & 1) && L->runs[i].level < minodd) minodd = L->runs[i].level;
    }
    for (int lvl = maxl; lvl >= minodd && lvl > 0; lvl--)
        for (int i = 0; i < L->nr;) {
            if (L->runs[i].level >= lvl) {
                int j = i;
                while (j < L->nr && L->runs[j].level >= lvl) j++;
                for (int a = i, b = j - 1; a < b; a++, b--) { Run t = L->runs[a]; L->runs[a] = L->runs[b]; L->runs[b] = t; }
                i = j;
            } else i++;
        }
    int cap = 32;
    L->gr = malloc(cap * sizeof(Group));
    double x = 0;
    for (int i = 0; i < L->nr; i++) {
        Run *r = &L->runs[i];
        L->bufs[i] = shape(f, P->t, P->n, r->start, r->len, r->level & 1);
        assign_cids(f, P->t, r->start + r->len, L->bufs[i]);
        unsigned n;
        hb_glyph_info_t *inf = hb_buffer_get_glyph_infos(L->bufs[i], &n);
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(L->bufs[i], &n);
        unsigned g = 0;
        while (g < n) {
            int w = P->word_of[inf[g].cluster];
            unsigned m = g;
            double gx = x;
            while (m < n && P->word_of[inf[m].cluster] == w) { x += pos[m].x_advance * k; m++; }
            int ls = inf[g].cluster, le = inf[g].cluster;
            for (unsigned t = g; t < m; t++) {
                if ((int)inf[t].cluster < ls) ls = inf[t].cluster;
                if ((int)inf[t].cluster > le) le = inf[t].cluster;
            }
            while (ls > r->start && P->word_of[ls - 1] == w) ls--;
            while (le + 1 < r->start + r->len && P->word_of[le + 1] == w) le++;
            if (L->ng == cap) { cap *= 2; L->gr = realloc(L->gr, cap * sizeof(Group)); }
            int sp = w < 0;
            L->gr[L->ng++] = (Group){ ls, le - ls + 1, (int)g, (int)m, i, sp, gx, x - gx };
            if (sp) L->nspaces++;
            g = m;
        }
    }
    L->width = x;
}

static void line_free(Line *L) {
    for (int i = 0; i < L->nr; i++) hb_buffer_destroy(L->bufs[i]);
    free(L->gr);
}

/* ---- glyph outlines as vector paths (for pieces that carry no text) ---- */
typedef struct { Buf *c; double x, y, k; } DrawCtx;
static void dr_move(hb_draw_funcs_t *d, void *p, hb_draw_state_t *st, float x, float y, void *u) {
    (void)d; (void)st; (void)u; DrawCtx *c = p; buf_printf(c->c, "%.2f %.2f m ", c->x + x * c->k, c->y + y * c->k); }
static void dr_line(hb_draw_funcs_t *d, void *p, hb_draw_state_t *st, float x, float y, void *u) {
    (void)d; (void)st; (void)u; DrawCtx *c = p; buf_printf(c->c, "%.2f %.2f l ", c->x + x * c->k, c->y + y * c->k); }
static void dr_quad(hb_draw_funcs_t *d, void *p, hb_draw_state_t *st, float cx, float cy, float x, float y, void *u) {
    (void)d; (void)u; DrawCtx *c = p;
    double x0 = st->current_x, y0 = st->current_y;
    double c1x = x0 + 2.0 / 3 * (cx - x0), c1y = y0 + 2.0 / 3 * (cy - y0);
    double c2x = x + 2.0 / 3 * (cx - x), c2y = y + 2.0 / 3 * (cy - y);
    buf_printf(c->c, "%.2f %.2f %.2f %.2f %.2f %.2f c ", c->x + c1x * c->k, c->y + c1y * c->k,
               c->x + c2x * c->k, c->y + c2y * c->k, c->x + x * c->k, c->y + y * c->k); }
static void dr_cubic(hb_draw_funcs_t *d, void *p, hb_draw_state_t *st, float ax, float ay, float bx, float by, float x, float y, void *u) {
    (void)d; (void)st; (void)u; DrawCtx *c = p;
    buf_printf(c->c, "%.2f %.2f %.2f %.2f %.2f %.2f c ", c->x + ax * c->k, c->y + ay * c->k,
               c->x + bx * c->k, c->y + by * c->k, c->x + x * c->k, c->y + y * c->k); }
static void dr_close(hb_draw_funcs_t *d, void *p, hb_draw_state_t *st, void *u) {
    (void)d; (void)st; (void)u; DrawCtx *c = p; buf_printf(c->c, "h "); }

static void glyph_as_path(Buf *out, Font *f, unsigned gid, double x, double y, double size) {
    static hb_draw_funcs_t *df;
    if (!df) {
        df = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(df, dr_move, NULL, NULL);
        hb_draw_funcs_set_line_to_func(df, dr_line, NULL, NULL);
        hb_draw_funcs_set_quadratic_to_func(df, dr_quad, NULL, NULL);
        hb_draw_funcs_set_cubic_to_func(df, dr_cubic, NULL, NULL);
        hb_draw_funcs_set_close_path_func(df, dr_close, NULL, NULL);
    }
    DrawCtx c = { out, x, y, size / f->upem };
    hb_font_draw_glyph(f->font, gid, df, &c);
    buf_printf(out, "f\n");
}

/* Draw a laid-out line with its visual left edge at x, baseline y.
 * stretch is added after every space (justification). */
static void line_emit(Line *L, Page *pg, double x, double y, double stretch, Color col, int links) {
    Para *P = L->P;
    Font *f = &fonts[P->fid].f;
    double k = P->size / f->upem;
    Buf *c = &pg->c;
    buf_printf(c, "%.3f %.3f %.3f rg\n", col.r, col.g, col.b);
    if (D.actualtext == 2) {
        buf_printf(c, "/Span <</ActualText ");
        put_utf16_hex(c, P->t + L->s, L->e - L->s, 1);
        buf_printf(c, ">> BDC\n");
    }
    /* pass 1: where every glyph is drawn, in visual order */
    typedef struct { unsigned cid; double x, y; int group, mark; } Item;
    int cap = 64, ni = 0;
    Item *it = malloc(cap * sizeof *it);
    Buf paths = {0};
    double shift = 0, line_end = x;
    for (int g = 0; g < L->ng; g++) {
        Group *G = &L->gr[g];
        hb_glyph_info_t *inf = hb_buffer_get_glyph_infos(L->bufs[G->run], NULL);
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(L->bufs[G->run], NULL);
        double target = x + G->x + shift;
        G->x = target;   /* absolute position, used for link boxes */
        for (int i = G->g0; i < G->g1; i++) {
            unsigned cid = inf[i].codepoint, gid = f->cid_gid[cid];
            double gx = target + pos[i].x_offset * k, gy = pos[i].y_offset * k;
            target += pos[i].x_advance * k;
            if (cid_is_empty(f, cid)) {
                /* a piece with no text of its own (e.g. an inserted kashida):
                 * draw it as a shape so no extractor sees a stray character */
                glyph_as_path(&paths, f, gid, gx, y + gy, P->size);
                continue;
            }
            if (ni == cap) { cap *= 2; it = realloc(it, cap * sizeof *it); }
            int mark = hb_ot_layout_get_glyph_class(f->face, gid) == HB_OT_LAYOUT_GLYPH_CLASS_MARK;
            it[ni++] = (Item){ cid, gx, gy, g, mark };
        }
        if (G->space) target += stretch, shift += stretch;
        line_end = target;
    }
    /* pass 2: one TJ for the whole line. Each glyph gets a CID whose width
     * reaches exactly the next glyph, so there are no positioning jumps. */
    buf_printf(c, "BT /F%d %.2f Tf 1 0 0 1 %.3f %.3f Tm [", P->fid, P->size, x, y);
    double pen = x, rise = 0;
    int cur_group = -1;
    for (int i = 0; i < ni; i++) {
        if (D.actualtext == 1 && it[i].group != cur_group) {
            Group *G = &L->gr[it[i].group];
            buf_printf(c, "] TJ%s/Span <</ActualText ", cur_group >= 0 ? " EMC " : " ");
            put_utf16_hex(c, P->t + G->lstart, G->llen, 1);
            buf_printf(c, ">> BDC [");
            cur_group = it[i].group;
        }
        /* a new bidi run starts a new text object at its own position; extractors
         * with their own bidi handling (pypdf) cope better with run-sized chunks */
        if (i > 0 && L->gr[it[i].group].run != L->gr[it[i - 1].group].run && !it[i].mark) {
            buf_printf(c, "] TJ%s ET BT /F%d %.2f Tf 1 0 0 1 %.3f %.3f Tm [", rise != 0 ? " 0 Ts" : "",
                       P->fid, P->size, it[i].x, y);
            pen = it[i].x; rise = 0;
        }
        if (it[i].y != rise) { buf_printf(c, "] TJ %.3f Ts [", it[i].y); rise = it[i].y; }
        double delta = it[i].x - pen;
        if (delta > 0.005 || delta < -0.005) buf_printf(c, "%.1f", -delta * 1000.0 / P->size);
        int w = 0;
        if (!it[i].mark) {
            double next = i + 1 < ni ? it[i + 1].x : line_end;
            /* the next glyph may be a mark drawn back over this one: aim past it */
            for (int j = i + 1; j < ni && it[j].mark; j++) next = j + 1 < ni ? it[j + 1].x : line_end;
            double adv = next - it[i].x;
            if (adv < 0) adv = 0;
            w = (int)lround(adv * 1000.0 / P->size);
        }
        unsigned cid = font_cid_width(f, it[i].cid, w);
        buf_printf(c, "<%04X>", cid);
        pen = it[i].x + w * P->size / 1000.0;
    }
    buf_printf(c, "] TJ%s%s ET\n", rise != 0 ? " 0 Ts" : "", D.actualtext == 1 && cur_group >= 0 ? " EMC" : "");
    if (paths.n) buf_put(c, paths.p, paths.n);
    free(it);
    if (D.actualtext == 2) buf_printf(c, "EMC\n");
    free(paths.p);
    /* citation links: "[n]" is split by bidi into several runs ("[", "n", "]"),
     * so find it in the logical text and box every group that overlaps it */
    if (links)
        for (int i = L->s; i < L->e; i++) {
            if (P->t[i] != '[') continue;
            int j = i + 1, n = 0;
            while (j < L->e && P->t[j] >= '0' && P->t[j] <= '9') n = n * 10 + (P->t[j++] - '0');
            if (j == i + 1 || j >= L->e || P->t[j] != ']') continue;
            double x0 = 1e9, x1 = -1e9;
            for (int g = 0; g < L->ng; g++) {
                Group *G = &L->gr[g];
                if (G->space || G->lstart > j || G->lstart + G->llen <= i) continue;
                if (G->x < x0) x0 = G->x;
                if (G->x + G->w > x1) x1 = G->x + G->w;
            }
            if (x1 > x0) {
                char tg[48]; snprintf(tg, sizeof tg, "bib:%d", n);
                add_annot(pg, x0, y - P->size * 0.3, x1, y + P->size * 0.85, tg);
            }
            i = j;
        }
}

/* One line of text. anchor: 'l' x is the left edge, 'c' centre, 'r' right edge.
 * Returns the width. */
static double text_line(Page *pg, const char *utf8, int fid, double size, Color col,
                        double x, double y, char anchor) {
    Para P; para_init(&P, utf8, fid, size);
    int s = 0, e = P.n;
    while (s < e && is_space(P.t[s])) s++;
    while (e > s && is_space(P.t[e - 1])) e--;
    double w = 0;
    if (s < e) {
        Line L; line_layout(&L, &P, s, e);
        w = L.width;
        double x0 = anchor == 'c' ? x - w / 2 : anchor == 'r' ? x - w : x;
        if (pg) line_emit(&L, pg, x0, y, 0, col, 0);
        line_free(&L);
    }
    para_free(&P);
    return w;
}
static double text_width(const char *utf8, int fid, double size) {
    return text_line(NULL, utf8, fid, size, INK, 0, 0, 'l');
}

typedef struct {
    int fid; double size; Color color;
    double lead;            /* baseline to baseline */
    int justify, center;
    double ind_r, ind_l;    /* indents from the right / left margin */
    int links;              /* turn [n] into links to the bibliography */
} PStyle;

/* Typeset a paragraph at D.y. Reports where its first line went. */
static void paragraph(const char *utf8, const PStyle *st, int *first_page, double *first_base) {
    Para P; para_init(&P, utf8, st->fid, st->size);
    Font *f = &fonts[st->fid].f;
    double maxw = TEXT_W - st->ind_r - st->ind_l;
    hb_codepoint_t spg;
    double space_w = hb_font_get_nominal_glyph(f->font, ' ', &spg)
                   ? hb_font_get_glyph_h_advance(f->font, spg) * st->size / f->upem : st->size * 0.25;
    int s = 0, first = 1;
    while (s < P.n) {
        while (s < P.n && is_space(P.t[s])) s++;
        if (s >= P.n) break;
        int e = s;
        double w = 0;
        while (e < P.n) {
            int ws = e;
            while (ws < P.n && is_space(P.t[ws])) ws++;
            int we = ws;
            while (we < P.n && !is_space(P.t[we])) we++;
            if (ws == we) break;
            double ww = shaped_width(f, P.t, P.n, ws, we - ws, P.levels[ws] & 1, st->size);
            double add = (e == s) ? ww : space_w + ww;
            if (e != s && w + add > maxw) break;
            w += add;
            e = we;
        }
        int last = 1;
        for (int t = e; t < P.n; t++) if (!is_space(P.t[t])) { last = 0; break; }
        ensure(st->lead);
        double base = D.y - st->size * 1.1;
        Line L; line_layout(&L, &P, s, e);
        double stretch = 0, x;
        if (st->justify && !last && L.nspaces > 0 && (maxw - L.width) / L.nspaces < st->size * 1.2)
            stretch = (maxw - L.width) / L.nspaces;
        double used = L.width + stretch * L.nspaces;
        if (st->center) x = MARGIN + st->ind_l + (maxw - used) / 2;
        else if (P.rtl) x = PAGE_W - MARGIN - st->ind_r - used;
        else x = MARGIN + st->ind_l;
        line_emit(&L, cur(), x, base, stretch, st->color, st->links);
        line_free(&L);
        if (first) {
            if (first_page) *first_page = D.npages - 1;
            if (first_base) *first_base = base;
            first = 0;
        }
        D.y -= st->lead;
        s = e;
    }
    para_free(&P);
}

static PStyle body_style(void) {
    return (PStyle){ font_need("body"), D.body_size, INK, D.body_size * 1.85, 1, 0, 0, 0, 1 };
}

/* ------------------------------------------------------------- citations */

static BibEntry *bib_find(const char *key) {
    for (int i = 0; i < D.nbib; i++) if (!strcmp(D.bib[i].key, key)) return &D.bib[i];
    return NULL;
}

/* where a node is in the input, for messages: "doc.qlm:12:5" */
static const char *src_name, *src_text;
static size_t src_len;
static const char *where(const gram_node *n) {
    static char buf[1100];
    size_t line, col;
    gram_locate(src_text, src_len, (size_t)(n->text - src_text), &line, &col);
    snprintf(buf, sizeof buf, "%s:%lu:%lu", src_name, (unsigned long)line, (unsigned long)col);
    return buf;
}

/* the text of a node as a new C string (quotes stripped from a string token) */
static char *node_str(const gram_node *n) {
    const char *t = n->text;
    size_t len = n->len;
    if (n->rule == QALAM_R_string && len >= 2) { t++; len -= 2; }
    char *r = malloc(len + 1);
    memcpy(r, t, len);
    r[len] = 0;
    return r;
}

/* Running text (an `inline` node) as written, with every [@key] replaced by
 * [n]; entries are numbered in the order they are first cited. */
static void inline_text(Buf *b, const gram_node *inl) {
    const char *p = inl->text, *end = inl->text + inl->len;
    for (const gram_node *k = inl->child; k; k = k->next) {
        if (k->rule != QALAM_R_cite) continue;
        buf_put(b, p, k->text - p);
        const gram_node *key = k->child;
        char *name = node_str(key);
        BibEntry *be = bib_find(name);
        if (!be) { fprintf(stderr, "%s: warning: no @bib entry called '%s'\n", where(key), name); buf_printf(b, "[?]"); }
        else {
            if (!be->num) be->num = ++D.ncited;
            buf_printf(b, "[%d]", be->num);
        }
        free(name);
        p = k->text + k->len;
    }
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    if (end > p) buf_put(b, p, end - p);
}

static char *inline_str(const gram_node *inl) {
    Buf b = {0};
    buf_put(&b, "", 0);
    inline_text(&b, inl);
    return b.p;
}

/* ---------------------------------------------------------------- blocks */

static void body_paragraph(const char *text) {
    PStyle st = body_style();
    paragraph(text, &st, NULL, NULL);
    D.y -= D.body_size * 0.55;
}

static void heading(int level, const char *text, int numbered) {
    Heading *h = &D.heads[D.nheads < 255 ? D.nheads++ : 255];
    h->level = level; h->numbered = numbered;
    snprintf(h->text, sizeof h->text, "%s", text);
    h->num[0] = 0;
    if (numbered) {
        char a[16], b[16];
        if (level == 1) { D.h1++; D.h2 = 0; ar_num(D.h1, a); snprintf(h->num, sizeof h->num, "%s", a); }
        else { D.h2++; ar_num(D.h1, a); ar_num(D.h2, b); snprintf(h->num, sizeof h->num, "%s\xD9\xAB%s", a, b); }
    }
    double size = level == 1 ? 19 : 15;
    double before = level == 1 ? 22 : 12;
    if (D.y < TOP - 1) D.y -= before;
    ensure(size * 2 + D.body_size * 4);         /* keep the heading with some text */
    char full[600];
    if (numbered) snprintf(full, sizeof full, "%s  %s", h->num, text);
    else snprintf(full, sizeof full, "%s", text);
    PStyle st = { font_need("heading"), size, ACCENT, size * 1.9, 0, 0, 0, 0, 0 };
    if (level == 2) st.color = (Color){ 0.12, 0.38, 0.42 };
    h->page = D.npages - 1;
    h->y = D.y;
    paragraph(full, &st, NULL, NULL);
    if (level == 1) {
        double y = D.y + size * 0.2;
        buf_printf(&cur()->c, "q %.3f %.3f %.3f RG 0.8 w %.2f %.2f m %.2f %.2f l S Q\n",
                   0.75, 0.85, 0.85, MARGIN, y, PAGE_W - MARGIN, y);
        buf_printf(&cur()->c, "q %.3f %.3f %.3f RG 2.4 w %.2f %.2f m %.2f %.2f l S Q\n",
                   ACCENT.r, ACCENT.g, ACCENT.b, PAGE_W - MARGIN - 70, y, PAGE_W - MARGIN, y);
        D.y -= 6;
    }
}

static void quote_block(const char *text) {
    PStyle st = { font_need("quote"), D.body_size + 3, (Color){ 0.22, 0.24, 0.28 },
                  (D.body_size + 2) * 1.9, 0, 1, 34, 34, 0 };
    D.y -= 4;
    int lines = (int)(text_width(text, st.fid, st.size) / (TEXT_W - 68)) + 2;
    ensure(st.lead * lines);   /* a quote never breaks across pages */
    int page = D.npages - 1;
    double top = D.y;
    /* soft background first so the text sits on top */
    Buf saved = cur()->c;
    cur()->c = (Buf){0};
    paragraph(text, &st, NULL, NULL);
    Buf textbuf = cur()->c;
    cur()->c = saved;
    if (page == D.npages - 1) {
        double bot = D.y + st.lead * 0.15;
        buf_printf(&cur()->c, "q 0.95 0.97 0.97 rg %.2f %.2f %.2f %.2f re f Q\n",
                   MARGIN + 18, bot, TEXT_W - 36, top - bot);
        buf_printf(&cur()->c, "q %.3f %.3f %.3f rg %.2f %.2f 3 %.2f re f Q\n",
                   ACCENT.r, ACCENT.g, ACCENT.b, PAGE_W - MARGIN - 21, bot, top - bot);
    }
    if (textbuf.n) buf_put(&cur()->c, textbuf.p, textbuf.n);
    free(textbuf.p);
    D.y -= 10;
}

static void list_item(const char *t) {
    PStyle st = body_style();
    st.ind_r = 22;
    int rtl = text_is_rtl(t);
    if (!rtl) { st.ind_r = 0; st.ind_l = 22; }
    int page; double base;
    paragraph(t, &st, &page, &base);
    double x = rtl ? PAGE_W - MARGIN - 8 : MARGIN + 8;
    fill_circle(&page_at(page)->c, x, base + D.body_size * 0.32, 2.6, ACCENT);
    D.y -= 2;
}

static void caption(const char *text) {
    char num[16], full[700];
    if (D.last_block == TABLE) { ar_num(++D.ntab, num); snprintf(full, sizeof full, "جدول %s: %s", num, text); }
    else { ar_num(++D.nfig, num); snprintf(full, sizeof full, "شكل %s: %s", num, text); }
    PStyle st = { font_need("sans"), 10.5, GREY, 10.5 * 1.9, 0, 1, 0, 0, 0 };
    paragraph(full, &st, NULL, NULL);
    D.y -= 10;
    D.last_block = 0;
}

/* --------------------------------------------------------------- drawing */

/* A TikZ-ish figure language. Coordinates are points from the figure's
 * top-left corner, y pointing down (like SVG).
 *   rect x y w h [radius=] [fill=] [stroke=] [width=] [dash=]
 *   circle cx cy r ...      ellipse cx cy rx ry ...
 *   line x1 y1 x2 y2 [arrow=end|start|both] ...
 *   path "M x y L x y C ... Q ... H x V y Z" ...   (lower case = relative)
 *   text x y "text" [font=] [size=] [color=] [anchor=left|center|right]
 */
static struct { double x0, top, w, h; } FIG;

static double FX(double x) { return FIG.x0 + x; }
static double FY(double y) { return FIG.top - y; }

static const char *opt(char tok[][512], int n, const char *key) {
    size_t kl = strlen(key);
    for (int i = 0; i < n; i++) if (!strncmp(tok[i], key, kl) && tok[i][kl] == '=') return tok[i] + kl + 1;
    return NULL;
}

/* positional numbers (tokens that are not key=value) */
static int nums(char tok[][512], int n, double *v, int max) {
    int k = 0;
    for (int i = 1; i < n && k < max; i++) {
        if (strchr(tok[i], '=')) continue;
        char *end;
        double d = strtod(tok[i], &end);
        if (end != tok[i] && *end == 0) v[k++] = d;
    }
    return k;
}

/* set colours/width/dash from options; returns paint operator */
static const char *paint_setup(Buf *c, char tok[][512], int n, int default_fill) {
    Color fill = parse_color(opt(tok, n, "fill"), (Color){ -1, -1, -1 });
    Color stroke = parse_color(opt(tok, n, "stroke"), (Color){ -1, -1, -1 });
    if (!opt(tok, n, "fill") && !opt(tok, n, "stroke")) {
        if (default_fill) fill = INK; else stroke = INK;
    }
    const char *w = opt(tok, n, "width");
    buf_printf(c, "%s w 1 J 1 j ", w ? w : "1");
    const char *dash = opt(tok, n, "dash");
    if (dash) buf_printf(c, "[%s %s] 0 d ", dash, dash);
    if (!no_color(fill)) buf_printf(c, "%.3f %.3f %.3f rg ", fill.r, fill.g, fill.b);
    if (!no_color(stroke)) buf_printf(c, "%.3f %.3f %.3f RG ", stroke.r, stroke.g, stroke.b);
    const char *op = (!no_color(fill) && !no_color(stroke)) ? "B" : !no_color(fill) ? "f" : "S";
    if (!no_color(stroke) && no_color(fill)) op = "S";
    return op;
}

static void ellipse_path(Buf *c, double cx, double cy, double rx, double ry) {
    double mx = rx * 0.5523, my = ry * 0.5523;
    buf_printf(c, "%.2f %.2f m ", cx + rx, cy);
    buf_printf(c, "%.2f %.2f %.2f %.2f %.2f %.2f c ", cx + rx, cy + my, cx + mx, cy + ry, cx, cy + ry);
    buf_printf(c, "%.2f %.2f %.2f %.2f %.2f %.2f c ", cx - mx, cy + ry, cx - rx, cy + my, cx - rx, cy);
    buf_printf(c, "%.2f %.2f %.2f %.2f %.2f %.2f c ", cx - rx, cy - my, cx - mx, cy - ry, cx, cy - ry);
    buf_printf(c, "%.2f %.2f %.2f %.2f %.2f %.2f c h ", cx + mx, cy - ry, cx + rx, cy - my, cx + rx, cy);
}

static void svg_path(Buf *c, const char *d) {
    double cx = 0, cy = 0, sx = 0, sy = 0;
    char cmd = 0;
    const char *p = d;
    double v[6];
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (!*p) break;
        if (isalpha((unsigned char)*p)) { cmd = *p++; if (cmd == 'Z' || cmd == 'z') { buf_printf(c, "h "); cx = sx; cy = sy; } continue; }
        int need = 0;
        switch (toupper(cmd)) { case 'M': case 'L': need = 2; break; case 'H': case 'V': need = 1; break;
                                case 'C': need = 6; break; case 'Q': need = 4; break; default: return; }
        for (int i = 0; i < need; i++) {
            while (*p == ' ' || *p == ',' || *p == '\t') p++;
            char *end; v[i] = strtod(p, &end);
            if (end == p) return;
            p = end;
        }
        int rel = islower((unsigned char)cmd);
        switch (toupper(cmd)) {
        case 'M':
            if (rel) { v[0] += cx; v[1] += cy; }
            cx = sx = v[0]; cy = sy = v[1];
            buf_printf(c, "%.2f %.2f m ", FX(cx), FY(cy));
            cmd = rel ? 'l' : 'L';
            break;
        case 'L':
            if (rel) { v[0] += cx; v[1] += cy; }
            cx = v[0]; cy = v[1];
            buf_printf(c, "%.2f %.2f l ", FX(cx), FY(cy));
            break;
        case 'H': cx = rel ? cx + v[0] : v[0]; buf_printf(c, "%.2f %.2f l ", FX(cx), FY(cy)); break;
        case 'V': cy = rel ? cy + v[0] : v[0]; buf_printf(c, "%.2f %.2f l ", FX(cx), FY(cy)); break;
        case 'C':
            if (rel) for (int i = 0; i < 6; i += 2) { v[i] += cx; v[i + 1] += cy; }
            buf_printf(c, "%.2f %.2f %.2f %.2f %.2f %.2f c ", FX(v[0]), FY(v[1]), FX(v[2]), FY(v[3]), FX(v[4]), FY(v[5]));
            cx = v[4]; cy = v[5];
            break;
        case 'Q': {
            if (rel) for (int i = 0; i < 4; i += 2) { v[i] += cx; v[i + 1] += cy; }
            double c1x = cx + 2.0 / 3 * (v[0] - cx), c1y = cy + 2.0 / 3 * (v[1] - cy);
            double c2x = v[2] + 2.0 / 3 * (v[0] - v[2]), c2y = v[3] + 2.0 / 3 * (v[1] - v[3]);
            buf_printf(c, "%.2f %.2f %.2f %.2f %.2f %.2f c ", FX(c1x), FY(c1y), FX(c2x), FY(c2y), FX(v[2]), FY(v[3]));
            cx = v[2]; cy = v[3];
            break; }
        }
    }
}

static void arrow_head(Buf *c, double x1, double y1, double x2, double y2, double w, Color col) {
    double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy);
    if (len < 0.01) return;
    dx /= len; dy /= len;
    double L = 7 + w * 2, H = 3.5 + w;
    double bx = x2 - dx * L, by = y2 - dy * L;
    buf_printf(c, "q %.3f %.3f %.3f rg %.2f %.2f m %.2f %.2f l %.2f %.2f l h f Q\n", col.r, col.g, col.b,
               x2, y2, bx - dy * H, by + dx * H, bx + dy * H, by - dx * H);
}

/* One `draw` node: the shape, then its numbers, strings and key=value options,
 * flattened into strings in the order they were written. */
static void draw_command(const gram_node *d) {
    char tok[32][512];
    int n = 0;
    for (const gram_node *k = d->child; k && n < 32; k = k->next) {
        if (k->rule == QALAM_R_option) {
            char *key = node_str(k->child), *val = node_str(k->child->next);
            snprintf(tok[n++], 512, "%s=%s", key, val);
            free(key); free(val);
        } else {
            char *t = node_str(k);
            snprintf(tok[n++], 512, "%s", t);
            free(t);
        }
    }
    if (n == 0) return;
    Buf *c = &cur()->c;
    double v[8];
    int k = nums(tok, n, v, 8);
    const char *cmd = tok[0];
    if (!strcmp(cmd, "rect") && k >= 4) {
        buf_printf(c, "q ");
        const char *op = paint_setup(c, tok, n, 0);
        const char *rs = opt(tok, n, "radius");
        double r = rs ? atof(rs) : 0;
        double x = FX(v[0]), y = FY(v[1] + v[3]), w = v[2], h = v[3];
        if (r <= 0) buf_printf(c, "%.2f %.2f %.2f %.2f re ", x, y, w, h);
        else {
            double m = r * (1 - 0.5523);
            buf_printf(c, "%.2f %.2f m %.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c ", x + r, y, x + w - r, y, x + w - m, y, x + w, y + m, x + w, y + r);
            buf_printf(c, "%.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c ", x + w, y + h - r, x + w, y + h - m, x + w - m, y + h, x + w - r, y + h);
            buf_printf(c, "%.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c ", x + r, y + h, x + m, y + h, x, y + h - m, x, y + h - r);
            buf_printf(c, "%.2f %.2f l %.2f %.2f %.2f %.2f %.2f %.2f c h ", x, y + r, x, y + m, x + m, y, x + r, y);
        }
        buf_printf(c, "%s Q\n", op);
    } else if (!strcmp(cmd, "circle") && k >= 3) {
        buf_printf(c, "q ");
        const char *op = paint_setup(c, tok, n, 0);
        ellipse_path(c, FX(v[0]), FY(v[1]), v[2], v[2]);
        buf_printf(c, "%s Q\n", op);
    } else if (!strcmp(cmd, "ellipse") && k >= 4) {
        buf_printf(c, "q ");
        const char *op = paint_setup(c, tok, n, 0);
        ellipse_path(c, FX(v[0]), FY(v[1]), v[2], v[3]);
        buf_printf(c, "%s Q\n", op);
    } else if (!strcmp(cmd, "line") && k >= 4) {
        Color col = parse_color(opt(tok, n, "stroke"), INK);
        const char *ws = opt(tok, n, "width");
        double w = ws ? atof(ws) : 1;
        const char *ar = opt(tok, n, "arrow");
        double x1 = FX(v[0]), y1 = FY(v[1]), x2 = FX(v[2]), y2 = FY(v[3]);
        /* shorten the line under arrow heads so the tip stays sharp */
        double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy), L = 7 + w * 2;
        double ax1 = x1, ay1 = y1, ax2 = x2, ay2 = y2;
        if (len > 0 && ar && (!strcmp(ar, "end") || !strcmp(ar, "both"))) { ax2 -= dx / len * L * 0.8; ay2 -= dy / len * L * 0.8; }
        if (len > 0 && ar && (!strcmp(ar, "start") || !strcmp(ar, "both"))) { ax1 += dx / len * L * 0.8; ay1 += dy / len * L * 0.8; }
        buf_printf(c, "q ");
        paint_setup(c, tok, n, 0);
        buf_printf(c, "%.3f %.3f %.3f RG %.2f %.2f m %.2f %.2f l S Q\n", col.r, col.g, col.b, ax1, ay1, ax2, ay2);
        if (ar && (!strcmp(ar, "end") || !strcmp(ar, "both"))) arrow_head(c, x1, y1, x2, y2, w, col);
        if (ar && (!strcmp(ar, "start") || !strcmp(ar, "both"))) arrow_head(c, x2, y2, x1, y1, w, col);
    } else if (!strcmp(cmd, "path") && n >= 2) {
        buf_printf(c, "q ");
        const char *op = paint_setup(c, tok, n, 0);
        svg_path(c, tok[1]);
        buf_printf(c, "%s Q\n", op);
    } else if (!strcmp(cmd, "text") && n >= 4) {
        double x = atof(tok[1]), y = atof(tok[2]);
        const char *fk = opt(tok, n, "font");
        const char *sz = opt(tok, n, "size");
        const char *an = opt(tok, n, "anchor");
        char a = !an ? 'c' : !strcmp(an, "left") ? 'l' : !strcmp(an, "right") ? 'r' : 'c';
        text_line(cur(), tok[3], font_need(fk ? fk : "sans"), sz ? atof(sz) : 10,
                  parse_color(opt(tok, n, "color"), INK), FX(x), FY(y), a);
    } else {
        fprintf(stderr, "%s: warning: '%s' needs more numbers\n", where(d), tok[0]);
    }
}

static void figure_begin(double w, double h) {
    D.y -= 6;
    ensure(h + 40);
    FIG.w = w; FIG.h = h;
    FIG.x0 = MARGIN + (TEXT_W - w) / 2;
    FIG.top = D.y;
}
static void figure_end(void) {
    D.y = FIG.top - FIG.h - 10;
    D.last_block = FIGURE;
}

/* ----------------------------------------------------------------- tables */

typedef struct { double frac[16]; int ncol; const gram_node *rows[256]; int nrows; } Table;

static void table_render(Table *T) {
    double size = 11, rh = size * 2.3, total = 0;
    for (int i = 0; i < T->ncol; i++) total += T->frac[i];
    double cw[16];
    for (int i = 0; i < T->ncol; i++) cw[i] = TEXT_W * T->frac[i] / total;
    int fbody = font_need("sans"), fhead = font_find("sansbold") >= 0 ? font_need("sansbold") : fbody;
    D.y -= 6;
    ensure(rh * (T->nrows < 4 ? T->nrows : 4) + 30);
    int body_i = 0;
    double table_top = D.y; int table_page = D.npages - 1;
    for (int r = 0; r < T->nrows; r++) {
        const gram_node *cellnode = T->rows[r]->child;
        int head = cellnode && cellnode->rule == QALAM_R_header;
        if (head) cellnode = cellnode->next;
        if (D.y - rh < BOTTOM) { new_page(1); table_top = D.y; table_page = D.npages - 1; }
        Buf *c = &cur()->c;
        double y0 = D.y - rh;
        if (head) buf_printf(c, "q %.3f %.3f %.3f rg %.2f %.2f %.2f %.2f re f Q\n", ACCENT.r, ACCENT.g, ACCENT.b, MARGIN, y0, TEXT_W, rh);
        else if (body_i++ % 2 == 0) buf_printf(c, "q 0.94 0.965 0.965 rg %.2f %.2f %.2f %.2f re f Q\n", MARGIN, y0, TEXT_W, rh);
        /* cells, first column on the right */
        double xr = PAGE_W - MARGIN;
        for (int col = 0; col < T->ncol && cellnode; col++, cellnode = cellnode->next) {
            char *t = node_str(cellnode);
            int numeric = *t != 0;
            for (char *q = t; *q; q++) if (!isdigit((unsigned char)*q) && *q != '.' && *q != '-' && *q != ' ') numeric = 0;
            double base = y0 + rh * 0.36;
            int fid = head ? fhead : fbody;
            Color col_c = head ? WHITE : INK;
            if (numeric || head) text_line(cur(), t, fid, size, col_c, xr - cw[col] / 2, base, 'c');
            else if (text_is_rtl(t)) text_line(cur(), t, fid, size, col_c, xr - 9, base, 'r');
            else text_line(cur(), t, fid, size, col_c, xr - cw[col] + 9, base, 'l');
            xr -= cw[col];
            free(t);
        }
        if (!head) buf_printf(c, "q 0.82 0.87 0.87 RG 0.5 w %.2f %.2f m %.2f %.2f l S Q\n", MARGIN, y0, PAGE_W - MARGIN, y0);
        D.y = y0;
        /* close the frame when the table ends or the page is about to break */
        if (r == T->nrows - 1 || D.y - rh < BOTTOM) {
            if (table_page == D.npages - 1)
                buf_printf(c, "q %.3f %.3f %.3f RG 1 w %.2f %.2f %.2f %.2f re S Q\n", ACCENT.r, ACCENT.g, ACCENT.b,
                           MARGIN, D.y, TEXT_W, table_top - D.y);
        }
    }
    D.y -= 8;
    D.last_block = TABLE;
    T->nrows = 0;
}

/* -------------------------------------------------- title, toc, chrome */

static void make_title(void) {
    Page *p = cur();
    p->chrome = 0;
    Buf *c = &p->c;
    /* top band and corner ornament */
    buf_printf(c, "q %.3f %.3f %.3f rg 0 %.2f %.2f 26 re f Q\n", ACCENT.r, ACCENT.g, ACCENT.b, PAGE_H - 26, PAGE_W);
    buf_printf(c, "q 0.80 0.62 0.30 rg 0 %.2f %.2f 4 re f Q\n", PAGE_H - 30, PAGE_W);
    double y = PAGE_H - 150;
    text_line(p, D.title, font_need("title"), 40, ACCENT, PAGE_W / 2, y, 'c');
    y -= 46;
    if (D.subtitle[0]) { text_line(p, D.subtitle, font_need("sans"), 15, GREY, PAGE_W / 2, y, 'c'); y -= 28; }
    buf_printf(c, "q 0.80 0.62 0.30 RG 1.2 w %.2f %.2f m %.2f %.2f l S Q\n", PAGE_W / 2 - 60, y, PAGE_W / 2 + 60, y);
    double by = 118;
    if (D.author[0]) { text_line(p, D.author, font_need("heading"), 14, INK, PAGE_W / 2, by, 'c'); by -= 26; }
    if (D.date[0]) text_line(p, D.date, font_need("sans"), 11, GREY, PAGE_W / 2, by, 'c');
    buf_printf(c, "q %.3f %.3f %.3f rg 0 0 %.2f 10 re f Q\n", ACCENT.r, ACCENT.g, ACCENT.b, PAGE_W);
    D.y = y - 20;
}

static void reserve_toc(void) {
    new_page(1);
    D.toc_page = D.npages - 1;
    new_page(1);
}

static void fill_toc(void) {
    if (D.toc_page < 0) return;
    Page *p = page_at(D.toc_page);
    double y = TOP - 30;
    text_line(p, "المحتويات", font_need("heading"), 22, ACCENT, PAGE_W - MARGIN, y, 'r');
    y -= 16;
    buf_printf(&p->c, "q %.3f %.3f %.3f RG 2.4 w %.2f %.2f m %.2f %.2f l S Q\n",
               ACCENT.r, ACCENT.g, ACCENT.b, PAGE_W - MARGIN - 70, y, PAGE_W - MARGIN, y);
    y -= 30;
    for (int i = 0; i < D.nheads; i++) {
        Heading *h = &D.heads[i];
        if (h->page == D.toc_page) continue;
        int l1 = h->level == 1;
        if (l1 && i > 0) y -= 6;
        double size = l1 ? 13.5 : 12.5;
        int fid = font_need(l1 ? "heading" : "body");
        double ind = l1 ? 0 : 24;
        char label[600], pn[16];
        if (h->numbered) snprintf(label, sizeof label, "%s  %s", h->num, h->text);
        else snprintf(label, sizeof label, "%s", h->text);
        ar_num(h->page + 1, pn);
        double wl = text_line(p, label, fid, size, l1 ? INK : (Color){ 0.25, 0.25, 0.28 }, PAGE_W - MARGIN - ind, y, 'r');
        double wn = text_line(p, pn, font_need("sans"), 12, l1 ? ACCENT : GREY, MARGIN, y, 'l');
        /* dotted leader */
        double xa = MARGIN + wn + 8, xb = PAGE_W - MARGIN - ind - wl - 8;
        if (xb > xa)
            buf_printf(&p->c, "q 0.6 0.6 0.62 RG 1.1 w 1 J [0 3.2] 0 d %.2f %.2f m %.2f %.2f l S Q\n", xa, y + 2.5, xb, y + 2.5);
        char tg[48]; snprintf(tg, sizeof tg, "head:%d", i);
        add_annot(p, MARGIN, y - 6, PAGE_W - MARGIN, y + size + 2, tg);
        y -= l1 ? 30 : 25;
    }
}

static void draw_chrome(void) {
    for (int i = 0; i < D.npages; i++) {
        Page *p = page_at(i);
        if (!p->chrome) continue;
        /* running header: document title on the right, current chapter on the left */
        const char *section = NULL;
        for (int h = 0; h < D.nheads; h++) if (D.heads[h].level == 1 && D.heads[h].page <= i) section = D.heads[h].text;
        if (i == D.toc_page) section = "المحتويات";
        double hy = PAGE_H - 50;
        text_line(p, D.title, font_need("sans"), 9, GREY, PAGE_W - MARGIN, hy, 'r');
        if (section) text_line(p, section, font_need("sans"), 9, GREY, MARGIN, hy, 'l');
        buf_printf(&p->c, "q 0.80 0.84 0.84 RG 0.6 w %.2f %.2f m %.2f %.2f l S Q\n", MARGIN, hy - 8, PAGE_W - MARGIN, hy - 8);
        char pn[16]; ar_num(i + 1, pn);
        fill_circle(&p->c, PAGE_W / 2, 40, 11, ACCENT);
        text_line(p, pn, font_need("sans"), 10, WHITE, PAGE_W / 2, 36.5, 'c');
    }
}

static void bibliography(void) {
    heading(1, "المراجع", 0);
    for (int n = 1; n <= D.ncited; n++) {
        BibEntry *b = NULL;
        for (int i = 0; i < D.nbib; i++) if (D.bib[i].num == n) b = &D.bib[i];
        if (!b) continue;
        PStyle st = { font_need("ref"), 11.5, INK, 11.5 * 1.6, 0, 0, 0, 30, 0 };
        int rtl = text_is_rtl(b->text);
        if (rtl) { st.ind_l = 0; st.ind_r = 30; }
        ensure(st.lead * 2);
        b->page = D.npages - 1;
        b->y = D.y;
        int pg; double base;
        paragraph(b->text, &st, &pg, &base);
        char lab[16]; snprintf(lab, sizeof lab, "[%d]", n);
        text_line(page_at(pg), lab, font_need("sans"), 11, ACCENT, rtl ? PAGE_W - MARGIN : MARGIN, base, rtl ? 'r' : 'l');
        D.y -= 6;
    }
}

/* ------------------------------------------------------------ PDF writer */

static int font_in_use(Font *f) {
    for (unsigned g = 1; g < f->nglyphs; g++) if (f->used[g]) return 1;
    return 0;
}

static void write_font(Pdf *p, Font *f, int id0, int tounicode, int index) {
    int FONT0 = id0, CIDF = id0 + 1, DESC = id0 + 2, FILE2 = id0 + 3, TOUNI = id0 + 4, CIDMAP = id0 + 5;
    hb_map_t *mapping = hb_map_create();
    hb_set_t *unis = hb_set_create();
    hb_face_collect_nominal_glyph_mapping(f->face, mapping, unis);
    hb_codepoint_t u = HB_SET_VALUE_INVALID;
    while (hb_set_next(unis, &u)) {
        hb_codepoint_t g = hb_map_get(mapping, u);
        int presentation = (u >= 0xFB50 && u <= 0xFDFF) || (u >= 0xFE70 && u <= 0xFEFF);
        if (g && g < f->nglyphs && f->used[g] && !presentation) { uint32_t cc = u; font_map(f, g, &cc, 1); }
    }
    hb_map_destroy(mapping);
    hb_set_destroy(unis);
    /* width-only variants inherit their glyph's text */
    for (int c = f->nglyphs; c < f->ncid; c++) {
        unsigned g = f->cid_gid[c];
        if (!f->uni[c] && f->uni[g]) {
            f->uni[c] = malloc(f->uni_len[g] * sizeof(uint32_t));
            memcpy(f->uni[c], f->uni[g], f->uni_len[g] * sizeof(uint32_t));
            f->uni_len[c] = f->uni_len[g];
        }
    }

    hb_subset_input_t *in = hb_subset_input_create_or_fail();
    hb_set_t *gs = hb_subset_input_glyph_set(in);
    hb_set_add(gs, 0);
    for (unsigned g = 0; g < f->nglyphs; g++) if (f->used[g]) hb_set_add(gs, g);
    hb_subset_input_set_flags(in, HB_SUBSET_FLAGS_RETAIN_GIDS);
    hb_face_t *sub = hb_subset_or_fail(f->face, in);
    hb_blob_t *subblob = sub ? hb_face_reference_blob(sub) : hb_blob_reference(f->blob);
    unsigned fontlen;
    const char *fontdata = hb_blob_get_data(subblob, &fontlen);
    char tag[8];
    unsigned hsh = 5381 + index * 7919; for (int i = 0; f->name[i]; i++) hsh = hsh * 33 + f->name[i];  /* unique per font slot */
    for (int i = 0; i < 6; i++) { tag[i] = 'A' + hsh % 26; hsh /= 26; }
    tag[6] = 0;

    pdf_begin_obj(p, FONT0);
    fprintf(p->fp, "<< /Type /Font /Subtype /Type0 /BaseFont /%s+%s /Encoding /Identity-H /DescendantFonts [%d 0 R]",
            tag, f->name, CIDF);
    if (tounicode) fprintf(p->fp, " /ToUnicode %d 0 R", TOUNI);
    fprintf(p->fp, " >>\nendobj\n");

    pdf_begin_obj(p, CIDF);
    fprintf(p->fp, "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /%s+%s "
                   "/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
                   "/FontDescriptor %d 0 R /DW 0 /W [", tag, f->name, DESC);
    for (int c = 0; c < f->ncid; c++) {
        if (!f->used[f->cid_gid[c]]) continue;
        fprintf(p->fp, " %d [%d]", c, f->cid_w[c]);
    }
    if (f->ncid > (int)f->nglyphs) fprintf(p->fp, " ] /CIDToGIDMap %d 0 R >>\nendobj\n", CIDMAP);
    else fprintf(p->fp, " ] /CIDToGIDMap /Identity >>\nendobj\n");
    if (f->ncid > (int)f->nglyphs) {
        unsigned char *m = malloc(f->ncid * 2);
        for (int c = 0; c < f->ncid; c++) { m[2 * c] = f->cid_gid[c] >> 8; m[2 * c + 1] = f->cid_gid[c] & 0xFF; }
        pdf_stream(p, CIDMAP, NULL, m, f->ncid * 2, 1);
        free(m);
    }
    hb_font_extents_t ext;
    hb_font_get_h_extents(f->font, &ext);
    double k = 1000.0 / f->upem;
    pdf_begin_obj(p, DESC);
    fprintf(p->fp, "<< /Type /FontDescriptor /FontName /%s+%s /Flags 4 /FontBBox [-1000 %d 2500 %d] "
                   "/ItalicAngle 0 /Ascent %d /Descent %d /CapHeight %d /StemV 80 /FontFile2 %d 0 R >>\nendobj\n",
            tag, f->name, (int)(ext.descender * k * 1.6), (int)(ext.ascender * k * 1.6),
            (int)(ext.ascender * k), (int)(ext.descender * k), (int)(ext.ascender * k), FILE2);
    char extra[64];
    snprintf(extra, sizeof extra, "/Length1 %u", fontlen);
    pdf_stream(p, FILE2, extra, fontdata, fontlen, 1);

    Buf cm = {0};
    buf_printf(&cm, "/CIDInit /ProcSet findresource begin\n12 dict begin\nbegincmap\n"
                    "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
                    "/CMapName /Qalam-UCS def\n/CMapType 2 def\n"
                    "1 begincodespacerange\n<0000> <FFFF>\nendcodespacerange\n");
    int g = 0;
    while (g < f->ncid) {
        int batch[100], nb = 0;
        for (; g < f->ncid && nb < 100; g++) if (f->used[f->cid_gid[g]] && f->uni[g] && f->uni_len[g]) batch[nb++] = g;
        if (!nb) break;
        buf_printf(&cm, "%d beginbfchar\n", nb);
        for (int i = 0; i < nb; i++) {
            buf_printf(&cm, "<%04X> ", batch[i]);
            put_utf16_hex(&cm, f->uni[batch[i]], f->uni_len[batch[i]], 0);
            buf_printf(&cm, "\n");
        }
        buf_printf(&cm, "endbfchar\n");
    }
    buf_printf(&cm, "endcmap\nCMapName currentdict /CMap defineresource pop\nend\nend\n");
    pdf_stream(p, TOUNI, NULL, cm.p, cm.n, 1);
    free(cm.p);
    hb_blob_destroy(subblob);
    if (sub) hb_face_destroy(sub);
    hb_subset_input_destroy(in);
}

static void utf8_pdf_string(FILE *fp, const char *s) {
    uint32_t buf[1024];
    int n = fribidi_charset_to_unicode(FRIBIDI_CHAR_SET_UTF8, s, strnlen(s, 1000), buf);
    Buf b = {0};
    put_utf16_hex(&b, buf, n, 1);
    fputs(b.p, fp);
    free(b.p);
}

static int resolve_target(const char *t, int *page, double *y) {
    int i;
    if (sscanf(t, "head:%d", &i) == 1 && i < D.nheads) { *page = D.heads[i].page; *y = D.heads[i].y; return 1; }
    if (sscanf(t, "bib:%d", &i) == 1)
        for (int k = 0; k < D.nbib; k++) if (D.bib[k].num == i) { *page = D.bib[k].page; *y = D.bib[k].y; return 1; }
    return 0;
}

static void write_pdf(const char *path, int tounicode) {
    Pdf p = { fopen(path, "wb"), NULL, 0, 0 };
    if (!p.fp) { fprintf(stderr, "qalam: cannot write %s\n", path); exit(1); }
    fprintf(p.fp, "%%PDF-1.7\n%%\xE2\xE3\xCF\xD3\n");
    int next = 1;
    int CAT = next++, PAGES = next++, OUTL = next++, INFO = next++;
    int *pg_id = malloc(D.npages * sizeof(int)), *ct_id = malloc(D.npages * sizeof(int)), *an_id = malloc(D.npages * sizeof(int));
    for (int i = 0; i < D.npages; i++) { pg_id[i] = next++; ct_id[i] = next++; an_id[i] = next; next += D.pages[i].nan; }
    int font_id[MAXFONTS];
    for (int i = 0; i < nfonts; i++) { font_id[i] = font_in_use(&fonts[i].f) ? next : 0; if (font_id[i]) next += 6; }
    int nh = D.nheads, out0 = next; next += nh;

    pdf_begin_obj(&p, CAT);
    /* note: no /ViewerPreferences /Direction /R2L - pdfium then reverses the
     * already-logical ActualText of every line */
    fprintf(p.fp, "<< /Type /Catalog /Pages %d 0 R /Lang (ar) /MarkInfo << /Marked true >> "
                  "/ViewerPreferences << /DisplayDocTitle true >> /PageMode /UseOutlines /Outlines %d 0 R >>\nendobj\n",
            PAGES, OUTL);
    pdf_begin_obj(&p, INFO);
    fprintf(p.fp, "<< /Title "); utf8_pdf_string(p.fp, D.title);
    if (D.author[0]) { fprintf(p.fp, " /Author "); utf8_pdf_string(p.fp, D.author); }
    fprintf(p.fp, " /Creator (qalam 0.2) /Producer (qalam 0.2 - HarfBuzz, FriBidi) >>\nendobj\n");

    pdf_begin_obj(&p, PAGES);
    fprintf(p.fp, "<< /Type /Pages /Count %d /Kids [", D.npages);
    for (int i = 0; i < D.npages; i++) fprintf(p.fp, " %d 0 R", pg_id[i]);
    fprintf(p.fp, " ] >>\nendobj\n");

    for (int i = 0; i < D.npages; i++) {
        Page *pg = &D.pages[i];
        pdf_begin_obj(&p, pg_id[i]);
        fprintf(p.fp, "<< /Type /Page /Parent %d 0 R /MediaBox [0 0 %.0f %.0f] /Resources << /Font <<", PAGES, PAGE_W, PAGE_H);
        for (int f = 0; f < nfonts; f++) if (font_id[f]) fprintf(p.fp, " /F%d %d 0 R", f, font_id[f]);
        fprintf(p.fp, " >> >> /Contents %d 0 R", ct_id[i]);
        if (pg->nan) {
            fprintf(p.fp, " /Annots [");
            for (int a = 0; a < pg->nan; a++) fprintf(p.fp, " %d 0 R", an_id[i] + a);
            fprintf(p.fp, " ]");
        }
        fprintf(p.fp, " >>\nendobj\n");
        pdf_stream(&p, ct_id[i], NULL, pg->c.p ? pg->c.p : "", pg->c.n, 1);
        for (int a = 0; a < pg->nan; a++) {
            Annot *A = &pg->an[a];
            int tp; double ty;
            pdf_begin_obj(&p, an_id[i] + a);
            fprintf(p.fp, "<< /Type /Annot /Subtype /Link /Rect [%.2f %.2f %.2f %.2f] /Border [0 0 0]",
                    A->x0, A->y0, A->x1, A->y1);
            if (resolve_target(A->target, &tp, &ty))
                fprintf(p.fp, " /Dest [%d 0 R /XYZ 0 %.2f 0]", pg_id[tp], ty + 20);
            fprintf(p.fp, " >>\nendobj\n");
        }
    }
    for (int f = 0; f < nfonts; f++) if (font_id[f]) write_font(&p, &fonts[f].f, font_id[f], tounicode, f);

    /* bookmarks: level-1 headings at the top, level-2 nested under the previous level-1 */
    int top[256], ntop = 0, parent[256];
    for (int i = 0; i < nh; i++) {
        if (D.heads[i].level == 1 || ntop == 0) { top[ntop++] = i; parent[i] = -1; }
        else parent[i] = top[ntop - 1];
    }
    pdf_begin_obj(&p, OUTL);
    if (ntop) fprintf(p.fp, "<< /Type /Outlines /First %d 0 R /Last %d 0 R /Count %d >>\nendobj\n", out0 + top[0], out0 + top[ntop - 1], nh);
    else fprintf(p.fp, "<< /Type /Outlines /Count 0 >>\nendobj\n");
    for (int i = 0; i < nh; i++) {
        Heading *h = &D.heads[i];
        int par = parent[i];
        int prev = -1, nxt = -1, first = -1, last = -1, kids = 0;
        for (int j = i - 1; j >= 0; j--) if (parent[j] == par) { prev = j; break; }
        for (int j = i + 1; j < nh; j++) if (parent[j] == par) { nxt = j; break; }
        if (par == -1) for (int j = i + 1; j < nh && parent[j] == i; j++) { if (first < 0) first = j; last = j; kids++; }
        char label[600];
        if (h->numbered) snprintf(label, sizeof label, "%s  %s", h->num, h->text); else snprintf(label, sizeof label, "%s", h->text);
        pdf_begin_obj(&p, out0 + i);
        fprintf(p.fp, "<< /Title "); utf8_pdf_string(p.fp, label);
        fprintf(p.fp, " /Parent %d 0 R", par < 0 ? OUTL : out0 + par);
        if (prev >= 0) fprintf(p.fp, " /Prev %d 0 R", out0 + prev);
        if (nxt >= 0) fprintf(p.fp, " /Next %d 0 R", out0 + nxt);
        if (first >= 0) fprintf(p.fp, " /First %d 0 R /Last %d 0 R /Count %d", out0 + first, out0 + last, kids);
        fprintf(p.fp, " /Dest [%d 0 R /XYZ 0 %.2f 0] >>\nendobj\n", pg_id[h->page], h->y + 20);
    }

    long xref = ftell(p.fp);
    fprintf(p.fp, "xref\n0 %d\n0000000000 65535 f \n", p.nobj + 1);
    for (int i = 1; i <= p.nobj; i++) {
        if (p.off[i]) fprintf(p.fp, "%010ld 00000 n \n", p.off[i]);
        else fprintf(p.fp, "0000000000 65535 f \n");
    }
    fprintf(p.fp, "trailer\n<< /Size %d /Root %d 0 R /Info %d 0 R >>\nstartxref\n%ld\n%%%%EOF\n", p.nobj + 1, CAT, INFO, xref);
    fclose(p.fp);
    free(p.off); free(pg_id); free(ct_id); free(an_id);
}

/* ------------------------------------------------------------------ input */

static char *read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "qalam: cannot open %s\n", path); exit(1); }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (fread(s, 1, n, fp) != (size_t)n) die("read error");
    s[n] = 0;
    fclose(fp);
    return s;
}


/* ------------------------------------------------- finding files, CLI */

#ifndef QALAM_VERSION
#define QALAM_VERSION "dev"
#endif

#if defined(_WIN32)
__declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *, char *, unsigned long);
#define PATHSEP ';'
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#include <limits.h>
#define PATHSEP ':'
#else
#include <unistd.h>
#include <limits.h>
#define PATHSEP ':'
#endif

static char exedir[4096];   /* folder of the qalam executable, with a trailing slash */
static char indir[4096];    /* folder of the input file, with a trailing slash        */

static int dir_len(const char *path) {
    int d = 0;
    for (int i = 0; path[i]; i++) if (path[i] == '/' || path[i] == '\\') d = i + 1;
    return d;
}

/* argv[0] is not enough: run from PATH it is just "qalam", so ask the system */
static void find_exedir(const char *argv0) {
    char p[4096];
    p[0] = 0;
#if defined(_WIN32)
    unsigned long n = GetModuleFileNameA(NULL, p, sizeof p);
    if (n == 0 || n >= sizeof p) p[0] = 0;
#elif defined(__APPLE__)
    char raw[4096];
    uint32_t n = sizeof raw;
    if (_NSGetExecutablePath(raw, &n) != 0 || !realpath(raw, p)) p[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    p[n > 0 ? n : 0] = 0;
#endif
    if (!p[0] && argv0) snprintf(p, sizeof p, "%s", argv0);
    int d = dir_len(p);
    if (d && d < (int)sizeof exedir) { memcpy(exedir, p, d); exedir[d] = 0; }
}

static int is_abs(const char *p) { return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':'); }

static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Where a font named in @font can be:
 *   as written (absolute, or relative to the current folder)
 *   next to the input file
 *   in each folder of $QALAM_FONTS
 *   in the fonts qalam ships with:  <exe>/../share/qalam/fonts   (Linux, macOS install)
 *                                   <exe>/../fonts                (Windows, release archives, a build/ in a checkout)
 *   in the system font folders                                                         */
static char *find_font(const char *name) {
    static char buf[8192];
    if (file_exists(name)) return strdup(name);
    if (is_abs(name)) return NULL;
    #define TRY(...) do { snprintf(buf, sizeof buf, __VA_ARGS__); if (file_exists(buf)) return strdup(buf); } while (0)
    if (indir[0]) TRY("%s%s", indir, name);
    const char *env = getenv("QALAM_FONTS");
    while (env && *env) {
        const char *c = strchr(env, PATHSEP);
        int l = c ? (int)(c - env) : (int)strlen(env);
        if (l) TRY("%.*s/%s", l, env, name);
        env = c ? c + 1 : NULL;
    }
    if (exedir[0]) {
        TRY("%s../share/qalam/fonts/%s", exedir, name);
        TRY("%s../fonts/%s", exedir, name);
        TRY("%sfonts/%s", exedir, name);
    }
#if defined(_WIN32)
    const char *win = getenv("WINDIR");
    TRY("%s\\Fonts\\%s", win ? win : "C:\\Windows", name);
    const char *la = getenv("LOCALAPPDATA");
    if (la) TRY("%s\\Microsoft\\Windows\\Fonts\\%s", la, name);
#elif defined(__APPLE__)
    TRY("/Library/Fonts/%s", name);
    TRY("/System/Library/Fonts/%s", name);
    const char *home = getenv("HOME");
    if (home) TRY("%s/Library/Fonts/%s", home, name);
#else
    const char *home = getenv("HOME");
    if (home) TRY("%s/.local/share/fonts/%s", home, name);
    if (home) TRY("%s/.fonts/%s", home, name);
    TRY("/usr/local/share/fonts/%s", name);
    TRY("/usr/share/fonts/truetype/%s", name);
#endif
    #undef TRY
    return NULL;
}

static void put_stderr(int c, void *ctx) { (void)ctx; fputc(c, stderr); }

static void usage(FILE *f) {
    fprintf(f,
        "qalam %s - Arabic-first documents, as PDFs you can copy and search\n"
        "\n"
        "usage: qalam [options] input.qlm          writes input.pdf\n"
        "       qalam [options] -o out.pdf input.qlm\n"
        "\n"
        "  -o FILE                  write the PDF to FILE\n"
        "  --check                  read and lay out the document, write nothing\n"
        "  --actualtext=line|word   ActualText span per line (default) or per word\n"
        "  --no-actualtext          rely on ToUnicode only (best for poppler/pdftotext)\n"
        "  --no-tounicode           omit ToUnicode (for testing: reproduces broken PDFs)\n"
        "  --fonts                  print where the bundled fonts are\n"
        "  --version, --help\n"
        "\n"
        "Fonts named in @font are looked for next to the input, in $QALAM_FONTS,\n"
        "in the fonts that come with qalam, and in the system font folders.\n", QALAM_VERSION);
}

int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL;
    int tounicode = 1, check = 0;
    find_exedir(argv[0]);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-actualtext")) D.actualtext = 0;
        else if (!strcmp(argv[i], "--actualtext=word")) D.actualtext = 1;
        else if (!strcmp(argv[i], "--actualtext=line")) D.actualtext = 2;
        else if (!strcmp(argv[i], "--no-tounicode")) tounicode = 0;
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "--version")) {
            printf("qalam %s (gram %s, HarfBuzz %s, FriBidi %s)\n", QALAM_VERSION, GRAM_VERSION, hb_version_string(), FRIBIDI_VERSION);
            return 0;
        }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(stdout); return 0; }
        else if (!strcmp(argv[i], "--fonts")) {
            char *p = find_font("Amiri-Regular.ttf");
            if (!p) { printf("no bundled fonts found (looked in %s../share/qalam/fonts and %s../fonts)\n", exedir, exedir); return 1; }
            p[dir_len(p)] = 0;
            printf("%s\n", p);
            return 0;
        }
        else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "qalam: unknown option %s\n\n", argv[i]); usage(stderr); return 2; }
        else if (!in) in = argv[i];
        else if (!out) out = argv[i];          /* qalam in.qlm out.pdf still works */
    }
    if (!in) { usage(stderr); return 2; }
    char outbuf[4096];
    if (!out) {
        int dot = -1, d = dir_len(in);
        for (int i = d; in[i]; i++) if (in[i] == '.') dot = i;
        if (dot < 0) dot = strlen(in);
        snprintf(outbuf, sizeof outbuf, "%.*s.pdf", dot, in);
        out = outbuf;
    }
    { int d = dir_len(in); if (d < (int)sizeof indir) { memcpy(indir, in, d); indir[d] = 0; } }
    char *src = read_file(in);

    size_t srclen = strlen(src);
    src_name = in; src_text = src; src_len = srclen;

    /* parse the whole document with gram (grammar: src/qalam.gram) */
    size_t memsize = srclen * 96 + (1 << 20);
    void *mem = malloc(memsize);
    gram_arena arena;
    gram_error err;
    gram_arena_init(&arena, mem, memsize);
    gram_grammar *g = gram_compile(&arena, QALAM_GRAMMAR, &err);
    if (!g) { fprintf(stderr, "qalam: internal grammar error: %s\n", err.msg); return 1; }
    gram_node *doc = gram_parse(g, "document", src, srclen, &arena, &err);
    if (!doc) {
        fprintf(stderr, "%s:", in);
        gram_error_print(&err, put_stderr, NULL);
        return 1;
    }

    /* pass 1: fonts and bibliography entries, so citations can come first */
    for (const gram_node *b = doc->child; b; b = b->next) {
        if (b->rule == QALAM_R_font) {
            if (nfonts == MAXFONTS) { fprintf(stderr, "%s: too many fonts (at most %d)\n", where(b), MAXFONTS); return 1; }
            char *key = node_str(b->child), *path = node_str(b->child->next);
            snprintf(fonts[nfonts].key, sizeof fonts[nfonts].key, "%s", key);
            char *found = find_font(path);
            if (!found) {
                fprintf(stderr, "%s: font '%s' not found (looked next to %s, in $QALAM_FONTS, in qalam's fonts folder and the system fonts)\n",
                        where(b->child->next), path, in);
                return 1;
            }
            font_load(&fonts[nfonts].f, found);
            free(found); free(key); free(path);
            nfonts++;
        } else if (b->rule == QALAM_R_bib && D.nbib < 128) {
            BibEntry *e = &D.bib[D.nbib++];
            memset(e, 0, sizeof *e);
            char *key = node_str(b->child);
            snprintf(e->key, sizeof e->key, "%s", key);
            free(key);
            e->text = node_str(b->child->next);
        }
    }
    if (font_find("body") < 0) die("no '@font body <file>' defined");

    /* pass 2: lay out every block in order */
    new_page(1);
    for (const gram_node *b = doc->child; b; b = b->next) {
        const gram_node *k = b->child;
        char *t;
        switch (b->rule) {
        case QALAM_R_font: case QALAM_R_bib: break;
        case QALAM_R_unknown:
            t = node_str(k);
            fprintf(stderr, "%s: unknown directive %s (known: @font @size @bib @title @subtitle @author @date "
                            "@maketitle @toc @pagebreak @bibliography @caption @figure @table)\n", where(k), t);
            return 1;
        case QALAM_R_size:      t = node_str(k); D.body_size = atof(t); free(t); break;
        case QALAM_R_title:     t = node_str(k); snprintf(D.title, sizeof D.title, "%s", t); free(t); break;
        case QALAM_R_subtitle:  t = node_str(k); snprintf(D.subtitle, sizeof D.subtitle, "%s", t); free(t); break;
        case QALAM_R_author:    t = node_str(k); snprintf(D.author, sizeof D.author, "%s", t); free(t); break;
        case QALAM_R_date:      t = node_str(k); snprintf(D.date, sizeof D.date, "%s", t); free(t); break;
        case QALAM_R_maketitle: make_title(); break;
        case QALAM_R_toc:       reserve_toc(); break;
        case QALAM_R_pagebreak: new_page(1); break;
        case QALAM_R_bibliography: bibliography(); break;
        case QALAM_R_caption:   t = node_str(k); caption(t); free(t); break;
        case QALAM_R_heading: {
            char *lv = node_str(k);
            t = node_str(k->next);
            if (!strcmp(lv, "##")) heading(2, t, 1);
            else if (!strcmp(lv, "#*")) heading(1, t, 0);
            else heading(1, t, 1);
            free(lv); free(t);
            break;
        }
        case QALAM_R_quote: t = inline_str(k); quote_block(t); free(t); break;
        case QALAM_R_item:  t = inline_str(k); list_item(t); free(t); break;
        case QALAM_R_para: {
            /* the lines of a paragraph, joined with a space */
            Buf p = {0};
            buf_put(&p, "", 0);
            for (const gram_node *line = k; line; line = line->next) {
                if (p.n) buf_put(&p, " ", 1);
                inline_text(&p, line->child);
            }
            body_paragraph(p.p);
            free(p.p);
            break;
        }
        case QALAM_R_figure: {
            char *w = node_str(k), *h = node_str(k->next);
            figure_begin(atof(w), atof(h));
            free(w); free(h);
            for (const gram_node *d = k->next->next; d; d = d->next)
                if (d->rule == QALAM_R_draw) draw_command(d);
            figure_end();
            break;
        }
        case QALAM_R_table: {
            static Table T;
            T.ncol = 0; T.nrows = 0;
            for (const gram_node *w = k->child; w && T.ncol < 16; w = w->next) {
                t = node_str(w);
                double v = atof(t);
                free(t);
                if (v <= 0) { fprintf(stderr, "%s: a column width must be more than 0\n", where(w)); return 1; }
                T.frac[T.ncol++] = v;
            }
            for (const gram_node *r = k->next; r && T.nrows < 256; r = r->next)
                if (r->rule == QALAM_R_row) T.rows[T.nrows++] = r;
            if (T.nrows) table_render(&T);
            break;
        }
        default: break;
        }
    }
    fill_toc();
    draw_chrome();
    if (check) { fprintf(stderr, "qalam: %s is fine (%d pages)\n", in, D.npages); return 0; }
    write_pdf(out, tounicode);
    fprintf(stderr, "qalam: wrote %s (%d pages)\n", out, D.npages);
    return 0;
}
