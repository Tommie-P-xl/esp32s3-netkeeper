/*
 * 编译期配置：WiFi / 账号宏只在首次开机时播种。
 * 已有 WiFi / 账号由 Flash 与 SD 配置库管理；Portal 兜底地址、轮询和电源参数
 * 仍是编译期设置，修改后需要重新编译烧录。
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

/* 当前学校的专用兜底配置；其他学校使用前必须修改，见 README。
 * 只填协议、主机及可选端口，不含成功页路径和查询参数。 */
#define APP_PORTAL_FALLBACK_HOST  "http://10.253.0.235"
#define APP_PORTAL_FALLBACK_ACID  "3"

/* -------------------------------------------------------- 实时守护与退避 */
/* 分层实时监测：外网轻量哨兵（无 DNS）+ 静默防抖 + 完整 204 兜底。
 * 插 USB 供电或电池亮屏时保持较高响应；电池熄屏时自动拉长间隔省电。 */
#define APP_SENTINEL_IP_PRIMARY        "223.5.5.5"     /* 阿里多线 Anycast */
#define APP_SENTINEL_IP_SECONDARY      "119.29.29.29"  /* 腾讯 DNSPod 多线 Anycast */
#define APP_SENTINEL_TCP_PORT          443             /* 降级 TCP 轻握手端口（BRAS 不劫持） */
#define APP_SENTINEL_TIMEOUT_MS        1500            /* 单次轻量探测超时 */
#define APP_SENTINEL_ACTIVE_MS         (15 * 1000)     /* 插电 / 电池亮屏哨兵间隔 */
#define APP_SENTINEL_SLEEP_MS          (45 * 1000)     /* 电池熄屏省电哨兵间隔 */
#define APP_SENTINEL_WAKE_COOLDOWN_MS  (5 * 1000)      /* 触摸亮屏即时探测最小冷却 */
#define APP_SENTINEL_CONFIRM_DELAY1_MS 1000            /* 主目标失手后切备用目标等待 */
#define APP_SENTINEL_CONFIRM_DELAY2_MS 2000            /* 双目标失手后的第三次确认等待 */
#define APP_FULL_CHECK_INTERVAL_MS     (15 * 60 * 1000)/* 完整 HTTP 204 兜底校检周期 */
#define APP_POLL_INTERVAL_MS           APP_FULL_CHECK_INTERVAL_MS

/* 非认证断联监听与业务拒绝退避：
 *   - 硬件/上联断开（Portal 不可达）：不发认证，轻探端口等待恢复（15 -> 30 -> 60s）
 *   - 校内在线但外网异常：不注销不重登，低频探测外网恢复（30s / 电池熄屏 60s）
 *   - 服务端明确拒绝认证（欠费/密码错/超限）：强指数退避（1 -> 3 -> 5 -> 10 min） */
#define APP_LINK_WAIT_BACKOFF_MS       { 15 * 1000, 30 * 1000, 60 * 1000 }
#define APP_WAN_WAIT_ACTIVE_MS         (30 * 1000)
#define APP_WAN_WAIT_SLEEP_MS          (60 * 1000)
#define APP_RETRY_BACKOFF_MS           { 60 * 1000, 180 * 1000, 300 * 1000, 600 * 1000 }

/* ------------------------------------------------------------ WiFi 超时 */
#define APP_WIFI_CONNECT_TIMEOUT_MS  20000
#define APP_WIFI_RETRY_MS            5000

/* ------------------------------------------------------------- 外网探测 */
/* 国内双多线 204 端点，避免海外 gstatic 不通时退化为单点探测。 */
#define APP_PROBE_URL_PRIMARY     "http://connect.rom.miui.com/generate_204"
#define APP_PROBE_URL_SECONDARY   "http://connectivitycheck.platform.hicloud.com/generate_204"

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

/* 单节 3.7 V / 1000 mAh、充电限制 4.2 V 的电池。 */
#define APP_BAT_CHARGE_CURRENT_MA 300
#define APP_BAT_TERM_CURRENT_MA   50
/* 软件关机需连续三次低压；关机锁定跨重启保留，恢复电压留出回差。 */
#define APP_BAT_OFF_MV            3300
#define APP_BAT_RESUME_MV         3500
#define APP_BAT_LOW_SAMPLES       3
/* PMU 的 VSYS 欠压兜底，区别于上面的电池端软件阈值。 */
#define APP_BAT_HARD_OFF_MV       3000

/* --------------------------------------------------------------- 调试 */
/* 启动时把 SRun 加密算法自检打一遍（bring-up 用，已验证过，默认关） */
#define APP_CRYPTO_SELFTEST       0
/* 启动 3 秒后统计一次界面各区域像素，确认 UI 真的画出来了（默认关） */
#define APP_UI_SNAPSHOT_PROBE     0
/* 空闲检测调试：每 2 秒打一行 int_low / press 计数 + 当前 idle 时间（默认关） */
#define APP_IDLE_DEBUG            0
