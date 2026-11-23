# EasyCoding Deck macOS CI

## 使用

打开仓库的 **Actions → macOS desktop build → Run workflow**：

- `arm64`：Apple Silicon（M 系列），默认。
- `x64`：Intel Mac。
- `both`：分别生成两套原生安装包，不是 universal 安装包。

`main` 和 `ci/macos-build` 的相关代码 push 也会触发默认 arm64 构建。先运行源码与 Node 测试，再进入对应架构的 macOS 26 runner。不会在 Windows 主机执行 macOS 编译。

**当前 CI 测试包最低要求 macOS 26.0。** 固定发行资源的 Mach-O `VersionMin` 为 26.0，因此 `build:mac:ci` 使用独立配置明确设置应用最低系统版本。此限制不是只改版本号即可消除；支持更早系统需要另行审核对应的兼容运行资源。

成功后，在该次运行页面的 **Artifacts** 下载：

- `.app.zip`：保留应用包结构与执行权限的 ZIP。
- `.dmg`：安装镜像。
- `SHA256SUMS.txt`：产物校验值。

产物保留 14 天，不自动发布 GitHub Release。

## 必需运行资源

干净 runner 没有本机已经准备的 FFmpeg。构建必须提供与现有 `pc/scripts/prepare-desktop-resources.mjs` 哈希完全匹配的 macOS FFmpeg 8.1.2。不会改用 Homebrew FFmpeg、删除许可证检查或接受任意新二进制。

默认 Apple Silicon 构建无需手动上传资源：CI 使用固定来源镜像 `https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.199/HachimiDock_0.1.199_macOS-arm64.dmg`，首先核对镜像 SHA-256 `9e56fb8b9875c55830d347f28a4e9dafa5652a7872060640af6b37b028a630ef`，只读挂载后只复制 `tools/` 中的 FFmpeg、构建说明、LGPL 文本和对应源码。不会运行来源应用，也不复制其业务配置或素材。复制后仍须命中本项目现有的二进制与源码哈希、许可与可重定位性门禁；默认来源的固定版本和 hash 不随 latest 更新。

Intel 构建需要手动提供已审核资源。

### 固定签名版资源审阅

静态审阅记录：[macOS CI 44054811882](https://github.com/liurbinbin/EasyCoding-Deck/actions/runs/44054811882)。固定镜像提取的 FFmpeg 已经过发行签名，整体文件 SHA-256 与打包前的原始构建不同：

- 原始构建 hash `e340c148c720888a528e7bd2e7867da3b4bea91d1f6e508c9526111257ee0468` 继续保留。
- 单独接受的固定签名版 hash：`fbbccfb581309412c5f9353196e128741c3ad4fac229fd07061ddebb70107262`。
- 签名 Identifier：`ffmpeg`；Developer ID Team：`GGSR568VYA`。接受该文件时还会执行 `codesign --verify --strict`。
- 64-bit arm64 Mach-O；依赖仅位于 `/usr/lib/` 与 `/System/Library/Frameworks/`，无 Homebrew 路径。
- 对应源码 hash 匹配，构建说明为 FFmpeg 8.1.2、LGPL-2.1-or-later、静态构建，禁用 GPL/nonfree/network/autodetect，使用 Apple VideoToolbox；不包含 libx264 或 libopenh264。

这只增加一个经明确来源校验的签名版哈希，不接受任意重签名文件、不删除签名、不关闭来源、许可证、源码或编码器检查。

也可以为 arm64 覆盖默认来源；请在仓库 **Settings → Secrets and variables → Actions → Variables** 配置：

| 架构 | HTTPS 下载 URL | 对应压缩包的 SHA-256 |
| --- | --- | --- |
| Apple Silicon | `MACOS_RUNTIME_ARM64_URL` | `MACOS_RUNTIME_ARM64_SHA256` |
| Intel | `MACOS_RUNTIME_X64_URL` | `MACOS_RUNTIME_X64_SHA256` |

URL 指向你提供的、可公开下载的已审核资源包，例如本仓库 release asset。不在 URL 中嵌入凭据，不将 token 填入普通 Variables。单架构手动运行也可以填写 `runtime_bundle_url` 和 `runtime_bundle_sha256` 输入；`both` 必须使用各自架构的 Variables。

资源包为 `.tar.gz`，根目录只能有四个普通文件：

```text
ffmpeg
ffmpeg.README
ffmpeg.LICENSE
ffmpeg-8.1.2.tar.xz
```

其中：

- `ffmpeg` 必须为对应架构的已审核 Mach-O，hash 读取现有准备脚本的 `auditedFfmpegBinarySha256`。
- `ffmpeg.README` 保留完整构建参数，至少含 `--disable-gpl`、`--disable-nonfree`、`h264_videotoolbox`，不得含被禁止的 GPL/nonfree 编码器配置。
- `ffmpeg.LICENSE` 必须为对应 LGPL 文本。
- `ffmpeg-8.1.2.tar.xz` 必须匹配现有固定源码 hash。

在有这些**已审核文件**的机器上可以打包：

```sh
tar -czf runtime-macos-arm64.tar.gz ffmpeg ffmpeg.README ffmpeg.LICENSE ffmpeg-8.1.2.tar.xz
shasum -a 256 runtime-macos-arm64.tar.gz
```

上传前确认素材和软件再分发权限。若没有已审核的 macOS FFmpeg 及构建说明，需先取得这些资源；随意本机重编译的二进制通常不会命中当前 hash，不能冒充已审核构建。资源缺失时 CI 会在打包前明确失败，不生成不完整安装包。

Node 由官方 `actions/setup-node` 提供对应架构的 Node 22，并通过现有准备脚本检查可重定位性；bridge 使用已提交 lockfile 安装生产依赖。下载资源分别放在 runner 临时目录的独立 download/input 子目录，只提取白名单普通文件，不从解压目录执行脚本。

## 签名与验证等级

当前调用 `npm run build:mac:ci`：显式将必需的 `CARGO_BUILD_TARGET` 传给 Tauri `--target`，确保编译、签名和产物收集使用同一个架构目录，并开启详细打包日志。构建 `.app` 和 `.dmg`，执行现有 ad-hoc 签名、稳定 bundle-id designated requirement 及 DMG 内容核验。CI 专用配置 `src-tauri/tauri.macos-ci.conf.json` 将最低系统版本明确设为 26.0，其他本机构建入口保持原样。

**ad-hoc 签名不是 Apple Developer ID 签名，也没有 Apple 公证。** 首次打开可能受到 Gatekeeper 限制；不保证直接双击即可运行，不建议关闭系统安全检查。正式对外分发需另行配置 Developer ID 与公证，证书和密码必须使用 GitHub Secrets，不能提交到仓库。

CI 构建成功只证明对应 runner 上构建、资源准备与打包检查完成，不代表设备连接、录音、播放或声学验收通过。

## 本地非编译检查

```sh
python -I .github/scripts/test_macos_runtime.py
node pc/scripts/check-source-safety.mjs
```
