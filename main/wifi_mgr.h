/*
 * WiFi 管理：把「连哪个网、信号如何、IP 多少」从 net_guard 里拆出来。
 *
 * 这样做是为了让"未联网时扫列表选网"这条 UI 流程能独立工作 —— 它只跟
 * wifi_mgr 打交道，不关心 Portal 认证。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode; /* wifi_auth_mode_t */
} wifi_ap_t;

typedef struct {
    bool connected; /* 已取得 IP；未连接时 ssid/ip 为空 */
    char ssid[33];
    char ip[16];
} wifi_link_t;

/** @brief 在同一临界区内复制连接状态、SSID 和 IP，不访问配置库或显示。 */
void wifi_mgr_get_link(wifi_link_t *out);

/** @brief 初始化 esp_wifi（STA 模式，不自动连）。 */
esp_err_t wifi_mgr_init(void);

/**
 * @brief 连到指定 WiFi。
 *
 * 开放网络传空密码即可（threshold 会自动放开）。
 */
esp_err_t wifi_mgr_connect(const char *ssid, const char *pass);

void wifi_mgr_disconnect(void);

/** @brief 当前是否已拿到 IP。 */
bool wifi_mgr_connected(void);

/** @brief 阻塞等待拿到 IP，返回 false 表示超时。 */
bool wifi_mgr_wait_ip(uint32_t timeout_ms);

void wifi_mgr_ip(char *out, size_t cap);
void wifi_mgr_ssid(char *out, size_t cap);

/** @brief 已连接且取得 IP 时，复制实际关联网络及正在使用的密码。 */
bool wifi_mgr_current_credentials(char *ssid, size_t scap, char *pass, size_t pcap);

/** @brief 掉线后由 net_guard 调用，重新发起连接。 */
void wifi_mgr_reconnect(void);

/**
 * @brief 主动扫描，阻塞约 3~6 秒。
 *
 * 注意：扫描期间会短暂打断当前连接（esp_wifi 的固有限制），所以只在用户
 * 打开"网络"页时调用。
 * @return 去重排序后的 AP 个数（信号强的在前）
 */
int wifi_mgr_scan(wifi_ap_t *out, int max);

#ifdef __cplusplus
}
#endif
