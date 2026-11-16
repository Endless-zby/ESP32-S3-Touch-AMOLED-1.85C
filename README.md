# AICC Phone — ESP32-S3-Touch-LCD-1.85C SIP 话机固件

把 Waveshare **ESP32-S3-Touch-LCD-1.85C（V2，圆形 360×360 触摸屏）** 做成一台独立的 SIP 电话，
一键呼叫 [aicc（ai-native-callcenter）](https://github.com/rasonyang/ai-native-callcenter) 的 AI 客服。
基于 Arduino 框架（esp32 core 3.3.x）+ LVGL 9.3，SIP/RTP 协议栈为项目自带的精简实现，G.711（PCMU/PCMA）语音。

```
aicc_phone/      Arduino 工程（打开 aicc_phone.ino 即可）
tools/           编译 / 烧录 / 串口 / 字体生成脚本
reference/       Waveshare 官方驱动示例（只读参考）
build/           编译产物（tools/build.sh 生成）
```

## 功能

- **配网**：首次开机自动开启热点 `AICC-Phone-XXXX`（密码 `12345678`），手机连上后浏览器会自动弹出
  配网页（或手动打开 `http://192.168.4.1`），页面列出附近 2.4G Wi-Fi，选一个输入密码即可。
- **网页设置**：同一页面可修改 SIP 服务器、端口、拨打号码、用户名/密码、是否注册、音量、麦克风增益。
  配网成功后在局域网内也可以通过 `http://<设备IP>/` 打开。
- **拨号盘**：圆屏内的 12 键拨号盘；不输入号码直接按绿色按钮 = 呼叫预设号码（默认 `8888`）。
- **通话**：呼出 / 来电接听 / 挂断 / 静音 / 音量调节，显示通话计时与收发音量电平。
- **SIP**：直接 INVITE（FreeSWITCH 5080 外部口，免注册）或注册模式（5060，digest 认证）二选一；
  自动通过 OPTIONS 响应的 `received/rport` 学习公网地址以穿越 NAT；RTP 对称收发，服务器侧自动纠正媒体地址。
- **音频**：ES8311 + ES7210 + NS4150B 功放，16 kHz 采样、软件重采样到 8 kHz。
- **回声消除**：使用乐鑫 esp-sr 的 AEC（`libs/ESP_SR_AEC`，预编译库），默认参数与 xiaozhi-esp32 在本板上相同
  （全双工低功耗模式 + 最强 NLP）。V2 板的 ES7210 第 3 路麦克风输入在硬件上接的是 ES8311 的线路输出
  （原理图 "AEC" 区块），ES7210 按 esp_codec_dev 的寄存器序列配置成 4 通道 TDM 后，64 位 I2S 帧里的
  4 个 16 位槽位依次是 MIC1 / MIC3 / MIC2 / MIC4，槽位 1 就是回声参考。网页里可选 AEC 模式、麦克风/参考槽位、
  参考通道增益；串口每 2 秒打印各槽位电平，便于核对。

## 编译环境

机器上已经具备：Arduino IDE 2（自带 arduino-cli 1.5.1）、esp32 core 3.3.10、
`~/Documents/Arduino/libraries/lvgl`（9.3.0）以及与之并列的 `lv_conf.h`（工程里有同一份副本）、
`~/Documents/Arduino/libraries/ESP_SR_AEC`（与仓库 `libs/ESP_SR_AEC` 相同）。

### 用脚本（推荐）

```bash
tools/build.sh          # 编译，产物在 build/
tools/flash.sh          # 自动找 /dev/cu.usbmodem* 烧录；也可指定端口 tools/flash.sh /dev/cu.usbmodemXXXX
tools/monitor.sh        # 115200 串口日志
```

### 用 Arduino IDE

打开 `aicc_phone/aicc_phone.ino`，开发板选 **Waveshare ESP32-S3-Touch-LCD-1.85**，并设置：

| 选项 | 值 |
|---|---|
| PSRAM | Enabled |
| Flash Mode | QIO 80MHz |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |

## 烧录

1. Type-C 线连接电脑。若系统没识别出串口，按住 **BOOT** 再按一下 **RESET**，松开 BOOT 进入下载模式。
2. `tools/flash.sh`（或 IDE 上传）。
3. 烧录完成后按 RESET，屏幕显示启动界面。

## 使用流程

1. 第一次开机 → 屏幕显示“配网模式”和热点名称。
2. 手机连接该热点，配网页面自动弹出（iOS/Android 的“登录网络”页），选择家里的 2.4G Wi-Fi，输入密码，点“连接并保存”。
3. 页面会显示连接结果；成功后设备自动进入拨号界面，热点会在 45 秒后关闭。
4. 按绿色按钮即可呼叫预设号码；通话中可静音、调音量、挂断。
5. 齿轮按钮 → 查看 IP / 服务器 / 状态，“重新配网”会清除 Wi-Fi 并重启。

## 语音唤醒与命令词

- 唤醒词 **"你好小安"**：乐鑫 esp-sr 官方 WakeNet9 模型 `wn9_nihaoxiaoan_tts2`（免费商用），常开本地识别。
- 命令词：乐鑫 MultiNet7 中文模型，只在唤醒后的 8 秒窗口内运行；词表在 `tools/srmodels/src/fst/commands_cn.txt`，
  id 与 `wake_word.h` 的 `WAKE_CMD_*` 对应。
- 模型包：`tools/pack_models.sh` 把 `tools/srmodels/src/` 打成 `tools/srmodels/srmodels_cn.bin`（约 3.25 MB），
  烧录时 `tools/flash.sh --models` 写入 4 MB 的 `model` 分区（见 `aicc_phone/partitions.csv`）。
- 灵敏度：网页配置页"唤醒灵敏度"三档（标准 / 宽松 / 严格），对应 WakeNet 阈值 默认 / −0.05 / +0.01。
- 核心自带的 esp-sr 为 2.4.6，只支持 WakeNet9 系列模型（wn9 / wn9l / wn9s），不支持 wn10。

## SIP 对接说明（aicc / FreeSWITCH）

- **默认（直连）**：INVITE `sip:<号码>@<服务器>:5080`，不注册。这是 FreeSWITCH external 口，
  来话进入 `public` 上下文 → `aicc_inbound.lua` → 按 `luacc.dids` 表查 DID → 转给 AI 机器人。
  **被叫号码必须是 aicc 里配置好的 DID**，否则 FreeSWITCH 回 `404 Not Found`。
- **注册模式**：在网页里把“注册到服务器”改为“是”，端口改 5060，用户名/密码填 aicc 的分机或
  模拟客户号码（如 `13800000001` / `aicc@123`）。注册后一样可以拨 DID，还能接收平台的回呼。
- 编解码：只提供 PCMU/PCMA（8 kHz G.711），与 aicc 机器人侧一致。

## 代码结构

| 文件 | 作用 |
|---|---|
| `aicc_phone.ino` | 主流程：Wi-Fi/配网状态机、SIP 与 UI 的粘合、通话看门狗 |
| `sip.h/.cpp` | 精简 SIP UA：REGISTER/INVITE/ACK/CANCEL/BYE/OPTIONS、digest 认证、重传、来电处理 |
| `rtp.h/.cpp` | RTP 收发 + 抖动缓冲 + 简单丢包隐藏 |
| `g711.*` | μ-law / A-law 编解码 |
| `audio.*` | ES8311/ES7210/I2S 初始化、音频任务（采集→AEC→RTP、RTP→播放、铃声/回铃音、按键音） |
| `libs/ESP_SR_AEC` | esp-sr 回声消除预编译库（Arduino 库形式，build.sh 用 `--library` 引入） |
| `ui.*` / `ui_strings.h` | LVGL 界面（所有控件都在圆形范围内）；中文字符串集中在 `ui_strings.h` |
| `font_cn_18.c` / `font_cn_24.c` | 自动生成的中文字体（思源黑体子集 + ASCII + LVGL 图标） |
| `web_portal.*` | 热点 + DNS 劫持 + 配网/设置网页 |
| `es7210_board.*` | ES7210 四通道 ADC 初始化（复刻 esp_codec_dev 的寄存器序列，TDM + 回声参考通道） |
| `es8311.*` | ES8311 解码器驱动（Waveshare 官方示例） |
| `display.*` / `esp_lcd_st77916.*` | ST77916 QSPI 面板驱动（来自 Waveshare 官方示例） |
| `touch.*` / `board_io.*` | CST816 触摸、TCA9554 扩展 IO、背光、I2C |
| `lvgl_port.*` | LVGL 显示/触摸接入 |
| `settings.*` | NVS 持久化配置 |
| `config.h` | 引脚与默认参数（板型 V1/V2 开关 `BOARD_V2`） |

新增界面文字后运行 `tools/gen_fonts.sh` 重新生成字体（需要 node/npx，使用 `tools/fonts/` 里的思源黑体）。

## 已知限制 / 后续可做

- AEC 的 CPU 开销尚未在真机测量：串口每 5 秒打印一行 `[AEC] avg ... us / chunk`，若平均值接近
  chunk 时长（chunk=256 时为 16 ms）就在网页里切到"低功耗"模式。AEC 关闭时退回半双工抑制。
- 未实现 DTMF（RFC 2833）发送、保持、转接。
- V1 板（PCM5101 + 数字麦）代码路径保留但未测试（`config.h` 里 `BOARD_V2 0`）。
- 仅 UDP 传输，不支持 TLS/SRTP。
