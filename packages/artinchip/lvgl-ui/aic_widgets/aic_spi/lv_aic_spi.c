/*
 * Copyright (C) 2025-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Huahui <huahui.mai@artinchip.com>
 *           Zhengcun <zhengcun.chen@artinchip.com>
 */

#include <stdatomic.h>
#include <rtthread.h>
#include <drivers/watchdog.h>
#include "lv_aic_spi.h"
#include "aic_iopoll.h"
#include "aic_ui.h"
#include "custom_Image.h"

#define TE_TIMEOUT_MS   100

#define ALIGN_8B(x) (((x) + (7)) & ~(7))

/*
 * DIAGNOSTIC (2026-09-03): force a deterministic horizontal color-bar
 * pattern onto the panel instead of the real LVGL/UI-Builder content, to
 * find out whether the diagonal band artifact seen with real content is a
 * transport bug (wrong CASET/RASET window, MADCTL scan direction, byte
 * order, ...) or is simply the actual UI content (e.g. a gradient
 * background) being photographed through curved, reflective glass.
 *
 * Bands are written already in the wire byte order the panel wants (BE,
 * confirmed via the logic analyzer: 18 C3), so this bypasses LVGL, GE2D,
 * and the rgb565 swap entirely -- it only exercises CASET/RASET, MADCTL,
 * and the SPI/QSPI transport. If the 6 bands come out as clean, horizontal,
 * non-slanted stripes in this exact top-to-bottom order (red, green, blue,
 * white, black, yellow), the transport is correct and the earlier
 * "diagonal band" photos were real UI content, not a bug. If the bands are
 * slanted/offset, the slant tells us exactly how many rows the address
 * window is drifting by.
 *
 * Set to 0 to go back to real content once this test has been read.
 *
 * 2026-09-04: re-enabled to test the V1.4 init (now with explicit COLMOD=
 * 0x66 / 18-bit in lv_st77916.c) against the byte-perfect-verified RGB666
 * test-bars path, instead of the real-content lvgl_swap (16-bit) path that
 * was mismatched against the panel's 18-bit format and produced diagonal
 * stripes last test.
 */
#define ZC_DEBUG_TEST_PATTERN 0

#ifdef  LV_SPI_BUS_WIDTH_1
static const struct lv_spi_config spi_configs[] = {
    [0] = {
        .spi_cfg = {
            .bus_name = "spi1",
            .dev_name = "spi1dev",
            .max_hz = 25000000,
            .mode = RT_SPI_MODE_0 | RT_SPI_MSB,
            .bus_width = 1,
            .rs_pin = "PD.11",
        },
        .bl_pin = "PE.12",
        .bl_low_active = false,
        .width = LV_LCD_WIDTH,
        .height = LV_LCD_HEIGHT,
    },
};
#elif defined LV_SPI_BUS_WIDTH_4
extern struct qspi_cfg qspi;
static const struct lv_spi_config spi_configs[] = {
    [0] = {
        .spi_cfg = {
            .bus_name = "qspi1",
            .dev_name = "qspi1dev",
            /*
             * 99MHz (source 200MHz / 2) left the lower half of the round
             * GRAM unwritten: the picture sat in the top of the circle and
             * the bottom stayed black. 50MHz writes all 360 rows.
             * QSPI0 is unchanged.
             */
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
            .max_hz = 50000000,
#else
            .max_hz = 25000000,
#endif
            .mode = RT_SPI_MODE_0 | RT_SPI_MSB,
            .bus_width = 4,
            .qspi_cfg = &qspi,
        },
#if defined(AIC_PANEL_ENABLE_GPIO)
        .bl_pin = AIC_PANEL_ENABLE_GPIO,
#else
        .bl_pin = "PE.12",
#endif
        .bl_low_active = false,
        .width = LV_LCD_WIDTH,
        .height = LV_LCD_HEIGHT,
    },
#if defined(AIC_LVGL_DOUBLE_DISP_DEMO) && defined(AIC_USING_QSPI2)
    [1] = {
        .spi_cfg = {
            .bus_name = "qspi2",
            .dev_name = "qspi2dev",
            .max_hz = 50000000,
            .mode = RT_SPI_MODE_0 | RT_SPI_MSB,
            .bus_width = 4,
            .qspi_cfg = &qspi,
        },
        .bl_pin = "PE.12",
        .bl_low_active = false,
        .width = LV_LCD_WIDTH,
        .height = LV_LCD_HEIGHT,
    },
#endif
};
#endif /* LV_SPI_BUS_WIDTH_4 */

#define LV_SPI_DEV_NUM    ARRAY_SIZE(spi_configs)

/* Array to hold all SPI display devices */
static struct lv_spi_dev **g_spi_devs = NULL;

static struct lv_spi_dev *lv_find_spi_dev(lv_display_t *disp)
{
    int i;

    for (i = 0; i < LV_SPI_DEV_NUM; i++) {
        if (g_spi_devs[i]->disp == disp) {
            return g_spi_devs[i];
        }
    }

    return NULL;
}

void lv_spi_write_buffer(struct lv_spi_dev *spi_dev, unsigned int cmd,
                         unsigned int len, const u8 *data)
{
    aic_spi_lcd_write_cmd(spi_dev->dev, cmd, len, data);
}

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
/* Panel D0 is PE.6 (IO1). Swap IO0/IO1 in the stored byte. */
static u8 zc_wire_d0d1(u8 b)
{
    return (u8)(((b & 0x10) << 1) | ((b & 0x20) >> 1) |
                ((b & 0x01) << 1) | ((b & 0x02) >> 1) |
                (b & 0xCC));
}

/* RGB565 → RGB666. Panel stays INVON (vendor 0x21); invert every pixel. */
static void zc_rgb565_to_rgb666(u16 c, u8 out[3], bool invert)
{
    u8 r5 = (c >> 11) & 0x1f;
    u8 g6 = (c >> 5) & 0x3f;
    u8 b5 = c & 0x1f;
    u8 r = (u8)((r5 << 3) | (r5 >> 2));
    u8 g = (u8)((g6 << 2) | (g6 >> 4));
    u8 b = (u8)((b5 << 3) | (b5 >> 2));

    if (invert) {
        r = (u8)~r;
        g = (u8)~g;
        b = (u8)~b;
    }
    /* RGB666 top 6 bits, then swap IO0/IO1 (panel D0 is PE.6). */
    out[0] = zc_wire_d0d1((u8)(r & 0xFC));
    out[1] = zc_wire_d0d1((u8)(g & 0xFC));
    out[2] = zc_wire_d0d1((u8)(b & 0xFC));
}
#endif

#if defined(AIC_PANEL_DBI_ZC_T1D8BP) && ZC_DEBUG_TEST_PATTERN
/*
 * 6 equal horizontal bands, top to bottom: red, green, blue, white, black,
 * yellow. 18-bit RGB666 wire order (H-I).
 */
static void zc_fill_test_bars(u8 *dst, int32_t width, int32_t height)
{
    static const u16 colors[6] = {
        0xF800, /* red    */
        0x07E0, /* green  */
        0x001F, /* blue   */
        0xFFFF, /* white  */
        0x0000, /* black  */
        0xFFE0, /* yellow */
    };
    int32_t band = height / 6;
    int32_t x, y;

    if (band <= 0)
        band = 1;

    for (y = 0; y < height; y++) {
        int32_t idx = y / band;
        u8 rgb[3];
        u8 *row;

        if (idx > 5)
            idx = 5;
        zc_rgb565_to_rgb666(colors[idx], rgb, true);
        row = dst + (size_t)y * width * 3;
        for (x = 0; x < width; x++) {
            row[x * 3 + 0] = rgb[0];
            row[x * 3 + 1] = rgb[1];
            row[x * 3 + 2] = rgb[2];
        }
    }
}
#endif

static void lv_disp_data_blt(struct lv_spi_dev *spi_dev)
{
    extern enum mpp_pixel_format lv_fmt_to_mpp_fmt(lv_color_format_t cf);
    struct mpp_ge *ge2d_dev = spi_dev->ge2d_dev;
    lv_disp_t *disp = spi_dev->disp;
    uint32_t src_buf = (uint32_t)(ulong)spi_dev->data;
    uint32_t dest_buf = (uint32_t)(ulong)spi_dev->tx_buf;
    int32_t src_width = lv_display_get_horizontal_resolution(disp);
    int32_t src_height = lv_display_get_vertical_resolution(disp);
    lv_color_format_t cf = lv_display_get_color_format(disp);
    int32_t src_stride = lv_draw_buf_width_to_stride(src_width, cf);
    int32_t dst_stride = (int32_t)ALIGN_8B(src_width * 2); // rgb565 bpp = 2;
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    enum mpp_pixel_format fmt = lv_fmt_to_mpp_fmt(cf);

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    /* 16-bit COLMOD is ignored on this QSPI glass. Send RGB666 + INVON invert. */
    if (rotation == LV_DISPLAY_ROTATION_0 && cf == LV_COLOR_FORMAT_RGB565 &&
        spi_dev->data && spi_dev->tx_buf) {
        const u8 *src = (const u8 *)(spi_dev->blt_src ? spi_dev->blt_src
                                                      : spi_dev->data);
        u8 *dst = spi_dev->tx_buf;
        int32_t x, y;
        rt_device_t wdt = rt_device_find("wdt");

        if (spi_dev->hold_released) {
            static unsigned src_log;
            if (src_log < 3U) {
                const u8 *m = src + (src_height / 2) * src_stride +
                              (src_width / 2) * 2;
                u16 p = (u16)m[0] | ((u16)m[1] << 8);

                src_log++;
                pr_info("zc-t1d8bp: src565 center=0x%04X\n", p);
            }
        }

        for (y = 0; y < src_height; y++) {
            const u8 *row = src + y * src_stride;
            u8 *out = dst + y * src_width * 3;
            bool inv = spi_dev->pixel_invert;

            if ((y & 31) == 0 && wdt)
                rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
            for (x = 0; x < src_width; x++) {
                u16 pix = (u16)row[x * 2] | ((u16)row[x * 2 + 1] << 8);
                u8 r = (u8)(((pix >> 11) & 0x1f) << 3);
                u8 g = (u8)(((pix >> 5) & 0x3f) << 2);
                u8 b = (u8)((pix & 0x1f) << 3);

                r |= (u8)(r >> 5);
                g |= (u8)(g >> 6);
                b |= (u8)(b >> 5);
                if (inv) {
                    r = (u8)~r;
                    g = (u8)~g;
                    b = (u8)~b;
                }
                out[x * 3 + 0] = zc_wire_d0d1((u8)(r & 0xFC));
                out[x * 3 + 1] = zc_wire_d0d1((u8)(g & 0xFC));
                out[x * 3 + 2] = zc_wire_d0d1((u8)(b & 0xFC));
            }
        }
        spi_dev->tx_len = (u32)(src_width * src_height * 3);
        aicos_dcache_clean_invalid_range((ulong *)spi_dev->tx_buf,
                                         (ulong)ALIGN_UP(spi_dev->tx_len, CACHE_LINE_SIZE));
        return;
    }
#endif

    struct ge_bitblt blt = { 0 };

    blt.src_buf.buf_type    = MPP_PHY_ADDR;
    blt.src_buf.phy_addr[0] = src_buf;
    blt.src_buf.stride[0]   = src_stride;
    blt.src_buf.size.width  = src_width;
    blt.src_buf.size.height = src_height;
    blt.src_buf.format      = fmt;

    blt.dst_buf.buf_type    = MPP_PHY_ADDR;
    blt.dst_buf.phy_addr[0] = dest_buf;
    blt.dst_buf.stride[0]   = dst_stride;
    blt.dst_buf.format      = MPP_FMT_RGB_565;

    if (rotation == LV_DISPLAY_ROTATION_0 || rotation == LV_DISPLAY_ROTATION_180) {
        blt.dst_buf.size.width  = src_width;
        blt.dst_buf.size.height = src_height;
    } else {
        blt.dst_buf.size.width  = src_height;
        blt.dst_buf.size.height = src_width;
    }
    blt.ctrl.dither_en      = 1;

    switch (rotation) {
    case LV_DISPLAY_ROTATION_0:
        blt.ctrl.flags = MPP_ROTATION_0;
        break;
    case LV_DISPLAY_ROTATION_90:
        /* LV_DISP_ROT_90 means display rotate 90 degrees counterclockwise,
         * so set degree to MPP_ROTATION_270
         */
        blt.ctrl.flags = MPP_ROTATION_270;
        break;
    case LV_DISPLAY_ROTATION_180:
        blt.ctrl.flags = MPP_ROTATION_180;
        break;
    case LV_DISPLAY_ROTATION_270:
        blt.ctrl.flags = MPP_ROTATION_90;
        break;
    default:
        break;
    }

    int ret = mpp_ge_bitblt(ge2d_dev, &blt);
    if (ret < 0) {
        LV_LOG_ERROR("mpp ge bitblt fail\n");
        return;
    }

    ret = mpp_ge_emit(ge2d_dev);
    if (ret < 0) {
        LV_LOG_ERROR("mpp ge emit fail\n");
        return;
    }

    ret = mpp_ge_sync(ge2d_dev);
    if (ret < 0) {
        LV_LOG_ERROR("mpp ge sync fail\n");
        return;
    }

    /* ST77916 QSPI RGB565 expects high-byte-first; LVGL buffer is LE. */
    lv_draw_sw_rgb565_swap(spi_dev->tx_buf, src_width * src_height);
    aicos_dcache_clean_invalid_range((ulong *)spi_dev->tx_buf, (ulong)ALIGN_UP(spi_dev->tx_len, CACHE_LINE_SIZE));
}

static void te_input_irq_handler(void *args)
{
    struct lv_spi_dev *spi_dev = args;

    aicos_wqueue_wakeup(spi_dev->te_queue);
}

static int te_input_pin_cfg(struct lv_spi_dev *spi_dev)
{
    if (spi_dev->te_queue && spi_dev->te_pin) {
        rt_pin_mode(spi_dev->te_pin, PIN_MODE_INPUT_PULLUP);
        rt_pin_attach_irq(spi_dev->te_pin, PIN_IRQ_MODE_RISING_FALLING,
                        te_input_irq_handler, spi_dev);

        rt_pin_irq_enable(spi_dev->te_pin, PIN_IRQ_ENABLE);
    }

    return 0;
}

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
static struct lv_spi_dev *lv_spi_primary(void);

__attribute__((weak)) void lv_spi_on_ramwr_idle(void)
{
}

__attribute__((weak)) void lv_spi_on_display_stall(void)
{
}

static void zc_drain_display_sem(struct lv_spi_dev *spi_dev)
{
    unsigned n = 0;

    while (spi_dev->display_sem && aicos_sem_take(spi_dev->display_sem, 0) == 0) {
        if (++n >= 64U)
            break;
    }
}

/* Leave exactly one present() token. Extra gives let JPEG race an in-flight RAMWR. */
static void zc_leave_one_sync_ready(struct lv_spi_dev *spi_dev)
{
    unsigned n = 0;

    if (!spi_dev->sync_ready)
        return;
    while (aicos_sem_take(spi_dev->sync_ready, 0) == 0) {
        if (++n >= 8U)
            break;
    }
    aicos_sem_give(spi_dev->sync_ready);
}

#ifndef ZC_RAMWR_SETTLE_MS
/* No TE: keep the next 0x2C off the previous scan. RAMWR is ~15.5ms. */
#define ZC_RAMWR_SETTLE_MS 8U
#endif

void lv_spi_unstick_ramwr(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();

    if (!spi)
        return;
    spi->draw_locked = false;
    spi->ramwr_busy = false;
    spi->ramwr_free_tick = lv_tick_get();
    if (spi->sync_ready)
        aicos_sem_give(spi->sync_ready);
}

static void zc_wdt_kick(rt_device_t wdt)
{
    if (wdt)
        rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
}

/* One CS, one 0x2C. Banded windows + yield wedged QSPI after ~300 frames. */
static int zc_ramwr_area(struct lv_spi_dev *spi_dev, u8 *buf, u32 len,
                         unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                         rt_device_t wdt)
{
    int ret;

    if (!buf || !len)
        return -1;
    /* Disp may already have passed its skip check before the NAND
     * program set the flag. Do not start this DMA. */
    if (custom_image_flash_save_busy() || custom_image_spill_pending() ||
        custom_image_signal_busy() || custom_nand_bus_busy())
        return -1;

    zc_wdt_kick(wdt);
    ret = aic_spi_lcd_flush_win(spi_dev->dev, x0, x1, y0, y1, buf, len);
    zc_wdt_kick(wdt);
    /* A second 388 KB DMA after a timeout is what stacked into the
     * watchdog reset in the middle of the next Panel_signal write. */
    if (ret < 0)
        pr_err("zc-t1d8bp: RAMWR fail %d len=%u\n", ret, (unsigned)len);
    return ret;
}

static int zc_ramwr_full(struct lv_spi_dev *spi_dev, u8 *buf, u32 len,
                         rt_device_t wdt)
{
    const struct lv_spi_config *config = spi_dev->config;

    return zc_ramwr_area(spi_dev, buf, len,
                         0, 0, config->width - 1, config->height - 1, wdt);
}
#endif

static void disp_thread(void *arg)
{
    struct lv_spi_dev *spi_dev = arg;
    const struct lv_spi_config *config = spi_dev->config;
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    rt_device_t wdt = rt_device_find("wdt");
    unsigned painted_gen = 0;
    unsigned stale = 0;
#endif

    while (1) {
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
        custom_image_spill_service();
        custom_round_play_service();
        /* A frame queued during the NAND write is still sitting here.
         * Send it before opening or decoding the next one. Not during
         * the write: that RAMWR shares the DMA channel with NAND. */
        if (custom_round_jpeg_streaming())
            spi_dev->tx_direct = 0;
        if (spi_dev->tx_direct &&
            !custom_image_flash_save_busy() && !custom_image_spill_pending() &&
            !custom_image_signal_busy() && !custom_nand_bus_busy())
            goto zc_send_direct;
        /* Saved GIF: the UI timer never reached sync start. Open it here,
         * after the USB callback has returned. */
        if (!custom_image_flash_save_busy() && !custom_image_spill_pending() &&
            !custom_image_signal_busy() && !custom_nand_bus_busy()) {
            custom_round_boot_to_signal();
            custom_round_kick_play();
        }
        /* sync play opened the file, then the GIF timer never queued a
         * RAMWR. Decode and queue the next frame on this thread. */
        if (custom_round_disp_pump()) {
            if (custom_image_flash_save_busy() || custom_image_spill_pending() ||
                custom_image_signal_busy() || custom_nand_bus_busy()) {
                zc_wdt_kick(wdt);
                if (aicos_sem_take(spi_dev->display_sem, 0) == 0 &&
                    spi_dev->sync_ready)
                    aicos_sem_give(spi_dev->sync_ready);
                aicos_msleep(1);
                continue;
            }
            if (spi_dev->tx_direct)
                goto zc_send_direct;
            aicos_msleep(1);
            continue;
        }
        /* NAND and the panel share the DMA engine. A 360x360 RAMWR during
         * the Panel_signal program times out and the glass stays dead
         * until the next power-on. Skip the frame until flash saved. */
        if (custom_image_flash_save_busy() || custom_image_spill_pending() ||
            custom_image_signal_busy() || custom_nand_bus_busy()) {
            zc_wdt_kick(wdt);
            if (aicos_sem_take(spi_dev->display_sem, 0) == 0 &&
                spi_dev->sync_ready)
                aicos_sem_give(spi_dev->sync_ready);
            aicos_msleep(1);
            continue;
        }
        if (spi_dev->hold_test_pattern && !spi_dev->hold_released) {
            zc_wdt_kick(wdt);
            if (aicos_sem_take(spi_dev->display_sem, 200) != 0)
                continue;
            if (spi_dev->hold_test_pattern && !spi_dev->hold_released) {
                aicos_sem_give(spi_dev->sync_ready);
                continue;
            }
        } else if (aicos_sem_take(spi_dev->display_sem, 1000) != 0 &&
                   !spi_dev->tx_direct) {
            static unsigned quiet;

            zc_wdt_kick(wdt);
            if (spi_dev->draw_locked)
                continue;
            /* A direct RGB666 may have been queued without a surviving
             * semaphore (this path used to drain it). Send that frame
             * instead of reprinting the old GRAM forever. */
            if (spi_dev->tx_direct)
                goto zc_send_direct;
            spi_dev->ramwr_busy = false;
            zc_drain_display_sem(spi_dev);
            if (spi_dev->tx_direct)
                goto zc_send_direct;
            /*
             * Do not give display_sem here. That used to look like a new
             * JPEG, then "stale wake" drained a real present() token and
             * the stream never resumed.
             *
             * The panel already holds the last image. Sending it again is
             * a 388 KB DMA on the same channel the GIF file is reading,
             * and that read then never returns.
             */
            zc_leave_one_sync_ready(spi_dev);
            if (spi_dev->tx_direct)
                goto zc_send_direct;
            if ((++quiet % 5U) == 1U)
                pr_info("zc-t1d8bp: no present 1s, hold GRAM gen=%u\n",
                        spi_dev->present_gen);
            lv_spi_on_display_stall();
            continue;
        }
#else
        aicos_sem_take(spi_dev->display_sem, AICOS_WAIT_FOREVER);
#endif

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
zc_send_direct:
        if (spi_dev->tx_direct) {
            uint8_t sel = spi_dev->tx_direct_sel;
            u32 len = spi_dev->tx_direct_len;
            unsigned x0 = spi_dev->ramwr_x;
            unsigned y0 = spi_dev->ramwr_y;
            unsigned ww = spi_dev->ramwr_w;
            unsigned hh = spi_dev->ramwr_h;
            u8 *buf = (sel < 2U) ? spi_dev->tx_pp[sel] : NULL;

            spi_dev->tx_sending_sel = sel;
            spi_dev->tx_direct = 0;
            spi_dev->ramwr_busy = true;
            if (buf && len && ww && hh) {
                spi_dev->tx_buf = buf;
                spi_dev->tx_len = len;
                aicos_dcache_clean_invalid_range((ulong *)buf,
                                                 (ulong)ALIGN_UP(len, CACHE_LINE_SIZE));
                zc_ramwr_area(spi_dev, buf, len, x0, y0,
                              x0 + ww - 1U, y0 + hh - 1U, wdt);
            }
            spi_dev->ramwr_busy = false;
            spi_dev->tx_done_gen++;
            /* Next write is a different TFT. Do not add the same-glass settle. */
            spi_dev->ramwr_free_tick = lv_tick_get();
            if (!spi_dev->bl_en && buf && len && ww && hh) {
                /* First picture is on the glass. Wake onto it, same as the blit path. */
                zc_wdt_kick(wdt);
                aic_spi_lcd_write_cmd(spi_dev->dev, 0x11, 0, RT_NULL);
                rt_thread_mdelay(120);
                zc_wdt_kick(wdt);
                aic_spi_lcd_write_cmd(spi_dev->dev, 0x29, 0, RT_NULL);
                aic_spi_lcd_backlight_gpio_enable(config->bl_pin, config->bl_low_active, true);
                spi_dev->bl_en = true;
                te_input_pin_cfg(spi_dev);
                pr_info("zc-t1d8bp: SLPOUT+DISPON after boot frame\n");
            }
            continue;
        }
        zc_wdt_kick(wdt);
        if (spi_dev->replay_splash && spi_dev->splash_keep) {
            spi_dev->ramwr_busy = true;
            zc_ramwr_full(spi_dev, spi_dev->splash_keep, spi_dev->tx_len, wdt);
            spi_dev->replay_splash = false;
            spi_dev->hold_test_pattern = true;
            spi_dev->hold_released = false;
            spi_dev->pixel_invert = true;
            spi_dev->ramwr_busy = false;
            spi_dev->ramwr_free_tick = lv_tick_get() + ZC_RAMWR_SETTLE_MS;
            aicos_sem_give(spi_dev->sync_ready);
            lv_spi_on_ramwr_idle();
            pr_info("zc-t1d8bp: restored splash\n");
            continue;
        }
        if (spi_dev->hold_released &&
            spi_dev->present_gen == painted_gen) {
            unsigned gen = spi_dev->present_gen;

            /* Leftover display_sem, not a new JPEG. Drain extras, then
             * re-check gen so a concurrent present() is not discarded. */
            zc_drain_display_sem(spi_dev);
            if (spi_dev->present_gen != gen)
                ; /* fall through and RAMWR the new frame */
            else {
                stale++;
                spi_dev->ramwr_busy = false;
                if (stale == 1U)
                    pr_info("zc-t1d8bp: stale wake gen=%u\n", gen);
                lv_spi_on_ramwr_idle();
                continue;
            }
        }
        if (spi_dev->hold_released) {
            painted_gen = spi_dev->present_gen;
            stale = 0;
        }
        spi_dev->blt_src = spi_dev->data;
        if (spi_dev->tx_pp[0] && spi_dev->tx_pp[1]) {
            spi_dev->tx_sel ^= 1U;
            spi_dev->tx_buf = spi_dev->tx_pp[spi_dev->tx_sel];
        }
        spi_dev->ramwr_busy = true;
        /* Drop the LVGL white splash. The panel stays asleep until the
         * first boot frame is written below. */
        if (!spi_dev->hold_released) {
            spi_dev->hold_test_pattern = true;
            spi_dev->ramwr_busy = false;
            spi_dev->ramwr_free_tick = lv_tick_get();
            lv_spi_on_ramwr_idle();
            aicos_sem_give(spi_dev->sync_ready);
            continue;
        }
#endif
        lv_disp_data_blt(spi_dev);

#if !defined(AIC_PANEL_DBI_ZC_T1D8BP)
        if (spi_dev->bl_en && spi_dev->te_queue) {
            int ret = aicos_wqueue_wait(spi_dev->te_queue, TE_TIMEOUT_MS);
            if (ret < 0)
                LV_LOG_ERROR("SPI wait TE irq timeout, ret: %d\n", ret);
        }
#endif

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
        /* Stay INVON. Single 0x2C for the whole GRAM. */
        zc_ramwr_full(spi_dev, spi_dev->tx_buf, spi_dev->tx_len, wdt);
        if (!spi_dev->bl_en) {
            /* This RAMWR is the boot frame. Wake onto that picture. */
            zc_wdt_kick(wdt);
            aic_spi_lcd_write_cmd(spi_dev->dev, 0x11, 0, RT_NULL);
            rt_thread_mdelay(120);
            zc_wdt_kick(wdt);
            aic_spi_lcd_write_cmd(spi_dev->dev, 0x29, 0, RT_NULL);
            aic_spi_lcd_backlight_gpio_enable(config->bl_pin, config->bl_low_active, true);
            spi_dev->bl_en = true;
            te_input_pin_cfg(spi_dev);
            pr_info("zc-t1d8bp: SLPOUT+DISPON after boot frame\n");
        }
        {
            static unsigned stream_log;

            stream_log++;
            if (stream_log <= 10U || (stream_log % 50U) == 0U) {
                u32 mid = (u32)(config->width * (config->height / 2) + config->width / 2) * 3U;

                pr_info("zc-t1d8bp: stream f=%u RGB666 inv=%d tx_len=%u first6=%02X %02X %02X mid=%02X %02X %02X\n",
                        stream_log,
                        (int)spi_dev->pixel_invert,
                        (unsigned)spi_dev->tx_len,
                        spi_dev->tx_buf[0], spi_dev->tx_buf[1], spi_dev->tx_buf[2],
                        spi_dev->tx_buf[mid], spi_dev->tx_buf[mid + 1], spi_dev->tx_buf[mid + 2]);
            }
        }
#else
        aic_spi_lcd_flush(spi_dev->dev, spi_dev->tx_buf, spi_dev->tx_len);
        aic_spi_lcd_wait_completion(spi_dev->dev);
#endif

#if !defined(AIC_PANEL_DBI_ZC_T1D8BP)
        if (!spi_dev->bl_en) {
            aic_spi_lcd_backlight_gpio_enable(config->bl_pin, config->bl_low_active, true);

            spi_dev->bl_en = true;
            te_input_pin_cfg(spi_dev);
        }
#endif

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
        spi_dev->ramwr_busy = false;
        /* No TE pin: wait after RAMWR so the next 0x2C is less likely
         * to start mid-scan. 360x360 RGB666 is ~15.5ms on a 50MHz 4-wire. */
        spi_dev->ramwr_free_tick = lv_tick_get() + ZC_RAMWR_SETTLE_MS;
        lv_spi_on_ramwr_idle();
#endif
        aicos_sem_give(spi_dev->sync_ready);
    }
}

static struct lv_spi_dev *lv_spi_setup(int id, int width, int height, int fb_size)
{
    const struct lv_spi_config *config = &spi_configs[id];
    struct lv_spi_dev *spi = NULL;
    int tx_size;

    spi = rt_malloc(sizeof(struct lv_spi_dev));
    if (!spi) {
        LV_LOG_ERROR("malloc spi dev failed\n");
        return NULL;
    }
    rt_memset(spi, 0, sizeof(*spi));

    spi->id = id;
    spi->fb_size = fb_size;
    spi->power_on = false;

    if (config->te_pin) {
        spi->te_pin = rt_pin_get(config->te_pin);
        if (spi->te_pin < 0) {
            LV_LOG_ERROR("get spi te pin %s failed\n", config->te_pin);
            rt_free(spi);
            return NULL;
        }

        spi->te_queue = aicos_wqueue_create();
    }

    spi->display_sem = aicos_sem_create(0);
    spi->sync_ready = aicos_sem_create(0);
    spi->frame_count = 0;
    spi->bl_en = false;
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    spi->pixel_invert = true; /* splash: INVON + software invert */
#endif

    spi->ge2d_dev = mpp_ge_open();
    spi->config = config;

    spi->dev = aic_spi_lcd_init(&config->spi_cfg);
    if (!spi->dev) {
        pr_err("aic spi lcd init failed\n");
        rt_free(spi);
        return NULL;
    }

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    /*
     * ZC panel can run in 18-bit/pixel (RGB666, 3 bytes/pixel) via the
     * TEST-BARS path -- tx_buf must be sized for the largest format this
     * build can send, not just the 16-bit (2 bytes/pixel) real-content path.
     * Bug found 2026-09-04: this was hardcoded to *2, so zc_fill_test_bars()
     * (which writes width*height*3 = 388800 bytes for 360x360) was
     * overflowing a 259200-byte buffer. tx_len is still set per-flush to the
     * actual bytes needed for whichever path runs (see lv_disp_data_blt).
     */
    tx_size = width * height * 3;
#else
    tx_size = width * height * 2;
#endif
    spi->tx_len = tx_size;
    spi->tx_buf = aicos_malloc_align(MEM_CMA, tx_size, CACHE_LINE_SIZE);
    if (!spi->tx_buf) {
        LV_LOG_ERROR("malloc display buf failed\n");
        rt_free(spi);
        return NULL;
    }
    aicos_dcache_clean_invalid_range((ulong *)spi->tx_buf, (ulong)ALIGN_UP(tx_size, CACHE_LINE_SIZE));
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    spi->tx_pp[0] = spi->tx_buf;
    spi->tx_pp[1] = aicos_malloc_align(MEM_CMA, tx_size, CACHE_LINE_SIZE);
    if (spi->tx_pp[1])
        aicos_dcache_clean_invalid_range((ulong *)spi->tx_pp[1],
                                         (ulong)ALIGN_UP(tx_size, CACHE_LINE_SIZE));
    spi->tx_sel = 0;
#endif

    return spi;
}

void lv_spi_screen_enable(struct lv_spi_dev *spi_dev)
{
#if defined(AIC_EPD_P118F04)
    /* J14 is the P118F04 e-ink. Do not clock QSPI1 into that glass. */
    (void)spi_dev;
    return;
#else
    aicos_thread_t thid = NULL;

    lv_spi_panel_enable(spi_dev);

    thid = aicos_thread_create(spi_dev->config->spi_cfg.bus_name,
                               8192, 20 + spi_dev->id, disp_thread, spi_dev);
    if (thid == NULL)
        LV_LOG_ERROR("Failed to create display thread\n");
#endif
}

static lv_color_format_t lv_display_fmt(enum mpp_pixel_format cf)
{
    lv_color_format_t fmt = LV_COLOR_FORMAT_ARGB8888;
    switch(cf) {
        case MPP_FMT_RGB_565:
            fmt = LV_COLOR_FORMAT_RGB565;
            break;
        case MPP_FMT_RGB_888:
            fmt = LV_COLOR_FORMAT_RGB888;
            break;
        case MPP_FMT_ARGB_8888:
            fmt = LV_COLOR_FORMAT_ARGB8888;
            break;
        case MPP_FMT_XRGB_8888:
            fmt = LV_COLOR_FORMAT_XRGB8888;
            break;
        default:
            LV_LOG_ERROR("unsupported format:%d", cf);
            break;
    }
    return fmt;
}
static void spi_dev_poweron(struct lv_spi_dev *spi_dev)
{
    if (!spi_dev->power_on) {
        lv_spi_screen_enable(spi_dev);
        spi_dev->power_on = true;
    }
}

void lv_spi_flush(lv_display_t *disp, u8 *data)
{
    struct lv_spi_dev *spi_dev = lv_find_spi_dev(disp);

    if (!spi_dev)
        return;

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    /* JPEG stream owns RAMWR. Taking sync_ready here steals the token and
     * freezes present() until the next 1s unstick — then it deadlocks. */
    if (spi_dev->hold_released)
        return;
#endif

    if (spi_dev->frame_count >= 1)
        aicos_sem_take(spi_dev->sync_ready, AICOS_WAIT_FOREVER);
    else
        spi_dev->frame_count++;

    spi_dev->data = data;

    aicos_sem_give(spi_dev->display_sem);
}

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
extern enum mpp_pixel_format lv_fmt_to_mpp_fmt(lv_color_format_t cf);

static struct lv_spi_dev *lv_spi_primary(void)
{
    if (!g_spi_devs || !g_spi_devs[0])
        return NULL;
    return g_spi_devs[0];
}

void lv_spi_release_hold(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();

    if (!spi)
        return;
    spi->hold_released = true;
    spi->hold_test_pattern = false;
    spi->replay_splash = false;
    /* JPEG is real sRGB (white circle, cyan/purple). Invert made cyan→red. */
    spi->pixel_invert = false;
}

int lv_spi_get_draw_info(void **buf, uint32_t *width, uint32_t *height,
                         uint32_t *stride, int *mpp_fmt)
{
    struct lv_spi_dev *spi = lv_spi_primary();
    lv_display_t *disp;
    lv_color_format_t cf;
    uint32_t w;
    void *src;

    if (!spi || !spi->disp)
        return -1;

    disp = spi->disp;
    src = spi->stream_buf[spi->stream_idx];
    if (!src) {
        lv_draw_buf_t *db = lv_display_get_buf_active(disp);

        if (!db || !db->data)
            return -1;
        src = db->data;
    }

    cf = lv_display_get_color_format(disp);
    w = lv_display_get_horizontal_resolution(disp);
    if (buf)
        *buf = src;
    if (width)
        *width = w;
    if (height)
        *height = lv_display_get_vertical_resolution(disp);
    if (stride)
        *stride = (uint32_t)lv_draw_buf_width_to_stride((int32_t)w, cf);
    if (mpp_fmt)
        *mpp_fmt = (int)lv_fmt_to_mpp_fmt(cf);
    return 0;
}

int lv_spi_can_present(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();

    if (!spi || spi->ramwr_busy || spi->draw_locked)
        return 0;
    if ((int32_t)(lv_tick_get() - spi->ramwr_free_tick) < 0)
        return 0;
    return 1;
}

int lv_spi_try_lock_draw(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();

    if (!spi || !lv_spi_can_present())
        return 0;

    /* Take the present token before any stream_buf write. JPEG and GIF
     * both blit then present(); without this they can share one buffer. */
    if (spi->frame_count >= 1) {
        if (aicos_sem_take(spi->sync_ready, 0) != 0)
            return 0;
    } else {
        spi->frame_count++;
    }
    spi->ramwr_busy = true;
    spi->draw_locked = true;
    return 1;
}

void lv_spi_unlock_draw(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();

    if (!spi || !spi->draw_locked)
        return;
    spi->draw_locked = false;
    spi->ramwr_busy = false;
    if (spi->sync_ready)
        aicos_sem_give(spi->sync_ready);
}

int lv_spi_tx_begin(void **buf)
{
    struct lv_spi_dev *spi = lv_spi_primary();
    uint32_t t0;

    if (!buf || !spi || !spi->tx_pp[0])
        return -1;
    if (!spi->tx_pp[1])
        spi->tx_fill_sel = 0;
    t0 = lv_tick_get();
    while (spi->ramwr_busy &&
           (!spi->tx_pp[1] || spi->tx_sending_sel == spi->tx_fill_sel)) {
        if (lv_tick_elaps(t0) > 80U)
            return -1;
        aicos_msleep(1);
    }
    *buf = spi->tx_pp[spi->tx_fill_sel];
    return 0;
}

int lv_spi_tx_wait_idle(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();
    uint32_t t0;

    if (!spi)
        return -1;
    t0 = lv_tick_get();
    /* tx_queued_gen moves before ramwr_busy is set, so both have to match. */
    while (spi->ramwr_busy || spi->tx_done_gen != spi->tx_queued_gen) {
        if (lv_tick_elaps(t0) > 200U) {
            /* A full 360x360 RGB666 is ~70 ms. 80 ms force-cleared a
             * transfer that was still on the wire, then the NAND read
             * started on that same DMA channel and never returned. */
            spi->ramwr_busy = false;
            spi->tx_done_gen = spi->tx_queued_gen;
            return -1;
        }
        aicos_msleep(1);
    }
    return 0;
}

int lv_spi_tx_queue(uint32_t len, unsigned x, unsigned y, unsigned w, unsigned h)
{
    struct lv_spi_dev *spi = lv_spi_primary();
    uint8_t sel;
    uint32_t t0;

    if (!spi || !len || !w || !h || !spi->tx_pp[0])
        return -1;
    sel = spi->tx_fill_sel;
    if (sel > 1U || !spi->tx_pp[sel])
        return -1;
    t0 = lv_tick_get();
    while (spi->tx_direct) {
        if (lv_tick_elaps(t0) > 80U)
            return -1;
        aicos_msleep(1);
    }
    spi->tx_direct_sel = sel;
    spi->tx_direct_len = len;
    spi->ramwr_x = (uint16_t)x;
    spi->ramwr_y = (uint16_t)y;
    spi->ramwr_w = (uint16_t)w;
    spi->ramwr_h = (uint16_t)h;
    spi->tx_queued_gen++;
    spi->tx_direct = 1;
    spi->tx_fill_sel = (uint8_t)(sel ^ 1U);
    if (!spi->tx_pp[spi->tx_fill_sel])
        spi->tx_fill_sel = sel;
    aicos_sem_give(spi->display_sem);
    return 0;
}

int lv_spi_present(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();
    void *buf = NULL;
    uint32_t w = 0, h = 0, stride = 0;
    int locked;

    if (!spi || !spi->disp) {
        lv_spi_unlock_draw();
        return -1;
    }

    lv_spi_release_hold();
    if (lv_spi_get_draw_info(&buf, &w, &h, &stride, NULL) < 0 || !buf) {
        lv_spi_unlock_draw();
        return -1;
    }

    aicos_dcache_clean_range((void *)buf,
                             (u32)ALIGN_UP(spi->fb_size, CACHE_LINE_SIZE));

    locked = spi->draw_locked ? 1 : 0;
    if (locked) {
        spi->draw_locked = false;
        /* Token already taken in try_lock_draw(). */
    } else if (spi->frame_count >= 1) {
        /* Never block the USB thread. If RAMWR is in flight, drop this JPEG. */
        if (spi->ramwr_busy ||
            ((int32_t)(lv_tick_get() - spi->ramwr_free_tick) < 0) ||
            aicos_sem_take(spi->sync_ready, 0) != 0) {
            static unsigned drop_log;
            if ((++drop_log % 15U) == 1U)
                pr_info("zc-t1d8bp: RAMWR busy, drop (%u)\n", drop_log);
            return 0;
        }
    } else {
        spi->frame_count++;
    }
    /* Claim before waking disp_thread so GIF/JPEG cannot write the same
     * stream_buf while RGB565→666 blit is still reading it. */
    spi->ramwr_busy = true;
    spi->data = buf;
    spi->present_gen++;
    /* Full-frame path. Drop a leftover GIF window so a later replay is not cropped. */
    spi->ramwr_w = 0;
    spi->ramwr_h = 0;
    aicos_sem_give(spi->display_sem);
    /* Next blit goes to the other draw buf so we never overwrite in-flight. */
    if (spi->stream_buf[0] && spi->stream_buf[1])
        spi->stream_idx ^= 1U;
    return 1;
}

void lv_spi_restore_splash(void)
{
    struct lv_spi_dev *spi = lv_spi_primary();

    if (!spi)
        return;
    /* Already queued, or already frozen on splash. */
    if (!spi->hold_released && (spi->replay_splash || spi->hold_test_pattern))
        return;
    spi->hold_released = false;
    spi->pixel_invert = true;
    spi->replay_splash = true;
    spi->hold_test_pattern = false;
    aicos_sem_give(spi->display_sem);
}
#endif

static void spi_dev_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lv_draw_buf_t *disp_buf = lv_display_get_buf_active(disp);
    struct lv_spi_dev *spi_dev = lv_find_spi_dev(disp);
    (void)px_map;

    if (!spi_dev)
        return;

    if (lv_disp_flush_is_last(disp)) {
        spi_dev_poweron(spi_dev);
        if (spi_dev->hold_test_pattern) {
            lv_display_flush_ready(disp);
            return;
        }
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
        /* After splash, only lv_spi_present() may queue RAMWR. */
        if (spi_dev->hold_released || spi_dev->frame_count >= 1) {
            lv_display_flush_ready(disp);
            return;
        }
#endif
        aicos_dcache_clean_invalid_range((ulong *)disp_buf->data, (ulong)ALIGN_UP(spi_dev->fb_size, CACHE_LINE_SIZE));
        lv_spi_flush(disp, disp_buf->data);
    }

    lv_display_flush_ready(disp);
}

int lv_spi_display_init(int use_frame_buffer)
{
    void *buf1 = NULL, *buf2 = NULL;
    struct aicfb_screeninfo info;
    struct mpp_fb *fb = NULL;
    int width, height, fb_size;
    int i;

    fb = mpp_fb_open();
    if (!fb) {
        LV_LOG_ERROR("open mpp fb failed");
        return -1;
    }
    mpp_fb_ioctl(fb, AICFB_GET_SCREENINFO, &info);

    lv_color_format_t cf = lv_display_fmt(info.format);
    if (cf == LV_COLOR_FORMAT_UNKNOWN)
        return -1;

    g_spi_devs = lv_malloc_zeroed(sizeof(struct lv_spi_dev *) * LV_SPI_DEV_NUM);
    if (!g_spi_devs) {
        LV_LOG_ERROR("malloc spi devs array failed");
        return -1;
    }

    for (i = 0; i < LV_SPI_DEV_NUM; i++) {
        width = spi_configs[i].width ? spi_configs[i].width : info.width;
        height = spi_configs[i].height ? spi_configs[i].height : info.height;

        width = ALIGN_8B(width);
        fb_size = width * height * lv_color_format_get_size(cf);

        struct lv_spi_dev *spi_dev = lv_spi_setup(i, width, height, fb_size);
        if (!spi_dev) {
            LV_LOG_ERROR("spi setup failed for device %d", i);
            goto err;
        }
        g_spi_devs[i] = spi_dev;

        if (i == 0 && use_frame_buffer) {
            buf1 = info.framebuffer;
#ifdef AIC_PAN_DISPLAY
            buf2 = info.framebuffer + info.smem_len;
#else
            buf2 = NULL;
#endif
        } else {
            buf1 = aicos_malloc_align(MEM_CMA, fb_size, CACHE_LINE_SIZE);
            if (!buf1) {
                LV_LOG_ERROR("malloc display buf1 failed");
                goto err;
            }
            buf2 = aicos_malloc_align(MEM_CMA, fb_size, CACHE_LINE_SIZE);
            if (!buf2) {
                LV_LOG_ERROR("malloc display buf2 failed");
                goto err;
            }
        }

        lv_display_t *disp = lv_display_create(width, height);
        lv_display_set_color_format(disp, cf);
        lv_display_set_flush_cb(disp, spi_dev_flush);

        lv_display_set_buffers(disp, buf1, buf2, fb_size, LV_DISPLAY_RENDER_MODE_DIRECT);
#if defined(LV_DISPLAY_ROTATE_EN) && defined(AIC_LVGL_DOUBLE_DISP_DEMO)
        lv_display_set_rotation(disp, LV_SECOND_ROTATE_DEGREE / 90);
#elif defined(LV_DISPLAY_ROTATE_EN) && defined(LV_USE_SPI_REPLACE_MIPI_DBI)
        lv_display_set_rotation(disp, LV_ROTATE_DEGREE / 90);
#endif

        spi_dev->disp = disp;
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
        spi_dev->stream_buf[0] = buf1;
        spi_dev->stream_buf[1] = buf2;
        spi_dev->stream_idx = 0;
#endif
    }

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    /* UI is paused — must paint the solid hold here (no LVGL flush). */
    if (g_spi_devs[0])
        spi_dev_poweron(g_spi_devs[0]);
#endif

    return 0;

err:
    if (g_spi_devs) {
        for (i = 0; i < LV_SPI_DEV_NUM; i++) {
            if (g_spi_devs[i]) {
                if (i == 0 && use_frame_buffer)
                    continue;

                if (g_spi_devs[i]->tx_buf)
                    aicos_free_align(MEM_CMA, g_spi_devs[i]->tx_buf);

                if (g_spi_devs[i]->ge2d_dev)
                    mpp_ge_close(g_spi_devs[i]->ge2d_dev);

                rt_free(g_spi_devs[i]);
            }
        }
        lv_free(g_spi_devs);
        g_spi_devs = NULL;
    }

    if (fb)
        mpp_fb_close(fb);

    LV_LOG_ERROR("create lv spi display failed");
    return -1;
}
