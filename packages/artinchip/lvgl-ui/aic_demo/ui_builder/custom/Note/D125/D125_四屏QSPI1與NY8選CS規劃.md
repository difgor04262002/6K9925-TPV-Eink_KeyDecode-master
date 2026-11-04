# D125 四屏規劃：單 QSPI1 + NY8 選 CS

> 適用平台：D125 / SDK 晶片名 **D12P**（`CONFIG_PRJ_CHIP="d12p"`）
> 適用板卡：`demo88-nor`
> 適用面板：ZC-T1D8BP-012（ST77916，360 × 360，QSPI RGB666，**無 DC/RS、無 TE**）
> NY8：NY8A051H（6 pin 小 MCU），吃 PD.26 的 1-wire 4-byte，GPIO 當各顆 `LCD_CS`
> 日期：2026-09-17
>
> QSPI1 腳位、封包、單屏 present 握手見同目錄 `D125_QSPI配置說明.md`。  
> CMA / blob 大小見 `D125_記憶體配置說明.md`。
>
> **結論先講**：電氣上可以用「一條 QSPI1 並聯資料線 + PD.26/NY8 選 CS」驅動四顆玻璃；四顆可以**同時各顯示不同畫面**（靠各顆 GRAM）。四顆**同時各播不同 GIF、還維持單屏那種 ~30 fps、保證不停頓**做不到——bus 一次只能寫一顆。現行韌體也還是單屏路徑，接上四顆不會自動變成四路獨立播放。

---

## 1. 要解決什麼問題

產品端希望：

- 只用 **QSPI1**（不開 QSPI2）
- **CLK / D0–D3 / RESET** 並聯到 4 塊 ZC-T1D8BP
- CS **不**用 SoC 再出三條硬體 CS，改由 **NY8 GPIO** 選哪一顆吃這次交易
- 選屏資料走現成的 **PD.26 MMOSIE 4-byte**
- 四塊屏各播不同 GIF，看起來要同時、不要黑、不要停住

這和現行「單顆圓屏 + `CUSTOM_ROUND_SINGLE_SCREEN_CAROUSEL` 在同一顆玻璃上輪 Panel_1–4 檔案」不是同一件事。

| | 現行韌體 | 本規劃目標 |
|--|----------|------------|
| 物理屏 | 1 | 4 |
| Panel_1–4 | 同一顆玻璃換檔輪播 | 四顆各播自己的資料夾 |
| QSPI | 一條，CS = `PE.5` 直出 | 一條，`PE.5` 當 strobe，NY8 拆成四條 CS |
| 同時四張圖 | 不適用 | GRAM 撐住，可以 |
| 四路 30 fps GIF | 單屏約 30 fps | **不行**（見 §6） |

---

## 2. 硬體架構

### 2.1 並聯的訊號（四顆共用）

| 訊號 | SoC / 來源 | 四顆怎麼接 |
|------|------------|------------|
| `SPI_CLK` | QSPI1 `PE.8` | 並聯（板子上經 TXB0106 往下一片 daisy-chain） |
| `SPI_D0`–`D3` | `PE.7 / PE.6 / PE.10 / PE.9` | 同上 |
| `LCD_RESET` | `PE.4` | 並聯（四顆一起復位） |
| 背光 | `PF.0` 等 | 可共用或各開，與選 CS 無關 |
| `LCD_CS` | **不要四顆直接並在 `PE.5`** | 各顆 CS 由該片 NY8 GPIO 出 |

`PE.5`（QSPI 硬體 CS）仍必須存在：控制器用它框住「1-wire 標頭 + 4-wire 像素」整筆交易。NY8 的工作是：**鎖存選屏 nibble，再跟 `PE.5` 同步去拉低對應那顆的 CS**。

建議邏輯（與邏輯分析儀一致）：

```
PD.26  ──1-wire 4-byte──► NY8 鎖存 0111 / 1011 / 1101 / 1110
PE.5   ──QSPI CS strobe──► NY8 當 enable（低有效）
                              │
              ┌───────────────┼───────────────┐
              ▼               ▼               ▼
         TFT1_CS         TFT2_CS  …      TFT4_CS
         (僅選中時跟隨 PE.5 脈衝；其餘維持高)
```

未選中的 CS 必須拉高，該顆忽略 CLK/資料。ST77916 在 CS 高時 GRAM **繼續顯示上一幀**，這就是「看起來四顆同時有畫面」的原因。

### 2.2 板級（Panel with Buffer）

`ZC-T1D8BP-012 Panel with Buffer`（2026-08-28）：

- `U1` NY8A051H 6 pin：吃 `NY8_CS`（來自上游 1-wire / CS 鏈），`#CS` 給本片 panel
- `U2` TXB0106：D0–D3、SCLK 緩衝後 `DOUT*` / `SCLK_O` → `J3`「To Next NY8」
- `CN1` 接 ZC-T1D8BP；`J2` 接上一片、`J3` 接下一片

四片是 **daisy-chain**，不是星狀四條等長線。50 MHz Quad 過四級 TXB0106 已在 datasheet 邊緣（§2.3）；**200 MHz 不可行**。

### 2.3 TXB0106 能不能讓每一塊 TFT 跑 200 MHz？

**不能。** 這顆是 6-bit **自動雙向電平轉換**（TI SCES709C），不是高速 clock buffer。TI 官網標 **Data rate (max) = 100 Mbps**，而且那是理想小負載；**不是** 200 MHz SCLK，更不是四顆各 200 MHz。

板子 `U2` 把 SCLK、D0–D3 轉到下一片（`J3`）。CLK 是方波，頻率 F 時高／低各約 `1/(2F)`：

| SCLK | 週期 | 高／低脈寬（50% duty） | 對 TXB0106（3.3 V 時 `tw` min **10 ns**、100 Mbps） |
|------|------|------------------------|------------------------------------------------------|
| 50 MHz（現況） | 20 ns | **10 ns** | 剛好踩在 `tw` 下限，單屏能過、已沒餘裕 |
| 100 MHz | 10 ns | **5 ns** | 脈寬只有規格的一半，**不合格** |
| **200 MHz** | **5 ns** | **2.5 ns** | 脈寬只有規格的 **1/4**，one-shot 也來不及 |

Datasheet 裡跟 200 MHz 直接衝突的點：

| 規格 | 值 | 200 MHz 為什麼不行 |
|------|-----|-------------------|
| 最大資料率 | **100 Mbps**（3.3 V／5 V 那一欄才到 100；1.2 V 只有 20 Mbps） | 200 MHz SDR clock ≈ **200 Mbps／線**，超過標稱上限一倍 |
| 資料脈寬 `tw` min | 100 Mbps 時 **10 ns** | 200 MHz 半週期 2.5 ns |
| One-shot（加速邊沿） | 大約開 **10 ns** | 200 MHz 一個週期只有 5 ns，one-shot 還沒關就來下一沿 → 振盪／誤觸發（TI 自己警告） |
| 穩態輸出阻抗 | 每 I/O 串 **約 4 kΩ**（自動方向用） | 推不動 FPC＋下一顆 TXB＋panel 輸入電容；邊沿靠 one-shot，穩態幾乎沒驅動 |
| 建議負載 | 集總電容 **≤ 70 pF**、走線要短 | daisy-chain 的連接器＋排線很容易超過；四級串更差 |
| 下拉／上拉 | 建議 **≥ 50 kΩ**，最好不要外掛 | 強上拉會跟 4 kΩ 打架 |

「每一塊都 200 MHz」還有兩層誤解：

1. **TXB0106 不會變成四條獨立 200 MHz bus。** 仍是一條 QSPI1，四顆輪流吃同一組 CLK/資料。轉換器再快，四顆也不會同時各寫 200 MHz。
2. **最後一顆要經過四級 TXB0106。** 每級數 ns 延遲＋失真疊加；200 MHz 週期 5 ns，第四片 CLK 會爛掉。就算第一片僥倖，第四片也不會「一樣 200 MHz 乾淨」。

D12P QSPI HAL 上限 200 MHz 只代表 **SoC 腳可以出那麼快**，出了 PE.8 之後第一顆 TXB0106 就把頻寬吃掉。ST77916 本身也沒有本專案 200 MHz 的驗證。

若將來要提高 clock，不要指望 TXB0106：應改 **單向高速 buffer**（例如 TXU／AXC 系列，TI 也建議單向訊號不要用 TXB），或縮短為星狀、每片獨立驅動。那是改板，不是改 `max_hz`。

**結論：** TXB0106 適合現況這種 **3.3 V、約 50 MHz、短線** 的轉接；**不能**讓每塊 TFT 跑 200 MHz。50 MHz 已經在這顆 IC 的脈寬上限附近。

#### 有沒有能到 200 MHz 的 buffer IC？

**有，但那是 clock／單向 CMOS fanout，不是 TXB 這一類自動雙向轉換；而且換成那些 IC 也無法讓 ST77916 玻璃吃 200 MHz。**

| 角色 | 不要用 | 可以考慮（3.3 V） | 官方能力 |
|------|--------|-------------------|----------|
| **SCLK 扇出** | TXB0106、TXS、一般 74LVC245 | **CDCLVC1104**（1:4）、CDCLVC1108（1:8） | 3.3 V 時 **fmax = 250 MHz**，腳對腳 skew 小於 50 ps。這才是「200 MHz clock buffer」 |
| **D0–D3 單向** | TXB0106（約 4 kΩ + one-shot） | **SN74AVC8T245**／**SN74AXC4T774**（DIR 綁死 Host→Panel） | AVC：兩軌 ≥ 1.8 V 時 **320 Mbps**（輕載 ~15 pF）。200 MHz 方波半週期 2.5 ns，320 Mbps 只相當約 **160 MHz clock**，**200 MHz 資料線沒有舒適餘裕** |
| **想 pin-to-pin 換掉 TXB** | — | **TXU0104**（固定方向，TI 建議單向訊號用 TXU 不要用 TXB） | 最大 **200 Mbps**，且是 3.3↔5 V 那一欄；**不是** 200 MHz SCLK |

板級要能接近 200 MHz，走線也要改，不能沿用現在的四級 daisy-chain：

1. **星狀 fanout**：SoC 一條 CLK 進 CDCLVC1104，四路等長出去到四片；資料同樣每片一顆單向 buffer，不要串四級。
2. **每路串 22–33 Ω** 靠近驅動端，FPC 當傳輸線看待（200 MHz 波長在 FR4 大約 70 cm 量級，十幾公分排線已經會反射）。
3. CLK 與 D0–D3 **等長**，否則 2.5 ns 半週期裡 setup/hold（ST77916 寫入 TSDS/TSDH min **7 ns**）會先爆，跟 buffer 速度無關。

就算 buffer 夠快，玻璃也不吃 200 MHz。Sitronix ST77916 SPEC：

| 項目 | 規格 | 換算 |
|------|------|------|
| QSPI 寫入 clock 週期 `TSCYCW` min | **16 ns** | 理論上限 **62.5 MHz** |
| SCL 高／低脈寬 min | **7 ns** | |
| 腳位說明 RDXP/SCLP | **Clock signal (Max = 50 MHz)** | vendor 明確寫死 |
| 讀取 clock | 週期 min 150 ns | ~6.7 MHz，本專案幾乎不讀 |

所以：**IC 可以 200 MHz ≠ 這塊 TFT 可以 200 MHz。** 換 CDCLVC 是為了把現況 50 MHz 的 CLK 扇出做得乾淨、或將來若換更快的玻璃；**不能**靠換 buffer 讓 ZC-T1D8BP 每片跑 200 MHz。四屏仍共用一條 QSPI1，buffer 再快也不會變成四條獨立 200 MHz bus。

### 2.4 為什麼 CS 不能改成「純 GPIO、不理 PE.5」

現行 RAMWR 線上長相（`lv_st77916` / `aic_spi_lcd`）：

```
CS↓  [1-wire 32-bit: 32 00 2C 00]  [4-wire 388800 B]  CS↑
```

一次 `flush_win` 其實是 **三次** CS：`0x2A` CASET、`0x2B` RASET、`0x2C` RAMWR。標頭與 payload 必須在**同一段 CS 低**裡。若用 GPIO 假裝 CS、與 QSPI 內部 `PE.5` 對不齊，0x32 標頭會和像素拆開，玻璃會花屏。

因此：**選屏 = NY8；時序框 = 仍是 QSPI 的 PE.5。**

---

## 3. PD.26 4-byte 規則（現行就有）

MMOSIE 在 `target/d12p/demo88-nor/mmosie.c`：PD.26 GPIO 1-wire，byte 內 MSB first。

編碼（值是 µs，註解裡的 ms 是舊的）：

| 位元 | 高 | 低 |
|------|----|----|
| 0 | 5 µs | 10 µs |
| 1 | 10 µs | 5 µs |

一幀畫面（`flush_win` = 2A + 2B + RAMWR）：

1. **RAMWR 之前**（實際在第一次 2A CS 下降前）：送 USB/選屏 payload **4 byte**，idle 後才讓 `LCD_CS` 下降。
2. **2A / 2B / RAMWR 期間不要再送**（殘脈衝會讓 NY8 誤碼）。
3. **RAMWR 的 CS 回到高**：立刻送 idle **`1,1,1,1`**（關中斷、不丟 worker）。

選屏 nibble（每個 byte 只當 0/1 用，`aic_mmosie_set_frame()` 會正規化）：

| 選哪顆 | 4-byte | USB F9-F2-F5 參數 | 低有效時意義 |
|--------|--------|-------------------|--------------|
| TFT 1 | `0,1,1,1` | `01` | 只拉第 1 顆 CS |
| TFT 2 | `1,0,1,1` | `02` | 只拉第 2 顆 CS |
| TFT 3 | `1,1,0,1` | `03` | 只拉第 3 顆 CS |
| TFT 4 | `1,1,1,0` | `04` | 只拉第 4 顆 CS |
| 全不選 | `1,1,1,1` | RAMWR 後自動送 | 四顆 CS 都高，GRAM 撐住 |

韌體預設 `s_frame[] = { 0, 1, 1, 1 }`（上電先打給第 1 顆）。  
USB：`F9 F9 F2 F5  [FA|FB|FC|FD]  [01..04]  F9 F9 F2 F5` → `aic_mmosie_set_frame(s_mmosie_map[idx-1])`，並 `custom_image_set_flash_panel(idx)`（存檔仍是 Panel_1–4）。

**注意**：註解寫「Bits go out while QSPI1 CS is low」，實作是 **CS 低之前** 送 preamble、**RAMWR CS 高之後** 送 `1111`。NY8 應以「preamble 鎖存、PE.5 當 strobe」為準，不要做成「CS 低才開始解 4-byte」。

---

## 4. 邏輯分析儀上看到的一幀

與單屏量測一致，四屏時每一顆的一次更新也應長這樣（只是 NY8 把 CS 導到不同玻璃）：

```
時間軸 ──►

PD.26   ████ preamble 4-byte ████░░░░░░░░░░░░░░░░░░░░░░████ 1111 ████
PE.5    ░░░░░░░░░░░░░░░░░░░░░░░█ 2A █░█ 2B █░████ RAMWR ████░░░░░░░░░
CLK/Dx  ░░░░░░░░░░░░░░░░░░░░░░░░短命令░░░░░░░長 4-wire 像素░░░░░░░░░░
NY8 CS* ░░░░░░░░░░░░░░░░░░░░░░░（跟隨 PE.5，僅選中那顆）░░░░░░░░░░░░
```

- preamble 期間 `LCD_CS` 為高，QSPI idle（MMOSIE 才能關 IRQ 打 5/10 µs）。
- 兩個短 CS = **2A 然後 2B**（設寫入視窗）；長 CS = **2C RAMWR** ≈ **15.5 ms**（50 MHz、360×360×3）。見下方 §4.1。
- `1111` 在長 CS 結束後立刻出現。此時四顆 CS 都應為高，畫面停在各顆最後一幀。

### 4.1 2A / 2B / 2C 是什麼（不是兩塊屏）

這三個是 ST77916 的**命令代號**（十六進位 `0x2A`、`0x2B`、`0x2C`），跟「第 2 顆 TFT」無關。玻璃把畫面存在自己的 GRAM；每次要換圖，`aic_spi_lcd_flush_win()` 會連送三筆，**每筆都單獨拉一次 CS**。

| 命令 | 名字 | 白話 | 分析儀上 |
|------|------|------|----------|
| **0x2A** | CASET（設欄／X） | 告訴玻璃：這次要寫的**左右**範圍（0～359） | 第一段**短** CS，只有幾個 byte |
| **0x2B** | RASET（設列／Y） | 告訴玻璃：這次要寫的**上下**範圍（0～359） | 第二段**短** CS |
| **0x2C** | RAMWR（寫記憶體） | 把 360×360 像素灌進剛設好的視窗 | **長** CS，整張 RGB666 |

可以想成印表機：**2A/2B 先設定列印範圍，2C 才開始噴墨。**

```
一次更新（同一顆屏、同一條 QSPI1）：

  CS↓  送 2A（X 起訖）  CS↑     ← 短
  CS↓  送 2B（Y 起訖）  CS↑     ← 短
  CS↓  送 2C（像素）    CS↑     ← 長，約 15.5 ms
```

四屏時 NY8 必須把這**三次**脈衝都導到**同一顆**玻璃：不能 2A 給第 1 顆、2C 給第 2 顆，否則座標跟像素對不上，會破圖或不顯示。

NY8 韌體必須：

1. 在 preamble 結束後就鎖存 nibble，**不要**等 CS 低才解。
2. 同一個 nibble 撐過 **三次** PE.5 脈衝（2A、2B、RAMWR），中間不要當新命令、也不要當成 idle。
3. GPIO 輸出要跟 PE.5 **同步脈衝**，不能從 preamble 一路靜態拉低到 `1111`（三次 SPI 命令會黏成一包）。
4. 收到 `1,1,1,1` 後四條 CS 都高。

---

## 5. 「同時顯示」實際是什麼

ST77916 有自己的 GRAM。CS 拉高之後面板繼續掃自己的 RAM，**不需要** QSPI 一直餵。

因此：

- 對 TFT1 送完一幀 GIF → `1111` → 去寫 TFT2 …
- 四顆可以**同時**各顯示不同靜態／上一幀動畫
- 這不是四條 QSPI 並行，是 **時分 + GRAM 撐住**

「不顯示」只會發生在：

- 該顆從未 init、或從未成功 RAMWR（RESET 並聯後只 init 了第 1 顆最常見）
- NY8 沒把 CS 跟到 PE.5（選錯或一直為高）
- 選屏 nibble 在 2A/2B/RAMWR 中間被改掉或被殘脈衝刷掉

「停頓」指的是**動畫**不再更新：沒輪到的那顆會停在最後一幀，但通常**不會黑掉**。

---

## 6. 時間預算（能不能四路 GIF 都不頓）

單筆全畫面 RGB666 @ 50 MHz、4-wire：

```
388800 × 8 / 4 / 50e6 ≈ 15.5 ms
```

### 6.1 這條公式每個數字從哪來

這是在算：**一整張圖有多少 bit，用 4 條線、50 MHz 時鐘送完要多久。** 完整拆解也寫在 `D125_QSPI配置說明.md` §7.1。

| 符號 | 值 | 意思 |
|------|-----|------|
| **388800** | byte | `360 × 360 × 3`。360×360 顆像素，線上 RGB666 **每像素 3 byte**（R、G、B 各一，各用高 6 bit）。LVGL 畫布是 RGB565（每像素 2 byte = 259200），送玻璃前會轉成這 388800。 |
| **8** | bit／byte | 1 byte = 8 bit。時鐘數的是 bit，所以先 `388800 × 8 = 3 110 400` bit。 |
| **4** | 線數 | Quad SPI：D0–D3 同一個 clock 各送 1 bit，等於每拍送 4 bit。 |
| **50e6** | Hz | `50e6 = 50 × 10^6 = 50 000 000` = **50 MHz**（QSPI1 `max_hz`，腳 PE.8）。 |

```
3 110 400 ÷ 4 = 777 600 個 clock
777 600 ÷ 50 000 000 = 0.015552 秒 ≈ 15.5 ms
```

這 **15.5 ms 只含 0x2C 像素**，不含 2A/2B、標頭、PD.26 preamble、settle。

四顆輪流滿幀（理想、不計 2A/2B、MMOSIE、settle）：

```
4 × 15.5 ms ≈ 62 ms  → 每顆最多約 16 fps
```

若仍套單屏的 `ZC_RAMWR_SETTLE_MS = 8`：

```
4 × (15.5 + 8) ms ≈ 94 ms  → 每顆約 10 fps
```

| 內容 | 四屏 round-robin 夠不夠 |
|------|-------------------------|
| 靜態圖 / 很少動 | 夠；GRAM 撐住 |
| GIF delay ≥ 約 80–100 ms（≤10–12 fps） | 大致跟得上，觀感接近「同時在動」 |
| GIF delay 20–40 ms（單屏常看到的） | **跟不上**，一定掉幀，看起來頓 |
| 四顆都要單屏那種 ~30 fps | **物理上不行**，要第二條 QSPI 或降更新率／不全幀 |

單屏現況加上 2A/2B、MMOSIE preamble、CS 間隙，軟體目標約 30 fps。四屏均分這條 bus，單顆不可能再維持 30 fps。

### 6.2 把 QSPI1 拉到 100 MHz 能不能救四屏 fps？

**SoC 可以配 100 MHz；不能當成已經能量產，更不能當成四路 30 fps 的解法。** 細節見 `D125_QSPI配置說明.md` **§3.2**。

- 現況：模組時鐘 100 MHz、SCLK **50 MHz**（÷2）。HAL 上限 200 MHz，所以控制器允許 100 MHz bus。
- 若線上真的 100 MHz：像素段 `388800×8/4/100e6 ≈ 7.8 ms`（50 MHz 的一半）。
- 四顆只算像素：`4 × 7.8 ≈ 31 ms` → 約 **32 fps／顆**（理想）。加上 2A/2B、`1111`、settle 仍會掉下來；**仍然不是**四顆各穩 30 fps。
- 建議若實驗：`CONFIG_AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ=200000000` 且 `.max_hz=100000000`（200÷2），看 log `qspi1 freq ( bus )`。不要只改 100e6 卻讓來源非整除。
- 四片 **TXB0106 daisy-chain** 在 100 MHz 比單屏更先失敗（轉換器不是高速 CLK buffer）。應先單屏量到 100 MHz 穩定，再談四屏。

**結論：** 100 MHz 最多讓「輪流寫四顆」從 ~16 fps 往上擠；救不了「四條獨立 30 fps」。現況維持 50 MHz。

---

## 7. 利弊

### 7.1 優點

| 點 | 說明 |
|----|------|
| 只佔一顆 QSPI master | D12P 的 QSPI0 給 NOR；QSPI1 給屏；不必開 QSPI2、不必再搶 PE 腳 |
| CS 選屏與現行協定相容 | `0,1,1,1` … `1,1,1,0` 與 USB `01..04`、預設 `s_frame` 已存在 |
| 時序框仍由 QSPI 產生 | 不破壞 0x02/0x32 封包與「一整幀一次 0x2C」 |
| 未選中不黑屏 | `1111` 後 GRAM 撐住，適合儀表／多錶面「四顆都亮」 |
| 少動的屏可以很久不寫 | 省 bus；只更新正在播 GIF 的那顆 |
| 板子已按 daisy-chain + NY8 設計 | 與 `Panel with Buffer` 原理圖方向一致 |
| MMOSIE 仍掛在 QSPI1 `PE.5` | 1-wire 與 LCD 錯開，不必把 PD.26 接到四條 CS 上 |

### 7.2 缺點與風險

| 點 | 說明 |
|----|------|
| 動畫 fps 硬上限 | 四顆滿幀輪詢約 10–16 fps／顆，無法四路 30 fps |
| 現行軟體是單屏 | 一個 `lv_spi_dev`、一個 `present()`、一套 GIF decoder；carousel 是同一顆玻璃換檔 |
| 開機必須 init × 4 | RESET 並聯 → 四顆同時出 reset；漏 init 的那顆會一直黑 |
| CMA 不夠四份大 blob | `CUSTOM_ROUND_GIF_BLOB_MAX = 2 MB`；四份再加四份 canvas（每份 `5×360×360`）會把 ~12 MB CMA 打爆。要共用 blob、縮小 GIF、或播時從 LittleFS 讀（讀檔本身會頓） |
| Daisy-chain 50 MHz | 四級 TXB0106 + 排線，可能要降 `max_hz`，RAMWR 更長、fps 更差 |
| NY8 時序極敏感 | 殘脈衝、CS 靜態拉低、2A/2B 中間重解 nibble → 花屏／選錯顆 |
| `1111` 與「全不選」綁在一起 | idle 不能拿這 4-byte 再當別的 GPIO 模式，除非另訂協定 |
| USB 選屏與播 GIF 會搶同一套 `s_frame` | 一邊 JPEG 打第 2 顆、一邊 GIF round-robin，要規定誰優先 |
| 無 TE | 單屏就可能水平撕裂；四屏輪詢不會比較好 |
| 不能分帶 + yield | 單屏約 300 幀就卡死 QSPI；四屏更不能走這條 |
| MMOSIE 必須 QSPI idle 才打 | 不可在 RAMWR 中關 IRQ 打 1-wire（餓死 DMA → 花屏 → `CMA alloc failed`） |
| 一顆壞 CS／NY8 | 可能讓 bus 上出現兩顆同時 CS 低（並聯爭用） |

### 7.3 明確「不要期待」的事項

- 不要期待「只並線、軟體零改」四顆就各播 Panel_1–4。
- 不要期待四顆 GIF 都跟現在單屏一樣順。
- 不要用 GPIO 取代 `PE.5` 去框 0x2C。
- 不要在 CS 還低時改 NY8 選屏或切 MUX。
- 不要把 PD.26 的 USB 任意 4-byte 和選屏 nibble 混用而不定義優先權。

---

## 8. 現行韌體差在哪（要四屏獨立播才需改）

現況（2026-09 單屏）：

- `lv_st77916` init **一次**
- `lv_spi_present()` 無 screen index
- GIF timer 解一份 `s_round_qspi_gif`，`present_argb8888` 打到「目前 `s_frame` 選中的那顆」（預設第 1 顆）
- `CUSTOM_ROUND_SINGLE_SCREEN_CAROUSEL=1`：註解已寫「1 TFT: rotate Panel_1..4。各 QSPI 屏只播自己資料夾時設 0」——**旗標預留了，路徑沒做**

若要四顆各播、且「不黑、動畫可接受地連續」，最少要：

1. **開機**：`set_frame(TFT1)` → init → 第一幀；TFT2–4 同樣。
2. **每次 RAMWR 前**：`aic_mmosie_set_frame()` 對應該顆，再 `flush_win`（preamble 會帶走這 4-byte）。
3. **Round-robin present**：TFT1 一幀 → 等 RAMWR 完 → TFT2 … 不要在 busy 時換選屏。
4. **四套 GIF 狀態**（或「只有正在動的屏保留 decoder，其餘 GRAM 撐住」）。
5. **CMA 策略**：一個 2 MB blob 輪流載、或限制單檔大小讓四份進得去。
6. **USB JPEG**：F9-F2-F5 `01..04` 已能改選屏；要規定與 GIF 輪詢的仲裁（例如 USB 來就暫停該顆 GIF 或整組輪詢）。
7. **1 s hold-GRAM 重送**：現在只重送「最後一個 `tx_buf`」到「當前選中的 CS」。四屏時要麼記住最後一顆、要麼四顆輪流補一幀，否則只有一顆被 1 s 路徑碰到。

這些**都還沒做**。本文件是規劃，不是已交付行為。

---

## 9. 建議的產品策略（在物理上限內）

較務實的組合：

| 策略 | 做法 | 觀感 |
|------|------|------|
| A. 四靜態／偶爾更新 | 各寫一幀後靠 GRAM | 四顆同時亮，幾乎不頓 |
| B. 一顆播 GIF，其餘靜態 | bus 幾乎都給那顆 | 那顆接近現在單屏 fps |
| C. 四顆慢 GIF（≤10 fps） | 固定 round-robin 滿幀 | 「都在動」，但不如 30 fps |
| D. 四顆 30 fps | 單 QSPI1 **拒絕** | 要 QSPI2 或第二顆 SoC |

推薦產品定義先選 **A 或 B**，C 當加分；不要把 D 寫進單 QSPI1 的 spec。

---

## 10. 驗證清單（硬體／NY8 先於大改軟體）

單屏邏輯分析儀已證明 preamble → 2A/2B/RAMWR → `1111`。四屏建議依序：

1. 只接第 1 顆，`s_frame = 0,1,1,1`，確認與現在單屏相同。
2. 四顆都接，仍只送 `0,1,1,1`：僅 TFT1 有畫面，2–4 應黑或殘留（若沒 init 則黑）。
3. 開機對 1–4 各 init + 各送純色幀：四顆應各顯示對應顏色，且 `1111` 後不灭。
4. 量 NY8 四條 CS：僅選中者跟隨 `PE.5` 三次脈衝；其餘恆高。
5. 量 daisy-chain 最後一顆的 CLK/資料眼圖；必要時降 50 MHz。
6. 再談四路 GIF round-robin（軟體工作量最大）。

---

## 11. 相關檔案

| 檔案 | 與本規劃的關係 |
|------|----------------|
| `target/d12p/demo88-nor/mmosie.c` | PD.26 preamble / `1111` |
| `packages/.../custom/custom.c` | `s_mmosie_map`、USB `01..04`、單屏 GIF carousel |
| `packages/.../aic_spi/lv_aic_spi.c` | 單 `present()`、RAMWR ~15.5 ms、settle 8 ms |
| `bsp/peripheral/spilcd/aic_spi_lcd.c` | `flush_win` 開頭 `aic_mmosie_preamble()` |
| `bsp/artinchip/drv/qspi/drv_qspi.c` | CS 低／高 hook MMOSIE |
| `target/d12p/demo88-nor/pinmux.c` | QSPI1 PE.5–10、RST PE.4、PD.26 |
| `Note/D125/D125_QSPI配置說明.md` | 單屏 QSPI 協定與破圖握手 |
| `Note/D125/D125_記憶體配置說明.md` | CMA 不夠四份 2 MB blob |

---

## 12. 一句話

**PD.26 的 `0111/1011/1101/1110` + NY8 跟 PE.5 同步，適合作四顆的 CS MUX，也適合作「四顆同時各顯示不同畫面」。它不適合作「四條獨立 30 fps GIF 匯流排」。** 要四路流暢動畫，先改產品 fps 預期或加第二條 QSPI；要四顆都亮、內容不同，這條路成立，但韌體還要 init×4 與按顆 `set_frame` + RAMWR，不是只並線。
