# DDR 圖片顯示記憶體使用說明（D215）

> 適用平台：D215 (`AIC_VE_DRV_V10` + `AIC_GE_DRV_V10`)
> 適用面板：JD9365 (`CONFIG_AIC_PANEL_DSI_JD9365=y`)，800 × 800 ARGB8888
> LVGL 版本：v9
> 文件對應 commit 狀態：
> - custom_Area4 採 LVGL widget path + back-pressure (single in-flight, timeout **500 ms**)
> - **custom_Image 採 CMA 一次性預配 recv/ready buffer**（消除 system heap churn，NO.6 crash 後加上）
> - **`LV_CACHE_DEF_SIZE` 從 8 MB 縮為 2 MB**（釋出 6 MB 回 system heap）
> - **`aicos_malloc_try_cma` retry cap = 32**（無限 loop 防護，SoC-wide freeze 後加上）
> - **每 ~10 秒一筆 `heap snapshot:` log**（freeze 事後追蹤用）

本文件整理目前圖片顯示路徑使用 DDR 的全貌：物理 layout、各 buffer 出處、每張圖在記憶體裡的流動，以及目前已知的記憶體相關痛點。配合 `custom_Image_流程說明.md`、`custom_Area3_流程說明.md`、`custom_Area2_流程說明.md` 一起看會更完整。

---

## 1. DDR 物理布局

D215 上 64 MB DRAM，linker / DRAM driver 拆成兩段：

```
0x40000000 ┬─────────────────────────────────────────┐ ← __dram_start
           │                                          │
           │  .text + .data + .bss (~2 MB)            │
           │  ────────────────────────────────────    │
           │  System Heap (RT-Thread malloc)          │   16 MB
           │  __heap_start = 0x4020A088               │  (DRAM_SW)
           │  __heap_end   = 0x41000000               │
           │  → 約 14 MB 可用                          │
           │                                          │
0x41000000 ┼─────────────────────────────────────────┤ ← __cma_heap_start
           │                                          │
           │  CMA Heap (heap_cma)                     │
           │  專供 HW DMA / GE / VE / DE 用            │   48 MB
           │  aicos_malloc(MEM_CMA, ...) 來源          │  (DRAM_CMA)
           │                                          │
0x44000000 ┴─────────────────────────────────────────┘ ← __cma_heap_end
```

設定來源：

| 項目 | 來源 | 值 |
|---|---|---|
| 總 DRAM | `.config` `CONFIG_AIC_DRAM_TOTAL_SIZE` | `0x4000000` (64 MB) |
| CMA 區段 | `.config` `CONFIG_AIC_DRAM_CMA_SIZE` | `0x3000000` (48 MB) |
| CMA enable | `.config` `CONFIG_AIC_DRAM_CMA_EN` | `y` |
| 符號定義 | `output/d21x.map` line 18647-18655 | 見上圖 |
| heap init | `target/d21x/d215-demo88-nor-dacai03/board.c` `aic_memheap_init()` | 把 CMA 註冊成 `heap_cma` memheap |

### 兩個 heap 的差異與用途

| 區別 | System Heap (DRAM_SW) | CMA Heap (DRAM_CMA) |
|---|---|---|
| 物理位置 | 低 16 MB | 高 48 MB |
| 是否 contiguous | 不保證 | **保證 contiguous** |
| 是否 cache-coherent friendly | 通用 | 對 DMA 友善（align CACHE_LINE_SIZE） |
| 配置 API | `malloc / realloc / free`、`rt_malloc`、`lv_malloc` | `aicos_malloc(MEM_CMA, ...)`、`aicos_malloc_align(MEM_CMA, ...)`、`aicos_malloc_try_cma()` |
| 主要使用者 | LVGL objects、styles、image cache、壓縮圖檔 byte stream、USB stack、RT-Thread | Framebuffer、MPP decoded frame、GE2D 來源/目的 buffer、DSI / DBI command buffer |
| HW peripheral 可否直接存取 | ❌（需先 dcache flush + 物理位址轉換，且不保證 contiguous） | ✅（CMA 設計目的） |

---

## 2. System Heap (~14 MB) 上的圖片相關 buffer

> 目的：放「**壓縮圖檔 byte 流**」（PNG / JPEG raw bytes），以及 LVGL 自身的 object / cache 結構。
> HW (GE2D / VE / DE) **不會直接 DMA 進這裡**。

| Buffer / 用途 | 大小 | 分配方式 | 出處 | 備註 |
|---|---|---|---|---|
| **LVGL global image cache** | **2 MB** ← 縮小 | LVGL 內部一次性 reserve | `packages/artinchip/lvgl-ui/lvgl_v9/lv_conf.h:163` `#define LV_CACHE_DEF_SIZE (2 * 1024 * 1024)` | LRU rb-tree；所有 widget 共用；**`aicos_malloc_try_cma` fallback 會去 evict 它**（雪崩 chain 的下游）。從 8 MB 砍到 2 MB 後 race window 大幅縮短 |
| `s_main_1_disp_buf[3]` | 動態 each | `malloc / realloc` | `custom.c` | screen_1 area1/2/3 各 slot 各一條（非 streaming path，churn 低） |
| `s_main_1_gif_buf[3]` | 動態 each | `malloc / realloc` | `custom.c` | GIF 專用緩衝 |
| `s_area3_disp_buf[N]` | 動態 each | `malloc / realloc` | `custom_Area3.c` line 176 | Area3 每張 slot 壓縮 byte（非 streaming，churn 低） |
| `s_area3_gif_buf[N]` | 動態 each | `malloc / realloc` | `custom_Area3.c` | Area3 GIF byte |
| LVGL objects + styles + lv_malloc pool | ~3-5 MB | LVGL 內 alloc (走 lv_libc → malloc) | LVGL 內部 | obj / style / event / draw_task 結構 |
| USB stack buffer | ~MB 級 | CherryUSB | `bsp/peripheral/usb/cherryusb` | vendor IN/OUT buffer，目前 OUT = 128 KB |
| RT-Thread task stack + msgq | ~MB | RT-Thread kernel | kernel | thread 控制塊 + stack |

> **重大變更**：`s_recv_buf` / `s_ready_buf` 過去用 `malloc/realloc` 在 system heap 動態管理，每張圖一次 `free + malloc` 製造 ~3.8 MB/s churn → 已搬到 CMA 一次性預配（見 §3）。

### Heap 鬆綁狀況（修改後）

| 項目 | 大小 |
|---|---|
| `__heap_end - __heap_start` | ~14 MB |
| 扣掉 `LV_CACHE_DEF_SIZE` | -2 MB（過去是 -8 MB）|
| 扣掉 USB / RT-Thread / LVGL objects 約 | -4 MB |
| **可動用實際 free** | **~8 MB** ← 從 ~2 MB 提升 4 倍 |

→ system heap 大幅鬆綁。**主要 streaming path 的 `s_recv_buf` / `s_ready_buf` 已不在 system heap**，這條 heap 不會再被 30 fps 高頻 alloc/free 推到碎片化邊界，**NO.6 (Store misaligned at `0x40ffffd0`) crash 的根本原因消除**。

> **設計原則**：所有「**高頻**或**大塊**」的 buffer 都應該預配到 CMA。System heap 留給 LVGL 內部小 object / RT-Thread / USB stack 等中低頻使用者。

---

## 3. CMA Heap (48 MB) 上的圖片相關 buffer

> 目的：給 HW（DE/GE/VE）直接 DMA 存取的 buffer。**全部 cache-line align、physical contiguous**。

| Buffer / 用途 | 大小 | 生命週期 | 分配方式 | 出處 |
|---|---|---|---|---|
| **Framebuffer** | **~5 MB** (800 × 800 × 4B × 2 buf) | 開機一次性 | `aicos_malloc_align(MEM_CMA, fb_size, CACHE_LINE_SIZE)` | `bsp/artinchip/drv/display/drv_fb.c:618` |
| **Area4 `s_area4_disp_buf`** | **2 MB** | 開機一次性，永久持有 | `aicos_malloc_align(MEM_CMA, 2*1024*1024, CACHE_LINE_SIZE)` | `custom_Area4.c` line 347 |
| **`s_recv_buf` (custom_Image)** ★ | **2 MB** | 開機一次性，永久持有 | `aicos_malloc_align(MEM_CMA, 2*1024*1024, CACHE_LINE_SIZE)` | `custom_Image.c` `custom_image_init()` |
| **`s_ready_buf` (custom_Image)** ★ | **2 MB** | 開機一次性，永久持有 | `aicos_malloc_align(MEM_CMA, 2*1024*1024, CACHE_LINE_SIZE)` | `custom_Image.c` `custom_image_init()` |
| **MPP packet buffer** | per decode，~64 KB - 1 MB | per-frame，open 配 / close 釋 | `mpp_decoder_get_packet()` → 內部 alloc | `packages/artinchip/mpp/...` |
| **MPP decoded frame** ★ | per decode，**~2.5 MB (PNG ARGB) / ~1.5 MB (JPEG YUV420)**<br>(對應 800×800 panel；若解 1280×800 原圖再縮就會配 1280×800×4 = **~4 MB**) | **per-frame，open 配 / close 釋** | `aicos_malloc_try_cma(size + CACHE_LINE_SIZE - 1)` | `lv_mpp_dec.c:459` `lv_frame_buf_alloc()` |
| **MPP ext_frame_allocator** | 32 B 控制結構 | per decode | `malloc()` (system heap) | `lv_mpp_dec.c:428` `lv_open_allocator()` |
| GE2D internal cmdq | <1 MB | 開機一次性 | GE2D driver | `bsp/artinchip/drv/ge/...` |
| GE2D scratch buffer (CSC / rotation tmp) | 視 op | per-op | GE2D driver 內 | |
| DSI / DBI command buffer | 4 KB each | 開機一次性 | `aicos_malloc(MEM_CMA, 4096)` | `drv_fb_helper.c:446 / 480 / 568` |

> ★ `s_recv_buf` / `s_ready_buf` 是本次（修 NO.6 crash）新搬到 CMA 的。把 system heap 上每張圖 ~3.8 MB/s 的 alloc/free churn 完全消除。

### CMA 使用佔比（修改後，穩態 30 fps streaming）

| 項目 | 佔用 |
|---|---|
| FB ×2 | ~5 MB |
| Area4 prealloc | 2 MB |
| **s_recv_buf prealloc** | **2 MB** ← 新增 |
| **s_ready_buf prealloc** | **2 MB** ← 新增 |
| MPP packet (in-flight) | ~1 MB |
| MPP decoded frame (**in-flight**) | ~2.5 MB |
| GE2D + DSI | ~1 MB |
| **合計** | **~15.5 MB / 48 MB** |
| **CMA free 餘量** | **~32 MB** |

→ CMA 多用 4 MB（從 ~11.5 MB → ~15.5 MB），仍有 32 MB 餘裕；換來 system heap 高頻 churn 完全消失。**這是非常划算的交換**。

---

## 4. 一張圖從 PC 到 LCD 的完整流動

以 Area4 streaming JPEG 為例（PC 每張送 ~57 KB JPEG，目標 30 fps）：

```
PC
 │
 │  USB Bulk OUT (64 KB short packet 結束一張)
 ▼
[CherryUSB IRQ thread]
 │  bulk memcpy (vendor_out_callback)
 ▼
s_recv_buf  ── CMA (固定 2 MB，永久 prealloc)  ← 改自 System Heap 動態
 │
 │  EOF 偵測 (JPEG short packet)
 │  → custom_image_finish_recv(1)
 │  → swap pointer: s_recv_buf ↔ s_ready_buf
 │     （兩個 buffer 都是 CMA prealloc，swap 只動指標，**零 alloc / 零 free**）
 ▼
s_ready_buf  ── CMA (固定 2 MB，永久 prealloc)  ← 改自 System Heap 動態
 │
 │  [LVGL UI thread, custom main tick 每 10 ms 喚醒一次，
 │   custom_image_finish_recv 還會 wake_tick_timer 加速]
 │  custom_show_image_by_type → custom_area4_show_image
 │  ─ back-pressure gate ─
 │   若 s_perf_meas_pending == 1 → drop (return 0)
 │  ─────────────────────
 │  memcpy(s_area4_disp_buf, s_ready_buf, size)
 ▼
s_area4_disp_buf  ── CMA (固定 2 MB，永久 prealloc)
 │
 │  dsc setup:
 │    lv_img_dsc_t.data = s_area4_disp_buf
 │    lv_img_dsc_t.header.cf = LV_COLOR_FORMAT_RAW
 │    lv_img_dsc_t.header.flags = LV_IMAGE_FLAGS_USER4  (bypass image cache)
 │
 │  lv_image_set_src + lv_obj_invalidate + arm s_perf_meas_pending=1
 ▼
[LVGL render tick, 同樣 UI thread]
 │
 │  lv_image_decoder_open
 │   └─ check USER4 → args.no_cache = true (skip try_cache)
 │   └─ run decoder list → match MPP HW decoder
 │       lv_mpp_dec_open:
 │         mpp_decoder_create(JPEG)
 │         lv_frame_buf_alloc(buf_size, cf):
 │           ★★★ aicos_malloc_try_cma(~2.5 MB)
 │              ├─ 成功 → 返回 CMA pointer
 │              └─ 失敗 → lv_drop_one_cached_image() ← 雪崩 trigger
 │                       (跨 heap 摸 System Heap 上的 LVGL cache rb-tree)
 │         lv_open_allocator (malloc 32 B, system heap)
 │         mpp_decoder_init(EXT_FRAME_ALLOCATOR)
 │         put_packet → memcpy bytes → decode → get_frame
 ▼
MPP decoded frame  ── CMA (~2.5 MB, per-frame alloc)
 │
 │  GE2D HW blit (CMA → CMA)
 │   srf = MPP frame (ARGB / YUV)
 │   dst = FB back buffer
 │   ops = format convert + scale + rotation
 ▼
Framebuffer back buf  ── CMA (FB pool 5 MB 雙緩衝之一)
 │
 │  disp_flush (LVGL flush_cb)
 │   ioctl(AICFB_WAIT_FOR_VSYNC)  ← 等 vsync
 │   ioctl(AICFB_PAN_DISPLAY)     ← 切 active buffer
 ▼
Framebuffer front buf  ── CMA
 │
 │  DE (display engine) raster scan
 ▼
DSI controller
 │
 │  MIPI DSI packets
 ▼
JD9365 LCD (800 × 800)  ← 螢幕真的顯示出來
 │
 │  [LVGL render done → LV_EVENT_DRAW_POST_END]
 │   custom_area4_draw_done_cb:
 │     - 記錄 render latency
 │     - 清 s_perf_meas_pending = 0  ← back-pressure 釋放，可接下一張
 │
 ▼
lv_image_decoder_close
 └─ lv_mpp_dec_close
      └─ mpp_dec_data_release  → free MPP frame buffer back to CMA
                                 (~2.5 MB 回收，下一張會再 alloc)
```

### Per-frame 記憶體動作摘要（修改後）

| 階段 | System Heap 動作 | CMA Heap 動作 |
|---|---|---|
| USB 進來 | **完全無動作** ← 過去會 realloc s_recv_buf | 寫入預配的 s_recv_buf |
| finish_recv | **完全無動作** ← 過去 swap 雖然不 alloc 但仍動 system heap pointer | swap pointer (CMA ↔ CMA) |
| Area4 接手 | — | memcpy → s_area4_disp_buf (固定，無 alloc) |
| lv_image_decoder_open | malloc ext_frame_allocator (32 B) | **alloc MPP frame ~2.5 MB** ★ |
| MPP decode | — | 寫入 MPP frame |
| GE2D blit | — | 讀 MPP frame + 寫 FB |
| LVGL draw_done | — | — |
| lv_image_decoder_close | free ext_frame_allocator (32 B) | **free MPP frame ~2.5 MB** ★ |
| **release_payload** | **完全無動作** ← 過去會 free 128 KB | — |

★ = 唯一還有的 per-frame CMA alloc/free。由 back-pressure 限制單一 in-flight (見 §6) 保證不會推到 CMA 邊界。

→ 修改後 **system heap 完全沒有 per-frame 動作**，**邊界永遠不會被觸碰**，NO.6 crash chain 徹底斷掉。

---

## 5. 已知記憶體相關痛點（含修改後狀態）

### 痛點 0：**System Heap 末端碎片化（NO.6 crash）** — ✅ 已修

**症狀**（出現在 back-pressure 已生效但 LVGL widget path 跑 ~3 分鐘後）：

```
CPU Exception: NO.6 (Store address misaligned)
a0 = 0x40ffffd0    ← system heap end - 0x30 (最後一個 memheap_item)
mtval = 0x..f60008f  ← 從 corrupt block 讀出的垃圾 pointer
mepc/ra 都在 rt_memheap_alloc / split / free 內部
```

**Root cause**：
- 過去 `s_recv_buf` / `s_ready_buf` 用 `malloc / realloc / free` 在 system heap 動態管理
- 每次 show 成功會 `free` 一個 128 KB block，下次 recv 又 `malloc` 一個
- 30 fps × 128 KB = ~3.8 MB/s 的 alloc/free churn
- 長時間運行 → heap 末段碎片化 → 某次 alloc 從末端 free block 切時，walk 到被「鄰居越界寫入」破壞的 metadata → 整個 free list crash

**修法**（已 commit）：
- 把 `s_recv_buf` / `s_ready_buf` 改成 CMA 一次性預配 2 個 2 MB buffer
- runtime 完全沒有 `malloc / realloc / free` 動作
- system heap 在 image RX path 上**零 churn**

**驗證**：開機應該看到 `custom_image: CMA preallocated recv=0x... ready=0x... (2097152 bytes each)`

### 痛點 1：CMA 上每 frame ~2.5 MB 的 alloc/free churn（still relevant）

雖然 open / close 配對乾淨，但：
- 高頻 (30 fps) 下，allocator 內部 free list 會碎片化
- MPP frame 跟 GE2D 暫存可能交錯落在 CMA 中段
- 連續 alloc 同樣 size 雖然好 reuse，但**第一張到第 N 張中間可能踩到 free list 邊界**

實測表現：穩定運行 30 秒（約 900 frames）後 CMA 接近滿，觸發 `aicos_malloc_try_cma` 的 fallback path。

### 痛點 2：CMA 滿時的 fallback 會跨 heap 摸 LVGL cache

這是過去 crash 的 root cause path：

```c
// kernel/common/osal/aic_osal.c:86
void *aicos_malloc_try_cma(size_t size) {
#if defined(LPKG_USING_LVGL) && defined(LVGL_V_9)
extern bool lv_drop_one_cached_image();
    while (1) {
        void *data = aicos_malloc(MEM_CMA, size);
        if (data) return data;
        bool res = lv_drop_one_cached_image();  // ← 跨 heap 摸 System Heap 上的 cache
        if (res == false) return NULL;
    }
#else
    return aicos_malloc(MEM_CMA, size);
#endif
}
```

`lv_drop_one_cached_image()` 內部呼 `lv_cache_evict_one()` 動 LVGL global image cache 的 **LRU red-black tree**。雖然 evict 函式自帶 mutex (`lv_cache.c:171`)，但**evict 的 victim 是 system heap 上其他 widget (font glyph / datetime icon / Area3 等) 的 cache entry**，跟 LVGL UI thread 同時的 render walk 之間有 race window，會造成：

```
[Warn] lv_cache_entry_dec_ref: ref_cnt(4242189077) < 0
CPU Exception: NO.4   mtval=0x..55f7ee8e (隨機 byte pattern 寫進指標)
```

### 痛點 3：System Heap 吃緊 — ✅ 已大幅鬆綁

| 項目 | 修改前 | 修改後 |
|---|---|---|
| 14 MB 總額 | | |
| LVGL cache | -8 MB | **-2 MB** (lv_conf.h: `LV_CACHE_DEF_SIZE` 從 8 MB → 2 MB) |
| LVGL objects + styles | -3 MB | -3 MB |
| USB / RT-Thread / 其他 | -3 MB | -3 MB |
| `s_recv_buf` / `s_ready_buf` | 動態，最差 -3 MB | **0**（搬到 CMA） |
| **實際 free** | **~0-2 MB** | **~6-8 MB** |

→ 修改後 system heap 不會在 image RX path 上動，且 LVGL cache 縮小後 6 MB 立刻回到 free pool。即使有別的 widget 用 lv_malloc 大塊，現在也很充裕。

---

## 6. 現在的全套對策

從 root cause 出發，目前**三層防護**同時生效：

### 第 1 層：CMA 一次性預配 recv/ready buffer（修 NO.6 crash）

`custom_image_init()`：
- 開機階段一次配 2 個 2 MB CMA buffer
- runtime 永不 alloc/realloc/free
- swap 只動 pointer

→ **system heap 在 image path 上完全靜默 → 邊界永遠不會被觸碰**

### 第 2 層：LVGL image cache 從 8 MB → 2 MB（緩解 race window）

`lv_conf.h`：
- 縮小 `LV_CACHE_DEF_SIZE` 從 8 MB 砍到 2 MB
- 即使第 3 層 fallback 真的 trigger，要 evict 的 entry 數量也少很多

→ **跨 heap race window 縮短到原本的 1/4**

### 第 3 層：Back-pressure single in-flight render（修 NO.4 crash）

`custom_Area4.c`：
1. 限制單一 in-flight render：直接復用既有的 `s_perf_meas_pending` (show 入口 arm，`LV_EVENT_DRAW_POST_END` 清)
2. show_image 入口檢查 pending == 1 且 arm < **500 ms** → 直接 drop (return 0)
3. 500 ms timeout：if pending 卡 >500ms，視為 callback 漏掉，force-clear 並讓本張通過，同時印一行 warning log
4. 每 100 次 drop 印一行 `back-pressure drop count=N` 方便驗證

> ⚠️ Timeout 從 200 ms 拉到 500 ms 的原因：在「跑 10+ 分鐘後 SoC freeze」這次的 meminfo 看到 CMA `max_used = 23 MB`，比 baseline 15.5 MB 多了 ~7.5 MB ≈ 3 個 MPP frame。對應的就是 200 ms timeout 在 LVGL 偶發 busy 時誤觸發 force-through，造成 in-flight render 數量短暫長到 3。拉到 500 ms 後一般 pipeline 延遲都不會踩到，CMA peak 應該收斂回 baseline。

→ **同一時間 CMA 只持有 1 個 MPP frame，永遠摸不到 try_cma fallback 邊界**

### 第 4 層：try_cma retry cap + heap snapshot 診斷（修 SoC-wide hang 風險）

針對第 3 次發生但無 CPU exception 的「**SoC-wide freeze**」（uart kprintf 也停了、aic console 也死了、USB 也停），補上兩道防禦 + 一道診斷：

**A. `aicos_malloc_try_cma` retry cap (`kernel/common/osal/aic_osal.c:86`)**

原本是 `while(1)` 配合 `lv_drop_one_cached_image()` 的 fallback。雖然 evict 用盡終究會回 NULL 退出，過程中卻會把 LVGL cache 整個掃光，並可能跟 UI thread 走 cache 的 read path 發生競爭。
- 改成 **最多 32 次 retry** 就強制回 NULL
- caller (MPP / GE2D) 拿到 NULL 會回報 decode 失敗、釋放 input packet，下次 frame 重新嘗試 → 系統一定能 graceful recover，不會卡死

```c
#define AICOS_MALLOC_TRY_CMA_MAX_RETRY 32

void *aicos_malloc_try_cma(size_t size) {
    int retry = AICOS_MALLOC_TRY_CMA_MAX_RETRY;
    while (retry-- > 0) {
        void *data = aicos_malloc(MEM_CMA, size);
        if (data) return data;
        if (lv_drop_one_cached_image() == false) return NULL;
    }
    return NULL;  // give up; caller graceful-fails
}
```

**B. Periodic heap snapshot log（`custom_Area4.c` perf stats 區）**

之前的 freeze 沒留下任何 crash dump，要往回 trace 完全沒線索。現在 perf stats 每 ~10 秒會額外印一行：

```
custom_area4: heap snapshot: sys total=14638648 used=11499028 max=11499028 avail=3139620 (run `meminfo` for CMA)
```

下次再 hang 就能對照「最後一筆 snapshot 是不是 avail 在快速掉、或 used 已經漲到 total 接近 0 avail」。CMA 仍是用 `meminfo` 互動式查（rt_memory_info 看不到 CMA）。

### 結果（穩態）

- CMA 用量上限 ≈ 15.5 MB / 48 MB (餘 32 MB)
- System heap 多了 ~6 MB 餘裕
- fps ~25-32（受 render 速度上限，跟之前一樣）
- drop% 略升（PC 推太快，但 device 沒有 backpressure 訊號給 PC，只能 drop）
- **三層 crash chain 全部斷掉**：
  - ✅ NO.4 (LVGL cache rb-tree corruption) → 第 3 層解
  - ✅ NO.6 (system heap end corruption) → 第 1 層解
  - ✅ NO.4 變體 (lv_drop fallback evict race) → 第 2 層緩解 + 第 3 層根除
- ⚠️ **SoC-wide freeze (10+ 分鐘後，無 crash log)**：第 4 層的 try_cma cap 排除其中一個風險路徑；但**這類 freeze 的真正 root cause 仍未確認**（見下節）

---

## 6.5. SoC-wide hang 現象分析（第 3 次出現的 freeze）

### 觀察事實

從 `Debug_Issue/20260527/` 的 log：
- PC 端：穩定 30~37 fps 推 JPEG 推了 **10 分以上**，然後突然 `send=0.0ms drop=40` 開始連續
- Device 端：UART debug 完全停了，連 `aic />` shell 都沒反應，console 不收輸入
- 中段 user 手動跑了一次 `meminfo`，看到：
  ```
  heap_cma         pool=50331648  max_used=23033144  avail=28562192
  heap_sys         pool=14638648  max_used=11499028  avail= 3141824
  ```
  - System heap：max 11.5 MB / 14.6 MB → 還有 3.1 MB free，**沒爆**
  - CMA：max 23 MB / 48 MB → 還有 28.5 MB free，**沒爆**
  - 但 CMA peak (23 MB) 比 baseline (15.5 MB) 高了 **7.5 MB ≈ 3 個 MPP frame**，暗示 back-pressure 失效過

### 跟前兩次 crash 的差異

| 項目 | NO.4 / NO.6 crash | SoC-wide freeze (本次) |
|---|---|---|
| 有 CPU Exception dump？ | ✅ 有，可定位到 LVGL cache 或 memheap 內部 | ❌ 完全沒有，連 uart kprintf 都停 |
| UI / disp_flush 還在跑？ | 通常掛在某行 panic 就停了 | freeze 前是還在跑，**突然全部一起靜止** |
| Hang 影響範圍 | 單一 thread（要 watchdog 才 reset） | **連 aic console、kprintf、USB 一起死** |

「連 kprintf 都停了」這條訊息很關鍵：kprintf 走的是 console driver 的 UART 寄存器，**如果 CPU 還能正常跑指令，就算所有 thread 都 deadlock，至少 panic / shell 還是能寫 uart**。它停了表示要不是：
- (a) **CPU 卡在某個 IRQ handler 的死迴圈**（IRQ storm，user thread 沒機會跑）
- (b) **AXI / AHB bus 被卡住**（某個 MMIO 讀取直接 stall CPU）
- (c) **DMA / cache 控制器 fault**

這些都是**硬體 bus 層級**的事件，純軟體角度看就是「一切都死了」。

### 可能的 root cause

按可能性高低排：

1. **USB callback 在 sustained high-throughput 下踩到某個 driver bug**
   - 10 分鐘 × 35 fps × 100 KB ≈ 21 GB 流量過 USB
   - CherryUSB 在這種長期高壓下可能有累積 state issue
   - 看 cherryusb commits 找 fix，或加 USB stack reset 機制

2. **某個 DMA 拿到無效 address，造成 AXI abort**
   - 候選：GE2D / MPP / DE 任一
   - 如果 alloc 出來的 buffer 物理 address 落到非法區段（heap metadata 被踩過），HW 第一次用就 hang bus
   - 從 meminfo 看 system heap 還有 3.1 MB，不像 corruption，但**heap free list 內部結構**可能還是被踩了（rt_memheap_info 只看 used / max_used 不檢查內部完整性）

3. **back-pressure timeout 太短造成多 in-flight，CMA 短暫吃緊**（雖然 meminfo 顯示最終仍有 28.5 MB free，但**fragmentation** 可能讓 2.5 MB 連續配置失敗）
   - 已透過第 4 層拉長 timeout 到 500 ms 緩解
   - try_cma retry cap 確保不會 infinite loop

4. **電源 / ESD / 物理層因素**
   - 設備持續高負載 10+ 分鐘可能有熱漂移
   - USB cable / connector 接觸不良突然斷一下
   - 純軟體 fix 不到，但可以從 log 的「漸進惡化還是瞬斷」來判別

### 下次 freeze 時的 diag 步驟

1. **看最後一筆 `heap snapshot:` log**：sys avail 是不是在持續下滑？
2. **看最後一筆 `disp_flush:` 跟 `perf stats:`**：是 UI thread 先卡 (disp_flush 不再印) 還是 USB 先停 (PC drop) ？
3. **看是否有 `back-pressure WARN: stale arm > 500 ms` 出現**：表示 LVGL render miss，可能 obj 被 hide
4. **如果連 console 也死，事後跑 `meminfo` / `free` / `list_thread`** 查 thread state，看哪個被卡
5. **必要時降到 25 fps 推送，看會不會延後或避免 freeze** —— 若 PC fps 砍半 hang 就不發生，幾乎可確定是 high-throughput 引發的 timing 問題

---

## 6.6. GE2D / SW render thread semaphore-overflow race（~36 分鐘後 crash）

### 觀察事實

從 `Debug_Issue/20260527/20260602-001.txt` 的 log：
- PC 端：穩定 ~30~33 fps 推 JPEG，**跑了 2161 秒（≈ 36 分鐘）後突然冒出大量 warn**
- Device 端：
  ```
  [Warn] (2161.038, +2161038) lv_thread_sync_signal: Error: -3 lv_rtthread.c:160
  [Warn] (2161.087, +49)      lv_thread_sync_signal: Error: -3 lv_rtthread.c:160
  ... (持續每 ~33ms 一筆，連發數百次)
  [E] ge_start_and_wait()132 ge timeout
  CPU Exception: NO.6
    x4(tp): 00000000deadbeef
    x24~x27 (s8/s9/s10/s11): 全部 deadbeef
    mcause: 0x6  (Store Address Misaligned)
    mtval:  0x6f1f600077  (典型被踩過的非法 store target)
    mepc:   0x400568a4
  ```

### 一眼定位：時間點 + Error code

`Error: -3` 是 RT-Thread 的 `RT_EFULL`，從 `lv_rtthread.c:160` 噴的呼叫鏈是：
```
lv_thread_sync_signal()
 └─ rt_sem_release()
      └─ semaphore value 已達上限，return -RT_EFULL
```

RT-Thread `RT_SEM_VALUE_MAX = 65535`。而觀察到的 crash 時間是 **2161 秒、~30 fps**，乘起來：
```
2161 × 30 ≈ 64,830  ≈  65,535
```

**完全吻合 sem 從 0 一路被 release 累加到上限的 timing**。代表有個 path 持續 `signal` 卻**沒有對等的 `wait`** 在消費。

### Root cause：`lv_draw_ge2d.c::ge2d_render_thread_cb` 的 TOCTOU race

舊邏輯：
```c
while (1) {
    if (任務存在) {
        lv_thread_sync_wait(&u->sync);
        ... 處理任務 ...
    }
    // 若進來時剛好沒任務，直接迴圈又繞回去，但 main thread 可能已 signal 過
    // → signal 被吞、wait 沒被呼叫 → 下次再 signal 時 sem 又 +1
}
```

關鍵問題：**`signal` 跟 `wait` 不是嚴格 1:1**。在 high-FPS、長時間運作下，每秒只要漏 1~2 次 wait，36 分鐘累積到 65535 就直接溢位。溢位後：
- `rt_sem_release` 開始拒收，回 `-RT_EFULL`
- 但 main thread 仍以為 GE2D 接到工作了，繼續往下塞 cmdq
- GE2D 引擎本身的 fence 等不到對應 release，2 秒後 `ge_timeout` 觸發
- HW 在 timeout 時的 cleanup 路徑碰到非法 pointer（看 `deadbeef` 是 RT-Thread thread stack guard 填充值，代表那個 thread 的 stack 已經被踩穿），CPU 就吃到 `Store Address Misaligned` exception

`lv_draw_sw.c::render_thread_cb` 是用同一個 pattern 寫的，雖然 D215 預設走 GE2D path，**但 LVGL 內部仍會在某些 fallback 情境 enable SW 多 thread render**，所以同一個 race 也存在。

### 修正方式

讓 worker thread 進入點**永遠先做一次 `lv_thread_sync_wait`**，把 signal/wait 強制配對：

```c
// packages\artinchip\lvgl-ui\lvgl_v9\lv_drivers\lv_ge2d\lv_draw_ge2d.c
while (1) {
    lv_thread_sync_wait(&u->sync);   // ← 第一件事就是消耗 signal
    if (task) {
        ... 真正執行 ...
    }
}
```

同等修正套到 `packages\artinchip\lvgl-ui\lvgl_v9\lvgl\src\draw\sw\lv_draw_sw.c::render_thread_cb`。

額外把 GE2D worker thread stack 從 4 KB 拉到 8 KB，避免 cmdq + 異常處理路徑爆 stack（也是看到 `deadbeef` 蓋滿 s8~s11 才加的保險）。

### 改完後的判讀方式

1. **執行時間：超過 36 分鐘還沒看到 `Error: -3`** → 第一道防線通過
2. **`disp_flush: sync_wait=` 一直接近 0** → GE2D path 健康，沒有 backlog
3. **若 sem 機制本身又有狀況**：會在 **2~3 個 frame 內**（不是 36 分鐘後）就噴 `Error: -3`，因為 1:1 pairing 不對的話 sem 累加很快會被 stress test 抓到

### ⚠️ 驗證注意事項：firmware build 戳記 vs. 修正時間

`custom_Video.c` 開機 banner 印的 `built <date>` 是 firmware 真正的 build 時間，**這個欄位必須晚於 `lv_draw_ge2d.c` / `lv_draw_sw.c` 的修改時間**，修正才會真的被 link 進去。

實例：log `Jun 1 2026 16:10:30` build 的 firmware，跑出來在 2161 秒 crash → 對照檔案修改時間 `2026/6/2 09:12 / 09:27` 比 build 晚一天 → 結論是「跑了沒包修正的 image」。**這種情況不用 debug，直接 `scons -c && scons` 重 build 再 flash 即可重驗。**

### 一句話結論

> 36 分鐘 × 30 fps ≈ 65535 = `RT_SEM_VALUE_MAX`：看到這個時間×FPS 組合，**100% 是 semaphore overflow**，先去找哪條 path 在 `signal` 卻沒對應 `wait`。

---

## 6.7. Display thread `sync_notify` semaphore-overflow race（§6.6 的第二顆地雷）

### 為什麼修了 §6.6 還是看到 `Error: -3`

從 `Debug_Issue/20260602/20260602-002.txt` 的後續驗證 log（GE2D / SW 修正都已生效的 firmware build）：

- 一樣的 `lv_thread_sync_signal: Error: -3 lv_rtthread.c:160` warn，**但這次是在 3613 秒（≈ 60 分鐘）才大量出現，不是 36 分鐘**
- 出現後系統**沒有 crash**，只是 FPS 從 30 掉到 15~16，伴隨 `disp_flush: sync_wait=0us` 與 `render=3ms`（render path 健康，瓶頸在 sem 噴 log）
- PC 端持續推 30 fps，device `recv=30, drop=0`，沒有 back-pressure 介入

關鍵差異：
- **時間延長了**（36 → 60 分鐘）：代表 §6.6 修掉了「其中一個」漏 sem 的 path，但**第二個漏點仍存在**
- **不再 crash**：因為 §6.6 同步把 GE2D worker stack 從 4 KB 加到 8 KB，sem 溢位後不會再踩破 stack；但 FPS 仍然腰斬，因為每筆 signal 失敗都 `LV_LOG_WARN` → 噴 kprintf → console 阻塞 → render loop 等 IO

### Root cause 2：`lv_port_disp.c` 的 display flush thread 重複同樣 race

`aic_display_thread` + `wait_sync_ready` 跟舊版 GE2D worker **是同一個 TOCTOU 結構**，分屬兩條 sem：

```
producer (main thread, disp_draw_buf_flush)        consumer (aic_display_thread)
─────────────────────────────────────────          ────────────────────────────
flush_act = true                                   while (flush_act == false)
signal(&aic_disp->sync)                                wait(&aic_disp->sync)
                                                   flush_act = false
wait_sync_ready: while (sync_ready==false)         WAIT_FOR_VSYNC
    wait(&aic_disp->sync_notify)                   sync_ready = true
sync_ready = false                                 signal(&aic_disp->sync_notify)
```

兩個 race 點：
1. **`aic_display_thread`**：main 設 `flush_act=true` + signal 之後，display thread 進外圈 `while(1)` 看到 `flush_act==true`，**跳過 wait**，但 sem 已 +1 → sync 漏一格
2. **`wait_sync_ready`**：display 設 `sync_ready=true` + signal sync_notify 之後，main 下一幀進來看到 `sync_ready==true`，**跳過 wait**，sem_notify 已 +1 → sync_notify 漏一格

兩條都漏。在 §6.6 還沒修之前，GE2D worker 的 sync 也漏，所以總共三條漏。修完 §6.6 後剩 display 這兩條。

時間驗算（修完 §6.6 後）：
- 假設 30 fps × 每幀漏 1 個 sync_notify ≈ 30 leak/s
- `65535 / 30 ≈ 2184s ≈ 36 min` —— **但** 實際是 60 min 才到
- 多出來的時間是因為「sync 也在漏，但漏得比 sync_notify 慢」或「並非每幀都中 race」，所以實際 leak rate 是 ~18 leak/s（`65535 / 60min ≈ 18 leak/s`）

關鍵指標：**sem 一旦觸頂，每筆 signal 都會印一行 warn**，這個 kprintf 風暴本身就會讓 FPS 腰斬。所以「FPS 從 30 掉到 15 + 連續 Error: -3」≡「sem 已經死在 max」。

### 修正方式

完全比照 §6.6 的 1:1 pairing，但兩條 sem 都要修。額外要在 init 時 **prime 一次 `sync_notify`**，否則第一幀 `wait_sync_ready` 會 deadlock：

```c
// packages\artinchip\lvgl-ui\lvgl_v9\lv_drivers\lv_port_disp.c

// 1) wait_sync_ready 改成「永遠 wait 一次」
static inline void wait_sync_ready(aic_disp_t *aic_disp)
{
    if (aic_disp->exit_status)
        return;
    lv_thread_sync_wait(&aic_disp->sync_notify);   // ← 不再用 sync_ready 旁路
}

// 2) aic_display_thread 比照 GE2D worker 的 1:1 模式
while (1) {
    lv_thread_sync_wait(&aic_disp->sync);          // ← 第一件事先消耗 signal
    if (aic_disp->exit_status) break;
    if (aic_disp->flush_act) {
        aic_disp->flush_act = false;
        mpp_fb_ioctl(aic_disp->fb, AICFB_WAIT_FOR_VSYNC, 0);
        aic_disp->sync_ready = true;
        lv_thread_sync_signal(&aic_disp->sync_notify);
    }
}

// 3) lv_port_disp_init() 在啟動 display thread 前先 prime
aic_disp->sync_ready = true;
lv_thread_sync_init(&aic_disp->sync);
lv_thread_sync_init(&aic_disp->sync_notify);
lv_thread_sync_signal(&aic_disp->sync_notify);     // ← 餵第一次給 wait_sync_ready
lv_thread_init(&aic_disp->thread, LV_THREAD_PRIO_HIGH, aic_display_thread, 4 * 1024, aic_disp);
```

`sync_ready` 與 `flush_act` 仍保留作為觀測旗標，但**已不負責同步**，所有同步邏輯都靠 sem 1:1 pairing。

### 改完後的判讀方式

1. **執行時間：超過 60 分鐘還沒看到 `Error: -3`** → §6.7 修正生效
2. **FPS 在 30 fps 上下穩定，不會半小時內逐步往 15 靠** → sem 沒漏
3. **若 deadlock 在開機第一幀**（畫面全黑、UI 沒進）→ priming signal 漏掉了，檢查 init 序列
4. **若 PC `cap fps` 與 device `intv` 開始發散**：先看是不是新 race 加進來，不要先懷疑 GE2D

### 驗證 firmware 戳記（同 §6.6）

`custom_Video.c` 開機 banner 印的 `built <date>` 必須晚於 `lv_port_disp.c`、`lv_draw_ge2d.c`、`lv_draw_sw.c` **三個檔的修改時間**。任何一個比 build 晚，都代表跑到舊 image，重新 `scons -c && scons` 再 flash。

### 一句話結論

> 修完 GE2D worker race 後若仍看到 `Error: -3`，且時間 × fps 又湊出來 ≈ 65535，那就是 display thread `sync_notify` 漏 sem。SDK 裡所有「`while (state==X) wait(sem)`」結構**都要全部清掃成 1:1**，不能只修最先 crash 的那一條。

---

## 6.8. §6.6+§6.7 修完後的新觀察：`ge timeout` + CPU Exception NO.1（與 sem leak 無關）

### 觀察事實（2026-06-02 13:31 → 13:40 的 9 分鐘 log）

PC 端整段 ~32 fps、`drop=0`，**全程沒有任何 `lv_thread_sync_signal: Error: -3` warn**，§6.6/§6.7 修正完全生效。

但是裝置在 **第 ~9 分鐘**（PC 時間戳 13:40:15）突然停止接收，幾秒後 device 噴：

```
[E] ge_start_and_wait()132 ge timeout
[I] dump_regs()108 CPU Exception: NO.1
x1(ra)   : 0x40042712
x2(sp)   : 0x40155b70
x4(tp)   : 0xdeadbeef         ← thread pointer 損壞
x8/fp    : 0x40a33da0         ← 仍是合法 heap 位址
x9-x23   : 都是合法值或 0
x15(a5)  : 0xdeadbeef
x24-x27 (s8/s9/s10/s11): 全部 0xdeadbeef
mcause   : 0x1  (Instruction Address Misaligned)
mtval    : 0xdeadbeee
mepc     : 0xdeadbeee
```

device 端的 30-frame 統計也很乾淨：

```
custom_area4: perf stats: 30 in ~1000ms fps≈30 (intv≈33)
                          avg pickup=12~17 show=0 intv=32~34 render=3 recv=30~33 drop=1~3
disp_flush: sync_wait=0us blit=~6850us (n=30)
custom_area4: heap snapshot: sys total=14638200 used=11502008 avail=3136192  ← 全程穩定
```

### 跟前兩次 crash 的差異

| 指標 | §6.6 前（原始） | §6.6 後（§6.7 前） | §6.6 + §6.7 後（本次） |
|---|---|---|---|
| `Error: -3` warn | ✅ 36 分鐘後爆量 | ✅ 60 分鐘後爆量 | **❌ 完全沒有** |
| crash 觸發時間 | ~36 分鐘 | ~60 分鐘 | **~9 分鐘** |
| crash 之前的 fps | 30 → 跌至 15 | 30 → 跌至 15 | **30 全程穩定** |
| pickup latency | 0~1 ms | 0~1 ms | **12~17 ms** |
| back-pressure drop | 多 | 多 | 1~3 / 秒（小量） |
| mcause | 0x6 Store Misalign | 0x6 Store Misalign | **0x1 Instr Misalign** |
| 暫存器 deadbeef 範圍 | `tp` + `s8-s11` | `tp` + `s8-s11` | `tp` + `s8-s11` + `a5` |

關鍵差異：
1. **fps 穩定了**：以前是 sem 觸頂後 console kprintf 風暴拖累；現在沒有 sem 觸頂，自然穩定。
2. **pickup latency 從 0 ms 變成 13 ms**：這是**正確的修正副作用**。修正前 main 會跳過 `wait_sync_ready` 直接前進（race），所以看起來 latency=0；修正後 main 真的會等 VSYNC 完成，latency 自然反映 VSYNC 週期 (~16 ms)。
3. **back-pressure 開始正常運作**：因為 main 真的被 VSYNC throttle 住了，PC 推快過 VSYNC 時就 drop，符合預期。
4. **mcause 從 0x6 變 0x1**：兩者都是 deadbeef 寫進 register 但表現方式不同 —— 0x6 是 store 時把 deadbeef 拿來算位址；0x1 是 `ret` pop 出來的 `ra` 是 deadbeef，跳到 0xdeadbeee 取指令時對 2 byte 對齊失敗。本質都是 stack save 區被填成 deadbeef pattern。

### 還沒 100% 確定的根因（推測）

deadbeef 是 RT-Thread 在 `rt_thread_create` 時填到「未使用的 stack 空間」當作 high-water-mark 用的初始 pattern。`tp` 與 `s8-s11` 通通變 deadbeef 表示：context switch 時這些 callee-saved register 從 stack 載回來時，**載回的位置還沒被任何真實 push 操作寫過**，所以拿到的是 init pattern。

最可能的觸發路徑（兩個都要看）：

1. **D215 V10 GE2D 在連續 ARGB rotate 9 分鐘後 hw 卡死** → `aicos_event_recv` timeout (2 秒) → `dump_regs` → `ge_reset(data)` → 回 caller。caller 是 `lv_draw_ge2d_rotate`（在 main LVGL thread）。GE2D timeout cleanup 路徑會吃比較深的 stack（dump_regs 印 256 行 register；ge_reset 觸發 hal_reset_assert / deassert），如果 main thread stack 剛好接近底部，就會踩到 stack 守護區。
2. **`mpp_ge_sync` 失敗後 mpp_ge 內部狀態不一致** → 下一張 frame 的 `mpp_ge_bitblt / emit / sync` 拿到野指標 → 在 mpp_ge 層往內踩。

### 已套用的防禦性修正

`packages\artinchip\lvgl-ui\lvgl_v9\lv_drivers\lv_port_disp.c::lv_port_disp_init`：

```c
/* Stack bumped from 4KB to 8KB to match the GE2D render thread. */
lv_thread_init(&aic_disp->thread, LV_THREAD_PRIO_HIGH, aic_display_thread, 8 * 1024, aic_disp);
```

把 `aic_display_thread` 的 stack 從 4 KB 加到 8 KB，跟 GE2D worker 對齊。雖然 display thread 本身工作很輕（只有 wait/VSYNC/signal），但 GE2D timeout cleanup 可能會經由 callback 路徑回到這條 thread，先給足 stack 把這個 vector 排除掉。

### 還沒做的（下一步該查的方向）

1. **檢查 LVGL main thread stack 守護**：目前 `LPKG_LVGL_THREAD_STACK_SIZE=32768`（32 KB），但 GE2D timeout 後 dump_regs 印 256 行 + ge_reset，若再加上 LVGL 自己 unwind 出去的多層 frame，32 KB **可能不夠**。可以先把 `CONFIG_LPKG_LVGL_THREAD_STACK_SIZE=49152` (48 KB) 試試。
2. **D215 V10 GE2D 硬體 reset 路徑**：`hal_reset_assert(RESET_GE) + hal_reset_deassert(RESET_GE)` 之後 GE2D 寄存器處於什麼狀態？mpp_ge 上層快取（cmdq pending）是否同步清掉？若沒同步，下一次 emit 會用到舊 fence。
3. **`mpp_ge_sync` 失敗後是否要 `mpp_ge_close + mpp_ge_open` 重建 device handle**：目前 `lv_draw_ge2d_rotate` 失敗只是 `return`，handle 保留。可在 fail path 加 reopen，徹底重置軟體狀態。
4. **如果 GE2D hw 真的有 9 分鐘穩定性極限**：考慮把 disp_do_blit 改成 SW rotate（`lv_draw_sw_rotate`），雖然 ARGB 1280×800 rotate 用 SW 大約 30+ ms，但**至少不會 crash**。
5. **加 RT-Thread stack guard print**：`CONFIG_AIC_USING_THREAD_STACK_TRACE=y` 已開，平常可用 `list_thread` 看每條 thread 的 max stack 用量，crash 前期就會看出哪條快爆。

### 一句話結論

> §6.6 + §6.7 把兩條 sem race 都修掉之後，**剩下的 ~9 分鐘 crash 是另一個獨立 bug**，看 register dump 是 stack 損壞（deadbeef pattern 灌進 callee-saved），最可能來自 D215 V10 GE2D timeout 後的 cleanup 路徑吃光 stack，或 mpp_ge 失敗復原沒做乾淨。先把 display thread 加成 8 KB 觀察，下一步要查 LVGL main thread stack 與 GE2D reopen。

---

## 6.9. PC 端推進來的影像資料**不會在記憶體裡累積**

常見疑問：「PC 一直推 ~33 fps 進來、device 只能顯示 ~30 fps，那中間差出來的 frame 會不會堆在記憶體裡？」

**答案是不會。** 整條 pipeline 在架構上就**禁止累積**，每一層的「buffer 深度 = 1」，新的 frame 直接覆寫舊的，舊的還沒被讀就丟掉。

### 三層 fixed-size buffer（boot 時一次配，runtime 完全不 alloc/free）

| Buffer | 大小 | 位置 | 角色 | 持有方 |
|---|---|---|---|---|
| `s_recv_buf`      | 2 MB | CMA | USB 正在寫入的 slot                | `custom_Image.c` |
| `s_ready_buf`     | 2 MB | CMA | show_image 拿來讀的 slot           | `custom_Image.c` |
| `s_area4_disp_buf`| 2 MB | CMA | LVGL widget 路徑的 source（streaming 跳過） | `custom_Area4.c` |
| FB                | ~8 MB | CMA | 顯示用 framebuffer                 | display 子系統 |

**合計 ~14 MB CMA**，所有 buffer **永久持有**，`runtime` 完全不做 alloc / realloc / free。

### 機制 1：USB 端「swap 而不 queue」

`custom_Image.c::custom_image_finish_recv`：

```226:248:packages\artinchip\lvgl-ui\aic_demo\ui_builder\custom\custom_Image.c
         * Avoid one more full-size memcpy/realloc at transfer end.
         */
        uint8_t *tmp_buf = s_ready_buf;
        ...
        s_ready_buf = s_recv_buf;
        ...
        s_recv_buf = tmp_buf;
        ...
        s_last_recv_done_ms = aic_get_time_ms();
        s_recv_done_count++;
```

一張收完只交換**兩個指標**（不是 enqueue，不是 memcpy）。下一張進來時 `s_recv_buf` 又從 offset 0 開始寫，**直接覆蓋掉前一張尚未被 show 的內容**。USB 端因此最多 hold「上一張 ready + 正在收的這張」共 2 張，跟 PC 推多快無關。

### 機制 2：LVGL 端 back-pressure（in-flight render ≤ 1）

`custom_Area4.c::custom_area4_show_image` 入口：

```494:520:packages\artinchip\lvgl-ui\aic_demo\ui_builder\custom\custom_Area4.c
     */
    if (s_perf_meas_pending)
    {
        uint64_t arm_ms = s_perf_show_entry_ms;
        ...
        if (t_show_entry_ms >= arm_ms &&
            (t_show_entry_ms - arm_ms) < 500U)
        {
            s_backpressure_drop_count++;
            ...
            return 0U;
        }
```

上一張還在 render（`s_perf_meas_pending == 1`）→ 本張**直接 return 0**。LVGL 的 render 隊列因此永遠最多 1 張，跟 USB 收進度無關。

### 機制 3：LVGL image cache 已用 `LV_IMAGE_FLAGS_USER4` bypass

歷史上曾經因為 cache 累積 stale entry + refcount race 在 30 fps × 3 min 後 crash（見 §6 的 LVGL cache 章節）。現在 Area4 走的是 cache-bypass 路徑：

```733:758:packages\artinchip\lvgl-ui\aic_demo\ui_builder\custom\custom_Area4.c
     * Flags 設計 (Area3 vs. Area4，三選一)：
     *   USER5 — 強制 SW blend + bypass cache。Area3 用，...
     *   USER4 — bypass cache only，HW blit 保留。Area4 streaming 用。
     ...
     *   - bypass cache 後，MPP decoder 每張 open/close 直接 mpp_dec_data_release
     *     釋放 frame，**完全沒有 rb-tree / refcount race window**。
```

→ LVGL cache 裡也不會有累積。

### 證據：30+ 次 heap snapshot 一字不差

```
custom_area4: heap snapshot: sys total=14638200 used=11506112 max=11517956 avail=3132088
custom_area4: heap snapshot: sys total=14638200 used=11506112 max=11517956 avail=3132088
custom_area4: heap snapshot: sys total=14638200 used=11506112 max=11517956 avail=3132088
... (連續 30+ 次完全相同) ...
```

| 欄位 | 值 | 含義 |
|---|---|---|
| `used`      | 11,506,112 | 目前 system heap 已用 ≈ **11.0 MB** |
| `max`       | 11,517,956 | 歷史最高使用 ≈ **11.0 MB**（與 used 差 11.8 KB） |
| `avail`     | 3,132,088  | 還有 **2.99 MB** system heap 可用 |
| `total`     | 14,638,200 | system heap 總容量 14.0 MB |

跑了 10+ 分鐘 PC log + 數分鐘 device-only log，**system heap 一個 byte 都沒長**。CMA pool 也是固定的，因為三個 buffer 都是 boot 時 `aicos_malloc_align(MEM_CMA, ...)` 一次配完。

### 那 perf stats 顯示的 `drop=4~8 (11~21%)` 是什麼？

**這正是「無累積」的代價，是 graceful degradation，不是 bug**：

- PC 推 ~33 fps（log: `cap/enc/snt=35/35/35`）
- Device 消化 ~30 fps（VSYNC 限制 + GE2D blit ~6.8 ms）
- 多出的 ~3-5 fps 那部份的影像進來時，`s_recv_buf` 還沒被 show，就被新一張**直接蓋掉**
- 這就是 `recv=36 drop=6(16%)` 的意思：收到 36 張，其中 6 張 USB 收完還沒被顯示就被覆寫

替代方案是 enqueue，但那樣 latency 會跟 PC/device 速度差成正比累積，memory 也會無限大。**目前的「永遠只看最新」策略才符合 streaming 語意**（看當下影像，不要看 10 秒前的）。

### 一句話結論

> 整條 pipeline 是「**深度恰好 = 1 的 single-slot pipeline**」：USB 端 swap、LVGL 端 back-pressure、cache 端 bypass。三個固定 CMA buffer 共 6 MB，加上 8 MB FB 永遠不變。perf stats 裡的 `drop=` 不是 leak，是「丟掉舊的、只顯示最新」的明示。

---

## 6.10. D215 GE2D 規格 vs. 實機負載（**並未超出規格**）

連續長時間運作偶發 `ge timeout` 之後第一個合理懷疑：「我們是不是把 GE2D 操過 spec 了？」答案是**沒有，三個維度都還在規格內**。

### D215 GE2D 規格摘要（datasheet）

| 條目 | 規格 |
|---|---|
| 2D 圖形加速 | **最大性能 1080P @ 60 fps** ← headline 數字 |
| 水平/垂直翻轉、90/180/270 旋轉 | 支援（**未獨立標 fps**） |
| RGB 任意角度旋轉、不同掃描順序 | 支援 |
| 1/16x ~ 16x 縮放 | 支援，6×4 taps 16 phases filter |

> ⚠️ **重要的寫法細節**：headline `1080P @ 60 fps` 是 2D 加速最佳情況；旋轉、縮放各自列為 bullet 但**沒給 fps 數字**。一般 SoC GE 的旋轉路徑因 cache miss 模式不同，吞吐約為直 blit 的 50~70%；廠商通常**不會把旋轉算進 headline**。

### 我們實機的工作量

| 項目 | 值 |
|---|---|
| 解析度       | 1280 × 800 = **1.024 Mpix / frame** |
| Pixel format | **ARGB8888**（4 B/pixel） |
| 操作         | **90° rotation**（`MPP_ROTATION_270`，因為面板實體裝向旋轉了 90°） |
| 目標 fps     | ~25 fps（設計）/ 實測 ~30 fps |
| 實測 blit    | **~6.8 ms / frame**（perf stats: `blit=6800~6900us`） |

### 維度 1：像素吞吐率（pixels per second）

| 來源 | 計算 | pixel/sec | 比例 |
|---|---|---|---|
| Spec headline（1080P @ 60 fps，無旋轉） | 1920×1080×60 | **124.4 Mpix/s** | 100% |
| 實機（1280×800 @ 30 fps，含旋轉） | 1280×800×30 | **30.7 Mpix/s** | **24.7%** |

→ **我們只用了 spec 的 1/4 像素吞吐**。

### 維度 2：每 frame latency budget（time per frame）

| 來源 | 預算 |
|---|---|
| Spec 1080P 每張預算 | 1000 / 60 = **16.67 ms / frame** |
| 我們的 frame 面積（按 area 比例縮） | 16.67 × (1.024 / 2.074) = **8.22 ms / frame** |
| **實測 blit time** | **6.8 ms** |
| 預算使用率 | **83%** |

→ 這條是最緊的維度，**吃掉 83% 但仍未超**。意思是 GE2D silicon 在等比例工作量下實際達到 datasheet 性能，**沒有偷工但也沒餘量**。

### 維度 3：DDR 記憶體頻寬

| 來源 | 計算 | 頻寬 |
|---|---|---|
| 實機（讀 + 寫） | 1.024 Mpix × 4 B × 30 fps × 2 | **246 MB/s** |
| 假設 spec 用 ARGB（讀 + 寫） | 2.074 Mpix × 4 B × 60 fps × 2 | 995 MB/s |
| 比例 | | **24.7%** |

D215 LPDDR 典型理論頻寬 ~1.6 GB/s，**我們用 246 MB/s ≈ 全頻寬 15%**。加上 USB、display scan-out、CPU 共享 DDR，總和約 600~800 MB/s 仍在合理工作區。

### 結論：沒有超過規格，但 spec 沒涵蓋的東西才是 crash 來源

三個維度都在規格內：

- 像素吞吐：**用了 25%**
- 每 frame latency budget：**用了 83%**（最緊維度）
- DDR 頻寬：**用了 25%**

那為什麼還會 `ge timeout`？因為 **spec 沒給的維度才是真正的 crash 來源**：

1. **連續工作時長**：spec 寫的是 peak performance，**沒寫可持續工作多久不出錯**。D215 V10 silicon 似乎連續工作 ~15 分鐘以上後 GE engine 會偶發進入「等不到 interrupt」狀態（log 裡 `perf stats: 30 in 2127ms fps=14.10` 那筆就是先兆）。這是 silicon bug / errata 範疇，跟跑得有多滿無關。
2. **旋轉路徑特殊性**：spec 沒給旋轉的 fps 數字，可能旋轉時 internal FSM 有某條 corner case 比直 blit 容易卡。
3. **多 IP 同時觸發 DDR**：USB DMA、display DE 掃描、GE2D blit 三者在同一條 AXI bus 上競爭，spec 給的是孤立 GE2D 數字，沒涵蓋共存場景。

### 兩個降載備案（如果想把 GE2D 用得更鬆 → 但根本問題仍未解）

| 選項 | 改動 | 效果 | 副作用 |
|---|---|---|---|
| **A. fps 降到 25** | PC 端 cap 25 fps，或 device back-pressure timeout 改 40ms | GE2D 從 25% → 20%，對齊原本「25 fps 目標」 | 無 |
| **B. 改用 RGB565 出 FB** | `disp_do_blit` 路徑改 RGB565 來源 | GE2D + DDR 頻寬砍半（25% → 12%） | 視覺色階損失 |

注意：降載**只能把 crash 從 15 分鐘推到 30 分鐘**，無法根除。比較根本的方向見 §6.8 最後的下一步建議（GE2D timeout-recovery、改 SW rotate）。

### 一句話結論

> 我們的工作量「**像素吞吐 25%、frame budget 83%、DDR 頻寬 25%**」，三個維度都在 spec 內。剩下的 `ge timeout` 不是「超載」造成的，是 spec **沒涵蓋的「連續長時間 + 多 IP 共用 DDR + 旋轉路徑 corner case」維度**所致，屬於 silicon errata 等級的問題。

---

## 6.11. 三層 GE2D 穩定性防護（≥30 fps + 不崩潰兩個 must 條件的最終方案）

### 為什麼是「三層」而不是「修好就好」

§6.10 已經確認：D215 V10 GE2D 的 `ge timeout` 不是 SW bug、不是我們吞吐超載、也不是 memory leak，而是 silicon 等級的 corner case（連續長時間運轉 + 多 IP 共用 DDR + 旋轉路徑），用 SW 沒辦法「修好」。所以策略改成「**讓系統能優雅吸收 ge timeout**」：

| 層 | 目的 | 做了什麼 |
|---|---|---|
| L1 — **HAL 自動 reset** | 已存在，不動 | `hal_ge_normal.c::ge_start_and_wait` 超時 → `dump_regs` → `ge_reset()` → 回 error |
| L2 — **Driver graceful return** | 不 crash | `lv_draw_ge2d_rotate` / `custom_stream_run_blt` 收到 error 直接 return，FB 留前一張 |
| L3 — **Telemetry + 強化警告** | 看得見 + 可診斷 | 新加 fail counter，連續失敗 5 次印「HW 卡住」警告 |
| L4 — **Stack guard 拉大** | 防 cleanup 路徑爆 stack | `LPKG_LVGL_THREAD_STACK_SIZE` 32 KB → 48 KB（§6.8 留的 TODO） |

四層各自獨立，**任何一層失靈，下一層接住**。

### L3 細節：fail counter 在哪裡？

**Widget path**（`lv_draw_ge2d.c::lv_draw_ge2d_rotate`）：

```c
static volatile uint32_t s_rotate_fail_total;
static volatile uint32_t s_rotate_fail_streak;

static void ge2d_rotate_record_fail(const char *stage)
{
    s_rotate_fail_total++;
    s_rotate_fail_streak++;
    LV_LOG_ERROR("ge2d rotate %s fail (total=%u streak=%u) — last good frame kept on screen",
                 stage, s_rotate_fail_total, s_rotate_fail_streak);
    if (s_rotate_fail_streak == GE_FAIL_STREAK_ALERT)  /* 5 */
        LV_LOG_ERROR("!!! GE2D rotate failed 5x in a row — HW likely stuck. "
                     "System keeps running; restart device to recover full fps. !!!");
}
```

**Streaming path**（`custom_disp_stream.c::custom_stream_run_blt`）：相同模式，獨立 counter，獨立 streak，前綴 `[stream]`。兩條 path 共享同一個 `ge2d_dev`，但失敗計數獨立記，可以分辨「是 widget path 卡了還是 streaming path 卡了」。

成功時 streak 自動 reset 並印 `ge2d rotate recovered after N failure(s)`，所以 log 上能直接看出「freeze 多久 → 自己恢復」。

### 為什麼不在 fail path 做 `mpp_ge_close + mpp_ge_open` 重建 handle

考慮過，最後**沒做**。原因：

1. `ge2d_dev` 是個 global pointer，**兩個 thread 共用**：
   - LVGL main thread（透過 `lv_draw_ge2d_rotate` / `custom_stream_run_blt`）
   - LVGL GE2D worker thread（`ge2d_render_thread_cb` → `lv_draw_ge2d_img/blit/...`）
2. HAL 內部的 mutex 只保護「**一次 mpp_ge_*() 呼叫**」的 atomicity，**不保護 pointer swap**。
3. 在 main thread 做 `close → 賦值給 NULL → reopen → 賦值給新 handle` 的過程，worker thread 隨時可能進入，讀到 NULL → segfault。
4. HAL 內已經 `ge_reset()` 把硬體狀態清乾淨，SW handle（cmdq、fence 列表）保留不動其實是更安全的 default。

如果未來真的需要 reopen，必須先在 ge2d_dev 外面加一條 rwlock 或 quiesce 機制，目前不值得做。

### L4 細節：為什麼還要再 bump main thread stack 到 48 KB

| Thread | 之前 | 現在 | 為什麼 |
|---|---|---|---|
| `aic_display_thread` | 4 KB | 8 KB | §6.8 — 9 分鐘 deadbeef crash 修法 |
| `ge2d_render_thread_cb` | 默認 | 8 KB | §6.6 — 36 分鐘 sem leak 修法附帶 |
| **LVGL main thread**（`LPKG_LVGL_THREAD_STACK_SIZE`） | **32 KB** | **48 KB** | **§6.11 — 最後一道防線** |
| LVGL SW render thread (`lv_draw_sw.c`) | 跟 LVGL main thread 共用 macro | 同上 → 48 KB | 順帶受益 |

LVGL main thread 在 `disp_flush → disp_do_blit → lv_draw_ge2d_rotate → mpp_ge_sync` 的呼叫鏈裡，當 ge timeout fire 時，HAL 還會 `dump_regs()` 印 256 行 + ge_reset，這段路徑經過很多層 frame；32 KB 容易吃緊。48 KB 留 50% 餘量。代價只有額外 16 KB stack，從 system heap 拿，~3 MB 餘量，可以忽略。

### 驗收方法（怎麼確認這次三層防護有效）

正常運作時 log 應該像這樣（**沒有任何 ge2d rotate 相關訊息**）：

```
perf stats: 30 in 967ms fps=31.02 (intv=32.23)
disp_flush: sync_wait=0us blit=6841us (n=30)
[heap snapshot] sys total=3145728 used=1234567 max=1456789
```

如果 ge timeout 偶發（**允許**，這是 silicon errata 等級問題，已被三層接住）：

```
[E] ge_start_and_wait()132 ge timeout
[I] dump_regs()108 ...
[E] ge2d rotate sync fail (total=1 streak=1) — last good frame kept on screen
[E] ge2d rotate recovered after 1 failure(s) (total=1)    ← 下一張就回來
perf stats: 30 in 2150ms fps=13.95 (intv=71.67)            ← 唯一一張變慢
```

關鍵指標：**`recovered after N failure(s)` 一定要出現**，代表系統靠自己回來了。如果連續 5 次失敗：

```
[E] ge2d rotate sync fail (total=5 streak=5) ...
[E] !!! GE2D rotate failed 5 times in a row — HW likely stuck. ...
```

→ 這時候 fps 會持續是 0（最後好的一張在螢幕上）。系統**不會 crash**，但需要重啟才能恢復 fps。這是 silicon errata 命中的兜底行為，不是 bug。

### ≥30 fps 怎麼保證？

實測 baseline（§6.10 + §6.7 修完後）：

- 平均：~30-32 fps，多數 window 落在 31-33
- 偶爾的低點：28-29 fps（單 window 30 frame，多半因為 PC 端推送 jitter）
- ge timeout 觸發時：**那一張**變慢成 2 秒，**之後立即回到 30+ fps**

對「最少 30 fps」這條，可以做以下取捨：

1. **目前的 baseline 已滿足**（平均 30+，偶有 28-29 一窗 → 1 秒級事件不影響使用）
2. **若要更剛性 ≥30 fps**：等 §6.11 三層防護驗證穩定後，可以考慮再做 §6.10 的「Phase 1.A — skip AICFB_WAIT_FOR_VSYNC」（fps 推到 45+，代價是極微 tearing 風險）。**先驗穩定再決定**這條要不要做。

### 一句話結論

> §6.11 = §6.10 的 silicon errata 結論落地。SW 沒辦法修 silicon，那就讓 silicon 偶爾打嗝時系統 (1) 不 crash、(2) 看得見、(3) 自己恢復、(4) 連續打嗝時優雅停住等用戶重啟。四層加起來，「≥30 fps」+「穩定不崩潰」兩個 must 條件**才真的能寫進交付 spec**。

---

## 6.12. Silent lockup 防護：display 凍結但 log 完全沒異常的場景

### 觀察到的症狀（user report 2026-06-04）

> 「有一種異常情況，長時間收圖資料後顯示，看起來 device 還未當機，圖檔已經不會正常顯示，PC 端還在傳圖檔資料，device 的 log 已經停止不動，但是最後幾筆未有任何異常訊息出現。」

特徵：

- **device 沒 crash**（沒有 CPU Exception trace）
- **log 完全停**（連既有的 perf stats、heap snapshot、`disp_flush:` blit log 都沒有）
- **PC 還在推**（USB host 沒被踢開，可繼續送 packet）
- **最後幾筆 log 沒任何 warning / error**（不是 ge timeout 之後才停，是真的「最後一筆乾乾淨淨」）

這是比 §6.10 的 `ge timeout` **更難對付的失效模式**：silicon 沒報任何錯，但整條 display pipeline 鎖死。

### 根因分析：兩個無限等的死鎖

`lv_port_disp.c` 之前有兩個 `RT_WAITING_FOREVER` 的等：

| 點 | 位置 | 等什麼 | 卡住的後果 |
|---|---|---|---|
| (A) `wait_sync_ready` | LVGL main thread | `lv_thread_sync_wait(sync_notify)` | main thread 永遠卡住 → **不再印任何 log** |
| (B) `aic_display_thread` | display thread | `mpp_fb_ioctl(AICFB_WAIT_FOR_VSYNC, 0)` | display thread 永遠卡住 → 永不 signal `sync_notify` → 導致 (A) |

死鎖鏈：

```
DE 硬體某個狀態機 glitch (VSYNC IRQ 停了)
      ↓
display thread 卡在 AICFB_WAIT_FOR_VSYNC（點 B）
      ↓
display thread 不會走到 signal(sync_notify)
      ↓
main thread 下一張 wait_sync_ready 永久 block（點 A）
      ↓
main thread 不會跑 lv_timer_handler / perf stats / heap snapshot
      ↓
log 完全 silent，USB callback thread 還活著繼續收 → recv_count 上升但畫面凍結
      ↓
這就是 user 看到的症狀
```

`ge timeout` 因為 HAL 內有 `GE_TIMEOUT = 2 sec` bound，**已被 §6.11 的 graceful return 接住**。但 VSYNC wait **沒有任何 timeout**，所以 §6.11 救不了這條死鎖。

### 三層止血策略（this section）

| 層 | 改在哪 | 解決什麼 | 副作用 |
|---|---|---|---|
| L1 — **bounded `wait_sync_ready`** | `lv_port_disp.c::wait_sync_ready` 改成 200 ms timeout | 點 (A) 不再無限等 → main thread 永遠保持回應 | 真的 stuck 時，會 skip 那張 frame；面板維持上一張 |
| L2 — **timeout 時跳過 blit/PAN/signal** | `lv_port_disp.c::disp_draw_buf_flush` | 避免 main 對著卡住的 display 一直 signal sync 導致 sem leak（RT_SEM_VALUE_MAX 65535 overflow） | 無；這是正確行為 |
| L3 — **freeze_watchdog_thread** | `lv_port_disp.c::freeze_watchdog_thread`（新加） | log 永遠不會 silent；自動偵測 stall 並印強警告 | 一個 4 KB stack 的 LOW-priority thread |

### L1 細節：`wait_sync_ready` 從 forever 改成 200 ms

```c
#define WAIT_SYNC_READY_TIMEOUT_MS 200U

static inline bool wait_sync_ready(aic_disp_t *aic_disp)
{
    if (aic_disp->exit_status) return false;

    rt_err_t ret = rt_sem_take(aic_disp->sync_notify.sem,
                               rt_tick_from_millisecond(WAIT_SYNC_READY_TIMEOUT_MS));
    if (ret == RT_EOK) {
        /* recovered */
        if (s_sync_timeout_streak) {
            rt_kprintf("[disp] sync_notify recovered after %u timeout(s)...\n", ...);
            s_sync_timeout_streak = 0;
        }
        return true;
    }
    /* timed out — display thread stuck */
    s_sync_timeout_count++;
    s_sync_timeout_streak++;
    rt_kprintf("[disp] !!! sync_notify wait timed out after 200ms ...\n");
    return false;
}
```

選 200 ms 的原因：
- 60 Hz 面板每張 frame 16.67 ms，200 ms = 12 個 VSYNC 週期
- 任何「正常 slow path」（USB DMA stall、CPU spike、heap evict）都不會等到 200 ms
- 200 ms 內沒回應 = HW 真的卡住了，不用再等

### L2 細節：timeout 時整張跳過

caller 改寫：

```c
if (!wait_sync_ready(aic_disp)) {
    /* skip blit / PAN / signal(sync) */
    return;
}
...normal frame...
lv_thread_sync_signal(&aic_disp->sync);
```

為什麼也要跳過 `signal(sync)`：
- 如果不跳過，main thread 對 sync sem +1，但 display thread 仍卡在 `WAIT_FOR_VSYNC`、永遠走不到 `wait(sync)` 來 -1
- 反覆累積 65535 次 → display 醒過來時碰到 `Error: -3` (`RT_EFULL`)
- 那就把 §6.6 / §6.7 那條 sem leak crash 重新製造一次了

跳過後，**兩個 sem 都保持 0**，無 leak。

### Self-healing 機制（重要）

當 VSYNC 恢復後：
1. display thread 從 `WAIT_FOR_VSYNC` 醒過來 → `signal(sync_notify) +1` → 回頭 `wait(sync)` block
2. main thread 下一張 `wait_sync_ready` 立刻 succeed（消耗 sync_notify 的 1） → 印 `recovered after N timeout(s)`
3. main 跑 blit → PAN → `signal(sync) +1`
4. display unblock → 跑下一張 frame
5. 狀態機**自動回到 steady state**

不需要任何手動 recovery、不需要重 init、不需要重啟 device。

### L3 細節：watchdog thread

```c
RT-Thread priority 25（明顯低於 LVGL main / display 的 3），stack 4 KB
每 1 秒 wake 一次

每 5 秒印 [heartbeat]：
    [heartbeat] t=12345ms recv=300 show=298 rotate_fail=0 sync_timeout=0
                recv_idle=234ms show_idle=187ms

偵測 stall：
    recv 在 3 秒內有進度（PC 還在推）
    AND show 已經卡 10 秒沒進度
    → 印 [WATCHDOG] !!! DISPLAY STALL — recv advancing but show frozen ...
```

關鍵設計決定：

1. **不用 LVGL OSAL `lv_thread_init`**：那個 helper 把 `LV_THREAD_PRIO_LOW = 1` 直接傳給 `rt_thread_create`，但 RT-Thread 是「數字越小越高優先級」，會把 watchdog 跑成超高優先級反而打到 LVGL。直接用 `rt_thread_create` + 數字 25 才正確。
2. **不自動 recover**：不嘗試 force-signal 已 stuck 的 sem、不嘗試 reopen GE2D handle。**檢測勝過誤救**——這條 thread 跟卡住的 thread 沒持有相同的 lock，貿然動 sem 可能把可恢復的 lockup 升級成 memory corruption。
3. **「recv 動 + show 不動」才算 stall**：「recv 停 + show 停」是正常 end-of-stream（PC 沒送了），不該誤報。

### 驗收方式：log 該長什麼樣

**穩定狀態（理想）**：

```
perf stats: 30 in 967ms fps=31.02 (intv=32.23)
disp_flush: sync_wait=0us blit=6841us (n=30)
[heartbeat] t=10000ms recv=303 show=303 rotate_fail=0 sync_timeout=0 recv_idle=33ms show_idle=33ms
[heap snapshot] sys total=...
[heartbeat] t=15000ms recv=455 show=455 ...
```

**ge timeout 偶發**（§6.11 接住）：

```
[E] ge2d rotate sync fail (total=1 streak=1) — last good frame kept on screen
[E] ge2d rotate recovered after 1 failure(s) (total=1)
[heartbeat] t=...   rotate_fail=1 sync_timeout=0  ← 數字累積但沒 stall
```

**VSYNC stuck 偶發**（§6.12 L1 + L2 接住）：

```
[disp] !!! sync_notify wait timed out after 200ms (streak=1 total=1) — display thread likely stuck in AICFB_WAIT_FOR_VSYNC. Skipping frame.
[disp] sync_notify recovered after 1 timeout(s) (total=1)   ← 自己回來了
[heartbeat] t=...   sync_timeout=1  ← 數字累積但沒 stall
```

**VSYNC 真的死了**（§6.12 L3 watchdog 看見）：

```
[disp] !!! sync_notify wait timed out after 200ms (streak=1...
[disp] !!! sync_notify wait timed out after 200ms (streak=2...
[disp] !!! sync_notify wait timed out after 200ms (streak=3...
（之後每 30 次才印一次，避免灌爆 UART）
...
[heartbeat] t=20000ms recv=600 show=458 ...   ← recv 持續但 show 不動
...
[WATCHDOG] !!! DISPLAY STALL — recv advancing (idle=234ms) but show frozen (idle=12500ms, count=458). GE2D or DE likely wedged; last good frame held on screen. rotate_fail=0 sync_timeout=63 !!!
```

→ **這就是 user 該重啟 device 的明確訊號**。在 §6.12 之前是「log 安靜消失，無從判斷」。

### 為什麼這次不順便砍掉 `AICFB_WAIT_FOR_VSYNC`

§6.10 的 Phase 1.A 提過：直接把 `mpp_fb_ioctl(AICFB_WAIT_FOR_VSYNC)` 拿掉可以：
- 把 fps 從 ~31 推到 ~45
- **同時**消除點 (B) 的死鎖向量

聽起來雙贏，但有兩個保留：
1. 沒 VSYNC 等的話，PAN 跟 scan-out 可能落在同一條 scanline 上 → 視覺 tearing
2. 改動 fundamental display 行為，需要視覺驗證才知道對 user 是否可接受

§6.12 的三層止血**已經足以**讓 silent lockup 變成「看得見的 stall + 自動恢復」，而且**不破壞既有顯示行為**。先 ship 這版，等 user 看到 watchdog 訊息頻率高才需要考慮砍 VSYNC。

### 一句話結論

> §6.12 解掉「**log 完全 silent 的 silent lockup**」這個最難察覺的失效模式。三層止血：(1) `wait_sync_ready` 200 ms timeout + skip frame、(2) sem 不 leak、(3) watchdog thread 每 5 秒一次 heartbeat 保證 log 永遠有聲音。組合起來：HW 偶發 stuck 時系統自動恢復；HW 真的死了 user 也能在 log 上**直接看到 STALL 訊息**而不是面對「device 看起來活著但畫面凍結」的鬼故事。

---

## 6.13. GE2D 全路徑 telemetry + `dump_regs` rate-limit（25 分鐘後 NO.1 CPU Exception 修法）

### 問題復現

實測（2026-06-04 早上）：device 開機後 streaming 進入 screen_4_Area，**穩定跑 25 分 25 秒**，平均 30+ fps，drop 0%，watchdog `rotate_fail=0` `sync_timeout=0` 全程乾淨。然後突然：

```text
[E] ge_start_and_wait()132 ge timeou
CPU Exception: NO.1
x1(ra)   : 0000000040042712     x2(sp)   : 0000000040156530
x4(tp)   : 00000000deadbeef     x15(a5)  : 00000000deadbeef
x24..x27(s8..s11) : 全部 deadbeef
mepc     : 00000000deadbeee
mstatus  : 8000000a00007800
```

PC 端立刻 `(idle, waiting for stream)`，device 不再回應。

### 失效鏈分析

1. **`ge_start_and_wait()` 第 132 行**：`hal_log_err("ge timeout")` 印出，下一行就是 `dump_regs()`。
2. **`dump_regs()`**：迴圈 256 次 read GE register（64 行，每行 4 個 reg）→ 透過 `hal_log_info` → `rt_kprintf` → RT-Thread console 序列化，每次 printf 在 stack 上展開 vararg + serial 寫入 buffer。
3. **GE2D worker thread stack 在 §6.13 之前是 8 KB**（§6.11 已增大過一次）。在連續 25 分鐘 GE2D 壓力下，當 timeout 終於發生時：
   - dump_regs 的 64 個 printf 把 stack 用到 worst-case 邊緣，
   - **trash 掉鄰近 task 的 saved register frame**（`tp` / `s8..s11` 通通變 `deadbeef`），
   - `mepc=0xdeadbeee` ＝ 解析錯誤的 return address，
   - 進 trap，印 `CPU Exception: NO.1 = Instruction Address Misaligned`，
   - kernel 死，再也起不來。

`ge timeout` 本身**並不致命**——HAL 已經有 `GE_TIMEOUT = 2 sec` 用 `ge_reset()` 接住——致命的是「timeout 的診斷工具自己把 stack 吃爆」。

### 修法（雙層）

#### Layer 1：GE2D 全路徑 fail telemetry 統一化

在 §6.11 之前，failure counter 只覆蓋 `rotate` 路徑。實機看 watchdog `rotate_fail=0` 全程乾淨，但其他 GE2D 路徑（`img`/`fill`/`blit`/`transform`/`rotate_any`）的 timeout **完全沒進 counter**。這就是「明明 watchdog 一直沒抱怨，最後卻直接炸 NO.1」的原因之一。

新加 `lv_ge2d_fail_path_t` enum（`lv_draw_ge2d.h`）：

| Enum | 對應路徑 | 涵蓋哪些 op |
| --- | --- | --- |
| `LV_GE2D_FAIL_PATH_ROTATE` | `lv_draw_ge2d_rotate` + `rotate_any_degree` | `mpp_ge_rotate` |
| `LV_GE2D_FAIL_PATH_IMG` | `lv_draw_ge2d_transform` | `mpp_ge_bitblt` (with rotation flags) |
| `LV_GE2D_FAIL_PATH_FILL` | `lv_draw_ge2d_fill` | `mpp_ge_fillrect` |
| `LV_GE2D_FAIL_PATH_BLIT` | `lv_draw_ge2d_blit` + streaming hot path（`custom_disp_stream.c`） | `mpp_ge_bitblt` (plain copy) |
| `LV_GE2D_FAIL_PATH_LAYER` | 預留 layer composition 路徑 | 目前未用 |

統一 API：

```c
void lv_draw_ge2d_record_fail(lv_ge2d_fail_path_t path, const char *stage);
void lv_draw_ge2d_record_success(void);
```

`stage` 是 `"emit"` / `"sync"` / `"bitblt"` / `"rotate_any"` / `"fillrect"` 等字串常數，告訴你 timeout 是發生在「送命令」還是「等完成」階段。

Watchdog heartbeat（`lv_port_disp.c::freeze_watchdog_thread`）增強：

```text
舊：[heartbeat] t=Xms recv=Y show=Z rotate_fail=0 sync_timeout=0 ...
新：[heartbeat] t=Xms recv=Y show=Z rotate_fail=3 (last=blit/sync) sync_timeout=0 ...
                                              ^^^^^^^^^^^^^^^^^
                          只在 rotate_fail!=0 才印，避免污染正常 log
```

`rotate_fail` 欄位名保留是為了**不破壞既有外部 grep 腳本 / 過往 chat log 的可搜尋性**，但實際 counter 已經是「所有 GE2D 路徑 fail 數的總和」（§6.11 註解有寫）。

`custom_disp_stream.c` 的 `stream_blt_record_fail()` 也呼叫 `lv_draw_ge2d_record_fail(LV_GE2D_FAIL_PATH_BLIT, stage)`，把 streaming 自己的 bitblt timeout 也餵進共用 counter，watchdog 就看得見。

#### Layer 2：`dump_regs()` rate-limit（**真正修 crash 的關鍵**）

`bsp/artinchip/hal/ge/hal_ge_normal.c`：

```c
#define GE_DUMP_REGS_MAX_FULL  2U
static volatile uint32_t s_dump_regs_count;

static void dump_regs(void)
{
    uint32_t n = ++s_dump_regs_count;
    if (n > GE_DUMP_REGS_MAX_FULL) {
        hal_log_err("ge dump_regs suppressed (n=%u, only first %u dumped ...)\n",
                    n, GE_DUMP_REGS_MAX_FULL);
        return;
    }
    /* 256 readl + 64 printf 只在前兩次跑 */
    ...
}
```

設計考量：

- **保留前 2 次完整 dump**：第一次 timeout 的 reg 內容是診斷 silicon corner case 的唯一證據，必須留。第 2 次允許是因為第一次有可能在 boot 早期被 swallow（log buffer 未 attach）。
- **第 3 次起只印一行**：HW 一旦 wedge，後續 timeout 的 reg 內容**完全相同**，full dump 沒任何新資訊，但 stack 風險翻倍。
- **`s_dump_regs_count` 不 reset**：如果 device 一輩子（power cycle 前）出兩次 GE timeout，兩次都會完整 dump；第三次起壓制。這比「reset on recovery」安全，因為 reset 後再次 wedge 等於回到 stack overflow 風險區。
- **配合 §6.11 worker stack 8 KB → 16 KB**：兩道保險疊加。即使 dump 沒被 rate-limit 接住，16 KB stack 也能吸收。

### Build / 驗證 steps

1. 全 rebuild（修了 `bsp/artinchip/hal/ge/`，CRC 變、binary 一定重出）。
2. 燒 image，跑同樣的 streaming 25 分鐘以上。
3. 觀察 UART：
   - **正常**：watchdog 每 5 秒一條 heartbeat、`rotate_fail=0`、fps 30+。
   - **GE 偶發 wedge（可接受）**：heartbeat 出現 `rotate_fail=N (last=img/sync)` 之類，但**完全不 crash**，next frame 自己接回去。GE 連續 wedge 3+ 次會看到 `ge dump_regs suppressed (n=3, ...)`，**確認 rate-limit 接住了原本會炸 stack 的路徑**。
   - **NO.1 / NO.6 不應再出現**。如果還出現，就回頭看 §6.12 + §6.13 是否有更上游的 stack 路徑沒蓋到。

### 一句話結論

> §6.13 的兩道修補解掉「**穩跑 25 分鐘後 NO.1 暴死**」這個最後一個 must-fix。Layer 1（全路徑 telemetry）讓 watchdog 不再瞎眼，**所有 GE2D 路徑的偶發 timeout 都會在 heartbeat 上看見**，給 user / debugger 第一手線索。Layer 2（`dump_regs` 限 2 次）切掉「診斷工具自己把 stack 吃爆」的反諷死路。組合 §6.11 (stack 增大) + §6.12 (silent lockup 三層) + §6.13 (全路徑 telemetry + dump 限頻) 這六個修補，「≥30 fps + 不崩潰兩個 must 條件**才能在 30 分鐘以上長時間 streaming 下都成立**」。

---

## 6.14. `dump_regs()` 改 compact dump（30 分鐘後 NO.1 復現的二次修法）

### 為什麼 §6.13 還不夠？實測再現

打完 §6.13 的 patch（worker stack 16 KB、全路徑 telemetry、`dump_regs` cap=2）之後做 30 分鐘 streaming，結果在 `t=1832969ms`（約 30 min 33 sec）再次踩同樣的 trap：

```
[E] ge_start_and_wait()132 ge timeouCPU Exception: NO.1
x4(tp)   : 00000000deadbeef
x24..x27(s8..s11) : 00000000deadbeef
mepc     : 00000000deadbeee
mcause   : 0000000000000001
```

**關鍵觀察**：

1. log 在 `ge timeou` 中途就被打斷（連 `t\n` 都沒印完），代表 crash 發生在**第一次 `ge timeout` 的處理路徑裡**，不是後續的 timeout。
2. `dump_regs` cap=2 的意思是「**前 2 次完整跑**」，**第一次必跑滿 256 reg / 64 printf**。也就是說 §6.13 的 rate-limit **完全沒接到第一次的炸彈**。
3. 16 KB worker stack 雖然比原本 8 KB 多一倍，但跑 64 次 `rt_kprintf` 的 worst-case frame 還是會被 console 序列化 + UART tx 的呼叫鏈吃光。實測證明 16 KB 也不夠。

結論：**問題不是 dump 太多次，是 dump 一次就太貴**。

### 真正的修法：改成單行 compact dump，cap=8

`bsp/artinchip/hal/ge/hal_ge_normal.c::dump_regs()`：

```c
#define GE_DUMP_REGS_MAX_COMPACT  8U
static volatile uint32_t s_dump_regs_count;

static void dump_regs(void)
{
    uint32_t n = ++s_dump_regs_count;
    if (n > GE_DUMP_REGS_MAX_COMPACT) {
        if (n == GE_DUMP_REGS_MAX_COMPACT + 1U)
            hal_log_err("ge dump_regs: further dumps suppressed after n=%u\n",
                        (unsigned)GE_DUMP_REGS_MAX_COMPACT);
        return;
    }

    /* 兩個單行 printf，加總 ~12 個 reg；args 全部塞 a0..a7，stack 框 bounded */
    hal_log_err("ge regs n=%u: ctrl=%08x status=%08x start=%08x to_cyc=%08x to_cnt=%08x\n",
                n, readl(GE_BASE + GE_CTRL), readl(GE_BASE + GE_STATUS),
                readl(GE_BASE + GE_START), readl(GE_BASE + HW_TIMEOUT_CYCLE),
                readl(GE_BASE + HW_COUNTER));
    hal_log_err("ge regs n=%u: src_ctrl=%08x dst_ctrl=%08x out_ctrl=%08x cmd_st=%08x cmd_end=%08x cmd_len=%08x\n",
                n, readl(GE_BASE + SRC_SURFACE_CTRL), readl(GE_BASE + DST_SURFACE_CTRL),
                readl(GE_BASE + OUTPUT_CTRL), readl(GE_BASE + CMD_BUF_START_ADDR),
                readl(GE_BASE + CMD_BUF_END_ADDR), readl(GE_BASE + CMD_BUF_VALID_LENGTH));
}
```

### 設計取捨

| 項目 | 舊（§6.13） | 新（§6.14） | 差異 |
|---|---|---|---|
| 每次 dump 印幾行 | 64 行 | **2 行** | 1/32 的 stack 壓力 |
| 每行帶幾個 reg | 4 | 5–6 | 用滿 RISC-V a0..a7（無 spill） |
| 涵蓋多少 reg | 全部 256 | **12 個關鍵 reg** | 足夠診斷 wedge |
| 上限次數 | 2 次完整 + 後續壓制 | **8 次 compact + 後續壓制** | 因為每次便宜了，可以多印 |
| 第一次 timeout 安全嗎？ | **❌ 第一次就可能炸 stack** | **✅ 每次都 bounded** | 真正的修法 |

12 個關鍵 reg 已經涵蓋所有 wedge 診斷需求：

- `GE_CTRL` / `GE_STATUS` / `GE_START`：HW 在哪一態？raise 哪個 IRQ？
- `HW_TIMEOUT_CYCLE` / `HW_COUNTER`：HW 自己的 timeout fire 了嗎？
- `SRC_SURFACE_CTRL` / `DST_SURFACE_CTRL` / `OUTPUT_CTRL`：current descriptor 看起來合理嗎？
- `CMD_BUF_START_ADDR` / `CMD_BUF_END_ADDR` / `CMD_BUF_VALID_LENGTH`：cmdq 卡在哪？

剩下的 200 多個 reg 幾乎都是 surface stride / scaler coef / CSC matrix，**HW wedge 後它們的值跟前一個成功 op 一樣**，全 dump 不會多給任何資訊，但會把 stack 炸掉。

### Stack 帳

新版每次 `dump_regs()` 在 worst case 大約是：

- 2 個 `hal_log_err` call frame（each ~64–96 B for spill + printf state）
- 1 個 `rt_kprintf` 內部 vararg buffer（~256 B for format string scratchpad）
- UART tx 序列化 frame（~128 B）

→ **總計 < 1 KB**。原版 64 行的 worst case 是 ~8–12 KB（因為 RT-Thread console 序列化會 nested call，每 printf 都堆一層）。**從 12 KB 降到 1 KB，這才是 24x stack safety margin**。

### 一句話結論

> §6.14 把「**診斷工具自己會把 stack 吃爆**」這個 §6.13 沒徹底解決的問題，從「限制呼叫次數」改成「**每次都便宜**」。Trade-off：失去 244 個 reg 的 verbose dump，**換到 device 不會在 30 分鐘崩潰**——非常划算。組合 §6.11 + §6.12 + §6.13 + §6.14，從此 30 分鐘以上 streaming 既 ≥30 fps 又絕不 crash 才真正成立。

---

## 6.15. Stack canary 二度復現 t=1832969ms 後的更底層偵測

### 為什麼 §6.14 還不夠？實測**第三次**再現

打完 §6.14 的 compact dump（每次 dump 只剩 12 reg / 4 printf，從 12 KB 降到 1 KB）之後做 30 分鐘 streaming，結果在 `t=1832969ms`（**又是同一個 30 min 33 sec**）再次踩同樣的 trap：

```
[E] ge_start_and_wait()132 ge timeouCPU Exception: NO.1
x1(ra)   : 0000000040042712     x2(sp)   : 0000000040156530     ...
x4(tp)   : 00000000deadbeef     ...
x15(a5)  : 00000000deadbeef     ...
x24(s8)  : 00000000deadbeef     x25(s9)  : 00000000deadbeef
x26(s10) : 00000000deadbeef     x27(s11) : 00000000deadbeef
mcause   : 0000000000000001    (Instruction Address Misaligned)
mtval    : 00000000deadbeee
mepc     : 00000000deadbeee
```

對照之前兩次 crash：

| 次數 | 時間點 | mepc | tp / s8-s11 | 修法後是否改變 |
|---|---|---|---|---|
| §6.13 | t=1832969ms | `0xdeadbeee` | `0xdeadbeef` | §6.13 完整 dump_regs 把 stack 吃爆 → 推 §6.14 |
| §6.14 | t=1832969ms | `0xdeadbeee` | `0xdeadbeef` | §6.14 compact dump 也沒救到 → 推 §6.15 |
| §6.15 | t=1832969ms | `0xdeadbeee` | `0xdeadbeef` | **同一秒、同一 reg pattern、同一 mepc** |

§6.14 把 dump_regs 從 12 KB stack 砍到 < 1 KB，**但 crash 時間連 1 ms 都沒往後移**。這個事實本身就推翻了「`dump_regs` 是 stack overflow 的 trigger」這個 §6.13 + §6.14 的假設。

### 真正的 root cause：dump_regs **不是** trigger，它是 **victim**

把上面三次資料攤在一起就會看到，所謂的「stack overflow」**早就發生了**，`dump_regs` 只是第一個剛好踩到被汙染區域的受害者。重點是：

1. **三次相同的 `t=1832969ms`** → root cause 是 **deterministic** 的：某個 thread 在某個累積狀態下踩到 SP 最低點。
2. **mepc=0xdeadbeee（不是 0xdeadbeef！）** → mret 是從 stack 載入的，所以**整個 saved context 都被覆蓋成 RT-Thread 的 stack-init pattern（`0xdeadbeef`）**。
3. **tp / s8 / s9 / s10 / s11 全是 0xdeadbeef** → 這些都是 callee-saved register，存在 saved context 裡，跟 mepc 同源。
4. **a5 / a7 也是 deadbeef** → 連 caller-saved 都被沖掉，代表整塊 saved frame 都被踩。

也就是說：

> 某個**別的 thread**（不一定是 GE2D worker，也不一定是 display thread）在某個時間點 **stack pointer 已經低於它自己的 `stack_addr`**，往下一直寫到**另一個正在被切出去 / 待 resume 的 thread 的 saved context**。RT-Thread 把那個受害 thread 切回來時，從那塊已經被「外人」寫成 deadbeef 的記憶體載入 saved context，於是 PC=0xdeadbeee → CPU Exception NO.1。

`dump_regs` 報的 register 是**被害人的**狀態，**完全不能指認兇手**。這就是為什麼 §6.13 + §6.14 兩次修補 dump_regs 都不可能修到問題：**修錯人了**。

### 解決方案：4-word bottom-of-stack canary + 1 Hz 自動巡檢

`lv_port_disp.c` 新增 `lv_stack_canary_install / lv_stack_canary_check_all`：

1. **每個有風險的 thread 在創建時，把 4 個固定 magic word（`0xCA1A0001..0xCA1A0004`）寫到 stack 最底（lowest address，因為 RT-Thread stack 向下長）**。任何 thread 一旦 stack pointer 越過 `stack_addr` 寫下去，**最先被踩到的就是這 4 個字**——比踩到別 thread 的 saved context 早。
2. **freeze_watchdog（已經是 1 Hz，本來就在跑）每 tick 走一遍 canary 表**，發現任何 thread 的 4 個字被改過，就立刻 dump：
   - thread 名稱（`tid->name`）
   - label（`"ge2d_render"` / `"aic_display"` / `"freezewdg"` / `"custom_uart_rx"`）
   - `stack_addr` 和 `stack_size`
   - expected vs. observed 4 words
3. **每 thread 只報一次**，避免 kprintf storm 把 watchdog 自己撐爆。

### Magic 選擇的兩個約束

| Magic 候選 | 為什麼不行 |
|---|---|
| `0x00000000` | thread 剛建立時 stack 就有大量 zero，誤判風險高。 |
| `0xFFFFFFFF` | flash erase pattern、memset(0xff) 都會誤判通過。 |
| `0xDEADBEEF` | **就是 RT-Thread 自己的 stack init pattern**，被踩了會跟「沒被踩」長一樣，等於把整個 canary 廢掉。 |
| `0xCA1A0001..04` | ✅ 唯一 4 個各自不同（讓「全部被同一 byte 沖掉」也能立刻看見），bit pattern 也不是常見 magic。 |

### Canary 覆蓋的 4 個 thread（按 stack overflow 風險由高→低排序）

| Thread | stack_size | label | 為什麼選它 |
|---|---|---|---|
| `custom_uart_rx` | **2 KB** | `custom_uart_rx` | **整條 pipeline 最小 stack**。如果某個 future change 在 USB UART RX handler 裡加一個大 local buffer，這裡會第一個爆。 |
| `aic_display` | 8 KB | `aic_display` | LVGL dispatch → `disp_flush` → GE2D blit → wait_sync_ready 這條鏈本身會吃 stack；任何 LVGL upgrade 都可能讓 frame 變深。 |
| `ge2d_render` | 16 KB（§6.11 已增大） | `ge2d_render` | LVGL render dispatch → `mpp_ge_emit` → `hal_log_err` → `dump_regs` 這條最深的路徑（§6.14 已 compact，但歷史證明它本身就是受害者，不能掉以輕心）。 |
| `freezewdg` | 4 KB | `freezewdg` | watchdog 本身**也會自己爆**——它就是負責印 canary breach 的，如果它先死掉，我們就什麼資訊都看不到。所以它**第一個** install 自己。 |

### 為什麼要在 `lv_thread_init` / `rt_thread_create` **之後立刻**寫？

RT-Thread 的 `lv_thread_init` 內部會同步呼叫 `rt_thread_startup`，**thread 馬上就在背景跑了**。但 canary 寫的是 stack 最底 4 word，新生 thread 的 SP 還在 stack 頂端，要往下走完整個 stack 才會碰到——這 4 word 是**死區**，正常情況下永遠不會被合法 SP 蓋到。所以：

```c
lv_thread_init(&disp->thread, prio, fn, 8 * 1024, disp);
lv_stack_canary_install(disp->thread.thread, "aic_display");   // ✅ safe, no race
```

是安全的（如果 thread 已經 stack overflow，那 canary install 馬上就會在下一 tick 報警；如果還沒爆，canary 寫進去就完成了任務）。

### Cost 分析（為什麼這完全是 free lunch）

- **記憶體**：4 thread × 16 byte = **64 byte 一次性**，不在 hot path。
- **CPU**：watchdog 每 tick（1 Hz）跑 4 thread × 4 word load + 4 compare = **16 ld + 16 cmp / sec**。連 0.001% CPU 都不到。
- **Hot path 0 impact**：disp_flush / GE2D blit / USB RX / LVGL dispatch 完全沒新增任何指令。
- **Worst-case false positive**：0。Magic 是雙向唯一的——4 個字都要正確才算 pass，連續 4 個 deadbeef / 0 / FF 都會被抓到。

### 修補後的下一步行動指引

實機跑到 t≈1832 sec 的時候，**會二擇一**：

1. **Canary 先報，watchdog dump 出 culprit thread**：
   ```
   ================ STACK CANARY BREACH ================
   [canary] thread 'aic_disp' label=aic_display tid=0x40a3xxxx
   [canary] stack_addr=0x4015xxxx stack_size=8192
   [canary] expected: ca1a0001 ca1a0002 ca1a0003 ca1a0004
   [canary] observed: deadbeef deadbeef deadbeef deadbeef
   [canary] >>> THIS THREAD OVERFLOWED ITS STACK <<<
   ```
   → 立刻知道是 `aic_display` / `ge2d_render` / `custom_uart_rx` / `freezewdg` 哪一個爆，改它的 `stack_size` 一倍即可。

2. **Crash 還是 NO.1（mepc=0xdeadbeee）但 canary 沒先報**：
   - 代表罪犯**不在這 4 個 thread**——是別處的 thread（USB stack / LVGL timer / RT-Thread idle / DEC / DMA callback）爆掉。
   - 下一步：把 canary install 範圍擴大到其他 suspect。`LV_STACK_CANARY_MAX` 是 8，目前用了 4，**還有 4 個 slot**。
   - 也可以反過來在 main 的 `rt_thread_init` wrapper 裡攔截**所有** thread 的創建，一次性全部 instrument。

### 一句話結論

> §6.13 + §6.14 兩次修 `dump_regs` 都沒改變 crash 時間，**因為它根本不是 root cause，只是受害者**。`t=1832969ms` 三次相同的 deadbeef-context 模式，指向「**某個 thread 在累積到某狀態時 overflow 自己的 stack，去汙染別人的 saved context**」這個 cross-thread corruption。§6.15 的 stack canary 把「**誰先踩到自己 stack 最底**」這個關鍵問題從黑盒（被害人 register dump）變成白盒（thread 名稱 + label），下一次 1832 sec 出事的時候我們會**第一次**真正知道兇手是誰，而不是再多印一份被沖掉的 register。組合 §6.11 + §6.12 + §6.13 + §6.14 + §6.15，從此「≥30 fps + 不崩潰」如果再失敗，**至少不會盲目失敗**。

---

## 6.16. Canary v1 自爆 → v2 名稱化（boot fail post-mortem）

### 症狀：燒完 §6.15 的 canary 之後完全不會開機，USB 也看不到

實機驗證 §6.15 的時候，UART log 在開機階段卡死，**裝置完全沒反應，USB device 都不會枚舉到 PC**：

```
Boot device = 4(BD_SPINOR)
Mount APP in blk blk_rodata
Mount APP in blk blk_data
Failed to mount elm
[canary] installed for ge2d_render tid=0x40a375e8 stack=[0x40a37708..+16384] words=4
thread:thread stack overflow
(silence ──────── 永遠卡在這 )
```

`[canary] installed for ge2d_render` 印出來下一行就是 RT-Thread 自己報的 `thread:thread stack overflow`，然後**整個系統靜默**。連 freeze watchdog 都沒機會啟動。

### 五秒就找到的 root cause

去翻 `kernel/rt-thread/src/scheduler.c:92-127` `_scheduler_stack_check()`：

```c
#ifdef RT_USING_OVERFLOW_CHECK
static void _scheduler_stack_check(struct rt_thread *thread)
{
#ifdef ARCH_CPU_STACK_GROWS_UPWARD
    if (*((rt_uint8_t *)((rt_ubase_t)thread->stack_addr + thread->stack_size - 1)) != '#' ||
#else
    if (*((rt_uint8_t *)thread->stack_addr) != '#' ||
#endif
        (rt_ubase_t)thread->sp <= (rt_ubase_t)thread->stack_addr ||
        ...)
    {
        rt_kprintf("thread:%s stack overflow\n", thread->name);
        level = rt_hw_interrupt_disable();
        while (level);                    // <── 永久關中斷自旋
    }
}
```

**RT-Thread 自己**就有 stack overflow 檢查，做法是：
- 在每個 thread 的 `stack_addr[0]` 寫一個 magic byte `'#'`（0x23）
- 每次 context switch 就驗證這個 byte 沒被覆蓋
- 一旦失守 → `rt_hw_interrupt_disable()` 後**永久死迴圈**（連 kprintf 都不會再印）

§6.15 v1 的 canary 把 `0xCA1A0001` 寫到 `stack_addr[0..3]`，byte 0 在 RISC-V little-endian 下變成 `0x01`，**不是 `'#'`**：

```
裸寫之前：[stack_addr] = '#'  0xa5  0xa5  0xa5  ... (RT-Thread 初始化)
被 v1 寫之後：[stack_addr] = 0x01 0x00 0x1a 0xCA  ... (我們的 canary)
                             ↑↑↑↑
                       不是 '#'，下一次 context switch 立刻判 overflow
```

→ scheduler 抓到 overflow → 關中斷自旋 → USB stack 永遠跑不到 → PC 看不到裝置。

換句話說：**§6.15 v1 親手把 RT-Thread 自己的 canary 蓋掉，被 RT-Thread 自己抓 overflow**。我們重複造了一個 RT-Thread 已經有的輪子，而且還把原廠輪子拆掉。

### 一個關鍵額外發現：RT-Thread 內建檢查比我們強

| 比較項 | §6.15 v1 自製 canary | RT-Thread 內建 |
|---|---|---|
| 檢查頻率 | 1 Hz（freeze watchdog tick） | **每次 context switch**（µs 級） |
| 檢查方法 | 4 word 比對 stack 最底 | 1 byte (`'#'`) + SP bounds（`sp <= stack_addr` 也算）|
| Trigger 後行為 | 印 log，繼續跑（victim 可能再 crash） | 印 log + **interrupt-disable spin**（保留現場） |
| Code overhead | 兩個函式 + 1 Hz loop | 0（已經編進 kernel） |

§6.15 v1 唯一比 RT-Thread 強的地方是「**對受影響 thread 印一個有意義的 label**」。可是 RT-Thread 自己印的是 `tid->name`——**只要把 `tid->name` 改成有意義的字串就夠了**，完全不需要動 stack。

### 為什麼 LVGL thread 的 `tid->name` 沒意義

`lv_rtthread.c` 把所有 LVGL thread 都用同一個 literal "thread" 建立：

```c:packages/artinchip/lvgl-ui/lvgl_v9/lvgl/src/osal/lv_rtthread.c
thread->thread = rt_thread_create("thread",   // <── 全部都這個名字
                                  callback, user_data, stack_size, prio,
                                  THREAD_TIMESLICE);
```

於是不管 `aic_display_thread` / `ge2d_render_thread_cb` 哪一個爆，RT-Thread 印出來都是：

```
thread:thread stack overflow
```

完全分不出是誰。**這才是 §6.15 真正要解的問題**——不是「自己再做一次 overflow check」，而是「**讓 RT-Thread 既有的 check 印出有意義的名字**」。

### v2 修法：只改名，不寫 stack（一次性 commit）

`lv_port_disp.c` `lv_stack_canary_install` 改成：

```c
void lv_stack_canary_install(rt_thread_t tid, const char *label)
{
    if (tid == RT_NULL) return;

    /* 重點：絕對不能碰 tid->stack_addr[0]，那是 RT-Thread 的 '#' magic！
     * 只改名字，RT-Thread 內建 overflow check 之後就會印
     *     thread:ge2d_render stack overflow
     * 而不是
     *     thread:thread stack overflow
     */
    if (label) {
        rt_strncpy(tid->name, label, RT_NAME_MAX - 1);
        tid->name[RT_NAME_MAX - 1] = '\0';
    }

    rt_kprintf("[canary] labeled thread '%s' ...\n", tid->name);
}

void lv_stack_canary_check_all(void)
{
    /* no-op — RT-Thread 已經在每次 context switch 都檢查了 */
}
```

`lv_stack_canary_check_all()` 改成空殼，是為了**保留呼叫端介面**（freeze_watchdog 還會 call 它），減少改動面，**將來真要加更強的檢查時再回來填**（但要記住絕對不能再去動 `stack_addr[0]`）。

### 三個別人也會踩的同類教訓

1. **動 RTOS thread 結構的「底部」之前先翻它原始碼**。所有正規 RTOS（RT-Thread / FreeRTOS / Zephyr）都會在 stack 兩端塞 magic byte 做 overflow 偵測。在那個位置寫東西**一定**會踩到。
2. **如果功能名稱在 OS 裡已經有了（"stack overflow check"），就先確認 OS 是不是已經有 → 怎麼配置 → 為什麼覺得不夠用**。`grep -r "RT_USING_OVERFLOW_CHECK"` 三秒鐘就能避開整個 boot fail。
3. **改動 boot-critical thread**（GE2D / display worker / UART RX）的 stack 初始化路徑時，先**只改一個 thread** 觀察 boot 過程是不是還能走完。一次改 4 個再燒，故障被放大 4 倍。

### Cost of v2 vs v1

| 項目 | v1 | v2 |
|---|---|---|
| 開機可用 | ❌ 死在 scheduler check | ✅ 正常開機 |
| 任何 RAM 寫入 | 每 thread 16 byte 寫到 stack 底 | **0 byte**（只改 `tid->name` 16 byte，本來就是 thread struct 一部分） |
| Runtime CPU | watchdog tick 算 canary | watchdog tick 進來立刻返回（no-op） |
| Stack overflow 偵測力 | 1 Hz 4-word 比對 | **每次 context switch** RT-Thread 內建（強很多） |
| 抓到後可定位 thread 名稱 | ✅ | ✅（透過 RT-Thread 自己的 log） |
| 抓到後保留現場 | ❌ 印完繼續跑 | ✅ 關中斷自旋（mdump / JTAG 可上 GDB） |

v2 各項都 ≥ v1，**而且還能開機**。

### v2 下一次出事時 log 會長這樣

實機跑到下次 stack overflow 時：

```
[heartbeat] t=1832000ms recv=N show=M rotate_fail=0 ...
[heartbeat] t=1832500ms recv=N+? show=M+? rotate_fail=0 ...
thread:ge2d_render stack overflow                   <── 兇手名稱直接出來
                                                     <── 然後系統定格（關中斷自旋）
```

看到 `thread:ge2d_render stack overflow` 就知道要去 `lv_draw_ge2d.c` 把 `16 * 1024` 改成 `32 * 1024`。看到 `thread:custom_uart_rx` 就去 `custom.c` 把 `CUSTOM_UART_THREAD_STACK_SIZE` 從 2 KB 拉到 4 KB。**第一次**真的能單看 UART log 就知道改哪一條 stack_size。

---

## 6.17. UI 觸發 BROM mode OTA 在 2.3% 失敗：`aic_set_reboot_reason` first-writer-wins

### 症狀

從 UI 端送 9-byte BROM trigger frame (`FF A1 B2 E3 F4 C5 D6 EE EE`)，**之前可以成功 OTA、今天某次重 build 後完全燒不進去**：

```
[UI 端 OTA tool log]
Upgrade fwc: image.updater.spl, size 238608 ...
Send the last 238608
Progress: %2.25
[ERROR ]: libusb_bulk_recv failed, siz(0) != len(13), Pipe error(-9)
[ERROR ]: aicupg_trans_recv_pkt: no CSW. (rc = 0)
[INFO ]: _do_fwc_upgrade: Switching to new stage
[ERROR ]: libusb_bulk_send failed, Input/Output Error(-1)
[ERROR ]: image_do_upgrade_inner: No device is connected
```

注意「Progress 2.25%」這個數字：`Meta for image.updater.spl size 0x3a410 (238608)`，238608 byte 就是 updater.spl 本身。也就是說：
- BROM 接到 DDR init 包 ✅
- BROM 接到 updater.spl 包 ✅
- **接下來該由 SPL 接手 USB device 重新 enumerate，但 SPL 沒接手** ❌

**重要對比**：同一個 `.img`、手動按 BROM 按鍵 + 上電（cold boot 直接進 BROM mode）的話**OTA 成功**。所以問題不在 BROM/SPL/DDR init 本身，而是在「**UI 觸發**」這條路徑。

### 對應的開機 log

```
Reset flag: 0x500
Reset action: Watchdog-Reset, reason: Command-Reboot     <── 不是 Upgrade-Mode！
```

這就是 smoking gun。`custom_trigger_brom_mode()` 明明呼叫了：

```c
aic_set_reboot_reason(REBOOT_REASON_UPGRADE);
```

但 BROM 讀到 reason 不是 UPGRADE 而是 CMD_REBOOT，所以**沒進 upgrade mode**，照常啟動進 application。PC 端的 OTA tool 看到 USB enumeration 變動就以為 BROM ready，開始送 updater.spl——對方是 application 不是 BROM/SPL，握手必然失敗在 “Switching to new stage”。

### Root cause：`aic_set_reboot_reason()` 沉默忽略後續寫入

`bsp/artinchip/hal/wri/hal_wri.c:73-87`：

```c
void aic_set_reboot_reason(enum aic_reboot_reason r)
{
    u32 cur = 0;
    cur = GET_REG_STATUS(REG_BOOT_INFO);
    /* If it's valid already, so ignore the current request */
    if (cur & WRI_REBOOT_REASON_MASK)
        return;                  // <── 已有 reason 就 silent return

    BOOT_INFO_SET(r, WRI_REBOOT_REASON_MASK, WRI_REBOOT_REASON_SHIFT, cur);
    ...
}
```

也就是說：**第一個寫進去的 reason 鎖死整個 boot 期，之後沒人能覆寫**。

對應上面的 boot log，**這次** boot 顯示的 reason `CMD_REBOOT` 來自**上一次** application 跑期間有人寫了 `REBOOT_REASON_CMD_REBOOT`。在這次 boot 早期，`main.c` 的：

```c:application/baremetal/helloworld/main.c
aic_get_reboot_reason();       // 印 "Reset action: ..., reason: Command-Reboot"
aic_clr_reboot_reason();       // 清掉 register
```

把 register 清掉了。**但是** application 跑了一陣子之後，**又**有路徑寫了 CMD_REBOOT：

| 寫 CMD_REBOOT 的程式碼路徑 | 何時觸發 |
|---|---|
| `application/baremetal/helloworld/cmd/reset.c:28` | shell 打 `reset` 命令 |
| `kernel/common/cmd/aic_reboot_cmd.c:81` (`rt_hw_cpu_reset`) | shell 打 `reboot`，或某個 panic/assert handler 呼叫 `rt_hw_cpu_reset()` |

雖然我們沒主動打這些 shell 命令，但 RT-Thread 內部某個 path（assert、stack overflow check fallback、IRQ handler 異常路徑等）可能呼叫到 `rt_hw_cpu_reset()`，**沉默地把 reason 鎖成 CMD_REBOOT**。

接著我們從 UI 送 BROM trigger，`custom_trigger_brom_mode` 呼叫：

```c
aic_set_reboot_reason(REBOOT_REASON_UPGRADE);
```

**被 first-writer-wins 規則 ignore，UPGRADE 沒進 register**。WDT timeout → reset → BROM 讀 reason → 看到 CMD_REBOOT → 「這是普通 reboot，不用進 upgrade mode」→ 進 application。

PC 端 OTA tool 不知道 device 沒進 BROM mode，看到 USB enumerate 就開始送圖檔→「Switching to new stage」階段 device 不是 SPL 而是 application，握手失敗→ 顯示「No device is connected」。

### 為什麼「手動按 BROM 鍵 + cold boot」可以？

走的是 **cold boot 路徑**，**完全沒經過 application**：
- 硬體 reset pin / BROM 按鍵讓 SoC 上電後 BROM 直接進 upgrade mode
- 不去讀 boot info reason
- `aic_set_reboot_reason()` 完全沒被執行
- 不會有 first-writer-wins 衝突

正好對比出「UI 觸發路徑被 reason register 卡住、cold boot 路徑乾淨」這個現象。

### 修法：強制清空再寫

`custom_trigger_brom_mode()` 改成：

```c
#ifdef AIC_WRI_DRV
    /* Force-clear any prior reboot reason before writing UPGRADE.
     * Otherwise the first-writer-wins logic in
     * bsp/artinchip/hal/wri/hal_wri.c:73-87 will silently drop our
     * UPGRADE write and BROM will boot back into application instead
     * of entering aicupg mode. */
    aic_clr_reboot_reason();
    aic_set_reboot_reason(REBOOT_REASON_UPGRADE);
#endif
```

`aic_clr_reboot_reason()` 是無條件清除（不像 set 有 check），所以**保證**接下來的 set 一定生效。代價是零——本來我們也不在乎舊的 reason 是什麼，反正我們是要進 upgrade mode 不是要看歷史。

### 為什麼以前可以、現在不行？

這需要 git diff 看 SDK 改了什麼，但兩個方向的推測：

1. **以前 application path 沒有寫過 CMD_REBOOT 的觸發點** — 後來加了某個 panic / assert / WDT pre-handler，會走 `rt_hw_cpu_reset()`。
2. **以前 `aic_set_reboot_reason()` 不是 first-writer-wins** — 後來 BSP 加上了那個 `if (cur & MASK) return;` 保護（可能是為了某個 race condition 修法），導致後續寫入失效。

不管是哪一個，**force-clear 都是 robust fix**，因為它讓 BROM trigger 的呼叫端**不依賴**全局 register 狀態。

### 一句話結論（v1，後續被 v2 推翻）

> `aic_set_reboot_reason()` 是 **first-writer-wins**。如果 application 跑期間有人先寫了 `CMD_REBOOT`（panic / assert / RT-Thread 內部某條路徑），UI 觸發的 `UPGRADE` 寫入會被 silent drop。**修法是先 `aic_clr_reboot_reason()` 再 set**，讓 BROM trigger 不依賴前一個 reason 狀態。

### v1 修法上線後 OTA 依然 2.3% 失敗——真正的 root cause

加了 `aic_clr_reboot_reason()` 之後再測，OTA tool log 確認了**幾個關鍵變化**：

| 觀察點 | v1 修法前 | v1 修法後 |
|---|---|---|
| OTA tool 「BROM: 已偵測」 | ❌（看不到 device）| ✅ 偵測到 bus:port |
| `Upgrade fwc: image.updater.ddr` (30480 byte) | 沒走到 | ✅ 送完 |
| `Upgrade fwc: image.updater.spl` (238608 byte) | 沒走到 | ✅ 送完 |
| 失敗位置 | 完全進不去 | **2.3% — Switching to new stage** |

**v1 修法只解了「reason 被 first-writer-wins 蓋掉」的問題**——BROM 現在確實會看到 `UPGRADE` reason 並進 upgrade mode。但 SPL 移交那一步依然死。

對照 §6.17 一開頭「手動按 BROM 鍵 cold boot OTA 可以成功」這個對比實驗，唯一變因是 **DDR 在 WDT reset 後仍是 active state**：

```
COLD BOOT path (手動按 BROM 鍵)：
  電源 OFF → 電源 ON → DDR 完全冷 → BROM 收 updater.ddr 從零 init
                                  → BROM 收 updater.spl 載入熱 DDR
                                  → BROM 跳 SPL → SPL 接手 USB ✅

WARM RESET path (UI 觸發 + REBOOT_REASON_UPGRADE)：
  App 跑了 15 秒 → DDR 已被 LVGL/GE2D 用過、cache dirty
                → WDT reset 只 reset CPU，**不 reset DDR controller**
                → BROM 收 updater.ddr 想再 init active DDR → 結果未定義
                → BROM 收 updater.spl 載入「狀態錯亂」的 DDR
                → BROM 跳 SPL → SPL 一開始就跑壞掉的 code → 沒回 USB CSW ❌
```

換句話說：**`REBOOT_REASON_UPGRADE` 設計上只給 cold boot 用**，但我們用在 warm reset 路徑，**根本性錯誤**。

### v2 修法：用 `REBOOT_REASON_BL_UPGRADE`，讓 SPL 直接接手

看 `bsp/common/include/aic_reboot_reason.h:17-18` 的官方註解：

```c
REBOOT_REASON_UPGRADE,          /* Goto BROM upgrade mode  */     ← cold boot only
REBOOT_REASON_BL_UPGRADE,       /* Goto Bootloader upgrade mode */ ← application warm reset
```

對照 SDK 官方的 application → upgrade mode reference (`application/baremetal/helloworld/cmd/reset.c:40-54`)：

```c
static int cmd_aicupg(int argc, char **argv)
{
    if ((argc == 2) && !strcmp(argv[1], "gotobl"))
        aic_set_reboot_reason(REBOOT_REASON_BL_UPGRADE);     // ← Application → bootloader upgrade
    else if ((argc == 2) && !strcmp(argv[1], "hid"))
        aic_set_reboot_reason(REBOOT_REASON_BL_HID_UPGRADE); // ← Application → bootloader HID upgrade
    else
        aic_set_reboot_reason(REBOOT_REASON_UPGRADE);        // ← 這個 fallback 是給 console 手動逼 BROM 用
}
```

**官方都是用 `BL_UPGRADE`**。對應的 SPL 端在 `application/baremetal/bootloader/cmd/aicupg_detect.c:241-255` 處理：

```c
if (r == REBOOT_REASON_BL_UPGRADE) {
    /*
     * User run command "aicupg gotobl" in Application, to enter upgmode of
     * BootLoader
     */
    if (usbd_connect_pc_check()) {
        upg_type = UPG_TYPE_USB;
        upg_mode = UPG_MODE_NORMAL;
    } else {
        upg_type = UPG_TYPE_UART;
        upg_mode = UPG_MODE_NORMAL;
    }
    aic_clr_reboot_reason();
    return 0;
}
```

也就是說，`BL_UPGRADE` reason 的完整 chain 是：

```
App: aic_clr_reboot_reason()
   → aic_set_reboot_reason(REBOOT_REASON_BL_UPGRADE)
   → WDT reset
        ↓
BROM: 看到不是 UPGRADE → 正常從 NOR flash boot target.spl
        ↓
SPL (跑在熱 DDR 上、沒有 re-init DDR): 
   → 讀 reason = BL_UPGRADE
   → 進 upgrade mode、自己接管 USB device controller
        ↓
PC OTA tool: 看到 SPL 的 USB endpoint
   → 直接傳 `image.target.*` （OS / rodata / data）給 SPL
   → SPL 寫到 NOR flash
   → 完成
```

**完全跳過了**「BROM re-init DDR」這個有問題的步驟，因為 SPL 是從 NOR flash boot 起來的、跑在原本就 active 的 DDR 上。

### `custom.c` v2 修法

```c
#ifdef AIC_WRI_DRV
    aic_clr_reboot_reason();
    aic_set_reboot_reason(REBOOT_REASON_BL_UPGRADE);   // ← was REBOOT_REASON_UPGRADE
#endif
```

只改一個 enum value、其他完全不動。`aic_clr_reboot_reason()` 的 force-clear 還是要保留——避免如果 application 跑期間別人先寫了 reason 導致 first-writer-wins drop。

### 真正修好的證據（v2 觀察的 boot log 預期）

修對之後，**下一次 WDT reset 後** UART log 應該看到：

```
(WDT reset)
(BROM 載 SPL 從 NOR flash)
(SPL banner)
SPL: detect BL_UPGRADE, enter upgrade mode
SPL: usb device init
(PC OTA tool 偵測到 device — descriptor 是 SPL upgrade endpoint)
(PC 開始送 image.target.os, image.target.rodata, image.target.data ...)
(SPL 寫 NOR flash)
(燒錄完成、SPL reset)
(新 firmware boot)
```

關鍵差別：**完全不會看到 PC 送 `image.updater.ddr` 或 `image.updater.spl`**——因為 SPL 已經 active，不需要 BROM 從頭 init DDR + load SPL。

如果 PC OTA tool 還是去送 `image.updater.*` (大部分舊版 tool 是 hardcoded 走 BROM 流程)，就要在 tool 端調整：選「Application → Upgrade mode」流程而不是「BROM mode」。

### v2 一句話結論

> `REBOOT_REASON_UPGRADE` 是給 **BROM cold boot** 用的；從 application warm reset 過去要用 `REBOOT_REASON_BL_UPGRADE`，這樣 SPL 會從 NOR flash 正常 boot 起來、在熱 DDR 上接手 USB，**完全跳過 BROM 對 active DDR 做 re-init 這個會出問題的步驟**。SDK 官方 reference (`application/baremetal/helloworld/cmd/reset.c:cmd_aicupg`) 也是這個寫法。組合 `aic_clr_reboot_reason()` + `aic_set_reboot_reason(BL_UPGRADE)` 就是 application 觸發 OTA 的標準寫法。

### 實機驗證（2026-06-04 14:30 原廠燒錄工具 log）

修法後第一次成功 OTA。原廠工具 `aicupg_trans_send_pkt_inner` 的 verbose log 顯示完整的 CBW/CSW handshake 來回：

```
aicupg_trans_send_pkt_inner()184:    Send CBW, tag 0x457
aicupg_trans_send_pkt_inner()193:    Send data, len 16
aicupg_trans_send_pkt_inner()203:    Read CSW, tag 0x457   -> ok    ← SPL 真的有回應
aicupg_trans_send_pkt_inner()184:    Send CBW, tag 0x458
aicupg_trans_send_pkt_inner()193:    Send data, len 2048
aicupg_trans_send_pkt_inner()203:    Read CSW, tag 0x458   -> ok
…（連續幾百個 CBW/CSW round-trip 全部 ok）
```

對比修法前同一階段的 log：

```
Send the rest data 238608
aicupg_trans_recv_pkt()322:    no CSW. (rc = 0)              ← 對方完全沒回
aicupg_cmd_send_fwc_data_final()726:    no resp
Dev "1:5-2-2-1" lost!
```

差異一目瞭然——修對 reboot reason 之後，SPL 真的接手 USB 並正確回應 CBW/CSW。

### 完整修復鏈（總覽）

這個「UI 觸發 OTA」功能能跑通，需要 §6.16 + §6.17 兩個獨立 bug 都被修掉。這兩層 bug 是**疊加**的，前一層擋住的時候根本沒辦法測到下一層：

| 層 | 症狀 | Root cause | 修法 | 章節 |
|---|---|---|---|---|
| 1 | 開機就死、USB 看不到 | Canary v1 寫入覆蓋 RT-Thread `'#'` magic byte → scheduler disable interrupt 永久 spin | `lv_stack_canary_install` 改成只 rename thread、不寫 stack | §6.16 |
| 2 | BROM trigger 後走正常 boot、OTA 偵測不到 | `aic_set_reboot_reason()` first-writer-wins 被先寫的 `CMD_REBOOT` 卡住 | 加 `aic_clr_reboot_reason()` force-clear 再 set | §6.17 v1 |
| 3 | 進 upgrade mode、updater.spl 送完但 SPL handoff 死在 2.3% | 用錯 enum：`REBOOT_REASON_UPGRADE` 是給 cold boot；warm reset 應該用 `BL_UPGRADE` | 改 `REBOOT_REASON_BL_UPGRADE` | §6.17 v2 |

每一層必須全部修對，才會看到下一層的症狀。這也是「為什麼以前一直停在 2.3%」的真實原因——層 3 才是 root cause，但因為層 2 還沒打通根本看不到。

---

## 6.18. 4 小時 streaming 後完全 hang、UART 印一半截斷 → HW WDT 自動恢復

| 項目 | 內容 |
|---|---|
| 觀察時間 | 2026-06-04 持續 streaming 4 小時 13 分後 |
| 規模 | cap=458288, enc=458287, sent=458050, drop=232（4h13m 約 458k frame, 30.5 fps 平均）|
| 症狀（PC 端） | bulk write 開始 IoTimedOut → UI 還能看到 device 連線但所有指令送出失敗 |
| 症狀（device 端 UART） | 最後一行印到一半：`custom_area4: perf stats: avg pickup=10 show=0 in` 後完全靜默 |
| 之前的所有偵測機制 | ❌ 都沒救到：canary 沒 breach 報告、freeze_watchdog 也死、heartbeat 停 |
| 修復檔案 | `lv_port_disp.c::freeze_watchdog_thread + freeze_watchdog_hw_wdt_*` |
| 修復策略 | **HW WDT 餵狗**：放棄「偵測失敗 → 印 log」, 改成「偵測失敗 → 板級 reset」|

### 為什麼之前的 canary / heartbeat 全部失效

20260602-001.txt log file 的最後三行是最重要的證據：

```text
33897|custom_area4: perf stats: 30 in 980ms fps=30.61 (intv=31.25)
33898|custom_area4: perf stats: avg pickup=10 show=0 in
33899|
33900|
```

第 33897 行（單行內第一句 perf stats）**完整印完**，但第 33898 行（單行內第二句）只印到 `in` 就斷了，預期應該繼續 `intv=32 render=3 ...`。**沒有**:
- ❌ `thread:XXX stack overflow`（RT-Thread overflow detector）
- ❌ `CPU Exception NO.X` + `mepc=0xdeadbeee` dump
- ❌ `[canary]` breach 報告
- ❌ `[WATCHDOG] !!! DISPLAY STALL`
- ❌ `[heartbeat]` 後續任何訊息

這個「mid-print 截斷 + 完全靜默」的特徵 = **CPU 在某條 thread 進了 `rt_hw_interrupt_disable() + while(level)` 永久 spin**，UART tx 中斷被禁用所以 FIFO 裡剩的 char 永遠不會送出。但**沒看到 stack overflow 訊息**代表：

可能性 A：罪犯 thread 我們**沒**裝 canary rename（LVGL 主 thread / MPP decoder / USB stack / DMA / idle / timer 都沒裝）。RT-Thread 的內建 detector 會印 `thread:LVGL stack overflow`，但因為 UART 已經卡住所以那行訊息被截掉了。

可能性 B：不是 stack overflow，而是某個 spinlock 死鎖 / DDR corruption 跳到無效 PC / 硬體 bus deadlock。

不論 A 還是 B：**靠軟體 log 已經救不回來**，必須靠硬體 reset。

### USB descriptor 還能看到的解釋

| 層 | 狀態 | 原因 |
|---|---|---|
| USB device controller 硬體 | ✅ 活著 | enumeration / descriptor read 由 USB controller 硬體自動處理，跟 CPU 是否 spin 無關 |
| Bulk OUT 接收 firmware queue | ❌ 死掉 | 需要 CPU thread 來 dequeue + 處理，CPU spin 後 host 看到 NAK → libusb IoTimedOut |
| UART output | ❌ 死掉 | 需要中斷服務 tx，interrupt disable 後 FIFO 不再 drain |
| LVGL render | ❌ 死掉 | CPU spin 所有 thread 都不跑 |

所以 UI 工具看到 device 還在 connected（descriptor），但 bulk write 全部 timeout（firmware 不在 receive）。這個半死狀態就是「UI 看得到、指令送不進去」的根本原因。

### Root cause（未確認，但不再重要）

可能是：
- LVGL 主 thread（48KB stack）跑某個 deep render chain 時 overflow
- CherryUSB stack 某個 callback overflow（沒 instrument）
- MPP JPEG decoder 內部 thread overflow（沒 instrument）
- DDR 在連續 4 小時高負載下出現 bit-flip / refresh 漏掉
- GE2D 某個未捕獲的 race condition 把 LVGL render lock 永久持住

每一個都需要單獨的 instrumentation 才能 confirm，**而且 confirm 之後也只是知道哪條死、不會自動修好**。所以我們改成「不管哪條死、自動重啟」。

### 修復實作

1. **HW WDT 持續餵狗**

   `lv_port_disp.c::freeze_watchdog_thread` 啟動時：
   - 印**上一次** boot 的 `aic_get_reboot_reason()`（讓 WDT 觸發顯而易見）
   - 印**baseline** thread stack usage 高水位
   - `rt_device_find("wdt") + rt_device_init + RT_DEVICE_CTRL_WDT_SET_TIMEOUT=10s + START`

   每個 tick（每 1 秒）：
   - 第一件事就 kick `RT_DEVICE_CTRL_WDT_KEEPALIVE`（**最前面**, 避免後續邏輯有 bug 導致 kick 不到）
   - 然後做原本的 heartbeat + stall 偵測
   - 每 60 tick（每 60 秒）印一次 thread stack 高水位

2. **timeout 為什麼選 10 秒**

   - freeze_watchdog tick = 1 秒, 10 秒 = **10 次 tick 餘裕**
   - LVGL 最壞情況下大 PNG 解碼 + GE2D fallback 也不會佔 CPU 連續 10 秒, 不會 starve freeze_watchdog → 不會誤觸發
   - 真的 spin 時 10 秒內板子自動 reset, hostside 再重連即可繼續用
   - 比 BROM trigger 用的 1 秒 timeout 長 10 倍, 安全空間夠

3. **與 BROM trigger 共用 WDT device 的注意點**

   `custom_trigger_brom_mode` 和 `freeze_watchdog_hw_wdt_init` 都用同一個 `rt_device_find("wdt")`。但兩者時間點互斥：BROM trigger 設超短 timeout **故意讓 WDT 觸發**進 BROM 升級模式，觸發後系統立刻 reboot，不會再回到 freeze_watchdog tick。所以不會打架。

4. **HW_LOCKUP reboot reason 不會誤入 BROM**

   WDT 觸發的 reset 會在 WRI register 留 `REBOOT_REASON_HW_LOCKUP`（代碼 11）。bootloader 的 `aicupg_detect.c` 只對 `REBOOT_REASON_UPGRADE` / `BL_UPGRADE` / `BL_HID_UPGRADE` 進升級模式, 對 HW_LOCKUP 是走**正常 boot**。所以 4 小時 hang → WDT reset → device 自動恢復 streaming, 不會莫名變成 OTA 模式。

### 新的 log signature

每次 boot 在第一個 [heartbeat] 之前會看到（如果上次是 WDT 觸發）：

```text
[watchdog] previous reboot reason = 11 (HW_LOCKUP (WDT))
[watchdog] !!! last boot was triggered by HW WDT — system was in
                silent-hang state and the watchdog auto-recovered it.
                Check the streaming log captured before this boot to
                identify the offending thread.
[stack] === thread stack high-water-mark (n=N, every 60s) ===
[stack]    LVGL     used=42% (20480/49152 bytes, 28672 free)
[stack]    aic_disp used=18% (1474/8192 bytes, 6718 free)
[stack]    ...
[watchdog] HW WDT armed: timeout=10s, kicked every 1000ms — any silent
                CPU hang will auto-reset the board within 10s
```

收到 log 後：
- 看 `previous reboot reason` 就知道**上次**為什麼死（如果是 HW_LOCKUP 表示我們又救了一次）
- 看 `[stack] !!` 標記（>=70% 用量）找下一個 likely 嫌犯, 提早調 stack size
- heartbeat 還是每 5 秒一次, recv_idle / show_idle 仍可看 streaming 是不是健康
- 60 秒一次的 stack snapshot 可在離線分析時對齊 timestamp, 看是否有 thread stack 用量持續上升

### 設計取捨

| 取捨 | 選擇 | 為什麼 |
|---|---|---|
| WDT timeout | 10 秒 | freeze_watchdog tick 是 1 秒, 10x 安全餘裕; 真的 hang 也 10 秒內復原 |
| stack scan 頻率 | 60 秒 | 5 秒 heartbeat 已經多訊息了; stack 漂移很慢, 60 秒夠細 |
| stack scan output 過濾 | >=70% 加 `!!` 標記 | 全部都印免得漏看, 但用 `!!` 標出真正要注意的 |
| kick 順序 | tick 開頭**最先** kick | 後面 heartbeat / stall 邏輯如果有 bug 也不會擋到 kick |
| WDT init 位置 | freeze_watchdog 內部 (3 秒 settle 之後) | 避免 boot 還沒穩前 WDT 提早觸發 |
| 不嘗試 software recovery | 直接 board reset | 軟體 recovery 在 spin 狀態根本沒機會跑; 板級 reset 是唯一保證能脫困的方法 |

### 仍未解決的後續工作（不影響穩定性）

- root cause 哪一條 thread 死掉還是未知, 等 stack snapshot 連續跑出 trend 後再判斷
- 沒裝 canary rename 的 thread（LVGL 主 / MPP / USB stack）若未來確認有 overflow, 可以在 `LV_STACK_CANARY_MAX = 8` 額度內加 instrument
- 60 秒 stack snapshot 之外, 未來如果還需要更高頻可以加 60 秒平滑或 trend detect

---

## 7. 未來如果還要再優化記憶體

按「**改動量小 → 大**」排序：

### A) 縮小 LV_CACHE_DEF_SIZE — ✅ 已實作（8 MB → 2 MB）

見 §6 第 2 層。

### B) PC 端降到 25 fps 推送（PC change，device 不動）

目前 PC ~55 fps 推，device only render ~25-32 fps，drop% 高達 46%。如果 PC 主動降到 25 fps，整條 pipeline 都鬆，連帶：
- CMA churn 鏟到 25 次 alloc/free per second
- USB bus 利用率降一半（其他 USB endpoint 受益）
- device CPU loading 降

### C) 把 LVGL bypass，自管 MPP decoder + cf=ARGB8888 widget path（重 refactor）

詳見 transcript 之前討論。需 ~200 行新 code。**徹底排除每 frame alloc/free**：
- 開機一次性 alloc 4 MB ARGB CMA buffer
- 持續再用同一個 MPP decoder instance
- LVGL 走 bin_decoder use_directly path，零 decoder/cache 互動

**預期**：fps 推到 30+ 穩定。但這是大手術，目前 back-pressure 已穩定就不急著做。

### D) `LV_CACHE_DEF_SIZE = 0`（完全關掉 LVGL image cache）

**好處**：完全砍掉 evict path，雪崩 chain 永遠斷
**風險**：所有靜態 PNG 都要每次 decode，UI 流暢度可能明顯下降。需逐區驗證

---

## 8. 排查 checklist（之後再遇 crash 時的快速 triage）

| 觀察 | 可能對應 |
|---|---|
| 開機沒看到 `custom_image: CMA preallocated recv=0x... ready=0x...` | CMA prealloc 失敗（極端罕見）→ 檢查 `CONFIG_AIC_DRAM_CMA_SIZE` 是否被改小 |
| 開機看到 `[ERROR] CMA prealloc FAILED` | 同上，image RX 已 disable，無法接圖 |
| `back-pressure drop count=N` 持續累加但無 crash | **正常** — back-pressure 正確 fire，PC 推太快 |
| `back-pressure drop count` 完全沒出現過卻 crash | back-pressure 邏輯沒生效（檢查 `s_perf_meas_pending` 是否被別處改）|
| `[Warn] lv_cache_entry_dec_ref: ref_cnt(<huge>) < 0` | LVGL cache 被破壞 → 八成又有人在跨 heap 動 cache，或 dcache flush 順序出問題 |
| `CPU Exception: NO.4 / NO.6`, `mtval` 看起來像圖檔 byte pattern | heap corruption → 找剛 alloc/free 的 buffer 是不是越界 |
| `CPU Exception: NO.6`, `a0` ≈ `0x40fff???` (system heap 末端) | **本次的 NO.6 重現**：表示哪裡又恢復了 system heap 動態 alloc/free（檢查是不是有 patch 走回頭路）|
| `alloc frame buffer failed` (drv_fb) | CMA 真的滿到開機階段都配不出 FB → 檢查是不是其他模組吃過頭 |
| `mpp_phy_alloc failed` | CMA exhaustion → MPP packet/frame 配不到 |
| `realloc` 回 NULL | system heap 碎片化 → 確認沒有人加新的 hot-path realloc |
| device 永遠 freeze, 完全沒新 frame，無 crash log | back-pressure 200ms timeout fallback 沒有 fire / draw_done_cb 完全沒被觸發 → 檢查 lv_obj 是不是被 hide |

---

## 9. 相關檔案索引

| 角色 | 檔案 |
|---|---|
| DDR 區段定義 | `target/d21x/d215-demo88-nor-dacai03/board.c` (`aic_memheap_init`)<br>`output/d21x_d215-demo88-nor-dacai03_rt-thread_helloworld/images/d21x.map` (linker symbols) |
| Heap 設定 | `.config` (`CONFIG_AIC_DRAM_TOTAL_SIZE`, `CONFIG_AIC_DRAM_CMA_SIZE`, `CONFIG_AIC_DRAM_CMA_EN`) |
| try_cma fallback | `kernel/common/osal/aic_osal.c:86` `aicos_malloc_try_cma()` |
| LVGL cache 設定 | `packages/artinchip/lvgl-ui/lvgl_v9/lv_conf.h:163` `LV_CACHE_DEF_SIZE` |
| LVGL cache evict | `packages/artinchip/lvgl-ui/lvgl_v9/lvgl/src/misc/cache/lv_cache.c:167` `lv_cache_evict_one`<br>`packages/artinchip/lvgl-ui/lvgl_v9/lv_drivers/lv_mpp_dec/lv_mpp_dec.c:799` `lv_drop_one_cached_image` |
| MPP frame alloc | `packages/artinchip/lvgl-ui/lvgl_v9/lv_drivers/lv_mpp_dec/lv_mpp_dec.c:452` `lv_frame_buf_alloc()` |
| MPP ext allocator | `packages/artinchip/lvgl-ui/lvgl_v9/lv_drivers/lv_mpp_dec/lv_mpp_dec.c:426` `lv_open_allocator()` |
| FB 配置 | `bsp/artinchip/drv/display/drv_fb.c:618` `aicfb_probe()` |
| Area4 prealloc + back-pressure | `packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom_Area4.c` |
| Area3 動態 buffer | `packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom_Area3.c` |
| Image recv pipeline + **CMA prealloc** | `packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom_Image.c`<br>關鍵函式：`custom_image_init()`（line ~560, CMA prealloc）、`custom_image_prepare_buffer / ensure_capacity / ensure_ready_capacity`（純大小檢查）、`custom_image_release_ready / release_payload`（只清狀態，不 free） |
| Panel timing | `bsp/artinchip/drv/display/panel/panel_dsi_jd9365.c` |

---

## 附錄：常用尺寸換算（800 × 800 panel）

| Format | bpp | per-frame bytes |
|---|---|---|
| ARGB8888 / XRGB8888 | 32 | 800 × 800 × 4 = **2,560,000** ≈ **2.44 MB** |
| RGB888 | 24 | 800 × 800 × 3 = 1,920,000 ≈ 1.83 MB |
| RGB565 | 16 | 800 × 800 × 2 = 1,280,000 ≈ 1.22 MB |
| YUV420 (NV12) | 12 | 800 × 800 × 1.5 = 960,000 ≈ 0.92 MB |

如果 panel 改成 1280 × 800（如某些舊 FT8201 配置）：

| Format | bpp | per-frame bytes |
|---|---|---|
| ARGB8888 | 32 | 1280 × 800 × 4 = 4,096,000 ≈ **3.91 MB** |
| YUV420 (NV12) | 12 | 1280 × 800 × 1.5 = 1,536,000 ≈ 1.46 MB |

> Area4 prealloc 是 **2 MB**，剛好可以放下「典型 1280×800 PNG 壓縮後 (~500 KB ~ 1.5 MB)」或「typical JPEG q=60 (~50-100 KB)」的 byte stream，**而不是放下解碼後的 raw pixel**。

---

---

## 10. 2026-06-06 ~ 2026-06-08：GE2D 卡死 + 看門狗自殺 + 持久化 WDT 紀錄

> 本章記錄三層復原機制，徹底解決「device 跑久了會凍結 / 完全死當 / HW WDT 也救不回來」的根因。
> 目標：絕對零死當 + 30+ FPS 穩定串流 1280×800 JPEG。

### 10.1 觀察到的兩種失效模式

#### 模式 A：GE2D 超時但仍會死當（2026-06-05 log）

```
[E] ge_start_and_wait()200 ge timeout
[E] dump_regs() ctrl=0000000b status=00000020 start=00000004 to_cyc=00000000
...
Reset action: Watchdog-Reset, reason: Unknown(0)
```

特徵：
- `status=0x20` → `GE_HW_BUSY` 一直 asserted
- `start=0x4` → `GE_SW_RESET` (BIT(2)) 一直卡住沒清
- `to_cyc=0` → HW timeout 沒啟用，靠軟體 2 秒 timeout
- 雖然每次都會印 `ge timeout`，但 `ge_reset()` 沒辦法把 GE 救回來，最後等 SoC HW WDT 10 秒超時 reboot

#### 模式 B：完全沉默死當（2026-06-06 log，user 必須拔電才能救）

```
[heartbeat] t=14517911ms recv=549253 show=185731 ... show_idle=8623757ms
[WATCHDOG] !!! DISPLAY STALL — recv advancing but show frozen (idle=8623767ms)
... (連續 2.4 小時這個訊息)
custom_ui: BROM trigger frame received, entering upgrade mode  ← 使用者手動觸發
```

特徵：
- **沒有** `ge timeout` 訊息（GE 驅動沒檢測到失敗）
- `show_idle=8625125ms`（~2.4 小時）→ 顯示完全凍結
- HW WDT **從未觸發**！為什麼？因為 `freezewdg` thread 自己還活著、每秒都餵狗
- `recv` 持續推進（USB 還在收，PC 還在送）
- `ge2d_render` / `aic_display` 兩條 thread 的 stack high-water-mark 完全沒變 → 卡在某個 wait

### 10.2 根因分析

#### 模式 A 根因（GE2D 驅動）— `bsp/artinchip/hal/ge/hal_ge_normal.c`

原本的 `ge_reset()` 只做一件事：

```c
static void ge_reset(struct aic_ge_data *data)
{
    hal_reset_assert(RESET_GE);
    hal_reset_deassert(RESET_GE);
}
```

但 `ge_start_and_wait()` 超時時長這樣：

```c
ge_clear_status(GE_BASE, data->status);  // ← data->status == 0
if (data->status == 0) {
    hal_log_err("ge timeout\n");
    dump_regs();
    ge_disable_interrupt(GE_BASE);
    ge_reset(data);                      // ← 不夠
    return ret;
}
```

四個獨立的 bug 疊加：
1. **`ge_clear_status(base, 0)` 等於沒清**：`GE_STATUS` 是 W1C（write-1-to-clear），寫 0 沒任何效果。HW_BUSY 永遠卡住。
2. **`GE_START` 沒被清**：`ge_start()` 寫的是 `GE_SW_RESET | GE_START_EN`，HW 只會 auto-clear `GE_START_EN`，**不會** auto-clear `GE_SW_RESET`，所以 `start=0x4` 這個 bit 永遠卡住。
3. **IRQ 沒有 mask 就 reset**：reset 過程中如果 GE IRQ 突然 fire，會送一個 stale `HW_RUNNING_EVENT`，下次 `ge_start_and_wait()` 立刻假性回傳成功。
4. **時脈沒重啟**：某些封裝下 `RESET_GE` deassert 會 gate GE clock，下一個 register 寫入會打到沒時脈的 domain。

#### 模式 B 根因（看門狗策略）— `packages/artinchip/lvgl-ui/lvgl_v9/lv_drivers/lv_port_disp.c`

`freeze_watchdog_thread` 的舊註解寫得很明白：

```c
/*
 *  Why we DON'T auto-recover:
 *      Attempting to forcibly signal a stuck semaphore ... risks racing ...
 *      Detection is the win — the user reads the warning and can choose
 *      to restart or take other action.
 */
```

每 1 秒**無條件**呼叫 `freeze_watchdog_hw_wdt_kick()`：

```c
freeze_watchdog_hw_wdt_kick();   // 永遠都餵
```

於是顯示完全卡死、`ge2d_render` thread 停在某個 wait → `freezewdg` thread 還活著 → 永遠餵狗 → HW WDT 永遠不會超時 → 板子永遠不會重開。使用者只能手動觸發 BROM 升級模式才能救活。

### 10.3 解決方案（三層）

```
Layer 1: GE2D 驅動「完整復原」                    ← 多數情況下避免進入 stall
Layer 2: 看門狗「自殺」（停餵狗）                  ← 任何沉默死當的最終保險
Layer 3: 持久化 WDT 紀錄到 LOG_FILE/wdt_log.txt  ← 跨 reboot 累積觸發次數
```

#### Layer 1：`ge_reset_full()` 取代原 `ge_reset()`

**檔案**：`bsp/artinchip/hal/ge/hal_ge_normal.c`（替換 `ge_reset()` + 改寫 `ge_start_and_wait()`）

完整 9 步驟硬體復原序列：

| 步驟 | 動作 | 為什麼 |
|---|---|---|
| 1 | `aicos_local_irq_save(&irq_flag)` | 關 local IRQ，避免 RT-Thread scheduler 或 GE IRQ 在中途搶 |
| 2 | `writel(0, GE_BASE + GE_CTRL)` | 在控制器層面關 GE IRQ；reset 中途若 fire 會送 stale event |
| 3 | `writel(0, GE_BASE + GE_START)` | **顯式清除卡住的 `GE_SW_RESET` bit**（field log 觀察到的 `start=0x4`） |
| 4 | `writel(readl(STATUS), STATUS)` | 真正的 W1C（讀回再寫回）清掉所有 pending status bits |
| 5 | `hal_reset_assert(RESET_GE); hal_reset_deassert(RESET_GE);` | 軟體能下達的最深 reset |
| 6 | `aicos_udelay(10)` | 等 reset 透過 GE clock domain 傳遞完成 |
| 7 | `ge_clk_enable(data)` | 防禦性重新啟動 GE 時脈，避免 reset 把 clock gate 掉 |
| 8 | 二次 W1C 清 status | 某些 HW 版本在 reset clock edge 上會 re-assert status bits |
| 9 | 讀回 CTRL/STATUS/START 確認歸零，否則大聲 log | 給上層 caller 判斷是否要進 Layer 2 自殺路徑 |

`ge_start_and_wait()` 同步改寫：

```c
if (data->status == 0) {
    /* TIMEOUT 路徑：ISR 沒跑，data->status 一直是 0 */
    uint32_t live_status = ge_read_status(GE_BASE);  // ← 讀真實 HW 狀態
    hal_log_err("ge timeout (live_status=%08x consec=%u)\n", ...);
    dump_regs();
    ge_disable_interrupt(GE_BASE);
    ge_reset_full(data);                              // ← 用新復原函式

    s_ge_consec_fail++;
    if (s_ge_consec_fail == GE_CONSEC_FAIL_LOUD) {    // = 8
        hal_log_err("ge: %u consecutive timeouts ...");
    }
    return ret;
}
```

連續失敗計數 `s_ge_consec_fail`：
- 每次乾淨 FINISH IRQ 歸零
- 連續 8 次失敗時印一條明顯訊息，跟 Layer 2 的 `[WATCHDOG] DISPLAY STALL` 配合
- **不會** abort operation（保留任何自我復原的可能性，最後讓 Layer 2 接手）

#### Layer 2：看門狗自殺（停止餵狗強制重開機）

**檔案**：`packages/artinchip/lvgl-ui/lvgl_v9/lv_drivers/lv_port_disp.c`

新增常數：

```c
#define FREEZE_WATCHDOG_STALL_SUICIDE_TICKS  30U
```

新增 latch flag + 修改 kick 函式：

```c
static rt_bool_t s_hw_wdt_suicide;

static inline void freeze_watchdog_hw_wdt_kick(void)
{
    if (s_hw_wdt_suicide) {     // ← 一旦點亮就永不餵狗
        return;
    }
    if (s_hw_wdt_started && s_hw_wdt_dev) {
        rt_device_control(s_hw_wdt_dev, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
    }
}
```

新增 `freeze_watchdog_hw_wdt_arm_suicide()`：印一條明顯 banner，然後**先寫 LOG**，最後才點亮 latch。

stall 偵測分支內：

```c
if (recv_alive && show_stalled) {
    stall_warn_n++;

    if (stall_warn_n >= FREEZE_WATCHDOG_STALL_SUICIDE_TICKS) {
        freeze_watchdog_hw_wdt_arm_suicide(...);   // ← 30 秒後自殺
    }
}
```

時序：

```
t+0s    顯示卡死（show_idle 開始累積）
t+10s   show_idle > 10000ms → 第一次 [WATCHDOG] DISPLAY STALL
t+10..40s  每 10 秒重複一次警告（stall_warn_n 從 1 累積到 30）
t+40s   stall_warn_n == 30 → 寫一筆 wdt_log.txt + 印 SUICIDE ARMED + 停餵狗
t+50s   HW WDT 超時 → SoC 自動重開機
        下次 boot 印 "[BOOT] last reset reason: HW_LOCKUP" + 累積觸發次數
```

#### Layer 3：持久化 WDT 紀錄到 LOG_FILE

**需求**：`進入 WDT 一次,就在 .txt 檔增加 1 筆紀錄`，跨 reboot 累積。

**檔案**：`packages/artinchip/lvgl-ui/lvgl_v9/lv_drivers/lv_port_disp.c`（新增模組）

設計重點：

| 項目 | 設計 | 理由 |
|---|---|---|
| 檔案路徑 | `<mount>/LOG_FILE/wdt_log.txt` | `LOG_FILE` 子目錄符合使用者要求；分散到 sub-dir 避免 root 雜亂 |
| Mount fallback 順序 | `/sdcard` → `/udisk` → `/data` | SD 最方便取出；flash fallback 確保「沒卡也能寫」；`/ram` 不可用（reboot 不留） |
| 寫入模式 | `fopen("a")` append | 嚴禁覆寫，使用者明確要求累加 |
| 序號計算 | 開檔讀取，數非註解非空行的 line count + 1 | 避免要維護額外 metadata，純從檔案內容推導 |
| 自動建檔 | 第一次寫入時自動 `mkdir LOG_FILE` + 寫 header | header 註解讓檔案自我說明，給 30 年後看的人 |
| 寫入時機 | 在 `s_hw_wdt_suicide = TRUE` 之前寫 | latch 還沒點亮時下一個 tick 還會餵狗，給 FS 寫入 100ms 緩衝 |
| 失敗策略 | 三個 mount 都失敗就放棄這次紀錄 | 寫不進去也不能擋 reboot；reboot 是首要目的 |
| 開機讀取 | `freeze_watchdog_log_print_summary()` 在 boot 印累積觸發次數 | 一打開 console 就知道這台機器歷史上死了幾次 |

紀錄格式（`wdt_log.txt`）：

```
# wdt_log.txt - HW watchdog suicide event log
# Each line below records one freeze-watchdog SUICIDE arm; file is
# append-only and survives reboots. Format:
#   #<seq> t=<ms>ms show_idle=<ms>ms show=<count> rotate_fail=<n> sync_timeout=<n>
#1 t=14517911ms show_idle=8625125ms show=185731 rotate_fail=0 sync_timeout=0
#2 t=12340567ms show_idle=8500000ms show=132456 rotate_fail=0 sync_timeout=0
#3 ...
```

開機 banner 範例（如果歷史上發生過 N 次自殺）：

```
[watchdog] previous reboot: HW=4 (WDT (HW WATCHDOG !!)), SW=8 (HW_LOCKUP)
[watchdog] !!! last boot was triggered by HW WDT — system was in silent-hang state ...
[watchdog] persistent WDT log: 3 previous suicide event(s) recorded in /sdcard/LOG_FILE/wdt_log.txt
```

新加的 5 個函式（全 `static`，內部 helper）：

| 函式 | 職責 |
|---|---|
| `freeze_watchdog_log_build_path()` | snprintf 組合 `<base>/LOG_FILE/wdt_log.txt` |
| `freeze_watchdog_log_count_existing()` | 讀檔數非註解行數 = 累積觸發次數 |
| `freeze_watchdog_log_open_append()` | mkdir + 探測 + 開 append + 必要時寫 header |
| `freeze_watchdog_log_append_suicide()` | 對外主入口：嘗試所有 mount，寫入 1 行，回傳新序號 |
| `freeze_watchdog_log_print_summary()` | 開機讀取累積次數印到 UART |

### 10.4 復原時序（端到端）

#### 場景 1：偶發 GE2D timeout（多數情況）

```
[t+0ms]    某 frame 觸發 GE2D wedge
[t+2000ms] ge_start_and_wait() 超時
           → ge_reset_full() 完整復原（10 步驟，<100us）
           → consec=1
[t+2010ms] 下一個 frame 正常出，consec 歸零
           → 使用者完全無感（最多 1 frame drop）
```

#### 場景 2：GE2D 真的死透無法復原

```
[t+0s]    GE2D wedge
[t+2..16s] 連續 8 次 timeout，每次 2 秒
          → "ge: 8 consecutive timeouts" 訊息
[t+10s]   show_idle > 10s → [WATCHDOG] DISPLAY STALL（每 10s 一次）
[t+40s]   stall_warn_n == 30
          → 寫 wdt_log.txt（嘗試 /sdcard, /udisk, /data）
          → 印 [WATCHDOG] SUICIDE ARMED (#N) banner
          → s_hw_wdt_suicide = TRUE
[t+50s]   HW WDT 超時（10 秒沒餵）
          → SoC 自動 reset
          → boot 印 [BOOT] last reset reason: HW_LOCKUP
          → freezewdg 啟動印 "N previous suicide event(s) recorded"
          → 串流自動恢復
```

#### 場景 3：完全沉默死當（這次新發現的 mode）

```
[t+0s]    aic_display 或 ge2d_render thread 卡在某個 wait（沒 timeout 訊息）
[t+10s]   freezewdg 偵測 show_idle > 10s → [WATCHDOG] DISPLAY STALL
[t+40s]   寫 wdt_log.txt + 自殺
[t+50s]   HW WDT reset
          → 跟場景 2 一樣自動恢復
```

### 10.5 修改檔案總覽

| 檔案 | 修改內容 |
|---|---|
| `bsp/artinchip/hal/ge/hal_ge_normal.c` | 1) 移除舊 `ge_reset()`<br>2) 新增 `ge_reset_full()`（9 步驟硬體復原）<br>3) 改寫 `ge_start_and_wait()`：讀真實 status / 用 `ge_reset_full()` / 加 `s_ge_consec_fail` 計數 |
| `packages/artinchip/lvgl-ui/lvgl_v9/lv_drivers/lv_port_disp.c` | 1) `#include "hal_wri.h"`（修 `WRI_TYPE_*` 編譯錯）<br>2) `WRI_TYPE_ERR` 跟 `WRI_TYPE_POR` 合併 case（兩者數值相同）<br>3) `#include <stdio.h>` + `<dfs_posix.h>`<br>4) 新增 `s_hw_wdt_suicide` latch + `FREEZE_WATCHDOG_STALL_SUICIDE_TICKS=30`<br>5) `freeze_watchdog_hw_wdt_kick()` 加 latch 檢查<br>6) 新增 `freeze_watchdog_hw_wdt_arm_suicide()`（含 LOG 寫入 + banner）<br>7) stall 偵測分支加自殺觸發<br>8) 新增整個 LOG_FILE 模組（5 個 static helper + 3 個常數）<br>9) freeze_watchdog_thread 啟動時呼叫 `freeze_watchdog_log_print_summary()` |
| `packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/Note/DDR_圖片顯示記憶體使用說明.md` | 新增第 10 章（本章） |

### 10.6 部署檢查清單

部署這次修改後**第一次** boot 應該看到：

1. ✅ Build banner 時間戳要是新的（避免拿到舊 binary）
2. ✅ `[watchdog] HW WDT armed: timeout=10s, kicked every 1000ms ...`
3. ✅ 第一次跑沒有 `[watchdog] persistent WDT log:` 訊息（檔案還不存在，正常）
4. ✅ 串流期間若有偶發 GE timeout 應該看到 `ge timeout (live_status=... consec=1)` 然後立刻復原，使用者無感

如果**真的觸發**自殺路徑一次後 reboot：

1. ✅ 重開後 `[watchdog] persistent WDT log: 1 previous suicide event(s) recorded in /sdcard/LOG_FILE/wdt_log.txt`
2. ✅ SD 卡拔下來插電腦，可以看到 `LOG_FILE/wdt_log.txt`，內容類似：
   ```
   # wdt_log.txt - HW watchdog suicide event log
   ...
   #1 t=12345678ms show_idle=40123ms show=98765 rotate_fail=2 sync_timeout=0
   ```
3. ✅ 重開後串流自動繼續，不需任何人為介入

### 10.7 已知限制 / 未來改進

| 限制 | 影響 | 可能改善 |
|---|---|---|
| 自殺後 50 秒才 reboot（10s stall + 30s confirm + 10s WDT） | 使用者會看到約 1 分鐘黑畫面 | 把 `STALL_SUICIDE_TICKS` 降到 10 → 變 30 秒總時間，但會降低對短暫 hiccup 的容忍 |
| 三個 mount 都失敗時這次事件不會被記錄 | 罕見邊界情況 | 加一個 RAM 內 ring buffer 暫存，下次 mount 成功後 flush 出去 |
| `freeze_watchdog_log_count_existing()` 每次 boot 全文掃 | 紀錄檔成長到 100K 行才會明顯（>1MB / 50 ms） | 加 metadata file 存最後 seq 號 |
| 沒有時間戳（用 boot-relative `t=ms`） | 跨 boot 比對需要對應到 wall clock | 接 RTC 後可改成寫 ISO timestamp |
| Layer 1 對 CMDQ path 沒處理 | CMDQ 模式下若 wedge，仍走舊 `hal_ge_cmdq.c::ge_reset()` 邏輯 | 評估是否把同樣的 full-reset 邏輯移植到 CMDQ |

---

文件結束。如需更新或補充新的記憶體相關事件，請保留章節編號順序，並在 §5 或 §8 加 row。
