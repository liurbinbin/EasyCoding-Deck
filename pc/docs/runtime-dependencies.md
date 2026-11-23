# EasyCoding Deck 原生运行资源

完整桌面开发和安装包构建需要目标平台的 Node.js、FFmpeg、bridge 生产依赖及内置媒体资源。`npm ci` 仅准备前端依赖，不代表这些原生资源已经就绪。

## Node.js

使用对应系统与架构的官方可重定位发行包，通过 `PET_MANAGER_NODE_BIN` 指定可执行文件。用于 macOS 分发的 Node 不应依赖 Homebrew 等构建机器路径。

准备脚本检查可执行格式、架构及 macOS 动态库依赖，并将资源暂存到 `src-tauri/generated-runtime/`。

## FFmpeg

使用经过审核的 **FFmpeg 8.1.2** 独立可执行文件，以及与其对应的构建说明、LGPL 文本和源码压缩包。准备脚本按目标平台核对版本、架构、来源与固定 SHA-256，不会下载未知替代品。

源码来源：[FFmpeg 8.1.2](https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz)。

| 环境变量 | 输入文件 |
| --- | --- |
| `PET_MANAGER_NODE_BIN` | 目标平台的官方 Node 可执行文件 |
| `PET_MANAGER_FFMPEG_BIN` | `ffmpeg` 或 `ffmpeg.exe` |
| `PET_MANAGER_FFMPEG_README` | `ffmpeg.README`，包含完整构建参数 |
| `PET_MANAGER_FFMPEG_LICENSE` | `ffmpeg.LICENSE` |
| `PET_MANAGER_FFMPEG_SOURCE_ARCHIVE` | 与运行资源匹配的 FFmpeg 8.1.2 源码压缩包 |
| `PET_MANAGER_ZLIB_LICENSE` | Windows 资源所需的 `zlib.LICENSE` |

准备后的 FFmpeg、声明与源码安装到应用资源的 `tools/` 目录。macOS 应用中的路径为 `EasyCoding Deck.app/Contents/Resources/tools/`。

显式重编译 FFmpeg 时，使用审核过的完整构建参数和对应源码，并在审查后同步更新 `scripts/prepare-desktop-resources.mjs` 中的目标哈希。不要删除来源、许可或哈希校验。

## Bridge 与媒体

bridge 生产依赖以 `src-tauri/bridge/packages/clawd-backend-service/package-lock.json` 为准。资源准备只检查本地已有依赖，不自动执行在线安装。

内置组件和形象的顺序、路径与元数据在 `builtin-catalog.json`；媒体文件位于 `public/`。生成的运行资源暂存在 `src-tauri/generated-runtime/`，无需加入版本控制。

在 `pc/` 中执行：

```sh
npm run prepare:desktop
```

资源缺失或不符合目标平台时，预检会停止。编译客户端不需要服务 Key；语音识别、实时对话和形象生成所用的运行时凭据由用户在设置中配置。

## 桌面预览核心与字体

`runtime-core/` 提供桌面组件预览使用的固定、有界 C 核心。普通开发直接使用已提交的 WASM；显式重建要求 LLVM/LLD **20.1.8** 和配套 wasm-ld。

字体素材生成器读取 `PET_MANAGER_FONT_ASSET_DIR`。该目录须包含：

```text
SourceHanSansSC-Normal.otf
font_license/SourceHanSansSC/LICENSE.txt
```

字体和授权来源由生成者审核；普通开发无需重新生成现有图集。

## 分发声明

项目许可与第三方说明分别见根目录 `LICENSE`、`THIRD_PARTY_NOTICES.md`。组件包自己的 `LICENSE.txt`、`THIRD_PARTY_NOTICES.txt` 和 `licenses/` 文件随包保留；FFmpeg 的 LGPL 文本、对应源码与来源校验是独立运行资源的一部分。

发行者须审核并提供所需版权声明、许可证与对应源码，不能将资源准备成功视为许可审核完成。

电脑状态组件使用 `sysinfo`，仅启用 system/network 功能，刷新 CPU、内存和网络计数，不枚举进程、用户名、主机名或文件。其 MIT 声明保存在相关组件包中，例如 `builtin-clawpkgs/computer-status/licenses/sysinfo-LICENSE.txt`。
