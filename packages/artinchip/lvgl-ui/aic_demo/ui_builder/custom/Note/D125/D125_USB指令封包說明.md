# D125 裝置與 PC 端 USB 指令封包說明

> 適用平台：D125 / D12P + `demo88-nor` + ZC-T1D8BP（PID **0xB309**）  
> 解析入口：`custom_recv_18bytes()`（`custom.c`）← CherryUSB vender bulk OUT  
> 影像組包：`custom_image_recv()`（`custom_Image.c`）  
> 寫入日期：2026-09-17  
>
> 本文件以**韌體實際接受的位元組**為準，不是過期的 Area2／Video 註解。同一條 USB 線也可走 UART（逐 byte 進同一個函式）。裝置**不會**對影像串流回 ACK；PC 不需要等 IN 才能送下一張 JPEG。

---

## 1. 實體通道

### 1.1 USB WinUSB vender（本方案主路徑）

來源：`packages/third-party/cherryusb/demo/usbd_vender_template.c`

| 項 | 值 |
|----|-----|
| VID | **0x0CF2**（ENE） |
| PID（本玻璃） | **0xB309**（`AIC_PANEL_DBI_ZC_T1D8BP`） |
| 類別 | Vendor specific `0xFF`，WinUSB（BOS + MS OS 2.0） |
| 介面 | 單功能時 MI_00；composite 時 vender 仍應在 **MI_00** |
| Bulk OUT | **EP 0x01**（PC → 裝置，所有指令與圖檔） |
| Bulk IN | **EP 0x81**（裝置 → PC，僅 launcher 通知／測試；圓屏幾乎不用） |
| HS wMaxPacketSize | 512 |
| 韌體 OUT 讀緩衝 | **131072** byte（`VENDER_OUT_READ_BUF_LEN`） |

OUT callback：`usbd_vender_out` → `custom_recv_18bytes(read_buffer, nbytes)` → 立刻再 `usbd_ep_start_read`。

PC 必須用 **WinUSB bulk OUT** 送下面所有封包。不要送到 USB Display 介面（若有開 composite 的 MI_01）。

### 1.2 UART（除錯／沒有 USB 時）

`ui_uart_rx` 逐 byte 呼叫 `custom_recv_18bytes(&ch, 1)`。協定與 USB 相同，但：

- 完整 18-byte AA、9-byte BROM、12-byte 副屏，UART 上要靠逐 byte 狀態機拼出來。
- 影像 payload 高熵，BROM／副屏逐 byte 比對在收圖期間會關掉。
- **不要**佔用 console UART（`RT_CONSOLE_DEVICE_NAME`）。

### 1.3 裝置 → PC（IN）

圓屏 JPEG／GIF **沒有**對應的成功／失敗回包。IN 現況：

| 來源 | 9 byte | 何時 |
|------|--------|------|
| `screen_main` 圖示點擊 | `FA FB FC id id id EC EB EA` | 僅 AA 01 大屏 launcher；**圓屏忽略 AA 01，不會送** |
| MSH `test_vender` | `FA FB FC 00 00 00 EC EB EA` | 手動測 IN |

PC 若要收 IN，必須先掛 **ReadPipe(EP 0x81)**，否則 Bus Hound 看不到。影像工具不必開 IN。

`usbd_vender_send_ack_bytes` 註解提到 CD-AA 的 `0x81`／`0x83` 覆蓋規則，那是其他產品線；**本方案 `custom.c` 不組這種 ACK。**

---

## 2. 韌體分流順序（PC 必須知道）

`custom_recv_18bytes` **固定順序**，同一個 USB 傳輸只會走第一個命中的控制框：

```
1. MMOSIE 10 或 11 byte（僅 ZC）
2. 整包恰等於 BROM 9 byte     → 立刻重置進升級，後面資料丟掉
3. 整包恰等於 USB Display 12 byte
4. 若已在副屏：只掃 disable／時間／BROM，不吃 AA、CD-BB
5. 逐 byte：時間、BROM、副屏（收圖中或已知 AA／CD-BB 時跳過）
6. 若 len==18 且 AA + 合法 cmd：切屏 + metrics
7. custom_image_recv：6-byte CD-BB 或影像 payload
```

規則：

- **控制封包請單獨一次 bulk OUT**（長度剛好等於該指令），不要跟 JPEG 黏在同一個 128 KB callback 的開頭又不是 MMOSIE 格式。
- MMOSIE 例外：同一筆 OUT 可以是「10/11 byte 控制 + 後面 GIF 檔頭」。
- 收圖進行中再送 AA／BROM／副屏，AA 會被 Image 丟掉；BROM 逐 byte 關閉。要切畫面請等短封包結束或先停流。

---

## 3. 指令總表（PC → 裝置）

| 名稱 | 長度 | 開頭 | D125 圓屏 | 作用 |
|------|------|------|-----------|------|
| AA 監控／切屏 | 18 | `AA` | 00、06 有效；01／02 忽略 | 切畫面 + CPU/GPU 數字 |
| Area4 圖 | 6 + payload | `CD BB 06` | **主路徑** | JPEG／PNG 串流上屏 |
| main_1 圖 | 6 + payload | `CD BB 02` | 不顯示（忽略 AA 02） | 大屏三槽 |
| MMOSIE 選片 | 10 或 11 | `F9 F9 F2 F5` | 有效 | NY8 四 bit + 指定 Panel_1..4 |
| 存檔 GIF／APNG | MMOSIE 後 raw | `GIF87a`／`GIF89a`／PNG | 有效 | 寫 `/data/Panel_n/` |
| 時間 | 10 | `DA EE 01` | 有效 | 更新 splash 時鐘 |
| USB Display 開 | 12 | `DE DF DA EF 11…` | 視 SKU | 進副屏、停 CD-BB |
| USB Display 關 | 12 | `DE DF DA EF 22…` | 視 SKU | 離開副屏 |
| BROM／升級 | 9 | `FF A1 B2 E3…` | **會立刻重置** | 進 bootloader 燒錄；不是壞硬體，但畫面與 App 會沒掉 |
| Transport 20B | 20 + payload | `55 42` | 可選 | 帶長度的影像／結束 |

**已刪、送了沒人理：** `CD BB 04`／`05`（Area2／3）、`AA 03`／`04`／`05`（Video／2_Area／3_Area）、`EF AA … EF` 監控列。

---

## 4. AA 18-byte（切屏 + 監控）

必須 **剛好 18 byte**、`data[0]=0xAA`。cmd 沒變則不重複切屏，但 metrics 每次都更新。

```
偏移  長度  內容
 0     1   0xAA
 1     1   cmd
 2–3   2   CPU 頻率，小端
 4     1   CPU usage
 5–6   2   GPU 頻率，小端
 7     1   GPU usage
 8     1   CPU 溫度
 9     1   GPU 溫度
10–11  2   Storage，小端
12–13  2   Storage 2，小端
14–15  2   RAM，小端
16–17  2   RAM 2，小端
```

| cmd | 意義 | D125 |
|-----|------|------|
| `0x00` | splash（`screen.c`） | 停 USB 收圖，回到 Panel GIF 轉盤 |
| `0x01` | `screen_main` | **忽略**（log 後 return） |
| `0x02` | `screen_main_1` | **忽略** |
| `0x06` | `screen_4_Area` | JPEG 串流；圓屏 `stream_acquire` |

圓屏建議：

- 開 JPEG：可省略 AA 06（第一張 Area4 CD-BB 會自動切）。要明確切仍可送 AA 06。
- 停 JPEG 回開機動畫：送 AA 00，或停送約 90 幀（idle watchdog）。

**AA 不算串流心跳。** 只打 AA 不能阻止回 splash。

範例（AA 06，其餘監控填 0）：

```
AA 06 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

---

## 5. CD-BB 6-byte + 影像 payload

### 5.1 命令框（必須單獨一筆，len = 6）

`CD BB <區段> <A> <B> CD`

#### Area4（D125 串流用這個）

```
CD BB 06 <fmt> 00 CD
```

| fmt | 格式 |
|-----|------|
| `0x00` | JPEG |
| `0x01` | PNG |

內部 slot 固定 **0x30**。GIF／APNG 不要走 Area4。

JPEG 範例：`CD BB 06 00 00 CD`  
PNG 範例：`CD BB 06 01 00 CD`

標頭 `custom_Area4.h` 舊註解（byte3=slot、byte4=PNG）**錯誤**，以這份為準。

#### main_1 三槽（圓屏收了也不會畫）

```
CD BB 02 <type> <slot> CD
```

| type | 格式 |
|------|------|
| `0x00` | JPEG |
| `0x01` | PNG |
| `0x02` | GIF |
| `0x03` | APNG |

| slot | 畫面 |
|------|------|
| `0x01`～`0x03` | `screen_main_1` 三張圖 |

### 5.2 Payload（命令之後的 OUT）

1. 裝置進入 `s_receiving_image`。
2. 後續 OUT 當純檔案位元組（可多包，每包最多 131072）。
3. **結束條件（RAW 模式，本方案預設）：**
   - 某一筆 OUT **長度 < 131072**（USB 短封包／最後一塊）→ `finish_by_short_packet`。
   - 或下一筆又是合法 6-byte CD-BB（收圖中會丟掉「巢狀命令」，但短包仍會結束上一張）。
4. JPEG 也靠短包結束，**不要**只靠檔內 `FF D9`（壓縮資料裡可能出現）。
5. PNG／GIF／APNG 同樣靠短包，不靠 IEND／`0x3B`（可能出現在壓縮段）。

PC 實務：

```
OUT  6B   CD BB 06 00 00 CD
OUT  n1   JPEG chunk（可 512 的倍數，可一次到 128 KB）
OUT  n2   …
OUT  nk   最後一塊，nk < 131072（即使檔案剛好是 128 KB 倍數，也要再送 0 長度或 1 byte 短包）
```

上限 **2 MB**／張。超過 reject。360 JPEG 通常 10–20 KB，一包就能結束。

收圖中若再送 18-byte AA，Image 會 **drop** 這筆控制框。

### 5.3 可選：20-byte transport header（`55 42`）

若 payload 開頭 20 byte 符合，改走「帶長度」模式：

```
 0–1   55 42
 2     type：0=影像，1=（保留），2=明確結束
 3–7   （實作只檢查後面 reserved）
 8–11  payload 長度，大端 u32，須 ≤ 2 MB
12–15  （未強制）
16–19  必須 0
```

`type==2` 且長度欄跑完可 `finish_recv`。現行 PC 工具多半**不送**這 20 byte，直接 RAW + 短包即可。

---

## 6. MMOSIE 選片（僅 ZC / PD.26）

圖檔**之前**送（建議獨立一筆 OUT）。

**10 byte：**

```
F9 F9 F2 F5  <cmd>  <idx>  F9 F9 F2 F5
```

**11 byte：**

```
F9 F9 F2 F5  <cmd>  F0  <idx>  F9 F9 F2 F5
```

| 欄位 | 允許值 |
|------|--------|
| cmd | `0xFA`～`0xFD`（韌體目前只檢查範圍，不分支） |
| idx | `0x01`～`0x04` 或 `0xF1`～`0xF4` |

| idx | PD.26 四 bit | USB 存檔目錄 |
|-----|--------------|----------------|
| 1 | 0,1,1,1 | `/data/Panel_1/Panel_001.gif`（或 .apng） |
| 2 | 1,0,1,1 | `/data/Panel_2/…` |
| 3 | 1,1,0,1 | `/data/Panel_3/…` |
| 4 | 1,1,1,0 | `/data/Panel_4/…` |

`Panel_0` **不能**用 USB 覆寫。

位元真正出現在 PD.26 是 **QSPI1 CS 低**的時候（2A 前導），不是 USB 收到當下。RAMWR 結束 CS 高會再送 `1111`。

同一筆 OUT 若 MMOSIE 後面還有資料，且開頭是 GIF／PNG magic，會直接開始存檔（見 §7）。若後面是 `CD BB` 或 JPEG `FF D8`，**不會**當檔案，arm 取消，改走串流。

---

## 7. 寫入 LittleFS 的 GIF／APNG

順序：

```
1. MMOSIE（指定 1..4）
2. 下一個非 CD-BB、非 JPEG SOI 的 OUT：檔頭必須是
      GIF87a / GIF89a  → 存 .gif
      89 50 4E 47 0D 0A 1A 0A → 若含 acTL 當 APNG，否則當靜態 PNG 會 skip 存檔
3. 繼續送檔案本體，最後短包結束
```

靜態 PNG（沒有 `acTL`）不會寫進 Panel 資料夾。JPEG 串流請用 §5，不要走這條。

存完後圓屏 debounce 700 ms，從 Panel_1 開始轉盤（結束 5 秒 boot hold）。

---

## 8. 時間 10-byte

可夾在任意非收圖、非 AA、非 CD-BB 的位元流裡（UART 逐 byte 也能組）。

```
DA EE 01  YY YY  MM DD HH NN SS
             │
             └─ 年，大端（例如 0x07 E9 = 2025）
```

| 偏移 | 內容 |
|------|------|
| 0–2 | `DA EE 01` |
| 3–4 | year 大端 |
| 5 | month 1–12 |
| 6 | day |
| 7 | hour 0–23 |
| 8 | minute |
| 9 | second |

非法日期會被丟棄。之後裝置用 `lv_tick` 自己加秒。圓屏 Area4 不畫時間 label。

---

## 9. USB Display 副屏 12-byte

必須 **整包長度 = 12** 且完全相符。

**Enable（進 PC 擴充螢幕，停 CD-BB）：**

```
DE DF DA EF 11 44 66 23 EF DA DF DE
```

**Disable（恢復 LVGL／串流）：**

```
DE DF DA EF 22 55 77 34 EF DA DF DE
```

Enable 後 `custom_recv_18bytes` 不再把資料當 AA／影像。本方案若沒編進 CherryUSB Display，enable 只打 stub log。COMBO SKU 會切 descriptor，PC 可能要重開 WinUSB。

**不要**在 USB OUT callback 裡呼叫 `usb_display_*`（韌體已丟給 `usbdisp_sw` 執行緒）。

---

## 10. BROM／升級 9-byte（會立刻重置，不是燒壞 IC）

「危險」是指**流程不可逆**，不是這 9 個 byte 會把 Flash／晶片寫壞。

韌體一認定整包相符就會：

1. 寫 env `aicupg_gotobl=1`
2. 設 reboot reason 為 **`REBOOT_REASON_BL_UPGRADE`**
3. 餵看門狗，**CPU 立刻 reset**

App、LVGL、圓屏 GIF 全部停掉。重啟後 SPL／bootloader 接管 USB，等同 console 下 `aicupg`，等 PC 燒錄工具。沒有「取消」回包；要回到正常畫面必須燒完或依燒錄流程退出後再開機。

因此：

- 量產／展示時**不要**把這 9 byte 跟 JPEG 黏在同一筆 OUT，也不要當心跳亂送。
- JPEG 高熵資料裡 theoretically 可能出現相同序列；韌體因此要求 **整包恰好 9 byte**，收圖期間也不做逐 byte 拼湊。仍請單獨傳。
- 這是給維修／升級用的後門，不是日常顯圖指令。

必須 **整包恰好 9 byte**（USB 最安全）：

```
FF A1 B2 E3 F4 C5 D6 EE EE
```

命中後：寫 env `aicupg_gotobl=1`，reboot reason **`REBOOT_REASON_BL_UPGRADE`**，餵 WDT。裝置進燒錄，**沒有回包**。

收圖／AA／CD-BB 期間不做逐 byte 拼湊，降低 JPEG 誤觸。仍請 PC **永遠單獨送這 9 byte**，不要嵌在圖檔裡。

UART 在非收圖時仍可能逐 byte 拼中。

---

## 11. D125 常用序列

### 11.1 即時 JPEG（圓屏主用途）

```
（可選）AA 06 + 18 個 metrics
循環：
  CD BB 06 00 00 CD
  JPEG bytes…
  最後一塊 < 131072
```

不送 AA 也可以；第一張會自動切 Area4、暫停 GIF。

停播：AA 00，或停止 CD-BB 約 2.5–3 秒。

### 11.2 更新某一格 GIF

```
F9 F9 F2 F5  FA  02  F9 F9 F2 F5     ← 選 Panel_2
GIF89a …（整檔）
短包結束
```

等 700 ms 後轉盤從有檔的 1–4 開始。

### 11.3 選片後立刻串流 JPEG（不要當檔案）

```
F9 F9 F2 F5 FA 01 F9 F9 F2 F5
CD BB 06 00 00 CD
FF D8 … JPEG
```

MMOSIE 後看到 `CD BB` 會取消 flash arm。idx 仍更新 PD.26／panel latch。

### 11.4 校時

```
DA EE 01 07 EA 09 11 0A 1E 00     ← 2026-09-17 10:30:00 為例（07 EA = 2026）
```

---

## 12. PC 實作注意

1. **一次 OUT 一個控制框。** 6／9／10／11／12／18 請獨立傳輸。
2. **HS bulk 最後一包必須短於 131072** 才能結束 RAW 圖。檔案長度是 131072 倍數時加一個 ZLP 或 1 byte。
3. 不要在 JPEG 進行中插入 AA，會被 drop。
4. 不要對影像等 IN ACK。
5. VID/PID 用 **0x0CF2 / 0xB309** 綁 WinUSB。
6. Composite 時指令仍走 **vender EP 0x01**，不是 Display 0x02。
7. 圓屏不要送 AA 01／02 以為能開 launcher。
8. Area4 只吃 JPEG／PNG；動畫 GIF 用 §7。
9. 日誌：`custom:`／`custom_ui:`／`custom_area4:`／`custom_img:`。開 `VENDER_OUT_LOG_VERBOSE` 會把 USB 打到約 2 MB/s，不要量產開。
10. 同一條線 UART 與 USB 都進 `custom_recv_18bytes`，不要兩邊同時灌指令。

---

## 13. 與其他說明的對照

| 檔案 | 內容 |
|------|------|
| 本檔 | **封包格式與 PC 怎麼送** |
| `D125_custom.c說明.md` | 分流、切屏、GIF 轉盤 |
| `D125_custom_Area4.c說明.md` | Area4 上屏、jpgdec |
| `custom_Image_流程說明.md` | 收圖緩衝（部分 Area2 敘述已過期） |
| `USB_Composite_Display功能與修改歷程.md` | 副屏／composite（D215 背景） |
| `D125_QSPI配置說明.md` | 玻璃怎麼畫，與 USB 無關 |

韌體核對點：`custom.c`（`custom_recv_18bytes`、MMOSIE、AA、時間、BROM、副屏）、`custom_Image.c`（CD-BB、短包、flash）、`custom_Area4.c`（`try_parse_cmd`）、`usbd_vender_template.c`（EP／VID／PID）。
