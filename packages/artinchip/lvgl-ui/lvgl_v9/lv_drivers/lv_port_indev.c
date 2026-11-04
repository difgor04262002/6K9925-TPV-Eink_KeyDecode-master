/*
 * Copyright (c) 2025, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Ning Fang <ning.fang@artinchip.com>
 */

#include <rtconfig.h>
#ifdef KERNEL_RTTHREAD
#include <lvgl.h>
#include <stdbool.h>
#include <rtthread.h>
#include <../components/drivers/include/drivers/touch.h>

static lv_indev_state_t last_state = LV_INDEV_STATE_REL;
static rt_int16_t last_x = 0;
static rt_int16_t last_y = 0;
static lv_indev_t *indev_touchpad;
static bool s_touch_ui_enabled;

static void input_read(lv_indev_t *indev_drv, lv_indev_data_t *data)
{
    data->point.x = last_x;
    data->point.y = last_y;
    data->state = last_state;
}

void aic_touch_set_ui_enabled(bool enable)
{
    s_touch_ui_enabled = enable;
    last_state = LV_INDEV_STATE_RELEASED;
    if (indev_touchpad) {
        lv_indev_enable(indev_touchpad, enable);
        if (!enable)
            lv_indev_reset(indev_touchpad, NULL);
    }
}

void aic_touch_get_last(int16_t *x, int16_t *y, int *pressed)
{
    if (x)
        *x = last_x;
    if (y)
        *y = last_y;
    if (pressed)
        *pressed = (last_state == LV_INDEV_STATE_PRESSED) ? 1 : 0;
}

void aic_touch_get_last_lvgl(lv_point_t *p, int *pressed)
{
    int32_t x = last_x;
    int32_t y = last_y;
    int32_t tmp;
    int32_t native_hor;
    int32_t native_ver;
    lv_display_t *disp;
    lv_display_rotation_t rot;

    if (pressed)
        *pressed = (last_state == LV_INDEV_STATE_PRESSED) ? 1 : 0;
    if (!p)
        return;

    /* Match indev_pointer_proc: it uses disp->hor_res/ver_res (unswapped). */
    disp = lv_display_get_default();
    rot = disp ? lv_display_get_rotation(disp) : LV_DISPLAY_ROTATION_0;
    if (rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270) {
        native_hor = lv_display_get_vertical_resolution(disp);
        native_ver = lv_display_get_horizontal_resolution(disp);
    } else {
        native_hor = disp ? lv_display_get_horizontal_resolution(disp) : 0;
        native_ver = disp ? lv_display_get_vertical_resolution(disp) : 0;
    }
    if (rot == LV_DISPLAY_ROTATION_180 || rot == LV_DISPLAY_ROTATION_270) {
        x = native_hor - x - 1;
        y = native_ver - y - 1;
    }
    if (rot == LV_DISPLAY_ROTATION_90 || rot == LV_DISPLAY_ROTATION_270) {
        tmp = y;
        y = x;
        x = native_ver - tmp - 1;
    }
    if (disp) {
        int32_t hor = lv_display_get_horizontal_resolution(disp);
        int32_t ver = lv_display_get_vertical_resolution(disp);

        if (hor > 0) {
            if (x < 0)
                x = 0;
            if (x >= hor)
                x = hor - 1;
        }
        if (ver > 0) {
            if (y < 0)
                y = 0;
            if (y >= ver)
                y = ver - 1;
        }
    }
    p->x = x;
    p->y = y;
}

void aic_touch_inputevent_cb(rt_int16_t x, rt_int16_t y, rt_uint8_t state)
{
    if (!s_touch_ui_enabled) {
        last_state = LV_INDEV_STATE_RELEASED;
        return;
    }
#ifdef AIC_BT_BT8858A
	extern int bt_hid_set_touch_event(int, unsigned short, unsigned short);
	bt_hid_set_touch_event(state, y, x);
#endif
    switch (state)
    {
    case RT_TOUCH_EVENT_UP:
        last_state = LV_INDEV_STATE_RELEASED;
        break;
    case RT_TOUCH_EVENT_MOVE:
    case RT_TOUCH_EVENT_DOWN:
#ifdef AIC_TOUCH_X_COORDINATE_RANGE
        if (x < 0)
            x = 0;
        if (x >= AIC_TOUCH_X_COORDINATE_RANGE)
            x = AIC_TOUCH_X_COORDINATE_RANGE - 1;
#endif
#ifdef AIC_TOUCH_Y_COORDINATE_RANGE
        if (y < 0)
            y = 0;
        if (y >= AIC_TOUCH_Y_COORDINATE_RANGE)
            y = AIC_TOUCH_Y_COORDINATE_RANGE - 1;
#endif
        last_x = x;
        last_y = y;
        last_state = LV_INDEV_STATE_PRESSED;
        break;
#ifdef AIC_MONKEY_TEST
    case RT_TOUCH_MONKEY_TEST:
        last_x = x;
        last_y = y;
        last_state = LV_INDEV_STATE_PRESSED;
        break;
#endif
    }
}

void lv_port_indev_init(void)
{
    indev_touchpad = lv_indev_create();
    lv_indev_set_type(indev_touchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_touchpad, input_read);
    /* Splash / Area4 / main_1: no UI touch until AA 01 (screen_main). */
    aic_touch_set_ui_enabled(false);
#ifdef AIC_USING_ENCODER
    extern lv_indev_t *lv_aic_encoder_create(void);
    lv_indev_t * encoder = lv_aic_encoder_create();
    if (encoder == NULL)
        return;

    lv_group_set_default(lv_group_create());
    lv_indev_set_group(encoder, lv_group_get_default());
#endif
#ifdef AIC_USE_LV_USB_MOUSE
    extern void lv_aic_mouse_hotplug_init(void);
    lv_aic_mouse_hotplug_init();
#endif
}
#endif
