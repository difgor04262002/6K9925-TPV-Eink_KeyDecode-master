# upgcmd 燒錄工具使用說明

> 適用平台：ArtInChip D21x 系列（含 D215）
> 工具來源：SDK `tools\` 目錄下的 `upgcmd.exe`（Windows）/ `upgcmd`（Linux）
> 文件目的：整理 upgcmd 的正確用法、各指令時機、常見錯誤解讀、典型量產 / 開發工作流程。
> 撰寫時間：2026-06-10

本文件以實際遇過的錯誤訊息為線索，回推 upgcmd 跟 SoC 各 boot stage 之間的協議與限制，幫助使用者避免錯用指令。

---

## 1. Boot Stage 概念

upgcmd 的所有指令都要看 device 端正處在哪個 boot stage 才知道能不能用。一台 ArtInChip SoC 從上電到 application 跑起來，會走過三層：

```
┌──────────────────────────────────────────────────┐
│  Stage 1: BROM (Boot ROM)                        │
│  ────────────────────────                        │
│  · 位置：SoC 內部 mask ROM（出廠焊死、無法更新）  │
│  · 任務：                                         │
│        - init 最小 CPU state                      │
│        - 等 PC 透過 USB 送 DDR init + SPL         │
│          （或從 NAND / NOR flash 載 SPL）         │
│  · USB 介面：aicupg vendor protocol               │
│  · 有 shell：❌ 沒有                              │
└──────────────────────────────────────────────────┘
                         ↓ 載入並跳到
┌──────────────────────────────────────────────────┐
│  Stage 2: SPL (Secondary Program Loader)         │
│  ────────────────────────                        │
│  · 位置：flash 固定位置（可更新）                 │
│  · 任務：                                         │
│        - init DDR                                 │
│        - init 周邊（UART/USB/MMC/SDC/...）        │
│        - 載入 application 到 DDR 並跳過去         │
│        - 提供 u-boot shell（可下 shcmd）          │
│  · USB 介面：aicupg vendor protocol               │
│  · 有 shell：✅ U-Boot                            │
└──────────────────────────────────────────────────┘
                         ↓ 載入並跳到
┌──────────────────────────────────────────────────┐
│  Stage 3: Application（你的 RT-Thread / OS）     │
│  ────────────────────────                        │
│  · 位置：flash 主分區                             │
│  · USB 介面：Application 自己定義                 │
│  · upgcmd：❌ 無法溝通                            │
└──────────────────────────────────────────────────┘
```

**為什麼分這麼多層**：BROM 是 mask ROM 永遠不能改、出廠就燒死，所以必須超級簡單、不容易壞。複雜的事（DDR init、flash IO、shell 解析）交給可更新的 SPL 做。

### 各 Stage 跟 upgcmd 的能力對照

| 能力 | BROM | SPL (U-Boot) | Application |
|---|---|---|---|
| `upgcmd -l` 看得到 | ✅ Boot stage 顯示 `Boot ROM` | ✅ 顯示 `Bootloader` | ❌ 看不到 |
| `upgcmd image <img>` 燒整套 firmware | ✅ | ✅ | ❌ |
| `upgcmd write/read` 操作 memory | ✅ | ✅ | ❌ |
| `upgcmd shcmd "..."` 執行 shell | ❌ **STALL** | ✅ | ❌ |
| `upgcmd continue` 跳出 BROM | ✅ | N/A | N/A |

**最常見的誤解**：以為 device 上線就能下 `shcmd`。事實上 `shcmd` 只有在 device 跑到 **SPL（U-Boot）** 這層才有用。

---

## 2. upgcmd 指令格式總覽

```
upgcmd  [options]              [command]  [arguments]
        ─────────              ─────────  ───────────
        用 -dash 開頭           沒 dash    command 的參數
        例：-d -p -v -l -i      例：image  例：image file 路徑
```

**Options（前綴 dash）跟 Commands（沒 dash）必須分清楚**：

### Options（單獨可用，或搭配 command）

| Option | 用途 | 範例 |
|---|---|---|
| `-h, --help` | 顯示說明 | `upgcmd -h` |
| `-l, --list` | 列所有連到的 device | `upgcmd -l` |
| `-i, --imagefile <file>` | parse image 檔內容 | `upgcmd -i xxx.img` |
| `-e, --extract <file>` | 從 image 抽出各 component | `upgcmd -e xxx.img` |
| `-d, --dev bus:portnum` | 指定 device（多裝置時） | `upgcmd -d 2:5-2-2-1 ...` |
| `-p, --progress` | 顯示燒錄進度條 | 加在燒錄指令前 |
| `-u, --uart COM<ID>` | 改用 UART（不用 USB） | `upgcmd -u COM3 ...` |
| `-b, --baudrate <rate>` | UART 速率 | `upgcmd -u COM3 -b 1500000 ...` |
| `-v, --verbose` | 詳細 log（除錯用） | 加在任何指令前 |

### Commands（必須緊接在 options 後面）

| Command | 用途 | 範例 |
|---|---|---|
| `chipdata <file>` | 讀 chip unique data | `upgcmd chipdata data.bin` |
| `jtagunlock <file>` | 解 JTAG lock | `upgcmd jtagunlock signed.bin` |
| `continue` | BROM 退出 USB loop 嘗試 boot | `upgcmd continue` |
| `write <addr> <file> [skip] [len]` | 寫檔到 device memory | `upgcmd write 0x40000000 boot.bin` |
| `read <addr> <length> <file>` | 讀 device memory 到檔 | `upgcmd read 0x40000000 0x1000 dump.bin` |
| `writel <addr> <val>` | 寫 32-bit | `upgcmd writel 0x18000000 0xDEADBEEF` |
| `readl <addr>` | 讀 32-bit | `upgcmd readl 0x18000000` |
| `memtest <addr> <size> <round>` | memory 讀寫測試 | `upgcmd memtest 0x40000000 0x10000 10` |
| `exec <addr>` | 從某 address 開始執行 | `upgcmd exec 0x40000000` |
| `hexdump <addr> <length>` | 印出 hex dump | `upgcmd hexdump 0x40000000 0x100` |
| `dump <media> <part> <file>` | dump 整個 partition 出來 | `upgcmd dump nor fwc rootfs.img` |
| `lsmedia <file>` | 列所有 storage media | `upgcmd lsmedia media.txt` |
| `lspart <media>` | 列 media 的 partition 表 | `upgcmd lspart nor` |
| `fill <addr> <len> <val>` | 把 memory 區塊填某 byte | `upgcmd fill 0x40000000 0x100 0xFF` |
| `clear <addr> <len>` | 清零 memory | `upgcmd clear 0x40000000 0x100` |
| **`shcmd "string"`** | **執行 u-boot shell 指令**（注意限制） | `upgcmd shcmd "reboot"` |
| **`image <file>`** | **燒整套 AIC image**（最常用） | `upgcmd image xxx.img` |
| `ramboot <fwc> <addr> <file>` | 把 component 載到 RAM 並跳過去執行 | 見 help |

### 標準寫法（強烈建議養成習慣）

```bash
upgcmd  -d <bus:port>  -p  [-v]  <command>  <arguments>
```

- `-d` 寫前面（明確指定 device）
- `-p` 燒錄類指令一定要加（看進度）
- `-v` 除錯時加
- 然後是 command 跟它的 args

---

## 3. 完整燒錄流程（image 指令）

### 標準燒錄指令

```bash
upgcmd -d <bus:port> -p image <image_file_path>
```

實際範例：

```bash
upgcmd -d 2:5-2-2-1 -p image D:\SDK\output\d21x_demo88-nor_rt-thread\images\d21x_demo88_nor_v1.0.0.img
```

### 燒錄前確認

```bash
# 1. 確認 device 在 BROM 階段
upgcmd -l
# 期望輸出：
#   Bus:Port 2:5-2-2-1
#       - Device 16, Class 0
#       - Boot stage: Boot ROM     ← 必須是這個
#       - Stage: 0

# 2.（可選）parse image 確認包了哪些 component
upgcmd -i D:\SDK\output\...\xxx.img
```

### 燒錄期間發生什麼

```
[Device 端]                                [PC 端 upgcmd]
   │
   ├─ Stage 0: Boot ROM 待機                │  upgcmd image xxx.img
   │                                       │
   │  ← updater.ddr (~30 KB)               │  → 送 DDR init binary
   │  ← BROM init DDR                       │
   │                                       │
   │  ← updater.spl (~240 KB)              │  → 送 SPL
   │  ← BROM 跳到 SPL                       │
   │                                       │
   ├═══════════════════════════════════════ ★「進 SPL」發生 ★
   │                                       │
   │  ← USB 重 enumerate                    │
   │  ← SPL 寫 flash                        │  → 送各 component
   │     (kernel / rootfs / fs / ...)      │     進度條跳動
   │                                       │
   │  ← 寫完回報 OK                         │  ← 顯示 "Burn complete"
   │  ← SPL 自動 reboot                     │
   │                                       │
   └─ Application 起來                      │  指令結束
                                           │  device 從 USB 消失
                                           │  upgcmd -l 看不到
```

### 燒完後該怎麼確認

```bash
# 跑 -l，期望 device 從清單消失
upgcmd -l
# 期望輸出：
#   There are 0 USB device in list:
#
#  ← 這代表 application 正常起來，aicupg endpoint 已關，是好事
```

要驗證 application 跑起來，**改用 UART console / 螢幕 / SSH** 等正常方式，**不要再用 upgcmd**。

---

## 4. `shcmd` 用法跟限制

### 正確格式

```bash
upgcmd -d <bus:port> shcmd "<u-boot shell 字串>"
```

關鍵：
- `-d <bus:port>` 必須擺在 `shcmd` **之前**，當 option 用
- 「`<u-boot shell 字串>`」要用**雙引號**包起來
- 只有 device 跑到 **SPL/U-Boot** 才能用

### 常見 shcmd 用法

```bash
# 在 U-Boot 階段 reboot
upgcmd -d 2:5-2-2-1 shcmd "reset"

# 從 U-Boot 跳到 application（如果 boot 卡在 U-Boot）
upgcmd -d 2:5-2-2-1 shcmd "boot"

# 看 U-Boot env
upgcmd -d 2:5-2-2-1 shcmd "printenv"

# 看 flash partition
upgcmd -d 2:5-2-2-1 shcmd "mtd list"
```

### `shcmd` 在 BROM 階段為什麼會失敗

**BROM 沒實作 u-boot shell**。BROM 只是 mask ROM 裡的固定載入器，能力範圍：
- USB aicupg protocol state machine
- DDR init binary 接收
- SPL 接收與跳轉
- `continue` 指令

**沒有**任何 shell 解析能力。所以在 BROM 階段送 `shcmd` 過去：

1. **舊版 BROM**：嘗試解 packet 結構，回個格式不對的 13-byte 封包，PC 端報「`Read len 13/16`」
2. **新版 BROM**：直接把 USB endpoint 設成 STALL，PC 端 send 階段就拿到「`Pipe error(-9)`」

兩種錯誤都代表「BROM 不認 shcmd」，**不是工具壞掉、也不是版本不對**。

### 三種錯誤格式對照

#### 錯誤格式 ①：option 跟 command 順序錯

```bash
upgcmd shcmd 2:5-2-2-1
       ^^^^^ ^^^^^^^^^
         │     └─ 被當成 u-boot shell 字串送出去
         └─ command
```

device 收到「`2:5-2-2-1`」這串字當 u-boot 指令解，當然失敗。**正確**：`-d` 必須在 `shcmd` 之前。

#### 錯誤格式 ②：flag 跟程式名連在一起

```bash
upgcmd-d 2:5-2-2-1 -p image xxx.img
^^^^^^^^
shell 認為這整串是程式名 → "command not found"
```

**正確**：`upgcmd` 跟 `-d` 之間要有**空格**。

#### 錯誤格式 ③：缺 command

```bash
upgcmd -d 2:5-2-2-1 D:\...\xxx.img
                    ^^^^^^^^^^^^^^^
                    被當成 command 名稱去找 → "Command not found"
```

**正確**：image 路徑前面要有 **`image`** 這個關鍵字告訴 upgcmd「我要燒 image」。

---

## 5. 常見錯誤訊息解讀

### 5.1 `aicupg_trans_recv_pkt(): Read len 13/16, Success(0)`

**意思**：PC 期待收 16 byte，device 實際只送了 13 byte 回來（13 byte 剛好是 USB MSC 的 CSW 標準長度）。

**原因**：

1. **最常見**：device 在 BROM 階段，被下了不支援的指令（如 `shcmd`），舊版 BROM 回了個跟 aicupg 不相容的封包
2. **較罕見**：PC 端 upgcmd 跟 SDK firmware 版本不匹配

**處理**：
- 跑 `upgcmd -l` 確認 device boot stage
- 如果是 Boot ROM，**不要下 shcmd**，改用 `image`、`write`、`continue`
- 如果是 Bootloader 還失敗，檢查 PC 端 upgcmd 版本

### 5.2 `aicupg_trans_send_pkt(): CSW tag 0x2, size 0, Pipe error(-9)`

**意思**：PC 連封包都送不出去；USB endpoint 處於 STALL 狀態（libusb error `LIBUSB_ERROR_PIPE` = -9）。

**原因**：
- Device 端硬體層拒收這個封包（endpoint stalled）
- 通常發生在「對 BROM 下 shcmd」的情況下，新版 BROM 直接 STALL 不收

**處理**：跟 5.1 一樣，先確認 boot stage，不要在 BROM 階段用 shcmd。

### 5.3 `Command '<檔案路徑>' not found!`

**意思**：upgcmd 把你給的東西當成 command 名稱（`image`、`shcmd`、`write` ...）去找，但沒這個 command。

**原因**：缺 command 關鍵字。例如：

```bash
upgcmd -d 2:5-2-2-1 xxx.img      # ← 缺 image
upgcmd -d 2:5-2-2-1 image xxx.img # ← 正確
```

**處理**：在路徑前面補對應的 command 名稱。

### 5.4 `'upgcmd-xxx' is not recognized as an internal or external command`

**意思**：Windows shell 找不到這個程式。

**原因**：`upgcmd` 跟後面的 flag 連在一起寫成 `upgcmd-d` / `upgcmd-l` 之類。

**處理**：`upgcmd` 跟 flag 之間補空格。

### 5.5 `-l` 列不出 device

**可能**：
1. Device 沒接 USB
2. Device 已經 boot 進 application（看到 0 個 device 是**正常結果**，代表上次燒錄成功）
3. USB driver 沒裝對（Windows 裝置管理員看 device 在哪個分類，應該是 WinUSB 或類似）
4. Device VID/PID 系統認不到（少見）

**處理**：
- 看 UART console 確認 device 在跑什麼
- Windows 裝置管理員查 USB device driver 狀態
- 如果想讓 device 回到 BROM 待燒錄，需要：斷電 + 按住 BROM 鍵 + 上電（依板子設計），或 application 端有實作「進 BROM」觸發機制（如本專案 `custom.c` 的 9-byte BROM trigger pattern）

---

## 6. 典型工作流程

### 場景 A：開發階段，反覆燒新版 firmware

```bash
# 每次 build 完跑這條
upgcmd -l                                              # 1. 確認 device 在 BROM
upgcmd -d 2:5-2-2-1 -p image <new_image>.img          # 2. 燒
# device 自動 reboot 進 application
# 看 UART 確認跑起來
```

之後要再燒，必須讓 device 重新進入 BROM 階段：
- **方法 1**：斷電 → 按住 BROM 按鍵 → 上電（硬體方法）
- **方法 2**：在 application 內實作 BROM 觸發機制，從 PC 端送特定 byte sequence 觸發（本專案做法，見 `custom.c::custom_trigger_brom_mode()`）
- **方法 3**：在 application 內呼叫 reboot 並設 reboot reason = `REBOOT_REASON_UPGRADE`

### 場景 B：量產，燒第一次空白 chip

```bash
upgcmd -l                                              # 必看 Boot ROM
upgcmd -d <bus:port> -p image <production_image>.img
# 等 burn complete
# device 自動 reboot 起來
# pass 測試
```

### 場景 C：U-Boot 階段除錯

只有當 device 卡在 U-Boot（沒進 application）才用：

```bash
upgcmd -l                                              # 確認 stage 是 Bootloader
upgcmd -d <bus:port> shcmd "printenv"                  # 看 env
upgcmd -d <bus:port> shcmd "mtd list"                  # 看 flash
upgcmd -d <bus:port> shcmd "boot"                      # 嘗試 boot 進 application
```

### 場景 D：dump 出 flash 內容

只有 device 在 SPL 或更後階段才有 storage media driver：

```bash
upgcmd -d <bus:port> lsmedia media.txt                 # 列 media
upgcmd -d <bus:port> lspart nor                        # 列 partition
upgcmd -d <bus:port> dump nor rootfs dump.bin          # dump partition
```

### 場景 E：BROM 卡住、不要燒只想讓它 boot

```bash
upgcmd -d <bus:port> continue
```

`continue` 會讓 BROM 退出 USB loop，按正常 boot 順序試（從 flash 載 SPL）。如果 flash 裡已經有 valid SPL，會正常起來；如果沒有，會再回到 BROM USB 待機。

---

## 7. 注意事項與常見陷阱

### 7.1 Image 必須跟 board / storage 配對

| 你 build 的 config | 只能燒進 |
|---|---|
| `d21x_demo88-nor-...` | NOR flash 的 demo88 板 |
| `d21x_demo128-nand-...` | NAND flash 的 demo128 板 |
| `d215-...-dacai03` | dacai03 panel 的 D215 板 |

**跨 board 或跨 storage type 燒進去**會：
- 輕度：boot fail，繼續停在 BROM 等下次燒
- 重度：partition table 蓋掉、SPL 寫錯位置，需要走 BROM 救援流程

**燒之前一定要先確認 image 跟手上 board 是同一份 config build 出來的**。

### 7.2 燒完不要再下 upgcmd 指令

很多人會「燒完想用 upgcmd reboot 確認」，這是錯誤的：
- Application 跑起來後 aicupg USB endpoint 已關
- 想 reboot：**直接斷電重上電**，或 device 端 app 自己有 reboot 機制
- 想驗證跑起來：**看 UART console**

### 7.3 BROM stage 0 看到只有 USB 是預期的

BROM 階段 UART 通常沒輸出（BROM 不一定 init UART driver）。只能用 USB 跟 PC 工具確認 device 還活著。
**看不到 UART log 不代表 device 死掉**，看 `upgcmd -l` 才是判斷 device 狀態的依據。

### 7.4 Verbose log 是除錯神器

任何指令前面加 `-v`：

```bash
upgcmd -d 2:5-2-2-1 -p -v image xxx.img
```

會印出每個 USB transaction 的詳細狀況。出錯時加 `-v` 重跑，把 log 完整貼出來才能進一步診斷。

### 7.5 多 device 環境

如果 PC 上同時接好幾顆 ArtInChip device，`-l` 會列多個，這時 `-d <bus:port>` **必加**。如果省略，help 寫的「Default use the first device」可能不是你要的那顆。

---

## 8. 快速排查 Flowchart

遇到 upgcmd 錯誤先按這個流程跑：

```
跑 upgcmd -l
  │
  ├─ 列不出 device
  │     │
  │     ├─ 想要燒新版 → device 沒在 BROM，要先讓它進 BROM
  │     │              （斷電 + BROM 鍵 / 軟體觸發）
  │     │
  │     └─ 剛燒完想驗證 → 正常結果！application 跑起來了
  │                       改看 UART console 或螢幕
  │
  ├─ 看到 Boot stage: Boot ROM
  │     │
  │     ├─ 要燒 → upgcmd -d <id> -p image <file>
  │     │
  │     ├─ 已燒過但停這裡 → image 跟 board 不匹配，或 SPL 跑不起來
  │     │                   檢查 image config，或 upgcmd -d <id> continue
  │     │
  │     └─ 想下 shcmd → ❌ 在這 stage 不能用
  │                     先 image 燒進去，讓 device 跑到 SPL/U-Boot
  │
  └─ 看到 Boot stage: Bootloader (或 SPL)
        │
        ├─ 要下 shell 指令 → upgcmd -d <id> shcmd "..."
        │
        └─ 要燒整套 firmware → upgcmd -d <id> -p image <file>
                              （SPL 也支援 image 燒錄）
```

---

## 9. 一句話總結

| 目的 | 一句話指令 |
|---|---|
| 看 device 在不在、停哪 stage | `upgcmd -l` |
| 燒整套 firmware（最常用） | `upgcmd -d <bus:port> -p image <file>.img` |
| 解析 image 內容 | `upgcmd -i <file>.img` |
| U-Boot 階段下指令 | `upgcmd -d <bus:port> shcmd "<cmd>"` |
| BROM 退出嘗試 boot | `upgcmd -d <bus:port> continue` |
| 除錯時加詳細 log | 任何指令前加 `-v` |

---

## 10. 對照本專案的相關設計

本專案 `custom.c` 內實作了「從 application 端觸發回到 BROM」的機制，跟 upgcmd 工作流配合使用：

| 元件 | 位置 | 用途 |
|---|---|---|
| BROM 觸發 byte sequence | `custom.c:401-403` `s_brom_pattern[]` | 9-byte 樣式 `0xFF 0xA1 0xB2 0xE3 0xF4 0xC5 0xD6 0xEE 0xEE` |
| 觸發判定（fast-path） | `custom.c:489-496` `custom_brom_packet_is_exact_match()` | USB 整包正好等於 9-byte pattern 時立刻觸發 |
| 觸發判定（byte-by-byte） | `custom.c:498-513` `custom_feed_brom_cmd_byte()` | UART 串流 state machine 後援 |
| 真正執行進 BROM | `custom.c:406-482` `custom_trigger_brom_mode()` | 設 `REBOOT_REASON_UPGRADE` + WDT timeout=0 → 硬體立刻 reset → 重啟後 BROM 進 USB upgrade mode |

這條 chain 走完後，PC 端就可以跑 `upgcmd -l` 看到 device 在 Boot ROM 階段、接著用 `upgcmd image` 燒新版 firmware，**完全不用人去按 BROM 按鍵**。對量產 / OTA 場景非常重要。

詳細的 BROM 觸發保護邏輯（為什麼要 gate 影像 payload、為什麼 fast-path + byte-by-byte 兩路）見 `custom.c` 對應段落的中文註解。

---

文件結束。如遇新的 upgcmd 錯誤訊息或工作流需求，請保留章節編號順序追加。
