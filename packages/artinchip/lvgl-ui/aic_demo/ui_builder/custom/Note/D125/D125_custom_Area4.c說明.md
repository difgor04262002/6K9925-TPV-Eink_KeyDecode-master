# D125 `custom_Area4.c` 說明

> 適用平台：D125 / SDK 晶片名 **D12P**（`CONFIG_PRJ_CHIP="d12p"`）  
> 適用板卡：`demo88-nor`  
> 適用面板：ZC-T1D8BP-012（`CONFIG_AIC_PANEL_DBI_ZC_T1D8BP`，360 × 360 QSPI ST77916）  
> 源碼：`packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom_Area4.c`（約 1138 行）  
> 標頭：同目錄 `custom_Area4.h`  
> 方案 defconfig：`target/configs/d12p_demo88-nor_rt-thread_helloworld_zc_t1d8bp_defconfig`
>
> 本文件講**這支檔現在實際在做什麼**。圓屏（D125）的熱路徑是「把 USB JPEG 丟進 `custom_disp_stream`」；檔案後半的 LVGL `lv_image` widget 路徑是大屏／MIPI 的 fallback，本方案幾乎走不到。
>
> 姊妹檔：`D125_custom.c說明.md`（誰呼叫 Area4）、`D125_USB指令封包說明.md`（CD-BB／AA 位元組）、`custom_Image_流程說明.md`（誰收 CD-BB）、`custom_disp_stream.h`（真正解碼與 QSPI present）、`D125_QSPI配置說明.md`、`D125_記憶體配置說明.md`。

---

## 1. 這支檔是什麼

`custom_Area4.c` 負責 **screen_4_Area 的「顯圖」**，不是收圖、也不是 QSPI 驅動。

| 它做 | 它不做 |
|------|--------|
| 辨識 Area4 slot（`0x30`） | 解析 USB payload 本體（`custom_Image.c`） |
| 把 CD-BB `06` 命令對成 PNG／JPEG | 切 AA 畫面（`custom.c`） |
| 把 bitstream 交給 stream 或 LVGL image | 寫 ST77916 RAMWR（`lv_aic_spi` / `custom_disp_stream`） |
| 一幀 in-flight 的 back-pressure | 播 GIF（圓屏 GIF 在 `custom.c` + gifdec） |
| 給 freeze watchdog 的 show 計數 | 操作 PD.26 MMOSIE |

一句話：**PC 用 CD-BB 把一張靜態 JPEG／PNG 標成 Area4 之後，本檔決定「這張要不要上屏、走哪條顯示管線」。**

GIF／APNG **明確拒絕**。圓屏動畫 GIF 不經過 Area4。

---

## 2. 在系統裡的位置

```
USB CD-BB 6B + payload
        │
        ▼
custom_Image.c          收進 buffer、slot / type
        │ take_pending
        ▼
custom.c                custom_area4_is_slot? → 必要時 AA 06
        │
        ▼
custom_area4_show_image
        │
        ├─ D125 圓屏（幾乎一定）
        │     custom_disp_stream_acquire（若還沒）
        │     custom_disp_stream_can_show?
        │     custom_disp_stream_show  → jpgdec worker → lv_spi_present
        │
        └─ 大屏 fallback（stream 失敗才走；圓屏失敗則保持 QSPI、不走 widget）
              1 MB CMA memcpy → lv_image_set_src → DRAW_POST_END
```

`custom.c` 相關呼叫：

| 時機 | 函式 |
|------|------|
| `custom_init` | `custom_area4_init` |
| tick 顯圖 | `custom_area4_is_slot` → `custom_area4_show_image` |
| 無 AA 06 就開流 | 先 `custom_show_screen_by_cmd(0x06)`，並 `clear_render_pending` |
| AA 00／idle 回 splash | 等 `render_pending()==0` 再 `leave_display` |
| QSPI stall | `clear_render_pending`（GIF 播放中則 `custom.c` 直接 return） |

`custom_Image.c`：6-byte 命令命中 `custom_area4_try_parse_cmd` 時，slot 改成 `0x30`、type 改成 PNG 或 JPEG。

---

## 3. 公開 API（`custom_Area4.h`）

| 函式 | 回傳／作用 |
|------|------------|
| `custom_area4_init()` | 清狀態；CMA 預配 **1 MB** 顯示緩衝（widget 路徑用）。失敗只打 log，之後 PNG widget 顯示會 skip |
| `custom_area4_is_slot(slot)` | `slot == 0x30` 為 1 |
| `custom_area4_try_parse_cmd(frame, &slot, &type)` | 辨識 Area4 的 6-byte CD-BB；成功寫 `slot=0x30` 與 PNG／JPEG |
| `custom_area4_show_image(type, slot, data, size)` | 上屏。1 = 已交給顯示管線（圓屏含「已 queue 給 jpgdec」）；0 = 丟掉／稍後再試 |
| `custom_area4_render_pending()` | 1 = 還在等 DRAW_POST_END（widget 路徑）。離開 Area4 前必須輪詢 |
| `custom_area4_clear_render_pending()` | 強制清 pending（自動切屏、stall 復原） |
| `custom_area4_leave_display()` | `stream_release`；拿掉 DRAW_POST_END；**不** `set_src(NULL)`、不藏最後一幀 |
| `custom_area4_get_total_show_count()` | 單調計數，給 `lv_port_disp` freeze watchdog |
| `custom_area4_on_display_stall()` | 清 pending，之後 **1500 ms** 內 `show_image` 直接 return 0 |

常數：

```c
CUSTOM_IMAGE_SLOT_AREA4_1 = 0x30   /* 與 Area2 0x10、Area3 0x20 不衝突；後兩者已刪 */
```

標頭註解裡有一段舊協定（byte3=slot、byte4=PNG）**和實作不符**。以 `.c` 的 `try_parse_cmd` 為準，見下一節。

---

## 4. 協定：Area4 的 CD-BB

固定 6 byte：

```
CD  BB  06  <fmt>  00  CD
```

| byte | 值 | 意義 |
|------|-----|------|
| 0, 1, 5 | `CD BB … CD` | 影像命令框 |
| 2 | `0x06` | **Area4 區段**（不是 AA 的 0x06，但兩者常一起出現） |
| 3 | `0x00` JPEG ／ `0x01` PNG | **格式**，不是 slot |
| 4 | 必須 `0x00` | 其餘值整包不認 |

命中後：

- `*mapped_slot = 0x30`（永遠 AREA4_1，不管 byte3）
- `*mapped_type = JPEG 或 PNG`

之後 USB 送 bitstream，短包結束（JPEG）或 IEND（PNG）由 Image 模組組完整張，tick 再 `show_image`。

對照 **main_1** 的 CD-BB：`CD BB 02 <type> <slot 01..03> CD`。Area4 用 byte2=`06` 這條特例，不要跟 slot `0x01` 搞混。

PC 常常**只送 CD-BB、不送 AA 06**。`custom.c` 會自動切 `screen_4_Area`；本檔圓屏還會在 `show_image` 裡若 stream 未 active 就 `custom_disp_stream_acquire()`。

---

## 5. `custom_area4_show_image` 流程

入口條件：slot 必須 `0x30`；type 只能 PNG／JPEG；`data/size` 非空。否則 `show skip` log 並 return 0。

之後依序：

### 5.1 圓屏：確保 stream 已 acquire

沒開過 `custom_disp_stream` 也先 acquire。這樣「沒 AA 06」的第一張 JPEG 仍能走 QSPI。

### 5.2 stall 冷卻

`custom_area4_on_display_stall` 設的 `s_stall_drop_until_ms` 未到 → return 0（payload 仍在 Image 裡，下一 tick 再試）。

### 5.3 大屏才有的兩個閘（圓屏編譯掉）

| 閘 | 值 | 用意 |
|----|-----|------|
| 最小間隔 | `CUSTOM_AREA4_MIN_INTERVAL_MS = 36` | 1280×720 GE rotate 不要卡在 37 fps |
| 預防性 GE reset | 每 4000 張 | D12P 的 `custom_area4_ge_soft_reset()` 是**空函式**（沒有 d21x `hal_ge_soft_reset`） |

### 5.4 Back-pressure（一幀 in-flight）

**大屏／widget：** `s_perf_meas_pending==1` 且距 arm 未滿 500 ms → drop（return 0）。超過 500 ms 當 DRAW_POST_END 漏掉，force-through 並 warning。

為什麼要擋：PC 若推 ~55 fps，LVGL 只能 ~30，多出來的每張會 `aicos_malloc_try_cma` 4 MB ARGB，CMA 滿了會 `lv_drop_one_cached_image()`，跟 UI thread cache walk 撞車 → ref_cnt 亂掉、CPU exception。一幀 in-flight 把 CMA 鎖在約 1 個 MPP frame。

**圓屏：** 不用 DRAW_POST_END。若 stream active：

1. `custom_disp_stream_can_show()==0`（`jpgdec` 的 `s_job_busy`）→ return 0，**保留 payload**。
2. 否則把 `s_perf_meas_pending` 清掉，避免舊 widget 旗標卡住 JPEG。

### 5.5 Stream 快路徑（D125 真正在用的）

`custom_disp_stream_is_active()` 為真時：

1. JPEG → `CUSTOM_DISP_STREAM_IMG_JPEG`（注意 stream 頭檔 JPEG 編碼是 `0x02`，Area4／Image 的 JPEG 是 `0x00`，`show_image` 會轉換）。
2. `custom_disp_stream_show(...)`：
   - 圓屏：`custom_round_on_jpeg_stream()` 暫停 GIF；bitstream **memcpy 進 job buffer**，`s_job_busy=1`，叫醒 `jpgdec` 執行緒。**UI tick 不解碼**（VE wait 最長 2 s 會卡死 10 ms timer 與 USB）。
   - 成功（已 queue）→ 更新 perf／`s_total_show_count`，return **1**。
3. `show` 回 0：
   - **圓屏且 worker 忙：** return 0，讓 Image 再送一次。
   - **圓屏其他失敗：** log `keep QSPI stream`，return **1**（吃掉這張，不 release）。  
     原因：若 release，LVGL `disp_flush` 在圓屏本來就是 no-op，fallback widget 也不會寫玻璃，畫面會凍死。
   - **大屏：** `custom_disp_stream_release()`，掉到下面 widget 路徑。之後要恢復 stream 必須再切一次 AA 06。

圓屏 **不要指望** 走到 §5.6。widget 路徑假設有 MIPI FB + DRAW_POST_END。

### 5.6 LVGL widget 路徑（大屏；本方案備援）

1. 確保 `screen_4_Area` 存在。
2. `custom_area4_setup_hw_decoder_once()`：再 `lv_mpp_dec_init()` 一次，把 MPP 插到 decoder 清單頭。只 init 不 deinit（deinit 會弄掉 cache mutex，回 splash 會 Illegal Instruction）。
3. 動態建立滿版 `lv_image`，掛 `LV_EVENT_DRAW_POST_END`。
4. payload **memcpy** 進 boot 時那塊 1 MB CMA（不 realloc）。超過 1 MB skip。
5. 雙 `lv_img_dsc_t` toggle；資料指標都指向**同一塊** CMA。LVGL9 的 cache key 不含 `data`，所以 flags 必須 **`LV_IMAGE_FLAGS_USER4`**（bypass cache、仍走 GE2D）。`USER5` 會變軟解 ~50 ms；flags=0 會 cache race 當機。**禁止**再呼叫 `lv_image_cache_drop`。
6. PNG 可預讀 IHDR 寬高；JPEG 交給 decoder SOF。
7. `custom_disp_apply_area4_stream_image` 滿版 STRETCH → `set_src` → arm pending → `invalidate`。
8. 真正解碼發生在第一次 draw；`draw_done_cb` 清 `s_perf_meas_pending`。

`leave_display` 不可 `lv_image_set_src(NULL)`（LVGL9 unknown type / Illegal instruction），也不可先 hide（會閃 Area4 底色白屏）。最後一幀留到 `lv_scr_load` 蓋過去。

---

## 6. 圓屏 JPEG 與 GIF 的關係

Area4 **不播 GIF**。USB JPEG 進來時：

1. `custom_disp_stream_show` 開頭呼叫 `custom_round_on_jpeg_stream()` → 停 GIF timer。
2. `jpgdec` 解完走與 GIF 相同的 `lv_spi_present()`／stream_buf。
3. JPEG 結束、idle watchdog 回 splash 後，`custom.c` 的 carousel 再播 Panel GIF。

`lv_spi_on_display_stall`：GIF 若仍 running，`custom.c` **不**呼叫 `custom_area4_on_display_stall` 那套 recover（避免 JPEG recover 洗掉 GIF）。

---

## 7. 生命週期與離開畫面

```
custom_area4_init          boot，配 1 MB CMA
        │
AA 06 / 自動切屏
        │
custom_disp_stream_acquire  圓屏：開 jpgdec、pause LVGL refr
        │
多次 show_image
        │
AA 00 或 idle ~90 幀
        │
custom.c 等 render_pending==0   （圓屏 stream 路徑 pending 通常已被清掉）
        │
custom_area4_leave_display
        │  stream_release（圓屏最多等 jpgdec 2.5 s）
        │  remove DRAW_POST_END
        ▼
splash / GIF carousel
```

**不要**在 `leave_display` 裡清 `s_perf_meas_pending`。那是「GE2D 還在 blit」的唯一訊號；提早清掉會讓 splash 切換撞上 in-flight blit → Illegal Instruction／poisoned registers（大屏踩過的坑）。

`clear_render_pending` 留給：自動切 AA 06 時舊 pending 會讓新圖永遠進不了；以及 stall 復原。

---

## 8. 效能與 watchdog 計數

時間單位：`aic_get_time_ms()`。

| 名稱 | 定義 |
|------|------|
| pickup | `show` 入口 − Image `recv_done`（UI 反應速度） |
| show | `show` 出口 − 入口（圓屏多半只是 memcpy+queue） |
| render | DRAW_POST_END − show 入口（僅 widget） |
| interval | 兩次 show 入口差（真實 fps） |
| drop | 視窗內 `recv_done` 增量 − 成功 show 數 |

每 `CUSTOM_AREA4_PERF_WINDOW`（30）張打一次 stats。`CUSTOM_AREA4_VERBOSE` 預設 0：每幀 log 關掉。開 1 會在 UI tick 上打 4 行 UART，30 fps 預算直接被吃掉，且會跟 stats 交錯。

`s_total_show_count` 永不歸零。freeze watchdog 解讀：

| recv_done | show_count | 含義 |
|-----------|------------|------|
| 在加 | 不動 | 顯示路徑卡住 |
| 不動 | 不動 | USB 也停了 |
| 不動 | 在加 | 正常播完／重播 |

Heap snapshot 每 10 個 window 才印（約 10 s），只看 system heap。CMA 用 shell `meminfo`。

Back-pressure drop 每 100 次印一行，避免 PC 灌爆 UART。

---

## 9. 記憶體（本模組）

| 區塊 | 大小 | 何時 | 用途 |
|------|------|------|------|
| `s_area4_disp_buf` | 1 MB CMA，cache-line 對齊 | init 一次，永不 free | widget 路徑 bitstream。360 JPEG 約 10–20 KB，遠小於 1 MB |
| `s_area4_image_dsc_pool[2]` | 兩個 descriptor | 靜態 | 避免解碼中改 dsc |
| stream job buffer | 在 `custom_disp_stream.c` | 圓屏 JPEG | **不是** Area4 這 1 MB |

圓屏熱路徑**不寫** 1 MB Area4 buffer，只把指標傳進 `stream_show`，由 jpgdec 自己的 bounce buffer 拷貝。1 MB 仍會在 boot 配掉，佔 CMA。細節見 `D125_記憶體配置說明.md`。

---

## 10. 編譯差異（務必分清）

| | D125 圓屏 `CUSTOM_DISP_IS_ROUND_PROFILE` | 大屏 MIPI／LVDS |
|--|------------------------------------------|-----------------|
| 顯示 | QSPI `lv_spi_present`，LVGL flush 關掉 | FB + PAN 或 LVGL flush |
| `show` | queue `jpgdec` | 同步 MPP + GE rotate，或 widget |
| `can_show` | worker 忙則 0 | 永遠 1 |
| 36 ms 間隔／GE pulse | 不編譯 | 有 |
| stream 失敗 | 保持 QSPI，return 1 | release，改 widget |
| DRAW_POST_END | 圓屏 stream 不依賴 | back-pressure 本體 |
| `ge_soft_reset` | 空 | D215 才有 HAL |

同一支 `.c` 兩套語意。改圓屏時不要把大屏的「失敗就 release stream」抄回來。

---

## 11. 日誌

- prefix：`custom_area4:`
- 永遠印：CMA 預配失敗、所有 `show skip`、decode info fail、stall recover、back-pressure 每 100 次、stale arm warning、圓屏 `keep QSPI stream`、大屏 fallback
- `CUSTOM_AREA4_VERBOSE=1` 才印：每幀 pickup/show、stream stats、heap snapshot

排查「PC 有傳沒畫面」：先看 skip 的 `img_type`／`size`／`screen not ready`／`src not ready`，再看 `custom.c` 有沒有自動 AA 06、`custom_disp_stream` 有沒有 acquire。

---

## 12. 改這支檔時不要踩的坑

1. **不要**讓 Area4 接受 GIF。圓屏 GIF 與 JPEG 搶同一條 present，應維持分家。
2. **不要**圓屏 `stream_show` 失敗就 `custom_disp_stream_release()`。
3. **不要** `lv_image_set_src(NULL)` 或 `lv_mpp_dec_deinit()`。
4. **不要**拿掉 USER4 又開 cache／`cache_drop`（大屏 30 fps 數分鐘 hang）。
5. **不要**把 back-pressure timeout 改回 200 ms（CMA 會同時握 2–3 張 MPP frame）。
6. **不要**在 `leave_display` 清 `s_perf_meas_pending`。
7. **不要**在 USB OUT 呼叫 `show_image`。只在 LVGL tick。
8. **不要**以為 Area4 一成功 show 玻璃就畫完。圓屏只是 queue；真正 RAMWR 在 `jpgdec` + `lv_aic_spi`。`can_show==0` 必須 return 0 讓 tick 重試。
9. **不要**改 CD-BB 卻只改 `.h` 註解。實作是 `06 + fmt + 00`。
10. **不要**開 `CUSTOM_AREA4_VERBOSE` 當量產預設。
11. D12P 沒有 GE soft reset；加 d21x HAL include 會編不過。
12. `s_total_show_count++` 在 stream 成功 queue 就加，不是 RAMWR 完成。watchdog「有 show 沒畫面」仍可能是 QSPI 卡住，要對 `lv_spi`。

---

## 13. 建議閱讀順序

1. 本檔 §1–§6（角色、協定、圓屏 `show_image`）。
2. `D125_custom.c說明.md` §7–§9（誰 tick、誰自動 AA 06）。
3. `custom_disp_stream.c`（`jpgdec`、`can_show`、`present_argb8888`）。
4. `custom_Image.c` / `custom_Image_流程說明.md`（CD-BB 組包）。
5. `lv_aic_spi.c`（RAMWR 握手，決定 `show` 之後會不會破圖）。
6. `D125_記憶體配置說明.md`（1 MB Area4 + stream job + GIF blob 怎麼擠 16 MB PSRAM）。
