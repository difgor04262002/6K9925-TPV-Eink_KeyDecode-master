/*
 * Stream a PNG of any size down to a BGR buffer that fits in CMA.
 * LVGL then scales that buffer onto the panel.
 */
#include <rtconfig.h>
#ifdef AIC_EPD_P118F04

#include <rtthread.h>
#include <string.h>
#include "aic_osal.h"
#include "miniz_tinfl.h"

#define PNG_ROW_MAX (256U * 1024U)

struct png_out {
    uint8_t *bgr;
    int src_w;
    int src_h;
    int out_w;
    int out_h;
    int next_oy;
    int color;
    int depth;
    int bpp;
    int rowbytes;
    uint8_t *cur;
    uint8_t *prev;
    uint8_t pal[256 * 3];
    int pal_n;
    uint8_t *scan;
    int scan_have;
    int scan_need;
    int row_i;
    int failed;
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c)
{
    int p = (int)a + (int)b - (int)c;
    int pa = p - (int)a;
    int pb = p - (int)b;
    int pc = p - (int)c;

    if (pa < 0)
        pa = -pa;
    if (pb < 0)
        pb = -pb;
    if (pc < 0)
        pc = -pc;
    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

static void unfilter(uint8_t *dst, const uint8_t *raw, const uint8_t *prev,
                     int n, int bpp, int type)
{
    int i;

    for (i = 0; i < n; i++) {
        uint8_t a = (i >= bpp) ? dst[i - bpp] : 0;
        uint8_t b = prev[i];
        uint8_t c = (i >= bpp) ? prev[i - bpp] : 0;
        uint8_t v = raw[i];

        if (type == 1)
            v = (uint8_t)(v + a);
        else if (type == 2)
            v = (uint8_t)(v + b);
        else if (type == 3)
            v = (uint8_t)(v + ((a + b) >> 1));
        else if (type == 4)
            v = (uint8_t)(v + paeth(a, b, c));
        dst[i] = v;
    }
}

static int sample8(const uint8_t *row, int x, int depth, int ch)
{
    int bpp = ch * (depth / 8);
    const uint8_t *p;

    if (depth == 16)
        return row[x * bpp];
    if (depth != 8)
        return 0;
    p = row + x * ch;
    return p[0];
}

static void pixel_bgr(struct png_out *o, const uint8_t *row, int x,
                      uint8_t *b, uint8_t *g, uint8_t *r)
{
    int ch = 1;
    int idx;

    if (o->color == 2)
        ch = 3;
    else if (o->color == 4)
        ch = 2;
    else if (o->color == 6)
        ch = 4;

    if (o->color == 3) {
        int bitpos = x * o->depth;
        int shift = 8 - o->depth - (bitpos & 7);

        idx = (row[bitpos >> 3] >> shift) & ((1 << o->depth) - 1);
        if (idx >= o->pal_n)
            idx = 0;
        *r = o->pal[idx * 3];
        *g = o->pal[idx * 3 + 1];
        *b = o->pal[idx * 3 + 2];
        return;
    }
    if (o->depth < 8) {
        *r = *g = *b = 255;
        return;
    }
    if (o->color == 0 || o->color == 4) {
        int v = sample8(row, x, o->depth, ch);

        *r = *g = *b = (uint8_t)v;
        return;
    }
    *r = (uint8_t)sample8(row, x, o->depth, ch);
    *g = row[x * (ch * (o->depth / 8)) + (o->depth / 8)];
    *b = row[x * (ch * (o->depth / 8)) + 2 * (o->depth / 8)];
    if (o->color == 6) {
        int a = row[x * (ch * (o->depth / 8)) + 3 * (o->depth / 8)];

        if (a < 128) {
            *r = 255;
            *g = 255;
            *b = 255;
        }
    }
}

static void emit_row(struct png_out *o)
{
    int oy = o->next_oy;

    while (oy < o->out_h) {
        int sy = (o->out_h <= 1) ? 0 :
                 (int)((uint32_t)oy * (uint32_t)(o->src_h - 1) / (uint32_t)(o->out_h - 1));
        int ox;

        if (sy != o->row_i)
            break;
        for (ox = 0; ox < o->out_w; ox++) {
            int sx = (o->out_w <= 1) ? 0 :
                     (int)((uint32_t)ox * (uint32_t)(o->src_w - 1) / (uint32_t)(o->out_w - 1));
            uint8_t *d = o->bgr + ((uint32_t)oy * (uint32_t)o->out_w + (uint32_t)ox) * 3U;
            uint8_t b, g, r;

            pixel_bgr(o, o->cur, sx, &b, &g, &r);
            d[0] = b;
            d[1] = g;
            d[2] = r;
        }
        oy++;
    }
    o->next_oy = oy;
}

static void take_bytes(struct png_out *o, const uint8_t *src, int n)
{
    int off = 0;

    while (off < n && !o->failed) {
        int space = o->scan_need - o->scan_have;
        int copy = n - off;

        if (copy > space)
            copy = space;
        memcpy(o->scan + o->scan_have, src + off, (size_t)copy);
        o->scan_have += copy;
        off += copy;
        if (o->scan_have < o->scan_need)
            break;
        if (o->scan[0] > 4) {
            o->failed = 1;
            return;
        }
        unfilter(o->cur, o->scan + 1, o->prev, o->rowbytes, o->bpp, o->scan[0]);
        emit_row(o);
        memcpy(o->prev, o->cur, (size_t)o->rowbytes);
        o->row_i++;
        o->scan_have = 0;
    }
}

static int shrink_to(int sw, int sh, uint32_t cap, int *dw, int *dh)
{
    int w = sw;
    int h = sh;

    if (w < 1 || h < 1)
        return -1;
    while ((uint64_t)w * (uint64_t)h * 3U > cap) {
        if (w >= h && w > 1)
            w = (w > 8) ? (w - w / 8) : (w - 1);
        else if (h > 1)
            h = (h > 8) ? (h - h / 8) : (h - 1);
        else
            return -1;
        if (w < 1)
            w = 1;
        if (h < 1)
            h = 1;
    }
    *dw = w;
    *dh = h;
    return 0;
}

int epd_png_to_bgr(const uint8_t *file, uint32_t flen, uint32_t cap,
                   uint8_t **out, int *ow, int *oh)
{
    struct png_out o;
    uint8_t *idat = RT_NULL;
    uint32_t idat_len = 0;
    uint32_t idat_cap;
    const uint8_t *p;
    const uint8_t *end;
    tinfl_decompressor *inf = RT_NULL;
    uint8_t *dict = RT_NULL;
    size_t in_ofs = 0;
    size_t dict_ofs = 0;
    int ch;
    int ok = 0;

    *out = RT_NULL;
    *ow = 0;
    *oh = 0;
    if (!file || flen < 24 || file[0] != 0x89 || file[1] != 'P' ||
        file[2] != 'N' || file[3] != 'G')
        return -1;

    memset(&o, 0, sizeof(o));
    o.src_w = (int)be32(file + 16);
    o.src_h = (int)be32(file + 20);
    o.depth = file[24];
    o.color = file[25];
    if (o.src_w < 1 || o.src_h < 1 || file[26] != 0 || file[27] != 0) {
        rt_kprintf("epd: png header bad\n");
        return -1;
    }
    if (file[28] != 0) {
        rt_kprintf("epd: png interlaced\n");
        return -1;
    }
    if (shrink_to(o.src_w, o.src_h, cap, &o.out_w, &o.out_h) != 0) {
        rt_kprintf("epd: png %dx%d too large\n", o.src_w, o.src_h);
        return -1;
    }

    ch = (o.color == 2) ? 3 : (o.color == 4) ? 2 : (o.color == 6) ? 4 : 1;
    if (o.depth != 8 && o.depth != 16 && !(o.color == 3 && (o.depth == 1 || o.depth == 2 || o.depth == 4 || o.depth == 8))) {
        rt_kprintf("epd: png depth %d color %d\n", o.depth, o.color);
        return -1;
    }
    if (o.depth >= 8)
        o.bpp = ch * (o.depth / 8);
    else
        o.bpp = 1;
    o.rowbytes = (o.src_w * o.depth * ch + 7) / 8;
    if (o.rowbytes <= 0 || (uint32_t)o.rowbytes > PNG_ROW_MAX) {
        rt_kprintf("epd: png row %d\n", o.rowbytes);
        return -1;
    }
    o.scan_need = o.rowbytes + 1;

    idat_cap = flen;
    idat = aicos_malloc(MEM_CMA, idat_cap);
    o.bgr = aicos_malloc(MEM_CMA, (uint32_t)o.out_w * (uint32_t)o.out_h * 3U);
    o.cur = aicos_malloc(MEM_CMA, (uint32_t)o.rowbytes);
    o.prev = aicos_malloc(MEM_CMA, (uint32_t)o.rowbytes);
    o.scan = aicos_malloc(MEM_CMA, (uint32_t)o.scan_need);
    if (!idat || !o.bgr || !o.cur || !o.prev || !o.scan) {
        rt_kprintf("epd: png alloc failed\n");
        goto done;
    }
    memset(o.bgr, 255, (uint32_t)o.out_w * (uint32_t)o.out_h * 3U);
    memset(o.prev, 0, (size_t)o.rowbytes);

    p = file + 8;
    end = file + flen;
    while (p + 12 <= end) {
        uint32_t n = be32(p);
        const uint8_t *type = p + 4;
        const uint8_t *data = p + 8;

        if ((uint32_t)(end - data) < n + 4U)
            break;
        if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            o.pal_n = (int)(n / 3U);
            if (o.pal_n > 256)
                o.pal_n = 256;
            memcpy(o.pal, data, (size_t)o.pal_n * 3U);
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (idat_len + n > idat_cap) {
                o.failed = 1;
                break;
            }
            memcpy(idat + idat_len, data, n);
            idat_len += n;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }
        p = data + n + 4;
    }
    if (o.failed || idat_len == 0) {
        rt_kprintf("epd: png no data\n");
        goto done;
    }

    inf = aicos_malloc(MEM_CMA, sizeof(*inf));
    dict = aicos_malloc(MEM_CMA, TINFL_LZ_DICT_SIZE);
    if (!inf || !dict) {
        rt_kprintf("epd: png alloc failed\n");
        goto done;
    }
    tinfl_init(inf);
    for (;;) {
        size_t in_size = idat_len - in_ofs;
        size_t out_size = TINFL_LZ_DICT_SIZE - dict_ofs;
        tinfl_status st = tinfl_decompress(inf, idat + in_ofs, &in_size, dict,
                                           dict + dict_ofs, &out_size,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER);

        in_ofs += in_size;
        if (out_size)
            take_bytes(&o, dict + dict_ofs, (int)out_size);
        if (o.failed) {
            rt_kprintf("epd: png filter\n");
            goto done;
        }
        if (out_size == 0 && in_size == 0) {
            rt_kprintf("epd: png inflate stall\n");
            goto done;
        }
        if (st != TINFL_STATUS_HAS_MORE_OUTPUT) {
            if (st != TINFL_STATUS_DONE)
                rt_kprintf("epd: png inflate %d\n", (int)st);
            else
                ok = 1;
            break;
        }
        dict_ofs = (dict_ofs + out_size) & (TINFL_LZ_DICT_SIZE - 1);
    }

done:
    if (ok) {
        *out = o.bgr;
        *ow = o.out_w;
        *oh = o.out_h;
        o.bgr = RT_NULL;
        rt_kprintf("epd: png %dx%d -> %dx%d\n", o.src_w, o.src_h, *ow, *oh);
    }
    if (o.bgr)
        aicos_free(MEM_CMA, o.bgr);
    if (o.cur)
        aicos_free(MEM_CMA, o.cur);
    if (o.prev)
        aicos_free(MEM_CMA, o.prev);
    if (o.scan)
        aicos_free(MEM_CMA, o.scan);
    if (idat)
        aicos_free(MEM_CMA, idat);
    if (inf)
        aicos_free(MEM_CMA, inf);
    if (dict)
        aicos_free(MEM_CMA, dict);
    return ok ? 0 : -1;
}

#endif
