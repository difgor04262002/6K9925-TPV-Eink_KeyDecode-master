/*
 * PC image path for the P118F04.
 *   B3 10 FF 11 22 33 FF 10 B3  start writing the following bytes
 *   B3 10 FF AA AA AA FF 10 B3  show the saved image, rotated 90 deg left
 *   B3 10 FF BB BB BB FF 10 B3  show the saved image, rotated 90 deg right
 * The file is /data/Eink_Image/image.jpg and may be a JPEG or a PNG.
 * A command may be padded with
 * zeros out to the USB packet size. Bytes after the start command, until
 * the next command, are the file body.
 */
#include <rtthread.h>

#ifdef AIC_EPD_P118F04

#include <dfs_posix.h>
#include <sys/stat.h>
#include <string.h>
#include <stdlib.h>
#include "aic_osal.h"
#include "lvgl.h"
#include "tjpgd.h"

#define EPD_W            240
#define EPD_H            206
#define EPD_FRAME_BYTES  ((EPD_W / 2) * EPD_H)
#define EPD_LV_SRC_MAX   (1024U * 1024U)
#define EPD_JPG_POOL     8192

#define EPD_DIR          "/data/Eink_Image"
#define EPD_PATH         "/data/Eink_Image/image.jpg"
/* Same size as the vendor bulk OUT read. A shorter callback is the end
 * of the host Write(). */
#define EPD_USB_FILL     131072U
#define EPD_RAM_MAX      (2U * 1024U * 1024U)

extern void epd_show_packed(const uint8_t *frame);
extern void epd_kick(void);
extern int epd_png_to_bgr(const uint8_t *file, uint32_t flen, uint32_t cap,
                          uint8_t **out, int *ow, int *oh);

static uint8_t *s_ram;
static uint32_t s_ram_cap;
static volatile uint32_t s_ram_len;
static volatile uint8_t s_accept;
static volatile uint8_t s_started;
static volatile uint8_t s_need_save;
static volatile uint8_t s_saving;
static volatile uint8_t s_pending_start;
static volatile uint8_t s_overflow;
static volatile uint8_t s_rot;
/* 0 upright, 1 = 90 CW, 2 = 180, 3 = 90 CCW. Each button adds one step. */
static uint8_t s_quarter;
static uint8_t s_frame[EPD_FRAME_BYTES];

typedef struct {
    int fd;
    uint8_t *rgb;
    int w;
    int h;
} jpg_io_t;

static void ensure_dir(void)
{
    struct stat st;

    if (stat(EPD_DIR, &st) != 0)
        (void)mkdir(EPD_DIR, 0777);
}

/* USB OUT callback runs in the controller interrupt. Only memcpy here. */
static void ram_append(const uint8_t *p, uint32_t n)
{
    uint32_t off;

    if (!s_ram || !n || s_overflow)
        return;
    off = s_ram_len;
    if (off >= s_ram_cap || n > s_ram_cap - off) {
        s_overflow = 1;
        return;
    }
    memcpy(s_ram + off, p, n);
    s_ram_len = off + n;
}

static void save_ram(uint32_t n)
{
    int fd;
    uint32_t off = 0;

    ensure_dir();
    fd = open(EPD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        rt_kprintf("epd: open %s failed\n", EPD_PATH);
        return;
    }
    while (off < n) {
        uint32_t chunk = n - off;
        int w;

        if (chunk > 65536U)
            chunk = 65536U;
        w = write(fd, s_ram + off, chunk);
        if (w <= 0) {
            rt_kprintf("epd: write failed\n");
            close(fd);
            return;
        }
        off += (uint32_t)w;
    }
    (void)fsync(fd);
    close(fd);
    rt_kprintf("epd: saved %u bytes%s\n", (unsigned)n,
               s_overflow ? " (truncated)" : "");
}

static int tail_is_pad(const uint8_t *d, uint32_t len)
{
    uint32_t i;

    for (i = 9; i < len; i++) {
        if (d[i])
            return 0;
    }
    return 1;
}

/* 1 = start file, 2 = rotate left, 3 = rotate right, 0 = not a command. */
static int eink_op(const uint8_t *d, uint32_t len)
{
    if (len < 9U)
        return 0;
    if (d[0] != 0xB3 || d[1] != 0x10 || d[2] != 0xFF)
        return 0;
    if (d[6] != 0xFF || d[7] != 0x10 || d[8] != 0xB3)
        return 0;
    if (d[3] == 0x11 && d[4] == 0x22 && d[5] == 0x33)
        return 1;
    if (d[3] == 0xAA && d[4] == 0xAA && d[5] == 0xAA)
        return 2;
    if (d[3] == 0xBB && d[4] == 0xBB && d[5] == 0xBB)
        return 3;
    return 0;
}

static void epd_usb_render(uint8_t quarter);

void epd_usb_mem_init(void)
{
    s_ram = rt_malloc(EPD_RAM_MAX);
    if (s_ram)
        s_ram_cap = EPD_RAM_MAX;
    else {
        s_ram = rt_malloc(512U * 1024U);
        s_ram_cap = s_ram ? (512U * 1024U) : 0;
    }
    rt_kprintf("epd: image ram %u\n", (unsigned)s_ram_cap);
}

void epd_usb_poll(void)
{
    uint8_t rot;
    uint8_t show = 0;

    if (s_started) {
        s_started = 0;
        ensure_dir();
        rt_kprintf("epd: recv %s\n", EPD_PATH);
    }
    if (s_need_save && !s_accept) {
        uint32_t n = s_ram_len;

        s_need_save = 0;
        s_saving = 1;
        if (s_ram && n) {
            save_ram(n);
            s_quarter = 0;
            show = 1;
        } else if (!s_rot) {
            rt_kprintf("epd: empty image\n");
        }
        s_ram_len = 0;
        s_overflow = 0;
        s_saving = 0;
        if (s_pending_start) {
            s_pending_start = 0;
            s_accept = 1;
            ensure_dir();
            rt_kprintf("epd: recv %s\n", EPD_PATH);
        }
    }
    rot = s_rot;
    if (rot) {
        s_rot = 0;
        if (rot == 2U)
            s_quarter = (uint8_t)((s_quarter + 3U) & 3U);
        else
            s_quarter = (uint8_t)((s_quarter + 1U) & 3U);
        rt_kprintf("epd: rot %s -> %s\n",
                   (rot == 2U) ? "left" : "right",
                   (s_quarter == 1U) ? "90 CW" :
                   (s_quarter == 2U) ? "180" :
                   (s_quarter == 3U) ? "90 CCW" : "0");
        show = 1;
    }
    if (show)
        epd_usb_render(s_quarter);
}

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t ndata)
{
    jpg_io_t *io = jd->device;
    int n;

    if (!ndata)
        return 0;
    if (!buf) {
        if (lseek(io->fd, (off_t)ndata, SEEK_CUR) < 0)
            return 0;
        return ndata;
    }
    n = read(io->fd, buf, ndata);
    if (n <= 0)
        return 0;
    return (size_t)n;
}

static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpg_io_t *io = jd->device;
    int rw = (int)rect->right - (int)rect->left + 1;
    int rh = (int)rect->bottom - (int)rect->top + 1;
    int y;
    const uint8_t *src = bitmap;

    if (rect->right >= io->w || rect->bottom >= io->h || rw <= 0 || rh <= 0)
        return 0;
    for (y = 0; y < rh; y++) {
        memcpy(io->rgb + (((int)rect->top + y) * io->w + (int)rect->left) * 3,
               src + y * rw * 3, (size_t)rw * 3);
    }
    return 1;
}

/* Panel nibbles: black, white, yellow, red, blue, green. */
static const uint8_t k_nib[6] = { 0, 1, 2, 3, 5, 6 };

/* A light chroma lift. Flats and highlights are left on the photo's tone. */
static void tone8(int *r, int *g, int *b)
{
    int y;
    int rr;
    int gg;
    int bb;

    if (*r < 0)
        *r = 0;
    if (*g < 0)
        *g = 0;
    if (*b < 0)
        *b = 0;
    if (*r > 255)
        *r = 255;
    if (*g > 255)
        *g = 255;
    if (*b > 255)
        *b = 255;
    y = (*r * 54 + *g * 183 + *b * 19) >> 8;
    rr = y + (*r - y) * 5 / 4;
    gg = y + (*g - y) * 5 / 4;
    bb = y + (*b - y) * 5 / 4;
    if (rr < 0)
        rr = 0;
    if (gg < 0)
        gg = 0;
    if (bb < 0)
        bb = 0;
    if (rr > 255)
        rr = 255;
    if (gg > 255)
        gg = 255;
    if (bb > 255)
        bb = 255;
    *r = rr;
    *g = gg;
    *b = bb;
}

/* Jarvis, divisor 48. The error is spread wide so the dots read lighter. */
static const int8_t k_jv[12 * 3] = {
    1, 0, 7,   2, 0, 5,
    -2, 1, 3,  -1, 1, 5,  0, 1, 7,  1, 1, 5,  2, 1, 3,
    -2, 2, 1,  -1, 2, 3,  0, 2, 5,  1, 2, 3,  2, 2, 1
};

/* 0 = lo ink, 255 = hi ink. Neutrals stay on black and white.
 * A light blue is nearer to white than to the blue ink, so an open
 * 6-color search paints the sky white and drops the blue onto the clouds. */
static int pair_level(int r, int g, int b, int *lo, int *hi)
{
    int y;
    int mx;
    int mn;
    int rr;
    int rg;
    int rb;

    y = (r * 54 + g * 183 + b * 19) >> 8;
    mx = r;
    if (g > mx)
        mx = g;
    if (b > mx)
        mx = b;
    mn = r;
    if (g < mn)
        mn = g;
    if (b < mn)
        mn = b;
    if (mx - mn < 32) {
        *lo = 0;
        *hi = 1;
        return y;
    }
    rr = r - mn;
    rg = g - mn;
    rb = b - mn;
    if (rb < 24 && rr > 40 && rg > 40 && rr * 2 > rg && rg * 2 > rr) {
        *lo = 2;
        *hi = 1;
        return b;
    }
    if (rb >= rr && rb >= rg) {
        *lo = 4;
        *hi = 1;
        return (r + g) / 2;
    }
    if (rg >= rr && rg >= rb) {
        if (y < 36) {
            *lo = 0;
            *hi = 5;
            return y * 255 / 36;
        }
        *lo = 5;
        *hi = 1;
        return (r + b) / 2;
    }
    if (y < 36) {
        *lo = 0;
        *hi = 3;
        return y * 255 / 36;
    }
    *lo = 3;
    *hi = 1;
    return (g + b) / 2;
}

static void spread1(int32_t *row, int w, int x, int e)
{
    if ((unsigned)x >= (unsigned)w)
        return;
    row[x] += e;
}

static void dither6(uint8_t *argb, int w, int h, uint8_t *idx)
{
    int32_t *rows;
    int y;

    rows = aicos_malloc(MEM_CMA, (uint32_t)w * 3U * sizeof(int32_t));
    if (!rows) {
        int n = w * h;
        int i;

        for (i = 0; i < n; i++) {
            const lv_color32_t *c = (const lv_color32_t *)(argb + (uint32_t)i * 4U);
            int r;
            int g;
            int b;
            int lo;
            int hi;
            int level;

            if (c->alpha < 128) {
                r = 255;
                g = 255;
                b = 255;
            } else {
                r = c->red;
                g = c->green;
                b = c->blue;
            }
            tone8(&r, &g, &b);
            level = pair_level(r, g, b, &lo, &hi);
            idx[i] = k_nib[level < 128 ? lo : hi];
        }
        rt_kprintf("epd: dither alloc failed\n");
        return;
    }
    memset(rows, 0, (uint32_t)w * 3U * sizeof(int32_t));
    for (y = 0; y < h; y++) {
        int32_t *cur = rows + ((uint32_t)(y % 3) * (uint32_t)w);
        int x0 = (y & 1) ? (w - 1) : 0;
        int dir = (y & 1) ? -1 : 1;
        int x = x0;

        while (x >= 0 && x < w) {
            const lv_color32_t *c =
                (const lv_color32_t *)(argb + ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4U);
            int r;
            int g;
            int b;
            int lo;
            int hi;
            int level;
            int acc;
            int er;
            int sr = 0;
            int t;

            if (c->alpha < 128) {
                r = 255;
                g = 255;
                b = 255;
            } else {
                r = c->red;
                g = c->green;
                b = c->blue;
            }
            tone8(&r, &g, &b);
            level = pair_level(r, g, b, &lo, &hi);
            acc = level * 16 + cur[x];
            if (acc < 128 * 16) {
                idx[(uint32_t)y * (uint32_t)w + (uint32_t)x] = k_nib[lo];
                er = acc;
            } else {
                idx[(uint32_t)y * (uint32_t)w + (uint32_t)x] = k_nib[hi];
                er = acc - 255 * 16;
            }
            for (t = 0; t < 12; t++) {
                int kdx = (int)k_jv[t * 3];
                int dy = (int)k_jv[t * 3 + 1];
                int wt = (int)k_jv[t * 3 + 2];
                int pe;

                if (kdx == 1 && dy == 0)
                    continue;
                pe = er * wt / 48;
                sr += pe;
                spread1(rows + ((uint32_t)((y + dy) % 3) * (uint32_t)w),
                        w, x + kdx * dir, pe);
            }
            if ((unsigned)(x + dir) < (unsigned)w)
                spread1(cur, w, x + dir, er - sr);
            else
                spread1(rows + ((uint32_t)((y + 1) % 3) * (uint32_t)w),
                        w, x, er - sr);
            x += dir;
        }
        memset(cur, 0, (uint32_t)w * sizeof(int32_t));
    }
    aicos_free(MEM_CMA, rows);
}

static void put_pix(int x, int y, uint8_t nib)
{
    uint32_t i;
    uint8_t *p;

    if (x < 0 || y < 0 || x >= EPD_W || y >= EPD_H)
        return;
    i = (uint32_t)y * (EPD_W / 2) + ((uint32_t)x >> 1);
    p = &s_frame[i];
    if (x & 1)
        *p = (uint8_t)((*p & 0xF0) | nib);
    else
        *p = (uint8_t)((*p & 0x0F) | (nib << 4));
}

/* Center-crop the source so the scaled picture covers lv_w x lv_h. */
static void cover_crop(int sw, int sh, int dw, int dh,
                       int *crop_x, int *crop_y, int *crop_w, int *crop_h)
{
    int cw;
    int ch;

    if (sw < 1)
        sw = 1;
    if (sh < 1)
        sh = 1;
    if ((uint32_t)sw * (uint32_t)dh >= (uint32_t)sh * (uint32_t)dw) {
        ch = sh;
        cw = (int)((uint32_t)sh * (uint32_t)dw / (uint32_t)dh);
    } else {
        cw = sw;
        ch = (int)((uint32_t)sw * (uint32_t)dh / (uint32_t)dw);
    }
    if (cw < 1)
        cw = 1;
    if (ch < 1)
        ch = 1;
    if (cw > sw)
        cw = sw;
    if (ch > sh)
        ch = sh;
    *crop_w = cw;
    *crop_h = ch;
    *crop_x = (sw - cw) / 2;
    *crop_y = (sh - ch) / 2;
}

static void fit_panel(uint8_t quarter, int *disp_w, int *disp_h, int *lv_w, int *lv_h)
{
    *disp_w = EPD_W;
    *disp_h = EPD_H;
    if (quarter & 1U) {
        *lv_w = EPD_H;
        *lv_h = EPD_W;
    } else {
        *lv_w = EPD_W;
        *lv_h = EPD_H;
    }
}

static int clamp8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return v;
}

/* Catmull-Rom weights, scaled so they add to 256. */
static void cubic_w(int f, int w[4])
{
    int f2 = f * f;
    int f3 = f2 * f;
    int sum;

    w[0] = (-f3 + 2 * f2 * 256 - f * 65536) / 131072;
    w[1] = (3 * f3 - 5 * f2 * 256 + 2 * 256 * 65536) / 131072;
    w[2] = (-3 * f3 + 4 * f2 * 256 + f * 65536) / 131072;
    w[3] = (f3 - f2 * 256) / 131072;
    sum = w[0] + w[1] + w[2] + w[3];
    w[1] += 256 - sum;
}

static int cubic_ch(const uint8_t *src, int sw, int sh, int stride,
                    int x_fp, int y_fp, int ch)
{
    int wx[4];
    int wy[4];
    int ix = x_fp >> 8;
    int iy = y_fp >> 8;
    int64_t acc = 0;
    int yy;

    cubic_w(x_fp & 255, wx);
    cubic_w(y_fp & 255, wy);
    for (yy = 0; yy < 4; yy++) {
        int sy = iy - 1 + yy;
        int row = 0;
        int xx;

        if (sy < 0)
            sy = 0;
        if (sy >= sh)
            sy = sh - 1;
        for (xx = 0; xx < 4; xx++) {
            int sx = ix - 1 + xx;

            if (sx < 0)
                sx = 0;
            if (sx >= sw)
                sx = sw - 1;
            row += (int)src[(uint32_t)sy * (uint32_t)stride +
                            (uint32_t)sx * 3U + (uint32_t)ch] * wx[xx];
        }
        acc += (int64_t)row * wy[yy];
    }
    return clamp8((int)(acc / 65536));
}

/* BGR crop in, BGRA out. Minify by averaging each destination pixel's
 * source block. Magnify with Catmull-Rom so edges stay narrow. */
static const char *scale_bgr(const uint8_t *src, int sw, int sh, int stride,
                             uint8_t *dst, int dw, int dh)
{
    int y;

    if (sw == dw && sh == dh) {
        for (y = 0; y < dh; y++) {
            int x;

            for (x = 0; x < dw; x++) {
                const uint8_t *s = src + ((uint32_t)y * (uint32_t)stride +
                                          (uint32_t)x * 3U);
                uint8_t *d = dst + ((uint32_t)y * (uint32_t)dw + (uint32_t)x) * 4U;

                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = 255;
            }
        }
        return "copy";
    }
    if (sw >= dw && sh >= dh) {
        for (y = 0; y < dh; y++) {
            int y0 = y * sh / dh;
            int y1 = (y + 1) * sh / dh;
            int x;

            if (y1 <= y0)
                y1 = y0 + 1;
            if (y1 > sh)
                y1 = sh;
            for (x = 0; x < dw; x++) {
                int x0 = x * sw / dw;
                int x1 = (x + 1) * sw / dw;
                uint32_t acc0 = 0;
                uint32_t acc1 = 0;
                uint32_t acc2 = 0;
                int n = 0;
                int sy;
                uint8_t *d;

                if (x1 <= x0)
                    x1 = x0 + 1;
                if (x1 > sw)
                    x1 = sw;
                for (sy = y0; sy < y1; sy++) {
                    const uint8_t *row = src + (uint32_t)sy * (uint32_t)stride +
                                         (uint32_t)x0 * 3U;
                    int sx;

                    for (sx = x0; sx < x1; sx++) {
                        acc0 += row[0];
                        acc1 += row[1];
                        acc2 += row[2];
                        row += 3;
                        n++;
                    }
                }
                if (n < 1)
                    n = 1;
                d = dst + ((uint32_t)y * (uint32_t)dw + (uint32_t)x) * 4U;
                d[0] = (uint8_t)(acc0 / (uint32_t)n);
                d[1] = (uint8_t)(acc1 / (uint32_t)n);
                d[2] = (uint8_t)(acc2 / (uint32_t)n);
                d[3] = 255;
            }
        }
        return "area";
    }
    for (y = 0; y < dh; y++) {
        int y_fp = (dh > 1) ? y * (sh - 1) * 256 / (dh - 1) : 0;
        int x;

        for (x = 0; x < dw; x++) {
            int x_fp = (dw > 1) ? x * (sw - 1) * 256 / (dw - 1) : 0;
            uint8_t *d = dst + ((uint32_t)y * (uint32_t)dw + (uint32_t)x) * 4U;

            d[0] = (uint8_t)cubic_ch(src, sw, sh, stride, x_fp, y_fp, 0);
            d[1] = (uint8_t)cubic_ch(src, sw, sh, stride, x_fp, y_fp, 1);
            d[2] = (uint8_t)cubic_ch(src, sw, sh, stride, x_fp, y_fp, 2);
            d[3] = 255;
        }
    }
    return "cubic";
}

/* BGR in. Scale a center crop so the picture fills 240x206. */
static void pack_rotated(const uint8_t *rgb, int sw, int sh, uint8_t quarter)
{
    int disp_w;
    int disp_h;
    int lv_w;
    int lv_h;
    int crop_x;
    int crop_y;
    int crop_w;
    int crop_h;
    int ox;
    int oy;
    int dy;
    const uint8_t *src;
    const char *mode;
    uint8_t *argb;
    uint8_t *idx;

    memset(s_frame, 0x11, sizeof(s_frame));
    if (!rgb || sw < 1 || sh < 1)
        return;
    fit_panel(quarter, &disp_w, &disp_h, &lv_w, &lv_h);
    cover_crop(sw, sh, lv_w, lv_h, &crop_x, &crop_y, &crop_w, &crop_h);
    argb = aicos_malloc(MEM_CMA, (uint32_t)lv_w * (uint32_t)lv_h * 4U);
    if (!argb) {
        rt_kprintf("epd: scale alloc failed\n");
        return;
    }
    src = rgb + ((uint32_t)crop_y * (uint32_t)sw + (uint32_t)crop_x) * 3U;
    mode = scale_bgr(src, crop_w, crop_h, sw * 3, argb, lv_w, lv_h);
    rt_kprintf("epd: scale %s %dx%d -> %dx%d\n", mode, crop_w, crop_h, lv_w, lv_h);
    idx = aicos_malloc(MEM_CMA, (uint32_t)lv_w * (uint32_t)lv_h);
    if (!idx) {
        rt_kprintf("epd: dither alloc failed\n");
        aicos_free(MEM_CMA, argb);
        return;
    }
    dither6(argb, lv_w, lv_h, idx);
    rt_kprintf("epd: dither hue %dx%d -> %dx%d\n", sw, sh, disp_w, disp_h);
    ox = (EPD_W - disp_w) / 2;
    oy = (EPD_H - disp_h) / 2;
    for (dy = 0; dy < disp_h; dy++) {
        int dx;

        for (dx = 0; dx < disp_w; dx++) {
            int sx;
            int sy;

            switch (quarter) {
            case 1:
                sx = dy;
                sy = lv_h - 1 - dx;
                break;
            case 2:
                sx = lv_w - 1 - dx;
                sy = lv_h - 1 - dy;
                break;
            case 3:
                sy = dx;
                sx = lv_w - 1 - dy;
                break;
            default:
                sx = dx;
                sy = dy;
                break;
            }
            if (sx < 0)
                sx = 0;
            if (sy < 0)
                sy = 0;
            if (sx >= lv_w)
                sx = lv_w - 1;
            if (sy >= lv_h)
                sy = lv_h - 1;
            put_pix(ox + (disp_w - 1 - dx), oy + dy,
                    idx[(uint32_t)sy * (uint32_t)lv_w + (uint32_t)sx]);
        }
    }
    aicos_free(MEM_CMA, idx);
    aicos_free(MEM_CMA, argb);
}

static void epd_render_png(int fd, uint32_t nbytes, uint8_t quarter)
{
    uint8_t *file;
    uint8_t *rgb = RT_NULL;
    int w = 0;
    int h = 0;
    uint32_t got = 0;

    file = aicos_malloc(MEM_CMA, nbytes);
    if (!file) {
        rt_kprintf("epd: png file alloc failed\n");
        close(fd);
        return;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        aicos_free(MEM_CMA, file);
        close(fd);
        return;
    }
    while (got < nbytes) {
        int rd = read(fd, file + got, nbytes - got);

        if (rd <= 0) {
            rt_kprintf("epd: png read failed\n");
            aicos_free(MEM_CMA, file);
            close(fd);
            return;
        }
        got += (uint32_t)rd;
    }
    close(fd);
    if (epd_png_to_bgr(file, nbytes, EPD_LV_SRC_MAX, &rgb, &w, &h) != 0) {
        aicos_free(MEM_CMA, file);
        return;
    }
    aicos_free(MEM_CMA, file);
    rt_kprintf("epd: show png %s\n",
               (quarter == 1U) ? "90 CW" :
               (quarter == 2U) ? "180" :
               (quarter == 3U) ? "90 CCW" : "0");
    pack_rotated(rgb, w, h, quarter);
    aicos_free(MEM_CMA, rgb);
    epd_show_packed(s_frame);
}

static void epd_usb_render(uint8_t quarter)
{
    jpg_io_t io;
    JDEC jd;
    void *pool;
    uint8_t *rgb = RT_NULL;
    int scale;
    int dw;
    int dh;
    JRESULT rc;
    uint8_t mag[8];
    int n;
    struct stat st;

    memset(&io, 0, sizeof(io));
    io.fd = open(EPD_PATH, O_RDONLY, 0);
    if (io.fd < 0) {
        rt_kprintf("epd: no image\n");
        return;
    }
    n = read(io.fd, mag, 8);
    if (n >= 4 && mag[0] == 0x89 && mag[1] == 'P' && mag[2] == 'N' && mag[3] == 'G') {
        if (fstat(io.fd, &st) != 0 || st.st_size < 24 || st.st_size > EPD_RAM_MAX) {
            rt_kprintf("epd: png size bad\n");
            close(io.fd);
            return;
        }
        epd_render_png(io.fd, (uint32_t)st.st_size, quarter);
        return;
    }
    if (n < 2 || mag[0] != 0xFF || mag[1] != 0xD8) {
        rt_kprintf("epd: %s is not a JPEG or PNG\n", EPD_PATH);
        close(io.fd);
        return;
    }
    if (lseek(io.fd, 0, SEEK_SET) < 0) {
        close(io.fd);
        return;
    }

    pool = rt_malloc(EPD_JPG_POOL);
    if (!pool) {
        rt_kprintf("epd: jpeg pool alloc failed\n");
        close(io.fd);
        return;
    }
    rc = jd_prepare(&jd, jpg_in, pool, EPD_JPG_POOL, &io);
    if (rc != JDR_OK) {
        rt_kprintf("epd: jpeg prepare %d\n", (int)rc);
        goto out;
    }

    scale = 0;
    for (;;) {
        dw = (int)jd.width >> scale;
        dh = (int)jd.height >> scale;
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        if ((uint32_t)dw * (uint32_t)dh * 3U <= EPD_LV_SRC_MAX)
            break;
        if (scale >= 3)
            break;
        scale++;
    }
    if ((uint32_t)dw * (uint32_t)dh * 3U > EPD_LV_SRC_MAX) {
        rt_kprintf("epd: jpeg %dx%d too large\n", jd.width, jd.height);
        goto out;
    }
    rgb = aicos_malloc(MEM_CMA, (uint32_t)dw * (uint32_t)dh * 3U);
    if (!rgb) {
        rt_kprintf("epd: rgb alloc failed\n");
        goto out;
    }
    memset(rgb, 0xFF, (uint32_t)dw * (uint32_t)dh * 3U);
    io.rgb = rgb;
    io.w = dw;
    io.h = dh;
    rc = jd_decomp(&jd, jpg_out, (uint8_t)scale);
    if (rc != JDR_OK) {
        rt_kprintf("epd: jpeg decode %d\n", (int)rc);
        goto out;
    }
    pack_rotated(rgb, dw, dh, quarter);
    rt_kprintf("epd: show jpeg %s %dx%d\n",
               (quarter == 1U) ? "90 CW" :
               (quarter == 2U) ? "180" :
               (quarter == 3U) ? "90 CCW" : "0",
               dw, dh);
    aicos_free(MEM_CMA, rgb);
    rgb = RT_NULL;
    rt_free(pool);
    pool = RT_NULL;
    close(io.fd);
    io.fd = -1;
    epd_show_packed(s_frame);
    return;

out:
    if (rgb)
        aicos_free(MEM_CMA, rgb);
    if (pool)
        rt_free(pool);
    if (io.fd >= 0)
        close(io.fd);
}

uint8_t epd_usb_on_out(const uint8_t *data, uint32_t len)
{
    int op;

    if (!data || !len)
        return 0;
    op = eink_op(data, len);
    if (op == 1) {
        if (s_saving) {
            s_pending_start = 1;
            return 1;
        }
        s_accept = 0;
        s_ram_len = 0;
        s_overflow = 0;
        s_accept = 1;
        if (len > 9U && !tail_is_pad(data, len))
            ram_append(data + 9, len - 9U);
        s_started = 1;
        epd_kick();
        return 1;
    }
    if (op == 2 || op == 3) {
        s_accept = 0;
        s_need_save = 1;
        s_rot = (uint8_t)op;
        epd_kick();
        return 1;
    }
    if (s_accept) {
        ram_append(data, len);
        if (len < EPD_USB_FILL) {
            s_accept = 0;
            s_need_save = 1;
            epd_kick();
        }
        return 1;
    }
    if (s_saving || s_pending_start)
        return 1;
    return 0;
}

#endif
