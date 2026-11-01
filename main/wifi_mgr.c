#include "wifi_mgr.h"

#include "sys_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

static const char *TAG = "wifi_mgr";

#define BIT_IP BIT0

static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif;
static char s_ip[16];
static char s_ssid[33];
static volatile bool s_connected;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "sta started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e = (const wifi_event_sta_connected_t *)data;
        char ssid[33] = {0};
        const size_t n = (e->ssid_len < sizeof(ssid) - 1) ? e->ssid_len : sizeof(ssid) - 1;
        memcpy(ssid, e->ssid, n);
        strlcpy(s_ssid, ssid, sizeof(s_ssid));
        ESP_LOGI(TAG, "associated with [%s]", s_ssid);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = (const wifi_event_sta_disconnected_t *)data;
        s_connected = false;
        s_ip[0] = '\0';
        xEventGroupClearBits(s_evt, BIT_IP);
        ESP_LOGW(TAG, "disconnected from [%s], reason=%d", s_ssid, e ? e->reason : -1);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "got ip %s (ssid [%s])", s_ip, s_ssid);
        xEventGroupSetBits(s_evt, BIT_IP);
        /* 联网成功就校一次时（幂等，换网/重连后会重新校） */
        sys_time_sntp_start();
    }
}

esp_err_t wifi_mgr_init(void)
{
    s_evt = xEventGroupCreate();
    if (s_evt == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    if (pass != NULL) {
        strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    }

    /* 有密码按 WPA2 起步（WPA3 让 supplicant 自己协商，路由器多是 WPA2/3 混合），
     * 没密码就是开放网络，threshold 必须放开到 OPEN，否则连不上。 */
    if (wc.sta.password[0] == '\0') {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    xEventGroupClearBits(s_evt, BIT_IP);
    s_connected = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_wifi_disconnect(); /* 换网时先断，避免旧连接卡住重连计数 */
    err = esp_wifi_connect();
    ESP_LOGI(TAG, "connect [%s] (%s) -> %s", ssid,
             wc.sta.password[0] ? "wpa2/3" : "open", esp_err_to_name(err));
    return err;
}

void wifi_mgr_disconnect(void)
{
    esp_wifi_disconnect();
    s_connected = false;
    xEventGroupClearBits(s_evt, BIT_IP);
}

void wifi_mgr_reconnect(void)
{
    if (s_ssid[0] != '\0') {
        ESP_LOGI(TAG, "reconnect to [%s]", s_ssid);
    }
    esp_wifi_connect();
}

bool wifi_mgr_connected(void)
{
    return s_connected;
}

bool wifi_mgr_wait_ip(uint32_t timeout_ms)
{
    const EventBits_t bits =
        xEventGroupWaitBits(s_evt, BIT_IP, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & BIT_IP) != 0;
}

void wifi_mgr_ip(char *out, size_t cap)
{
    strlcpy(out, s_ip, cap);
}

void wifi_mgr_ssid(char *out, size_t cap)
{
    strlcpy(out, s_ssid, cap);
}

int wifi_mgr_scan(wifi_ap_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    wifi_scan_config_t cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start failed: %s", esp_err_to_name(err));
        return -1;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) {
        return 0;
    }

    /* 记录数可能远多于 max，按需分配再截断 */
    wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        return -1;
    }

    uint16_t got = num;
    err = esp_wifi_scan_get_ap_records(&got, recs);
    if (err != ESP_OK) {
        free(recs);
        return -1;
    }

    int n = 0;
    for (uint16_t i = 0; i < got; i++) {
        if (recs[i].ssid[0] == '\0') {
            continue; /* 隐藏网络 */
        }

        /* 同名 SSID 只留信号最强的那条 */
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (strcmp(out[k].ssid, (const char *)recs[i].ssid) == 0) {
                if (recs[i].rssi > out[k].rssi) {
                    out[k].rssi = recs[i].rssi;
                    out[k].authmode = recs[i].authmode;
                }
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }

        if (n >= max) {
            continue;
        }
        strlcpy(out[n].ssid, (const char *)recs[i].ssid, sizeof(out[n].ssid));
        out[n].rssi = recs[i].rssi;
        out[n].authmode = (uint8_t)recs[i].authmode;
        n++;
    }

    free(recs);

    /* 简单插入排序：信号强的在前 */
    for (int i = 1; i < n; i++) {
        wifi_ap_t key = out[i];
        int j = i - 1;
        while (j >= 0 && out[j].rssi < key.rssi) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }

    /* 连着的那个排最前，方便一眼看到 */
    for (int i = 0; i < n; i++) {
        if (strcmp(out[i].ssid, s_ssid) == 0) {
            wifi_ap_t key = out[i];
            memmove(&out[1], &out[0], (size_t)i * sizeof(wifi_ap_t));
            out[0] = key;
            break;
        }
    }

    ESP_LOGI(TAG, "scan found %d AP(s)", n);
    return n;
}
