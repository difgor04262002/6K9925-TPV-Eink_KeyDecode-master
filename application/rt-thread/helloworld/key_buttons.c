/* KEY1..KEY5 on PD.23..PD.27, active high with external pull-downs. */
#include <rtthread.h>
#include <rtdevice.h>

#ifdef AIC_EPD_P118F04

#define KEY_COUNT           5
#define KEY_DEBOUNCE_COUNT  5
#define KEY_POLL_MS         10

static const char *const key_pin_names[KEY_COUNT] = {
    "PD.23", "PD.24", "PD.25", "PD.26", "PD.27"
};

static void key_scan_thread(void *parameter)
{
    rt_base_t pins[KEY_COUNT];
    rt_uint8_t stable[KEY_COUNT] = {0};
    rt_uint8_t sample[KEY_COUNT] = {0};
    rt_uint8_t count[KEY_COUNT] = {0};
    int i;

    (void)parameter;
    for (i = 0; i < KEY_COUNT; i++) {
        pins[i] = rt_pin_get(key_pin_names[i]);
        if (pins[i] < 0) {
            rt_kprintf("key: invalid pin %s\n", key_pin_names[i]);
            return;
        }
        rt_pin_mode(pins[i], PIN_MODE_INPUT);
    }

    while (1) {
        for (i = 0; i < KEY_COUNT; i++) {
            rt_uint8_t high = rt_pin_read(pins[i]) == PIN_HIGH;

            if (high != sample[i]) {
                sample[i] = high;
                count[i] = 1;
            } else if (count[i] < KEY_DEBOUNCE_COUNT) {
                count[i]++;
            }

            if (count[i] == KEY_DEBOUNCE_COUNT && stable[i] != sample[i]) {
                stable[i] = sample[i];
                if (stable[i])
                    rt_kprintf("KEY%d pressed (%s)\n", i + 1, key_pin_names[i]);
            }
        }
        rt_thread_mdelay(KEY_POLL_MS);
    }
}

static int key_buttons_init(void)
{
    rt_thread_t thread = rt_thread_create("keys", key_scan_thread, RT_NULL,
                                          1024, 23, 10);

    if (thread == RT_NULL) {
        rt_kprintf("key: scan thread create failed\n");
        return -RT_ENOMEM;
    }
    rt_thread_startup(thread);
    return RT_EOK;
}
INIT_APP_EXPORT(key_buttons_init);

#endif /* AIC_EPD_P118F04 */
