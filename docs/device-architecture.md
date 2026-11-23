# EasyCoding Deck 设备架构

EasyCoding Deck 将电脑上的 Agent、语音与内容服务连接到五屏五键设备。客户端与设备通过 USB CDC 或认证 WSS 通信。

## 目录与模块

| 目录 | 职责 |
| --- | --- |
| `pc/src/` | 操作界面、状态镜像、形象管理与设置 |
| `pc/src-tauri/src/` | 设备连接 owner、USB/WSS、音频链路、凭据与 IPC |
| `pc/src-tauri/bridge/` | 本机 Agent 状态、会话接口与事件汇聚 |
| `firmware-s3/src/` | 消息分发、五屏绘制、输入、采播音频、网络与电源 |
| `firmware-s3/include/` | 模块接口与硬件引脚 |
| `integrations/` | 桌面应用的本机集成插件 |

```text
Agent / 桌面对话应用
          │ 状态、输入、审批
          ▼
React UI ── Tauri IPC ── Rust 设备与音频管理
                              ├── USB CDC
                              └── 认证 WSS
                                     │
                                     ▼
                          ESP32-S3 协议路由
                          ├── 五屏与形象
                          ├── 五键输入
                          ├── 录音与播放
                          ├── 状态灯
                          └── 电源与诊断
```

Node bridge 的 hook HTTP、Agent Session Bus 与 DSH 服务只监听 loopback。设备连接不会将这些服务开放到局域网。

## 硬件

设备使用 LILYGO T-Keyboard Voice V0.3、ESP32-S3 N16R8、16 MiB Flash 和 8 MiB OPI PSRAM。

- 显示：五块 128×128 GC9107，共享 SPI、独立 CS，无触摸输入。
- 输入：五个实体按键，K4 同时承担电源管理操作。
- 音频：ES7210 采集、ES8389 播放，USB/WSS 传输 16 kHz 单声道 PCM16。
- 电源与扩展：AXP2101、14 颗 WS2812、CN5 检测和 QWIIC 接口。

引脚以 `firmware-s3/include/pin_config.h` 和当前板型定义为准。

## 五屏与输入

| 屏幕 | 内容 |
| --- | --- |
| 1 | 宠物形象与动画 |
| 2 | 按键操作提示 |
| 3 | 语音反馈、消息与对话字幕 |
| 4 | 控制提示与返回/取消 |
| 5 | 当前 Agent 与任务状态 |

按键事件使用 `button.sw1..sw5.short_press/long_press/hold`。客户端保存动作映射，固件执行当前配置。确认与返回各对应一个物理短按键；审批状态消费这些按键，避免再次触发普通会话切换。

K4 长按由电源配置独占，可设置 2～10 秒的关机或深度休眠。达到时长后在按键松开时执行。

## 连接与认证

Rust 持有设备身份、连接代次、boot identity、native scope、writer 与 waiter。React 只提交配网、连接、断开和忘记配对意图，不直接建立 WebSocket 或保存配对 secret。

- USB 用于首次配网、配对与设备端凭据撤销。
- Wi-Fi 使用设备独立 TLS identity、USB 固定的证书指纹与 HMAC challenge-response。
- mDNS 和手动地址只产生候选，认证成功后才能接入业务。
- USB/WSS 共用 `{topic,payload}` 业务语义，连接切换创建新的 generation。
- 旧回执和异步结果不能影响新连接；结果未知的写操作不自动重放。

消息字段见 [设备协议](../firmware-s3/protocol.md)，Wi-Fi 实现与验收见 [传输记录](s3-wifi-transport-progress.md)。

## 语音与形象

按键语音将录音交给电脑识别，并写入用户当前选定的 Agent 草稿；发送需另行确认。实时对话通过电脑的 ASR、模型和 TTS 服务运行，设备负责字幕与播放。

对话为半双工：播放回复时暂停采集，播放完成后恢复。音频和形象事务绑定精确 owner；断线或切换时终止旧事务，不跨连接续传。

形象为 128×128 RGB565 帧，最多保存 14 个动作状态。LittleFS 使用临时写入和提交过程保留当前有效形象，K5 默认在设备本地循环动作。

## 验证

- 源码契约：HMAC、消息与队列边界、请求归属及生命周期。
- 构建：桌面平台类型接线、固件链接和资源预算。
- 实机：USB/WSS 切换、网络稳定性、按键、形象持久化、音频、功耗和睡眠唤醒。

三个证据等级分别记录。测试通过或协议回执不代表实机性能和声学验收完成。
