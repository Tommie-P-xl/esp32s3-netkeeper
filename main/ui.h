/*
 * 主界面：状态卡片 + 五行信息 + 三个按钮。
 *
 * 按钮是唯一的交互入口（需求：不再"碰一下就检测"）：
 *   [立即检测] 手动触发一次检测
 *   [WiFi]     扫描/选网/改密码
 *   [账户]     当前 WiFi 下的认证账号管理
 */
#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 建主界面。必须在 ui_lock() 保护下调用。 */
void ui_create(lv_display_t *disp);

/** @brief 主屏对象（其它界面用它返回）。 */
lv_obj_t *ui_screen_main(void);

/**
 * @brief UI 心跳计数（500 ms 一次）。
 *
 * 用来从网页确认 LVGL 任务还活着 —— 这个任务的串口输出时有时无，
 * 不能拿日志当存活依据。
 */
uint32_t ui_tick_count(void);

#ifdef __cplusplus
}
#endif
