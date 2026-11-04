#ifndef CUSTOM_AREA4_CODE
#define CUSTOM_AREA4_CODE

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ui_objects.h"
#include "aic_ui.h"
#include "ui_util.h"
#include "custom_Image.h"

/*
 * Area4 (screen_4_Area) 影像 slot 編碼，與 Area2/Area3 的 0x10/0x20 區段不衝突。
 */
enum
{
    CUSTOM_IMAGE_SLOT_AREA4_1 = 0x30
};

void custom_area4_init(void);
uint8_t custom_area4_is_slot(uint8_t slot_byte);
/* Clear stuck DRAW_POST_END back-pressure (e.g. after auto screen switch). */
void custom_area4_clear_render_pending(void);
/*
 * 1 = last show_image() is still waiting for its DRAW_POST_END (GE2D/MPP
 * decode+blit still in flight). Poll this before switching away from Area4
 * (e.g. before lv_scr_load(splash)) — do not assume "one LVGL tick" is
 * enough, that raced GE2D on splash load and crashed with Illegal
 * Instruction / poisoned registers.
 */
uint8_t custom_area4_render_pending(void);
/*
 * Leave Area4 display path before loading another screen.
 * Detaches DRAW_POST_END and releases the stream fast-path. Keep src —
 * set_src(NULL) is invalid in LVGL v9. Call only once
 * custom_area4_render_pending() is 0 (or timed out) — this detaches the
 * only signal that in-flight render was still running.
 */
void custom_area4_leave_display(void);
/*
 * 解析 6-byte 控制命令：
 *   0xCD 0xBB 0x06 0x01 0x00 0xCD
 * 表示 PC 端要送一張 1280x800 PNG 圖到 screen_4_Area。
 * byte2 = 0x06 為 Area4 區段識別、byte3 = 0x01 固定 slot、byte4 = 0x00 固定為 PNG 格式。
 *
 * 命中時：
 *   *mapped_slot ← CUSTOM_IMAGE_SLOT_AREA4_1
 *   *mapped_type ← CUSTOM_IMAGE_TYPE_PNG
 *   回傳 1。
 */
uint8_t custom_area4_try_parse_cmd(const uint8_t *cmd_frame, uint8_t *mapped_slot, uint8_t *mapped_type);
uint8_t custom_area4_show_image(uint8_t img_type, uint8_t slot_byte, const uint8_t *img_data, uint32_t img_size);

/*
 * Monotonic counter of frames that successfully made it through the display
 * pipeline (DRAW_POST_END for widget path, GE2D blit success for streaming
 * path). Used by the freeze_watchdog_thread in lv_port_disp.c to detect a
 * silent lockup: if custom_image_get_recv_done_count() keeps advancing but
 * this value stays flat for many seconds, the display path is stuck.
 */
uint32_t custom_area4_get_total_show_count(void);

/*
 * Called by freeze-watchdog on DISPLAY STALL: clear stuck render-pending
 * gate and drop new frames briefly so GE/VE soft-recover can breathe.
 */
void custom_area4_on_display_stall(void);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_AREA4_CODE */
