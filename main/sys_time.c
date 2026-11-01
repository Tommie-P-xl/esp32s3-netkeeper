#include "sys_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "time";

/* 2024-01-01 UTC：之前的时间一定是没同步过的 1970/2000 年 */
#define TIME_VALID_EPOCH   1704067200LL

static bool s_sntp_inited;
static volatile bool s_synced;

static void sync_cb(struct timeval *tv)
{
    (void)tv;
    s_synced = true;
    char buf[8];
    sys_time_str(buf, sizeof(buf));
    ESP_LOGI(TAG, "SNTP synced -> %s", buf);
}

void sys_time_init(void)
{
    /* 固定东八区；用 POSIX TZ 串，不依赖 tzdata */
    setenv("TZ", "CST-8", 1);
    tzset();
}

void sys_time_sntp_start(void)
{
    if (!s_sntp_inited) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
        cfg.wait_for_sync = false;   /* 不阻塞，sync_cb 通知 */
        cfg.sync_cb = sync_cb;
        cfg.smooth_sync = false;
        if (esp_netif_sntp_init(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "SNTP init failed");
            return;
        }
        s_sntp_inited = true;
        ESP_LOGI(TAG, "SNTP started (ntp.aliyun.com)");
        return;
    }
    /* 已经初始化过：换网/重连后强制立刻重新对时 */
    esp_netif_sntp_start();
}

bool sys_time_valid(void)
{
    return (long long)time(NULL) >= TIME_VALID_EPOCH;
}

void sys_time_str(char *out, int cap)
{
    const time_t now = time(NULL);
    if ((long long)now < TIME_VALID_EPOCH) {
        snprintf(out, (size_t)cap, "--:--");
        return;
    }
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(out, (size_t)cap, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}
