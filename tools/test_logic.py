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
for name in ("esp_err.h", "esp_log.h", "esp_timer.h", "nvs.h", "esp_wifi.h",
             "lvgl.h", "esp_sleep.h", "driver/i2c_master.h", "bsp/esp-bsp.h",
             "freertos/FreeRTOS.h", "freertos/task.h", "freertos/semphr.h"):
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
#include "srun_port.c"
static int primary_status=503,secondary_status=204,calls;
static char last_json[1024], last_login[5120];
esp_err_t nvs_open(const char *s,int m,nvs_handle_t *h){return ESP_ERR_NOT_FOUND;}
esp_err_t nvs_get_str(nvs_handle_t h,const char*k,char*v,size_t*n){return ESP_ERR_NOT_FOUND;}
esp_err_t nvs_set_str(nvs_handle_t h,const char*k,const char*v){return 0;}
esp_err_t nvs_commit(nvs_handle_t h){return 0;}
void nvs_close(nvs_handle_t h){}
void wifi_mgr_ssid(char *s,size_t n){strlcpy(s,"test",n);}
void srun_hmac_md5_hex(const char*k,const char*m,char*out){memset(out,'a',32);out[32]=0;}
void srun_sha1_hex(const char*m,size_t n,char*out){memset(out,'b',40);out[40]=0;}
void srun_encode_info(const char*j,const char*t,char*out,size_t n){strlcpy(last_json,j,sizeof(last_json));strlcpy(out,"{SRBX1}a+/=",n);}
esp_err_t http_get_capture(const char*u,int timeout,http_resp_t*r){
 memset(r,0,sizeof(*r));r->status=200;calls++;
 if(!strcmp(u,APP_PROBE_URL_PRIMARY))r->status=primary_status;
 else if(!strcmp(u,APP_PROBE_URL_SECONDARY)){r->status=secondary_status;strlcpy(r->location,"http://portal.test/login?ac_id=9",sizeof(r->location));}
 else if(strstr(u,"get_challenge"))strlcpy(r->body,"jsonp({\"challenge\":\"token\",\"client_ip\":\"10.1.2.3\"})",sizeof(r->body));
 else if(strstr(u,"action=login")){strlcpy(last_login,u,sizeof(last_login));strlcpy(r->body,"jsonp({\"error\":\"ok\",\"suc_msg\":\"login_ok\"})",sizeof(r->body));}
 else if(strstr(u,"action=logout"))strlcpy(r->body,"jsonp({\"error\":\"ok\",\"suc_msg\":\"logout_ok\"})",sizeof(r->body));
 else if(strstr(u,"rad_user_info"))strlcpy(r->body,"jsonp({\"user_name\":\"old-account\"})",sizeof(r->body));
 else r->status=503;
 return ESP_OK;
}
int main(void){
 portal_info_t pi;char detail[96],user[64];
 int errors[]={403,500,503};
 for(size_t i=0;i<3;i++){primary_status=errors[i];calls=0;assert(srun_probe(&pi)==PROBE_ONLINE && calls==2);}
 primary_status=503;secondary_status=302;calls=0;
 assert(srun_probe(&pi)==PROBE_NEED_AUTH && !strcmp(pi.portal_base,"http://portal.test") && !strcmp(pi.ac_id,"9"));
 primary_status=204;calls=0;assert(srun_probe(&pi)==PROBE_ONLINE && calls==1);
 strlcpy(pi.portal_base,"http://portal.test",sizeof(pi.portal_base));strlcpy(pi.ac_id,"9",sizeof(pi.ac_id));
 assert(srun_login(&pi,"u+&","a\"b\\c","@x",detail,sizeof(detail))==LOGIN_OK);
 assert(strstr(last_json,"a\\\"b\\\\c") && strstr(last_login,"username=u%2B%26%40x"));
 char longpass[64];memset(longpass,'\\',63);longpass[63]=0;
 assert(srun_login(&pi,"normal",longpass,"",detail,sizeof(detail))==LOGIN_OK);
 assert(srun_online_user(&pi,user,sizeof(user))==ESP_OK && !strcmp(user,"old-account"));
 assert(srun_logout(&pi,user,detail,sizeof(detail)));
 puts("PASS HTTP fallback, secondary Portal discovery, escaped credentials, query and logout");
}
'''

GUARD_TEST = r'''
#include "common.h"
#include "net_guard.c"
static int probe_count, logout_count,login_count,query_error;
static bool logout_ok=true;
static char online_user[64]="old";
probe_result_t srun_probe(portal_info_t*pi){memset(pi,0,sizeof(*pi));strlcpy(pi->portal_base,"http://portal.test",64);probe_count++;return PROBE_ONLINE;}
esp_err_t srun_online_user(const portal_info_t*pi,char*u,size_t n){strlcpy(u,online_user,n);return query_error;}
bool srun_logout(const portal_info_t*pi,const char*u,char*d,size_t n){logout_count++;return logout_ok;}
login_result_t srun_login(const portal_info_t*pi,const char*u,const char*p,const char*domain,char*d,size_t n){
 login_count++;strlcpy(online_user,u,sizeof(online_user));strlcpy(d,"ok",n);return LOGIN_OK;
}
int main(void){
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

gcc = shutil.which("gcc")
if not gcc:
    raise SystemExit("gcc is required for host C regression tests")
for name, code, helpers in (("http", HTTP_TEST, True), ("battery", BATTERY_TEST, False),
                            ("srun", SRUN_TEST, True), ("guard", GUARD_TEST, False)):
    src = OUT / f"{name}_test.c"
    src.write_text(code, encoding="utf-8")
    exe = OUT / (name + (".exe" if __import__("os").name == "nt" else ""))
    command = [gcc, "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
               "-Wl,--gc-sections", "-I", str(OUT), "-I", str(ROOT / "main"), str(src)]
    if helpers:
        command.append(str(OUT / "http_helpers.c"))
    subprocess.run(command + ["-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
