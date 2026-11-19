# EasyCoding Deck 桌面客户端

EasyCoding Deck 使用 React/Vite、Tauri 2、Rust 和 Node bridge，连接 ESP32-S3 设备，提供 Agent 状态跟随、按键语音输入、实时对话与形象管理。支持 macOS arm64 与 Windows x64。

客户端版本见 [package.json](package.json)，设备固件见 [firmware-s3](../firmware-s3/README.md)，各 Agent 的平台支持和权限要求见 [接入矩阵](docs/agent-integration-audit.md)。服务凭据由用户在设置中配置，不进入公开构建。

## 环境要求

- Node.js 22+、Rust stable 和 Tauri 2 对应的平台依赖。
- Windows：MSVC C++ Build Tools、WebView2。
- macOS：Xcode Command Line Tools。
- 原生运行资源：目标平台可重定位 Node、审核过的 FFmpeg 及对应声明和源码、bridge 生产依赖。

资源准备见 [runtime-dependencies.md](docs/runtime-dependencies.md)。素材通过普通 Git 保存，无需 Git LFS。

## 网页预览

在 `pc/` 中运行：

```sh
npm ci
npm run dev:web
```

网页预览仅用于 UI，不提供真实设备、系统权限或本机 Agent 功能。

## 完整桌面开发

先准备原生资源，再启动客户端：

```sh
npm run prepare:desktop
```

POSIX shell：

```sh
PET_MANAGER_ALLOW_DEV_ORIGIN=1 npm run dev
```

PowerShell：

```powershell
$env:PET_MANAGER_ALLOW_DEV_ORIGIN="1"
npm run dev
```

开发模式会自动连接设备并启动本机 bridge，不是无副作用的 UI 预览。`PET_MANAGER_ALLOW_DEV_ORIGIN` 仅用于本地开发，不应进入发布环境。

## 测试与检查

以下命令在 `pc/` 中运行：

```sh
npm test
npm run test:bridge
npm run build:web
cargo test --manifest-path src-tauri/Cargo.toml --lib
```

`npm test` 包含 provenance 检查和前端 Node 测试；`build:web` 会编译前端；Rust 测试也涉及编译。仅运行单个 Node 测试时，直接调用 `node --test`，不要把文件路径附加到 `npm test`。

从仓库根目录执行的非编译检查示例：

```sh
python -I firmware-s3/tests/test_transport_contract.py
node --test pc/src/dashboard/S3WifiPanel.test.js pc/src/s3-realtime-streaming.test.js
node pc/scripts/check-source-safety.mjs
node --test pc/scripts/check-source-safety.test.mjs
```

### 离线验收

在依赖已经就绪的环境中，从仓库根目录执行：

```sh
node pc/scripts/test-framework-offline.mjs --output /absolute/evidence/directory
```

默认阶段包括入口自测、前端和 bridge 测试、S3 transport unittest，以及 workflow、截图和表单模块的轻量 Rust harness。它不联网、安装依赖、连接设备或运行 ignored 实机用例，但 Rust harness 仍涉及宿主编译。

- `--browser --browser-executable /absolute/path/to/chromium`：增加本机浏览器合成 UI 检查。
- `--desktop-check`：增加离线完整桌面 Rust 类型检查，不验证安装包与签名。
- `--fail-fast`：首个阶段失败后停止后续阶段。
- `--node`、`--npm`、`--python`、`--cargo`、`--git`：指定已有工具。

输出目录中保存独立运行记录、各阶段日志、测试计数和源码摘要。缺依赖、跳过或忽略测试、结果不可解析以及运行期间源码变化均不会被记为通过。报告不表示实机性能、声学或发布就绪验收通过。

## 构建安装包

在 `pc/` 中运行：

```sh
# macOS arm64，本地 ad-hoc 签名与 DMG
npm run build:mac:local

# Windows x64，在 Windows 开发终端执行
npm run build:win
```

资源准备脚本不会联网补齐未知替代品。macOS ad-hoc 签名不等于 Developer ID 签名或公证；Windows 发布签名也需发行者自行配置。

## 目录导航

| 路径 | 内容 |
| --- | --- |
| `src/` | React 界面、设备状态镜像与前端测试 |
| `src-tauri/src/` | 设备连接、USB/WSS、音频、凭据和 Tauri IPC |
| `src-tauri/bridge/` | 托管的 Node Agent bridge 与 Session Bus |
| `src-tauri/permissions/`、`src-tauri/capabilities/` | 本地应用权限边界 |
| `public/` | 形象、音效和静态资源 |
| `builtin-clawpkgs/` | 内置组件资源 |
| `builtin-catalog.json` | 内置组件顺序和形象元数据 |
| `runtime-core/` | 桌面预览的有界 C 核心 |
| `scripts/` | 资源准备、打包、检查与显式生成工具 |
| `skills/petui/` | 组件创作工具、schema 和语义契约 |
| `tests/` | 静态契约夹具、浏览器与手动验收工具 |
| `docs/` | Agent、语音、资源与功能文档 |

## 组件预览与生成

桌面组件使用有界声明和固定运行时，不执行包内任意代码。预览资源的来源在 `runtime-core/`，已提交的 WASM 与适配器可直接用于普通开发；设备支持始终依据实际 capabilities 判断。

显式重建共享 WASM 时使用 LLVM/LLD **20.1.8**，通过 `CLANG` 指定 clang，并提供配套 wasm-ld。不要因编译器版本不同而重写产物以绕过字节一致性检查。

字体素材生成器通过 `PET_MANAGER_FONT_ASSET_DIR` 读取已审核字体及许可证目录。普通开发无需重新生成字体图集。

## 安全与许可

本机 Agent Session Bus、hook HTTP 与 DSH 服务只监听 loopback，不应通过代理或端口转发暴露。设备凭据、服务 Key 和私人对话不得进入公开日志或资源包。

项目许可见 [LICENSE](../LICENSE)，第三方声明见 [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)，安全反馈见 [SECURITY.md](../SECURITY.md)。
