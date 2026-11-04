/*
 * Copyright (C) 2026 ArtInChip Technology Co., Ltd.
 *
 * screen_main: Android-like app launcher grid → USB vender IN notify.
 * Long-press edit: jiggle + drag with empty-slot insert; order saved to /data.
 */

#include "ui_objects.h"
#include "aic_ui.h"
#include "ui_util.h"
#include "custom/custom_disp_profile.h"
#include <rtconfig.h>
#ifdef KERNEL_RTTHREAD
#include <rtthread.h>
#endif
#include "usbd_vender.h"
#include "lv_port_indev.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/*
 * Android-style launcher — full-screen 5×2 tile on logical UI (e.g. 1280×720):
 *  - each cell = screen_w/5 × screen_h/2 (icons sized to fill cell)
 *  - long-press → edit (jiggle) + pick up icon
 *  - drag → insert into slot (others shift); drop to commit
 *  - tap wallpaper → exit edit
 */
#define SCREEN_MAIN_MARGIN_X        0   /* full-bleed grid */
#define SCREEN_MAIN_MARGIN_Y        0
#define SCREEN_MAIN_COLS            5
#define SCREEN_MAIN_ROWS            2
#define SCREEN_MAIN_SLOTS         (SCREEN_MAIN_COLS * SCREEN_MAIN_ROWS)
#define SCREEN_MAIN_LABEL_H        22
#define SCREEN_MAIN_LABEL_GAP       4
#define SCREEN_MAIN_CELL_PAD       12
#define SCREEN_MAIN_ICON_RATIO     82   /* icon side vs max square in cell */

#define SCREEN_MAIN_PRESS_SCALE   230
#define SCREEN_MAIN_PRESS_RECOLOR_OPA LV_OPA_40
#define SCREEN_MAIN_PRESS_SINK    4

#define SCREEN_MAIN_JIGGLE_DEG     45   /* unused when jiggle disabled */
#define SCREEN_MAIN_JIGGLE_MS     140
#define SCREEN_MAIN_SHIFT_MS      140
#define SCREEN_MAIN_DRAG_TIMER_MS  33 /* H0499 GE2D full-frame rotate cannot hold 16 ms */
#define SCREEN_MAIN_DRAG_SLOP       8   /* px: edit-mode press+move → start drag */
#define SCREEN_MAIN_DRAG_TIMEOUT_MS 30000 /* emergency if UP never arrives */
#define SCREEN_MAIN_DRAG_RELEASE_MS  80 /* ignore brief false UP mid-drag */
/* H0499: release sits in the gap (y≈336 down / y≈383 up) vs split at 360. */
#define SCREEN_MAIN_ROW_HYST_PX      80
/* WORD/FILES sit on the physical Y clamp (~80px of travel). */
#define SCREEN_MAIN_EDGE_FIRST_PX    40
#define SCREEN_MAIN_USE_JIGGLE        0 /* 0: avoid GE/I2C overload during edit */
#define SCREEN_MAIN_DONE_W        120
#define SCREEN_MAIN_DONE_H         48
#define SCREEN_MAIN_EMPTY         ((int8_t)-1)

#define SCREEN_MAIN_ORDER_PATH    "/data/screen_main_order.bin"
#define SCREEN_MAIN_ORDER_MAGIC   0x324D5301u /* 'SM02' 5×2 layout */

typedef struct {
    const char *img_path;
    const char *title;
    const uint8_t *pkt;
    const char *log_name;
} screen_main_app_t;

typedef struct {
    lv_obj_t *cell;
    lv_obj_t *img;
    lv_obj_t *lab;
    int8_t app_id;
    int8_t slot;
} screen_main_icon_t;

static const uint8_t s_pkt_line[9]       = { 0xFA, 0xFB, 0xFC, 0x00, 0x00, 0x00, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_outlook[9]    = { 0xFA, 0xFB, 0xFC, 0x01, 0x01, 0x01, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_excel[9]      = { 0xFA, 0xFB, 0xFC, 0x02, 0x02, 0x02, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_powerpoint[9] = { 0xFA, 0xFB, 0xFC, 0x03, 0x03, 0x03, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_word[9]       = { 0xFA, 0xFB, 0xFC, 0x04, 0x04, 0x04, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_teams[9]      = { 0xFA, 0xFB, 0xFC, 0x05, 0x05, 0x05, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_chrome[9]     = { 0xFA, 0xFB, 0xFC, 0x06, 0x06, 0x06, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_edge[9]       = { 0xFA, 0xFB, 0xFC, 0x07, 0x07, 0x07, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_wechat[9]     = { 0xFA, 0xFB, 0xFC, 0x08, 0x08, 0x08, 0xEC, 0xEB, 0xEA };
static const uint8_t s_pkt_files[9]      = { 0xFA, 0xFB, 0xFC, 0x09, 0x09, 0x09, 0xEC, 0xEB, 0xEA };

static const screen_main_app_t s_apps[] = {
    { LVGL_IMAGE_PATH(line_2335324.png),        "LINE",       s_pkt_line,       "LINE" },
    { LVGL_IMAGE_PATH(application_15238349.png),"Outlook",    s_pkt_outlook,    "OUTLOOK" },
    { LVGL_IMAGE_PATH(file_12959566.png),       "Excel",      s_pkt_excel,      "EXCEL" },
    { LVGL_IMAGE_PATH(paper_12959487.png),      "PowerPoint", s_pkt_powerpoint, "PPT" },
    { LVGL_IMAGE_PATH(paper_12959478.png),      "Word",       s_pkt_word,       "WORD" },
    { LVGL_IMAGE_PATH(teams_906349.png),        "Teams",      s_pkt_teams,      "TEAMS" },
    { LVGL_IMAGE_PATH(chrome_356007.png),       "Chrome",     s_pkt_chrome,     "CHROME" },
    { LVGL_IMAGE_PATH(microsoft_5968890.png),   "Edge",       s_pkt_edge,       "EDGE" },
    { LVGL_IMAGE_PATH(wechat_3992638.png),      "WeChat",     s_pkt_wechat,     "WECHAT" },
    { LVGL_IMAGE_PATH(folder_14970903.png),     "Files",      s_pkt_files,      "FILES" },
};

#define SCREEN_MAIN_APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

static screen_main_icon_t s_icons[SCREEN_MAIN_APP_COUNT];
static int8_t s_order[SCREEN_MAIN_SLOTS];
static int8_t s_order_drag_base[SCREEN_MAIN_SLOTS];
static int32_t s_cell_w;
static int32_t s_cell_h;
static int32_t s_icon_sz;
static int32_t s_row_gap;
static lv_obj_t *s_scr_obj;
static bool s_edit_mode;
static bool s_dragging;
static int8_t s_drag_app_id = SCREEN_MAIN_EMPTY;
static int8_t s_drag_from_slot = SCREEN_MAIN_EMPTY;
static int8_t s_hover_slot = SCREEN_MAIN_EMPTY;
static int32_t s_drag_grab_x;
static int32_t s_drag_grab_y;
static int32_t s_drag_origin_cx;
static bool s_order_loaded;
static lv_indev_t *s_drag_indev;
static lv_timer_t *s_drag_timer;
static lv_obj_t *s_done_btn;
static int8_t s_edit_arm_app = SCREEN_MAIN_EMPTY;
static int32_t s_edit_press_x;
static int32_t s_edit_press_y;
static uint32_t s_drag_start_tick;
static uint32_t s_drag_up_tick; /* 0 = finger seen down */

static void screen_main_apply_layout(bool animate, int8_t skip_app_id);
static void screen_main_set_edit_mode(bool enable);
static void screen_main_save_order(void);
static void screen_main_set_icon_pressed(screen_main_icon_t *ic, bool pressed);
static void screen_main_drag_end(bool commit);
static void screen_main_drag_follow(void);
static void screen_main_drag_preview(int8_t hover);
static void screen_main_set_peers_clickable(bool enable);
static void screen_main_edit_arm_clear(void);
static bool screen_main_edit_arm_moved(void);
static void screen_main_done_show(bool show);
static void screen_main_stop_all_jiggle(void);
static void screen_main_try_exit_edit_from_tap(void);

static void screen_main_default_order(void)
{
    int i;

    for (i = 0; i < SCREEN_MAIN_SLOTS; i++)
        s_order[i] = (i < SCREEN_MAIN_APP_COUNT) ? (int8_t)i : SCREEN_MAIN_EMPTY;
}

static bool screen_main_order_valid(const int8_t order[SCREEN_MAIN_SLOTS])
{
    bool seen[SCREEN_MAIN_APP_COUNT];
    int i;
    int filled = 0;

    memset(seen, 0, sizeof(seen));
    for (i = 0; i < SCREEN_MAIN_SLOTS; i++) {
        int8_t id = order[i];
        if (id == SCREEN_MAIN_EMPTY)
            continue;
        if (id < 0 || id >= SCREEN_MAIN_APP_COUNT || seen[id])
            return false;
        seen[id] = true;
        filled++;
    }
    return filled == SCREEN_MAIN_APP_COUNT;
}

static void screen_main_load_order(void)
{
    FILE *fp;
    uint32_t magic = 0;
    int8_t tmp[SCREEN_MAIN_SLOTS];

    if (s_order_loaded)
        return;

    screen_main_default_order();
    fp = fopen(SCREEN_MAIN_ORDER_PATH, "rb");
    if (fp) {
        if (fread(&magic, 1, sizeof(magic), fp) == sizeof(magic) &&
            magic == SCREEN_MAIN_ORDER_MAGIC &&
            fread(tmp, 1, sizeof(tmp), fp) == sizeof(tmp) &&
            screen_main_order_valid(tmp)) {
            memcpy(s_order, tmp, sizeof(s_order));
#ifdef KERNEL_RTTHREAD
            rt_kprintf("screen_main: loaded order from %s\n", SCREEN_MAIN_ORDER_PATH);
#endif
        }
        fclose(fp);
    }
#ifdef KERNEL_RTTHREAD
    {
        int i;

        rt_kprintf("screen_main: order");
        for (i = 0; i < SCREEN_MAIN_SLOTS; i++) {
            int8_t id = s_order[i];

            if (id >= 0 && id < SCREEN_MAIN_APP_COUNT)
                rt_kprintf(" %s", s_apps[id].log_name);
            else
                rt_kprintf(" -");
        }
        rt_kprintf("\n");
    }
#endif
    s_order_loaded = true;
}

static void screen_main_save_order(void)
{
    FILE *fp;
    uint32_t magic = SCREEN_MAIN_ORDER_MAGIC;

    if (!screen_main_order_valid(s_order))
        return;

    fp = fopen(SCREEN_MAIN_ORDER_PATH, "wb");
    if (!fp) {
#ifdef KERNEL_RTTHREAD
        rt_kprintf("screen_main: save order failed (%s)\n", SCREEN_MAIN_ORDER_PATH);
#endif
        return;
    }
    fwrite(&magic, 1, sizeof(magic), fp);
    fwrite(s_order, 1, sizeof(s_order), fp);
    fclose(fp);
#ifdef KERNEL_RTTHREAD
    rt_kprintf("screen_main: order saved\n");
#endif
}

static void screen_main_slot_xy(int8_t slot, int32_t *x, int32_t *y)
{
    int32_t col = slot % SCREEN_MAIN_COLS;
    int32_t row = slot / SCREEN_MAIN_COLS;

    /* Full-screen tile: cells abut (5×2 covers W×H). */
    *x = SCREEN_MAIN_MARGIN_X + col * s_cell_w;
    *y = SCREEN_MAIN_MARGIN_Y + row * s_cell_h;
    (void)s_row_gap;
}

static int8_t screen_main_slot_at_point(int32_t x, int32_t y)
{
    int32_t col;
    int32_t row;
    int from_col;
    int from_row;

    if (s_cell_w <= 0 || s_cell_h <= 0)
        return 0;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    col = x / s_cell_w;
    row = y / s_cell_h;
    if (col >= SCREEN_MAIN_COLS)
        col = SCREEN_MAIN_COLS - 1;
    if (row >= SCREEN_MAIN_ROWS)
        row = SCREEN_MAIN_ROWS - 1;

    if (!s_dragging || s_drag_from_slot < 0)
        return (int8_t)(row * SCREEN_MAIN_COLS + col);

    from_col = s_drag_from_slot % SCREEN_MAIN_COLS;
    from_row = s_drag_from_slot / SCREEN_MAIN_COLS;

    /* Cross-row: the finger is released in the visual gap, ~20px short
     * of the hard split. Shift the boundary toward the start row. */
    if (from_row == 0 && y >= (s_cell_h - SCREEN_MAIN_ROW_HYST_PX))
        row = 1;
    else if (from_row == 1 && y < (s_cell_h + SCREEN_MAIN_ROW_HYST_PX))
        row = 0;

    /* Rightmost column: physical Y clamp leaves ~80px of travel, not a
     * full 256px cell. 40px toward the neighbour changes slot; further
     * travel steps by ~2/3 cell. Left 4 columns keep the absolute grid. */
    if (from_col == (SCREEN_MAIN_COLS - 1)) {
        int32_t dx = x - s_drag_origin_cx;
        int32_t next_px = (s_cell_w * 2) / 3;
        int steps;

        if (next_px < SCREEN_MAIN_EDGE_FIRST_PX)
            next_px = SCREEN_MAIN_EDGE_FIRST_PX;

        if (dx < 0) {
            int32_t adx = -dx;

            if (adx >= SCREEN_MAIN_EDGE_FIRST_PX) {
                steps = 1 + (adx - SCREEN_MAIN_EDGE_FIRST_PX) / next_px;
                col = from_col - steps;
                if (col < 0)
                    col = 0;
            }
        }
    }

    return (int8_t)(row * SCREEN_MAIN_COLS + col);
}

static int8_t screen_main_find_slot_of_app(int8_t app_id)
{
    int i;

    for (i = 0; i < SCREEN_MAIN_SLOTS; i++) {
        if (s_order[i] == app_id)
            return (int8_t)i;
    }
    return SCREEN_MAIN_EMPTY;
}

static screen_main_icon_t *screen_main_icon_by_app(int8_t app_id)
{
    if (app_id < 0 || app_id >= SCREEN_MAIN_APP_COUNT)
        return NULL;
    return &s_icons[app_id];
}

/* Move item from `from` to `to` with insert (shift), empty slots included. */
static void screen_main_insert_move(int8_t order[SCREEN_MAIN_SLOTS], int8_t from, int8_t to)
{
    int8_t moving;
    int i;

    if (from < 0 || to < 0 || from >= SCREEN_MAIN_SLOTS || to >= SCREEN_MAIN_SLOTS || from == to)
        return;

    moving = order[from];
    if (from < to) {
        for (i = from; i < to; i++)
            order[i] = order[i + 1];
        order[to] = moving;
    } else {
        for (i = from; i > to; i--)
            order[i] = order[i - 1];
        order[to] = moving;
    }
}

static void screen_main_send_notify(const uint8_t pkt[9], const char *name)
{
    int ret = usbd_vender_send_ack_bytes(pkt, 9);
#ifdef KERNEL_RTTHREAD
    rt_kprintf("screen_main: %s press IN=0x%02x ret=%d %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
               name, usbd_vender_get_in_ep(), ret,
               pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6], pkt[7], pkt[8]);
#else
    (void)name;
    (void)ret;
#endif
}

static void screen_main_anim_set_x(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

static void screen_main_anim_set_y(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void screen_main_anim_set_rot(void *var, int32_t v)
{
    lv_obj_set_style_transform_rotation((lv_obj_t *)var, v, LV_PART_MAIN);
}

static void screen_main_stop_jiggle(lv_obj_t *cell)
{
    lv_anim_delete(cell, screen_main_anim_set_rot);
    lv_obj_set_style_transform_rotation(cell, 0, LV_PART_MAIN);
}

static void screen_main_start_jiggle(lv_obj_t *cell, uint32_t delay_ms)
{
#if !SCREEN_MAIN_USE_JIGGLE
    (void)cell;
    (void)delay_ms;
    return;
#else
    lv_anim_t a;

    screen_main_stop_jiggle(cell);
    lv_obj_set_style_transform_pivot_x(cell, s_cell_w / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(cell, s_cell_h / 2, LV_PART_MAIN);

    lv_anim_init(&a);
    lv_anim_set_var(&a, cell);
    lv_anim_set_values(&a, -SCREEN_MAIN_JIGGLE_DEG, SCREEN_MAIN_JIGGLE_DEG);
    lv_anim_set_time(&a, SCREEN_MAIN_JIGGLE_MS);
    lv_anim_set_playback_time(&a, SCREEN_MAIN_JIGGLE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, screen_main_anim_set_rot);
    lv_anim_start(&a);
#endif
}

static void screen_main_move_cell(lv_obj_t *cell, int32_t x, int32_t y, bool animate)
{
    lv_anim_delete(cell, screen_main_anim_set_x);
    lv_anim_delete(cell, screen_main_anim_set_y);

    if (!animate) {
        lv_obj_set_pos(cell, x, y);
        return;
    }

    {
        lv_anim_t ax;
        lv_anim_t ay;

        lv_anim_init(&ax);
        lv_anim_set_var(&ax, cell);
        lv_anim_set_values(&ax, lv_obj_get_x(cell), x);
        lv_anim_set_time(&ax, SCREEN_MAIN_SHIFT_MS);
        lv_anim_set_path_cb(&ax, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&ax, screen_main_anim_set_x);
        lv_anim_start(&ax);

        lv_anim_init(&ay);
        lv_anim_set_var(&ay, cell);
        lv_anim_set_values(&ay, lv_obj_get_y(cell), y);
        lv_anim_set_time(&ay, SCREEN_MAIN_SHIFT_MS);
        lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&ay, screen_main_anim_set_y);
        lv_anim_start(&ay);
    }
}

static void screen_main_apply_layout(bool animate, int8_t skip_app_id)
{
    int i;

    for (i = 0; i < SCREEN_MAIN_SLOTS; i++) {
        int8_t app_id = s_order[i];
        screen_main_icon_t *ic;
        int32_t x;
        int32_t y;

        if (app_id == SCREEN_MAIN_EMPTY || app_id == skip_app_id)
            continue;

        ic = screen_main_icon_by_app(app_id);
        if (!ic || !ic->cell)
            continue;

        ic->slot = (int8_t)i;
        screen_main_slot_xy((int8_t)i, &x, &y);
        screen_main_move_cell(ic->cell, x, y, animate);
    }
}

static void screen_main_set_clickable(lv_obj_t *obj, bool enable)
{
    if (!obj)
        return;
    if (enable)
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    else
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

static void screen_main_set_peers_clickable(bool enable)
{
    int i;

    for (i = 0; i < SCREEN_MAIN_APP_COUNT; i++) {
        if (!s_icons[i].cell)
            continue;
        /* Keep the dragged icon hittable; peer images used to steal the gesture. */
        if (!enable && s_dragging && (int8_t)i == s_drag_app_id)
            continue;
        screen_main_set_clickable(s_icons[i].cell, enable);
        /* Image stays non-clickable; otherwise it steals the press from the cell. */
        if (!enable)
            screen_main_set_clickable(s_icons[i].img, false);
    }
    if (!enable)
        screen_main_set_clickable(s_done_btn, false);
    else if (s_edit_mode)
        screen_main_set_clickable(s_done_btn, true);
}

static void screen_main_drag_follow(void)
{
    screen_main_icon_t *ic;
    lv_point_t p;
    int32_t cx;
    int32_t cy;

    if (!s_dragging)
        return;

    ic = screen_main_icon_by_app(s_drag_app_id);
    if (!ic || !ic->cell)
        return;

    if (!s_drag_indev) {
        s_drag_indev = lv_indev_get_next(NULL);
        while (s_drag_indev && lv_indev_get_type(s_drag_indev) != LV_INDEV_TYPE_POINTER)
            s_drag_indev = lv_indev_get_next(s_drag_indev);
    }
    if (!s_drag_indev)
        return;

    /* Keep following after PRESS_LOST. Commit only after a real, sustained UP.
     * Use rotated driver coords (same space as widgets). Raw last_x/y is
     * still portrait FB space and would slide the icon off the finger.
     * Do not 4s-timeout while the finger is still down — that dropped the
     * icon mid-drag. Emergency timeout only if still pressed for 30s. */
    {
        int pressed = 0;

        aic_touch_get_last_lvgl(&p, &pressed);
        if (pressed &&
            lv_tick_elaps(s_drag_start_tick) >= SCREEN_MAIN_DRAG_TIMEOUT_MS) {
#ifdef KERNEL_RTTHREAD
            rt_kprintf("screen_main: drag timeout — force end/exit edit\n");
#endif
            screen_main_drag_end(true);
            screen_main_set_edit_mode(false);
            if (s_drag_indev)
                lv_indev_reset(s_drag_indev, NULL);
            return;
        }
        if (pressed) {
            /* A new contact after a brief UP must be near the icon.
             * WORD at y=1279 often UP then a far DOWN; 80ms resume
             * teleported the icon across the screen. */
            if (s_drag_up_tick != 0) {
                int32_t ex = lv_obj_get_x(ic->cell) + s_drag_grab_x;
                int32_t ey = lv_obj_get_y(ic->cell) + s_drag_grab_y;
                int32_t dx = p.x - ex;
                int32_t dy = p.y - ey;

                if ((dx * dx + dy * dy) > (300 * 300)) {
                    screen_main_drag_end(true);
                    return;
                }
            }
            s_drag_up_tick = 0;
            /* indev re-applies PRESSED every cycle; strip it so press-scale
             * cannot run during GE2D rotate (icon leaves the caption). */
            lv_obj_remove_state(ic->cell, LV_STATE_PRESSED);
            if (ic->img)
                lv_obj_remove_state(ic->img, LV_STATE_PRESSED);
        } else {
            if (s_drag_up_tick == 0)
                s_drag_up_tick = lv_tick_get();
            if (lv_tick_elaps(s_drag_up_tick) >= SCREEN_MAIN_DRAG_RELEASE_MS) {
                screen_main_drag_end(true);
                return;
            }
            return;
        }
    }
    {
        int32_t nx = p.x - s_drag_grab_x;
        int32_t ny = p.y - s_drag_grab_y;

        if (nx != lv_obj_get_x(ic->cell) || ny != lv_obj_get_y(ic->cell))
            lv_obj_set_pos(ic->cell, nx, ny);

        cx = nx + s_cell_w / 2;
        cy = ny + s_cell_h / 2;
    }
    screen_main_drag_preview(screen_main_slot_at_point(cx, cy));
}

static void screen_main_drag_timer_cb(lv_timer_t *t)
{
    (void)t;
    screen_main_drag_follow();
}

static void screen_main_stop_all_jiggle(void)
{
    int i;

    for (i = 0; i < SCREEN_MAIN_APP_COUNT; i++) {
        if (s_icons[i].cell)
            screen_main_stop_jiggle(s_icons[i].cell);
    }
}

static void screen_main_done_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code != LV_EVENT_CLICKED && code != LV_EVENT_SHORT_CLICKED)
        return;
    if (s_dragging)
        screen_main_drag_end(true);
    screen_main_edit_arm_clear();
    screen_main_set_edit_mode(false);
}

static void screen_main_done_show(bool show)
{
    if (!s_done_btn)
        return;
    if (show) {
        lv_obj_clear_flag(s_done_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_done_btn, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_foreground(s_done_btn);
    } else {
        lv_obj_add_flag(s_done_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void screen_main_create_done_btn(lv_obj_t *parent)
{
    lv_obj_t *lab;

    s_done_btn = lv_obj_create(parent);
    lv_obj_set_size(s_done_btn, SCREEN_MAIN_DONE_W, SCREEN_MAIN_DONE_H);
    lv_obj_align(s_done_btn, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_style_bg_color(s_done_btn, lv_color_hex(0x1A1A1A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_done_btn, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_done_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_done_btn, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_done_btn, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_radius(s_done_btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_done_btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_done_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_done_btn, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_done_btn, screen_main_done_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_done_btn, screen_main_done_event_cb, LV_EVENT_SHORT_CLICKED, NULL);

    lab = lv_label_create(s_done_btn);
    lv_label_set_text(lab, "Done");
    lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    if (fs_rog_fonts_strix_scar_regular_18)
        lv_obj_set_style_text_font(lab, fs_rog_fonts_strix_scar_regular_18, LV_PART_MAIN);
    else if (fs_rog_fonts_strix_scar_regular_36)
        lv_obj_set_style_text_font(lab, fs_rog_fonts_strix_scar_regular_36, LV_PART_MAIN);
    lv_obj_center(lab);
    lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
}

static void screen_main_set_edit_mode(bool enable)
{
    int i;

    if (s_edit_mode == enable)
        return;

    s_edit_mode = enable;
    if (!enable)
        screen_main_edit_arm_clear();
    for (i = 0; i < SCREEN_MAIN_APP_COUNT; i++) {
        if (!s_icons[i].cell)
            continue;
        if (enable && !s_dragging)
            screen_main_start_jiggle(s_icons[i].cell, (uint32_t)(i * 17u));
        else
            screen_main_stop_jiggle(s_icons[i].cell);
        /* Edit cue without transform anim (jiggle off): slight dim. */
        lv_obj_set_style_opa(s_icons[i].cell,
                             enable ? LV_OPA_80 : LV_OPA_COVER, LV_PART_MAIN);
    }
    screen_main_done_show(enable);

#ifdef KERNEL_RTTHREAD
    rt_kprintf("screen_main: edit_mode=%d\n", (int)enable);
#endif
}

static void screen_main_drag_begin(int8_t app_id)
{
    screen_main_icon_t *ic = screen_main_icon_by_app(app_id);
    lv_point_t p;

    if (!ic || !ic->cell || s_dragging)
        return;

    screen_main_edit_arm_clear();

    s_drag_from_slot = screen_main_find_slot_of_app(app_id);
    if (s_drag_from_slot < 0)
        return;

    memcpy(s_order_drag_base, s_order, sizeof(s_order));
    s_drag_app_id = app_id;
    s_hover_slot = s_drag_from_slot;
    s_dragging = true;
    s_drag_start_tick = lv_tick_get();
    s_drag_up_tick = 0;

    s_drag_indev = lv_indev_active();
    if (!s_drag_indev || lv_indev_get_type(s_drag_indev) != LV_INDEV_TYPE_POINTER) {
        s_drag_indev = lv_indev_get_next(NULL);
        while (s_drag_indev && lv_indev_get_type(s_drag_indev) != LV_INDEV_TYPE_POINTER)
            s_drag_indev = lv_indev_get_next(s_drag_indev);
    }

    {
        int pressed = 0;

        aic_touch_get_last_lvgl(&p, &pressed);
        if (!pressed) {
            if (s_drag_indev)
                lv_indev_get_point(s_drag_indev, &p);
            else {
                p.x = lv_obj_get_x(ic->cell) + s_cell_w / 2;
                p.y = lv_obj_get_y(ic->cell) + s_cell_h / 2;
            }
        }
    }
        /* Phone-style: keep the press offset so the icon stays under the
         * fingertip instead of snapping to the cell centre. */
        s_drag_grab_x = p.x - lv_obj_get_x(ic->cell);
        s_drag_grab_y = p.y - lv_obj_get_y(ic->cell);
        s_drag_origin_cx = lv_obj_get_x(ic->cell) + s_cell_w / 2;

    /* Pause jiggle + peer hits so crossing other icons cannot steal the gesture. */
    screen_main_stop_all_jiggle();
    screen_main_set_peers_clickable(false);
    screen_main_set_icon_pressed(ic, false);
    if (ic->lab)
        lv_obj_add_flag(ic->lab, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(ic->cell, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_PRESS_LOCK);
    if (ic->img)
        lv_obj_add_flag(ic->img, LV_OBJ_FLAG_PRESS_LOCK);
    /* Bigger than the screen: finger never leaves the hit-test of this tile. */
    lv_obj_set_ext_click_area(ic->cell, (s_cell_w > s_cell_h ? s_cell_w : s_cell_h) * SCREEN_MAIN_COLS);
    /* No scale/rotate transform while dragging — H0499 flush already GE2D-rotates
     * the full frame; extra transform made follow much hitchier than R520. */
    lv_obj_set_style_opa(ic->cell, LV_OPA_90, LV_PART_MAIN);
    screen_main_done_show(true);
    /* Icon above Done / peers so hit-test cannot jump to another tile. */
    lv_obj_move_foreground(ic->cell);

    if (!s_drag_timer)
        s_drag_timer = lv_timer_create(screen_main_drag_timer_cb, SCREEN_MAIN_DRAG_TIMER_MS, NULL);
    else
        lv_timer_resume(s_drag_timer);
    lv_timer_reset(s_drag_timer);

    if (!s_edit_mode)
        screen_main_set_edit_mode(true);

#ifdef KERNEL_RTTHREAD
    rt_kprintf("screen_main: drag begin app=%d from=%d\n", (int)app_id, (int)s_drag_from_slot);
#endif
}

static void screen_main_drag_preview(int8_t hover)
{
    if (!s_dragging || hover < 0 || hover >= SCREEN_MAIN_SLOTS)
        return;
    if (hover == s_hover_slot)
        return;

    /* Remember the drop slot only. Live-shifting peers made it look like
     * the gesture had jumped to another icon. */
#ifdef KERNEL_RTTHREAD
    rt_kprintf("screen_main: hover %d -> %d\n", (int)s_hover_slot, (int)hover);
#endif
    s_hover_slot = hover;
}

static void screen_main_drag_end(bool commit)
{
    screen_main_icon_t *ic;
    int8_t ended_app = s_drag_app_id;

    if (!s_dragging)
        return;

    if (s_drag_timer)
        lv_timer_pause(s_drag_timer);

    ic = screen_main_icon_by_app(s_drag_app_id);
    if (ic && ic->cell) {
        lv_obj_clear_flag(ic->cell, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_PRESS_LOCK);
        if (ic->img)
            lv_obj_clear_flag(ic->img, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_set_ext_click_area(ic->cell, 0);
        lv_obj_set_style_opa(ic->cell, s_edit_mode ? LV_OPA_80 : LV_OPA_COVER, LV_PART_MAIN);
        if (ic->lab)
            lv_obj_clear_flag(ic->lab, LV_OBJ_FLAG_HIDDEN);
    }

    if (!commit) {
        memcpy(s_order, s_order_drag_base, sizeof(s_order));
    } else {
        memcpy(s_order, s_order_drag_base, sizeof(s_order));
        screen_main_insert_move(s_order, s_drag_from_slot, s_hover_slot);
        screen_main_save_order();
    }

    s_dragging = false;
    s_drag_app_id = SCREEN_MAIN_EMPTY;
    s_drag_from_slot = SCREEN_MAIN_EMPTY;
    s_hover_slot = SCREEN_MAIN_EMPTY;
    s_drag_indev = NULL;
    screen_main_set_peers_clickable(true);
    screen_main_apply_layout(true, SCREEN_MAIN_EMPTY);

    if (s_edit_mode) {
        int i;
        for (i = 0; i < SCREEN_MAIN_APP_COUNT; i++) {
            if (s_icons[i].cell)
                screen_main_start_jiggle(s_icons[i].cell, (uint32_t)(i * 17u));
        }
        screen_main_done_show(true);
    }

#ifdef KERNEL_RTTHREAD
    rt_kprintf("screen_main: drag end app=%d commit=%d\n", (int)ended_app, (int)commit);
#else
    (void)ended_app;
#endif
}

static void screen_main_bg_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED) {
        if (s_dragging)
            return; /* Timer commits after a sustained UP; a 1-sample UP is ignored. */
        else if (s_edit_mode && s_edit_arm_app >= 0 && !screen_main_edit_arm_moved())
            screen_main_try_exit_edit_from_tap();
        return;
    }

    if (code == LV_EVENT_CLICKED || code == LV_EVENT_SHORT_CLICKED) {
        if (s_dragging)
            return;
        if (s_edit_mode)
            screen_main_try_exit_edit_from_tap();
    }
}

static void screen_main_set_icon_pressed(screen_main_icon_t *ic, bool pressed)
{
    if (!ic || !ic->img)
        return;
    if (pressed)
        lv_obj_add_state(ic->img, LV_STATE_PRESSED);
    else
        lv_obj_remove_state(ic->img, LV_STATE_PRESSED);
}

static void screen_main_edit_arm_clear(void)
{
    s_edit_arm_app = SCREEN_MAIN_EMPTY;
}

static void screen_main_edit_arm_press(int8_t app_id)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;

    s_edit_arm_app = app_id;
    if (indev) {
        lv_indev_get_point(indev, &p);
        s_edit_press_x = p.x;
        s_edit_press_y = p.y;
    } else {
        s_edit_press_x = 0;
        s_edit_press_y = 0;
    }
}

static bool screen_main_edit_arm_moved(void)
{
    lv_indev_t *indev;
    lv_point_t p;
    int32_t dx;
    int32_t dy;

    if (s_edit_arm_app < 0)
        return false;

    indev = lv_indev_active();
    if (!indev)
        return false;
    lv_indev_get_point(indev, &p);
    dx = p.x - s_edit_press_x;
    dy = p.y - s_edit_press_y;
    if (dx < 0)
        dx = -dx;
    if (dy < 0)
        dy = -dy;
    return (dx >= SCREEN_MAIN_DRAG_SLOP) || (dy >= SCREEN_MAIN_DRAG_SLOP);
}

static void screen_main_try_exit_edit_from_tap(void)
{
    if (s_dragging || !s_edit_mode)
        return;
    screen_main_edit_arm_clear();
    screen_main_set_edit_mode(false);
}

static void screen_main_icon_event_cb(lv_event_t *e)
{
    screen_main_icon_t *ic = lv_event_get_user_data(e);
    lv_event_code_t code;
    const screen_main_app_t *app;
    bool armed_here;

    if (!ic)
        return;

    /* Another tile must not arm / pick up while this drag is active. */
    if (s_dragging && ic->app_id != s_drag_app_id)
        return;

    code = lv_event_get_code(e);
    app = &s_apps[ic->app_id];

    if (code == LV_EVENT_SHORT_CLICKED) {
        if (s_dragging)
            return;
        if (s_edit_mode) {
            screen_main_try_exit_edit_from_tap();
            return;
        }
        /* Already sent on PRESSED; keep this as fallback if press was lost. */
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        if (s_edit_mode && !s_dragging) {
            /* Freeze jiggle so hit-test / click delivery stays reliable. */
            screen_main_stop_all_jiggle();
            screen_main_set_icon_pressed(ic, false);
            screen_main_edit_arm_press(ic->app_id);
            screen_main_done_show(true);
        } else if (!s_dragging) {
            screen_main_set_icon_pressed(ic, true);
            /* Send on press — waiting for SHORT_CLICKED fails if INT stays low. */
            screen_main_send_notify(app->pkt, app->log_name);
        }
        return;
    }

    if (code == LV_EVENT_LONG_PRESSED) {
        screen_main_set_icon_pressed(ic, false);
        screen_main_edit_arm_clear();
        if (!s_dragging)
            screen_main_drag_begin(ic->app_id);
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (s_dragging && s_drag_app_id == ic->app_id) {
            screen_main_drag_follow();
            return;
        }
        if (s_edit_mode && !s_dragging && s_edit_arm_app == ic->app_id &&
            screen_main_edit_arm_moved()) {
            screen_main_edit_arm_clear();
            screen_main_drag_begin(ic->app_id);
            screen_main_drag_follow();
        }
        return;
    }

    if (code == LV_EVENT_RELEASED) {
        screen_main_set_icon_pressed(ic, false);
        armed_here = (s_edit_arm_app == ic->app_id);
        if (s_dragging && s_drag_app_id == ic->app_id) {
            screen_main_edit_arm_clear();
            return; /* Timer commits after a sustained UP. */
        }
        /*
         * Do not rely on SHORT_CLICKED alone — jiggle / PRESS_LOST often swallows it.
         * Tap without moving past slop → exit edit.
         */
        if (s_edit_mode && armed_here && !screen_main_edit_arm_moved()) {
            screen_main_try_exit_edit_from_tap();
            return;
        }
        if (s_edit_mode && !s_dragging) {
            /* Press aborted oddly: resume jiggle, keep Done visible. */
            int i;
            screen_main_edit_arm_clear();
            for (i = 0; i < SCREEN_MAIN_APP_COUNT; i++) {
                if (s_icons[i].cell)
                    screen_main_start_jiggle(s_icons[i].cell, (uint32_t)(i * 17u));
            }
            screen_main_done_show(true);
        } else {
            screen_main_edit_arm_clear();
        }
        return;
    }

    if (code == LV_EVENT_PRESS_LOST) {
        screen_main_set_icon_pressed(ic, false);
        /* Keep arm — RELEASED may arrive on bg/Done; jiggle already frozen. */
        return;
    }
}

static void screen_main_layout_metrics(int32_t *cell_w, int32_t *cell_h,
                                      int32_t *icon_sz, int32_t *row_gap)
{
    int32_t scr_w = (int32_t)CUSTOM_DISP_IMG_W;
    int32_t scr_h = (int32_t)CUSTOM_DISP_IMG_H;
    int32_t usable_w;
    int32_t usable_h;
    int32_t side_w;
    int32_t side_h;
    int32_t side;

    /* Tile the whole logical screen (e.g. 1280×720 → cell 256×360). */
    usable_w = scr_w - SCREEN_MAIN_MARGIN_X * 2;
    usable_h = scr_h - SCREEN_MAIN_MARGIN_Y * 2;
    if (usable_w < SCREEN_MAIN_COLS)
        usable_w = SCREEN_MAIN_COLS;
    if (usable_h < SCREEN_MAIN_ROWS)
        usable_h = SCREEN_MAIN_ROWS;

    *cell_w = usable_w / SCREEN_MAIN_COLS;
    *cell_h = usable_h / SCREEN_MAIN_ROWS;
    *row_gap = 0;

    side_w = *cell_w - SCREEN_MAIN_CELL_PAD * 2;
    side_h = *cell_h - SCREEN_MAIN_CELL_PAD * 2
             - SCREEN_MAIN_LABEL_GAP - SCREEN_MAIN_LABEL_H;
    if (side_w < 48)
        side_w = 48;
    if (side_h < 48)
        side_h = 48;
    side = (side_w < side_h) ? side_w : side_h;
    side = (side * SCREEN_MAIN_ICON_RATIO) / 100;
    if (side < 48)
        side = 48;
    *icon_sz = side;
}

static void screen_main_style_press_fx(lv_obj_t *img, int32_t icon_sz)
{
    (void)icon_sz;
#if !defined(AIC_PANEL_DSI_H0499S010)
    lv_obj_set_style_transform_pivot_x(img, icon_sz / 2, LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(img, icon_sz / 2, LV_PART_MAIN);

#if LVGL_VERSION_MAJOR >= 9
    lv_obj_set_style_transform_scale_x(img, SCREEN_MAIN_PRESS_SCALE, LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale_y(img, SCREEN_MAIN_PRESS_SCALE, LV_STATE_PRESSED);
#else
    lv_obj_set_style_transform_zoom(img, SCREEN_MAIN_PRESS_SCALE, LV_STATE_PRESSED);
#endif
    lv_obj_set_style_translate_y(img, SCREEN_MAIN_PRESS_SINK, LV_STATE_PRESSED);
#endif
    lv_obj_set_style_image_recolor(img, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_image_recolor_opa(img, SCREEN_MAIN_PRESS_RECOLOR_OPA, LV_STATE_PRESSED);
}

static void screen_main_create_icon(lv_obj_t *parent, int8_t app_id, int8_t slot)
{
    const screen_main_app_t *app = &s_apps[app_id];
    screen_main_icon_t *ic = &s_icons[app_id];
    int32_t cell_x;
    int32_t cell_y;
    int32_t ix;
    int32_t iy;

    screen_main_slot_xy(slot, &cell_x, &cell_y);

    ic->app_id = app_id;
    ic->slot = slot;
    ic->cell = lv_obj_create(parent);
    lv_obj_set_pos(ic->cell, cell_x, cell_y);
    lv_obj_set_size(ic->cell, s_cell_w, s_cell_h);
    lv_obj_set_style_bg_opa(ic->cell, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ic->cell, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ic->cell, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(ic->cell, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ic->cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ic->cell, LV_OBJ_FLAG_OVERFLOW_VISIBLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ic->cell, screen_main_icon_event_cb, LV_EVENT_ALL, ic);

    ix = (s_cell_w - s_icon_sz) / 2;
    /* Vertically center icon in the area above the caption. */
    {
        int32_t content_h = s_cell_h - SCREEN_MAIN_LABEL_H - SCREEN_MAIN_LABEL_GAP;
        iy = (content_h - s_icon_sz) / 2;
        if (iy < SCREEN_MAIN_CELL_PAD)
            iy = SCREEN_MAIN_CELL_PAD;
    }

    ic->img = lv_img_create(ic->cell);
    lv_img_set_src(ic->img, app->img_path);
    lv_obj_set_pos(ic->img, ix, iy);
    lv_obj_set_size(ic->img, s_icon_sz, s_icon_sz);
#if LVGL_VERSION_MAJOR >= 9
    lv_image_set_inner_align(ic->img, LV_IMAGE_ALIGN_STRETCH);
    lv_image_set_scale(ic->img, LV_SCALE_NONE);
#else
    lv_img_set_zoom(ic->img, 256);
#endif
    lv_obj_add_flag(ic->img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(ic->img, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    screen_main_style_press_fx(ic->img, s_icon_sz);

    ic->lab = lv_label_create(ic->cell);
    lv_label_set_text(ic->lab, app->title);
    lv_label_set_long_mode(ic->lab, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ic->lab, s_cell_w - 4);
    lv_obj_set_style_text_align(ic->lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic->lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    if (fs_rog_fonts_strix_scar_regular_18)
        lv_obj_set_style_text_font(ic->lab, fs_rog_fonts_strix_scar_regular_18, LV_PART_MAIN);
    else if (fs_rog_fonts_strix_scar_regular_36)
        lv_obj_set_style_text_font(ic->lab, fs_rog_fonts_strix_scar_regular_36, LV_PART_MAIN);
    lv_obj_align(ic->lab, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_clear_flag(ic->lab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ic->lab, LV_OBJ_FLAG_EVENT_BUBBLE);
}

void screen_main_create(ui_manager_t *ui)
{
    screen_main_t *scr = screen_main_get(ui);
    int i;
    lv_obj_t **slots[SCREEN_MAIN_APP_COUNT];

    if (!ui->auto_del && scr->obj) {
        return;
    }

    s_scr_obj = NULL;
    s_edit_mode = false;
    s_dragging = false;
    s_drag_app_id = SCREEN_MAIN_EMPTY;
    s_drag_from_slot = SCREEN_MAIN_EMPTY;
    s_hover_slot = SCREEN_MAIN_EMPTY;
    s_drag_indev = NULL;
    s_edit_arm_app = SCREEN_MAIN_EMPTY;
    s_done_btn = NULL;
    if (s_drag_timer) {
        lv_timer_delete(s_drag_timer);
        s_drag_timer = NULL;
    }
    memset(s_icons, 0, sizeof(s_icons));
    screen_main_load_order();

    scr->obj = lv_obj_create(NULL);
    s_scr_obj = scr->obj;
    lv_obj_set_scrollbar_mode(scr->obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr->obj, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scr->obj, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_clear_flag(scr->obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr->obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr->obj, screen_main_bg_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr->obj, screen_main_bg_event_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(scr->obj, screen_main_bg_event_cb, LV_EVENT_RELEASED, NULL);

    slots[0] = &scr->image_line;
    slots[1] = &scr->image_outlook;
    slots[2] = &scr->image_excel;
    slots[3] = &scr->image_powerpoint;
    slots[4] = &scr->image_word;
    slots[5] = &scr->image_teams;
    slots[6] = &scr->image_chrome;
    slots[7] = &scr->image_edge;
    slots[8] = &scr->image_wechat;
    slots[9] = &scr->image_files;

    screen_main_layout_metrics(&s_cell_w, &s_cell_h, &s_icon_sz, &s_row_gap);

    for (i = 0; i < SCREEN_MAIN_SLOTS; i++) {
        int8_t app_id = s_order[i];
        if (app_id == SCREEN_MAIN_EMPTY)
            continue;
        screen_main_create_icon(scr->obj, app_id, (int8_t)i);
        if (app_id < SCREEN_MAIN_APP_COUNT && slots[app_id])
            *slots[app_id] = s_icons[app_id].img;
    }

    screen_main_create_done_btn(scr->obj);
}
