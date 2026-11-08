#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "batt_mgr.h"
#include "db.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net_guard.h"
#include "ui.h"
#include "ui_setup.h"
#include "wifi_mgr.h"
#include "http_util.h"

static const char *TAG = "web";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

/* ------------------------------------------------------------- 小工具 */

static esp_err_t read_body(httpd_req_t *req, char *buf, size_t cap)
{
    const int total = req->content_len;
    if (total <= 0 || (size_t)total >= cap) {
        return ESP_FAIL;
    }
    int got = 0;
    while (got < total) {
        const int r = httpd_req_recv(req, buf + got, (size_t)(total - got));
        if (r <= 0) {
            return ESP_FAIL;
        }
        got += r;
    }
    buf[got] = '\0';
    return ESP_OK;
}

/* 必填字段要求完整解码成功，不能把超长值当作有效前缀。 */
static bool form_get(char *body, const char *key, char *out, size_t cap)
{
    return http_form_get(body, key, out, cap) == ESP_OK;
}

/* 可选字段允许缺失，但格式错误或容量不足必须让整个操作失败。 */
static bool form_optional(char *body, const char *key, char *out, size_t cap, bool *present)
{
    const esp_err_t err = http_form_get(body, key, out, cap);
    if (present) *present = err == ESP_OK;
    return err == ESP_OK || err == ESP_ERR_NOT_FOUND;
}

static esp_err_t reply(httpd_req_t *req, bool ok, const char *msg)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg ? msg : "");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static void json_esc(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    for (const char *p = src; *p != '\0' && o + 2 < cap; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }
    dst[o] = '\0';
}

/* ------------------------------------------------------- 验证码 / token */
/*
 * 防误改：屏幕上实时显示 6 位验证码（5 分钟换一次），网页端先验证换
 * token，再拿 token 调写接口。token 独立于验证码 —— 码换了不影响已
 * 解锁的页面（本次开机中最多 7 天有效；重启后需要重新解锁）。
 */
#define WEB_CODE_TTL_MS    (5 * 60 * 1000)
#define WEB_TOKEN_TTL_MS   (7LL * 24 * 60 * 60 * 1000)
#define WEB_TOKEN_LEN      33   /* 32 hex + NUL */
#define WEB_MAX_TOKENS     8
#define WEB_AUTH_LOCK_MS   (60 * 1000)
#define WEB_AUTH_MAX_FAIL  5

static portMUX_TYPE s_auth_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_code[7];
static int64_t s_code_gen_ms;
static struct {
    char tok[WEB_TOKEN_LEN];
    int64_t born_ms;
    bool used;
} s_tokens[WEB_MAX_TOKENS];
static int s_auth_fails;
static int64_t s_auth_lock_until_ms;

/* 持锁调用：码过期就换新的 */
static void code_refresh_locked(void)
{
    const int64_t now = esp_timer_get_time() / 1000;
    if (s_code_gen_ms == 0 || now - s_code_gen_ms >= WEB_CODE_TTL_MS) {
        snprintf(s_code, sizeof(s_code), "%06u", (unsigned)(esp_random() % 1000000u));
        s_code_gen_ms = now;
    }
}

void web_code_get(char out[7])
{
    portENTER_CRITICAL(&s_auth_mux);
    code_refresh_locked();
    memcpy(out, s_code, 7);
    portEXIT_CRITICAL(&s_auth_mux);
}

int64_t web_code_remain_ms(void)
{
    portENTER_CRITICAL(&s_auth_mux);
    code_refresh_locked();
    const int64_t left = WEB_CODE_TTL_MS - (esp_timer_get_time() / 1000 - s_code_gen_ms);
    portEXIT_CRITICAL(&s_auth_mux);
    return left > 0 ? left : 0;
}

static void token_issue(char *out, size_t cap)
{
    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_auth_mux);
    int slot = -1;
    int64_t oldest = now;
    for (int i = 0; i < WEB_MAX_TOKENS; i++) {
        if (!s_tokens[i].used) {
            slot = i;
            break;
        }
        if (slot < 0 || s_tokens[i].born_ms < oldest) {
            oldest = s_tokens[i].born_ms;
            slot = i;
        }
    }
    snprintf(s_tokens[slot].tok, sizeof(s_tokens[slot].tok), "%08x%08x",
             (unsigned)esp_random(), (unsigned)esp_random());
    s_tokens[slot].born_ms = now;
    s_tokens[slot].used = true;
    strlcpy(out, s_tokens[slot].tok, cap);
    portEXIT_CRITICAL(&s_auth_mux);
}

static bool token_check(const char *tok)
{
    if (tok == NULL || tok[0] == '\0') {
        return false;
    }
    const int64_t now = esp_timer_get_time() / 1000;
    bool ok = false;
    portENTER_CRITICAL(&s_auth_mux);
    for (int i = 0; i < WEB_MAX_TOKENS; i++) {
        if (s_tokens[i].used && strcmp(s_tokens[i].tok, tok) == 0) {
            ok = (now - s_tokens[i].born_ms) <= WEB_TOKEN_TTL_MS;
            break;
        }
    }
    portEXIT_CRITICAL(&s_auth_mux);
    return ok;
}

static esp_err_t reply_need_auth(httpd_req_t *req)
{
    char buf[112];
    snprintf(buf, sizeof(buf),
             "{\"ok\":false,\"need_auth\":true,\"msg\":\"请先输入板子屏幕上的验证码\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* 读 body 并校验 token；返回 false 表示已回错误响应，调用方直接 return ESP_OK */
static bool authed_body(httpd_req_t *req, char *body, size_t cap)
{
    if (read_body(req, body, cap) != ESP_OK) {
        reply(req, false, "请求体为空");
        return false;
    }
    char tok[WEB_TOKEN_LEN];
    if (!form_get(body, "token", tok, sizeof(tok)) || !token_check(tok)) {
        reply_need_auth(req);
        return false;
    }
    return true;
}

/* 只验证浏览器保存的会话，不签发 token，也不计入验证码错误次数。 */
static esp_err_t h_session(httpd_req_t *req)
{
    char body[96], tok[WEB_TOKEN_LEN];
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (read_body(req, body, sizeof(body)) != ESP_OK ||
        !form_get(body, "token", tok, sizeof(tok)) || !token_check(tok)) {
        return reply_need_auth(req);
    }
    return reply(req, true, "");
}

/* 用验证码换 token。连错 5 次锁 60 秒，防手贱穷举 */
static esp_err_t h_auth(httpd_req_t *req)
{
    char body[96];
    if (read_body(req, body, sizeof(body)) != ESP_OK) {
        return reply(req, false, "请求体为空");
    }

    const int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_auth_mux);
    const bool locked = now < s_auth_lock_until_ms;
    portEXIT_CRITICAL(&s_auth_mux);
    if (locked) {
        return reply(req, false, "尝试太频繁，请稍后再试");
    }

    char code[8];
    if (!form_get(body, "code", code, sizeof(code))) {
        return reply(req, false, "缺少验证码");
    }

    char want[7];
    web_code_get(want);
    if (strcmp(code, want) != 0) {
        portENTER_CRITICAL(&s_auth_mux);
        if (++s_auth_fails >= WEB_AUTH_MAX_FAIL) {
            s_auth_fails = 0;
            s_auth_lock_until_ms = esp_timer_get_time() / 1000 + WEB_AUTH_LOCK_MS;
        }
        portEXIT_CRITICAL(&s_auth_mux);
        return reply(req, false, "验证码错误或已过期");
    }

    portENTER_CRITICAL(&s_auth_mux);
    s_auth_fails = 0;
    portEXIT_CRITICAL(&s_auth_mux);

    char tok[WEB_TOKEN_LEN];
    token_issue(tok, sizeof(tok));

    char buf[96];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"msg\":\"\",\"token\":\"%s\"}", tok);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

/* ---------------------------------------------------------------- 页面 */

static esp_err_t h_index(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

/* ---------------------------------------------------------------- 状态 */

static const char *state_text(net_state_t st)
{
    switch (st) {
    case NET_ONLINE: return "外网正常";
    case NET_NEED_AUTH: return "需要认证";
    case NET_AUTHING: return "正在登录";
    case NET_AUTH_FAILED: return "认证失败";
    case NET_CHECKING: return "检测中";
    case NET_WIFI_CONNECTING: return "连接 WiFi 中";
    case NET_WIFI_DOWN: return "WiFi 未连接";
    case NET_NO_ACCOUNT: return "缺少认证账号";
    case NET_NEED_SETUP: return "尚未选择 WiFi";
    case NET_ERROR: return "外网不可达";
    case NET_BOOT:
    default: return "启动中";
    }
}

static const char *state_id(net_state_t st)
{
    switch (st) {
    case NET_ONLINE: return "ONLINE";
    case NET_NEED_AUTH: return "NEED_AUTH";
    case NET_AUTHING: return "AUTHING";
    case NET_AUTH_FAILED: return "AUTH_FAILED";
    case NET_CHECKING: return "CHECKING";
    case NET_WIFI_CONNECTING: return "WIFI_CONNECTING";
    case NET_WIFI_DOWN: return "WIFI_DOWN";
    case NET_NO_ACCOUNT: return "NO_ACCOUNT";
    case NET_NEED_SETUP: return "NEED_SETUP";
    case NET_ERROR: return "ERROR";
    case NET_BOOT:
    default: return "BOOT";
    }
}

static esp_err_t h_state(httpd_req_t *req)
{
    net_status_t st;
    net_guard_get(&st);

    const int64_t now = esp_timer_get_time() / 1000;
    int64_t next_in = st.next_check_ms - now;
    if (next_in < 0) {
        next_in = 0;
    }

    char ssid[80], target[80], ip[32], acct[80], portal[96], detail[160];
    json_esc(ssid, sizeof(ssid), st.ssid);
    json_esc(target, sizeof(target), st.target);
    json_esc(ip, sizeof(ip), st.ip);
    json_esc(acct, sizeof(acct), st.account);
    json_esc(portal, sizeof(portal), st.portal);
    json_esc(detail, sizeof(detail), st.detail);

    batt_info_t bat;
    batt_mgr_get(&bat);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char head[640];
    snprintf(head, sizeof(head),
             "{\"state\":\"%s\",\"stext\":\"%s\",\"ssid\":\"%s\",\"target\":\"%s\","
             "\"ip\":\"%s\",\"account\":\"%s\",\"auth_count\":%d,\"next_in_ms\":%lld,"
             "\"uptime_ms\":%lld,\"detail\":\"%s\",\"portal\":\"%s\",\"sd\":%s,\"ui\":%u,"
             "\"stage\":%d,"
             "\"bat_present\":%s,\"bat_pct\":%d,\"bat_mv\":%d,\"bat_usb\":%s,"
             "\"bat_charging\":%s,\"bat_min\":%d,\"nets\":[",
             state_id(st.state), state_text(st.state), ssid, target, ip, acct,
             st.auth_count, (long long)next_in, (long long)(now - st.boot_ms),
             detail, portal, db_sd_present() ? "true" : "false",
             (unsigned)ui_tick_count(), ui_setup_scan_stage(),
             bat.present ? "true" : "false", bat.pct, bat.mv,
             bat.usb ? "true" : "false", bat.charging ? "true" : "false",
             bat.minutes_left);

    esp_err_t err = httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
    if (err != ESP_OK) {
        return err;
    }

    /* 注意：必须在 db_read_begin() 之前取 active，db_active_ssid() 自己会加锁，
     * 在持锁状态下再调就是死锁（httpd 任务会挂住，后续请求全部超时）。 */
    char active[33];
    db_active_ssid(active, sizeof(active));

    db_read_begin();

    const int n = db_net_count();
    for (int i = 0; i < n; i++) {
        const db_net_t *net = db_net_at(i);
        if (net == NULL) {
            continue;
        }

        char essid[80], ecur[80];
        json_esc(essid, sizeof(essid), net->ssid);
        json_esc(ecur, sizeof(ecur), net->current);

        char buf[384];
        snprintf(buf, sizeof(buf),
                 "%s{\"ssid\":\"%s\",\"active\":%s,\"current\":\"%s\",\"has_pass\":%s,\"accts\":[",
                 (i > 0) ? "," : "", essid,
                 (strcmp(active, net->ssid) == 0) ? "true" : "false",
                 ecur, net->wifi_pass[0] ? "true" : "false");
        err = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
        if (err != ESP_OK) {
            db_read_end();
            return err;
        }

        for (int a = 0; a < net->acct_count; a++) {
            char euser[80];
            json_esc(euser, sizeof(euser), net->acct[a].user);
            snprintf(buf, sizeof(buf), "%s{\"user\":\"%s\"}", (a > 0) ? "," : "", euser);
            err = httpd_resp_send_chunk(req, buf, HTTPD_RESP_USE_STRLEN);
            if (err != ESP_OK) {
                db_read_end();
                return err;
            }
        }
        err = httpd_resp_send_chunk(req, "]}", 2);
        if (err != ESP_OK) {
            db_read_end();
            return err;
        }
    }
    db_read_end();

    /* 必须补一个空 chunk 作为 chunked 响应的结束块，否则浏览器 fetch 会
     * 一直等 body 结束，页面永远停在「读取中」（curl 看不出来，它对 EOF 宽容）。 */
    err = httpd_resp_send_chunk(req, "]}", 2);
    if (err != ESP_OK) {
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ---------------------------------------------------------------- 写操作 */

static bool net_exists(const char *ssid)
{
    db_read_begin();
    const bool ok = db_find(ssid) != NULL;
    db_read_end();
    return ok;
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    char body[1024];
    if (!authed_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    char ssid[DB_SSID_LEN], pass[DB_PASS_LEN], use[8];
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return reply(req, false, "WiFi 名称缺失、格式错误或过长");
    }
    bool has_pass = false;
    if (!form_optional(body, "pass", pass, sizeof(pass), &has_pass) ||
        !form_optional(body, "use", use, sizeof(use), NULL)) {
        return reply(req, false, "参数格式错误或过长");
    }

    const bool exists = net_exists(ssid);
    const bool want_use = (use[0] == '1');

    if (!exists) {
        /* 新网络：先存下来试连，连上才切过去 —— 否则加错 SSID 会把正在用的
         * 网络踢掉，网页也跟着失联。 */
        if (!db_ensure_net(ssid, has_pass ? pass : "")) {
            return reply(req, false, "网络数量已达上限或存储失败");
        }
        net_guard_try_ssid(ssid);
    } else if (has_pass) {
        if (!db_ensure_net(ssid, pass)) {
            return reply(req, false, "保存失败，请检查存储状态");
        }
        net_guard_try_ssid(ssid);
    } else if (want_use) {
        if (db_set_active_ssid(ssid) != ESP_OK) {
            return reply(req, false, "保存失败，请检查存储状态");
        }
        net_guard_trigger();
    } else {
        net_guard_trigger();
    }

    ESP_LOGI(TAG, "wifi %s -> %s", ssid, exists ? "update" : "add");
    return reply(req, true, "ok");
}

static esp_err_t h_acct(httpd_req_t *req)
{
    char body[1024];
    if (!authed_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    char ssid[DB_SSID_LEN], user[DB_USER_LEN], pass[DB_PASS_LEN], use[8];
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return reply(req, false, "WiFi 名称缺失、格式错误或过长");
    }
    if (!form_get(body, "user", user, sizeof(user)) || user[0] == '\0') {
        return reply(req, false, "账号缺失、格式错误或过长");
    }
    if (!form_optional(body, "pass", pass, sizeof(pass), NULL) ||
        !form_optional(body, "use", use, sizeof(use), NULL)) {
        return reply(req, false, "参数格式错误或过长");
    }

    esp_err_t err = db_upsert_acct(ssid, user, pass);
    if (err != ESP_OK) {
        return reply(req, false, "账号数量已达上限或存储失败");
    }
    if (use[0] == '1') {
        db_set_current_acct(ssid, user);
        db_set_active_ssid(ssid);
        net_guard_switch_account(ssid, user);
    } else {
        net_guard_trigger();
    }

    ESP_LOGI(TAG, "acct %s@%s saved", user, ssid);
    return reply(req, true, "ok");
}

static esp_err_t h_use(httpd_req_t *req)
{
    char body[512];
    if (!authed_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    char ssid[DB_SSID_LEN], user[DB_USER_LEN];
    if (!form_get(body, "ssid", ssid, sizeof(ssid)) ||
        !form_get(body, "user", user, sizeof(user))) {
        return reply(req, false, "参数缺失、格式错误或过长");
    }

    if (db_set_current_acct(ssid, user) != ESP_OK) {
        return reply(req, false, "没有这个账号");
    }
    db_set_active_ssid(ssid);
    net_guard_switch_account(ssid, user);
    return reply(req, true, "ok");
}

static esp_err_t h_remove(httpd_req_t *req)
{
    char body[512];
    if (!authed_body(req, body, sizeof(body))) {
        return ESP_OK;
    }

    char type[8], ssid[DB_SSID_LEN], user[DB_USER_LEN];
    if (!form_get(body, "type", type, sizeof(type))) {
        return reply(req, false, "参数缺失、格式错误或过长");
    }
    if (!form_get(body, "ssid", ssid, sizeof(ssid))) {
        return reply(req, false, "WiFi 名称缺失、格式错误或过长");
    }

    esp_err_t err;
    if (strcmp(type, "wifi") == 0) {
        err = db_remove_net(ssid);
    } else {
        if (!form_get(body, "user", user, sizeof(user))) {
            return reply(req, false, "账号缺失、格式错误或过长");
        }
        err = db_remove_acct(ssid, user);
    }

    net_guard_trigger();
    if (err == ESP_ERR_INVALID_STATE) {
        return reply(req, false, "当前网络正在使用，请先切换网络");
    }
    return reply(req, err == ESP_OK, err == ESP_OK ? "ok" : "条目不存在或存储失败");
}

static esp_err_t h_check(httpd_req_t *req)
{
    char body[64];
    if (!authed_body(req, body, sizeof(body))) {
        return ESP_OK;
    }
    net_guard_trigger();
    return reply(req, true, "ok");
}

static esp_err_t h_reload(httpd_req_t *req)
{
    char body[64];
    if (!authed_body(req, body, sizeof(body))) {
        return ESP_OK;
    }
    const esp_err_t err = db_reload();
    net_guard_trigger();
    return reply(req, err == ESP_OK, err == ESP_OK ? "ok" : "读取失败");
}

static esp_err_t h_restore(httpd_req_t *req)
{
    char body[64];
    if (!authed_body(req, body, sizeof(body))) {
        return ESP_OK;
    }
    const esp_err_t err = db_restore_from_sd();
    net_guard_trigger();
    return reply(req, err == ESP_OK, err == ESP_OK ? "ok" : "SD 卡上没有可用配置");
}

/* ---------------------------------------------------------------- 启动 */

esp_err_t web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = APP_WEB_PORT;
    cfg.max_uri_handlers = 12;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t srv = NULL;
    esp_err_t err = httpd_start(&srv, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/",            .method = HTTP_GET,  .handler = h_index},
        {.uri = "/api/state",   .method = HTTP_GET,  .handler = h_state},
        {.uri = "/api/auth",    .method = HTTP_POST, .handler = h_auth},
        {.uri = "/api/session", .method = HTTP_POST, .handler = h_session},
        {.uri = "/api/wifi",    .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/acct",    .method = HTTP_POST, .handler = h_acct},
        {.uri = "/api/use",     .method = HTTP_POST, .handler = h_use},
        {.uri = "/api/remove",  .method = HTTP_POST, .handler = h_remove},
        {.uri = "/api/check",   .method = HTTP_POST, .handler = h_check},
        {.uri = "/api/reload",  .method = HTTP_POST, .handler = h_reload},
        {.uri = "/api/restore", .method = HTTP_POST, .handler = h_restore},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        err = httpd_register_uri_handler(srv, &routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i].uri, esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "config panel on http://<板子IP>:%d", APP_WEB_PORT);
    return ESP_OK;
}
