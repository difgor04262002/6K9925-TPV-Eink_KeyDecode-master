/*
 * 15x WS2812B on J7. PD.8 is SPI3 MOSI (function 3).
 * Q4 is a non-inverting level shifter: gate biased to 3.3V, source is
 * PD.8 with R58 pull-up, drain is J7 pin2 with R59 pull-up to 5V.
 * PD.8 low makes DIN low. Idle GPIO low holds the WS2812 reset.
 *
 * rt_spi_configure() only programs the controller after this device owns
 * the bus. take_bus() does that. Otherwise the handle index stays 0 and
 * the bytes go out QSPI0.
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <finsh.h>
#include <stdlib.h>
#include <string.h>
#include <aic_hal_gpio.h>
#include "custom_argb.h"

#define ARGB_LED_COUNT    15
#define ARGB_RESET_BYTES  512
#define ARGB_FRAME_BYTES  (ARGB_RESET_BYTES + ARGB_LED_COUNT * 24 + ARGB_RESET_BYTES + 1)
#define ARGB_SPI_HZ       6250000
#define ARGB_PIN_GROUP    PD_GROUP
#define ARGB_PIN_NUM      8
#define ARGB_FUNC_GPIO    1
#define ARGB_FUNC_SPI     3
/*
 * Request 6250000 Hz. 200 MHz divides evenly, and the QSPI divider
 * (source/2, then >>4) yields 6187500 Hz, 161.6 ns per SPI bit.
 * DIN 0 = 0xC0: 323 ns high, 970 ns low.
 * DIN 1 = 0xF8: 808 ns high, 485 ns low.
 * The shifter is non-inverting, so MOSI matches DIN.
 */
#define ARGB_DIN_BIT0     0xC0
#define ARGB_DIN_BIT1     0xF8

extern rt_err_t aic_spi_bus_attach_device(const char *bus_name,
                                          const char *device_name);

static struct rt_spi_device *s_spi;
static rt_mutex_t s_lock;
static uint8_t s_led[ARGB_LED_COUNT][3];
static uint8_t s_frame[ARGB_FRAME_BYTES] __attribute__((aligned(8)));
static uint8_t s_level[256];
static uint8_t s_ready;
static uint8_t s_bit0;
static uint8_t s_bit1;
static uint8_t s_reset;

extern volatile void *cfg_reg(unsigned int group, unsigned int pin);
extern volatile void *gen_reg(unsigned int group, unsigned int offset);

static unsigned argb_out_bit(void)
{
    unsigned long val;

    val = *(volatile unsigned long *)gen_reg(ARGB_PIN_GROUP, 0x4);
    return (unsigned)((val >> ARGB_PIN_NUM) & 1u);
}

static unsigned argb_in_bit(void)
{
    unsigned long val;

    val = *(volatile unsigned long *)gen_reg(ARGB_PIN_GROUP, 0x0);
    return (unsigned)((val >> ARGB_PIN_NUM) & 1u);
}

static void argb_drive(int high)
{
    hal_gpio_set_func(ARGB_PIN_GROUP, ARGB_PIN_NUM, ARGB_FUNC_GPIO);
    hal_gpio_set_drive_strength(ARGB_PIN_GROUP, ARGB_PIN_NUM, 3);
    hal_gpio_set_bias_pull(ARGB_PIN_GROUP, ARGB_PIN_NUM, PIN_PULL_DIS);
    if (high)
        hal_gpio_set_output(ARGB_PIN_GROUP, ARGB_PIN_NUM);
    else
        hal_gpio_clr_output(ARGB_PIN_GROUP, ARGB_PIN_NUM);
    hal_gpio_direction_output(ARGB_PIN_GROUP, ARGB_PIN_NUM);
    if (high)
        hal_gpio_set_output(ARGB_PIN_GROUP, ARGB_PIN_NUM);
    else
        hal_gpio_clr_output(ARGB_PIN_GROUP, ARGB_PIN_NUM);
    hal_gpio_set_value(ARGB_PIN_GROUP, ARGB_PIN_NUM, high ? 1u : 0u);
}

static void argb_pin_gpio(void)
{
    argb_drive(0);
}

static void argb_pin_spi(void)
{
    /* Drop GPIO output-enable so SPI3 MOSI owns the pad. */
    hal_gpio_direction_input(ARGB_PIN_GROUP, ARGB_PIN_NUM);
    hal_gpio_set_func(ARGB_PIN_GROUP, ARGB_PIN_NUM, ARGB_FUNC_SPI);
}

static void argb_set_polar(int invert)
{
    if (invert) {
        s_bit0 = (uint8_t)~ARGB_DIN_BIT0;
        s_bit1 = (uint8_t)~ARGB_DIN_BIT1;
        s_reset = 0xFF;
    } else {
        s_bit0 = ARGB_DIN_BIT0;
        s_bit1 = ARGB_DIN_BIT1;
        s_reset = 0x00;
    }
}

static uint8_t *argb_put_byte(uint8_t *p, uint8_t value)
{
    unsigned i;

    for (i = 0; i < 8; i++) {
        *p++ = (value & 0x80) ? s_bit1 : s_bit0;
        value <<= 1;
    }
    return p;
}

static int argb_flush_locked(void)
{
    struct rt_spi_message msg;
    uint8_t *p;
    unsigned i;
    rt_size_t sent;
    rt_err_t err;

    if (!s_ready || s_spi == RT_NULL)
        return -RT_ERROR;

    memset(s_frame, s_reset, ARGB_RESET_BYTES);
    p = s_frame + ARGB_RESET_BYTES;
    for (i = 0; i < ARGB_LED_COUNT; i++) {
        p = argb_put_byte(p, s_led[i][1]);
        p = argb_put_byte(p, s_led[i][0]);
        p = argb_put_byte(p, s_led[i][2]);
    }
    memset(p, s_reset, ARGB_RESET_BYTES + 1);

    err = rt_spi_take_bus(s_spi);
    if (err != RT_EOK)
        return err;

    argb_pin_spi();
    rt_memset(&msg, 0, sizeof(msg));
    msg.send_buf = s_frame;
    msg.length = ARGB_FRAME_BYTES;
    msg.cs_take = 1;
    msg.cs_release = 1;
    sent = s_spi->bus->ops->xfer(s_spi, &msg);
    argb_pin_gpio();
    rt_spi_release_bus(s_spi);

    if (sent != ARGB_FRAME_BYTES)
        return -RT_EIO;
    return RT_EOK;
}

static int argb_lock(void)
{
    if (s_lock == RT_NULL)
        return -RT_ERROR;
    return rt_mutex_take(s_lock, RT_WAITING_FOREVER);
}

/* Lock is held on entry and released here. One non-inverted frame. */
static void argb_flush_both(void)
{
    int err;

    argb_set_polar(0);
    err = argb_flush_locked();
    rt_mutex_release(s_lock);
    rt_kprintf("argb: %s %d\n", err ? "failed" : "ok", err);
}

void custom_argb_init(void)
{
    struct rt_spi_configuration cfg;
    rt_err_t err;

    /* Paused: do not mux PD.8 or start SPI3. */
    rt_kprintf("argb: paused\n");
    return;

    if (s_ready)
        return;

    argb_pin_gpio();
    memset(s_led, 0, sizeof(s_led));

    err = aic_spi_bus_attach_device("spi3", "argb0");
    if (err != RT_EOK) {
        rt_kprintf("argb: attach spi3 failed %d\n", err);
        return;
    }
    s_spi = (struct rt_spi_device *)rt_device_find("argb0");
    if (s_spi == RT_NULL || s_spi->bus == RT_NULL) {
        rt_kprintf("argb: argb0 missing\n");
        return;
    }

    rt_memset(&cfg, 0, sizeof(cfg));
    cfg.mode = RT_SPI_MODE_0 | RT_SPI_MSB | RT_SPI_NO_CS;
    cfg.data_width = 8;
    cfg.max_hz = ARGB_SPI_HZ;
    rt_spi_configure(s_spi, &cfg);
    err = rt_spi_take_bus(s_spi);
    if (err != RT_EOK) {
        rt_kprintf("argb: configure spi3 failed %d\n", err);
        return;
    }
    rt_spi_release_bus(s_spi);

    s_lock = rt_mutex_create("argb", RT_IPC_FLAG_PRIO);
    if (s_lock == RT_NULL) {
        rt_kprintf("argb: mutex failed\n");
        return;
    }

    s_ready = 1;
    argb_set_polar(0);
    if (argb_lock() == RT_EOK) {
        err = argb_flush_locked();
        rt_mutex_release(s_lock);
    } else {
        err = -RT_ERROR;
    }
    if (err != RT_EOK) {
        s_ready = 0;
        argb_pin_gpio();
        rt_kprintf("argb: off frame failed %d\n", err);
    } else {
        rt_kprintf("argb: spi3 PD8 MOSI, 15 off\n");
    }
}

static int argb_parse_u8(const char *text, unsigned *out)
{
    char *end = RT_NULL;
    unsigned long v;

    if (text == RT_NULL || text[0] == '\0')
        return -1;
    v = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || v > 255)
        return -1;
    *out = (unsigned)v;
    return 0;
}

static void argb_spi_hold(int high)
{
    struct rt_spi_message msg;
    unsigned i;
    rt_size_t sent;

    memset(s_level, high ? 0xFF : 0x00, sizeof(s_level));
    if (rt_spi_take_bus(s_spi) != RT_EOK) {
        rt_kprintf("argb: spi bus busy\n");
        return;
    }
    argb_pin_spi();
    for (i = 0; i < 1500; i++) {
        rt_memset(&msg, 0, sizeof(msg));
        msg.send_buf = s_level;
        msg.length = sizeof(s_level);
        msg.cs_take = 1;
        msg.cs_release = 1;
        sent = s_spi->bus->ops->xfer(s_spi, &msg);
        if (sent != sizeof(s_level)) {
            rt_kprintf("argb: spi hold failed\n");
            break;
        }
        if ((i & 63) == 0)
            rt_thread_mdelay(1);
    }
    argb_drive(0);
    rt_spi_release_bus(s_spi);
}

static void argb_blink(void)
{
    unsigned i;
    unsigned long cfg;

    rt_kprintf("argb: blink gpio cfg %08x out %08x\n",
               (unsigned int)(unsigned long)cfg_reg(ARGB_PIN_GROUP, ARGB_PIN_NUM),
               (unsigned int)(unsigned long)gen_reg(ARGB_PIN_GROUP, 0x4));
    for (i = 0; i < 5; i++) {
        argb_drive(0);
        cfg = *(volatile unsigned long *)cfg_reg(ARGB_PIN_GROUP, ARGB_PIN_NUM);
        rt_kprintf("argb: gpio 0 cfg=%08x out=%u in=%u\n",
                   (unsigned int)cfg, argb_out_bit(), argb_in_bit());
        rt_thread_mdelay(500);
        argb_drive(1);
        cfg = *(volatile unsigned long *)cfg_reg(ARGB_PIN_GROUP, ARGB_PIN_NUM);
        rt_kprintf("argb: gpio 1 cfg=%08x out=%u in=%u\n",
                   (unsigned int)cfg, argb_out_bit(), argb_in_bit());
        rt_thread_mdelay(500);
    }

    rt_kprintf("argb: blink pull\n");
    hal_gpio_set_func(ARGB_PIN_GROUP, ARGB_PIN_NUM, ARGB_FUNC_GPIO);
    hal_gpio_direction_input(ARGB_PIN_GROUP, ARGB_PIN_NUM);
    hal_gpio_set_bias_pull(ARGB_PIN_GROUP, ARGB_PIN_NUM, PIN_PULL_UP);
    rt_thread_mdelay(500);
    hal_gpio_set_bias_pull(ARGB_PIN_GROUP, ARGB_PIN_NUM, PIN_PULL_DOWN);
    rt_thread_mdelay(500);

    rt_kprintf("argb: blink spi 0\n");
    argb_spi_hold(0);
    rt_kprintf("argb: blink spi 1\n");
    argb_spi_hold(1);
    argb_drive(0);
    rt_kprintf("argb: blink done\n");
}

static void argb_usage(void)
{
    rt_kprintf("argb off\n");
    rt_kprintf("argb fill <r> <g> <b>\n");
    rt_kprintf("argb led <0-14> <r> <g> <b>\n");
    rt_kprintf("argb test\n");
    rt_kprintf("argb blink\n");
}

static void argb_cmd(int argc, char **argv)
{
    unsigned n, r, g, b;

    if (!s_ready) {
        rt_kprintf("argb: not ready\n");
        return;
    }
    if (argc < 2) {
        argb_usage();
        return;
    }

    if (!strcmp(argv[1], "off")) {
        if (argb_lock() != RT_EOK)
            return;
        memset(s_led, 0, sizeof(s_led));
        argb_flush_both();
        return;
    } else if (!strcmp(argv[1], "fill") && argc == 5) {
        if (argb_parse_u8(argv[2], &r) || argb_parse_u8(argv[3], &g) ||
            argb_parse_u8(argv[4], &b)) {
            argb_usage();
            return;
        }
        if (argb_lock() != RT_EOK)
            return;
        for (n = 0; n < ARGB_LED_COUNT; n++) {
            s_led[n][0] = (uint8_t)r;
            s_led[n][1] = (uint8_t)g;
            s_led[n][2] = (uint8_t)b;
        }
        argb_flush_both();
        return;
    } else if (!strcmp(argv[1], "led") && argc == 6) {
        if (argb_parse_u8(argv[2], &n) || n >= ARGB_LED_COUNT ||
            argb_parse_u8(argv[3], &r) || argb_parse_u8(argv[4], &g) ||
            argb_parse_u8(argv[5], &b)) {
            argb_usage();
            return;
        }
        if (argb_lock() != RT_EOK)
            return;
        s_led[n][0] = (uint8_t)r;
        s_led[n][1] = (uint8_t)g;
        s_led[n][2] = (uint8_t)b;
        argb_flush_both();
        return;
    } else if (!strcmp(argv[1], "test")) {
        if (argb_lock() != RT_EOK)
            return;
        memset(s_led, 0, sizeof(s_led));
        s_led[0][0] = 255;
        s_led[1][1] = 255;
        s_led[2][2] = 255;
        argb_flush_both();
        return;
    } else if (!strcmp(argv[1], "blink")) {
        argb_blink();
        return;
    } else {
        argb_usage();
        return;
    }

}
MSH_CMD_EXPORT_ALIAS(argb_cmd, argb, WS2812B strip on PD8);
