# EasyCoding Deck 设备固件

面向 **LILYGO T-Keyboard Voice V0.3 / ESP32-S3 N16R8** 的 Arduino / PlatformIO 固件，与 [EasyCoding Deck 桌面客户端](../pc/README.md) 配合使用。

## 模块与能力

- 五块 GC9107 128×128 非触摸按键屏，共享 SPI、独立片选。
- 五键短按、长按与按住事件，按键配置持久化到 NVS。
- ES7210 录音、ES8389 播放、设备音量与提示音。
- USB CDC JSONL 与局域网 WSS 传输。
- 14 颗 WS2812 状态灯、AXP2101 电源遥测、关机与深度休眠。
- LittleFS 形象存储，最多 14 个 RGB565 动作状态。
- 设备诊断、连接状态、队列与内存统计。

硬件和模块分工见 [设备架构](../docs/device-architecture.md)，消息格式与能力字段见 [protocol.md](protocol.md)。

## 源码结构

| 路径 | 内容 |
| --- | --- |
| `src/main.cpp` | 初始化、消息分发、Agent 状态与审批流程 |
| `src/board_runtime.cpp` | 五屏绘制、按键交互与形象播放 |
| `src/audio_service.cpp` | 采集、播放、提示音与传输归属 |
| `src/wifi_transport.cpp` | Wi-Fi、TLS、配对认证与 WSS 收发 |
| `src/protocol_transport.cpp` | 统一连接 owner、有界队列与消息发送 |
| `src/input_config.cpp` | 按键配置的校验与持久化 |
| `src/power_manager.cpp` | 电源、关机、睡眠与唤醒 |
| `include/` | 模块接口与引脚配置 |
| `boards/` | 板型定义 |
| `tests/` | transport 契约与 HMAC 向量 |
| `tools/` | 显式执行的串口实机检查 |

## 构建

从仓库根目录运行：

```sh
pio run -d firmware-s3 -e t_keyboard_voice
```

环境、工具链和依赖版本由 [platformio.ini](platformio.ini) 指定。pioarduino 平台使用固定 `55.03.311` GitHub release URL（Arduino ESP32 3.3.11），不从默认 PlatformIO registry 请求同名版本。USB monitor 为 **115200 baud**，upload speed 为 **921600 baud**。

Flash 布局见 [partitions.csv](partitions.csv)：16 MiB Flash 中分配两个 3 MiB 应用槽，LittleFS 约 9.88 MiB，用于保存形象及原子更新所需的临时文件。应用槽布局本身不代表已提供无线固件更新功能。刷写或更改分区前应备份数据，文件系统重新初始化后需再次同步形象。

## 屏幕与按键

- 屏幕 1 连续绘制当前宠物的 128×128 动画。
- 屏幕 2、4 显示按键与控制提示。
- 屏幕 3 显示语音发送结果和实时对话字幕。
- 屏幕 5 显示当前 Agent 与任务状态。

映射为 SEND/CANCEL 的屏幕优先显示对应动作。审批提示使用当前确认与返回键；仅在相关动作未绑定时使用 KEY1/KEY4。任务光效避让按键与录音提示，不覆盖必要反馈。

| 按键 | 短按 | 长按或按住 |
| --- | --- | --- |
| K1 | 确认 / 发送 | 按住说话，松开转写 |
| K2 | 上一个会话 | 可配置 |
| K3 | 下一个会话 | 可配置 |
| K4 | 返回 / 取消 | 按电源配置关机或深度休眠 |
| K5 | 循环宠物动作 | 开始 / 结束实时对话 |

K4 长按独立配置为 2～10 秒，并在达到时长且松开后执行；它不作为普通按键动作绑定。设备保存的配置优先于默认配置。

## USB 与 Wi-Fi

USB 使用一行一个 `{topic,payload}` 的 `pet-usb-jsonl-v4`。Wi-Fi 使用相同业务语义，每条 WebSocket text message 恰好包含一个完整 JSON envelope，wire protocol 为 `pet-wss-json-v1`。

首次配网和配对必须通过当前 USB owner 发起：

| 请求 | 结果 |
| --- | --- |
| `network/scan` | 通过 `network/scan-state`、`network/scan-result` 返回扫描状态和结果 |
| `network/config` | 写入 Wi-Fi 与配对凭据，以 `network/config-ack` 确认 |
| `network/status-query` | 返回不含密码或配对 secret 的网络状态 |
| `network/forget` | 清除 Wi-Fi 凭据、配对 secret 和设备 TLS identity |

设备在 8443 端口提供 `wss://<设备IP>:8443/device/v1`。每台设备生成独立 P-256 私钥与证书；客户端固定 USB 配置响应中的证书 SHA-256 指纹，并完成 HMAC challenge-response。认证字节格式、mDNS 和 subprotocol 常量见 [协议文档](protocol.md)。

只有认证成功的连接可以成为业务 owner。同一时刻仅有一个 owner；切换或断开连接会终止旧录音、播放、审批及未提交的形象事务，不会自动重放结果未知的操作。

## 音频

传输音频为 **16 kHz、单声道 PCM16**。ES7210 采集后进行通道选择、DC 去除与限幅；ES8389 使用固定小块将数据转换为 48 kHz 双声道输出。

实时对话使用半双工模式：播报期间暂停收音，播放结束后恢复。播放块严格检查序号、长度与会话归属；乱序、超长或资源不足会拒绝处理，不伪造成功回执。收发队列和播放缓冲均有固定上限。

## 非编译检查

从仓库根目录运行：

```sh
python -I firmware-s3/tests/test_transport_contract.py
```

测试覆盖源码契约、HMAC 向量与队列边界，不编译固件、不连接设备，也不替代网络、音频与功耗的实机验收。

## 实机检查

```powershell
.\firmware-s3\tools\s3_serial_smoke.ps1 -Port COM5
```

该脚本默认重新保存按键配置并将音量设为 75%；`-SkipConfig` 不跳过音量修改，`-PlayTone` 会播放音频。运行前须确认设备、端口与这些副作用。协议回执不等于真实听感或声学验收通过。
