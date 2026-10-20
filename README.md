# esp32canteen / 01

基于 **ESP32-S3-DevKitC-1** 的音频播放控制项目：通过板载按键控制串口音频模块的播放、暂停和切歌，并用板载 RGB LED 做状态指示与上电自检；支持手机网页配网，自动连接家中的 2.4 GHz WiFi。

## 功能

- 上电 RGB 自检：红 → 绿 → 蓝 → 白，各 200 ms，之后由状态灯接管
- WiFi 配网：无凭证时开启开放热点 `ESP32-Config`，手机连接后访问 `http://192.168.4.1` 配置 WiFi（支持强制门户自动弹出、扫描周边 WiFi）
- 连接成功后保存凭证到 NVS 并关闭配网热点；重启后自动连接路由器，掉线自动重连
- 运行中长按 **BOOT** 键 3 秒：清除配网信息并重启，重新进入配网模式
- RGB 状态指示：配网中蓝色慢闪、连接中黄色、连接成功绿色、掉线橙色慢闪、错误红色快闪
- GPIO4 按键控制：
  - **短按**：播放 / 暂停切换
  - **长按 0.8 秒**：下一首
- GPIO5 音量按键：
  - **短按**：音量减 1 级
  - **长按 0.8 秒**：音量加 1 级；按住不放时每 200 ms 继续加 1 级
- 通过 UART0 与串口音频模块通信（115200 8N1），按模块协议组帧并校验
- 通过 UART1 接收扫码枪数据（115200 8N1），按行结束符或 100 ms 空闲分帧，以日志形式打印文本和十六进制
- 周期性查询播放器状态（每 2 秒），自动同步停止/播放/暂停状态
- 日志输出到 USB Serial/JTAG，不占用协议串口和扫码串口

## 硬件连接

| 功能 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| 板载 RGB LED | GPIO48 | DevKitC-1 **v1.0**；v1.1 为 GPIO38 |
| 播放按键 | GPIO4 | 内部上拉，低电平按下，另一端接 GND |
| 音量按键 | GPIO5 | 内部上拉，低电平按下，另一端接 GND |
| BOOT 按键 | GPIO0 | 板载 BOOT，低电平按下；运行中长按 3 秒清除配网信息 |
| 扫码枪 TX → ESP32 RX | GPIO18 | UART1 RX，115200 8N1 |
| ESP32 TX → 模块 RX | GPIO43 | UART0 TX |
| ESP32 RX ← 模块 TX | GPIO44 | UART0 RX |
| 电源 | 3V3 / 5V | 按模块要求供电 |
| 地 | GND | 必须共地 |

> 音频模块的 TX/RX 需要与 ESP32 **交叉连接**。
>
> 扫码枪只需将 **TX 接 GPIO18**（并共地）；本项目只接收扫码数据，不向扫码枪发送指令。
>
> 若扫码枪的结束符不是 CR/LF，程序会在串口空闲 100 ms 后把已收到的内容作为一帧输出。
>
> RGB 引脚可通过 [main/main.c](main/main.c) 中的 `RGB_LED_GPIO` 修改。v1.0 为 `48`，v1.1 为 `38`。

## 按键行为

### 播放按键（GPIO4）

| 当前状态 | 短按 | 长按 ≥ 0.8 s |
| --- | --- | --- |
| 播放中 | 暂停（`AA`） | 下一首（`AC`） |
| 暂停中 | 继续播放（`AA`） | 下一首（`AC`） |
| 停止 | 播放第 1 首（`A0 00 01`） | 下一首（`AC`） |

长按在按住达到阈值时立即触发一次，松开不会再触发短按。

### 音量按键（GPIO5）

| 操作 | 行为 |
| --- | --- |
| 短按 | 音量减 1 级（`AE`），最低 0 级不再变化 |
| 长按 ≥ 0.8 s | 音量加 1 级；按住期间每 200 ms 再加 1 级，最高 31 级 |

音量由音频模块掉电记忆，上电时通过 `C1` 查询与模块同步，调节时发送 `AE`。

## 扫码枪

扫码枪接 UART1（RX = GPIO18，115200 8N1）。**每次扫码只输出一条日志**，包含扫码序号、字节数、条码文本（不可打印字符显示为 `.`）和十六进制：

```
I (12345) scanner: scan #1 (13 bytes): 6901234567890 | 36 39 30 31 32 33 34 35 36 37 38 39 30
```

- 一帧以 `\r` 或 `\n` 结束；
- 若 100 ms 内没有新数据，已收到的内容也会作为一帧输出（兼容不带结束符的扫码枪）；
- 单帧超过 256 字节会截断并给出 `truncated` 告警；
- 十六进制超过 64 字节时只显示前 64 字节，以 `...` 结尾。

## WiFi 配网

### 首次配网

1. 上电后若 NVS 中没有 WiFi 凭证（或启动时连接路由器失败），设备开启开放热点 **ESP32-Config**（无密码）。
2. 手机连接该热点。iOS / Android 通常会自动弹出配网页；若没有弹出，请用浏览器打开 <http://192.168.4.1>。
3. 点击「扫描」选择周边 WiFi，或手动输入 SSID；输入密码（开放网络留空），点击「连接」。
4. 设备尝试连接路由器，最长等待 15 秒：
   - **成功**：页面提示连接成功，2 秒后自动关闭配网热点，设备以 STA 模式运行；
   - **失败**：页面显示具体原因（找不到 AP、密码错误、连接超时等），热点保持开启，可直接重试。

### 行为说明

- 仅支持 **2.4 GHz** WiFi；输入 5 GHz SSID 会提示找不到 AP。
- 凭证保存在 NVS 命名空间 `wifi_cfg`（键 `ssid` / `pass`），**只有连接成功后**才写入。
- 已保存凭证时上电先尝试连接；15 秒内失败则自动开启配网热点（旧凭证保留在表单中预填）。
- 连接期间使用 APSTA 模式，手机可以保持在配网热点上等待结果；成功后关闭热点。
- 运行中掉线每 5 秒自动重连。
- 配网热点运行期间通过 DNS 劫持 + HTTP 302 实现强制门户，任意域名都会跳转到配置页。
- 手机后台应用（微信 `mmtls`、各类上报 SDK 等）经 DNS 劫持发起的 GET 会跳转到配置页，其它 HTTP 方法返回 204；非 HTTP 数据（如 TLS 握手）会得到 400。配网期间 `httpd` 的 405/400 客户端告警被抑制，串口不会刷屏。

### 重新配网

设备运行中长按 **BOOT** 键 3 秒，串口日志会提示清除 WiFi 凭证；**松开 BOOT 后**设备自动重启并进入配网模式（等待松开是为了避免触发下载模式）。

> BOOT 是 ESP32-S3 的下载模式 strapping 引脚：**上电/复位瞬间按住会进入串口下载模式，应用不会启动**。请在上电完成后再长按。

### 状态灯

| 颜色 | 含义 |
| --- | --- |
| 蓝色慢闪 | 配网热点运行中 |
| 黄色常亮 | 正在连接路由器 |
| 绿色亮 1.5 s 后熄灭 | 连接成功 |
| 橙色慢闪 | 联网后掉线，正在重连 |
| 红色快闪 | 初始化失败或正在清除配置 |

## 软件架构

| 文件 | 职责 |
| --- | --- |
| [main/main.c](main/main.c) | 应用入口、按键/扫码枪/配网初始化、BOOT 复位手势 |
| [main/player.c](main/player.c) | UART0 初始化、协议组帧/校验、回包解析、播放状态与音量控制 |
| [main/player.h](main/player.h) | 播放器接口 |
| [main/button.c](main/button.c) | 多实例按键轮询、消抖、短按/长按/连发识别 |
| [main/button.h](main/button.h) | 按键接口 |
| [main/scanner.c](main/scanner.c) | UART1 扫码枪接收、分帧与日志输出 |
| [main/scanner.h](main/scanner.h) | 扫码枪接口 |
| [main/wifi_prov.c](main/wifi_prov.c) | WiFi 初始化、STA 连接/重连、SoftAP 配网、HTTP 服务、强制门户 DNS、NVS 凭证 |
| [main/wifi_prov.h](main/wifi_prov.h) | 配网接口 |
| [main/prov_web.h](main/prov_web.h) | 内嵌配网页、成功页、错误页 HTML |
| [main/status_led.c](main/status_led.c) | RGB 自检与状态灯任务 |
| [main/status_led.h](main/status_led.h) | 状态灯接口与颜色定义 |
| [main/idf_component.yml](main/idf_component.yml) | 组件依赖（`espressif/led_strip`） |
| [docs/audio-player.md](docs/audio-player.md) | 串口音频模块协议参考 |

## 环境要求

- ESP-IDF **v6.1**
- 目标芯片：**esp32s3**
- Python 依赖由 ESP-IDF 环境提供；`led_strip` 组件由组件管理器自动下载

## 构建与烧录

```bash
# 若尚未设置目标芯片
idf.py set-target esp32s3

idf.py build

# 通过板载 USB 原生口（USB Serial/JTAG）烧录并查看日志
idf.py -p COM8 flash monitor
```

说明：

- 端口号按实际设备修改；本项目日志控制台已配置为 **USB Serial/JTAG**（`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`），因此建议使用板子的 USB 原生口。
- 不要使用 UART 口查看日志：UART0 已用于连接音频模块，UART1 已用于扫码枪。扫码数据和系统日志都从 USB Serial/JTAG 输出。
- 首次构建需要联网下载 `espressif/led_strip` 组件。
- 首次生成 `sdkconfig` 时会应用 [sdkconfig.defaults](sdkconfig.defaults)，把应用分区切到 1500 KB（`Single factory app (large)`），以容纳 WiFi/HTTP 功能。

### 日志口说明（DevKitC-1 有两个 USB 口）

| 板子上的口 | 对应 | 能看到什么 |
| --- | --- | --- |
| `UART` | 板载 USB-UART 桥（如 COM9） | ROM 启动信息；UART0 上的音频协议二进制（以 `~` / `0x7E` 开头） |
| `USB` | ESP32-S3 原生 USB Serial/JTAG | 应用的 `I (...) player`、`I (...) scanner` 等日志 |

- 若只能看到 `ESP-ROM:...` 和 `~` 开头的二进制，说明连到了 `UART` 口；查看扫码日志需要把数据线接到 `USB` 口。
- UART 口上出现以 `~` 开头的乱码是正常的协议帧，不是程序故障。

## 目录结构

```
.
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── main.c
│   ├── player.c / player.h
│   ├── button.c / button.h
│   ├── scanner.c / scanner.h
│   ├── wifi_prov.c / wifi_prov.h
│   ├── prov_web.h
│   └── status_led.c / status_led.h
├── docs/
│   └── audio-player.md
├── sdkconfig.defaults
└── README.md
```

## 已知说明

- 播放器在**停止状态**下发送 `AA`（暂停/继续）无效，本项目会改为发送 `A0 00 01` 播放第 1 首。
- 音频模块的播放模式（`AF`）掉电不记忆，上电恢复为单曲不循环。
- 音量（`AE`）由音频模块掉电记忆；若模块无应答，程序按默认 15 级处理。
- 扫码枪只接 RX（GPIO18）；若实际接线使用其他引脚，修改 [main/main.c](main/main.c) 中的 `SCANNER_RX_GPIO`。
- 配网热点为**开放网络**（无密码），仅在未配置或连接失败时开启，配网完成后自动关闭。
- 配网和联网使用 WiFi 射频，与 UART0/UART1、RGB LED 无引脚冲突；WiFi 发射瞬间电流较大，USB 供电较弱时建议增加储能电容。
- 若设备已经联网但需要更换 WiFi，长按 **BOOT** 3 秒清除凭证并重启，无需拆机或重新烧录。
- 若更换开发板版本，需要确认 RGB LED 引脚（v1.0 = GPIO48，v1.1 = GPIO38）。

## 相关文档

- [串口音频模块协议参考](docs/audio-player.md)
- [ESP32-S3-DevKitC-1 用户指南](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)
