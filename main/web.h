/*
 * 局域网配置面板。
 *
 * 板子连上 WiFi 后，同一个网段里的手机/电脑打开屏幕上的 IP 就能管理
 * 「哪些 WiFi、每个 WiFi 下有哪些认证账号」。所有操作直接改 SD 卡上的
 * net.cfg，跟屏幕上点出来的效果完全一致 —— 两套入口，一份数据。
 *
 * 防误改：屏幕实时显示 6 位验证码（5 分钟换一次），网页必须先验证才能
 * 改配置。验证通过发的 token 存浏览器里，验证码刷新不影响已登录会话。
 *
 * 路由：
 *   GET  /              配置页面（构建时把 index.html 嵌进固件）
 *   GET  /api/state     当前状态 + 全部网络/账号（只读，不需要验证码）
 *   POST /api/auth      用验证码换 token（code）
 *   POST /api/wifi      添加/更新 WiFi（token, ssid, pass, use）
 *   POST /api/acct      添加/更新账号（token, ssid, user, pass, use）
 *   POST /api/use       切换当前账号（token, ssid, user）
 *   POST /api/remove    删除（token, type=wifi|acct, ssid, user）
 *   POST /api/check     立即检测（token）
 *   POST /api/reload    从 SD 卡重读配置（token）
 *   POST /api/restore   用 SD 覆盖片上 Flash（token）
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 启动 HTTP 服务（WiFi 初始化之后调用即可，IP 出来前访问不到而已）。 */
esp_err_t web_start(void);

/**
 * @brief 取当前网页验证码（6 位数字）。
 *
 * 验证码每 5 分钟自动换新；每次取的时候顺带检查过期，所以拿不到旧码。
 * @param out 至少 7 字节
 */
void web_code_get(char out[7]);

/** @brief 当前验证码剩余有效毫秒数（UI 倒计时用）。 */
int64_t web_code_remain_ms(void);

#ifdef __cplusplus
}
#endif
