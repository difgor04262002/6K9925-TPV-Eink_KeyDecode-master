# Luban-Lite 燒錄 IMG 完整說明

> **文件版本**：2026-06-18  
> **適用 SDK**：Luban-Lite RTOS SDK（本專案 D21x / D215）  
> **資料來源**：SDK 內 `doc/topics/sdk/advanced/`、`doc/topics/sdk/burnsys/`、`tools/scripts/mk_image.py`、`upgcmd_燒錄工具使用說明.md`  
> **說明**：工作區內未附 `Luban-Lite_SDK.pdf`；本文件依 **RTOS SDK 使用指南 → 使用指南 → 配置烧录镜像** 及 **应用场景 → 系统更新** 章節整理。

---

## 目錄

1. [概述](#1-概述)
2. [名詞與 Boot Stage](#2-名詞與-boot-stage)
3. [IMG 映像結構](#3-img-映像結構)
4. [image_cfg.json 完整說明](#4-image_cfgjson-完整說明)
5. [編譯與打包流程](#5-編譯與打包流程)
6. [燒錄方式總覽](#6-燒錄方式總覽)
7. [方式一：upgcmd（USB 命令列）](#7-方式一upgcmdusb-命令列)
8. [方式二：AiBurn（GUI）](#8-方式二aiburngui)
9. [方式三：SD 卡燒錄](#9-方式三sd-卡燒錄)
10. [方式四：U 碟燒錄](#10-方式四u-碟燒錄)
11. [方式五：UART 燒錄](#11-方式五uart-燒錄)
12. [從 Application 觸發燒錄模式](#12-從-application-觸發燒錄模式)
13. [d215-demo88-nor-dacai03 實例](#13-d215-demo88-nor-dacai03-實例)
14. [常見錯誤與排除](#14-常見錯誤與排除)
15. [量產與離線燒錄](#15-量產與離線燒錄)
16. [官方文件對照表](#16-官方文件對照表)

---

## 1. 概述

Luban-Lite 將 Bootloader、Application、檔案系統等元件打包成單一 **`.img` 燒錄映像**，透過多種媒介寫入 Flash：

| 媒介 | 工具 | 典型場景 |
|------|------|----------|
| USB | `upgcmd`、AiBurn | 開發、除錯、量產線 |
| SD 卡 | 複製 `.img` + `bootcfg.txt` | 現場無 PC、批量 |
| U 碟 | SPL 偵測 FAT 檔案 | 維修升級 |
| UART | AiBurn / `upgcmd -u` | USB 不可用時（平台有限制） |

**核心配置檔**：`target/<chip>/<board>/pack/image_cfg.json`  
**打包工具**：`tools/scripts/mk_image.py`（build 時自動呼叫）

---

## 2. 名詞與 Boot Stage

燒錄前必須確認裝置處於哪個 **Boot Stage**，否則指令會失敗。

```
┌──────────────────────────────────────────────────┐
│  Stage 1: BROM（Boot ROM）                        │
│  · Mask ROM，無法更新                             │
│  · 支援：upgcmd image / write / read / continue  │
│  · 不支援：shcmd（會 Pipe error / STALL）         │
└──────────────────────────────────────────────────┘
                         ↓
┌──────────────────────────────────────────────────┐
│  Stage 2: SPL / tinySPL（Bootloader）             │
│  · Flash spl 分區，可更新                         │
│  · 支援：image / write / read / shcmd              │
└──────────────────────────────────────────────────┘
                         ↓
┌──────────────────────────────────────────────────┐
│  Stage 3: Application（RT-Thread）                │
│  · upgcmd 無法連線（USB 為應用自定義協議）         │
│  · 需透過 aicupg gotobl / BROM 觸發 重新燒錄      │
└──────────────────────────────────────────────────┘
```

### 各 Stage 能力對照

| 能力 | BROM | SPL | Application |
|------|------|-----|-------------|
| `upgcmd -l` 可見 | ✅ Boot ROM | ✅ Bootloader | ❌ |
| `upgcmd image <img>` | ✅ | ✅ | ❌ |
| `upgcmd shcmd "..."` | ❌ | ✅ | ❌ |
| `upgcmd continue` | ✅ | N/A | N/A |

> 詳細 upgcmd 語法請參閱同目錄 `upgcmd_燒錄工具使用說明.md`。

---

## 3. IMG 映像結構

### 3.1 檔案組成

最終 `.img` 由 `mk_image.py` 產生，結構為：

```
┌─────────────────┐
│  Image Header   │  映像標頭（platform/product/version）
├─────────────────┤
│  FWC Meta       │  各元件 metadata（512B 對齊）
├─────────────────┤
│  FWC Data       │  實際韌體資料（2048B 對齊）
└─────────────────┘
```

### 3.2 映像內元件類型

| 類型 | 說明 | 範例 |
|------|------|------|
| **updater** | 燒錄時載入 RAM 執行，不寫 Flash | `ddr`、`spl` |
| **target** | 寫入 Flash 分區 | `spl`、`os`、`env`、`rodata`、`data` |

### 3.3 伴隨產物

打包後 `output/<project>/images/` 目錄通常包含：

| 檔案 | 說明 |
|------|------|
| `d21x_<product>_vX.Y.Z.img` | 完整燒錄映像 |
| `bootcfg.txt` | SD/U 碟燒錄用配置 |
| `image_part_table.bin` | 分區表二進位 |
| `bootloader.bin` / `bootloader.aic` | Bootloader 中間產物 |
| `d21x_os.itb` | Application FIT 映像 |

---

## 4. image_cfg.json 完整說明

**路徑範例**：`target/d21x/d215-demo88-nor-dacai03/pack/image_cfg.json`

JSON 支援 `//` 行註解（`mk_image.py` 會剝除）。

### 4.1 頂層結構

```json
{
    "spi-nor": { ... },      // 或 "spi-nand" / "mmc"
    "image": { ... },
    "pre-process": { ... }
}
```

### 4.2 媒體與分區表（`spi-nor` 等）

```json
"spi-nor": {
    "size": "16m",
    "partitions": {
        "spl":    { "size": "512k" },
        "env":    { "size": "128k" },
        "env_r":  { "size": "128k" },
        "os":     { "size": "2m" },
        "os_r":   { "size": "2m" },
        "rodata": { "size": "6m" },
        "data":   { "size": "4m" }
    }
}
```

**規則**：

- 分區名稱對應 `target` 中的 `part` 欄位
- 所有分區大小總和不得超過 `size`
- 修改分區後必須 **重編 Bootloader** 並 **重燒完整 img**

### 4.3 image.info（映像標頭）

```json
"info": {
    "platform": "d21x",
    "product": "demo88_nor",
    "version": "1.0.0",
    "media": {
        "type": "spi-nor",
        "device_id": 0
    }
}
```

- `version` 格式：`x.y.z`，寫入映像標頭
- `media.type` 必須與實際 Flash 類型一致

### 4.4 image.updater（RAM 執行元件）

燒錄時 **依序** 下載到 RAM 並執行：

```json
"updater": {
    "ddr": {
        "file": "usbupg-ddr-init.aic",
        "attr": ["required", "run"],
        "ram": "0x00103000"
    },
    "spl": {
        "file": "bootloader.aic",
        "attr": ["required", "run"],
        "ram": "0x41000000"
    }
}
```

| 欄位 | 說明 |
|------|------|
| `file` | 中間產物檔名（在 `pack/` 目錄） |
| `attr` | `required`=必須；`run`=下載後立即執行 |
| `ram` | 載入位址 |

**USB 燒錄時序**：

1. BROM 收到 `ddr` → 初始化 DDR
2. BROM 收到 `spl` → 跳轉 tinySPL（USB 重新列舉）
3. SPL 收到 `target` 各元件 → 寫入 Flash

### 4.5 image.target（寫入 Flash）

```json
"target": {
    "spl": {
        "file": "bootloader.aic",
        "attr": ["mtd", "required"],
        "part": ["spl"]
    },
    "os": {
        "file": "d21x_os.itb",
        "attr": ["mtd", "required"],
        "part": ["os"]
    },
    "rodata": {
        "file": "rodata.fatfs",
        "attr": ["mtd", "optional"],
        "part": ["rodata"]
    }
}
```

| attr 值 | 說明 |
|---------|------|
| `mtd` | 寫入 MTD 分區 |
| `uffs` | UFFS 檔案系統映像 |
| `required` | 缺少則打包/燒錄失敗 |
| `optional` | 可選元件 |

### 4.6 pre-process（中間檔產生）

| 步驟類型 | 功能 | 範例 |
|----------|------|------|
| `aicimage` | 產生 `.aic` 啟動映像 | `loader.aic`、`usbupg-ddr-init.aic` |
| `concatenate` | 串接多個 `.aic` | `bootloader.aic` = pbp + loader |
| `itb` | 產生 FIT 映像 | `d21x_os.itb` from `d21x_os.its` |
| `uboot_env` | 產生 env.bin | 從 `env.txt` |

**bootloader.aic 打包鏈（dacai03）**：

```
ddr_init.bin + d21x.pbp  →  usbupg-ddr-init.aic / pbp_ext.aic
bootloader.bin           →  loader.aic（含 LOAD_BASE / TEXT_BASE）
[pbp_ext.aic, loader.aic] →  concatenate → bootloader.aic
```

`loader.aic` 的 load/entry 位址來自編譯後的：

- `CONFIG_AIC_BOOTLOADER_LOAD_BASE`
- `CONFIG_AIC_BOOTLOADER_TEXT_BASE`

> 若未先編譯 Bootloader，`mk_image.py` 會報錯：`Please compile the bootloader first.`

---

## 5. 編譯與打包流程

### 5.1 完整流程圖

```
① 編譯 Bootloader
   scons --apply-def=<board>_baremetal_bootloader_defconfig
   scons
        ↓
   產出 target/.../pack/bootloader.bin

② 編譯 Application
   scons --apply-def=<board>_rt-thread_helloworld_defconfig
   scons
        ↓
   產出 d21x_os.itb、rodata.fatfs、data.lfs 等

③ mk_image.py 自動執行（scons 內建）
   讀取 image_cfg.json
   執行 pre-process
   組裝 .img + bootcfg.txt
        ↓
   output/.../images/d21x_<product>_vX.Y.Z.img
```

### 5.2 一鍵編譯（推薦）

```bash
# 在 SDK 根目錄，已 lunch 專案後
mb    # OneStep：Bootloader + App + img
```

或手動：

```bash
scons --apply-def=d21x_d215-demo88-nor-dacai03_baremetal_bootloader_defconfig && scons
scons --apply-def=d21x_d215-demo88-nor-dacai03_rt-thread_helloworld_defconfig && scons
```

### 5.3 產物位置

```
output/d21x_d215-demo88-nor-dacai03_rt-thread_helloworld/images/
├── d21x_demo88_nor_v1.0.0.img    ← 燒錄用
├── bootcfg.txt                    ← SD 卡燒錄用
├── image_part_table.bin
├── bootloader.aic
├── d21x_os.itb
├── rodata.fatfs
└── data.lfs
```

### 5.4 僅重新打包（不重新編譯）

```bash
python tools/scripts/mk_image.py \
    -c target/d21x/d215-demo88-nor-dacai03/pack/image_cfg.json \
    -d target/d21x/d215-demo88-nor-dacai03/pack \
    -o output/.../images/test.img
```

### 5.5 解析既有 img

```bash
upgcmd -i output/.../d21x_demo88_nor_v1.0.0.img
```

---

## 6. 燒錄方式總覽

| 方式 | 工具 | 裝置狀態 | D21x 刷機 | D21x 升級 |
|------|------|----------|-----------|-----------|
| USB 線 | upgcmd / AiBurn | BROM 或 SPL | ✅ | ✅ |
| SD 卡 | 複製檔案 | BROM 或 SPL | ✅ | ✅ |
| U 碟 | FAT 檔案 | SPL | ❌ | ✅ |
| UART | AiBurn / upgcmd -u | BROM/SPL | ❌* | ❌* |
| 離線燒錄器 | SP32 等 | 生產 | ✅ | — |

\* D21x 官方平台表標示 UART **不支援刷機**；其他晶片（D13x 等）可能支援。

---

## 7. 方式一：upgcmd（USB 命令列）

**工具位置**：`tools/upgcmd.exe`（Windows）/ `tools/upgcmd`（Linux）

### 7.1 進入燒錄模式

任選其一：

| 方法 | 說明 |
|------|------|
| 空 Flash 上電 | 自動進 BROM USB |
| 按住燒錄鍵（PA0 等）上電 | 進 BROM 或 SPL 燒錄 |
| SPI Flash 腳位短路 | 強制 BROM |
| Application 執行 `aicupg` / BROM 觸發 | 重啟進 SPL/BROM |

### 7.2 標準燒錄指令

```bash
# 1. 列出 USB 裝置與 Boot Stage
upgcmd -l

# 輸出範例：
# Bus 002 Device 005: ID 33c3:6677 Artinchip Device
#   Boot stage: Boot ROM

# 2. 燒錄完整映像（注意 image 子命令）
upgcmd -d 2:5-2-2-1 -p image D:\path\to\d21x_demo88_nor_v1.0.0.img

# 參數說明：
#   -d <bus:port>   裝置 ID（來自 -l）
#   -p              顯示進度條
#   image           子命令（不可省略！）
#   <路徑>          .img 檔案

# 3. 可選：顯示詳細 log
upgcmd -d 2:5-2-2-1 -p -v image D:\path\to\image.img
```

### 7.3 燒錄過程（三階段）

```
[階段 1] BROM
  PC 送 updater.ddr  → DDR 初始化
  PC 送 updater.spl → 跳轉 tinySPL
  （USB 裝置重新列舉，VID/PID 可能不變）

[階段 2] SPL
  PC 送 target.spl    → 寫入 spl 分區
  PC 送 target.env    → 寫入 env 分區
  PC 送 target.os     → 寫入 os 分區
  PC 送 target.rodata → 寫入 rodata 分區
  ...

[階段 3] 完成
  裝置自動重啟 → 進入 Application
  upgcmd -l 看不到裝置（正常）
```

### 7.4 SPL 階段除錯命令

```bash
# 僅在 Boot stage: Bootloader 時有效
upgcmd -d <id> shcmd "printenv"
upgcmd -d <id> shcmd "mtd list"
upgcmd -d <id> shcmd "reboot"

# 跳出 BROM USB 等待迴圈
upgcmd -d <id> continue
```

### 7.5 常見語法錯誤

| 錯誤輸入 | 問題 |
|----------|------|
| `upgcmd-d 2:5-2-2-1` | 缺少空格 |
| `upgcmd -d ID file.img` | 缺少 `image` 子命令 |
| `upgcmd -d ID shcmd reboot`（BROM 階段） | BROM 不支援 shcmd |

---

## 8. 方式二：AiBurn（GUI）

**官方工具**，圖形化燒錄，功能與 upgcmd USB 燒錄等效。

### 8.1 USB 燒錄步驟

1. 開啟 AiBurn → **烧写镜像**
2. 裝置進入燒錄模式（燒錄鍵 / 空 Flash / `aicupg`）
3. 裝置圖示變藍 → 「ArtInChip 设备已连接」
4. 選擇 `.img` 檔案
5. 點 **开始** → 等待進度完成
6. 裝置自動重啟

### 8.2 製作啟動卡（SD 卡量產）

AiBurn → **制作启动卡**（需管理員權限）：

- 選擇 MMC 類型映像（若要做 SD 啟動卡）
- 格式化 SD 卡為 FAT32
- 寫入映像與啟動配置

---

## 9. 方式三：SD 卡燒錄

### 9.1 準備檔案

從 `output/.../images/` 複製到 SD 卡 **根目錄**：

```
<SD卡根目錄>/
├── d21x_demo88_nor_v1.0.0.img
└── bootcfg.txt
```

### 9.2 SD 卡格式要求

| 項目 | 要求 |
|------|------|
| 分區 | **單一分區** |
| 檔案系統 | **FAT32**（非 exFAT、非 FAT16） |
| 檔案位置 | 根目錄 |

### 9.3 操作步驟

1. 將 SD 卡插入裝置
2. 上電
3. Console 顯示 `AiBurn SDCard` → 燒錄進行中
4. 完成後顯示 `AiBurn done`
5. **拔出 SD 卡**，重新上電（平台不會自動重啟，避免重複燒錄）

### 9.4 BROM vs SPL 路徑

| 路徑 | 條件 |
|------|------|
| BROM SD 燒錄 | SD 為啟動碟且 FAT32 上有兩個檔案 |
| SPL SD 燒錄 | Bootloader 啟用 `AICUPG_SDCARD_ENABLE` |

---

## 10. 方式四：U 碟燒錄

### 10.1 條件

- Bootloader 啟用 `AICUPG_UDISK_ENABLE`
- USB0 接 U 碟，FAT32 格式
- U 碟根目錄有 `.img` + `bootcfg.txt`

### 10.2 流程

1. 插入 U 碟
2. 上電 → SPL 偵測到燒錄檔案
3. 自動燒錄 → Console 顯示進度
4. 完成後手動重啟

> D21x 平台表：U 碟支援 **升級**，不支援 **刷機**（全碟）。

---

## 11. 方式五：UART 燒錄

### 11.1 AiBurn 串口燒錄

1. AiBurn → 連接方式選 **串口**
2. 選擇 COM 埠、鮑率（115200～3M，僅 **uart0**）
3. 裝置需 **獨立供電**（非 PC USB 供電）
4. 點 **连接** → **开始**

### 11.2 upgcmd UART 模式

```bash
upgcmd -u COM3 -b 1500000 image D:\path\to\image.img
```

### 11.3 平台限制

**D21x 官方文件標示 UART 不支援刷機**。若需 UART 燒錄，請確認晶片型號與 SDK 版本說明。

---

## 12. 從 Application 觸發燒錄模式

Application 跑起來後 `upgcmd` 無法直接連線，需先讓裝置回到 BROM/SPL：

| 方法 | 說明 |
|------|------|
| RT-Thread Shell：`aicupg` | 重啟進 SPL 燒錄模式 |
| Shell：`aicupg gotobl` | 設 `REBOOT_REASON_BL_UPGRADE` |
| 自訂 BROM 觸發（9-byte pattern） | 本專案 `custom.c` |
| 硬體燒錄鍵 + 重啟 | 進 BROM |
| `aicupg brom`（在 SPL Console） | WDT 重啟進 BROM |

**BROM 觸發 9-byte 序列**（本專案）：

```
0xFF 0xA1 0xB2 0xE3 0xF4 0xC5 0xD6 0xEE 0xEE
```

---

## 13. d215-demo88-nor-dacai03 實例

### 13.1 分區表與位址

**媒體**：SPI NOR 16MB（`device_id: 0`）

| 分區 | 大小 | 起始位址 | 結束位址 | 燒錄檔案 |
|------|------|----------|----------|----------|
| spl | 512 KB | `0x000000` | `0x07FFFF` | `bootloader.aic` |
| env | 128 KB | `0x080000` | `0x09FFFF` | `env.bin` |
| env_r | 128 KB | `0x0A0000` | `0x0BFFFF` | `env.bin` |
| os | 2 MB | `0x0C0000` | `0x2BFFFF` | `d21x_os.itb` |
| os_r | 2 MB | `0x2C0000` | `0x4BFFFF` | （A/B 備份槽） |
| rodata | 6 MB | `0x4C0000` | `0xABFFFF` | `rodata.fatfs` |
| data | 4 MB | `0xAC0000` | `0xEBFFFF` | `data.lfs` |
| 未使用 | ~1.25 MB | `0xEC0000` | `0xFFFFFF` | — |

### 13.2 完整燒錄指令範例

```bash
# 1. 編譯（產出 img）
scons --apply-def=d21x_d215-demo88-nor-dacai03_rt-thread_helloworld_defconfig
scons -j 8

# 2. 確認 img 路徑
dir output\d21x_d215-demo88-nor-dacai03_rt-thread_helloworld\images\*.img

# 3. 裝置進入燒錄模式後
cd tools
upgcmd -l
upgcmd -d 2:5-2-2-1 -p -v image D:\01-WORK\ENE\Project_Git\04-TFT_LCD\Code\Luban-Lite_RTOS_SDK_D215-master\output\d21x_d215-demo88-nor-dacai03_rt-thread_helloworld\images\d21x_demo88_nor_v1.0.0.img
```

### 13.3 SD 卡燒錄範例

複製到 FAT32 SD 卡根目錄：

```
d21x_demo88_nor_v1.0.0.img
bootcfg.txt
```

插入裝置上電即可。

### 13.4 驗證燒錄成功

1. 重啟後 UART 出現 RT-Thread 啟動 log
2. `upgcmd -l` 看不到裝置（正常，已進 Application）
3. 若需再次燒錄：燒錄鍵 / BROM 觸發 / `aicupg`

---

## 14. 常見錯誤與排除

### 14.1 upgcmd 錯誤

| 錯誤訊息 | 原因 | 處理 |
|----------|------|------|
| `Read len 13/16` | 在 BROM 下執行 `shcmd` | 改用 `image` 子命令 |
| `Pipe error(-9)` | BROM STALL 不支援的命令 | 確認 Boot stage；BROM 只能用 `image` |
| `Command '<path>' not found` | 缺少 `image` 關鍵字 | `upgcmd -d ID image file.img` |
| `-l` 顯示 0 個裝置 | 未進燒錄模式 / 已進 Application | 燒錄鍵重啟 |
| 燒錄後 `shcmd` 無反應 | 正常，已進 Application | 預期行為 |
| `Please compile the bootloader first` | 打包時 Bootloader 未編譯 | 先編譯 bootloader defconfig |

### 14.2 啟動失敗

| 現象 | 可能原因 | 處理 |
|------|----------|------|
| 停在 `tinySPL` 不動 | `os` 分區無效 ITB | 重燒完整 img |
| `nor_boot` 失敗 | 分區表與 Bootloader 不匹配 | 重編 Bootloader + 重燒 |
| 反覆進 BROM | Flash 空白或 spl 損壞 | 重新 USB 燒錄 |
| 燒錯映像（NAND img 燒 NOR） | 映像 media 類型不符 | 使用正確 board 的 img |

### 14.3 SD 卡燒錄失敗

| 問題 | 處理 |
|------|------|
| 無 `AiBurn SDCard` 訊息 | 確認 FAT32、單分區、兩個檔案在根目錄 |
| 燒錄後重複進入 | 正常，需手動拔卡重啟 |
| AiBurn 製卡失敗 | 以管理員執行；重新格式化 SD |

### 14.4 映像打包錯誤

| 錯誤 | 處理 |
|------|------|
| 分區大小超出 Flash | 調整 `image_cfg.json` partitions |
| 缺少 required 元件 | 確認 pre-process 產出檔存在於 pack/ |
| LOAD_BASE 未解析 | 先編譯 Bootloader |

---

## 15. 量產與離線燒錄

### 15.1 離線 SPI NOR 燒錄器

適用場景：無 PC、批量生產。

- 使用 **SP32 燒錄器** + 離線燒錄板
- 參考：`doc/topics/sdk/burnsys/burn_packaged_spinor.html`
- 需準備 **雙 Bootloader**（燒錄板用 SPL + 產品用 SPL）

### 15.2 刷机 vs 升級

| 類型 | 說明 | 場景 |
|------|------|------|
| **刷机** | 全碟擦除、重寫分區表與所有元件 | 出廠、換 Flash |
| **升级** | 就地更新部分分區，保留資料 | OTA、現場維修 |

### 15.3 安全燒錄（SPI ENC）

若啟用韌體加密：

- `image_cfg.json` 使用 `.enc` 結尾元件
- 需配置 eFuse 與 SPI ENC 金鑰
- 參考：`doc/topics/sdk/secure/`

---

## 16. 官方文件對照表

| 本文件章節 | SDK webhelp 路徑 |
|-----------|-----------------|
| IMG 結構、工具 | `doc/topics/sdk/advanced/fs_image.html` |
| image_cfg.json | `doc/topics/sdk/advanced/burn-cfg-file.html` |
| 分區表 | `doc/topics/sdk/linux/burn-image-partition-table.html` |
| 燒錄媒介流程 | `doc/topics/sdk/advanced/burn-procedure-by-diff-media.html` |
| 系統更新總覽 | `doc/topics/sdk/burnsys/burnsys_user_guide.html` |
| 更新流程圖 | `doc/topics/sdk/burnsys/flow.html` |
| 平台能力表 | `doc/topics/sdk/burnsys/statis.html` |
| USB 燒錄 | `doc/topics/sdk/burnsys/usb.html` |
| SD 卡燒錄 | `doc/topics/sdk/burnsys/sdcard.html` |
| UART 燒錄 | `doc/topics/sdk/burnsys/uart.html` |
| AiBurn 操作 | `doc/topics/tools/AiBurn-burn-image.html` |
| BringUp 燒錄 | `doc/topics/sdk/bringup/compile-and-burn-firmware.html` |
| 離線 NOR 燒錄 | `doc/topics/sdk/burnsys/burn_packaged_spinor.html` |

**瀏覽方式**：開啟 `doc/index.html`

---

## 附錄 A：快速指令卡

```bash
# === 編譯 ===
scons --apply-def=d21x_d215-demo88-nor-dacai03_baremetal_bootloader_defconfig && scons
scons --apply-def=d21x_d215-demo88-nor-dacai03_rt-thread_helloworld_defconfig && scons

# === USB 燒錄 ===
upgcmd -l
upgcmd -d <bus:port> -p -v image <path>\d21x_demo88_nor_v1.0.0.img

# === 解析映像 ===
upgcmd -i <path>\d21x_demo88_nor_v1.0.0.img

# === SPL 除錯 ===
upgcmd -d <id> shcmd "printenv"
upgcmd -d <id> continue
```

## 附錄 B：相關文件

| 文件 | 路徑 |
|------|------|
| upgcmd 詳細語法 | `custom/Note/upgcmd_燒錄工具使用說明.md` |
| Bootloader 完整說明 | `custom/Note/Luban-Lite_Bootloader完整說明.md` |
| 分區配置 | `target/d21x/d215-demo88-nor-dacai03/pack/image_cfg.json` |

## 附錄 C：燒錄檢查清單

- [ ] 確認 `image_cfg.json` 分區與硬體 Flash 容量一致
- [ ] 已編譯 Bootloader（`bootloader.bin` 存在）
- [ ] 已編譯 Application（`d21x_os.itb` 存在）
- [ ] `.img` 的 `media.type` 與板卡一致（spi-nor）
- [ ] 裝置處於 BROM 或 SPL（`upgcmd -l` 確認）
- [ ] 使用正確指令格式（含 `image` 子命令）
- [ ] 燒錄完成後重啟驗證 RT-Thread 啟動
