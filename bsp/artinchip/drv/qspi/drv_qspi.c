/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <aic_core.h>
#include <aic_drv.h>
#include <aic_hal.h>
#include <hal_qspi.h>
#include <hal_dma.h>
#include <aic_dma_id.h>
#include <aic_clk_id.h>
#include <aic_time.h>
#include <aic_osal.h>

#define ASYNC_DATA_SIZE 64

RT_WEAK void aic_mmosie_on_qspi_cs_low(u32 qspi_idx, u32 xfer_len)
{
    (void)qspi_idx;
    (void)xfer_len;
}

RT_WEAK void aic_mmosie_before_qspi_cs_high(u32 qspi_idx)
{
    (void)qspi_idx;
}

RT_WEAK void aic_mmosie_on_qspi_cs_high(u32 qspi_idx)
{
    (void)qspi_idx;
}

RT_WEAK void aic_mmosie_set_frame(const u8 frame[4])
{
    (void)frame;
}

RT_WEAK void aic_mmosie_preamble(void)
{
}

struct aic_qspi {
    struct rt_spi_bus dev;
    char *name;
    uint32_t idx;
    uint32_t clk_id;
    uint32_t clk_in_hz;
    uint32_t dma_port_id;
    uint32_t irq_num;
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
    uint32_t cs_num;
#endif
    qspi_master_handle handle;
    struct rt_qspi_configuration configuration;
    rt_sem_t xfer_sem;
    bool inited;
    uint32_t rxd_dlymode;
    uint32_t txd_dlymode;
    uint32_t txc_dlymode;
    bool nonblock;
    struct rt_completion *completion;
};

static struct aic_qspi qspi_controller[] = {
#if defined(AIC_USING_QSPI0)
    {
        .name = "qspi0",
        .idx = 0,
        .clk_id = CLK_QSPI0,
        .clk_in_hz = AIC_DEV_QSPI0_MAX_SRC_FREQ_HZ,
        .dma_port_id = DMA_ID_SPI0,
        .irq_num = QSPI0_IRQn,
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        .cs_num = AIC_QSPI0_CS_NUM,
#endif
        .rxd_dlymode = AIC_DEV_QSPI0_DELAY_MODE,
#if defined(AIC_QSPI_DRV_V20)
        .txd_dlymode = AIC_DEV_QSPI0_TXD_DELAY_MODE,
        .txc_dlymode = AIC_DEV_QSPI0_TX_CLK_DELAY_MODE,
#endif
    },
#endif
#if defined(AIC_USING_QSPI1) && !defined(AIC_QSPI1_BUS_SPI)
    {
        .name = "qspi1",
        .idx = 1,
        .clk_id = CLK_QSPI1,
        .clk_in_hz = AIC_DEV_QSPI1_MAX_SRC_FREQ_HZ,
        .dma_port_id = DMA_ID_SPI1,
        .irq_num = QSPI1_IRQn,
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        .cs_num = AIC_QSPI1_CS_NUM,
#endif
        .rxd_dlymode = AIC_DEV_QSPI1_DELAY_MODE,
#if defined(AIC_QSPI_DRV_V20)
        .txd_dlymode = AIC_DEV_QSPI1_TXD_DELAY_MODE,
        .txc_dlymode = AIC_DEV_QSPI1_TX_CLK_DELAY_MODE,
#endif
    },
#endif
#if defined(AIC_USING_QSPI2) && !defined(AIC_QSPI2_BUS_SPI)
    {
        .name = "qspi2",
        .idx = 2,
        .clk_id = CLK_QSPI2,
        .clk_in_hz = AIC_DEV_QSPI2_MAX_SRC_FREQ_HZ,
        .dma_port_id = DMA_ID_SPI2,
        .irq_num = QSPI2_IRQn,
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        .cs_num = AIC_QSPI2_CS_NUM,
#endif
        .rxd_dlymode = AIC_DEV_QSPI2_DELAY_MODE,
#if defined(AIC_QSPI_DRV_V20)
        .txd_dlymode = AIC_DEV_QSPI2_TXD_DELAY_MODE,
        .txc_dlymode = AIC_DEV_QSPI2_TX_CLK_DELAY_MODE,
#endif
    },
#endif
#if defined(AIC_USING_QSPI3) && !defined(AIC_QSPI3_BUS_SPI)
    {
        .name = "qspi3",
        .idx = 3,
        .clk_id = CLK_QSPI3,
        .clk_in_hz = AIC_DEV_QSPI3_MAX_SRC_FREQ_HZ,
        .dma_port_id = DMA_ID_SPI3,
        .irq_num = QSPI3_IRQn,
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        .cs_num = AIC_QSPI3_CS_NUM,
#endif
        .rxd_dlymode = AIC_DEV_QSPI3_DELAY_MODE,
#if defined(AIC_QSPI_DRV_V20)
        .txd_dlymode = AIC_DEV_QSPI3_TXD_DELAY_MODE,
        .txc_dlymode = AIC_DEV_QSPI3_TX_CLK_DELAY_MODE,
#endif
    },
#endif
#if defined(AIC_USING_QSPI4) && !defined(AIC_QSPI4_BUS_SPI)
    {
        .name = "qspi4",
        .idx = 4,
        .clk_id = CLK_QSPI4,
        .clk_in_hz = AIC_DEV_QSPI4_MAX_SRC_FREQ_HZ,
        .dma_port_id = DMA_ID_SPI4,
        .irq_num = QSPI4_IRQn,
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        .cs_num = AIC_QSPI4_CS_NUM,
#endif
        .rxd_dlymode = AIC_DEV_QSPI4_DELAY_MODE,
#if defined(AIC_QSPI_DRV_V20)
        .txd_dlymode = AIC_DEV_QSPI4_TXD_DELAY_MODE,
        .txc_dlymode = AIC_DEV_QSPI4_TX_CLK_DELAY_MODE,
#endif
    },
#endif
};

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
/* Short register writes finish in a few microseconds. Hold LCD CS. */
#define QSPI_LCD_CS_MIN_US       15U//600U
#define QSPI_LCD_CS_HIGH_MIN_US  15U//600U
#define QSPI_LCD_IDX             1U
static u64 s_lcd_cs_low_us;
static u64 s_lcd_cs_high_us;

static void qspi_lcd_cs_mark_low(struct aic_qspi *qspi)
{
    if (qspi->idx == QSPI_LCD_IDX)
        s_lcd_cs_low_us = aic_get_time_us();
}

static void qspi_lcd_cs_hold_min(struct aic_qspi *qspi)
{
    u64 elapsed;

    if (qspi->idx != QSPI_LCD_IDX || !s_lcd_cs_low_us)
        return;

    elapsed = aic_get_time_us() - s_lcd_cs_low_us;
    s_lcd_cs_low_us = 0;
    if (elapsed < QSPI_LCD_CS_MIN_US)
        aic_udelay((u32)(QSPI_LCD_CS_MIN_US - elapsed));
}

static void qspi_lcd_cs_mark_high(struct aic_qspi *qspi)
{
    if (qspi->idx == QSPI_LCD_IDX)
        s_lcd_cs_high_us = aic_get_time_us();
}

/* Idle high before the next 0000 / falling edge. */
static void qspi_lcd_cs_gap_min(struct aic_qspi *qspi)
{
    u64 elapsed;

    if (qspi->idx != QSPI_LCD_IDX || !s_lcd_cs_high_us)
        return;

    elapsed = aic_get_time_us() - s_lcd_cs_high_us;
    if (elapsed < QSPI_LCD_CS_HIGH_MIN_US)
        aic_udelay((u32)(QSPI_LCD_CS_HIGH_MIN_US - elapsed));
}
#else
static void qspi_lcd_cs_mark_low(struct aic_qspi *qspi)
{
    (void)qspi;
}

static void qspi_lcd_cs_hold_min(struct aic_qspi *qspi)
{
    (void)qspi;
}

static void qspi_lcd_cs_mark_high(struct aic_qspi *qspi)
{
    (void)qspi;
}

static void qspi_lcd_cs_gap_min(struct aic_qspi *qspi)
{
    (void)qspi;
}
#endif

#ifdef AIC_QSPI_DRV_DEBUG
void dump_cmd(uint8_t *data, uint32_t len)
{
    uint32_t i;

    printf("CMD: (%lu) ", (unsigned long)len);
    for (i = 0; i < len; i++)
        printf("%02X ", data[i]);
    printf("\n");
}
#endif

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
/*
 * J14: panel D0=PE.6, D1=PE.7, D2=PE.10, D3=PE.9.
 * QSPI1 still drives IO0 on PE.7 (MOSI) and IO1 on PE.6 (MISO).
 * 1-wire bits must leave on PE.6. Quad bytes swap IO0 with IO1.
 */
static void lcd_pe_gpio_out(const char *name, int high)
{
    long pin = hal_gpio_name2pin(name);
    unsigned int g, p;

    if (pin < 0)
        return;
    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);
    hal_gpio_set_func(g, p, 1);
    if (high)
        hal_gpio_set_output(g, p);
    else
        hal_gpio_clr_output(g, p);
    hal_gpio_direction_output(g, p);
    if (high)
        hal_gpio_set_output(g, p);
    else
        hal_gpio_clr_output(g, p);
}

static void lcd_pe_spi_func(const char *name)
{
    long pin = hal_gpio_name2pin(name);
    unsigned int g, p;

    if (pin < 0)
        return;
    g = GPIO_GROUP(pin);
    p = GPIO_GROUP_PIN(pin);
    hal_gpio_set_func(g, p, 6);
}

/*
 * Mode 0, MSB first, data on PE.6. Register writes, no per-bit pin lookup.
 * ~0.2 us/edge so a command header stays in the microsecond range.
 */
extern volatile void *gen_reg(u32 group, u32 offset);

static volatile void *s_lcd_set;
static volatile void *s_lcd_clr;
static u32 s_lcd_d0;
static u32 s_lcd_clk;
static int s_lcd_bb_ready;

static void lcd_bb_init(void)
{
    long d0 = hal_gpio_name2pin("PE.6");
    long clk = hal_gpio_name2pin("PE.8");
    unsigned int g;

    if (d0 < 0 || clk < 0)
        return;
    g = GPIO_GROUP(d0);
    s_lcd_set = gen_reg(g, 0x14);
    s_lcd_clr = gen_reg(g, 0x10);
    s_lcd_d0 = 1U << GPIO_GROUP_PIN(d0);
    s_lcd_clk = 1U << GPIO_GROUP_PIN(clk);
    s_lcd_bb_ready = 1;
    pr_info("qspi1 LCD D0=PE.6 D1=PE.7 D2=PE.10 D3=PE.9\n");
}

static void lcd_bb_edge(void)
{
    u32 c0, c;

    __asm__ volatile("csrr %0, mcycle" : "=r"(c0));
    do {
        __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    } while ((u32)(c - c0) < 80U);
}

static void lcd_bitbang_d0(const uint8_t *buf, uint32_t len)
{
    uint32_t i;
    int b;

    if (!s_lcd_bb_ready)
        lcd_bb_init();
    if (!s_lcd_bb_ready)
        return;

    lcd_pe_gpio_out("PE.6", 0);
    lcd_pe_gpio_out("PE.8", 0);
    for (i = 0; i < len; i++) {
        uint8_t v = buf[i];

        for (b = 0; b < 8; b++) {
            if (v & 0x80)
                writel(s_lcd_d0, s_lcd_set);
            else
                writel(s_lcd_d0, s_lcd_clr);
            lcd_bb_edge();
            writel(s_lcd_clk, s_lcd_set);
            lcd_bb_edge();
            writel(s_lcd_clk, s_lcd_clr);
            v <<= 1;
        }
    }
    lcd_pe_spi_func("PE.6");
    lcd_pe_spi_func("PE.8");
}
#endif

static uint32_t address_copy(uint32_t addr, uint32_t size, uint8_t *dst)
{
    uint32_t i;

    i = 0;
    while (size) {
        dst[i++] = (addr >> (8 * (size - 1))) & 0xFF;
        size--;
    }
    return i;
}

static rt_err_t qspi_send_command(struct aic_qspi *qspi,
                                  struct rt_qspi_message *qspi_message)
{
    uint32_t single_cnt, rest_cnt, cmd_rest_bw, dmycnt, dmybw;
    uint8_t cmdbuf1[16], cmdbuf2[16];
    struct qspi_transfer t;
    rt_err_t ret = RT_EOK;

    single_cnt = 0;
    rest_cnt = 0;
    cmd_rest_bw = HAL_QSPI_BUS_WIDTH_SINGLE;
    dmybw = HAL_QSPI_BUS_WIDTH_SINGLE;
    if (qspi_message->instruction.qspi_lines == 1) {
        cmdbuf1[single_cnt++] = qspi_message->instruction.content;
    } else if (qspi_message->instruction.qspi_lines > 1) {
        cmdbuf2[rest_cnt++] = qspi_message->instruction.content;
        cmd_rest_bw = qspi_message->instruction.qspi_lines;
        dmybw = qspi_message->instruction.qspi_lines;
    }

    if ((qspi_message->address.size != 0) &&
        (qspi_message->address.qspi_lines == 1)) {
        single_cnt +=
            address_copy(qspi_message->address.content,
                         qspi_message->address.size, &cmdbuf1[single_cnt]);
        dmybw = qspi_message->address.qspi_lines;
    } else if ((qspi_message->address.size != 0) &&
               (qspi_message->address.qspi_lines > 1)) {
        rest_cnt +=
            address_copy(qspi_message->address.content,
                         qspi_message->address.size, &cmdbuf2[rest_cnt]);
        cmd_rest_bw = qspi_message->address.qspi_lines;
        dmybw = qspi_message->address.qspi_lines;
    }

    dmycnt = (dmybw * qspi_message->dummy_cycles) / 8;
    if ((dmycnt != 0) && (dmybw == 1)) {
        memset(&cmdbuf1[single_cnt], 0, dmycnt);
        single_cnt += dmycnt;
    } else if (dmycnt != 0) {
        memset(&cmdbuf2[rest_cnt], 0, dmycnt);
        rest_cnt += dmycnt;
    }

#ifdef AIC_QSPI_DRV_DEBUG
    if (single_cnt)
        dump_cmd(cmdbuf1, single_cnt);
    if (rest_cnt)
        dump_cmd(cmdbuf2, rest_cnt);
#endif
    /* Command + Address + Dummy stage */
    if (single_cnt > 0) {
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
        if (qspi->idx == 1) {
            lcd_bitbang_d0(cmdbuf1, single_cnt);
        } else
#endif
        {
        hal_qspi_master_set_bus_width(&qspi->handle, HAL_QSPI_BUS_WIDTH_SINGLE);
        t.rx_data = NULL;
        t.tx_data = cmdbuf1;
        t.data_len = single_cnt;
        /* Data is short, use sync API to send */
        ret = hal_qspi_master_transfer_sync(&qspi->handle, &t);
        if (ret) {
            pr_err("Send command failed.\n");
            return -1;
        }
        }
    }
    if (rest_cnt > 0) {
        hal_qspi_master_set_bus_width(&qspi->handle, cmd_rest_bw);
        t.rx_data = NULL;
        t.tx_data = cmdbuf2;
        t.data_len = rest_cnt;
        /* Data is short, use sync API to send */
        ret = hal_qspi_master_transfer_sync(&qspi->handle, &t);
        if (ret) {
            pr_err("Send command failed.\n");
            return -1;
        }
    }

    return 0;
}

static rt_uint32_t drv_qspi_send_async(struct aic_qspi *qspi, struct qspi_transfer *t)
{
    rt_completion_init(qspi->completion);

    return hal_qspi_master_transfer_async(&qspi->handle, t);
}

static rt_uint32_t drv_qspi_send(struct aic_qspi *qspi,
                                 struct rt_spi_message *message,
                                 const uint8_t *tx, uint32_t size)
{
    struct rt_qspi_message *qspi_message;
    struct qspi_transfer t;
    rt_err_t ret = RT_EOK;

    RT_ASSERT(qspi != RT_NULL);
    RT_ASSERT(message != RT_NULL);

    qspi_master_handle *h;
    struct qspi_bm_transfer bm_t = { 0 };
    h = &qspi->handle;
    if (h->bit_mode) {
        bm_t.tx_data = (uint8_t *)tx;
        bm_t.tx_bits_len = size;
        ret = hal_qspi_master_transfer_bit_mode(h, &bm_t);
        return ret;
    }

    qspi_message = (struct rt_qspi_message *)message;

    /* Command + Address + Dummy stage */
    ret = qspi_send_command(qspi, qspi_message);
    if (ret)
        goto err;

    /* Data stage */
    if (size > 0) {
#ifdef AIC_QSPI_DRV_DEBUG
        printf("tx data bus width: %u, len %lu\n",
               qspi_message->qspi_data_lines, (unsigned long)size);
#endif
        hal_qspi_master_set_bus_width(&qspi->handle,
                                      qspi_message->qspi_data_lines);
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
        /* Quad pixels are already D0/D1 swapped when the RGB666 buffer is filled. */
        if (qspi->idx == 1 && qspi_message->qspi_data_lines <= 1) {
            lcd_bitbang_d0(tx, size);
            return size;
        }
#endif
        t.rx_data = NULL;
        t.tx_data = (uint8_t *)tx;
        t.data_len = size;
        if (qspi->nonblock)
            ret = drv_qspi_send_async(qspi, &t);
        else
            ret = hal_qspi_master_transfer_sync(&qspi->handle, &t);
        if (ret) {
            pr_err("Send data failed.\n");
            goto err;
        }
    }

    return size;
err:
    return 0;
}
static rt_uint32_t drv_qspi_receive(struct aic_qspi *qspi,
                                    struct rt_spi_message *message, uint8_t *rx,
                                    uint32_t size)
{
    struct rt_qspi_message *qspi_message;
    struct qspi_transfer t;
    rt_err_t ret = RT_EOK;

    RT_ASSERT(qspi != RT_NULL);
    RT_ASSERT(message != RT_NULL);
    RT_ASSERT(rx != RT_NULL);
    RT_ASSERT(size != 0);

    qspi_master_handle *h;
    struct qspi_bm_transfer bm_t = { 0 };
    h = &qspi->handle;
    if (h->bit_mode) {
        bm_t.rx_data = rx;
        bm_t.rx_bits_len = size;
        ret = hal_qspi_master_transfer_bit_mode(h, &bm_t);
        return ret;
    }

    qspi_message = (struct rt_qspi_message *)message;

    /* Command + Address + Dummy stage */
    ret = qspi_send_command(qspi, qspi_message);
    if (ret)
        goto err;

    /* Data stage */
    if (size > 0) {
#ifdef AIC_QSPI_DRV_DEBUG
        printf("rx data bus width: %u, len %lu\n",
               qspi_message->qspi_data_lines, (unsigned long)size);
#endif
        hal_qspi_master_set_bus_width(&qspi->handle,
                                      qspi_message->qspi_data_lines);
        t.tx_data = NULL;
        t.rx_data = (uint8_t *)rx;
        t.data_len = size;
        ret = hal_qspi_master_transfer_sync(&qspi->handle, &t);
        if (ret) {
            pr_err("Recv data failed.\n");
            goto err;
        }
    }

    return size;
err:
    return 0;
}

static rt_uint32_t qspi_xfer(struct rt_spi_device *device,
                             struct rt_spi_message *message)
{
    const rt_uint8_t *sndb = message->send_buf;
    rt_uint8_t *rcvb = message->recv_buf;
    struct rt_qspi_configuration *cfg;
    struct aic_qspi *qspi;
    struct rt_qspi_message *qspi_msg;
    rt_uint32_t ret = 0;
    u32 cs_num = 0;

    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(device->bus != RT_NULL);

    qspi = (struct aic_qspi *)device->bus;
    cfg = &qspi->configuration;
    qspi_msg = (struct rt_qspi_message *)message;

    if (message->cs_take && !(cfg->parent.mode & RT_SPI_NO_CS)) {
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        cs_num = qspi->cs_num;
#endif
        qspi_lcd_cs_gap_min(qspi);
        /* 4 bits finish before PE.5 moves, so each TFT CS is already selected. */
        aic_mmosie_on_qspi_cs_low(qspi->idx, message->length);
        hal_qspi_master_set_cs(&qspi->handle, cs_num, true);
        qspi_lcd_cs_mark_low(qspi);
    }

    if (rcvb)
        ret = drv_qspi_receive(qspi, message, rcvb, message->length);
    else if (sndb || qspi_msg->instruction.qspi_lines)
        ret = drv_qspi_send(qspi, message, sndb, message->length);

    if (message->cs_release && !(cfg->parent.mode & RT_SPI_NO_CS)) {
#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        cs_num = qspi->cs_num;
#endif
        if (!qspi->nonblock) {
            qspi_lcd_cs_hold_min(qspi);
            aic_mmosie_before_qspi_cs_high(qspi->idx);
            hal_qspi_master_set_cs(&qspi->handle, cs_num, false);
            qspi_lcd_cs_mark_high(qspi);
            aic_mmosie_on_qspi_cs_high(qspi->idx);
        }
    }

    return ret;
}

static void qspi_master_async_callback(qspi_master_handle *h, void *priv)
{
    struct aic_qspi *qspi = priv;
    u32 cs_num = 0;

#if defined(AIC_QSPI_MULTIPLE_CS_NUM)
        cs_num = qspi->cs_num;
#endif
    rt_sem_release(qspi->xfer_sem);
    qspi_lcd_cs_hold_min(qspi);
    aic_mmosie_before_qspi_cs_high(qspi->idx);
    hal_qspi_master_set_cs(&qspi->handle, cs_num, false);
    qspi_lcd_cs_mark_high(qspi);
    aic_mmosie_on_qspi_cs_high(qspi->idx);
    rt_completion_done(qspi->completion);
}

static irqreturn_t qspi_irq_handler(int irq_num, void *arg)
{
    qspi_master_handle *h = arg;

    rt_interrupt_enter();
    hal_qspi_master_irq_handler(h);
    rt_interrupt_leave();

    return IRQ_HANDLED;
}

static rt_err_t qspi_hw_init(struct aic_qspi *qspi)
{
    struct qspi_master_config cfg;
    rt_uint32_t bus_hz;
    rt_err_t ret = RT_EOK;

    rt_memset(&cfg, 0, sizeof(cfg));
    cfg.idx = qspi->idx;
    cfg.clk_id = qspi->clk_id;

    bus_hz = qspi->configuration.parent.max_hz;
    if (bus_hz < HAL_QSPI_MIN_FREQ_HZ)
        bus_hz = HAL_QSPI_MIN_FREQ_HZ;
    if (bus_hz > HAL_QSPI_MAX_FREQ_HZ)
        bus_hz = HAL_QSPI_MAX_FREQ_HZ;
    cfg.max_hz = bus_hz;
    cfg.clk_in_hz = qspi->clk_in_hz;
    if (qspi->clk_in_hz % bus_hz)
        cfg.clk_in_hz = bus_hz;
    if (cfg.clk_in_hz < HAL_QSPI_INPUT_MIN_FREQ_HZ)
        cfg.clk_in_hz = HAL_QSPI_INPUT_MIN_FREQ_HZ;

    if (qspi->configuration.parent.mode & RT_SPI_MSB)
        cfg.lsb_en = false;
    else
        cfg.lsb_en = true;
    if (qspi->configuration.parent.mode & RT_SPI_CPHA)
        cfg.cpha = HAL_QSPI_CPHA_SECOND_EDGE;
    else
        cfg.cpha = HAL_QSPI_CPHA_FIRST_EDGE;

    if (qspi->configuration.parent.mode & RT_SPI_CPOL)
        cfg.cpol = HAL_QSPI_CPOL_ACTIVE_LOW;
    else
        cfg.cpol = HAL_QSPI_CPOL_ACTIVE_HIGH;
    if (qspi->configuration.parent.mode & RT_SPI_CS_HIGH)
        cfg.cs_polarity = HAL_QSPI_CS_POL_VALID_HIGH;
    else
        cfg.cs_polarity = HAL_QSPI_CS_POL_VALID_LOW;
    cfg.rx_dlymode = qspi->rxd_dlymode;
    cfg.tx_dlymode = aic_convert_tx_dlymode(qspi->txc_dlymode, qspi->txd_dlymode);

    ret = hal_qspi_master_init(&qspi->handle, &cfg);
    if (ret) {
        pr_err("qspi init failed.\n");
        return ret;
    }

    if (!qspi->inited) {
#ifdef AIC_DMA_DRV
        /* Only request DMA channels on first init.
         * After deep sleep, old dma_rx/dma_tx are still valid. */
        if (qspi->handle.dma_rx == NULL || qspi->handle.dma_tx == NULL) {
            struct qspi_master_dma_config dmacfg;
            rt_memset(&dmacfg, 0, sizeof(dmacfg));
            dmacfg.port_id = qspi->dma_port_id;

            ret = hal_qspi_master_dma_config(&qspi->handle, &dmacfg);
            if (ret) {
                pr_err("qspi dma config failed.\n");
                return ret;
            }
        }
#endif
        ret = hal_qspi_master_register_cb(&qspi->handle, qspi_master_async_callback, qspi);
        if (ret) {
            pr_err("qspi register async callback failed.\n");
            return ret;
        }
        aicos_request_irq(qspi->irq_num, qspi_irq_handler, 0, NULL, (void *)&qspi->handle);
        aicos_irq_enable(qspi->irq_num);
    }

    bus_hz = qspi->configuration.parent.max_hz;
    if (bus_hz > HAL_QSPI_MAX_FREQ_HZ)
        bus_hz = HAL_QSPI_MAX_FREQ_HZ;
    hal_qspi_master_set_bus_freq(&qspi->handle, bus_hz);

    return ret;
}

void qspi_long_wait_hook(void)
{
    rt_device_t wdt = rt_device_find("wdt");

    /* Yield stays on this priority. The GIF thread is lower, so a long
     * TFT DMA has to sleep before that thread can decode the next frame.
     * Kick here: a stuck 388 KB wait used to outlast the watchdog and
     * reset the chip while Panel_signal was still being written. */
    if (wdt)
        rt_device_control(wdt, RT_DEVICE_CTRL_WDT_KEEPALIVE, RT_NULL);
    rt_thread_mdelay(1);
}

int aic_qspi_bus_recover(const char *bus_name)
{
    struct aic_qspi *qspi = NULL;
    int i;

    if (!bus_name)
        return -RT_EINVAL;

    for (i = 0; i < (int)ARRAY_SIZE(qspi_controller); i++) {
        if (qspi_controller[i].name &&
            rt_strcmp(qspi_controller[i].name, bus_name) == 0) {
            qspi = &qspi_controller[i];
            break;
        }
    }
    if (!qspi || !qspi->inited)
        return -RT_ERROR;

#ifdef AIC_DMA_DRV
    if (qspi->handle.dma_tx)
        hal_dma_chan_stop((struct aic_dma_chan *)qspi->handle.dma_tx);
    if (qspi->handle.dma_rx)
        hal_dma_chan_stop((struct aic_dma_chan *)qspi->handle.dma_rx);
#endif
    qspi->nonblock = false;
    aic_mmosie_before_qspi_cs_high(qspi->idx);
    hal_qspi_master_set_cs(&qspi->handle, 0, false);
    qspi_lcd_cs_mark_high(qspi);
    aic_mmosie_on_qspi_cs_high(qspi->idx);
    hal_qspi_master_fifo_reset(&qspi->handle, HAL_QSPI_TX_FIFO);
    hal_qspi_master_fifo_reset(&qspi->handle, HAL_QSPI_RX_FIFO);
    /* Re-init clock/mode after a wedged transfer (FIFO reset alone is not enough). */
    if (qspi_hw_init(qspi) != RT_EOK)
        return -RT_ERROR;
    pr_warn("%s: bus recover + reinit\n", bus_name);
    return RT_EOK;
}

static rt_err_t qspi_configure(struct rt_spi_device *device,
                               struct rt_spi_configuration *configuration)
{
    struct aic_qspi *qspi;
    rt_err_t ret = RT_EOK;

    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(configuration != RT_NULL);

    qspi = (struct aic_qspi *)device->bus;

#ifdef AIC_SUPPORT_SPI_IN_BIT_MODE
    if (configuration->mode & RT_SPI_3WIRE) {
        qspi->handle.bit_mode = true;
    } else {
        qspi->handle.bit_mode = false;
    }
#else
    qspi->handle.bit_mode = false;
#endif

    if ((rt_memcmp(&qspi->configuration, configuration,
                   sizeof(struct rt_spi_configuration)) != 0) ||
        (qspi->handle.bit_mode == true)) {
        rt_memcpy(&qspi->configuration, configuration,
                  sizeof(struct rt_spi_configuration));

        ret = qspi_hw_init(qspi);
        if (ret)
            return ret;
    }
    qspi->inited = true;
    return ret;
}

#ifdef AIC_PM_DRV_V15
static int qspi_suspend(const struct rt_device *device, rt_uint8_t mode)
{
    struct aic_qspi *qspi = rt_container_of(device, struct aic_qspi, dev.parent);
    rt_uint32_t status;

    switch (mode) {
    case PM_SLEEP_MODE_IDLE:
        break;
    case PM_SLEEP_MODE_LIGHT:
    case PM_SLEEP_MODE_STANDBY:
        /* Wait for any in-flight transfer to complete.
         * Async nonblock transfers use qspi->status. */
        status = hal_qspi_master_get_status(&qspi->handle);
        if (status & HAL_QSPI_STATUS_IN_PROGRESS) {
            pr_warn("qspi%d transfer in progress, block light sleep\n",
                    qspi->idx);
            return -EBUSY;
        }
        hal_clk_disable(qspi->clk_id);
        break;
    case PM_SLEEP_MODE_DEEP:
        /* Always disable clock regardless of init state.
         * Consumers have already stopped new transfers.
         * If a transfer is still in progress, it's a bug in the consumer.
         * Assert reset and disable clock directly. */
        hal_clk_disable_assertrst(qspi->clk_id);
        hal_clk_disable(qspi->clk_id);
        break;
    default:
        break;
    }
    return 0;
}

static void qspi_resume(const struct rt_device *device, rt_uint8_t mode)
{
    struct aic_qspi *qspi = rt_container_of(device, struct aic_qspi, dev.parent);
    rt_err_t ret;

    if (!qspi->inited) {
        /* If QSPI was not initialized before sleep,
         * no need to reinit hardware or enable clk. */
        return;
    }
    switch (mode) {
    case PM_SLEEP_MODE_IDLE:
        break;
    case PM_SLEEP_MODE_LIGHT:
    case PM_SLEEP_MODE_STANDBY:
        hal_clk_enable(qspi->clk_id);
        break;
    case PM_SLEEP_MODE_DEEP:
        /* DMA channel does not need to be re-requested.
         * hal_dma_init() keeps 'used' and clears 'desc'.
         * hal_qspi_master_init() enables clk and deasserts reset internally.
         * Just call qspi_hw_init() here. */
        ret = qspi_hw_init(qspi);
        if (ret)
            pr_err("qspi%d resume reinit failed: %d\n", qspi->idx, ret);
        break;
    default:
        break;
    }
}

static struct rt_device_pm_ops aic_qspi_pm_ops = {
    SET_DEVICE_PM_OPS(qspi_suspend, qspi_resume)
    NULL,
};

static int qspi_pm_init(void)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(qspi_controller); i++) {
        rt_pm_device_register(&qspi_controller[i].dev.parent,
                              &aic_qspi_pm_ops);
    }

    return 0;
}
INIT_DEVICE_EXPORT(qspi_pm_init);
#endif

static rt_err_t spi_nonblock_set(struct rt_spi_device *device,
                               rt_uint32_t nonblock)
{
    struct aic_qspi *qspi;

    qspi = (struct aic_qspi *)device->bus;
    if (nonblock)
        qspi->nonblock = true;
    else
        qspi->nonblock = false;

    if (nonblock) {
        if (!qspi->completion)
            qspi->completion = (struct rt_completion *)rt_malloc(sizeof(struct rt_completion));

        if (!qspi->completion)
            return -RT_ERROR;
    }

    return RT_EOK;
}

static rt_uint32_t spi_get_transfer_status(struct rt_spi_device *device)
{
    struct aic_qspi *qspi;

    qspi = (struct aic_qspi *)device->bus;

    return hal_qspi_master_get_status(&qspi->handle);
}

static void qspi_set_rxdelay_mode(struct rt_spi_device *device, rt_uint32_t mode)
{
    struct aic_qspi *qspi;

    qspi = (struct aic_qspi *)device->bus;

    hal_qspi_master_set_rxdelay_mode(&qspi->handle, mode);
}

#if defined(AIC_QSPI_DRV_V11) || defined(AIC_QSPI_DRV_V12)
static void spi_disp_control(struct rt_spi_device *device, struct spi_disp_config *spi_disp)
{
    struct aic_qspi *qspi;
    struct spi_display_init_config hal_disp_config;
    struct spi_display_param hal_disp_param;

    rt_memset(&hal_disp_config, 0, sizeof(hal_disp_config));
    rt_memset(&hal_disp_param, 0, sizeof(hal_disp_param));

    qspi = (struct aic_qspi *)device->bus;

    if (spi_disp == NULL || spi_disp->disp_config == NULL || spi_disp->disp_param == NULL) {
        pr_err("spi_disp_control: invalid parameters\n");
        return;
    }

    hal_disp_config.dma_en = spi_disp->disp_config->dma_en;
    hal_disp_config.spi_mode = spi_disp->disp_config->spi_mode;
    hal_disp_config.disp_on = spi_disp->disp_config->disp_on;

    hal_disp_param.mtw = spi_disp->disp_param->mtw;
    hal_disp_param.vsw_en = spi_disp->disp_param->vsw_en;
    hal_disp_param.vbp_en = spi_disp->disp_param->vbp_en;
    hal_disp_param.vfp_en = spi_disp->disp_param->vfp_en;

    hal_disp_param.len_stride = spi_disp->disp_param->len_stride;
    hal_disp_param.len = spi_disp->disp_param->len;

    hal_disp_param.hvbp = spi_disp->disp_param->hvbp;
    hal_disp_param.hvld = spi_disp->disp_param->hvld;
    hal_disp_param.hvfp = spi_disp->disp_param->hvfp;

    hal_disp_param.cmd_vsw = spi_disp->disp_param->cmd_vsw;
    hal_disp_param.cmd_vbp = spi_disp->disp_param->cmd_vbp;
    hal_disp_param.cmd_vld = spi_disp->disp_param->cmd_vld;
    hal_disp_param.cmd_vfp = spi_disp->disp_param->cmd_vfp;

    hal_disp_param.count = spi_disp->disp_param->count;

    hal_spi_disp_init(&qspi->handle, &hal_disp_config);
    hal_spi_disp_set_mode(&qspi->handle, &hal_disp_param);
}

static void spi_disp_trans_data(struct rt_spi_device *device, struct drv_spi_trans_data *spi_disp_trans)
{
    struct aic_qspi *qspi;
    struct spi_display_data hal_disp_data;

    rt_memset(&hal_disp_data, 0, sizeof(hal_disp_data));

    qspi = (struct aic_qspi *)device->bus;

    rt_completion_init(qspi->completion);

    hal_disp_data.tx_buf  = spi_disp_trans->tx_buf;
    hal_disp_data.en      = spi_disp_trans->en;
    hal_disp_data.frm_cnt = spi_disp_trans->frm_cnt;

    hal_spi_disp_restart(&qspi->handle, &hal_disp_data);
}
#endif

static rt_err_t qspi_wait_completion(struct rt_spi_device *device)
{
    rt_err_t result = RT_EOK;
    struct aic_qspi *qspi;

    qspi = (struct aic_qspi *)device->bus;

    /* Sync transfers never allocate completion; waiting would NULL-deref. */
    if (!qspi->nonblock || !qspi->completion)
        return RT_EOK;

    /* Full 360x360 RGB565 @1MHz needs ~2.1s; keep margin for slow clocks. */
    result = rt_completion_wait(qspi->completion, 10000);
    if (result != RT_EOK)
        pr_err("qspi wait transfer done timeout\n");

    return result;
}

rt_err_t aic_qspi_bus_attach_device(const char *bus_name,
                                    const char *device_name, rt_uint32_t pin,
                                    rt_uint8_t data_line_width,
                                    void (*enter_qspi_mode)(),
                                    void (*exit_qspi_mode)())
{
    struct rt_qspi_device *qspi_device = RT_NULL;
    rt_err_t result = RT_EOK;

    RT_ASSERT(bus_name != RT_NULL);
    RT_ASSERT(device_name != RT_NULL);
    RT_ASSERT(data_line_width == 1 || data_line_width == 2 ||
              data_line_width == 4);

    qspi_device =
        (struct rt_qspi_device *)rt_malloc(sizeof(struct rt_qspi_device));
    if (qspi_device == RT_NULL) {
        pr_err("Failed to malloc memory for qspi device.\n");
        result = RT_ENOMEM;
        goto __exit;
    }

    qspi_device->enter_qspi_mode = enter_qspi_mode;
    qspi_device->exit_qspi_mode = exit_qspi_mode;
    qspi_device->config.qspi_dl_width = data_line_width;

    result = rt_spi_bus_attach_device(&qspi_device->parent, device_name,
                                      bus_name, RT_NULL);

__exit:
    if (result != RT_EOK) {
        if (qspi_device)
            rt_free(qspi_device);
    }

    return result;
}

static const struct rt_spi_ops aic_qspi_ops = {
    .configure = qspi_configure,
    .xfer = qspi_xfer,
    .nonblock = spi_nonblock_set,
    .gstatus = spi_get_transfer_status,
    .delaymode = qspi_set_rxdelay_mode,
    .wait_completion = qspi_wait_completion,
#if defined(AIC_QSPI_DRV_V11) || defined(AIC_QSPI_DRV_V12)
    .spi_disp = spi_disp_control,
    .spi_trans_data = spi_disp_trans_data,
#endif
};

static int rt_hw_qspi_bus_init(void)
{
    char sem_name[RT_NAME_MAX];
    rt_err_t ret = RT_EOK;
    int i;

    for (i = 0; i < ARRAY_SIZE(qspi_controller); i++) {
        ret = rt_qspi_bus_register(&qspi_controller[i].dev,
                                   qspi_controller[i].name, &aic_qspi_ops);
        if (ret != RT_EOK)
            break;
        rt_sprintf(sem_name, "%s_s", qspi_controller[i].name);
        qspi_controller[i].xfer_sem =
            rt_sem_create(sem_name, 0, RT_IPC_FLAG_PRIO);
        if (!qspi_controller[i].xfer_sem) {
            ret = RT_ENOMEM;
            break;
        }

    }

    return ret;
}

INIT_BOARD_EXPORT(rt_hw_qspi_bus_init);
