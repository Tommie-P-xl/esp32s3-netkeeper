#include "srun_port.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "http_util.h"
#include "srun_crypto.h"

static const char *TAG = "srun";

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
    strlcpy(pi->portal_base, APP_PORTAL_FALLBACK_HOST, sizeof(pi->portal_base));
    strlcpy(pi->ac_id, APP_PORTAL_FALLBACK_ACID, sizeof(pi->ac_id));

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

probe_result_t srun_probe(portal_info_t *pi)
{
    http_resp_t *r = calloc(1, sizeof(http_resp_t));
    if (r == NULL) {
        return PROBE_NO_LINK;
    }

    memset(pi, 0, sizeof(*pi));
    strlcpy(pi->portal_base, APP_PORTAL_FALLBACK_HOST, sizeof(pi->portal_base));
    strlcpy(pi->ac_id, APP_PORTAL_FALLBACK_ACID, sizeof(pi->ac_id));

    probe_result_t res = PROBE_NO_LINK;

    /* 第一跳：主探测地址 */
    esp_err_t err = http_get_capture(APP_PROBE_URL_PRIMARY, APP_HTTP_TIMEOUT_MS, r);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "probe %s -> HTTP %d loc=[%s]",
                 APP_PROBE_URL_PRIMARY, r->status, r->location);

        if (r->status == 204) {
            strlcpy(pi->detail, "已联网", sizeof(pi->detail));
            res = PROBE_ONLINE;
        } else if (r->status >= 300 && r->status < 400) {
            parse_location(r->location, pi);
            snprintf(pi->detail, sizeof(pi->detail), "被劫持 -> %s", pi->portal_base);
            res = PROBE_NEED_AUTH;
        } else if (r->status == 200) {
            /* 有些 Portal 直接 200 返回登录页，URL 不变 */
            parse_location(r->location, pi);
            strlcpy(pi->detail, "返回登录页（200）", sizeof(pi->detail));
            res = PROBE_NEED_AUTH;
        }
    } else {
        ESP_LOGW(TAG, "probe primary failed: %s", esp_err_to_name(err));

        /* 第二跳：换个 204 地址，排除单个域名的问题 */
        err = http_get_capture(APP_PROBE_URL_SECONDARY, APP_HTTP_TIMEOUT_MS, r);
        if (err == ESP_OK && r->status == 204) {
            strlcpy(pi->detail, "已联网", sizeof(pi->detail));
            res = PROBE_ONLINE;
            goto done;
        }

        /* 第三跳：直接摸兜底 Portal，能通说明只是 DNS 挂了 */
        char url[128];
        snprintf(url, sizeof(url), "%s/srun_portal_pc?ac_id=%s&theme=pro",
                 APP_PORTAL_FALLBACK_HOST, APP_PORTAL_FALLBACK_ACID);
        err = http_get_capture(url, APP_HTTP_TIMEOUT_MS, r);
        if (err == ESP_OK && r->status > 0) {
            snprintf(pi->detail, sizeof(pi->detail), "兜底 Portal 可达（HTTP %d）", r->status);
            res = PROBE_NEED_AUTH;
        } else {
            strlcpy(pi->detail, "探不到 Portal", sizeof(pi->detail));
            res = PROBE_NO_LINK;
        }
    }

done:
    free(r);
    return res;
}

/* ---------------------------------------------------------------- login */

login_result_t srun_login(const portal_info_t *pi, const char *user, const char *pass,
                          const char *domain, char *detail, size_t detail_cap)
{
    char username[64];
    char url[1024];
    char hmd5[33];
    char info[640];
    char info_enc[900];
    char chksum[41];
    char json[384];
    char ip[20] = "";
    char token[80] = "";

    http_resp_t *r = calloc(1, sizeof(http_resp_t));
    if (r == NULL) {
        strlcpy(detail, "内存不足", detail_cap);
        return LOGIN_FAIL;
    }

    snprintf(username, sizeof(username), "%s%s", user, domain ? domain : "");

    /* --- 1. 取 challenge（同时从服务端视角拿 client_ip，NAT 后面也对） --- */
    snprintf(url, sizeof(url), "%s/cgi-bin/get_challenge?callback=jsonp&username=%s&ip=&_=%lld",
             pi->portal_base, username, (long long)(esp_timer_get_time() / 1000));

    esp_err_t err = http_get_capture(url, APP_HTTP_TIMEOUT_MS, r);
    if (err != ESP_OK) {
        snprintf(detail, detail_cap, "取 challenge 失败: %s", esp_err_to_name(err));
        goto fail;
    }

    {
        char *js = strip_jsonp(r->body);
        if (js == NULL) {
            snprintf(detail, detail_cap, "challenge 响应异常: %.60s", r->body);
            goto fail;
        }
        json_get(js, "challenge", token, sizeof(token));
        json_get(js, "client_ip", ip, sizeof(ip));
    }

    if (token[0] == '\0') {
        snprintf(detail, detail_cap, "challenge 为空，Portal 可能换了接口");
        goto fail;
    }
    ESP_LOGI(TAG, "challenge=%s client_ip=%s", token, ip);

    /* --- 2. hmd5 --- */
    srun_hmac_md5_hex(token, pass, hmd5);

    /* --- 3. info --- */
    snprintf(json, sizeof(json),
             "{\"username\":\"%s\",\"password\":\"%s\",\"ip\":\"%s\","
             "\"acid\":\"%s\",\"enc_ver\":\"srun_bx1\"}",
             username, pass, ip, pi->ac_id);
    srun_encode_info(json, token, info, sizeof(info));
    if (info[0] == '\0') {
        snprintf(detail, detail_cap, "info 生成失败");
        goto fail;
    }

    /* --- 4. chksum --- */
    {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s%s%s%s%s%s%s%s%s%s%s%s%s%s",
                 token, username,
                 token, hmd5,
                 token, pi->ac_id,
                 token, ip,
                 token, "200",
                 token, "1",
                 token, info);
        srun_sha1_hex(buf, strlen(buf), chksum);
    }

    /* --- 5. 发起登录 --- */
    http_url_encode(info, info_enc, sizeof(info_enc));
    snprintf(url, sizeof(url),
             "%s/cgi-bin/srun_portal?callback=jsonp&action=login&username=%s"
             "&password=%%7BMD5%%7D%s&os=Windows+NT&name=Windows&double_stack=0"
             "&chksum=%s&info=%s&ac_id=%s&ip=%s&n=200&type=1&_=%lld",
             pi->portal_base, username, hmd5, chksum, info_enc, pi->ac_id, ip,
             (long long)(esp_timer_get_time() / 1000));

    err = http_get_capture(url, APP_HTTP_TIMEOUT_MS + 4000, r);
    if (err != ESP_OK) {
        snprintf(detail, detail_cap, "登录请求失败: %s", esp_err_to_name(err));
        goto fail;
    }

    {
        char *js = strip_jsonp(r->body);
        if (js == NULL) {
            snprintf(detail, detail_cap, "登录响应异常(HTTP %d): %.60s", r->status, r->body);
            goto fail;
        }

        char err_code[24] = "";
        char suc[48] = "";
        char emsg[80] = "";
        json_get(js, "error", err_code, sizeof(err_code));
        json_get(js, "suc_msg", suc, sizeof(suc));
        json_get(js, "error_msg", emsg, sizeof(emsg));

        ESP_LOGI(TAG, "login resp: error=%s suc_msg=%s error_msg=%s", err_code, suc, emsg);

        login_result_t lr;
        if (strcmp(err_code, "ok") == 0) {
            if (strcmp(suc, "ip_already_online_error") == 0) {
                snprintf(detail, detail_cap, "该 IP 已在线");
                lr = LOGIN_ALREADY;
            } else {
                snprintf(detail, detail_cap, "登录成功 %s", suc);
                lr = LOGIN_OK;
            }
        } else {
            snprintf(detail, detail_cap, "%s %s", err_code, emsg);
            lr = LOGIN_FAIL;
        }
        free(r);
        return lr;
    }

fail:
    free(r);
    return LOGIN_FAIL;
}
