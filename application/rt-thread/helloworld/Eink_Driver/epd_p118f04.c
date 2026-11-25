/*
 * P118F04 / KEGM011800F04. Controller scan is 240 source x 206 gate.
 * Sequence follows the vendor sample EPD_Init_from_OTP(), then sends
 * epd_carpet_frame (the carpet JPEG, quantized to 6 colors).
 * Frame layout is 120 bytes per row, 206 rows. A 206-wide packing
 * is sheared into horizontal stripes on this panel.
 * The sample's SPI_MODE is 3-wire: one SDA flag bit, then 8 data bits.
 * DC is held low and is not used as the command/data select.
 *
 * J14 pin   SoC     Module
 *  5        PE.5    10 CSB
 *  6        PE.8    11 SCL
 *  7        PE.11    8 RST_N
 *  9        PE.7    12 SDA
 * 10        PE.10    9 DC
 * 11        PE.9     7 BUSY_N  (input, adapter pull-up)
 * PE.6 is left unconnected.
 */
#include <rtthread.h>
#include <aic_core.h>
#include <aic_hal_gpio.h>
#include <aic_time.h>

#ifdef AIC_EPD_P118F04

#define EPD_W            240
#define EPD_H            206
#define EPD_FRAME_BYTES  ((EPD_W / 2) * EPD_H)
#define EPD_BUSY_MS      40000

/*
 * The glass already shows epd_carpet_frame. The factory waveform
 * drives every pixel through blue, yellow and red before it settles,
 * so a boot-time refresh flashes. Leave the bistable image in place.
 * Set this to 0 when the frame itself changes and must be written once.
 */
#define EPD_HOLD_IMAGE   1

extern const uint8_t epd_carpet_frame[EPD_FRAME_BYTES];

static long s_cs;
static long s_scl;
static long s_sda;
static long s_dc;
static long s_rst;
static long s_busy;
static uint8_t s_pins_ok;
static uint8_t s_sem_ok;
static struct rt_semaphore s_job_sem;
void epd_kick(void)
{
    if (!s_sem_ok)
        return;
    rt_sem_release(&s_job_sem);
}

static void epd_level(long pin, int high)
{
    unsigned int g = GPIO_GROUP(pin);
    unsigned int p = GPIO_GROUP_PIN(pin);

    if (high)
        hal_gpio_set_output(g, p);
    else
        hal_gpio_clr_output(g, p);
}

static void epd_out(long pin, int high)
{
    unsigned int g = GPIO_GROUP(pin);
    unsigned int p = GPIO_GROUP_PIN(pin);

    hal_gpio_set_func(g, p, 1);
    hal_gpio_set_bias_pull(g, p, high ? PIN_PULL_UP : PIN_PULL_DIS);
    epd_level(pin, high);
    hal_gpio_direction_output(g, p);
    epd_level(pin, high);
}

static void epd_in(long pin)
{
    unsigned int g = GPIO_GROUP(pin);
    unsigned int p = GPIO_GROUP_PIN(pin);

    hal_gpio_set_func(g, p, 1);
    hal_gpio_set_bias_pull(g, p, PIN_PULL_UP);
    hal_gpio_direction_input(g, p);
}

static int epd_read(long pin)
{
    unsigned int g = GPIO_GROUP(pin);
    unsigned int p = GPIO_GROUP_PIN(pin);
    unsigned int v = 1;

    hal_gpio_get_value(g, p, &v);
    return v ? 1 : 0;
}

static void epd_sda_out(void)
{
    epd_out(s_sda, 1);
}

static void epd_sda_in(void)
{
    unsigned int g = GPIO_GROUP(s_sda);
    unsigned int p = GPIO_GROUP_PIN(s_sda);

    hal_gpio_set_func(g, p, 1);
    hal_gpio_set_bias_pull(g, p, PIN_PULL_DIS);
    hal_gpio_direction_input(g, p);
}

static int epd_wait_busy(void)
{
    rt_tick_t start = rt_tick_get();
    rt_tick_t limit = rt_tick_from_millisecond(EPD_BUSY_MS);

    while (!epd_read(s_busy)) {
        if ((rt_tick_t)(rt_tick_get() - start) > limit) {
            rt_kprintf("epd: BUSY_N stayed low\n");
            return -1;
        }
        rt_thread_mdelay(1);
    }
    return 0;
}

/* PON/DRF pull BUSY_N low while the rails or the waveform run, then
 * release it. Already-high is not a finished power-on. */
static int epd_wait_pulse(const char *name, int arm_ms)
{
    int arm;
    int ms;

    for (arm = 0; arm < arm_ms; arm++) {
        if (!epd_read(s_busy))
            break;
        rt_thread_mdelay(1);
    }
    if (arm >= arm_ms) {
        rt_kprintf("epd: %s busy stayed high\n", name);
        return -1;
    }
    for (ms = arm; ms < EPD_BUSY_MS; ms++) {
        if (epd_read(s_busy)) {
            rt_kprintf("epd: %s busy %d ms\n", name, ms);
            return 0;
        }
        rt_thread_mdelay(1);
    }
    rt_kprintf("epd: %s BUSY_N stayed low\n", name);
    return -1;
}

/* HX5325-A R01h: bit[6:0] 0x00..0x78 is 3.0V..15.0V. 0xFF is not in the table. */
static int epd_volt_in_range(const uint8_t volt[5], uint8_t vcom)
{
    int i;

    if (volt[0] == 0xFF)
        return 0;
    for (i = 1; i < 5; i++) {
        if ((volt[i] & 0x7F) > 0x78)
            return 0;
    }
    return (vcom & 0x7F) <= 0x50;
}

static void epd_shift_out(uint8_t data)
{
    int i;

    /* HX5325-A TSDS is 30ns. Data has to be stable before SCL rises. */
    for (i = 0; i < 8; i++) {
        epd_level(s_scl, 0);
        epd_level(s_sda, (data & 0x80) ? 1 : 0);
        aic_udelay(1);
        epd_level(s_scl, 1);
        aic_udelay(1);
        data <<= 1;
    }
}

/* 3-wire flag: SDA 0 = command, SDA 1 = data, then one clock. */
static void epd_flag_bit(int level)
{
    epd_level(s_scl, 0);
    epd_level(s_sda, level);
    aic_udelay(1);
    epd_level(s_scl, 1);
    aic_udelay(1);
}

static void epd_cmd(uint8_t cmd)
{
    epd_sda_out();
    epd_level(s_cs, 0);
    epd_flag_bit(0);
    epd_shift_out(cmd);
    epd_level(s_cs, 1);
    epd_level(s_scl, 0);
}

static void epd_data(uint8_t data)
{
    epd_sda_out();
    epd_level(s_cs, 0);
    epd_flag_bit(1);
    epd_shift_out(data);
    epd_level(s_cs, 1);
    epd_level(s_scl, 0);
}

static uint8_t epd_data_in(void)
{
    uint8_t data = 0;
    int i;

    epd_sda_out();
    epd_level(s_cs, 0);
    epd_flag_bit(1);
    epd_sda_in();
    for (i = 0; i < 8; i++) {
        epd_level(s_scl, 0);
        aic_udelay(1);
        data <<= 1;
        data |= epd_read(s_sda) ? 0x01 : 0x00;
        epd_level(s_scl, 1);
        aic_udelay(1);
    }
    epd_level(s_scl, 0);
    epd_level(s_cs, 1);
    epd_sda_out();
    return data;
}

static uint8_t s_volt[5] = { 0x00, 0x1E, 0x25, 0x28, 0x32 };
static uint8_t s_vcom = 0x28;
static uint8_t s_frame = 0x02;

static void epd_bus_claim(void)
{
    /* Deep sleep ignores every command until RST_N. Drive the pins
     * here so that pulse is GPIO, not whatever QSPI1 left on PE. */
    epd_out(s_cs, 1);
    epd_out(s_scl, 0);
    epd_out(s_sda, 1);
    epd_out(s_dc, 0);
    epd_out(s_rst, 1);
    epd_in(s_busy);
}

static void epd_reset(void)
{
    epd_level(s_cs, 1);
    epd_level(s_dc, 0);
    epd_level(s_rst, 1);
    rt_thread_mdelay(20);
    epd_level(s_rst, 0);
    rt_thread_mdelay(20);
    epd_level(s_rst, 1);
    rt_thread_mdelay(20);
}

static void epd_write_rails(void)
{
    int i;

    epd_cmd(0x00);
    epd_data(0x17);
    epd_data(0xE9);
    epd_cmd(0x01);
    epd_data(0x0F);
    for (i = 0; i < 5; i++)
        epd_data(s_volt[i]);
    epd_cmd(0x06);
    epd_data(0x17);
    epd_data(0x17);
    epd_data(0x27);
    epd_cmd(0x30);
    epd_data(s_frame);
    epd_cmd(0x82);
    epd_data(s_vcom);
}

/* R04h runs only while BUSY_N is high. No parameter in the datasheet. */
static int epd_pon(void)
{
    if (epd_wait_busy())
        return -1;
    rt_kprintf("epd: cmd 04\n");
    epd_cmd(0x04);
    epd_data(0x00);
    return epd_wait_pulse("pon", 200);
}

static int epd_init_otp(void)
{
    uint8_t volt[5];
    uint8_t vcom;
    uint8_t frame;
    int i;

    epd_bus_claim();
    epd_reset();
    rt_kprintf("epd: busy %d after reset\n", epd_read(s_busy));
    if (epd_wait_busy())
        return -1;

    epd_cmd(0x00);
    epd_data(0x0F);
    epd_data(0x69);

    epd_cmd(0x01);
    epd_data(0x00);

    epd_cmd(0x04);
    if (epd_wait_busy())
        return -1;
    epd_cmd(0x20);
    epd_data(0x00);
    if (epd_wait_busy())
        return -1;

    epd_cmd(0xF5);
    epd_data(0xA5);

    epd_cmd(0x94);
    (void)epd_data_in();
    volt[0] = epd_data_in();
    volt[1] = epd_data_in();
    volt[2] = epd_data_in();
    volt[3] = epd_data_in();
    volt[4] = epd_data_in();
    vcom = epd_data_in();
    frame = epd_data_in();

    epd_cmd(0xF5);
    epd_data(0x00);

    rt_kprintf("epd: volt %02X %02X %02X %02X %02X vcom %02X frame %02X\n",
               volt[0], volt[1], volt[2], volt[3], volt[4], vcom, frame);
    /* P118 init writes these bytes into R01h/R30h/R82h, then the later
     * 0x04 generates the rails. A failed 0x94 read is 0xFF, and R01h
     * source code 0x7F is past 15.0V. Keep the OTP this panel returned
     * when the controller answered: gate ±20V, sources +6.0/-6.7/+7.0/-8.0V,
     * VCOM -2.00V, 50Hz. */
    if (!epd_volt_in_range(volt, vcom) || frame == 0xFF) {
        volt[0] = 0x00;
        volt[1] = 0x1E;
        volt[2] = 0x25;
        volt[3] = 0x28;
        volt[4] = 0x32;
        vcom = 0x28;
        frame = 0x02;
        rt_kprintf("epd: volt use panel otp\n");
    }
    for (i = 0; i < 5; i++)
        s_volt[i] = volt[i];
    s_vcom = vcom;
    s_frame = frame;

    epd_cmd(0x66);
    epd_data(0x49);
    epd_data(0x55);
    epd_data(0x13);
    epd_data(0x5D);
    epd_data(0x05);
    epd_data(0x10);
    aic_udelay(10);

    epd_cmd(0x61);
    epd_data(0x00);
    epd_data(0xF0);
    epd_data(0x00);
    epd_data(0xCE);
    aic_udelay(10);

    epd_cmd(0x13);
    epd_data(0x00);
    epd_data(0x00);
    aic_udelay(10);

    epd_cmd(0x00);
    epd_data(0x17);
    epd_data(0xE9);
    aic_udelay(10);

    epd_cmd(0x01);
    epd_data(0x0F);
    epd_data(volt[0]);
    epd_data(volt[1]);
    epd_data(volt[2]);
    epd_data(volt[3]);
    epd_data(volt[4]);
    aic_udelay(10);

    epd_cmd(0x06);
    epd_data(0x17);
    epd_data(0x17);
    epd_data(0x27);
    aic_udelay(10);

    epd_cmd(0x30);
    epd_data(frame);
    aic_udelay(10);

    epd_cmd(0x82);
    epd_data(vcom);
    aic_udelay(10);

    epd_cmd(0xE3);
    epd_data(0x01);

    epd_cmd(0x50);
    epd_data(0x3F);

    /* Qingyue OTP sample for this glass. Color waveform stays the one
     * stored in the panel (0x20 0x00). This second plane is 703 x 0xFF
     * and a trailing 0x00, matching their LUT1. */
    epd_cmd(0x21);
    for (i = 0; i < 703; i++)
        epd_data(0xFF);
    epd_data(0x00);
    /* HX5325-A R00h P_mode=1 is 3-bit, the deepest mode. R61h is the
     * panel maximum: 240 source by 206 gate. The film has six particles. */
    rt_kprintf("epd: 240x206 3bit 6color\n");
    rt_kprintf("epd: otp waveform\n");
    return 0;
}

static void epd_show_frame(const uint8_t *frame)
{
    uint32_t i;

    epd_cmd(0x10);
    for (i = 0; i < EPD_FRAME_BYTES; i++)
        epd_data(frame[i]);
}

static void epd_show_carpet(void)
{
    epd_show_frame(epd_carpet_frame);
}

static int epd_update(void)
{
    /* R04h is ignored unless BUSY_N is high. Deep sleep ignores it until
     * RST_N. If the first one does not run, wake the controller and
     * write the rails again, then issue 0x04 once more. */
    if (epd_pon()) {
        epd_bus_claim();
        epd_reset();
        rt_kprintf("epd: busy %d after reset\n", epd_read(s_busy));
        if (epd_wait_busy())
            return -1;
        epd_write_rails();
        if (epd_pon())
            return -1;
    }

    epd_cmd(0x12);
    epd_data(0x00);
    if (epd_wait_pulse("refresh", 1000))
        return -1;

    epd_cmd(0x02);
    epd_data(0x00);
    if (epd_wait_pulse("off", 200))
        return -1;
    return 0;
}

static void epd_sleep(void)
{
    epd_cmd(0x07);
    epd_data(0xA5);
}

void epd_show_packed(const uint8_t *frame)
{
    if (!s_pins_ok || !frame)
        return;
    rt_kprintf("epd: refresh\n");
    if (epd_init_otp())
        return;
    epd_show_frame(frame);
    if (epd_update()) {
        rt_kprintf("epd: booster not started\n");
        return;
    }
    epd_sleep();
    rt_kprintf("epd: refresh done\n");
}

static int epd_pins_init(void)
{
    long pins[6];
    int i;

    s_cs = hal_gpio_name2pin("PE.5");
    s_scl = hal_gpio_name2pin("PE.8");
    s_sda = hal_gpio_name2pin("PE.7");
    s_dc = hal_gpio_name2pin("PE.10");
    s_rst = hal_gpio_name2pin("PE.11");
    s_busy = hal_gpio_name2pin("PE.9");
    pins[0] = s_cs;
    pins[1] = s_scl;
    pins[2] = s_sda;
    pins[3] = s_dc;
    pins[4] = s_rst;
    pins[5] = s_busy;
    for (i = 0; i < 6; i++) {
        if (pins[i] < 0) {
            rt_kprintf("epd: pin lookup failed\n");
            return -1;
        }
    }

    epd_bus_claim();
    return 0;
}

extern void epd_usb_mem_init(void);
extern void epd_usb_poll(void);

static void epd_thread(void *arg)
{
    (void)arg;
    epd_usb_mem_init();
    if (epd_pins_init())
        return;
    s_pins_ok = 1;
    if (EPD_HOLD_IMAGE) {
        rt_kprintf("epd: hold\n");
    } else {
        rt_kprintf("epd: carpet 240x206\n");
        if (!epd_init_otp()) {
            epd_show_carpet();
            if (!epd_update()) {
                epd_sleep();
                rt_kprintf("epd: carpet done\n");
            }
        }
    }
    for (;;) {
        rt_sem_take(&s_job_sem, RT_WAITING_FOREVER);
        epd_usb_poll();
    }
}

static int epd_p118f04_start(void)
{
    rt_thread_t th;

    rt_sem_init(&s_job_sem, "epdjob", 0, RT_IPC_FLAG_FIFO);
    s_sem_ok = 1;
    th = rt_thread_create("epd", epd_thread, RT_NULL, 32768, 22, 10);
    if (th)
        rt_thread_startup(th);
    else
        rt_kprintf("epd: thread create failed\n");
    return 0;
}
INIT_APP_EXPORT(epd_p118f04_start);

#endif
