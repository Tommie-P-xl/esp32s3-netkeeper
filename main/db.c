#include "db.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app_config.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "db";

#define NVS_NS       "portal"
#define NVS_KEY_CFG  "cfg"
#define NVS_KEY_HASH "sd_hash"

static db_net_t s_nets[DB_MAX_NETS];
static int s_count;
static char s_active[DB_SSID_LEN];
static char s_last_ok[DB_SSID_LEN];
static bool s_sd;
static uint32_t s_sd_hash;      /* 最后一次与 SD 同步时的文本哈希 */
static SemaphoreHandle_t s_mtx;
static nvs_handle_t s_nvs;

/* 序列化用的缓冲，避免每次改动都 malloc 4 KB */
static char s_text[DB_TEXT_MAX];

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

    if (s_active[0] != '\0') {
        sb_put(&sb, "active=%s\n", s_active);
    }
    if (s_last_ok[0] != '\0') {
        sb_put(&sb, "last_ok=%s\n", s_last_ok);
    }

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
    esp_err_t err = nvs_set_blob(s_nvs, NVS_KEY_CFG, s_text, len);
    if (err == ESP_OK) {
        s_sd_hash = fnv1a(s_text);
        err = nvs_set_u32(s_nvs, NVS_KEY_HASH, s_sd_hash);
    }
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs write failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t save_sd_locked(void)
{
    if (!s_sd) {
        return ESP_OK;
    }

    mkdir(DB_SD_DIR, 0775);

    const char *tmp = DB_SD_DIR "/net.tmp";
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "cannot open %s", tmp);
        return ESP_FAIL;
    }

    /* BOM：Windows 记事本打开中文注释不乱码，解析时跳过 */
    fputs("\xEF\xBB\xBF", f);
    const size_t len = strlen(s_text);
    const size_t wr = fwrite(s_text, 1, len, f);
    const bool ok = (fclose(f) == 0) && (wr == len);
    if (!ok) {
        ESP_LOGW(TAG, "sd write short");
        return ESP_FAIL;
    }

    remove(DB_SD_FILE);
    if (rename(tmp, DB_SD_FILE) != 0) {
        ESP_LOGW(TAG, "sd rename failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t db_save(void)
{
    lock();
    const esp_err_t e1 = save_nvs_locked();
    const esp_err_t e2 = save_sd_locked();
    unlock();

    return (e1 != ESP_OK) ? e1 : e2;
}

/* ---------------------------------------------------------------- 解析 */

static void seed_defaults_locked(void)
{
    s_count = 0;
    memset(s_nets, 0, sizeof(s_nets));

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
static void parse_text_locked(char *buf)
{
    memset(s_nets, 0, sizeof(s_nets));
    s_count = 0;
    s_active[0] = '\0';

    db_net_t *cur = NULL;
    char *save = NULL;

    for (char *line = strtok_r(buf, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        char *p = line;

        /* 首行可能带 UTF-8 BOM */
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
            (unsigned char)p[2] == 0xBF) {
            p += 3;
        }

        p = str_trim(p);
        if (*p == '\0' || *p == '#' || *p == ';' || *p == '\r') {
            continue;
        }

        if (*p == '[') {
            char *close = strchr(p, ']');
            if (close == NULL) {
                continue;
            }
            *close = '\0';
            char *name = str_trim(p + 1);
            if (strcasecmp(name, "general") == 0) {
                cur = NULL;
            } else if (name[0] != '\0') {
                cur = net_ensure_locked(name);
            }
            continue;
        }

        char *eq = strchr(p, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *key = str_trim(p);
        char *val = str_trim(eq + 1);

        if (strcasecmp(key, "active") == 0) {
            strlcpy(s_active, val, sizeof(s_active));
            continue;
        }
        if (strcasecmp(key, "last_ok") == 0) {
            strlcpy(s_last_ok, val, sizeof(s_last_ok));
            continue;
        }
        if (cur == NULL) {
            continue;
        }

        if (strcasecmp(key, "wifi") == 0 || strcasecmp(key, "wifi_pass") == 0 ||
            strcasecmp(key, "password") == 0) {
            strlcpy(cur->wifi_pass, val, sizeof(cur->wifi_pass));
        } else if (strcasecmp(key, "current") == 0) {
            strlcpy(cur->current, val, sizeof(cur->current));
        } else if (strcasecmp(key, "account") == 0) {
            char *bar = strchr(val, '|');
            if (bar == NULL) {
                continue;
            }
            *bar = '\0';
            char *user = str_trim(val);
            char *pass = str_trim(bar + 1);
            if (*user == '\0' || cur->acct_count >= DB_MAX_ACCT) {
                continue;
            }
            db_acct_t *a = &cur->acct[cur->acct_count++];
            strlcpy(a->user, user, sizeof(a->user));
            strlcpy(a->pass, pass, sizeof(a->pass));
        }
    }
}

/* 从 NVS 读文本；成功后 s_text 里有内容 */
static esp_err_t load_nvs_locked(void)
{
    size_t len = sizeof(s_text);
    esp_err_t err = nvs_get_blob(s_nvs, NVS_KEY_CFG, s_text, &len);
    if (err != ESP_OK) {
        return err;
    }
    s_text[sizeof(s_text) - 1] = '\0';

    uint32_t h = 0;
    s_sd_hash = (nvs_get_u32(s_nvs, NVS_KEY_HASH, &h) == ESP_OK) ? h : 0;

    parse_text_locked(s_text);
    return ESP_OK;
}

/* 从 SD 读文本到 s_text */
static esp_err_t read_sd_locked(void)
{
    if (!s_sd) {
        return ESP_ERR_INVALID_STATE;
    }
    FILE *f = fopen(DB_SD_FILE, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t n = fread(s_text, 1, sizeof(s_text) - 1, f);
    fclose(f);
    s_text[n] = '\0';
    return (n > 0) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/*
 * 加载优先级（按需求）：SD 卡 > 片上 Flash。
 *
 *   SD 里有文件且能解析出内容  -> 以卡为准，直接覆盖 NVS（用户手改的内容生效）
 *   SD 没卡 / 没文件 / 解析为空 -> 用 NVS
 *   NVS 也没有                -> 编译期默认值播种，并写一份到卡上
 *
 * "能解析出内容" 的判定：至少有一个 [SSID] 段，或者至少有 active=。
 * 这样卡里被写坏了（比如记事本存成乱码）不会被当成有效配置，Flash 的还留着。
 */
esp_err_t db_reload(void)
{
    lock();

    esp_err_t nvs_err = load_nvs_locked();
    const bool nvs_ok = (nvs_err == ESP_OK) && (s_count > 0 || s_active[0] != '\0');

    char sd_text[DB_TEXT_MAX];
    bool sd_has_file = false;

    if (s_sd) {
        FILE *f = fopen(DB_SD_FILE, "rb");
        if (f != NULL) {
            const size_t n = fread(sd_text, 1, sizeof(sd_text) - 1, f);
            fclose(f);
            sd_text[n] = '\0';
            sd_has_file = (n > 0);
        }
    }

    esp_err_t result = ESP_OK;

    if (sd_has_file) {
        parse_text_locked(sd_text);
        const bool sd_valid = (s_count > 0) || (s_active[0] != '\0');

        if (sd_valid) {
            /* SD 优先：覆盖 Flash。卡本身不重写，保留用户手改的排版。 */
            result = save_nvs_locked();
            ESP_LOGI(TAG, "SD is authoritative -> flash overwritten (%d nets)", s_count);
        } else if (nvs_ok) {
            /* 卡里内容无效，退回 Flash，不破坏卡上的原文件 */
            load_nvs_locked();
            ESP_LOGW(TAG, "SD content unparsable, falling back to flash");
        } else {
            seed_defaults_locked();
            ESP_LOGW(TAG, "SD content unparsable and no flash config, seeded defaults");
            result = save_nvs_locked();
        }
    } else if (nvs_ok) {
        ESP_LOGI(TAG, "load from flash (sd %s)", s_sd ? "has no file" : "absent");
        save_sd_locked(); /* 卡在但没文件：补写一份做备份 */
    } else {
        seed_defaults_locked();
        ESP_LOGI(TAG, "seeded defaults (no config anywhere)");
        result = save_nvs_locked();
        save_sd_locked();
    }

    const int count = s_count;
    unlock();

    ESP_LOGI(TAG, "loaded %d network(s), active=[%s], sd=%d", count, s_active, (int)s_sd);
    return result;
}

esp_err_t db_restore_from_sd(void)
{
    lock();
    const esp_err_t rd = read_sd_locked();
    if (rd != ESP_OK) {
        unlock();
        return rd;
    }
    parse_text_locked(s_text);
    const esp_err_t err = save_nvs_locked();
    save_sd_locked();
    unlock();

    ESP_LOGI(TAG, "restored from SD: %d nets", s_count);
    return err;
}

esp_err_t db_init(void)
{
    if (s_mtx == NULL) {
        s_mtx = xSemaphoreCreateMutex();
        if (s_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = bsp_sdcard_mount();
    if (err == ESP_OK) {
        s_sd = true;
        ESP_LOGI(TAG, "SD mounted at %s", BSP_SD_MOUNT_POINT);
    } else {
        s_sd = false;
        ESP_LOGW(TAG, "no SD card (%s), flash-only mode", esp_err_to_name(err));
    }

    return db_reload();
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
    if (strcmp(s_last_ok, ssid ? ssid : "") != 0) {
        strlcpy(s_last_ok, ssid ? ssid : "", sizeof(s_last_ok));
        save_nvs_locked();
        save_sd_locked();
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
    db_net_t *n = net_ensure_locked(ssid);
    if (n != NULL && wifi_pass != NULL) {
        strlcpy(n->wifi_pass, wifi_pass, sizeof(n->wifi_pass));
    }
    /* 新增网络不直接抢 active：先让 net_guard 试连，连上了才切过去。
     * 否则用户加一个连不上的 SSID，会把正在用的网络也踢掉。 */
    if (s_active[0] == '\0') {
        strlcpy(s_active, ssid, sizeof(s_active));
    }
    save_nvs_locked();
    save_sd_locked();
    unlock();

    ESP_LOGI(TAG, "ensure net [%s]", ssid);
    return n;
}

esp_err_t db_set_active_ssid(const char *ssid)
{
    lock();
    strlcpy(s_active, ssid ? ssid : "", sizeof(s_active));
    const esp_err_t err = save_nvs_locked();
    save_sd_locked();
    unlock();
    return err;
}

esp_err_t db_remove_net(const char *ssid)
{
    lock();
    esp_err_t err = ESP_ERR_NOT_FOUND;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) {
            memmove(&s_nets[i], &s_nets[i + 1], (size_t)(s_count - i - 1) * sizeof(db_net_t));
            s_count--;
            memset(&s_nets[s_count], 0, sizeof(db_net_t));
            if (strcmp(s_active, ssid) == 0) {
                s_active[0] = '\0';
            }
            err = save_nvs_locked();
            save_sd_locked();
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
    const esp_err_t err = save_nvs_locked();
    save_sd_locked();
    unlock();

    ESP_LOGI(TAG, "upsert acct [%s] %s -> %s", ssid, user, esp_err_to_name(err));
    return err;
}

esp_err_t db_remove_acct(const char *ssid, const char *user)
{
    lock();
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
            err = save_nvs_locked();
            save_sd_locked();
        }
    }
    unlock();
    ESP_LOGI(TAG, "remove acct [%s] %s: %s", ssid, user, esp_err_to_name(err));
    return err;
}

esp_err_t db_set_current_acct(const char *ssid, const char *user)
{
    lock();
    db_net_t *n = net_find_locked(ssid);
    if (n == NULL || acct_find_locked(n, user) < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(n->current, user, sizeof(n->current));
    const esp_err_t err = save_nvs_locked();
    save_sd_locked();
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
