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
#include "ui_common.h"   /* ui_lock / ui_unlock：查 LVGL 的触摸状态要用显示锁 */
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

/* 是否"真的按下了"。
 *
 * 不能拿触摸 INT 的电平当活动依据：INT 被拉低有两种原因——真实触摸，
 * 以及报告还没被读走。这块屏在充电时面板噪声会让触摸芯片不断产生报告，
 * INT 几乎一直是低的；老代码用 level 刷新空闲计时，结果插着 USB 就永远不熄屏。
 *
 * 现在只认 LVGL 那边"按下"的状态：噪声不会让触摸芯片报出真实坐标，
 * 所以不会误判。INT 仍是廉价的门控——只有它低的时候才去问 LVGL。 */
static bool any_indev_pressed(void)
{
    bool pressed = false;

    if (!ui_lock(20)) {
        return false;
    }
    for (lv_indev_t *i = lv_indev_get_next(NULL); i != NULL; i = lv_indev_get_next(i)) {
        if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) {
            pressed = true;
            break;
        }
    }
    ui_unlock();
    return pressed;
}

static void power_task(void *arg)
{
    (void)arg;

    /* 唤醒用「高 -> 低」的边沿，不用电平：
     * 触摸芯片会把 INT 拉住直到报告被读走，一旦这状态卡住（充电噪声、丢读），
     * 用电平判断就会一直认为"有人碰着"，屏幕永远不熄、熄了也立刻被点亮。
     * 边沿只在真的按下那一刻出现，卡低时没有边沿，也就不会反复唤醒。 */
    bool int_prev_low = (gpio_get_level(BSP_LCD_TOUCH_INT) == 0);
    TickType_t blank_tick = 0;

#if APP_IDLE_DEBUG
    uint32_t dbg_low = 0, dbg_press = 0;
    TickType_t dbg_t0 = xTaskGetTickCount();
#endif

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        const bool int_low = (gpio_get_level(BSP_LCD_TOUCH_INT) == 0);
        const bool falling = int_low && !int_prev_low;
        int_prev_low = int_low;

        if (s_screen_on) {
            /* 亮屏时：只有 LVGL 那边真的"按下"才算活动。
             * 用 INT 电平的话，卡低会让空闲计时被无限刷新（插着 USB 就永不熄屏）。 */
            if (int_low && any_indev_pressed()) {
                s_last_activity = now;
            }
        } else if (falling && (now - blank_tick) >= pdMS_TO_TICKS(300)) {
            /* 熄屏后留 300 ms 死区：刚熄灭时手还在屏上，避免刚黑就亮 */
            panel_on();
            s_last_activity = now;
        }

        uint32_t idle_ms = (uint32_t)(now - s_last_activity) * portTICK_PERIOD_MS;

        if (s_screen_on) {
            if (idle_ms >= IDLE_TO_BLANK_MS) {
                panel_off();
                blank_tick = now;
            }
        } else if (!s_low_power && idle_ms >= IDLE_TO_BLANK_MS + BLANK_TO_IDLE_MS) {
            enter_low_power();
        }

#if APP_IDLE_DEBUG
        if (int_low) {
            dbg_low++;
        }
        if (s_screen_on && int_low && any_indev_pressed()) {
            dbg_press++;
        }
        if (now - dbg_t0 >= pdMS_TO_TICKS(2000)) {
            ESP_LOGI(TAG, "idle=%lums int_low=%lu press=%lu screen=%d",
                     (unsigned long)idle_ms, (unsigned long)dbg_low,
                     (unsigned long)dbg_press, (int)s_screen_on);
            dbg_low = 0;
            dbg_press = 0;
            dbg_t0 = now;
        }
#endif

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

bool power_mgr_is_low_power(void)
{
    return s_low_power;
}
