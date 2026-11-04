#ifndef CUSTOM_DISP_PROFILE_H
#define CUSTOM_DISP_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <rtconfig.h>
#include "lvgl.h"

/*
 * Display geometry profiles (match panel + LV_ROTATE_DEGREE in defconfig):
 *
 * - LVDS 1024x600: stretch UI/images to panel size.
 * - FL7707 / YX092 bar + LV_ROTATE_90/270: FB 462x1920, logical 1920x462.
 * - FL7707 / YX092 bar without rotate: FB/logical 462x1920.
 * - R520 ST7123P / H0499 ST7121P + LV_ROTATE_90/270: logical 1280x720,
 *   full-screen STRETCH (do NOT apply 720/800 ≈ 0.9 scale — that caused
 *   black borders).
 * - R520 / H0499 without rotate: FB/logical 720x1280.
 * - Default: 1280x800 UIBuilder layout.
 */
#if defined(AIC_DISP_LVDS) || defined(AIC_PANEL_DSI_EK79007AD)
#define CUSTOM_DISP_IMG_W           1024
#define CUSTOM_DISP_IMG_H           600
#define CUSTOM_DISP_IS_LVDS_PROFILE 1
#define CUSTOM_DISP_IS_BAR_PROFILE  0
#elif defined(AIC_PANEL_DSI_FL7707_92IN) && defined(LV_ROTATE_DEGREE) \
    && ((LV_ROTATE_DEGREE == 90) || (LV_ROTATE_DEGREE == 270))
/* Logical 1920x462 matches TPV_92inch / bar viewing. Do NOT reuse R520 scale. */
#define CUSTOM_DISP_IMG_W           1920
#define CUSTOM_DISP_IMG_H           462
#define CUSTOM_DISP_IS_LVDS_PROFILE 0
#define CUSTOM_DISP_IS_BAR_PROFILE  1
#define CUSTOM_DISP_IS_R520_ROTATE  0
#define CUSTOM_DISP_IS_FL7707_ROTATE 1
#elif defined(AIC_PANEL_DSI_FL7707_92IN)
#define CUSTOM_DISP_IMG_W           462
#define CUSTOM_DISP_IMG_H           1920
#define CUSTOM_DISP_IS_LVDS_PROFILE 0
#define CUSTOM_DISP_IS_BAR_PROFILE  1
#elif (defined(AIC_PANEL_DSI_R520120T01) || defined(AIC_PANEL_DSI_H0499S010)) \
    && defined(LV_ROTATE_DEGREE) \
    && ((LV_ROTATE_DEGREE == 90) || (LV_ROTATE_DEGREE == 270))
#define CUSTOM_DISP_IMG_W           1280
#define CUSTOM_DISP_IMG_H           720
#define CUSTOM_DISP_IS_LVDS_PROFILE 0
#define CUSTOM_DISP_IS_BAR_PROFILE  1
#define CUSTOM_DISP_IS_R520_ROTATE  1
#elif defined(AIC_PANEL_DSI_R520120T01) || defined(AIC_PANEL_DSI_H0499S010)
#define CUSTOM_DISP_IMG_W           720
#define CUSTOM_DISP_IMG_H           1280
#define CUSTOM_DISP_IS_LVDS_PROFILE 0
#define CUSTOM_DISP_IS_BAR_PROFILE  1
#define CUSTOM_DISP_IS_R520_ROTATE  0
#elif defined(AIC_PANEL_DBI_ZC_T1D8BP)
#define CUSTOM_DISP_IMG_W           360
#define CUSTOM_DISP_IMG_H           360
#define CUSTOM_DISP_IS_LVDS_PROFILE 0
#define CUSTOM_DISP_IS_BAR_PROFILE  0
#define CUSTOM_DISP_IS_ROUND_PROFILE 1
#else
#define CUSTOM_DISP_IMG_W           1280
#define CUSTOM_DISP_IMG_H           800
#define CUSTOM_DISP_IS_LVDS_PROFILE 0
#define CUSTOM_DISP_IS_BAR_PROFILE  0
#endif

#ifndef CUSTOM_DISP_IS_R520_ROTATE
#define CUSTOM_DISP_IS_R520_ROTATE  0
#endif

#ifndef CUSTOM_DISP_IS_FL7707_ROTATE
#define CUSTOM_DISP_IS_FL7707_ROTATE 0
#endif

#ifndef CUSTOM_DISP_IS_ROUND_PROFILE
#define CUSTOM_DISP_IS_ROUND_PROFILE 0
#endif

/* Design size equals panel logical size — no extra UI scale. */
#ifndef CUSTOM_DISP_UI_DESIGN_W
#define CUSTOM_DISP_UI_DESIGN_W     CUSTOM_DISP_IMG_W
#endif

#ifndef CUSTOM_DISP_UI_DESIGN_H
#define CUSTOM_DISP_UI_DESIGN_H     CUSTOM_DISP_IMG_H
#endif

#define CUSTOM_DISP_IMG_ARGB_BYTES  ((uint32_t)CUSTOM_DISP_IMG_W * (uint32_t)CUSTOM_DISP_IMG_H * 4U)

void custom_disp_apply_lv_obj_image(lv_obj_t *img_obj);
void custom_disp_apply_screen_root(lv_obj_t *root_obj);
void custom_disp_apply_area4_screen(lv_obj_t *root_obj);
void custom_disp_apply_area4_stream_image(lv_obj_t *img_obj);
void custom_disp_apply_splash_screen(lv_obj_t *root_obj, lv_obj_t *img_obj);
/* 9.2" MIPI splash: stretch aic_player to panel logical size (or keep UIBuilder layout if rotated) */
void custom_disp_apply_splash_player(lv_obj_t *root_obj, lv_obj_t *player_obj);
#if defined(AIC_PANEL_DSI_FL7707_92IN)
/* FL7707 9.2": rotate splash image 90° CCW + scale to fill 462x1920; layout datetime label */
void custom_disp_apply_fl7707_splash_layout(lv_obj_t *root_obj, lv_obj_t *img_obj, lv_obj_t *label_obj);
#endif
#if defined(AIC_PANEL_DSI_R520120T01) || defined(AIC_PANEL_DSI_H0499S010) \
    || defined(AIC_PANEL_DSI_EK79007AD) || defined(AIC_DISP_LVDS)
/* R520 / H0499 / EK79007AD: full-screen stretch. Does not unhide label. */
void custom_disp_apply_r520_splash_layout(lv_obj_t *root_obj, lv_obj_t *img_obj, lv_obj_t *label_obj);
#endif
#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
void custom_disp_apply_round_splash_layout(lv_obj_t *root_obj, lv_obj_t *img_obj, lv_obj_t *label_obj);
#endif
void custom_disp_apply_screen_backgrounds(void);

#ifdef __cplusplus
}
#endif

#endif /* CUSTOM_DISP_PROFILE_H */
