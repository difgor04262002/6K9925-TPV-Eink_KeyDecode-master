# D125 QSPI 配置說明

> 適用平台：D125 / SDK 晶片名 **D12P**（`CONFIG_PRJ_CHIP="d12p"`）
> 適用板卡：`demo88-nor`
> 適用面板：ZC-T1D8BP-012（ST77916，360 × 360，**無 DC/RS、無 TE**）
> 方案 defconfig：`target/configs/d12p_demo88-nor_rt-thread_helloworld_zc_t1d8bp_defconfig`
>
> 本文件只講**目前這套方案實際在用的 QSPI**。D215 MIPI / 舊 DBI-on-PD 路徑不要套過來。

兩顆控制器分工：

| 控制器 | 裝置名 | 用途 | 本方案是否啟用 |
|--------|--------|------|----------------|
| **QSPI0** | `qspi0` | SPI NOR（開機 + 檔案系統） | ✅ `CONFIG_AIC_USING_QSPI0` + `CONFIG_AIC_QSPI0_DEVICE_SPINOR` |
| **QSPI1** | `qspi1` / `qspi1dev` | LCD 像素與 init | ✅ `CONFIG_AIC_USING_QSPI1`，**不是** NOR |
| QSPI2 | `qspi2` | 第二顆 SPI 屏 | ❌ 未開 |

像素**不走** DE / MIPI-DBI 引擎，走 `LV_USE_SPI_REPLACE_MIPI_DBI`：`lv_st77916` 初始化 + `lv_aic_spi` / `aic_spi_lcd` 推 RAMWR。`panel_dbi_zc_t1d8bp.c` 只提供 `screeninfo` / timing，**不碰** QSPI1 腳與 DBI 引擎。

2026-09 圓屏現況（細節 §11–§14）：

- 開機 UI 是 LittleFS **`/data/Panel_0/Panel_000.gif`**，**不再**顯示 `zc_t1d8bp_185_small.png`。
- GIF 與 USB JPEG **同一條** `lv_spi_present()` / 整幀 0x2C，不走 LVGL `flush`。
- 這片玻璃**沒有 TE**。軟體用 `ramwr_busy` + `draw_locked` + `ZC_RAMWR_SETTLE_MS` 降低破圖與連續 RAMWR 疊掃描；無法對到真正 VBLANK，水平撕裂仍可能殘留。
- 四顆並聯、PD.26/NY8 選 CS 的規劃與利弊：同目錄 `D125_四屏QSPI1與NY8選CS規劃.md`。現行韌體仍是單屏。

---

## 1. 腳位

### 1.1 QSPI1 → LCD（J25 DVP 座改接面板）

來源：`target/d12p/demo88-nor/pinmux.c`（`AIC_USING_QSPI1`）  
Func **6** = PE 上的 SPI1（DVP=3、EPWM=7，不要搶）。

| SoC 腳 | 功能 | 接到面板 | Pull / Drive |
|--------|------|----------|--------------|
| **PE.5** | SPI1_CS | **LCD_CS** | Pull-up，drive 3 |
| **PE.6** | SPI1_MISO | **SPI_D1** | 無 pull，drive 3 |
| **PE.7** | SPI1_MOSI | **SPI_D0** | 無 pull，drive 3 |
| **PE.8** | SPI1_CLK | **SPI_CLK** | 無 pull，drive 3 |
| **PE.9** | SPI1_HOLD | **SPI_D3** | 無 pull，drive 3 |
| **PE.10** | SPI1_WP | **SPI_D2** | 無 pull，drive 3 |
| **PE.4** | GPIO | **LCD_RESET** | GPIO func 1，Pull-down；開機先拉低 |
| **PF.0** | GPIO | 背光 enable | `CONFIG_AIC_PANEL_ENABLE_GPIO="PF.0"`，低有效關燈 |
| **PD.26** | GPIO | MMOSIE 1-wire | **不是** QSPI MOSI |

沒有 DC/RS、沒有 TE。命令/資料靠 ST77916 QSPI 編碼（見 §4）。

RST 由 pinmux / `demo88_lcd_hold_dark()` 先拉低，避免上電露出上一幀 GRAM；`lv_st77916.c` 的 `lv_spi_panel_enable()` 再打 reset pulse（低 20 ms → 高 120 ms）。背光等第一幀 RAMWR + DISPON 後才由 `lv_aic_spi` 打開，避免 datasheet 寫的「電源/gamma 還沒寫完就開燈」。

### 1.2 QSPI0 → SPI NOR

非 SIP Flash 時（本方案）：

| SoC 腳 | 典型功能 |
|--------|----------|
| PB.0–PB.5 | QSPI0 CLK / CS / D0–D3（func 2，Pull-up） |

SIP 路徑會改走 PB.12–17，本 `demo88-nor` 不用。

### 1.3 不要接錯的腳

| 腳 | 為什麼容易搞錯 |
|----|----------------|
| PD.26 | 只做 MMOSIE GPIO 1-wire，**不是** SPI1/SPI5 MOSI |
| PE.4–10 | 開了 QSPI1 就**不要**再配 DVP func 3 |
| 舊 DBI/DE QSPI on PD | `LV_USE_SPI_REPLACE_MIPI_DBI` 時不用 |

---

## 2. Kconfig / 軟體開關

defconfig 相關項：

| 項 | 值 | 意義 |
|----|-----|------|
| `CONFIG_AIC_USING_QSPI0` | y | NOR 控制器 |
| `CONFIG_AIC_DEV_QSPI0_MAX_SRC_FREQ_HZ` | **200000000** | QSPI0 時鐘源上限 200 MHz |
| `CONFIG_AIC_QSPI0_DEVICE_SPINOR` | y | QSPI0 掛 SPI NOR |
| `CONFIG_AIC_USING_QSPI1` | y | LCD 控制器 |
| `CONFIG_AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ` | **未寫 → 預設 100000000** | QSPI1 時鐘源 100 MHz |
| `CONFIG_AIC_QSPI1_BUS_WIDTH` | 未寫 → 預設 **4**（Quad） | 控制器資料線 |
| `CONFIG_AIC_QSPI1_DEVICE_SPINOR` | 未開 | QSPI1 **不是** Flash |
| `CONFIG_LV_USE_SPI_REPLACE_MIPI_DBI` | y | 像素走 generic QSPI，不走 DBI |
| `CONFIG_LV_SPI_ST77916` | y | 編譯 `lv_st77916.c` |
| `CONFIG_LV_SPI_BUS_WIDTH_4` | y | LCD 走 4-wire QSPI（不是 1-wire SPI） |
| `CONFIG_AIC_QSPI_LCD_ST77916` | y | `bsp/peripheral/spilcd` ST77916 面板支援 |
| `CONFIG_AIC_PANEL_DBI_ZC_T1D8BP` | y | 本玻璃 + screeninfo |
| `CONFIG_AIC_USING_DMA` | y | QSPI DMA |

QSPI1 若誤開 `AIC_QSPI1_BUS_SPI`，會改走標準 SPI bus、不再註冊 `qspi1` 這個 QSPI master。本方案必須維持 **QSPI master + 4-wire**。

---

## 3. 時鐘

兩顆控制器的模組時鐘都從 **PLL FRA0** 分（`CLK_QSPI0` / `CLK_QSPI1`，`aic_clk_def.c`，暫存器 `0x0464` 一帶）。

### 3.1 QSPI1（LCD）實際 bus

`lv_aic_spi.c` 向驅動要：

| 項 | 值 |
|----|-----|
| 裝置 | `bus_name="qspi1"`，`dev_name="qspi1dev"` |
| `max_hz` | **50 MHz**（ZC 面板；其他 SPI 屏仍 25 MHz） |
| mode | `RT_SPI_MODE_0` + MSB |
| `bus_width` | 4 |

`drv_qspi` 的 `clk_in_hz` 預設 **100 MHz**。100 能整除 50，所以來源維持 100 MHz，控制器 **÷2 → 約 50 MHz SCLK**。

註解裡的舊實驗（不要當現況）：

| 曾用過的 `max_hz` | 結果 |
|-------------------|------|
| 32 MHz | 來源被改成非整除，落到 ~60.9 MHz /2 ≈ **30.5 MHz**，加上 10 ms tick 對齊後 RAMWR 約 32 ms，約 25 fps |
| 50 MHz（現況） | RGB666 整幀線上約 **15.5 ms**，配合 wake-on-RAMWR-idle 可清 **30 fps** |

1-wire @ 5 MHz 整幀約 0.62 s（~1.6 fps），只當除錯，量產必須 4-wire。

硬體上限：`HAL_QSPI_MAX_FREQ_HZ = 200 MHz`，`HAL_QSPI_MIN_FREQ_HZ = 100 kHz`。

### 3.2 QSPI1 能否拉到 100 MHz？

**SoC／驅動：可以配。玻璃與排線：沒驗證，不能當現況。四屏 daisy-chain 更不該一開始就上 100 MHz。**

現況是 **SCLK 50 MHz**（模組時鐘 100 MHz ÷ 2）。HAL 把 bus 上限寫成 `HAL_QSPI_MAX_FREQ_HZ = 200000000`，所以 **QSPI1 控制器本身允許 100 MHz**，不是被 50 MHz 寫死。

#### 驅動怎麼分頻

`drv_qspi`：`clk_in_hz`（Kconfig `AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ`，本方案預設 **100 MHz**）必須能**整除** `max_hz`，否則會把來源改成 `max_hz`，PLL 對不齊，SCLK 會漂（32 MHz 實驗變成 ~30.5 MHz 就是這樣）。

`hal_qspi_master_set_bus_freq()` 用 CDR1（`sclk >> n`）或 CDR2（`sclk / (2*(n+1))`）。100 MHz 來源、要 50 MHz：CDR1 `n=1` → 剛好 50 MHz。

若要 **SCLK = 100 MHz**，有兩種配法：

| 配法 | `AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ` | `lv_aic_spi.c` `.max_hz` | 控制器 | 評價 |
|------|----------------------------------|--------------------------|--------|------|
| A. 來源＝bus（1:1） | 維持 `100000000` | 改成 `100000000` | `sclk >> 0` = 100 MHz | 改一處就會出 100 MHz；1:1 時鐘品質通常不如 ÷2 |
| B. 來源 200 MHz ÷2（建議若真要試） | 改成 `200000000`（QSPI0 NOR 已是這個） | `100000000` | 200÷2 = 100 MHz | 跟現在「100÷2＝50」同一套想法，較穩 |

開機 log 會印 `qspi1 freq ( bus ): …Hz`，改完**一定要看這行**，不要只改 `max_hz` 以為一定是 100。

#### 100 MHz 時一幀要多久

同一條公式，只換分母（見 §7.1）：

```
388800 × 8 / 4 / 100e6 = 3 110 400 / 400 000 000 = 0.007776 s ≈ 7.8 ms
```

（50 MHz 是 15.5 ms，時鐘加倍，像素段時間減半。）

| | 50 MHz（現況） | 100 MHz（若線上真的到） |
|--|----------------|-------------------------|
| 0x2C 像素段 | ≈ 15.5 ms | ≈ 7.8 ms |
| 單屏理論上限 | ~30 fps（含開銷） | 像素段可擠更高，仍受 2A/2B、GIF 解碼、無 TE 撕裂限制 |
| 四屏輪流滿幀（只算像素） | ≈ 62 ms → ~16 fps／顆 | ≈ 31 ms → ~32 fps／顆（仍**沒有**四路各 30 fps 的餘裕，settle/2A/2B 還會再吃） |

DMA：100 MHz × 4 bit ≈ **50 MB/s**，388800 byte 一幀，PSRAM CMA 通常不是瓶頸。

#### 為什麼還是不要預設改 100 MHz

| 關卡 | 50 MHz | 100 MHz |
|------|--------|---------|
| D12P QSPI1 HAL | 允許 | 允許（≤ 200 MHz） |
| ST77916 QSPI 規格 | 貼著上限 | **不合格**：寫入 clock 週期 `TSCYCW` min **16 ns**（理論 ≤ 62.5 MHz），腳位寫 **Max = 50 MHz**；100 MHz 週期只有 10 ns、高／低各 5 ns（規格要 ≥ 7 ns）。這是超頻玻璃，不是「先改看看就合法」 |
| 本方案 ST77916 實測 | 現行量產路徑 | **沒有** 100 MHz 燒機紀錄。vendor 表是 `…QSPI_OK`，專案選 50 MHz 是穩定點 |
| PE.5–10、drive 3、無端接 | 單屏可工作 | 邊沿變快一倍，反射／串音容易讓 0x32 標頭或 RGB666 錯位 → 花屏 |
| 四屏 TXB0106 daisy-chain | 已經偏兇 | TXB0106 官方最大 **100 Mbps**、`tw` min **10 ns**（約等於 50 MHz 時鐘脈寬）。**100／200 MHz 都不合格**。見四屏規劃 **§2.3** |
| RX sample delay | 本路徑幾乎只寫、少讀 | 1:1 時 `AIC_DEV_QSPI1_DELAY_MODE` 更敏感（若有讀回） |
| MMOSIE | CS 高才打 PD.26 | 與 SCLK 無關；100 MHz 不會讓 1-wire 變快 |

#### 若要實驗，建議順序

1. **只改單屏**，不要四顆並聯。
2. 優先配法 B：`CONFIG_AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ=200000000` + `.max_hz = 100000000`。
3. 看 `qspi1 freq ( bus )` 是否真的 100000000。
4. 邏輯分析儀量 PE.8 週期是否 10 ns；量一次完整 2A/2B/2C，長 CS 是否約 7.8 ms。
5. 畫面穩再考慮中間檔。`max_hz` 必須被來源**整除**（不要再走 32 MHz 那種漂頻）。
6. 花屏／偶發破圖 → 立刻退回 50 MHz。不要用 100 MHz 去賭四屏 fps。

**結論：** QSPI1 **控制器可以**配 100 MHz；**目前這塊 ST77916 + TXB0106 不該先改。** 玻璃規格卡在 50 MHz，TXB 脈寬卡在 10 ns。單屏超頻實驗最多當實驗室項目（配法 B、量 PE.8），不能當量產預設。四屏 daisy-chain 更不要先上 100 MHz。現況維持 **50 MHz**。

### 3.3 QSPI0（NOR）

| 項 | 值 |
|----|-----|
| 時鐘源上限 | **200 MHz**（defconfig 寫死） |
| NOR bus 頻率 | `AIC_QSPI0_DEVICE_SPINOR_FREQ` 預設 **100 MHz**（defconfig 未覆寫） |
| RX sample delay | `AIC_DEV_QSPI0_DELAY_MODE` 預設 **2**（半周期） |

### 3.4 `display_timing.pixelclock` 不是 QSPI 頻率

`panel_dbi_zc_t1d8bp.c` 裡 `pixelclock = 41328000` 是給 aicfb `screeninfo` 算 porch 用的紙上數字。這片玻璃**沒有** DE 的 HSYNC/VSYNC。實際刷新由 `disp_thread` / JPEG `present()` 的 RAMWR 長度決定。改這個值不會改 PE.8 的 SCLK。

---

## 4. ST77916 QSPI 封包（無 DC 腳）

`lv_st77916.c`：

```c
.cmd_content   = 0x02002C00,  /* 1-wire 寫命令樣板 */
.flush_content = 0x32002C00,  /* 1-wire 標頭 + 4-wire 像素 */
.qspi_lines    = 1,           /* 標頭走 1 線 */
.data_lines    = 4,           /* RAMWR payload 走 4 線 */
.disp_mode     = false,       /* 不用 QSPI display-engine 模式 */
```

`aic_spi_lcd.c` 組命令標頭：

```c
content = (cmd_content & 0xFFFF00FF) | (cmd << 8);
/* → 0x02 00 <CMD> 00 */
```

線上長相（每次 **一顆 CS**）：

```
命令（2A / 2B / init）：
  CS↓  [1-wire 32-bit: 02 00 CMD 00]  [1-wire data…]  CS↑

整幀 RAMWR（0x2C）：
  CS↓  [1-wire 32-bit: 32 00 2C 00]  [4-wire 像素]  CS↑
```

### 4.1 2A / 2B / 2C 是什麼（不是兩塊屏）

這三個數字是 ST77916 的**命令代號**（十六進位），不是 TFT 編號。玻璃把畫面存在自己的 GRAM 裡；每次要換圖，驅動會連送三筆，**每筆都單獨拉一次 CS**（低→送完→高）。

| 命令 | 名字 | 白話 | 分析儀上 |
|------|------|------|----------|
| **0x2A** | CASET（Column Address Set） | 設定這次要寫的**左右／X**範圍（本片 0～359） | **短** CS，只有幾個 byte |
| **0x2B** | RASET（Row Address Set） | 設定這次要寫的**上下／Y**範圍（0～359） | 又一段**短** CS |
| **0x2C** | RAMWR（Memory Write） | 把像素灌進剛設好的視窗 | **長** CS，約 15.5 ms |

可以想成印表機：**2A/2B 先設定列印範圍，2C 才開始噴墨。**  
沒有 2A/2B，玻璃不知道像素填到哪；沒有 2C，座標設了也不會出現新畫面。

所以邏輯分析儀上「兩個短 CS 再加一條很長的 CS」= 一次完整更新（2A → 2B → 2C），不是兩顆屏。四屏時 NY8 必須把這**三次** CS 都接到**同一顆**玻璃。

| 第一個 byte | 意義 |
|-------------|------|
| `0x02` | 1-wire write command（init、CASET、RASET） |
| `0x32` | 4-wire write memory（像素） |

`disp_mode = false`：不用 controller 的 display-refresh 硬模式，軟體每次自己送 0x2C。

**一整幀一次 0x2C**。分帶 window + yield 曾在約 300 幀後卡死 QSPI，不要回去。

ZC 這片 **COLMOD 16-bit 常被玻璃忽略**；實送路徑是 RGB666 + INVON。`tx_buf` 因此按 360×360×3 = **388800** 配（見記憶體說明）。LVGL draw buf 仍是 RGB565。

---

## 5. 軟體路徑

開機 splash 之後，LVGL `spi_dev_flush()` **不再**推像素。圓屏畫面只有兩條入口，最後都進同一個 `disp_thread`：

```
Panel_0..4 GIF                         USB JPEG
  gd_get_frame / gd_render_frame         VE decode
        │                                      │
        ▼                                      ▼
custom_disp_stream_present_argb8888    custom_stream_blit_to_spi
  ARGB8888 → RGB565                    RGB565 crop-copy
        │                                      │
        └──────────┬───────────────────────────┘
                   ▼
        lv_spi_try_lock_draw()     ← 先佔 stream_buf + sync_ready
        lv_spi_present()           ← ramwr_busy=1、present_gen++、翻 stream_idx
                   │
                   ▼
lv_aic_spi.c  disp_thread          RGB565 → RGB666（tx_pp ping-pong）
                   │
                   ▼
aic_spi_lcd_flush_win()
        │  ① aic_mmosie_preamble()     ← USB 4-byte，idle 後才讓 2A CS 下降
        │  ② 0x2A CASET / 0x2B RASET   ← 短 CS，QSPI1
        │  ③ 0x2C RAMWR                ← 長 CS + DMA，4-wire
        ▼
drv_qspi.c  qspi_xfer()
        │  CS↓ 前：aic_mmosie_on_qspi_cs_low()   （只在 length≥256 當 RAMWR）
        │  CS↑ 後：aic_mmosie_on_qspi_cs_high()  （只送 1,1,1,1）
        ▼
HAL QSPI1  @ 0x10401000   IRQ 43   DMA_ID_SPI1
        ▼
PE.5–10
```

握手 API（`lv_aic_spi.h`，僅 `AIC_PANEL_DBI_ZC_T1D8BP`）：

| API | 意義 |
|-----|------|
| `lv_spi_can_present()` | `!ramwr_busy && !draw_locked` 且已過 `ramwr_free_tick` |
| `lv_spi_try_lock_draw()` | 先拿走 `sync_ready`，設 `draw_locked` + `ramwr_busy`，**再**寫 `stream_buf` |
| `lv_spi_unlock_draw()` | blit / present 失敗時還 token |
| `lv_spi_present()` | 若已 lock 不再搶 sem；設 `ramwr_busy`、叫醒 `disp_thread`、翻 `stream_idx` |
| `lv_spi_unstick_ramwr()` | 清 `draw_locked` / `ramwr_busy`，`ramwr_free_tick = now` |

ZC 專用行為：

| 行為 | 為什麼 |
|------|--------|
| RAMWR **同步**（`nonblock=0`） | 非同步曾在 DMA 還沒完就放 CS，下一筆 2A timeout |
| timeout 走 `zc_qspi_recover()` | 重 init `qspi1`，失敗再清 TX/RX FIFO |
| 命令也強制 sync | 避免上一幀 async DMA 還沒完就送 2A |
| `disp_thread` stack 8 KB、prio 20 | 推 RAMWR；JPEG / GIF 由 `present()` 持有 token |
| 串流中不偷 `sync_ready` | 假 complete 會弄丟 present token，畫面停 |
| `present()` 立刻設 `ramwr_busy` | 以前只在 `disp_thread` 醒來後才標 busy，GIF 會寫到還在 DMA 的那塊 `stream_buf` |
| RAMWR 結束後 `ZC_RAMWR_SETTLE_MS`（**8 ms**） | 沒有 TE，下一筆 0x2C 至少離開上一幀掃描一小段 |
| 1 s 無 present 且 `draw_locked` | **不要**重送 GRAM、不要 `leave_one_sync_ready`（會多吐一個 token） |

硬體：`QSPI1_BASE = 0x10401000`，`QSPI1_IRQn = 43`。

---

## 6. 和 MMOSIE 的關係（QSPI 空閒才打）

`target/d12p/demo88-nor/mmosie.c` 掛在 QSPI1 CS 邊上，**只監聽 idx==1**。

一幀畫面（`flush_win` = 2A + 2B + RAMWR）：

1. **RAMWR 之前**：`aic_mmosie_preamble()` 送 USB 來的 4 byte，idle 後才讓第一個 LCD_CS（2A）下降。
2. **2A / 2B 短 CS**：不再送 MMOSIE（殘脈衝會讓 NY8 誤碼）。
3. **RAMWR CS 回到高**：立刻送 `1,1,1,1`（在 `aic_mmosie_on_qspi_cs_high`，不丟 worker）。

發送時必須 **QSPI idle + 整幀 IRQ mask**，否則 5/10 µs 高脈衝會被拉長（`aic_udelay` 開中斷時見過 ~46 µs）。**不可**在 RAMWR 進行中關 IRQ，會餓死 QSPI DMA、花屏，接著 `CMA alloc failed`。

CS hook 在 `drv_qspi.c`：`set_cs(true)` **之前** call low；`set_cs(false)` **之後** call high。

---

## 7. Buffer 與線上時間（360×360）

細節見同目錄 `D125_記憶體配置說明.md`。和 QSPI 直接相關的：

| Buffer | 大小 | 誰用 |
|--------|------|------|
| `tx_buf` / `tx_pp[1]` | 各 388800 CMA | 4-wire RAMWR DMA |
| `splash_keep` | 388800 CMA | 重播開機畫面（硬體 GRAM hold，不是 PNG） |
| LVGL draw ×2 | 各 259200 CMA | RGB565 來源；即 `stream_buf[0/1]` |
| `s_round_gif_blob` | 最大 **2 MB** CMA | `/data/Panel_N` 整檔讀進來再 `gd_open_gif_data` |
| `s_round_gif_prev` | 最大 360×360×4 = **518400** CMA | GIF disposal 3 畫布備份（第一次碰到 disposal 3 才配） |

粗估 50 MHz、4-wire、RGB666 388800 byte：

```
388800 × 8 / 4 / 50e6 ≈ 15.5 ms
```

### 7.1 這條公式每個數字從哪來

這是在算：**把一整張圖的位元，用幾條線、多快的時鐘送完，要幾秒。**

```
時間(秒) = 總位元數 ÷ (每拍送幾位 × 每秒幾拍)
         = (位元組數 × 8) ÷ (資料線數 × SCLK 頻率)
```

| 符號 | 值 | 來源 |
|------|-----|------|
| **388800** | 位元組（byte） | 一張全畫面的 QSPI payload。本片 **360 × 360** 像素，線上是 **RGB666**（每個像素 R、G、B 各 1 byte，只用高 6 bit）→ `360 × 360 × 3 = 388800`。程式裡是 `tx_len = width * height * 3`（`lv_aic_spi.c`）。**不是** LVGL 的 RGB565：那是 `360 × 360 × 2 = 259200`，先在 `stream_buf` 裡，再轉成這 388800 才上線。 |
| **8** | bit／byte | 1 個 byte = 8 個 bit。QSPI 時鐘數的是 **bit**，所以先把 388800 byte 換成 `388800 × 8 = 3 110 400` bit。 |
| **4** | 資料線數 | `LV_SPI_BUS_WIDTH_4`：像素走 **4-wire**（D0–D3）。每一個 SCLK 同時送 4 bit，所以總 bit 要再除以 4 才是「要幾個 clock」。命令標頭仍是 1-wire，這條公式**只算 0x2C 的像素段**。 |
| **50e6** | Hz（每秒幾個 clock） | 科學記號：`50e6 = 50 × 10^6 = 50 000 000` = **50 MHz**。就是 `lv_aic_spi.c` 向 QSPI1 要的 `max_hz`（PE.8 的 SCLK）。 |

代入：

```
3 110 400 bit ÷ 4 線 = 777 600 個 SCLK
777 600 ÷ 50 000 000 Hz = 0.015552 秒 ≈ 15.5 ms
```

沒算進去、所以實測會長一點的：2A/2B 兩段短 CS、0x32 標頭 32 bit、MMOSIE preamble、CS 升降、`ZC_RAMWR_SETTLE_MS`（8 ms）。單屏軟體目標大約 30 fps，是「15.5 ms 像素 + 其餘開銷」還能擠進約 33 ms 一幀。

若改成 1-wire（`/ 1` 而不是 `/ 4`），同樣 388800 byte 會變成約 **62 ms**，不能當量產。

SCLK 改 100 MHz 時分母改成 `100e6` → 像素段約 **7.8 ms**。能不能真的跑 100 MHz 見 **§3.2**（SoC 可以，本玻璃／排線沒保證）。

---

## 8. Init 序列

真正寫進玻璃的是 `lv_st77916.c` 的 **ST77916_BOE 1.8'' 360×360 B3 V1.4_20250331_4_QSPI_OK**，不是 `panel_dbi_zc_t1d8bp.c` 裡那份 `zc_t1d8bp_commands[]`（replace 模式下 `panel_enable()` 直接 return，不執行 DBI command list）。

開機 log 應對到：

```text
zc-t1d8bp: SPI1 QSPI replace mode (PE5-10, RST PE.4)
zc-t1d8bp: SPI1 initial.c V1.4 20250331 QSPI_OK (PE5-10, RST PE.4)
```

---

## 9. 改配置時改哪裡

| 想改什麼 | 改哪 |
|----------|------|
| LCD SCLK | `lv_aic_spi.c` `.max_hz`（現況 50e6）。若試 100 MHz：見 §3.2，並把 `CONFIG_AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ` 配成 **整除**（建議 200e6 ÷2，不要 1:1 當第一刀） |
| LCD 腳位 | `target/d12p/demo88-nor/pinmux.c` `AIC_USING_QSPI1` 區塊 |
| RST / 背光 | `lv_st77916.c` `ST77916_RST_PIN`（PE.4）、`CONFIG_AIC_PANEL_ENABLE_GPIO` |
| 命令/像素編碼 | `lv_st77916.c` `cmd_content` / `flush_content` / `data_lines` |
| Init table | `lv_st77916.c` `lv_spi_panel_enable()`（ZC 分支） |
| NOR 頻率 | `CONFIG_AIC_QSPI0_DEVICE_SPINOR_FREQ`、`CONFIG_AIC_DEV_QSPI0_MAX_SRC_FREQ_HZ` |
| 改回 1-wire SPI | **不要**在這片玻璃上開 `LV_SPI_BUS_WIDTH_1`；沒 DC 腳且幀時間不可用 |
| QSPI1 誤當 NOR | 不要開 `AIC_QSPI1_DEVICE_SPINOR` |
| RAMWR 後等待 | `lv_aic_spi.c` `ZC_RAMWR_SETTLE_MS`（現況 8） |
| 開機 GIF 秒數 | `custom.c` `CUSTOM_ROUND_BOOT_GIF_MS`（現況 5000） |
| 使用者 GIF 停留 | `custom.c` `CUSTOM_ROUND_GIF_DWELL_MS`（現況 8000） |
| 工廠開機 GIF | `target/d12p/demo88-nor/pack/data/Panel_0/Panel_000.gif`（打進 `data.lfs`） |

---

## 10. 相關原始檔

| 檔案 | 內容 |
|------|------|
| `target/d12p/demo88-nor/pinmux.c` | QSPI0/1 腳、RST、PD.26 |
| `target/configs/d12p_demo88-nor_rt-thread_helloworld_zc_t1d8bp_defconfig` | 開關與 QSPI0 200 MHz |
| `target/d12p/common/Kconfig.board` | QSPI1 來源預設 100 MHz、4-wire |
| `packages/artinchip/lvgl-ui/aic_widgets/aic_spi/lv_aic_spi.c` | 50 MHz、qspi1、tx ping-pong、present 握手 |
| `packages/artinchip/lvgl-ui/aic_widgets/aic_spi/lv_aic_spi.h` | `ramwr_busy` / `draw_locked` / `ramwr_free_tick` |
| `packages/artinchip/lvgl-ui/aic_widgets/aic_spi/lv_st77916.c` | V1.4 init、0x02/0x32、RST PE.4 |
| `bsp/peripheral/spilcd/aic_spi_lcd.c` | 組包、sync RAMWR、preamble、recover |
| `bsp/artinchip/drv/qspi/drv_qspi.c` | 控制器、DMA、CS hook |
| `bsp/artinchip/drv/display/panel/panel_dbi_zc_t1d8bp.c` | replace 模式、screeninfo |
| `target/d12p/demo88-nor/mmosie.c` | 依 QSPI1 CS 送 1-wire |
| `bsp/artinchip/sys/d12p/include/aic_soc.h` | `QSPI1_BASE`、IRQ |
| `packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom.c` | Panel_0 輪播、GIF timer、disposal 3 |
| `packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom_disp_stream.c` | ARGB→565、JPEG lock-then-blit |
| `packages/artinchip/lvgl-ui/aic_demo/ui_builder/custom/custom_Image.c` | `/data/Panel_0` mkdir、讀 `Panel_00N.gif` |
| `packages/artinchip/lvgl-ui/aic_demo/ui_builder/screen.c` | ZC splash **隱藏** PNG，不載 `zc_t1d8bp_185_small.png` |
| `packages/artinchip/lvgl-ui/lvgl_v9/lvgl/src/libs/gif/gifdec.c` | 開檔一律 `memset(frame, bgindex)` |
| `target/d12p/demo88-nor/pack/image_cfg.json` | `data` = **8m** → `data.lfs` |
| `target/d12p/demo88-nor/pack/data/Panel_0/Panel_000.gif` | 工廠開機 GIF 來源 |

---

## 11. 開機畫面：Panel_0 GIF（2026-09）

靜態手錶 PNG **不再**當開機 UI。`screen_create_zc_t1d8bp_splash()` 仍建 `image_8`，但加上 `LV_OBJ_FLAG_HIDDEN`，也不 `lv_img_set_src(zc_t1d8bp_185_small.png)`。`custom_disp_apply_round_splash_layout()` 同樣把圖藏起來。`screen_reload_splash_image()` 忽略傳入的 PNG src。

真正畫面由 `custom_round_boot_start()` → `custom_round_carousel_poll()` 播 GIF，路徑與 JPEG 相同（`custom_disp_stream_acquire()` + `lv_spi_present()`）。**不要**載 UIBuilder `screen_main_1`（1280×800 會撐爆 MPP cache）。

### 11.1 LittleFS 目錄

| 路徑 | 誰寫 | 檔名 |
|------|------|------|
| `/data/Panel_0/Panel_000.gif` | 工廠包進 `data.lfs` | 開機 / 待機 |
| `/data/Panel_1/Panel_001.gif` … `/data/Panel_4/Panel_004.gif` | USB F9-F2-F5 存檔 | 使用者輪播 |

- `CUSTOM_FLASH_BOOT_PANEL = 0`，`CUSTOM_FLASH_PANEL_COUNT = 4`（使用者槽仍是 1–4）。
- `custom_image_init()` 會 `mkdir` `Panel_0` 與 `Panel_1`–`Panel_4`。
- `custom_image_panel_gif_path()` / `size()` / `load_panel_gif()` 接受 **0–4**。
- USB 存檔**只准** 1–4，不能覆寫工廠 `Panel_0`。

### 11.2 輪播時序

`CUSTOM_ROUND_SINGLE_SCREEN_CAROUSEL=1`（單顆圓屏轉資料夾，不是四顆各播自己的）：

| 階段 | 時間 | 行為 |
|------|------|------|
| 開機 `s_round_boot_hold=1` | `CUSTOM_ROUND_BOOT_GIF_MS` = **5000** | 只播 Panel_0 |
| 5 s 後若 Panel_1–4 有檔 | `CUSTOM_ROUND_GIF_DWELL_MS` = **8000** | 1→2→3→4 循環 |
| 5 s 後若 1–4 皆空 | — | **繼續** Panel_0（rewind + 清 canvas） |
| 使用者槽播完且沒下一檔 | — | 退回 Panel_0 |
| USB 正在收檔 | debounce **700 ms** | `custom_round_carousel_notify()` 清 `boot_hold`，收完再從使用者槽開始 |

換檔前必須 `lv_spi_can_present()`，避免 RAMWR 中途改解另一份 GIF。

### 11.3 燒錄

`image_cfg.json` 的 `data` 分區 **8m**，檔案是 `data.lfs`。工廠 GIF 要放在：

```text
target/d12p/demo88-nor/pack/data/Panel_0/Panel_000.gif
```

只燒 `os`、不重包 `data.lfs` 的話，板上沒有 Panel_0，開機就不會播這段。整包燒錄必須含 data。

---

## 12. Present 握手（對破圖 / 撕裂）

這片玻璃沒有 TE，整幀 RGB666 ≈ **15.5 ms**，掃描也大約一個 frame。軟體能擋的是「寫到還在送的 buffer」和「上一幀沒送完就解下一幀」；**對不齊 VBLANK 的水平撕裂無法消光**。

### 12.1 狀態旗標（`struct lv_spi_dev`）

| 旗標 | 誰設 / 誰清 | 作用 |
|------|-------------|------|
| `ramwr_busy` | `try_lock_draw()` / `present()` 設 1；`disp_thread` RAMWR 完、`unlock`、`unstick` 清 0 | `can_present()` 的主閘 |
| `draw_locked` | `try_lock_draw()` 設 1；`present()` 成功或 `unlock_draw()` 清 0 | 表示「這塊 `stream_buf` 正在被 GIF/JPEG 畫，別人不准寫」 |
| `ramwr_free_tick` | RAMWR / splash restore / 1 s hold-GRAM 之後 = `lv_tick_get() + 8` | 8 ms 內 `can_present()` 仍為 0 |
| `present_gen` | 只有 `lv_spi_present()` 加 | `disp_thread` 用來分辨真 present 與 leftover `display_sem` |
| `stream_idx` | `present()` 排隊後互斥翻 | 下一筆 blit 寫另一塊 RGB565，不覆蓋 in-flight |

### 12.2 正確順序

```
can_present()?                         ← 換檔、JPEG 解碼前先問
     │
try_lock_draw()                        ← 拿走 sync_ready，busy=1，locked=1
     │
寫 stream_buf[stream_idx]              ← GIF ARGB→565 或 JPEG CPU copy
     │
present()                              ← locked 路徑不再 take sem
     │    ramwr_busy 保持 1
     │    present_gen++
     │    give display_sem
     │    stream_idx ^= 1
     ▼
disp_thread
     RGB565 → tx_pp RGB666
     zc_ramwr_full()                   ← 一整幀一次 0x2C
     ramwr_busy = 0
     ramwr_free_tick = now + 8
     give sync_ready
```

失敗時：`unlock_draw()` 還 token。`present()` 若已吃掉 lock，回 0 時呼叫端再 `unlock` 也沒關係（`!draw_locked` 直接 return）。

### 12.3 為什麼舊路徑會破圖

| 舊行為 | 結果 |
|--------|------|
| `present()` 給 `display_sem` 之後才在 `disp_thread` 設 `ramwr_busy` | 中間空窗，下一幀 GIF 寫到還在 RGB565→666 的那塊 |
| GIF timer：`gd_get_frame()` 成功、`present()` 失敗就 `return` | 解碼器已往前，下一 tick 再 `get_frame` = **跳幀**，disposal 錯，看起來像破圖 |
| GIF 與 JPEG 都 `get_draw_info()` 再 blit，最後才 `present()` | 兩路可同時寫同一 `stream_buf`，送出去是半幀 |
| `gif_open()` 只在 `bgindex != 0` 時清 `frame` | `bgindex==0` 的第一幀 `dispose()` 把垃圾畫上 canvas |
| gifdec disposal 3 是 no-op | 「還原上一張畫布」沒做，透明疊圖錯層 |

### 12.4 現況對策

1. **`present()` 立刻 `ramwr_busy=1`**，再叫醒 `disp_thread`。
2. **`try_lock_draw()`**：GIF `present_argb8888()`、JPEG `blit_to_spi()` 之前先鎖。JPEG 解碼仍必須等 `can_present()`（註解：RAMWR 期間跑 VE 會卡死 QSPI / WDT）。GIF LZW 從 CMA blob 解，**可以**在 RAMWR 期間先解好。
3. **ready-frame**：`s_round_gif_frame_ready=1` 表示 canvas 已是要送的那幀；`present()` 失敗不呼叫下一次 `gd_get_frame()`。
4. **`ZC_RAMWR_SETTLE_MS=8`**：`disp_thread` 正常 RAMWR、1 s hold-GRAM、restore splash 都寫 `ramwr_free_tick`。
5. **1 s timeout**：若 `draw_locked` 只踢 WDT、`continue`，不重送、不補 `sync_ready`。
6. **stale wake**：清 `ramwr_busy`，避免 leftover sem 把 busy 卡死。

---

## 13. GIF 解碼與送圖規則

入口：`custom.c` 的 `custom_round_qspi_gif_timer_cb()`（10 ms timer），不是 `lv_gif` widget（那個會走 LVGL flush，splash 之後已被丟掉）。

```
if (!frame_ready) {
    等到 gce.delay（最少 20 ms）
    若上一幀 disposal==3 → 把備份矩形還原到 canvas
    gd_get_frame()                    // 內含上一幀 dispose()
    若結束 → rewind、清 canvas、再 get 第一幀
    若本幀 disposal==3 → 先備份 canvas 矩形（render 前）
    gd_render_frame(canvas)
    frame_ready = 1
}
present_argb8888(canvas)              // 內部 try_lock → blit → present
成功才 frame_ready = 0、記下 last tick
```

| 規則 | 實作 |
|------|------|
| 送失敗不跳幀 | `frame_ready` 保住 canvas，下一 tick 只重送 |
| 開檔清乾淨 | `gd_open` 後 `memset(frame, bgindex)` + `custom_round_gif_reset_canvas()` |
| disposal 2 | gifdec 把矩形填背景（原本就有） |
| disposal 3 | `s_round_gif_prev` 備份 / 還原矩形；gifdec case 3 仍是 no-op |
| rewind / 換檔 | `need_restore=0`、`frame_ready=0`、canvas 填背景 |
| blob 不放回系統 heap | 2 MB CMA 常駐；以前在 2 MB `MEM_DEFAULT` 上 free/realloc 會踩 QSPI FB |
| 尺寸上限 | 寬高須 >0 且 ≤ `CUSTOM_DISP_IMG_W/H`（360） |

`gifdec.c`（LVGL v9）開檔改為**一律** `memset(gif->frame, bgindex, width*height)`。這會影響所有走這份 gifdec 的 GIF，不只圓屏。

ARGB→RGB565 在 `custom_disp_stream_present_argb8888()`：先 `try_lock`，`memset` 目標為黑，再依比例置中。目前**不理 alpha**（disposal 2 透明像素已是背景色）。

---

## 14. 已知限制與除錯

| 現象 | 原因 | 軟體能不能消 |
|------|------|----------------|
| 動畫缺一塊 / 錯層 / 花一幀 | 跳幀、兩路搶 `stream_buf`、disposal 3、第一幀垃圾 | **應已擋**（§12、§13） |
| 一條水平撕裂、上下兩半不同幀 | RAMWR 寫 GRAM 時面板正在掃同一塊；無 TE | **不能完全消**。8 ms settle 只降低「上一筆還沒掃完又開下一筆 0x2C」 |
| 畫面停、log `RAMWR busy, drop` | token 被佔或 settle 還沒到 | 正常丟幀；GIF 會重送同一張。卡超過 1 s 走 `lv_spi_unstick_ramwr()` |
| log `stale wake` | leftover `display_sem`、`present_gen` 沒加 | 不該再偷走真 present |
| 開機仍是黑或靜態圖 | 沒燒 `data.lfs` / 沒有 `Panel_000.gif` / 仍看到 PNG | 查 pack 與 `screen.c` 是否還 `set_src` PNG |
| JPEG 與 GIF 同時搶屏 | USB 進圖會 `custom_round_on_jpeg_stream()` 暫停 GIF timer | JPEG 結束後靠 carousel poll 再播 |

硬體上限（不要再往軟體堆假 TE）：

- 沒有 TE 腳，`disp_thread` **不等** TE IRQ（非 ZC 的 SPI 屏才 `aicos_wqueue_wait`）。
- 整幀 0x2C 約 15.5 ms，60 Hz 掃描約 16.7 ms，幾乎一定會跟掃描重疊。
- **不要**改回分帶 window + yield（約 300 幀後 QSPI 卡死）。
- **不要**在 RAMWR 期間關 IRQ 打 MMOSIE（餓死 DMA → 花屏 → `CMA alloc failed`）。

相關記憶體數字見同目錄 `D125_記憶體配置說明.md`。
