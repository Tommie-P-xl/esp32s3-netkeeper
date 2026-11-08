#include "ui_setup.h"

#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "db.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_guard.h"
#include "ui.h"
#include "ui_common.h"
#include "wifi_mgr.h"

#define MAX_AP   20
#define MAX_ROWS 24

/* ------------------------------------------------------------ 状态 */

static lv_obj_t *s_scr_wifi;
static lv_obj_t *s_list_wifi;
static lv_obj_t *s_wifi_tip;

static lv_obj_t *s_scr_pass;
static lv_obj_t *s_pass_ta;
static char s_pending_ssid[DB_SSID_LEN];

static lv_obj_t *s_scr_acct;
static lv_obj_t *s_list_acct;
static lv_obj_t *s_acct_title;   /* 固定标题「认证账号」，不拼动态内容 */
static lv_obj_t *s_acct_sub;     /* 副标题：当前 SSID（单独一行，够宽） */
static char s_acct_ssid[DB_SSID_LEN];

static lv_obj_t *s_scr_add;
static lv_obj_t *s_add_user;
static lv_obj_t *s_add_pass;

static volatile bool s_scanning;

/* 断点计数：扫描任务卡住时，靠它从网页看走到哪一步了（这个任务的串口
 * 输出不可靠，不能用日志定位）。0=空闲 1=开始扫 2=扫完 3=拿到显示锁
 * 4=清空列表 5=行建完 6=放锁 7=结束 */
static volatile int s_scan_stage;

int ui_setup_scan_stage(void)
{
    return s_scan_stage;
}

static void start_scan(void); /* 前向声明：删除按钮回调里也要刷新列表 */
static void build_pass_screen(void);
static void build_add_screen(void);

/* 行回调只带指针，所以载荷必须活得比控件久 */
static char s_row_ssid[MAX_ROWS][DB_SSID_LEN];
static lv_obj_t *s_row_connected[MAX_ROWS];
static int s_wifi_rows;
static char s_row_user[MAX_ROWS][DB_USER_LEN];
static wifi_ap_t s_aps[MAX_AP];

/* ------------------------------------------------------------ 工具 */

static void go_main(void)
{
    lv_screen_load(ui_screen_main());
}

static bool net_saved(const char *ssid)
{
    db_read_begin();
    const bool ok = db_find(ssid) != NULL;
    db_read_end();
    return ok;
}

/* -------------------------------------------------- WiFi 列表 */

static void refresh_wifi_status(lv_timer_t *timer)
{
    (void)timer;
    if (s_scr_wifi == NULL || lv_screen_active() != s_scr_wifi) {
        return;
    }
    /* 只读短临界区内的连接快照，不在显示锁里访问配置库。 */
    wifi_link_t link;
    wifi_mgr_get_link(&link);
    for (int i = 0; i < s_wifi_rows; i++) {
        ui_obj_hidden(s_row_connected[i],
                      !link.connected || strcmp(s_row_ssid[i], link.ssid) != 0);
    }
}

static void row_ap_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);

    if (net_saved(ssid)) {
        /* 已经存过：直接切过去，然后去管它的账号 */
        db_set_active_ssid(ssid);
        net_guard_trigger();
        ui_toast("正在切换网络");
        strlcpy(s_acct_ssid, ssid, sizeof(s_acct_ssid));
        ui_setup_open_accounts();
        return;
    }

    /* 新网络：先要密码 */
    strlcpy(s_pending_ssid, ssid, sizeof(s_pending_ssid));
    if (s_scr_pass == NULL) {
        build_pass_screen();
    }
    lv_textarea_set_text(s_pass_ta, "");
    lv_screen_load(s_scr_pass);
}

/* 列表行上的「删除」：把已保存的网络从库里摘掉 */
static void row_del_cb(lv_event_t *e)
{
    const char *ssid = (const char *)lv_event_get_user_data(e);
    const esp_err_t err = db_remove_net(ssid);
    if (err == ESP_OK) {
        ui_toast("已删除该网络");
        net_guard_trigger();
        start_scan();
    } else if (err == ESP_ERR_INVALID_STATE) {
        ui_toast("请先切换网络");
    } else {
        ui_toast("删除失败");
    }
}

/*
 * 扫描任务：跑在独立线程。
 *
 * 关键约束一：**不要握着显示锁去拿数据库锁**。之前这里在锁里面调 net_saved()
 * （内部要拿 db 互斥量），两个锁一嵌套就把 LVGL 任务卡死了：扫描线程占着
 * 显示锁等 db 锁，而 db 锁持有者又要出图 —— 整块屏幕从此不动。
 * 现在先在锁外把需要的数据（信号、是否已保存）算好，再进锁只做控件操作。
 *
 * 关键约束二：**锁要用 ui_lock()，不能用 bsp_display_lock() 判返回值**。
 * 后者把 esp_err_t 当 bool 返回，ESP_OK=0 成了 false —— `if (bsp_display_lock())`
 * 等于在拿到锁的时候跳过 unlock，锁被永久扣住。见 ui_common.h。
 */
static void scan_task(void *arg)
{
    (void)arg;

    const int n = wifi_mgr_scan(s_aps, MAX_AP);
    s_scan_stage = 2;

    /* 锁外预取：数据库查询 + RSSI 文案 */
    bool saved[MAX_ROWS];
    char sub[MAX_ROWS][72];
    const int rows = (n > 0) ? ((n < MAX_ROWS) ? n : MAX_ROWS) : 0;
    for (int i = 0; i < rows; i++) {
        strlcpy(s_row_ssid[i], s_aps[i].ssid, sizeof(s_row_ssid[i]));
        saved[i] = net_saved(s_row_ssid[i]);
        snprintf(sub[i], sizeof(sub[i]), "%d dBm%s%s", s_aps[i].rssi,
                 (s_aps[i].authmode == WIFI_AUTH_OPEN) ? " · 开放" : "",
                 saved[i] ? " · 已保存" : "");
    }

    if (ui_lock(2000)) {
        s_scan_stage = 3;
        if (s_list_wifi != NULL) {
            s_wifi_rows = 0;
            lv_obj_clean(s_list_wifi);
            s_scan_stage = 4;

            if (n <= 0) {
                lv_label_set_text(s_wifi_tip, n == 0 ? "没扫到 WiFi" : "扫描失败，稍后再试");
            } else {
                lv_label_set_text(s_wifi_tip, "");

                for (int i = 0; i < rows; i++) {
                    lv_obj_t *row = ui_row_card(s_list_wifi, 52);
                    ui_obj_clickable(row, true);
                    lv_obj_add_event_cb(row, row_ap_cb, LV_EVENT_CLICKED, s_row_ssid[i]);

                    lv_obj_t *name = ui_label(row, s_row_ssid[i], &ui_font_18, UI_COL_TEXT);
                    lv_obj_align(name, LV_ALIGN_LEFT_MID, 4, -8);

                    /* 这行混着中文（· 开放 / · 已保存），必须用 ui_font_18：
                     * montserrat_14 没有这些字形，会画成方框。 */
                    lv_obj_t *info = ui_label(row, sub[i], &ui_font_18, UI_COL_MUTED);
                    lv_obj_align(info, LV_ALIGN_LEFT_MID, 4, 12);

                    s_row_connected[i] = ui_label(row, "已连接", &ui_font_18, UI_COL_GREEN);
                    lv_obj_align(s_row_connected[i], LV_ALIGN_RIGHT_MID, -84, 0);
                    ui_obj_hidden(s_row_connected[i], true);

                    /* 已保存的网络给一个删除入口（需求：板上可直接删 WiFi）。
                     * 按钮可点，点击不会冒泡到整行，所以不会误触发连接。 */
                    if (saved[i]) {
                        lv_obj_t *del = ui_button(row, "删除", false, row_del_cb, s_row_ssid[i]);
                        lv_obj_set_size(del, 76, 34);
                        lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);
                    }
                }
                s_wifi_rows = rows;
                refresh_wifi_status(NULL);
            }
            s_scan_stage = 5;
        }
        ui_unlock();
        s_scan_stage = 6;
    }

    s_scanning = false;
    s_scan_stage = 7;
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    if (s_scanning) {
        return;
    }
    s_scanning = true;
    lv_label_set_text(s_wifi_tip, "扫描中…");
    s_wifi_rows = 0;
    lv_obj_clean(s_list_wifi);

    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 4, NULL) != pdPASS) {
        s_scanning = false;
        lv_label_set_text(s_wifi_tip, "任务创建失败");
    }
}

static void on_rescan(lv_event_t *e)
{
    (void)e;
    start_scan();
}

static void on_wifi_back(lv_event_t *e)
{
    (void)e;
    go_main();
}

static void on_acct_from_wifi(lv_event_t *e)
{
    (void)e;
    char active[DB_SSID_LEN] = "";
    db_active_ssid(active, sizeof(active));
    if (active[0] == '\0') {
        ui_toast("先选一个 WiFi");
        return;
    }
    strlcpy(s_acct_ssid, active, sizeof(s_acct_ssid));
    ui_setup_open_accounts();
}

static void build_wifi_screen(void)
{
    s_scr_wifi = ui_screen_new();
    ui_header(s_scr_wifi, "选择 WiFi", on_wifi_back);

    s_wifi_tip = ui_label(s_scr_wifi, "", &ui_font_18, UI_COL_MUTED);
    lv_obj_set_pos(s_wifi_tip, 26, 62);

    s_list_wifi = lv_obj_create(s_scr_wifi);
    lv_obj_set_pos(s_list_wifi, 20, 92);
    lv_obj_set_size(s_list_wifi, 440, 300);
    lv_obj_set_style_bg_opa(s_list_wifi, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_wifi, 0, 0);
    lv_obj_set_style_pad_all(s_list_wifi, 6, 0);
    lv_obj_set_style_pad_row(s_list_wifi, 8, 0);
    lv_obj_set_flex_flow(s_list_wifi, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list_wifi, LV_DIR_VER);

    lv_obj_t *b1 = ui_button(s_scr_wifi, "重新扫描", false, on_rescan, NULL);
    lv_obj_set_pos(b1, 26, 406);
    lv_obj_set_size(b1, 200, 48);

    lv_obj_t *b2 = ui_button(s_scr_wifi, "账户管理", true, on_acct_from_wifi, NULL);
    lv_obj_set_pos(b2, 254, 406);
    lv_obj_set_size(b2, 200, 48);
    lv_timer_create(refresh_wifi_status, 500, NULL);
}

void ui_setup_open_wifi(void)
{
    if (s_scr_wifi == NULL) {
        build_wifi_screen();
    }
    lv_screen_load(s_scr_wifi);
    start_scan();
}

/* -------------------------------------------------- 密码输入 */

static void pass_cancel_cb(lv_event_t *e)
{
    (void)e;
    lv_screen_load(s_scr_wifi);
}

static void do_connect_with_pass(void)
{
    const char *pass = lv_textarea_get_text(s_pass_ta);

    /* 连上之前就把这一条写进库：即使这次连不上，用户也不用重输密码。
     * 用 try 而不是直接切 active —— 连不上时保留原来能用的网络。 */
    db_ensure_net(s_pending_ssid, pass);
    net_guard_try_ssid(s_pending_ssid);

    ui_toast("正在连接");
    strlcpy(s_acct_ssid, s_pending_ssid, sizeof(s_acct_ssid));
    ui_setup_open_accounts();
}

static void pass_ok_cb(lv_event_t *e)
{
    (void)e;
    do_connect_with_pass();
}

static void kb_pass_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        do_connect_with_pass();
    } else if (code == LV_EVENT_CANCEL) {
        lv_screen_load(s_scr_wifi);
    }
}

static void build_pass_screen(void)
{
    s_scr_pass = ui_screen_new();
    ui_header(s_scr_pass, "输入 WiFi 密码", pass_cancel_cb);

    lv_obj_t *tip = ui_label(s_scr_pass, "密码", &ui_font_18, UI_COL_MUTED);
    lv_obj_set_pos(tip, 26, 66);

    s_pass_ta = lv_textarea_create(s_scr_pass);
    lv_obj_set_pos(s_pass_ta, 26, 92);
    lv_obj_set_size(s_pass_ta, 428, 52);
    lv_textarea_set_one_line(s_pass_ta, true);
    lv_textarea_set_password_mode(s_pass_ta, true);
    lv_textarea_set_placeholder_text(s_pass_ta, "开放网络留空即可");
    lv_obj_set_style_text_font(s_pass_ta, &ui_font_18, 0);
    lv_obj_set_style_bg_color(s_pass_ta, UI_COL_CARD, 0);
    lv_obj_set_style_text_color(s_pass_ta, UI_COL_TEXT, 0);
    lv_obj_set_style_border_width(s_pass_ta, 0, 0);
    lv_obj_set_style_radius(s_pass_ta, 12, 0);

    lv_obj_t *ok = ui_button(s_scr_pass, "连接", true, pass_ok_cb, NULL);
    lv_obj_set_pos(ok, 254, 160);
    lv_obj_set_size(ok, 200, 48);

    lv_obj_t *cancel = ui_button(s_scr_pass, "取消", false, pass_cancel_cb, NULL);
    lv_obj_set_pos(cancel, 26, 160);
    lv_obj_set_size(cancel, 200, 48);

    lv_obj_t *kb = lv_keyboard_create(s_scr_pass);
    lv_obj_set_size(kb, 480, 250);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_pass_ta);
    lv_obj_add_event_cb(kb, kb_pass_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(kb, UI_COL_CARD, 0);
}

/* -------------------------------------------------- 账户列表 */

static void acct_back_cb(lv_event_t *e)
{
    (void)e;
    go_main();
}

static void acct_use_cb(lv_event_t *e)
{
    const char *user = (const char *)lv_event_get_user_data(e);
    if (db_set_current_acct(s_acct_ssid, user) == ESP_OK) {
        db_set_active_ssid(s_acct_ssid);
        net_guard_trigger();
        ui_toast("已设为自动登录账号");
    } else {
        ui_toast("切换失败");
    }
}

static void acct_del_cb(lv_event_t *e)
{
    const char *user = (const char *)lv_event_get_user_data(e);
    if (db_remove_acct(s_acct_ssid, user) == ESP_OK) {
        ui_toast("已删除");
        net_guard_trigger();
        ui_setup_open_accounts();
    } else {
        ui_toast("删除失败");
    }
}

static void refresh_acct_list(void)
{
    lv_obj_clean(s_list_acct);

    /* SSID 走副标题行，标题保持固定文案 */
    lv_label_set_text(s_acct_sub, s_acct_ssid);

    if (!db_sd_present()) {
        lv_obj_t *w = ui_label(s_list_acct, "未插 SD 卡，配置只存在片上 Flash", &ui_font_18, UI_COL_MUTED);
        lv_obj_set_style_pad_bottom(w, 6, 0);
    }

    /* 先在锁外把账号列表拷出来，再进 UI。理由同 scan_task：
     * 握着 db 锁去建控件，一旦 LVGL 侧要拿锁就会互相等死。 */
    bool is_cur[MAX_ROWS];
    int n = 0;

    db_read_begin();
    {
        const db_net_t *net = db_find(s_acct_ssid);
        if (net != NULL) {
            n = (net->acct_count < MAX_ROWS) ? net->acct_count : MAX_ROWS;
            for (int i = 0; i < n; i++) {
                strlcpy(s_row_user[i], net->acct[i].user, sizeof(s_row_user[i]));
                is_cur[i] = (strcmp(net->current, net->acct[i].user) == 0) ||
                            (net->current[0] == '\0' && i == 0);
            }
        }
    }
    db_read_end();

    for (int i = 0; i < n; i++) {
        const bool cur = is_cur[i];

        lv_obj_t *row = ui_row_card(s_list_acct, 56);

        lv_obj_t *u = ui_label(row, s_row_user[i], &ui_font_18, UI_COL_TEXT);
        lv_obj_align(u, LV_ALIGN_LEFT_MID, 4, -8);

        if (cur) {
            lv_obj_t *tag = ui_label(row, "自动登录", &ui_font_18, UI_COL_GREEN);
            lv_obj_align(tag, LV_ALIGN_LEFT_MID, 4, 12);
        }

        lv_obj_t *use = ui_button(row, cur ? "当前" : "使用", cur, acct_use_cb, s_row_user[i]);
        lv_obj_set_size(use, 88, 36);
        lv_obj_align(use, LV_ALIGN_RIGHT_MID, -104, 0);

        lv_obj_t *del = ui_button(row, "删除", false, acct_del_cb, s_row_user[i]);
        lv_obj_set_size(del, 88, 36);
        lv_obj_align(del, LV_ALIGN_RIGHT_MID, -4, 0);
    }

    if (n == 0) {
        lv_obj_t *empty = ui_label(s_list_acct, "这里还没有账号，点下面的「添加账号」", &ui_font_18, UI_COL_MUTED);
        lv_obj_set_style_pad_top(empty, 8, 0);
    }
}

static void on_add_acct(lv_event_t *e)
{
    (void)e;
    if (s_scr_add == NULL) {
        build_add_screen(); /* 懒建：之前忘了调用，按钮点了没反应 */
    }
    lv_textarea_set_text(s_add_user, "");
    lv_textarea_set_text(s_add_pass, "");
    lv_screen_load(s_scr_add);
}

static void build_acct_screen(void)
{
    s_scr_acct = ui_screen_new();
    s_acct_title = ui_header(s_scr_acct, "认证账号", acct_back_cb);

    /* SSID 单独一行显示，不要拼进标题：标题行还要放时钟和返回键，
     * SSID 长短不一，拼进去迟早互相遮挡。这一行有 428px 宽，
     * 完整的 SSID（最长 32 字节）一定放得下。 */
    s_acct_sub = ui_label(s_scr_acct, "", &ui_font_18, UI_COL_MUTED);
    lv_obj_set_pos(s_acct_sub, 26, 44);
    lv_obj_set_width(s_acct_sub, 428);
    lv_label_set_long_mode(s_acct_sub, LV_LABEL_LONG_DOT);

    s_list_acct = lv_obj_create(s_scr_acct);
    lv_obj_set_pos(s_list_acct, 20, 70);
    lv_obj_set_size(s_list_acct, 440, 322);
    lv_obj_set_style_bg_opa(s_list_acct, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_acct, 0, 0);
    lv_obj_set_style_pad_all(s_list_acct, 6, 0);
    lv_obj_set_style_pad_row(s_list_acct, 8, 0);
    lv_obj_set_flex_flow(s_list_acct, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list_acct, LV_DIR_VER);

    lv_obj_t *add = ui_button(s_scr_acct, "添加账号", true, on_add_acct, NULL);
    lv_obj_set_pos(add, 26, 406);
    lv_obj_set_size(add, 200, 48);

    lv_obj_t *back = ui_button(s_scr_acct, "回到主页", false, acct_back_cb, NULL);
    lv_obj_set_pos(back, 254, 406);
    lv_obj_set_size(back, 200, 48);
}

void ui_setup_open_accounts(void)
{
    if (s_acct_ssid[0] == '\0') {
        db_active_ssid(s_acct_ssid, sizeof(s_acct_ssid));
    }
    if (s_acct_ssid[0] == '\0') {
        ui_toast("先选一个 WiFi");
        ui_setup_open_wifi();
        return;
    }

    if (s_scr_acct == NULL) {
        build_acct_screen();
    }
    refresh_acct_list();
    lv_screen_load(s_scr_acct);
}

/* -------------------------------------------------- 新增账号 */

static void add_cancel_cb(lv_event_t *e)
{
    (void)e;
    ui_setup_open_accounts();
}

static void do_add_acct(void)
{
    const char *user = lv_textarea_get_text(s_add_user);
    const char *pass = lv_textarea_get_text(s_add_pass);

    if (user[0] == '\0') {
        ui_toast("请填写账号");
        return;
    }

    if (db_upsert_acct(s_acct_ssid, user, pass) != ESP_OK) {
        ui_toast("保存失败（已达上限或 SD 只读）");
        return;
    }
    db_set_current_acct(s_acct_ssid, user);
    db_set_active_ssid(s_acct_ssid);
    net_guard_trigger();

    ui_toast("账号已保存");
    ui_setup_open_accounts();
}

static void add_ok_cb(lv_event_t *e)
{
    (void)e;
    do_add_acct();
}

static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_user_data(e);
    lv_keyboard_set_textarea(kb, lv_event_get_target(e));
}

static void kb_add_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        do_add_acct();
    } else if (code == LV_EVENT_CANCEL) {
        ui_setup_open_accounts();
    }
}

static void build_add_screen(void)
{
    s_scr_add = ui_screen_new();
    ui_header(s_scr_add, "添加账号", add_cancel_cb);

    /* 键盘固定占底部 250px（顶边 y=230）。所有输入控件和按钮必须排在
     * y<230 内，否则会被键盘盖住（之前按钮 y=218 起，正好压在键盘下）。 */
    lv_obj_t *kb = lv_keyboard_create(s_scr_add);
    lv_obj_set_size(kb, 480, 250);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(kb, kb_add_cb, LV_EVENT_ALL, NULL);
    lv_obj_set_style_bg_color(kb, UI_COL_CARD, 0);

    lv_obj_t *t1 = ui_label(s_scr_add, "账号", &ui_font_18, UI_COL_MUTED);
    lv_obj_set_pos(t1, 26, 54);

    s_add_user = lv_textarea_create(s_scr_add);
    lv_obj_set_pos(s_add_user, 26, 76);
    lv_obj_set_size(s_add_user, 428, 40);
    lv_textarea_set_one_line(s_add_user, true);
    lv_textarea_set_placeholder_text(s_add_user, "学号 / 手机号");
    lv_obj_set_style_text_font(s_add_user, &ui_font_18, 0);
    lv_obj_set_style_bg_color(s_add_user, UI_COL_CARD, 0);
    lv_obj_set_style_text_color(s_add_user, UI_COL_TEXT, 0);
    lv_obj_set_style_border_width(s_add_user, 0, 0);
    lv_obj_set_style_radius(s_add_user, 12, 0);
    lv_obj_add_event_cb(s_add_user, ta_focus_cb, LV_EVENT_CLICKED, kb);

    lv_obj_t *t2 = ui_label(s_scr_add, "密码", &ui_font_18, UI_COL_MUTED);
    lv_obj_set_pos(t2, 26, 122);

    s_add_pass = lv_textarea_create(s_scr_add);
    lv_obj_set_pos(s_add_pass, 26, 144);
    lv_obj_set_size(s_add_pass, 428, 40);
    lv_textarea_set_one_line(s_add_pass, true);
    lv_textarea_set_password_mode(s_add_pass, true);
    lv_textarea_set_placeholder_text(s_add_pass, "校园网密码");
    lv_obj_set_style_text_font(s_add_pass, &ui_font_18, 0);
    lv_obj_set_style_bg_color(s_add_pass, UI_COL_CARD, 0);
    lv_obj_set_style_text_color(s_add_pass, UI_COL_TEXT, 0);
    lv_obj_set_style_border_width(s_add_pass, 0, 0);
    lv_obj_set_style_radius(s_add_pass, 12, 0);
    lv_obj_add_event_cb(s_add_pass, ta_focus_cb, LV_EVENT_CLICKED, kb);

    /* 按钮底边 190+36=226，距键盘顶 4px */
    lv_obj_t *ok = ui_button(s_scr_add, "保存", true, add_ok_cb, NULL);
    lv_obj_set_pos(ok, 254, 190);
    lv_obj_set_size(ok, 200, 36);

    lv_obj_t *cancel = ui_button(s_scr_add, "取消", false, add_cancel_cb, NULL);
    lv_obj_set_pos(cancel, 26, 190);
    lv_obj_set_size(cancel, 200, 36);

    lv_keyboard_set_textarea(kb, s_add_user);
}

#if APP_UI_CAPTURE_SETUP
/* 抓图用：跳过真实点击流程直接进输入页，配 dump_fb 出截图 */
void ui_setup_cap_pass(void)
{
    strlcpy(s_pending_ssid, "ExampleWiFi", sizeof(s_pending_ssid));
    if (s_scr_pass == NULL) {
        build_pass_screen();
    }
    lv_textarea_set_text(s_pass_ta, "");
    lv_screen_load(s_scr_pass);
}

void ui_setup_cap_add(void)
{
    if (s_scr_add == NULL) {
        build_add_screen();
    }
    lv_screen_load(s_scr_add);
}
#endif
