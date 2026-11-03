/*
 * 电池状态：走板载 AXP2101（I2C 0x34）读电压与充放电状态。
 *
 * 寄存器（AXP2101 datasheet 6.13）：
 *   reg00  status1 : bit5 = VBUS good，bit3 = 电池在位
 *   reg01  status2 : bit[6:5] 电流方向；bit[2:0] 充电阶段，4 为充满
 *   reg34  + reg35 : VBAT，14 位，high 6 bits + low 8 bits，单位 mV
 *   regA4          : E-Gauge 电量百分比（本板没做电池标定，只用于对照，不采信）
 *
 * 两个现实约束，代码里都按"说清楚"的方式处理：
 *   1. AXP2101 没有电池电流 ADC（它只有 VBAT/VBUS/VSYS 电压和芯片温度），
 *      所以"还能用多久"只能是**功耗模型估算**，不是实测电流。
 *   2. 充电时 VBAT 会被抬高（线损 + 极化），直接用会把电量算高，这里按
 *      APP_BAT_CHG_OFFSET_MV 减掉一个经验值再查表。
 */
#include "batt_mgr.h"

#include <string.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power_mgr.h"

#include "bsp/esp-bsp.h"

static const char *TAG = "batt";

#define AXP_ADDR          0x34
#define AXP_REG_STATUS1   0x00
#define AXP_REG_STATUS2   0x01
#define AXP_REG_VBAT_H    0x34
#define AXP_REG_VBAT_L    0x35
#define AXP_REG_GAUGE     0xA4

#define POLL_MS           2000

static i2c_master_dev_handle_t s_dev;
static bool s_ok;               /* AXP 能通 */
static nvs_handle_t s_nvs;
static bool s_low_latched;
static unsigned s_low_samples;
static batt_info_t s_info;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* 锂电 3.30–4.15 V 的分段曲线，带负载时电压会塌，取值偏保守 */
static const struct {
    int mv;
    int pct;
} s_curve[] = {
    { 3300,   0 },
    { 3450,   5 },
    { 3550,  12 },
    { 3650,  25 },
    { 3750,  45 },
    { 3850,  65 },
    { 3950,  82 },
    { 4050,  93 },
    { 4150, 100 },
};

static int pct_from_mv(int mv)
{
    const int n = (int)(sizeof(s_curve) / sizeof(s_curve[0]));
    if (mv <= s_curve[0].mv) {
        return 0;
    }
    if (mv >= s_curve[n - 1].mv) {
        return 100;
    }
    for (int i = 1; i < n; i++) {
        if (mv < s_curve[i].mv) {
            const int v0 = s_curve[i - 1].mv, p0 = s_curve[i - 1].pct;
            const int v1 = s_curve[i].mv, p1 = s_curve[i].pct;
            return p0 + (mv - v0) * (p1 - p0) / (v1 - v0);
        }
    }
    return 100;
}

/* 没有电流计，只能按当前工作状态估一个放电电流 */
static int est_current_ma(void)
{
    if (power_mgr_screen_is_on()) {
        return APP_BAT_CUR_SCREEN_ON_MA;
    }
    if (power_mgr_is_low_power()) {
        return APP_BAT_CUR_IDLE_LOW_MA;
    }
    return APP_BAT_CUR_IDLE_MA;
}

static esp_err_t axp_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 200);
}

static esp_err_t axp_init(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (bus == NULL) {
        return ESP_FAIL;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t st = 0;
    err = axp_read(AXP_REG_STATUS1, &st, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not responding: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "AXP2101 online, status1=0x%02x", st);
    return ESP_OK;
}

/* 只修改指定字段，并读回确认，避免误改同寄存器内的其它电源设置。 */
static esp_err_t axp_update(uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t old = 0, check = 0;
    esp_err_t err = axp_read(reg, &old, 1);
    if (err != ESP_OK) return err;
    const uint8_t wanted = (old & (uint8_t)~mask) | (value & mask);
    if (wanted != old) {
        uint8_t data[] = {reg, wanted};
        err = i2c_master_transmit(s_dev, data, sizeof(data), 200);
        if (err != ESP_OK) return err;
    }
    err = axp_read(reg, &check, 1);
    return err == ESP_OK && (check & mask) != (wanted & mask) ? ESP_FAIL : err;
}

static esp_err_t latch_set(bool low)
{
    if (s_low_latched == low) return ESP_OK;
    esp_err_t err = nvs_set_u8(s_nvs, "low_cutoff", low);
    if (err == ESP_OK) err = nvs_commit(s_nvs);
    if (err == ESP_OK) s_low_latched = low;
    return err;
}

static esp_err_t read_voltage(uint8_t *status, int *mv)
{
    uint8_t v[2];
    esp_err_t err = axp_read(AXP_REG_STATUS1, status, 1);
    if (err == ESP_OK) err = axp_read(AXP_REG_VBAT_H, v, sizeof(v));
    if (err == ESP_OK) *mv = ((v[0] & 0x3f) << 8) | v[1];
    return err;
}

static void low_power_off(void)
{
    const esp_err_t saved = latch_set(true);
    if (saved != ESP_OK) ESP_LOGE(TAG, "cannot persist low-battery lock: %s", esp_err_to_name(saved));
    /* 关机前重新检查 USB；刚插入 Type-C 时允许继续运行和充电。 */
    for (int i = 0; i < 3; ++i) {
        uint8_t status = 0, ctl = 0;
        if (axp_read(AXP_REG_STATUS1, &status, 1) == ESP_OK && (status & 0x20)) return;
        if (axp_read(0x10, &ctl, 1) == ESP_OK) {
            const uint8_t data[] = {0x10, (uint8_t)(ctl | 1)};
            i2c_master_transmit(s_dev, data, sizeof(data), 200);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    /* PMU 命令异常时也不再亮屏或联网耗电；下次按键启动仍走开机检查。 */
    ESP_LOGE(TAG, "PMU power-off did not complete, entering deep sleep");
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_deep_sleep_start();
}

esp_err_t batt_mgr_init(void)
{
    _Static_assert(APP_BAT_OFF_MV < APP_BAT_RESUME_MV, "battery recovery needs hysteresis");
    _Static_assert(APP_BAT_HARD_OFF_MV >= 2600 && APP_BAT_HARD_OFF_MV <= 3300 &&
                   APP_BAT_HARD_OFF_MV % 100 == 0, "invalid PMU cutoff");
    _Static_assert(APP_BAT_CHARGE_CURRENT_MA >= 100 && APP_BAT_CHARGE_CURRENT_MA <= 1000 &&
                   (APP_BAT_CHARGE_CURRENT_MA <= 200 ? APP_BAT_CHARGE_CURRENT_MA % 25 == 0 :
                    APP_BAT_CHARGE_CURRENT_MA % 100 == 0), "invalid charge current");
    _Static_assert(APP_BAT_TERM_CURRENT_MA > 0 && APP_BAT_TERM_CURRENT_MA <= 200 &&
                   APP_BAT_TERM_CURRENT_MA % 25 == 0, "invalid termination current");
    esp_err_t err = axp_init();
    if (err != ESP_OK) return err;
    err = nvs_open("battery", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) return err;
    uint8_t locked = 0;
    err = nvs_get_u8(s_nvs, "low_cutoff", &locked);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    s_low_latched = locked != 0;

    /* VBAT ADC；关机后仍由 PMU 持续执行 VSYS 欠压保护。 */
    if ((err = axp_update(0x30, 0x01, 0x01)) != ESP_OK ||
        (err = axp_update(0x24, 0x07, (APP_BAT_HARD_OFF_MV - 2600) / 100)) != ESP_OK) return err;
    uint8_t status = 0;
    int mv = 0;
    for (int i = 0; i < 5; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
        err = read_voltage(&status, &mv);
        if (err == ESP_OK && ((status & 0x20) || mv > 0)) break;
    }
    if (err != ESP_OK || (!(status & 0x20) && mv <= 0)) return ESP_FAIL;
    if (!(status & 0x20) && (mv <= APP_BAT_OFF_MV ||
        (s_low_latched && mv < APP_BAT_RESUME_MV))) {
        ESP_LOGW(TAG, "battery boot blocked: %d mV, recovery %d mV or USB", mv, APP_BAT_RESUME_MV);
        low_power_off();
    }
    if (mv >= APP_BAT_RESUME_MV && (err = latch_set(false)) != ESP_OK) return err;

    const uint8_t current = APP_BAT_CHARGE_CURRENT_MA <= 200 ? APP_BAT_CHARGE_CURRENT_MA / 25 :
                            8 + (APP_BAT_CHARGE_CURRENT_MA - 200) / 100;
    /* 4.2 V / 300 mA，终止电流 50 mA；自动终止、预充及总充电超时保护开启。 */
    if ((err = axp_update(0x64, 0x07, 0x03)) != ESP_OK ||
        (err = axp_update(0x62, 0x1f, current)) != ESP_OK ||
        (err = axp_update(0x63, 0x1f, 0x10 | (APP_BAT_TERM_CURRENT_MA / 25))) != ESP_OK ||
        (err = axp_update(0x67, 0x44, 0x44)) != ESP_OK ||
        (err = axp_update(0x18, 0x02, 0x02)) != ESP_OK) return err;
    ESP_LOGI(TAG, "verified charger: 4200 mV, %d mA, termination %d mA; battery %d mV, USB=%d",
             APP_BAT_CHARGE_CURRENT_MA, APP_BAT_TERM_CURRENT_MA, mv, !!(status & 0x20));
    s_ok = true;
    return ESP_OK;
}

static void batt_poll(void)
{
    uint8_t st1 = 0, st2 = 0, vh[2] = { 0, 0 };

    if (axp_read(AXP_REG_STATUS1, &st1, 1) != ESP_OK ||
        axp_read(AXP_REG_STATUS2, &st2, 1) != ESP_OK ||
        axp_read(AXP_REG_VBAT_H, vh, 2) != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        memset(&s_info, 0, sizeof(s_info));
        s_info.minutes_left = -1;
        portEXIT_CRITICAL(&s_lock);
        s_low_samples = 0;
        return;
    }

    const int mv = ((vh[0] & 0x3F) << 8) | vh[1];
    const bool usb = (st1 & 0x20) != 0;
    const int chg_state = st2 & 0x07;

    batt_info_t info;
    memset(&info, 0, sizeof(info));
    info.present = (st1 & 0x08) != 0 || (!usb && mv > 0);
    info.usb = usb;
    /* 0/1/2/3 是涓流/预充/恒流/恒压，4 完成，5 未充电。 */
    info.charging = info.present && usb && (chg_state <= 3);
    info.full = info.present && usb && (chg_state == 4);
    info.mv = mv;
    info.pct = info.present ? pct_from_mv(mv - (info.charging ? APP_BAT_CHG_OFFSET_MV : 0)) : 0;
    info.minutes_left = -1;

    /* 只有"在用电池放电"才给续航估计；插着 USB（充电中/已充满）不显示 */
    if (info.present && !usb && info.pct > 0) {
        const int ma = est_current_ma();
        const int left_mah = APP_BATTERY_MAH * info.pct / 100;
        info.minutes_left = (ma > 0) ? (left_mah * 60 / ma) : -1;
    }

    portENTER_CRITICAL(&s_lock);
    s_info = info;
    portEXIT_CRITICAL(&s_lock);

    if (mv >= APP_BAT_RESUME_MV) {
        const esp_err_t err = latch_set(false);
        if (err != ESP_OK) ESP_LOGW(TAG, "battery lock clear failed: %s", esp_err_to_name(err));
    }
    const bool low = !usb && info.present && (mv <= APP_BAT_OFF_MV ||
                     (s_low_latched && mv < APP_BAT_RESUME_MV));
    s_low_samples = low ? s_low_samples + 1 : 0;
    if (s_low_samples >= APP_BAT_LOW_SAMPLES) {
        ESP_LOGW(TAG, "low battery %d mV, shutting down", mv);
        low_power_off();
        s_low_samples = 0;
    }
}

static void batt_task(void *arg)
{
    (void)arg;
    for (;;) {
        batt_poll();
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

esp_err_t batt_mgr_start(void)
{
    s_info.minutes_left = -1;

    if (!s_ok) return ESP_ERR_INVALID_STATE;

    batt_poll();   /* 先把第一次结果填上，UI 不用等 2 秒 */

    if (xTaskCreate(batt_task, "batt", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void batt_mgr_get(batt_info_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_info;
    portEXIT_CRITICAL(&s_lock);
    if (!s_ok) {
        memset(out, 0, sizeof(*out));
        out->minutes_left = -1;
    }
}
