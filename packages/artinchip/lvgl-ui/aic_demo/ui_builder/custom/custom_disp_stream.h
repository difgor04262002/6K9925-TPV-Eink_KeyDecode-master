#ifndef CUSTOM_DISP_STREAM_H
#define CUSTOM_DISP_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/*
 * ----------------------------------------------------------------------------
 *  custom_disp_stream — Area4 direct framebuffer streaming (Option C)
 * ----------------------------------------------------------------------------
 *
 *  目的：把 Area4 收到的 PNG / JPEG bitstream 直接走 MPP HW decode + GE2D HW
 *  rotate 寫進 panel framebuffer，再 PAN_DISPLAY，整套跳過 LVGL 的 image
 *  decoder cache / blend / flush_thread 流程。
 *
 *  為什麼需要：
 *  --------------------------------------------------------------------------
 *  原本 Area4 透過 lv_image_set_src + lv_obj_invalidate 走 LVGL 的 refresh：
 *      memcpy(payload→CMA buf) → lv_image_set_src
 *        → LVGL refresh tick (~10 ms)
 *          → MPP decode (memory cache hit/miss)
 *          → GE2D blend ARGB → draw_buf
 *          → cache_clean(draw_buf)
 *          → flush_thread.wait_vsync
 *          → GE2D rotate(draw_buf → FB)
 *          → PAN_DISPLAY
 *
 *  測量結果 intv≈39 ms / fps≈25，扣掉 render≈3 ms 與 GE2D blit≈7 ms 之後仍有
 *  ~30 ms 是「LVGL 內部把 1280x800 ARGB 重新解 + 二次 blend + cache 清理」的
 *  純損耗。瓶頸不在 LCD vsync (sync_wait≈0us)。
 *
 *  做法 (C)：
 *  --------------------------------------------------------------------------
 *  在 screen_4_Area 進入時 acquire 旗標：
 *      → custom_disp_streaming_mode = 1
 *      → LVGL 的 disp_draw_buf_flush 變 no-op
 *  Area4 推圖時走：
 *      JPEG/PNG → MPP HW decode → GE2D HW rotate(270°) → FB[idx] → PAN_DISPLAY
 *  離開 screen_4_Area 時 release：
 *      → custom_disp_streaming_mode = 0
 *      → 強制 LVGL 重新 invalidate active screen，畫面接回 LVGL 控制
 *
 *  預期效果：intv 從 ~38 ms 降到 ~13-17 ms (decode 5 + GE2D 7 + pan_display 1)，
 *  fps 從 25-26 推到 50+。LCD panel 上限 60Hz (16.67ms vsync)。
 *
 *  Threading：
 *  --------------------------------------------------------------------------
 *  acquire / release / show() 在 LVGL UI thread 上呼叫。
 *  ZC-T1D8BP：show() 只把 JPEG 拷進 bounce buffer，decode/blit/present 在
 *  jpgdec worker，避免 VE wait（最長 2s）卡住 10ms tick 與 USB heartbeat。
 *  custom_disp_streaming_mode 是 volatile uint8_t，無需 mutex。
 *
 *  Lifecycle：
 *  --------------------------------------------------------------------------
 *  - custom_disp_stream_init() 在 boot 時呼叫一次 (CMA buffer 預配 + GE 開啟)。
 *  - acquire/release 可任意配對，重複 acquire 會被忽略 (idempotent)。
 *  - show 在未 acquire 的狀態下會直接 return 0，由呼叫端 (custom_Area4_show_image)
 *    做 fallback。
 */

/* Image type encoding matches CUSTOM_IMAGE_TYPE_PNG / CUSTOM_IMAGE_TYPE_JPEG. */
#define CUSTOM_DISP_STREAM_IMG_PNG  0x01U
#define CUSTOM_DISP_STREAM_IMG_JPEG 0x02U

void custom_disp_stream_init(void);

/* Returns 1 if streaming is currently acquired (i.e. LVGL flush is no-op). */
uint8_t custom_disp_stream_is_active(void);

/*
 * acquire: 取得獨佔顯示權；release: 還回去並 invalidate active screen 讓 LVGL
 * 再畫一次。
 *
 * acquire 前應確認目前 active screen 已經 load 完 (avoid 把進入動畫畫一半的內容
 * 卡在 FB 上)。release 後第一張 LVGL flush 會接管 FB[buf_id]。
 */
void custom_disp_stream_acquire(void);
void custom_disp_stream_release(void);

/* Rebuild the JPEG decoder after a display stall. Stay in stream mode. */
void custom_disp_stream_recover(void);

/* 1 = show() may queue a JPEG. 0 = worker busy, keep the payload. */
uint8_t custom_disp_stream_can_show(void);

/*
 * Show one frame straight into the panel FB.
 *   img_type : CUSTOM_DISP_STREAM_IMG_PNG or CUSTOM_DISP_STREAM_IMG_JPEG
 *   data     : bitstream bytes (must remain valid for the call duration only)
 *   size     : bitstream length in bytes
 *
 * Returns 1 on success (frame committed to PAN_DISPLAY), 0 on any failure
 * (decoder/GE error, no FB info, not acquired). On failure the previous frame
 * stays on screen.
 */
uint8_t custom_disp_stream_show(uint8_t img_type, const uint8_t *data, uint32_t size);

/*
 * ZC-T1D8BP: after splash, LVGL flush is ignored. GIF/JPEG must write the
 * QSPI stream RGB565 buffer and call lv_spi_present() (same path as JPEG).
 * src is gifdec canvas (BGRA8888). Scaled and letterboxed to 360×360.
 * Returns 1 if RAMWR was queued, 0 if busy/failed (caller retries).
 */
uint8_t custom_disp_stream_present_argb8888(const uint8_t *px, uint32_t src_w, uint32_t src_h);
/* 1 = draw Upgrade on the next present. 0 = boot picture only. */
void custom_disp_stream_upgrade_draw(uint8_t on);
/* 1 while PC is sending or NOR is erasing. Dots keep cycling on each frame. */
uint8_t custom_ui_upgrade_on(void);
/*
 * Same pixels, but RGB666 goes straight to the idle QSPI buffer.
 * full_frame=1 paints the whole 360x360 (black bars included).
 * full_frame=0 sends only the fitted GIF rectangle.
 */
/* Write RGB666 into the idle TX slot. Does not start CS — call queue after the panel select. */
uint8_t custom_disp_stream_fill_argb8888_direct(const uint8_t *px, uint32_t src_w,
                                                uint32_t src_h, uint8_t full_frame);
/* Opaque full-frame GIF indexes. Same letterbox as the BGRA fill. */
uint8_t custom_disp_stream_fill_index_scaled(const uint8_t *index, uint32_t src_w,
                                             uint32_t src_h, const uint8_t *rgb,
                                             uint8_t full_frame);
/* Draw Upgrade onto the RGB666 buffer just filled. No-op when the transfer is idle. */
void custom_disp_stream_stamp_upgrade(void);
/* Cover-scale a composited BGRA canvas onto the whole 360x360 glass. */
uint8_t custom_disp_stream_fill_bgra_cover(const uint8_t *px, uint32_t src_w,
                                           uint32_t src_h);
/* Palette indexes, RGB table (3 bytes/entry).
 * use_tindex: transparent pixels come from bgra (B,G,R,A), not a second full convert. */
uint8_t custom_disp_stream_fill_index_direct(const uint8_t *index, uint32_t src_w,
                                             uint32_t src_h, const uint8_t *rgb,
                                             uint8_t full_frame, const uint8_t *bgra,
                                             uint8_t tindex, uint8_t use_tindex);
uint8_t custom_disp_stream_queue_rgb666(void);
/* Decode one JPEG and cover-scale it into a dst_w x dst_h RGB666 buffer (top 6 bits). */
uint8_t custom_disp_stream_build_loadin(const uint8_t *jpeg, uint32_t size,
                                        uint8_t *dst, uint32_t dst_w, uint32_t dst_h);
/* Copy a full-frame RGB666 image into the idle TX slot. Queue after the panel select. */
uint8_t custom_disp_stream_fill_rgb666_full(const uint8_t *rgb, uint32_t w, uint32_t h);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_DISP_STREAM_H */
