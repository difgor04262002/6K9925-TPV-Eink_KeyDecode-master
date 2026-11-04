#include "custom_Image.h"
#include "custom.h"     /* custom_ui_wake_tick_timer() — wake LVGL timer ASAP from USB recv */
/*
 * Area2 / Area3 模組已於 2026-06-16 整批移除（精簡為 Area4-only streaming demo）。
 * custom_Area2.h / custom_Area3.h 不再 include；對應的 try_parse_cmd 條件分支也
 * 一併刪除。任何遺留 0xCD 0xBB 0x04/0x05 frame 會落入下面 fast-path 之外，被
 * 視為未知 cmd 直接 drop。
 */
#include "custom_Area4.h"
#include <stdlib.h>
#include <string.h>
#include "aic_core.h"   /* required for u64 typedef before pulling aic_time.h */
#include "aic_time.h"   /* aic_get_time_ms() for perf instrumentation timestamps */
#include "aic_osal.h"
#ifdef KERNEL_RTTHREAD
#include <rtthread.h>
#include <rtdevice.h>
#include <dfs_posix.h>
#include <dfs_fs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#define CUSTOM_IMG_LOG(fmt, ...) rt_kprintf("custom_image: " fmt "\n", ##__VA_ARGS__)
static struct rt_mutex s_custom_img_bus_mx;
static uint8_t s_custom_img_bus_mx_ready;
#else
#include <stdio.h>
#define CUSTOM_IMG_LOG(fmt, ...) printf("custom_image: " fmt "\n", ##__VA_ARGS__)
#endif

/*
 * CUSTOM_IMG_VERBOSE
 *
 *   0 (default): 關掉「每張影像都會印一次」的 per-frame log
 *                (cmd accepted / magic detected / recv done ok)。
 *                這幾條 rt_kprintf 跑在 USB recv 路徑上，每張 JPEG
 *                3 行 × ~1-3 ms / 行 ≈ 3-9 ms 直接吃掉 frame budget，
 *                跑串流時請務必關掉。
 *   1         : 還原全部 per-frame log，只在排查單張影像細節時開。
 *
 * 異常事件 (recv done fail / cmd accepted but buffer alloc failed /
 * drop ctrl frame / drop nested image cmd) 不受此開關控制，永遠會印。
 */
#ifndef CUSTOM_IMG_VERBOSE
#define CUSTOM_IMG_VERBOSE 0
#endif

#if CUSTOM_IMG_VERBOSE
#define CUSTOM_IMG_VLOG(fmt, ...) CUSTOM_IMG_LOG(fmt, ##__VA_ARGS__)
#else
#define CUSTOM_IMG_VLOG(fmt, ...) ((void)0)
#endif

/*
 * Perf instrumentation:
 *   - s_last_recv_done_ms: ms timestamp of the most recent successful finish_recv.
 *   - s_recv_done_count:   monotonic count of successful finish_recv calls.
 * Read by custom_Area4 to compute pickup latency / drop rate against shown frames.
 * Updated only inside finish_recv() on the success branch.
 *
 * Stream-idle watchdog:
 *   - s_last_stream_activity_ms: any accepted CD-BB / payload / completed frame.
 *     Must be updated inside custom_image_recv (not only at custom_recv_18bytes
 *     entry), otherwise inter-frame gaps or len!=6 bulk chunks look "idle".
 *   - s_frame_interval_ms: EWMA of successive finish_recv gaps; used to turn
 *     "N missed frames" into an idle time budget (default 33 ms ≈ 30 fps).
 */
static volatile uint64_t s_last_recv_done_ms;
static volatile uint32_t s_recv_done_count;
static volatile uint64_t s_last_stream_activity_ms;
static volatile uint32_t s_frame_interval_ms;

enum
{
    CUSTOM_IMAGE_DEFAULT_FRAME_MS = 33U, /* ~30 fps until measured */
    CUSTOM_IMAGE_FRAME_MS_MIN = 16U,     /* clamp: >60 fps bursts */
    CUSTOM_IMAGE_FRAME_MS_MAX = 200U     /* clamp: slow / stalled gaps */
};

static void custom_image_note_stream_activity_internal(void)
{
    s_last_stream_activity_ms = aic_get_time_ms();
}

enum
{
    CUSTOM_IMAGE_CMD_LEN = 6,
    CUSTOM_IMAGE_CTRL_FRAME_LEN = 20,
    /*
     * Recv / ready buffer 固定大小 (1 MB)。
     *
     * 過去用 malloc/realloc/free 動態管理 → 每次「show 成功」會 free 一塊 +
     * 下次 recv 時 malloc 一塊，30 fps × 128 KB 約 3.8 MB/s 的 system heap
     * churn。長時間運行會使 heap 末段碎片化，最後一個 free block 的 metadata
     * 在某次 alloc/split 時被「越界寫入 + walk corrupt pointer」連坐，導致：
     *
     *     CPU Exception: NO.6 (Store misaligned)
     *     a0 = 0x40ffffd0  ← system heap end - 0x30 (最後一個 memheap_item)
     *     mtval = 0x..f60008f  ← 從 corrupt block 讀出的垃圾 pointer
     *
     * 改 CMA 一次性預配 2 個固定 buffer 後：
     *   - System heap 完全不會被這條 path 動到 → 邊界不被觸碰 → corrupt 不再發生
     *   - 2 個 buffer 永久存在；swap 只交換 pointer，**不 alloc / 不 free / 不 memcpy**
     *   - Panel_1..4 各可收 2 MB GIF/APNG；JPEG 串流仍遠小於這個上限
     *   - 4 MB CMA（2 × 2 MB）。D12P CMA 池約 12 MB，須省給 VE/QSPI
     *
     * 任何 >2 MB 的 payload 直接 reject。
     */
    CUSTOM_IMAGE_BUFFER_SIZE = (2 * 1024 * 1024),
#if CUSTOM_FLASH_SIGNAL_ENABLE
    /* Panel_signal may hold a 6 MB GIF. The second buffer stays 2 MB. */
    CUSTOM_IMAGE_MAX_SIZE = CUSTOM_FLASH_SIGNAL_MAX,
#else
    CUSTOM_IMAGE_MAX_SIZE = CUSTOM_IMAGE_BUFFER_SIZE,
#endif
    /*
     * Keep in sync with VENDER_OUT_READ_BUF_LEN in
     * packages/third-party/cherryusb/demo/usbd_vender_template.c.
     *
     * Buffer 演進：4096 → 8192 → 16384 → 32768 → 65536 → 131072。
     * 每次加大都直接砍掉一次 USB callback round-trip + bus_lock 重抓。
     *
     * 128 KB 的設計目標：1280x800 JPEG (q≈80, ~120 KB) **一次 callback** 全收，
     * 減少「兩個 chunk 之間 device 端 re-arm USB read」的 stall (~5-10 ms)。
     *
     * 短封包判定 (len < CUSTOM_USB_CHUNK_FULL) 仍標記 PNG/APNG/GIF/JPEG
     * end-of-transfer；放大 buffer 後 short-packet 仍然會在 PC 不滿一個
     * full chunk 時發出 (USB bulk 短封包語意不變)。
     */
    CUSTOM_USB_CHUNK_FULL = 131072,
    CUSTOM_RECV_MODE_RAW = 0,
    CUSTOM_RECV_MODE_TRANSPORT = 1,
    CUSTOM_RECV_MODE_AUTO = 2
};

static uint8_t s_img_cmd_frame[CUSTOM_IMAGE_CMD_LEN];
static volatile uint8_t s_pending_img_type;
static volatile uint8_t s_pending_img_slot;
static uint8_t s_req_img_type;
static uint8_t s_recv_img_type;
static uint8_t s_recv_target_slot;
static uint8_t s_receiving_image;
static uint32_t s_recv_size;
static uint32_t s_ready_size;
static uint8_t s_ready_img_type;
static uint8_t s_ready_slot;
static uint8_t *s_recv_buf;
static uint32_t s_recv_cap;
static uint8_t *s_ready_buf;
static uint32_t s_ready_cap;
static unsigned s_flash_panel; /* 1..4 from 0xF01..0xF04; 0 = do not save */
static unsigned s_recv_panel;   /* folder latched when this file's first byte arrived */
static uint8_t s_flash_file_arm; /* 1 = next raw GIF/APNG after mmosie is a file */
static unsigned s_xfer_cmd_mask; /* bits 0..3 from FE 0A FA / F1..F4 */
static uint8_t s_fa_seq;         /* FE 0A FA: files follow as Panel_1..4 */
static void custom_flash_recv_use_large(void);
static void custom_flash_recv_release_large(void);
static void custom_flash_object_path(unsigned panel, const char *ext,
                                     char *buf, uint32_t buflen);
static unsigned s_fa_next;
static uint8_t s_flash_play_ok;
static char s_flash_play_lvgl[48];
static uint8_t s_img_hdr_buf[CUSTOM_IMAGE_CTRL_FRAME_LEN];
static uint32_t s_img_hdr_cnt;
static uint32_t s_img_payload_left;
static uint8_t s_recv_mode;

static uint8_t s_tail12[12];
static uint8_t s_tail12_cnt;
static uint8_t s_prev_byte;
static uint8_t s_have_prev_byte;
static uint8_t s_wait_magic;
static uint8_t s_magic_win[8];
static uint8_t s_magic_win_cnt;
static uint8_t s_magic_logged;

static uint8_t custom_image_is_transport_header20(const uint8_t *data)
{
    uint32_t payload_len;

    if (!data)
    {
        return 0U;
    }
    if (data[0] != 0x55U || data[1] != 0x42U)
    {
        return 0U;
    }
    /* type: image/video-frame/video-end */
    if (data[2] > 0x02U)
    {
        return 0U;
    }
    /* reserved bytes in host header builder */
    if (data[16] != 0U || data[17] != 0U || data[18] != 0U || data[19] != 0U)
    {
        return 0U;
    }

    payload_len = ((uint32_t)data[8] << 24) |
                  ((uint32_t)data[9] << 16) |
                  ((uint32_t)data[10] << 8) |
                  ((uint32_t)data[11]);
    if (payload_len > CUSTOM_IMAGE_MAX_SIZE)
    {
        return 0U;
    }
    return 1U;
}

static uint32_t custom_image_parse_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

/*
 * 預配 + 大小檢查替代過去的 malloc/realloc：
 *
 *   - s_recv_buf / s_ready_buf 在 custom_image_init() 一次性從 CMA 配出 2 個
 *     CUSTOM_IMAGE_BUFFER_SIZE buffer，整個 runtime 不再 alloc / realloc / free。
 *   - prepare_buffer() 只是 sanity check，不會真的去 alloc。
 *   - ensure_capacity() / ensure_ready_capacity() 只比較 need_size 跟固定 cap，
 *     payload > cap 就直接 fail (理論上不該發生，因為 transport header 也守了
 *     CUSTOM_IMAGE_MAX_SIZE 上限)。
 *
 * 為什麼要這樣：見 enum 中 CUSTOM_IMAGE_BUFFER_SIZE 的長註解。簡言之 = 消除
 * 每張圖在 system heap 上的 malloc/free 動作，徹底避開「heap 末端 block metadata
 * 被覆寫 → CPU exception NO.6 / NO.4」這條 crash chain。
 */
static uint8_t custom_image_prepare_buffer(void)
{
    /*
     * 預配在 init() 一次完成。如果 init 階段 CMA prealloc 失敗，s_recv_buf
     * 會是 NULL，這裡據此回 0；上層會 log "cmd accepted but buffer alloc failed"
     * 然後就無法接圖了 (排查需求：開機要看 init 階段的 CUSTOM_IMG_LOG)。
     *
     * 不再嘗試在 runtime 補配 (避免引入新的 alloc churn)。
     */
    if (!s_recv_buf || s_recv_cap < CUSTOM_IMAGE_BUFFER_SIZE)
        return 0;
    /* Panel_signal keeps one 6 MB block and no second buffer. */
    if (s_recv_cap >= CUSTOM_FLASH_SIGNAL_MAX)
        return 1;
    if (!s_ready_buf || s_ready_cap < CUSTOM_IMAGE_BUFFER_SIZE)
        return 0;
    return 1;
}

static uint8_t custom_image_ensure_capacity(uint32_t need_size)
{
    /*
     * 純大小檢查，不 realloc。
     * payload > 1 MB 直接 reject — transport header 那層也已經
     * 守了 CUSTOM_IMAGE_MAX_SIZE，所以這條 path 正常不會 fire。
     */
    return (need_size <= s_recv_cap) ? 1U : 0U;
}

static uint8_t custom_flash_buf_has_actl(const uint8_t *buf, uint32_t size)
{
    uint32_t i;

    if (!buf || (size < 8U))
    {
        return 0U;
    }
    for (i = 0U; (i + 4U) <= size; i++)
    {
        if ((buf[i] == 'a') && (buf[i + 1] == 'c') &&
            (buf[i + 2] == 'T') && (buf[i + 3] == 'L'))
        {
            return 1U;
        }
    }
    return 0U;
}

static void custom_flash_save_ready_gif_apng(void);

#ifdef KERNEL_RTTHREAD
static volatile uint8_t s_flash_save_busy;
static volatile uint8_t s_usb_out_hold;
static volatile uint8_t s_save_pending;
static volatile uint8_t s_usb_rearm_hold;
static uint8_t *s_save_ptr;
static uint32_t s_save_len;
static uint8_t s_save_kind;
static unsigned s_save_panel;
static uint8_t *s_pend_ptr;
static uint32_t s_pend_len;
static uint8_t s_pend_kind;
static unsigned s_pend_panel;
static aicos_sem_t s_save_sem;
static aicos_thread_t s_save_thread;
static uint8_t s_save_prio;
static uint8_t s_save_thread_ok;
static volatile unsigned s_purge_mask;
static volatile uint8_t s_purge_acks;
static volatile uint8_t s_purge_done_n;
static volatile uint8_t s_save_kick;

uint8_t custom_image_flash_save_busy(void)
{
    return s_flash_save_busy;
}

uint8_t custom_image_save_pending(void)
{
    return s_save_pending;
}

uint8_t custom_image_usb_rearm_hold(void)
{
    return s_usb_rearm_hold;
}

void custom_image_usb_hold_rearm(void)
{
    s_usb_rearm_hold = 1U;
}

uint8_t custom_image_usb_out_held(void)
{
    return (s_usb_out_hold || s_flash_save_busy) ? 1U : 0U;
}

void custom_image_usb_out_hold_for_loadin(void)
{
    s_usb_out_hold = 1U;
}

void custom_image_usb_out_release(void)
{
    /* Do not start a USB read from here. The OUT callback owns re-arm;
     * a second start_read cancels the file body. */
    s_usb_out_hold = 0U;
}

#else
uint8_t custom_image_flash_save_busy(void)
{
    return 0U;
}

uint8_t custom_image_save_pending(void)
{
    return 0U;
}

uint8_t custom_image_usb_rearm_hold(void)
{
    return 0U;
}

void custom_image_usb_hold_rearm(void)
{
}

uint8_t custom_image_usb_out_held(void)
{
    return 0U;
}

void custom_image_usb_out_hold_for_loadin(void)
{
}

void custom_image_usb_out_release(void)
{
}
#endif

static int custom_flash_write_all(int fd, const uint8_t *ptr, uint32_t len,
                                   rt_device_t wdt)
{
    uint32_t off = 0U;

    while (off < len)
    {
        int n;
        uint32_t chunk = len - off;

        if (chunk > 65536U)
            chunk = 65536U;
        if (wdt)
            rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
        n = write(fd, ptr + off, chunk);
        if (n <= 0)
            return -1;
        off += (uint32_t)n;
        /* The tail after 4MB used to sit here with no further line.
         * One line per 512KB shows the NOR write is still moving. */
        if ((off & 0x3FFFFU) == 0U || off >= len)
            CUSTOM_IMG_LOG("Panel_signal writing %u/%u",
                           (unsigned)off, (unsigned)len);
        aicos_msleep(1);
    }
    return 0;
}

/* The 6 MB block is only for the file being received. Keeping it from boot
 * left no contiguous CMA for the 2 MB GIF blob, and the carousel retried
 * forever. Grow on arm, give it back after the save. */
static void custom_flash_recv_use_large(void)
{
#if CUSTOM_FLASH_SIGNAL_ENABLE
    uint8_t *big;
    uint8_t *old;

    if (s_receiving_image)
        return;
    /* The boot GIF does not use this buffer. Dropping a stale frame lets
     * the two 2 MB blocks be freed when the 6 MB alloc has to be retried. */
    if ((s_ready_buf != s_save_ptr) && (s_ready_buf != s_pend_ptr))
    {
        s_ready_size = 0U;
        s_ready_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    }
    if (s_ready_cap > s_recv_cap)
    {
        uint8_t *tmp = s_ready_buf;
        uint32_t cap = s_ready_cap;

        s_ready_buf = s_recv_buf;
        s_ready_cap = s_recv_cap;
        s_recv_buf = tmp;
        s_recv_cap = cap;
    }
    if (s_recv_cap >= CUSTOM_FLASH_SIGNAL_MAX && s_recv_buf)
        return;
    /* Do not free the 2 MB pair. The boot GIF sits in the middle of CMA,
     * so those 4 MB are not one 6 MB block. Freeing them stopped the
     * first chunk and the glass. */
    if (s_save_ptr || s_pend_ptr)
        return;
    big = (uint8_t *)aicos_malloc_align(MEM_CMA, CUSTOM_FLASH_SIGNAL_MAX,
                                        CACHE_LINE_SIZE);
    if (!big)
    {
        CUSTOM_IMG_LOG("Panel_signal %u alloc fail, write in %u-byte chunks",
                       (unsigned)CUSTOM_FLASH_SIGNAL_MAX, (unsigned)s_recv_cap);
        return;
    }
    old = s_recv_buf;
    s_recv_buf = big;
    s_recv_cap = CUSTOM_FLASH_SIGNAL_MAX;
    if (old && old != s_ready_buf && old != s_save_ptr && old != s_pend_ptr)
        aicos_free_align(MEM_CMA, old);
    CUSTOM_IMG_LOG("Panel_signal recv %u bytes", (unsigned)s_recv_cap);
#endif
}

static void custom_flash_recv_release_large(void)
{
#if CUSTOM_FLASH_SIGNAL_ENABLE
    uint8_t *old;
    uint8_t *small;

    if (s_receiving_image || s_save_pending)
        return;
    if (s_ready_cap > CUSTOM_IMAGE_BUFFER_SIZE && s_ready_cap > s_recv_cap)
    {
        uint8_t *tmp = s_ready_buf;
        uint32_t cap = s_ready_cap;

        s_ready_buf = s_recv_buf;
        s_ready_cap = s_recv_cap;
        s_recv_buf = tmp;
        s_recv_cap = cap;
    }
    if (s_recv_cap <= CUSTOM_IMAGE_BUFFER_SIZE)
        return;
    if (!s_recv_buf || s_recv_buf == s_save_ptr || s_recv_buf == s_pend_ptr ||
        s_recv_buf == s_ready_buf)
        return;
    old = s_recv_buf;
    s_recv_buf = NULL;
    s_recv_cap = 0U;
    if (old)
        aicos_free_align(MEM_CMA, old);
    if (s_ready_buf && s_ready_buf != s_save_ptr && s_ready_buf != s_pend_ptr &&
        s_ready_cap > CUSTOM_IMAGE_BUFFER_SIZE)
    {
        aicos_free_align(MEM_CMA, s_ready_buf);
        s_ready_buf = NULL;
        s_ready_cap = 0U;
    }
    small = (uint8_t *)aicos_malloc_align(MEM_CMA, CUSTOM_IMAGE_BUFFER_SIZE,
                                          CACHE_LINE_SIZE);
    if (!small)
    {
        CUSTOM_IMG_LOG("recv 2MB restore fail");
        return;
    }
    s_recv_buf = small;
    s_recv_cap = CUSTOM_IMAGE_BUFFER_SIZE;
    if (!s_ready_buf)
    {
        s_ready_buf = (uint8_t *)aicos_malloc_align(MEM_CMA,
                                                    CUSTOM_IMAGE_BUFFER_SIZE,
                                                    CACHE_LINE_SIZE);
        s_ready_cap = s_ready_buf ? CUSTOM_IMAGE_BUFFER_SIZE : 0U;
    }
    CUSTOM_IMG_LOG("Panel_signal recv back to %u bytes", (unsigned)s_recv_cap);
#endif
}

/* The two 2 MB blocks sit in the middle of the 14 MB CMA. The free tail
 * after them is under 5 MB, so a 5.8 MB GIF never fits until they are
 * given back. USB OUT uses its own SRAM buffer, not these. */
void custom_image_park_usb_ram(void)
{
    if (s_receiving_image || s_save_pending || s_save_ptr || s_pend_ptr ||
        custom_image_signal_busy() || custom_image_spill_pending() ||
        custom_image_flash_save_busy())
        return;
    if (s_recv_buf) {
        aicos_free_align(MEM_CMA, s_recv_buf);
        s_recv_buf = NULL;
        s_recv_cap = 0U;
    }
    if (s_ready_buf) {
        aicos_free_align(MEM_CMA, s_ready_buf);
        s_ready_buf = NULL;
        s_ready_cap = 0U;
        s_ready_size = 0U;
    }
    CUSTOM_IMG_LOG("usb ram parked");
}

void *custom_image_take_play_ram(uint32_t sz)
{
    void *p;

    if (sz == 0U)
        return NULL;
    custom_image_park_usb_ram();
    p = aicos_malloc(MEM_CMA, sz);
    if (!p)
        p = aicos_malloc_try_cma(sz);
    if (!p)
        CUSTOM_IMG_LOG("play ram alloc fail %u", (unsigned)sz);
    return p;
}

void custom_image_give_play_ram(void *p)
{
    if (p)
        aicos_free(MEM_CMA, p);
}

void custom_image_restore_usb_ram(void)
{
    if (!s_recv_buf) {
        s_recv_buf = (uint8_t *)aicos_malloc_align(MEM_CMA,
                                                   CUSTOM_IMAGE_BUFFER_SIZE,
                                                   CACHE_LINE_SIZE);
        s_recv_cap = s_recv_buf ? CUSTOM_IMAGE_BUFFER_SIZE : 0U;
    }
    if (!s_ready_buf) {
        s_ready_buf = (uint8_t *)aicos_malloc_align(MEM_CMA,
                                                    CUSTOM_IMAGE_BUFFER_SIZE,
                                                    CACHE_LINE_SIZE);
        s_ready_cap = s_ready_buf ? CUSTOM_IMAGE_BUFFER_SIZE : 0U;
    }
    CUSTOM_IMG_LOG("usb ram restore recv=%p %u ready=%p %u",
                   (void *)s_recv_buf, (unsigned)s_recv_cap,
                   (void *)s_ready_buf, (unsigned)s_ready_cap);
}

uint8_t custom_image_read_path(const char *path, uint8_t *dst, uint32_t cap,
                               uint32_t *out)
{
#ifdef KERNEL_RTTHREAD
    int fd;
    uint32_t off = 0U;
    rt_device_t wdt;

    if (out)
        *out = 0U;
    if (!path || !dst || cap == 0U)
        return 0U;
    wdt = rt_device_find("wdt");
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0U;
    while (off < cap) {
        uint32_t chunk = cap - off;
        int n;

        if (chunk > 65536U)
            chunk = 65536U;
        n = read(fd, dst + off, chunk);
        if (n <= 0)
            break;
        off += (uint32_t)n;
        if (wdt)
            rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
    }
    close(fd);
    if (out)
        *out = off;
    return (off == cap) ? 1U : 0U;
#else
    (void)path;
    (void)dst;
    (void)cap;
    if (out)
        *out = 0U;
    return 0U;
#endif
}

static void custom_flash_object_path(unsigned panel, const char *ext,
                                     char *buf, uint32_t buflen)
{
    const char *dir;

    if (!buf || !buflen || !ext)
        return;
    buf[0] = '\0';
    dir = custom_flash_panel_dir(panel);
    if (!dir)
        return;
#if CUSTOM_FLASH_SIGNAL_ENABLE
    if (panel == 1U)
    {
#ifdef KERNEL_RTTHREAD
        rt_snprintf(buf, buflen, "%s/Panel_signal.%s", dir, ext);
#else
        snprintf(buf, buflen, "%s/Panel_signal.%s", dir, ext);
#endif
        return;
    }
#endif
#ifdef KERNEL_RTTHREAD
    rt_snprintf(buf, buflen, "%s/Panel_%03u.%s", dir, panel, ext);
#else
    snprintf(buf, buflen, "%s/Panel_%03u.%s", dir, panel, ext);
#endif
}

/* close() returns before the directory entry is on NAND. Power-off
 * then kept Panel_signal.gif and dropped Panel_signal.gif.2. */
#ifdef KERNEL_RTTHREAD
static void custom_flash_fsync_fd(int fd)
{
    if (fd >= 0)
        (void)fsync(fd);
}

static void custom_flash_fsync_path(const char *path)
{
    int fd;

    if (!path || path[0] == '\0')
        return;
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return;
    (void)fsync(fd);
    close(fd);
}
#endif

static void custom_flash_save_ready_gif_apng(void)
{
#ifdef KERNEL_RTTHREAD
    const char *dir;
    const char *ext;
    char path[64];
    char neu[72];
    char stale[64];
    int fd;
    int inplace = 0;
    rt_device_t wdt;

    s_flash_play_ok = 0U;

    if ((s_save_panel < 1U) || (s_save_panel > CUSTOM_FLASH_PANEL_COUNT))
    {
        return;
    }
    if ((s_save_kind != CUSTOM_IMAGE_TYPE_GIF) &&
        (s_save_kind != CUSTOM_IMAGE_TYPE_APNG))
    {
        return;
    }
    if ((s_save_kind == CUSTOM_IMAGE_TYPE_APNG) &&
        !custom_flash_buf_has_actl(s_save_ptr, s_save_len))
    {
        CUSTOM_IMG_LOG("flash skip PNG without acTL (%u bytes)",
                       (unsigned)s_save_len);
        return;
    }
    if (!s_save_ptr || (s_save_len == 0U))
    {
        return;
    }

    dir = custom_flash_panel_dir(s_save_panel);
    if (!dir)
    {
        return;
    }
    ext = (s_save_kind == CUSTOM_IMAGE_TYPE_GIF) ? "gif" : "apng";
    custom_flash_object_path(s_save_panel, ext, path, sizeof(path));
    if (path[0] == '\0')
        return;

    rt_snprintf(stale, sizeof(stale), "%s/recv.gif", dir);
    (void)unlink(stale);
    rt_snprintf(stale, sizeof(stale), "%s/recv.apng", dir);
    (void)unlink(stale);
    custom_flash_object_path(s_save_panel,
                             (s_save_kind == CUSTOM_IMAGE_TYPE_GIF) ? "apng" : "gif",
                             stale, sizeof(stale));
    (void)unlink(stale);
    /* FE 0A already removed the live GIF. Program the final name
     * directly so NOR is not written twice. */
    {
        struct stat st;

        if (stat(path, &st) != 0)
        {
            uint64_t t0 = aic_get_time_ms();

            wdt = rt_device_find("wdt");
            fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (fd < 0)
            {
                CUSTOM_IMG_LOG("flash save open fail %s errno=%d", path, errno);
                return;
            }
            CUSTOM_IMG_LOG("flash writing %s (%u bytes)", path,
                           (unsigned)s_save_len);
            if (custom_flash_write_all(fd, s_save_ptr, s_save_len, wdt) != 0)
            {
                CUSTOM_IMG_LOG("flash save write fail %s errno=%d", path, errno);
                close(fd);
                (void)unlink(path);
                return;
            }
            custom_flash_fsync_fd(fd);
            close(fd);
            rt_snprintf(s_flash_play_lvgl, sizeof(s_flash_play_lvgl), "L:%s", path);
            s_flash_play_ok = 1U;
            CUSTOM_IMG_LOG("flash saved %s (%u bytes, %u ms)", path,
                           (unsigned)s_save_len,
                           (unsigned)(aic_get_time_ms() - t0));
            custom_round_note_panel_saved(s_save_panel);
            return;
        }
    }
    /* Old file is still there. Write beside it and rename after. */
    rt_snprintf(neu, sizeof(neu), "%s.new", path);
    (void)unlink(neu);

    wdt = rt_device_find("wdt");
    fd = open(neu, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        CUSTOM_IMG_LOG("flash save open fail %s errno=%d", neu, errno);
        inplace = 1;
    }
    else
    {
        uint64_t t0 = aic_get_time_ms();

        CUSTOM_IMG_LOG("flash writing %s (%u bytes)", neu, (unsigned)s_save_len);
        if (custom_flash_write_all(fd, s_save_ptr, s_save_len, wdt) != 0)
        {
            CUSTOM_IMG_LOG("flash save write fail %s errno=%d", neu, errno);
            close(fd);
            (void)unlink(neu);
            inplace = 1;
        }
        else
        {
            custom_flash_fsync_fd(fd);
            close(fd);
            if (rename(neu, path) != 0)
            {
                CUSTOM_IMG_LOG("flash rename fail %s -> %s errno=%d",
                               neu, path, errno);
                (void)unlink(neu);
                return;
            }
            custom_flash_fsync_path(path);
            rt_snprintf(s_flash_play_lvgl, sizeof(s_flash_play_lvgl), "L:%s", path);
            s_flash_play_ok = 1U;
            CUSTOM_IMG_LOG("flash saved %s (%u bytes, %u ms)", path,
                           (unsigned)s_save_len,
                           (unsigned)(aic_get_time_ms() - t0));
        }
    }
    if (inplace)
    {
        uint64_t t0 = aic_get_time_ms();

        /* No room for the old file and the new one. The old GIF is
         * removed only in this case. */
        CUSTOM_IMG_LOG("flash full, replace in place %s", path);
        (void)unlink(path);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0)
        {
            CUSTOM_IMG_LOG("flash save open fail %s errno=%d", path, errno);
            return;
        }
        if (custom_flash_write_all(fd, s_save_ptr, s_save_len, wdt) != 0)
        {
            CUSTOM_IMG_LOG("flash save write fail %s errno=%d", path, errno);
            close(fd);
            (void)unlink(path);
            return;
        }
        custom_flash_fsync_fd(fd);
        close(fd);
        rt_snprintf(s_flash_play_lvgl, sizeof(s_flash_play_lvgl), "L:%s", path);
        s_flash_play_ok = 1U;
        CUSTOM_IMG_LOG("flash saved %s (%u bytes, %u ms)", path,
                       (unsigned)s_save_len,
                       (unsigned)(aic_get_time_ms() - t0));
    }
    custom_round_note_panel_saved(s_save_panel);
#endif
}

#ifdef KERNEL_RTTHREAD
static uint8_t custom_flash_save_needed(void)
{
    if ((s_save_panel < 1U) || (s_save_panel > CUSTOM_FLASH_PANEL_COUNT))
        return 0U;
    if ((s_save_kind != CUSTOM_IMAGE_TYPE_GIF) &&
        (s_save_kind != CUSTOM_IMAGE_TYPE_APNG))
        return 0U;
    if ((s_save_kind == CUSTOM_IMAGE_TYPE_APNG) &&
        !custom_flash_buf_has_actl(s_save_ptr, s_save_len))
        return 0U;
    if (!s_save_ptr || (s_save_len == 0U))
        return 0U;
    return 1U;
}

extern void usbd_vender_out_rearm(void);

#if !CUSTOM_FLASH_SIGNAL_ENABLE
uint8_t custom_image_spill_pending(void)
{
    return 0U;
}

uint8_t custom_image_signal_busy(void)
{
    return 0U;
}

void custom_image_spill_service(void)
{
}
#endif

static uint32_t s_panel_gif_sz[5];
static uint8_t s_panel_gif_sz_known[5];
static volatile uint8_t s_nand_bus;
static volatile uint8_t s_load_abort;

void custom_image_load_cancel(void)
{
    s_load_abort = 1U;
}

void custom_image_load_resume(void)
{
    s_load_abort = 0U;
}

void custom_nand_bus_enter(void)
{
    if (s_nand_bus < 250U)
        s_nand_bus++;
}

void custom_nand_bus_leave(void)
{
    if (s_nand_bus)
        s_nand_bus--;
}

uint8_t custom_nand_bus_busy(void)
{
    return s_nand_bus ? 1U : 0U;
}

#if CUSTOM_FLASH_SIGNAL_ENABLE
/* A 6 MB CMA block is not available while the boot GIF and the two 2 MB
 * image buffers are live. Park a full buffer, append it on this thread,
 * and hold the next USB OUT until that chunk is on flash. */
/* ISR (USB) writes these while flsave reads them. They must be volatile:
 * a plain static can stay NULL in a register and the second 2MB chunk
 * is never written. */
static uint8_t * volatile s_spill_ptr;
static volatile uint32_t s_spill_len;
static uint8_t * volatile s_spill_tail_ptr;
static volatile uint32_t s_spill_tail_len;
static volatile uint8_t s_spill_kick;
static volatile uint8_t s_spill_final;
static volatile uint8_t s_spill_abort;
static volatile uint8_t s_spill_active;
static volatile uint8_t s_spill_session;
static volatile uint8_t s_spill_need_rearm;
static uint32_t s_spill_total;
static unsigned s_spill_panel;
static uint8_t s_spill_kind;
static int s_spill_fd = -1;
/* A write that starts at file offset 4194304 does not return. Each
 * piece is its own file and never grows past 2MB. */
#define CUSTOM_SPILL_SEG (2U * 1024U * 1024U)
static uint8_t s_spill_seg;

static const char *custom_signal_seg_ext(uint8_t seg, uint8_t apng, uint8_t part)
{
    if (part)
    {
        if (apng)
        {
            if (seg == 1U) return "apng.part2";
            if (seg == 2U) return "apng.part3";
            if (seg >= 3U) return "apng.part4";
            return "apng.part";
        }
        if (seg == 1U) return "gif.part2";
        if (seg == 2U) return "gif.part3";
        if (seg >= 3U) return "gif.part4";
        return "gif.part";
    }
    if (apng)
    {
        if (seg == 1U) return "apng.2";
        if (seg == 2U) return "apng.3";
        if (seg >= 3U) return "apng.4";
        return "apng";
    }
    if (seg == 1U) return "gif.2";
    if (seg == 2U) return "gif.3";
    if (seg >= 3U) return "gif.4";
    return "gif";
}

uint8_t custom_image_spill_pending(void)
{
    return (s_spill_ptr || s_spill_tail_ptr) ? 1U : 0U;
}

uint8_t custom_image_signal_busy(void)
{
    return (s_spill_session || s_spill_active || s_spill_final ||
            s_spill_kick || s_spill_ptr || s_spill_tail_ptr) ? 1U : 0U;
}

static int custom_signal_spill_append(const uint8_t *ptr, uint32_t len)
{
    char path[64];
    rt_device_t wdt;
    const char *ext;

    if (!ptr || len == 0U)
        return 0;
    if ((s_spill_total + len) > CUSTOM_FLASH_SIGNAL_MAX)
    {
        CUSTOM_IMG_LOG("Panel_signal over 8MB");
        return -1;
    }
    CUSTOM_IMG_LOG("Panel_signal write %u bytes", (unsigned)len);
    /* Do not drop priority here. The tail already has part3 open.
     * Sleeping behind the glass meant this thread never reached
     * flash writing. */
    /* One append can cross a 2MB edge. The next piece is a new file. */
    if (s_spill_total < ((uint32_t)s_spill_seg + 1U) * CUSTOM_SPILL_SEG &&
        (s_spill_total + len) > ((uint32_t)s_spill_seg + 1U) * CUSTOM_SPILL_SEG)
    {
        uint32_t n = ((uint32_t)s_spill_seg + 1U) * CUSTOM_SPILL_SEG - s_spill_total;

        if (custom_signal_spill_append(ptr, n) != 0)
            return -1;
        return custom_signal_spill_append(ptr + n, len - n);
    }
    ext = custom_signal_seg_ext(s_spill_seg,
                                s_spill_kind == CUSTOM_IMAGE_TYPE_APNG, 1U);
    custom_flash_object_path(s_spill_panel, ext, path, sizeof(path));
    if (path[0] == '\0')
        return -1;
    wdt = rt_device_find("wdt");
    if (s_spill_fd >= 0 && s_spill_total == 0U)
    {
        close(s_spill_fd);
        s_spill_fd = -1;
    }
    if (s_spill_fd < 0)
    {
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        uint32_t local = s_spill_total - (uint32_t)s_spill_seg * CUSTOM_SPILL_SEG;

        /* Seeking to 4MB and writing there does not return. */
        if (local != 0U)
        {
            CUSTOM_IMG_LOG("flash save no fd at %u", (unsigned)s_spill_total);
            return -1;
        }
        (void)unlink(path);
        s_spill_fd = open(path, flags, 0666);
        if (s_spill_fd < 0)
        {
            CUSTOM_IMG_LOG("flash save open fail %s errno=%d", path, errno);
            return -1;
        }
        s_spill_active = 1U;
        CUSTOM_IMG_LOG("flash writing %s at 0 file %u", path,
                       (unsigned)s_spill_total);
    }
    else
        CUSTOM_IMG_LOG("flash writing %s at %u file %u", path,
                       (unsigned)(s_spill_total - (uint32_t)s_spill_seg * CUSTOM_SPILL_SEG),
                       (unsigned)s_spill_total);
    /* Keep this fd at the end of the part. Closing and seeking back to
     * 4MB makes the next write rebuild the block list one byte at a time
     * and never return. */
    if (custom_flash_write_all(s_spill_fd, ptr, len, wdt) != 0)
    {
        CUSTOM_IMG_LOG("flash save write fail %s errno=%d", path, errno);
        close(s_spill_fd);
        s_spill_fd = -1;
        return -1;
    }
    s_spill_total += len;
    CUSTOM_IMG_LOG("Panel_signal stored %u", (unsigned)s_spill_total);
    /* This piece is full. Close it and open the next file at offset 0
     * before waiting for more bytes. Opening that file later, after a
     * 4MB CTZ exists, does not return. */
    if (s_spill_fd >= 0 && s_spill_seg < 3U &&
        (s_spill_total % CUSTOM_SPILL_SEG) == 0U)
    {
        char nxt[64];
        const char *next_ext;

        CUSTOM_IMG_LOG("Panel_signal seg %u close", (unsigned)s_spill_seg);
        custom_flash_fsync_fd(s_spill_fd);
        close(s_spill_fd);
        s_spill_fd = -1;
        s_spill_seg++;
        CUSTOM_IMG_LOG("Panel_signal seg %u closed", (unsigned)s_spill_seg);
        next_ext = custom_signal_seg_ext(s_spill_seg,
                                         s_spill_kind == CUSTOM_IMAGE_TYPE_APNG, 1U);
        custom_flash_object_path(s_spill_panel, next_ext, nxt, sizeof(nxt));
        if (nxt[0] == '\0')
            return -1;
        (void)unlink(nxt);
        s_spill_fd = open(nxt, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (s_spill_fd < 0)
        {
            CUSTOM_IMG_LOG("seg open fail %s errno=%d", nxt, errno);
            return -1;
        }
        CUSTOM_IMG_LOG("Panel_signal seg %u ready", (unsigned)s_spill_seg);
    }
    return 0;
}

static void custom_signal_spill_close(uint8_t unlink_file)
{
    char path[64];
    const char *ext;

    if (s_spill_fd >= 0)
    {
        close(s_spill_fd);
        s_spill_fd = -1;
    }
    ext = (s_spill_kind == CUSTOM_IMAGE_TYPE_APNG) ? "apng.part" : "gif.part";
    custom_flash_object_path(s_spill_panel ? s_spill_panel : 1U, ext,
                             path, sizeof(path));
    if (unlink_file && path[0] != '\0')
        (void)unlink(path);
    ext = custom_signal_seg_ext(1U, s_spill_kind == CUSTOM_IMAGE_TYPE_APNG, 1U);
    custom_flash_object_path(s_spill_panel ? s_spill_panel : 1U, ext,
                             path, sizeof(path));
    if (unlink_file && path[0] != '\0')
        (void)unlink(path);
    ext = custom_signal_seg_ext(2U, s_spill_kind == CUSTOM_IMAGE_TYPE_APNG, 1U);
    custom_flash_object_path(s_spill_panel ? s_spill_panel : 1U, ext,
                             path, sizeof(path));
    if (unlink_file && path[0] != '\0')
        (void)unlink(path);
    ext = custom_signal_seg_ext(3U, s_spill_kind == CUSTOM_IMAGE_TYPE_APNG, 1U);
    custom_flash_object_path(s_spill_panel ? s_spill_panel : 1U, ext,
                             path, sizeof(path));
    if (unlink_file && path[0] != '\0')
        (void)unlink(path);
    s_spill_seg = 0U;
    s_spill_ptr = NULL;
    s_spill_len = 0U;
    s_spill_tail_ptr = NULL;
    s_spill_tail_len = 0U;
    s_spill_final = 0U;
    s_spill_active = 0U;
    s_spill_session = 0U;
    s_spill_total = 0U;
    s_spill_abort = 0U;
}

static void custom_signal_spill_rearm(void)
{
    if (!s_spill_need_rearm && !s_usb_rearm_hold)
        return;
    s_spill_need_rearm = 0U;
    s_usb_rearm_hold = 0U;
    usbd_vender_out_rearm();
}


static void custom_signal_spill_fail(void)
{
    custom_signal_spill_close(1U);
    custom_signal_spill_rearm();
}

static uint8_t custom_signal_park_full(uint32_t extra);

static void custom_signal_spill_run(void)
{
    unsigned wait_n = 0U;

    for (;;)
    {
        uint8_t need_rearm = s_spill_need_rearm;
        uint8_t *chunk;
        uint32_t chunk_len;
        rt_base_t lvl;

        lvl = rt_hw_interrupt_disable();
        chunk = s_spill_ptr;
        chunk_len = s_spill_len;
        if (chunk && chunk_len)
        {
            s_spill_ptr = NULL;
            s_spill_len = 0U;
            s_spill_kick = 0U;
        }
        rt_hw_interrupt_enable(lvl);

        if (s_spill_abort)
        {
            custom_signal_spill_close(1U);
            CUSTOM_IMG_LOG("Panel_signal spill aborted");
            custom_signal_spill_rearm();
            return;
        }
        if (chunk && chunk_len)
        {
            if (custom_signal_spill_append(chunk, chunk_len) != 0)
            {
                custom_signal_spill_fail();
                return;
            }
            /* This chunk is closed. The other buffer may already be full. */
            s_spill_active = 0U;
        }
        else if (!s_spill_ptr)
            s_spill_kick = 0U;
        if (s_spill_tail_ptr && s_spill_tail_len)
        {
            uint8_t *tail = s_spill_tail_ptr;
            uint32_t tail_len = s_spill_tail_len;

            /* Drop the flag before the NOR write. Leaving it set made the
             * GIF thread sit on the filesystem lock for the whole tail. */
            s_spill_tail_ptr = NULL;
            s_spill_tail_len = 0U;
            if (custom_signal_spill_append(tail, tail_len) != 0)
            {
                custom_signal_spill_fail();
                return;
            }
        }
        if (s_spill_final && s_spill_tail_ptr && s_spill_tail_len)
            continue;
        if (s_spill_final)
        {
            char part[64];
            char dest[64];
            const char *pext;
            const char *dext;

            pext = (s_spill_kind == CUSTOM_IMAGE_TYPE_APNG) ? "apng.part" : "gif.part";
            dext = (s_spill_kind == CUSTOM_IMAGE_TYPE_APNG) ? "apng" : "gif";
            custom_flash_object_path(s_spill_panel, pext, part, sizeof(part));
            custom_flash_object_path(s_spill_panel, dext, dest, sizeof(dest));
            if (s_spill_fd >= 0)
            {
                custom_flash_fsync_fd(s_spill_fd);
                close(s_spill_fd);
                s_spill_fd = -1;
            }
            if (s_spill_seg > 0U &&
                s_spill_total == (uint32_t)s_spill_seg * CUSTOM_SPILL_SEG)
            {
                char extra[64];
                uint8_t apng = (s_spill_kind == CUSTOM_IMAGE_TYPE_APNG) ? 1U : 0U;

                custom_flash_object_path(s_spill_panel,
                                        custom_signal_seg_ext(s_spill_seg, apng, 1U),
                                        extra, sizeof(extra));
                if (extra[0] != '\0')
                    (void)unlink(extra);
                s_spill_seg--;
            }
            if (dest[0] != '\0' && part[0] != '\0')
            {
                uint8_t seg;
                uint8_t apng = (s_spill_kind == CUSTOM_IMAGE_TYPE_APNG) ? 1U : 0U;
                uint8_t ok = 1U;

                for (seg = 0U; seg < 4U; seg++)
                {
                    char gone[64];

                    custom_flash_object_path(s_spill_panel,
                                            custom_signal_seg_ext(seg, apng, 0U),
                                            gone, sizeof(gone));
                    if (gone[0] != '\0')
                        (void)unlink(gone);
                }
                if (rename(part, dest) != 0)
                {
                    CUSTOM_IMG_LOG("flash rename fail %s -> %s errno=%d",
                                   part, dest, errno);
                    ok = 0U;
                }
                else
                    custom_flash_fsync_path(dest);
                for (seg = 1U; ok && seg <= s_spill_seg; seg++)
                {
                    char src[64];
                    char dst[64];

                    custom_flash_object_path(s_spill_panel,
                                            custom_signal_seg_ext(seg, apng, 1U),
                                            src, sizeof(src));
                    custom_flash_object_path(s_spill_panel,
                                            custom_signal_seg_ext(seg, apng, 0U),
                                            dst, sizeof(dst));
                    if (src[0] == '\0' || dst[0] == '\0' || rename(src, dst) != 0)
                    {
                        CUSTOM_IMG_LOG("flash rename fail seg %u errno=%d",
                                       (unsigned)seg, errno);
                        (void)unlink(dest);
                        ok = 0U;
                    }
                    else
                        custom_flash_fsync_path(dst);
                }
                if (ok)
                {
                    if (s_spill_panel <= 4U) {
                        s_panel_gif_sz[s_spill_panel] = s_spill_total;
                        s_panel_gif_sz_known[s_spill_panel] = 1U;
                    }
                    rt_snprintf(s_flash_play_lvgl, sizeof(s_flash_play_lvgl),
                                "L:%s", dest);
                    s_flash_play_ok = 1U;
                    CUSTOM_IMG_LOG("flash saved %s (%u bytes)", dest,
                                   (unsigned)s_spill_total);
                }
            }
            custom_round_note_panel_saved(s_spill_panel);
            s_spill_kick = 0U;
            s_spill_active = 0U;
            s_spill_session = 0U;
            s_spill_final = 0U;
            s_spill_total = 0U;
            s_spill_seg = 0U;
            if (need_rearm)
                custom_signal_spill_rearm();
            return;
        }
        if (!s_spill_ptr && s_recv_buf && s_recv_size &&
            (s_recv_cap <= s_recv_size ||
             ((s_recv_cap - s_recv_size) < CUSTOM_USB_CHUNK_FULL)))
        {
            if (custom_signal_park_full(0U))
                continue;
        }
        if (!need_rearm)
            return;
        /* A chunk can arrive while this iteration still has a stale view.
         * Write it before arming USB again. A second start_read drops the
         * packet that just filled the buffer, and the glass then stops. */
        if (s_spill_ptr || s_spill_tail_ptr || s_spill_kick || s_spill_final)
            continue;
        custom_signal_spill_rearm();
        if (!s_spill_kick && !s_spill_abort && !s_spill_ptr &&
            !s_spill_tail_ptr && !s_spill_final)
            CUSTOM_IMG_LOG("Panel_signal wait tail");
        wait_n = 0U;
        while (!s_spill_kick && !s_spill_abort && !s_spill_ptr &&
               !s_spill_tail_ptr && !s_spill_final)
        {
            if (aicos_sem_take(s_save_sem, 200) == 0)
            {
                wait_n = 0U;
                continue;
            }
            if (s_receiving_image)
                wait_n = 0U;
            else if (++wait_n >= 300U)
            {
                CUSTOM_IMG_LOG("Panel_signal tail timeout");
                s_spill_abort = 1U;
                break;
            }
        }
    }
}

static void custom_signal_kick_worker(void)
{
    s_spill_kick = 1U;
    if (!s_flash_save_busy)
        s_flash_save_busy = 1U;
    aicos_sem_give(s_save_sem);
}

/* Called after the USB callback has released the bus lock and decided
 * whether to rearm OUT. The first 2MB of a second file never reached
 * write while a file GIF still had the CPU. 19 is above that thread.
 * append() drops back to the normal priority before the NOR program. */
void custom_image_spill_boost(void)
{
    rt_uint8_t prio = 19;

    if (!s_save_thread)
        return;
    /* A write is already on NOR. Raising this thread above the glass
     * freezes the picture for the whole chunk. */
    if (s_spill_active)
        return;
    if (!s_spill_ptr && !s_spill_tail_ptr && !s_spill_kick && !s_spill_final)
        return;
    rt_thread_control((rt_thread_t)s_save_thread,
                      RT_THREAD_CTRL_CHANGE_PRIORITY, &prio);
    aicos_sem_give(s_save_sem);
}

/* NOR programming stays on flsave. Doing it here stalled the glass. */
void custom_image_spill_service(void)
{
}

static uint8_t custom_signal_park_full(uint32_t extra)
{
    uint8_t *full;
    uint32_t full_cap;
    uint32_t full_len;

    if (s_spill_kick || s_spill_ptr || s_spill_active)
        return 0U;
    if (!s_recv_buf || !s_ready_buf || (s_ready_buf == s_recv_buf))
        return 0U;
    if ((s_ready_buf == s_save_ptr) || (s_ready_buf == s_pend_ptr))
        return 0U;
    if (s_recv_size == 0U)
        return 0U;
    if (extra > s_ready_cap)
        return 0U;
    if ((s_spill_total + s_recv_size + extra) > CUSTOM_FLASH_SIGNAL_MAX)
    {
        CUSTOM_IMG_LOG("Panel_signal over 8MB");
        return 0U;
    }

    full = s_recv_buf;
    full_cap = s_recv_cap;
    full_len = s_recv_size;
    s_recv_buf = s_ready_buf;
    s_recv_cap = s_ready_cap;
    s_recv_size = 0U;
    s_ready_buf = full;
    s_ready_cap = full_cap;
    s_ready_size = 0U;
    s_ready_img_type = CUSTOM_IMAGE_TYPE_INVALID;

    /* Session stays set until the tail is renamed or the spill aborts.
     * s_spill_active drops after each chunk so the next 2 MB can park.
     * The short packet arrives after this pointer is taken and before
     * active/total are set; without session it was saved as its own file. */
    s_spill_session = 1U;
    s_spill_ptr = full;
    s_spill_len = full_len;
    s_spill_panel = s_recv_panel;
    s_spill_kind = s_recv_img_type;
    s_spill_final = 0U;
    /* The other 2 MB buffer is now recv. Keep USB reading into it.
     * Hold only once that buffer is also nearly full. */
    s_spill_need_rearm = 1U;
    s_usb_rearm_hold = 0U;
    custom_signal_kick_worker();
    CUSTOM_IMG_LOG("Panel_signal spill %u bytes", (unsigned)full_len);
    return 1U;
}

static uint8_t custom_signal_take_overflow(const uint8_t *data, uint32_t remain)
{
    uint32_t room;

    if (!data || (remain == 0U) || !s_recv_buf)
        return 0U;
    if (s_recv_panel != 1U)
        return 0U;
    if ((s_recv_img_type != CUSTOM_IMAGE_TYPE_GIF) &&
        (s_recv_img_type != CUSTOM_IMAGE_TYPE_APNG))
        return 0U;

    room = (s_recv_cap > s_recv_size) ? (s_recv_cap - s_recv_size) : 0U;
    if (room > remain)
        room = remain;
    if (room > 0U)
    {
        memcpy(&s_recv_buf[s_recv_size], data, room);
        s_recv_size += room;
        data += room;
        remain -= room;
    }
    if (remain == 0U)
        return 1U;
    if (!custom_signal_park_full(remain))
        return 0U;
    if (!s_recv_buf || ((s_recv_cap - s_recv_size) < remain))
        return 0U;
    memcpy(&s_recv_buf[s_recv_size], data, remain);
    s_recv_size += remain;
    return 1U;
}

static void custom_signal_queue_tail(void)
{
    uint8_t *tail = s_recv_buf;
    uint32_t n = s_recv_size;
    uint32_t cap = s_recv_cap;

    if (!tail || (n == 0U))
        return;
    s_spill_panel = s_recv_panel;
    s_spill_kind = s_recv_img_type;
    /* Publish the bytes first. final is what wakes the writer; if it
     * is set before the pointer, the writer closes a short file. */
    if (s_spill_ptr || s_spill_active || s_spill_session || (s_spill_total > 0U))
    {
        /* The full chunk may already have been taken by the writer.
         * Keep this short packet as the tail. Putting it in spill_ptr
         * let the writer rename the .part and drop these bytes. */
        s_spill_tail_ptr = tail;
        s_spill_tail_len = n;
        s_spill_need_rearm = 1U;
        s_usb_rearm_hold = 1U;
    }
    else if (s_ready_buf && (s_ready_buf != tail) &&
             (s_ready_buf != s_save_ptr) && (s_ready_buf != s_pend_ptr))
    {
        s_spill_ptr = tail;
        s_spill_len = n;
        s_recv_buf = s_ready_buf;
        s_recv_cap = s_ready_cap;
        s_ready_buf = tail;
        s_ready_cap = cap;
        s_ready_size = 0U;
        s_ready_img_type = CUSTOM_IMAGE_TYPE_INVALID;
        /* Hold the next OUT until this tail is on flash and the fd is
         * closed. FE 0C opening the same file mid-write froze the GIF. */
        s_spill_need_rearm = 1U;
        s_usb_rearm_hold = 1U;
    }
    else
    {
        s_spill_ptr = tail;
        s_spill_len = n;
        s_spill_need_rearm = 1U;
        s_usb_rearm_hold = 1U;
    }
    s_spill_final = 1U;
    custom_signal_kick_worker();
    CUSTOM_IMG_LOG("Panel_signal tail %u bytes", (unsigned)n);
}

static void custom_signal_abort_from_recv(void)
{
    if (!s_spill_active && !s_spill_ptr && !s_spill_tail_ptr &&
        !s_spill_kick && !s_spill_session &&
        (s_spill_total == 0U) && (s_spill_fd < 0))
    {
        return;
    }
    s_spill_abort = 1U;
    s_spill_final = 0U;
    s_spill_need_rearm = 1U;
    s_usb_rearm_hold = 1U;
    custom_signal_kick_worker();
}
#endif

static uint8_t s_panel_gif_sz_hold;

static void custom_image_panel_gif_size_hold_on(void);

static void custom_flash_purge_panel(unsigned panel)
{
    const char *dir;
    char path[64];
    rt_device_t wdt;

    if (panel < 1U || panel > CUSTOM_FLASH_PANEL_COUNT)
        return;
    dir = custom_flash_panel_dir(panel);
    if (!dir)
        return;
    wdt = rt_device_find("wdt");
    if (wdt)
        rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
    custom_flash_object_path(panel, "gif", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "apng", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.new", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "apng.new", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.part", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "apng.part", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.2", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "apng.2", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.part2", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "apng.part2", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.3", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.part3", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.4", path, sizeof(path));
    (void)unlink(path);
    custom_flash_object_path(panel, "gif.part4", path, sizeof(path));
    (void)unlink(path);
    rt_snprintf(path, sizeof(path), "%s/recv.gif", dir);
    (void)unlink(path);
    rt_snprintf(path, sizeof(path), "%s/recv.apng", dir);
    (void)unlink(path);
    s_panel_gif_sz[panel] = 0U;
    s_panel_gif_sz_known[panel] = 0U;
    CUSTOM_IMG_LOG("purged %s", dir);
}

static void custom_flash_purge_run(void)
{
    unsigned m;
    uint8_t acks;
    unsigned p;

    m = s_purge_mask;
    acks = s_purge_acks;
    s_purge_mask = 0U;
    s_purge_acks = 0U;
    for (p = 0U; p < 4U; p++) {
        if (m & (1U << p))
            custom_flash_purge_panel(p + 1U);
    }
    if (acks == 0U && m != 0U)
        acks = 1U;
    s_purge_done_n = (uint8_t)(s_purge_done_n + acks);
    custom_ui_wake_tick_timer();
}

static void custom_flash_save_worker(void *arg)
{
    (void)arg;
    for (;;)
    {
        if (aicos_sem_take(s_save_sem, AICOS_WAIT_FOREVER) != 0)
            continue;
        do
        {
        for (;;)
        {
#if CUSTOM_FLASH_SIGNAL_ENABLE
            if (s_spill_kick || s_spill_abort || s_spill_ptr ||
                s_spill_tail_ptr || s_spill_final)
            {
                custom_signal_spill_run();
                if (s_save_thread && s_save_prio)
                {
                    rt_uint8_t prio = s_save_prio;

                    rt_thread_control((rt_thread_t)s_save_thread,
                                      RT_THREAD_CTRL_CHANGE_PRIORITY, &prio);
                }
                continue;
            }
#endif
            if (s_purge_mask || s_purge_acks)
            {
                custom_flash_purge_run();
                continue;
            }
            if (!s_save_kick && s_save_pending)
            {
                /* Purge held the thread. The finished GIF is waiting. */
                s_save_ptr = s_pend_ptr;
                s_save_len = s_pend_len;
                s_save_kind = s_pend_kind;
                s_save_panel = s_pend_panel;
                s_save_pending = 0U;
                s_save_kick = 1U;
                if (s_recv_buf == s_save_ptr)
                {
                    uint8_t *tmp = s_ready_buf;
                    uint32_t cap = s_ready_cap;

                    s_ready_buf = s_recv_buf;
                    s_ready_cap = s_recv_cap;
                    s_recv_buf = tmp;
                    s_recv_cap = cap;
                    s_ready_size = s_save_len;
                    s_ready_img_type = s_save_kind;
                }
                s_usb_rearm_hold = 0U;
                usbd_vender_out_rearm();
                custom_ui_wake_tick_timer();
                continue;
            }
            if (!s_save_kick)
                break;
            s_save_kick = 0U;
            s_panel_gif_sz_hold = 1U;
            custom_flash_save_ready_gif_apng();
            s_save_ptr = NULL;
            custom_flash_recv_release_large();
            if (s_usb_rearm_hold)
            {
                s_usb_rearm_hold = 0U;
                usbd_vender_out_rearm();
            }
        }
        s_panel_gif_sz_hold = 0U;
        s_flash_save_busy = 0U;
        if (s_purge_mask || s_purge_acks || s_save_kick || s_save_pending
#if CUSTOM_FLASH_SIGNAL_ENABLE
            || s_spill_kick || s_spill_abort || s_spill_ptr ||
            s_spill_tail_ptr || s_spill_final
#endif
            )
        {
            /* finish_recv queued work while busy was still 1, so it did
             * not give the semaphore. Run that work before sleeping. */
            s_flash_save_busy = 1U;
            continue;
        }
        break;
        } while (1);
        custom_ui_wake_tick_timer();
    }
}

static void custom_flash_save_kick(void)
{
    s_save_ptr = s_ready_buf;
    s_save_len = s_ready_size;
    s_save_kind = s_ready_img_type;
    s_save_panel = s_recv_panel;
    if (!custom_flash_save_needed())
    {
        if (s_save_len)
            CUSTOM_IMG_LOG("flash save skip panel=%u kind=%u len=%u",
                           s_save_panel, (unsigned)s_save_kind,
                           (unsigned)s_save_len);
        return;
    }
    CUSTOM_IMG_LOG("flash save arm %u bytes panel %u",
                   (unsigned)s_save_len, s_save_panel);
    /* USB OUT completion runs in the controller interrupt. A NOR erase
     * there never returns to the scheduler, so the boot GIF cannot move.
     * Disp is priority 20+id. Match the LVGL thread so a 1MB program is
     * not stuck behind GIF decode for the whole transfer. */
    if (!s_save_thread_ok || s_flash_save_busy)
    {
        if (s_flash_save_busy)
        {
            /* The previous return dropped the file with no log. FE 0C
             * then acked 0D and the glass had nothing left to play. */
            if (!s_save_pending)
            {
                s_pend_ptr = s_save_ptr;
                s_pend_len = s_save_len;
                s_pend_kind = s_save_kind;
                s_pend_panel = s_save_panel;
                s_save_pending = 1U;
                custom_image_usb_hold_rearm();
                CUSTOM_IMG_LOG("flash save queued %u bytes panel %u",
                               (unsigned)s_save_len, s_save_panel);
            }
            else
                CUSTOM_IMG_LOG("flash save busy, drop %u bytes",
                               (unsigned)s_save_len);
        }
        else
        {
            custom_flash_save_ready_gif_apng();
            s_save_ptr = NULL;
            custom_flash_recv_release_large();
        }
        return;
    }
    s_save_kick = 1U;
    s_flash_save_busy = 1U;
    /* Cache sizes before the writer takes LittleFS. A stat from the
     * GIF timer during this erase never returned, so flash writing
     * was not printed. */
    s_panel_gif_sz_hold = 1U;
    aicos_sem_give(s_save_sem);
}

void custom_image_purge_request(unsigned mask)
{
    mask &= 0x0FU;
    if (mask == 0U)
    {
        s_purge_done_n++;
        custom_ui_wake_tick_timer();
        return;
    }
    s_purge_mask |= mask;
    if (s_purge_acks < 255U)
        s_purge_acks++;
    if (!s_save_thread_ok)
    {
        CUSTOM_IMG_LOG("purge skipped, save thread missing");
        s_purge_mask = 0U;
        s_purge_acks = 0U;
        s_purge_done_n++;
        custom_ui_wake_tick_timer();
        return;
    }
    if (s_flash_save_busy)
        return;
    s_flash_save_busy = 1U;
    aicos_sem_give(s_save_sem);
}

uint8_t custom_image_purge_done_take(void)
{
    if (s_purge_done_n == 0U)
        return 0U;
    s_purge_done_n--;
    return 1U;
}

static void custom_flash_save_thread_start(void)
{
    aicos_thread_t th;
    uint32_t prio;

    if (s_save_thread_ok)
        return;
    if (!s_save_sem)
        s_save_sem = aicos_sem_create(0);
    if (!s_save_sem)
    {
        CUSTOM_IMG_LOG("flash save sem fail");
        return;
    }
    /* Display is 20, LVGL is 21. Priority 19 ran this thread inside
     * the USB callback and the first 2MB never printed write.
     * Stay behind LVGL. The 4MB tail is a second file, not a higher
     * priority. */
    prio = (RT_THREAD_PRIORITY_MAX * 2U / 3U) + 2U;
    if (prio >= RT_THREAD_PRIORITY_MAX)
        prio = RT_THREAD_PRIORITY_MAX - 1U;
    s_save_prio = (uint8_t)prio;
    th = aicos_thread_create("flsave", 16384, prio, custom_flash_save_worker, NULL);
    s_save_thread = th;
    if (!th)
    {
        CUSTOM_IMG_LOG("flash save thread fail");
        return;
    }
    s_save_thread_ok = 1U;
}
#else
static void custom_flash_save_kick(void)
{
}

void custom_image_purge_request(unsigned mask)
{
    (void)mask;
}

uint8_t custom_image_purge_done_take(void)
{
    return 1U;
}
#endif

static void custom_image_finish_recv(uint8_t ok)
{
    uint8_t accepted = 0U;

    if (ok && s_recv_buf && s_recv_size > 0)
    {
        /*
         * Avoid one more full-size memcpy/realloc at transfer end.
         * On large PNG, duplicating recv->ready can fail on fragmented heap.
         * Swap recv/ready buffers so UI consumes the completed buffer directly.
         */
#if defined(KERNEL_RTTHREAD) && CUSTOM_FLASH_SIGNAL_ENABLE
        if (s_spill_ptr || s_spill_tail_ptr || s_spill_active ||
            s_spill_session || (s_spill_total > 0U))
        {
            custom_signal_queue_tail();
            accepted = 1U;
        }
        else
#endif
        if (custom_image_flash_save_busy())
        {
            /* Leave the in-progress erase buffer alone. Park this GIF in
             * the recv buffer until the writer takes it. USB stays stopped
             * so the next file cannot overwrite this one. */
            if (!custom_image_save_pending() && s_recv_buf)
            {
#ifdef KERNEL_RTTHREAD
                s_pend_ptr = s_recv_buf;
                s_pend_len = s_recv_size;
                s_pend_kind = s_recv_img_type;
                s_pend_panel = s_recv_panel;
                s_save_pending = 1U;
                custom_image_usb_hold_rearm();
#endif
                accepted = 1U;
                CUSTOM_IMG_LOG("flash save queued %u bytes panel %u",
                               (unsigned)s_recv_size, s_recv_panel);
            }
            else
            {
                CUSTOM_IMG_LOG("flash save busy, drop %u bytes panel %u",
                               (unsigned)s_recv_size, s_recv_panel);
            }
        }
        else
        {
            uint8_t *tmp_buf = s_ready_buf;
            uint32_t tmp_cap = s_ready_cap;
            s_ready_buf = s_recv_buf;
            s_ready_cap = s_recv_cap;
            s_recv_buf = tmp_buf;
            s_recv_cap = tmp_cap;
            CUSTOM_IMG_VLOG("recv done ok, type=%u, size=%u", s_recv_img_type, s_recv_size);
            s_ready_size = s_recv_size;
            s_ready_img_type = s_recv_img_type;
            s_ready_slot = s_recv_target_slot;
            s_pending_img_type = s_recv_img_type;
            s_pending_img_slot = s_recv_target_slot;
            custom_flash_save_kick();
            /* The 6 MB block was the only buffer. Hold the next OUT
             * until that block is on flash and the 2 MB pair is back. */
            if (!s_recv_buf)
                custom_image_usb_hold_rearm();
            accepted = 1U;
        }
        /*
         * Perf timestamp + counter (read by custom_Area4 to compute pickup latency
         * and recv-vs-show drop rate). Updated only on success so the timestamp
         * reflects a "ready buffer is now available for UI" event.
         *
         * Also learn inter-frame interval for Area4 idle watchdog
         * (miss_frames × interval → splash).
         */
        {
            uint64_t now_ms = aic_get_time_ms();
            if (s_last_recv_done_ms != 0U && now_ms > s_last_recv_done_ms)
            {
                uint32_t dt = (uint32_t)(now_ms - s_last_recv_done_ms);
                if (dt >= CUSTOM_IMAGE_FRAME_MS_MIN && dt <= CUSTOM_IMAGE_FRAME_MS_MAX)
                {
                    uint32_t prev = s_frame_interval_ms;
                    if (prev == 0U)
                    {
                        s_frame_interval_ms = dt;
                    }
                    else
                    {
                        /* Light EWMA: 3/4 history + 1/4 new sample. */
                        s_frame_interval_ms = (prev * 3U + dt) / 4U;
                    }
                }
            }
            s_last_recv_done_ms = now_ms;
        }
        s_recv_done_count++;
        custom_image_note_stream_activity_internal();
        /*
         * Wake the LVGL screen-switch timer immediately so the next refresh
         * picks up this fresh image without waiting up to one full 10 ms tick.
         * Same pattern used by custom_Video.c at recv-finish points. Safe to
         * call from USB-recv thread context: it just marks the lv_timer ready
         * via lv_timer_ready(); LVGL handler runs from its own thread and
         * sees the flag on its next iteration.
         */
        custom_ui_wake_tick_timer();
    }
    else
    {
        CUSTOM_IMG_LOG("recv done fail, type=%u, size=%u", s_recv_img_type, s_recv_size);
#if defined(KERNEL_RTTHREAD) && CUSTOM_FLASH_SIGNAL_ENABLE
        custom_signal_abort_from_recv();
#endif
    }

    /*
     * After recv/ready swap, s_recv_buf holds the *previous* ready allocation.
     * Do NOT free it here: LVGL may still be decoding/copying that payload while
     * USB already finished the next image (same bug as show skip type=0/1).
     * Recycle via custom_image_prepare_buffer() on the following cmd / realloc path.
     */

    s_receiving_image = 0;
    /* Only a file that has a buffer of its own moves the FA cursor.
     * A dropped file must not skip Panel_3 / Panel_4.
     * Panel_signal takes the one file and does not walk Panel_2..4. */
#if !CUSTOM_FLASH_SIGNAL_ENABLE
    if (accepted && s_fa_seq && s_fa_next < CUSTOM_FLASH_PANEL_COUNT)
    {
        s_fa_next++;
        custom_image_set_flash_panel(s_fa_next);
        CUSTOM_IMG_LOG("fa seq next Panel_%u", s_fa_next);
    }
#else
    (void)accepted;
#endif
    s_recv_size = 0;
    s_tail12_cnt = 0;
    s_have_prev_byte = 0;
    s_wait_magic = 0;
    s_magic_win_cnt = 0;
    s_magic_logged = 0;
    s_req_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_recv_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_img_hdr_cnt = 0U;
    s_img_payload_left = 0U;
    s_recv_mode = CUSTOM_RECV_MODE_RAW;
}

static uint8_t custom_image_check_eof(uint8_t byte)
{
    uint8_t done = 0;

    if (s_tail12_cnt < sizeof(s_tail12))
    {
        s_tail12[s_tail12_cnt++] = byte;
    }
    else
    {
        memmove(&s_tail12[0], &s_tail12[1], sizeof(s_tail12) - 1);
        s_tail12[sizeof(s_tail12) - 1] = byte;
    }

    switch (s_recv_img_type)
    {
    case CUSTOM_IMAGE_TYPE_JPEG:
        if (s_have_prev_byte && s_prev_byte == 0xFF && byte == 0xD9)
        {
            done = 1;
        }
        break;
    case CUSTOM_IMAGE_TYPE_PNG:
    case CUSTOM_IMAGE_TYPE_APNG:
        /*
         * Do NOT finish by scanning IEND signature bytes.
         * The 12-byte IEND+CRC sequence can appear inside compressed IDAT/fdAT payload,
         * causing premature truncation and "chunk too big" / CRC failures.
         * Use USB bulk short packet end-of-transfer instead (see custom_image_recv()).
         */
        break;
    case CUSTOM_IMAGE_TYPE_GIF:
        /* GIF trailer(0x3B) may appear in compressed payload; do not finish here. */
        break;
    default:
        break;
    }

    s_prev_byte = byte;
    s_have_prev_byte = 1;
    return done;
}

static void custom_image_finish_by_short_packet(void)
{
    if (!s_receiving_image || s_recv_size == 0 || !s_recv_buf)
    {
        return;
    }
    /* USB bulk short packet marks end-of-transfer for PNG/APNG/GIF reliably. */
    custom_image_finish_recv(1);
}

static uint8_t custom_image_is_magic_ready(uint8_t byte)
{
    static const uint8_t png_sig[8] = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    static const uint8_t gif87a[6] = {'G', 'I', 'F', '8', '7', 'a'};
    static const uint8_t gif89a[6] = {'G', 'I', 'F', '8', '9', 'a'};
    uint8_t idx;

    if (s_magic_win_cnt < sizeof(s_magic_win))
    {
        s_magic_win[s_magic_win_cnt++] = byte;
    }
    else
    {
        memmove(&s_magic_win[0], &s_magic_win[1], sizeof(s_magic_win) - 1);
        s_magic_win[sizeof(s_magic_win) - 1] = byte;
    }

    if (s_magic_win_cnt >= 3)
    {
        idx = s_magic_win_cnt - 3;
        if (s_magic_win[idx] == 0xFF &&
            s_magic_win[idx + 1] == 0xD8 &&
            s_magic_win[idx + 2] == 0xFF)
        {
            s_recv_img_type = CUSTOM_IMAGE_TYPE_JPEG;
            if (!s_magic_logged)
            {
                CUSTOM_IMG_VLOG("magic detected JPEG");
                s_magic_logged = 1;
            }
            return 1;
        }
    }

    if (s_magic_win_cnt >= 8)
    {
        idx = s_magic_win_cnt - 8;
        if (memcmp(&s_magic_win[idx], png_sig, 8) == 0)
        {
            s_recv_img_type = (s_req_img_type == CUSTOM_IMAGE_TYPE_APNG) ? CUSTOM_IMAGE_TYPE_APNG : CUSTOM_IMAGE_TYPE_PNG;
            if (!s_magic_logged)
            {
                CUSTOM_IMG_VLOG("magic detected %s",
                                (s_recv_img_type == CUSTOM_IMAGE_TYPE_APNG) ? "APNG" : "PNG");
                s_magic_logged = 1;
            }
            return 1;
        }
    }

    if (s_magic_win_cnt >= 6)
    {
        idx = s_magic_win_cnt - 6;
        if (memcmp(&s_magic_win[idx], gif87a, 6) == 0 ||
            memcmp(&s_magic_win[idx], gif89a, 6) == 0)
        {
            s_recv_img_type = CUSTOM_IMAGE_TYPE_GIF;
            if (!s_magic_logged)
            {
                CUSTOM_IMG_VLOG("magic detected GIF");
                s_magic_logged = 1;
            }
            return 1;
        }
    }

    return 0;
}

static void custom_image_push_magic_to_buffer(void)
{
    uint8_t start;
    uint8_t i;

    if (s_recv_img_type == CUSTOM_IMAGE_TYPE_JPEG)
    {
        start = s_magic_win_cnt - 3;
    }
    else if (s_recv_img_type == CUSTOM_IMAGE_TYPE_PNG || s_recv_img_type == CUSTOM_IMAGE_TYPE_APNG)
    {
        start = s_magic_win_cnt - 8;
    }
    else
    {
        start = s_magic_win_cnt - 6;
    }

    for (i = start; i < s_magic_win_cnt; i++)
    {
        if (!custom_image_ensure_capacity(s_recv_size + 1U))
        {
            custom_image_finish_recv(0);
            return;
        }
        s_recv_buf[s_recv_size++] = s_magic_win[i];
        (void)custom_image_check_eof(s_magic_win[i]);
    }
}

static void custom_image_feed_payload_byte(uint8_t byte)
{
    if (!s_receiving_image || !s_recv_buf)
    {
        return;
    }

    if (!custom_image_ensure_capacity(s_recv_size + 1U))
    {
#if defined(KERNEL_RTTHREAD) && CUSTOM_FLASH_SIGNAL_ENABLE
        if ((s_recv_panel == 1U) &&
            ((s_recv_img_type == CUSTOM_IMAGE_TYPE_GIF) ||
             (s_recv_img_type == CUSTOM_IMAGE_TYPE_APNG)) &&
            custom_signal_park_full(1U))
        {
            /* parked; store this byte in the other buffer */
        }
        else
#endif
        {
            custom_image_finish_recv(0);
            return;
        }
    }

    if (s_wait_magic)
    {
        if (!custom_image_is_magic_ready(byte))
        {
            return;
        }
        s_wait_magic = 0;
        custom_image_push_magic_to_buffer();
        return;
    }

    s_recv_buf[s_recv_size] = byte;
    s_recv_size++;

    if (custom_image_check_eof(byte))
    {
        custom_image_finish_recv(1);
    }
}

uint8_t custom_image_is_receiving(void)
{
    return s_receiving_image ? 1U : 0U;
}

void custom_image_abort_recv(void)
{
    if (s_receiving_image)
    {
        custom_image_finish_recv(0);
    }
}

void custom_image_note_stream_activity(void)
{
    custom_image_note_stream_activity_internal();
}

uint8_t custom_image_stream_is_fresh(uint32_t miss_frames)
{
    uint64_t now;
    uint64_t last_act;
    uint64_t last_done;
    uint64_t newest;
    uint32_t interval_ms;
    uint32_t idle_ms;

    /*
     * Freshness is timestamp-only. Do NOT treat s_receiving_image /
     * s_pending_img_type as forever-fresh: a CD-BB with no following payload
     * leaves receiving=1, and that previously blocked Area4 → splash forever.
     * Payload / CD-BB / finish_recv keep s_last_stream_activity_ms alive;
     * stuck RX without packets expires after (miss_frames × interval) and
     * the watchdog aborts.
     */
    if (miss_frames == 0U)
    {
        return 0U;
    }

    interval_ms = s_frame_interval_ms;
    if (interval_ms == 0U)
    {
        interval_ms = CUSTOM_IMAGE_DEFAULT_FRAME_MS;
    }
    if (interval_ms < CUSTOM_IMAGE_FRAME_MS_MIN)
    {
        interval_ms = CUSTOM_IMAGE_FRAME_MS_MIN;
    }
    else if (interval_ms > CUSTOM_IMAGE_FRAME_MS_MAX)
    {
        interval_ms = CUSTOM_IMAGE_FRAME_MS_MAX;
    }
    idle_ms = miss_frames * interval_ms;

    now = aic_get_time_ms();
    last_act = s_last_stream_activity_ms;
    last_done = s_last_recv_done_ms;
    newest = last_act;
    if (last_done > newest)
    {
        newest = last_done;
    }

    if (newest != 0U && now >= newest && (now - newest) < (uint64_t)idle_ms)
    {
        return 1U;
    }
    return 0U;
}

uint64_t custom_image_get_last_recv_done_ms(void)
{
    return s_last_recv_done_ms;
}

uint32_t custom_image_get_recv_done_count(void)
{
    return s_recv_done_count;
}

void custom_image_bus_lock(void)
{
#ifdef KERNEL_RTTHREAD
    if (s_custom_img_bus_mx_ready)
    {
        rt_mutex_take(&s_custom_img_bus_mx, RT_WAITING_FOREVER);
    }
#endif
}

void custom_image_bus_unlock(void)
{
#ifdef KERNEL_RTTHREAD
    if (s_custom_img_bus_mx_ready)
    {
        rt_mutex_release(&s_custom_img_bus_mx);
    }
#endif
}

const char * const custom_flash_panel_dirs[CUSTOM_FLASH_PANEL_COUNT] = {
    CUSTOM_FLASH_PANEL_ROOT "/Panel_1",
    CUSTOM_FLASH_PANEL_ROOT "/Panel_2",
    CUSTOM_FLASH_PANEL_ROOT "/Panel_3",
    CUSTOM_FLASH_PANEL_ROOT "/Panel_4",
};

const char *custom_flash_panel_dir(unsigned panel_0_to_4)
{
    if (panel_0_to_4 == CUSTOM_FLASH_BOOT_PANEL)
    {
        return CUSTOM_FLASH_BOOT_DIR;
    }
#if CUSTOM_FLASH_SIGNAL_ENABLE
    /* Panel_1..4 stay in the tree and are not used. */
    if (panel_0_to_4 == 1U)
        return CUSTOM_FLASH_SIGNAL_DIR;
    return NULL;
#else
    if ((panel_0_to_4 < 1U) || (panel_0_to_4 > CUSTOM_FLASH_PANEL_COUNT))
    {
        return NULL;
    }
    return custom_flash_panel_dirs[panel_0_to_4 - 1U];
#endif
}

void custom_image_set_flash_panel(unsigned panel_1_to_4)
{
#if CUSTOM_FLASH_SIGNAL_ENABLE
    if (panel_1_to_4 != 1U)
        return;
    custom_flash_recv_use_large();
#else
    if ((panel_1_to_4 < 1U) || (panel_1_to_4 > CUSTOM_FLASH_PANEL_COUNT))
    {
        return;
    }
#endif
    s_flash_panel = panel_1_to_4;
    s_flash_file_arm = 1U;
}

unsigned custom_image_flash_panel(void)
{
    return s_flash_panel;
}

unsigned custom_image_xfer_panel(void)
{
#ifdef KERNEL_RTTHREAD
    if (s_flash_save_busy && s_save_panel >= 1U &&
        s_save_panel <= CUSTOM_FLASH_PANEL_COUNT)
        return s_save_panel;
    if (s_save_pending && s_pend_panel >= 1U &&
        s_pend_panel <= CUSTOM_FLASH_PANEL_COUNT)
        return s_pend_panel;
#endif
    return s_flash_panel;
}

void custom_image_xfer_cmd_mask_or(unsigned mask)
{
    s_xfer_cmd_mask |= mask & 0x0FU;
}

void custom_image_xfer_cmd_mask_clear(void)
{
    s_xfer_cmd_mask = 0U;
}

void custom_image_fa_seq_begin(void)
{
    s_fa_seq = 1U;
    s_fa_next = 1U;
    custom_image_set_flash_panel(1U);
}

void custom_image_fa_seq_end(void)
{
    s_fa_seq = 0U;
}

unsigned custom_image_xfer_mask(void)
{
    unsigned mask = s_xfer_cmd_mask;

#ifdef KERNEL_RTTHREAD
    if (s_flash_save_busy && s_save_panel >= 1U &&
        s_save_panel <= CUSTOM_FLASH_PANEL_COUNT)
        mask |= 1U << (s_save_panel - 1U);
    if (s_save_pending && s_pend_panel >= 1U &&
        s_pend_panel <= CUSTOM_FLASH_PANEL_COUNT)
        mask |= 1U << (s_pend_panel - 1U);
    if (custom_image_is_receiving() && s_flash_panel >= 1U &&
        s_flash_panel <= CUSTOM_FLASH_PANEL_COUNT)
        mask |= 1U << (s_flash_panel - 1U);
#endif
    if (mask == 0U && s_flash_panel >= 1U &&
        s_flash_panel <= CUSTOM_FLASH_PANEL_COUNT)
        mask |= 1U << (s_flash_panel - 1U);
    return mask;
}

const char *custom_image_flash_play_src(void)
{
#ifdef KERNEL_RTTHREAD
    return s_flash_play_ok ? s_flash_play_lvgl : NULL;
#else
    return NULL;
#endif
}

static uint32_t custom_image_file_bytes(const char *path)
{
#ifdef KERNEL_RTTHREAD
    struct stat st;

    if (!path || stat(path, &st) != 0 || st.st_size <= 0)
        return 0U;
    return (uint32_t)st.st_size;
#else
    (void)path;
    return 0U;
#endif
}

uint8_t custom_image_panel_gif_path(unsigned panel_0_to_4, char *buf, uint32_t buflen)
{
    const char *dir;

    if (!buf || (buflen < 32U) || (panel_0_to_4 > CUSTOM_FLASH_PANEL_COUNT))
    {
        return 0U;
    }
#if CUSTOM_FLASH_SIGNAL_ENABLE
    /* Signal wins when it has a file. Otherwise play Panel_1.
     * The size is already known after spill. stat() of that 2MB
     * piece walks the whole chain and never returns if a RAMWR
     * starts on the same DMA engine. */
    if (panel_0_to_4 == 1U)
    {
        uint8_t have_signal;

        if (s_panel_gif_sz_known[1])
            have_signal = (s_panel_gif_sz[1] > 0U) ? 1U : 0U;
        else
            have_signal = (custom_image_file_bytes(CUSTOM_FLASH_SIGNAL_DIR "/Panel_signal.gif") > 0U)
                          ? 1U : 0U;
        if (have_signal)
        {
#ifdef KERNEL_RTTHREAD
            rt_snprintf(buf, buflen, "%s/Panel_signal.gif", CUSTOM_FLASH_SIGNAL_DIR);
#else
            snprintf(buf, buflen, "%s/Panel_signal.gif", CUSTOM_FLASH_SIGNAL_DIR);
#endif
        }
        else
        {
#ifdef KERNEL_RTTHREAD
            rt_snprintf(buf, buflen, "%s/Panel_001.gif", custom_flash_panel_dirs[0]);
#else
            snprintf(buf, buflen, "%s/Panel_001.gif", custom_flash_panel_dirs[0]);
#endif
        }
        return 1U;
    }
#endif
    dir = custom_flash_panel_dir(panel_0_to_4);
    if (!dir)
    {
        return 0U;
    }
#ifdef KERNEL_RTTHREAD
    rt_snprintf(buf, buflen, "%s/Panel_%03u.gif", dir, panel_0_to_4);
#else
    snprintf(buf, buflen, "%s/Panel_%03u.gif", dir, panel_0_to_4);
#endif
    return 1U;
}

static uint32_t custom_image_panel_gif_size_stat(unsigned panel_0_to_4)
{
#ifdef KERNEL_RTTHREAD
    char path[48];
    struct stat st;

    if (!custom_image_panel_gif_path(panel_0_to_4, path, sizeof(path)))
        return 0U;
    if ((stat(path, &st) != 0) || (st.st_size <= 0))
        return 0U;
#if CUSTOM_FLASH_SIGNAL_ENABLE
    if (panel_0_to_4 == 1U)
    {
        struct stat extra;
        char more[56];
        uint32_t sum = (uint32_t)st.st_size;
        unsigned n;

        for (n = 2U; n <= 4U; n++)
        {
            rt_snprintf(more, sizeof(more), "%s.%u", path, n);
            if (stat(more, &extra) == 0 && extra.st_size > 0)
                sum += (uint32_t)extra.st_size;
        }
        return sum;
    }
#endif
    return (uint32_t)st.st_size;
#else
    (void)panel_0_to_4;
    return 0U;
#endif
}

/* Filled on the save thread before NOR erase. Callers then skip stat,
 * so the animation does not sit inside the LittleFS lock for every frame. */
static void custom_image_panel_gif_size_hold_on(void)
{
    unsigned p;

    for (p = 0U; p <= 4U; p++)
        s_panel_gif_sz[p] = custom_image_panel_gif_size_stat(p);
    s_panel_gif_sz_hold = 1U;
}

uint32_t custom_image_panel_gif_size(unsigned panel_1_to_4)
{
    if (panel_1_to_4 <= 4U && s_panel_gif_sz_known[panel_1_to_4])
        return s_panel_gif_sz[panel_1_to_4];
    if (panel_1_to_4 <= 4U &&
        (s_panel_gif_sz_hold || custom_image_flash_save_busy() ||
         custom_image_spill_pending()))
        return s_panel_gif_sz[panel_1_to_4];
    return custom_image_panel_gif_size_stat(panel_1_to_4);
}

uint8_t custom_image_load_panel_gif(unsigned panel_1_to_4, uint8_t *dst, uint32_t cap,
                                    uint32_t *size_out)
{
#ifdef KERNEL_RTTHREAD
    char path[48];
    uint32_t want;
    uint32_t off;
    int fd;

    if (size_out)
    {
        *size_out = 0U;
    }
    if (!dst || (cap == 0U))
    {
        return 0U;
    }
    want = custom_image_panel_gif_size(panel_1_to_4);
    if ((want == 0U) || (want > cap))
    {
        return 0U;
    }
    if (!custom_image_panel_gif_path(panel_1_to_4, path, sizeof(path)))
    {
        return 0U;
    }
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
    {
        CUSTOM_IMG_LOG("load gif open fail %s errno=%d", path, errno);
        return 0U;
    }
    off = 0U;
    while (off < want)
    {
        uint32_t chunk = want - off;
        int n;
        unsigned piece;

        if (s_load_abort || custom_image_flash_save_busy() ||
            custom_image_spill_pending()) {
            close(fd);
            return 0U;
        }
        if (chunk > 65536U)
            chunk = 65536U;
        n = read(fd, dst + off, chunk);
        if (n <= 0)
        {
            /* Panel_signal is stored as gif, gif.2, gif.3. */
            close(fd);
            fd = -1;
            for (piece = 2U; piece <= 4U && off < want; piece++)
            {
                char more[64];
                rt_device_t wdt = rt_device_find("wdt");

                rt_snprintf(more, sizeof(more), "%s.%u", path, piece);
                fd = open(more, O_RDONLY, 0);
                if (fd < 0)
                    break;
                if (wdt)
                    rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
                while (off < want)
                {
                    if (s_load_abort) {
                        close(fd);
                        return 0U;
                    }
                    chunk = want - off;
                    if (chunk > 65536U)
                        chunk = 65536U;
                    n = read(fd, dst + off, chunk);
                    if (n <= 0)
                        break;
                    off += (uint32_t)n;
                }
                close(fd);
                fd = -1;
                if (n > 0)
                    break;
            }
            if (off < want)
            {
                CUSTOM_IMG_LOG("load gif read fail %s got=%u want=%u errno=%d",
                               path, (unsigned)off, (unsigned)want, errno);
                return 0U;
            }
            break;
        }
        off += (uint32_t)n;
        if ((off & ~0x3FFFFU) != ((off - (uint32_t)n) & ~0x3FFFFU))
        {
            rt_device_t wdt = rt_device_find("wdt");

            if (wdt)
                rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
        }
    }
    if (fd >= 0)
        close(fd);
    if (size_out)
    {
        *size_out = want;
    }
    return 1U;
#else
    (void)panel_1_to_4;
    (void)dst;
    (void)cap;
    (void)size_out;
    return 0U;
#endif
}

uint32_t custom_image_peek_panel_gif(unsigned panel_0_to_4, uint8_t *dst, uint32_t cap)
{
#ifdef KERNEL_RTTHREAD
    char path[48];
    int fd;
    int n;

    if (!dst || (cap == 0U))
        return 0U;
    if (!custom_image_panel_gif_path(panel_0_to_4, path, sizeof(path)))
        return 0U;
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
        return 0U;
    n = read(fd, dst, cap);
    close(fd);
    return (n > 0) ? (uint32_t)n : 0U;
#else
    (void)panel_0_to_4;
    (void)dst;
    (void)cap;
    return 0U;
#endif
}

uint32_t custom_image_loadin_jpeg_size(void)
{
#ifdef KERNEL_RTTHREAD
    char path[48];
    struct stat st;

    rt_snprintf(path, sizeof(path), "%s/LOADIN.jpeg", CUSTOM_FLASH_BOOT_DIR);
    if ((stat(path, &st) != 0) || (st.st_size <= 0))
    {
        return 0U;
    }
    return (uint32_t)st.st_size;
#else
    return 0U;
#endif
}

uint8_t custom_image_load_loadin_jpeg(uint8_t *dst, uint32_t cap, uint32_t *size_out)
{
#ifdef KERNEL_RTTHREAD
    char path[48];
    uint32_t want;
    uint32_t off;
    int fd;

    if (size_out)
    {
        *size_out = 0U;
    }
    if (!dst || (cap == 0U))
    {
        return 0U;
    }
    want = custom_image_loadin_jpeg_size();
    if ((want == 0U) || (want > cap))
    {
        return 0U;
    }
    rt_snprintf(path, sizeof(path), "%s/LOADIN.jpeg", CUSTOM_FLASH_BOOT_DIR);
    fd = open(path, O_RDONLY, 0);
    if (fd < 0)
    {
        CUSTOM_IMG_LOG("loadin open fail %s errno=%d", path, errno);
        return 0U;
    }
    off = 0U;
    while (off < want)
    {
        int n = read(fd, dst + off, want - off);

        if (n <= 0)
        {
            CUSTOM_IMG_LOG("loadin read fail %s got=%u want=%u errno=%d",
                           path, (unsigned)off, (unsigned)want, errno);
            close(fd);
            return 0U;
        }
        off += (uint32_t)n;
    }
    close(fd);
    if (size_out)
    {
        *size_out = want;
    }
    return 1U;
#else
    (void)dst;
    (void)cap;
    (void)size_out;
    return 0U;
#endif
}

#ifdef KERNEL_RTTHREAD
static uint8_t custom_flash_data_ready(void)
{
    struct dfs_filesystem *fs = dfs_filesystem_lookup(CUSTOM_FLASH_PANEL_ROOT);
    const char *name;

    if (!fs || !fs->ops || !fs->ops->name)
        return 0U;
    name = fs->ops->name;
    return (strcmp(name, "lfs") == 0 || strcmp(name, "elm") == 0) ? 1U : 0U;
}

static uint8_t custom_flash_data_recover(void)
{
    int ret;

    (void)dfs_unmount(CUSTOM_FLASH_PANEL_ROOT);
#ifdef LPKG_USING_LITTLEFS
    CUSTOM_IMG_LOG("/data missing or corrupt — mkfs lfs data");
    ret = dfs_mkfs("lfs", "data");
    if (ret != RT_EOK)
    {
        CUSTOM_IMG_LOG("mkfs lfs data fail %d", ret);
        return 0U;
    }
    ret = dfs_mount("data", CUSTOM_FLASH_PANEL_ROOT, "lfs", 0, 0);
#else
    CUSTOM_IMG_LOG("/data missing or corrupt — mkfs elm blk_data");
    ret = dfs_mkfs("elm", "blk_data");
    if (ret != RT_EOK)
    {
        CUSTOM_IMG_LOG("mkfs elm blk_data fail %d", ret);
        return 0U;
    }
    ret = dfs_mount("blk_data", CUSTOM_FLASH_PANEL_ROOT, "elm", 0, 0);
#endif
    if (ret != RT_EOK)
    {
        CUSTOM_IMG_LOG("mount /data after mkfs fail %d", ret);
        return 0U;
    }
    CUSTOM_IMG_LOG("formatted and mounted /data");
    return 1U;
}
#endif

#if defined(AIC_PANEL_DBI_ZC_T1D8BP) && defined(KERNEL_RTTHREAD)
extern const uint8_t factory_boot_gif[];
extern const uint8_t factory_boot_gif_end[];

/* USB save is Panel_1..4 only. After /data is formatted, put the factory
 * boot GIF back so the next play in this boot can find it. */
static void custom_flash_restore_boot_gif(void)
{
    char path[48];
    struct stat st;
    const uint8_t *src = factory_boot_gif;
    uint32_t len = (uint32_t)(factory_boot_gif_end - factory_boot_gif);
    uint32_t off;
    int fd;
    rt_device_t wdt;

    if (len < 6U)
        return;
    rt_snprintf(path, sizeof(path), "%s/Panel_000.gif", CUSTOM_FLASH_BOOT_DIR);
    if (stat(path, &st) == 0 && st.st_size > 0)
        return;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
    {
        CUSTOM_IMG_LOG("boot gif restore open fail %s errno=%d", path, errno);
        return;
    }
    wdt = rt_device_find("wdt");
    CUSTOM_IMG_LOG("boot gif restore %s (%u bytes)", path, (unsigned)len);
    off = 0U;
    while (off < len)
    {
        int n;
        uint32_t chunk = len - off;

        if (chunk > 65536U)
            chunk = 65536U;
        if (wdt)
            rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
        n = write(fd, src + off, chunk);
        if (n <= 0)
        {
            CUSTOM_IMG_LOG("boot gif restore write fail %s wrote=%u errno=%d",
                           path, (unsigned)off, errno);
            close(fd);
            (void)unlink(path);
            return;
        }
        off += (uint32_t)n;
    }
    close(fd);
    CUSTOM_IMG_LOG("boot gif restored %s", path);
}
#endif

static void custom_flash_ensure_panel_dirs(void)
{
#ifdef KERNEL_RTTHREAD
    unsigned i;
    struct stat st;
    uint8_t tried_fmt = 0U;

retry:
    rt_kprintf("custom_image: /data %s\n",
               custom_flash_data_ready() ? "mounted" : "absent");
    if (!custom_flash_data_ready())
    {
        if (!tried_fmt)
        {
            tried_fmt = 1U;
            if (custom_flash_data_recover())
            {
                goto retry;
            }
        }
        CUSTOM_IMG_LOG("/data not mounted, skip Panel dirs");
        return;
    }

#if CUSTOM_FLASH_SIGNAL_ENABLE
    if (stat(CUSTOM_FLASH_SIGNAL_DIR, &st) != 0)
    {
        if (mkdir(CUSTOM_FLASH_SIGNAL_DIR, 0x777) == 0)
        {
            CUSTOM_IMG_LOG("created %s", CUSTOM_FLASH_SIGNAL_DIR);
        }
        else if (!tried_fmt)
        {
            tried_fmt = 1U;
            if (custom_flash_data_recover())
            {
                goto retry;
            }
            CUSTOM_IMG_LOG("mkdir %s failed errno=%d",
                           CUSTOM_FLASH_SIGNAL_DIR, errno);
        }
        else
        {
            CUSTOM_IMG_LOG("mkdir %s failed errno=%d",
                           CUSTOM_FLASH_SIGNAL_DIR, errno);
        }
    }
    (void)i;
    if (stat(custom_flash_panel_dirs[0], &st) != 0)
    {
        if (mkdir(custom_flash_panel_dirs[0], 0x777) == 0)
            CUSTOM_IMG_LOG("created %s", custom_flash_panel_dirs[0]);
        else
            CUSTOM_IMG_LOG("mkdir %s failed errno=%d",
                           custom_flash_panel_dirs[0], errno);
    }
#else
    for (i = 0U; i < CUSTOM_FLASH_PANEL_COUNT; i++)
    {
        if (stat(custom_flash_panel_dirs[i], &st) == 0)
        {
            continue;
        }
        if (mkdir(custom_flash_panel_dirs[i], 0x777) == 0)
        {
            CUSTOM_IMG_LOG("created %s", custom_flash_panel_dirs[i]);
        }
        else if (!tried_fmt)
        {
            tried_fmt = 1U;
            if (custom_flash_data_recover())
            {
                goto retry;
            }
            CUSTOM_IMG_LOG("mkdir %s failed errno=%d",
                           custom_flash_panel_dirs[i], errno);
        }
        else
        {
            CUSTOM_IMG_LOG("mkdir %s failed errno=%d",
                           custom_flash_panel_dirs[i], errno);
        }
    }
#endif
    if (stat(CUSTOM_FLASH_BOOT_DIR, &st) != 0)
    {
        if (mkdir(CUSTOM_FLASH_BOOT_DIR, 0x777) == 0)
        {
            CUSTOM_IMG_LOG("created %s", CUSTOM_FLASH_BOOT_DIR);
        }
        else
        {
            CUSTOM_IMG_LOG("mkdir %s failed errno=%d", CUSTOM_FLASH_BOOT_DIR, errno);
        }
    }
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
    custom_flash_restore_boot_gif();
#if CUSTOM_FLASH_SIGNAL_ENABLE
    CUSTOM_IMG_LOG("gif bytes P0=%u P1=%u signal=%u",
                   (unsigned)custom_image_panel_gif_size(0U),
                   (unsigned)custom_image_file_bytes("/data/Panel_1/Panel_001.gif"),
                   (unsigned)custom_image_file_bytes(CUSTOM_FLASH_SIGNAL_DIR "/Panel_signal.gif"));
#else
    CUSTOM_IMG_LOG("gif bytes P0=%u P1=%u P2=%u P3=%u P4=%u",
                   (unsigned)custom_image_panel_gif_size(0U),
                   (unsigned)custom_image_panel_gif_size(1U),
                   (unsigned)custom_image_panel_gif_size(2U),
                   (unsigned)custom_image_panel_gif_size(3U),
                   (unsigned)custom_image_panel_gif_size(4U));
#endif
#endif
#if defined(AIC_EPD_P118F04)
    if (stat("/data/Eink_Image", &st) != 0)
    {
        if (mkdir("/data/Eink_Image", 0x777) == 0)
            CUSTOM_IMG_LOG("created /data/Eink_Image");
        else
            CUSTOM_IMG_LOG("mkdir /data/Eink_Image failed errno=%d", errno);
    }
#endif
#endif
}

void custom_image_init(void)
{
#ifdef KERNEL_RTTHREAD
    if (!s_custom_img_bus_mx_ready)
    {
        rt_mutex_init(&s_custom_img_bus_mx, "cimg", RT_IPC_FLAG_FIFO);
        s_custom_img_bus_mx_ready = 1U;
    }
    custom_flash_save_thread_start();
#endif
    memset(s_img_cmd_frame, 0, sizeof(s_img_cmd_frame));
    s_pending_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_pending_img_slot = CUSTOM_IMAGE_SLOT_1;
    s_req_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_recv_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_recv_target_slot = CUSTOM_IMAGE_SLOT_1;
    s_receiving_image = 0;
    s_recv_size = 0;
    s_ready_size = 0;
    s_ready_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_ready_slot = CUSTOM_IMAGE_SLOT_1;
    s_img_hdr_cnt = 0U;
    s_img_payload_left = 0U;
    s_recv_mode = CUSTOM_RECV_MODE_RAW;
    s_tail12_cnt = 0;
    s_have_prev_byte = 0;
    s_wait_magic = 0;
    s_magic_win_cnt = 0;
    s_magic_logged = 0;
    s_last_recv_done_ms = 0U;
    s_recv_done_count = 0U;
    s_last_stream_activity_ms = 0U;
    s_frame_interval_ms = 0U;
    s_flash_panel = 0U;
    s_flash_file_arm = 0U;
    s_flash_play_ok = 0U;
    s_flash_play_lvgl[0] = '\0';

    /*
     * --------------------------------------------------------------------
     *  從 CMA 一次性預配 2 個 image buffer (recv + ready)。
     * --------------------------------------------------------------------
     *
     *  過去 (動態 alloc 版本) 每張 show 成功要做一次 free + 一次 malloc，
     *  30 fps × 128 KB 直接砸出 ~3.8 MB/s 的 system heap churn。長時間運行下
     *  heap 末端會碎片化，最後一個 free block 的 metadata 被覆寫 →
     *  CPU Exception NO.6 (Store address misaligned)。
     *
     *  改 CMA 一次性預配後：
     *    - System heap 在這條 path 上完全靜默 (零 alloc / 零 free)
     *    - 2 個 buffer 永久存在；swap 只交換 pointer
     *    - 不再有「heap 末段碎片化」的風險
     *    - 2 MB CMA 用量 (2 × 1 MB)
     *
     *  如果 CMA 預配失敗 (極端罕見)：兩個指標都會是 NULL，prepare_buffer()
     *  回 0，所有 image 命令都會被拒；開機日誌會看到下面的 ERROR 行，
     *  排查時請優先確認 CMA 池有沒有被其他模組佔走。
     */
    s_recv_buf = (uint8_t *)aicos_malloc_align(MEM_CMA,
                                               CUSTOM_IMAGE_BUFFER_SIZE,
                                               CACHE_LINE_SIZE);
    s_ready_buf = (uint8_t *)aicos_malloc_align(MEM_CMA,
                                                CUSTOM_IMAGE_BUFFER_SIZE,
                                                CACHE_LINE_SIZE);
    if (s_recv_buf && s_ready_buf)
    {
        s_recv_cap = CUSTOM_IMAGE_BUFFER_SIZE;
        s_ready_cap = CUSTOM_IMAGE_BUFFER_SIZE;
        CUSTOM_IMG_LOG("CMA recv=%p %u bytes, ready=%p %u bytes",
                       (void *)s_recv_buf, (unsigned)s_recv_cap,
                       (void *)s_ready_buf, (unsigned)s_ready_cap);
    }
    else
    {
        s_recv_cap = 0U;
        s_ready_cap = 0U;
        CUSTOM_IMG_LOG("[ERROR] CMA prealloc FAILED, image RX disabled. recv=%p ready=%p",
                       (void *)s_recv_buf, (void *)s_ready_buf);
    }

    custom_flash_ensure_panel_dirs();
}

static uint8_t custom_image_flash_file_magic(const uint8_t *data, uint32_t len,
                                            uint8_t *type_out)
{
    static const uint8_t png_sig[8] = {
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU
    };

    if (!data || !type_out)
    {
        return 0U;
    }
    if ((len >= 6U) &&
        ((memcmp(data, "GIF87a", 6) == 0) || (memcmp(data, "GIF89a", 6) == 0)))
    {
        *type_out = CUSTOM_IMAGE_TYPE_GIF;
        return 1U;
    }
    if ((len >= 8U) && (memcmp(data, png_sig, 8) == 0))
    {
        *type_out = CUSTOM_IMAGE_TYPE_APNG;
        return 1U;
    }
    return 0U;
}

static void custom_image_begin_flash_file(uint8_t img_type)
{
    if (!custom_image_prepare_buffer())
    {
        CUSTOM_IMG_LOG("flash file: no buffer");
        s_flash_file_arm = 0U;
        return;
    }

    s_req_img_type = img_type;
    s_recv_img_type = img_type;
    s_recv_target_slot = CUSTOM_IMAGE_SLOT_1;
    s_recv_mode = CUSTOM_RECV_MODE_RAW;
    s_receiving_image = 1;
    s_recv_size = 0;
    s_ready_size = 0U;
    s_ready_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_tail12_cnt = 0;
    s_have_prev_byte = 0;
    s_wait_magic = 0;
    s_magic_win_cnt = 0;
    s_magic_logged = 1;
    s_img_hdr_cnt = 0U;
    s_img_payload_left = 0U;
    s_flash_file_arm = 0U;
    s_recv_panel = s_flash_panel;
    custom_image_note_stream_activity_internal();
    CUSTOM_IMG_LOG("flash file recv start type=%u panel=%u",
                   (unsigned)img_type, s_recv_panel);
}

uint8_t custom_image_try_start_flash_file(const uint8_t *data, uint32_t len)
{
    uint8_t img_type = CUSTOM_IMAGE_TYPE_INVALID;

    if (s_receiving_image)
    {
        return 1U;
    }
    if (!s_flash_file_arm || !data || (len == 0U))
    {
        return 0U;
    }

    /* JPEG 串流：控制包後是 CD-BB 或 SOI，不要當成檔案。 */
    if ((len >= CUSTOM_IMAGE_CMD_LEN) &&
        (data[0] == 0xCDU) && (data[1] == 0xBBU) &&
        (data[CUSTOM_IMAGE_CMD_LEN - 1U] == 0xCDU))
    {
        s_flash_file_arm = 0U;
        return 0U;
    }
    if ((len >= 2U) && (data[0] == 0xFFU) && (data[1] == 0xD8U))
    {
        s_flash_file_arm = 0U;
        return 0U;
    }

    if (!custom_image_flash_file_magic(data, len, &img_type))
    {
        return 0U;
    }

    custom_image_begin_flash_file(img_type);
    return s_receiving_image ? 1U : 0U;
}

void custom_image_recv(const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if (!data || len == 0)
    {
        return;
    }

    (void)custom_image_try_start_flash_file(data, len);

    if (s_receiving_image)
    {
        if (len == 18 && data[0] == 0xAA &&
            (data[1] == 0x00 || data[1] == 0x01 || data[1] == 0x02 || data[1] == 0x06))
        {
            CUSTOM_IMG_LOG("drop ctrl frame during image recv, len=%u", len);
            return;
        }
        uint8_t area4_mapped_slot = CUSTOM_IMAGE_SLOT_1;
        uint8_t area4_mapped_type = CUSTOM_IMAGE_TYPE_INVALID;
        if (len == CUSTOM_IMAGE_CMD_LEN &&
            data[0] == 0xCD && data[1] == 0xBB && data[5] == 0xCD &&
            ((data[2] == 0x02 &&
              (data[4] == CUSTOM_IMAGE_SLOT_1 || data[4] == CUSTOM_IMAGE_SLOT_2 || data[4] == CUSTOM_IMAGE_SLOT_3)) ||
             custom_area4_try_parse_cmd(data, &area4_mapped_slot, &area4_mapped_type)))
        {
            CUSTOM_IMG_LOG("drop nested image cmd during image recv");
            return;
        }
        /* Real image payload chunk — keep Area4 idle watchdog alive. */
        custom_image_note_stream_activity_internal();
        for (i = 0; i < len && s_receiving_image; i++)
        {
            /*
             * Bulk fast-path for PNG / APNG / GIF payload.
             *
             * Once magic is detected and mode is RAW, the remainder of every
             * USB chunk is pure payload — for these three formats the
             * byte-by-byte EOF scan in custom_image_check_eof() is explicitly
             * a no-op (see comments at the PNG/APNG/GIF cases there: USB short
             * packet marks end-of-transfer, IEND / GIF trailer 0x3B can appear
             * inside compressed data). The per-byte function call +
             * ensure_capacity check + 11-byte memmove on s_tail12[] is pure
             * overhead.
             *
             * Switching to a single memcpy() per chunk on 300 KB PNGs cuts
             * USB-side processing from ~50–200 ms down to single-digit ms,
             * and removes ~3.3 MB / PNG of wasted memmove traffic.
             *
             * Kept inside the loop body (rather than outside) so the path
             * also kicks in mid-chunk, immediately after magic is detected
             * during the first chunk.
             *
             * JPEG also goes through the fast path: USB bulk short packet (any
             * chunk_len < CUSTOM_USB_CHUNK_FULL) reliably marks end-of-transfer
             * on this device, so the FF D9 byte scan in custom_image_check_eof()
             * is redundant. PC tools send "next CD BB cmd" or close idle after
             * each JPEG, both produce a short packet → finish_by_short_packet().
             * The FF D9 case in check_eof() is left in place as a fallback for
             * non-fast-path callers but is never triggered while this branch is
             * active.
             */
            if (s_recv_mode == CUSTOM_RECV_MODE_RAW &&
                !s_wait_magic &&
                (s_recv_img_type == CUSTOM_IMAGE_TYPE_PNG ||
                 s_recv_img_type == CUSTOM_IMAGE_TYPE_APNG ||
                 s_recv_img_type == CUSTOM_IMAGE_TYPE_GIF ||
                 s_recv_img_type == CUSTOM_IMAGE_TYPE_JPEG))
            {
                uint32_t remain = len - i;
                if (remain > 0U)
                {
                    uint32_t need_size = s_recv_size + remain;
                    if (!custom_image_ensure_capacity(need_size))
                    {
#if defined(KERNEL_RTTHREAD) && CUSTOM_FLASH_SIGNAL_ENABLE
                        if (custom_signal_take_overflow(&data[i], remain))
                            break;
                        if (s_spill_active || s_spill_ptr)
                        {
                            s_spill_need_rearm = 1U;
                            s_usb_rearm_hold = 1U;
                            break;
                        }
#endif
                        custom_image_finish_recv(0);
                        break;
                    }
                    memcpy(&s_recv_buf[s_recv_size], &data[i], remain);
                    s_recv_size = need_size;
                }
                break;
            }

            uint8_t b = data[i];

            if (s_recv_mode == CUSTOM_RECV_MODE_AUTO)
            {
                s_img_hdr_buf[s_img_hdr_cnt++] = b;
                if (s_img_hdr_cnt < CUSTOM_IMAGE_CTRL_FRAME_LEN)
                {
                    continue;
                }

                if (custom_image_is_transport_header20(s_img_hdr_buf))
                {
                    s_recv_mode = CUSTOM_RECV_MODE_TRANSPORT;
                    s_img_payload_left = custom_image_parse_u32_be(&s_img_hdr_buf[8]);
                    s_img_hdr_cnt = 0U;
                    if (s_img_payload_left == 0U)
                    {
                        continue;
                    }
                    continue;
                }
                else
                {
                    uint32_t k;
                    s_recv_mode = CUSTOM_RECV_MODE_RAW;
                    for (k = 0U; k < s_img_hdr_cnt && s_receiving_image; k++)
                    {
                        custom_image_feed_payload_byte(s_img_hdr_buf[k]);
                    }
                    s_img_hdr_cnt = 0U;
                    continue;
                }
            }

            if (s_recv_mode == CUSTOM_RECV_MODE_RAW)
            {
                custom_image_feed_payload_byte(b);
                continue;
            }

            if (s_img_payload_left == 0U)
            {
                s_img_hdr_buf[s_img_hdr_cnt++] = b;
                if (s_img_hdr_cnt < CUSTOM_IMAGE_CTRL_FRAME_LEN)
                {
                    continue;
                }

                if (!custom_image_is_transport_header20(s_img_hdr_buf))
                {
                    /* Non-transport noise while receiving image; ignore this 20-byte window and keep scanning. */
                    memmove(&s_img_hdr_buf[0], &s_img_hdr_buf[1], CUSTOM_IMAGE_CTRL_FRAME_LEN - 1U);
                    s_img_hdr_cnt = CUSTOM_IMAGE_CTRL_FRAME_LEN - 1U;
                    continue;
                }

                s_img_hdr_cnt = 0U;
                s_img_payload_left = custom_image_parse_u32_be(&s_img_hdr_buf[8]);
                if (s_img_hdr_buf[2] == 0x02U)
                {
                    /* Explicit image end packet from host. */
                    custom_image_finish_recv(1);
                    break;
                }
                if (s_img_payload_left == 0U)
                {
                    continue;
                }
            }
            else
            {
                custom_image_feed_payload_byte(b);
                s_img_payload_left--;
            }
        }
        if (s_recv_mode == CUSTOM_RECV_MODE_RAW && len < CUSTOM_USB_CHUNK_FULL)
        {
            custom_image_finish_by_short_packet();
        }
#if defined(KERNEL_RTTHREAD) && CUSTOM_FLASH_SIGNAL_ENABLE
        /* A second 2 MB buffer is filling while the first is programmed.
         * Stop USB before that buffer overflows and the file is aborted. */
        if ((s_spill_active || s_spill_ptr) && s_recv_buf &&
            (s_recv_cap <= s_recv_size ||
             ((s_recv_cap - s_recv_size) < CUSTOM_USB_CHUNK_FULL)))
        {
            s_spill_need_rearm = 1U;
            s_usb_rearm_hold = 1U;
        }
#endif
        return;
    }

    if (len != CUSTOM_IMAGE_CMD_LEN)
    {
        return;
    }

    memcpy(s_img_cmd_frame, data, CUSTOM_IMAGE_CMD_LEN);
    if (s_img_cmd_frame[0] != 0xCD ||
        s_img_cmd_frame[1] != 0xBB ||
        s_img_cmd_frame[5] != 0xCD)
    {
        return;
    }

    if (s_img_cmd_frame[2] == 0x02)
    {
        if (s_img_cmd_frame[4] != CUSTOM_IMAGE_SLOT_1 &&
            s_img_cmd_frame[4] != CUSTOM_IMAGE_SLOT_2 &&
            s_img_cmd_frame[4] != CUSTOM_IMAGE_SLOT_3)
        {
            return;
        }

        if (s_img_cmd_frame[3] != CUSTOM_IMAGE_TYPE_JPEG &&
            s_img_cmd_frame[3] != CUSTOM_IMAGE_TYPE_PNG &&
            s_img_cmd_frame[3] != CUSTOM_IMAGE_TYPE_GIF &&
            s_img_cmd_frame[3] != CUSTOM_IMAGE_TYPE_APNG)
        {
            return;
        }
    }
    else
    {
        /*
         * Area2 / Area3 路徑已移除（2026-06-16）。只剩 Area4 fixed-purpose
         * command (0xCD 0xBB 0x06 0x01 0x00 0xCD)，內部會把 byte3 normalize
         * 成 PNG（1280x800）。其他不認的 6-byte cmd 一律 drop。
         */
        uint8_t area4_type = CUSTOM_IMAGE_TYPE_INVALID;
        if (custom_area4_try_parse_cmd(s_img_cmd_frame, &s_recv_target_slot, &area4_type))
        {
            s_img_cmd_frame[3] = area4_type;
        }
        else
        {
            return;
        }
    }

    if (custom_image_prepare_buffer())
    {
        s_req_img_type = s_img_cmd_frame[3];
        s_recv_img_type = s_img_cmd_frame[3];
        if (s_img_cmd_frame[2] == 0x02)
        {
            s_recv_target_slot = s_img_cmd_frame[4];
            s_recv_mode = CUSTOM_RECV_MODE_TRANSPORT;
        }
        else
        {
            s_recv_mode = CUSTOM_RECV_MODE_AUTO;
        }
        CUSTOM_IMG_VLOG("cmd accepted, req_type=%u slot=%u, chunk_len=%u", s_img_cmd_frame[3], s_img_cmd_frame[4], len);
        s_receiving_image = 1;
        /* CD BB 06: JPEG/PNG stream. Stop the GIF before the first payload byte. */
        if (s_img_cmd_frame[2] == 0x06U)
            custom_round_jpeg_stream_begin();
        custom_image_note_stream_activity_internal();
        s_recv_size = 0;
        s_tail12_cnt = 0;
        s_have_prev_byte = 0;
        s_wait_magic = 1;
        s_magic_win_cnt = 0;
        s_magic_logged = 0;
        s_img_hdr_cnt = 0U;
        s_img_payload_left = 0U;
    }
    else
    {
        CUSTOM_IMG_LOG("cmd accepted but buffer alloc failed");
    }
}

uint8_t custom_image_take_pending(uint8_t *slot_out)
{
    uint8_t img_type = s_pending_img_type;
    uint8_t slot = s_pending_img_slot;

    s_pending_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_pending_img_slot = CUSTOM_IMAGE_SLOT_1;
    if (slot_out)
    {
        *slot_out = slot;
    }
    return img_type;
}

const uint8_t *custom_image_get_data(uint8_t img_type, uint32_t *size)
{
    if (size)
    {
        *size = 0;
    }

    if (img_type != s_ready_img_type)
    {
        return NULL;
    }

    if (!s_ready_buf || s_ready_size == 0)
    {
        return NULL;
    }

    if (size)
    {
        *size = s_ready_size;
    }

    return s_ready_buf;
}

/*
 * Release 語意改變：
 *   過去 (動態 alloc 版本)：free() buffer，下次 prepare_buffer 會 malloc。
 *   現在 (CMA 預配版本)：buffer 永久存在，這裡只清狀態 (size = 0, type =
 *   INVALID)，pointer 不動，下次 recv 直接寫入。
 *
 * 為什麼還要保留 release_payload(const void *) 介面？
 *   - 呼叫者 (custom.c 主 tick) 用 rel_payload (= get_data 結果) 判斷 "哪一個
 *     buffer 是本次 show 用過、可以回收"。
 *   - 在 swap 模型下，s_ready_buf 可能在 LVGL 還在 decode 時就被 USB 端
 *     swap 換掉；release_payload 拿到的 rel_payload 是「show 進來時的指標」，
 *     可能是現在的 s_ready_buf 或 s_recv_buf 其中之一。
 *   - 兩個 buffer 都是 prealloc，所以實質上「不管 rel_payload 指哪個都不 free」，
 *     只是清 size / type 狀態。
 *
 * 注意：清狀態時要小心 — 如果 rel_payload 是「之前 swap 過去的 OLD recv」
 * (= 現在的 s_recv_buf)，這個 buffer 下一次 recv 就會被覆寫，所以不需要清狀態
 * (recv 開始時會重設 s_recv_size = 0)。只有 s_ready_buf 的狀態需要清，否則
 * 下一個 get_data() 還會看到 stale size/type。
 */
void custom_image_release_ready(void)
{
    /*
     * 不 free 預配 buffer！只清狀態。
     * Buffer 留著給下一張圖：USB recv 完 swap 後它會變成 s_recv_buf，
     * 再下一張 recv 直接寫到這裡，零 alloc。
     */
    s_ready_size = 0U;
    s_ready_img_type = CUSTOM_IMAGE_TYPE_INVALID;
    s_ready_slot = CUSTOM_IMAGE_SLOT_1;
}

void custom_image_release_payload(const uint8_t *payload)
{
    if (!payload)
    {
        return;
    }
    if (payload == s_ready_buf)
    {
        /* 標記 ready buffer 已被消費；buffer 本身留著供下一張 recv 複用 */
        custom_image_release_ready();
        return;
    }
    /*
     * payload == s_recv_buf 的情境：
     *   show 進來時 rel_payload = 當時的 s_ready_buf。如果 LVGL 解碼期間 USB
     *   又完成了下一張，swap 把舊 ready 換成 s_recv_buf，這時 rel_payload 變
     *   成「現在的 s_recv_buf」。過去版本要 free 它，但現在 prealloc 直接留著
     *   ──下次 recv 自然會覆寫 s_recv_size 從 0 起寫，舊資料就被蓋掉。
     *
     *   沒有任何狀態變數需要清 (s_recv_size 在 recv start 重設，
     *   s_recv_img_type 在 recv 開始時設新值)。
     */
    (void)payload;
}

uint8_t custom_image_resync_wait(uint8_t *type_io, uint8_t *slot_io)
{
    uint32_t sz = 0U;

    if (!type_io || !slot_io)
    {
        return 0U;
    }
    if (*type_io != CUSTOM_IMAGE_TYPE_INVALID &&
        custom_image_get_data(*type_io, &sz) != NULL && sz > 0U)
    {
        return 0U;
    }
    if (!s_ready_buf || s_ready_size == 0U || s_ready_img_type == CUSTOM_IMAGE_TYPE_INVALID)
    {
        return 0U;
    }
    if (*type_io == s_ready_img_type && *slot_io == s_ready_slot)
    {
        return 0U;
    }
    *type_io = s_ready_img_type;
    *slot_io = s_ready_slot;
    return 1U;
}
