/*
 * 配置流程界面：选 WiFi -> 输密码 -> 管账户。
 *
 * 三个屏按需创建、复用一个实例：
 *   wifi 屏  ：扫描结果列表（已保存的带删除按钮），点一行 = 连它
 *   pass 屏  ：给未保存的 WiFi 输密码，确定后写库并连接
 *   acct 屏  ：某个 WiFi 下的认证账号列表，可切换/删除/新增
 *
 * 扫描会阻塞 3~6 秒，所以放在独立任务里跑，扫完再用 ui_lock 刷界面，
 * 免得 LVGL 主循环卡住（卡住的话触摸和熄屏计时都会跟着停）。
 */
#pragma once

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 打开「选择 WiFi」，保留已有列表并在后台扫描。 */
void ui_setup_open_wifi(void);

/** @brief 打开当前 WiFi 的账户管理。 */
void ui_setup_open_accounts(void);

/** @brief 扫描任务的断点位置（调试用，见 ui_setup.c 里的 s_scan_stage）。 */
int ui_setup_scan_stage(void);

#ifdef __cplusplus
}
#endif
