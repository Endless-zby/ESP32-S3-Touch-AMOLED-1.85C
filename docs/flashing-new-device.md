# 新设备烧录指南

> 给一台全新的 Waveshare **ESP32-S3-Touch-LCD-1.85C（V2）** 刷上本仓库最新固件。
> 不需要安装 Arduino IDE，也不需要编译；仓库里直接带着编译好的镜像。

---

## 1. 你需要什么

| 项目 | 说明 |
|------|------|
| 板子 | ESP32-S3-Touch-LCD-1.85C **V2**（有 ES7210 回采芯片的那版） |
| 线 | 一根 **能传数据的** USB-C 线（很多充电线没有数据线，插上电脑不出串口） |
| 电脑 | macOS / Linux / Windows 均可 |
| esptool | 任选其一：`pip3 install esptool`；或已装 Arduino IDE + esp32 core（自带） |
| 本仓库 | `git clone git@github.com:Endless-zby/ESP32-S3-Touch-AMOLED-1.85C.git`，或网页下载 zip |

驱动：这块板走 ESP32-S3 原生 USB，macOS / Linux 免驱；Windows 10/11 一般也免驱，若设备管理器有黄叹号再装 Espressif 的 USB 驱动。

---

## 2. 仓库里的镜像在哪

```
build/
  aicc_phone.ino.bootloader.bin   bootloader            → 0x0
  aicc_phone.ino.partitions.bin   分区表                 → 0x8000
  boot_app0.bin                   OTA 启动槽初始数据       → 0xe000
  aicc_phone.ino.bin              应用固件（约 2.4 MB）    → 0x10000
  aicc_phone.ino.merged.bin       上面四个合成的整片镜像   → 0x0（备用，见 5.2）
tools/srmodels/
  srmodels_cn.bin                 唤醒词模型包（3.25 MB）  → 0x410000
```

`build/` 里永远是 **master 最新提交对应的** 镜像（每次提交前重新编译）。
想知道这组镜像对应哪个版本：

```bash
git log -1 --format='%h %ad %s' --date=short -- build/aicc_phone.ino.bin
grep FW_VERSION aicc_phone/config.h
```

> 模型包很少变，但**新设备第一次必须烧**，否则串口会报 `[WAKE] ... error`，"你好小安"不起作用。

---

## 3. 连接板子

1. 用 USB-C 线连接板子与电脑（板上只有一个 USB-C 口）。
2. 正常情况下板子会自动出现一个串口：
   - macOS：`/dev/cu.usbmodemXXXX`
   - Linux：`/dev/ttyACM0`（第一次需要 `sudo usermod -aG dialout $USER` 后重新登录）
   - Windows：设备管理器里的 `COMx`
3. 如果没有出现串口（全新板子或上一版固件把 USB 配坏了）：**按住 BOOT 键不放，点一下 RESET，再松开 BOOT**，板子进入下载模式，串口就会出现。

---

## 4. 烧录（推荐）：一条命令

在仓库根目录执行，`--models` 让它连唤醒词模型一起写：

```bash
tools/flash_release.sh --models
```

脚本会自动找 esptool 和串口。也可以显式指定：

```bash
tools/flash_release.sh /dev/cu.usbmodem2101 --models
```

常用选项：

| 选项 | 作用 |
|------|------|
| `--models` | 同时写入唤醒词模型包（新设备必加；日常只升固件可不加） |
| `--erase` | 先整片擦除（会清掉 Wi-Fi / SIP / API Key 等设置；二手板或怀疑 flash 脏时用） |
| `--baud 460800` | 线太长或写入失败时降速 |

正常输出以 `Hash of data verified.` 结尾并自动复位。整个过程约 30–60 秒。

Windows 没有 bash 的话，直接用第 5.1 节的手动命令。

---

## 5. 手动烧录（不想用脚本 / Windows）

### 5.1 四个镜像 + 模型（等价于脚本）

```bash
esptool --chip esp32s3 --port <串口> --baud 921600 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0      build/aicc_phone.ino.bootloader.bin \
  0x8000   build/aicc_phone.ino.partitions.bin \
  0xe000   build/boot_app0.bin \
  0x10000  build/aicc_phone.ino.bin \
  0x410000 tools/srmodels/srmodels_cn.bin
```

Windows 把 `<串口>` 换成 `COM5` 之类；esptool 通过 `pip install esptool` 安装后命令名就是 `esptool`（老版本是 `esptool.py`）。

### 5.2 用合并镜像

`aicc_phone.ino.merged.bin` 已把 bootloader、分区表、boot_app0、应用拼成一个 16 MB 文件，从 0x0 写入即可；
**它不含模型包**，所以新设备仍要再写一次 0x410000：

```bash
esptool --chip esp32s3 --port <串口> --baud 921600 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 build/aicc_phone.ino.merged.bin \
  0x410000 tools/srmodels/srmodels_cn.bin
```

### 5.3 有 Arduino 环境的开发机

`tools/flash.sh <串口> [--models]` 走 arduino-cli 上传，效果相同；它依赖 Arduino IDE 自带的 arduino-cli 路径，只适合开发机。

---

## 6. 烧完以后：第一次配置

1. 板子复位后屏幕显示启动画面，随后进入**配网模式**，屏上显示热点名 `AICC-Phone-XXXX` 和密码 `12345678`。
2. 手机连这个热点，浏览器会自动弹出配网页（没弹就手动打开 `http://192.168.4.1`）。
3. 选择酒店/办公室的 **2.4 GHz** Wi-Fi，输入密码，提交。屏幕提示"配网成功"后进入主界面。
4. 在同一局域网的电脑/手机上打开 `http://<设备IP>/`（IP 在设备 **设置 → 设备信息** 里看），在「语音助手」卡片填入百炼 **API Key**，其余保持默认，保存。
5. 对设备说 **「你好小安」**，听到提示音并开口打招呼，就成功了。
6. 想测前台呼叫：说「帮我打给前台」，或拨号盘直接按绿色按钮（默认呼 `8888`）。

SIP 服务器、拨打号码、音量、麦克风等都在同一个网页里，默认值已对好现有 FreeSWITCH。

---

## 7. 确认烧上去的版本

- 屏幕：**设置 → 设备信息** 显示 `版本 1.0.0`。
- 串口：复位后第一行 `[APP] AICC Phone 1.0.0 (board V2) reset=…`。
  看串口可用 `python3 tools/serial_log.py <串口> 60 /tmp/boot.log`（打开串口会复位板子）。
- 网页：首页顶部显示固件版本。

---

## 8. 常见问题

| 现象 | 原因 / 处理 |
|------|-------------|
| 找不到串口 | 换一根带数据的 USB-C 线；按住 BOOT 点 RESET 进下载模式；Linux 加入 `dialout` 组 |
| `A fatal error occurred: Failed to connect` | 进下载模式后重试；换 USB 口；`--baud 460800` |
| 写到一半报错 / 校验失败 | 线不好或 USB hub 供电不足，直连电脑并降波特率 |
| 烧完白屏/黑屏不启动 | 多半是只烧了 `aicc_phone.ino.bin` 没烧 bootloader/分区表；按第 4 或 5.1 节完整烧一次 |
| 能对话但"你好小安"叫不醒，串口 `[WAKE] ... error` | 没烧模型包，补一条 `write_flash 0x410000 tools/srmodels/srmodels_cn.bin` |
| 配网后网页打不开 | 手机和设备不在同一网段；或设备连的是 5 GHz（它只支持 2.4 GHz） |
| 唤醒后提示"语音助手暂不可用" | 网页里还没填 API Key，或 Key 无效 |
| 设置项全部恢复默认 | 用了 `--erase`，属正常；重新配网即可 |

---

## 9. 开发者：更新仓库里的镜像

改完代码后在开发机执行：

```bash
tools/build.sh                 # 生成 build/*.bin
git add build/aicc_phone.ino.bin build/aicc_phone.ino.merged.bin \
        build/aicc_phone.ino.bootloader.bin build/aicc_phone.ino.partitions.bin build/boot_app0.bin
git commit -m "..." && git push
```

`.gitignore` 只放行这五个文件，`build/` 里的目标文件、`.elf`、`.map` 不会进仓库。
分区表或 bootloader 没动时，后三个文件内容不会变，git 不会产生新的差异。
