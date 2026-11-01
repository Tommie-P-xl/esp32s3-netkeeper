#include "ui_common.h"

#include <stdio.h>

#include "esp_err.h"
#include "esp_lv_adapter.h"
#include "sys_time.h"

static lv_obj_t *s_toast;

/* 顶部时钟：每块屏一个 label，tick 统一刷新 */
#define MAX_CLOCKS 8
static lv_obj_t *s_clocks[MAX_CLOCKS];
static int s_clock_n;

void ui_clock_attach(lv_obj_t *scr, bool beside_back)
{
    if (s_clock_n >= MAX_CLOCKS) {
        return;
    }
    lv_obj_t *c = ui_label(scr, "--:--", &ui_font_18, UI_COL_MUTED);
    if (beside_back) {
        /* 返回按钮在 TOP_RIGHT -22（宽 72），时间排在它左边留 10px 间隙 */
        lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -104, 20);
    } else {
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 20);
    }
    s_clocks[s_clock_n++] = c;
    ui_clock_tick();
}

void ui_clock_tick(void)
{
    char buf[8];
    sys_time_str(buf, sizeof(buf));
    for (int i = 0; i < s_clock_n; i++) {
        if (s_clocks[i] != NULL) {
            lv_label_set_text(s_clocks[i], buf);
        }
    }
}

bool ui_lock(int timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;
}

void ui_unlock(void)
{
    esp_lv_adapter_unlock();
}

lv_obj_t *ui_screen_new(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static void back_btn_cb(lv_event_t *e)
{
    lv_event_cb_t cb = (lv_event_cb_t)lv_event_get_user_data(e);
    if (cb != NULL) {
        cb(e);
    }
}

lv_obj_t *ui_header(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *t = ui_label(scr, title, &ui_font_18, UI_COL_TEXT);
    lv_obj_set_pos(t, 26, 18);
    /* 标题可能很长（如「认证账号 · 某个很长的SSID」），超宽就省略，
     * 否则会一路压到时钟和返回按钮上。 */
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, (back_cb != NULL) ? 250 : UI_W - 52);

    ui_clock_attach(scr, back_cb != NULL);

    if (back_cb != NULL) {
        lv_obj_t *b = lv_button_create(scr);
        lv_obj_set_size(b, 72, 34);
        lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -22, 12);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_set_style_bg_color(b, UI_COL_CARD2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_add_event_cb(b, back_btn_cb, LV_EVENT_CLICKED, (void *)back_cb);

        lv_obj_t *bl = ui_label(b, "返回", &ui_font_18, UI_COL_TEXT);
        lv_obj_center(bl);
    }
    return t;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, bool primary,
                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, primary ? UI_COL_TEXT : UI_COL_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);

    lv_obj_t *l = ui_label(b, text, &ui_font_18, primary ? UI_COL_BG : UI_COL_TEXT);
    lv_obj_center(l);

    if (cb != NULL) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    }
    return b;
}

lv_obj_t *ui_row_card(lv_obj_t *parent, int height)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, height);
    lv_obj_set_style_bg_color(row, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static void toast_timer_cb(lv_timer_t *t)
{
    lv_obj_t *box = (lv_obj_t *)lv_timer_get_user_data(t);
    if (box != NULL) {
        lv_obj_delete(box);
    }
    if (s_toast == box) {
        s_toast = NULL;
    }
    lv_timer_delete(t);
}

void ui_toast(const char *msg)
{
    lv_obj_t *scr = lv_screen_active();

    if (s_toast != NULL && lv_obj_is_valid(s_toast)) {
        lv_obj_delete(s_toast);
    }

    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_style_bg_color(box, UI_COL_TEXT, 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 20, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_hor(box, 18, 0);
    lv_obj_set_style_pad_ver(box, 8, 0);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(box, LV_ALIGN_BOTTOM_MID, 0, -92);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = ui_label(box, msg, &ui_font_18, UI_COL_BG);
    lv_obj_center(l);

    s_toast = box;
    lv_timer_create(toast_timer_cb, 1800, box);
}
