#include "net_guard.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "db.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "srun_port.h"
#include "wifi_mgr.h"

static const char *TAG = "net_guard";

static SemaphoreHandle_t s_trigger;

static net_status_t s_st;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const int32_t s_backoff_ms[] = APP_RETRY_BACKOFF_MS;

static char s_last_target[33];
static char s_try_ssid[33]; /* 待试连的目标，见 net_guard_try_ssid() */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

/* ------------------------------------------------------------- 状态发布 */

static void state_set(net_state_t st, const char *fmt, ...)
{
    char buf[sizeof(s_st.detail)] = "";

    if (fmt != NULL) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
    }

    portENTER_CRITICAL(&s_lock);
    s_st.state = st;
    strlcpy(s_st.detail, buf, sizeof(s_st.detail));
    portEXIT_CRITICAL(&s_lock);
}

void net_guard_get(net_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
}

void net_guard_trigger(void)
{
    if (s_trigger != NULL) {
        xSemaphoreGive(s_trigger);
    }
}

void net_guard_try_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_try_ssid, ssid, sizeof(s_try_ssid));
    portEXIT_CRITICAL(&s_lock);
    net_guard_trigger();
}

/* 等待 wait_ms，期间可以被「立即检测」打断。
 * 返回 true 表示被打断（该立刻重来一轮）。 */
static bool idle_wait(uint32_t wait_ms)
{
    int64_t deadline = now_ms() + wait_ms;
    for (;;) {
        if (xSemaphoreTake(s_trigger, 0) == pdTRUE) {
            return true;
        }
        if (now_ms() >= deadline) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ------------------------------------------------------------ 检测一次 */

static bool run_cycle(const char *ssid, const char *user, const char *pass, bool switching)
{
    portal_info_t pi;
    char detail[96];

    state_set(NET_CHECKING, "正在检测外网");

    probe_result_t pr = srun_probe(&pi);

    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.portal, pi.portal_base, sizeof(s_st.portal));
    s_st.last_check_ms = now_ms();
    portEXIT_CRITICAL(&s_lock);

    if (pr == PROBE_ONLINE) {
        char actual[64] = "", wanted[64];
        snprintf(wanted, sizeof(wanted), "%s%s", user, APP_PORTAL_DOMAIN);
        const esp_err_t query = srun_online_user(&pi, actual, sizeof(actual));
        if (query == ESP_OK && strcmp(actual, wanted) != 0) {
            switching = true;
            state_set(NET_AUTHING, "正在切换认证账号");
            if (!srun_logout(&pi, actual, detail, sizeof(detail))) {
                state_set(NET_AUTH_FAILED, "%s", detail);
                return false;
            }
            /* 已退出旧会话，本轮必须登录新账号，不能再次走“外网已通”捷径。 */
        } else if (query == ESP_OK || (!switching && query != ESP_ERR_NOT_FOUND)) {
            state_set(NET_ONLINE, query == ESP_OK ? "已联网，认证账号已核实" : "已联网（认证账号未核验）");
            return true;
        } else if (query != ESP_ERR_NOT_FOUND) {
            state_set(NET_AUTH_FAILED, "无法查询原认证账号，请检查 Portal 地址");
            return false;
        }
    }

    if (pr == PROBE_NO_LINK) {
        state_set(NET_ERROR, "%s", pi.detail);
        return false;
    }

    state_set(NET_NEED_AUTH, "%s", pi.detail);
    state_set(NET_AUTHING, "账号 %s 认证中", user);

    char detail2[96] = "";
    login_result_t lr = srun_login(&pi, user, pass, APP_PORTAL_DOMAIN,
                                  detail, sizeof(detail));

    if (lr == LOGIN_FAIL && strcmp(pi.portal_base, APP_PORTAL_FALLBACK_HOST) != 0) {
        /* 有的路由会把 302 引到自己的页面：换兜底 Portal 再试一次 */
        portal_info_t fb;
        memset(&fb, 0, sizeof(fb));
        strlcpy(fb.portal_base, APP_PORTAL_FALLBACK_HOST, sizeof(fb.portal_base));
        strlcpy(fb.ac_id, APP_PORTAL_FALLBACK_ACID, sizeof(fb.ac_id));

        ESP_LOGW(TAG, "retry against fallback portal %s", fb.portal_base);
        lr = srun_login(&fb, user, pass, APP_PORTAL_DOMAIN, detail2, sizeof(detail2));
        if (lr != LOGIN_FAIL) {
            pi = fb;
            strlcpy(detail, detail2, sizeof(detail));
        } else {
            char both[96];
            snprintf(both, sizeof(both), "%s | 兜底 %s", detail, detail2);
            strlcpy(detail, both, sizeof(detail));
        }
    }

    if (lr == LOGIN_FAIL) {
        ESP_LOGW(TAG, "login failed: %s", detail);
        state_set(NET_AUTH_FAILED, "认证失败: %s", detail);
        return false;
    }

    ESP_LOGI(TAG, "login ok: %s", detail);

    /* 认证返回成功不等于真有网：再探一次才算数 */
    probe_result_t pr2 = srun_probe(&pi);
    if (pr2 == PROBE_ONLINE) {
        /* “IP 已在线”也可能仍属于旧账号，切换场景必须核实身份。 */
        if (switching || lr == LOGIN_ALREADY) {
            char actual[64] = "", wanted[64];
            snprintf(wanted, sizeof(wanted), "%s%s", user, APP_PORTAL_DOMAIN);
            if (srun_online_user(&pi, actual, sizeof(actual)) != ESP_OK || strcmp(actual, wanted)) {
                state_set(NET_AUTH_FAILED, "外网已通，但新认证账号尚未确认");
                return false;
            }
        }
        portENTER_CRITICAL(&s_lock);
        s_st.auth_count++;
        portEXIT_CRITICAL(&s_lock);
        state_set(NET_ONLINE, "认证成功，外网已通 (%s)", detail);
        return true;
    }

    state_set(NET_AUTH_FAILED, "认证成功但外网仍不通 (%s)", detail);
    return false;
}

/* ---------------------------------------------------------------- 连接 */

static bool ensure_connected(const char *target, const char *pass)
{
    char cur[33] = "";
    wifi_mgr_ssid(cur, sizeof(cur));

    const bool need = !wifi_mgr_connected() || strcmp(s_last_target, target) != 0;

    if (need) {
        state_set(NET_WIFI_CONNECTING, "连接 %s", target);
        wifi_mgr_connect(target, pass);
        strlcpy(s_last_target, target, sizeof(s_last_target));

        if (!wifi_mgr_wait_ip(APP_WIFI_CONNECT_TIMEOUT_MS)) {
            state_set(NET_WIFI_DOWN, "连不上 %s", target);
            return false;
        }
    }

    wifi_mgr_ip(s_st.ip, sizeof(s_st.ip));
    wifi_mgr_ssid(s_st.ssid, sizeof(s_st.ssid));
    return true;
}

/* ------------------------------------------------------------------ task */

static void net_task(void *arg)
{
    (void)arg;
    int conn_fail = 0;
    char applied_ssid[DB_SSID_LEN] = "", applied_user[DB_USER_LEN] = "";

    for (;;) {
        char target[33] = "";
        char pass[DB_PASS_LEN] = "";
        char user[DB_USER_LEN] = "";
        char acct_pass[DB_PASS_LEN] = "";

        /* --- 0. 有没有人要求试连一个新网络 --- */
        char try_ssid[33] = "";
        portENTER_CRITICAL(&s_lock);
        strlcpy(try_ssid, s_try_ssid, sizeof(try_ssid));
        s_try_ssid[0] = '\0';
        portEXIT_CRITICAL(&s_lock);

        if (try_ssid[0] != '\0') {
            db_read_begin();
            const db_net_t *tn = db_find(try_ssid);
            if (tn != NULL) {
                strlcpy(pass, tn->wifi_pass, sizeof(pass));
            }
            db_read_end();

            state_set(NET_WIFI_CONNECTING, "尝试连接 %s", try_ssid);
            wifi_mgr_connect(try_ssid, pass);

            if (wifi_mgr_wait_ip(APP_WIFI_CONNECT_TIMEOUT_MS)) {
                /* 连上了才把守护目标切过去，失败不影响原来能用的网络 */
                db_set_active_ssid(try_ssid);
                db_set_last_ok(try_ssid);
                strlcpy(s_last_target, try_ssid, sizeof(s_last_target));
                conn_fail = 0;
                ESP_LOGI(TAG, "trial connect ok, target -> [%s]", try_ssid);
            } else {
                state_set(NET_WIFI_DOWN, "连不上 %s，保持原网络", try_ssid);
                ESP_LOGW(TAG, "trial connect to [%s] failed", try_ssid);
                s_last_target[0] = '\0'; /* 强制回到 active 那个网络 */
                idle_wait(2000);
                continue;
            }
        }

        db_active_ssid(target, sizeof(target));

        portENTER_CRITICAL(&s_lock);
        strlcpy(s_st.target, target, sizeof(s_st.target));
        portEXIT_CRITICAL(&s_lock);

        /* --- 还没选网 --- */
        if (target[0] == '\0') {
            state_set(NET_NEED_SETUP, "尚未选择 WiFi");
            if (s_last_target[0] != '\0') {
                wifi_mgr_disconnect();
                s_last_target[0] = '\0';
            }
            idle_wait(2000);
            continue;
        }

        /* --- 取 WiFi 密码 --- */
        db_read_begin();
        const db_net_t *n = db_find(target);
        if (n != NULL) {
            strlcpy(pass, n->wifi_pass, sizeof(pass));
        }
        db_read_end();

        /* --- 连网 --- */
        if (!ensure_connected(target, pass)) {
            conn_fail++;
            /* 连续两次连不上：换上次成功的，或者库里任意另一个网络。
             * 这一步是防呆 —— 否则用户误填一个不存在的 SSID，板子就再也连不上，
             * 网页面板也跟着失联，只能拔卡救砖。 */
            if (conn_fail >= 2) {
                conn_fail = 0;

                char alt[33] = "";
                db_last_ok(alt, sizeof(alt));
                if (alt[0] != '\0' && strcmp(alt, target) == 0) {
                    alt[0] = '\0'; /* last_ok 就是刚失败的这个，没用 */
                }
                if (alt[0] == '\0') {
                    db_other_ssid(target, alt, sizeof(alt)); /* 库里挑一个别的 */
                }

                if (alt[0] != '\0') {
                    ESP_LOGW(TAG, "[%s] unreachable, falling back to [%s]", target, alt);
                    state_set(NET_WIFI_DOWN, "连不上 %s，回退 %s", target, alt);
                    db_set_active_ssid(alt);
                } else {
                    ESP_LOGW(TAG, "no fallback network, clear target");
                    state_set(NET_NEED_SETUP, "网络都连不上，请重新选择");
                    db_set_active_ssid("");
                }
            }
            idle_wait(APP_WIFI_RETRY_MS);
            continue;
        }
        conn_fail = 0;
        db_set_last_ok(target);

        /* --- 取该 WiFi 下的认证账号 --- */
        if (!db_current_acct(target, user, sizeof(user), acct_pass, sizeof(acct_pass))) {
            state_set(NET_NO_ACCOUNT, "%s 下还没有认证账号", target);
            idle_wait(3000);
            continue;
        }

        portENTER_CRITICAL(&s_lock);
        strlcpy(s_st.account, user, sizeof(s_st.account));
        portEXIT_CRITICAL(&s_lock);

        /* --- 检测 + 必要时认证 --- */
        const bool switching = !strcmp(applied_ssid, target) && applied_user[0] && strcmp(applied_user, user);
        const bool online = run_cycle(target, user, acct_pass, switching);
        if (online) {
            strlcpy(applied_ssid, target, sizeof(applied_ssid));
            strlcpy(applied_user, user, sizeof(applied_user));
        }

        portENTER_CRITICAL(&s_lock);
        s_st.fail_streak = online ? 0 : (s_st.fail_streak + 1);
        const int streak = s_st.fail_streak;
        portEXIT_CRITICAL(&s_lock);

        int64_t wait_ms;
        if (online) {
            wait_ms = APP_POLL_INTERVAL_MS;
        } else {
            const int n_backoff = (int)(sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0]));
            wait_ms = (streak <= n_backoff) ? s_backoff_ms[streak - 1] : APP_POLL_INTERVAL_MS;
        }

        portENTER_CRITICAL(&s_lock);
        s_st.next_check_ms = now_ms() + wait_ms;
        portEXIT_CRITICAL(&s_lock);

        /* --- 等下一次；期间每秒看一次连接还在不在 --- */
        int64_t deadline = now_ms() + wait_ms;
        while (now_ms() < deadline) {
            if (xSemaphoreTake(s_trigger, 0) == pdTRUE) {
                ESP_LOGI(TAG, "manual check");
                break;
            }
            if (!wifi_mgr_connected()) {
                ESP_LOGW(TAG, "wifi lost, reconnect now");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
}

/* ------------------------------------------------------------------ init */

void net_guard_start(void)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = NET_BOOT;
    strlcpy(s_st.detail, "启动中", sizeof(s_st.detail));
    s_st.boot_ms = now_ms();
    s_last_target[0] = '\0';

    s_trigger = xSemaphoreCreateBinary();
    if (s_trigger == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    if (xTaskCreate(net_task, "net_guard", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }

    ESP_LOGI(TAG, "guard up: poll=%d min", APP_POLL_INTERVAL_MS / 60000);
}
