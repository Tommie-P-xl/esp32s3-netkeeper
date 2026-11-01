#include "power_mgr.h"

#include <stdint.h>

#include "esp_log.h"
#include "esp_err.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "app_config.h"

static const char *TAG = "power_mgr";

/* ---------------------------------------------------------------------------
 * Deep idle strategy
 *
 * 1 = esp_light_sleep_start(), GPIO level wake on the touch INT. Lowest
 *     current (~1-2 mA), but it gates the USB-Serial-JTAG clock: the console
 *     disappears and the chip has been observed to stay unreachable. 电池供电
 *     时可以改成 1（不再需要串口），见 README「电池供电」一节。
 * 0 = stay awake, drop the CPU to 80 MHz. ~25-30 mA instead of 1-2 mA, but the
 *     console stays alive and every wake is guaranteed.
 */
#define POWER_MGR_LIGHT_SLEEP   0

#define IDLE_CPU_MHZ         80
#define ACTIVE_CPU_MHZ       240

/* 熄屏与降频的时机由 app_config.h 统一定义 */
#define IDLE_TO_BLANK_MS     APP_SCREEN_OFF_MS
#define BLANK_TO_IDLE_MS     APP_IDLE_AFTER_MS
/* Safety net for the light sleep build. */
#define SLEEP_RECHECK_S      (30 * 60)
/* Touch polling period. Short enough to feel instant. */
#define POLL_MS              10

static int s_brightness = APP_BRIGHTNESS;
static volatile bool s_screen_on = true;
static volatile TickType_t s_last_activity = 0;
static bool s_low_power = false;

static void set_cpu_mhz(int mhz);

/* ------------------------------------------------------------------ panel */

static void panel_off(void)
{
    s_screen_on = false;
    bsp_display_brightness_set(0);
    ESP_LOGI(TAG, "no touch for %d s, panel off", IDLE_TO_BLANK_MS / 1000);
}

static void panel_on(void)
{
    if (s_low_power) {
        s_low_power = false;
        set_cpu_mhz(ACTIVE_CPU_MHZ);
    }
    bsp_display_brightness_set(s_brightness);
    s_screen_on = true;
    ESP_LOGI(TAG, "panel on");
}

/* ---------------------------------------------------------------- cpu freq */

static void set_cpu_mhz(int mhz)
{
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = mhz,
        .min_freq_mhz = mhz,
        .light_sleep_enable = false,
    };
    esp_err_t err = esp_pm_configure(&pm_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure(%d MHz): %s", mhz, esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "cpu clock -> %d MHz", mhz);
}

/* ------------------------------------------------------------- deep idle */

static void enter_low_power(void)
{
    s_low_power = true;

#if POWER_MGR_LIGHT_SLEEP
    ESP_LOGI(TAG, "no touch for %d s, light sleep (touch GPIO%d wakes)",
             (IDLE_TO_BLANK_MS + BLANK_TO_IDLE_MS) / 1000, BSP_LCD_TOUCH_INT);

    esp_sleep_enable_gpio_wakeup();
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_RECHECK_S * 1000000ULL);

    while (true) {
        gpio_wakeup_enable(BSP_LCD_TOUCH_INT, GPIO_INTR_LOW_LEVEL);
        esp_light_sleep_start();

        uint32_t causes = esp_sleep_get_wakeup_causes();
        if (!(causes & (1u << ESP_SLEEP_WAKEUP_TIMER))) {
            ESP_LOGI(TAG, "woke up, cause bitmap 0x%02x", (unsigned)causes);
            break;
        }
    }
    gpio_wakeup_disable(BSP_LCD_TOUCH_INT);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);

    panel_on();
    s_last_activity = xTaskGetTickCount();
#else
    ESP_LOGI(TAG, "no touch for %d s, idle: cpu %d MHz, panel off",
             (IDLE_TO_BLANK_MS + BLANK_TO_IDLE_MS) / 1000, IDLE_CPU_MHZ);
    set_cpu_mhz(IDLE_CPU_MHZ);
#endif
}

/* -------------------------------------------------------------- supervisor */

static void power_task(void *arg)
{
    (void)arg;

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        /* Level, not edge: the CST9217 keeps INT asserted until the host reads
         * the report, and nothing reads it while the panel is blanked. */
        if (gpio_get_level(BSP_LCD_TOUCH_INT) == 0) {
            s_last_activity = now;
            if (!s_screen_on) {
                panel_on();
            }
        }

        uint32_t idle_ms = (uint32_t)(now - s_last_activity) * portTICK_PERIOD_MS;

        if (s_screen_on) {
            if (idle_ms >= IDLE_TO_BLANK_MS) {
                panel_off();
            }
        } else if (!s_low_power && idle_ms >= IDLE_TO_BLANK_MS + BLANK_TO_IDLE_MS) {
            enter_low_power();
        }

        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

/* -------------------------------------------------------------------- init */

lv_display_t *power_mgr_display_start(void)
{
    return bsp_display_start();
}

esp_err_t power_mgr_start(void)
{
    int level = bsp_display_brightness_get();
    s_brightness = (level > 0) ? level : 100;

    /* The touch INT is open-drain; give it a defined level so a floating pin
     * is not mistaken for a touch. */
    gpio_set_pull_mode(BSP_LCD_TOUCH_INT, GPIO_PULLUP_ONLY);

    s_screen_on = true;
    s_last_activity = xTaskGetTickCount();

    if (xTaskCreatePinnedToCore(power_task, "power_mgr", 4096, NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "idle supervisor up: panel off after %d s, cpu %d MHz after %d s",
             IDLE_TO_BLANK_MS / 1000, IDLE_CPU_MHZ,
             (IDLE_TO_BLANK_MS + BLANK_TO_IDLE_MS) / 1000);
    return ESP_OK;
}

bool power_mgr_screen_is_on(void)
{
    return s_screen_on;
}
