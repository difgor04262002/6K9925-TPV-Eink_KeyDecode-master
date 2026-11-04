# D125 `custom.c` 說明

> 適用平台：D125 / SDK 晶片名 **D12P**（`CONFIG_PRJ_CHIP="d12p"`）  
> 適用板卡：`demo88-nor`  
> 適用面板：ZC-T1D8BP-012（`CONFIG_AIC_PANEL_DBI_ZC_T1D8BP`，360 × 360 QSPI ST77916）  
> 源碼：`packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom.c`（約 3940 行）  
> 標頭：同目錄 `custom.h`  
> 方案 defconfig：`target/configs/d12p_demo88-nor_rt-thread_helloworld_zc_t1d8bp_defconfig`
>
> 本文件講**這支檔現在實際在做什麼**，圓屏路徑為主。同一支 `.c` 仍編譯進 D215 MIPI / LVDS / 條屏方案（`#if CUSTOM_DISP_IS_*`），那些分支在本方案是關掉的。
>
> 舊檔 `Note/custom_流程說明.md` 還寫 Area2 / Area3 / Video，**已過期**。USB／UART 封包格式見 `D125_USB指令封包說明.md`。協定與收圖細節另見 `Note/custom_Image_流程說明.md`。Area4 JPEG／PNG 顯圖見 `D125_custom_Area4.c說明.md`。QSPI 腳位與 RAMWR 見同目錄 `D125_QSPI配置說明.md`。記憶體見 `D125_記憶體配置說明.md`。四屏規劃見 `D125_四屏QSPI1與NY8選CS規劃.md`。

---

## 1. 這支檔是什麼

`custom.c` 是 UI Builder 專案的**應用總控**，不是面板驅動。它做四件事：

1. 吃 PC／主機送來的 USB／UART 位元組（`custom_recv_18bytes`）。
2. 用 LVGL 10 ms timer 切畫面、取待顯圖、轉盤播 GIF（`custom_screen_switch_timer_cb`）。
3. 把圖送到正確的顯示路徑：圓屏走 QSPI `lv_spi_present()`；大屏走 LVGL widget／Area4。
4. 處理少數系統指令：時間、USB Display 副屏、BROM 升級、ZC 的 MMOSIE 選片。

它**不**直接操作 QSPI1 暫存器。寫玻璃是 `lv_aic_spi.c` + `custom_disp_stream.c`。它也**不**解析 CD-BB 影像封包本體，那是 `custom_Image.c`。

2026-06-16 已整批拿掉 `custom_Area2` / `custom_Area3` / `custom_Video`。合法畫面命令只剩：

| AA cmd | 畫面 | D125 圓屏 |
|--------|------|-----------|
| `0x00` | splash（`screen.c`） | 用，開機與 idle GIF 掛在這裡 |
| `0x01` | `screen_main` | **忽略**（1280×800 UI 放不下 360 圓屏） |
| `0x02` | `screen_main_1` | **忽略**（同上，會爆 MPP cache） |
| `0x06` | `screen_4_Area` | USB JPEG 串流 |

`0x03` Video / `0x04` 2_Area / `0x05` 3_Area 已刪，收到會落到 `else { return; }`。

---

## 2. 檔案鄰居

| 檔案 | 與 `custom.c` 的關係 |
|------|----------------------|
| `custom.h` | 對外 API |
| `custom_Image.c` | USB 收圖、LittleFS `/data/Panel_0..4`、CD-BB |
| `custom_Area4.c` | Area4 JPEG／PNG 顯示（圓屏再轉 `custom_disp_stream`） |
| `custom_disp_stream.c` | ARGB／JPEG → `lv_spi_present()` |
| `custom_disp_profile.h` | `CUSTOM_DISP_IS_ROUND_PROFILE`、360×360 |
| `screen.c` | splash 物件；圓屏會把 `image_8`（舊 PNG）藏起來 |
| `lv_aic_spi.c` | QSPI RAMWR、`lv_spi_can_present` / `try_lock_draw` |
| `gifdec.c` | `gd_open_gif_*` / `gd_get_frame`；首幀 `frame` 先填 bgindex |
| `mmosie.c`（DRV） | PD.26 1-wire 四位元 |

```
USB / UART
    │
    ▼
custom_recv_18bytes          ← 協定分流（本檔）
    ├─ MMOSIE 10/11B         → aic_mmosie_set_frame + 指定 Panel
    ├─ BROM 9B / USB-Disp 12B
    ├─ AA 18B                → s_pending_cmd + metrics
    └─ 其餘                  → custom_image_recv（CD-BB 與 payload）
                              │
LVGL 10 ms timer              │
custom_screen_switch_timer_cb │
    ├─ 切屏                  custom_show_screen_by_cmd
    ├─ 圓屏 carousel         custom_round_carousel_poll
    └─ take_pending          custom_show_image_by_type
                              ├─ Area4 JPEG → custom_disp_stream
                              └─ GIF → gd_* + present_argb8888
```

---

## 3. 編譯開關（為什麼圓屏行為不同）

`custom_disp_profile.h`：定義了 `AIC_PANEL_DBI_ZC_T1D8BP` 時

- `CUSTOM_DISP_IS_ROUND_PROFILE = 1`
- `CUSTOM_DISP_IMG_W/H = 360`

本檔大量 `#if CUSTOM_DISP_IS_ROUND_PROFILE`。圓屏開啟時：

- 不載入 `screen_main` / `screen_main_1`。
- splash 的 `image_8`（`zc_t1d8bp_185_small.png`）**隱藏**，改播 `/data/Panel_0/Panel_000.gif`。
- GIF 不走 LVGL `lv_gif` flush，走 `gifdec` + `custom_disp_stream_present_argb8888`。
- USB 顯圖時**先放** `custom_image_bus_lock`，避免 JPEG 解碼期間 NAK 主機。
- `lv_spi_on_ramwr_idle` 會叫醒 10 ms timer。

其他面板才會編譯：H0499 splash CMA 快照、FL7707 條屏排版、main_1 三槽 JPEG/PNG/GIF widget、lodepng 軟解。

日誌：`CUSTOM_UI_VERBOSE` 預設 0。重要事件用 `CUSTOM_UI_ALWAYS` / `CUSTOM_KLOG_ALWAYS`（`rt_kprintf`）。

---

## 4. 對外函式（`custom.h`）

| 函式 | 誰呼叫 | 作用 |
|------|--------|------|
| `custom_init()` | UI 啟動（`ui_init` 之後） | 清狀態、Area4／stream／Image init、開 10 ms timer、開 UART RX |
| `custom_recv_18bytes(data, len)` | USB vendor OUT、UART 逐 byte | 唯一協定入口 |
| `custom_ui_wake_tick_timer()` | Image 收完、RAMWR idle | `lv_timer_ready`，讓下一輪立刻 pickup |
| `custom_ui_screen_cmd()` | 其他模組 | 目前畫面：0 splash / 0x01 main / 0x06 Area4 |
| `custom_usb_display_mode_is_active()` | 串流路徑 | 副屏期間不要蓋 VIDEO layer |
| `custom_round_on_jpeg_stream()` | Area4／stream 開始 JPEG | 暫停 GIF timer，把 QSPI 讓給 JPEG |
| `lv_spi_on_ramwr_idle()` | `lv_aic_spi`（僅 ZC） | RAMWR 結束 → wake tick |
| `lv_spi_on_display_stall()` | `lv_aic_spi`（僅 ZC） | 傳輸卡住：GIF 播放中直接 return；否則 `custom_disp_stream_recover` |

`s_pending_cmd` 空閒值必須是 **`0xFF`**，不能用 0。`0x00` 是真的 AA 00 splash。

---

## 5. 執行緒與 timer

| 執行脈絡 | 週期／觸發 | 可做／不可做 |
|----------|------------|----------------|
| LVGL UI thread | `custom_screen_switch_timer_cb`，10 ms | 切屏、解 GIF、present、改 widget |
| UART `ui_uart_rx` | 有 byte 就跑 | **只**呼叫 `custom_recv_18bytes(&ch, 1)`。堆疊 **2 KB**，已裝 stack canary。禁止在這裡解圖、invalidate |
| USB bulk OUT | 主機送封包 | 同樣進 `custom_recv_18bytes`。圓屏解 JPEG **不可**握著 image lock |
| `usbdisp_sw` | 副屏切換 | 才能呼叫 `usb_display_*`；OUT callback 裡不行 |

UART 候選：`uart4` → `uart1` … **跳過** `RT_CONSOLE_DEVICE_NAME`，否則會把 MSH 控制台 RX 搶走。

---

## 6. `custom_init` 做什麼（圓屏）

順序（節錄，約 3793 行）：

1. 副屏 worker、parser 歸零；開機維持 LVGL／CD-BB，**不**自動進 USB Display。
2. `s_current_screen_cmd = 0`（splash），觸控關閉（圓屏本來就幾乎不用）。
3. 清 round GIF 狀態；carousel 預設 `s_round_boot_hold = 1`。
4. `custom_area4_init()`、`custom_disp_stream_init()`、`custom_image_init()`。
5. `custom_round_boot_start()`：下一個 tick 就去播 Panel_0。
6. `custom_prefer_tjpgd_decoder()`（圓屏 JPEG 仍可能走 Area4／MPP，依 stream 實作）。
7. `lv_timer_create(..., 10, NULL)`。
8. `custom_uart_init()`。

開機畫面**不是** PNG splash，是 LittleFS `Panel_0` GIF（沒檔就停）。

---

## 7. `custom_recv_18bytes` 分流順序

入口約 3631 行。順序固定，後面的 parser 會被前面的「已知封包」擋掉，避免 JPEG 高熵資料誤觸 BROM。

```
1. ZC：custom_try_mmosie_ctrl（10 或 11 byte）
      成功 → aic_mmosie_set_frame + custom_image_set_flash_panel
      若後面還有資料 → custom_image_try_start_flash_file
2. 整包恰為 BROM 9 byte → 立刻進升級
3. 整包恰為 USB Display enable/disable 12 byte
4. 若已在副屏：只掃 disable / 時間 / BROM，不吃 AA、CD-BB
5. 判斷 is_aa_frame（18B、AA + 00/01/02/06）
        is_image_cmd（6B、CD BB … CD）
6. 收圖中或已知 framing → 重置 BROM / usbdisp 逐 byte 狀態
7. 否則逐 byte：時間、BROM、usbdisp
8. 若 len==18 且 AA 合法 cmd：
      s_pending_cmd（cmd 有變才排隊）
      解析 metrics（CPU/GPU/RAM…）→ s_metrics_dirty
9. custom_image_recv（真正吃 CD-BB 與影像 payload）
```

**AA 18 byte 佈局（小端 16-bit）：**

| offset | 內容 |
|--------|------|
| 0 | `0xAA` |
| 1 | cmd：00 / 01 / 02 / 06 |
| 2–3 | CPU 頻率 |
| 4 | CPU usage |
| 5–6 | GPU 頻率 |
| 7 | GPU usage |
| 8 | CPU 溫度 |
| 9 | GPU 溫度 |
| 10–11 | Storage |
| 12–13 | Storage 2 |
| 14–15 | RAM |
| 16–17 | RAM 2 |

圓屏沒有 main_1 標籤，metrics 仍會存，只是 Area4 fast-path 不畫。AA **不會**刷新 stream idle 心跳；只有 CD-BB／payload 算「還在串流」，否則約 90 幀沒圖就回 splash。

**CD-BB 6 byte**（影像開始）：`CD BB <type> <slot> <len_hi? 見 Image 檔> CD`。型別：

| 值 | 意義 |
|----|------|
| `0x00` | JPEG |
| `0x01` | PNG |
| `0x02` | GIF |
| `0x03` | APNG |

slot `0x01..0x03` 給 main_1；Area4 用 `custom_area4_is_slot`（`0x30`）。圓屏 JPEG 幾乎都當 Area4。PC 常**不送 AA 06** 就開 CD-BB，`custom_show_image_by_type` 會自動切 0x06，否則圖畫在非 active screen、DRAW_POST_END 不來，看起來像死機。

---

## 8. 畫面切換 `custom_show_screen_by_cmd`

| cmd | 行為（圓屏） |
|-----|----------------|
| `0x00` | `custom_show_splash_screen`：停 USB 收圖；若正在 Area4 則設 `s_pending_splash_after_area4`，等 GE2D／最後一幀再 load splash，並 `custom_round_idle_start()` |
| `0x01` / `0x02` | log「ignored on round panel」後 return |
| `0x06` | 建 `screen_4_Area`、`custom_disp_stream_acquire()`、記 stream activity |

離開 Area4 會 `custom_image_abort_recv` + `custom_area4_leave_display`。

觸控：`custom_touch_sync_to_screen` 只在 `0x01` 打開。圓屏永遠當 splash／Area4，觸控保持關。ST7123 的 I2C pause 邏輯是大屏用的。

---

## 9. 10 ms tick：`custom_screen_switch_timer_cb`

約 2909 行，每 10 ms 一次（RAMWR idle 可提前 ready）。順序：

1. 若 `s_pending_splash_after_area4` 且 Area4 已畫完 → `custom_load_splash_screen`。
2. 若副屏剛關掉 → splash。
3. 副屏模式中 → **整段 return**（不要 Area4 pickup）。
4. `s_pending_cmd != 0xFF` → `custom_show_screen_by_cmd`。
5. `custom_stream_idle_watchdog`：Area4 連續約 90 幀沒新鮮資料 → splash（約 2.5–3 s；以前 30 幀太緊，USB 中斷會重建 splash 跟 MPP 打架）。
6. **圓屏且不在 0x06**：`custom_round_carousel_poll()`。
7. 圓屏 Area4：每秒印 USB `recv_done` heartbeat。
8. **非 0x06**：更新時間與 metrics 標籤。0x06 跳過（widget 不在 active screen，每次 5–15 ms 會拖死 fps）。
9. `custom_image_take_pending` → `custom_show_image_by_type`。圓屏在 show 前 **unlock** bus。

`cmd == 0` 與 `cmd != 0` 兩段 show 邏輯幾乎重複，差別只在「這一 tick 是否剛切過屏」。兩邊圓屏都是 unlock 再解碼。

---

## 10. 圓屏 GIF：為什麼不走 LVGL gif widget

splash 之後 `spi_dev_flush()` 不再把 LVGL 畫到玻璃。若用 `lv_gif`，畫面只存在 FB，QSPI 看不到。所以：

1. 檔案（或 USB GIF）拷進 CMA blob，上限 **`CUSTOM_ROUND_GIF_BLOB_MAX` = 2 MB**。
2. `gd_open_gif_data` / `gd_open_gif_file`。
3. 10 ms 的 `s_round_qspi_gif_timer` 解一幀、`present_argb8888`。
4. **不要** `aicos_free` 這個 blob 再配。從 2 MB 系統 heap 重配 400 KB+ 曾打壞 QSPI frame buffer。`custom_round_qspi_gif_stop` 只 close decoder、sz=0，CMA 留下一輪。

尺寸超過 360×360 直接拒絕。

### 10.1 timer：`custom_round_qspi_gif_timer_cb`

這是破圖修復的核心：

1. 還沒 `s_round_gif_frame_ready`：等 GCE delay（×10 ms，最少 20 ms）→ disposal 3 還原 → `gd_get_frame`。檔尾 rewind、canvas 填背景再取第一幀。
2. `gd_render_frame` 到 `gif->canvas`（ARGB8888），設 `frame_ready = 1`。
3. `custom_disp_stream_present_argb8888`。  
   **失敗就 return，不清 `frame_ready`。** 下一 round 再試同一幀。若失敗還 `gd_get_frame`，會跳幀，看起來像破圖。
4. 成功才清 ready、更新 `s_round_qspi_gif_last`。

`custom_disp_stream` 裡會 `lv_spi_try_lock_draw`；RAMWR 忙或 settle 8 ms 未滿會失敗，這是預期。

### 10.2 disposal 與首幀垃圾

| 動作 | 函式 | 原因 |
|------|------|------|
| 開檔把 indexed `frame[]` 填 `bgindex` | `play_flash_gif` + `gifdec` | 否則第一幀透明處是 heap 垃圾 |
| canvas 填背景色 BGRA | `custom_round_gif_reset_canvas` | rewind／新檔第一張乾淨 |
| disposal 3 先備份矩形 | `custom_round_gif_save_prev`（CMA） | 下一幀要還原「繪製前」 |
| 取下一幀前還原 | `custom_round_gif_restore_prev` | 標準 GIF restore-to-previous |
| 播 GIF 藏 `image_8` | `play_flash_gif` | 不要 PNG 跟 GIF 疊在 splash |

Disposal 2（還原背景）在 `gifdec.c`；3 在本檔。

### 10.3 JPEG 插入

USB 開始 JPEG：`custom_round_on_jpeg_stream()` 把 `s_round_qspi_gif_running = 0` 並 pause GIF timer。JPEG 走同一條 `lv_spi_present`。`lv_spi_on_display_stall` 若 GIF 仍 running 則不呼叫 JPEG recover，避免互相洗畫面。

---

## 11. 單屏轉盤（Panel_0…4）

開關：`CUSTOM_ROUND_SINGLE_SCREEN_CAROUSEL` 預設 **1**（一塊 TFT 輪播四個資料夾）。將來一 QSPI 對一資料夾時設 0。

| 常數 | 預設 | 意義 |
|------|------|------|
| `CUSTOM_ROUND_BOOT_GIF_MS` | 5000 | 開機只播 Panel_0 的時間 |
| `CUSTOM_ROUND_GIF_DWELL_MS` | 8000 | 之後每個使用者 GIF 停留 |
| `CUSTOM_ROUND_CAROUSEL_DEBOUNCE_MS` | 700 | USB 存檔後等一下再切，避免寫一半就 open |

資料夾（`custom_Image.h`）：

| panel | 路徑 | 誰寫 |
|-------|------|------|
| 0 | `/data/Panel_0/Panel_000.gif` | 燒錄包／工廠開機動畫；USB **不覆寫** |
| 1–4 | `/data/Panel_n/…` | USB 依 MMOSIE idx 存 |

`custom_round_carousel_poll`（tick、且不在 Area4）：

1. USB 正在收檔 → 只把 debounce 往後推。
2. `s_round_carousel_reload`：boot_hold 則 Panel_0，否則第一個有檔的 1–4，都沒有就 Panel_0。
3. 播到一半：boot 5 s 到了就進 1–4；某片 8 s 到了就 `next_gif_panel`。只有一片就 rewind 繼續播。
4. `lv_spi_can_present()` 為假時不切檔，避免 RAMWR 中途換 decoder。

USB 送來 GIF：`custom_show_image_by_type` **不立刻播記憶體裡那份**，而是 `custom_round_carousel_notify()`（結束 boot_hold、debounce 後從 1 開始轉）。檔已由 Image 模組寫進對應 Panel 資料夾。

`custom_round_play_panel_file`：load 進既有 CMA blob → `custom_round_play_flash_gif`。

---

## 12. MMOSIE（僅 `AIC_PANEL_DBI_ZC_T1D8BP`）

PC 在圖檔前送 10 或 11 byte：

```
F9 F9 F2 F5  [FA|FB|FC|FD]  [01..04 或 F1..F4]  F9 F9 F2 F5
11 byte 變體：… cmd F0 0n F9 F9 F2 F5
```

| idx | PD.26 四 bit（CS mux 給 NY8） |
|-----|------------------------------|
| 1 | `0,1,1,1` |
| 2 | `1,0,1,1` |
| 3 | `1,1,0,1` |
| 4 | `1,1,1,0` |

`custom_try_mmosie_ctrl` 只更新 payload（`aic_mmosie_set_frame`）並 `custom_image_set_flash_panel`。**真正在 PD.26 送出是 QSPI1 CS 低的時候**（`mmosie.c`，2A 前導碼；RAMWR 結束 CS 高再送 `1111`）。本檔不管腳位時序。

現行韌體仍是**一塊玻璃**。idx 用來選 USB 存到哪個資料夾、以及轉盤播哪個檔。四塊並聯硬體的利弊見四屏規劃文件，**尚未**在 `custom.c` 做 round-robin present。

---

## 13. 其他仍在本檔的協定（圓屏較少用）

### 13.1 時間

逐 byte 組 10 byte 時間框 → `custom_datetime_set`。之後靠 `lv_tick` 自己加秒，寫 splash `label_1`。圓屏 Area4 不更新 label。

### 13.2 USB Display 副屏

整包 12 byte：

- Enable：`DE DF DA EF 11 44 66 23 EF DA DF DE`
- Disable：`DE DF DA EF 22 55 77 34 EF DA DF DE`

工作丟給 `usbdisp_sw` 執行緒。啟用後 `s_usb_display_mode = 1`，tick 不再 pickup 圖。圓屏專案若沒開 CherryUSB Display，enable 是 stub log。

### 13.3 BROM／升級

整包 9 byte：`FF A1 B2 E3 F4 C5 D6 EE EE`  
寫 env `aicupg_gotobl=1`，reason 必須 `REBOOT_REASON_BL_UPGRADE`（不是 `REBOOT_REASON_UPGRADE`），再餵 WDT。收圖／AA／CD-BB 期間不做逐 byte 比對，以免 JPEG 誤觸。

---

## 14. `custom_show_image_by_type`（圓屏精簡路徑）

約 2185 行：

1. Area4 slot → 必要時自動 AA 06 → `custom_area4_show_image`。
2. 圓屏 + GIF → carousel notify 或直接 `play_flash_gif`。
3. 圓屏 + APNG → `lv_aic_player`（少用；QSPI 無 LVGL flush 時 APNG 可能只在 FB）。
4. 圓屏其他 type → return 0（JPEG 應走 Area4 slot）。
5. 非圓屏：main_1 三槽、double buffer、progressive JPEG 拒絕、PNG 可 lodepng。

---

## 15. 關鍵狀態機（圓屏）

| 變數 | 意義 |
|------|------|
| `s_current_screen_cmd` | 0 splash / 0x06 Area4 |
| `s_pending_cmd` | 下一屏；閒置 0xFF |
| `s_round_qspi_gif_running` | GIF timer 可解可送 |
| `s_round_gif_frame_ready` | 已解、等 present 成功 |
| `s_round_boot_hold` | 還在 5 s 開機 Panel_0 |
| `s_round_carousel_panel` | 正在播的資料夾 0–4 |
| `s_round_carousel_reload` | 到點要換檔 |
| `s_wait_image_type` | tick 還沒 show 成功的 pending 圖 |
| `s_usb_display_mode` | 副屏，凍結串流 UI |

---

## 16. 改這支檔時不要踩的坑

1. **不要**在 UART／USB OUT 裡 `lv_scr_load`、解 GIF、present。只改 flag／queue。
2. **不要**把 `s_pending_cmd` 閒置設成 0。
3. **不要** present 失敗後再 `gd_get_frame`。
4. **不要**每次換 GIF 就 free CMA blob。
5. **不要**圓屏 `lv_scr_load(screen_main_1)`。
6. **不要**圓屏 JPEG 路徑握著 `custom_image_bus_lock` 解碼。
7. **不要**以為改 `custom.c` 就能讓四塊屏各 30 fps：還是一條 QSPI1。選片在 MMOSIE，帶寬在 `lv_aic_spi`。
8. **不要**把 SCLK 改 100 MHz 當 `custom.c` 的 fps 解法。玻璃／TXB 規格卡在 50 MHz，見 QSPI 說明 §3.2。
9. GIF 路徑與 JPEG 共用 present；改 handshake 要兩邊一起看 `lv_aic_spi` 的 `ramwr_busy` / `draw_locked` / `ZC_RAMWR_SETTLE_MS`。
10. `CUSTOM_UI_VERBOSE=1` 會在熱路徑打很多 log，圓屏 30 fps 會抖。

---

## 17. 建議閱讀順序

1. 本檔 §1–§11（總控 + 圓屏 GIF）。
2. `D125_USB指令封包說明.md`（PC 要送的每一種框）。
3. `D125_custom_Area4.c說明.md` + `custom_Area4.c`（USB JPEG／PNG 怎麼上屏）。
4. `custom_Image_流程說明.md` + `custom_Image.c`（CD-BB、LittleFS）。
5. `custom_disp_stream.c`、`lv_aic_spi.c`（present 握手）。
6. `D125_QSPI配置說明.md`（腳、2A/2B/2C、50 MHz）。
7. `D125_記憶體配置說明.md`（2 MB blob、stream_buf）。
8. 若要四屏硬體：`D125_四屏QSPI1與NY8選CS規劃.md`（規劃，不是現行 `custom.c` 行為）。
