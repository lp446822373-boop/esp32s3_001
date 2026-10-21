# 更新日志

## 未发布

### 新增

- 新增 I2C OLED（GM009605V4 / SSD1306 128×64）驱动与显示任务，SDA = GPIO8、SCL = GPIO9
- 常规页面显示 WiFi 连接状态、信号格、扫码枪通信状态、音频模块通信状态
- 扫码后显示「已扫码 / 正在上报」，随后整屏回显服务端返回结果：金额、员工、餐次、流水号、提示信息，5 秒后回到常规页面
- 待确认结果拿到最终状态后自动刷新屏幕提示
- GB2312 一级汉字 16×16 点阵与 8×16 ASCII 点阵字库
- 字库生成脚本 `tools/gen_font.py`

### 接口

- `wifi_prov_get_state()`、`wifi_prov_get_rssi()`、`wifi_prov_get_ssid()`
- `scanner_get_stats()`
- `player_get_last_reply_us()`
- `terminal_get_snapshot()`

### 配置

- 新增 `CANTEEN_DISPLAY_*` Kconfig 选项：屏开关、I2C 引脚、地址、行数、对比度、结果页停留时间、滚动间隔

### 说明

- 屏幕属于可选外设，初始化失败只记录日志，不影响扫码、联网和语音
- 字库覆盖 GB2312 一级汉字；生僻字（二级汉字）显示为空心方框，可扩展 `tools/gen_font.py` 的区段后重新生成
