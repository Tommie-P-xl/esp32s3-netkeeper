/*
 * 编译期默认值 —— 只在 SD 卡上还没有配置文件时用一次（首次开机播种）。
 * 之后所有配置都从 /sdcard/portal/net.cfg 读，改配置不需要重新烧录。
 *
 * 目标板：Waveshare ESP32-S3-Touch-AMOLED-2.16
 *   ESP32-S3 N16R8 + CO5300 480x480 AMOLED + CST9217 触摸 + microSD（1-bit SDMMC）
 *   ESP-IDF v6.1
 */
#pragma once

/* ------------------------------------------------ 首次开机的播种配置 */
/* 以下均为示例占位，请换成自己的真实参数后编译。
 * 运行期配置以 SD 卡 /sdcard/portal/net.cfg 为准（SD 优先），
 * 这些宏只在两块存储都为空时播种一次。 */
#define APP_WIFI_SSID          "your-wifi-ssid"
#define APP_WIFI_PASSWORD      "your-wifi-password"

#define APP_PORTAL_USER        "your-student-id"
#define APP_PORTAL_PASSWORD    "your-portal-password"
/* 学号后面要不要带域后缀。多数校园网登录页默认为空，失败可试 "@cmcc" */
#define APP_PORTAL_DOMAIN      ""

/* 自动探测失败（抓不到 302 跳转）时的兜底 Portal */
#define APP_PORTAL_FALLBACK_HOST  "http://0.0.0.0"
#define APP_PORTAL_FALLBACK_ACID  "1"

/* ---------------------------------------------------------------- 轮询 */
#define APP_POLL_INTERVAL_MS   (5 * 60 * 1000)
#define APP_RETRY_BACKOFF_MS   { 30 * 1000, 60 * 1000, 120 * 1000 }

/* ------------------------------------------------------------ WiFi 超时 */
#define APP_WIFI_CONNECT_TIMEOUT_MS  20000
#define APP_WIFI_RETRY_MS            5000

/* ------------------------------------------------------------- 外网探测 */
#define APP_PROBE_URL_PRIMARY     "http://connect.rom.miui.com/generate_204"
#define APP_PROBE_URL_SECONDARY   "http://www.gstatic.com/generate_204"

#define APP_HTTP_TIMEOUT_MS       8000

/* ---------------------------------------------------------------- 网页 */
#define APP_WEB_PORT              80

/* ---------------------------------------------------------------- 电源 */
/* 屏幕亮度上限（%）。电池供电嫌费电就调低，AMOLED 亮度对功耗影响很直接。 */
#define APP_BRIGHTNESS            100
/* 亮屏多久算「没人碰」-> 熄屏（ms） */
#define APP_SCREEN_OFF_MS         10000
/* 熄屏后再多久进「休眠」（降频）（ms） */
#define APP_IDLE_AFTER_MS         (60 * 1000)

/* ---------------------------------------------------------------- 电池 */
/* 你实际装的电池容量（mAh）。续航估计只有这一个参数要按实物改。 */
#define APP_BATTERY_MAH           1000
/*
 * AXP2101 没有电池电流 ADC（只有电压和芯片温度），所以续航只能是估算：
 * 按"当前工作状态"取一个典型放电电流，再用 剩余容量 / 电流 折算。
 * 下面三个数是这块板实测量级上的经验值，换板/换屏要重估。
 */
#define APP_BAT_CUR_SCREEN_ON_MA  120   /* 亮屏 + 240 MHz + WiFi 连着 */
#define APP_BAT_CUR_IDLE_MA       45    /* 熄屏，CPU 仍 240 MHz */
#define APP_BAT_CUR_IDLE_LOW_MA   28    /* 熄屏 + 降频 80 MHz */
/* 充电时 VBAT 被抬高（线损 + 极化），查表前先减掉，否则电量会虚高 */
#define APP_BAT_CHG_OFFSET_MV     40

/* --------------------------------------------------------------- 调试 */
/* 启动时把 SRun 加密算法自检打一遍（bring-up 用，已验证过，默认关） */
#define APP_CRYPTO_SELFTEST       0
/* 启动 3 秒后统计一次界面各区域像素，确认 UI 真的画出来了（默认关） */
#define APP_UI_SNAPSHOT_PROBE     0
/* 启动 3 秒后把整屏显存以十六进制打到串口，用于在电脑上还原界面截图（默认关） */
#define APP_UI_DUMP_FB            0
/* 配合上面的抓图：自动切到各界面各截一张（默认关） */
#define APP_UI_CAPTURE_SETUP      0
/* 显存 dump 的降采样倍数：2 = 半分辨率(240x240，快)；1 = 原分辨率 480x480(清晰，慢) */
#define APP_UI_DUMP_STEP          1
/* 空闲检测调试：每 2 秒打一行 int_low / press 计数 + 当前 idle 时间（默认关） */
#define APP_IDLE_DEBUG            0
