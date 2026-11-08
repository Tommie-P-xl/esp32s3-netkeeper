#include "http_util.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "http";

#define UA_STR "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " \
               "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"

static esp_err_t capture_header(esp_http_client_event_t *event)
{
    if (event->event_id == HTTP_EVENT_ON_HEADER && event->user_data &&
        event->header_key && event->header_value &&
        strcasecmp(event->header_key, "Location") == 0) {
        http_resp_t *out = event->user_data;
        /* 截断跳转地址不能用于认证发现，保留为空让上层继续其它探测。 */
        if (strlcpy(out->location, event->header_value, sizeof(out->location)) >= sizeof(out->location)) {
            out->location[0] = '\0';
        }
    }
    return ESP_OK;
}

esp_err_t http_get_capture(const char *url, int timeout_ms, http_resp_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->status = -1;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = timeout_ms,
        .disable_auto_redirect = true,
        .event_handler = capture_header,
        .user_data = out,
        /* 2048 不是随手写的：SRun 的登录响应会带一串 Set-Cookie，实测响应头
         * 1226 字节，512 会直接报 ESP_ERR_HTTP_HEADER_TOO_LONG。 */
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (cli == NULL) {
        ESP_LOGE(TAG, "init failed: %s", url);
        return ESP_FAIL;
    }

    esp_http_client_set_header(cli, "User-Agent", UA_STR);
    esp_http_client_set_header(cli, "Accept", "*/*");
    esp_http_client_set_header(cli, "Connection", "close");

    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed (%s): %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(cli);
        return err;
    }

    if (esp_http_client_fetch_headers(cli) < 0) {
        ESP_LOGW(TAG, "fetch headers failed: %s", url);
        esp_http_client_cleanup(cli);
        return ESP_FAIL;
    }

    out->status = esp_http_client_get_status_code(cli);

    /* 把 body 读干净：只保留前 HTTP_RESP_BODY_MAX-1 字节，其余丢弃，
     * 这样连接能正常关闭，不会卡在 TIME_WAIT 里。 */
    char chunk[256];
    for (;;) {
        int r = esp_http_client_read(cli, chunk, sizeof(chunk));
        if (r <= 0) {
            break;
        }
        const size_t room = HTTP_RESP_BODY_MAX - 1 - out->body_len;
        if (room > 0) {
            const size_t n = ((size_t)r < room) ? (size_t)r : room;
            memcpy(out->body + out->body_len, chunk, n);
            out->body_len += n;
            out->body[out->body_len] = '\0';
        }
    }

    esp_http_client_close(cli);
    esp_http_client_cleanup(cli);

    ESP_LOGD(TAG, "GET %s -> %d, loc=[%s], body=%u B",
             url, out->status, out->location, (unsigned)out->body_len);
    return ESP_OK;
}

bool http_url_encode(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (cap == 0) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)in; *p != '\0'; p++) {
        const unsigned char c = *p;
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                          c == '.' || c == '~';

        if (safe) {
            if (o + 1 >= cap) {
                out[0] = '\0';
                return false;
            }
            out[o++] = (char)c;
        } else {
            if (o + 3 >= cap) {
                out[0] = '\0';
                return false;
            }
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0F];
        }
    }
    out[o] = '\0';
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

esp_err_t http_form_get(const char *body, const char *key, char *out, size_t cap)
{
    if (!body || !key || !out || cap == 0) return ESP_ERR_INVALID_ARG;
    out[0] = '\0';
    const size_t klen = strlen(key);
    for (const char *p = body; *p;) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > klen && !strncmp(p, key, klen) && p[klen] == '=') {
            size_t n = 0;
            for (p += klen + 1; p < end; ++p) {
                unsigned char c = (unsigned char)*p;
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (end - p < 3 || hex_value(p[1]) < 0 || hex_value(p[2]) < 0) {
                        out[0] = '\0';
                        return ESP_ERR_INVALID_ARG;
                    }
                    c = (unsigned char)((hex_value(p[1]) << 4) | hex_value(p[2]));
                    p += 2;
                }
                if (!c || n + 1 >= cap) {
                    out[0] = '\0';
                    return c ? ESP_ERR_INVALID_SIZE : ESP_ERR_INVALID_ARG;
                }
                out[n++] = (char)c;
            }
            out[n] = '\0';
            return ESP_OK;
        }
        p = *end ? end + 1 : end;
    }
    return ESP_ERR_NOT_FOUND;
}

bool http_json_escape(const char *in, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    if (!cap) return false;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        const size_t need = *p < 0x20 ? 6 : ((*p == '"' || *p == '\\') ? 2 : 1);
        if (n + need >= cap) { out[0] = '\0'; return false; }
        if (*p < 0x20) {
            out[n++] = '\\'; out[n++] = 'u'; out[n++] = '0'; out[n++] = '0';
            out[n++] = hex[*p >> 4]; out[n++] = hex[*p & 15];
        } else {
            if (need == 2) out[n++] = '\\';
            out[n++] = (char)*p;
        }
    }
    out[n] = '\0';
    return true;
}
