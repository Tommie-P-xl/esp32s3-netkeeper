/*
 * 系统时间：SNTP 校时 + 常驻计时。
 *
 * 设备没有电池后备 RTC，掉电归零；所以开机后只要连上网就自动找 NTP 校时
 * （东八区），之后靠系统时钟自己走。校时前时间无效，界面显示 "--:--"。
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 设定时区（东八区）。开机调一次。 */
void sys_time_init(void);

/**
 * @brief 启动/重启 SNTP 校时。
 *
 * 幂等：每次拿到 IP 都可以调（换了网络会重新校）。
 */
void sys_time_sntp_start(void);

/** @brief 时间是否已同步（大于 2024-01-01 视为有效）。 */
bool sys_time_valid(void);

/** @brief 当前时间 "HH:MM"；未同步输出 "--:--"。out 至少 8 字节。 */
void sys_time_str(char *out, int cap);

#ifdef __cplusplus
}
#endif
