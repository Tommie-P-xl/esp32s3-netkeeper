"""Host regression tests for the firmware's changed logic (requires gcc).

Compile the actual C sources with small hardware/HTTP stubs. No board, network
credentials or external Python packages are needed. Generated files stay local.
Run: python tools/test_logic.py
"""
from pathlib import Path
import subprocess
import shutil

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / ".local" / "logic-tests"
OUT.mkdir(parents=True, exist_ok=True)

COMMON = r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <setjmp.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_SIZE 3
#define ESP_ERR_NOT_FOUND 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_NVS_NOT_FOUND 6
static inline const char *esp_err_to_name(int e) { return "mock-error"; }
static inline size_t strlcpy(char *d, const char *s, size_t n) {
 size_t k=strlen(s); if(n) {size_t z=k<n-1?k:n-1;memcpy(d,s,z);d[z]=0;} return k;
}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
typedef unsigned nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *, int, nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_set_str(nvs_handle_t,const char *,const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
static inline int64_t esp_timer_get_time(void) {return 123456789;}
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(...) ((void)0)
#define portEXIT_CRITICAL(...) ((void)0)
#define pdMS_TO_TICKS(x) (x)
#define pdPASS 1
#define pdTRUE 1
#define xTaskCreate(...) pdPASS
#define vTaskDelay(...) ((void)0)
typedef void *SemaphoreHandle_t;
#define xSemaphoreCreateBinary() ((void *)1)
#define xSemaphoreGive(...) 1
#define xSemaphoreTake(...) 0
typedef void lv_display_t;
typedef void *i2c_master_dev_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef struct {int dev_addr_length,device_address,scl_speed_hz;} i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 0
esp_err_t i2c_master_transmit_receive(void *,const uint8_t *,size_t,void *,size_t,int);
esp_err_t i2c_master_transmit(void *,const uint8_t *,size_t,int);
esp_err_t i2c_master_bus_add_device(void *,const i2c_device_config_t *,void **);
i2c_master_bus_handle_t bsp_i2c_get_handle(void);
#define ESP_SLEEP_WAKEUP_ALL 0
void esp_sleep_disable_wakeup_source(int);
void esp_deep_sleep_start(void);
'''
for name in ("esp_err.h", "esp_log.h", "esp_timer.h", "nvs.h", "esp_wifi.h", "esp_event.h", "esp_netif.h", "esp_http_client.h",
             "lvgl.h", "esp_sleep.h", "driver/i2c_master.h", "bsp/esp-bsp.h",
             "freertos/FreeRTOS.h", "freertos/task.h", "freertos/semphr.h", "freertos/event_groups.h"):
    p = OUT / name
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text('#include "common.h"\n', encoding="utf-8")
(OUT / "common.h").write_text(COMMON, encoding="utf-8")

# These pure helpers are compiled verbatim; exclude only the platform HTTP client.
http = (ROOT / "main/http_util.c").read_text(encoding="utf-8")
(OUT / "http_helpers.c").write_text(
    '#include "common.h"\n#include "http_util.h"\n' + http[http.index("bool http_url_encode("):], encoding="utf-8")

HTTP_TEST = r'''
#include "common.h"
#include "http_util.h"
int main(void) {
 char out[64], enc[256];
 assert(http_form_get("ssid=%E6%A0%A1%E5%9B%AD%E6%97%A0%E7%BA%BF%E7%BD%91%E7%BB%9C", "ssid", out,33)==ESP_OK);
 assert(!strcmp(out,"校园无线网络"));
 assert(http_form_get("ssid=12345678901234567890123456789012","ssid",out,33)==ESP_OK);
 assert(http_form_get("ssid=123456789012345678901234567890123","ssid",out,33)==ESP_ERR_INVALID_SIZE && !out[0]);
 assert(http_form_get("pass=%2B%26%25%22%5C+z","pass",out,sizeof(out))==ESP_OK && !strcmp(out,"+&%\"\\ z"));
 const char *bad[]={"pass=%","pass=%A","pass=%GG","pass=a%00b"};
 for(size_t i=0;i<4;i++) assert(http_form_get(bad[i],"pass",out,sizeof(out))==ESP_ERR_INVALID_ARG && !out[0]);
 assert(http_form_get("use=1","pass",out,sizeof(out))==ESP_ERR_NOT_FOUND && !out[0]);
 assert(http_form_get("pass=&use=1","pass",out,sizeof(out))==ESP_OK && !out[0]);
 assert(http_json_escape("a\"b\\c\n\t",out,sizeof(out)) && !strcmp(out,"a\\\"b\\\\c\\u000a\\u0009"));
 assert(!http_json_escape("\"",out,2) && !out[0]);
 assert(http_url_encode("u+&@",enc,sizeof(enc)) && !strcmp(enc,"u%2B%26%40"));
 assert(!http_url_encode("+",out,3) && !out[0]);
 puts("PASS form decode, UTF-8 limits, malformed input, JSON and URL encoding");
}
'''

BATTERY_TEST = r'''
#include "common.h"
#include "batt_mgr.c"
static uint8_t regs[256], saved;
static jmp_buf stopped;
static bool read_error;
esp_err_t nvs_open(const char *s,int m,nvs_handle_t *h){*h=1;return 0;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char*k,uint8_t*v){*v=saved;return 0;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char*k,uint8_t v){saved=v;return 0;}
esp_err_t nvs_commit(nvs_handle_t h){return 0;}
void *bsp_i2c_get_handle(void){return (void*)1;}
esp_err_t i2c_master_bus_add_device(void*b,const i2c_device_config_t*c,void**d){*d=(void*)1;return 0;}
esp_err_t i2c_master_transmit_receive(void*d,const uint8_t*r,size_t rn,void*v,size_t n,int t){
 if(read_error)return ESP_FAIL;memcpy(v,regs+*r,n);return 0;
}
esp_err_t i2c_master_transmit(void*d,const uint8_t*v,size_t n,int t){
 regs[v[0]]=v[1];if(v[0]==0x10 && (v[1]&1))longjmp(stopped,1);return 0;
}
void esp_sleep_disable_wakeup_source(int s){}
void esp_deep_sleep_start(void){longjmp(stopped,2);}
bool power_mgr_screen_is_on(void){return false;}
bool power_mgr_is_low_power(void){return false;}
static void voltage(int mv){regs[0x34]=mv>>8;regs[0x35]=mv&255;}
static void reset(bool usb,int mv){
 memset(regs,0,sizeof(regs));regs[0]=8|(usb?32:0);regs[0x64]=0xe7;voltage(mv);
 s_dev=NULL;s_ok=false;s_low_latched=false;s_low_samples=0;read_error=false;
}
int main(void){
 saved=0;reset(true,3800);assert(batt_mgr_init()==0);
 assert(regs[0x64]==0xe3 && (regs[0x62]&31)==9 && (regs[0x63]&31)==0x12);
 assert((regs[0x24]&7)==4 && (regs[0x67]&0x44)==0x44 && (regs[0x18]&2));
 for(int phase=0;phase<=7;phase++){
  regs[1]=0x10|phase;batt_poll();assert(s_info.charging==(phase<=3));assert(s_info.full==(phase==4));
 }
 reset(false,3200);if(!setjmp(stopped)){batt_mgr_init();assert(!"low boot permitted");}assert(saved==1);
 reset(false,3400);if(!setjmp(stopped)){batt_mgr_init();assert(!"locked boot permitted");}
 reset(true,3200);assert(batt_mgr_init()==0 && saved==1);
 regs[0]=8;batt_poll();batt_poll();if(!setjmp(stopped)){batt_poll();assert(!"unplugged low battery kept running");}
 reset(false,3500);assert(batt_mgr_init()==0 && saved==0);
 voltage(3290);batt_poll();batt_poll();voltage(3600);batt_poll();assert(!s_low_samples);
 voltage(3290);batt_poll();read_error=true;batt_poll();assert(!s_low_samples);read_error=false;
 batt_poll();batt_poll();if(!setjmp(stopped)){batt_poll();assert(!"low shutdown missing");}
 assert(saved==1);
 reset(true,3200);assert(batt_mgr_init()==0);voltage(3500);batt_poll();assert(!saved);
 puts("PASS charger readback, all charge states, persistent cutoff, boot lock, USB and hysteresis");
}
'''

SRUN_TEST = r'''
#include "common.h"
#include "app_config.h"
static char fallback_host[64]=APP_PORTAL_FALLBACK_HOST;
#undef APP_PORTAL_FALLBACK_HOST
#define APP_PORTAL_FALLBACK_HOST fallback_host
#include "srun_port.c"
static int primary_status=503,secondary_status=204,calls,queries,challenges,logins;
enum {INFO_OFFLINE,INFO_ONLINE,INFO_HTML,INFO_FAIL};
static int info_mode;
static bool cached;
static char cached_host[64],cached_acid[8],cached_ssid[33];
static char primary_location[192],secondary_location[192];
static char blocked_host[64];
static char last_json[1024], last_login[5120];
esp_err_t nvs_open(const char *s,int m,nvs_handle_t *h){if(m==NVS_READONLY && !cached)return ESP_ERR_NOT_FOUND;*h=1;return ESP_OK;}
esp_err_t nvs_get_str(nvs_handle_t h,const char*k,char*v,size_t*n){
 const char *value=!strcmp(k,"host")?cached_host:!strcmp(k,"acid")?cached_acid:cached_ssid;
 if(strlen(value)+1>*n)return ESP_ERR_INVALID_SIZE;strlcpy(v,value,*n);return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h,const char*k,const char*v){
 if(!strcmp(k,"host"))strlcpy(cached_host,v,sizeof(cached_host));
 else if(!strcmp(k,"acid"))strlcpy(cached_acid,v,sizeof(cached_acid));
 else strlcpy(cached_ssid,v,sizeof(cached_ssid));return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h){cached=true;return ESP_OK;}
void nvs_close(nvs_handle_t h){}
void wifi_mgr_ssid(char *s,size_t n){strlcpy(s,"test",n);}
void srun_hmac_md5_hex(const char*k,const char*m,char*out){memset(out,'a',32);out[32]=0;}
void srun_sha1_hex(const char*m,size_t n,char*out){memset(out,'b',40);out[40]=0;}
void srun_encode_info(const char*j,const char*t,char*out,size_t n){strlcpy(last_json,j,sizeof(last_json));strlcpy(out,"{SRBX1}a+/=",n);}
esp_err_t http_get_capture(const char*u,int timeout,http_resp_t*r){
 memset(r,0,sizeof(*r));r->status=200;calls++;
 assert(!strstr(u,"http://0.0.0.0") && !strstr(u,"https://0.0.0.0"));
 if(blocked_host[0] && !strncmp(u,blocked_host,strlen(blocked_host)))return ESP_FAIL;
 if(!strcmp(u,APP_PROBE_URL_PRIMARY)){r->status=primary_status;strlcpy(r->location,primary_location,sizeof(r->location));}
 else if(!strcmp(u,APP_PROBE_URL_SECONDARY)){r->status=secondary_status;strlcpy(r->location,secondary_location,sizeof(r->location));}
 else if(strstr(u,"get_challenge")){++challenges;strlcpy(r->body,"jsonp({\"challenge\":\"token\",\"client_ip\":\"10.1.2.3\"})",sizeof(r->body));}
 else if(strstr(u,"action=login")){++logins;strlcpy(last_login,u,sizeof(last_login));strlcpy(r->body,"jsonp({\"error\":\"ok\",\"suc_msg\":\"login_ok\"})",sizeof(r->body));}
 else if(strstr(u,"action=logout"))strlcpy(r->body,"jsonp({\"error\":\"ok\",\"suc_msg\":\"logout_ok\"})",sizeof(r->body));
 else if(strstr(u,"rad_user_info")){
  ++queries;if(info_mode==INFO_FAIL)return ESP_FAIL;
  strlcpy(r->body,info_mode==INFO_ONLINE?"jsonp({\"user_name\":\"old-account\"})":
    info_mode==INFO_OFFLINE?"jsonp({\"error\":\"not_online_error\"})":"<html>unrelated, page</html>",sizeof(r->body));
 }
 else r->status=503;
 return ESP_OK;
}
int main(void){
 portal_info_t pi;char detail[96],user[64];
 int errors[]={403,500,503};
 for(size_t i=0;i<3;i++){primary_status=errors[i];calls=0;assert(srun_probe(&pi)==PROBE_ONLINE && calls==2);}
 primary_status=200;calls=0;assert(srun_probe(&pi)==PROBE_ONLINE && calls==2 && !queries && !challenges);
 primary_status=302;strlcpy(primary_location,"http://portal.test/srun_portal_pc?ac_id=9",sizeof(primary_location));
 calls=0;assert(srun_probe(&pi)==PROBE_ONLINE && calls==2 && !queries && !cached);
 primary_location[0]=0;
 primary_status=503;secondary_status=302;calls=0;
 strlcpy(secondary_location,"http://portal.test/login?ac_id=9",sizeof(secondary_location));
 assert(srun_probe(&pi)==PROBE_NEED_AUTH && !strcmp(pi.portal_base,"http://portal.test") && !strcmp(pi.ac_id,"9"));
 assert(queries==1 && cached && !challenges && !logins);
 cached=false;primary_status=200;secondary_status=200;secondary_location[0]=0;info_mode=INFO_ONLINE;
 assert(srun_probe(&pi)==PROBE_NO_LINK && strstr(pi.detail,"在线") && !challenges && !logins);
 info_mode=INFO_HTML;assert(srun_probe(&pi)==PROBE_NO_LINK && !challenges && !logins);
 strlcpy(fallback_host,"http://0.0.0.0",sizeof(fallback_host));cached=true;
 strlcpy(cached_host,"http://0.0.0.0:80",sizeof(cached_host));strlcpy(cached_acid,"3",sizeof(cached_acid));strlcpy(cached_ssid,"test",sizeof(cached_ssid));
 const int before_queries=queries;
 assert(srun_probe(&pi)==PROBE_NO_LINK && !pi.portal_base[0] && queries==before_queries);
 assert(strstr(pi.detail,"未发现") && !challenges && !logins);
 calls=0;assert(srun_login(&pi,"user","password","",detail,sizeof(detail))==LOGIN_FAIL && !calls);
 assert(!srun_logout(&pi,"user",detail,sizeof(detail)) && !calls);
 assert(srun_online_user(&pi,user,sizeof(user))==ESP_ERR_INVALID_STATE && !calls);
 cached=false;strlcpy(fallback_host,"http://fallback.test",sizeof(fallback_host));
 portal_defaults(&pi);
 assert(!parse_location("http://router.test/login",&pi));
 assert(!parse_location("http://0.0.0.0/srun_portal_pc?ac_id=3",&pi));
 assert(!parse_location("http://portal.test/login?ac_id=3oops",&pi));
 assert(parse_location("http://portal.test:8080?ac_id=9",&pi) && !strcmp(pi.portal_base,"http://portal.test:8080"));
 assert(parse_location("https://portal.test/srun_portal_success?ac_id=3&theme=pro",&pi) && !strcmp(pi.ac_id,"3"));
 primary_status=204;calls=0;assert(srun_probe(&pi)==PROBE_ONLINE && calls==1);
 primary_status=503;secondary_status=503;info_mode=INFO_OFFLINE;
 assert(srun_probe(&pi)==PROBE_NEED_AUTH && !strcmp(pi.portal_base,"http://fallback.test"));
 cached=true;strlcpy(cached_host,"http://stale.test",sizeof(cached_host));
 strlcpy(cached_acid,"1",sizeof(cached_acid));strlcpy(cached_ssid,"test",sizeof(cached_ssid));
 strlcpy(blocked_host,cached_host,sizeof(blocked_host));
 assert(srun_probe(&pi)==PROBE_NEED_AUTH && !strcmp(pi.portal_base,"http://fallback.test"));
 blocked_host[0]=0;
 strlcpy(pi.portal_base,"http://portal.test",sizeof(pi.portal_base));strlcpy(pi.ac_id,"9",sizeof(pi.ac_id));
 assert(srun_login(&pi,"u+&","a\"b\\c","@x",detail,sizeof(detail))==LOGIN_OK);
 assert(strstr(last_json,"a\\\"b\\\\c") && strstr(last_login,"username=u%2B%26%40x"));
 char longpass[64];memset(longpass,'\\',63);longpass[63]=0;
 assert(srun_login(&pi,"normal",longpass,"",detail,sizeof(detail))==LOGIN_OK);
 info_mode=INFO_ONLINE;assert(srun_online_user(&pi,user,sizeof(user))==ESP_OK && !strcmp(user,"old-account"));
 assert(srun_logout(&pi,user,detail,sizeof(detail)));
 puts("PASS ambiguous HTTP 200/302, secondary 204, verified SRun discovery, invalid Portal guards, cache, credentials and logout");
}
'''

GUARD_TEST = r'''
#include "common.h"
#include "net_guard.c"
static int probe_count, logout_count,login_count,query_error;
static bool logout_ok=true;
static char online_user[64]="old";
void wifi_mgr_get_link(wifi_link_t *out){*out=(wifi_link_t){.connected=true};}
probe_result_t srun_probe(portal_info_t*pi){memset(pi,0,sizeof(*pi));strlcpy(pi->portal_base,"http://portal.test",64);probe_count++;return PROBE_ONLINE;}
esp_err_t srun_online_user(const portal_info_t*pi,char*u,size_t n){strlcpy(u,online_user,n);return query_error;}
bool srun_logout(const portal_info_t*pi,const char*u,char*d,size_t n){logout_count++;return logout_ok;}
login_result_t srun_login(const portal_info_t*pi,const char*u,const char*p,const char*domain,char*d,size_t n){
 login_count++;strlcpy(online_user,u,sizeof(online_user));strlcpy(d,"ok",n);return LOGIN_OK;
}
int main(void){
 assert(run_cycle("wifi","new","password",false));
 assert(!logout_count && !login_count && !strcmp(s_st.account,"old"));
 query_error=ESP_ERR_NOT_FOUND;assert(run_cycle("wifi","new","password",false) && !login_count);
 query_error=ESP_OK;probe_count=0;
 assert(run_cycle("wifi","new","password",true));
 assert(logout_count==1 && login_count==1 && probe_count==2 && !strcmp(online_user,"new"));
 assert(run_cycle("wifi","new","password",false));assert(logout_count==1 && login_count==1);
 strlcpy(online_user,"old",64);logout_ok=false;
 assert(!run_cycle("wifi","new","password",true) && login_count==1);
 query_error=ESP_FAIL;logout_ok=true;
 assert(!run_cycle("wifi","new","password",true) && login_count==1);
 puts("PASS live account switch, same-account no-op, logout and identity-query failure");
}
'''

# Compile the actual power transitions and supervisor loop with controlled time,
# touch and display-lock stubs, so retries exercise the firmware's real code.
power = (ROOT / "main/power_mgr.c").read_text(encoding="utf-8")
(OUT / "power-under-test.c").write_text(
    '#include "app_config.h"\ntypedef unsigned TickType_t;\n' +
    power[power.index("#define POWER_MGR_LIGHT_SLEEP"):power.index("/* ---------------------------------------------------------------- cpu freq */")] +
    power[power.index("static void power_task("):power.index("/* -------------------------------------------------------------------- init */")],
    encoding="utf-8")

POWER_TEST = r'''
#include "common.h"
static int lock_calls, lock_failures, writes, cpu_mhz, steps, limit, gpio_calls;
static unsigned now;
static bool locked, driver_fails, touch_edge;
static jmp_buf finish;
static void test_delay(unsigned ticks);
#undef vTaskDelay
#define vTaskDelay(ticks) test_delay(ticks)
#define BSP_LCD_TOUCH_INT 11
#define portTICK_PERIOD_MS 1
static int gpio_get_level(int pin);
static unsigned xTaskGetTickCount(void);
static bool any_indev_pressed(void);
static void enter_low_power(void);
bool ui_lock(int timeout_ms);
void ui_unlock(void);
esp_err_t bsp_display_brightness_set(int brightness);
#include "power-under-test.c"
static void set_cpu_mhz(int mhz){cpu_mhz=mhz;}
static int gpio_get_level(int pin){return touch_edge && gpio_calls++ ? 0 : 1;}
static unsigned xTaskGetTickCount(void){return now;}
static bool any_indev_pressed(void){return false;}
static void enter_low_power(void){s_low_power=true;}
static void test_delay(unsigned ticks){now+=ticks;if(++steps>=limit)longjmp(finish,1);}
bool ui_lock(int timeout_ms){
 assert(timeout_ms==50);++lock_calls;
 if(lock_calls<=lock_failures)return false;
 assert(!locked);locked=true;return true;
}
void ui_unlock(void){assert(locked);locked=false;}
esp_err_t bsp_display_brightness_set(int brightness){
 assert(locked);++writes;return driver_fails?ESP_FAIL:ESP_OK;
}
static void reset(void){
 lock_calls=lock_failures=writes=cpu_mhz=steps=gpio_calls=0;
 locked=driver_fails=touch_edge=false;limit=1;now=11000;
 s_screen_on=true;s_low_power=false;s_last_activity=0;
}
int main(void){
 reset();lock_failures=1;
 if(!setjmp(finish))power_task(NULL);
 assert(s_screen_on && !writes && !locked);
 reset();driver_fails=true;
 if(!setjmp(finish))power_task(NULL);
 assert(s_screen_on && writes==1 && !locked);
 reset();
 if(!setjmp(finish))power_task(NULL);
 assert(!s_screen_on && writes==1 && !locked);
 reset();s_screen_on=false;s_low_power=true;driver_fails=true;
 assert(!panel_on() && !s_screen_on && s_low_power && !cpu_mhz && !locked);
 /* One touch edge, two busy-lock failures, then success without another edge. */
 reset();s_screen_on=false;s_low_power=true;touch_edge=true;
 now=1000;limit=3;lock_failures=2;
 if(!setjmp(finish))power_task(NULL);
 assert(lock_calls==3 && writes==1 && s_screen_on && !s_low_power);
 assert(cpu_mhz==240 && s_last_activity==1020 && !locked);
 puts("PASS display serialization, failed blank/wake state, retained touch wake and retries");
}
'''

DISPLAY_TEST = r'''
#include "common.h"
#define ESP_ERROR_CHECK(e) assert((e)==ESP_OK)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
typedef void *esp_lcd_panel_handle_t;
typedef struct {
 esp_err_t (*custom_draw_bitmap)(lv_display_t *,esp_lcd_panel_handle_t,
                                 int,int,int,int,const void *,void *);
} esp_lv_adapter_draw_bitmap_callbacks_t;
static esp_lv_adapter_draw_bitmap_callbacks_t registered;
static uint8_t dma[48008];
static int allocations, transfers, lock_depth;
static bool no_memory, no_display, pending, fail_transfer;
lv_display_t *bsp_display_start(void){return no_display?NULL:(void *)1;}
uint32_t lv_display_get_draw_buf_size(lv_display_t *d){return 48000;}
void *heap_caps_malloc(size_t n,unsigned caps){
 assert(n==48000 && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA));
 ++allocations;return no_memory?NULL:dma;
}
esp_err_t esp_lv_adapter_lock(int ms){assert(ms==-1 && !lock_depth);++lock_depth;return ESP_OK;}
void esp_lv_adapter_unlock(void){assert(lock_depth==1);--lock_depth;}
esp_err_t esp_lv_adapter_set_draw_bitmap_callbacks(lv_display_t *d,
 const esp_lv_adapter_draw_bitmap_callbacks_t *c,void *ctx){
 assert(lock_depth==1);registered=*c;return ESP_OK;
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t p,
 int x1,int y1,int x2,int y2,const void *pixels){
 assert(!pending && pixels==dma);++transfers;
 if(fail_transfer)return ESP_FAIL;
 pending=true;return ESP_OK;
}
#include "display_port.c"
int main(void){
 no_display=true;assert(!display_port_start() && !allocations);
 no_display=false;no_memory=true;
 assert(!display_port_start() && allocations==1 && !lock_depth);
 assert(!registered.custom_draw_bitmap);
 no_memory=false;allocations=0;
 assert(display_port_start() && allocations==1 && !lock_depth);
 uint8_t pixels[48000];memset(dma,0xcc,sizeof(dma));
 for(int i=0;i<100;i++){
   const int rows=i%2?50:2;
   memset(pixels,i,sizeof(pixels));
   assert(registered.custom_draw_bitmap((void *)1,(void *)2,0,0,480,rows,pixels,NULL)==ESP_OK);
   assert(!memcmp(dma,pixels,480*rows*2));
   memset(pixels,0xff,sizeof(pixels));
   assert(dma[0]==i && dma[480*rows*2-1]==i);
   pending=false; /* Normal adapter done ISR unblocks LVGL's next flush. */
 }
 assert(allocations==1 && transfers==100 && dma[48000]==0xcc);
 assert(draw_bitmap((void *)1,(void *)2,0,0,480,52,pixels,NULL)==ESP_ERR_INVALID_SIZE);
 assert(draw_bitmap((void *)1,(void *)2,0,0,0,2,pixels,NULL)==ESP_ERR_INVALID_ARG);
 assert(transfers==100 && dma[48000]==0xcc);
 fail_transfer=true;
 assert(draw_bitmap((void *)1,(void *)2,0,0,480,2,pixels,NULL)==ESP_FAIL && !pending);
 puts("PASS reusable DMA buffer, packed pixel copy, allocation/start failures, size checks and transfer error");
}
'''
for name in ("esp_heap_caps.h", "esp_lv_adapter.h", "esp_lv_adapter_display.h"):
    (OUT / name).write_text('#include "common.h"\n', encoding="utf-8")

RECOVERY_TEST = r'''
#include "common.h"
static int64_t clock_us;
static void advance_time(unsigned ms);
#undef vTaskDelay
#define vTaskDelay(ms) advance_time(ms)
#define esp_timer_get_time() clock_us
#include "net_guard.c"
static jmp_buf finish;
static char active[33], attempted[33], last_ok[33];
static db_net_t saved;
static wifi_link_t link;
static int connects, waits, clears, changes, recover_after, connect_error;
static bool wrong_trial_ip;
static void advance_time(unsigned ms) {
 clock_us+=(int64_t)ms*1000;
 if(s_st.state==NET_NO_ACCOUNT || s_st.state==NET_NEED_SETUP)longjmp(finish,1);
 assert(clock_us<180000000); /* A stuck setup/retry loop must fail, not hang. */
}
void db_read_begin(void){}
void db_read_end(void){}
const db_net_t *db_find(const char *ssid){return &saved;}
void db_active_ssid(char *out,size_t n){strlcpy(out,active,n);}
esp_err_t db_set_active_ssid(const char *ssid){++changes;if(!ssid[0])++clears;strlcpy(active,ssid,sizeof(active));return ESP_OK;}
esp_err_t db_set_last_ok(const char *ssid){strlcpy(last_ok,ssid,sizeof(last_ok));return ESP_OK;}
void db_last_ok(char *out,size_t n){strlcpy(out,last_ok,n);}
bool db_other_ssid(const char *avoid,char *out,size_t n){out[0]=0;return false;}
bool db_current_acct(const char *ssid,char *u,size_t un,char *p,size_t pn){return false;}
void wifi_mgr_get_link(wifi_link_t *out){*out=link;}
bool wifi_mgr_connected(void){return link.connected;}
esp_err_t wifi_mgr_connect(const char *ssid,const char *pass){
 ++connects;strlcpy(attempted,ssid,sizeof(attempted));memset(&link,0,sizeof(link));return connect_error;
}
bool wifi_mgr_wait_ip(uint32_t timeout_ms){
 ++waits;
 if(connects<recover_after){clock_us+=(int64_t)timeout_ms*1000;return false;}
 link.connected=true;strlcpy(link.ssid,wrong_trial_ip?"primary":attempted,sizeof(link.ssid));strlcpy(link.ip,"192.168.5.5",sizeof(link.ip));return true;
}
void wifi_mgr_disconnect(void){memset(&link,0,sizeof(link));}
probe_result_t srun_probe(portal_info_t *pi){assert(false);return PROBE_NO_LINK;}
esp_err_t srun_online_user(const portal_info_t *pi,char *u,size_t n){return ESP_FAIL;}
bool srun_logout(const portal_info_t *pi,const char *u,char *d,size_t n){return false;}
login_result_t srun_login(const portal_info_t *pi,const char *u,const char *p,const char *domain,char *d,size_t n){return LOGIN_FAIL;}
static void reset(void){
 memset(&s_st,0,sizeof(s_st));memset(&link,0,sizeof(link));
 strlcpy(active,"primary",sizeof(active));strlcpy(last_ok,"primary",sizeof(last_ok));
 s_last_target[0]=s_try_ssid[0]=0;
 clock_us=0;connects=waits=clears=changes=connect_error=0;recover_after=3;wrong_trial_ip=false;
}
static void run(void){if(!setjmp(finish))net_task(NULL);}
int main(void){
 reset();run();
 assert(connects==3 && waits==3 && !clears && !changes && !strcmp(active,"primary"));
 assert(s_st.state==NET_NO_ACCOUNT && !strcmp(link.ssid,"primary"));
 reset();strlcpy(last_ok,"backup",sizeof(last_ok));run();
 assert(connects==3 && changes==1 && !clears && !strcmp(active,"backup") && !strcmp(link.ssid,"backup"));
 reset();active[0]=0;run();assert(s_st.state==NET_NEED_SETUP && !connects);
 reset();strlcpy(s_try_ssid,"bad-trial",sizeof(s_try_ssid));recover_after=2;run();
 assert(connects==2 && !changes && !strcmp(active,"primary") && !strcmp(link.ssid,"primary"));
 reset();strlcpy(s_try_ssid,"bad-trial",sizeof(s_try_ssid));recover_after=1;wrong_trial_ip=true;run();
 assert(connects==1 && !changes && !strcmp(active,"primary")); /* A late old-network IP cannot select the trial. */
 reset();s_st.state=NET_ONLINE;s_st.next_check_ms=90000;
 strlcpy(s_st.ip,"192.168.5.99",sizeof(s_st.ip));strlcpy(s_st.ssid,"old",sizeof(s_st.ssid));
 strlcpy(s_st.account,"old-account",sizeof(s_st.account));strlcpy(s_st.portal,"old-portal",sizeof(s_st.portal));
 net_status_t out;net_guard_get(&out);
 assert(out.state==NET_WIFI_DOWN && !out.ip[0] && !out.ssid[0] && !out.account[0] && !out.portal[0] && !out.next_check_ms);
 link.connected=true;strlcpy(link.ssid,"primary",sizeof(link.ssid));strlcpy(link.ip,"192.168.5.5",sizeof(link.ip));
 assert(ensure_connected("primary","password") && !connects); /* Stale last_target cannot force a reconnect. */
 net_guard_get(&out);assert(!strcmp(out.ip,link.ip) && !strcmp(out.ssid,link.ssid));
 recover_after=1;assert(ensure_connected("other","password") && connects==1 && !strcmp(link.ssid,"other"));
 connect_error=ESP_FAIL;assert(!ensure_connected("primary","password") && waits==1);
 s_st.next_check_ms=90000;state_set(NET_NO_ACCOUNT,"no account");
 assert(!s_st.next_check_ms && !s_st.account[0] && !s_st.portal[0]);
 puts("PASS repeated WiFi timeouts recover automatically, fallback, setup, failed trial, real SSID/IP and driver errors");
}
'''

WIFI_TEST = r'''
#include "common.h"
typedef int esp_event_base_t;
#define WIFI_EVENT 1
#define IP_EVENT 2
#define WIFI_EVENT_STA_START 1
#define WIFI_EVENT_STA_CONNECTED 2
#define WIFI_EVENT_STA_DISCONNECTED 3
#define IP_EVENT_STA_GOT_IP 1
#define IP_EVENT_STA_LOST_IP 2
#define ESP_EVENT_ANY_ID -1
#define BIT0 1
#define pdFALSE 0
#define WIFI_AUTH_OPEN 0
#define WIFI_AUTH_WPA2_PSK 3
#define WPA3_SAE_PWE_BOTH 0
#define WIFI_IF_STA 0
#define WIFI_MODE_STA 1
#define WIFI_PS_MIN_MODEM 0
#define WIFI_SCAN_TYPE_ACTIVE 0
#define ESP_ERROR_CHECK(x) assert((x)==ESP_OK)
typedef struct {uint8_t ssid[32];uint8_t ssid_len;} wifi_event_sta_connected_t;
typedef struct {int reason;} wifi_event_sta_disconnected_t;
typedef struct {uint8_t ssid[32];int8_t rssi;uint8_t authmode;} wifi_ap_record_t;
typedef struct {struct {uint8_t ssid[32],password[64];struct {int authmode;} threshold;int sae_pwe_h2e;struct {bool capable,required;} pmf_cfg;} sta;} wifi_config_t;
typedef struct {void *ssid,*bssid;int channel;bool show_hidden;int scan_type;} wifi_scan_config_t;
typedef struct {int unused;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
typedef int esp_netif_t;
typedef struct {struct {struct {unsigned bytes[4];} ip;} ip_info;} ip_event_got_ip_t;
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(p) (p)->bytes[0],(p)->bytes[1],(p)->bytes[2],(p)->bytes[3]
typedef unsigned EventBits_t;
typedef unsigned *EventGroupHandle_t;
static unsigned bits;
static int registered_ip=-2,sntp,ap_error;
static bool driver_busy;
static wifi_config_t driver_config;
static wifi_ap_record_t ap;
static EventGroupHandle_t xEventGroupCreate(void){return &bits;}
static unsigned xEventGroupClearBits(EventGroupHandle_t e,unsigned b){*e&=~b;return *e;}
static unsigned xEventGroupSetBits(EventGroupHandle_t e,unsigned b){*e|=b;return *e;}
static unsigned xEventGroupWaitBits(EventGroupHandle_t e,unsigned b,bool clear,bool all,unsigned ms){return *e;}
static esp_netif_t *esp_netif_create_default_wifi_sta(void){static int n;return &n;}
static esp_err_t esp_wifi_init(const wifi_init_config_t *ic){return ESP_OK;}
static esp_err_t esp_event_handler_instance_register(int base,int id,void (*fn)(void *,int,int32_t,void *),void *a,void *b){if(base==IP_EVENT)registered_ip=id;return ESP_OK;}
static esp_err_t esp_wifi_set_mode(int m){return ESP_OK;}
static esp_err_t esp_wifi_start(void){return ESP_OK;}
static esp_err_t esp_wifi_set_ps(int ps){return ESP_OK;}
static esp_err_t esp_wifi_set_config(int iface,const wifi_config_t *c){if(driver_busy)return ESP_ERR_INVALID_STATE;driver_config=*c;return ESP_OK;}
static esp_err_t esp_wifi_get_config(int iface,wifi_config_t *c){*c=driver_config;return ESP_OK;}
static esp_err_t esp_wifi_disconnect(void){driver_busy=false;return ESP_OK;}
static esp_err_t esp_wifi_connect(void){driver_busy=true;return ESP_OK;}
static esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *out){*out=ap;return ap_error;}
static esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *cfg,bool block){return ESP_OK;}
static esp_err_t esp_wifi_scan_get_ap_num(uint16_t *n){*n=0;return ESP_OK;}
static esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n,wifi_ap_record_t *r){return ESP_OK;}
void sys_time_sntp_start(void){++sntp;}
#include "wifi_mgr.c"
static void got_ip(void){
 ip_event_got_ip_t e={.ip_info.ip.bytes={192,168,5,5}};
 event_handler(NULL,IP_EVENT,IP_EVENT_STA_GOT_IP,&e);
}
int main(void){
 assert(wifi_mgr_init()==ESP_OK && registered_ip==ESP_EVENT_ANY_ID);
 wifi_event_sta_connected_t e={.ssid="primary",.ssid_len=7};
 event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,&e);
 wifi_link_t link;wifi_mgr_get_link(&link);assert(!link.connected && !link.ssid[0] && !link.ip[0]);
 got_ip();wifi_mgr_get_link(&link);assert(link.connected && !strcmp(link.ssid,"primary") && !strcmp(link.ip,"192.168.5.5") && wifi_mgr_wait_ip(1));
 strlcpy((char *)ap.ssid,"primary",32);strlcpy((char *)driver_config.sta.ssid,"primary",32);strlcpy((char *)driver_config.sta.password,"password",64);
 char ssid[33],pass[65];assert(wifi_mgr_current_credentials(ssid,sizeof(ssid),pass,sizeof(pass)) && !strcmp(pass,"password"));
 event_handler(NULL,IP_EVENT,IP_EVENT_STA_LOST_IP,NULL);
 wifi_mgr_get_link(&link);assert(!link.connected && !link.ssid[0] && !link.ip[0] && !bits && !wifi_mgr_wait_ip(1));
 assert(!wifi_mgr_current_credentials(ssid,sizeof(ssid),pass,sizeof(pass)));
 got_ip();wifi_mgr_get_link(&link);assert(link.connected && !strcmp(link.ssid,"primary"));
 wifi_event_sta_disconnected_t d={.reason=200};
 event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&d);
 wifi_mgr_get_link(&link);assert(!link.connected && !link.ssid[0] && !link.ip[0] && !s_ssid[0] && !bits);
 wifi_mgr_ssid(ssid,sizeof(ssid));assert(!ssid[0]);wifi_mgr_ip(ssid,sizeof(ssid));assert(!ssid[0]);
 event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,&e);got_ip();wifi_mgr_disconnect();
 wifi_mgr_get_link(&link);assert(!link.connected && !link.ip[0] && !link.ssid[0] && !bits);
 event_handler(NULL,WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,&e);got_ip();
 driver_busy=true; /* A timed-out driver attempt must be stopped before set_config. */
 assert(wifi_mgr_connect("other","password")==ESP_OK);
 wifi_mgr_get_link(&link);assert(!link.connected && !link.ip[0] && !link.ssid[0] && !bits);
 puts("PASS actual WiFi events: association, IP acquisition/loss/recovery, disconnect, explicit disconnect and network switch");
}
'''

# Exercise the exact LVGL refresh callback without linking the rest of LVGL.
setup = (ROOT / "main/ui_setup.c").read_text(encoding="utf-8")
(OUT / "wifi_ui_callback.c").write_text(
    setup[setup.index("static void refresh_wifi_status("):setup.index("static void row_ap_cb(")], encoding="utf-8")
WIFI_UI_TEST = r'''
#include "common.h"
#include "wifi_mgr.h"
typedef struct {bool hidden;} lv_obj_t;
typedef int lv_timer_t;
static lv_obj_t screen,other,tags[2];
static lv_obj_t *s_scr_wifi=&screen,*visible=&screen;
static lv_obj_t *s_row_connected[]={&tags[0],&tags[1]};
static char s_row_ssid[][33]={"primary","backup"};
static int s_wifi_rows=2;
static wifi_link_t link;
static lv_obj_t *lv_screen_active(void){return visible;}
static void ui_obj_hidden(lv_obj_t *o,bool hidden){o->hidden=hidden;}
void wifi_mgr_get_link(wifi_link_t *out){*out=link;}
#include "wifi_ui_callback.c"
int main(void){
 link.connected=true;strlcpy(link.ssid,"primary",sizeof(link.ssid));refresh_wifi_status(NULL);
 assert(!tags[0].hidden && tags[1].hidden);
 link.connected=false;refresh_wifi_status(NULL);assert(tags[0].hidden && tags[1].hidden);
 link.connected=true;strlcpy(link.ssid,"backup",sizeof(link.ssid));refresh_wifi_status(NULL);
 assert(tags[0].hidden && !tags[1].hidden);
 visible=&other;link.connected=false;refresh_wifi_status(NULL);assert(!tags[1].hidden);
 visible=&screen;s_wifi_rows=0;refresh_wifi_status(NULL);assert(!tags[1].hidden);
 s_wifi_rows=2;refresh_wifi_status(NULL);assert(tags[0].hidden && tags[1].hidden);
 puts("PASS WiFi page labels follow disconnect/reconnect/switch, with hidden page and cleared-list guards");
}
'''

TRANSPORT_TEST = r'''
#include "common.h"
typedef struct esp_http_client_event {
 int event_id;void *user_data;char *header_key,*header_value;
} esp_http_client_event_t;
#define HTTP_EVENT_ON_HEADER 1
typedef struct {
 const char *url;int timeout_ms;bool disable_auto_redirect;
 int buffer_size,buffer_size_tx;
 esp_err_t (*event_handler)(esp_http_client_event_t *);void *user_data;
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static esp_http_client_config_t config;
static bool init_fail,open_fail,headers_fail;
static int cleanup,closed,status=302;
static char *location="http://portal.test/srun_portal_pc?ac_id=3";
static esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){config=*c;return init_fail?NULL:(void *)1;}
static int esp_http_client_set_header(void *c,const char *k,const char *v){return ESP_OK;}
static int esp_http_client_open(void *c,int len){return open_fail?ESP_FAIL:ESP_OK;}
static int64_t esp_http_client_fetch_headers(void *c){
 if(headers_fail)return -1;
 if(location){
  esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.user_data=config.user_data,
   .header_key="lOcAtIoN",.header_value=location};
  assert(config.event_handler && config.event_handler(&e)==ESP_OK);
 }
 return 0;
}
static int esp_http_client_get_status_code(void *c){return status;}
static int esp_http_client_read(void *c,char *out,int n){return 0;}
static int esp_http_client_close(void *c){++closed;return ESP_OK;}
static int esp_http_client_cleanup(void *c){++cleanup;return ESP_OK;}
#include "http_util.c"
int main(void){
 http_resp_t response;
 assert(http_get_capture("http://probe.test",8000,&response)==ESP_OK);
 assert(config.disable_auto_redirect && response.status==302 && !strcmp(response.location,location));
 assert(cleanup==1 && closed==1);
 location=NULL;status=204;
 assert(http_get_capture("http://probe.test",8000,&response)==ESP_OK && response.status==204 && !response.location[0]);
 char too_long[256];memset(too_long,'a',sizeof(too_long)-1);too_long[255]=0;location=too_long;
 assert(http_get_capture("http://probe.test",8000,&response)==ESP_OK && !response.location[0]);
 headers_fail=true;assert(http_get_capture("http://probe.test",8000,&response)==ESP_FAIL && cleanup==4 && closed==3);
 headers_fail=false;open_fail=true;assert(http_get_capture("http://probe.test",8000,&response)==ESP_FAIL && cleanup==5);
 open_fail=false;init_fail=true;assert(http_get_capture("http://probe.test",8000,&response)==ESP_FAIL && cleanup==5);
 assert(http_get_capture("http://probe.test",8000,NULL)==ESP_ERR_INVALID_ARG);
 puts("PASS real HTTP transport: response Location callback, case-insensitive headers, reset, truncation and failure cleanup");
}
'''

gcc = shutil.which("gcc")
if not gcc:
    raise SystemExit("gcc is required for host C regression tests")
for name, code, helpers in (("http", HTTP_TEST, True), ("battery", BATTERY_TEST, False),
                            ("srun", SRUN_TEST, True), ("guard", GUARD_TEST, False),
                            ("power", POWER_TEST, False), ("display", DISPLAY_TEST, False),
                            ("recovery", RECOVERY_TEST, False), ("wifi", WIFI_TEST, False),
                            ("wifi_ui", WIFI_UI_TEST, False), ("transport", TRANSPORT_TEST, False)):
    src = OUT / f"{name}_test.c"
    src.write_text(code, encoding="utf-8")
    exe = OUT / (name + (".exe" if __import__("os").name == "nt" else ""))
    command = [gcc, "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
               "-Wl,--gc-sections", "-I", str(OUT), "-I", str(ROOT / "main"), str(src)]
    if helpers:
        command.append(str(OUT / "http_helpers.c"))
    subprocess.run(command + ["-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
