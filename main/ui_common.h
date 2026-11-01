/*
 * 界面公共件：配色、字体、按钮/标签工厂、屏幕底板。
 *
 * 所有界面文件共用这些，避免 ui.c 和 ui_setup.c 各写一套颜色和字号。
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 中文 18px，工具链生成（tools/fontgen），覆盖源码里出现的全部汉字 */
LV_FONT_DECLARE(ui_font_18);

#define UI_COL_BG      lv_color_hex(0x000000)
#define UI_COL_CARD    lv_color_hex(0x121212)
#define UI_COL_CARD2   lv_color_hex(0x1C1C1C)
#define UI_COL_LINE    lv_color_hex(0x2A2A2A)
#define UI_COL_TEXT    lv_color_hex(0xF2F2F2)
#define UI_COL_MUTED   lv_color_hex(0x8A8A8A)
#define UI_COL_DIM     lv_color_hex(0x5A5A5A)
#define UI_COL_GREEN   lv_color_hex(0x22C55E)
#define UI_COL_AMBER   lv_color_hex(0xF59E0B)
#define UI_COL_RED     lv_color_hex(0xEF4444)
#define UI_COL_SKY     lv_color_hex(0x38BDF8)

#define UI_W 480
#define UI_H 480

/*
 * LVGL 9.6 起把 lv_obj_add_flag() / lv_obj_remove_flag() 标成 deprecated，
 * 改成"一个 flag 一个专用 setter"。这里包一层：新版本走 setter、老版本退回
 * flag API，两边都不产生弃用警告，也不用把版本判断散到每个界面文件里。
 */
static inline void ui_obj_scrollable(lv_obj_t *obj, bool en)
{
#if LV_VERSION_CHECK(9, 6, 0)
    lv_obj_set_scrollable(obj, en);
#else
    if (en) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
#endif
}

static inline void ui_obj_clickable(lv_obj_t *obj, bool en)
{
#if LV_VERSION_CHECK(9, 6, 0)
    lv_obj_set_clickable(obj, en);
#else
    if (en) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    }
#endif
}

/** @brief 一块干净的黑色屏幕（自动套上底色、关滚动）。 */
lv_obj_t *ui_screen_new(void);

/** @brief 标题栏：左边标题，右边返回按钮（back_cb 为 NULL 时不画）。 */
lv_obj_t *ui_header(lv_obj_t *scr, const char *title, lv_event_cb_t back_cb);

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color);

/**
 * @brief 统一样式的按钮。
 * @param primary true = 反色强调
 */
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, bool primary,
                    lv_event_cb_t cb, void *user_data);

/** @brief 一行不可点的容器（列表用的卡片底）。 */
lv_obj_t *ui_row_card(lv_obj_t *parent, int height);

/** @brief 底部轻提示，1.8 秒后自己消失。 */
void ui_toast(const char *msg);

/**
 * @brief 在屏幕顶部挂一个系统时间显示（HH:MM），全部界面统一。
 *
 * @param beside_back true = 靠右排在「返回」按钮左边（有返回键的二级页用，
 *        避免居中后和长标题撞车）；false = 顶部居中（主界面用）。
 */
void ui_clock_attach(lv_obj_t *scr, bool beside_back);

/** @brief 刷新所有已挂载的时钟文本（由 ui 的定时器驱动）。 */
void ui_clock_tick(void);

/**
 * @brief 拿显示锁（LVGL 互斥），true = 拿到了。
 *
 * 别用 bsp_display_lock() 的返回值：它把底层 esp_lv_adapter_lock() 的
 * esp_err_t 原样转成 bool，而 ESP_OK=0 表示成功 —— 成功反而变成 false。
 * 谁写 `if (bsp_display_lock(...))` 谁就死锁：拿到锁却当没拿到，
 * 跳过 unlock，LVGL 从那一刻起永远等不到锁。
 */
bool ui_lock(int timeout_ms);

/** @brief 放显示锁，和 ui_lock() 配对。 */
void ui_unlock(void);

#ifdef __cplusplus
}
#endif
