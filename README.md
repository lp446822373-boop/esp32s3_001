# esp32canteen / 01

基于 **ESP32-S3-DevKitC-1** 的音频播放控制项目：通过板载按键控制串口音频模块的播放、暂停和切歌，并用板载 RGB LED 做上电自检指示。

## 功能

- 上电 RGB 自检：红 → 绿 → 蓝 → 白，各 200 ms，之后熄灭
- GPIO4 按键控制：
  - **短按**：播放 / 暂停切换
  - **长按 0.8 秒**：下一首
- 通过 UART0 与串口音频模块通信（115200 8N1），按模块协议组帧并校验
- 周期性查询播放器状态（每 2 秒），自动同步停止/播放/暂停状态
- 日志输出到 USB Serial/JTAG，不占用协议串口

## 硬件连接

| 功能 | ESP32-S3 引脚 | 说明 |
| --- | --- | --- |
| 板载 RGB LED | GPIO48 | DevKitC-1 **v1.0**；v1.1 为 GPIO38 |
| 播放按键 | GPIO4 | 内部上拉，低电平按下，另一端接 GND |
| ESP32 TX → 模块 RX | GPIO43 | UART0 TX |
| ESP32 RX ← 模块 TX | GPIO44 | UART0 RX |
| 电源 | 3V3 / 5V | 按模块要求供电 |
| 地 | GND | 必须共地 |

> 音频模块的 TX/RX 需要与 ESP32 **交叉连接**。
>
> RGB 引脚可通过 [main/main.c](main/main.c) 中的 `RGB_LED_GPIO` 修改。v1.0 为 `48`，v1.1 为 `38`。

## 按键行为

| 当前状态 | 短按 | 长按 ≥ 0.8 s |
| --- | --- | --- |
| 播放中 | 暂停（`AA`） | 下一首（`AC`） |
| 暂停中 | 继续播放（`AA`） | 下一首（`AC`） |
| 停止 | 播放第 1 首（`A0 00 01`） | 下一首（`AC`） |

长按在按住达到阈值时立即触发一次，松开不会再触发短按。

## 软件架构

| 文件 | 职责 |
| --- | --- |
| [main/main.c](main/main.c) | 应用入口、RGB 自检、模块与按键初始化、事件绑定 |
| [main/player.c](main/player.c) | UART0 初始化、协议组帧/校验、回包解析、播放状态机 |
| [main/player.h](main/player.h) | 播放器接口 |
| [main/button.c](main/button.c) | GPIO 按键轮询、消抖、短按/长按识别 |
| [main/button.h](main/button.h) | 按键接口 |
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
- 不要使用 UART 口查看日志：UART0 已用于连接音频模块。
- 首次构建需要联网下载 `espressif/led_strip` 组件。

## 目录结构

```
.
├── CMakeLists.txt
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml
│   ├── main.c
│   ├── player.c / player.h
│   └── button.c / button.h
├── docs/
│   └── audio-player.md
├── sdkconfig
└── README.md
```

## 已知说明

- 播放器在**停止状态**下发送 `AA`（暂停/继续）无效，本项目会改为发送 `A0 00 01` 播放第 1 首。
- 音频模块的播放模式（`AF`）掉电不记忆，上电恢复为单曲不循环。
- 若更换开发板版本，需要确认 RGB LED 引脚（v1.0 = GPIO48，v1.1 = GPIO38）。

## 相关文档

- [串口音频模块协议参考](docs/audio-player.md)
- [ESP32-S3-DevKitC-1 用户指南](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html)
