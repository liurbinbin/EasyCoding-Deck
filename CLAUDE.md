# CLAUDE.md

## 项目范围

- 主要子系统：
  - `pc/`：React/Vite 界面、Tauri/Rust 原生层、托管的 Node Agent bridge、形象与组件资源。
  - `firmware-s3/`：LILYGO T-Keyboard Voice V0.3 的 ESP32-S3/Arduino 固件。
  - `integrations/`：桌面应用的本机集成插件。
  - `server/community/web/`：独立或嵌入使用的社区静态前端。
  - `docs/`：设备架构、网络设计与验收记录。
- 对外名称不等于机器标识。协议字段、capability、环境变量、包 ID、持久化 key、系统凭据路径和第三方版权/来源信息必须保持真实、兼容，不做全局盲替换。
- `main` 可能比已发布安装包更新。不同子系统的版本号独立，不把单个 manifest 或示例视为全仓库版本。

## 工具与验证

- 桌面：Node.js 22+、Rust stable 和 Tauri 2 平台依赖；Windows 需要 MSVC C++ Build Tools 与 WebView2，macOS 需要 Xcode Command Line Tools。
- 设备：Arduino/PlatformIO，环境与依赖以 `firmware-s3/platformio.ini` 为准。
- 桌面固定 WASM 的显式重建使用 LLVM/LLD 20.1.8；普通开发无需重新生成已提交产物。
- 当前协作约束：编译由用户执行。只修改源码并运行非编译检查，不执行 PlatformIO、Cargo build/check/test、npm build 或其他编译命令。
- 仓库没有统一 lint/format 入口，也没有 `npm run lint`，不要编造。
- 源码检查、宿主构建、设备传输、实机性能和声学验证分别记录，未执行或跳过不等于通过。

### 非编译检查

从仓库根目录运行：

```sh
python -I firmware-s3/tests/test_transport_contract.py
node --test pc/src/dashboard/S3WifiPanel.test.js pc/src/s3-realtime-streaming.test.js
node pc/scripts/check-source-safety.mjs
node --test pc/scripts/check-source-safety.test.mjs
```

前端与 bridge Node 测试分别为 `npm --prefix pc test` 和 `npm --prefix pc run test:bridge`。单文件测试直接使用 `node --test`；`npm test -- <file>` 不会透传 Node test flags，runner 会将额外参数当作目录。

### 供用户执行的开发入口

在 `pc/` 中：

```sh
npm ci
npm run dev:web
npm run prepare:desktop
npm run build:win
npm run build:mac:local
```

`dev:web` 仅预览 UI。完整开发需要 `PET_MANAGER_ALLOW_DEV_ORIGIN=1 npm run dev`，会自动连接设备并启动本机 bridge；该变量不进入发布环境。资源准备不自动下载未知替代品，缺资源应停止。

固件构建从仓库根目录执行：

```sh
pio run -d firmware-s3 -e t_keyboard_voice
```

统一离线验收入口为 `node pc/scripts/test-framework-offline.mjs --output /absolute/evidence/directory`。它不联网或连接设备，但默认 Rust harness 涉及宿主编译，因此本会话不执行完整入口。

串口实机检查是 `firmware-s3/tools/s3_serial_smoke.ps1`，不是只读测试：默认重新保存按键配置并将音量设为 75%；`-SkipConfig` 不跳过音量修改，`-PlayTone` 会播放音频。实机、录音、播放、刷写和重置操作均需明确授权。

## 桌面架构

- `pc/index.html` → `src/main.jsx` → `src/App.jsx` 为 React 入口。DeviceContext 轮询/监听原生状态，UI 不另建串口或 WebSocket 状态机。
- Rust `UsbSerialManager` 持有设备身份、连接代次、boot、USB/WSS writer、ACK、音频与事务生命周期。React 只提交配网、连接、断开、忘记配对和业务意图。
- `src-tauri/src/main.rs` 调用 `lib.rs::run()`。新增 IPC 同步检查 handler、capability 和 `permissions/local-app-commands.json`；远程 community frame 不拥有本地主界面权限。
- Rust 监管 `src-tauri/bridge/packages/clawd-backend-service/src/headless-mqtt.js`。该 Node runtime 提供 hook HTTP 和内嵌 Agent Session Bus，不因文件名将当前同步架构描述为 MQTT。
- React 可直接访问 loopback Session Bus 的 status/sessions/events，再通过 IPC 将设备可见会话和绑定交给 Rust；另有 Node 原子状态快照到 Rust/USB 的路径，并非全部请求经过 React → Rust IPC。
- Agent 状态跟随、草稿注入、确认发送、切换桌面对话和工具审批是独立能力，不能相互推断。
- 宠物实时对话为设备采集 → USB/WSS → Rust ASR → 模型/受控工具 → TTS → 设备播放。当前设备采用半双工模式，不依赖普通按键语音或 Python sidecar。
- `service_bridge/` 是组件的受限服务组合层，不是 Node Session Bus；包声明不能扩大 IPC、路径或 URL 权限。

## 设备与连接边界

- 设备为五屏、五键、USB CDC、Wi-Fi/WSS 的 ESP32-S3 目标。协议、默认键位、电源行为和 LittleFS 以当前实现及 `firmware-s3/protocol.md` 为准。
- 功能依据 `hello` 返回的实际 capabilities 启用，不根据版本号、另一项 capability 或包 metadata 推断。
- 首次配网、配对和设备端凭据撤销使用精确 USB owner；WSS 固定通过 USB 获取的证书 SHA-256 指纹，再完成 nonce-bound HMAC。mDNS 和手动地址只是候选来源，不是认证。
- 同时只有一个业务 owner。连接切换创建新 generation，旧回执和异步任务不能污染新连接。
- 写操作绑定精确身份、连接代次和 requestId；超时/未知结果不当作未写入并重放，先只读核对事务状态。
- 音频、审批和形象事务不跨连接续传。S3 形象为 128×128 RGB565 状态动画，不直接解码 MP4。

## 组件与资源

- PetUI 是声明式、有界运行时，不是浏览器或任意脚本环境。契约入口为 `pc/skills/petui/SKILL.md`，实际设备能力优先于 package metadata。
- 内置组件在 `pc/builtin-clawpkgs/`，成员和顺序由 `pc/builtin-catalog.json` 决定。
- `pc/runtime-core/` 仅提供桌面预览的固定纯 C 核心；生成的 WASM 与适配器通过相应 `build-*-runtime.mjs` 显式重建，不直接手改生成字节。
- 组件变更同步检查 Rust admission、React 预览、schema、Python 工具和契约测试，不能只修改 UI 就声称设备支持。
- 原生资源、字体与许可材料见 `pc/docs/runtime-dependencies.md`；`.h264`、`.mjpg`、`.wav`、`.bin` 为普通 Git binary，不擅自改用 Git LFS。
- 保留第三方版权、适用授权、来源与 provenance。构建成功不代表许可审核完成。

## DSH、社区与安全

- DSH ↔ Cordis plugin ↔ PC loopback bridge，再由 PC ↔ 设备；设备不直接连接 DSH。
- `/inject` 的 terminal `admitted` 仅表示精确 session 队列接纳，不表示任务完成；cancel 同样只表示请求已提交。
- 审批只有一个 terminal answerer，不与原生审批界面竞速。输入目标不明确、窗口变化或写入结果未知时停止，不自行选历史会话或重发。
- `server/community/web/app.js` 的嵌入模式只接受精确 origin/channel 的 postMessage，独立模式请求 `/api/v1`。远程页面不能指定任意 Tauri command、路径或 URL，下载不自动同步到设备。
- Agent、Hook、Session Bus 与 DSH 服务必须保持 loopback；公共构建不得嵌入服务凭据，运行时密钥走现有安全存储路径。
- 修改带 `[Sync]` 头部的文件时同步指定的 `.folder.md` 与契约文档。

## 进一步阅读

- 根目录 `README.md`：产品与目录入口。
- `pc/README.md`：桌面开发、资源与离线验收。
- `firmware-s3/README.md`、`firmware-s3/protocol.md`：固件与协议。
- `docs/device-architecture.md`：模块、连接和设备边界。
- `pc/docs/agent-integration-audit.md`：Agent 能力与权限。
- `pc/docs/realtime-chat.md`：实时对话。
- `integrations/dsh-easycoding-deck/README.md`：DSH 插件契约。
