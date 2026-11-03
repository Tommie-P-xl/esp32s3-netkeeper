/*
 * 极简 HTTP GET 封装。
 *
 * 只用得到两样东西：状态码 / Location 头 / 响应体（截断保存）。
 * 关键是 disable_auto_redirect —— 未认证时 Portal 靠 302 把浏览器拽到登录页，
 * 那个 302 的 Location 就是我们要抓的「Portal 地址 + ac_id」。
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HTTP_RESP_LOCATION_MAX 192
#define HTTP_RESP_BODY_MAX     1400

typedef struct {
    int status;                              /* HTTP 状态码，-1 表示没拿到 */
    char location[HTTP_RESP_LOCATION_MAX];   /* Location 头，没有则为空串 */
    char body[HTTP_RESP_BODY_MAX];           /* 响应体（截断，永远以 0 结尾） */
    size_t body_len;
} http_resp_t;

/**
 * @brief GET 一个 URL，不跟随跳转。
 *
 * @param timeout_ms 单次连接 + 读取的超时
 * @return ESP_OK 表示拿到了 HTTP 响应（状态码可能是 302/200 等）；
 *         ESP_FAIL / ESP_ERR_TIMEOUT 表示连不上或读超时。
 */
esp_err_t http_get_capture(const char *url, int timeout_ms, http_resp_t *out);

/**
 * @brief 把字符串做百分号编码（用于把 info/chksum 塞进查询串）。
 */
bool http_url_encode(const char *in, char *out, size_t cap);

/* 解码后检查容量；缺失返回 NOT_FOUND，非法编码和超长均拒绝，不返回截断值。 */
esp_err_t http_form_get(const char *body, const char *key, char *out, size_t cap);

/* JSON 字符串内容转义（不含外层引号），容量不足返回 false。 */
bool http_json_escape(const char *in, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
