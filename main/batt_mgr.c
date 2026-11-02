/*
 * 电池状态：走板载 AXP2101（I2C 0x34）读电压与充放电状态。
 *
 * 寄存器（AXP2101 datasheet 6.13，充电/放电各 dump 一次整表对比过）：
 *   reg00  status1 : bit5 = VBUS good（USB 在位）
 *   reg01  status2 : bit[7:5] 充电状态：0 待机 / 1 充电中 / 2 放电 / 3 充电完成
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
#define PRESENT_MV        3000   /* 低于这个电压当作没插电池 */

static i2c_master_dev_handle_t s_dev;
static bool s_ok;               /* AXP 能通 */
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
        return;
    }

    const int mv = ((vh[0] & 0x3F) << 8) | vh[1];
    const bool usb = (st1 & 0x20) != 0;
    const int chg_state = (st2 >> 5) & 0x07;

    batt_info_t info;
    memset(&info, 0, sizeof(info));
    info.present = (mv >= PRESENT_MV);
    info.usb = usb;
    /* status2[7:5]: 0 待机 / 1 充电中 / 2 放电 / 3 充电完成
     * 注意 standby(0) 和 done(3) 都不是"正在充"：满电后 AXP2101 会停充进待机，
     * 这时 USB 还在，但电流已经是零了。分开表示，界面才不会把"停充"说成"充电中"。 */
    info.charging = info.present && usb && (chg_state == 1);
    info.full = info.present && usb && (chg_state == 3);
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

    if (axp_init() != ESP_OK) {
        s_ok = false;
        return ESP_FAIL;
    }
    s_ok = true;

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
