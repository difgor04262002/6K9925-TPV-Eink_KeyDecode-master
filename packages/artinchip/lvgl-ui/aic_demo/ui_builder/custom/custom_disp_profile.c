#include "custom_disp_profile.h"
#include "ui_objects.h"
#include "aic_ui.h"
#if defined(AIC_PANEL_DSI_FL7707_92IN)
#include "lv_aic_player.h"
#endif

void custom_disp_apply_lv_obj_image(lv_obj_t *img_obj)
{
    if (!img_obj) {
        return;
    }

#if CUSTOM_DISP_IS_LVDS_PROFILE || CUSTOM_DISP_IS_BAR_PROFILE \
    || CUSTOM_DISP_IS_ROUND_PROFILE
    lv_obj_set_pos(img_obj, 0, 0);
    lv_obj_set_size(img_obj, CUSTOM_DISP_IMG_W, CUSTOM_DISP_IMG_H);
#if LVGL_VERSION_MAJOR >= 9
    lv_image_set_inner_align(img_obj, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_scale(img_obj, 256);
#else
    lv_img_set_zoom(img_obj, 256);
#endif
#else
    (void)img_obj;
#endif
}

void custom_disp_apply_screen_root(lv_obj_t *root_obj)
{
    if (!root_obj) {
        return;
    }

#if CUSTOM_DISP_IS_LVDS_PROFILE || CUSTOM_DISP_IS_BAR_PROFILE \
    || CUSTOM_DISP_IS_ROUND_PROFILE
    /* Full-screen root — no 720/800 (0.9) transform scale. */
    lv_obj_set_size(root_obj, CUSTOM_DISP_IMG_W, CUSTOM_DISP_IMG_H);
    lv_obj_set_style_transform_scale_x(root_obj, 256, LV_PART_MAIN);
    lv_obj_set_style_transform_scale_y(root_obj, 256, LV_PART_MAIN);
    /* Kill LVGL light-theme scr colour so screen switches do not flash white. */
    lv_obj_set_style_bg_color(root_obj, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root_obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(root_obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(root_obj, 0, LV_PART_MAIN);
#else
    (void)root_obj;
#endif
}

void custom_disp_apply_area4_screen(lv_obj_t *root_obj)
{
    if (!root_obj) {
        return;
    }

#if CUSTOM_DISP_IS_LVDS_PROFILE || CUSTOM_DISP_IS_BAR_PROFILE \
    || CUSTOM_DISP_IS_ROUND_PROFILE
    lv_obj_set_size(root_obj, CUSTOM_DISP_IMG_W, CUSTOM_DISP_IMG_H);
#endif
    custom_disp_apply_screen_root(root_obj);
}

void custom_disp_apply_area4_stream_image(lv_obj_t *img_obj)
{
    if (!img_obj) {
        return;
    }

    lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(img_obj, LV_OPA_100, 0);
    lv_obj_set_style_img_opa(img_obj, LV_OPA_100, 0);
    lv_obj_set_style_bg_opa(img_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_pos(img_obj, 0, 0);
    lv_obj_set_size(img_obj, CUSTOM_DISP_IMG_W, CUSTOM_DISP_IMG_H);

#if LVGL_VERSION_MAJOR >= 9
    lv_image_set_inner_align(img_obj, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_scale(img_obj, 256);
#else
    lv_img_set_zoom(img_obj, 256);
#endif
}

void custom_disp_apply_splash_screen(lv_obj_t *root_obj, lv_obj_t *img_obj)
{
    custom_disp_apply_area4_screen(root_obj);
    if (!img_obj)
        return;

#if defined(AIC_PANEL_DSI_FL7707_92IN) && !CUSTOM_DISP_IS_FL7707_ROTATE
    /* Full layout (rotate+scale) is applied by custom_disp_apply_fl7707_splash_layout */
    lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(img_obj, LV_OPA_100, 0);
#else
    custom_disp_apply_area4_stream_image(img_obj);
#endif
    lv_obj_move_background(img_obj);
}

#if defined(AIC_PANEL_DSI_FL7707_92IN)
/* Splash PNG native size (assets/image/20260423-005-001_1288x804.png) */
#define FL7707_SPLASH_SRC_W  1288
#define FL7707_SPLASH_SRC_H  804
/* LVGL: +angle is clockwise 0.1deg; 2700 = 270° CW = 90° CCW (向左轉90°) */
#define FL7707_SPLASH_ROT_CCW90  2700

void custom_disp_apply_fl7707_splash_layout(lv_obj_t *root_obj, lv_obj_t *img_obj, lv_obj_t *label_obj)
{
    uint32_t scale_x;
    uint32_t scale_y;
    int32_t pivot_x;
    int32_t pivot_y;

    if (root_obj) {
        custom_disp_apply_area4_screen(root_obj);
    }
    if (!img_obj) {
        return;
    }

    /*
     * Scale first so that after 90° CCW the visual size is CUSTOM_DISP_IMG
     * (462x1920): pre-rotate size must be 1920x462 (W×H).
     */
    scale_x = ((uint32_t)CUSTOM_DISP_IMG_H * 256U) / (uint32_t)FL7707_SPLASH_SRC_W; /* → 1920 */
    scale_y = ((uint32_t)CUSTOM_DISP_IMG_W * 256U) / (uint32_t)FL7707_SPLASH_SRC_H; /* → 462 */
    if (scale_x == 0U) {
        scale_x = 1U;
    }
    if (scale_y == 0U) {
        scale_y = 1U;
    }

    pivot_x = FL7707_SPLASH_SRC_W / 2;
    pivot_y = FL7707_SPLASH_SRC_H / 2;

    lv_obj_clear_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(img_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_img_opa(img_obj, LV_OPA_COVER, 0);
    lv_obj_set_pos(img_obj,
                   (int32_t)CUSTOM_DISP_IMG_W / 2 - pivot_x,
                   (int32_t)CUSTOM_DISP_IMG_H / 2 - pivot_y);

#if LVGL_VERSION_MAJOR >= 9
    lv_image_set_pivot(img_obj, pivot_x, pivot_y);
    lv_image_set_rotation(img_obj, FL7707_SPLASH_ROT_CCW90);
    lv_image_set_scale_x(img_obj, (int32_t)scale_x);
    lv_image_set_scale_y(img_obj, (int32_t)scale_y);
#else
    lv_img_set_pivot(img_obj, pivot_x, pivot_y);
    lv_img_set_angle(img_obj, FL7707_SPLASH_ROT_CCW90);
    lv_img_set_zoom(img_obj, (uint16_t)((scale_x + scale_y) / 2U));
#endif
    lv_obj_move_background(img_obj);

    if (label_obj) {
        /*
         * Mockup: datetime centered under logo, ~bottom of octagon frame.
         * Visual landscape 1920x462 → FB after rot90CCW fill:
         *   FB_x ≈ 0.83 * 462 (bottom band), FB_y = 1920/2 (horizontal center).
         */
        const int32_t time_w = 560;
        const int32_t time_h = 40;
        const int32_t time_cx = ((int32_t)CUSTOM_DISP_IMG_W * 83) / 100; /* ~383 */
        const int32_t time_cy = (int32_t)CUSTOM_DISP_IMG_H / 2;          /* 960 */

        lv_obj_clear_flag(label_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(label_obj, time_w, time_h);
        lv_obj_set_style_text_color(label_obj, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_align(label_obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_x(label_obj, time_w / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_pivot_y(label_obj, time_h / 2, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(label_obj, FL7707_SPLASH_ROT_CCW90, LV_PART_MAIN);
        lv_obj_set_pos(label_obj, time_cx - time_w / 2, time_cy - time_h / 2);
        lv_obj_move_foreground(label_obj);
    }
}
#endif /* AIC_PANEL_DSI_FL7707_92IN */

#if defined(AIC_PANEL_DSI_R520120T01) || defined(AIC_PANEL_DSI_H0499S010) \
    || defined(AIC_PANEL_DSI_EK79007AD) || defined(AIC_DISP_LVDS)
void custom_disp_apply_r520_splash_layout(lv_obj_t *root_obj, lv_obj_t *img_obj, lv_obj_t *label_obj)
{
    if (root_obj) {
        custom_disp_apply_area4_screen(root_obj);
    }
    if (img_obj) {
        custom_disp_apply_area4_stream_image(img_obj);
        lv_obj_move_background(img_obj);
    }
    if (label_obj) {
        const int32_t time_w = 560;
        const int32_t time_h = 40;
        const int32_t time_x = ((int32_t)CUSTOM_DISP_IMG_W - time_w) / 2;
        const int32_t time_y = (int32_t)CUSTOM_DISP_IMG_H - 80;

        /* Layout only — keep hidden so FB snapshot does not bake the clock. */
        lv_obj_set_size(label_obj, time_w, time_h);
        lv_obj_set_pos(label_obj, time_x, time_y);
        lv_obj_set_style_text_color(label_obj, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_align(label_obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_move_foreground(label_obj);
    }
}
#endif /* R520 / H0499 / EK79007AD / LVDS splash layout */

#if defined(AIC_PANEL_DBI_ZC_T1D8BP)
void custom_disp_apply_round_splash_layout(lv_obj_t *root_obj, lv_obj_t *img_obj, lv_obj_t *label_obj)
{
    if (root_obj) {
        custom_disp_apply_area4_screen(root_obj);
        lv_obj_set_style_bg_color(root_obj, lv_color_hex(0x000000), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(root_obj, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(root_obj, 0, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(root_obj, false, LV_PART_MAIN);
    }
    if (img_obj) {
        lv_obj_set_pos(img_obj, 0, 0);
        lv_obj_set_size(img_obj, CUSTOM_DISP_IMG_W, CUSTOM_DISP_IMG_H);
#if LVGL_VERSION_MAJOR >= 9
        lv_image_set_inner_align(img_obj, LV_IMAGE_ALIGN_STRETCH);
        lv_image_set_scale(img_obj, 256);
#else
        lv_img_set_zoom(img_obj, 256);
#endif
        lv_obj_add_flag(img_obj, LV_OBJ_FLAG_HIDDEN);
    }
    if (label_obj) {
        const int32_t time_w = 320;
        const int32_t time_h = 28;

        lv_label_set_long_mode(label_obj, LV_LABEL_LONG_CLIP);
        if (fs_rog_fonts_strix_scar_regular_18) {
            lv_obj_set_style_text_font(label_obj, fs_rog_fonts_strix_scar_regular_18, LV_PART_MAIN);
        }
        lv_obj_set_size(label_obj, time_w, time_h);
        lv_obj_align(label_obj, LV_ALIGN_BOTTOM_MID, 0, -18);
        lv_obj_set_style_text_color(label_obj, lv_color_hex(0xffffff), LV_PART_MAIN);
        lv_obj_set_style_text_align(label_obj, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_clear_flag(label_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(label_obj);
    }
}
#endif

void custom_disp_apply_splash_player(lv_obj_t *root_obj, lv_obj_t *player_obj)
{
    if (!root_obj) {
        return;
    }

    custom_disp_apply_area4_screen(root_obj);
    if (!player_obj) {
        return;
    }

#if defined(AIC_PANEL_DSI_FL7707_92IN)
    lv_aic_player_set_pivot(player_obj, 0, 0);
#if CUSTOM_DISP_IS_FL7707_ROTATE
    /* TPV_92inch on logical 1920x462 */
    lv_obj_set_pos(player_obj, 12, 11);
    lv_aic_player_set_width(player_obj, 1894);
    lv_aic_player_set_height(player_obj, 444);
#else
    lv_obj_set_pos(player_obj, 0, 0);
    lv_aic_player_set_width(player_obj, CUSTOM_DISP_IMG_W);
    lv_aic_player_set_height(player_obj, CUSTOM_DISP_IMG_H);
#endif
    lv_obj_move_background(player_obj);
#else
    (void)player_obj;
#endif
}

void custom_disp_apply_screen_backgrounds(void)
{
#if CUSTOM_DISP_IS_LVDS_PROFILE || CUSTOM_DISP_IS_BAR_PROFILE \
    || CUSTOM_DISP_IS_ROUND_PROFILE
    screen_t *splash_scr;
    screen_main_t *main_scr;
    screen_main_1_t *main_1_scr;
    screen_4_Area_t *area4_scr;

    splash_scr = screen_get(&ui_manager);
    if (splash_scr && splash_scr->obj) {
#if defined(AIC_PANEL_DSI_FL7707_92IN) && !CUSTOM_DISP_IS_FL7707_ROTATE
        custom_disp_apply_fl7707_splash_layout(splash_scr->obj, splash_scr->image_8,
                                               splash_scr->label_1);
#elif defined(AIC_PANEL_DBI_ZC_T1D8BP)
        custom_disp_apply_round_splash_layout(splash_scr->obj, splash_scr->image_8,
                                              splash_scr->label_1);
#elif defined(AIC_PANEL_DSI_R520120T01) || defined(AIC_PANEL_DSI_H0499S010) \
    || defined(AIC_PANEL_DSI_EK79007AD) || defined(AIC_DISP_LVDS)
        custom_disp_apply_r520_splash_layout(splash_scr->obj, splash_scr->image_8,
                                             splash_scr->label_1);
#else
        custom_disp_apply_splash_screen(splash_scr->obj, splash_scr->image_8);
#endif
    }

    main_scr = screen_main_get(&ui_manager);
    if (main_scr && main_scr->obj) {
        custom_disp_apply_screen_root(main_scr->obj);
        /* App icons keep their own size/pos — do not stretch as full-screen bg */
    }

    main_1_scr = screen_main_1_get(&ui_manager);
    if (main_1_scr && main_1_scr->obj) {
        custom_disp_apply_screen_root(main_1_scr->obj);
        if (main_1_scr->image_BackGround) {
            custom_disp_apply_lv_obj_image(main_1_scr->image_BackGround);
        }
    }

    area4_scr = screen_4_Area_get(&ui_manager);
    if (area4_scr && area4_scr->obj) {
        custom_disp_apply_area4_screen(area4_scr->obj);
    }
#endif
}
