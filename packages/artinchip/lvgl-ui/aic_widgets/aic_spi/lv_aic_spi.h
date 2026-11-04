/*
 * Copyright (C) 2025-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Huahui <huahui.mai@artinchip.com>
 */

#include <string.h>
#include <rtdevice.h>
#include <aic_core.h>
#include <drv_qspi.h>

#include "lvgl.h"
#include <mpp_fb.h>
#include <mpp_ge.h>

#include "aic_spi_lcd.h"

#if defined(LV_SPI_ST77916)
#define  LV_LCD_WIDTH    360
#define  LV_LCD_HEIGHT   360
#elif defined(LV_SPI_ST7789)
#define  LV_LCD_WIDTH    320
#define  LV_LCD_HEIGHT   320
#elif defined(LV_SPI_ST77912)
#define  LV_LCD_WIDTH    240
#define  LV_LCD_HEIGHT   240
#elif defined(LV_SPI_GC9D01N)
#define  LV_LCD_WIDTH    160
#define  LV_LCD_HEIGHT   160
#else
#define  LV_LCD_WIDTH    0
#define  LV_LCD_HEIGHT   0
#endif

struct lv_spi_config {
    const aic_spi_lcd_cfg_t spi_cfg;
    const char *bl_pin;
    const char *te_pin;
    bool bl_low_active;
    unsigned int width;
    unsigned int height;
};

struct lv_spi_dev {
    const struct lv_spi_config *config;
    aic_spi_lcd_dev_t *dev;

    /* LVGL Draw Buffer */
    unsigned char *data;
    unsigned int fb_size;

    /* SPI Transfer Buffer (ZC: tx_pp[2] ping-pong RGB666) */
    unsigned char *tx_buf;
    unsigned char *tx_pp[2];
    unsigned int tx_sel;
    unsigned char *splash_keep;
    unsigned char *blt_src;
    bool replay_splash;
    volatile unsigned int present_gen; /* incremented only by lv_spi_present() */
    unsigned int tx_len;
    struct mpp_ge *ge2d_dev;

    int te_pin;
    aicos_wqueue_t te_queue;
    aicos_sem_t display_sem;
    aicos_sem_t sync_ready;
    unsigned int frame_count;

    bool bl_en;
    bool power_on;
    bool hold_test_pattern; /* freeze splash until JPEG/LVGL takes over */
    bool hold_released;     /* JPEG stream owns RAMWR; cleared on splash restore */
    bool pixel_invert;      /* always on: vendor INVON 0x21, software invert */
    volatile bool ramwr_busy;
    volatile bool draw_locked; /* try_lock_draw() owns stream_buf until present/unlock */
    volatile uint32_t ramwr_free_tick; /* can_present() after this (VBLANK settle) */
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    /* GIF writes RGB666 straight into tx_pp[tx_fill_sel]; disp_thread only DMAs it. */
    volatile uint8_t tx_direct;
    volatile uint8_t tx_direct_sel;
    volatile uint8_t tx_sending_sel;
    volatile uint8_t tx_fill_sel;
    volatile uint32_t tx_direct_len;
    volatile uint32_t tx_queued_gen;
    volatile uint32_t tx_done_gen;
    volatile uint16_t ramwr_x, ramwr_y, ramwr_w, ramwr_h;
#endif
    void *stream_buf[2];
    unsigned int stream_idx;

    lv_display_t *disp;
    unsigned int id;
};

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
void lv_spi_release_hold(void);
int lv_spi_get_draw_info(void **buf, uint32_t *width, uint32_t *height,
                         uint32_t *stride, int *mpp_fmt);
int lv_spi_can_present(void);
int lv_spi_try_lock_draw(void);
void lv_spi_unlock_draw(void);
int lv_spi_present(void);
/* Fill the idle RGB666 slot while the other slot is on the wire. */
int lv_spi_tx_begin(void **buf);
/* CS for this buffer must already match the panel select (call after tx_wait_idle). */
int lv_spi_tx_queue(uint32_t len, unsigned x, unsigned y, unsigned w, unsigned h);
int lv_spi_tx_wait_idle(void);
void lv_spi_restore_splash(void);
/* Clear a stuck RAMWR so the next JPEG can present again. */
void lv_spi_unstick_ramwr(void);
/* Weak in lv_aic_spi.c. App may wake the JPEG tick as soon as RAMWR ends. */
void lv_spi_on_ramwr_idle(void);
/* Weak. 1s with no present(); unstick decode/USB without leaving Area4. */
void lv_spi_on_display_stall(void);
#endif

void lv_spi_flush(lv_display_t *disp, u8 *data);

void lv_spi_write_buffer(struct lv_spi_dev *spi_dev,
                         unsigned int cmd, unsigned int len, const u8 *data);

int lv_spi_display_init(int use_frame_buffer);

/*
 * defined in the spi tft controller dirver lv_xxx.c
 */
void lv_spi_panel_enable(struct lv_spi_dev *dev);

#define lv_spi_write_seq(dev, cmd, seq...)                  \
    do {                                                    \
        static const u8 d[] = { seq };                      \
        lv_spi_write_buffer(dev, cmd, ARRAY_SIZE(d), d);    \
    } while(0);

/*
 * Init ArtInChip SoC SPI Controller and enable
 * lcd peripheral by calling lv_spi_panel_enable()
 */
void lv_spi_screen_enable(struct lv_spi_dev *spi_dev);

void lv_qspi_disp_mode_init(struct lv_spi_dev *spi_dev);
