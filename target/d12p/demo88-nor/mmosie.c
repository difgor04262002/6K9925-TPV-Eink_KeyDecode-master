/*
 * MMOSIE 1-wire on PD.26 (GPIO).
 *
 * Spec (table notes about 1ms/3ms are leftover; values are us):
 *   T0H=0.5us  T0L=1.5us   bit 0
 *   T1H=1.5us  T1L=0.5us   bit 1
 *   Trst idle low between 4-bit frames
 *
 *   Panel_0 boot: 0,0,0,0 so all four NY8 outputs follow PE.5.
 *   Panel_1..4: 0,1,1,1 / 1,0,1,1 / 1,1,0,1 / 1,1,1,0.
 *   Idle on every CS rise: 1,1,1,1.
 *   Wire width is MMOSIE_BITS_PER_ENTRY below.
 *
 * GPIO function 1, same as before the flash change. Each bit ends low.
 * The 4 bits and TRST finish before PE.5 changes:
 *   before PE.5 falls: select frame, then TRST+CS_EXTRA, line stays low.
 *   before PE.5 rises: 1,1,1,1, then TRST, line stays low.
 *
 * Send only while QSPI is idle, with IRQs masked for the whole 4-bit
 * frame, so highs stay 0.5/1.5us.
 */

#include <aic_core.h>

#if defined(KERNEL_RTTHREAD) && defined(AIC_PANEL_DBI_ZC_T1D8BP)

#include <rtthread.h>
#include <rthw.h>
#include <aic_hal_gpio.h>
#include <aic_time.h>
#include <sys_freq.h>

/*
 * PD.26 wire width. Frame values do not change.
 *   4 = 4 bits, one pulse per 0/1, frame[0] first.
 *   8 = 4 bytes, each 0/1 sent as 8 bits MSB first (32 pulses).
 */
#define BITS_PER_ENTRY  4
#define BYTES_PER_ENTRY 8
#define MMOSIE_BITS_PER_ENTRY  BITS_PER_ENTRY//4

#define MMOSIE_QSPI1_IDX   1
#define MMOSIE_RAMWR_MIN   256U
#define MMOSIE_T0H_US      0.5
#define MMOSIE_T0L_US      1.5
#define MMOSIE_T1H_US      1.5
#define MMOSIE_T1L_US      0.5
/* Tenths of a microsecond. 0.5 and 1.5 must not be truncated to 0 and 1. */
#define MMOSIE_US_X10(us)  ((u32)((us) * 10.0))
#define MMOSIE_TRST_US     18//2.5//5U//120U
#define MMOSIE_CS_EXTRA_US 18//2.5//5U//145U

/* 0 = keep the 4-bit encoder, do not drive PD.26. PE.5 CS is unchanged. */
#ifndef MMOSIE_PD26_ENABLE
#define MMOSIE_PD26_ENABLE 0
#endif

static volatile u8 s_frame[4] = { 0, 0, 0, 0 };
static const u8 s_idle_frame[4] = { 1, 1, 1, 1 };
static const u8 s_zero_frame[4] = { 0, 0, 0, 0 };

static unsigned int s_g, s_p;
static volatile int s_inited;
static volatile int s_preamble_done;
static volatile int s_short_pair;
static volatile int s_ramwr_cs;
static u32 s_ticks_per_us = 4;
static u32 s_cyc_per_us = 480;
static u32 s_pin_mask;
static volatile void *s_set_reg;
static volatile void *s_clr_reg;
static u32 s_cyc_t0h;
static u32 s_cyc_t0l;
static u32 s_cyc_t1h;
static u32 s_cyc_t1l;

extern volatile void *gen_reg(u32 group, u32 offset);

/* Same offsets as GEN_OUT_CLR_REG / GEN_OUT_SET_REG in aic_hal_gpio.c. */
#define MMOSIE_OUT_CLR_REG  0x10
#define MMOSIE_OUT_SET_REG  0x14

static inline u32 mmosie_cy(void)
{
    u32 c;

    __asm__ volatile("csrr %0, mcycle" : "=r"(c));
    return c;
}

static u32 mmosie_cycles_x10(u32 us_x10)
{
    u32 cyc = (us_x10 * s_cyc_per_us) / 10U;

    return cyc ? cyc : 1U;
}

static void mmosie_send_cycles(u32 high, u32 low)
{
    u32 t;

    writel(s_pin_mask, s_set_reg);
    t = mmosie_cy();
    while ((mmosie_cy() - t) < high)
        ;
    writel(s_pin_mask, s_clr_reg);
    t = mmosie_cy();
    while ((mmosie_cy() - t) < low)
        ;
}

static void mmosie_wait_cycles(u32 cycles)
{
    u32 t = mmosie_cy();

    while ((mmosie_cy() - t) < cycles)
        ;
}

static u32 mmosie_writel_cycles(void)
{
    rt_base_t level;
    u32 c0, c1, i;
    const u32 reps = 64U;

    /* Pin is already high. SET does not edge, so NY8 does not see a bit. */
    level = rt_hw_interrupt_disable();
    c0 = mmosie_cy();
    for (i = 0; i < reps; i++)
        writel(s_pin_mask, s_set_reg);
    c1 = mmosie_cy();
    rt_hw_interrupt_enable(level);
    if (c1 <= c0)
        return 1U;
    return (c1 - c0) / reps;
}

static u32 mmosie_sub_cyc(u32 value, u32 sub)
{
    if (value > sub + 1U)
        return value - sub;
    return 1U;
}

static void mmosie_calibrate(void)
{
    rt_base_t level;
    u64 t0, t1;
    u32 c0, c1;
    u32 ticks;
    u32 wr;

    s_ticks_per_us = (u32)(drv_get_sys_freq() / 1000000);
    if (!s_ticks_per_us)
        s_ticks_per_us = 4U;

    /* mcycle counts core clocks, including fetch stalls. A counted
     * spin does not: a NOR fetch stretch turned 0.5 us into ~1 us. */
    level = rt_hw_interrupt_disable();
    t0 = aic_get_ticks();
    c0 = mmosie_cy();
    do {
        t1 = aic_get_ticks();
    } while ((u32)(t1 - t0) < s_ticks_per_us * 2000U);
    c1 = mmosie_cy();
    rt_hw_interrupt_enable(level);

    ticks = (u32)(t1 - t0);
    if (ticks && c1 != c0)
        s_cyc_per_us = (u32)(((u64)(c1 - c0) * s_ticks_per_us) / ticks);
    if (s_cyc_per_us < 20U)
        s_cyc_per_us = 480U;

    /* SET-only. A CLR here is a falling edge and selects every NY8. */
    wr = mmosie_writel_cycles();

    s_cyc_t0h = mmosie_sub_cyc(mmosie_cycles_x10(MMOSIE_US_X10(MMOSIE_T0H_US)), wr);
    s_cyc_t0l = mmosie_sub_cyc(mmosie_cycles_x10(MMOSIE_US_X10(MMOSIE_T0L_US)), wr);
    s_cyc_t1h = mmosie_sub_cyc(mmosie_cycles_x10(MMOSIE_US_X10(MMOSIE_T1H_US)), wr);
    s_cyc_t1l = mmosie_sub_cyc(mmosie_cycles_x10(MMOSIE_US_X10(MMOSIE_T1L_US)), wr);
}

static void mmosie_send_bit(u8 one)
{
    if (one)
        mmosie_send_cycles(s_cyc_t1h, s_cyc_t1l);
    else
        mmosie_send_cycles(s_cyc_t0h, s_cyc_t0l);
}

static void mmosie_send_frame(const u8 frame[4], u32 tail_x10)
{
    rt_base_t level;
    unsigned b, bit;
    u8 v, one;

    /* 4 bits, then the low tail. PE.5 moves only after this returns. */
    level = rt_hw_interrupt_disable();
    for (b = 0; b < 4U; b++) {
#if MMOSIE_BITS_PER_ENTRY == 8
        v = frame[b] ? 1U : 0U;
        for (bit = 0; bit < 8U; bit++) {
            one = (v >> (7U - bit)) & 1U;
            mmosie_send_bit(one);
        }
#else
        one = frame[b] ? 1U : 0U;
        mmosie_send_bit(one);
        (void)v;
        (void)bit;
#endif
    }
    if (tail_x10)
        mmosie_wait_cycles(mmosie_cycles_x10(tail_x10));
    rt_hw_interrupt_enable(level);
}

void aic_mmosie_set_frame(const u8 frame[4])
{
    if (!frame)
        return;

    s_frame[0] = frame[0] ? 1U : 0U;
    s_frame[1] = frame[1] ? 1U : 0U;
    s_frame[2] = frame[2] ? 1U : 0U;
    s_frame[3] = frame[3] ? 1U : 0U;
}

static void mmosie_send_select(void)
{
    u8 frame[4];

    frame[0] = s_frame[0];
    frame[1] = s_frame[1];
    frame[2] = s_frame[2];
    frame[3] = s_frame[3];
    mmosie_send_frame(frame, MMOSIE_US_X10(MMOSIE_TRST_US + MMOSIE_CS_EXTRA_US));
}

void aic_mmosie_preamble(void)
{
    if (!s_inited)
        return;
    /* 2A/2B/RAMWR each send the select on their own CS falling edge. */
    s_preamble_done = 1;
}

void aic_mmosie_on_qspi_cs_low(u32 qspi_idx, u32 xfer_len)
{
    if (qspi_idx != MMOSIE_QSPI1_IDX || !s_inited)
        return;

    if (xfer_len >= MMOSIE_RAMWR_MIN) {
        s_preamble_done = 1;
        s_ramwr_cs = 1;
    }

    if (s_preamble_done)
        mmosie_send_select();
    else {
        mmosie_send_frame(s_zero_frame, MMOSIE_US_X10(MMOSIE_TRST_US + MMOSIE_CS_EXTRA_US));
    }
    s_short_pair = 1;
}

void aic_mmosie_before_qspi_cs_high(u32 qspi_idx)
{
    if (qspi_idx != MMOSIE_QSPI1_IDX || !s_inited)
        return;
    if (!s_short_pair)
        return;

    s_short_pair = 0;
    mmosie_send_frame(s_idle_frame, MMOSIE_US_X10(MMOSIE_TRST_US));
}

void aic_mmosie_on_qspi_cs_high(u32 qspi_idx)
{
    if (qspi_idx != MMOSIE_QSPI1_IDX || !s_inited)
        return;
    if (!s_ramwr_cs)
        return;
    s_ramwr_cs = 0;
    s_preamble_done = 0;
}

static int aic_mmosie_init(void)
{
    long pin;

#if !MMOSIE_PD26_ENABLE
    rt_kprintf("mmosie: PD.26 4-bit send off\n");
    return 0;
#endif
    pin = hal_gpio_name2pin("PD.26");

    if (pin < 0) {
        rt_kprintf("mmosie: PD.26 gpio failed\n");
        return -1;
    }

    s_g = GPIO_GROUP(pin);
    s_p = GPIO_GROUP_PIN(pin);
    s_pin_mask = 1U << s_p;
    s_set_reg = gen_reg(s_g, MMOSIE_OUT_SET_REG);
    s_clr_reg = gen_reg(s_g, MMOSIE_OUT_CLR_REG);
    hal_gpio_set_func(s_g, s_p, 1);
    hal_gpio_set_bias_pull(s_g, s_p, PIN_PULL_DOWN);
    hal_gpio_set_drive_strength(s_g, s_p, 3);
    hal_gpio_clr_output(s_g, s_p);
    hal_gpio_direction_output(s_g, s_p);
    hal_gpio_clr_output(s_g, s_p);

    mmosie_calibrate();
    writel(s_pin_mask, s_clr_reg);

    s_inited = 1;
    rt_kprintf("mmosie: PD.26 GPIO %u cyc/us T0=%u/%u T1=%u/%u\n",
               (unsigned)s_cyc_per_us,
               (unsigned)s_cyc_t0h, (unsigned)s_cyc_t0l,
               (unsigned)s_cyc_t1h, (unsigned)s_cyc_t1l);
    return 0;
}
INIT_DEVICE_EXPORT(aic_mmosie_init);

#endif /* KERNEL_RTTHREAD && AIC_PANEL_DBI_ZC_T1D8BP */
