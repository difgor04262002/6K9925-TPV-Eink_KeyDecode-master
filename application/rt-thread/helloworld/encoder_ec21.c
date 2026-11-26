/* EC21A encoder: A = PD.20, B = PD.21, C = GND. */
#include <rtthread.h>
#include <rtdevice.h>

#ifdef AIC_EPD_P118F04

#define ENCODER_SAMPLE_MS       1
/* Six 1 ms samples span at least 5 ms, the specified maximum bounce time. */
#define ENCODER_STABLE_SAMPLES  6
#define ENCODER_EDGES_PER_STEP  2
#define ENCODER_VALUE_MIN       (-128)
#define ENCODER_VALUE_MAX       127

/* Clockwise A leads B: 00 -> 10 -> 11 -> 01 -> 00. */
static const signed char encoder_transition[16] = {
     0, -1,  1,  0,
     1,  0,  0, -1,
    -1,  0,  0,  1,
     0,  1, -1,  0
};

static rt_uint8_t encoder_read(rt_base_t pin_a, rt_base_t pin_b)
{
    return ((rt_pin_read(pin_a) == PIN_HIGH) << 1) |
            (rt_pin_read(pin_b) == PIN_HIGH);
}

static void encoder_thread(void *parameter)
{
    rt_base_t pin_a = rt_pin_get("PD.20");
    rt_base_t pin_b = rt_pin_get("PD.21");
    rt_uint8_t stable, sample, count = 0;
    int edges = 0, value = 0;

    (void)parameter;
    if (pin_a < 0 || pin_b < 0) {
        rt_kprintf("EC21: invalid A/B pin\n");
        return;
    }

    /* The PCB supplies the pull-ups; C is tied to ground. */
    rt_pin_mode(pin_a, PIN_MODE_INPUT);
    rt_pin_mode(pin_b, PIN_MODE_INPUT);
    stable = encoder_read(pin_a, pin_b);
    sample = stable;
    rt_kprintf("EC21 value: %d\n", value);

    while (1) {
        rt_uint8_t current = encoder_read(pin_a, pin_b);

        if (current != sample) {
            sample = current;
            count = 1;
        } else if (count < ENCODER_STABLE_SAMPLES) {
            count++;
        }

        if (count == ENCODER_STABLE_SAMPLES && stable != sample) {
            if ((stable ^ sample) == 3) {
                /* Both bits changed: an intermediate state was missed. */
                edges = 0;
            } else {
                edges += encoder_transition[(stable << 2) | sample];
                if (edges >= ENCODER_EDGES_PER_STEP) {
                    if (value == ENCODER_VALUE_MAX)
                        value = ENCODER_VALUE_MIN;
                    else
                        value++;
                    rt_kprintf("EC21 value: %d\n", value);
                    edges = 0;
                } else if (edges <= -ENCODER_EDGES_PER_STEP) {
                    if (value == ENCODER_VALUE_MIN)
                        value = ENCODER_VALUE_MAX;
                    else
                        value--;
                    rt_kprintf("EC21 value: %d\n", value);
                    edges = 0;
                }
            }
            stable = sample;
        }
        rt_thread_mdelay(ENCODER_SAMPLE_MS);
    }
}

static int encoder_ec21_init(void)
{
    rt_thread_t thread = rt_thread_create("ec21", encoder_thread, RT_NULL,
                                          1024, 23, 10);

    if (thread == RT_NULL) {
        rt_kprintf("EC21: scan thread create failed\n");
        return -RT_ENOMEM;
    }
    else {
        rt_kprintf("EC21: scan thread create pass\n");
    }
    rt_thread_startup(thread);
    return RT_EOK;
}
INIT_APP_EXPORT(encoder_ec21_init);

#endif /* AIC_EPD_P118F04 */
