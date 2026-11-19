<div align="center">
  <h1>EasyCoding Deck</h1>
  <p><strong>五屏五键的 AI Agent 桌面交互终端</strong></p>
  <p>通过 USB 或安全 Wi-Fi 连接电脑，在桌面小屏上查看任务状态、使用按键语音，并与自己的桌面伙伴交谈。</p>
  <p><a href="README.en.md">English</a> · <a href="#快速开始">快速开始</a> · <a href="pc/README.md">客户端开发</a> · <a href="firmware-s3/README.md">设备固件</a></p>
</div>

## 项目简介

**EasyCoding Deck** 由桌面客户端和 ESP32-S3 设备固件组成，硬件使用 **LILYGO T-Keyboard Voice V0.3**。电脑负责 Agent 接入、语音识别、模型对话与语音合成；设备负责五屏显示、按键、音频采播、状态灯与电源管理。

## 主要功能

| 功能 | 使用方式 |
| --- | --- |
| Agent 状态 | 查看工作、等待、完成与错误状态，通过宠物动作、屏幕光效和状态灯获得反馈。 |
| 五键交互 | 配置短按、长按和按住动作，用于会话切换、确认、取消与语音操作。 |
| 按键语音输入 | 按住语音键录音，松开后由电脑转写到目标 Agent 草稿，单独确认后发送。 |
| 实时对话 | 通过电脑的 ASR、对话模型和 TTS 服务交谈，在设备查看字幕、收听回复。 |
| 自定义形象 | 将素材转换为 128×128 RGB565 动画，最多保存 14 个动作状态，断电后保留。 |
| 审批交互 | 对已适配的 Agent 请求显示 ALLOW/DENY，使用确认与返回键提交决定。 |
| USB / Wi-Fi | USB CDC 支持有线连接和首次配网；配对后可通过局域网 WSS 传输状态、输入、音频与形象。 |
| 电源与诊断 | 查看电池、充电和连接状态，调整音量，设置关机或深度休眠。 |

Agent 的状态跟随、草稿写入、发送确认和工具审批是独立能力，支持情况见 [Agent 接入矩阵](pc/docs/agent-integration-audit.md)。设备功能依据 `hello` 返回的实际 capabilities 启用。

## 硬件

| 模块 | 配置 |
| --- | --- |
| 主控 | ESP32-S3 N16R8，240 MHz |
| 存储 | 16 MiB Flash、8 MiB OPI PSRAM |
| 显示 | 5 块 GC9107，单屏 128×128，共享 SPI、独立片选 |
| 输入 | 5 个实体按键 |
| 录音 | ES7210，默认 MIC1 + MIC2；检测到 AUX 时使用第三路采集 |
| 播放 | ES8389；16 kHz 单声道 PCM16 输入，48 kHz 双声道输出 |
| 状态灯 | 14 颗 WS2812 |
| 电源 | AXP2101，支持电池与充电遥测、关机和深度休眠 |
| 通信 | 原生 USB CDC、2.4 GHz Wi-Fi / WSS |

板型与引脚见 [board 定义](firmware-s3/boards/t_keyboard_voice_esp32s3.json)和 [pin_config.h](firmware-s3/include/pin_config.h)。

### 五屏布局

1. **屏幕 1**：宠物形象与动作动画。
2. **屏幕 2**：当前按键操作提示。
3. **屏幕 3**：消息、语音发送反馈与实时对话字幕。
4. **屏幕 4**：控制提示，默认用于返回与取消。
5. **屏幕 5**：当前 Agent 与任务状态。

屏幕内容随按键配置和交互状态变化。审批时，ALLOW/DENY 显示在当前确认与返回键对应的屏幕上。

## 快速开始

### 1. 通过 USB 连接

1. 准备配套的 EasyCoding Deck 客户端与设备固件，使用可传输数据的 USB 线连接设备。
2. 启动完整桌面应用，在设备首页确认设备已连接，检查固件版本和音频状态。
3. 选择要跟随的 Agent。需要语音输入时，打开目标对话，并授予必要的系统辅助功能权限。
4. 在设置中填写语音与对话服务凭据，在形象页面配置形象和声音，再同步到设备。
5. 验证按键、状态显示和短句语音，按需配置 Wi-Fi。

服务 API Key 由用户自行配置，使用语音、模型或内容生成服务可能产生费用。

### 2. 默认按键

设备保存的自定义映射优先；K1～K5 在协议中对应 SW1～SW5。

| 操作 | 默认行为 |
| --- | --- |
| K1 短按 | 确认 / 发送；有审批请求时允许 |
| 按住 K1，松开 | 按住约 500 ms 开始录音，松开后结束录音并交给电脑转写 |
| K2 短按 | 上一个会话 |
| K3 短按 | 下一个会话 |
| K4 短按 | 返回 / 取消；有审批请求时拒绝 |
| K4 长按后松开 | 默认达到 3 秒后关机，可设置为 2～10 秒或深度休眠 |
| K5 短按 | 循环当前形象的动作 |
| K5 长按后松开 | 开始 / 结束实时对话 |

K4 长按由电源管理独占。语音草稿的确认发送与 Agent 工具审批是两种不同操作。

### 3. 配置 Wi-Fi

1. 保持设备通过 USB 连接，在设备首页打开「S3 Wi-Fi 传输」。
2. 扫描周边网络或填写 SSID，输入 2.4 GHz 网络密码，选择「通过 USB 配网并配对」。
3. 确认电脑与设备可以在局域网互访，查看设备 IP、RSSI 与网络状态。
4. 选择已配对设备。mDNS 不可用时，可填写 `wss://<设备IP>:8443/device/v1`。
5. 点击「连接 Wi-Fi」，等待客户端显示连接已认证。移除 USB 数据线后，设备仍需电池或外部供电。

配对使用通过 USB 获取的证书 SHA-256 指纹和 HMAC-SHA256 challenge-response。网络发现不等于认证，不要关闭证书校验。Wi-Fi 连接仍需要电脑端客户端运行。

同一时刻只有一个业务连接 owner。切换连接或断线会终止当前录音、播放、审批和未提交的形象同步，不会跨连接续传或自动重放结果未知的写操作。清除设备端配网凭据需要通过 USB 执行「忘记配对」。

## 目录结构

```text
.
├── pc/                         桌面客户端、Agent bridge 与资源工具
│   ├── src/                    React 界面与前端测试
│   ├── src-tauri/              Rust 原生层与 Node bridge
│   ├── public/                 形象、音效和静态资源
│   ├── builtin-clawpkgs/       内置组件资源
│   ├── builtin-catalog.json    内置组件顺序与形象元数据
│   ├── runtime-core/           桌面预览的有界 C 核心
│   ├── scripts/                资源准备、打包和检查工具
│   ├── skills/                 组件创作工具与契约
│   └── tests/                  测试夹具与验收工具
├── firmware-s3/                ESP32-S3 设备固件
│   ├── src/                    显示、输入、音频、网络与电源实现
│   ├── include/                模块接口与引脚定义
│   ├── boards/                 板型配置
│   ├── tests/                  transport 契约测试
│   └── tools/                  串口实机检查工具
├── integrations/               桌面应用集成插件
├── server/community/web/       社区静态页面
├── docs/                       设备架构、网络设计与验收记录
└── assets/                     项目图片
```

React 提交操作并显示状态；Rust 管理设备身份、USB/WSS、连接代次、回执和音频生命周期；Node bridge 管理本机 Agent 接入。Agent Session Bus、hook HTTP 与 DSH 服务仅监听 loopback。

## 开发

### 桌面客户端

需要 Node.js 22+、Rust stable 和 Tauri 2 平台依赖。Windows 需要 MSVC C++ Build Tools 与 WebView2；macOS 需要 Xcode Command Line Tools。

```sh
cd pc
npm ci
npm run dev:web
```

`dev:web` 仅预览 UI。完整桌面开发需先准备可重定位 Node、审核过的 FFmpeg 和 bridge 依赖，见 [客户端开发说明](pc/README.md)与 [原生运行资源](pc/docs/runtime-dependencies.md)。

### 设备固件

从仓库根目录构建：

```sh
pio run -d firmware-s3 -e t_keyboard_voice
```

工具链和依赖锁定在 `firmware-s3/platformio.ini`。USB monitor 为 **115200 baud**，upload speed 为 **921600 baud**。设备 Flash 布局见 [partitions.csv](firmware-s3/partitions.csv)；刷写前备份所需数据。

### 非编译检查

从仓库根目录运行：

```sh
python -I firmware-s3/tests/test_transport_contract.py
node --test pc/src/dashboard/S3WifiPanel.test.js pc/src/s3-realtime-streaming.test.js
node pc/scripts/check-source-safety.mjs
```

这些检查不连接设备，也不代表固件构建、实机性能或声学验收通过。串口实机脚本的副作用与使用方式见 [固件说明](firmware-s3/README.md)。

## 常见问题

**不用 Wi-Fi 能用吗？** 可以，USB 支持完整的设备连接，也是首次配网的入口。

**无线连接后可以关闭客户端吗？** 实时状态、语音识别、模型对话和语音合成需要电脑端服务，客户端需保持运行。

**可以边播报边说话吗？** 当前对话使用半双工模式，播报期间暂停采集，回复结束后恢复收音。

**设备直接播放 MP4 吗？** 形象素材由电脑转换为 RGB565 动画帧后同步到设备，设备播放转换后的帧。

**搜到设备却连接不上？** 检查 USB 配对是否完成、局域网是否互通、8443 端口是否可达，以及认证阶段的状态。手动 IP 仅用于发现，不会绕过认证。

**怎样反馈问题？** 提供客户端与固件版本、操作系统、连接方式、复现步骤和脱敏错误。安全问题按 [SECURITY.md](SECURITY.md) 私下反馈，不要公开密钥、Wi-Fi 密码、配对 secret、录音或私人对话。

## 许可与致谢

项目许可见 [LICENSE](LICENSE)，第三方声明见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。感谢项目贡献者，以及 LILYGO、Espressif、Arduino、LovyanGFX、ArduinoJson、SensorLib、Tauri、React、Node.js、FFmpeg、Lucide 和 Clawd on Desk 等项目提供的硬件、工具与软件基础。
