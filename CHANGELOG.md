# Changelog

本项目的版本变更记录。格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

实现细节与踩过的坑在 [README](README.md#实现细节教学向) / [踩坑记录](README.md#踩坑记录) 里，这里只记"哪个版本变了什么"。

## [Unreleased]

暂无。

## 首个可用版本 - 2026-11-02

首个可用版本：一块 ESP32-S3 触摸屏板 = 校园网（SRun）自动认证器。

### 新增

- **自动认证**：探测 `generate_204` → 被 302 劫持就从 `Location` 抓 Portal 与 `ac_id`
  → 按 SRun 三字段（`hmd5` / `info` / `chksum`）登录 → 登录成功后再探一次外网确认。（`srun_port.c`、`srun_crypto.c`）
- **掉线自愈**：独立守护任务 5 分钟轮询，失败按 30 / 60 / 120 s 退避；连不上的网络自动回退
  `last_ok` 或库里其他网络，避免"填错一个 SSID 就失联"。（`net_guard.c`）
- **480×480 触摸界面**：主界面状态卡、选 WiFi、输密码、账号管理、添加账号；
  顶部时钟；自生成中文子集字体（约 730 KB）。（`ui.c`、`ui_setup.c`、`ui_font_18.c`）
- **局域网配置面板**：`index.html` 编译期嵌进固件，6 位验证码 + token 鉴权，
  手机/电脑浏览器即可改配置、立即检测、重载 SD。（`web.c`）
- **配置双份存储**：NVS + SD 双写、SD 优先，人可读 INI 可直接用读卡器改；没插卡也能用。（`db.c`）
- **电池状态**：读 AXP2101 的 VBAT 与充放电状态位，显示电量百分比；放电时给续航估计
  （按亮屏/熄屏/降频三档电流估算），充电时只报电量。三档充电状态：`充电中` / `已充满` / `已接电源`。（`batt_mgr.c`）
- **省电**：10 s 无触摸熄屏、再 60 s 降到 80 MHz，触摸即回；充电时同样生效。（`power_mgr.c`）
- **CI**：push/PR 自动编译并上传 artifact，推版本 tag 自动发布固件。（`.github/workflows/ci.yml`）

### 修复

- 空闲判定从"触摸 INT 电平"改为"边沿唤醒 + 真实按下"，修掉**插着充电器时不熄屏、熄灭后又自己亮**。
- 抓图流程改为异步：LVGL 线程只做快照，串口发送交给独立任务，不再冻住界面和计时器。
- 快照前 `lv_refr_now` + 30 ms，修掉 `assert failed: spi_device_release_bus` 导致的偶发整机复位。
- 烧录后追加 `watchdog-reset`，修掉"烧完停在 ROM 下载模式、屏幕全黑"。
- 消除 LVGL 9.6 的 `lv_obj_add_flag/remove_flag` 弃用告警、未使用变量告警、
  legacy I2C 头的 EOL 提示。
- 电池 98% 已停充时不再误显示"充电中"。

### 已知限制

- 只实现 SRun 协议；换学校需要替换 `srun_port.c` 的请求参数与 `srun_crypto.c` 的加密字段。
- 续航是按功耗模型估算（AXP2101 没有电流 ADC），`APP_BATTERY_MAH` 与实际电池不符时会有偏差。
- 验证码鉴权只防同局域网误改，不是对抗性安全机制。
- WiFi 密码与认证密码在 SD 卡上是明文。
