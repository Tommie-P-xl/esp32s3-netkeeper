/*
 * 电池状态：插了电池才显示电量（插着 USB 时只显示百分比和充电状态，
 * 不显示续航估计）。
 *
 * 数据来源是板载 AXP2101（I2C 0x34），细节见 batt_mgr.c 顶部注释。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;      /* PMU 电池在位状态，电池供电时结合 VBAT 校验 */
    bool usb;          /* USB / VBUS 在位 */
    bool charging;     /* 正在充电（含恒压收尾段） */
    bool full;         /* 充到截止，USB 仍在 */
    int  mv;           /* 电池电压（mV） */
    int  pct;          /* 0..100 */
    int  minutes_left; /* 预计还能用多少分钟；-1 = 不适用（充电中 / 没电池） */
} batt_info_t;

/**
 * @brief NVS 初始化后、屏幕/SD/WiFi 初始化前调用；配置 PMU 并执行低压开机检查。
 */
esp_err_t batt_mgr_init(void);

/** @brief 启动电池轮询与运行期低电压保护，显示初始化失败时也必须调用。 */
esp_err_t batt_mgr_start(void);

/** @brief 取一份当前快照（内部加锁，可跨任务调用）。 */
void batt_mgr_get(batt_info_t *out);

#ifdef __cplusplus
}
#endif
