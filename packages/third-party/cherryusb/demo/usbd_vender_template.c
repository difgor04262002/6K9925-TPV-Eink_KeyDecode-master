/*
 * Copyright (c) 2023-2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 */

#include "usbd_core.h"
#include "usbd_vender.h"
#include "aic_drv.h"
#include <string.h>
#include <rtconfig.h>
#include <rtthread.h>

extern void custom_recv_18bytes(const uint8_t *data, uint32_t len) __attribute__((weak));
__attribute__((weak)) uint8_t custom_image_usb_rearm_hold(void)
{
    return 0U;
}
__attribute__((weak)) void custom_image_spill_boost(void)
{
}

/*!< endpoint address */
#define IN_EP  0x81
#define OUT_EP 0x01

#define USBD_VID           0x0CF2//ENE//0x33C3

/* USB PID by TFT panel / display interface (rtconfig.h uses AIC_* without CONFIG_ prefix) */
#if defined(LPKG_ENE_USB_PID_B310)
#define USBD_PID           0xB310  /* 6K9925 TPV Eink KeyDecode */
#elif defined(AIC_PANEL_DSI_R520120T01)
#define USBD_PID           0xB304  /* ST7123 (R520120T01) */
#elif defined(AIC_PANEL_DSI_FL7707_92IN)
#define USBD_PID           0xB305  /* FL7707N 9.2" (YX092CTIPS / AV092K5U-N10) */
#elif defined(AIC_PANEL_DSI_H0499S010)
#define USBD_PID           0xB307  /* ST7121P H0499S010 5.0" */
#elif defined(AIC_DISP_LVDS)
#define USBD_PID           0xB306  /* LVDS */
#elif defined(AIC_PANEL_DSI_EK79007AD)
#define USBD_PID           0xB308  /* D125CCS EK79007AD 1024x600 */
#elif defined(AIC_PANEL_DBI_ZC_T1D8BP)
#define USBD_PID           0xB309  /* ZC-T1D8BP-012 360x360 QSPI */
#else
#define USBD_PID           0xB308  /* D125CCS */
#endif

#define USBD_MAX_POWER     100
#define USBD_LANGID_STRING 1033

#ifndef LPKG_CHERRYUSB_DEVICE_COMPOSITE
/*
 * Microsoft OS 2.0 vendor request code (single-function mode only).
 *
 * Windows issues a vendor IN control transfer with bRequest == this code
 * and wIndex == WINUSB_REQUEST_GET_DESCRIPTOR_SET to retrieve the MS OS 2.0
 * descriptor set advertised in the BOS descriptor below.
 *
 * In composite mode the equivalent BOS / MS OS 2.0 set is owned by
 * composite_template.c (with a Function Subset that pins WinUSB to the
 * vender interface only); this template stays minimal and identical to
 * the upstream demo.
 */
#define MSOS20_VENDOR_CODE 0x01

/* Set Header(10) + Compatible ID(20) + Registry Property(132) = 162 bytes. */
#define MSOS20_DESCRIPTOR_SET_LENGTH  0xA2
#endif

/*!< config descriptor size */
#define USB_CONFIG_SIZE (9 + 9 + 7 + 7)

#ifdef CONFIG_USB_HS
#define MAX_MPS 512
#else
#define MAX_MPS 64
#endif

/*!< global descriptor */
static const uint8_t vender_descriptor[] = {
    /*
     * Use bcdUSB = 0x0210 (USB 2.1) so that the host queries the Binary Object
     * Store. Windows only follows MS OS 2.0 enumeration when a BOS descriptor
     * is present, so this is required for automatic WinUSB binding.
     */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_1, 0xFF, 0x00, 0x00, USBD_VID, USBD_PID, 0x0001, 0x01),
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x1, 0x01, 0xC0, USBD_MAX_POWER),
    USB_INTERFACE_DESCRIPTOR_INIT(0x00, 0x00, 0x02, 0xff, 0x00, 0x00, 0x01),
    USB_ENDPOINT_DESCRIPTOR_INIT(OUT_EP, USB_ENDPOINT_TYPE_BULK, MAX_MPS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(IN_EP, USB_ENDPOINT_TYPE_BULK, MAX_MPS, 0x00),
    ///////////////////////////////////////
    /// string0 descriptor
    ///////////////////////////////////////
    USB_LANGID_INIT(USBD_LANGID_STRING),
    ///////////////////////////////////////
    /// string1 descriptor
    ///////////////////////////////////////
    0x14,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'E', 0x00,                  /* wcChar0 */
    'N', 0x00,                  /* wcChar1 */
    'E', 0x00,                  /* wcChar2 */
    'W', 0x00,                  /* wcChar3 */
    'i', 0x00,                  /* wcChar4 */
    'n', 0x00,                  /* wcChar5 */
    'U', 0x00,                  /* wcChar6 */
    'S', 0x00,                  /* wcChar7 */
    'B', 0x00,                  /* wcChar8 */
    ///////////////////////////////////////
    /// string2 descriptor
    ///////////////////////////////////////
    0x2C,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    'E', 0x00,                  /* wcChar0 */
    'N', 0x00,                  /* wcChar1 */
    'E', 0x00,                  /* wcChar2 */
    'W', 0x00,                  /* wcChar3 */
    'i', 0x00,                  /* wcChar4 */
    'n', 0x00,                  /* wcChar5 */
    'U', 0x00,                  /* wcChar6 */
    'S', 0x00,                  /* wcChar7 */
    'B', 0x00,                  /* wcChar8 */
    ' ', 0x00,                  /* wcChar9 */
    'V', 0x00,                  /* wcChar10 */
    'E', 0x00,                  /* wcChar11 */
    'N', 0x00,                  /* wcChar12 */
    'D', 0x00,                  /* wcChar13 */
    'E', 0x00,                  /* wcChar14 */
    'R', 0x00,                  /* wcChar15 */
    ' ', 0x00,                  /* wcChar16 */
    'D', 0x00,                  /* wcChar17 */
    'E', 0x00,                  /* wcChar18 */
    'M', 0x00,                  /* wcChar19 */
    'O', 0x00,                  /* wcChar20 */
    ///////////////////////////////////////
    /// string3 descriptor (iSerialNumber)
    ///////////////////////////////////////
    /*
     * 序號改過。Windows 會以 VID/PID/Serial 為 key 快取
     * 「此裝置不支援 MS OS 2.0」的判定。改一個沒看過的序號可以強迫
     * Windows 把它當全新裝置，重新走一次 BOS + MSOS 2.0 流程，
     * 進而自動掛上 winusb.sys。
     */
    0x16,                       /* bLength */
    USB_DESCRIPTOR_TYPE_STRING, /* bDescriptorType */
    '6', 0x00,                  /* wcChar0 */
    'K', 0x00,                  /* wcChar1 */
    '9', 0x00,                  /* wcChar2 */
    '9', 0x00,                  /* wcChar3 */
    '0', 0x00,                  /* wcChar4 */
    '1', 0x00,                  /* wcChar5 */
    '2', 0x00,                  /* wcChar6 */
    '0', 0x00,                  /* wcChar7 */
    '2', 0x00,                  /* wcChar8 */
    '6', 0x00,                  /* wcChar9 */
#ifdef CONFIG_USB_HS
    ///////////////////////////////////////
    /// device qualifier descriptor
    ///////////////////////////////////////
    0x0a,
    USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00,
    0x02,
    0x02,
    0x02,
    0x01,
    0x40,
    0x01,
    0x00,
#endif
    0x00
};

#ifndef LPKG_CHERRYUSB_DEVICE_COMPOSITE
/*
 * Binary Object Store (BOS) descriptor — single-function mode only.
 *
 * 5-byte BOS header + a single 28-byte Microsoft OS 2.0 Platform Capability
 * descriptor (UUID = D8DD60DF-4589-4CC7-9CD2-659D9E648A9F) = 33 bytes total.
 * Windows fetches BOS when bcdUSB >= 0x0210 and follows the MS OS 2.0 path
 * advertised in the Platform Capability.
 *
 * In composite mode this is owned by composite_template.c.
 */
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t vender_bos_descriptor[] = {
    /* BOS Descriptor (5 bytes) */
    0x05,                   /* bLength */
    0x0F,                   /* bDescriptorType: BOS */
    0x21, 0x00,             /* wTotalLength = 33 */
    0x01,                   /* bNumDeviceCaps */

    /* MS OS 2.0 Platform Capability Descriptor (28 bytes) */
    0x1C,                   /* bLength = 28 */
    0x10,                   /* bDescriptorType: DEVICE CAPABILITY */
    0x05,                   /* bDevCapabilityType: PLATFORM */
    0x00,                   /* bReserved */
    /* PlatformCapabilityUUID = {D8DD60DF-4589-4CC7-9CD2-659D9E648A9F} (LE) */
    0xDF, 0x60, 0xDD, 0xD8,
    0x89, 0x45,
    0xC7, 0x4C,
    0x9C, 0xD2,
    0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
    /* Capability data */
    0x00, 0x00, 0x03, 0x06, /* dwWindowsVersion = 0x06030000 (Win 8.1+) */
    MSOS20_DESCRIPTOR_SET_LENGTH, 0x00, /* wMSOSDescriptorSetTotalLength (162 or 178) */
    MSOS20_VENDOR_CODE,     /* bMS_VendorCode */
    0x00,                   /* bAltEnumCode */
};

/*
 * Microsoft OS 2.0 Descriptor Set — single-function mode only (162 bytes).
 *
 *     Set Header(10) + Compatible ID(20) + Registry Property(132)
 *
 * Microsoft spec lets single-function single-config devices omit the
 * Configuration / Function Subsets (Compatible ID then applies device-wide).
 * The composite-mode variant (with subsets pinning WinUSB to one interface)
 * lives in composite_template.c.
 *
 * Compatible ID "WINUSB" → Windows auto-installs winusb.sys.
 * Registry Property "DeviceInterfaceGUIDs" → user-mode apps locate the
 * device via SetupDi*() or CM_Get_Device_Interface_List_*() and our GUID.
 */
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t vender_msosv2_descriptor[MSOS20_DESCRIPTOR_SET_LENGTH] = {
    /* MS OS 2.0 Descriptor Set Header (10 bytes) */
    0x0A, 0x00,             /* wLength = 10 */
    0x00, 0x00,             /* wDescriptorType = MS_OS_20_SET_HEADER_DESCRIPTOR */
    0x00, 0x00, 0x03, 0x06, /* dwWindowsVersion = 0x06030000 (Win 8.1+) */
    MSOS20_DESCRIPTOR_SET_LENGTH, 0x00, /* wTotalLength = 162 */

    /* MS OS 2.0 Compatible ID Descriptor (20 bytes) */
    0x14, 0x00,             /* wLength = 20 */
    0x03, 0x00,             /* wDescriptorType = MS_OS_20_FEATURE_COMPATIBLE_ID */
    'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,             /* CompatibleID */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,       /* SubCompatibleID */

    /* MS OS 2.0 Registry Property Descriptor (132 bytes) */
    0x84, 0x00,             /* wLength = 132 */
    0x04, 0x00,             /* wDescriptorType = MS_OS_20_FEATURE_REG_PROPERTY */
    0x07, 0x00,             /* wPropertyDataType = REG_MULTI_SZ */
    0x2A, 0x00,             /* wPropertyNameLength = 42 */
    /* bPropertyName = L"DeviceInterfaceGUIDs\0" (21 wide chars = 42 bytes) */
    'D', 0x00, 'e', 0x00, 'v', 0x00, 'i', 0x00,
    'c', 0x00, 'e', 0x00, 'I', 0x00, 'n', 0x00,
    't', 0x00, 'e', 0x00, 'r', 0x00, 'f', 0x00,
    'a', 0x00, 'c', 0x00, 'e', 0x00, 'G', 0x00,
    'U', 0x00, 'I', 0x00, 'D', 0x00, 's', 0x00,
    0x00, 0x00,
    0x50, 0x00,             /* wPropertyDataLength = 80 */
    /* bPropertyData = L"{1D4B2365-4749-48EA-B38A-7C6FDDDD7E26}\0\0" (40 wide chars = 80 bytes) */
    '{', 0x00, '1', 0x00, 'D', 0x00, '4', 0x00,
    'B', 0x00, '2', 0x00, '3', 0x00, '6', 0x00,
    '5', 0x00, '-', 0x00, '4', 0x00, '7', 0x00,
    '4', 0x00, '9', 0x00, '-', 0x00, '4', 0x00,
    '8', 0x00, 'E', 0x00, 'A', 0x00, '-', 0x00,
    'B', 0x00, '3', 0x00, '8', 0x00, 'A', 0x00,
    '-', 0x00, '7', 0x00, 'C', 0x00, '6', 0x00,
    'F', 0x00, 'D', 0x00, 'D', 0x00, 'D', 0x00,
    'D', 0x00, '7', 0x00, 'E', 0x00, '2', 0x00,
    '6', 0x00, '}', 0x00,
    0x00, 0x00, 0x00, 0x00,
};

static struct usb_bos_descriptor vender_bos_desc = {
    .string     = vender_bos_descriptor,
    .string_len = sizeof(vender_bos_descriptor),
};

static struct usb_msosv2_descriptor vender_msosv2_desc = {
    .compat_id     = vender_msosv2_descriptor,
    .compat_id_len = sizeof(vender_msosv2_descriptor),
    .vendor_code   = MSOS20_VENDOR_CODE,
};
#endif /* !LPKG_CHERRYUSB_DEVICE_COMPOSITE */

/*
 * Host MP4 Bulk OUT uses 20-byte UB header + up to 2048-byte payload (=2068).
 *
 * 進化史 (調 buffer 大小直接決定 area4 frame rate)：
 *   4K → 8K → 16K → 32K → 64K → 128K
 *
 * 為什麼要繼續加大：
 *   2026-05-29 實測 1280x800 JPEG (q≈80, ~120 KB) 用 64K buffer：
 *     - 一張需要 2 個 USB OUT chunk (65536 + ~54KB)
 *     - chunk 之間 device 端要：custom_image_recv (memcpy + state) → bus_unlock →
 *       USB driver re-arm 下一次 read → 重新等待 PC 送下一個 chunk
 *     - 估計每張 ~5-10 ms 浪費在這個「中間 stall」
 *
 *   把 buffer 推到 128 KB 後，120 KB JPEG **一次 callback** 就能塞進來，整張
 *   只剩一個 short packet → finish_recv。USB driver 重新 arm 也只發生一次，
 *   預期 interval 從 ~50 ms (peak burst) 再壓 5-10 ms。
 *
 * 硬體上限：
 *   AIC UDC DXEPTSIZ_XFER_SIZE_MASK = 0x7FFFF → 單次 transfer 最大 ~512 KB；
 *   DXEPTSIZ_PKT_CNT_MASK = 0x3FF → 最多 1023 packets。
 *   128 KB / 512 MPS = 256 packets，在 1023 packet 上限以內，安全。
 *
 * 記憶體成本：
 *   read_buffer 是 USB_NOCACHE_RAM_SECTION (內部 SRAM 或 DMA 安全區)。
 *   64 KB → 128 KB 多 +64 KB；對 D215 平台一般夠用，編譯期會檢查 linker
 *   section overflow。若連結失敗，請降回 64 KB 或檢查 link script。
 *
 * IMPORTANT: keep CUSTOM_USB_CHUNK_FULL in custom_Image.c in sync; short-packet
 * (len < CUSTOM_USB_CHUNK_FULL) marks PNG/APNG/GIF end-of-transfer.
 */
#define VENDER_OUT_READ_BUF_LEN 131072
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t read_buffer[VENDER_OUT_READ_BUF_LEN];
static USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t write_buffer[2048];

volatile bool ep_tx_busy_flag = false;
static volatile uint8_t s_vender_ack_pending;
/* Was 8 — silently dropped 9B screen_main FA FB FC … notify */
static uint8_t s_vender_ack_buf[64];
static uint32_t s_vender_ack_len;

extern struct usbd_endpoint vender_out_ep;

void usbd_vender_out_rearm(void)
{
    usbd_ep_start_read(0, vender_out_ep.ep_addr, read_buffer, VENDER_OUT_READ_BUF_LEN);
}

#ifdef CONFIG_USB_HS
#define MAX_MPS 512
#else
#define MAX_MPS 64
#endif

void usbd_vender_out(uint8_t busid, uint8_t ep, uint32_t nbytes);
void usbd_vender_in(uint8_t busid, uint8_t ep, uint32_t nbytes);
/*!< endpoint call back */
struct usbd_endpoint vender_out_ep = {
    .ep_addr = OUT_EP,
    .ep_cb = usbd_vender_out
};

struct usbd_endpoint vender_in_ep = {
    .ep_addr = IN_EP,
    .ep_cb = usbd_vender_in
};

#ifdef LPKG_CHERRYUSB_DEVICE_COMPOSITE
void usbd_comp_vender_event_handler(uint8_t busid, uint8_t event)
#else
void usbd_event_handler(uint8_t busid, uint8_t event)
#endif
{
    (void)busid;
    switch (event) {
        case USBD_EVENT_RESET:
            break;
        case USBD_EVENT_CONNECTED:
            break;
        case USBD_EVENT_DISCONNECTED:
            break;
        case USBD_EVENT_RESUME:
            break;
        case USBD_EVENT_SUSPEND:
            break;
        case USBD_EVENT_CONFIGURED:
            /* Host (re)configured: drop stale IN so notify can re-arm. */
            ep_tx_busy_flag = false;
            s_vender_ack_pending = 0U;
            s_vender_ack_len = 0U;
            /* setup first out ep read transfer */
            usbd_ep_start_read(0, vender_out_ep.ep_addr , read_buffer, VENDER_OUT_READ_BUF_LEN);
            break;
        case USBD_EVENT_SET_REMOTE_WAKEUP:
            break;
        case USBD_EVENT_CLR_REMOTE_WAKEUP:
            break;

        default:
            break;
    }
}

/*
 * VENDER_OUT_LOG_VERBOSE
 *
 *   0 (default): 完全關掉 per-chunk「vender out data」log。
 *                這條 print 跑在 USB OUT bulk callback 的 hot path，
 *                每個 JPEG/PNG 影格至少 3 行 rt_kprintf，會把實測 USB
 *                throughput 從 HS 應有的 >20 MB/s 砍到 ~2 MB/s，
 *                直接決定 area4 frame rate 上限。所以正常 runtime 一定關。
 *
 *   1         : 還原原本行為，per-chunk 印出 ep + nbytes。只在排查
 *                「USB 沒收到資料 / chunk size 異常」時手動開來看。
 *
 * 過去的 2048/20/2068 過濾是為了避開 USB video bulk 洗版；現在改用整段
 * 開關後不再需要那個 size-based filter (verbose 模式裡反而希望全收)。
 */
#ifndef VENDER_OUT_LOG_VERBOSE
#define VENDER_OUT_LOG_VERBOSE 0
#endif

void usbd_vender_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
#if VENDER_OUT_LOG_VERBOSE
    USB_LOG_INFO("vender out data %#x %d\n", ep, nbytes);
#else
    (void)ep;
#endif
    if (custom_recv_18bytes && nbytes > 0) {
        custom_recv_18bytes(read_buffer, nbytes);
    }

    /* Arm the next OUT. Hold only when both RAM buffers are full: one is
     * still erasing and the other already has the next finished GIF. The
     * writer rearms once it takes that GIF. */
    if (!custom_image_usb_rearm_hold())
        usbd_ep_start_read(0, vender_out_ep.ep_addr, read_buffer, VENDER_OUT_READ_BUF_LEN);
    custom_image_spill_boost();
}

static void usbd_vender_try_send_pending_ack(void)
{
    int ret;

    if (s_vender_ack_pending == 0U || ep_tx_busy_flag)
    {
        return;
    }
    if (s_vender_ack_len == 0U || s_vender_ack_len > sizeof(write_buffer))
    {
        s_vender_ack_pending = 0U;
        return;
    }
    memcpy(write_buffer, s_vender_ack_buf, s_vender_ack_len);
    ret = usbd_ep_start_write(0, vender_in_ep.ep_addr, write_buffer, s_vender_ack_len);
    if (ret == 0)
    {
        s_vender_ack_pending = 0U;
        ep_tx_busy_flag = true;
        rt_kprintf("vender IN ep=0x%02x write len=%u (pending flush)\n",
                   vender_in_ep.ep_addr, (unsigned)s_vender_ack_len);
    }
    else
    {
        rt_kprintf("vender IN ep=0x%02x flush fail ret=%d len=%u\n",
                   vender_in_ep.ep_addr, ret, (unsigned)s_vender_ack_len);
    }
}

void usbd_vender_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    (void)busid;
    /* Completes only after host ReadPipe consumes the packet. */
    rt_kprintf("vender IN complete ep=0x%02x nbytes=%u\n", ep, (unsigned)nbytes);
    (void)ep;
    (void)nbytes;
    ep_tx_busy_flag = false;
    usbd_vender_try_send_pending_ack();
}

/*
 * Bulk IN notify/ACK. Buffer must be DMA-safe (write_buffer is USB_NOCACHE).
 * Returns: 0=armed, 1=queued (waiting host IN), <0=dropped/error.
 *
 * Host MUST keep a ReadPipe pending on vender Bulk IN, or Bus Hound
 * shows nothing (no host IN tokens).
 */
int usbd_vender_send_ack_bytes(const uint8_t *p, uint32_t len)
{
    int ret;

    if (!p || len == 0U || len > sizeof(write_buffer) || len > sizeof(s_vender_ack_buf))
    {
        rt_kprintf("vender IN drop len=%u max=%u\n",
                   (unsigned)len, (unsigned)sizeof(s_vender_ack_buf));
        return -1;
    }
    if (ep_tx_busy_flag)
    {
        /*
         * 新 CD AA 的 0x83 必須覆蓋尚未送出的 0x81，否則 PC 在等 0x83 時會先收到上一輪 STOP。
         */
        if (len >= 3U && p[2] == 0x83U)
        {
            usbd_vender_clear_pending_ack();
        }
        memcpy(s_vender_ack_buf, p, len);
        s_vender_ack_len = len;
        s_vender_ack_pending = 1U;
        rt_kprintf("vender IN ep=0x%02x queued len=%u (busy, wait host read)\n",
                   vender_in_ep.ep_addr, (unsigned)len);
        return 1;
    }
    memcpy(write_buffer, p, len);
    ret = usbd_ep_start_write(0, vender_in_ep.ep_addr, write_buffer, len);
    if (ret == 0)
    {
        ep_tx_busy_flag = true;
        rt_kprintf("vender IN ep=0x%02x armed len=%u (wait host ReadPipe)\n",
                   vender_in_ep.ep_addr, (unsigned)len);
        return 0;
    }
    memcpy(s_vender_ack_buf, p, len);
    s_vender_ack_len = len;
    s_vender_ack_pending = 1U;
    if (ret == -2)
        ep_tx_busy_flag = true;
    rt_kprintf("vender IN ep=0x%02x start_write fail ret=%d len=%u (queued)\n",
               vender_in_ep.ep_addr, ret, (unsigned)len);
    return ret;
}

void usbd_vender_kick_pending_ack(void)
{
    usbd_vender_try_send_pending_ack();
}

void usbd_vender_clear_pending_ack(void)
{
    s_vender_ack_pending = 0U;
    s_vender_ack_len = 0U;
}

uint8_t usbd_vender_get_in_ep(void)
{
    return vender_in_ep.ep_addr;
}

uint8_t usbd_vender_get_out_ep(void)
{
    return vender_out_ep.ep_addr;
}

static struct usbd_interface intf0;

#ifdef LPKG_CHERRYUSB_DEVICE_COMPOSITE
#include "composite_template.h"
int usbd_comp_vender_init(uint8_t *ep_table, void *data)
{
    (void)data;
    vender_out_ep.ep_addr = ep_table[0];
    vender_in_ep.ep_addr = ep_table[1];
    usbd_add_interface(0, usbd_vender_init_intf(0, &intf0));
    usbd_add_endpoint(0, &vender_out_ep);
    usbd_add_endpoint(0, &vender_in_ep);
    /*
     * BOS / MS OS 2.0 are owned by composite_template.c in composite mode.
     * It registers them right after usbd_desc_register() (which wipes
     * g_usbd_core via memset) but before usbd_initialize(), so WinUSB
     * still auto-binds to this vender interface — see make_comp_winusb_*()
     * and usbd_composite_dev_load() in composite_template.c.
     */
    return 0;
}
#endif

int vender_device_init(void)
{
    const uint8_t data[10] = { 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30 };

    memcpy(&write_buffer[0], data, 10);
    memset(&write_buffer[10], 'a', 2038);

#ifndef LPKG_CHERRYUSB_DEVICE_COMPOSITE
    usbd_desc_register(0, vender_descriptor);
    /*
     * Advertise the MS OS 2.0 platform capability via BOS, then publish the
     * MS OS 2.0 descriptor set (162 bytes, single-function form). Together
     * they cause Windows 8.1+ to load the inbox WinUSB driver automatically
     * (no .inf required).
     *
     * Ordering: these MUST come AFTER usbd_desc_register() above, because
     * usbd_desc_register() memset(0)s g_usbd_core (wiping any
     * previously-registered BOS / MSOS 2.0 pointers).
     */
    usbd_bos_desc_register(0, &vender_bos_desc);
    usbd_msosv2_desc_register(0, &vender_msosv2_desc);
    usbd_add_interface(0, usbd_vender_init_intf(0, &intf0));
    usbd_add_endpoint(0, &vender_out_ep);
    usbd_add_endpoint(0, &vender_in_ep);
    usbd_initialize(0, USB_DEV_BASE, usbd_event_handler);
#else
    /*
     * Composite mode just hands off the descriptor + callbacks. The
     * composite framework owns BOS / MSOS 2.0 (it must, because Windows
     * needs a single Function Subset that targets the vender interface
     * only — see composite_template.c).
     */
    usbd_comp_func_register(vender_descriptor,
                            usbd_comp_vender_event_handler,
                            usbd_comp_vender_init, "vender");
#endif
    return 0;
}
USB_INIT_APP_EXPORT(vender_device_init);

static int cmd_test_vender(int argc, char **argv)
{
    static const uint8_t pkt[9] = {
        0xFAU, 0xFBU, 0xFCU, 0x00U, 0x00U, 0x00U, 0xECU, 0xEBU, 0xEAU
    };
    int ret;

    (void)argc;
    (void)argv;
    rt_kprintf("test_vender_usb: OUT=0x%02x IN=0x%02x busy=%u pending=%u\n",
               vender_out_ep.ep_addr, vender_in_ep.ep_addr,
               (unsigned)ep_tx_busy_flag, (unsigned)s_vender_ack_pending);
    ret = usbd_vender_send_ack_bytes(pkt, sizeof(pkt));
    rt_kprintf("test_vender_usb: send ret=%d (0=armed 1=queued)\n", ret);
    return 0;
}

MSH_CMD_EXPORT_ALIAS(cmd_test_vender, test_vender_usb, Send 9B LINE notify on vender IN);
