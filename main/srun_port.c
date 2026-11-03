#include "srun_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"
#include "srun_crypto.h"
#include "nvs.h"
#include "wifi_mgr.h"

static const char *TAG = "srun";

/* 只缓存发现过的 Portal 和对应 WiFi，不保存认证密码。在线重启后仍能注销旧账号。 */
static void portal_defaults(portal_info_t *pi)
{
    strlcpy(pi->portal_base, APP_PORTAL_FALLBACK_HOST, sizeof(pi->portal_base));
    strlcpy(pi->ac_id, APP_PORTAL_FALLBACK_ACID, sizeof(pi->ac_id));
    nvs_handle_t h;
    if (nvs_open("srun", NVS_READONLY, &h) != ESP_OK) return;
    char ssid[33] = "", saved[33] = "", host[64] = "", acid[8] = "";
    wifi_mgr_ssid(ssid, sizeof(ssid));
    size_t ns = sizeof(saved), nh = sizeof(host), na = sizeof(acid);
    if (nvs_get_str(h, "ssid", saved, &ns) == ESP_OK && !strcmp(ssid, saved) &&
        nvs_get_str(h, "host", host, &nh) == ESP_OK &&
        nvs_get_str(h, "acid", acid, &na) == ESP_OK && host[0] && acid[0]) {
        strlcpy(pi->portal_base, host, sizeof(pi->portal_base));
        strlcpy(pi->ac_id, acid, sizeof(pi->ac_id));
    }
    nvs_close(h);
}

static void portal_remember(const portal_info_t *pi)
{
    if (!strcmp(pi->portal_base, "http://0.0.0.0")) return;
    portal_info_t old = {0};
    portal_defaults(&old);
    if (!strcmp(old.portal_base, pi->portal_base) && !strcmp(old.ac_id, pi->ac_id)) return;
    nvs_handle_t h;
    if (nvs_open("srun", NVS_READWRITE, &h) != ESP_OK) return;
    char ssid[33] = "";
    wifi_mgr_ssid(ssid, sizeof(ssid));
    esp_err_t err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(h, "host", pi->portal_base);
    if (err == ESP_OK) err = nvs_set_str(h, "acid", pi->ac_id);
    if (err == ESP_OK) err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "Portal cache write failed: %s", esp_err_to_name(err));
    nvs_close(h);
}

/* -------------------------------------------------------------- helpers */

/* jsonp 响应形如 jQuery1234_567({...})，把最外层壳剥掉 */
static char *strip_jsonp(const char *body)
{
    static char buf[HTTP_RESP_BODY_MAX];

    if (body == NULL) {
        return NULL;
    }

    const char *start = strchr(body, '{');
    const char *end = strrchr(body, '}');
    if (start == NULL || end == NULL || end <= start) {
        return NULL;
    }

    size_t n = (size_t)(end - start) + 1;
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    memcpy(buf, start, n);
    buf[n] = '\0';
    return buf;
}

/* ESP-IDF v6.1 已不再随树提供 cJSON，而 SRun 的响应都是扁平对象，
 * 这里用最小的「取值」实现代替整套 JSON 库，少一个依赖。 */
static bool json_get(const char *json, const char *key, char *out, size_t cap)
{
    if (cap > 0) {
        out[0] = '\0';
    }
    if (json == NULL || key == NULL || cap == 0) {
        return false;
    }

    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(json, pat);
    if (p == NULL) {
        return false;
    }
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != ':') {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }

    size_t o = 0;
    if (*p == '"') {
        p++;
        while (*p != '\0' && *p != '"' && o + 1 < cap) {
            if (*p == '\\' && p[1] != '\0') {
                p++;
            }
            out[o++] = *p++;
        }
    } else {
        while (*p != '\0' && *p != ',' && *p != '}' && o + 1 < cap) {
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return true;
}

/* 从 Location 里抠出 http://host 与 ac_id */
static void parse_location(const char *loc, portal_info_t *pi)
{
    portal_defaults(pi);

    if (loc == NULL || loc[0] == '\0') {
        return;
    }

    if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
        const char *host_start = strstr(loc, "//");
        const char *slash = (host_start != NULL) ? strchr(host_start + 2, '/') : NULL;
        const size_t host_len = (slash != NULL)
                                    ? (size_t)(slash - loc)
                                    : strlen(loc);
        if (host_len > 0 && host_len < sizeof(pi->portal_base)) {
            memcpy(pi->portal_base, loc, host_len);
            pi->portal_base[host_len] = '\0';
        }
    }

    const char *ac = strstr(loc, "ac_id=");
    if (ac != NULL) {
        ac += 6;
        size_t i = 0;
        while (ac[i] >= '0' && ac[i] <= '9' && i < sizeof(pi->ac_id) - 1) {
            pi->ac_id[i] = ac[i];
            i++;
        }
        pi->ac_id[i] = '\0';
    }
}

/* ---------------------------------------------------------------- probe */

static probe_result_t probe_response(const http_resp_t *r, portal_info_t *pi)
{
    if (r->status == 204) {
        strlcpy(pi->detail, "已联网", sizeof(pi->detail));
        return PROBE_ONLINE;
    }
    if ((r->status >= 300 && r->status < 400) || r->status == 200) {
        parse_location(r->location, pi);
        snprintf(pi->detail, sizeof(pi->detail), "需要校园网认证（HTTP %d）", r->status);
        portal_remember(pi);
        return PROBE_NEED_AUTH;
    }
    return PROBE_NO_LINK;
}

probe_result_t srun_probe(portal_info_t *pi)
{
    /* 包括分配失败路径，调用方也始终拿到已初始化的状态。 */
    memset(pi, 0, sizeof(*pi));
    portal_defaults(pi);
    http_resp_t *r = calloc(1, sizeof(*r));
    if (!r) {
        strlcpy(pi->detail, "探测内存不足", sizeof(pi->detail));
        return PROBE_NO_LINK;
    }
    probe_result_t res = PROBE_NO_LINK;
    const char *urls[] = {APP_PROBE_URL_PRIMARY, APP_PROBE_URL_SECONDARY};
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); ++i) {
        const esp_err_t err = http_get_capture(urls[i], APP_HTTP_TIMEOUT_MS, r);
        if (err == ESP_OK) {
            res = probe_response(r, pi);
            if (res != PROBE_NO_LINK) goto done;
            ESP_LOGW(TAG, "probe HTTP %d, trying next endpoint", r->status);
        }
    }
    char url[160];
    snprintf(url, sizeof(url), "%s/srun_portal_pc?ac_id=%s&theme=pro", pi->portal_base, pi->ac_id);
    if (http_get_capture(url, APP_HTTP_TIMEOUT_MS, r) == ESP_OK &&
        (r->status == 200 || (r->status >= 300 && r->status < 400))) {
        strlcpy(pi->detail, "兜底 Portal 可达", sizeof(pi->detail));
        res = PROBE_NEED_AUTH;
    } else {
        strlcpy(pi->detail, "外网与 Portal 均不可达", sizeof(pi->detail));
    }
done:
    free(r);
    return res;
}

/* 查询服务端实际在线账号，避免把界面选中的账号当成已经登录的账号。 */
esp_err_t srun_online_user(const portal_info_t *pi, char *user, size_t cap)
{
    user[0] = '\0';
    if (!strcmp(pi->portal_base, "http://0.0.0.0")) return ESP_ERR_INVALID_STATE;
    http_resp_t *r = calloc(1, sizeof(*r));
    if (!r) return ESP_ERR_NO_MEM;
    char url[192];
    snprintf(url, sizeof(url), "%s/cgi-bin/rad_user_info?callback=jsonp&_=%lld",
             pi->portal_base, (long long)(esp_timer_get_time() / 1000));
    esp_err_t err = http_get_capture(url, APP_HTTP_TIMEOUT_MS, r);
    if (err == ESP_OK) {
        err = ESP_FAIL;
        if (r->status == 200) {
            char *js = strip_jsonp(r->body);
            if (js && (json_get(js, "user_name", user, cap) || json_get(js, "username", user, cap)) && user[0]) {
                err = ESP_OK;
            } else if (!js && strchr(r->body, ',')) {
                const size_t n = (size_t)(strchr(r->body, ',') - r->body);
                if (n && n < cap && r->body[0] != '<') {
                    memcpy(user, r->body, n); user[n] = '\0'; err = ESP_OK;
                }
            } else if (strstr(r->body, "not_online")) {
                err = ESP_ERR_NOT_FOUND;
            }
        }
    }
    free(r);
    return err;
}

/* 注销前用 challenge 取得服务端看到的 IP，兼容设备在 NAT 后的情况。 */
bool srun_logout(const portal_info_t *pi, const char *user, char *detail, size_t cap)
{
    http_resp_t *r = calloc(1, sizeof(*r));
    if (!r) { strlcpy(detail, "内存不足", cap); return false; }
    char encoded[192], url[512], ip[20] = "";
    bool ok = false;
    if (!http_url_encode(user, encoded, sizeof(encoded))) goto done;
    snprintf(url, sizeof(url), "%s/cgi-bin/get_challenge?callback=jsonp&username=%s&ip=&_=%lld",
             pi->portal_base, encoded, (long long)(esp_timer_get_time() / 1000));
    if (http_get_capture(url, APP_HTTP_TIMEOUT_MS, r) != ESP_OK) goto done;
    char *js = strip_jsonp(r->body);
    if (!js || !json_get(js, "client_ip", ip, sizeof(ip)) || !ip[0]) goto done;
    /* client_ip 必须是地址，不能把任意响应文本拼入查询串。 */
    char ip_enc[60];
    if (!http_url_encode(ip, ip_enc, sizeof(ip_enc))) goto done;
    snprintf(url, sizeof(url), "%s/cgi-bin/srun_portal?callback=jsonp&action=logout&username=%s&ac_id=%s&ip=%s&_=%lld",
             pi->portal_base, encoded, pi->ac_id, ip_enc, (long long)(esp_timer_get_time() / 1000));
    if (http_get_capture(url, APP_HTTP_TIMEOUT_MS, r) != ESP_OK) goto done;
    js = strip_jsonp(r->body);
    char error[32] = "", msg[80] = "";
    if (js) {
        json_get(js, "error", error, sizeof(error));
        json_get(js, "suc_msg", msg, sizeof(msg));
        ok = r->status == 200 && (!strcmp(error, "ok") || !strcmp(msg, "logout_ok") ||
                                  !strcmp(error, "not_online_error"));
    }
done:
    strlcpy(detail, ok ? "旧账号已退出" : "退出旧账号失败，未切换认证账号", cap);
    free(r);
    return ok;
}

/* 编码后的字段可显著变长，工作区放堆上，避免增加守护任务栈的压力。 */
typedef struct {
    http_resp_t response;
    char username[64], user_enc[192], user_json[384], pass_json[384];
    char url[5120], json[1024], info[1400], info_enc[4200], checksum_input[2400];
    char hmd5[33], chksum[41], ip[20], token[80];
} login_workspace_t;

login_result_t srun_login(const portal_info_t *pi, const char *user, const char *pass,
                         const char *domain, char *detail, size_t detail_cap)
{
    login_workspace_t *w = calloc(1, sizeof(*w));
    if (!w) { strlcpy(detail, "内存不足", detail_cap); return LOGIN_FAIL; }
    login_result_t result = LOGIN_FAIL;
    strlcpy(detail, "认证参数过长或格式错误", detail_cap);
    int n = snprintf(w->username, sizeof(w->username), "%s%s", user, domain ? domain : "");
    if (n < 0 || (size_t)n >= sizeof(w->username) ||
        !http_url_encode(w->username, w->user_enc, sizeof(w->user_enc)) ||
        !http_json_escape(w->username, w->user_json, sizeof(w->user_json)) ||
        !http_json_escape(pass, w->pass_json, sizeof(w->pass_json))) goto done;
    n = snprintf(w->url, sizeof(w->url), "%s/cgi-bin/get_challenge?callback=jsonp&username=%s&ip=&_=%lld",
                 pi->portal_base, w->user_enc, (long long)(esp_timer_get_time() / 1000));
    if (n < 0 || (size_t)n >= sizeof(w->url)) goto done;
    esp_err_t err = http_get_capture(w->url, APP_HTTP_TIMEOUT_MS, &w->response);
    if (err != ESP_OK) { snprintf(detail, detail_cap, "取 challenge 失败: %s", esp_err_to_name(err)); goto done; }
    char *js = strip_jsonp(w->response.body);
    if (!js || w->response.status != 200) { strlcpy(detail, "challenge 响应异常", detail_cap); goto done; }
    json_get(js, "challenge", w->token, sizeof(w->token));
    json_get(js, "client_ip", w->ip, sizeof(w->ip));
    if (!w->token[0] || !w->ip[0] || strspn(w->ip, "0123456789.:") != strlen(w->ip)) {
        strlcpy(detail, "challenge 或客户端 IP 无效", detail_cap); goto done;
    }
    srun_hmac_md5_hex(w->token, pass, w->hmd5);
    n = snprintf(w->json, sizeof(w->json),
                 "{\"username\":\"%s\",\"password\":\"%s\",\"ip\":\"%s\",\"acid\":\"%s\",\"enc_ver\":\"srun_bx1\"}",
                 w->user_json, w->pass_json, w->ip, pi->ac_id);
    if (n < 0 || (size_t)n >= sizeof(w->json)) goto done;
    srun_encode_info(w->json, w->token, w->info, sizeof(w->info));
    if (!w->info[0]) { strlcpy(detail, "info 生成失败", detail_cap); goto done; }
    n = snprintf(w->checksum_input, sizeof(w->checksum_input), "%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                 w->token, w->username, w->token, w->hmd5, w->token, pi->ac_id,
                 w->token, w->ip, w->token, "200", w->token, "1", w->token, w->info);
    if (n < 0 || (size_t)n >= sizeof(w->checksum_input)) goto done;
    srun_sha1_hex(w->checksum_input, strlen(w->checksum_input), w->chksum);
    if (!http_url_encode(w->info, w->info_enc, sizeof(w->info_enc))) goto done;
    n = snprintf(w->url, sizeof(w->url),
                 "%s/cgi-bin/srun_portal?callback=jsonp&action=login&username=%s"
                 "&password=%%7BMD5%%7D%s&os=Windows+NT&name=Windows&double_stack=0"
                 "&chksum=%s&info=%s&ac_id=%s&ip=%s&n=200&type=1&_=%lld",
                 pi->portal_base, w->user_enc, w->hmd5, w->chksum, w->info_enc, pi->ac_id, w->ip,
                 (long long)(esp_timer_get_time() / 1000));
    if (n < 0 || (size_t)n >= sizeof(w->url)) goto done;
    err = http_get_capture(w->url, APP_HTTP_TIMEOUT_MS + 4000, &w->response);
    if (err != ESP_OK) { snprintf(detail, detail_cap, "登录请求失败: %s", esp_err_to_name(err)); goto done; }
    js = strip_jsonp(w->response.body);
    if (!js || w->response.status != 200) { strlcpy(detail, "登录响应异常", detail_cap); goto done; }
    char error[24] = "", suc[48] = "", message[80] = "";
    json_get(js, "error", error, sizeof(error));
    json_get(js, "suc_msg", suc, sizeof(suc));
    json_get(js, "error_msg", message, sizeof(message));
    if (!strcmp(error, "ok")) {
        result = !strcmp(suc, "ip_already_online_error") ? LOGIN_ALREADY : LOGIN_OK;
        strlcpy(detail, result == LOGIN_ALREADY ? "该 IP 已在线" : "登录成功", detail_cap);
        portal_remember(pi);
    } else {
        snprintf(detail, detail_cap, "%s %s", error, message);
    }
done:
    free(w);
    return result;
}
