#include "custom_Area4.h"
#include "custom_Image.h" /* custom_image_get_last_recv_done_ms / get_recv_done_count */
#include "custom_disp_stream.h" /* Option C: direct FB streaming fast-path */
#include "custom_disp_profile.h"
#include <stdlib.h>
#include <string.h>
#include "aic_core.h"  /* aicos_malloc_align(MEM_CMA, ...), CACHE_LINE_SIZE */
#include "aic_time.h"  /* aic_get_time_ms() for perf instrumentation */
/*
 * D215 HAL exposes hal_ge_soft_reset(); D12P does not. Skip the
 * preventive pulse rather than importing d21x GE internals.
 */
static void custom_area4_ge_soft_reset(void)
{
}
#if LVGL_VERSION_MAJOR >= 9
#include "lv_mpp_dec.h"
#include "lv_tjpgd.h"
#include "lv_image_decoder.h"
#endif
#ifdef KERNEL_RTTHREAD
#include <rtthread.h>
#define CUSTOM_AREA4_LOG(fmt, ...) rt_kprintf("custom_area4: " fmt "\n", ##__VA_ARGS__)
#else
#include <stdio.h>
#define CUSTOM_AREA4_LOG(fmt, ...) printf("custom_area4: " fmt "\n", ##__VA_ARGS__)
#endif

/*
 * CUSTOM_AREA4_VERBOSE
 *
 *   0 (default): 關掉「每張影像都會印一次」的 per-frame log
 *                (decode info ok / show area4 ok / perf: pickup= /
 *                 perf: render= 來自 draw_done_cb)。
 *                這幾條 rt_kprintf 是序列在 LVGL UI tick 上的，
 *                每張 4 行 × ~1-3 ms / 行 ≈ 4-12 ms，直接把 30 fps
 *                目標的 frame budget (33 ms) 砍掉一截。
 *                而且 per-frame "perf: render=" 來自 draw_done_cb，
 *                會跟 30-frame perf stats 在另一個 thread 交錯，
 *                造成 stats 那行被切到中間 → 統計訊息看不清楚 (已知 bug)。
 *
 *   1         : 還原 per-frame log，排查單張影像細節時開。
 *
 * 不受開關控制 (永遠會印)：
 *   - CMA preallocation failed
 *   - show skip ... (所有錯誤路徑)
 *   - decode info fail (異常)
 */
#ifndef CUSTOM_AREA4_VERBOSE
#define CUSTOM_AREA4_VERBOSE 0
#endif

#if CUSTOM_AREA4_VERBOSE
#define CUSTOM_AREA4_VLOG(fmt, ...) CUSTOM_AREA4_LOG(fmt, ##__VA_ARGS__)
#else
#define CUSTOM_AREA4_VLOG(fmt, ...) ((void)0)
#endif

/*
 * screen_4_Area 預設只有 scr->obj (整片 1280x800 背景，無 container/image widget)，
 * 因此 Area4 由本模組動態建立 lv_image / lv_img 物件並掛在 scr->obj 之下，
 * 滿版顯示 PC 透過 0xCD 0xBB 0x06 0x01 0x00 0xCD 命令送來的 PNG 圖檔。
 */
#define CUSTOM_AREA4_IMG_W CUSTOM_DISP_IMG_W
#define CUSTOM_AREA4_IMG_H CUSTOM_DISP_IMG_H

/*
 * Boot 時一次性從 CMA 預配置 1 MB buffer 給 Area4 fallback 使用。
 * 360×360 JPEG 約 10–20 KB；1280x800 JPEG/PNG 通常也在 1 MB 內。
 * 永久持有，不釋放、不 realloc。
 */
#define CUSTOM_AREA4_BUF_SIZE (1U * 1024U * 1024U)

static uint8_t *s_area4_disp_buf;
static uint32_t s_area4_disp_buf_cap;
static uint8_t s_area4_slot_toggle;
static lv_img_dsc_t s_area4_image_dsc_pool[2];
static lv_obj_t *s_area4_dyn_img_obj;

/*
 * ---- PNG decode / render performance instrumentation ------------------------
 *
 * Timestamps in aic_get_time_ms() units.
 *
 * Per-frame log (every successful show):
 *     "perf: pickup=Ams show=Bms"
 * Per-frame log (after LVGL finishes drawing the image):
 *     "perf: render=Cms total=Dms"   (D = show entry → LV_EVENT_DRAW_POST_END)
 * Aggregated log every CUSTOM_AREA4_PERF_WINDOW shows:
 *     "perf stats: N shows in Xms (Y fps), avg pickup=Aa show=Ba render=Ca,
 *                  recv=R drop=R-N (Z%)"
 *
 * pickup = T_show_entry − T_recv_done       (UI thread responsiveness)
 * show   = T_show_exit  − T_show_entry      (memcpy + lv_image_decoder_get_info
 *                                            + lv_image_set_src; SYNCHRONOUS)
 * render = T_draw_done  − T_show_entry      (true decode + GE2D blit, i.e. the
 *                                            lodepng / MPP cost we're hunting)
 *
 * drop  = recv_done_count_delta − shown_in_window
 *         (PNGs that arrived but were overwritten by next recv before UI picked
 *          them up = effective frame drop rate)
 */
#define CUSTOM_AREA4_PERF_WINDOW 30U
/* Cap show rate so GE rotate of 1280x720 cannot sit at 37 fps. */
#define CUSTOM_AREA4_MIN_INTERVAL_MS  36U
/* Pulse GE (not VE) every ~2 min at 30 fps; VE reset would kill MPP. */
#define CUSTOM_AREA4_GE_PULSE_EVERY   4000U

static volatile uint64_t s_perf_show_entry_ms;       /* set at show_image entry, read in draw cb */
static volatile uint8_t  s_perf_meas_pending;        /* 1 = waiting for next DRAW_POST_END to fire */
static uint64_t s_perf_last_show_entry_ms;           /* previous show entry, used to compute show-to-show interval */

/*
 * ---- Back-pressure: 限制單一 in-flight render ------------------------------
 *
 * 為什麼必須加：
 *   PC 端以 ~55 fps 推 JPEG (recv=56 drop=26 寫在 perf stats)，device 端 LVGL
 *   widget 路徑大約只能 render 32 fps。差出來的 ~23 fps 全部進 lv_image
 *   decoder open path → 每張要 aicos_malloc_try_cma(CMA, 4 MB ARGB frame)。
 *
 *   穩定運行 30 秒後，CMA 累積壓力觸發 try_cma 內建 fallback：
 *       kernel/common/osal/aic_osal.c:86 aicos_malloc_try_cma
 *           if (CMA full) lv_drop_one_cached_image()  ← !!!
 *
 *   這把 LVGL global image cache 的 LRU rb-tree 拉進 evict path，跟 LVGL UI
 *   thread 同一時間正在 render 的 cache walk 撞 race window，於是看到
 *       [Warn] lv_cache_entry_dec_ref: ref_cnt(4242189077) < 0
 *       CPU Exception: NO.4 mtval=0x..55f7ee8e (隨機 byte pattern 寫進指標)
 *
 *   修法：強制 device 端 1 render in-flight。同一時間 CMA 只可能持有 1 個 MPP
 *   frame (~4 MB)，距離 CMA 邊界差很遠，永遠不會 trigger evict 路徑。
 *
 * 機制：
 *   - show_image() 入口檢查 s_perf_meas_pending：若仍 == 1 且離 arm 時間 < 500 ms，
 *     drop 此張 (return 0)。下一張 USB 收到時又會 overwrite s_ready_buf，反正
 *     最新的才有意義。
 *   - 500 ms timeout：DRAW_POST_END 假如因 obj 被 hide 或其他原因沒 fire，
 *     不會永久卡死。實測 HW MPP render ~10 ms；之前用 200 ms 在 LVGL 短暫
 *     busy 時會誤觸發 force-clear，造成 CMA 同時握住 2~3 個 MPP frame
 *     (從 meminfo max_used CMA = 23 MB → baseline 15.5 MB + 7.5 MB ≈ 3 frame)。
 *     拉到 500 ms 後，正常 pipeline 延遲都不會踩到 timeout，CMA peak 應該
 *     收斂到 baseline。
 *   - inflight 訊號直接用既存 s_perf_meas_pending 不再新增變數，避免雙重狀態。
 *
 * 預期效果：
 *   - fps 從 32 略降到 ~25-28 (受 render 速度上限)
 *   - drop% 略升 (因 PC 還在 55 fps 推)，從 ~46% 到 ~55%，可接受
 *   - **不再 crash**。CMA 不會被推到 evict 邊界。
 */
static uint32_t s_backpressure_drop_count;
/* After DISPLAY STALL soft-recover: drop show_image until this timestamp. */
static volatile uint64_t s_stall_drop_until_ms;
/*
 * Periodic log：每 100 次 drop 印一行，方便驗證 back-pressure 真的有 fire。
 * 不直接每張印，否則 PC 推太快時這個 log 反而把 UART 灌爆。
 */
#define CUSTOM_AREA4_BP_LOG_EVERY 100U

/*
 * Heap snapshot 頻率 (以 perf window 為單位)。
 * 一個 perf window ≈ 30 frames，30 fps 下 ≈ 1 秒，所以 10 = 約每 10 秒一次。
 * 取樣間隔短一點不會影響效能 (rt_memory_info 很輕)；之所以不每 window 都印
 * 是希望維持 UART log 可讀性，每秒兩行 perf 已經很多了。
 */
#define CUSTOM_AREA4_HEAP_LOG_EVERY_N_WIN 10U
static uint32_t s_perf_heap_log_counter;

/*
 * ---- Monotonic show counter for silent-lockup watchdog (2026-06-04) --------
 *
 * `s_perf_window_show_count` resets every CUSTOM_AREA4_PERF_WINDOW frames, so
 * external observers can't tell "did 30 frames just finish?" vs "are we frozen
 * at the start of a new window?". This counter never resets — it's a pure
 * monotonic frame-shown count for the freeze_watchdog_thread in lv_port_disp.c
 * to compare against custom_image_get_recv_done_count() and detect:
 *
 *   recv advancing  +  show flat  =  display path stuck (panel frozen)
 *   recv flat       +  show flat  =  USB also stopped (link issue)
 *   recv flat       +  show advancing = normal end-of-stream
 *
 * Incremented from two sites (sync-mode show and async DRAW_POST_END), so we
 * always see real "frame committed to render pipeline" events, not just
 * "show_image entered".
 */
static volatile uint32_t s_total_show_count;
static uint32_t s_last_ge_pulse_show;

uint32_t custom_area4_get_total_show_count(void)
{
    return s_total_show_count;
}

void custom_area4_on_display_stall(void)
{
    /* Unstick back-pressure if DRAW_POST_END never fired. */
    s_perf_meas_pending = 0U;
    /* Pause new decode/show for ~1.5s while GE/VE reset settles. */
    s_stall_drop_until_ms = aic_get_time_ms() + 1500U;
    CUSTOM_AREA4_LOG("display stall recover: clear pending, drop shows for 1500ms");
}

void custom_area4_clear_render_pending(void)
{
    s_perf_meas_pending = 0U;
}

/*
 * 1 = last show_image() is still waiting for its DRAW_POST_END (GE2D/MPP
 * decode+blit still in flight). Callers that are about to switch away from
 * Area4 (e.g. idle-watchdog auto-splash) must poll this instead of assuming
 * "one LVGL tick" is enough — HW render is normally ~3-10 ms but has no hard
 * upper bound under load, and switching screens while a blit is still
 * in-flight against s_area4_dyn_img_obj / s_area4_disp_buf is exactly the
 * race that produced the "Illegal Instruction" / 0xdeadbeef-register crashes
 * seen right after "stream idle N missed frames -> splash".
 */
uint8_t custom_area4_render_pending(void)
{
    return s_perf_meas_pending;
}

#if LVGL_VERSION_MAJOR >= 9
static void custom_area4_draw_done_cb(lv_event_t *e);
#endif

void custom_area4_leave_display(void)
{
    /*
     * Do NOT force-clear s_perf_meas_pending here. It is the only signal
     * that tells the caller "GE2D/MPP is still mid-blit on the last Area4
     * frame" — clearing it early made custom_area4_render_pending() lie,
     * so the splash-switch path had no way to know it was unsafe to
     * proceed. Let it clear naturally via custom_area4_draw_done_cb()
     * (DRAW_POST_END), or via the existing 500 ms stale-arm fallback in
     * show_image(). Callers must poll custom_area4_render_pending() before
     * loading another screen.
     */
    custom_disp_stream_release();

    if (!s_area4_dyn_img_obj)
    {
        return;
    }

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_remove_event_cb(s_area4_dyn_img_obj, custom_area4_draw_done_cb);
#endif
    /* Keep the last frame visible until lv_scr_load covers it.
     * Hiding here exposed the Area4 root (was 0xfcfcfc) for one tick → white flash.
     * Do not lv_image_set_src(NULL) — unknown image type / Illegal instruction. */
    CUSTOM_AREA4_VLOG("leave display (src kept, last frame visible)");
}

static uint64_t s_perf_window_start_ms;
static uint32_t s_perf_window_show_count;
static uint32_t s_perf_window_recv_base;
static uint32_t s_perf_pickup_acc_ms;
static uint32_t s_perf_show_acc_ms;
static uint32_t s_perf_render_acc_ms;
static uint32_t s_perf_render_sample_count;          /* render samples may < show count if draw cb misses */
static uint32_t s_perf_interval_acc_ms;              /* show-to-show interval accumulator (real FPS metric) */
static uint32_t s_perf_interval_sample_count;        /* first show in window has no previous → 1 less sample */
static uint8_t  s_perf_initialized;

static uint8_t custom_area4_png_get_size(const uint8_t *buf, uint32_t len, uint16_t *w, uint16_t *h)
{
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    uint32_t ww;
    uint32_t hh;

    if (!buf || !w || !h || len < 24U)
    {
        return 0U;
    }
    if (memcmp(buf, png_sig, sizeof(png_sig)) != 0)
    {
        return 0U;
    }
    if (!(buf[12] == 'I' && buf[13] == 'H' && buf[14] == 'D' && buf[15] == 'R'))
    {
        return 0U;
    }

    ww = ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16) |
         ((uint32_t)buf[18] << 8)  |  (uint32_t)buf[19];
    hh = ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16) |
         ((uint32_t)buf[22] << 8)  |  (uint32_t)buf[23];
    if (ww == 0U || hh == 0U || ww > 4096U || hh > 4096U)
    {
        return 0U;
    }
    *w = (uint16_t)ww;
    *h = (uint16_t)hh;
    return 1U;
}

static uint8_t custom_area4_is_image_src_ready(const void *src)
{
#if LVGL_VERSION_MAJOR >= 9
    lv_image_header_t header;
    return (lv_image_decoder_get_info(src, &header) == LV_RESULT_OK) ? 1U : 0U;
#else
    lv_img_header_t header;
    return (lv_img_decoder_get_info(src, &header) == LV_RES_OK) ? 1U : 0U;
#endif
}

#if LVGL_VERSION_MAJOR >= 9
/*
 * 把 MPP 硬解器再註冊一次到 decoder list 頭部，讓 Area4 JPEG 走 HW。
 *
 *   - PNG  → MPP_CODEC_VIDEO_DECODER_PNG  (lodepng SW ~80 ms 降到 ~10-15 ms)
 *   - JPEG → MPP_CODEC_VIDEO_DECODER_MJPEG (tjpgd SW ~30-50 ms 降到 ~5-10 ms)
 *
 * 同一個 MPP decoder 入口會用 magic byte (FF D8 vs 89 PNG) 自動分流到不同 codec。
 *
 * 只呼叫 init、不呼叫 deinit：
 *   - boot 的 MPP instance 仍保留 splash PNG cache。
 *   - 第二次 init 把新 instance 插到頭部，Area4 JPEG 才會穩定上屏
 *     （16:29 拿掉後 recv/show 有數字但畫面空的）。
 *   - 禁止 lv_mpp_dec_deinit()：會刪掉 decoder 與 cache mutex，
 *     回到 splash 時 lv_scr_load 走 dangling cache → CPU Exception
 *     NO.2（Illegal Instruction）。
 */
static void custom_area4_setup_hw_decoder_once(void)
{
    static uint8_t s_done;
    if (s_done)
    {
        return;
    }
    s_done = 1U;

    lv_mpp_dec_init();
    CUSTOM_AREA4_VLOG("HW MPP decoder re-registered at head of decoder list");
}
#endif

/*
 * 把 PC 送來的 PNG bytes memcpy 進 boot 時預配好的 1 MB CMA buffer。
 *
 * 預配 buffer 在 custom_area4_init 一次性配出後永久持有，runtime 完全不做 alloc/realloc/free，
 * 因此這裡只需要做大小檢查與 memcpy，永遠不會 OOM。
 *
 * 失敗條件僅二者：
 *   1) init 階段 CMA 預配失敗 (s_area4_disp_buf == NULL)
 *   2) 圖檔超過 1 MB 預算（守住上限避免 buffer overrun）
 */
static uint8_t custom_area4_copy_into_prealloc(const uint8_t *src, uint32_t size)
{
    if (!s_area4_disp_buf || s_area4_disp_buf_cap == 0U)
    {
        return 0U;
    }
    if (!src || size == 0U)
    {
        return 0U;
    }
    if (size > s_area4_disp_buf_cap)
    {
        return 0U;
    }

    memcpy(s_area4_disp_buf, src, size);
    return 1U;
}

#if LVGL_VERSION_MAJOR >= 9
/*
 * Fires after LVGL has finished drawing the area4 image object on a render tick.
 *
 * We arm s_perf_meas_pending in show_image; this callback consumes the first fire
 * after arming and logs render latency (= T_draw_done − T_show_entry, i.e. the
 * full lodepng decode + GE2D blit time). We then disarm so subsequent draws
 * (no new frame; just LVGL refresh ticks redrawing the same image) don't spam
 * the log.
 *
 * Caveat: LVGL may issue partial-area redraws, so "render" timing here is the
 * end-to-end interval from show_image entry to the FIRST DRAW_POST_END that
 * fires afterwards. Decode happens inside lv_image_decoder's cache miss path,
 * which is triggered on the first draw of the (new) src. This matches our goal:
 * "how long does it take from giving LVGL a new PNG to the screen actually
 * reflecting it?".
 */
static void custom_area4_draw_done_cb(lv_event_t *e)
{
    uint64_t now_ms;
    uint64_t entry_ms;
    uint32_t render_ms;
    uint32_t show_ms;

    (void)e;

    if (!s_perf_meas_pending)
    {
        return;
    }
    s_perf_meas_pending = 0U;

    now_ms = aic_get_time_ms();
    entry_ms = s_perf_show_entry_ms;
    if (now_ms < entry_ms)
    {
        /* Clock race / wrap — discard sample. */
        return;
    }
    render_ms = (uint32_t)(now_ms - entry_ms);

    /*
     * show_ms was logged at show_image exit; recompute here purely for the
     * "total" reference (which is render_ms since both are referenced from
     * the same show entry). Kept as separate output field to make grep'ing
     * for per-frame and per-window stats easier.
     */
    show_ms = render_ms;
    (void)show_ms;

    s_perf_render_acc_ms += render_ms;
    s_perf_render_sample_count++;

    CUSTOM_AREA4_VLOG("perf: render=%ums (decode + GE2D blit)",
                     (unsigned)render_ms);
}
#endif

void custom_area4_init(void)
{
    s_area4_slot_toggle = 0U;
    memset(s_area4_image_dsc_pool, 0, sizeof(s_area4_image_dsc_pool));
    s_area4_dyn_img_obj = NULL;

    s_perf_show_entry_ms = 0U;
    s_perf_meas_pending = 0U;
    s_stall_drop_until_ms = 0U;
    s_perf_last_show_entry_ms = 0U;
    s_perf_window_start_ms = 0U;
    s_perf_window_show_count = 0U;
    s_perf_window_recv_base = 0U;
    s_perf_pickup_acc_ms = 0U;
    s_perf_show_acc_ms = 0U;
    s_perf_render_acc_ms = 0U;
    s_perf_render_sample_count = 0U;
    s_perf_interval_acc_ms = 0U;
    s_perf_interval_sample_count = 0U;
    s_perf_initialized = 0U;
    s_backpressure_drop_count = 0U;

    /*
     * 從 CMA 一次性預配 1 MB；之後 runtime 全程不再 alloc/realloc，
     * 也不會把壓力傳染到 system heap。
     */
    s_area4_disp_buf = (uint8_t *)aicos_malloc_align(MEM_CMA,
                                                    CUSTOM_AREA4_BUF_SIZE,
                                                    CACHE_LINE_SIZE);
    if (s_area4_disp_buf)
    {
        s_area4_disp_buf_cap = CUSTOM_AREA4_BUF_SIZE;
        CUSTOM_AREA4_VLOG("CMA preallocated %u bytes at %p",
                         (unsigned)CUSTOM_AREA4_BUF_SIZE,
                         (void *)s_area4_disp_buf);
    }
    else
    {
        s_area4_disp_buf_cap = 0U;
        CUSTOM_AREA4_LOG("CMA preallocation %u bytes FAILED, Area4 will be unable to display PNG",
                         (unsigned)CUSTOM_AREA4_BUF_SIZE);
    }
}

uint8_t custom_area4_is_slot(uint8_t slot_byte)
{
    return (slot_byte == CUSTOM_IMAGE_SLOT_AREA4_1) ? 1U : 0U;
}

uint8_t custom_area4_try_parse_cmd(const uint8_t *cmd_frame, uint8_t *mapped_slot, uint8_t *mapped_type)
{
    if (!cmd_frame)
    {
        return 0U;
    }

    /*
     * Area4 命令格式 (固定 1280x800、固定 slot AREA4_1)：
     *   byte0=0xCD byte1=0xBB byte2=0x06 byte3=<fmt> byte4=0x00 byte5=0xCD
     *
     * byte3 編碼資料格式：
     *   0x01 → PNG  (歷史相容；無損；~300 KB/張)
     *   0x00 → JPEG (新增；有損 q=80-85 約 70-120 KB/張，PC 端壓 JPEG 後 USB 傳輸
     *                時間從 ~50 ms 降到 ~15-20 ms，是衝 30 fps 的主要手段)
     *
     * 兩種格式 device 端都走 MPP HW decoder + GE2D HW blit。
     * (mpp_decoder_create 同時支援 PNG 跟 JPEG codec，差別只在解碼 stream 入口)
     */
    if (cmd_frame[0] != 0xCDU || cmd_frame[1] != 0xBBU ||
        cmd_frame[2] != 0x06U || cmd_frame[4] != 0x00U ||
        cmd_frame[5] != 0xCDU)
    {
        return 0U;
    }

    uint8_t mapped;
    if (cmd_frame[3] == 0x01U)
    {
        mapped = CUSTOM_IMAGE_TYPE_PNG;
    }
    else if (cmd_frame[3] == 0x00U)
    {
        mapped = CUSTOM_IMAGE_TYPE_JPEG;
    }
    else
    {
        return 0U;
    }

    if (mapped_slot)
    {
        *mapped_slot = CUSTOM_IMAGE_SLOT_AREA4_1;
    }
    if (mapped_type)
    {
        *mapped_type = mapped;
    }
    return 1U;
}

uint8_t custom_area4_show_image(uint8_t img_type, uint8_t slot_byte, const uint8_t *img_data, uint32_t img_size)
{
    screen_4_Area_t *area4_scr;
    lv_obj_t *area4_parent;
    lv_img_dsc_t *dsc;
    const void *src;
    uint16_t pw = 0U;
    uint16_t ph = 0U;

    /* ---- perf: timestamp & pickup latency ---- */
    uint64_t t_show_entry_ms;
    uint64_t t_recv_done_ms;
    uint32_t pickup_lat_ms;

    if (!custom_area4_is_slot(slot_byte))
    {
        return 0U;
    }
    /*
     * Area4 接受 PNG 與 JPEG (1280x800)。兩者解碼都走 MPP HW path
     * (custom_area4_setup_hw_decoder_once)，差別只在 codec 入口；
     * 對 GE2D HW blit、CMA buffer、dsc 結構而言完全等價。
     * GIF/APNG 仍然不支援 (需要 frame-by-frame 解碼，不適合單張 set_src 模型)。
     * 不符合的型別走 log 而非靜默 return，方便排查「PC 傳了但沒顯示」的情境。
     */
    if (img_type != CUSTOM_IMAGE_TYPE_PNG &&
        img_type != CUSTOM_IMAGE_TYPE_JPEG)
    {
        CUSTOM_AREA4_LOG("show skip, unsupported img_type=%u (only PNG/JPEG accepted)",
                         img_type);
        return 0U;
    }
    if (!img_data || img_size == 0U)
    {
        CUSTOM_AREA4_LOG("show skip, empty payload img_type=%u size=%u",
                         img_type, (unsigned)img_size);
        return 0U;
    }

#if CUSTOM_DISP_IS_ROUND_PROFILE
    /* Host may send JPEG without AA 06; still take the QSPI stream path. */
    if (!custom_disp_stream_is_active())
        custom_disp_stream_acquire();
#endif

    t_show_entry_ms = aic_get_time_ms();
    if (s_stall_drop_until_ms != 0U && t_show_entry_ms < s_stall_drop_until_ms)
    {
        return 0U;
    }
    if (s_stall_drop_until_ms != 0U && t_show_entry_ms >= s_stall_drop_until_ms)
    {
        s_stall_drop_until_ms = 0U;
    }
#if !CUSTOM_DISP_IS_ROUND_PROFILE
    if (s_perf_last_show_entry_ms != 0U &&
        t_show_entry_ms < s_perf_last_show_entry_ms + CUSTOM_AREA4_MIN_INTERVAL_MS)
    {
        return 0U;
    }
    if (s_total_show_count >= CUSTOM_AREA4_GE_PULSE_EVERY &&
        (s_total_show_count - s_last_ge_pulse_show) >= CUSTOM_AREA4_GE_PULSE_EVERY &&
        !s_perf_meas_pending)
    {
        s_last_ge_pulse_show = s_total_show_count;
        CUSTOM_AREA4_LOG("preventive GE reset at show=%u",
                         (unsigned)s_total_show_count);
        custom_area4_ge_soft_reset();
        s_stall_drop_until_ms = t_show_entry_ms + 80U;
        return 0U;
    }
#endif
    t_recv_done_ms = custom_image_get_last_recv_done_ms();
    /*
     * Guard against the first frame (t_recv_done_ms == 0) and any clock-race
     * where show_entry could appear before recv_done — both unlikely but
     * cheap to defend.
     */
    pickup_lat_ms = (t_recv_done_ms != 0U && t_show_entry_ms >= t_recv_done_ms)
                    ? (uint32_t)(t_show_entry_ms - t_recv_done_ms)
                    : 0U;

    /*
     * ----------------------------------------------------------------------
     *  Back-pressure：擋住「上一張還在 LVGL render」就先進來的 show 請求
     * ----------------------------------------------------------------------
     *  s_perf_meas_pending 是既存的 render-in-flight 訊號 (show 入口 arm = 1，
     *  DRAW_POST_END 清 = 0)。直接拿來當 back-pressure gate。
     *
     *  Timeout safety：if arm > 500 ms 還沒清，視為 DRAW_POST_END 漏掉 (例如
     *  obj 被切走 hidden)，force-clear 並讓本張通過，否則 Area4 會永久不更新。
     *  (timeout 從原本 200 ms 拉到 500 ms，見下方詳細註解。)
     */
#if CUSTOM_DISP_IS_ROUND_PROFILE
    /* QSPI stream does not use DRAW_POST_END. A stale pending flag must
     * not block JPEG while USB is still sending. */
    if (custom_disp_stream_is_active() && !custom_disp_stream_can_show())
        return 0U;
    if (custom_disp_stream_is_active())
        s_perf_meas_pending = 0U;
#endif
    if (s_perf_meas_pending)
    {
        uint64_t arm_ms = s_perf_show_entry_ms;
        /*
         * Timeout 從 200 ms 拉到 500 ms：
         *
         * 之前 200 ms 太短，遇到 LVGL UI thread 短暫 busy 跑其他 widget 時就會
         * fire force-clear，讓「上一張仍 in-flight」狀態下又放下一張通過，
         * 結果 CMA 同時握有 2~3 個 MPP frame (2.5 MB × 3 ≈ 7.5 MB 暴衝)。
         * 從 meminfo 看 max_used CMA = 23 MB (baseline 15.5 MB) → 高峰 +7.5 MB
         * 正好對應 3 個 in-flight frame，證明 200 ms timeout 有漏。
         *
         * 真實 render 一張 3~10 ms (HW MPP)；500 ms 大於任何可能的 LVGL pipeline
         * 排程延遲，但仍能在 obj 真的被 hide / draw cb 永久 miss 的場景下恢復。
         */
        if (t_show_entry_ms >= arm_ms &&
            (t_show_entry_ms - arm_ms) < 500U)
        {
            s_backpressure_drop_count++;
            if ((s_backpressure_drop_count % CUSTOM_AREA4_BP_LOG_EVERY) == 0U)
            {
                CUSTOM_AREA4_LOG("back-pressure drop count=%u (PC sending faster than HW can render)",
                                 (unsigned)s_backpressure_drop_count);
            }
            return 0U;
        }
        /*
         * arm stale > 500 ms → 視為 DRAW_POST_END 漏掉或畫面被切走，強制 reset。
         * 這條 path 升級成 warning log (取代之前的 silent 行為)：若真的常常 fire，
         * 代表 LVGL 那邊有問題，需要進一步調查；同時 force-through 仍是必要的，
         * 避免 Area4 永久不更新。
         */
        CUSTOM_AREA4_LOG("back-pressure WARN: stale arm > 500 ms (LVGL render miss?), force-through");
        s_perf_meas_pending = 0U;
    }

    /*
     * ----------------------------------------------------------------------
     *  Option C fast-path: direct FB streaming (custom_disp_stream)
     * ----------------------------------------------------------------------
     *
     *  當 streaming-mode 已 acquire (screen_4_Area 已 active 且 stream 旗標
     *  已開) 時，整套 LVGL widget 路徑全跳過：
     *      decode (MPP) → GE2D rotate → FB → PAN_DISPLAY
     *  預期把 intv 從 ~38 ms 推到 ~13-17 ms。
     *
     *  Acquire / release 由 custom.c 的 screen 切換 callback 控管：
     *      cmd 0x06 → custom_disp_stream_acquire()
     *      其他 cmd → custom_disp_stream_release()
     *
     *  Fallback：若 streaming 失敗 (parse / decoder / GE2D) 回 0，本函式繼續
     *  走原 LVGL widget 路徑，確保「fast-path 出錯時畫面仍會更新」。
     */
    if (custom_disp_stream_is_active())
    {
        uint8_t stream_type = (img_type == CUSTOM_IMAGE_TYPE_JPEG)
                              ? CUSTOM_DISP_STREAM_IMG_JPEG
                              : CUSTOM_DISP_STREAM_IMG_PNG;

        if (custom_disp_stream_show(stream_type, img_data, img_size))
        {
            uint32_t recv_done_now = custom_image_get_recv_done_count();
            uint64_t t_show_exit_ms = aic_get_time_ms();
            uint32_t show_dur_ms = (t_show_exit_ms >= t_show_entry_ms)
                                   ? (uint32_t)(t_show_exit_ms - t_show_entry_ms)
                                   : 0U;

            uint32_t interval_ms = 0U;
            if (s_perf_last_show_entry_ms != 0U &&
                t_show_entry_ms >= s_perf_last_show_entry_ms)
            {
                interval_ms = (uint32_t)(t_show_entry_ms - s_perf_last_show_entry_ms);
                s_perf_interval_acc_ms += interval_ms;
                s_perf_interval_sample_count++;
            }
            s_perf_last_show_entry_ms = t_show_entry_ms;

            CUSTOM_AREA4_VLOG("perf: stream pickup=%ums show=%ums interval=%ums",
                              (unsigned)pickup_lat_ms,
                              (unsigned)show_dur_ms,
                              (unsigned)interval_ms);

            if (!s_perf_initialized)
            {
                s_perf_window_start_ms = t_show_exit_ms;
                s_perf_window_recv_base = recv_done_now;
                s_perf_initialized = 1U;
            }
            s_perf_pickup_acc_ms += pickup_lat_ms;
            s_perf_show_acc_ms += show_dur_ms;
            s_perf_window_show_count++;
            s_total_show_count++;  /* monotonic frame-shown count for freeze watchdog */

            if (s_perf_window_show_count >= CUSTOM_AREA4_PERF_WINDOW)
            {
                uint32_t win_dur = (t_show_exit_ms >= s_perf_window_start_ms)
                                   ? (uint32_t)(t_show_exit_ms - s_perf_window_start_ms)
                                   : 0U;
                uint32_t recv_in_win = (recv_done_now >= s_perf_window_recv_base)
                                       ? (recv_done_now - s_perf_window_recv_base)
                                       : 0U;
                uint32_t drop = (recv_in_win > s_perf_window_show_count)
                                ? (recv_in_win - s_perf_window_show_count)
                                : 0U;
                uint32_t avg_pickup = s_perf_pickup_acc_ms / s_perf_window_show_count;
                uint32_t avg_show   = s_perf_show_acc_ms / s_perf_window_show_count;
                uint32_t avg_interval = s_perf_interval_sample_count
                                        ? (s_perf_interval_acc_ms / s_perf_interval_sample_count)
                                        : 0U;
                uint32_t fps_x100 = win_dur
                                    ? (s_perf_window_show_count * 100000U / win_dur)
                                    : 0U;
                uint32_t fps_interval_x100 = avg_interval
                                             ? (100000U / avg_interval)
                                             : 0U;
                uint32_t drop_pct = recv_in_win
                                    ? (drop * 100U / recv_in_win)
                                    : 0U;

                CUSTOM_AREA4_VLOG("stream stats: %u in %ums fps=%u.%02u (intv=%u.%02u)",
                                 (unsigned)s_perf_window_show_count,
                                 (unsigned)win_dur,
                                 (unsigned)(fps_x100 / 100U), (unsigned)(fps_x100 % 100U),
                                 (unsigned)(fps_interval_x100 / 100U), (unsigned)(fps_interval_x100 % 100U));
                CUSTOM_AREA4_VLOG("stream stats: avg pickup=%u show=%u intv=%u recv=%u drop=%u(%u%%)",
                                 (unsigned)avg_pickup,
                                 (unsigned)avg_show,
                                 (unsigned)avg_interval,
                                 (unsigned)recv_in_win,
                                 (unsigned)drop,
                                 (unsigned)drop_pct);

                s_perf_window_start_ms = t_show_exit_ms;
                s_perf_window_recv_base = recv_done_now;
                s_perf_window_show_count = 0U;
                s_perf_pickup_acc_ms = 0U;
                s_perf_show_acc_ms = 0U;
                s_perf_render_acc_ms = 0U;
                s_perf_render_sample_count = 0U;
                s_perf_interval_acc_ms = 0U;
                s_perf_interval_sample_count = 0U;
            }
            return 1U;
        }
        /*
         * stream show 失敗 → 立刻 release streaming-mode，讓 LVGL 的 disp_flush
         * 重新接管 FB。否則 fallback 到下面的 LVGL widget path 也是死路：
         *   widget path 呼叫 lv_image_set_src + invalidate 後，
         *   LVGL 會 trigger disp_flush，但 streaming flag 還在，
         *   disp_draw_buf_flush 變成 no-op (skip blit + PAN_DISPLAY)，
         *   FB 永遠不會更新，畫面凍結。
         *
         * Release 之後本張 frame 由下方的 LVGL widget path 顯示。
         * 後續再進來時 streaming 已經 0，會持續走 LVGL widget path。
         * 要恢復 streaming 只能透過再切一次 screen (cmd 0x06)。
         */
#if CUSTOM_DISP_IS_ROUND_PROFILE
        if (!custom_disp_stream_can_show())
            return 0U;
        CUSTOM_AREA4_LOG("stream show failed, keep QSPI stream");
        return 1U;
#else
        custom_disp_stream_release();
        CUSTOM_AREA4_LOG("stream show failed → released streaming, fallback to LVGL widget");
#endif
    }

    area4_scr = screen_4_Area_get(&ui_manager);
    if (!area4_scr->obj)
    {
        screen_4_Area_create(&ui_manager);
        area4_scr = screen_4_Area_get(&ui_manager);
    }
    if (!area4_scr->obj)
    {
        CUSTOM_AREA4_LOG("show skip, screen_4_Area not ready");
        return 0U;
    }
    area4_parent = area4_scr->obj;

#if LVGL_VERSION_MAJOR >= 9
    /* 啟用 MPP HW decoder：1280x800 PNG 解碼預期從 ~80 ms 降到 ~10 ms */
    custom_area4_setup_hw_decoder_once();
#endif

    /*
     * 先確保 LVGL 物件已建立。建立失敗時直接離開，舊畫面保持原狀。
     * 預配 CMA buffer 永久持有，所以這個檢查純粹是為了避免「memcpy 進去卻無法顯示」的浪費。
     */
    if (!s_area4_dyn_img_obj || lv_obj_get_parent(s_area4_dyn_img_obj) != area4_parent)
    {
        if (s_area4_dyn_img_obj)
        {
            lv_obj_del(s_area4_dyn_img_obj);
            s_area4_dyn_img_obj = NULL;
        }
#if LVGL_VERSION_MAJOR >= 9
        s_area4_dyn_img_obj = lv_image_create(area4_parent);
        /*
         * Register draw-done callback once per obj lifetime so render-latency
         * timing fires for each subsequent set_src+invalidate. Event-cb stack
         * is per-obj; when the obj is deleted above the prior registration
         * goes with it, so re-registering on the new obj is correct.
         */
        if (s_area4_dyn_img_obj)
        {
            lv_obj_add_event_cb(s_area4_dyn_img_obj, custom_area4_draw_done_cb,
                                LV_EVENT_DRAW_POST_END, NULL);
        }
#else
        s_area4_dyn_img_obj = lv_img_create(area4_parent);
#endif
    }
    if (!s_area4_dyn_img_obj)
    {
        CUSTOM_AREA4_LOG("show skip, lv_image_create failed");
        return 0U;
    }
#if LVGL_VERSION_MAJOR >= 9
    /* leave_display() removes the cb and hides the widget; restore both. */
    lv_obj_remove_event_cb(s_area4_dyn_img_obj, custom_area4_draw_done_cb);
    lv_obj_add_event_cb(s_area4_dyn_img_obj, custom_area4_draw_done_cb,
                        LV_EVENT_DRAW_POST_END, NULL);
#endif
    lv_obj_clear_flag(s_area4_dyn_img_obj, LV_OBJ_FLAG_HIDDEN);

    /*
     * boot 時已預配好的 1 MB CMA buffer 是 Area4 永久 display buffer，
     * 直接 memcpy 進去即可；runtime 不做任何 alloc → 不會 OOM。
     */
    if (img_size > s_area4_disp_buf_cap)
    {
        CUSTOM_AREA4_LOG("show skip, image size %u exceeds prealloc cap %u",
                         (unsigned)img_size, (unsigned)s_area4_disp_buf_cap);
        return 0U;
    }
    if (!custom_area4_copy_into_prealloc(img_data, img_size))
    {
        CUSTOM_AREA4_LOG("show skip, copy into prealloc failed (buf=%p cap=%u size=%u)",
                         (void *)s_area4_disp_buf,
                         (unsigned)s_area4_disp_buf_cap,
                         (unsigned)img_size);
        return 0U;
    }

    /* Double-buffer dsc 以避免下一張圖把仍在解碼的 dsc 覆寫 */
    s_area4_slot_toggle ^= 1U;
    dsc = &s_area4_image_dsc_pool[s_area4_slot_toggle];
    dsc->data = s_area4_disp_buf;
    dsc->data_size = img_size;

#if LVGL_VERSION_MAJOR >= 9
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    /*
     * Flags 設計 (Area3 vs. Area4，三選一)：
     *   USER5 — 強制 SW blend + bypass cache。Area3 用，避開「lodepng SW decode →
     *           buffer 不在 CMA → GE2D MPP_PHY_ADDR 取錯實體位址 → 破圖」。
     *   USER4 — bypass cache only，HW blit 保留。Area4 streaming 用。
     *   0     — 走 LVGL 標準 cache + GE2D HW blit。streaming 30 fps × 3 min 後
     *           rb-tree 在 race 下崩壞 → CPU exc NO.4 / 當機。**已禁用**。
     *
     * 為什麼 Area4 必須 bypass cache：
     *   - src pointer 固定 (s_area4_image_dsc_pool 雙緩衝 toggle)，data 每張改變。
     *   - LVGL 9 cache key 由 (src_type, src) 組成，data 不參與。
     *   - 同一 src 重複 insert → cache 看不出差異 → 累積 stale entry + refcount
     *     race → rb-tree corruption。
     *   - bypass cache 後，MPP decoder 每張 open/close 直接 mpp_dec_data_release
     *     釋放 frame，**完全沒有 rb-tree / refcount race window**。
     *
     * 為什麼選 USER4 而非 USER5：
     *   - USER5 在 lv_draw_ge2d.c 會被 reject (見該檔 ~line 171)，強制走 SW blend，
     *     render 從 ~3 ms 變 ~50 ms，FPS 從 ~25 降到 ~10-15，無法達到 30 fps 目標。
     *   - USER4 只在 lv_image_decoder.c 觸發 args.no_cache = true，
     *     GE2D path 完全不受影響，繼續 HW blit。
     *
     * 如果觀察到 Area4 顯示破圖 / 顏色亂掉，先把這行改回 LV_IMAGE_FLAGS_USER5
     * 即可立刻退回 SW blend (代價：render 多 ~50 ms，FPS 砍半)。
     */
    dsc->header.flags = LV_IMAGE_FLAGS_USER4;
    dsc->header.cf = LV_COLOR_FORMAT_RAW;
    dsc->header.w = 0;
    dsc->header.h = 0;
    dsc->header.stride = 0;
#else
    dsc->header.cf = LV_IMG_CF_RAW_ALPHA;
    dsc->header.always_zero = 0;
    dsc->header.reserved = 0;
    dsc->header.w = 0;
    dsc->header.h = 0;
#endif

    /*
     * 先填入 PNG IHDR 解出的尺寸，讓 LVGL 第一時間有正確 w/h；解碼器拿到後仍會以實際結果覆寫。
     * JPEG 圖檔的尺寸由 MPP 解碼器在 lv_image_decoder_get_info() 內透過 JPEG SOF marker 取得，
     * 不靠這裡的 PNG IHDR 預讀；保持 w/h=0 即可，下方的 lv_image_decoder_get_info 會補齊。
     */
    if (custom_area4_png_get_size(img_data, img_size, &pw, &ph))
    {
        dsc->header.w = pw;
        dsc->header.h = ph;
    }

    src = (const void *)dsc;
    if (!custom_area4_is_image_src_ready(src))
    {
        CUSTOM_AREA4_LOG("show skip, area4 src not ready (size=%u)", (unsigned)img_size);
        return 0U;
    }

#if LVGL_VERSION_MAJOR >= 9
    {
        lv_image_header_t dec_header;
        if (lv_image_decoder_get_info(src, &dec_header) == LV_RESULT_OK)
        {
            CUSTOM_AREA4_VLOG("decode info ok w=%u h=%u cf=%u",
                             (unsigned)dec_header.w, (unsigned)dec_header.h,
                             (unsigned)dec_header.cf);
        }
        else
        {
            CUSTOM_AREA4_LOG("decode info fail size=%u", (unsigned)img_size);
        }
    }
#endif

    /* 固定滿版：與 splash 共用 custom_disp_apply_area4_stream_image */
    custom_disp_apply_area4_stream_image(s_area4_dyn_img_obj);
    lv_obj_move_foreground(s_area4_dyn_img_obj);

#if LVGL_VERSION_MAJOR >= 9
    /*
     * 不再呼叫 lv_image_cache_drop()！
     *
     * 上面 dsc->header.flags = LV_IMAGE_FLAGS_USER4 已讓 lv_image_decoder_open()
     * 在 try_cache 之前就 reject，並設 dsc->args.no_cache = true；MPP decoder
     * 接著也跳過 lv_image_decoder_add_to_cache。整條 path 完全不碰 image cache。
     *
     * 過去版本曾呼 lv_image_cache_drop(src) 或 drop(NULL) 想清掉 stale entry，
     * 但那其實掩蓋了 race；而且 drop_all (即 drop(NULL)) 在 LVGL 9 的
     * lv_cache_lru_rb.c drop_all_cb 會在某些 entry refcount > 0 時直接
     * lv_rb_destroy() / _lv_ll_clear()，留下 dangling pointer，下一次
     * release 又踩到 → 30 fps × 數分鐘後系統 hang (沒有 CPU exception log).
     *
     * USER4 bypass 是真正的修法，不需要再 drop 任何東西。
     */
    lv_image_set_inner_align(s_area4_dyn_img_obj, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_src(s_area4_dyn_img_obj, src);
#else
    lv_img_cache_invalidate_src(src);
    lv_img_set_src(s_area4_dyn_img_obj, NULL);
    lv_img_set_src(s_area4_dyn_img_obj, src);
#endif

    /*
     * Arm render-latency measurement just before triggering redraw so the FIRST
     * subsequent DRAW_POST_END (from this invalidate) consumes the timestamp.
     * Anything queued before this point is already drained.
     */
    s_perf_show_entry_ms = t_show_entry_ms;
    s_perf_meas_pending = 1U;

    lv_obj_invalidate(area4_parent);
    lv_obj_invalidate(s_area4_dyn_img_obj);
    {
        const char *type_name = (img_type == CUSTOM_IMAGE_TYPE_JPEG) ? "JPEG"
                              : (img_type == CUSTOM_IMAGE_TYPE_PNG)  ? "PNG"
                              : (img_type == CUSTOM_IMAGE_TYPE_APNG) ? "APNG"
                              : (img_type == CUSTOM_IMAGE_TYPE_GIF)  ? "GIF"
                              : "UNKNOWN";
        CUSTOM_AREA4_VLOG("show area4 ok type=%s slot=%u size=%u",
                          type_name, slot_byte, (unsigned)img_size);
    }

    /* ---- perf: per-frame + window summary ------------------------------- */
    {
        uint64_t t_show_exit_ms = aic_get_time_ms();
        uint32_t show_dur_ms = (t_show_exit_ms >= t_show_entry_ms)
                               ? (uint32_t)(t_show_exit_ms - t_show_entry_ms)
                               : 0U;
        uint32_t recv_done_now = custom_image_get_recv_done_count();

        /*
         * Show-to-show interval = true effective frame budget.
         * 第一張沒有前次參考 (s_perf_last_show_entry_ms == 0) → 跳過樣本累加。
         * 之後每張都 log + 累加到 window，便於和 first-tile render 對照。
         */
        uint32_t interval_ms = 0U;
        if (s_perf_last_show_entry_ms != 0U &&
            t_show_entry_ms >= s_perf_last_show_entry_ms)
        {
            interval_ms = (uint32_t)(t_show_entry_ms - s_perf_last_show_entry_ms);
            s_perf_interval_acc_ms += interval_ms;
            s_perf_interval_sample_count++;
        }
        s_perf_last_show_entry_ms = t_show_entry_ms;

        CUSTOM_AREA4_VLOG("perf: pickup=%ums show=%ums interval=%ums",
                          (unsigned)pickup_lat_ms, (unsigned)show_dur_ms,
                          (unsigned)interval_ms);

        if (!s_perf_initialized)
        {
            s_perf_window_start_ms = t_show_exit_ms;
            s_perf_window_recv_base = recv_done_now;
            s_perf_initialized = 1U;
        }
        s_perf_pickup_acc_ms += pickup_lat_ms;
        s_perf_show_acc_ms += show_dur_ms;
        s_perf_window_show_count++;
        s_total_show_count++;  /* monotonic frame-shown count for freeze watchdog */

        if (s_perf_window_show_count >= CUSTOM_AREA4_PERF_WINDOW)
        {
            uint32_t win_dur = (t_show_exit_ms >= s_perf_window_start_ms)
                               ? (uint32_t)(t_show_exit_ms - s_perf_window_start_ms)
                               : 0U;
            uint32_t recv_in_win = (recv_done_now >= s_perf_window_recv_base)
                                   ? (recv_done_now - s_perf_window_recv_base)
                                   : 0U;
            uint32_t drop = (recv_in_win > s_perf_window_show_count)
                            ? (recv_in_win - s_perf_window_show_count)
                            : 0U;
            uint32_t avg_pickup = s_perf_pickup_acc_ms / s_perf_window_show_count;
            uint32_t avg_show   = s_perf_show_acc_ms / s_perf_window_show_count;
            uint32_t avg_render = s_perf_render_sample_count
                                  ? (s_perf_render_acc_ms / s_perf_render_sample_count)
                                  : 0U;
            uint32_t avg_interval = s_perf_interval_sample_count
                                    ? (s_perf_interval_acc_ms / s_perf_interval_sample_count)
                                    : 0U;
            /* fps_x100 = shows / (win_dur / 1000) × 100 = shows × 100000 / win_dur */
            uint32_t fps_x100 = win_dur
                                ? (s_perf_window_show_count * 100000U / win_dur)
                                : 0U;
            /*
             * fps_interval = 1000 / avg_interval × 100，這是「實際的 show-to-show」
             * 反推 FPS，跟 fps_x100 (用 window 起迄時間) 在穩態應該一致；不一致代表
             * window 前後有 idle/burst。
             */
            uint32_t fps_interval_x100 = avg_interval
                                         ? (100000U / avg_interval)
                                         : 0U;
            uint32_t drop_pct = recv_in_win
                                ? (drop * 100U / recv_in_win)
                                : 0U;

            /*
             * RT-Thread `rt_kprintf` 內部 buffer = RT_CONSOLEBUF_SIZE (預設 128
             * bytes 含 prefix)。原本單行 ~169 chars 會被截在「render=」之後，
             * 後續欄位完全消失 → 一連串 stats 黏在一起。
             * 拆成兩行 (各 ~80 chars) 才印得完，每行各自 \n flush，也避開
             * 跨行交錯。兩行用同一個 "perf stats" prefix 方便 grep 對齊。
             */
            CUSTOM_AREA4_VLOG("perf stats: %u in %ums fps=%u.%02u (intv=%u.%02u)",
                             (unsigned)s_perf_window_show_count,
                             (unsigned)win_dur,
                             (unsigned)(fps_x100 / 100U), (unsigned)(fps_x100 % 100U),
                             (unsigned)(fps_interval_x100 / 100U), (unsigned)(fps_interval_x100 % 100U));
            CUSTOM_AREA4_VLOG("perf stats: avg pickup=%u show=%u intv=%u render=%u(n=%u) recv=%u drop=%u(%u%%)",
                             (unsigned)avg_pickup,
                             (unsigned)avg_show,
                             (unsigned)avg_interval,
                             (unsigned)avg_render,
                             (unsigned)s_perf_render_sample_count,
                             (unsigned)recv_in_win,
                             (unsigned)drop,
                             (unsigned)drop_pct);

            /*
             * Heap snapshot：每 10 個 perf window (≈ 10 秒) 印一次 system heap
             * 使用率快照。先前出現的「跑 10 分鐘後突然整顆 SoC 卡死、連 uart
             * 都停了」的情境很難重現，提早把 heap 走勢印出來，下次再 hang 就
             * 能比對最後一筆快照是不是 heap free 在劇降。
             *
             * 只看 system heap：
             *   - CMA 用 `meminfo` 隨時查得到 (rt-thread shell 指令)
             *   - system heap 才是之前 NO.6 corruption 的源頭，最關鍵
             *
             * 不每 window 印是因為 rt_memory_info() 內部會 disable IRQ
             * 進 critical zone (短暫 ~us 等級)，不想每秒都打。
             */
            s_perf_heap_log_counter++;
            if (s_perf_heap_log_counter >= CUSTOM_AREA4_HEAP_LOG_EVERY_N_WIN)
            {
#ifdef KERNEL_RTTHREAD
                rt_size_t hs_total = 0, hs_used = 0, hs_max = 0;
                rt_memory_info(&hs_total, &hs_used, &hs_max);
                CUSTOM_AREA4_VLOG("heap snapshot: sys total=%u used=%u max=%u avail=%u (run `meminfo` for CMA)",
                                 (unsigned)hs_total,
                                 (unsigned)hs_used,
                                 (unsigned)hs_max,
                                 (unsigned)(hs_total - hs_used));
#endif
                s_perf_heap_log_counter = 0U;
            }

            /* Reset window for next batch. */
            s_perf_window_start_ms = t_show_exit_ms;
            s_perf_window_recv_base = recv_done_now;
            s_perf_window_show_count = 0U;
            s_perf_pickup_acc_ms = 0U;
            s_perf_show_acc_ms = 0U;
            s_perf_render_acc_ms = 0U;
            s_perf_render_sample_count = 0U;
            s_perf_interval_acc_ms = 0U;
            s_perf_interval_sample_count = 0U;
        }
    }

    return 1U;
}
