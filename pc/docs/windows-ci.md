# EasyCoding Deck Windows CI

## 运行与下载

打开仓库 **Actions → Windows desktop build → Run workflow**。`main` 上桌面、集成或本工作流相关变更也会自动触发构建。

工作流使用 `windows-2022` 的原生 x64 runner、Node 22、Rust stable 和现有 `npm run build:win` 入口，生成：

- NSIS `.exe` 安装器。
- `.msi` 安装包。
- `SHA256SUMS.txt` 校验文件。

成功后从该次运行的 **Artifacts → EasyCoding-Deck-Windows-x64-运行编号** 下载。产物保留 14 天，不自动发布 GitHub Release。EXE 和 MSI 是两种安装格式，选择其中一种，不要重复安装。

**当前没有配置 Authenticode 签名。** Windows SmartScreen 可能提示未知发布者；不建议关闭系统安全检查。正式签名需单独准备证书与安全的签名流程，不能把私钥或密码提交到仓库。

## 固定运行资源

干净 runner 上没有已审核的 FFmpeg。`.github/scripts/prepare-windows-runtime.py` 使用固定公开载体：

- 来源：`https://github.com/ai-maker-lab/HachimoDock/releases/download/v0.1.199/HachimiDock_0.1.199_Windows-x64-setup.exe`。
- 安装器 SHA-256：`273a78c0aa6a2cfaa06060b30e9a187f3fd0ff6d2f5113ccfe47ed356fd53b0d`。

先校验安装器整体哈希，再使用 runner 已安装的 7-Zip 列出内容，仅平铺提取五个唯一普通文件：`ffmpeg.exe`、`ffmpeg.README`、`ffmpeg.LICENSE`、`ffmpeg-8.1.2.tar.xz`、`zlib.LICENSE`。**不会运行来源安装器或来源应用，不复制其业务配置、素材或 Node。** 下载、输入目录相互隔离。

提取后核对现有 `pc/scripts/prepare-desktop-resources.mjs` 中 Windows x64 二进制精确哈希、FFmpeg 8.1.2 源码哈希、LGPL 构建声明和 zlib 许可归属。拒绝 GPL/nonfree、libx264 和 libopenh264 构建。Node 来自官方固定 SHA 的 `actions/setup-node`，bridge 依赖按已提交 lockfile 安装。构建前继续执行现有运行资源架构与编码器探测，不用未知替代品绕过门禁。

## 验证边界

编译只在 GitHub Actions 执行，不在 Windows 开发主机启动本机编译。离线资源导入测试：

```sh
python -I .github/scripts/test_windows_runtime.py
```

工作流还运行源码发布检查、前端与 bridge 测试。CI 构建成功不代表安装器交互、设备连接、录音、播放或声学验收通过。安装包成功上传与上述实机验证分别记录。
