# 本地生成 UI 截图

从仓库根目录执行：

```powershell
python -m pip install -r tools/screenshots/requirements.txt
python tools/screenshots.py
```

输出直接替换 `docs/images/` 中的十一张 PNG：主页、缺少认证账号主页、WiFi 列表、WiFi 扫描中、
WiFi 密码输入、账号管理、添加账号、网页已解锁、网页验证码入口、删除 WiFi 确认、删除账号确认。
版本由根目录 `VERSION` 维护，截图工具不另设项目版本。

## 依赖

- Python、Pillow（上面的 requirements 文件）。
- GCC / G++、CMake、Ninja。Windows 可用 MinGW-w64 的 `gcc` / `g++`，
  CMake 和 Ninja 可复用 ESP-IDF 安装附带的工具；加入 `PATH` 后可直接运行。
  驱动优先从 `PATH` 查找，也支持在 `IDF_TOOLS_PATH` 下查找 CMake / Ninja。
- 与固件一致的 LVGL 源码。默认复用 `managed_components/lvgl__lvgl/`；
  首次拉取项目时可在 ESP-IDF 环境中执行 `idf.py reconfigure` 下载组件，不需要烧录。
- 网页截图使用 Node.js / npm、Chrome 和 `@playwright/cli`。工具通过 `npx`
  调用 CLI，首次使用可能下载 npm 包；浏览器默认选择已安装的 Chrome。

```powershell
python tools/screenshots.py --device-only
python tools/screenshots.py --web-only
python tools/screenshots.py --lvgl-path "D:/path/to/lvgl"
```

所有工作目录、PPM 中间图和日志都在 `.local/screenshots/`，浏览器诊断输出在
`output/playwright/`。首次生成需要编译主机版 LVGL；后续由 CMake / Ninja 增量构建。
无需 ESP32、串口、SD 卡或网络认证，也不会修改开发板配置。

## 如何保证与当前界面一致

设备截图执行原有 `main/ui.c`、`main/ui_setup.c`、`main/ui_common.c`，
使用同一份 `main/ui_font_18.c` 和 LVGL 控件，不用另一套 HTML / 绘图代码仿制设备页面。
`render.c` 仅为这些代码提供模拟硬件接口与演示数据，并按顺序打开真实页面。
渲染采用 480×480 离屏快照，在电脑内存中直接转 PNG，没有串口十六进制传输。

WiFi 扫描任务在模拟器中先挂起，完成后执行真实扫描结果处理，因此能够分别截图
“已完成”和“扫描中”，并检查相同结果没有替换原列表控件。
两种删除确认框复用真实按钮回调，同时验证取消不删除、确认后才删除、目标独立复制、
重复确认不重复删除，以及当前联网 WiFi 的原有删除保护。
主页遍历全部 11 种状态，验证英文状态统一为 40 号字体且完整显示，
包括完整名称 `NO ACCOUNT`；同时检查中文说明、电池及验证码的矩形范围没有交叠。
主机不模拟硬件触摸、QSPI、DMA、WiFi 时序或真实电池测量；屏幕实际效果仍需板上验证。

网页截图打开原始 `main/index.html`；Python 仅在 `127.0.0.1` 临时提供演示 API。
`web.js` 通过真实验证码表单解锁，然后分别截取两种页面状态。

## 演示数据和隐私

SSID 使用 `ExampleWiFi`、`LibraryDemo`、`LabDemo`、`GuestDemo`；账号是
`demo-user-01` 等示例字符串；IP 使用文档示例地址 `192.0.2.10`，
验证码固定为 `123456`。密码、电池、运行时间与认证次数也都是编造的数据。
工具没有读取真实配置、串口、浏览器个人资料或设备接口的路径。

要更换演示数据，修改 `render.c` 和 `tools/screenshots.py` 中的固定数据即可。
请继续使用示例参数。旧串口导出、还原、像素化遮挡工具以及固件内对应的慢速抓图模式已移除。
