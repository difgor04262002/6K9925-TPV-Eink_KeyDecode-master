/*
 * ZC-T1D8BP-012 1.8" 360x360 QSPI TFT (ST77916), no DC/RS, no TE pin.
 *
 * D125CxS-DEMO wiring via J25 (DVP connector) -> SPI1 (pinmux func 6):
 * J14 wiring (panel connector, not the SoC MOSI/MISO names):
 *   SPI_D0  -> PE.6
 *   SPI_D1  -> PE.7
 *   SPI_D2  -> PE.10
 *   SPI_D3  -> PE.9
 *   SPI_CLK -> PE.8
 *   LCD_CS  -> PE.5
 *   LCD_RST -> PE.11
 *
 * Pixel path: LV_USE_SPI_REPLACE_MIPI_DBI + qspi1 → lv_st77916.c owns
 * reset + init + flush. This file only supplies FB timing / screeninfo;
 * zc_t1d8bp_commands[] matches lv_st77916.c 20250331 QSPI_OK only.
 * SPI replace never executes this table.
 * Working pixel path: 0x02 1-wire RGB666 + INVON invert.
 */

#include "panel_com.h"
#include "panel_dbi.h"
#include <aic_hal_gpio.h>
#ifdef KERNEL_RTTHREAD
#include <rtdevice.h>
#include <artinchip_fb.h>
#endif

/*
 * Canonical table = lv_st77916.c ZC path only:
 * ST77916_BOE 1.8'' 360x360 B3 V1.4_20250331_4_QSPI_OK.
 * SPI replace does not execute this table.
 */
static const u8 zc_t1d8bp_commands[] = {
    /* V1.4_20250331_4_QSPI_OK — no COLMOD */
    0xF0, 1, 0x28,
    0xF2, 1, 0x28,
    0x73, 1, 0xF0,
    0x76, 1, 0x0F,
    0x7C, 1, 0xD1,
    0x83, 1, 0xE0,
    0x84, 1, 0x61,
    0xF2, 1, 0x82,
    0xF0, 1, 0x00,
    0xF0, 1, 0x01,
    0xF1, 1, 0x01,
    0xB0, 1, 0x52,
    0xB1, 1, 0x49,
    0xB2, 1, 0x24,
    0xB3, 1, 0x01,
    0xB4, 1, 0x66,
    0xB5, 1, 0x44,
    0xB6, 1, 0xC5,
    0xB7, 1, 0x40,
    0xB8, 1, 0x86,
    0xB9, 1, 0x15,
    0xBA, 1, 0x00,
    0xBB, 1, 0x08,
    0xBC, 1, 0x08,
    0xBD, 1, 0x00,
    0xBE, 1, 0x00,
    0xBF, 1, 0x07,
    0xC0, 1, 0x80,
    0xC1, 1, 0x10,
    0xC2, 1, 0x37,
    0xC3, 1, 0x80,
    0xC4, 1, 0x10,
    0xC5, 1, 0x37,
    0xC6, 1, 0xA9,
    0xC7, 1, 0x41,
    0xC8, 1, 0x01,
    0xC9, 1, 0xA9,
    0xCA, 1, 0x41,
    0xCB, 1, 0x01,
    0xCC, 1, 0x7F,
    0xCD, 1, 0x7F,
    0xCE, 1, 0xFF,
    0xD0, 1, 0x91,
    0xD1, 1, 0x68,
    0xD2, 1, 0x68,
    0xF5, 2, 0x00, 0xA5,
    0xF1, 1, 0x10,
    0xF0, 1, 0x00,
    0xF0, 1, 0x02,
    0xE0, 14, 0xF0, 0x0E, 0x14, 0x0B, 0x0B, 0x16, 0x3A, 0x44, 0x4E, 0x18, 0x14, 0x13, 0x2F, 0x35,
    0xE1, 14, 0xF0, 0x0D, 0x13, 0x0B, 0x0A, 0x16, 0x39, 0x43, 0x4E, 0x17, 0x13, 0x13, 0x2E, 0x34,
    0xF0, 1, 0x10,
    0xF3, 1, 0x10,
    0xE0, 1, 0x09,
    0xE1, 1, 0x00,
    0xE2, 1, 0x03,
    0xE3, 1, 0x00,
    0xE4, 1, 0xE0,
    0xE5, 1, 0x06,
    0xE6, 1, 0x21,
    0xE7, 1, 0x00,
    0xE8, 1, 0x05,
    0xE9, 1, 0x82,
    0xEA, 1, 0xDE,
    0xEB, 1, 0xC0,
    0xEC, 1, 0x40,
    0xED, 1, 0x84,
    0xEE, 1, 0xFF,
    0xEF, 1, 0x71,
    0xF8, 1, 0xFF,
    0xF9, 1, 0x50,
    0xFA, 1, 0xFF,
    0xFB, 1, 0xF3,
    0xFC, 1, 0x00,
    0xFD, 1, 0x00,
    0xFE, 1, 0x00,
    0xFF, 1, 0x00,
    0x60, 1, 0x42,
    0x61, 1, 0xDF,
    0x62, 1, 0x40,
    0x63, 1, 0x40,
    0x64, 1, 0x02,
    0x65, 1, 0x00,
    0x66, 1, 0x00,
    0x67, 1, 0x00,
    0x68, 1, 0x00,
    0x69, 1, 0x00,
    0x6A, 1, 0x00,
    0x6B, 1, 0x00,
    0x70, 1, 0x42,
    0x71, 1, 0xDF,
    0x72, 1, 0x40,
    0x73, 1, 0x40,
    0x74, 1, 0x01,
    0x75, 1, 0x00,
    0x76, 1, 0x00,
    0x77, 1, 0x00,
    0x78, 1, 0x00,
    0x79, 1, 0x00,
    0x7A, 1, 0x00,
    0x7B, 1, 0x00,
    0x80, 1, 0x48,
    0x81, 1, 0x00,
    0x82, 1, 0x04,
    0x83, 1, 0x02,
    0x84, 1, 0xDC,
    0x85, 1, 0x00,
    0x86, 1, 0x00,
    0x87, 1, 0x00,
    0x88, 1, 0x48,
    0x89, 1, 0x00,
    0x8A, 1, 0x06,
    0x8B, 1, 0x02,
    0x8C, 1, 0xDE,
    0x8D, 1, 0x00,
    0x8E, 1, 0x00,
    0x8F, 1, 0x00,
    0x90, 1, 0x48,
    0x91, 1, 0x00,
    0x92, 1, 0x08,
    0x93, 1, 0x02,
    0x94, 1, 0xE0,
    0x95, 1, 0x00,
    0x96, 1, 0x00,
    0x97, 1, 0x00,
    0x98, 1, 0x48,
    0x99, 1, 0x00,
    0x9A, 1, 0x0A,
    0x9B, 1, 0x02,
    0x9C, 1, 0xE2,
    0x9D, 1, 0x00,
    0x9E, 1, 0x00,
    0x9F, 1, 0x00,
    0xA0, 1, 0x48,
    0xA1, 1, 0x00,
    0xA2, 1, 0x03,
    0xA3, 1, 0x02,
    0xA4, 1, 0xDB,
    0xA5, 1, 0x00,
    0xA6, 1, 0x00,
    0xA7, 1, 0x00,
    0xA8, 1, 0x48,
    0xA9, 1, 0x00,
    0xAA, 1, 0x05,
    0xAB, 1, 0x02,
    0xAC, 1, 0xDD,
    0xAD, 1, 0x00,
    0xAE, 1, 0x00,
    0xAF, 1, 0x00,
    0xB0, 1, 0x48,
    0xB1, 1, 0x00,
    0xB2, 1, 0x07,
    0xB3, 1, 0x02,
    0xB4, 1, 0xDF,
    0xB5, 1, 0x00,
    0xB6, 1, 0x00,
    0xB7, 1, 0x00,
    0xB8, 1, 0x48,
    0xB9, 1, 0x00,
    0xBA, 1, 0x09,
    0xBB, 1, 0x02,
    0xBC, 1, 0xE1,
    0xBD, 1, 0x00,
    0xBE, 1, 0x00,
    0xBF, 1, 0x00,
    0xC0, 1, 0x65,
    0xC1, 1, 0x74,
    0xC2, 1, 0x47,
    0xC3, 1, 0x56,
    0xC4, 1, 0xAA,
    0xC5, 1, 0x11,
    0xC6, 1, 0x00,
    0xC7, 1, 0x2A,
    0xC8, 1, 0xA2,
    0xC9, 1, 0x33,
    0xD0, 1, 0x65,
    0xD1, 1, 0x74,
    0xD2, 1, 0x47,
    0xD3, 1, 0x56,
    0xD4, 1, 0xAA,
    0xD5, 1, 0x11,
    0xD6, 1, 0x00,
    0xD7, 1, 0x2A,
    0xD8, 1, 0xA2,
    0xD9, 1, 0x33,
    0xF3, 1, 0x01,
    0xF0, 1, 0x00,
    0x21, 1, 0x00,
    0x11, 1, 0x00,
    0x00, 1, 120,
    /* 0x29 deferred until first RAMWR (lv_aic_spi.c disp_thread). */
};

#define ZC_T1D8BP_RESET_PIN "PE.11"

static struct gpio_desc reset;

static void zc_set_ui_alpha_opaque(void)
{
#ifdef KERNEL_RTTHREAD
    struct aicfb_alpha_config alpha = {0};
    rt_device_t fb = rt_device_find("aicfb");

    if (!fb)
        return;

    alpha.layer_id = AICFB_LAYER_TYPE_UI;
    if (rt_device_control(fb, AICFB_GET_ALPHA_CONFIG, &alpha) != RT_EOK)
        return;

    alpha.enable = 1;
    alpha.mode = AICFB_GLOBAL_ALPHA_MODE;
    alpha.value = 0xFF;
    rt_device_control(fb, AICFB_UPDATE_ALPHA_CONFIG, &alpha);
#endif
}

static void zc_refresh_ui_layer(void)
{
#ifdef KERNEL_RTTHREAD
    struct aicfb_layer_data layer = {0};
    rt_device_t fb = rt_device_find("aicfb");

    if (!fb)
        return;

    layer.layer_id = AICFB_LAYER_TYPE_UI;
    layer.rect_id = 0;
    if (rt_device_control(fb, AICFB_GET_LAYER_CONFIG, &layer) != RT_EOK)
        return;

    layer.enable = 1;
    rt_device_control(fb, AICFB_UPDATE_LAYER_CONFIG, &layer);
#endif
}

static void zc_fill_fb_colorblocks(void)
{
#ifdef KERNEL_RTTHREAD
    struct rt_device_graphic_info info = {0};
    rt_device_t fb = rt_device_find("aicfb");
    unsigned char color[5][3] = {
        { 0x00, 0x00, 0xFF },
        { 0x00, 0xFF, 0x00 },
        { 0xFF, 0x00, 0x00 },
        { 0x00, 0x00, 0x00 },
        { 0xFF, 0xFF, 0xFF },
    };
    unsigned char *pos;
    u32 x, y, index;

    if (!fb || rt_device_control(fb, RTGRAPHIC_CTRL_GET_INFO, &info) != RT_EOK ||
        !info.framebuffer || info.pitch < 2) {
        pr_err("zc-t1d8bp: aicfb not ready\n");
        return;
    }

    for (y = 0; y < info.height; y++) {
        pos = (unsigned char *)info.framebuffer + y * info.pitch;
        for (x = 0; x < info.width; x++) {
            index = (y / 100 + x / 100) % 5;
            *(pos++) = (color[index][0] >> 3) | ((color[index][1] & 0x1c) << 3);
            *(pos++) = ((color[index][1] & 0xe0) >> 5) | (color[index][2] & 0xf8);
        }
    }

    aicos_dcache_clean_range((unsigned long *)info.framebuffer,
                             (unsigned long)info.pitch * info.height);
    pr_info("zc-t1d8bp: fb colorblocks %ux%u @%p\n",
            info.width, info.height, info.framebuffer);
#endif
}

static void zc_fill_fb_solid(u16 color)
{
#ifdef KERNEL_RTTHREAD
    struct rt_device_graphic_info info = {0};
    rt_device_t fb = rt_device_find("aicfb");
    unsigned char *pos;
    u32 x, y;
    u8 hi = (color >> 8) & 0xff;
    u8 lo = color & 0xff;

    if (!fb || rt_device_control(fb, RTGRAPHIC_CTRL_GET_INFO, &info) != RT_EOK ||
        !info.framebuffer || info.pitch < 2) {
        pr_err("zc-t1d8bp: aicfb not ready\n");
        return;
    }

    /*
     * Espressif LVGL uses swap_bytes for ST77916 QSPI. Store RGB565 as
     * wire order (hi, lo) so DBI 4SDA does not send LE 00,F8 for red.
     */
    for (y = 0; y < info.height; y++) {
        pos = (unsigned char *)info.framebuffer + y * info.pitch;
        for (x = 0; x < info.width; x++) {
            *pos++ = hi;
            *pos++ = lo;
        }
    }

    aicos_dcache_clean_range((unsigned long *)info.framebuffer,
                             (unsigned long)info.pitch * info.height);
    pr_info("zc-t1d8bp: fb solid %04x be %ux%u @%p bytes=%02x%02x\n", color,
            info.width, info.height, info.framebuffer,
            ((unsigned char *)info.framebuffer)[0],
            ((unsigned char *)info.framebuffer)[1]);
#endif
}

static void zc_fill_fb_stripes(void)
{
#ifdef KERNEL_RTTHREAD
    struct rt_device_graphic_info info = {0};
    rt_device_t fb = rt_device_find("aicfb");
    u16 *base;
    u32 x, y, stride_px, band;

    if (!fb || rt_device_control(fb, RTGRAPHIC_CTRL_GET_INFO, &info) != RT_EOK ||
        !info.framebuffer || info.pitch < 2) {
        pr_err("zc-t1d8bp: aicfb not ready\n");
        return;
    }

    band = info.height / 6;
    if (!band)
        band = 1;

    stride_px = info.pitch / 2;
    base = (u16 *)info.framebuffer;
    for (y = 0; y < info.height; y++) {
        u16 pix = ((y / band) & 1) ? 0xF800 : 0x0000;

        for (x = 0; x < info.width; x++)
            base[y * stride_px + x] = pix;
    }

    aicos_dcache_clean_range((unsigned long *)info.framebuffer,
                             (unsigned long)info.pitch * info.height);
    aicos_dcache_invalid_range((unsigned long *)info.framebuffer,
                               (unsigned long)info.pitch * info.height);
    pr_info("zc-t1d8bp: fb stripes %ux%u band=%u top=%04x mid=%04x\n",
            info.width, info.height, band, base[0],
            base[(info.height / 2) * stride_px]);
#endif
}

static int panel_prepare(void)
{
#ifdef LV_USE_SPI_REPLACE_MIPI_DBI
    /*
     * Reset owned solely by lv_spi_panel_enable() (PE.11). Avoid double
     * pulse from aic_panel prepare + LVGL SPI init.
     */
    pr_info("zc-t1d8bp: prepare skip RST (SPI replace; lv_st77916 owns PE.11)\n");
    return 0;
#else
    panel_get_gpio(&reset, ZC_T1D8BP_RESET_PIN);

    panel_gpio_set_value(&reset, 1);
    aic_delay_ms(10);
    panel_gpio_set_value(&reset, 0);
    aic_delay_ms(20);
    panel_gpio_set_value(&reset, 1);
    aic_delay_ms(120);
    return 0;
#endif
}

static int panel_enable(struct aic_panel *panel)
{
#ifdef LV_USE_SPI_REPLACE_MIPI_DBI
    /*
     * Pixels go through SPI1/QSPI (lv_aic_spi). Keep this panel only for
     * FB timing / screeninfo; do not touch the DBI engine or PD pins.
     *
     * Do NOT call panel_backlight_enable() here: it drives
     * AIC_PANEL_ENABLE_GPIO (PF.0), the same physical backlight pin
     * lv_aic_spi.c's disp_thread() turns on itself once the first real
     * frame has been flushed to the panel. Enabling it this early (this
     * callback runs before lv_st77916.c has finished the vendor init
     * table) re-creates the ST77916 datasheet's "no visible-artifact
     * guarantee once host writes power/gamma regs with backlight already
     * on" case (Power ON/OFF Sequence, Notes 2/3) — that race is what the
     * logic-analyzer capture showed as the flicker.
     *
     * Force BL off: pinmux leaves PF.0 as GPIO with undefined level, and
     * aicfb_probe runs ~0.5s before SPI init / first RAMWR.
     */
    pr_info("zc-t1d8bp: SPI1 QSPI replace mode (PE5-10, RST PE.11)\n");
    panel_backlight_disable(panel, 0);
    return 0;
#else
    struct spi_cfg *spi_cfg = panel->dbi->spi;

    pr_info("zc-t1d8bp: legacy DBI path (not recommended on PE SPI1)\n");

    spi_cfg->code[0] = 0x02;
    spi_cfg->code[1] = 0x00;
    spi_cfg->code[2] = 0x00;
    spi_cfg->qspi_mode = 0;
    spi_cfg->vbp_num = 0;
    spi_cfg->code1_cfg = 0;
    panel->dbi->first_line = 0;
    panel->dbi->other_line = 0;
    panel_di_enable(panel, 0);
    panel_dbi_commands_execute(panel, &panel->dbi->commands);

    spi_cfg->code[0] = 0x32;
    spi_cfg->code[1] = 0x00;
    spi_cfg->code[2] = 0x2C;
    spi_cfg->qspi_mode = 0;
    panel->dbi->first_line = 0;
    panel->dbi->other_line = 0;
    panel_di_enable(panel, 0);

    zc_set_ui_alpha_opaque();
    zc_fill_fb_solid(0xF800);
    zc_refresh_ui_layer();
    panel_de_timing_enable(panel, 0);
    panel_backlight_enable(panel, 0);
    return 0;
#endif
}

static struct aic_panel_funcs zc_t1d8bp_funcs = {
    .prepare = panel_prepare,
    .enable = panel_enable,
    .disable = panel_default_disable,
    .unprepare = panel_default_unprepare,
    .register_callback = panel_register_callback,
};
/******************************************************
 * porch 值搭配 5MHz 會得到什麼：
 * htotal = hactive + hback_porch + hfront_porch + hsync_len
 *        = 360 + 20 + 20 + 20 = 420
 * vtotal = vactive + vback_porch + vfront_porch + vsync_len
 *        = 360 + 20 + 20 + 10 = 410
 * 總「像素」數 = 420 × 410 = 172200
 * 每幀時間 = 172200 / 5,000,000 ≈ 34.44 ms
 * 更新率   ≈ 29.0 fps 
 * 也就是說，只改 pixelclock: 2000000, → pixelclock: 5000000,，
 * 其餘欄位原封不動，隱含更新率就會從原本的 11.6fps 提升到約 29fps，對一般 UI 來說已經是很合理的目標值，不需要再去動 porch。
 * 如果你想湊一個更整齊的數字（例如剛好 30fps），可以把總像素數壓到 166,667 左右，做法是稍微縮小 porch，
 * 例如：
 * c
 * static struct display_timing zc_t1d8bp_timing = {
 *     .pixelclock   = 5000000,
 *     .hactive      = 360,
 *     .hback_porch  = 10,
 *     .hfront_porch = 10,
 *     .hsync_len    = 10,   // htotal = 390
 *     .vactive      = 360,
 *     .vback_porch  = 10,
 *     .vfront_porch = 10,
 *     .vsync_len    = 5,    // vtotal = 385
 * };
 * 390×385 = 150150，150150/5,000,000 ≈ 30.03ms ≈ 33.3fps，
 * 或是你可以自己代公式 htotal × vtotal = pixelclock / 目標fps 微調 porch 湊到你要的數字——
 * 但既然這些 porch 本來就對應不到實體訊號，湊到剛好某個整數 fps 其實沒有功能上的意義，
 * 跟保留現在這組隨便挑的值（能算出 29fps）比起來沒有本質差別，
 * 純粹是想讓數字好看一點的話才需要動。
 * 另外提醒一下（跟上一輪講的一樣）：
 * 目前這條路徑實際的像素刷新是靠 LVGL 自己的 semaphore（disp_thread）在推，
 * 跟這個 display_timing 算出來的更新率是否真的有被 DE 框架拿去節流，
 * 我們還沒證實，而且 ZC_T1D8BP 分支目前 hold_test_pattern=true 
 * 會讓 disp_thread 根本不會被建立——所以現在調這個值，在畫面上不會有任何立即可見的效果，
 * 等你把上一輪提到的背光時序、還有 hold_test_pattern 那段除錯樁碼都處理掉、UI 真的開始持續刷新之後，
 * 這個 pixelclock 才會真正影響到你感受到的流暢度。
 ******************************************************/

/******************************************************
 * 如果 pixelclock 改成 10,000,000，
 * 一樣不用動 hactive/vactive，porch/sync-len 也可以完全不動，
 * 直接算一下目前這組值搭配 10MHz 的結果：
 * htotal = 360 + 20 + 20 + 20 = 420
 * vtotal = 360 + 20 + 20 + 10 = 410
 * 總「像素」數 = 420 × 410 = 172200
 * 每幀時間 = 172200 / 10,000,000 ≈ 17.22 ms
 * 更新率   ≈ 58.1 fps
 * 也就是說只改 pixelclock: 2000000, → pixelclock: 10000000,，
 * 其餘欄位維持原樣，隱含更新率就會落在約 58fps，已經很接近 60fps 這個常見目標，
 * 不需要再調整其他欄位。
 * 如果想湊到剛好 60fps（總像素數要壓到 166,667 左右），可以把 porch 稍微再收一點，例如：
 * c
 * static struct display_timing zc_t1d8bp_timing = {
 *     .pixelclock   = 10000000,
 *     .hactive      = 360,
 *     .hback_porch  = 10,
 *     .hfront_porch = 10,
 *     .hsync_len    = 10,   // htotal = 390
 *     .vactive      = 360,
 *     .vback_porch  = 6,
 *     .vfront_porch = 6,
 *     .vsync_len    = 5,    // vtotal = 377
 * };
 * 390×377 = 147030，147030/10,000,000 ≈ 14.7ms ≈ 68fps，比目標略快；
 * 想更精準湊到 60fps 可以用公式 htotal × vtotal = pixelclock / 目標fps 自己微調任一 porch 值反推——公式：
 * 目標總像素數 = 10,000,000 / 60 ≈ 166,667，例如維持 htotal=420 不變，
 * 反推 vtotal ≈ 166667/420 ≈ 397，
 * 也就是把 vertical porch 總和（vback_porch+vfront_porch+vsync_len）
 * 從現在的 50 調到約 37 即可（比如 15/15/7）。
 * 同樣提醒一次：這幾個 porch 值在這片 QSPI/DBI 面板上沒有對應實體訊號，
 * 湊到整數 fps 純粹是數字好看，不影響功能；而且目前 ZC_T1D8BP 分支
 * 因為 hold_test_pattern=true 讓畫面刷新根本沒有持續運作，
 * 這個 pixelclock 改了之後也還要等你處理完背光時序、拿掉那段除錯樁碼，
 * 才會在實際畫面上感受到差異。
******************************************************/

static struct display_timing zc_t1d8bp_timing = {
    .pixelclock   = 41328000,//41328000,//61992000,//41328000,//5000000,//2000000,

    .hactive      = 360,
    .hback_porch  = 20,
    .hfront_porch = 20,
    .hsync_len    = 20,

    .vactive      = 360,
    .vback_porch  = 20,
    .vfront_porch = 20,
    .vsync_len    = 10,
};

static struct spi_cfg spi = {
    .qspi_mode = 0,
    .vbp_num   = 0,
    .code1_cfg = 0,
    .code      = { 0x02, 0x00, 0x00 },
};

static struct panel_dbi dbi = {
    .type = SPI,
    .format = SPI_4SDA_RGB565,
    .flags = DBI_REFRESH_LINE | DBI_COMMAND_INSERT_NONE,
    .spi = &spi,
    .commands = {
        .buf = zc_t1d8bp_commands,
        .len = ARRAY_SIZE(zc_t1d8bp_commands),
    }
};

struct aic_panel dbi_zc_t1d8bp = {
    .name = "panel-zc-t1d8bp",
    .timings = &zc_t1d8bp_timing,
    .funcs = &zc_t1d8bp_funcs,
    .dbi = &dbi,
    .connector_type = AIC_DBI_COM,
};
