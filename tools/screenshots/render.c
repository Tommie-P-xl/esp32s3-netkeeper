/* Compile the real configuration UI, exposing its private screens only inside
 * this host executable. No screenshot hooks are compiled into the firmware. */
#include <assert.h>
#include <stdlib.h>
#include "../../main/ui_setup.c"
#include "batt_mgr.h"
#include "power_mgr.h"
#include "sys_time.h"
#include "web.h"

static db_net_t demo_nets[] = {
    {.ssid = "ExampleWiFi", .wifi_pass = "example-only", .acct_count = 2,
     .acct = {{"demo-user-01", "demo-password"}, {"demo-user-02", "demo-password"}},
     .current = "demo-user-01"},
    {.ssid = "LabDemo", .wifi_pass = "example-only"},
};
static void (*pending_scan)(void *);
static int net_deletions, account_deletions;
static char deleted_ssid[DB_SSID_LEN], deleted_user[DB_USER_LEN];

int64_t esp_timer_get_time(void) { return 4550000000LL; }
bool db_sd_present(void) { return true; }
void db_read_begin(void) {}
void db_read_end(void) {}
const db_net_t *db_find(const char *ssid)
{
    for (unsigned i = 0; i < sizeof(demo_nets) / sizeof(*demo_nets); ++i)
        if (!strcmp(ssid, demo_nets[i].ssid)) return &demo_nets[i];
    return NULL;
}
void db_active_ssid(char *out, size_t cap) { strlcpy(out, "ExampleWiFi", cap); }
esp_err_t db_set_active_ssid(const char *ssid) { (void)ssid; return ESP_OK; }
esp_err_t db_remove_net(const char *ssid)
{
    if (!strcmp(ssid, "ExampleWiFi")) return ESP_ERR_INVALID_STATE;
    ++net_deletions;
    strlcpy(deleted_ssid, ssid, sizeof(deleted_ssid));
    return ESP_OK;
}
db_net_t *db_ensure_net(const char *ssid, const char *pass)
{ (void)ssid; (void)pass; return &demo_nets[0]; }
esp_err_t db_upsert_acct(const char *ssid, const char *user, const char *pass)
{ (void)ssid; (void)user; (void)pass; return ESP_OK; }
esp_err_t db_remove_acct(const char *ssid, const char *user)
{
    ++account_deletions;
    strlcpy(deleted_ssid, ssid, sizeof(deleted_ssid));
    strlcpy(deleted_user, user, sizeof(deleted_user));
    return ESP_OK;
}
esp_err_t db_set_current_acct(const char *ssid, const char *user)
{ (void)ssid; (void)user; return ESP_OK; }
void net_guard_trigger(void) {}
void net_guard_switch_account(const char *ssid, const char *user) { (void)ssid; (void)user; }
void net_guard_try_ssid(const char *ssid) { (void)ssid; }
void net_guard_get(net_status_t *out)
{
    *out = (net_status_t){.state = NET_ONLINE, .ssid = "ExampleWiFi", .target = "ExampleWiFi",
        .ip = "192.0.2.10", .account = "demo-user-01", .portal = "http://portal.example.edu",
        .auth_count = 2, .boot_ms = 0, .next_check_ms = 4700000, .detail = "外网正常"};
}
bool power_mgr_screen_is_on(void) { return true; }
void batt_mgr_get(batt_info_t *out)
{ *out = (batt_info_t){.present=true, .usb=true, .charging=true, .pct=82, .mv=4080, .minutes_left=-1}; }
void sys_time_str(char *out, int cap) { strlcpy(out, "12:30", cap); }
void web_code_get(char out[7]) { memcpy(out, "123456", 7); }
int64_t web_code_remain_ms(void) { return 240000; }
void wifi_mgr_get_link(wifi_link_t *out)
{ *out = (wifi_link_t){.connected=true, .ssid="ExampleWiFi", .ip="192.0.2.10"}; }
int wifi_mgr_scan(wifi_ap_t *out, int max)
{
    const wifi_ap_t aps[] = {{"ExampleWiFi",-42,WIFI_AUTH_WPA2_PSK},
        {"LibraryDemo",-56,WIFI_AUTH_WPA2_PSK}, {"LabDemo",-65,WIFI_AUTH_WPA2_PSK},
        {"GuestDemo",-72,WIFI_AUTH_OPEN}};
    int n = max < 4 ? max : 4;
    memcpy(out, aps, n * sizeof(*out));
    return n;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    assert(pending_scan == NULL);
    pending_scan = fn;
    return pdPASS;
}
void vTaskDelete(void *task) { (void)task; }
static void complete_scan(void)
{
    void (*fn)(void *) = pending_scan;
    assert(fn);
    pending_scan = NULL;
    fn(NULL);
}
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(disp); }
static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}
static void capture(const char *folder, const char *name)
{
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    assert(snap && snap->header.w == 480 && snap->header.h == 480);
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n480 480\n255\n");
    for (unsigned y = 0; y < 480; ++y) {
        const uint8_t *row = snap->data + y * snap->header.stride;
        for (unsigned x = 0; x < 480; ++x) {
            const uint8_t rgb[] = {row[3*x+2], row[3*x+1], row[3*x]};
            size_t written = fwrite(rgb, 1, 3, file);
            assert(written == 3);
        }
    }
    fclose(file);
    lv_draw_buf_destroy(snap);
    printf("Rendered %s\n", name);
}
static lv_obj_t *find_button(lv_obj_t *parent, const char *text)
{
    if (lv_obj_check_type(parent, &lv_button_class)) {
        lv_obj_t *label = lv_obj_get_child(parent, 0);
        if (label && lv_obj_check_type(label, &lv_label_class) &&
            !strcmp(lv_label_get_text(label), text)) return parent;
    }
    for (unsigned i = 0; i < lv_obj_get_child_count(parent); ++i) {
        lv_obj_t *button = find_button(lv_obj_get_child(parent, i), text);
        if (button) return button;
    }
    return NULL;
}
static void click(lv_obj_t *parent, const char *text)
{
    lv_obj_t *button = find_button(parent, text);
    assert(button);
    lv_obj_send_event(button, LV_EVENT_CLICKED, NULL);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    lv_init();
    lv_display_t *disp = lv_display_create(480, 480);
    static uint8_t pixels[480 * 48 * 2];
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);
    ui_create(disp);
    advance(600);
    capture(argv[1], "01_main");
    ui_setup_open_wifi();
    complete_scan();
    advance(100);
    capture(argv[1], "02_wifi");
    lv_obj_t *first = lv_obj_get_child(s_list_wifi, 0);
    start_scan();
    advance(250);
    capture(argv[1], "02_wifi_scanning");
    complete_scan();
    assert(first == lv_obj_get_child(s_list_wifi, 0));
    click(lv_obj_get_child(s_list_wifi, 2), "删除");
    assert(s_delete_modal && net_deletions == 0);
    advance(100);
    capture(argv[1], "08_delete_wifi");
    click(s_delete_modal, "取消");
    assert(!s_delete_modal && net_deletions == 0);
    strlcpy(s_pending_ssid, "LibraryDemo", sizeof(s_pending_ssid));
    build_pass_screen();
    lv_screen_load(s_scr_pass);
    lv_textarea_set_password_show_time(s_pass_ta, 0);
    lv_textarea_set_text(s_pass_ta, "example-only");
    advance(100);
    capture(argv[1], "03_pass");
    strlcpy(s_acct_ssid, "ExampleWiFi", sizeof(s_acct_ssid));
    ui_setup_open_accounts();
    advance(100);
    capture(argv[1], "04_acct");
    click(lv_obj_get_child(s_list_acct, 1), "删除");
    assert(s_delete_modal && account_deletions == 0);
    advance(100);
    capture(argv[1], "09_delete_account");
    click(s_delete_modal, "取消");
    assert(!s_delete_modal && account_deletions == 0);
    on_add_acct(NULL);
    lv_textarea_set_text(s_add_user, "demo-user-03");
    lv_textarea_set_password_show_time(s_add_pass, 0);
    lv_textarea_set_text(s_add_pass, "example-only");
    advance(100);
    capture(argv[1], "05_add");

    /* Confirmations hold their own target even if a row's backing data changes. */
    char row_ssid[DB_SSID_LEN] = "LabDemo", row_user[DB_USER_LEN] = "demo-user-02";
    show_delete_confirm(row_ssid, NULL);
    strlcpy(row_ssid, "AnotherDemo", sizeof(row_ssid));
    show_delete_confirm("AnotherDemo", NULL); /* An existing modal cannot be overwritten. */
    click(s_delete_modal, "确认删除");
    assert(net_deletions == 1 && !strcmp(deleted_ssid, "LabDemo"));
    confirm_delete_cb(NULL);
    assert(net_deletions == 1);
    complete_scan();
    show_delete_confirm("ExampleWiFi", row_user);
    strlcpy(row_user, "different-demo", sizeof(row_user));
    click(s_delete_modal, "确认删除");
    assert(account_deletions == 1 && !strcmp(deleted_ssid, "ExampleWiFi") && !strcmp(deleted_user, "demo-user-02"));
    confirm_delete_cb(NULL);
    assert(account_deletions == 1);
    show_delete_confirm("ExampleWiFi", NULL);
    click(s_delete_modal, "确认删除");
    assert(!s_delete_modal && net_deletions == 1); /* Active network protection still applies. */
    puts("PASS real LVGL delete dialogs: cancel, explicit confirmation, frozen targets, duplicate guard and active-network protection");
    return 0;
}
