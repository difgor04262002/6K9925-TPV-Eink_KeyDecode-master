# 6K9925-TPV-Eink_KeyDecode-master

## 電子紙模組

面板是清越 P118F04 / KEGM011800F04，1.18 吋、六色，實體解析度 240×206。程式把圖縮到這 240×206 個點再送上玻璃，不能把點數做得比面板更多。編譯條件是 `AIC_EPD_P118F04`。

程式在 `application/rt-thread/helloworld/Eink_Driver/`。

| 檔案 | 功能 |
|------|------|
| `epd_usb_image.c` | 接收電腦傳來的 JPEG 或 PNG，存檔、縮放、抖色，再叫面板刷新 |
| `epd_png.c` | 把 PNG 解成 BGR |
| `epd_p118f04.c` | 對 HX5325 送初始化、圖框和升壓刷新 |
| `epd_carpet_frame.c` | 出廠畫面的打包圖框 |
| `epd_tjpgd.c` | JPEG 解碼 |
| `SConscript` | 把上面的檔案加進 helloworld |

### 對外函式

| 函式 | 檔案 | 功能 |
|------|------|------|
| `epd_usb_on_out()` | `epd_usb_image.c` | USB bulk OUT 的入口。辨認 9-byte 命令，或把後面的圖檔先收進記憶體 |
| `epd_kick()` | `epd_p118f04.c` | 叫醒 `epd` 執行緒，讓它離開 USB 中斷去存檔和刷新 |
| `epd_usb_mem_init()` | `epd_usb_image.c` | 開機時配置收圖用的 RAM |
| `epd_usb_poll()` | `epd_usb_image.c` | 在 `epd` 執行緒裡存檔、解圖、呼叫 `epd_show_packed()` |
| `epd_png_to_bgr()` | `epd_png.c` | 把 PNG 檔解成 BGR，寬高由輸出參數帶回 |
| `epd_show_packed()` | `epd_p118f04.c` | 把已打包的 240×206 六色圖框寫進控制器並刷新 |

`epd_p118f04_start()` 用 `INIT_APP_EXPORT` 在開機建立 `epd` 執行緒。`EPD_HOLD_IMAGE` 為 1 時，開機不重刷出廠圖，等電腦傳圖才更新。

### 傳圖命令

圖檔存在 `/data/Eink_Image/image.jpg`，內容可以是 JPEG 或 PNG。命令長度 9 bytes，後面可補 0 到 USB 封包長度。

| 命令 | 功能 |
|------|------|
| `B3 10 FF 11 22 33 FF 10 B3` | 開始收圖。命令之後到下一道命令之前的資料是圖檔 |
| `B3 10 FF AA AA AA FF 10 B3` | 把已存的圖逆時針轉 90 度再顯示 |
| `B3 10 FF BB BB BB FF 10 B3` | 把已存的圖順時針轉 90 度再顯示 |

旋轉是累加的，0 度、90 度、180 度、270 度循環。圖會居中裁成滿版 240×206。來源比面板大時用區塊平均縮小；比面板小時用 Catmull-Rom 放大。最後抖成六色：黑、白、黃、紅、藍、綠。每兩個畫素編成 1 byte，一幀 24720 bytes。

### 面板刷新

`epd_show_packed()` 的順序：

1. `epd_init_otp()`：硬體復位，讀 OTP 電壓代碼（`0x94`），寫回電源、升壓和 VCOM。讀到的代碼超出規格時，不把 `FF` 寫進升壓暫存器。
2. `epd_show_frame()`：用 `0x10` 送出圖框。
3. `epd_update()`：`0x04` 啟動升壓，`0x12` 跑波形，`0x02` 關閉升壓。BUSY 沒有拉低代表升壓沒有啟動，此時不送深度睡眠。
4. `epd_sleep()`：波形跑完後送 `0x07 A5`，控制器進入深度睡眠。下一張圖會先拉 `RST_N` 再重新讀 OTP。

SPI 是 3-wire：CS 每 byte 拉一次，第一個 bit 為 0 表示命令、為 1 表示資料，後面 8 個 bit。DC 保持低電位。

| 信號 | SoC |
|------|-----|
| CSB | PE.5 |
| SDA | PE.7 |
| SCL | PE.8 |
| BUSY_N | PE.9 |
| DC | PE.10 |
| RST_N | PE.11 |

PE.6 沒有接到模組。