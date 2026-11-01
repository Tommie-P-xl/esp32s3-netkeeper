#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "batt_mgr.h"
#include "db.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_guard.h"
#include "power_mgr.h"
#include "ui_common.h"
#include "ui_setup.h"
#include "web.h"

static const char *TAG = "ui";

typedef struct {
    lv_obj_t *scr;
    lv_obj_t *dot;
    lv_obj_t *state_big;
    lv_obj_t *state_cn;
    lv_obj_t *v_ssid;
    lv_obj_t *v_ip;
    lv_obj_t *v_auth;
    lv_obj_t *v_next;
    lv_obj_t *v_uptime;
    lv_obj_t *v_code;      /* 网页验证码 */
    lv_obj_t *v_code_ttl;  /* 验证码倒计时 */
    lv_obj_t *v_bat;       /* 电池：插了电池才显示，没插整行隐藏 */
    lv_obj_t *msg;
} ui_refs_t;

static ui_refs_t s_ui;
static int64_t s_last_off_ms;
static volatile uint32_t s_ticks;

uint32_t ui_tick_count(void)
{
    return s_ticks;
}

static void fmt_dur(char *out, size_t cap, int64_t seconds)
{
    if (seconds < 0) {
        seconds = 0;
    }
    const int64_t h = seconds / 3600;
    const int64_t m = (seconds % 3600) / 60;
    const int64_t s = seconds % 60;

    if (h > 0) {
        snprintf(out, cap, "%lld:%02lld:%02lld", (long long)h, (long long)m, (long long)s);
    } else {
        snprintf(out, cap, "%02lld:%02lld", (long long)m, (long long)s);
    }
}

static void fmt_state(net_state_t st, const char **big, const char **cn, lv_color_t *col)
{
    switch (st) {
    case NET_ONLINE:
        *big = "ONLINE"; *cn = "外网正常"; *col = UI_COL_GREEN; break;
    case NET_NEED_AUTH:
        *big = "AUTH"; *cn = "需要认证"; *col = UI_COL_AMBER; break;
    case NET_AUTHING:
        *big = "LOGIN"; *cn = "正在登录"; *col = UI_COL_SKY; break;
    case NET_AUTH_FAILED:
        *big = "FAILED"; *cn = "认证失败"; *col = UI_COL_RED; break;
    case NET_CHECKING:
        *big = "CHECK"; *cn = "检测外网中"; *col = UI_COL_SKY; break;
    case NET_WIFI_CONNECTING:
        *big = "LINKING"; *cn = "正在连接 WiFi"; *col = UI_COL_SKY; break;
    case NET_WIFI_DOWN:
        *big = "NO WIFI"; *cn = "WiFi 未连接"; *col = UI_COL_RED; break;
    case NET_NO_ACCOUNT:
        *big = "NO ACCT"; *cn = "缺少认证账号"; *col = UI_COL_AMBER; break;
    case NET_NEED_SETUP:
        *big = "SETUP"; *cn = "请先选择 WiFi"; *col = UI_COL_AMBER; break;
    case NET_ERROR:
        *big = "OFFLINE"; *cn = "外网不可达"; *col = UI_COL_RED; break;
    case NET_BOOT:
    default:
        *big = "BOOT"; *cn = "启动中"; *col = UI_COL_SKY; break;
    }
}

/* y 是绝对坐标；k 左对齐，返回值右对齐 */
static lv_obj_t *make_row(lv_obj_t *scr, int y, const char *k)
{
    lv_obj_t *lk = ui_label(scr, k, &ui_font_18, UI_COL_MUTED);
    lv_obj_set_pos(lk, 26, y);

    lv_obj_t *lv = ui_label(scr, "-", &ui_font_18, UI_COL_TEXT);
    lv_label_set_long_mode(lv, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lv, 300);
    lv_obj_set_style_text_align(lv, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lv, LV_ALIGN_TOP_RIGHT, -26, y);
    return lv;
}

/* --------------------------------------------------------------- 按钮 */

static void on_check(lv_event_t *e)
{
    (void)e;
    net_guard_trigger();
    ui_toast("已请求立即检测");
}

static void on_wifi(lv_event_t *e)
{
    (void)e;
    ui_setup_open_wifi();
}

static void on_acct(lv_event_t *e)
{
    (void)e;
    ui_setup_open_accounts();
}

/* --------------------------------------------------------------- 刷新 */

static void ui_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_ticks++;
    ui_clock_tick();

    /* 心跳：LVGL 任务如果卡死，这条会停。排查冻结类问题时最有用。
     * 用 fwrite 而不是 ESP_LOG —— 实测 LVGL 任务里的 ESP_LOG 一行都出不来，
     * 换成直接写 stdout 才能确认这个任务到底还活着没有。 */
    static uint32_t tick;
    if ((++tick % 10) == 0) {
        char hb[16];
        const int n = snprintf(hb, sizeof(hb), "T%u\n", (unsigned)tick / 10);
        fwrite(hb, 1, (size_t)n, stdout);
        fflush(stdout);
    }

    const int64_t now = esp_timer_get_time() / 1000;

    /* 熄屏期间 5 秒刷一次就够 */
    if (!power_mgr_screen_is_on() && (now - s_last_off_ms) < 5000) {
        return;
    }
    s_last_off_ms = now;

    net_status_t st;
    net_guard_get(&st);

    const char *big = "";
    const char *cn = "";
    lv_color_t col = UI_COL_SKY;
    fmt_state(st.state, &big, &cn, &col);

    lv_label_set_text(s_ui.state_big, big);
    lv_label_set_text(s_ui.state_cn, cn);
    lv_obj_set_style_bg_color(s_ui.dot, col, 0);

    /* 优先显示连着的那个，没连上就显示目标 */
    lv_label_set_text(s_ui.v_ssid, st.ssid[0] ? st.ssid : (st.target[0] ? st.target : "-"));

    /* IP 行直接给出完整配置地址，照着敲就行 */
    if (st.ip[0]) {
        char url[48];
        snprintf(url, sizeof(url), "http://%s", st.ip);
        lv_label_set_text(s_ui.v_ip, url);
    } else {
        lv_label_set_text(s_ui.v_ip, "未联网");
    }
    lv_label_set_text_fmt(s_ui.v_auth, "%d 次", st.auth_count);

    if (st.next_check_ms > now) {
        char buf[16];
        fmt_dur(buf, sizeof(buf), (st.next_check_ms - now) / 1000);
        lv_label_set_text(s_ui.v_next, buf);
    } else {
        lv_label_set_text(s_ui.v_next, "--:--");
    }

    {
        char buf[16];
        fmt_dur(buf, sizeof(buf), (now - st.boot_ms) / 1000);
        lv_label_set_text(s_ui.v_uptime, buf);
    }

    /* 电池：没插电池整行不显示；插着 USB 只报百分比 + 充电状态（不估续航） */
    {
        batt_info_t bat;
        batt_mgr_get(&bat);

        if (!bat.present) {
            ui_obj_hidden(s_ui.v_bat, true);
        } else {
            char buf[40];
            if (bat.usb) {
                snprintf(buf, sizeof(buf), "电池 %d%% · %s", bat.pct,
                         bat.full ? "已充满" : "充电中");
                lv_obj_set_style_text_color(s_ui.v_bat, UI_COL_GREEN, 0);
            } else {
                const lv_color_t col = (bat.pct <= 15) ? UI_COL_RED : UI_COL_SKY;
                if (bat.minutes_left < 0) {
                    snprintf(buf, sizeof(buf), "电池 %d%%", bat.pct);
                } else if (bat.minutes_left >= 60) {
                    snprintf(buf, sizeof(buf), "电池 %d%% · %dh%02dm", bat.pct,
                             bat.minutes_left / 60, bat.minutes_left % 60);
                } else {
                    snprintf(buf, sizeof(buf), "电池 %d%% · %dm", bat.pct, bat.minutes_left);
                }
                lv_obj_set_style_text_color(s_ui.v_bat, col, 0);
            }
            /* 文本没变就别刷，否则每次 tick 都会触发一次重绘 */
            if (strcmp(lv_label_get_text(s_ui.v_bat), buf) != 0) {
                lv_label_set_text(s_ui.v_bat, buf);
            }
            ui_obj_hidden(s_ui.v_bat, false);
        }
    }

    /* 网页验证码：5 分钟换一次，防同网段的人误改配置 */
    {
        char code[7];
        web_code_get(code);
        char pretty[8];
        snprintf(pretty, sizeof(pretty), "%.3s %.3s", code, code + 3);
        lv_label_set_text(s_ui.v_code, pretty);

        const int64_t left_s = web_code_remain_ms() / 1000;
        char ttl[24];
        snprintf(ttl, sizeof(ttl), "剩余 %02lld:%02lld",
                 (long long)(left_s / 60), (long long)(left_s % 60));
        lv_label_set_text(s_ui.v_code_ttl, ttl);
    }

    lv_label_set_text(s_ui.msg, st.detail);
    lv_obj_set_style_text_color(s_ui.msg, (st.state == NET_ONLINE) ? UI_COL_MUTED : col, 0);
}

/* ---------------------------------------------------------- 像素自检 */

#if APP_UI_SNAPSHOT_PROBE
static void snapshot_probe(lv_timer_t *t)
{
    lv_timer_delete(t);

    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (snap == NULL) {
        ESP_LOGW(TAG, "snapshot failed");
        return;
    }

    const uint16_t *px = (const uint16_t *)snap->data;
    const uint32_t stride = snap->header.stride / 2;
    const int w = (int)snap->header.w;
    const int h = (int)snap->header.h;

    static const int bounds[] = {0, 50, 190, 360, 400, 480};
    const int bands = (int)(sizeof(bounds) / sizeof(bounds[0])) - 1;

    for (int b = 0; b < bands; b++) {
        int lit = 0;
        const int y0 = bounds[b] < h ? bounds[b] : h;
        const int y1 = bounds[b + 1] < h ? bounds[b + 1] : h;
        for (int y = y0; y < y1; y++) {
            for (int x = 0; x < w; x++) {
                if (px[y * stride + x] != 0) {
                    lit++;
                }
            }
        }
        const int total = (y1 - y0) * w;
        if (total > 0) {
            ESP_LOGI(TAG, "band y%03d-%03d : %.1f%% lit (%d/%d)",
                     y0, y1, 100.0f * lit / total, lit, total);
        }
    }

    lv_draw_buf_destroy(snap);
}
#endif

/* ---------------------------------------------------------- 显存导出 */

/*
 * 调试用：把整屏按 RGB565 以十六进制逐行打到串口，电脑端还原成 PNG。
 * 板子没有截图能力，这是"确认界面真的长对了"的唯一硬办法。
 * 只在 APP_UI_DUMP_FB=1 时编译。
 */
#if APP_UI_DUMP_FB
/* 串口发送部分单独跑在任务里。
 *
 * 为什么拆：整屏 480x480 要传 ~1MB，按 USB CDC 的实际吞吐要 90 秒以上。
 * 如果直接在 lv_timer 回调里发，LVGL 任务会被占住这么久——表现就是界面冻住、
 * 时钟和倒计时都不走、连熄屏都推迟（第一次抓 README 截图时就踩过）。
 * 现在 LVGL 线程只做一次快照（几十毫秒），发送交给下面这个任务。 */
static bool s_dump_busy;

static void dump_writer_task(void *arg)
{
    lv_draw_buf_t *snap = (lv_draw_buf_t *)arg;

    const uint16_t *px = (const uint16_t *)snap->data;
    const uint32_t stride = snap->header.stride / 2;
    const int w = (int)snap->header.w;
    const int h = (int)snap->header.h;

    /* APP_UI_DUMP_STEP=2 时 2x2 取平均降到 1/4 数据量（约 30 秒/屏，够看布局）；
     * =1 时原分辨率 480x480（README 截图用，串口要传 ~1MB，慢）。 */
    const int step = (APP_UI_DUMP_STEP >= 1) ? APP_UI_DUMP_STEP : 1;
    const int dw = w / step;
    const int dh = h / step;

    const int64_t t0 = esp_timer_get_time();
    printf("FBDUMP %d %d\n", dw, dh);
    fflush(stdout);

    /* 攒够 8 行再一次性写：USB CDC 按行小包写实测只有 ~8KB/s，
     * 批量写能快好几倍，否则原分辨率一屏要传两分钟。 */
    enum { ROWS_PER_BATCH = 8 };
    static char batch[ROWS_PER_BATCH * (4 * 480 + 16)];
    static char line[4 * 480 + 8];
    int bo = 0;
    int in_batch = 0;

    for (int y = 0; y < dh; y++) {
        int o = snprintf(line, sizeof(line), "R%03d ", y);
        for (int x = 0; x < dw; x++) {
            uint32_t v;
            if (step == 1) {
                v = px[y * stride + x];
            } else {
                uint32_t sum = 0;
                for (int dy = 0; dy < step; dy++) {
                    for (int dx = 0; dx < step; dx++) {
                        sum += px[(y * step + dy) * stride + x * step + dx];
                    }
                }
                v = sum / (uint32_t)(step * step);
            }
            snprintf(&line[o], 5, "%04x", v);
            o += 4;
        }
        line[o++] = '\n';

        memcpy(batch + bo, line, (size_t)o);
        bo += o;
        in_batch++;

        if (in_batch >= ROWS_PER_BATCH || y == dh - 1) {
            fwrite(batch, 1, (size_t)bo, stdout);
            fflush(stdout);
            /* 必须让出一下：USB CDC 没有流控，主机来不及读就整块丢掉，
             * 表现是日志和 dump 一起消失。 */
            vTaskDelay(2);
            bo = 0;
            in_batch = 0;
        }
    }
    ESP_LOGI(TAG, "dump framebuffer done, %dx%d in %lld ms",
             dw, dh, (esp_timer_get_time() - t0) / 1000);
    printf("FBDUMP END\n");
    fflush(stdout);

    lv_draw_buf_destroy(snap);
    s_dump_busy = false;
    vTaskDelete(NULL);
}

/* 在 LVGL 线程里只做快照，几十毫秒就返回 */
static void dump_fb_now(void)
{
    if (s_dump_busy) {
        ESP_LOGW(TAG, "previous dump still running, skip");
        return;
    }

    /* 抓图前先逼一次刷新：快照会触发一次离屏渲染，如果此时面板的 QSPI 刷新
     * 还在传输中，两层 SPI 事务会撞在一起，实测偶发
     * `assert failed: spi_device_release_bus`（板子直接复位，一整个抓图序列报废）。
     * lv_refr_now + 短暂等待能把在途的刷新收干净，之后再快照。 */
    lv_refr_now(NULL);
    vTaskDelay(pdMS_TO_TICKS(30));

    /* 必须先建缓冲再自己清零：lv_snapshot_take() 不保证缓冲区初始化，
     * 拿到的是未清零的堆内存（实测整屏 0xf7be），画出来的图完全是假的。 */
    lv_draw_buf_t *snap =
        lv_snapshot_create_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (snap == NULL) {
        ESP_LOGW(TAG, "snapshot buffer alloc failed");
        return;
    }
    memset(snap->data, 0, snap->data_size);
    if (lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, snap) !=
        LV_RESULT_OK) {
        ESP_LOGW(TAG, "snapshot failed");
        lv_draw_buf_destroy(snap);
        return;
    }

    ESP_LOGI(TAG, "dump framebuffer start");
    s_dump_busy = true;
    if (xTaskCreate(dump_writer_task, "fbdump", 4096, snap, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "dump task create failed");
        s_dump_busy = false;
        lv_draw_buf_destroy(snap);
    }
}

static void dump_fb(lv_timer_t *t)
{
    lv_timer_delete(t);
    dump_fb_now();
}

/* 抓图脚本用：依次切到各界面各截一张。
 * 只有 APP_UI_CAPTURE_SETUP=1 时才编进来。 */
#if APP_UI_CAPTURE_SETUP
static void cap_open_wifi(lv_timer_t *t)
{
    lv_timer_delete(t);
    ui_setup_open_wifi();
}

static void cap_open_acct(lv_timer_t *t)
{
    lv_timer_delete(t);
    ui_setup_open_accounts();
}

static void cap_open_pass(lv_timer_t *t)
{
    lv_timer_delete(t);
    ui_setup_cap_pass();
}

static void cap_open_add(lv_timer_t *t)
{
    lv_timer_delete(t);
    ui_setup_cap_add();
}
#endif
#endif

/* --------------------------------------------------------------- 创建 */

lv_obj_t *ui_screen_main(void)
{
    return s_ui.scr;
}

void ui_create(lv_display_t *disp)
{
    ESP_LOGI(TAG, "screen %dx%d",
             lv_display_get_horizontal_resolution(disp),
             lv_display_get_vertical_resolution(disp));

    lv_obj_t *scr = ui_screen_new();
    s_ui.scr = scr;
    /* ---------------- 标题 ---------------- */
    lv_obj_t *title = ui_label(scr, "校园网守护", &ui_font_18, UI_COL_TEXT);
    lv_obj_set_pos(title, 26, 18);

    ui_clock_attach(scr, false);

    lv_obj_t *brand = ui_label(scr, "ESP32-S3", &lv_font_montserrat_14, UI_COL_DIM);
    lv_obj_align(brand, LV_ALIGN_TOP_RIGHT, -26, 22);

    /* ---------------- 状态卡 ---------------- */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 428, 128);
    lv_obj_set_pos(card, 26, 52);
    lv_obj_set_style_bg_color(card, UI_COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 22, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    ui_obj_scrollable(card, false);
    ui_obj_clickable(card, false);

    s_ui.dot = lv_obj_create(card);
    lv_obj_set_size(s_ui.dot, 26, 26);
    lv_obj_set_pos(s_ui.dot, 28, 51);
    lv_obj_set_style_radius(s_ui.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_ui.dot, UI_COL_SKY, 0);
    lv_obj_set_style_bg_opa(s_ui.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ui.dot, 0, 0);
    ui_obj_clickable(s_ui.dot, false);

    s_ui.state_big = ui_label(card, "BOOT", &lv_font_montserrat_40, UI_COL_TEXT);
    lv_obj_set_pos(s_ui.state_big, 70, 18);

    s_ui.state_cn = ui_label(card, "启动中", &ui_font_18, UI_COL_MUTED);
    lv_obj_set_pos(s_ui.state_cn, 72, 78);

    /* ---------------- 电池（卡左下） ----------------
     * 没插电池时整行隐藏；插着 USB 只显示百分比+充电状态，不估续航。
     * 位置在状态中文下面，宽 178 px，够放「电池 100% · 充电中」。 */
    s_ui.v_bat = ui_label(card, "", &ui_font_18, UI_COL_SKY);
    lv_obj_set_pos(s_ui.v_bat, 72, 102);
    lv_obj_set_width(s_ui.v_bat, 180);
    lv_label_set_long_mode(s_ui.v_bat, LV_LABEL_LONG_CLIP);
    ui_obj_hidden(s_ui.v_bat, true);

    /* ---------------- 网页验证码（卡右侧） ---------------- */
    /* 中文必须走 ui_font_18，montserrat 没有汉字字形（会画成方框） */
    lv_obj_t *code_cap = ui_label(card, "网页验证码", &ui_font_18, UI_COL_DIM);
    lv_obj_set_pos(code_cap, 252, 14);

    s_ui.v_code = ui_label(card, "000 000", &lv_font_montserrat_28, UI_COL_AMBER);
    lv_obj_set_pos(s_ui.v_code, 252, 40);

    s_ui.v_code_ttl = ui_label(card, "剩余 05:00", &ui_font_18, UI_COL_DIM);
    lv_obj_set_pos(s_ui.v_code_ttl, 252, 84);

    /* ---------------- 信息行 ---------------- */
    s_ui.v_ssid = make_row(scr, 192, "网络");
    s_ui.v_ip = make_row(scr, 228, "配置网址");
    s_ui.v_auth = make_row(scr, 264, "重新认证");
    s_ui.v_next = make_row(scr, 300, "下次检测");
    s_ui.v_uptime = make_row(scr, 336, "运行时长");
    lv_obj_set_style_text_color(s_ui.v_auth, UI_COL_AMBER, 0);

    /* ---------------- 最近结果 ---------------- */
    s_ui.msg = ui_label(scr, "启动中", &ui_font_18, UI_COL_MUTED);
    lv_label_set_long_mode(s_ui.msg, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_ui.msg, 428);
    lv_obj_set_pos(s_ui.msg, 26, 372);

    /* ---------------- 按钮 ---------------- */
    lv_obj_t *b1 = ui_button(scr, "立即检测", true, on_check, NULL);
    lv_obj_set_pos(b1, 26, 406);
    lv_obj_set_size(b1, 134, 48);

    lv_obj_t *b2 = ui_button(scr, "WiFi", false, on_wifi, NULL);
    lv_obj_set_pos(b2, 171, 406);
    lv_obj_set_size(b2, 134, 48);

    lv_obj_t *b3 = ui_button(scr, "账户", false, on_acct, NULL);
    lv_obj_set_pos(b3, 316, 406);
    lv_obj_set_size(b3, 134, 48);

    lv_timer_create(ui_timer_cb, 500, NULL);
#if APP_UI_SNAPSHOT_PROBE
    lv_timer_create(snapshot_probe, 3000, NULL);
#endif
#if APP_UI_DUMP_FB
#if APP_UI_CAPTURE_SETUP
    /* 单屏 dump 实测：240x240 约 30s，480x480 约 95s（串口吞吐限制）。
     * 间隔必须大于一次 dump 的耗时，否则切屏和 dump 的定时器会在同一轮里
     * 连着触发，dump 抓到的是还没重绘的旧画面。 */
    /* 主页那张必须等联网 + SNTP 都完成再抓，否则拍出来是「LINKING / 未联网 /
     * 运行时长 00:02」，当 README 首图完全不像个成品。30 秒足够连上 WiFi、
     * 拿到 IP、认证成功，后面的间隔按单张 dump 约 95 秒排。 */
    lv_timer_create(dump_fb, 30000, NULL);         /* 1 主页（已联网） */
    lv_timer_create(cap_open_wifi, 145000, NULL);
    lv_timer_create(dump_fb, 148000, NULL);        /* 2 选网 */
    lv_timer_create(cap_open_acct, 263000, NULL);
    lv_timer_create(dump_fb, 266000, NULL);        /* 3 账户 */
    lv_timer_create(cap_open_pass, 381000, NULL);
    lv_timer_create(dump_fb, 384000, NULL);        /* 4 输密码 */
    lv_timer_create(cap_open_add, 499000, NULL);
    lv_timer_create(dump_fb, 502000, NULL);        /* 5 添加账号 */
#else
    lv_timer_create(dump_fb, 30000, NULL);
#endif
#endif

    /* ui_screen_new() 建的是一个新屏幕对象，必须显式加载，
     * 否则面板上还是 BSP 给的默认屏（白底），我们画的东西一个都看不见。 */
    lv_screen_load(scr);
}
