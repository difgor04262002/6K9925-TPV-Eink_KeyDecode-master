# Luban-Lite Bootloader（tinySPL）完整說明

> **文件版本**：2026-06-18  
> **適用 SDK**：Luban-Lite RTOS SDK（本專案 D21x / D215）  
> **資料來源**：SDK 內 `doc/topics/sdk/boot/`（RTOS SDK 使用指南）、`application/baremetal/bootloader/` 原始碼、`target/` 板級配置  
> **說明**：工作區內未附 `Luban-Lite_SDK.pdf`；本文件依 SDK 官方 webhelp 與程式碼整理，內容與 PDF 版 **RTOS SDK 使用指南 → 高级应用 → 启动引导** 章節對應。

---

## 目錄

1. [概述](#1-概述)
2. [啟動流程（BROM → PBP → tinySPL → Application）](#2-啟動流程)
3. [tinySPL 軟體架構](#3-tinyspl-軟體架構)
4. [原始碼目錄結構](#4-原始碼目錄結構)
5. [編譯與產物](#5-編譯與產物)
6. [Kconfig 配置詳解](#6-kconfig-配置詳解)
7. [連結腳本與記憶體配置](#7-連結腳本與記憶體配置)
8. [開機命令與 Console](#8-開機命令與-console)
9. [aicupg 升級機制](#9-aicupg-升級機制)
10. [與 Application 的銜接](#10-與-application-的銜接)
11. [除錯與測試](#11-除錯與測試)
12. [d215-demo88-nor-dacai03 範例](#12-d215-demo88-nor-dacai03-範例)
13. [常見問題](#13-常見問題)
14. [官方文件對照表](#14-官方文件對照表)

---

## 1. 概述

Luban-Lite SDK 的 Bootloader 稱為 **tinySPL**（Secondary Program Loader），是一個 **baremetal** 程式，具備：

- 從 SPI NOR / SPI NAND / eMMC·SD 載入 Application
- USB / UART / SD 卡 / U 碟 **燒錄與升級**（aicupg 協議）
- 互動式 **Console**（類 U-Boot 命令列）

**重要**：本 SDK **不使用 U-Boot**，tinySPL 為 ArtInChip 自研輕量 Bootloader。

啟動時 Console 會印出：

```
tinySPL [Built on <date> <time>]
```

---

## 2. 啟動流程

### 2.1 四階段總覽

```
上電 / Reset
    │
    ▼
┌─────────────────────────────────────────┐
│  BROM（Boot ROM）                        │
│  · SoC 內建 Mask ROM，無法更新            │
│  · 最小 CPU 初始化                        │
│  · 掃描啟動介質或等待 USB 燒錄            │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  PBP（PreBootProgram）                   │
│  · DDR 初始化（ddr_init.bin + d21x.pbp） │
│  · 打包在 bootloader.aic 內               │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  tinySPL（本文件主體）                    │
│  · 位置：Flash `spl` 分區                 │
│  · 載入 os 分區 FIT 映像 → 跳轉 Application│
│  · 或進入 aicupg 燒錄模式                 │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│  Application（RT-Thread / baremetal）    │
│  · 位置：Flash `os` 分區（d21x_os.itb）   │
└─────────────────────────────────────────┘
```

### 2.2 BROM 啟動介質掃描順序

依官方 **系统更新流程**（`doc/topics/sdk/burnsys/flow.html`）：

1. SDMC1（SD 卡）
2. SPI NAND
3. SPI NOR
4. SDMC0（eMMC）

**BROM 判斷邏輯摘要**：

| 條件 | 行為 |
|------|------|
| SD 卡為啟動碟 | 從 SD 卡啟動 |
| SD FAT32 上有 `.img` + `bootcfg.txt` | SD 卡燒錄模式 |
| SPI NAND/NOR/eMMC 有有效資料 | 正常啟動 |
| 空 Flash / 燒錄鍵 / USB 連 PC | 進入 USB 燒錄（BROM aicupg） |

### 2.3 tinySPL 執行流程

入口：`application/baremetal/bootloader/main.c`

```
Reset_Handler (startup_gcc.S)
  → SystemInit / cache
  → main()
      → hal_efuse_init / hal_syscfg_probe / drv_dma_init
      → bd = aic_get_boot_device()
      → board_init()        // 時鐘、Pinmux、Heap、UART
      → console_init()
      → bl_upgmode_detect() // 是否進入 aicupg
      → console_set_bootcmd("nor_boot" | "mmc_boot" | ...)
      → console_loop()
          → 先執行 bootcmd 載入 Application
          → 失敗則停在 aic> 命令列
```

---

## 3. tinySPL 軟體架構

### 3.1 分層

| 層級 | 內容 |
|------|------|
| **Application 層** | `main.c`、Console 命令（`cmd/`） |
| **升級層** | `lib/aicupg/`（USB/UART 傳輸、NOR/NAND/MMC 寫入） |
| **映像層** | `lib/fitimage/`（FIT/ITB 解析）、`lib/common/image.c` |
| **驅動層** | BSP baremetal 驅動（QSPI NOR、SDMC、USB CherryUSB） |
| **HAL/Board** | 與 Application 共用 `target/<chip>/<board>/` |

### 3.2 設計特點（官方設計說明）

- **Baremetal**：無 RTOS 執行緒，開機路徑精簡
- **懶加載驅動**：按需初始化，縮短啟動時間
- **Console + bootcmd**：類 U-Boot 的 `bootcmd` 自動啟動機制
- **命令註冊**：`CONSOLE_CMD` 巨集將命令放入 `.tinyspl.console.cmd` section

---

## 4. 原始碼目錄結構

```
application/baremetal/bootloader/
├── main.c                    # 入口
├── Kconfig                   # Bootloader 所有編譯選項
├── SConscript
├── ldscript/
│   ├── d21x_bootloader_gcc.ld.S   # D21x 連結腳本模板
│   └── d21x_bootloader_gcc.ld     # 編譯後展開（勿手改）
├── cmd/
│   ├── nor_boot.c            # SPI NOR 啟動
│   ├── nand_boot.c           # SPI NAND 啟動
│   ├── mmc_boot.c            # eMMC/SD 啟動
│   ├── xip_boot.c            # XIP 直接從 Flash 執行（可選）
│   ├── aicupg.c              # aicupg 命令
│   ├── aicupg_detect.c       # 開機升級模式偵測
│   ├── aicupg_erase.c
│   ├── mem.c / part.c / reset.c / run.c / ...
│   └── progress_bar.c        # 開機進度條（需 DE 驅動）
├── include/
│   ├── aicupg.h / boot.h / image.h / fitimage.h
│   └── data_trans_layer.h    # aicupg USB 傳輸層 VID/PID 定義
└── lib/
    ├── common/               # boot_app.c, image.c, config_parse.c
    ├── fitimage/             # spl_load_simple_fit()
    ├── aicupg/               # nor_fwc.c, nand_fwc.c, upg_main.c, ...
    └── absystem/               # A/B 系統（可選）
```

---

## 5. 編譯與產物

### 5.1 獨立編譯 Bootloader

```bash
# 套用 bootloader defconfig
scons --apply-def=d21x_d215-demo88-nor-dacai03_baremetal_bootloader_defconfig

# 編譯
scons -j 8
```

**defconfig 關鍵項**：

```
CONFIG_PRJ_KERNEL="baremetal"
CONFIG_PRJ_APP="bootloader"
CONFIG_PRJ_CUSTOM_LDS="application/baremetal/bootloader/ldscript/d21x_bootloader_gcc.ld"
```

### 5.2 產物路徑

| 產物 | 路徑 |
|------|------|
| ELF / map | `output/d21x_<board>_baremetal_bootloader/images/` |
| 原始 binary | `target/d21x/<board>/pack/bootloader.bin` |

### 5.3 一鍵編譯（Bootloader + App + 打包 img）

使用 OneStep 腳本（`tools/scripts/onestep/`）：

| 命令 | 說明 |
|------|------|
| `bm` | Bootloader menuconfig |
| `mu` / `ms` | 只編譯 Bootloader |
| `mb` / `m` | Bootloader + Application + 最終 `.img` |

`make_boot.bat` 邏輯：

1. 讀取目前 app 的 `.defconfig`
2. 自動切換到 `<chip>_<board>_baremetal_bootloader_defconfig` 編譯 Bootloader
3. 還原 app defconfig

### 5.4 何時必須重編 Bootloader

| 情況 | 是否重編 |
|------|----------|
| 修改 `image_cfg.json` 分區表 | **是** |
| 修改 aicupg / 啟動介質 Kconfig | **是** |
| 新板卡首次 BringUp | **是** |
| 只改 Application 程式、分區未變 | 否（沿用既有 `bootloader.bin`） |

> **注意**：僅編譯 Bootloader **不會**產生最終燒錄 `.img`；完整映像需在 **Application 專案** build 時由 `mk_image.py` 打包。

---

## 6. Kconfig 配置詳解

選單路徑：**Bootloader options**（`application/baremetal/bootloader/Kconfig`）

### 6.1 記憶體

| 選項 | 預設 | 說明 |
|------|------|------|
| `AIC_BOOTLOADER_MEM_AUTO` | y | 自動計算 TEXT/HEAP 位址寫入 `rtconfig.h` |
| `AIC_BOOTLOADER_TEXT_BASE` | — | 手動：程式連結位址（auto 關閉時） |
| `AIC_BOOTLOADER_TEXT_SIZE` | `0x3FF00` | 程式區大小上限 |
| `AIC_BOOTLOADER_HEAP_BASE` | — | 手動：Heap 起始 |
| `AIC_BOOTLOADER_HEAP_SIZE` | `0xC0000` | Heap 大小 |

**d215 dacai03 bootloader defconfig 實際值**：

- `TEXT_SIZE = 0x7FF00`（約 512KB − 256B）
- `HEAP_SIZE = 0x280000`（2.5MB）

### 6.2 Console

| 選項 | 預設 | 說明 |
|------|------|------|
| `AIC_BOOTLOADER_CONSOLE_UART` | 0 | Console 使用的 UART 編號 |

需在 **Board options** 啟用對應 `AIC_USING_UARTx`。

### 6.3 啟動介質驅動

| 選項 | 預設 | 說明 |
|------|------|------|
| `AIC_BOOTLOADER_MMC_SUPPORT` | n | eMMC/SD 啟動 |
| `AIC_BOOTLOADER_SPINOR_SUPPORT` | y | SPI NOR |
| `AIC_BOOTLOADER_SPINAND_SUPPORT` | y | SPI NAND |
| `AIC_BOOTLOADER_PSRAM_EN` | n | PSRAM |
| `AIC_BOOTLOADER_AXICFG_SUPPORT` | n | AXI 匯流排優先權 |

### 6.4 aicupg 升級（`AICUPG_SUPPORT`）

| 選項 | 預設 | 說明 |
|------|------|------|
| `AICUPG_USB_ENABLE` | y | USB 升級（CherryUSB） |
| `AICUPG_UART_ENABLE` | n | UART 升級 |
| `AICUPG_HID_ENABLE` | n | HID 升級 |
| `AICUPG_SDCARD_ENABLE` | n | SD 卡 FAT 升級 |
| `AICUPG_UDISK_ENABLE` | n | U 碟 FAT 升級 |
| `AICUPG_NOR_ARTINCHIP` | n | 寫入 SPI NOR |
| `AICUPG_NAND_ARTINCHIP` | n | 寫入 SPI NAND |
| `AICUPG_MMC_ARTINCHIP` | n | 寫入 MMC |
| `AICUPG_FORCE_UPGRADE_SUPPORT` | n | 開機強制升級偵測 |
| `AICUPG_LOG_BUFFER_SUPPORT` | y | 遠端 log buffer（D21x: `0x100000`, 12KB） |

### 6.5 Console 命令

| Kconfig | 命令 | 功能 |
|---------|------|------|
| `AIC_BOOTLOADER_CMD_NOR_BOOT` | `nor_boot` | 從 NOR `os` 分區啟動 |
| `AIC_BOOTLOADER_CMD_NAND_BOOT` | `nand_boot` | 從 NAND 啟動 |
| `AIC_BOOTLOADER_CMD_MMC_BOOT` | `mmc_boot` | 從 eMMC/SD 啟動 |
| `AIC_BOOTLOADER_CMD_XIP_BOOT` | `xip_boot` | XIP 模式啟動 |
| `AIC_BOOTLOADER_CMD_SPI_NOR` | `spinor` | SPI NOR 測試 |
| `AIC_BOOTLOADER_CMD_MTD` | `mtd` | MTD 讀寫 |
| `AIC_BOOTLOADER_CMD_MEM` | `mem` | 記憶體讀寫 |
| `AIC_BOOTLOADER_CMD_PART` | `part` | 分區資訊 |

### 6.6 連結腳本

**Chip options** → **Custom link script path**：

```
application/baremetal/bootloader/ldscript/d21x_bootloader_gcc.ld
```

---

## 7. 連結腳本與記憶體配置

### 7.1 D21x 記憶體地圖

```
0x40000000 ┌──────────────────────────────┐
           │  低址 DRAM（保留區）           │  ← nor_boot 載入 Application ITB
           │  0x40000000 ~ 0x43D00000     │     reserve_size ≈ 61 MB
           ├──────────────────────────────┤
0x43D00000 │  AIC 映像載入點（LOAD_BASE）  │  ← bootloader.aic 載入此處
0x43D00100 ├──────────────────────────────┤
           │  DRAM_SW（.text/.rodata/     │  ← tinySPL 程式本體
           │           .data/.bss）         │     LENGTH = TEXT_SIZE
0x43D80000 ├──────────────────────────────┤
           │  Heap（malloc）                │  ← USB 升級、FIT 解析緩衝
0x44000000 └──────────────────────────────┘
           │  其餘 DRAM / CMA               │
0x43FFFFFF └──────────────────────────────┘  （64MB DRAM 末端）
```

### 7.2 自動計算公式（`AIC_BOOTLOADER_MEM_AUTO=y`）

```
heap_base = CPU_DRAM_BASE + AIC_DRAM_TOTAL_SIZE - AIC_BOOTLOADER_HEAP_SIZE
text_base = heap_base - AIC_BOOTLOADER_TEXT_SIZE
load_base = text_base - 0x100    // AIC 256-byte header
```

**dacai03 實際值**：

| 符號 | 位址 |
|------|------|
| `LOAD_BASE` | `0x43D00000` |
| `TEXT_BASE`（entry） | `0x43D00100` |
| `HEAP` | `0x43D80000` ~ `0x44000000` |

### 7.3 Section 配置摘要

| Section | 內容 |
|---------|------|
| `.text` | 程式碼、`Reset_Handler` |
| `.rodata` | 常數、`.usbh_class_info`、ctors |
| `.data` | 初值變數、**`.tinyspl.console.cmd`** 命令表 |
| `.bss` | 零初始化變數 |
| `._dram_sw_sw_heap` | 512B 佔位（非主 heap） |

### 7.4 USB 燒錄時 RAM 位址（`image_cfg.json`）

| 元件 | RAM 位址 | 說明 |
|------|----------|------|
| `usbupg-ddr-init.aic` | `0x00103000` | PBP/DDR init（BROM 階段） |
| `bootloader.aic` | `0x41000000` | SPL 載入（BROM → SPL 跳轉） |

---

## 8. 開機命令與 Console

### 8.1 bootcmd 自動啟動

`main.c` 依 `aic_get_boot_device()` 設定：

| boot_device | bootcmd |
|-------------|---------|
| `BD_SPINOR` | `nor_boot` 或 `xip_boot` |
| `BD_SPINAND` | `nand_boot` |
| `BD_SDMC0/1` | `mmc_boot` |

### 8.2 nor_boot 流程

```c
mtd = mtd_get_device("os");
spl_load_simple_fit(&info, &entry_point);  // 解析 d21x_os.itb
boot_app((void *)entry_point);              // 跳轉 RT-Thread
```

`boot_app()` 會：清 cache → 關中斷 → `ep(boot_device, boot_args)` 跳轉。

### 8.3 進入互動 Console

- 開機時按住 **Ctrl+C**
- `bootcmd` 執行失敗
- 升級過程中 Ctrl+C

常用命令：

```
help
nor_boot
aicupg usb 0
aicupg brom          # 重啟進 BROM 燒錄
mtd read/write ...
mem ...
printenv / setenv  # 若啟用 env
```

---

## 9. aicupg 升級機制

### 9.1 開機自動偵測（`bl_upgmode_detect`）

`cmd/aicupg_detect.c` 依序檢查：

| 條件 | 行為 |
|------|------|
| `REBOOT_REASON_BL_UPGRADE` | Application 執行 `aicupg gotobl` 後重啟 |
| `REBOOT_REASON_UPGRADE` | 進 BROM 升級（需清除 reason） |
| `bd == BD_USB` | USB 啟動 → USB/UART 升級 |
| SD FAT32 啟動碟 | `aicupg fat mmc` |
| U 碟偵測 | `aicupg fat udisk` |
| UserID 未鎖 + PC 連線 | 燒錄 UserID |
| Force upgrade | 強制升級模式 |

### 9.2 USB 燒錄三階段（PC upgcmd）

```
1. PC → BROM：送 updater.ddr（DDR init）
2. PC → BROM：送 updater.spl（bootloader.aic）→ 裝置 USB 重新列舉
3. PC → SPL：送完整 .img target 元件 → 寫入 Flash 各分區 → 自動重啟
```

### 9.3 USB Descriptor（SPL 階段）

檔案：`packages/third-party/cherryusb/demo/aicupg_for_usb.c`

| 項目 | 預設值 |
|------|--------|
| VID | `0x33C3` |
| PID | `0x6677` |
| Manufacturer | `Artinchip` |
| Product | `Artinchip Device` |

修改 VID/PID/名稱請改此檔的 `#define` 與 String Descriptor，並同步 PC 端 `upgcmd` 與 libusbK INF。

### 9.4 D21x 平台升級能力

| 功能 | USB | UART | SD 卡 | U 碟 |
|------|-----|------|-------|------|
| 刷機（全碟） | ✅ | ❌ | ✅ | ❌ |
| 升級（OTA 式） | ✅ | ❌ | ✅ | ✅ |

---

## 10. 與 Application 的銜接

### 10.1 Boot 參數傳遞

`bsp/artinchip/sys/d21x/include/boot_param.h`：

- `aic_get_boot_device()`：從哪個介質啟動
- `aic_get_boot_reason()`：冷/暖啟動
- `aic_get_boot_args()`：映像版本等

### 10.2 從 Application 觸發燒錄

| 方法 | 說明 |
|------|------|
| Shell：`aicupg` / `aicupg gotobl` | 設 reboot reason 後重啟進 SPL |
| 自訂 9-byte BROM 觸發 | 本專案 `custom.c` → `custom_trigger_brom_mode()` |
| 硬體燒錄鍵 / 空 Flash | 進 BROM USB |

### 10.3 Application USB（與 aicupg 不同）

RT-Thread 開機後使用 `usbd_vender_template.c`（Vendor USB），VID/PID 與 aicupg **不同**。此階段 `upgcmd` **無法**連線。

---

## 11. 除錯與測試

### 11.1 啟用 aicupg 除錯

**Bootloader options → Debug**：

- `AICUPG_DEBUG`：aicupg 協議 log
- `AIC_SHOW_BOOT_TIME`：各階段啟動耗時

### 11.2 測試要點（官方測試指南）

1. 確認 `tinySPL` banner 出現
2. 確認 `nor_boot` 能載入 Application
3. USB 燒錄後重啟進 Application
4. Ctrl+C 進 Console，`help` 列出命令
5. `mtd read` 驗證分區內容

### 11.3 常用除錯手段

- 查看 `output/.../baremetal_bootloader/images/d21x.map` 確認 section 大小
- 增大 `AIC_BOOTLOADER_HEAP_SIZE` 若 USB 升級 OOM
- 增大 `AIC_BOOTLOADER_TEXT_SIZE` 若連結 overflow

---

## 12. d215-demo88-nor-dacai03 範例

### 12.1 defconfig 對照

| 用途 | defconfig 檔 |
|------|-------------|
| Bootloader | `target/configs/d21x_d215-demo88-nor-dacai03_baremetal_bootloader_defconfig` |
| Application | `target/configs/d21x_d215-demo88-nor-dacai03_rt-thread_helloworld_defconfig` |

### 12.2 Bootloader 關鍵配置

```
CONFIG_AIC_BOOTLOADER_CMD_NOR_BOOT=y
CONFIG_AICUPG_SDCARD_ENABLE=y
CONFIG_AICUPG_NOR_ARTINCHIP=y
CONFIG_AIC_BOOTLOADER_TEXT_SIZE=0x7FF00
CONFIG_AIC_BOOTLOADER_HEAP_SIZE=0x280000
CONFIG_AIC_DRAM_TOTAL_SIZE=0x4000000
```

### 12.3 Flash 分區（16MB SPI NOR）

| 分區 | 大小 | 內容 |
|------|------|------|
| spl | 512 KB | `bootloader.aic` |
| env / env_r | 各 128 KB | `env.bin` |
| os | 2 MB | `d21x_os.itb` |
| os_r | 2 MB | A/B 備份槽 |
| rodata | 6 MB | `rodata.fatfs` |
| data | 4 MB | `data.lfs` |

### 12.4 bootloader.aic 打包鏈

```
ddr_init.bin + d21x.pbp  →  pbp_ext.aic
bootloader.bin           →  loader.aic
concatenate              →  bootloader.aic  →  燒入 spl 分區
```

---

## 13. 常見問題

| 問題 | 原因 | 處理 |
|------|------|------|
| `mk_image.py`: compile bootloader first | 未先編譯 Bootloader | 先 `scons` bootloader defconfig |
| 改分區後無法啟動 | spl 分區或 bootcmd 不匹配 | 重編 Bootloader 並重燒 |
| USB 升級後 `upgcmd -l` 看不到裝置 | 正常，已進 Application | 用 UART Console 確認 |
| `nor_boot` 失敗 | `os` 分區無有效 ITB | 重燒完整 `.img` |
| Heap 不足 / USB 傳輸失敗 | HEAP_SIZE 太小 | 增大 `AIC_BOOTLOADER_HEAP_SIZE` |
| 連結 overflow | TEXT_SIZE 太小 | 增大 `AIC_BOOTLOADER_TEXT_SIZE` 或關閉不需要的命令 |

---

## 14. 官方文件對照表

| 本文件章節 | SDK webhelp 路徑 |
|-----------|-----------------|
| 概述、啟動流程 | `doc/topics/sdk/boot/lite_boot_direct.html` |
| Kconfig、連結腳本 | `doc/topics/sdk/boot/lite_boot_config_guide.html` |
| 軟體架構 | `doc/topics/sdk/boot/lite_boot_design_guide.html` |
| 除錯 | `doc/topics/sdk/boot/lite_boot_debug_guide.html` |
| 測試 | `doc/topics/sdk/boot/lite_boot_test_guide.html` |
| 系統更新流程 | `doc/topics/sdk/burnsys/flow.html` |
| BringUp 編譯燒錄 | `doc/topics/sdk/bringup/compile-and-burn-firmware.html` |
| OneStep 命令 | `doc/topics/sdk/advanced/commands.html`（onestep 章節） |

**瀏覽方式**：開啟 `doc/index.html` → **高级应用** → **启动引导**。

---

## 附錄：快速指令卡

```bash
# 編譯 Bootloader
scons --apply-def=d21x_d215-demo88-nor-dacai03_baremetal_bootloader_defconfig && scons -j8

# Bootloader menuconfig
scons --apply-def=..._baremetal_bootloader_defconfig && scons --menuconfig

# 完整映像（Bootloader + App + img）
# 使用 OneStep: mb
```

```
# Console（UART）
aic> nor_boot
aic> aicupg usb 0
aic> aicupg brom
aic> help
```
