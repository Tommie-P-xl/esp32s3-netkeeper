/*
 * SRun Portal：探测 + 登录。
 *
 * 探测思路（不依赖任何硬编码的 Portal IP）：
 *   1. GET 一个期望 204 的地址（generate_204）。
 *        - 204            -> 已联网
 *        - 302/302-like   -> 被 Portal 劫持，Location 里就是登录页地址 + ac_id
 *        - 连不上         -> 链路或 DNS 有问题，再试兜底 Portal
 *   2. 拿着 Portal 地址走 SRun 经典登录流程。
 *
 * ac_id 与 Portal IP 全部从 302 的 Location 里现取，换校区/换 Portal 也能自己适应。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char portal_base[64]; /* "http://x.x.x.x"，302 自动发现 */
    char ac_id[8];        /* "3" */
    char detail[96];      /* 给人看的一句话 */
} portal_info_t;

typedef enum {
    PROBE_ONLINE = 0,  /* 外网通 */
    PROBE_NEED_AUTH,   /* 被劫持到 Portal，需要认证 */
    PROBE_NO_LINK,     /* Portal 都摸不到（WiFi 掉了 / 网关不通） */
} probe_result_t;

/**
 * @brief 探测当前是否已联网，未联网时顺便把 Portal 地址与 ac_id 抓出来。
 */
probe_result_t srun_probe(portal_info_t *pi);

/** @brief 查询 Portal 实际在线账号；未在线返回 ESP_ERR_NOT_FOUND。 */
esp_err_t srun_online_user(const portal_info_t *pi, char *user, size_t cap);

/** @brief 注销实际在线账号，供同一 WiFi 上切换认证账号使用。 */
bool srun_logout(const portal_info_t *pi, const char *user, char *detail, size_t cap);

typedef enum {
    LOGIN_OK = 0,   /* 认证成功 */
    LOGIN_ALREADY,  /* 服务端说该 IP 已在线，等价于成功 */
    LOGIN_FAIL,
} login_result_t;

/**
 * @brief 走一遍 SRun 登录。
 *
 * @param detail 输出：失败原因或服务端返回的 suc_msg
 */
login_result_t srun_login(const portal_info_t *pi, const char *user, const char *pass,
                          const char *domain, char *detail, size_t detail_cap);

#ifdef __cplusplus
}
#endif
