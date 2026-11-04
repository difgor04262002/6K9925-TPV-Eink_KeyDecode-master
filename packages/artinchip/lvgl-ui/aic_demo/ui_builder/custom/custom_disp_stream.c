/*
 * Copyright (C) 2026, ENE Technology Inc.
 *
 * custom_disp_stream.c — Area4 direct framebuffer streaming (Option C)
 *
 *   Decode → GE2D rotate → FB → PAN_DISPLAY，整套跳過 LVGL 的 image decoder
 *   cache / blend / flush_thread 流程，把 Area4 PNG/JPEG show-to-show interval
 *   從 ~38 ms (LVGL 路徑) 推到 ~13-17 ms (硬體 pipeline 物理下限)。
 *
 *   See custom_disp_stream.h for the design rationale + threading contract.
 *
 * 重要設計決策 (痛苦學到的)：
 *
 *   (1) Persistent decoder：decoder 在 acquire/codec switch 時 create + init
 *       一次，跨 frame 重複使用。release 才 destroy。配合 uvc_player 的標準
 *       streaming pattern。每 frame mpp_decoder_init 會從 CMA 配 60-435KB
 *       bitstream buffer，30 fps 跑兩秒就撕碎 CMA → GE2D 拿不到連續區塊 →
 *       timeout。Persistent decoder 把這個 alloc churn 完全消除。
 *
 *   (2) 大 bitstream buffer：config.bitstream_buffer_size = 1 MB，覆蓋
 *       1280×800 任何 PNG/JPEG。avoid PNG 大圖塞不下。
 *
 *   (3) MPP 自管 frame buffer：不再用 ext_frame_allocator。讓 MPP 在 init
 *       時一次配出 frame ring (extra_frame_num=1 → 2 frame slots)，per-frame
 *       由 frame_manager 內部循環使用。我們透過 get_frame/put_frame 取得
 *       phy_addr 餵 GE2D。零 per-frame CMA 動作。
 *
 *   (4) Codec switch：PNG ↔ JPEG 切換時 destroy + recreate decoder。少見事件。
 */

#include "custom_disp_stream.h"
#include "custom_disp_profile.h"
#include "custom_Image.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "aic_core.h"
#include "aic_time.h"

#include "mpp_decoder.h"
#include "mpp_dec_type.h"
#include "mpp_ge.h"
#include "mpp_fb.h"
#include "mpp_types.h"
#include "artinchip_fb.h"

#include "lv_port_disp.h"
#include "lv_mpp_dec.h"           /* lv_jpeg_decoder_info, lv_png_decoder_info */
#include "lv_draw_ge2d.h"         /* get_ge2d_device */
#include "lv_draw_ge2d_utils.h"   /* lv_fmt_to_mpp_fmt */
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
void lv_spi_release_hold(void);
int lv_spi_get_draw_info(void **buf, uint32_t *width, uint32_t *height,
                         uint32_t *stride, int *mpp_fmt);
int lv_spi_can_present(void);
int lv_spi_try_lock_draw(void);
void lv_spi_unlock_draw(void);
int lv_spi_present(void);
int lv_spi_tx_begin(void **buf);
int lv_spi_tx_queue(uint32_t len, unsigned x, unsigned y, unsigned w, unsigned h);
int lv_spi_tx_wait_idle(void);
void lv_spi_restore_splash(void);
void lv_spi_unstick_ramwr(void);
#endif
#if CUSTOM_DISP_IS_ROUND_PROFILE
void custom_round_on_jpeg_stream(void);
#endif

#ifndef LV_GE2D_FAIL_PATH_BLIT
#define LV_GE2D_FAIL_PATH_BLIT 3
static inline void lv_draw_ge2d_record_fail(int path, const char *stage)
{
    (void)path;
    (void)stage;
}
static inline void lv_draw_ge2d_record_success(void)
{
}
#endif

#ifdef KERNEL_RTTHREAD
#include <rtthread.h>
#define CUSTOM_STREAM_LOG(fmt, ...) rt_kprintf("custom_stream: " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#define CUSTOM_STREAM_LOG(fmt, ...) printf("custom_stream: " fmt "\n", ##__VA_ARGS__)
#endif

/*
 * CUSTOM_STREAM_VERBOSE
 *   0: per-frame log 關掉 (預設)；只印 acquire/release/error/perf-stats。
 *   1: 每張影像印 dec/blit/pan latency。除錯用。
 */
#ifndef CUSTOM_STREAM_VERBOSE
#define CUSTOM_STREAM_VERBOSE 0
#endif

#if CUSTOM_STREAM_VERBOSE
#define CUSTOM_STREAM_VLOG(fmt, ...) CUSTOM_STREAM_LOG(fmt, ##__VA_ARGS__)
#else
#define CUSTOM_STREAM_VLOG(fmt, ...) ((void)0)
#endif

#define CUSTOM_STREAM_PERF_WINDOW 30U

/*
 * 1280×800 ARGB worst case = 1280*800*4 = 4 MB；JPEG q=95 在 1280×800 約
 * 200 KB，PNG 對純色面板可達 450 KB。bitstream buffer 一律配 1 MB，覆蓋。
 */
#define CUSTOM_STREAM_BITSTREAM_BUF_SIZE (1024U * 1024U)

static volatile uint8_t s_initialized;
static int              s_direct_buf_id;
static struct mpp_fb   *s_cached_fb;

/* Persistent decoder + tracked codec. */
static struct mpp_decoder *s_dec;
static enum mpp_codec_type s_dec_codec = (enum mpp_codec_type)-1;
static int                 s_dec_pix_fmt;

/*
 * Intermediate ARGB buffer for 2-step blit (YUV→ARGB CSC step's destination).
 *
 *   D215 GE2D (AIC_VE_DRV_V10) 不支援 YUV src + rotation 同 op，會在 30 張內
 *   timeout 然後鎖死 GE/VE subsystem 32ms+。LVGL widget 渲染 YUV image 的標準
 *   pattern 是 2 步：先 YUV→ARGB CSC (無旋轉) blit 到 draw_buf，再 ARGB→ARGB
 *   rotation blit 到 FB。兩個個別 op 在 V10 都已驗證穩定。
 *
 *   我們仿照這個 pattern，prealloc 一塊 4 MB CMA 當中間 ARGB buffer，
 *   per-frame 重複使用，零 churn。
 */
#define CUSTOM_STREAM_INTER_BUF_SIZE CUSTOM_DISP_IMG_ARGB_BYTES
static void   *s_inter_argb_vaddr;
static uint32_t s_inter_argb_phys;

/* ---- Performance instrumentation ----------------------------------------- */
static uint32_t s_perf_count;
static uint64_t s_perf_window_start_us;
static uint64_t s_perf_dec_acc_us;
static uint64_t s_perf_blit_acc_us;
static uint64_t s_perf_pan_acc_us;
static uint64_t s_perf_getpkt_acc_us;
static uint64_t s_perf_memcpy_acc_us;
static uint64_t s_perf_decode_acc_us;
static uint64_t s_perf_getfrm_acc_us;
static uint32_t s_total_frame_count;
static uint32_t s_slow_log_budget;  /* per-acquire budget for verbose SLOW logs */

#if CUSTOM_DISP_IS_ROUND_PROFILE
/*
 * Decode on a worker so the 10 ms LVGL tick (USB heartbeat / pickup) never
 * blocks inside mpp_decoder_decode(). VE wait is up to 2 s; that used to
 * freeze present() and look like a QSPI hang.
 */
#define CUSTOM_STREAM_JOB_MAX  (48U * 1024U)
static uint8_t *s_job_buf;
static uint32_t s_job_size;
static uint8_t s_job_type;
static volatile uint8_t s_job_busy;
static volatile uint8_t s_need_rebuild;
static aicos_sem_t s_job_sem;
static aicos_thread_t s_job_thread;

static uint8_t custom_stream_show_body(uint8_t img_type, const uint8_t *data,
                                       uint32_t size);
static void custom_stream_job_start(void);
#endif

/*
 * 計算給定 image type + bitstream 的 codec、format。失敗時 return 0。
 * 用 LVGL 既有 helpers 讀 header；面對 PNG color_type / JPEG subsampling
 * 變動都能正確處理。
 */
static uint8_t custom_stream_parse_header(uint8_t img_type, const uint8_t *data, uint32_t size,
                                          enum mpp_codec_type *codec, lv_image_header_t *header)
{
    lv_result_t res;

    memset(header, 0, sizeof(*header));

    if (img_type == CUSTOM_DISP_STREAM_IMG_JPEG) {
        res = lv_jpeg_decoder_info((const char *)data, header, size, false, false);
        if (res != LV_RESULT_OK) {
            return 0U;
        }
        *codec = MPP_CODEC_VIDEO_DECODER_MJPEG;
        return 1U;
    }
    if (img_type == CUSTOM_DISP_STREAM_IMG_PNG) {
        res = lv_png_decoder_info((const char *)data, header, size, false);
        if (res != LV_RESULT_OK) {
            return 0U;
        }
        *codec = MPP_CODEC_VIDEO_DECODER_PNG;
        return 1U;
    }
    return 0U;
}

/*
 * Build / rebuild persistent decoder for a given codec.
 * Old decoder (if any) is destroyed first.
 *
 * 失敗時 s_dec=NULL，return 0。後續 show() 會 fallback。
 */
static uint8_t custom_stream_rebuild_decoder(enum mpp_codec_type codec, int pix_fmt)
{
    if (s_dec) {
        mpp_decoder_destory(s_dec);
        s_dec = NULL;
        s_dec_codec = (enum mpp_codec_type)-1;
        s_dec_pix_fmt = 0;
    }

    s_dec = mpp_decoder_create(codec);
    if (!s_dec) {
        CUSTOM_STREAM_LOG("decoder_create FAILED codec=%d", (int)codec);
        return 0U;
    }

    struct decode_config config;
    memset(&config, 0, sizeof(config));
    config.bitstream_buffer_size = CUSTOM_STREAM_BITSTREAM_BUF_SIZE;
    config.packet_count = 1;
    /*
     * extra_frame_num=1 → MPP 內部配兩塊 frame buffer (current + previous)，
     * 跨 frame 循環使用。配合 streaming pattern (與 uvc_player 一致)。
     */
    config.extra_frame_num = (codec == MPP_CODEC_VIDEO_DECODER_MJPEG) ? 1 : 0;
    config.pix_fmt = (enum mpp_pixel_format)pix_fmt;

    if (mpp_decoder_init(s_dec, &config) != 0) {
        CUSTOM_STREAM_LOG("decoder_init FAILED codec=%d bitstream=%u",
                          (int)codec, (unsigned)config.bitstream_buffer_size);
        mpp_decoder_destory(s_dec);
        s_dec = NULL;
        return 0U;
    }

    s_dec_codec = codec;
    s_dec_pix_fmt = pix_fmt;
    CUSTOM_STREAM_LOG("decoder ready codec=%d pix_fmt=%d bitstream=%u",
                      (int)codec, pix_fmt, (unsigned)config.bitstream_buffer_size);
    return 1U;
}

static void custom_stream_destroy_decoder(void)
{
    if (s_dec) {
        mpp_decoder_destory(s_dec);
        s_dec = NULL;
        s_dec_codec = (enum mpp_codec_type)-1;
        s_dec_pix_fmt = 0;
    }
}

/*
 * Helper: choose MPP rotation enum from LVGL rotation enum.
 *   LV90 → MPP270, LV270 → MPP90 (LVGL is counter-clockwise, MPP is clockwise).
 */
static inline uint32_t lv_rot_to_mpp_rot(lv_display_rotation_t rotation)
{
    switch (rotation) {
    case LV_DISPLAY_ROTATION_0:   return MPP_ROTATION_0;
    case LV_DISPLAY_ROTATION_90:  return MPP_ROTATION_270;
    case LV_DISPLAY_ROTATION_180: return MPP_ROTATION_180;
    case LV_DISPLAY_ROTATION_270:
    default:                      return MPP_ROTATION_90;
    }
}

/*
 * ---- GE2D streaming failure telemetry ----
 *
 * Streaming path shares ge2d_dev with the LVGL GE2D worker thread and with
 * lv_draw_ge2d_rotate (widget path). D215 V10 GE2D can ge_timeout (2 sec)
 * after long runs of back-to-back full-frame rotation — see §6.10 / §6.11 of
 * DDR_圖片顯示記憶體使用說明.md.
 *
 * Failure here is graceful — caller falls back to the LVGL widget path (or, if
 * that also fails, the panel keeps the last good frame). The counter exists so
 * we can SEE the 2 s freezes happening instead of treating them as a hang.
 *
 * Streak threshold matches lv_draw_ge2d.c (5 consecutive failures = strong
 * warning). System keeps running regardless.
 */
#define STREAM_GE_FAIL_STREAK_ALERT 5U

static volatile uint32_t s_stream_blt_fail_total;
static volatile uint32_t s_stream_blt_fail_streak;

uint32_t custom_disp_stream_get_blt_fail_total(void)
{
    return s_stream_blt_fail_total;
}

static void stream_blt_record_fail(const char *stage, int err)
{
    s_stream_blt_fail_total++;
    s_stream_blt_fail_streak++;
    rt_kprintf("[stream] ge2d %s fail ret=%d (total=%u streak=%u) — falling back to widget path\n",
               stage, err,
               (unsigned)s_stream_blt_fail_total,
               (unsigned)s_stream_blt_fail_streak);
    if (s_stream_blt_fail_streak == STREAM_GE_FAIL_STREAK_ALERT) {
        rt_kprintf("[stream] !!! ge2d failed %u times in a row — HW likely stuck. "
                   "System keeps running, restart device to recover full fps. !!!\n",
                   (unsigned)STREAM_GE_FAIL_STREAK_ALERT);
    }
    /* Also feed the shared GE2D telemetry so the watchdog heartbeat reports
     * streaming-side timeouts. Tagged as BLIT because the streaming path
     * is a plain bitblt op (no rotate / no transform). */
    lv_draw_ge2d_record_fail(LV_GE2D_FAIL_PATH_BLIT, stage);
}

/*
 * Single GE2D bitblt op (build → emit → sync). Used for both steps.
 */
static int custom_stream_run_blt(struct mpp_ge *ge, struct ge_bitblt *blt)
{
    int ret = mpp_ge_bitblt(ge, blt);
    if (ret < 0) {
        stream_blt_record_fail("bitblt", ret);
        return ret;
    }
    ret = mpp_ge_emit(ge);
    if (ret < 0) {
        stream_blt_record_fail("emit", ret);
        return ret;
    }
    ret = mpp_ge_sync(ge);
    if (ret < 0) {
        stream_blt_record_fail("sync", ret);
        return ret;
    }

    /* success — clear streak so we know HW recovered */
    if (s_stream_blt_fail_streak) {
        rt_kprintf("[stream] ge2d recovered after %u failure(s) (total=%u)\n",
                   (unsigned)s_stream_blt_fail_streak,
                   (unsigned)s_stream_blt_fail_total);
        s_stream_blt_fail_streak = 0;
    }
    /* Mirror into the shared streak counter too. */
    lv_draw_ge2d_record_success();
    return 0;
}

/*
 * 1-step GE2D bitblt：src → FB (CSC + rotation 同 op，如果 src 是 YUV)
 *
 *   重要：D215 是 AIC_VE_DRV_V10 + AIC_GE_DRV_V10。試過：
 *     - 1-step YUV+rotation：跑 ~30 frames OK，第 31 張 ge timeout 後鎖死
 *     - 2-step (CSC step1 → rotation step2)：f=2 開始每 op 32ms，更糟
 *
 *   結論：V10 GE2D HW 對「back-to-back full-frame」非常敏感。1-step 雖然 30
 *   張後會掛，但前 30 張至少能跑滿 41 fps；2-step 從 f=2 就 8 fps。1-step 是
 *   兩害取其輕。
 *
 *   補上 LVGL lv_draw_ge2d_blit 對 YUV image 用的標準設定：
 *     - crop_en=1 (src + dst 全圖 crop)
 *     - alpha_en=0, src_alpha_mode=2, src_global_alpha=255 (no blending)
 *   這些是 LVGL widget render YUV image 的 path，已長期穩定。我們之前的
 *   1-step 沒設這些，可能就是 30 frame timeout 的近因。
 *
 *   stale ARGB intermediate buffer 留著但不用 (2-step path 已 disable)，避免
 *   每次 acquire 都重新 alloc 4 MB CMA。
 */
static int custom_stream_blit_to_fb(struct mpp_buf *src, aic_disp_t *aic_disp, int fb_idx,
                                    lv_display_rotation_t rotation)
{
    struct mpp_ge *ge = get_ge2d_device();
    if (!ge) {
        return -1;
    }
    (void)s_inter_argb_phys;  /* keep buffer alive for future revisit */

    struct ge_bitblt blt;
    memset(&blt, 0, sizeof(blt));
    blt.ctrl.flags            = lv_rot_to_mpp_rot(rotation);
    blt.ctrl.alpha_en         = 0;
    blt.ctrl.src_alpha_mode   = 2;
    blt.ctrl.src_global_alpha = 255;

    /* src — 含所有 plane addr / stride / format (decoded YUV 或 ARGB) */
    memcpy(&blt.src_buf, src, sizeof(struct mpp_buf));
    blt.src_buf.buf_type    = MPP_PHY_ADDR;
    blt.src_buf.crop_en     = 1;
    blt.src_buf.crop.x      = 0;
    blt.src_buf.crop.y      = 0;
    blt.src_buf.crop.width  = src->size.width;
    blt.src_buf.crop.height = src->size.height;

    /* dst — panel-native FB, rotation makes geometry swap */
    int dst_w, dst_h;
#if CUSTOM_DISP_IS_ROUND_PROFILE
    /* Same MJPEG+GE2D path; scale decoded JPEG/PNG into 360x360. */
    dst_w = (int)aic_disp->info.width;
    dst_h = (int)aic_disp->info.height;
#else
    if (rotation == LV_DISPLAY_ROTATION_90 || rotation == LV_DISPLAY_ROTATION_270) {
        dst_w = (int)src->size.height;
        dst_h = (int)src->size.width;
    } else {
        dst_w = (int)src->size.width;
        dst_h = (int)src->size.height;
    }
#endif

    blt.dst_buf.buf_type    = MPP_PHY_ADDR;
    blt.dst_buf.phy_addr[0] = (uint32_t)(ulong)(aic_disp->info.framebuffer + fb_idx * aic_disp->fb_size);
    blt.dst_buf.stride[0]   = aic_disp->info.stride;
    blt.dst_buf.format      = aic_disp->info.format;
    blt.dst_buf.size.width  = dst_w;
    blt.dst_buf.size.height = dst_h;
    blt.dst_buf.crop_en     = 1;
    blt.dst_buf.crop.x      = 0;
    blt.dst_buf.crop.y      = 0;
    blt.dst_buf.crop.width  = dst_w;
    blt.dst_buf.crop.height = dst_h;

    return custom_stream_run_blt(ge, &blt);
}

#if CUSTOM_DISP_IS_ROUND_PROFILE
/* JPEG is already 360x360 RGB565 (VE pads to 368). Straight CPU crop-copy. */
static int custom_stream_blit_to_spi(struct mpp_buf *src)
{
    void *dst = NULL;
    uint32_t dst_w = 0, dst_h = 0, dst_stride = 0;
    int dst_fmt = 0;
    const u8 *s;
    u8 *d;
    uint32_t src_stride, copy_w, copy_h, y;
    u16 mid_pix = 0;

    if (lv_spi_get_draw_info(&dst, &dst_w, &dst_h, &dst_stride, &dst_fmt) < 0 || !dst)
        return -1;
    (void)dst_fmt;
    if (!src->phy_addr[0] || src->stride[0] == 0)
        return -1;

    s = (const u8 *)(uintptr_t)src->phy_addr[0];
    d = (u8 *)dst;
    src_stride = src->stride[0];
    copy_w = src->size.width < (int)dst_w ? (uint32_t)src->size.width : dst_w;
    copy_h = src->size.height < (int)dst_h ? (uint32_t)src->size.height : dst_h;

    for (y = 0; y < copy_h; y++)
        memcpy(d + y * dst_stride, s + y * src_stride, (size_t)copy_w * 2U);
    aicos_dcache_clean_range(d,
                             (u32)ALIGN_UP((unsigned long)dst_stride * dst_h,
                                           CACHE_LINE_SIZE));

    {
        const u8 *mid = d + (dst_h / 2U) * dst_stride + (dst_w / 2U) * 2U;

        mid_pix = (u16)mid[0] | ((u16)mid[1] << 8);
    }
    if (s_total_frame_count < 3U)
        CUSTOM_STREAM_LOG("cpu copy %ux%u stride=%u mid=0x%04X",
                          (unsigned)copy_w, (unsigned)copy_h,
                          (unsigned)src_stride, mid_pix);
    return 0;
}

/* 8x8, bit 7 is the left pixel. Glyphs for "Upgrade....". */
static const char s_upg_chars[] = ".Uadegpr";
static const uint8_t s_upg_font[][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18 }, /* . */
    { 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00 }, /* U */
    { 0x00, 0x00, 0x3C, 0x06, 0x3E, 0x66, 0x3E, 0x00 }, /* a */
    { 0x06, 0x06, 0x3E, 0x66, 0x66, 0x66, 0x3E, 0x00 }, /* d */
    { 0x00, 0x00, 0x3C, 0x66, 0x7E, 0x60, 0x3C, 0x00 }, /* e */
    { 0x00, 0x00, 0x3E, 0x66, 0x66, 0x3E, 0x06, 0x3C }, /* g */
    { 0x00, 0x00, 0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60 }, /* p */
    { 0x00, 0x00, 0x6C, 0x76, 0x60, 0x60, 0x60, 0x00 }, /* r */
};

static const uint8_t *upg_glyph(char ch)
{
    const char *p = s_upg_chars;

    while (*p) {
        if (*p == ch)
            return s_upg_font[(unsigned)(p - s_upg_chars)];
        p++;
    }
    return s_upg_font[0];
}

static void upg_fill(uint8_t *dst, uint32_t dst_w, uint32_t dst_h, uint32_t stride,
                     int x, int y, int n, uint16_t color)
{
    int iy, ix;

    for (iy = 0; iy < n; iy++) {
        int py = y + iy;

        if (py < 0 || (uint32_t)py >= dst_h)
            continue;
        for (ix = 0; ix < n; ix++) {
            int px = x + ix;
            uint16_t *pix;

            if (px < 0 || (uint32_t)px >= dst_w)
                continue;
            pix = (uint16_t *)(dst + (uint32_t)py * stride + (uint32_t)px * 2U);
            *pix = color;
        }
    }
}

static void upg_draw_char(uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                          uint32_t stride, int x0, int y0, int scale, char ch)
{
    const uint8_t *g = upg_glyph(ch);
    int row, col;

    for (row = 0; row < 8; row++) {
        uint8_t bits = g[row];

        for (col = 0; col < 8; col++) {
            int sx;
            int sy;

            if ((bits & (uint8_t)(0x80U >> col)) == 0U)
                continue;
            sx = x0 + col * scale;
            sy = y0 + row * scale;
            upg_fill(dst, dst_w, dst_h, stride, sx - 1, sy - 1,
                     scale + 2, 0x0000U);
        }
    }
    for (row = 0; row < 8; row++) {
        uint8_t bits = g[row];

        for (col = 0; col < 8; col++) {
            int sx;
            int sy;

            if ((bits & (uint8_t)(0x80U >> col)) == 0U)
                continue;
            sx = x0 + col * scale;
            sy = y0 + row * scale;
            upg_fill(dst, dst_w, dst_h, stride, sx, sy, scale, 0xFFFFU);
        }
    }
}

static uint8_t s_upgrade_draw = 1U;

void custom_disp_stream_upgrade_draw(uint8_t on)
{
    s_upgrade_draw = on ? 1U : 0U;
}

static void custom_disp_stream_upgrade_overlay(uint8_t *dst, uint32_t dst_w,
                                               uint32_t dst_h, uint32_t stride)
{
    static const char word[] = "Upgrade";
    const int scale = 2;
    const int dots_max = 4;
    int cell;
    int width;
    int x0;
    int y0;
    int i;
    unsigned dots;

    /* Whole transfer, not only the NOR erase. Dots follow the clock so
     * they keep appearing and vanishing on every new frame. */
    if (!s_upgrade_draw || !custom_ui_upgrade_on() || dst_w < 16U ||
        dst_h < 32U || stride < dst_w * 2U)
        return;

    /* Four slots stay reserved so "Upgrade" does not slide.
     * 0 dots, then one more each step, then all gone again. */
    dots = (unsigned)((aic_get_time_us() / 250000ULL) % (uint64_t)(dots_max + 1));
    cell = 8 * scale;
    width = (int)(sizeof(word) - 1U + (unsigned)dots_max) * cell;
    x0 = (int)dst_w / 2 - width / 2;
    y0 = (int)dst_h - cell - (int)(dst_h / 8U);
    if (y0 < 0)
        y0 = 0;

    for (i = 0; word[i] != '\0'; i++)
        upg_draw_char(dst, dst_w, dst_h, stride, x0 + i * cell, y0, scale, word[i]);
    for (i = 0; i < (int)dots; i++) {
        upg_draw_char(dst, dst_w, dst_h, stride,
                      x0 + ((int)(sizeof(word) - 1U) + i) * cell, y0, scale, '.');
    }
}

uint8_t custom_disp_stream_present_argb8888(const uint8_t *px, uint32_t src_w, uint32_t src_h)
{
    void *dst = NULL;
    uint32_t dst_w = 0, dst_h = 0, dst_stride = 0;
    uint32_t fit_w, fit_h, ox, oy, x, y;
    uint8_t *d;
    int queued;

    if (!px || src_w == 0U || src_h == 0U)
        return 0U;
    if (!lv_spi_try_lock_draw())
        return 0U;
    if (lv_spi_get_draw_info(&dst, &dst_w, &dst_h, &dst_stride, NULL) < 0 || !dst) {
        lv_spi_unlock_draw();
        return 0U;
    }
    if (dst_w == 0U || dst_h == 0U || dst_stride < dst_w * 2U) {
        lv_spi_unlock_draw();
        return 0U;
    }

    d = (uint8_t *)dst;
    memset(d, 0, (size_t)dst_stride * dst_h);

    if (src_w * dst_h >= src_h * dst_w) {
        fit_w = dst_w;
        fit_h = (src_h * dst_w) / src_w;
        if (fit_h == 0U)
            fit_h = 1U;
        if (fit_h > dst_h)
            fit_h = dst_h;
    } else {
        fit_h = dst_h;
        fit_w = (src_w * dst_h) / src_h;
        if (fit_w == 0U)
            fit_w = 1U;
        if (fit_w > dst_w)
            fit_w = dst_w;
    }
    ox = (dst_w - fit_w) / 2U;
    oy = (dst_h - fit_h) / 2U;

    for (y = 0U; y < fit_h; y++) {
        uint32_t sy = (y * src_h) / fit_h;
        uint16_t *row = (uint16_t *)(d + (oy + y) * dst_stride + ox * 2U);
        const uint8_t *src_row = px + sy * src_w * 4U;

        for (x = 0U; x < fit_w; x++) {
            uint32_t sx = (x * src_w) / fit_w;
            const uint8_t *p = src_row + sx * 4U;
            uint8_t b = p[0];
            uint8_t g = p[1];
            uint8_t r = p[2];

            row[x] = (uint16_t)(((r & 0xF8U) << 8) | ((g & 0xFCU) << 3) | (b >> 3));
        }
    }

    custom_disp_stream_upgrade_overlay(d, dst_w, dst_h, dst_stride);
    aicos_dcache_clean_range(d,
                             (u32)ALIGN_UP((unsigned long)dst_stride * dst_h,
                                           CACHE_LINE_SIZE));
    queued = lv_spi_present();
    if (queued <= 0) {
        lv_spi_unlock_draw();
        return 0U;
    }
    {
        static uint8_t s_gif_present_log;

        if (!s_gif_present_log) {
            s_gif_present_log = 1U;
            CUSTOM_STREAM_LOG("gif present %ux%u -> %ux%u fit=%ux%u",
                              (unsigned)src_w, (unsigned)src_h,
                              (unsigned)dst_w, (unsigned)dst_h,
                              (unsigned)fit_w, (unsigned)fit_h);
        }
    }
    return 1U;
}

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
static uint32_t s_rgb666_len;
static uint32_t s_rgb666_x, s_rgb666_y, s_rgb666_w, s_rgb666_h;
static uint8_t *s_rgb666_dst;
static uint16_t s_sx_map[480];
static uint32_t s_sx_src, s_sx_fit;

static void zc_sx_prepare(uint32_t src_w, uint32_t fit_w)
{
    uint32_t x;
    uint32_t xstep;
    uint32_t xacc;

    if (fit_w > 480U)
        fit_w = 480U;
    if (s_sx_src == src_w && s_sx_fit == fit_w && fit_w != 0U)
        return;
    xstep = fit_w ? (src_w << 16) / fit_w : 1U;
    if (xstep == 0U)
        xstep = 1U;
    xacc = 0U;
    for (x = 0U; x < fit_w; x++) {
        uint32_t sx = xacc >> 16;

        xacc += xstep;
        if (sx >= src_w)
            sx = src_w - 1U;
        s_sx_map[x] = (uint16_t)sx;
    }
    s_sx_src = src_w;
    s_sx_fit = fit_w;
}

/* Panel D0 is PE.6 (IO1). Swap IO0/IO1 while the pixel is stored. */
static uint8_t zc_wire_byte(uint8_t b)
{
    return (uint8_t)(((b & 0x10U) << 1) | ((b & 0x20U) >> 1) |
                     ((b & 0x01U) << 1) | ((b & 0x02U) >> 1) |
                     (b & 0xCCU));
}

static uint8_t s_zc_wire_lut[64];
static uint8_t s_zc_wire_lut_ok;

static void zc_wire_lut_init(void)
{
    unsigned i;

    if (s_zc_wire_lut_ok)
        return;
    for (i = 0U; i < 64U; i++)
        s_zc_wire_lut[i] = zc_wire_byte((uint8_t)(i << 2));
    s_zc_wire_lut_ok = 1U;
}

static void zc_rgb666_pix(uint8_t *out, const uint8_t *p);

/* One divide per axis. Repeated source rows are copied, not resampled.
 * Nearest-neighbor letterbox, same pixels as the per-pixel loop. */
static void zc_scale_contain(uint8_t *dst, uint32_t stride_px,
                             uint32_t ox, uint32_t oy,
                             uint32_t fit_w, uint32_t fit_h,
                             const uint8_t *px, uint32_t src_w, uint32_t src_h)
{
    uint32_t y;
    uint32_t prev_sy = 0xffffffffU;
    uint8_t *prev_out = NULL;
    uint32_t ystep = (src_h << 16) / fit_h;

    zc_wire_lut_init();
    zc_sx_prepare(src_w, fit_w);
    if (ystep == 0U)
        ystep = 1U;
    for (y = 0U; y < fit_h; y++) {
        uint32_t sy = (y * ystep) >> 16;
        const uint8_t *srow;
        uint8_t *out;
        uint32_t x;

        if (sy >= src_h)
            sy = src_h - 1U;
        out = dst + ((size_t)(oy + y) * stride_px + ox) * 3U;
        if (sy == prev_sy && prev_out) {
            memcpy(out, prev_out, (size_t)fit_w * 3U);
            continue;
        }
        srow = px + (size_t)sy * src_w * 4U;
        {
            uint8_t *d = out;

            for (x = 0U; x < fit_w; x++) {
                const uint8_t *p = srow + (size_t)s_sx_map[x] * 4U;

                *d++ = s_zc_wire_lut[p[2] >> 2];
                *d++ = s_zc_wire_lut[p[1] >> 2];
                *d++ = s_zc_wire_lut[p[0] >> 2];
            }
        }
        prev_sy = sy;
        prev_out = out;
    }
}

/* Index plane plus the GIF palette. Same letterbox as zc_scale_contain,
 * without the BGRA canvas in between. */
static void zc_scale_contain_index(uint8_t *dst, uint32_t stride_px,
                                   uint32_t ox, uint32_t oy,
                                   uint32_t fit_w, uint32_t fit_h,
                                   const uint8_t *index, uint32_t src_w,
                                   uint32_t src_h, const uint8_t *rgb)
{
    uint8_t pal[256 * 3];
    uint32_t i;
    uint32_t y;
    uint32_t prev_sy = 0xffffffffU;
    uint8_t *prev_out = NULL;
    uint32_t ystep = (src_h << 16) / fit_h;

    zc_wire_lut_init();
    zc_sx_prepare(src_w, fit_w);
    for (i = 0U; i < 256U; i++) {
        const uint8_t *c = rgb + i * 3U;

        pal[i * 3U] = s_zc_wire_lut[c[0] >> 2];
        pal[i * 3U + 1U] = s_zc_wire_lut[c[1] >> 2];
        pal[i * 3U + 2U] = s_zc_wire_lut[c[2] >> 2];
    }
    if (ystep == 0U)
        ystep = 1U;
    for (y = 0U; y < fit_h; y++) {
        uint32_t sy = (y * ystep) >> 16;
        const uint8_t *srow;
        uint8_t *out;
        uint32_t x;
        uint8_t *d;

        if (sy >= src_h)
            sy = src_h - 1U;
        out = dst + ((size_t)(oy + y) * stride_px + ox) * 3U;
        if (sy == prev_sy && prev_out) {
            memcpy(out, prev_out, (size_t)fit_w * 3U);
            continue;
        }
        srow = index + (size_t)sy * src_w;
        d = out;
        for (x = 0U; x < fit_w; x++) {
            const uint8_t *c = pal + (uint32_t)srow[s_sx_map[x]] * 3U;

            *d++ = c[0];
            *d++ = c[1];
            *d++ = c[2];
        }
        prev_sy = sy;
        prev_out = out;
    }
}

static void zc_rgb666_pix(uint8_t *out, const uint8_t *p)
{
    /* gifdec canvas is B,G,R,A. Stream path keeps pixel_invert off (same as JPEG). */
    zc_wire_lut_init();
    out[0] = s_zc_wire_lut[p[2] >> 2];
    out[1] = s_zc_wire_lut[p[1] >> 2];
    out[2] = s_zc_wire_lut[p[0] >> 2];
}

uint8_t custom_disp_stream_fill_argb8888_direct(const uint8_t *px, uint32_t src_w,
                                                uint32_t src_h, uint8_t full_frame)
{
    void *unused = NULL;
    uint8_t *dst;
    uint32_t dst_w = 0, dst_h = 0, dst_stride = 0;
    uint32_t fit_w, fit_h, ox, oy;
    uint32_t x0, y0, ww, hh, len;

    s_rgb666_len = 0U;
    s_rgb666_dst = NULL;
    if (!px || src_w == 0U || src_h == 0U)
        return 0U;
    lv_spi_release_hold();
    if (lv_spi_get_draw_info(&unused, &dst_w, &dst_h, &dst_stride, NULL) < 0 ||
        dst_w == 0U || dst_h == 0U)
        return 0U;

    /* Whole canvas, scaled up to the largest size that still fits in 360x360.
     * 1:1 left a small picture in the middle. Cover-scale cut the sides off. */
    if (src_w * dst_h >= src_h * dst_w) {
        fit_w = dst_w;
        fit_h = (src_h * dst_w) / src_w;
        if (fit_h == 0U)
            fit_h = 1U;
        if (fit_h > dst_h)
            fit_h = dst_h;
    } else {
        fit_h = dst_h;
        fit_w = (src_w * dst_h) / src_h;
        if (fit_w == 0U)
            fit_w = 1U;
        if (fit_w > dst_w)
            fit_w = dst_w;
    }
    ox = (dst_w - fit_w) / 2U;
    oy = (dst_h - fit_h) / 2U;

    if (full_frame) {
        x0 = 0U;
        y0 = 0U;
        ww = dst_w;
        hh = dst_h;
    } else {
        x0 = ox;
        y0 = oy;
        ww = fit_w;
        hh = fit_h;
    }
    len = ww * hh * 3U;
    if (lv_spi_tx_begin((void **)&dst) != 0 || !dst)
        return 0U;

    if (full_frame) {
        /* Bars only. A picture that already fills 360x360 is written in full. */
        if (fit_w != dst_w || fit_h != dst_h)
            memset(dst, 0x00, len);
        zc_scale_contain(dst, dst_w, ox, oy, fit_w, fit_h, px, src_w, src_h);
    } else {
        /* Picture only. Black bars stay from the first full frame on that glass. */
        zc_scale_contain(dst, ww, 0U, 0U, fit_w, fit_h, px, src_w, src_h);
    }

    s_rgb666_len = len;
    s_rgb666_x = x0;
    s_rgb666_y = y0;
    s_rgb666_w = ww;
    s_rgb666_h = hh;
    s_rgb666_dst = dst;
    return 1U;
}

uint8_t custom_disp_stream_fill_index_scaled(const uint8_t *index, uint32_t src_w,
                                             uint32_t src_h, const uint8_t *rgb,
                                             uint8_t full_frame)
{
    void *unused = NULL;
    uint8_t *dst;
    uint32_t dst_w = 0, dst_h = 0, dst_stride = 0;
    uint32_t fit_w, fit_h, ox, oy;
    uint32_t x0, y0, ww, hh, len;

    s_rgb666_len = 0U;
    s_rgb666_dst = NULL;
    if (!index || !rgb || src_w == 0U || src_h == 0U)
        return 0U;
    lv_spi_release_hold();
    if (lv_spi_get_draw_info(&unused, &dst_w, &dst_h, &dst_stride, NULL) < 0 ||
        dst_w == 0U || dst_h == 0U)
        return 0U;
    if (src_w * dst_h >= src_h * dst_w) {
        fit_w = dst_w;
        fit_h = (src_h * dst_w) / src_w;
        if (fit_h == 0U)
            fit_h = 1U;
        if (fit_h > dst_h)
            fit_h = dst_h;
    } else {
        fit_h = dst_h;
        fit_w = (src_w * dst_h) / src_h;
        if (fit_w == 0U)
            fit_w = 1U;
        if (fit_w > dst_w)
            fit_w = dst_w;
    }
    ox = (dst_w - fit_w) / 2U;
    oy = (dst_h - fit_h) / 2U;
    if (full_frame) {
        x0 = 0U;
        y0 = 0U;
        ww = dst_w;
        hh = dst_h;
    } else {
        x0 = ox;
        y0 = oy;
        ww = fit_w;
        hh = fit_h;
    }
    len = ww * hh * 3U;
    if (lv_spi_tx_begin((void **)&dst) != 0 || !dst)
        return 0U;
    if (full_frame && (fit_w != dst_w || fit_h != dst_h))
        memset(dst, 0x00, len);
    if (full_frame)
        zc_scale_contain_index(dst, dst_w, ox, oy, fit_w, fit_h,
                               index, src_w, src_h, rgb);
    else
        zc_scale_contain_index(dst, ww, 0U, 0U, fit_w, fit_h,
                               index, src_w, src_h, rgb);
    s_rgb666_len = len;
    s_rgb666_x = x0;
    s_rgb666_y = y0;
    s_rgb666_w = ww;
    s_rgb666_h = hh;
    s_rgb666_dst = dst;
    return 1U;
}

static void upg_fill_rgb666(uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                            int x, int y, int n, const uint8_t rgb[3])
{
    int iy, ix;

    for (iy = 0; iy < n; iy++) {
        int py = y + iy;
        uint8_t *row;

        if (py < 0 || (uint32_t)py >= dst_h)
            continue;
        row = dst + (size_t)py * dst_w * 3U;
        for (ix = 0; ix < n; ix++) {
            int px = x + ix;

            if (px < 0 || (uint32_t)px >= dst_w)
                continue;
            row[(uint32_t)px * 3U] = rgb[0];
            row[(uint32_t)px * 3U + 1U] = rgb[1];
            row[(uint32_t)px * 3U + 2U] = rgb[2];
        }
    }
}

static void upg_draw_char_rgb666(uint8_t *dst, uint32_t dst_w, uint32_t dst_h,
                                 int x0, int y0, int scale, char ch,
                                 const uint8_t ink[3], const uint8_t edge[3])
{
    const uint8_t *g = upg_glyph(ch);
    int row, col;

    for (row = 0; row < 8; row++) {
        uint8_t bits = g[row];

        for (col = 0; col < 8; col++) {
            if ((bits & (uint8_t)(0x80U >> col)) == 0U)
                continue;
            upg_fill_rgb666(dst, dst_w, dst_h,
                            x0 + col * scale - 1, y0 + row * scale - 1,
                            scale + 2, edge);
        }
    }
    for (row = 0; row < 8; row++) {
        uint8_t bits = g[row];

        for (col = 0; col < 8; col++) {
            if ((bits & (uint8_t)(0x80U >> col)) == 0U)
                continue;
            upg_fill_rgb666(dst, dst_w, dst_h,
                            x0 + col * scale, y0 + row * scale, scale, ink);
        }
    }
}

void custom_disp_stream_stamp_upgrade(void)
{
    static const char word[] = "Upgrade";
    const int scale = 2;
    const int dots_max = 4;
    uint8_t ink[3];
    uint8_t edge[3];
    int cell;
    int width;
    int x0;
    int y0;
    int i;
    unsigned dots;

    if (!s_rgb666_dst || !s_upgrade_draw || !custom_ui_upgrade_on() ||
        s_rgb666_x != 0U || s_rgb666_y != 0U ||
        s_rgb666_w < 16U || s_rgb666_h < 32U)
        return;
    zc_wire_lut_init();
    ink[0] = ink[1] = ink[2] = s_zc_wire_lut[0x3F];
    edge[0] = edge[1] = edge[2] = 0U;
    dots = (unsigned)((aic_get_time_us() / 250000ULL) % (uint64_t)(dots_max + 1));
    cell = 8 * scale;
    width = (int)(sizeof(word) - 1U + (unsigned)dots_max) * cell;
    x0 = (int)s_rgb666_w / 2 - width / 2;
    y0 = (int)s_rgb666_h - cell - (int)(s_rgb666_h / 8U);
    if (y0 < 0)
        y0 = 0;
    for (i = 0; word[i] != '\0'; i++)
        upg_draw_char_rgb666(s_rgb666_dst, s_rgb666_w, s_rgb666_h,
                             x0 + i * cell, y0, scale, word[i], ink, edge);
    for (i = 0; i < (int)dots; i++)
        upg_draw_char_rgb666(s_rgb666_dst, s_rgb666_w, s_rgb666_h,
                             x0 + ((int)(sizeof(word) - 1U) + i) * cell, y0,
                             scale, '.', ink, edge);
}

uint8_t custom_disp_stream_fill_bgra_cover(const uint8_t *px, uint32_t src_w,
                                           uint32_t src_h)
{
    void *unused = NULL;
    uint8_t *dst;
    uint32_t dst_w = 0, dst_h = 0, dst_stride = 0;
    uint32_t x, y;
    int fit_w;

    s_rgb666_len = 0U;
    if (!px || src_w == 0U || src_h == 0U)
        return 0U;
    lv_spi_release_hold();
    if (lv_spi_get_draw_info(&unused, &dst_w, &dst_h, &dst_stride, NULL) < 0 ||
        dst_w == 0U || dst_h == 0U)
        return 0U;
    if (lv_spi_tx_begin((void **)&dst) != 0 || !dst)
        return 0U;

    /* Same cover as LOADIN: the glass is full, extra source is cropped from the center. */
    fit_w = (dst_w * src_h >= dst_h * src_w) ? 1 : 0;
    for (y = 0U; y < dst_h; y++) {
        uint32_t sy;
        uint8_t *out = dst + (size_t)y * dst_w * 3U;

        if (fit_w) {
            uint32_t vis_h = (src_w * dst_h) / dst_w;
            uint32_t y0;

            if (vis_h == 0U)
                vis_h = 1U;
            y0 = (src_h > vis_h) ? (src_h - vis_h) / 2U : 0U;
            sy = y0 + (y * vis_h) / dst_h;
        } else {
            sy = (y * src_h) / dst_h;
        }
        if (sy >= src_h)
            sy = src_h - 1U;
        for (x = 0U; x < dst_w; x++) {
            uint32_t sx;
            const uint8_t *p;

            if (fit_w) {
                sx = (x * src_w) / dst_w;
            } else {
                uint32_t vis_w = (src_h * dst_w) / dst_h;
                uint32_t x0;

                if (vis_w == 0U)
                    vis_w = 1U;
                x0 = (src_w > vis_w) ? (src_w - vis_w) / 2U : 0U;
                sx = x0 + (x * vis_w) / dst_w;
            }
            if (sx >= src_w)
                sx = src_w - 1U;
            p = px + ((size_t)sy * src_w + sx) * 4U;
            zc_rgb666_pix(out + x * 3U, p);
        }
    }

    s_rgb666_len = dst_w * dst_h * 3U;
    s_rgb666_x = 0U;
    s_rgb666_y = 0U;
    s_rgb666_w = dst_w;
    s_rgb666_h = dst_h;
    return 1U;
}

uint8_t custom_disp_stream_fill_index_direct(const uint8_t *index, uint32_t src_w,
                                             uint32_t src_h, const uint8_t *rgb,
                                             uint8_t full_frame, const uint8_t *bgra,
                                             uint8_t tindex, uint8_t use_tindex)
{
    void *unused = NULL;
    uint8_t *dst;
    uint32_t dst_w = 0, dst_h = 0, dst_stride = 0;
    uint32_t fit_w, fit_h, ox, oy;
    uint32_t x0, y0, ww, hh, len;
    uint32_t x, y;

    if (use_tindex && !bgra)
        use_tindex = 0U;

    s_rgb666_len = 0U;
    if (!index || !rgb || src_w == 0U || src_h == 0U)
        return 0U;
    lv_spi_release_hold();
    if (lv_spi_get_draw_info(&unused, &dst_w, &dst_h, &dst_stride, NULL) < 0 ||
        dst_w == 0U || dst_h == 0U)
        return 0U;

    fit_w = src_w;
    fit_h = src_h;
    if (fit_w > dst_w)
        fit_w = dst_w;
    if (fit_h > dst_h)
        fit_h = dst_h;
    ox = (dst_w - fit_w) / 2U;
    oy = (dst_h - fit_h) / 2U;
    if (full_frame) {
        x0 = 0U;
        y0 = 0U;
        ww = dst_w;
        hh = dst_h;
    } else {
        x0 = ox;
        y0 = oy;
        ww = fit_w;
        hh = fit_h;
    }
    len = ww * hh * 3U;
    if (lv_spi_tx_begin((void **)&dst) != 0 || !dst)
        return 0U;

    if (full_frame)
        memset(dst, 0x00, len);
    for (y = 0U; y < fit_h; y++) {
        const uint8_t *irow = index + y * src_w;
        uint8_t *out = full_frame ? dst + ((oy + y) * dst_w + ox) * 3U
                                  : dst + y * ww * 3U;

        for (x = 0U; x < fit_w; x++) {
            uint8_t idx = irow[x];
            const uint8_t *c;

            if (use_tindex && idx == tindex) {
                c = bgra + (y * src_w + x) * 4U;
                out[x * 3U] = (uint8_t)(c[2] & 0xFCU);
                out[x * 3U + 1U] = (uint8_t)(c[1] & 0xFCU);
                out[x * 3U + 2U] = (uint8_t)(c[0] & 0xFCU);
            } else {
                c = rgb + (uint32_t)idx * 3U;
                out[x * 3U] = (uint8_t)(c[0] & 0xFCU);
                out[x * 3U + 1U] = (uint8_t)(c[1] & 0xFCU);
                out[x * 3U + 2U] = (uint8_t)(c[2] & 0xFCU);
            }
        }
    }

    s_rgb666_len = len;
    s_rgb666_x = x0;
    s_rgb666_y = y0;
    s_rgb666_w = ww;
    s_rgb666_h = hh;
    return 1U;
}

uint8_t custom_disp_stream_queue_rgb666(void)
{
    if (s_rgb666_len == 0U || s_rgb666_w == 0U || s_rgb666_h == 0U)
        return 0U;
    if (lv_spi_tx_queue(s_rgb666_len, s_rgb666_x, s_rgb666_y,
                        s_rgb666_w, s_rgb666_h) != 0)
        return 0U;
    return 1U;
}

static void loadin_cover_rgb565(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                                uint32_t stride, uint8_t *dst,
                                uint32_t dst_w, uint32_t dst_h)
{
    uint32_t x, y;
    int fit_w;

    fit_w = (dst_w * src_h >= dst_h * src_w) ? 1 : 0;
    for (y = 0U; y < dst_h; y++) {
        uint32_t sy;
        uint8_t *out = dst + (size_t)y * dst_w * 3U;

        if (fit_w) {
            uint32_t vis_h = (src_w * dst_h) / dst_w;
            uint32_t y0;

            if (vis_h == 0U)
                vis_h = 1U;
            y0 = (src_h > vis_h) ? (src_h - vis_h) / 2U : 0U;
            sy = y0 + (y * vis_h) / dst_h;
        } else {
            sy = (y * src_h) / dst_h;
        }
        if (sy >= src_h)
            sy = src_h - 1U;
        for (x = 0U; x < dst_w; x++) {
            uint32_t sx;
            const uint8_t *p;
            uint16_t pix;
            uint8_t r, g, b;

            if (fit_w) {
                sx = (x * src_w) / dst_w;
            } else {
                uint32_t vis_w = (src_h * dst_w) / dst_h;
                uint32_t x0;

                if (vis_w == 0U)
                    vis_w = 1U;
                x0 = (src_w > vis_w) ? (src_w - vis_w) / 2U : 0U;
                sx = x0 + (x * vis_w) / dst_w;
            }
            if (sx >= src_w)
                sx = src_w - 1U;
            p = src + (size_t)sy * stride + (size_t)sx * 2U;
            pix = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
            r = (uint8_t)(((pix >> 11) & 0x1fU) << 3);
            g = (uint8_t)(((pix >> 5) & 0x3fU) << 2);
            b = (uint8_t)((pix & 0x1fU) << 3);
            r |= (uint8_t)(r >> 5);
            g |= (uint8_t)(g >> 6);
            b |= (uint8_t)(b >> 5);
            out[x * 3U] = (uint8_t)(r & 0xFCU);
            out[x * 3U + 1U] = (uint8_t)(g & 0xFCU);
            out[x * 3U + 2U] = (uint8_t)(b & 0xFCU);
        }
    }
}

uint8_t custom_disp_stream_build_loadin(const uint8_t *jpeg, uint32_t size,
                                        uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    struct mpp_decoder *dec;
    struct decode_config config;
    struct mpp_packet packet;
    struct mpp_frame frame;
    const uint8_t *src;
    uint32_t sw, sh, stride;
    int ret;

    if (!jpeg || size == 0U || !dst || dst_w == 0U || dst_h == 0U ||
        size > CUSTOM_STREAM_BITSTREAM_BUF_SIZE)
        return 0U;

    dec = mpp_decoder_create(MPP_CODEC_VIDEO_DECODER_MJPEG);
    if (!dec)
        return 0U;
    memset(&config, 0, sizeof(config));
    config.bitstream_buffer_size = CUSTOM_STREAM_BITSTREAM_BUF_SIZE;
    config.packet_count = 1;
    config.extra_frame_num = 1;
    config.pix_fmt = MPP_FMT_RGB_565;
    if (mpp_decoder_init(dec, &config) != 0) {
        mpp_decoder_destory(dec);
        CUSTOM_STREAM_LOG("loadin decoder init fail");
        return 0U;
    }
    memset(&packet, 0, sizeof(packet));
    if (mpp_decoder_get_packet(dec, &packet, size) != 0 || !packet.data) {
        mpp_decoder_destory(dec);
        CUSTOM_STREAM_LOG("loadin get_packet fail");
        return 0U;
    }
    memcpy(packet.data, jpeg, size);
    packet.size = size;
    packet.flag = PACKET_FLAG_EOS;
    if (mpp_decoder_put_packet(dec, &packet) != 0) {
        mpp_decoder_destory(dec);
        CUSTOM_STREAM_LOG("loadin put_packet fail");
        return 0U;
    }
    ret = mpp_decoder_decode(dec);
    if (ret != 0 && ret != DEC_NO_EMPTY_FRAME) {
        mpp_decoder_destory(dec);
        CUSTOM_STREAM_LOG("loadin decode fail %d", ret);
        return 0U;
    }
    memset(&frame, 0, sizeof(frame));
    if (mpp_decoder_get_frame(dec, &frame) != 0) {
        mpp_decoder_destory(dec);
        CUSTOM_STREAM_LOG("loadin get_frame fail");
        return 0U;
    }
    sw = (uint32_t)frame.buf.size.width;
    sh = (uint32_t)frame.buf.size.height;
    if (frame.buf.crop_en && frame.buf.crop.width > 0 && frame.buf.crop.height > 0) {
        sw = (uint32_t)frame.buf.crop.width;
        sh = (uint32_t)frame.buf.crop.height;
    }
    stride = frame.buf.stride[0];
    src = (const uint8_t *)(uintptr_t)frame.buf.phy_addr[0];
    if (src && stride >= sw * 2U && sh > 0U)
        aicos_dcache_invalid_range((void *)src, (u32)ALIGN_UP(stride * sh, CACHE_LINE_SIZE));
    if (!src || sw == 0U || sh == 0U || stride < sw * 2U) {
        mpp_decoder_put_frame(dec, &frame);
        mpp_decoder_destory(dec);
        CUSTOM_STREAM_LOG("loadin bad frame %ux%u stride=%u",
                          (unsigned)sw, (unsigned)sh, (unsigned)stride);
        return 0U;
    }
    loadin_cover_rgb565(src, sw, sh, stride, dst, dst_w, dst_h);
    CUSTOM_STREAM_LOG("loadin %ux%u -> %ux%u",
                      (unsigned)sw, (unsigned)sh, (unsigned)dst_w, (unsigned)dst_h);
    mpp_decoder_put_frame(dec, &frame);
    mpp_decoder_destory(dec);
    return 1U;
}

uint8_t custom_disp_stream_fill_rgb666_full(const uint8_t *rgb, uint32_t w, uint32_t h)
{
    uint8_t *dst;
    uint32_t len;

    s_rgb666_len = 0U;
    if (!rgb || w == 0U || h == 0U)
        return 0U;
    len = w * h * 3U;
    lv_spi_release_hold();
    if (lv_spi_tx_begin((void **)&dst) != 0 || !dst)
        return 0U;
    memcpy(dst, rgb, len);
    s_rgb666_len = len;
    s_rgb666_x = 0U;
    s_rgb666_y = 0U;
    s_rgb666_w = w;
    s_rgb666_h = h;
    return 1U;
}
#endif
#else
uint8_t custom_disp_stream_present_argb8888(const uint8_t *px, uint32_t src_w, uint32_t src_h)
{
    (void)px;
    (void)src_w;
    (void)src_h;
    return 0U;
}

uint8_t custom_disp_stream_fill_argb8888_direct(const uint8_t *px, uint32_t src_w,
                                                uint32_t src_h, uint8_t full_frame)
{
    (void)px;
    (void)src_w;
    (void)src_h;
    (void)full_frame;
    return 0U;
}

uint8_t custom_disp_stream_fill_index_scaled(const uint8_t *index, uint32_t src_w,
                                             uint32_t src_h, const uint8_t *rgb,
                                             uint8_t full_frame)
{
    (void)index;
    (void)src_w;
    (void)src_h;
    (void)rgb;
    (void)full_frame;
    return 0U;
}

void custom_disp_stream_stamp_upgrade(void)
{
}

uint8_t custom_disp_stream_fill_bgra_cover(const uint8_t *px, uint32_t src_w,
                                           uint32_t src_h)
{
    (void)px;
    (void)src_w;
    (void)src_h;
    return 0U;
}

uint8_t custom_disp_stream_fill_index_direct(const uint8_t *index, uint32_t src_w,
                                             uint32_t src_h, const uint8_t *rgb,
                                             uint8_t full_frame, const uint8_t *bgra,
                                             uint8_t tindex, uint8_t use_tindex)
{
    (void)index;
    (void)src_w;
    (void)src_h;
    (void)rgb;
    (void)full_frame;
    (void)bgra;
    (void)tindex;
    (void)use_tindex;
    return 0U;
}

uint8_t custom_disp_stream_queue_rgb666(void)
{
    return 0U;
}

uint8_t custom_disp_stream_build_loadin(const uint8_t *jpeg, uint32_t size,
                                        uint8_t *dst, uint32_t dst_w, uint32_t dst_h)
{
    (void)jpeg;
    (void)size;
    (void)dst;
    (void)dst_w;
    (void)dst_h;
    return 0U;
}

uint8_t custom_disp_stream_fill_rgb666_full(const uint8_t *rgb, uint32_t w, uint32_t h)
{
    (void)rgb;
    (void)w;
    (void)h;
    return 0U;
}
#endif

#if CUSTOM_DISP_IS_ROUND_PROFILE
static void custom_stream_job_thread(void *arg)
{
    (void)arg;
    while (1) {
        aicos_sem_take(s_job_sem, AICOS_WAIT_FOREVER);
        if (!s_job_buf || s_job_size == 0U) {
            s_job_busy = 0U;
            continue;
        }
        if (custom_disp_streaming_mode) {
            uint64_t wait_ms = aic_get_time_ms();

            while (custom_disp_streaming_mode && !lv_spi_can_present()) {
                if (aic_get_time_ms() - wait_ms >= 1000U) {
                    CUSTOM_STREAM_LOG("job: RAMWR stuck, unstick");
                    lv_spi_unstick_ramwr();
                    wait_ms = aic_get_time_ms();
                }
                aicos_msleep(2);
            }
            if (custom_disp_streaming_mode)
                (void)custom_stream_show_body(s_job_type, s_job_buf, s_job_size);
        }
        s_job_busy = 0U;
    }
}

static void custom_stream_job_start(void)
{
    if (s_job_sem)
        return;

    s_job_buf = aicos_malloc(MEM_DEFAULT, CUSTOM_STREAM_JOB_MAX);
    s_job_sem = aicos_sem_create(0);
    if (s_job_buf && s_job_sem)
        s_job_thread = aicos_thread_create("jpgdec", 8192, 21,
                                           custom_stream_job_thread, NULL);
    if (!s_job_buf || !s_job_sem || !s_job_thread)
        CUSTOM_STREAM_LOG("job thread setup incomplete buf=%p sem=%p th=%p",
                          (void *)s_job_buf, (void *)s_job_sem,
                          (void *)s_job_thread);
    else
        CUSTOM_STREAM_LOG("job thread ready (decode off LVGL tick)");
}
#endif

void custom_disp_stream_init(void)
{
    if (s_initialized) {
        return;
    }
    aic_disp_t *aic_disp = custom_disp_get_aic_disp();
#if CUSTOM_DISP_IS_ROUND_PROFILE
    if (!aic_disp || !aic_disp->fb) {
        s_cached_fb = NULL;
        s_direct_buf_id = 0;
        s_initialized = 1U;
        custom_stream_job_start();
        CUSTOM_STREAM_LOG("init ok (B309 QSPI, no aic_disp)");
        return;
    }
#else
    if (!aic_disp || !aic_disp->fb) {
        CUSTOM_STREAM_LOG("init skip, lv_port_disp not ready (aic_disp=%p)", (void *)aic_disp);
        return;
    }
#endif

    s_cached_fb = aic_disp->fb;
    s_direct_buf_id = 0;

    /*
     * Option C streaming 在 D215 (AIC_VE_DRV_V10 + AIC_GE_DRV_V10) 已 disable
     * (custom.c 不再呼叫 acquire/release)；保留 init 跟整套 code 是為了未來
     * 改 PNG-only 路線時可以快速復活。所以這裡 NOT prealloc 4 MB intermediate
     * buffer，省 CMA。如果以後要重新啟用 2-step path，把這段 alloc 加回來即可。
     */

    s_initialized = 1U;
#if CUSTOM_DISP_IS_ROUND_PROFILE
    custom_stream_job_start();
#endif
    CUSTOM_STREAM_VLOG("init ok (option C disabled on V10), fb=%p w=%u h=%u stride=%u fmt=%d",
                      (void *)aic_disp->info.framebuffer,
                      (unsigned)aic_disp->info.width,
                      (unsigned)aic_disp->info.height,
                      (unsigned)aic_disp->info.stride,
                      (int)aic_disp->info.format);
}

uint8_t custom_disp_stream_is_active(void)
{
    return custom_disp_streaming_mode ? 1U : 0U;
}

uint8_t custom_disp_stream_can_show(void)
{
#if CUSTOM_DISP_IS_ROUND_PROFILE
    /* Tick may queue while RAMWR is busy; worker waits for can_present. */
    if (s_job_busy)
        return 0U;
    return 1U;
#else
    return 1U;
#endif
}

void custom_disp_stream_acquire(void)
{
    if (!s_initialized) {
        custom_disp_stream_init();
        if (!s_initialized) {
            CUSTOM_STREAM_LOG("acquire FAILED, init still not ready (lv_port_disp_init not run yet?)");
            return;
        }
    }
    if (custom_disp_streaming_mode) {
        CUSTOM_STREAM_LOG("acquire skip, already active");
        return;
    }
    /* Hold splash until the first JPEG present(). Releasing here flushed
     * an empty buffer (mid=FF) and raced QSPI into WDT. */
    /*
     * Pause LVGL 的 refresh timer。streaming 期間 LVGL UI 完全不用更新，
     * 暫停可以省下：
     *   - 每 LV_DEF_REFR_PERIOD 一次的 widget invalidate 掃描
     *   - 任何衍生的 GE2D lock 競爭 (我們的 bitblt 不會跟 LVGL render 互卡)
     *   - 不必要的 cache flush + flush_thread vsync wait
     * release 後再 resume + invalidate active screen 讓 LVGL 接管。
     */
    lv_timer_t *refr = lv_display_get_refr_timer(NULL);
    if (refr) {
        lv_timer_pause(refr);
    }
    /*
     * 從 LVGL 當前 buf_id 的對立 index 開始寫，避免 single-frame race。
     */
    aic_disp_t *aic_disp = custom_disp_get_aic_disp();
    if (aic_disp) {
        s_direct_buf_id = (aic_disp->buf_id > 0) ? 0 : 1;
    }
    s_perf_count = 0U;
    s_perf_window_start_us = 0U;
    s_perf_dec_acc_us = 0U;
    s_perf_blit_acc_us = 0U;
    s_perf_pan_acc_us = 0U;
    s_perf_getpkt_acc_us = 0U;
    s_perf_memcpy_acc_us = 0U;
    s_perf_decode_acc_us = 0U;
    s_perf_getfrm_acc_us = 0U;
    s_slow_log_budget = 5U;  /* allow up to 5 verbose breakdowns per acquire */
#if CUSTOM_DISP_IS_ROUND_PROFILE
    custom_stream_job_start();
    s_need_rebuild = 0U;
#endif
    custom_disp_streaming_mode = 1U;
    CUSTOM_STREAM_LOG("acquire ok, start_buf_id=%d", s_direct_buf_id);
}

void custom_disp_stream_release(void)
{
    if (!custom_disp_streaming_mode) {
        return;
    }
    custom_disp_streaming_mode = 0U;
#if CUSTOM_DISP_IS_ROUND_PROFILE
    {
        unsigned waited = 0;

        while (s_job_busy && waited < 2500U) {
            aicos_msleep(10);
            waited += 10U;
        }
        if (s_job_busy)
            CUSTOM_STREAM_LOG("release: decoder still busy after %ums", waited);
    }
#endif
    /*
     * Destroy persistent decoder — pairs 1:1 with acquire-side rebuild on
     * next acquire's first show. If we kept it alive across acquire/release
     * cycles, codec/state could go stale and we'd waste 1MB CMA when not
     * streaming.
     */
#if CUSTOM_DISP_IS_ROUND_PROFILE
    if (!s_job_busy)
        custom_stream_destroy_decoder();
    else
        CUSTOM_STREAM_LOG("release: skip destroy, in-flight decode");
#else
    custom_stream_destroy_decoder();
#endif
    /*
     * Resume LVGL refresh timer (paused in acquire) and force-invalidate
     * active screen so the panel transitions cleanly back to LVGL UI.
     */
#if CUSTOM_DISP_IS_ROUND_PROFILE
    lv_spi_restore_splash();
#endif
    {
        lv_timer_t *refr = lv_display_get_refr_timer(NULL);
        if (refr) {
            lv_timer_resume(refr);
        }
    }
#if !CUSTOM_DISP_IS_ROUND_PROFILE
    lv_obj_t *scr = lv_screen_active();
    if (scr) {
        lv_obj_invalidate(scr);
    }
#endif
    CUSTOM_STREAM_LOG("release ok, total_frames=%u", (unsigned)s_total_frame_count);
}

void custom_disp_stream_recover(void)
{
    if (!custom_disp_streaming_mode)
        return;

#if CUSTOM_DISP_IS_ROUND_PROFILE
    /* Never destroy s_dec here: the worker may be inside mpp_decoder_decode.
     * Next show_body() rebuilds after that call returns. */
    s_need_rebuild = 1U;
    CUSTOM_STREAM_LOG("recover: stay in stream, rebuild after in-flight decode");
#else
    CUSTOM_STREAM_LOG("recover: keep decoder, stay in stream mode");
#endif
}

#if CUSTOM_DISP_IS_ROUND_PROFILE
uint8_t custom_disp_stream_show(uint8_t img_type, const uint8_t *data, uint32_t size)
{
    custom_round_on_jpeg_stream();
    if (!custom_disp_streaming_mode || !s_initialized || !data || size == 0U)
        return 0U;

    if (!s_job_sem || !s_job_buf || !s_job_thread)
        return custom_stream_show_body(img_type, data, size);

    if (s_job_busy)
        return 0U;

    if (size > CUSTOM_STREAM_JOB_MAX) {
        CUSTOM_STREAM_LOG("job drop, jpeg %u > %u",
                          (unsigned)size, (unsigned)CUSTOM_STREAM_JOB_MAX);
        return 1U;
    }

    memcpy(s_job_buf, data, size);
    s_job_size = size;
    s_job_type = img_type;
    s_job_busy = 1U;
    aicos_sem_give(s_job_sem);
    return 1U;
}

static uint8_t custom_stream_show_body(uint8_t img_type, const uint8_t *data, uint32_t size)
#else
uint8_t custom_disp_stream_show(uint8_t img_type, const uint8_t *data, uint32_t size)
#endif
{
    if (!custom_disp_streaming_mode) {
        return 0U;
    }
    if (!s_initialized || !data || size == 0U) {
        return 0U;
    }

#if CUSTOM_DISP_IS_ROUND_PROFILE
    /* RAMWR owns PSRAM. Decode/copy during DMA hangs QSPI and trips WDT. */
    {
        static uint64_t busy_since_ms;

        if (!lv_spi_can_present()) {
            uint64_t now = aic_get_time_ms();

            if (!busy_since_ms)
                busy_since_ms = now;
            else if (now - busy_since_ms >= 1000U) {
                CUSTOM_STREAM_LOG("RAMWR stuck %ums, unstick and retry JPEG",
                                  (unsigned)(now - busy_since_ms));
                lv_spi_unstick_ramwr();
                busy_since_ms = now;
            }
            /* 0 = keep the payload; 1 would release it and drop a live USB frame. */
            return 0U;
        }
        busy_since_ms = 0;
    }
#endif

    aic_disp_t *aic_disp = custom_disp_get_aic_disp();
#if !CUSTOM_DISP_IS_ROUND_PROFILE
    if (!aic_disp || !aic_disp->fb) {
        return 0U;
    }
#endif

    /* ---- parse header ---- */
    enum mpp_codec_type codec = MPP_CODEC_VIDEO_DECODER_MJPEG;
    lv_image_header_t header;
    if (!custom_stream_parse_header(img_type, data, size, &codec, &header)) {
        CUSTOM_STREAM_LOG("show fail, parse_header type=%u size=%u", img_type, (unsigned)size);
        return 0U;
    }

    /* Refuse oversize images so we never overrun bitstream_buffer_size. */
    if (size > CUSTOM_STREAM_BITSTREAM_BUF_SIZE) {
        CUSTOM_STREAM_LOG("show fail, bitstream %u > buffer %u",
                          (unsigned)size, (unsigned)CUSTOM_STREAM_BITSTREAM_BUF_SIZE);
        return 0U;
    }

    /*
     * Decoder 輸出格式跟 header.cf 走 (JPEG 自然吐 YUV420P，PNG 吐 ARGB)。
     *
     * 重要：D215 是 AIC_VE_DRV_V10，MJPEG decoder HW 沒有 YUV→ARGB CSC，
     * 強迫設 pix_fmt=ARGB 完全沒用，還是吐 YUV。實測過 frame 5 就 ge timeout。
     *
     * 解法在 custom_stream_blit_to_fb 走 2-step：
     *   step1: YUV src → ARGB 中間 buffer (CSC only, 無 rotation)
     *   step2: ARGB 中間 buffer → ARGB FB (rotation only, 無 CSC)
     * 兩個個別 GE2D op 在 V10 都已驗證穩定 (LVGL widget render 就這樣跑)。
     * 只有「YUV + rotation 同 op」會卡死整個 GE2D 32ms+。
     */
    int wanted_pix = (int)lv_fmt_to_mpp_fmt(header.cf);
#if CUSTOM_DISP_IS_ROUND_PROFILE
    /* VE RGB888→GE2D→RGB565 on this path produced 00 00 00 wire bytes
     * (white once INVON is still on). Ask the JPEG decoder for RGB565. */
    if (codec == MPP_CODEC_VIDEO_DECODER_MJPEG)
        wanted_pix = (int)MPP_FMT_RGB_565;
    if (s_need_rebuild) {
        s_need_rebuild = 0U;
        CUSTOM_STREAM_LOG("rebuild decoder after stall");
        custom_stream_destroy_decoder();
    }
#endif
    if (!s_dec || s_dec_codec != codec || s_dec_pix_fmt != wanted_pix) {
        if (!custom_stream_rebuild_decoder(codec, wanted_pix)) {
            CUSTOM_STREAM_LOG("show fail, rebuild_decoder codec=%d", (int)codec);
            return 0U;
        }
    }

    uint64_t t_dec_start = aic_get_time_us();

    /* ---- feed packet + decode ---- */
    struct mpp_packet packet;
    memset(&packet, 0, sizeof(packet));
    if (mpp_decoder_get_packet(s_dec, &packet, size) != 0 || !packet.data) {
        CUSTOM_STREAM_LOG("get_packet skip size=%u frame=%u",
                          (unsigned)size, (unsigned)s_total_frame_count);
#if CUSTOM_DISP_IS_ROUND_PROFILE
        custom_stream_destroy_decoder();
        return 1U;
#else
        custom_stream_destroy_decoder();
        return 0U;
#endif
    }
    uint64_t t_getpkt_end = aic_get_time_us();
    memcpy(packet.data, data, size);
    packet.size = size;
    packet.flag = PACKET_FLAG_EOS;
    uint64_t t_memcpy_end = aic_get_time_us();
    if (mpp_decoder_put_packet(s_dec, &packet) != 0) {
        CUSTOM_STREAM_LOG("put_packet skip frame=%u",
                          (unsigned)s_total_frame_count);
#if CUSTOM_DISP_IS_ROUND_PROFILE
        custom_stream_destroy_decoder();
        return 1U;
#else
        custom_stream_destroy_decoder();
        return 0U;
#endif
    }

    int dec_ret = mpp_decoder_decode(s_dec);
    uint64_t t_decode_end = aic_get_time_us();
    if ((uint32_t)(t_decode_end - t_memcpy_end) > 100000U)
        CUSTOM_STREAM_LOG("decode slow %uus ret=%d size=%u frame=%u",
                          (unsigned)(t_decode_end - t_memcpy_end),
                          dec_ret, (unsigned)size, (unsigned)s_total_frame_count);
    if (dec_ret != 0 && dec_ret != DEC_NO_EMPTY_FRAME) {
        CUSTOM_STREAM_LOG("decode skip ret=%d size=%u frame=%u",
                          dec_ret, (unsigned)size, (unsigned)s_total_frame_count);
#if CUSTOM_DISP_IS_ROUND_PROFILE
        /* Keep QSPI stream; one bad JPEG must not hand the panel back to LVGL. */
        custom_stream_destroy_decoder();
        (void)custom_stream_rebuild_decoder(codec, wanted_pix);
        return 1U;
#else
        custom_stream_destroy_decoder();
        return 0U;
#endif
    }

    /* ---- get + use frame ---- */
    struct mpp_frame frame;
    memset(&frame, 0, sizeof(frame));
    if (mpp_decoder_get_frame(s_dec, &frame) != 0) {
        CUSTOM_STREAM_LOG("get_frame skip frame=%u",
                          (unsigned)s_total_frame_count);
#if CUSTOM_DISP_IS_ROUND_PROFILE
        custom_stream_destroy_decoder();
        return 1U;
#else
        custom_stream_destroy_decoder();
        return 0U;
#endif
    }
#if CUSTOM_DISP_IS_ROUND_PROFILE
    if (s_total_frame_count < 3U)
        CUSTOM_STREAM_LOG("dec %ux%u fmt=%d",
                          (unsigned)frame.buf.size.width,
                          (unsigned)frame.buf.size.height,
                          (int)frame.buf.format);
#endif
    uint64_t t_dec_end = aic_get_time_us();

    /* ---- GE2D to panel buffer ---- */
    int fb_idx = s_direct_buf_id;
    lv_display_t *disp = lv_display_get_default();
    lv_display_rotation_t rotation = disp ? lv_display_get_rotation(disp) : LV_DISPLAY_ROTATION_0;
    int ret;

#if CUSTOM_DISP_IS_ROUND_PROFILE
    (void)aic_disp;
    (void)fb_idx;
    (void)rotation;
    if (!lv_spi_try_lock_draw()) {
        mpp_decoder_put_frame(s_dec, &frame);
        return 1U;
    }
    ret = custom_stream_blit_to_spi(&frame.buf);
#else
    ret = custom_stream_blit_to_fb(&frame.buf, aic_disp, fb_idx, rotation);
#endif
    uint64_t t_blit_end = aic_get_time_us();

    /* return frame ownership BEFORE next decode; safe because GE2D synced */
    mpp_decoder_put_frame(s_dec, &frame);

    if (ret < 0) {
        CUSTOM_STREAM_LOG("blit skip ret=%d frame=%u",
                          ret, (unsigned)s_total_frame_count);
#if CUSTOM_DISP_IS_ROUND_PROFILE
        lv_spi_unlock_draw();
        return 1U;
#else
        custom_stream_destroy_decoder();
        return 0U;
#endif
    }

#if CUSTOM_DISP_IS_ROUND_PROFILE
    {
        int queued = lv_spi_present();

        if (queued <= 0) {
            lv_spi_unlock_draw();
            CUSTOM_STREAM_LOG("present skip frame=%u",
                              (unsigned)s_total_frame_count);
            return 1U;
        }
        if (queued > 0 &&
            (s_total_frame_count < 5U || (s_total_frame_count % 30U) == 0U))
            CUSTOM_STREAM_LOG("qspi present f=%u jpg=%u",
                              (unsigned)(s_total_frame_count + 1U),
                              (unsigned)size);
    }
    uint64_t t_pan_end = aic_get_time_us();
#else
    /* ---- PAN_DISPLAY ---- */
    int pan_idx = fb_idx;
    mpp_fb_ioctl(s_cached_fb, AICFB_PAN_DISPLAY, &pan_idx);
    uint64_t t_pan_end = aic_get_time_us();

    s_direct_buf_id ^= 1;
#endif

    /* ---- perf accounting ---- */
    uint32_t getpkt_us = (uint32_t)(t_getpkt_end - t_dec_start);
    uint32_t memcpy_us = (uint32_t)(t_memcpy_end - t_getpkt_end);
    uint32_t decode_us = (uint32_t)(t_decode_end - t_memcpy_end);
    uint32_t getfrm_us = (uint32_t)(t_dec_end - t_decode_end);
    uint32_t dec_us = (uint32_t)(t_dec_end - t_dec_start);
    uint32_t blit_us = (uint32_t)(t_blit_end - t_dec_end);
    uint32_t pan_us = (uint32_t)(t_pan_end - t_blit_end);

    s_total_frame_count++;

    /*
     * Per-frame slow breakdown：限額 s_slow_log_budget 張，避免 log spam 把
     * UI thread 拖死。確認哪一階段 > 20 ms：get_packet (mutex contention) /
     * memcpy (CMA bandwidth) / decode (HW wait) / get_frame (frame_manager
     * lock) / blit (GE2D lock 被 LVGL render thread 搶走)。
     */
    if ((dec_us > 20000U || blit_us > 20000U) && s_slow_log_budget > 0U) {
        s_slow_log_budget--;
        CUSTOM_STREAM_LOG("SLOW f=%u dec=%uus[getpkt=%u memcpy=%u decode=%u getfrm=%u] blit=%uus pan=%uus jpgKB=%u",
                          (unsigned)s_total_frame_count,
                          dec_us, getpkt_us, memcpy_us, decode_us, getfrm_us,
                          blit_us, pan_us, (unsigned)(size / 1024U));
    }

    CUSTOM_STREAM_VLOG("show ok type=%u dec=%uus blit=%uus pan=%uus fb_idx=%d",
                       (unsigned)img_type, dec_us, blit_us, pan_us, fb_idx);

    if (s_perf_count == 0U) {
        s_perf_window_start_us = t_dec_start;
    }
    s_perf_dec_acc_us += dec_us;
    s_perf_blit_acc_us += blit_us;
    s_perf_pan_acc_us += pan_us;
    s_perf_getpkt_acc_us += getpkt_us;
    s_perf_memcpy_acc_us += memcpy_us;
    s_perf_decode_acc_us += decode_us;
    s_perf_getfrm_acc_us += getfrm_us;
    s_perf_count++;
    if (s_perf_count >= CUSTOM_STREAM_PERF_WINDOW) {
        uint64_t win_us = (t_pan_end > s_perf_window_start_us)
                          ? (t_pan_end - s_perf_window_start_us) : 0U;
        uint32_t avg_dec = (uint32_t)(s_perf_dec_acc_us / s_perf_count);
        uint32_t avg_blit = (uint32_t)(s_perf_blit_acc_us / s_perf_count);
        uint32_t avg_pan = (uint32_t)(s_perf_pan_acc_us / s_perf_count);
        uint32_t avg_getpkt = (uint32_t)(s_perf_getpkt_acc_us / s_perf_count);
        uint32_t avg_memcpy = (uint32_t)(s_perf_memcpy_acc_us / s_perf_count);
        uint32_t avg_decode = (uint32_t)(s_perf_decode_acc_us / s_perf_count);
        uint32_t avg_getfrm = (uint32_t)(s_perf_getfrm_acc_us / s_perf_count);
        uint32_t fps_x100 = win_us
                            ? (uint32_t)(((uint64_t)s_perf_count * 100000000ULL) / win_us)
                            : 0U;
        CUSTOM_STREAM_LOG("perf: total=%u win=%u in %ums fps=%u.%02u dec=%uus blit=%uus pan=%uus",
                          (unsigned)s_total_frame_count,
                          (unsigned)s_perf_count,
                          (unsigned)(win_us / 1000ULL),
                          (unsigned)(fps_x100 / 100U),
                          (unsigned)(fps_x100 % 100U),
                          (unsigned)avg_dec,
                          (unsigned)avg_blit,
                          (unsigned)avg_pan);
        CUSTOM_STREAM_LOG("perf: dec_breakdown: getpkt=%uus memcpy=%uus decode=%uus getfrm=%uus",
                          avg_getpkt, avg_memcpy, avg_decode, avg_getfrm);
        s_perf_count = 0U;
        s_perf_dec_acc_us = 0U;
        s_perf_blit_acc_us = 0U;
        s_perf_pan_acc_us = 0U;
        s_perf_getpkt_acc_us = 0U;
        s_perf_memcpy_acc_us = 0U;
        s_perf_decode_acc_us = 0U;
        s_perf_getfrm_acc_us = 0U;
    }

    return 1U;
}
