/*
 * 网络守护：连 WiFi -> 分层实时轻哨兵监测 -> 静默防抖 -> 门控认证。
 *
 * 监测与自愈策略：
 *   - 在线稳态：使用无 DNS 轻量探针（ICMP 单包优先，自动降级 TCP:443 或校内
 *     rad_user_info）；插 USB / 亮屏 15 s 一次，电池熄屏 45 s 一次，触摸亮屏
 *     顺带轻测一次；每 15 分钟跑一次完整双 204 兜底校检。
 *   - 抗波动防抖：单目标失手不改状态，1 s 后切备用目标复测；双目标失败先问
 *     校内 rad_user_info 实锤掉认证才立即登录，否则 2 s 后第三次确认。
 *   - 非认证断联绝不疯狂试认证：
 *       * 网线/上联断开（Portal 不可达）：0 次认证请求，轻探 Portal 端口退避监听
 *         （15 -> 30 -> 60 s），等到 Portal 重新可达且 not_online 才自动认证。
 *       * 校内在线但外网异常：0 次认证/注销，低频监测外网与会话恢复。
 *       * 服务端明确拒绝（欠费/密码错/超限）：强指数退避（1 -> 3 -> 5 -> 10 min）。
 *
 * 状态机（net_state_t）：
 *   NEED_SETUP   库里没有 active 的 WiFi，等用户在界面里选
 *   WIFI_*       正在连 / 连不上
 *   NO_ACCOUNT   网通了但该 WiFi 下还没有认证账号
 *   CHECKING     正在探测外网
 *   ONLINE       外网通（后台运行轻量哨兵）
 *   NEED_AUTH / AUTHING / AUTH_FAILED  认证流程
 *   ERROR        上联/外网异常（不发盲目认证）
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
    char sentinel[12];    /* 当前轻哨兵模式：ICMP / TCP443 / PORTAL / FULL */
    int auth_count;       /* 成功重新认证次数 */
    int fail_streak;
    int64_t boot_ms;
    int64_t last_check_ms;
    int64_t next_check_ms;
    char detail[96];
} net_status_t;

void net_guard_get(net_status_t *out);

/** @brief 立刻做一次完整检测，不等周期（同时清零离线退避）。 */
void net_guard_trigger(void);

/** @brief 熄屏触摸唤醒时调用：通知在线稳态哨兵做一次无感轻探测（带冷却）。 */
void net_guard_on_wake(void);

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
