/*
 * 网络守护：连 WiFi -> 5 分钟一次外网探测 -> 掉线自动走 SRun 认证。
 *
 * 与上一版的区别：目标网络和账号不再写死，全部来自 SD 卡上的配置库（db）。
 * 守护任务每 500 ms 醒一次，所以界面上换网、换账号、网页里加账号都能立刻生效，
 * 不用重启板子。
 *
 * 状态机（net_state_t）：
 *   NEED_SETUP   库里没有 active 的 WiFi，等用户在界面里选
 *   WIFI_*       正在连 / 连不上
 *   NO_ACCOUNT   网通了但该 WiFi 下还没有认证账号
 *   CHECKING     正在探测外网
 *   ONLINE       外网通
 *   NEED_AUTH / AUTHING / AUTH_FAILED  认证流程
 *   ERROR        连 Portal 都摸不到
 *
 * 轮询周期固定 5 分钟，熄屏、降频都照跑；认证失败按 30/60/120 秒退避三次。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_BOOT = 0,
    NET_NEED_SETUP,
    NET_WIFI_CONNECTING,
    NET_WIFI_DOWN,
    NET_NO_ACCOUNT,
    NET_CHECKING,
    NET_ONLINE,
    NET_NEED_AUTH,
    NET_AUTHING,
    NET_AUTH_FAILED,
    NET_ERROR,
} net_state_t;

typedef struct {
    net_state_t state;
    char ssid[33];        /* 当前连着的 WiFi */
    char target[33];      /* 配置里指定的目标 WiFi */
    char ip[16];
    char portal[64];
    char account[40];     /* 正在用的认证账号 */
    int auth_count;       /* 成功重新认证次数 */
    int fail_streak;
    int64_t boot_ms;
    int64_t last_check_ms;
    int64_t next_check_ms;
    char detail[96];
} net_status_t;

void net_guard_get(net_status_t *out);

/** @brief 立刻做一次检测，不等周期。 */
void net_guard_trigger(void);

/** @brief 用户明确选择使用账号时调用；普通检测和开机不注销现有会话。 */
void net_guard_switch_account(const char *ssid, const char *user);

/**
 * @brief 试连一个 WiFi，连上了才把它设为守护目标。
 *
 * 新加网络走这条路：连不上就保留原来能用的网络，不会把用户踢下线。
 */
void net_guard_try_ssid(const char *ssid);

/** @brief 启动守护任务（wifi_mgr_init 之后调用）。 */
void net_guard_start(void);

#ifdef __cplusplus
}
#endif
