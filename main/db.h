/*
 * 配置库：同一份 INI 文本，双写「片上 NVS」和「SD 卡文件」。
 *
 * 为什么两份：
 *   1. 没插 SD 卡的人也要能用 —— 配置进 NVS，照样一次烧录长期使用。
 *   2. 插了卡的人多一份备份 —— NVS 损坏/SD 换板子都能救回来。
 *
 * 读写规则（决定了"手改 SD 卡"能不能生效）：
 *   - 每次改动：NVS 和 SD 都写一遍，两处内容始终一致。
 *   - 开机 / 重载 / 热插入：有效 SD 优先；缺失或损坏则由 NVS 重建 SD。
 *   - 拔卡：切换到 NVS；独立任务每秒探测，熄屏和 80 MHz 省电时照常运行。
 *   - 同步始终保留当前已连接且取得 IP 的 WiFi、实际密码、全部认证账号，
 *     并保持该 WiFi 为 active；其余网络完全以有效 SD 为准（ABC + DEF -> ADEF）。
 *   - 无活跃连接时不合并旧网络；容量不足以保留活跃网络时拒绝整次导入。
 *   - 写失败返回错误并在后台重试；无变动不重复写 Flash。
 *
 * 文本格式（人可以直接用读卡器改，UTF-8，CRLF/LF 都认）：
 *
 *   # 井号开头是注释
 *   [general]
 *   active=MyHomeWiFi
 *   last_ok=MyHomeWiFi
 *
 *   [MyHomeWiFi]                 ; 段名就是 SSID，允许带空格
 *   wifi=wifi密码
 *   current=20230000000
 *   account=20230000000|密码
 *
 * 容量：单份文本上限 DB_TEXT_MAX 字节（NVS 的块大小限制），一般够放
 * 十几组网络。超出会拒绝写入并在日志里报错，不会静默丢数据。
 *
 * 线程模型：所有函数内部自己加锁。需要成段遍历时用 db_read_begin/end 包住，
 * 期间不要再调用会改库的函数。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DB_MAX_NETS      16
#define DB_MAX_ACCT      8
#define DB_SSID_LEN      33
#define DB_PASS_LEN      64
#define DB_USER_LEN      40
#define DB_TEXT_MAX      4000   /* NVS 单条 blob 的安全上限 */

#define DB_SD_DIR        "/sdcard/portal"
#define DB_SD_FILE       DB_SD_DIR "/net.cfg"

typedef struct {
    char user[DB_USER_LEN];
    char pass[DB_PASS_LEN];
} db_acct_t;

typedef struct {
    char ssid[DB_SSID_LEN];
    char wifi_pass[DB_PASS_LEN];
    int acct_count;
    db_acct_t acct[DB_MAX_ACCT];
    char current[DB_USER_LEN];
} db_net_t;

/** @brief 打开 NVS 存储、尝试挂载 SD、按规则加载配置。 */
esp_err_t db_init(void);

/** @brief SD 卡当前是否可用（可用时配置会同时镜像到卡上）。 */
bool db_sd_present(void);

/** @brief 重新按规则加载（网页「重载配置」用它）。 */
esp_err_t db_reload(void);

/** @brief 导入有效 SD 配置到 NVS，同样保留活跃网络；无效配置返回错误。 */
esp_err_t db_restore_from_sd(void);

/** @brief 立刻落盘（改动类接口内部已调用）。 */
esp_err_t db_save(void);

/* ---- 读 ---- */
void db_read_begin(void);
void db_read_end(void);
int db_net_count(void);
const db_net_t *db_net_at(int idx);
const db_net_t *db_find(const char *ssid);

void db_active_ssid(char *out, size_t cap);
esp_err_t db_set_active_ssid(const char *ssid);

/** @brief 上一次真正连上并拿到 IP 的网络（连不上目标时回退用）。 */
void db_last_ok(char *out, size_t cap);
esp_err_t db_set_last_ok(const char *ssid);

/**
 * @brief 在库里另找一个不是 avoid 的网络。
 * @return true 表示找到了
 */
bool db_other_ssid(const char *avoid, char *out, size_t cap);

/* ---- 写（自带锁 + 自动双写） ---- */
db_net_t *db_ensure_net(const char *ssid, const char *wifi_pass);
esp_err_t db_remove_net(const char *ssid);

esp_err_t db_upsert_acct(const char *ssid, const char *user, const char *pass);
esp_err_t db_remove_acct(const char *ssid, const char *user);
esp_err_t db_set_current_acct(const char *ssid, const char *user);

bool db_current_acct(const char *ssid, char *user, size_t ucap, char *pass, size_t pcap);

void db_dump(void);

#ifdef __cplusplus
}
#endif
