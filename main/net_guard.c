#include "net_guard.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "batt_mgr.h"
#include "db.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_probe.h"
#include "power_mgr.h"
#include "srun_port.h"
#include "wifi_mgr.h"

static const char *TAG = "net_guard";

static SemaphoreHandle_t s_trigger;

static net_status_t s_st;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const int32_t s_auth_backoff_ms[] = APP_RETRY_BACKOFF_MS;
static const int32_t s_link_backoff_ms[] = APP_LINK_WAIT_BACKOFF_MS;
static const char *const s_sentinel_ips[2] = {
    APP_SENTINEL_IP_PRIMARY,
    APP_SENTINEL_IP_SECONDARY,
};

typedef enum {
    SENTINEL_UNKNOWN = 0,
    SENTINEL_ICMP,
    SENTINEL_TCP443,
    SENTINEL_PORTAL,
    SENTINEL_FULL_ONLY,
} sentinel_mode_t;

typedef enum {
    CYCLE_ONLINE = 0,       /* 外网正常 */
    CYCLE_LINK_DOWN,        /* 硬件/上联断开（Portal 不可达，0 认证） */
    CYCLE_ONLINE_NO_WAN,    /* 校内会话仍在线但外网异常（0 认证/注销） */
    CYCLE_AUTH_REJECTED,    /* Portal 可达且 not_online，但认证失败或未放行 */
} cycle_result_t;

typedef enum {
    WAIT_TIMEOUT = 0,
    WAIT_TRIGGER,
    WAIT_WIFI_LOST,
} wait_event_t;

typedef enum {
    SENTINEL_OK = 0,
    SENTINEL_DOWN,
    SENTINEL_INTERRUPTED,
} sentinel_check_t;

static char s_last_target[33];
static char s_try_ssid[33]; /* 待试连的目标，见 net_guard_try_ssid() */
static char s_switch_ssid[33], s_switch_user[40];

static sentinel_mode_t s_sentinel_mode;
static char s_sentinel_ssid[33];
static int s_sentinel_pref;
static bool s_icmp_bypasses_auth;
static bool s_wake_probe;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *sentinel_name(sentinel_mode_t m)
{
    switch (m) {
    case SENTINEL_ICMP: return "ICMP";
    case SENTINEL_TCP443: return "TCP443";
    case SENTINEL_PORTAL: return "PORTAL";
    case SENTINEL_FULL_ONLY: return "FULL";
    case SENTINEL_UNKNOWN:
    default: return "";
    }
}

static void sentinel_publish(sentinel_mode_t m)
{
    s_sentinel_mode = m;
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.sentinel, sentinel_name(m), sizeof(s_st.sentinel));
    portEXIT_CRITICAL(&s_lock);
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
    s_st.next_check_ms = 0;
    if (st == NET_NEED_SETUP || st == NET_WIFI_CONNECTING || st == NET_WIFI_DOWN ||
        st == NET_NO_ACCOUNT) {
        s_st.account[0] = '\0';
        s_st.portal[0] = '\0';
        s_st.sentinel[0] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

void net_guard_get(net_status_t *out)
{
    wifi_link_t link;
    wifi_mgr_get_link(&link);
    portENTER_CRITICAL(&s_lock);
    *out = s_st;
    portEXIT_CRITICAL(&s_lock);
    /* 页面和网页统一使用驱动事件维护的当前链路，绝不展示上轮的地址。 */
    strlcpy(out->ssid, link.ssid, sizeof(out->ssid));
    strlcpy(out->ip, link.ip, sizeof(out->ip));
    if (!link.connected) {
        out->next_check_ms = 0;
        out->account[0] = '\0';
        out->portal[0] = '\0';
        out->sentinel[0] = '\0';
        if (out->state != NET_BOOT && out->state != NET_NEED_SETUP &&
            out->state != NET_WIFI_CONNECTING && out->state != NET_WIFI_DOWN) {
            out->state = NET_WIFI_DOWN;
            strlcpy(out->detail, "WiFi 断开，正在重试", sizeof(out->detail));
        }
    }
}

void net_guard_trigger(void)
{
    if (s_trigger != NULL) {
        xSemaphoreGive(s_trigger);
    }
}

void net_guard_on_wake(void)
{
    portENTER_CRITICAL(&s_lock);
    s_wake_probe = true;
    portEXIT_CRITICAL(&s_lock);
}

void net_guard_switch_account(const char *ssid, const char *user)
{
    portENTER_CRITICAL(&s_lock);
    strlcpy(s_switch_ssid, ssid, sizeof(s_switch_ssid));
    strlcpy(s_switch_user, user, sizeof(s_switch_user));
    portEXIT_CRITICAL(&s_lock);
    net_guard_trigger();
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

static bool battery_sleeping(void)
{
    batt_info_t bat;
    batt_mgr_get(&bat);
    return bat.present && !bat.usb && !power_mgr_screen_is_on();
}

static int64_t sentinel_interval_ms(void)
{
    return battery_sleeping() ? APP_SENTINEL_SLEEP_MS : APP_SENTINEL_ACTIVE_MS;
}

static bool consume_wake_flag(void)
{
    bool wake;
    portENTER_CRITICAL(&s_lock);
    wake = s_wake_probe;
    s_wake_probe = false;
    portEXIT_CRITICAL(&s_lock);
    return wake;
}

/* 可中断等待：支持手动触发和 WiFi 断线立即打断。 */
static wait_event_t wait_interruptible(uint32_t wait_ms, bool watch_wifi, uint32_t step_ms)
{
    const int64_t deadline = now_ms() + wait_ms;
    for (;;) {
        if (xSemaphoreTake(s_trigger, 0) == pdTRUE) {
            return WAIT_TRIGGER;
        }
        if (watch_wifi && !wifi_mgr_connected()) {
            return WAIT_WIFI_LOST;
        }
        if (now_ms() >= deadline) {
            return WAIT_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
}

/* 等待 wait_ms，期间可以被「立即检测」打断。 */
static bool idle_wait(uint32_t wait_ms)
{
    return wait_interruptible(wait_ms, false, 200) == WAIT_TRIGGER;
}

/* -------------------------------------------------------- 轻量哨兵探针 */

static bool probe_external_single(sentinel_mode_t mode, const char *ip)
{
    if (mode == SENTINEL_ICMP) {
        return net_probe_icmp(ip, APP_SENTINEL_TIMEOUT_MS);
    }
    if (mode == SENTINEL_TCP443) {
        return net_probe_tcp(ip, APP_SENTINEL_TCP_PORT, APP_SENTINEL_TIMEOUT_MS);
    }
    return false;
}

/* 上线后按当前网络实测自动选择最轻探针：ICMP -> TCP:443 -> 校内 Portal -> FULL。 */
static void calibrate_sentinel(const char *ssid, const portal_info_t *pi)
{
    if (strcmp(s_sentinel_ssid, ssid) != 0) {
        strlcpy(s_sentinel_ssid, ssid, sizeof(s_sentinel_ssid));
        s_sentinel_mode = SENTINEL_UNKNOWN;
        s_sentinel_pref = 0;
        s_icmp_bypasses_auth = false;
    }
    if (s_sentinel_mode != SENTINEL_UNKNOWN) {
        sentinel_publish(s_sentinel_mode);
        return;
    }

    if (!s_icmp_bypasses_auth) {
        if (net_probe_icmp(s_sentinel_ips[0], APP_SENTINEL_TIMEOUT_MS)) {
            s_sentinel_pref = 0;
            sentinel_publish(SENTINEL_ICMP);
            ESP_LOGI(TAG, "sentinel calibrated: ICMP (%s)", s_sentinel_ips[0]);
            return;
        }
        if (net_probe_icmp(s_sentinel_ips[1], APP_SENTINEL_TIMEOUT_MS)) {
            s_sentinel_pref = 1;
            sentinel_publish(SENTINEL_ICMP);
            ESP_LOGI(TAG, "sentinel calibrated: ICMP (%s)", s_sentinel_ips[1]);
            return;
        }
    }

    if (net_probe_tcp(s_sentinel_ips[0], APP_SENTINEL_TCP_PORT, APP_SENTINEL_TIMEOUT_MS)) {
        s_sentinel_pref = 0;
        sentinel_publish(SENTINEL_TCP443);
        ESP_LOGI(TAG, "sentinel calibrated: TCP443 (%s)", s_sentinel_ips[0]);
        return;
    }
    if (net_probe_tcp(s_sentinel_ips[1], APP_SENTINEL_TCP_PORT, APP_SENTINEL_TIMEOUT_MS)) {
        s_sentinel_pref = 1;
        sentinel_publish(SENTINEL_TCP443);
        ESP_LOGI(TAG, "sentinel calibrated: TCP443 (%s)", s_sentinel_ips[1]);
        return;
    }

    if (pi != NULL && srun_portal_configured(pi)) {
        char online_user[64];
        if (srun_online_user(pi, online_user, sizeof(online_user)) == ESP_OK) {
            sentinel_publish(SENTINEL_PORTAL);
            ESP_LOGI(TAG, "sentinel calibrated: PORTAL (%s)", pi->portal_base);
            return;
        }
    }

    sentinel_publish(SENTINEL_FULL_ONLY);
    ESP_LOGW(TAG, "sentinel calibrated: FULL_ONLY fallback");
}

/*
 * 轻量哨兵静默防抖确认：
 *   1. 首选 IP 失手不改 UI，等 1 s 换备用 IP 复测；备用通则判偶发波动并切换首选。
 *   2. 主备双失手时，若存在校园 Portal 立刻用 ~10 ms 校内 rad_user_info 裁决：
 *      - 明确 not_online -> 100% 实锤认证掉了，当场返回 SENTINEL_DOWN 立即认证。
 *      - 其余情况（校内在线或不可达）-> 再等 2 s 做第三次外网确认，仍不通才升级。
 */
static sentinel_check_t sentinel_check_and_confirm(const portal_info_t *pi)
{
    if (s_sentinel_mode == SENTINEL_ICMP || s_sentinel_mode == SENTINEL_TCP443) {
        const int first = s_sentinel_pref & 1;
        const int second = 1 - first;

        if (probe_external_single(s_sentinel_mode, s_sentinel_ips[first])) {
            return SENTINEL_OK;
        }

        if (wait_interruptible(APP_SENTINEL_CONFIRM_DELAY1_MS, true, 200) != WAIT_TIMEOUT) {
            return SENTINEL_INTERRUPTED;
        }
        if (probe_external_single(s_sentinel_mode, s_sentinel_ips[second])) {
            s_sentinel_pref = second;
            ESP_LOGI(TAG, "sentinel primary blip, switched to %s", s_sentinel_ips[second]);
            return SENTINEL_OK;
        }

        /* 主备双失败：先用毫秒级校内接口确认是不是认证掉了。 */
        if (pi != NULL && srun_portal_configured(pi)) {
            char actual[64];
            const esp_err_t q = srun_online_user(pi, actual, sizeof(actual));
            if (q == ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "sentinel confirmed auth dropped via portal");
                return SENTINEL_DOWN;
            }
        }

        if (wait_interruptible(APP_SENTINEL_CONFIRM_DELAY2_MS, true, 200) != WAIT_TIMEOUT) {
            return SENTINEL_INTERRUPTED;
        }
        if (probe_external_single(s_sentinel_mode, s_sentinel_ips[first]) ||
            probe_external_single(s_sentinel_mode, s_sentinel_ips[second])) {
            ESP_LOGI(TAG, "sentinel recovered on 3rd check (transient jitter)");
            return SENTINEL_OK;
        }

        ESP_LOGW(TAG, "sentinel confirmed offline after 3 checks (%s)",
                 sentinel_name(s_sentinel_mode));
        return SENTINEL_DOWN;
    }

    if (s_sentinel_mode == SENTINEL_PORTAL && pi != NULL && srun_portal_configured(pi)) {
        char actual[64];
        const esp_err_t q = srun_online_user(pi, actual, sizeof(actual));
        if (q == ESP_OK) {
            return SENTINEL_OK;
        }
        if (q == ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "portal sentinel detected not_online");
            return SENTINEL_DOWN;
        }
        if (wait_interruptible(APP_SENTINEL_CONFIRM_DELAY2_MS, true, 200) != WAIT_TIMEOUT) {
            return SENTINEL_INTERRUPTED;
        }
        return srun_online_user(pi, actual, sizeof(actual)) == ESP_OK
                   ? SENTINEL_OK
                   : SENTINEL_DOWN;
    }

    return SENTINEL_OK;
}

/* 离线轻监听：只在有“可恢复迹象”时才唤醒完整 run_cycle()，不盲目刷 204/登录。 */
static bool probe_any_external_fast(void)
{
    if (s_sentinel_mode == SENTINEL_TCP443 || s_icmp_bypasses_auth) {
        return net_probe_tcp(s_sentinel_ips[0], APP_SENTINEL_TCP_PORT, APP_SENTINEL_TIMEOUT_MS) ||
               net_probe_tcp(s_sentinel_ips[1], APP_SENTINEL_TCP_PORT, APP_SENTINEL_TIMEOUT_MS);
    }
    if (net_probe_icmp(s_sentinel_ips[0], APP_SENTINEL_TIMEOUT_MS) ||
        net_probe_icmp(s_sentinel_ips[1], APP_SENTINEL_TIMEOUT_MS)) {
        return true;
    }
    return net_probe_tcp(s_sentinel_ips[0], APP_SENTINEL_TCP_PORT, APP_SENTINEL_TIMEOUT_MS);
}

/* ------------------------------------------------------------ 检测一次 */

static cycle_result_t run_cycle(const char *ssid, const char *user, const char *pass,
                                bool switching, portal_info_t *out_pi)
{
    portal_info_t pi;
    char detail[96];

    state_set(NET_CHECKING, "正在检测外网");

    probe_result_t pr = srun_probe(&pi);
    if (out_pi != NULL) {
        *out_pi = pi;
    }

    portENTER_CRITICAL(&s_lock);
    strlcpy(s_st.portal, pi.portal_base, sizeof(s_st.portal));
    s_st.last_check_ms = now_ms();
    portEXIT_CRITICAL(&s_lock);

    if (pr == PROBE_ONLINE) {
        char actual[64] = "", wanted[64];
        snprintf(wanted, sizeof(wanted), "%s%s", user, APP_PORTAL_DOMAIN);
        const esp_err_t query = srun_online_user(&pi, actual, sizeof(actual));
        if (!switching || (query == ESP_OK && !strcmp(actual, wanted))) {
            if (query == ESP_OK) {
                portENTER_CRITICAL(&s_lock);
                strlcpy(s_st.account, actual, sizeof(s_st.account));
                portEXIT_CRITICAL(&s_lock);
            }
            state_set(NET_ONLINE, query != ESP_OK ? "已联网（认证账号未核验）" :
                      strcmp(actual, wanted) ? "已联网，保留现有认证会话" : "已联网，认证账号已核实");
            return CYCLE_ONLINE;
        }
        if (query == ESP_OK) {
            state_set(NET_AUTHING, "正在切换认证账号");
            if (!srun_logout(&pi, actual, detail, sizeof(detail))) {
                state_set(NET_AUTH_FAILED, "%s", detail);
                return CYCLE_AUTH_REJECTED;
            }
            /* 已退出旧会话，本轮必须登录新账号，不能再次走“外网已通”捷径。 */
        } else if (query != ESP_ERR_NOT_FOUND) {
            state_set(NET_AUTH_FAILED, "无法查询原认证账号，请检查 Portal 地址");
            return CYCLE_AUTH_REJECTED;
        }
    }

    /* 硬件/网线断联：Portal 不可达，绝不发起认证！ */
    if (pr == PROBE_NO_LINK) {
        state_set(NET_ERROR, "%s", pi.detail);
        return CYCLE_LINK_DOWN;
    }

    /* 校内会话仍在线但外网不通：绝不误注销、绝不误重登！ */
    if (pr == PROBE_ONLINE_NO_WAN) {
        state_set(NET_ERROR, "%s", pi.detail);
        return CYCLE_ONLINE_NO_WAN;
    }

    state_set(NET_NEED_AUTH, "%s", pi.detail);
    state_set(NET_AUTHING, "账号 %s 认证中", user);

    char detail2[96] = "";
    login_result_t lr = srun_login(&pi, user, pass, APP_PORTAL_DOMAIN,
                                   detail, sizeof(detail));

    if (lr == LOGIN_FAIL && strcmp(pi.portal_base, APP_PORTAL_FALLBACK_HOST) != 0) {
        /* 仅当编译期兜底地址本身有效时才尝试兜底重试。 */
        portal_info_t fb;
        memset(&fb, 0, sizeof(fb));
        strlcpy(fb.portal_base, APP_PORTAL_FALLBACK_HOST, sizeof(fb.portal_base));
        strlcpy(fb.ac_id, APP_PORTAL_FALLBACK_ACID, sizeof(fb.ac_id));
        if (srun_portal_configured(&fb)) {
            ESP_LOGW(TAG, "retry against fallback portal %s", fb.portal_base);
            lr = srun_login(&fb, user, pass, APP_PORTAL_DOMAIN, detail2, sizeof(detail2));
            if (lr != LOGIN_FAIL) {
                pi = fb;
                if (out_pi != NULL) {
                    *out_pi = pi;
                }
                strlcpy(detail, detail2, sizeof(detail));
            } else {
                char both[96];
                snprintf(both, sizeof(both), "%s | 兜底 %s", detail, detail2);
                strlcpy(detail, both, sizeof(detail));
            }
        }
    }

    if (lr == LOGIN_FAIL) {
        ESP_LOGW(TAG, "login failed: %s", detail);
        state_set(NET_AUTH_FAILED, "认证失败: %s", detail);
        return CYCLE_AUTH_REJECTED;
    }

    ESP_LOGI(TAG, "login ok: %s", detail);

    /* 认证返回成功不等于真有网：再探一次才算数 */
    probe_result_t pr2 = srun_probe(&pi);
    if (out_pi != NULL) {
        *out_pi = pi;
    }
    if (pr2 == PROBE_ONLINE) {
        /* “IP 已在线”也可能仍属于旧账号，切换场景必须核实身份。 */
        if (switching || lr == LOGIN_ALREADY) {
            char actual[64] = "", wanted[64];
            snprintf(wanted, sizeof(wanted), "%s%s", user, APP_PORTAL_DOMAIN);
            if (srun_online_user(&pi, actual, sizeof(actual)) != ESP_OK || strcmp(actual, wanted)) {
                state_set(NET_AUTH_FAILED, "外网已通，但新认证账号尚未确认");
                return CYCLE_AUTH_REJECTED;
            }
        }
        portENTER_CRITICAL(&s_lock);
        s_st.auth_count++;
        portEXIT_CRITICAL(&s_lock);
        state_set(NET_ONLINE, "认证成功，外网已通 (%s)", detail);
        return CYCLE_ONLINE;
    }

    state_set(NET_AUTH_FAILED, "认证成功但外网仍不通 (%s)", detail);
    return CYCLE_AUTH_REJECTED;
}

/* ----------------------------------------------------- 稳态与恢复监听 */

/* 在线稳态：轻量哨兵实时值守 + 15 分钟完整 204 兜底；返回 true 表示由兜底到期退出。 */
static bool online_watch(const char *ssid, const portal_info_t *pi)
{
    calibrate_sentinel(ssid, pi);
    consume_wake_flag();

    const int64_t start_ms = now_ms();
    int64_t last_probe_ms = start_ms;

    for (;;) {
        const int64_t now = now_ms();
        const int64_t full_due = start_ms + APP_FULL_CHECK_INTERVAL_MS;
        const int64_t interval = sentinel_interval_ms();
        int64_t probe_due = (s_sentinel_mode == SENTINEL_FULL_ONLY)
                                ? full_due
                                : (last_probe_ms + interval);
        if (probe_due > full_due) {
            probe_due = full_due;
        }

        portENTER_CRITICAL(&s_lock);
        s_st.next_check_ms = probe_due;
        portEXIT_CRITICAL(&s_lock);

        if (xSemaphoreTake(s_trigger, 0) == pdTRUE) {
            ESP_LOGI(TAG, "manual check");
            return false;
        }
        if (!wifi_mgr_connected()) {
            ESP_LOGW(TAG, "wifi lost, reconnect now");
            return false;
        }
        if (now >= full_due) {
            ESP_LOGI(TAG, "periodic full 204 check");
            return true;
        }

        const bool wake = consume_wake_flag();
        const bool wake_due = wake && (now - last_probe_ms >= APP_SENTINEL_WAKE_COOLDOWN_MS);
        if (s_sentinel_mode != SENTINEL_FULL_ONLY && (now >= probe_due || wake_due)) {
            const sentinel_check_t res = sentinel_check_and_confirm(pi);
            if (res == SENTINEL_OK) {
                last_probe_ms = now_ms();
                continue;
            }
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/*
 * 硬件/网线断联恢复监听：
 *   绝不触发认证、绝不轮询双 8 s HTTP 204 超时。
 *   只做 1.5 s 轻量端口/外网恢复探测（15 -> 30 -> 60 s，电池熄屏 60 s），
 *   直到 Portal 重新可达（具备提交认证条件）或外网直接恢复才退出重检。
 */
static void wait_link_recovery(const portal_info_t *last_pi)
{
    const int n_steps = (int)(sizeof(s_link_backoff_ms) / sizeof(s_link_backoff_ms[0]));
    int streak = 0;

    for (;;) {
        const int idx = (streak < n_steps) ? streak : (n_steps - 1);
        const int32_t wait_ms = battery_sleeping() ? s_link_backoff_ms[n_steps - 1]
                                                   : s_link_backoff_ms[idx];
        portENTER_CRITICAL(&s_lock);
        s_st.next_check_ms = now_ms() + wait_ms;
        portEXIT_CRITICAL(&s_lock);

        const wait_event_t ev = wait_interruptible((uint32_t)wait_ms, true, 500);
        if (ev != WAIT_TIMEOUT) {
            return;
        }

        portal_info_t pi = {0};
        const bool has_portal = (last_pi != NULL && srun_portal_configured(last_pi))
                                    ? (pi = *last_pi, true)
                                    : srun_portal_current(&pi);
        if (has_portal && net_probe_origin_tcp(pi.portal_base, APP_SENTINEL_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "portal reachable again (%s), resuming check", pi.portal_base);
            return;
        }
        if (probe_any_external_fast()) {
            ESP_LOGI(TAG, "external network reachable again, resuming check");
            return;
        }

        if (streak < n_steps - 1) {
            streak++;
        }
    }
}

/*
 * 校内认证会话仍在线、但外网出口异常时的轻监听：
 *   不注销、不重登；低频检测外网恢复，并顺带确认会话是否已变成 not_online。
 */
static void wait_wan_recovery(const portal_info_t *pi)
{
    for (;;) {
        const int32_t wait_ms = battery_sleeping() ? APP_WAN_WAIT_SLEEP_MS
                                                   : APP_WAN_WAIT_ACTIVE_MS;
        portENTER_CRITICAL(&s_lock);
        s_st.next_check_ms = now_ms() + wait_ms;
        portEXIT_CRITICAL(&s_lock);

        const wait_event_t ev = wait_interruptible((uint32_t)wait_ms, true, 500);
        if (ev != WAIT_TIMEOUT) {
            return;
        }

        if (probe_any_external_fast()) {
            ESP_LOGI(TAG, "WAN recovered, resuming full check");
            return;
        }
        if (pi != NULL && srun_portal_configured(pi)) {
            char actual[64];
            const esp_err_t q = srun_online_user(pi, actual, sizeof(actual));
            if (q == ESP_ERR_NOT_FOUND) {
                ESP_LOGI(TAG, "session became not_online, ready to authenticate");
                return;
            }
            if (q != ESP_OK) {
                /* 连校内 Portal 也不可达了，切入硬件断联恢复监听。 */
                state_set(NET_ERROR, "上联断开，认证服务器不可达");
                wait_link_recovery(pi);
                return;
            }
        }
    }
}

/* ---------------------------------------------------------------- 连接 */

static bool ensure_connected(const char *target, const char *pass)
{
    wifi_link_t link;
    wifi_mgr_get_link(&link);

    const bool need = !link.connected || strcmp(link.ssid, target) != 0;

    if (need) {
        state_set(NET_WIFI_CONNECTING, "连接 %s", target);
        const esp_err_t err = wifi_mgr_connect(target, pass);
        strlcpy(s_last_target, target, sizeof(s_last_target));

        if (err != ESP_OK || !wifi_mgr_wait_ip(APP_WIFI_CONNECT_TIMEOUT_MS)) {
            state_set(NET_WIFI_DOWN, "连不上 %s", target);
            return false;
        }
    }

    wifi_mgr_get_link(&link);
    if (!link.connected || strcmp(link.ssid, target) != 0) {
        state_set(NET_WIFI_DOWN, "连不上 %s", target);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ task */

static void net_task(void *arg)
{
    (void)arg;
    int conn_fail = 0;
    char applied_ssid[DB_SSID_LEN] = "", applied_user[DB_USER_LEN] = "";
    bool due_full_check = false;

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

            /* 与常规重连共用校验，旧连接迟到的 GOT_IP 不能使新目标试连成功。 */
            if (ensure_connected(try_ssid, pass)) {
                /* 连上了才把守护目标切过去，失败不影响原来能用的网络 */
                db_set_active_ssid(try_ssid);
                db_set_last_ok(try_ssid);
                strlcpy(s_last_target, try_ssid, sizeof(s_last_target));
                conn_fail = 0;
                due_full_check = false;
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
             * 没有备用网络时保留原目标持续重试。 */
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
                    ESP_LOGW(TAG, "no fallback network, keep [%s] and retry", target);
                    state_set(NET_WIFI_DOWN, "连不上 %s，继续重试", target);
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
        portENTER_CRITICAL(&s_lock);
        const bool requested_switch = !strcmp(s_switch_ssid, target) && !strcmp(s_switch_user, user);
        s_switch_ssid[0] = s_switch_user[0] = '\0';
        portEXIT_CRITICAL(&s_lock);
        const bool switching = requested_switch ||
            (!strcmp(applied_ssid, target) && applied_user[0] && strcmp(applied_user, user));

        const int prev_auths = s_st.auth_count;
        portal_info_t pi = {0};
        const cycle_result_t result = run_cycle(target, user, acct_pass, switching, &pi);

        /* 若是在 ICMP 哨兵一直报通的情况下，15 分钟完整兜底竟然触发了认证，
         * 说明当前学校防火墙在未认证时也放行 ICMP，自动永久跳过 ICMP 改用 TCP:443。 */
        if (due_full_check && s_sentinel_mode == SENTINEL_ICMP &&
            (result == CYCLE_AUTH_REJECTED || s_st.auth_count > prev_auths)) {
            ESP_LOGW(TAG, "ICMP bypassed portal auth on [%s], switching sentinel to TCP443", target);
            s_icmp_bypasses_auth = true;
            s_sentinel_mode = SENTINEL_UNKNOWN;
        }
        due_full_check = false;

        if (result == CYCLE_ONLINE) {
            strlcpy(applied_ssid, target, sizeof(applied_ssid));
            strlcpy(applied_user, user, sizeof(applied_user));
            portENTER_CRITICAL(&s_lock);
            s_st.fail_streak = 0;
            portEXIT_CRITICAL(&s_lock);

            due_full_check = online_watch(target, &pi);
            continue;
        }

        if (result == CYCLE_LINK_DOWN) {
            /* 网线/上联断开：0 次认证重试，轻监听等 Portal 或外网恢复。 */
            portENTER_CRITICAL(&s_lock);
            s_st.fail_streak = 0;
            portEXIT_CRITICAL(&s_lock);
            wait_link_recovery(&pi);
            continue;
        }

        if (result == CYCLE_ONLINE_NO_WAN) {
            /* 校内会话仍在线：不注销不重登，低频等外网恢复或会话下线。 */
            portENTER_CRITICAL(&s_lock);
            s_st.fail_streak = 0;
            portEXIT_CRITICAL(&s_lock);
            wait_wan_recovery(&pi);
            continue;
        }

        /* CYCLE_AUTH_REJECTED：服务端明确拒绝或认证未放行，强退避防狂撞认证服务器。 */
        portENTER_CRITICAL(&s_lock);
        const int streak = ++s_st.fail_streak;
        portEXIT_CRITICAL(&s_lock);

        const int n_backoff = (int)(sizeof(s_auth_backoff_ms) / sizeof(s_auth_backoff_ms[0]));
        const int idx = (streak <= n_backoff) ? (streak - 1) : (n_backoff - 1);
        const int32_t wait_ms = s_auth_backoff_ms[idx];

        portENTER_CRITICAL(&s_lock);
        s_st.next_check_ms = now_ms() + wait_ms;
        portEXIT_CRITICAL(&s_lock);

        if (wait_interruptible((uint32_t)wait_ms, true, 500) == WAIT_TRIGGER) {
            /* 用户主动重试或修改了账号密码：清零退避计数。 */
            portENTER_CRITICAL(&s_lock);
            s_st.fail_streak = 0;
            portEXIT_CRITICAL(&s_lock);
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
    s_switch_ssid[0] = s_switch_user[0] = '\0';
    s_sentinel_mode = SENTINEL_UNKNOWN;
    s_sentinel_ssid[0] = '\0';
    s_sentinel_pref = 0;
    s_icmp_bypasses_auth = false;
    s_wake_probe = false;

    s_trigger = xSemaphoreCreateBinary();
    if (s_trigger == NULL) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    if (xTaskCreate(net_task, "net_guard", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return;
    }

    ESP_LOGI(TAG, "guard up: sentinel=%ds/%ds full=%dmin",
             APP_SENTINEL_ACTIVE_MS / 1000, APP_SENTINEL_SLEEP_MS / 1000,
             APP_FULL_CHECK_INTERVAL_MS / 60000);
}
