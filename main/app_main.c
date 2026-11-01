/*
 * 校园网守护 —— ESP32-S3 + AMOLED 屏 + SD 卡配置库。
 *
 * 启动顺序（顺序有讲究）：
 *   nvs -> netif -> 事件循环           WiFi 的前提
 *   db_init()                          挂载 SD、读配置；没卡就只在内存里跑
 *   显示 -> UI                         先把界面点起来，用户能立刻看到状态
 *   wifi_mgr -> net_guard -> web       网络、守护、网页面板
 *
 * 一次烧录长期使用：WiFi 与账号都在 SD 卡的 /portal/net.cfg 里，改配置不用重烧。
 */
#include <stdio.h>

#include "app_config.h"
#include "db.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net_guard.h"
#include "nvs_flash.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "power_mgr.h"
#include "srun_crypto.h"
#include "sys_time.h"
#include "ui.h"
#include "ui_common.h"
#include "ui_setup.h"
#include "web.h"
#include "wifi_mgr.h"

static const char *TAG = "app";

/* 启动 2.5 秒后检查：没配网就直接把扫网页面推到用户面前 */
static void boot_hint(lv_timer_t *t)
{
    lv_timer_delete(t);

    if (!db_sd_present()) {
        ui_toast("未插 SD 卡，配置只存片上 Flash");
    }

    char active[DB_SSID_LEN] = "";
    db_active_ssid(active, sizeof(active));
    if (active[0] == '\0') {
        ui_setup_open_wifi();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Portal Guard booting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs re-init (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sys_time_init();   /* 东八区；校时等拿到 IP 后再做 */

#if APP_CRYPTO_SELFTEST
    srun_crypto_selftest();
#endif

    /* 配置库先起来：界面要显示"SD 有没有插入" */
    db_init();
    db_dump();

    lv_display_t *disp = power_mgr_display_start();
    if (disp != NULL) {
        bsp_display_backlight_on();

        ui_lock(-1);
        ui_create(disp);
        lv_timer_create(boot_hint, 2500, NULL);
        ui_unlock();

        power_mgr_start();
    } else {
        ESP_LOGE(TAG, "display init failed, UI disabled");
    }

    wifi_mgr_init();
    net_guard_start();
    web_start();

    ESP_LOGI(TAG, "boot done");
}
