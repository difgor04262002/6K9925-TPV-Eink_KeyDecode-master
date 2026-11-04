/*
 * Copyright (c) 2024, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Ning Fang <ning.fang@artinchip.com>
 */

#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>

void lv_port_indev_init(void);
void aic_touch_inputevent_cb(rt_int16_t x, rt_int16_t y, rt_uint8_t state);
/* Touch UI is only active on screen_main (AA 01). Other screens drop events. */
void aic_touch_set_ui_enabled(bool enable);
/* Latest driver sample (not gated by LVGL wait_until_release / PRESS_LOST). */
void aic_touch_get_last(int16_t *x, int16_t *y, int *pressed);
/* Same sample after LVGL display rotation — matches widget / get_point space. */
void aic_touch_get_last_lvgl(lv_point_t *p, int *pressed);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
