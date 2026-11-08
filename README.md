<div align="center">

# esp32s3-netkeeper

**把一块 ESP32-S3 触摸屏小板，变成会自己登录校园网的网守护**

检测外网 → 掉线自动重认证 → 5 分钟轮询 → 屏幕上看得见状态

当前版本见 [VERSION](VERSION) · 支持 SD / Flash 实时同步、SD 热插拔与活跃网络保护，
完整更新记录见 [CHANGELOG.md](CHANGELOG.md)。

最新修正：网页未解锁时仅显示验证码卡片，验证成功后自动切换到完整配置面板，锁定后恢复验证码入口。
浏览器缓存的会话先向板子校验，通过前不展示状态和账号信息。
WiFi 页先展示已有列表并后台扫描，结果未变化时保留列表；刷新按钮与旋转指示同步。
同时修复认证跳转地址读取和 HTTP 200 误判，确认 SRun 会话状态后再发起认证；
正常检测保留已有手动认证会话。WiFi 掉线后仍保留目标重连，页面状态跟随真实链路更新。
网页轮询保留编辑中的输入框与草稿，避免输入时失焦、手机键盘收起。

此前的充电状态识别、低电关机与开机检查、中文配置和认证特殊字符处理，
以及同一 WiFi 下的认证账号切换功能继续保留。网页仅移除底部的 SD 配置说明卡片，
配置保存、双存储同步及读卡器编辑功能保持不变。项目版本号只在 `VERSION` 文件中维护。

[![License](https://img.shields.io/badge/License-MIT-3DA639.svg?style=flat-square)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v6.1-E7352C.svg?style=flat-square&logo=espressif&logoColor=white)](https://github.com/espressif/esp-idf)
[![LVGL](https://img.shields.io/badge/LVGL-9.6-2C3E50.svg?style=flat-square)](https://lvgl.io)
[![Platform](https://img.shields.io/badge/ESP32--S3-N16R8-8E44AD.svg?style=flat-square)](https://www.espressif.com/)
[![Board](https://img.shields.io/badge/Board-AMOLED%202.16-E67E22.svg?style=flat-square)](https://www.waveshare.com/)
[![Protocol](https://img.shields.io/badge/Portal-SRun%20%2F%20%E6%B7%B1%E6%BE%9C-1ABC9C.svg?style=flat-square)](#实现细节教学向)

[![build](https://github.com/Tommie-P-xl/esp32s3-netkeeper/actions/workflows/ci.yml/badge.svg)](https://github.com/Tommie-P-xl/esp32s3-netkeeper/actions/workflows/ci.yml)
[![release](https://img.shields.io/github/v/release/Tommie-P-xl/esp32s3-netkeeper?style=flat-square&label=release)](https://github.com/Tommie-P-xl/esp32s3-netkeeper/releases)
[![changelog](https://img.shields.io/badge/changelog-CHANGELOG.md-6C7A89.svg?style=flat-square)](CHANGELOG.md)

</div>

> [!IMPORTANT]
> **仓库中的 Portal 默认地址和 `ac_id` 是当前学校的专用配置，并非通用参数。**
> 其他学校使用前必须按下文「使用前配置校园认证地址」修改 `main/app_config.h`，重新编译烧录；
> 同时核对 WiFi、账号、密码和学校的认证协议。非 SRun 校园网需要移植认证流程。

面向 **SRun Portal**（国内高校最常见的深澜认证系统）。移植到别的 Portal 系统
（Dr.COM、华为等）需要替换 `main/srun_port.c` 的登录流程，
细节见[第 14 节](#14-想换成别的-portal-系统drcom--华为--锐捷)。

> [!NOTE]
> 一块板子 = 一个不用管的网守护。第一次配网在屏幕上点两下，之后断电、换网、掉线都自己恢复；
> 配置同时存在 Flash 和 SD 卡，丢了哪一份都能救回来。

## 目录

- [界面预览](#界面预览)
- [本地生成截图](#本地生成截图)
- [支持硬件](#支持硬件)
- [功能](#功能)
- [编译与烧录](#编译与烧录)
  - [使用前配置校园认证地址](#使用前配置校园认证地址)
  - [本地编译与烧录](#本地编译与烧录)
- [自动化构建与发布](#自动化构建与发布)
- [配置存在哪、谁优先](#配置存在哪谁优先)
  - [文件格式（`/portal/net.cfg`，UTF-8）](#文件格式portalnetcfgutf-8)
- [屏幕操作](#屏幕操作)
- [网页配置](#网页配置)
- [目录结构](#目录结构)
- [电池与功耗](#电池与功耗)
  - [电量显示](#电量显示)
  - [充电和低电保护](#充电和低电保护)
  - [省电策略](#省电策略)
- [认证与输入](#认证与输入)
  - [同一 WiFi 切换认证账号](#同一-wifi-切换认证账号)
  - [输入处理与探测](#输入处理与探测)
- [排错](#排错)
  - [WiFi 掉线后的恢复与旧固件升级](#wifi-掉线后的恢复与旧固件升级)
  - [常见现象](#常见现象)
- [已知限制](#已知限制)
- [实现细节（教学向）](#实现细节教学向)
  - [1. 整体形状](#1-整体形状)
  - [2. 任务与线程模型](#2-任务与线程模型)
  - [3. 启动顺序（`app_main.c`）](#3-启动顺序app_mainc)
  - [4. 网络守护主循环（`net_guard.c`）](#4-网络守护主循环net_guardc)
  - [5. 外网探测：怎么判断有没有网、Portal 在哪（`srun_port.c`）](#5-外网探测怎么判断有没有网portal-在哪srun_portc)
  - [6. SRun 认证算法：三个字段是怎么算出来的（`srun_crypto.c`）](#6-srun-认证算法三个字段是怎么算出来的srun_cryptoc)
  - [7. 登录请求与响应判定（`srun_port.c`）](#7-登录请求与响应判定srun_portc)
  - [8. 配置库：一份数据，两处存储（`db.c`）](#8-配置库一份数据两处存储dbc)
  - [9. 局域网配置面板（`web.c` + `index.html`）](#9-局域网配置面板webc--indexhtml)
  - [10. UI：一条线程、一把锁、一套排版规则（`ui.c` / `ui_common.c` / `ui_setup.c`）](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc)
  - [11. 电源管理（`power_mgr.c`）](#11-电源管理power_mgrc)
  - [12. 电池状态（`batt_mgr.c` + AXP2101）](#12-电池状态batt_mgrc--axp2101)
  - [13. 构建期配置（`sdkconfig.defaults` 里几个非默认项）](#13-构建期配置sdkconfigdefaults-里几个非默认项)
  - [14. 想换成别的 Portal 系统（Dr.COM / 华为 / 锐捷）](#14-想换成别的-portal-系统drcom--华为--锐捷)
- [技术路线与关键取舍](#技术路线与关键取舍)
- [踩坑记录](#踩坑记录)
  - [显示与 LVGL](#显示与-lvgl)
  - [触摸与空闲休眠](#触摸与空闲休眠)
  - [网页与认证](#网页与认证)
  - [电池与 PMIC](#电池与-pmic)
  - [构建、烧录与抓图工具](#构建烧录与抓图工具)
- [许可](#许可)

## 界面预览

| 主页 | 选 WiFi |
|---|---|
| ![主页](docs/images/01_main.png) | ![选 WiFi](docs/images/02_wifi.png) |

| 输入 WiFi 密码 | 账号管理 |
|---|---|
| ![输密码](docs/images/03_pass.png) | ![账号管理](docs/images/04_acct.png) |

| 添加账号 | 网页配置面板 |
|---|---|
| ![添加账号](docs/images/05_add.png) | ![网页配置](docs/images/06_web.png) |

| WiFi 后台扫描中 | 网页验证码入口 |
|---|---|
| ![扫描中](docs/images/02_wifi_scanning.png) | ![网页待解锁](docs/images/07_web_locked.png) |

| 删除 WiFi 前确认 | 删除账号前确认 |
|---|---|
| ![删除 WiFi 确认框](docs/images/08_delete_wifi.png) | ![删除账号确认框](docs/images/09_delete_account.png) |

> [!NOTE]
> 设备页面由电脑上的 LVGL 直接执行当前界面源码，按 480×480 原分辨率离屏渲染，
> 网页截图来自当前 `main/index.html`。WiFi、账号、密码、验证码、电池和网络状态均为模拟数据，
> 没有读取开发板的真实配置，也无需在真实截图上遮挡隐私；截图不代表一次真实网络认证或电池测量。
> 主页演示的是电池在位、USB 充电状态；实际无电池时该行隐藏。

---

## 本地生成截图

电脑可以直接运行现有 UI 代码生成图片，不需要从开发板传输显存：

```powershell
python -m pip install -r tools/screenshots/requirements.txt
python tools/screenshots.py
```

设备端复用实际 LVGL 界面源码和中文字体，以 480×480 离屏渲染；网页端打开实际 HTML，
由本地演示 API 提供数据。输出直接更新 `docs/images/` 中的十张图片，包括扫描中、待解锁和删除确认框。
所有数据均为模拟值，不读取或更改板子的 WiFi、账号、验证码或电池信息。

需要 Python / Pillow、原生 GCC / G++、CMake、Ninja；网页截图还需要 Node.js 和 Chrome。
LVGL 默认复用 `managed_components/lvgl__lvgl`，首次可用 `idf.py reconfigure` 获取依赖。
详细依赖、单独生成设备图或网页图的命令见 [截图工具说明](tools/screenshots/README.md)。
首次需要编译主机版 LVGL，后续自动增量编译；本地截图仍不能替代真实触摸、显示传输和网络测试。

---

## 支持硬件

| 项 | 说明 |
|---|---|
| 板子 | [Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/)（ESP32-S3 N16R8） |
| 屏幕 | CO5300 480×480 AMOLED（QSPI）+ CST9217 触摸 |
| 存储 | 片上 NVS（必需）+ microSD（可选，1-bit SDMMC） |
| 电源 | AXP2101（I2C 0x34）：电池电压 / USB 在位 / 充放电状态 |
| 框架 | ESP-IDF **v6.1** |

换板子的话，只需要替换 `main/power_mgr.c` 里的显示/背光调用和
`managed_components` 里的 BSP，其余逻辑与硬件无关。

---

## 功能

- **自动认证**：使用主、备 `generate_204` 地址检测外网，解析跳转并核实 SRun 会话，
  明确未认证后再登录；普通 HTTP 200/3xx 不直接触发认证。登录后再次检测外网。
- **WiFi 页面后台刷新**：进入先显示已有结果，后台扫描不阻塞选网或返回；
  列表信息变化时才更新，旋转进度与刷新按钮跟随实际任务切换。
- **5 分钟轮询，永不停止**：熄屏、降频状态下照跑；失败按 30/60/120 秒退避三次。
- **多 WiFi / 多账号**：一个 WiFi 可存多个认证账号，点一下切换。
- **三种配置方式**，改的都是同一份数据：
  1. 屏幕上点（扫描 → 输密码 → 选账号）
  2. 浏览器打开板子 IP（页面带增删改，需输入屏幕上的 6 位验证码解锁）
  3. 把 SD 卡插到电脑上直接改文本文件
- **网页验证码防护**：屏幕实时显示 6 位验证码（5 分钟换新）+ 倒计时，
  未解锁时页面只显示验证码入口，验证成功后自动切换到完整配置面板；
  已解锁会话在本次开机期间最多有效 7 天，换码不影响会话，
  重启或重新烧录会使旧会话失效，重新进入网页会先向板子校验。
- **系统时钟**：所有界面顶部显示时间，联网后 SNTP 自动校时（东八区）。
- **双存储与热插拔**：片上 NVS + SD 卡同步，**有效 SD 优先，保留当前联网 WiFi**；
  后台约每秒检测拔插，熄屏和 80 MHz 省电状态照常处理（见下）。
- **自愈**：目标网络连不上会自动回退到上次成功的、或库里其它网络，
  没有备用网络时保留原目标并持续重试；路由器恢复后自动重新连接。
- **省电**：10 秒无触摸熄屏，再 60 秒把 CPU 降到 80 MHz；触摸即亮。
- **电池状态**：插了电池就在状态卡里显示电量百分比；**用电池供电**时再给一条
  「预计还能用多久」（按当前亮屏/降频状态估算），**插着 USB 充电**时只显示电量和
  「充电中 / 已充满」、不估续航；**没插电池整行不显示**。

---

## 编译与烧录

### 使用前配置校园认证地址

`main/app_config.h` 当前保留以下学校专用兜底配置，其他学校必须修改后才能用于本校认证：

```c
#define APP_PORTAL_FALLBACK_HOST  "http://10.253.0.235"
#define APP_PORTAL_FALLBACK_ACID  "3"
```

从本校实际登录页或成功页的网址提取这两个值。例如本次提供的网址是
`http://10.253.0.235/srun_portal_success?ac_id=3&theme=pro`：HOST 仅取协议、主机及可选端口，
不包含 `/srun_portal_success`、`/srun_portal_pc` 路径或查询参数；ACID 取 `ac_id` 的值。
如果学校使用 HTTPS 或非默认端口，也必须保留，例如 `https://portal.example.edu:8443`。
`http://0.0.0.0` 是无效占位地址，不能用来登录。

这两个认证参数直接参与每次启动后的探测，修改后需要重新编译烧录。
当前网页和屏幕没有手动编辑 Portal 地址的入口；发现并核实的认证地址按 WiFi 缓存在 NVS，
旧缓存无法连接时仍会尝试新配置的兜底地址，不需要清空 WiFi 或账号配置。
WiFi、校园网账号及密码可以通过屏幕或网页设置；已有配置不会因修改首次播种宏而自动覆盖。

### 本地编译与烧录

需要 **ESP-IDF v6.1**。先在你的 IDF 目录里跑一次 `export.ps1`（或手动设置
`IDF_PATH` + `IDF_PYTHON_ENV_PATH`），然后：

```powershell
# 1. 编译
.\build.ps1

# 2. 烧录（默认 COM6，可传参）
.\flash.ps1 COM6
```

两个脚本都用 `$PSScriptRoot` 定位工程目录，放在任何路径下都能用；产物在
`build/esp32s3_netkeeper.bin`。首次编译时组件管理器会按 `main/idf_component.yml`
自动拉取 LVGL 与 BSP 到 `managed_components/`（不入库）。

> [!WARNING]
> **flash-mode 必须是 `dio`**。这个板子用 `qio` 烧录后 ROM 会在第一段加载后挂死，
> 表现为 TG0WDT 复位循环、串口一个字都看不到。`flash.ps1` 里已经写死 dio。

`flash.ps1` 烧完会自己做一次复位让固件跑起来。这一步有坑，值得说清楚：

- 这块板的 **EN 没有接到 USB 的 RTS**，所以 esptool 的 `--after hard-reset`（拉 RTS）
  在这里是**空操作**。烧完固件停在 ROM 下载模式，画面不亮、串口没日志、网页也打不开，
  看起来就像"固件编坏了"。
- 能用的是 `--after watchdog-reset`（CPU 级复位，会正常从 flash 启动），但它会让
  esptool 丢掉串口、返回非 0，所以脚本把它拆成两步：写入用 `no-reset`（退出码只反映写入结果），
  然后单独发一次 watchdog 复位、忽略它的返回码。
- 若复位后串口仍然安静，**长按 PWR 一次**（PMIC 全断电再上电）必定能起来。

首次开机若两块存储都为空，会用 `main/app_config.h` 里的默认值播种一份配置，
所以"改完宏定义烧一次"就能直接用。

`main/ui_font_18.c` 是提交进仓库的（约 730 KB），因此**不装 Node 也能直接编译**；
只有改了界面中文文案才需要重新生成，步骤见「[目录结构](#目录结构)」里的字体小节。

首次开机若两块存储都为空，会用 `main/app_config.h` 里的默认值播种一份配置，
所以"改完宏定义烧一次"就能直接用。

---

## 自动化构建与发布

`.github/workflows/ci.yml` 两件事：

| 触发 | 做什么 |
|---|---|
| push 到 `main` / PR | 在 `espressif/idf:v6.1` 容器里跑 `idf.py build`，同时打印告警数；产物上传成 Actions artifact（保留 30 天） |
| 推与 `VERSION` 文件一致的 `v*` tag | **先编译**，成功后创建或更新 Release 并上传固件 |

> [!NOTE]
> Release 中提供固件压缩包和独立的 bin 文件；Actions artifact 也保留构建产物。

项目版本只在仓库根目录的 `VERSION` 文件中维护；固件版本、发布标签和 Release 标题均从它取得。
修改该文件并提交后，运行：

```bash
tag="v$(tr -d '\r\n' < VERSION)"
git tag -a "$tag" -m "$tag"
git push origin "$tag"     # 推完 Actions 会自动发布 Release
```

版本之间改了什么记在 [CHANGELOG.md](CHANGELOG.md) 里（本 README 只讲"现在是怎么做的"，
不堆历史）；发版时同步在那边加一节。

可复用的回归和截图工具保留在 `tools/`；实机配置备份、日志、临时测试产物、构建目录
及代码索引由 `.gitignore` 排除，不提交到源码仓库。

---

## 配置存在哪、谁优先

**写入路径：一次改动，两处落地**

```mermaid
flowchart LR
    M["改配置<br/>（屏幕 / 网页 / 读卡器）"] --> NVS[("片上 NVS<br/>namespace = portal")]
    M --> SD[("SD 卡<br/>/portal/net.cfg")]
    NVS -.->|"内容始终一致"| SD
```

**开机、重载及插卡时的加载顺序**

```mermaid
flowchart TD
    B([开机]) --> Q1{"SD 卡挂载成功？"}
    Q1 -- 否 --> N1["只读 NVS"]
    Q1 -- 是 --> Q2{"卡上有 net.cfg 且能解析？"}
    Q2 -- 是 --> S1["以 SD 为准，保留当前联网 WiFi 后同步两处"]
    Q2 -- 否 --> Q3{"NVS 里有配置？"}
    Q3 -- 是 --> N2["用 NVS，顺手补写一份到 SD"]
    Q3 -- 否 --> D1["用编译期默认值播种（NVS + SD 都写）"]
```

- 没插卡或运行中拔卡：使用片上 Flash；修改立即写入 Flash。
- 插入卡：有效 `/portal/net.cfg` 优先；缺失、仅有注释或损坏时，用 Flash 数据重建卡上配置。
  运行中直接修改卡上文件也会在下一次检测时加载，拿读卡器改完再插回不需要重启。
- 每次配置修改立即同步两处；写失败返回错误并在后台重试。检测无变化时不重复写 Flash。
- **活跃网络例外**：同步时若 WiFi 已关联并取得 IP，保留该网络的实际 WiFi 密码、全部校园网
  账号密码及当前选中账号，并继续以它为联网目标。Flash 为 ABC、当前连接 A、SD 为 DEF 时，
  最终两处都是 **ADEF**；B、C 不保留。SD 也含 A 且内容冲突时，同样保留正在使用的 A 信息。
  未连接时无此例外，最终两处为 DEF。这里按 WiFi 已取得 IP 判断，不要求外网探测刚刚成功。
- 网页「重载配置」「从 SD 恢复」遵守同样的活跃网络保护；无效 SD 的强制恢复返回错误。
  当前联网网络不能直接删除，可先切换网络再删除；账号的主动增删改仍正常同步。
- 有效数据库要求段名、WiFi 字段、账号格式与引用完整；超长、截断或部分损坏的文件不会覆盖
  Flash。显式空库可写为 `[general]` 加 `active=`。同步后文件会重写为标准格式。
- 数据上限仍为 16 个网络、每网 8 个账号、文本 4000 字节。SD 已满而无法保留活跃网络，
  或合并后文本超限时，拒绝本次导入并记录错误，不擅自丢弃任一网络。
- 拔插由独立 FreeRTOS 任务约每秒探测，实际延迟还包含卡命令超时和挂载时间；不依赖屏幕刷新。
  支持当前默认的熄屏及 80 MHz 降频模式。源码中关闭的实验性手动 light-sleep 分支不在此范围。

### 文件格式（`/portal/net.cfg`，UTF-8）

```ini
# 井号开头是注释
[general]
# 开机自动连的 WiFi，也是守护目标
active=MyHomeWiFi
# 上次成功的网络，自动维护，不用手改
last_ok=MyHomeWiFi

[MyHomeWiFi]             ; 段名就是 SSID，可以带空格
# 开放网络留空
wifi=wifi密码
# 当前使用的认证账号
current=20230000000
account=20230000000|密码1
# 一个 SSID 下可以写多行
account=13800000000|密码2
```

容量上限 4000 字节（NVS 单条 blob 限制），够放十几组网络。

---

## 屏幕操作

| 位置 | 作用 |
|---|---|
| **立即检测** | 手动触发一次检测（只有点它才会立即检测，碰屏幕不算） |
| **WiFi** | 扫描列表（已保存的带「删除」）、选网、输密码 |
| **账户** | 当前 WiFi 下的账号：使用 / 删除 / 添加 |
| 顶部中间 | 系统时间（联网后自动校准） |
| 状态卡右侧 | 网页验证码 + 倒计时（网页改配置要用） |

- 10 秒无操作熄屏，再 60 秒降频休眠；点一下屏幕即亮。
- 没配网时，开机 2.5 秒后自动跳到扫描页。
- WiFi 页再次进入时立即保留上次列表，同时在后台扫描；结果未变化时不重建列表，
  网络、信号或保存状态有变化才更新。扫描期间仍可选网或返回，扫描失败保留原列表；
  开机后首次进入仍需等待首次扫描，「重新扫描」可手动触发更新。
- 「重新扫描」按钮内置状态提示：扫描期间显示旋转圆圈与「扫描中…」，并暂停重复点击；
  完成或失败后恢复刷新图标与可点击按钮。动画直接跟随扫描任务，不按固定倒计时结束，
  也不会为展示扫描状态而清空或重建 WiFi 列表。
- WiFi 列表直接排在顶部标题栏下方，错误或空结果提示放在标题栏中间，避免空提示行撑开间距。
- 屏幕中删除 WiFi 或账号时先显示确认框，列出目标网络和账号；只有点击「确认删除」才修改配置，
  「取消」关闭弹窗。待删除对象独立复制，后台扫描刷新不会将删除操作指向另一条记录。
- 触摸只负责唤醒，不会误触发检测。

---

## 网页配置

板子连上网后，屏幕「配置网址」那一行就是入口，浏览器打开即可：

```
http://192.168.x.x/
```

**修改配置需要先解锁**：输入板子屏幕右上角显示的 6 位验证码（每 5 分钟自动更换，
屏幕上有倒计时）。解锁后的会话在板子本次开机期间最多有效 7 天，不受换码影响；
板子重启、重新烧录或会话失效后需要重新输入验证码。同一局域网的其他设备需要自行解锁。
进入页面时只显示「需要验证码」卡片，状态和网络/账号面板默认隐藏；验证码正确，
或浏览器保存的会话通过板子校验后，才自动切换到完整配置面板。
首次访问、会话失效或校验失败时保留入口；点击「锁定页面」会隐藏完整面板并恢复验证码卡片。

状态卡每 3 秒更新。编辑账号或密码时，网络列表变化会暂缓到结束编辑后的下一次刷新，
保留输入焦点、选区和已有网络的草稿。保存失败时保留内容；保存成功后只清空本次提交的内容，
不会清掉请求发出后继续输入的新草稿。升级固件后请刷新或重新打开网页以加载新的页面代码。

| 方法 | 路径 | 参数 | 说明 |
|---|---|---|---|
| GET | `/` | — | 配置页 |
| GET | `/api/state` | — | 状态 + 全部网络/账号（只读） |
| POST | `/api/auth` | `code` | 用验证码换 token |
| POST | `/api/session` | `token` | 只校验已有会话，不签发或续期 token，不计入验证码错误次数 |
| POST | `/api/wifi` | `token`,`ssid`,`pass`,`use` | 加/改 WiFi（新网络先试连，成功才切换） |
| POST | `/api/acct` | `token`,`ssid`,`user`,`pass`,`use` | 加/改认证账号 |
| POST | `/api/use` | `token`,`ssid`,`user` | 切换当前账号 |
| POST | `/api/remove` | `token`,`type=wifi\|acct`,`ssid`,`user` | 删除 |
| POST | `/api/check` | `token` | 立即检测 |
| POST | `/api/reload` | `token` | 重载配置 |
| POST | `/api/restore` | `token` | 导入有效 SD 到 Flash，保留活跃网络 |

写接口缺少或 token 失效时返回 `{"ok":false,"need_auth":true}`。
连错 5 次验证码会锁 60 秒。

> 提示：如果浏览器一直显示「连接板子失败」，检查系统代理（如 Clash）
> 是否拦截了内网 IP，把 `192.168.*` 加进直连名单。

---

## 目录结构

```
main/
  app_main.c        启动顺序：NVS → 配置库 → 屏 → WiFi → 守护 → 网页
  app_config.h      WiFi/账号首次播种值，以及持续使用的 Portal、轮询、电源编译期参数
  db.{c,h}          配置库：NVS + SD 双写、SD 优先、INI 解析
  wifi_mgr.{c,h}    WiFi 连接/扫描/IP 状态（不含认证逻辑）
  net_guard.{c,h}   5 分钟轮询、掉线重认证、试连与回退
  srun_port.{c,h}   Portal 探测与 SRun 登录
  srun_crypto.{c,h} MD5/SHA1/HMAC-MD5 + XXTEA 变体 + SRun base64
  http_util.{c,h}   极简 HTTP GET（能抓 Location）
  web.{c,h}         局域网配置面板 + JSON API + 验证码鉴权
  sys_time.{c,h}    SNTP 校时 + 系统时间
  batt_mgr.{c,h}    电池：AXP2101 读电压/充放电状态，按功耗模型估续航
  power_mgr.{c,h}   熄屏、触摸唤醒与 CPU 降频，统一屏幕亮度访问
  display_port.{c,h} BSP 显示初始化 + 可复用的内部 DMA 传输缓冲
  ui.c              主界面
  ui_setup.c        选网 / 输密码 / 账号管理
  ui_common.{c,h}   配色、字体、按钮工厂、顶部时钟
  ui_font_18.c      生成的中文字体（勿手改）
  index.html        配置页（构建时嵌进固件）
tools/
  monitor.py        抓串口日志；默认复位，--no-reset 保持设备运行
  test_logic.py     主机回归：编译真实 C 逻辑，覆盖输入、认证、电池、电源、显示传输与掉线恢复
  test_web_focus.js 浏览器回归：轮询焦点、选区、草稿与保存结果（Playwright CLI run-code，接口均模拟）
  test_web_session.js 浏览器回归：首次进入、缓存会话、重启失效、异步校验与重新解锁（接口均模拟）
  screenshots.py    一键生成设备端和网页截图，无需开发板或串口
  screenshots/     原界面源码的 LVGL 主机渲染器、模拟数据、网页截图与依赖说明
  collect_symbols.py 扫描源码里的所有非 ASCII 字，生成 symbols.txt
  font_coverage.py  检查符号表是否覆盖某个界面文案
  fontgen/          lv_font_conv 生成 ui_font_18.c（node gen.js）
```

截图统一使用[本地生成截图](#本地生成截图)流程；旧串口显存导出、还原和像素化工具已移除。

改界面文案后必须重新生成字体（LVGL 自带 CJK 字体缺「网/认/证/检/测」等简体字）：

```powershell
python tools\collect_symbols.py            # 源码 → tools\fontgen\symbols.txt
cd tools\fontgen
npm install                                # 只有第一次要
node gen.js                                # → tools\fontgen\ui_font_18.c
copy ui_font_18.c ..\..\main\ui_font_18.c
```

`gen.js` 默认用 `C:/Windows/Fonts/simhei.ttf`，换字体用环境变量：
`FONT=/path/to/NotoSansCJKsc-Regular.otf node gen.js`。

---

## 电池与功耗

### 电量显示

插上 MX1.25 锂电后，主界面状态卡左下角会自动多出一行（**没插电池整行不显示**）：

| 场景 | 显示 |
|---|---|
| 电池供电（拔了 USB） | `电池 62% · 3h20m` —— 剩下的百分比 + 预计还能用多久 |
| USB 在位、正在充电 | `电池 87% · 充电中` —— 只报电量，**不估续航**（充电时电流方向相反，估算没有意义） |
| 充满后已停充 | `电池 100% · 已充满` |
| 接了电源但没在充（满电待机、限流等） | `电池 98% · 已接电源` |
| 没插电池 | 不显示 |

后三种都是"USB 在位"，区别在 AXP2101 的充电状态位（`0x01[2:0]`：0–3 正在充电 / 4 充满 / 5 未充电），
所以界面不会把"停充"说成"充电中"。

续航是按**当前功耗状态**估的：亮屏约 120 mA、熄屏约 45 mA、熄屏降频约 28 mA。
AXP2101 没有电池电流 ADC，所以这是模型估算而不是实测——`APP_BATTERY_MAH`
（默认 1000 mAh）要按你的实际电池改，三个电流常量见
[第 12 节](#12-电池状态batt_mgrc--axp2101)。

同一份数据也可以通过网页接口读：
`GET /api/state` 里的 `bat_present` / `bat_pct` / `bat_mv` / `bat_usb` / `bat_charging` / `bat_min`。

### 充电和低电保护

适用电池：单节锂离子电池，标称 3.7 V、1000 mAh，充电限制电压 4.2 V。
初始化配置并读回确认 4.2 V 充电电压、300 mA 恒流限值和 50 mA 终止电流，
开启自动充电终止及充电超时保护。满电后由 PMU 自动停充，电压下降到芯片再充电条件后自动补充充电；
屏幕显示的估算百分比不参与停充判断。更换电池时需核对电池允许的充电电压和电流。

- 电池供电时，每 2 秒采样一次，连续 3 次不高于 3.30 V，记录低电锁定并请求 PMU 关机。
- 下次按电源键时，先检查电池；低电锁定且低于 3.50 V 时再次关机，不启动屏幕、SD 和网络。
- Type-C 供电允许启动和充电；电池达到 3.50 V 后解除锁定。插着 Type-C 但电池仍低时保留锁定，
  此时拔掉 Type-C 也会重新触发低电保护。
- PMU 的 VSYS 硬件欠压关机阈值设为 3.00 V，作为软件保护的兜底；它与电池端 VBAT 的阈值不同。

这是固件开机检查：按键可能让主控短暂上电执行检查，不代表硬件电源键被彻底禁止。
关机仍存在 PMU 自身的微小耗电，不能替代电池保护板或长期存放时的断开电池。
参数集中在 `main/app_config.h`，低电锁定使用独立 NVS 命名空间，不更改 SD 配置文件格式。

### 省电策略

默认策略是「熄屏 + CPU 降频到 80 MHz」，串口仍可用；电流随亮度、网络和板载负载变化，
续航估计使用配置中的经验功耗，不应把它当成实时电流测量。

亮屏、熄屏命令与 LVGL 绘图共用显示锁，避免两个任务并发访问同一个屏幕 SPI 设备。
切页期间显示锁繁忙时会保留唤醒请求并重试；命令成功后才更新屏幕状态。
屏幕传输缓冲在网络启动前一次性分配并复用，避免 WiFi 扫描时临时 DMA 内存不足导致部分画面未能刷新。

日常省电策略不进入 light sleep / deep sleep，仍保留 WiFi 和后台检测。
低电压保护触发时，由 AXP2101 执行关机，属于另一条保护流程。

AMOLED 亮度对功耗影响很大，`APP_BRIGHTNESS` 调到 60 左右通常够看。

---

## 认证与输入

### 同一 WiFi 切换认证账号

屏幕或网页选择另一个账号后，保持 WiFi 连接，查询 Portal 实际在线账号，
注销旧会话并登录新账号；切换后核对在线账号和外网连通性，失败会显示原因。
启动和普通「立即检测」保留已经能上网的手动认证会话，即使实际在线账号与保存的自动登录账号不同，
也不会主动注销；此时状态详情会说明保留现有会话，网页状态中的账号显示已查询到的实际在线账号。
需要切换时，在账号管理页明确点选「使用」或「当前」，或在网页选择使用账号。
发现的 Portal 地址按当前 WiFi 缓存在 NVS，在线重启后仍可使用。
首次安装时若设备已在线、此前尚未发现 Portal，则需先经历一次 Portal 自动发现/认证，
或设置现有 `APP_PORTAL_FALLBACK_HOST` 和 `APP_PORTAL_FALLBACK_ACID`。
无法识别 Portal 或无法核实身份时，不会把旧会话冒充为切换成功。

### 输入处理与探测

网页字段先完整 URL 解码，再按 UTF-8 字节数检查容量；非法编码、零字节和超长值拒绝保存。
认证时对 JSON 字符串和 URL 参数分别编码，支持密码中的双引号、反斜杠及用户名中的特殊字符。
主探测站返回 403、500、503 等无法判断联网状态的响应时继续尝试备用站，备用站也识别 Portal 跳转。

---

## 排错

### WiFi 掉线后的恢复与旧固件升级

旧固件连续两次连接超时且没有备用网络时，会把守护目标 `active` 清空，
因此主页进入 SETUP。与此同时，WiFi 页仅凭上次连接的名称标记“已连接”，
主页也可能保留旧 IP；看到这些信息并不代表当前链路仍有效。
本次实机读回确认 `active` 已为空，但 `last_ok` 和已保存网络仍在。

更新后的行为：每次连接等待 IP 最多 20 秒，失败后等待 5 秒继续重试；
连续失败时优先尝试备用网络，没有备用网络也保留原目标。
路由器恢复后自动重新连接，取得 IP 后继续原有的外网检测和校园网认证流程。
WiFi 页每 500 ms 更新“已连接”标记，主页及网页离线时清除旧地址与失效的检测倒计时。
WiFi 已连接但校园网认证失效时，仍按原认证流程处理。

升级时使用正常固件烧录流程，保留 NVS 和 SD 配置，不执行整片擦除。
若旧固件已经清空目标，升级本身不会猜测并重新选择网络：

1. 进入 WiFi 页，点选原来的已保存网络；仅点“重新扫描”或“立即检测”不会恢复目标。
2. 等待获得 IP，确认主页配置地址更新；已有 WiFi 密码和校园网账号无需重输。
3. 验证断网恢复时，让目标网络不可用超过两轮连接超时，再恢复网络，观察是否自动重连；
   同时检查 WiFi 页连接标记、主页地址及后续外网检测状态。

当前十一组主机回归、ESP-IDF 本地编译、固件版本与分区核对、实机烧录校验及启动心跳检查已通过。
主机测试模拟了连续超时后的恢复；这与在真实路由器上完成断网恢复验收是两项独立验证。

### 常见现象

| 现象 | 原因 |
|---|---|
| 串口只有 `ets_loader.c 78` 然后反复复位 | 烧录用了 qio，改回 dio |
| 烧完/接上电池后屏幕全黑、串口没日志、网页打不开 | 芯片停在 ROM 下载模式（本板 EN 未接 USB RTS，`hard-reset` 是空操作） | **长按 PWR 断电再开机**；或确认 `flash.ps1` 的 reset 步骤跑过了 |
| 屏幕出现方块/空白字 | 字体没重新生成，见上面 |
| 网页打不开 | 板子没连上 WiFi（看屏幕「配置网址」行），或与手机不在同一网段 |
| 旧固件掉线后停在 SETUP，WiFi 页却显示已连接 | 旧逻辑在两次连接失败且无备用网络后清空目标，并残留旧 SSID/IP；更新后保留目标重试。若目标此前已被清空，在 WiFi 页重新点选已保存网络一次 |
| 网页一直「连接板子失败」 | 浏览器走了系统代理，把 `192.168.*` 加进代理直连名单 |
| 网页提示需要验证码 | 屏幕右上角有 6 位验证码，输入后解锁；连错 5 次锁 60 秒 |
| 时间显示 `--:--` | 还没联网校时，联网后自动同步 |
| 认证一直失败 | 先核对学校的 Portal 地址、`ac_id` 和网络可达性，再检查账号密码及域后缀 |
| 取 challenge 失败 | 尚未建立到认证服务器的连接，检查认证地址、协议、端口及校园网链路；这一步不能说明密码错误 |
| 电池那一行不显示 | 没插电池；或 AXP2101 没应答（串口搜 `AXP2101 not responding`） |
| 电量显示虚高、一拔线就掉一截 | 充电时 VBAT 被抬高，调大 `APP_BAT_CHG_OFFSET_MV` |
| 续航估计不准 | `APP_BATTERY_MAH` 和实际电池不符；三个 `APP_BAT_CUR_*_MA` 要按自己的板子重估 |
| 插着充电器屏幕不熄 / 熄了又自己亮 | 触摸 INT 被芯片拉住（旧逻辑用电平判活动）。已改为「边沿唤醒 + 真实按下才算活动」，见第 11 节 |
| 卡里的配置没生效 | 格式不对（看 `[SSID]` 段和 `account=用户\|密码`），或超过 4000 字节 |

---

## 已知限制

- 只实现 SRun 协议；`ac_id`/Portal 地址靠 302 自动发现，兜底值在
  `app_config.h` 里配置（`APP_PORTAL_FALLBACK_HOST` / `APP_PORTAL_FALLBACK_ACID`），
  换学校按自己学校的参数改。
- 验证码鉴权防的是"同一局域网误改"，不是防专业攻击者；别把板子放进不可信网络。
- WiFi 密码与认证密码在 SD 卡上是**明文**，与多数路由器配置一致。
- 无电池后备 RTC，掉电后时间归零，联网自动重新校时。

## 实现细节（教学向）

下面按「一个请求从开机到联网」的顺序，把每个子模块在做什么、为什么这么写讲一遍。
想照着做一块自己的，或者想改成别的 Portal 系统，看这一节就够。

### 1. 整体形状

整块板子只有一件事要保证：**外网一直是通的**。围绕这一件事分成四个平级的角色，
互相之间只通过「配置库」和「状态快照」耦合，谁挂了都不影响别人活着：

```mermaid
flowchart LR
    SD[("SD 卡<br/>/portal/net.cfg")]
    DB["db<br/>NVS + SD 双写"]
    NG["net_guard<br/>守护任务"]
    UI["ui<br/>LVGL 单任务"]
    WEB["web<br/>httpd + 验证码鉴权"]

    UI <-->|"读写配置"| DB
    WEB <-->|"增删改"| DB
    DB <-->|"镜像"| SD
    NG <-->|"读配置"| DB
    NG -->|"状态快照"| UI
    NG -->|"状态快照"| WEB
```

三条设计原则贯穿全项目：

1. **状态只有一个来源**：`net_guard` 里那份 `net_status_t`。屏幕和网页都只是读它，
   谁都不许自己判断"现在通不通"。
2. **配置只有一个来源**：`db` 里的内存模型。屏幕上点的、网页上提交的、读卡器改的，
   最后都落到同一份 `net.cfg`。
3. **UI 线程只有一条**：LVGL 不是线程安全的，所有控件操作要么在 LVGL 任务里，
   要么拿着显示锁。

### 2. 任务与线程模型

| 任务 | 谁创建 | 栈 | 干什么 | 阻塞在哪 |
|---|---|---|---|---|
| `main` | IDF | 6144 | 初始化，建完各任务就退出循环 | — |
| `net_guard` | `app_main` | 8192 | 连 WiFi、探外网、认证、退避 | `vTaskDelay(500)` / 信号量 |
| `db_sd` | `db_init` | 6144 | 探测 SD 拔插、重新挂载、同步配置及重试写入 | `vTaskDelay(1000)` / 配置锁 |
| LVGL task | `esp_lv_adapter` | BSP 默认 | 跑 `lv_timer_handler`、重绘 | 显示锁 + 刷新周期 15 ms |
| `httpd` | `web_start` | 默认 | 接受 HTTP 连接 | socket |
| `power_mgr` | `power_mgr_start` | 默认 | 10 ms 轮询触摸 INT、管熄屏 | `vTaskDelay(10)` |
| 扫描任务 | 打开 WiFi 页或点「重新扫描」时临时建，不重复启动 | 默认 | 后台扫一圈 AP（任务内阻塞 3~6 s），保留现有列表 | 扫描 API |
| WiFi 事件任务 | IDF | — | `WIFI_EVENT` / `IP_EVENT` 回调 | 事件队列 |

同步手段按"临界区有多长"来选，这是刻意区分的：

- **显示锁** `ui_lock(ms)` / `ui_unlock()`：包 `esp_lv_adapter_lock()`，递归互斥，
  可以拿很久，用于 LVGL 控件操作和同一屏幕 SPI 设备上的亮度命令。
- **配置锁**：`db.c` 内部一把 mutex，对外暴露 `db_read_begin()` / `db_read_end()`
  让你成段读（期间别调会改库的函数，会自锁）。
- **状态快照**：`net_guard` 用 `portENTER_CRITICAL`，因为拷的是 300 字节的普通结构体，
  纯内存拷贝，不需要让任务睡眠——用 mutex 反而是浪费。

### 3. 启动顺序（`app_main.c`）

顺序不是随手排的，每一步都是后面一步的前提，且**尽量让用户早点看到画面**：

```
nvs_flash_init()          配置库和低电锁定要存 NVS
batt_mgr_init()          配置 PMU、执行低电开机检查；未通过则停止后续初始化
esp_netif_init()          协议栈
esp_event_loop_create()   事件循环，WiFi 事件要投递到这里
sys_time_init()           设东八区（校时等拿到 IP 再说）
db_init()                 挂 SD、加载配置，启动 SD 热插拔与同步后台任务
power_mgr_display_start() QSPI + LVGL 初始化，预留 DMA 传输缓冲，返回 lv_display_t*
ui_create()               ★ 建主界面，用户此刻开始看到东西
boot_hint (2.5 s 后)      没配网就直接把"选 WiFi"页推上去；没插卡弹提示
power_mgr_start()         熄屏/降频看门狗（屏幕初始化成功时）
batt_mgr_start()          后台电池轮询和低电保护，即使屏幕不可用也启动
wifi_mgr_init()           STA 起来，按配置里的 active 自动连
net_guard_start()         守护循环
web_start()               httpd 监听 80
```

`db_init()` 先加载配置，供配置页面和开机提示判断 SD 状态与当前目标网络。
UI 在 `wifi_mgr_init()` 之前创建，让用户在 WiFi 关联期间也能看到状态；
网页状态显示当前存储方式，主页不再单独展示存储信息行。

### 4. 网络守护主循环（`net_guard.c`）

`net_task` 是一个"永远不退出"的循环，每一轮做六件事：

```mermaid
flowchart TD
    S([循环开始]) --> A{"① 有人要求试连新网络？"}
    A -- 有 --> A1["连它"]
    A1 --> A2{"拿到 IP 且 SSID 为目标？"}
    A2 -- 是 --> A3["把 active / last_ok 切过去"]
    A3 --> B
    A2 -- 否 --> A4["保持原网络，等 2 s"] --> S
    A -- 无 --> B["② 读 active SSID"]
    B --> B1{"为空？"}
    B1 -- 是 --> B2["状态 NEED_SETUP，等 2 s"] --> S
    B1 -- 否 --> C["③ 从库里取该 SSID 的 WiFi 密码"]
    C --> D["④ 确保已连接并等 IP（最多 20 s）"]
    D --> D1{"成功？"}
    D1 -- 否 --> D2["失败计数 +1；连续 2 次 → 尝试备用网络；无备用则保留目标"] --> D3["等 5 s"] --> S
    D1 -- 是 --> E["⑤ 取该网络当前认证账号"]
    E --> E1{"有账号？"}
    E1 -- 否 --> E2["状态 NO_ACCOUNT，等 3 s"] --> S
    E1 -- 是 --> F["⑥ run_cycle：探测 → 必要时认证 → 再探测确认"]
    F --> F1{"外网通？"}
    F1 -- 是 --> F2["睡 5 分钟（APP_POLL_INTERVAL_MS）"] --> S
    F1 -- 否 --> F3["按 30 s / 60 s / 120 s 退避，之后回到 5 分钟"] --> S
```

状态机就是 `net_status_t.state`，共 11 个值（`net_guard.h`）：

```mermaid
stateDiagram-v2
    [*] --> BOOT
    BOOT --> WIFI_CONNECTING : 守护任务启动
    WIFI_CONNECTING --> CHECKING : 拿到 IP
    WIFI_CONNECTING --> WIFI_DOWN : 20 s 超时
    CHECKING --> ONLINE : 探测到 204
    CHECKING --> NEED_AUTH : 被 302 劫持
    CHECKING --> ERROR : 探不到 Portal / 无链路
    CHECKING --> NO_ACCOUNT : 该网络没有认证账号
    NEED_AUTH --> AUTHING : 组装 hmd5 / info / chksum
    AUTHING --> ONLINE : 认证后再探一次通了
    AUTHING --> AUTH_FAILED : Portal 返回失败
    ONLINE --> CHECKING : 下次轮询 / 掉线
    AUTH_FAILED --> CHECKING : 退避后重试
    NO_ACCOUNT --> CHECKING : 补了账号
    WIFI_DOWN --> WIFI_CONNECTING : 保留目标重试 / 尝试备用网络
    NEED_SETUP --> WIFI_CONNECTING : 用户选好网络
```

三个"防呆"设计，都是为了不把板子搞成砖：

1. **试连失败不回退目标**（步骤①）。网页上加了个新 WiFi，如果连不上就直接把 active
   改过去，用户会瞬间失去网页访问（IP 变了），只能拔卡救。所以只有**拿到 IP 之后**
   才 `db_set_active_ssid()`。
2. **连不上自动换网**（步骤④）。`conn_fail >= 2` 时优先回退 `last_ok`（上次真正联通的），
   没有再挑库里任意一个别的；没有备用网络就保留 active，等待 5 秒后继续尝试，
   每次等待 IP 最多 20 秒。只有未选择目标网络时才进入 `NEED_SETUP`。
3. **认证成功不等于有网**。`srun_login` 返回成功后再探一次外网，通了才算 `ONLINE`，
   同时 `auth_count++`。很多 Portal 会返回"成功"但实际没放行（限速、欠费、绑定设备数超限），
   这一步能抓到。

等待期间不是死睡：每 500 ms 看一次 `wifi_mgr_connected()`，掉线立刻重来一轮；
同时轮询 `s_trigger` 信号量，网页点「立即检测」能把它叫醒（`idle_wait()`）。

主页与网页从同一份连接快照获取当前 SSID/IP；断线或丢失 IP 后清除旧地址和失效的检测倒计时。
WiFi 页每 500 ms 刷新连接标记，只有已取得 IP 且 SSID 匹配的网络才显示“已连接”。
网络配置、WiFi 密码和认证账号继续保留，不因暂时离线而删除。

### 5. 外网探测：怎么判断有没有网、Portal 在哪（`srun_port.c`）

`get_204` 是这套东西的地基：请求一个**一定返回 204 且内容为空**的地址，
如果被劫持，网关会用 302 把你甩到登录页——**302 的 Location 就是 Portal 地址**。

```c
probe_result_t srun_probe(portal_info_t *pi);
```

判定规则：

| 现象 | 结论 | 后续动作 |
|---|---|---|
| HTTP 204 | 真的通 | 状态 ONLINE，直接睡 5 分钟 |
| HTTP 3xx | 跳转候选，尚未证明需要认证 | 从响应头提取 SRun 跳转地址和 `ac_id`，继续备用检测 |
| HTTP 200 | 结果不明确，不能直接判断未认证 | 继续备用 204 检测，不把普通网页当成校园认证 |
| 请求失败 | 可能是 DNS、单域名或链路问题 | 换备用 204 地址，再查询有效的 SRun 认证地址 |

`Location` 通过 `HTTP_EVENT_ON_HEADER` 回调捕获；`esp_http_client_get_header()` 读取的是请求头，
不能用来取得服务器的跳转响应头。过长的跳转地址不接受截断结果。
`parse_location()` 提取 `scheme://host[:port]` 和有效 `ac_id`，普通网页跳转不直接当成 SRun 地址。
任一探测地址返回 204 就按外网已通处理；否则依次尝试发现地址、同 WiFi 缓存和学校专用兜底地址，
通过 `/cgi-bin/rad_user_info` 确认协议与会话状态：明确返回未在线才进入登录；
已有在线会话而外网探测异常时，显示检测异常并稍后重试，不发起登录或注销。
空地址、无效地址以及 `0.0.0.0` 不会被用于查询、登录或注销。

主地址（小米）和备用地址（gstatic）共同减少单一探测服务异常的误判；
兜底地址必须按学校配置。认证服务能访问不代表外网已经恢复，仍以外网探测结果判断联网状态。

### 6. SRun 认证算法：三个字段是怎么算出来的（`srun_crypto.c`）

SRun（深澜）的登录页是 JS 前端算完再提交的，逆向出来就三个字段。
`token` 是服务端先给的挑战值（`get_challenge` 返回的 `challenge`）：

```
hmd5   = HMAC-MD5(key = token, msg = 明文密码)               → 32 位小写 hex
info   = "{SRBX1}" + SRunBase64( xencode(json, token) )
chksum = SHA1( token+username + token+hmd5 + token+acid + token+ip
             + token+n(200)   + token+type(1) + token+info )   → 40 位小写 hex
```

其中 `json` 是：

```json
{"username":"学号","password":"密码","ip":"10.x.x.x","acid":"3","enc_ver":"srun_bx1"}
```

两个必踩的坑，写在 `srun_crypto.h` 顶部了：

1. **Base64 字母表是改过的**：
   `LVoJPiCN2R8G90yg+hmFHuacZ1OWMnrsSTXkYpUq/3dlbfKwv6xztjI7DeBE45QA`
   用标准表算出来的字符串服务端一定不认。
2. **xencode 是 XXTEA 变体**，JS 原版 for 循环跑完后 `p == n`，收尾那一轮用
   `k[(n & 3) ^ e]`；写成 `k[((n-1) & 3) ^ e]` 不报错，只是结果全错。
   这个 bug 我们是在电脑端 Python 参考实现里先踩到、再同步到 C 的。

还有一个**已知边界**：JS 的 `s()` 按 UTF-16 code unit 取字符，这里的实现按字节打包。
用户名/密码/ip/ac_id 全是 ASCII 时两者完全等价；一旦有中文（比如中文 WiFi 密码当认证密码）
结果会不同，届时要先转 UTF-16LE。这条写进注释而不是"假装支持"。

想验证自己的移植对不对，打开 `APP_CRYPTO_SELFTEST`，`srun_crypto_selftest()`
会用固定输入（token/密码/JSON 都是故意编的）跑一遍并把中间值打到串口，
和注释里的期望值逐字节对比即可。

### 7. 登录请求与响应判定（`srun_port.c`）

标准的 SRun 两步：

```
GET /cgi-bin/get_challenge?callback=jsonp&username=<u>&ip=&_=<ms>
    → jsonp({ "challenge": "...", "client_ip": "10.x.x.x" })
       注意：client_ip 是服务端看到的地址，NAT 后面也是对的，不要用本地 IP

GET /cgi-bin/srun_portal?callback=jsonp&action=login
    &username=<u>&password=%7BMD5%7D<hmd5>&os=Windows+NT&name=Windows
    &double_stack=0&chksum=<chksum>&info=<urlencode(info)>
    &ac_id=<ac>&ip=<client_ip>&n=200&type=1&_=<ms>
    → jsonp({ "error": "ok", "suc_msg": "login_ok", ... })
```

响应不是标准 JSON，是 `jQuery1234_567({...})` 这种 JSONP，所以先 `strip_jsonp()`
掐头去尾。字段都是扁平的，也没必要拖一个 cJSON 进固件——`json_get()` 三十行搞定。

判定逻辑：

- `error == "ok"` 且 `suc_msg == "ip_already_online_error"` → 该 IP 已经在线，当作成功
- `error == "ok"` → 成功
- 其它 → 失败，把 `error` + `error_msg` 原样显示到屏幕上（排错全靠它）

失败后还有一次**兜底重试**：有的路由器会把 302 甩到自己的页面而不是校园 Portal，
这时用 `APP_PORTAL_FALLBACK_HOST` 再登一次，两次的失败原因会拼在一起显示。

### 8. 配置库：一份数据，两处存储（`db.c`）

内存模型是定长数组，不做动态分配（嵌入式里碎片比容量更可怕）：

```c
#define DB_MAX_NETS  16      // 最多 16 个网络
#define DB_MAX_ACCT  8       // 每个网络最多 8 个账号
#define DB_TEXT_MAX  4000    // 序列化文本上限（NVS 单条 blob 的安全值）
```

序列化成人能读的 INI，是为了"读卡器能改"这个需求；`db_save()` 一次写两处：

```
改内存 → 序列化成文本 → 写 NVS（一个 blob）→ 写 SD（net.tmp → rename）
```

SD 写入先完成 `net.tmp` 并检查写入及关闭结果，再替换 `net.cfg`。FAT 替换旧文件需要
先删除旧路径，因此不承诺掉电原子性；重启时缺失或损坏的配置由 Flash 重建。
Flash 写失败和 SD 写失败分别保留重试状态，SD 成功后才更新其同步标记。

本次修复了空卡备份只剩第一行注释的问题：INI 解析会就地修改文本缓冲区，
因此每次写卡都先重新序列化完整数据库。后台 `db_sd` 任务与界面刷新独立，
每秒检查卡状态；拔卡切到 Flash，插卡按有效 SD 优先规则同步，并保留活跃网络。

读的时候有效 SD 优先，并保留当前联网网络，分支见前面「[配置存在哪](#配置存在哪谁优先)」一节。
`db_read_begin()/db_read_end()` 是给"要连着读好几个字段"的场景用的
（比如遍历网络列表建 UI），期间别调写接口，否则 mutex 自锁。

### 9. 局域网配置面板（`web.c` + `index.html`）

`index.html` 在**编译期**被嵌进固件（`CMakeLists.txt` 里的 `EMBED_TXTFILES`），
所以网页本身不依赖文件系统。一共十一个路由：

| 路由 | 方法 | 鉴权 | 作用 |
|---|---|---|---|
| `/` | GET | 否 | 配置页面 |
| `/api/state` | GET | 否 | 状态 + 全部网络/账号（只读，方便看） |
| `/api/auth` | POST | — | 用 6 位验证码换 token |
| `/api/session` | POST | 校验已有 token | 只读验证会话，不签发或续期 |
| `/api/wifi` | POST | 是 | 添加/更新 WiFi |
| `/api/acct` | POST | 是 | 添加/更新账号 |
| `/api/use` | POST | 是 | 切换当前账号 |
| `/api/remove` | POST | 是 | 删网络或账号 |
| `/api/check` | POST | 是 | 立即检测 |
| `/api/reload` | POST | 是 | 重载 SD 配置 |
| `/api/restore` | POST | 是 | 有效 SD 同步至 Flash，保留活跃网络 |

`/api/state` 是不鉴权的只读状态接口；写配置必须验证 token。
`/api/session` 只检查已保存的 token，不修改配置，也不增加验证码错误次数。

**验证码 + token 的模型**（防的是"同一局域网的人误改"，不是防专业攻击者）：

```mermaid
sequenceDiagram
    participant S as 屏幕
    participant B as 板子（web.c）
    participant W as 浏览器

    S->>B: 显示 6 位码（esp_random，TTL 5 min，带倒计时）
    W->>B: POST /api/auth  { code }
    alt 验证码正确
        B-->>W: 16 字符 token（8 个槽位，替换最早签发的槽位）
        Note over W: 存 localStorage<br/>之后每个写请求都带上
        Note over B: 本次开机内最多有效 7 天<br/>换码不影响会话，重启即失效
    else 连错 5 次
        B-->>W: 锁 60 s（避免 6 位数被穷举）
    end
```

网页初始只显示验证码入口，整个配置面板默认隐藏，避免加载时短暂露出状态和账号列表；
验证码正确后才显示完整配置面板。存在浏览器缓存 token 时先请求 `/api/session`，有效后切换到面板。
点击「锁定页面」或写请求返回会话失效时，立即隐藏完整面板并恢复验证码入口。
旧响应不能覆盖新解锁或主动锁定操作。页面每 3 秒刷新状态，网络数据不变时保留列表；
正在编辑时延迟列表重建，保存后才清理相应草稿，避免手机键盘因轮询而收起。

**这里有一个必须记住的 HTTP 细节**：`/api/state` 用 `httpd_resp_send_chunk()`
分块发 JSON，发完最后一段**必须再发一个空块**收尾：

```c
httpd_resp_send_chunk(req, "]}", 2);
return httpd_resp_send_chunk(req, NULL, 0);   /* ← 少了这行，浏览器永远转圈 */
```

curl 对"连接关闭即结束"很宽容，所以命令行测没问题；浏览器 `fetch()` 会一直等
chunked 的结束块，表现就是页面卡在「读取中」。

### 10. UI：一条线程、一把锁、一套排版规则（`ui.c` / `ui_common.c` / `ui_setup.c`）

LVGL 9 跑在 `esp_lv_adapter` 起的单任务里，刷新周期 15 ms。任何别的任务碰控件
都必须先 `ui_lock()`：

```c
bool ui_lock(int timeout_ms) {
    return esp_lv_adapter_lock(timeout_ms) == ESP_OK;   /* 注意：== ESP_OK 才是拿到锁 */
}
```

> [!CAUTION]
> **这个项目耗时最久的 bug 就在这里**。Waveshare BSP 的
> `bool bsp_display_lock()` 直接 `return esp_lv_adapter_lock()`，
> 而后者返回 `esp_err_t`，`ESP_OK == 0`——转成 `bool` 后**成功变成 false**。
> 于是 `if (bsp_display_lock(2000)) { ... }` 在**拿到锁的时候跳过整个临界区**，
> 连 `unlock()` 都不执行，递归互斥量被永久扣住，LVGL 任务从此冻死。
> 症状极具迷惑性：芯片活着（HTTP 正常、`uptime` 在涨）、只有画面不动。
> 现在全项目只用 `ui_lock()/ui_unlock()`，禁止判断 BSP 那个函数的返回值。

屏幕 480×480，坐标直接用像素写死（`ui_common.h` 里的配色常量 + `ui_font_18`）。
三条排版规则是从踩坑里总结出来的：

1. **键盘占底部 250 px**（顶边 y=230），所以带键盘的页面，输入框和按钮必须排在 y<230。
   最早「添加账号」的保存按钮放在 y=218、高 42，正好被键盘压掉，点了没反应。
2. **标题行只放固定文案**。动态内容（SSID）另起一行：标题宽度限制 250 px +
   `LV_LABEL_LONG_DOT` 兜底，否则 `认证账号 · 一个很长的SSID` 会一路压到时钟和返回键上。
3. **混排中文的标签一律用 `ui_font_18`**，不能用 `lv_font_montserrat_14`。
   后者既没有 `·`(U+00B7) 也没有任何汉字，画出来全是方框（tofu）。

**字体为什么必须自己生成**：LVGL 自带的 `lv_font_source_han_sans_sc_16_cjk`
是偏日文的子集，缺「网 / 认 / 证 / 检 / 测」这些简体字，直接拿来做这个界面会满地方块。
所以用 `lv_font_conv` 从系统黑体按**源码里实际用到的字**生成一个 18 px 子集字体：
`collect_symbols.py` 扫所有 `.c/.h` 里的非 ASCII 字符 → `symbols.txt` → `node gen.js`。
这样字体只有 730 KB，而不是全量 CJK 的十几 MB。

几个子界面的实现要点：

- **选 WiFi**：扫描要阻塞 3~6 s，所以放在临时任务里跑，扫完把结果拿到手，
  **先在锁外**把"信号强度 / 是否已保存"算好，再进锁只做控件操作。
  进入页面先显示已有列表，同时后台扫描；结果相同时保留原控件，有变化才重建，
  扫描失败也不清空旧列表。旧列表的 SSID 回调载荷只在显示锁内更换，避免扫描时点错网络。
  早期版本在显示锁里调 `net_saved()`（内部要拿配置锁），两个锁一嵌套就把 LVGL 卡死了。
- **选 WiFi 的行**：整行可点（连这个网），右侧「删除」按钮单独可点且不冒泡，
  这样"点行连网 / 点删除删网"互不干扰。
- **账户管理**：SSID 单独一行显示（428 px 宽），账号列表每行两个按钮
  （`使用/当前` 与 `删除`），当前账号高亮。
- **顶部时钟**：挂在 `ui_header()` 里，所以每个子界面自动都有；主界面单独居中。
  内容来自 `sys_time`，每 500 ms 刷一次（和状态刷新同一个 `lv_timer`）。

**切页割裂与残影的排查结论**：从主页进入 WiFi 页时，曾出现短暂卡顿和新旧画面交错，
扫描结束后仍留有部分旧内容。复现时抓到 `Failed to allocate priv TX buffer` 和
`Draw bitmap failed: ESP_ERR_NO_MEM`：BSP 的绘图缓冲位于 PSRAM，SPI 每次传输还需临时申请
内部 DMA 缓冲；内部连续内存不足时，该显示块未能发送，后续局部刷新也不会补齐整页。
重启只能暂时缓解，不能消除这条失败路径。

`display_port.c` 现按 LVGL 绘图缓冲的实际大小，在 WiFi 启动前一次性预留内部 DMA 内存并复用。
适配层完成像素打包及 RGB565 字节交换后，将数据复制到这块内存再发送；沿用原有完成回调，
上一块传输完成后才复用缓冲。无需改动页面布局，也不更改 BSP 的触摸配置。
修正后记录了 18 轮扫描，原有 DMA 分配与传输失败未再出现，实机反复切页也已确认画面正常。

### 11. 电源管理（`power_mgr.c`）

三级策略，都靠"最后一次触摸时间"驱动：

```mermaid
stateDiagram-v2
    [*] --> ACTIVE
    ACTIVE --> BLANK : 10 s 无触摸（背光 0，LVGL 照常跑）
    BLANK --> IDLE : 再 60 s（CPU 降到 80 MHz）
    BLANK --> ACTIVE : 触摸（INT 高→低边沿）
    IDLE --> ACTIVE : 触摸（回 240 MHz + 背光）
```

`POWER_MGR_LIGHT_SLEEP` 默认 **0**（不真睡）。改成 1 会 `esp_light_sleep_start()`
并按触摸 INT 电平唤醒，电流从 ~25 mA 降到 ~1–2 mA，但代价是 USB-Serial/JTAG
的时钟被门控，**串口会消失**——调固件时千万别开。

触摸检测是 10 ms 轮询 `gpio_get_level(BSP_LCD_TOUCH_INT)`，但**两条路径的判定不一样**，
这一点很容易写错：

| 场景 | 判定 | 为什么 |
|---|---|---|
| 息屏后要**唤醒** | INT 出现「高→低」的**边沿**（且熄灭超过 300 ms） | 触摸芯片会把 INT 拉住直到报告被读走。灯一熄就没人读报告，INT 可能一直是低的——用电平判断会在熄灭瞬间把自己点回来，来回闪 |
| 亮屏时算**活动** | INT 低 **且** LVGL 那边 `lv_indev_get_state() == PRESSED` | 充电时面板噪声会让芯片不断产生报告，INT 被拉低；用电平当活动会让空闲计时永远被刷新，**插着 USB 就再也不熄屏** |

也就是说：INT 只当作"可能有触摸"的廉价门控，真正认定触摸要问 LVGL。
改之前用的是纯电平，实测出现过"充电时一直亮着"——根因就是 INT 卡低。

亮度命令也会访问屏幕 SPI，必须与 LVGL 绘图共用显示锁。此前实机出现过屏幕停住、
不自动熄灭，但网页仍可访问，任务栈显示电源任务和绘图任务同时卡在 SPI 访问中。
现在启动亮屏及运行期亮度设置都在显示锁内执行；锁繁忙时保留唤醒请求并重试，
命令成功后才更新屏幕状态，避免切页期间漏掉唯一一次触摸边沿。

> [!NOTE]
> 插着 USB 充电时，熄屏（10 s）和降频（70 s）**照样执行**，和电池供电一致；
> 只有「电量行」从"续航估计"变成"充电中"。

### 12. 电池状态（`batt_mgr.c` + AXP2101）

三件事：**判断有没有电池**、**算电量**、**估续航**。前两件是读数，第三件是模型——
因为 AXP2101 **没有电池电流 ADC**（它只有 VBAT/VBUS/VSYS 电压和芯片温度），
拿不到实测放电电流，所以"还能用多久"只能是估算。

寄存器（以 AXP2101 数据手册的字段定义为准）：

| 寄存器 | 含义 |
|---|---|
| `0x00` status1 | bit5 = VBUS good（USB 在位），bit3 = 电池在位 |
| `0x01` status2 | bit[6:5] 电流方向；bit[2:0] 充电阶段：0–3 充电中 / 4 充电完成 / 5 未充电 |
| `0x34` + `0x35` | VBAT，14 位（高 6 位 + 低 8 位），单位 mV |
| `0xA4` | E-Gauge 电量百分比（本板没做电池标定，只用于对照，**不采信**） |

**判断"有没有插电池"**：使用 PMU 电池在位状态，电池供电时结合 VBAT 校验；
低电压不再被当作未插电池。电池保护在显示、SD 和 WiFi 初始化之前执行。

**电量**：分段线性查表（3.30 V→0%，4.15 V→100%）。有两个刻意的处理：

- 带负载时电压会塌，所以曲线整体取得保守（3.75 V 只算 45%，而不是常见的 55–60%）；
- **充电时 VBAT 会被抬高**（线损 + 极化），查表前先减 `APP_BAT_CHG_OFFSET_MV`（40 mV），
  否则充着电会显示 95% 甚至 100%，一拔线就掉到 80%。

**续航**：`剩余容量 / 当前典型电流 × 60`，三个电流档位按工作状态取：

| 状态 | 电流（`app_config.h`） |
|---|---|
| 亮屏 + 240 MHz + WiFi | `APP_BAT_CUR_SCREEN_ON_MA` = 120 mA |
| 熄屏，CPU 仍 240 MHz | `APP_BAT_CUR_IDLE_MA` = 45 mA |
| 熄屏 + 降频 80 MHz | `APP_BAT_CUR_IDLE_LOW_MA` = 28 mA |

容量只有 `APP_BATTERY_MAH` 一个参数要按实物改（默认 1000 mAh）。换屏、改亮度、
加外设之后这三个电流值会偏，需要重新估——所以它显示的是"约 XhYYm"，不是精确值。

**显示规则**（`ui.c` 的电池分支，位置在状态卡左下角）：

| 情形 | 显示 |
|---|---|
| 没插电池 | 整行隐藏（`ui_obj_hidden`） |
| 有电池 + USB 在位 + `status2[2:0]=0…3` | `电池 87% · 充电中`（绿色，不给续航） |
| 有电池 + USB 在位 + `status2[2:0]=4` | `电池 100% · 已充满`（绿色，不给续航） |
| 有电池 + 没 USB | `电池 62% · 3h20m`（≤15% 转红色） |

为什么不给充电中估续航：充电时电流方向是相反的，用放电模型算出来的数字没有意义。

一个实现细节：这个标签**只有文本真的变化时才 `lv_label_set_text()`**。状态定时器
500 ms 跑一次，如果每次都无条件 set，LVGL 会每 500 ms 重绘一次这块区域，
白白多耗电——对一块电池供电的表来说这就是"功能自己吃掉了自己的续航"。

### 13. 构建期配置（`sdkconfig.defaults` 里几个非默认项）

| 配置 | 为什么 |
|---|---|
| `CONFIG_SPIRAM_MODE_OCT` + `SPIRAM_SPEED_80M` | 板子是 N16R8，8 线 PSRAM，不设会认不到或跑 40M |
| `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` / `RODATA` | 把代码和常量搬进 PSRAM，省内部 RAM 给 LVGL 缓冲 |
| `CONFIG_PM_ENABLE` | `esp_lv_adapter` 的自动休眠要建 `NO_LIGHT_SLEEP` 锁，不开这个会 assert |
| `CONFIG_ESP_CONSOLE_UART_BAUDRATE=2000000` | CDC 控制台，高波特率才追得上日志量 |
| `CONFIG_FREERTOS_HZ=1000` | `vTaskDelay(pdMS_TO_TICKS(10))` 才真的是 10 ms |
| `CONFIG_LV_USE_FONT_COMPRESSED` | 生成字体用了 4 bpp，需要它 |
| `CONFIG_MBEDTLS_MD5_C` / `SHA1_C` | SRun 算法要 MD5 和 SHA1 |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=6144` | `app_main` 里 `db_dump()` 打日志会吃掉不少栈 |

### 14. 想换成别的 Portal 系统（Dr.COM / 华为 / 锐捷）

改动被刻意收在一个文件里：

| 文件 | 要改什么 |
|---|---|
| `srun_port.c` | `srun_probe()` 的判定规则、`srun_login()` 的请求参数与响应字段 |
| `srun_crypto.c` | 如果对方的密码字段不是 HMAC-MD5 + 自定义 base64，替换这三个函数即可 |
| `app_config.h` | 兜底 Portal 地址 / `ac_id` / 探测 URL / 轮询与退避时间 |

`net_guard.c`、`db.c`、`web.c`、UI 全部与协议无关，一行都不用动——它们只关心
"`probe` 说要不要认证、`login` 说成没成功"。

---

## 技术路线与关键取舍

一句话概括路线：**协议层尽量窄（只做 SRun 一家做透），其余部分尽量笨（能查表就不搞模型，
能手算就不上库）**。每个决策后面都写清备选方案为什么被否——这才是"路线"而不是"实现细节"。

| 决策 | 选了什么 | 为什么，以及否掉了什么 |
|---|---|---|
| 认证协议 | 只实现 SRun，字段级对齐登录页 JS | 换学校的成本压在 `srun_port.c` + `srun_crypto.c` 两个文件里；不做"通用 Portal 框架"，那种抽象在没有第二个真实样本前一定是错的 |
| 网络判定 | `generate_204` 探测 + 302 抓 `ac_id` | 不解析登录页 HTML（学校一改版就崩）；不写死 Portal 域名（NAT 后面取到的 `client_ip` 更可靠） |
| 认证成功判定 | 登录返回 ok **后**再探一次外网 | 只信 Portal 的返回值会踩"返回成功但没放行"（限速、欠费、设备数超限），多一次 GET 换来状态可信 |
| UI 线程模型 | LVGL 单任务 + 一把递归锁 | 不用"每屏一个任务"：LVGL 不是线程安全的，多任务只会把竞态藏得更深；也不用全局无锁，跨任务的写最终会打架 |
| 配置存储 | NVS + SD 双写，SD 优先 | 只存 NVS 的话，改配置必须重烧；只用 SD 的话，没插卡就不能用。双写 + 人可读 INI 支持读卡器修改后热插回板子，活跃网络仍保留 |
| 电量计算 | AXP2101 电压查表 + 充电补偿 | AXP2101 有 E-Gauge（`0xA4`），但没做电池标定时读数不可信，所以**不采信**它，用确定性更强的电压曲线 |
| 续航估计 | 按工作状态取电流的功耗模型 | AXP2101 **没有电流 ADC**，库仑计也不存在；与其编一个"精确"数字，不如明确写成"约 XhYYm"并把三个电流常量暴露在 `app_config.h` 里 |
| 中文字体 | 从系统黑体按源码用字生成子集（约 730 KB） | LVGL 自带 CJK 字体是偏日文的子集，缺「网/认/证/检/测」；全量 CJK 十几 MB 不适合 8 MB app 分区。生成物入库，保证 clone 后不装 Node 也能编 |
| 配置页 | `index.html` 编译期嵌进固件 | 不需要文件系统、不怕 SD 卡坏；代价是改页面必须重编，对这个规模可以接受 |
| 抓图 | 串口 dump 显存 + 电脑端还原 PNG | 板上没有截图能力，也没有可写的大容量存储；dump 走独立任务（见踩坑），抓图时界面照常动 |
| 烧录参数 | 写死 `--flash-mode dio` | 这块板 `qio` 会在第一段加载后挂死，属于硬件/ROM 层面的坑，不给"可配置"留口子 |
| CI | 固定 `espressif/idf:v6.1` 容器 | 不用 `latest`：IDF 大版本之间 Kconfig 会变，`sdkconfig.defaults` 一旦对不上就是一堆莫名报错 |

明确**没做**的三件事，以及原因：

- **不做 OTA**：设备装在宿舍墙上，一次烧录长期使用；OTA 要配套分区切换、回滚、签名，收益不抵复杂度。
- **不做多协议适配层**：见上表第一行，没有第二个真实样本就不抽象。
- **不做手机 App**：局域网网页 + 屏幕已经覆盖全部操作，多一个 App 就多一套发布与兼容负担。

---

## 踩坑记录

按子系统分组。每条都是「症状 → 根因 → 修法」，修法后面标了对应章节，方便回看实现。

### 显示与 LVGL

| 症状 | 根因 | 修法 |
|---|---|---|
| 主页切入 WiFi 页割裂、卡顿，扫描后仍有旧画面 | PSRAM 绘图数据传输时临时申请内部 DMA 缓冲失败，出现 `ESP_ERR_NO_MEM`，部分显示块未发送 | `display_port.c` 在网络启动前预留并复用内部 DMA 缓冲；等传输完成后再复用。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |
| 屏幕停住且不自动熄灭，网页仍可访问 | 电源任务与 LVGL 绘图并发访问同一屏幕 SPI 设备 | 所有亮度命令与绘图共用显示锁；拿锁失败不提前改变屏幕状态，保留唤醒请求重试。[§11](#11-电源管理power_mgrc) |
| 画面冻住、点屏没反应，但 HTTP 正常、`uptime` 在涨 | `bsp_display_lock()` 把 `esp_err_t` 直接转 `bool`，而 `ESP_OK==0`——拿到锁反而判成失败，临界区被跳过、锁泄漏 | 全项目只用 `ui_lock()`（`== ESP_OK` 判定），禁用 BSP 那个函数的返回值。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |
| 汉字变方框（tofu） | 混排行用了 `lv_font_montserrat_14`，它既没有 `·` 也没有汉字 | 混排一律 `ui_font_18`，并用 `font_coverage.py` 校验覆盖率。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |
| 「网 / 认 / 证 / 检 / 测」大面积缺字 | LVGL 自带 `SOURCE_HAN_SANS_SC_16_CJK` 其实是偏日文的子集 | 按源码用字生成子集字体（`collect_symbols.py` → `node gen.js`）。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |
| 输入框和按钮被键盘挡住、点了没反应 | 键盘固定占底部 250 px（顶边 y=230），按钮排在 y=218 | 带键盘的页面，控件一律排在 `y < 230`。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |
| 标题太长压到时钟和返回键 | 把动态 SSID 拼进了标题行 | 标题只放固定文案，动态内容另起一行 + `LV_LABEL_LONG_DOT`。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |
| 编译时一堆 `lv_obj_remove_flag is deprecated` | LVGL 9.6 把 flag API 换成"一个 flag 一个 setter" | `ui_common.h` 用 `LV_VERSION_CHECK` 包一层 `ui_obj_scrollable/clickable/hidden`，新旧版本都不告警。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |

### 触摸与空闲休眠

| 症状 | 根因 | 修法 |
|---|---|---|
| 插着充电器时屏幕不熄，或熄灭后立刻又亮 | 拿触摸 **INT 电平**当"有活动"。CST9217 会把 INT 拉住直到报告被读走，一旦卡住（充电噪声、熄屏后没人读），空闲计时被每 10 ms 刷新 | 唤醒只认 **高→低边沿**（且熄灭 >300 ms）；活动必须 **INT 低 + `lv_indev_get_state()==PRESSED`**。[§11](#11-电源管理power_mgrc) |
| 熄屏后点击没反应 | 同上：INT 脉冲被漏采 / 当时还在 dump 显存；旧代码没有"边沿 + 死区"的概念 | 边沿唤醒 + 300 ms 死区；抓图改成独立任务，不再占住 LVGL。[§11](#11-电源管理power_mgrc) |
| 「添加账号」「输密码」页进不去 | `build_add_screen()` 定义了却从没被调用，`s_scr_add` / `s_scr_pass` 恒为 NULL | 改成懒建：进屏时若为 NULL 先建再进。[§10](#10-ui一条线程一把锁一套排版规则uic--ui_commonc--ui_setupc) |

### 网页与认证

| 症状 | 根因 | 修法 |
|---|---|---|
| 浏览器一直「读取中」，`curl` 却正常 | `httpd_resp_send_chunk()` 分块发完没发结束块；`curl` 容忍"连接关闭即结束"，`fetch()` 会一直等 | 末尾补 `httpd_resp_send_chunk(req, NULL, 0)`。[§9](#9-局域网配置面板webc--indexhtml) |

### 电池与 PMIC

| 症状 | 根因 | 修法 |
|---|---|---|
| 98% 已停充，界面却写「充电中」 | 只看"USB 在位"，没看 AXP2101 的充电状态位（`0x01[2:0]`：0–3 充电 / 4 充满 / 5 未充电） | 分三档：充电中 / 已充满 / 已接电源。[§12](#12-电池状态batt_mgrc--axp2101) |
| 电量显示虚高，一拔线就掉一截 | 充电时 VBAT 被线损和极化抬高 | 查表前减 `APP_BAT_CHG_OFFSET_MV`（40 mV），且只在实际充电时减。[§12](#12-电池状态batt_mgrc--axp2101) |

### 构建、烧录与抓图工具

| 症状 | 根因 | 修法 |
|---|---|---|
| 仅采集串口日志，采集结束后却突然重启 | 实机读到 `ESP_RST_USB`；pySerial 默认 DTR/RTS 状态会使端口开关改变控制信号，仅跳过重启序列仍不够 | `monitor.py` 在打开端口前将 DTR/RTS 设为关闭；保持设备运行时使用 `--no-reset`。修正后连续开关串口三次，运行时间和界面计数均持续增长 |
| 烧完就挂，串口只有 `ets_loader.c 78` 然后反复复位 | 用了 `qio` 烧录，ROM 按 QIO 启动后在第一段加载后挂死 | `flash.ps1` 写死 `--flash-mode dio`。 |
| 烧完屏幕全黑、串口一个字都没有、网页也打不开 | 这块板 **EN 没接 USB 的 RTS**，`--after hard-reset` 是空操作，芯片停在 ROM 下载模式 | 写入用 `no-reset`，成功后再单独发 `watchdog-reset`；仍不行长按 PWR 冷启动。 |
| 旧串口抓图很慢、日志缺行或抓图干扰屏幕刷新 | 整屏像素先转十六进制，再经过无流控串口发送，既慢又可能丢行 | 已移除此抓图链路；`python tools/screenshots.py` 在电脑上运行同一份 UI 代码并直接生成 PNG，使用模拟数据 |
| 改了 UI 源码但文档截图仍是旧的 | 截图是生成文件，不会随源码自动变化 | 重新运行截图工具，统一写入仓库的 `docs/images/`；提交时一并包含源码、图片和文档 |
| 界面文字全是方块，但字体文件明明有中文 | `sdkconfig` 里 `CONFIG_LV_FONT_*` 没开，或生成字体的 `--bpp` 与 `LV_USE_FONT_COMPRESSED` 不匹配 | 对齐 `sdkconfig.defaults`；改字体参数后必须整机重编。 |

---

## 许可

MIT
