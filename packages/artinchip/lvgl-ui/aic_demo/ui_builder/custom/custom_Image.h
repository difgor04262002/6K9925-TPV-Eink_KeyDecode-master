#ifndef CUSTOM_IMAGE_CODE
#define CUSTOM_IMAGE_CODE

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#ifndef CUSTOM_IMAGE_JPEG_FILE
#define CUSTOM_IMAGE_JPEG_FILE recv_image.jpg
#endif

#ifndef CUSTOM_IMAGE_PNG_FILE
#define CUSTOM_IMAGE_PNG_FILE recv_image.png
#endif

#ifndef CUSTOM_IMAGE_GIF_FILE
#define CUSTOM_IMAGE_GIF_FILE recv_image.gif
#endif

enum
{
    CUSTOM_IMAGE_TYPE_JPEG = 0x00,
    CUSTOM_IMAGE_TYPE_PNG = 0x01,
    CUSTOM_IMAGE_TYPE_GIF = 0x02,
    CUSTOM_IMAGE_TYPE_APNG = 0x03,
    CUSTOM_IMAGE_TYPE_INVALID = 0xFF
};

/* Byte 4 in 0xCD 0xBB 0x02 [type] [slot] 0xCD — maps to screen_main_1 image_1..image_3 */
enum
{
    CUSTOM_IMAGE_SLOT_1 = 0x01,
    CUSTOM_IMAGE_SLOT_2 = 0x02,
    CUSTOM_IMAGE_SLOT_3 = 0x03
};

/* Writable LittleFS on NOR: /data/Panel_0 (factory boot GIF).
 * Panel_1..4 are paused. The user file is /data/Panel_signal, up to 8 MB. */
#define CUSTOM_FLASH_PANEL_COUNT 4
#define CUSTOM_FLASH_BOOT_PANEL  0U
#define CUSTOM_FLASH_PANEL_ROOT  "/data"
#define CUSTOM_FLASH_BOOT_DIR    CUSTOM_FLASH_PANEL_ROOT "/Panel_0"
#ifndef CUSTOM_FLASH_SIGNAL_ENABLE
#define CUSTOM_FLASH_SIGNAL_ENABLE 1
#endif
#define CUSTOM_FLASH_SIGNAL_DIR  CUSTOM_FLASH_PANEL_ROOT "/Panel_signal"
#define CUSTOM_FLASH_SIGNAL_MAX  (8U * 1024U * 1024U)
extern const char * const custom_flash_panel_dirs[CUSTOM_FLASH_PANEL_COUNT];
const char *custom_flash_panel_dir(unsigned panel_1_to_4);
/* 0..4: Panel_0 is factory boot/idle GIF. USB save is 1..4 only. */
/* Latch Panel_1..4 from F9-F2-F5 index (0xF01..0xF04). 0 = none. */
void custom_image_set_flash_panel(unsigned panel_1_to_4);
unsigned custom_image_flash_panel(void);
/* Panel whose NOR erase/program is in progress, else the folder PC just named. */
unsigned custom_image_xfer_panel(void);
/* Bits 0..3: folders this transfer is showing as boot+Upgrade.
 * FA sets all four. F1..F4 set that folder, plus any folder still saving. */
unsigned custom_image_xfer_mask(void);
void custom_image_xfer_cmd_mask_or(unsigned mask);
void custom_image_xfer_cmd_mask_clear(void);
/* FE 0A FA: the next files land on Panel_1 .. Panel_4 in order. */
void custom_image_fa_seq_begin(void);
void custom_image_fa_seq_end(void);
/* Drop the old GIF in those folders (bits 0..3 = Panel_1..4). 0x0B is
 * sent after this finishes, from the UI thread. */
void custom_image_purge_request(unsigned mask);
uint8_t custom_image_purge_done_take(void);
void custom_round_note_panel_saved(unsigned panel_1_to_4);
/* After F9-F2-F5, start raw GIF/APNG recv if payload begins with file magic. */
uint8_t custom_image_try_start_flash_file(const uint8_t *data, uint32_t len);
/* LVGL src after a successful flash save, e.g. "L:/data/Panel_1/Panel_001.gif". NULL if none. */
const char *custom_image_flash_play_src(void);
/* Posix path /data/Panel_N/Panel_00N.gif. Panel 0 allowed. Returns 0 if out of range. */
uint8_t custom_image_panel_gif_path(unsigned panel_0_to_4, char *buf, uint32_t buflen);
/* 0 if the GIF is missing. Panel 0 allowed. */
uint32_t custom_image_panel_gif_size(unsigned panel_0_to_4);
/* Load Panel_N GIF into dst. Panel 0 allowed. Returns 1 on success. */
uint8_t custom_image_load_panel_gif(unsigned panel_0_to_4, uint8_t *dst, uint32_t cap,
                                    uint32_t *size_out);
/* /data/Panel_0/LOADIN.jpeg. 0 if missing. */
uint32_t custom_image_loadin_jpeg_size(void);
/* Read that file into dst. Returns 1 on success. */
uint8_t custom_image_load_loadin_jpeg(uint8_t *dst, uint32_t cap, uint32_t *size_out);

void custom_image_init(void);
/*
 * Serialize USB RX (custom_image_recv) vs LVGL image display (take_pending/get_data/show/release).
 * Prevents recv<->ready swap + reuse racing with custom_image_release_payload() (heap corruption).
 */
void custom_image_bus_lock(void);
void custom_image_bus_unlock(void);
void custom_image_spill_boost(void);
void custom_image_recv(const uint8_t *data, uint32_t len);
/*
 * 1 = 目前正處於影像 payload 接收中 (s_receiving_image)。
 * 上層可在影像接收期間關閉「全字節 state-machine 比對」(如 BROM 觸發)，
 * 避免高熵 PNG/JPEG/GIF/MP4 位元組誤觸控制序列。
 */
uint8_t custom_image_is_receiving(void);
/* Abort in-flight image RX (e.g. stream idle → splash). Safe if not receiving. */
void custom_image_abort_recv(void);
/* Mark image-stream activity (CD-BB / payload / completed frame). */
void custom_image_note_stream_activity(void);
/*
 * 1 = still "live" for Area4 idle watchdog: last CD-BB / payload /
 *     completed-frame timestamp within (miss_frames × frame_interval_ms).
 *     frame_interval_ms is learned from successive finish_recv successes
 *     (default 33 ms ≈ 30 fps). Stuck RX without packets is NOT treated as
 *     fresh (so idle can abort and return to splash).
 */
uint8_t custom_image_stream_is_fresh(uint32_t miss_frames);
/* Returns image type; writes target slot (CUSTOM_IMAGE_SLOT_*) when type is not INVALID. */
uint8_t custom_image_take_pending(uint8_t *slot_out);
const uint8_t *custom_image_get_data(uint8_t img_type, uint32_t *size);
void custom_image_release_ready(void);
/*
 * After a successful display pass, free exactly the RX heap block used as source.
 * Pass the pointer last obtained from custom_image_get_data() for this attempt (same LVGL tick).
 * Handles recv/ready swap completing during LVGL decode — release_ready() alone corrupts state.
 */
void custom_image_release_payload(const uint8_t *payload);
/*
 * If the UI is still waiting on an older image type but USB completed a newer one
 * (recv swaps ready buffer), align wait type/slot to the current ready payload.
 * Returns 1 when *type_io / *slot_io were updated.
 */
uint8_t custom_image_resync_wait(uint8_t *type_io, uint8_t *slot_io);

/*
 * Perf instrumentation (consumed by custom_Area4 for PNG decode/render measurement).
 *
 * custom_image_get_last_recv_done_ms:
 *   ms timestamp (aic_get_time_ms units) of the most recent successful finish_recv.
 *   Used to compute "pickup latency" = show_image entry − recv done.
 *
 * custom_image_get_recv_done_count:
 *   Monotonic count of successful finish_recv calls. Subtract two snapshots over
 *   a measurement window to derive recv-rate / drop-rate vs. show throughput.
 */
uint64_t custom_image_get_last_recv_done_ms(void);
uint32_t custom_image_get_recv_done_count(void);

/* NOR write of a received GIF runs off the USB callback. While this is 1,
 * do not open LittleFS. The other RAM buffer may still take the next file. */
uint8_t custom_image_flash_save_busy(void);
/* 1 = the recv buffer already holds a finished GIF waiting for the writer.
 * USB must not be re-armed until that buffer is handed off. */
uint8_t custom_image_save_pending(void);
uint8_t custom_image_usb_rearm_hold(void);
/* 1 while a full chunk is queued and the writer has not taken it yet. */
uint8_t custom_image_spill_pending(void);
/* 1 from the first Panel_signal byte until flash saved. Gaps between
 * 2 MB segments still count: FE 0C must not start playback in that gap. */
uint8_t custom_image_signal_busy(void);
/* 1 while a GIF frame is being read from NAND. The panel DMA must wait. */
void custom_nand_bus_enter(void);
void custom_nand_bus_leave(void);
uint8_t custom_nand_bus_busy(void);
/* Drop a RAM copy that is still reading. Playback must not wait on it. */
void custom_image_load_cancel(void);
void custom_image_load_resume(void);
/* Write one queued Panel_signal chunk. Safe to call from the display thread. */
void custom_image_spill_service(void);
/* Open a saved GIF. The display thread calls this; the USB callback must not. */
void custom_round_kick_play(void);
/* Boot clip has been up for 3 s and Panel_signal is on flash. */
void custom_round_boot_to_signal(void);
/* 1 while a USB Area4 JPEG/PNG stream owns the glass. */
uint8_t custom_round_jpeg_streaming(void);
/* Free the two 2 MB USB blocks. Put them back with restore. */
void custom_image_park_usb_ram(void);
/* Next transfer: display thread frees the play pieces and restores USB. */
void custom_round_play_release_req(void);
void custom_round_play_service(void);
void *custom_image_take_play_ram(uint32_t sz);
/* Read one file into dst. *out is the byte count. */
uint8_t custom_image_read_path(const char *path, uint8_t *dst, uint32_t cap,
                               uint32_t *out);
void custom_image_give_play_ram(void *p);
void custom_image_restore_usb_ram(void);
/* 1 = display thread is playing the file. Decode one frame if it is due. */
uint8_t custom_round_disp_pump(void);
void custom_image_usb_hold_rearm(void);
/* USB OUT is held so LVGL can put LOADIN.jpeg on the glass before the
 * file body, or so the NOR write can finish before the next packet. */
uint8_t custom_image_usb_out_held(void);
void custom_image_usb_out_hold_for_loadin(void);
void custom_image_usb_out_release(void);
/* First bytes of Panel_N/Panel_NNN.gif, for the open-fail log. */
uint32_t custom_image_peek_panel_gif(unsigned panel_0_to_4, uint8_t *dst, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_IMAGE_CODE */
