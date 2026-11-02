#include "db.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdmmc_cmd.h"
#include "wifi_mgr.h"
#include "net_guard.h"

static const char *TAG = "db";

#define NVS_NS       "portal"
#define NVS_KEY_CFG  "cfg"
#define NVS_KEY_HASH "sd_hash"

static db_net_t s_nets[DB_MAX_NETS];
static int s_count;
static char s_active[DB_SSID_LEN];
static char s_last_ok[DB_SSID_LEN];
static bool s_sd;
static uint32_t s_sd_hash;      /* 只在 SD 成功读取/写入后更新 */
static uint32_t s_nvs_hash;
static bool s_nvs_synced;
static bool s_sd_dirty;
static bool s_loaded;
static TaskHandle_t s_sd_task;
static SemaphoreHandle_t s_mtx;
static nvs_handle_t s_nvs;

/* 序列化用的缓冲，避免每次改动都 malloc 4 KB */
static char s_text[DB_TEXT_MAX];

/* 导入或修改失败时回滚；全部受 s_mtx 保护，不占后台任务栈。 */
typedef struct {
    db_net_t nets[DB_MAX_NETS];
    int count;
    char active[DB_SSID_LEN];
    char last_ok[DB_SSID_LEN];
} db_snapshot_t;
static db_snapshot_t s_before;

static void snapshot_locked(void)
{
    memcpy(s_before.nets, s_nets, sizeof(s_nets));
    s_before.count = s_count;
    strlcpy(s_before.active, s_active, sizeof(s_active));
    strlcpy(s_before.last_ok, s_last_ok, sizeof(s_last_ok));
}

static void rollback_locked(void)
{
    memcpy(s_nets, s_before.nets, sizeof(s_nets));
    s_count = s_before.count;
    strlcpy(s_active, s_before.active, sizeof(s_active));
    strlcpy(s_last_ok, s_before.last_ok, sizeof(s_last_ok));
}

/* ------------------------------------------------------------- 小工具 */

static char *str_trim(char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void lock(void)
{
    if (s_mtx != NULL) {
        xSemaphoreTake(s_mtx, portMAX_DELAY);
    }
}

static void unlock(void)
{
    if (s_mtx != NULL) {
        xSemaphoreGive(s_mtx);
    }
}

/* 调用方必须已持锁 */
static db_net_t *net_find_locked(const char *ssid)
{
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) {
            return &s_nets[i];
        }
    }
    return NULL;
}

static db_net_t *net_ensure_locked(const char *ssid)
{
    db_net_t *n = net_find_locked(ssid);
    if (n != NULL) {
        return n;
    }
    if (s_count >= DB_MAX_NETS) {
        return NULL;
    }
    n = &s_nets[s_count++];
    memset(n, 0, sizeof(*n));
    strlcpy(n->ssid, ssid, sizeof(n->ssid));
    return n;
}

static int acct_find_locked(const db_net_t *n, const char *user)
{
    for (int i = 0; i < n->acct_count; i++) {
        if (strcmp(n->acct[i].user, user) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------- 序列化 */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow;
} sbuf_t;

static void sb_put(sbuf_t *sb, const char *fmt, ...)
{
    if (sb->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(sb->buf + sb->len, sb->cap - sb->len, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= sb->cap - sb->len) {
        sb->overflow = true;
        return;
    }
    sb->len += (size_t)n;
}

/* 生成完整文本（含结尾 0）。返回 false 表示超容量。 */
static bool serialize_locked(void)
{
    sbuf_t sb = {.buf = s_text, .cap = sizeof(s_text), .len = 0, .overflow = false};

    sb_put(&sb, "# esp32s3-netkeeper 配置（可以直接用读卡器改，改完插回板子重载）\n"
                "#\n"
                "# [general]  active=  开机自动连的 WiFi，也是守护目标\n"
                "# [SSID]     wifi=    WiFi 密码，开放网络留空\n"
                "#            current= 当前使用的认证账号\n"
                "#            account=用户名|密码   可写多行，一行一个账号\n"
                "#\n"
                "# 段名就是 WiFi 名字，可以带空格；以 # 开头的行是注释。\n"
                "\n[general]\n");

    sb_put(&sb, "active=%s\nlast_ok=%s\n", s_active, s_last_ok);

    for (int i = 0; i < s_count; i++) {
        const db_net_t *n = &s_nets[i];
        sb_put(&sb, "\n[%s]\n", n->ssid);
        sb_put(&sb, "wifi=%s\n", n->wifi_pass);
        if (n->current[0] != '\0') {
            sb_put(&sb, "current=%s\n", n->current);
        }
        for (int a = 0; a < n->acct_count; a++) {
            sb_put(&sb, "account=%s|%s\n", n->acct[a].user, n->acct[a].pass);
        }
    }

    if (sb.overflow) {
        ESP_LOGE(TAG, "config too large for %d bytes, write rejected", DB_TEXT_MAX);
        s_text[0] = '\0';
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- 存盘 */

static esp_err_t save_nvs_locked(void)
{
    if (!serialize_locked()) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t len = strlen(s_text) + 1;
    const uint32_t hash = fnv1a(s_text);
    if (s_nvs_synced && hash == s_nvs_hash) {
        return ESP_OK;
    }
    s_nvs_synced = false;
    esp_err_t err = nvs_set_blob(s_nvs, NVS_KEY_CFG, s_text, len);
    if (err == ESP_OK) {
        err = nvs_set_u32(s_nvs, NVS_KEY_HASH, hash);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed: %s", esp_err_to_name(err));
    } else {
        s_nvs_hash = hash;
        s_nvs_synced = true;
    }
    return err;
}

static esp_err_t save_sd_locked(void)
{
    if (!s_sd) {
        return ESP_OK;
    }

    /* 解析会修改 s_text，任何写卡入口都必须重新生成完整文本。 */
    if (!serialize_locked()) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_sd_dirty = true;

    mkdir(DB_SD_DIR, 0775);

    const char *tmp = DB_SD_DIR "/net.tmp";
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot open %s", tmp);
        return ESP_FAIL;
    }

    /* BOM：Windows 记事本打开中文注释不乱码，解析时跳过 */
    const bool bom_ok = fputs("\xEF\xBB\xBF", f) >= 0;
    const size_t len = strlen(s_text);
    const size_t wr = fwrite(s_text, 1, len, f);
    const bool ok = (fclose(f) == 0) && (wr == len) && bom_ok;
    if (!ok) {
        ESP_LOGW(TAG, "sd write short");
        return ESP_FAIL;
    }

    remove(DB_SD_FILE);
    if (rename(tmp, DB_SD_FILE) != 0) {
        ESP_LOGW(TAG, "sd rename failed");
        return ESP_FAIL;
    }
    s_sd_hash = fnv1a(s_text);
    s_sd_dirty = false;
    return ESP_OK;
}

/* 先验证大小，禁止把截断配置写进任一介质。写失败由后台重试。 */
static esp_err_t save_both_locked(void)
{
    if (!serialize_locked()) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_err_t flash = save_nvs_locked();
    const esp_err_t sd = save_sd_locked();
    return flash != ESP_OK ? flash : sd;
}

static esp_err_t commit_edit_locked(void)
{
    const esp_err_t err = save_both_locked();
    if (err == ESP_ERR_INVALID_SIZE) {
        rollback_locked();
    }
    return err;
}

esp_err_t db_save(void)
{
    lock();
    const esp_err_t err = save_both_locked();
    unlock();

    return err;
}

/* ---------------------------------------------------------------- 解析 */

static void seed_defaults_locked(void)
{
    s_count = 0;
    memset(s_nets, 0, sizeof(s_nets));
    s_last_ok[0] = '\0';

    db_net_t *n = net_ensure_locked(APP_WIFI_SSID);
    if (n != NULL) {
        strlcpy(n->wifi_pass, APP_WIFI_PASSWORD, sizeof(n->wifi_pass));
        strlcpy(n->acct[0].user, APP_PORTAL_USER, sizeof(n->acct[0].user));
        strlcpy(n->acct[0].pass, APP_PORTAL_PASSWORD, sizeof(n->acct[0].pass));
        n->acct_count = 1;
        strlcpy(n->current, APP_PORTAL_USER, sizeof(n->current));
    }
    strlcpy(s_active, APP_WIFI_SSID, sizeof(s_active));

    ESP_LOGI(TAG, "seeded defaults: ssid=%s user=%s", APP_WIFI_SSID, APP_PORTAL_USER);
}

/*
 * 解析一段文本。直接就地改 buf（把 '=' 和 ']' 变成 '\0'），所以传进来的
 * 必须是自己独占的副本。
 */
static bool parse_text_locked(char *buf)
{
    memset(s_nets, 0, sizeof(s_nets));
    s_count = 0;
    s_active[0] = '\0';
    s_last_ok[0] = '\0';
    db_net_t *cur = NULL;
    bool general = false, saw_general = false, saw_active = false;
    bool saw_wifi[DB_MAX_NETS] = {0};
    if (strncmp(buf, "\xEF\xBB\xBF", 3) == 0) {
        buf += 3;
    }
    char *save = NULL;
    for (char *line = strtok_r(buf, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = str_trim(line);
        if (!*p || *p == '#' || *p == ';') {
            continue;
        }
        if (*p == '[') {
            char *close = strchr(p, ']');
            if (!close) { return false; }
            *close = '\0';
            char *tail = str_trim(close + 1);
            if (*tail && *tail != '#' && *tail != ';') { return false; }
            char *name = str_trim(p + 1);
            general = strcasecmp(name, "general") == 0;
            cur = NULL;
            if (general) {
                if (saw_general) { return false; }
                saw_general = true;
            } else {
                if (!*name || strlen(name) >= DB_SSID_LEN || net_find_locked(name)) {
                    return false;
                }
                cur = net_ensure_locked(name);
                if (!cur) { return false; }
            }
            continue;
        }
        char *eq = strchr(p, '=');
        if (!eq) { return false; }
        *eq = '\0';
        char *key = str_trim(p), *val = str_trim(eq + 1);
        if (general) {
            if (strlen(val) >= DB_SSID_LEN) { return false; }
            if (strcasecmp(key, "active") == 0) {
                strlcpy(s_active, val, sizeof(s_active));
                saw_active = true;
            } else if (strcasecmp(key, "last_ok") == 0) {
                strlcpy(s_last_ok, val, sizeof(s_last_ok));
            } else { return false; }
        } else if (!cur) {
            return false;
        } else if (strcasecmp(key, "wifi") == 0 || strcasecmp(key, "wifi_pass") == 0 ||
                   strcasecmp(key, "password") == 0) {
            if (strlen(val) >= DB_PASS_LEN) { return false; }
            strlcpy(cur->wifi_pass, val, sizeof(cur->wifi_pass));
            saw_wifi[cur - s_nets] = true;
        } else if (strcasecmp(key, "current") == 0) {
            if (strlen(val) >= DB_USER_LEN) { return false; }
            strlcpy(cur->current, val, sizeof(cur->current));
        } else if (strcasecmp(key, "account") == 0) {
            char *bar = strchr(val, '|');
            if (!bar) { return false; }
            *bar = '\0';
            char *user = str_trim(val), *pass = str_trim(bar + 1);
            if (!*user || strlen(user) >= DB_USER_LEN || strlen(pass) >= DB_PASS_LEN ||
                cur->acct_count >= DB_MAX_ACCT || acct_find_locked(cur, user) >= 0) {
                return false;
            }
            db_acct_t *acct = &cur->acct[cur->acct_count++];
            strlcpy(acct->user, user, sizeof(acct->user));
            strlcpy(acct->pass, pass, sizeof(acct->pass));
        } else { return false; }
    }
    for (int i = 0; i < s_count; ++i) {
        if (!saw_wifi[i] || (s_nets[i].current[0] &&
            acct_find_locked(&s_nets[i], s_nets[i].current) < 0)) {
            return false;
        }
    }
    if (s_active[0] && !net_find_locked(s_active)) { return false; }
    if (s_last_ok[0] && !net_find_locked(s_last_ok)) { s_last_ok[0] = '\0'; }
    /* 显式空库可由 active= 表达；空文件、注释、孤立段名不算数据库。 */
    return s_count > 0 || (saw_general && saw_active && !s_active[0]);
}

/* 只保护当前真实关联且取得 IP 的网络，不用历史 last_ok 冒充活跃网络。 */
static bool live_network_locked(db_net_t *live)
{
    char ssid[DB_SSID_LEN], pass[DB_PASS_LEN];
    if (!wifi_mgr_current_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        return false;
    }
    const db_net_t *old = net_find_locked(ssid);
    memset(live, 0, sizeof(*live));
    if (old) { *live = *old; }
    strlcpy(live->ssid, ssid, sizeof(live->ssid));
    strlcpy(live->wifi_pass, pass, sizeof(live->wifi_pass));
    return true;
}

static bool keep_live_locked(const db_net_t *live)
{
    if (live->ssid[0]) {
        db_net_t *net = net_ensure_locked(live->ssid);
        if (!net) { return false; }
        *net = *live;  /* 密码、全部账号及当前账号一起保留 */
        strlcpy(s_active, live->ssid, sizeof(s_active));
        strlcpy(s_last_ok, live->ssid, sizeof(s_last_ok));
    }
    return serialize_locked();
}

static esp_err_t load_nvs_locked(void)
{
    size_t len = sizeof(s_text);
    esp_err_t err = nvs_get_blob(s_nvs, NVS_KEY_CFG, s_text, &len);
    if (err != ESP_OK) { return err; }
    if (!len || s_text[len - 1] != '\0' || strlen(s_text) + 1 != len) {
        return ESP_ERR_INVALID_SIZE;
    }
    s_nvs_hash = fnv1a(s_text);
    s_nvs_synced = true;
    return parse_text_locked(s_text) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

/* 完整读取并检查 I/O、超长及嵌入零字节，不能接受被截断的前半份配置。 */
static esp_err_t read_sd_locked(void)
{
    if (!s_sd) { return ESP_ERR_INVALID_STATE; }
    FILE *f = fopen(DB_SD_FILE, "rb");
    if (!f) { return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL; }
    unsigned char bom[3];
    const size_t prefix = fread(bom, 1, sizeof(bom), f);
    if (prefix != 3 || memcmp(bom, "\xEF\xBB\xBF", 3) != 0) {
        rewind(f);
    }
    const size_t n = fread(s_text, 1, sizeof(s_text) - 1, f);
    const int extra = fgetc(f);
    const bool io_ok = !ferror(f);
    const bool close_ok = fclose(f) == 0;
    if (!io_ok || !close_ok) { return ESP_FAIL; }
    s_text[n] = '\0';
    if (!n || extra != EOF || memchr(s_text, '\0', n)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* SD 导入的事务：无效配置不能留下半个库，合并超限也不能丢网络。 */
static esp_err_t import_sd_locked(const db_net_t *live)
{
    snapshot_locked();
    const uint32_t hash = fnv1a(s_text);
    if (!parse_text_locked(s_text)) {
        rollback_locked();
        return ESP_ERR_INVALID_ARG;
    }
    if (!keep_live_locked(live)) {
        rollback_locked();
        ESP_LOGE(TAG, "SD import rejected: no room for connected network");
        return ESP_ERR_INVALID_SIZE;
    }
    s_sd_hash = hash;
    s_sd_dirty = hash != fnv1a(s_text);
    const esp_err_t flash = save_nvs_locked();
    const esp_err_t sd = s_sd_dirty ? save_sd_locked() : ESP_OK;
    ESP_LOGI(TAG, "SD authoritative: %d networks, connected network %s", s_count,
             live->ssid[0] ? "retained" : "absent");
    return flash != ESP_OK ? flash : sd;
}

static esp_err_t reload_locked(void)
{
    db_net_t live = {0};
    live_network_locked(&live);
    snapshot_locked();
    /* Flash 写失败时，保留待重试的内存数据，不加载较旧的 Flash。 */
    if (!s_loaded || s_nvs_synced) {
        if (load_nvs_locked() != ESP_OK) {
            s_nvs_synced = false;
            rollback_locked();
            if (!s_loaded) { seed_defaults_locked(); }
        }
    }
    if (!keep_live_locked(&live)) {
        rollback_locked();
        return ESP_ERR_INVALID_SIZE;
    }
    s_loaded = true;
    if (s_sd) {
        const esp_err_t rd = read_sd_locked();
        if (rd == ESP_OK) {
            const esp_err_t err = import_sd_locked(&live);
            if (err != ESP_ERR_INVALID_ARG) { return err; }
            ESP_LOGW(TAG, "invalid SD database -> rebuilding from flash");
        } else if (rd != ESP_ERR_NOT_FOUND && rd != ESP_ERR_INVALID_SIZE) {
            return rd;
        }
    }
    return save_both_locked();
}

esp_err_t db_reload(void)
{
    lock();
    const esp_err_t err = reload_locked();
    unlock();
    return err;
}

esp_err_t db_restore_from_sd(void)
{
    lock();
    db_net_t live = {0};
    live_network_locked(&live);
    esp_err_t err = read_sd_locked();
    if (err == ESP_OK) { err = import_sd_locked(&live); }
    unlock();
    return err;
}

static void detach_sd_locked(void)
{
    s_sd = false;
    s_sd_dirty = false;
    s_sd_hash = 0;
    /* BSP 卸载会释放 card；立即清空它，禁止对悬空指针再次探测。 */
    const esp_err_t err = esp_vfs_fat_sdcard_unmount(BSP_SD_MOUNT_POINT, bsp_sdcard);
    bsp_sdcard = NULL;
    ESP_LOGW(TAG, "SD unavailable -> flash-only (%s)", esp_err_to_name(err));
    reload_locked();
}

/* 没有卡检测 GPIO：每秒发卡状态命令，拔卡后卸载，插卡后重新挂载。
 * 独立 FreeRTOS 任务，熄屏/80 MHz 省电时照常运行。 */
static void sd_poll_locked(void)
{
    if (s_sd && (!bsp_sdcard || sdmmc_get_status(bsp_sdcard) != ESP_OK)) {
        detach_sd_locked();
        return;
    }
    if (!s_nvs_synced) { save_nvs_locked(); }
    if (!s_sd) {
        if (bsp_sdcard_mount() == ESP_OK) {
            s_sd = true;
            s_sd_dirty = false;
            s_sd_hash = 0;
            ESP_LOGI(TAG, "SD inserted, reconciling databases");
            reload_locked();
            net_guard_trigger();
        }
        return;
    }
    /* 本次挂载期间写失败的内容先补写，不能导入该卡上的旧版本。 */
    if (s_sd_dirty) {
        save_both_locked();
        return;
    }
    const esp_err_t rd = read_sd_locked();
    if (rd == ESP_FAIL) {
        detach_sd_locked();
    } else if (rd != ESP_OK) {
        save_both_locked();
    } else if (fnv1a(s_text) != s_sd_hash) {
        db_net_t live = {0};
        live_network_locked(&live);
        const esp_err_t err = import_sd_locked(&live);
        if (err == ESP_ERR_INVALID_ARG) { save_both_locked(); }
        if (err == ESP_OK) { net_guard_trigger(); }
    }
}

static void sd_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        lock();
        sd_poll_locked();
        unlock();
    }
}

esp_err_t db_init(void)
{
    if (!s_mtx) {
        s_mtx = xSemaphoreCreateMutex();
        if (!s_mtx) { return ESP_ERR_NO_MEM; }
    }
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) { return err; }
    lock();
    s_sd = bsp_sdcard_mount() == ESP_OK;
    err = reload_locked();
    unlock();
    if (!s_sd_task && xTaskCreate(sd_task, "db_sd", 6144, NULL, 3, &s_sd_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "database ready: %d networks, SD=%d; hotplug polling 1 s", s_count, s_sd);
    return err;
}

bool db_sd_present(void)
{
    return s_sd;
}

/* ------------------------------------------------------------ 读接口 */

void db_read_begin(void)
{
    lock();
}

void db_read_end(void)
{
    unlock();
}

int db_net_count(void)
{
    return s_count;
}

const db_net_t *db_net_at(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return NULL;
    }
    return &s_nets[idx];
}

const db_net_t *db_find(const char *ssid)
{
    if (ssid == NULL) {
        return NULL;
    }
    return net_find_locked(ssid);
}

void db_active_ssid(char *out, size_t cap)
{
    lock();
    strlcpy(out, s_active, cap);
    unlock();
}

void db_last_ok(char *out, size_t cap)
{
    lock();
    strlcpy(out, s_last_ok, cap);
    unlock();
}

esp_err_t db_set_last_ok(const char *ssid)
{
    lock();
    snapshot_locked();
    if (strcmp(s_last_ok, ssid ? ssid : "") != 0) {
        strlcpy(s_last_ok, ssid ? ssid : "", sizeof(s_last_ok));
        const esp_err_t err = commit_edit_locked();
        unlock();
        return err;
    }
    unlock();
    return ESP_OK;
}

bool db_other_ssid(const char *avoid, char *out, size_t cap)
{
    bool found = false;
    lock();
    for (int i = 0; i < s_count; i++) {
        if (avoid != NULL && strcmp(s_nets[i].ssid, avoid) == 0) {
            continue;
        }
        strlcpy(out, s_nets[i].ssid, cap);
        found = true;
        break;
    }
    unlock();
    return found;
}

/* ------------------------------------------------------------ 写接口 */

db_net_t *db_ensure_net(const char *ssid, const char *wifi_pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return NULL;
    }

    lock();
    snapshot_locked();
    db_net_t *n = net_ensure_locked(ssid);
    if (!n) {
        unlock();
        return NULL;
    }
    if (n != NULL && wifi_pass != NULL) {
        strlcpy(n->wifi_pass, wifi_pass, sizeof(n->wifi_pass));
    }
    /* 新增网络不直接抢 active：先让 net_guard 试连，连上了才切过去。
     * 否则用户加一个连不上的 SSID，会把正在用的网络也踢掉。 */
    if (s_active[0] == '\0') {
        strlcpy(s_active, ssid, sizeof(s_active));
    }
    const esp_err_t err = n ? commit_edit_locked() : ESP_ERR_NO_MEM;
    unlock();

    ESP_LOGI(TAG, "ensure net [%s]", ssid);
    return err == ESP_OK ? n : NULL;
}

esp_err_t db_set_active_ssid(const char *ssid)
{
    lock();
    snapshot_locked();
    strlcpy(s_active, ssid ? ssid : "", sizeof(s_active));
    const esp_err_t err = commit_edit_locked();
    unlock();
    return err;
}

esp_err_t db_remove_net(const char *ssid)
{
    lock();
    snapshot_locked();
    char live[DB_SSID_LEN], pass[DB_PASS_LEN];
    if (wifi_mgr_current_credentials(live, sizeof(live), pass, sizeof(pass)) &&
        strcmp(live, ssid) == 0) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) {
            memmove(&s_nets[i], &s_nets[i + 1], (size_t)(s_count - i - 1) * sizeof(db_net_t));
            s_count--;
            memset(&s_nets[s_count], 0, sizeof(db_net_t));
            if (strcmp(s_last_ok, ssid) == 0) { s_last_ok[0] = '\0'; }
            if (strcmp(s_active, ssid) == 0) {
                s_active[0] = '\0';
            }
            err = commit_edit_locked();
            break;
        }
    }
    unlock();
    ESP_LOGI(TAG, "remove net [%s]: %s", ssid, esp_err_to_name(err));
    return err;
}

esp_err_t db_upsert_acct(const char *ssid, const char *user, const char *pass)
{
    if (ssid == NULL || user == NULL || user[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    snapshot_locked();
    db_net_t *n = net_ensure_locked(ssid);
    if (n == NULL) {
        unlock();
        return ESP_ERR_NO_MEM;
    }

    int idx = acct_find_locked(n, user);
    if (idx >= 0) {
        strlcpy(n->acct[idx].pass, pass ? pass : "", sizeof(n->acct[idx].pass));
    } else {
        if (n->acct_count >= DB_MAX_ACCT) {
            unlock();
            return ESP_ERR_NO_MEM;
        }
        idx = n->acct_count++;
        strlcpy(n->acct[idx].user, user, sizeof(n->acct[idx].user));
        strlcpy(n->acct[idx].pass, pass ? pass : "", sizeof(n->acct[idx].pass));
    }
    if (n->current[0] == '\0') {
        strlcpy(n->current, user, sizeof(n->current));
    }
    const esp_err_t err = commit_edit_locked();
    unlock();

    ESP_LOGI(TAG, "upsert acct [%s] %s -> %s", ssid, user, esp_err_to_name(err));
    return err;
}

esp_err_t db_remove_acct(const char *ssid, const char *user)
{
    lock();
    snapshot_locked();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    db_net_t *n = net_find_locked(ssid);
    if (n != NULL) {
        const int idx = acct_find_locked(n, user);
        if (idx >= 0) {
            memmove(&n->acct[idx], &n->acct[idx + 1],
                    (size_t)(n->acct_count - idx - 1) * sizeof(db_acct_t));
            n->acct_count--;
            memset(&n->acct[n->acct_count], 0, sizeof(db_acct_t));
            if (strcmp(n->current, user) == 0) {
                n->current[0] = '\0';
            }
            err = commit_edit_locked();
        }
    }
    unlock();
    ESP_LOGI(TAG, "remove acct [%s] %s: %s", ssid, user, esp_err_to_name(err));
    return err;
}

esp_err_t db_set_current_acct(const char *ssid, const char *user)
{
    lock();
    snapshot_locked();
    db_net_t *n = net_find_locked(ssid);
    if (n == NULL || acct_find_locked(n, user) < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(n->current, user, sizeof(n->current));
    const esp_err_t err = commit_edit_locked();
    unlock();
    return err;
}

bool db_current_acct(const char *ssid, char *user, size_t ucap, char *pass, size_t pcap)
{
    bool ok = false;

    lock();
    const db_net_t *n = net_find_locked(ssid);
    if (n != NULL && n->acct_count > 0) {
        int idx = acct_find_locked(n, n->current);
        if (idx < 0) {
            idx = 0;
        }
        strlcpy(user, n->acct[idx].user, ucap);
        strlcpy(pass, n->acct[idx].pass, pcap);
        ok = true;
    }
    unlock();

    return ok;
}

void db_dump(void)
{
    lock();
    ESP_LOGI(TAG, "---- config (sd=%d) active=[%s] ----", (int)s_sd, s_active);
    for (int i = 0; i < s_count; i++) {
        const db_net_t *n = &s_nets[i];
        ESP_LOGI(TAG, " [%s] wifi_pass=%s current=%s accts=%d",
                 n->ssid, n->wifi_pass[0] ? "***" : "(open)", n->current, n->acct_count);
        for (int a = 0; a < n->acct_count; a++) {
            ESP_LOGI(TAG, "   - %s", n->acct[a].user);
        }
    }
    unlock();
}
