# EasyCoding Deck Third-Party Notices

EasyCoding Deck 的项目许可证只适用于相应自有材料，不改变第三方源码、依赖、驱动、字体或素材的原始许可。各源文件与依赖包中的版权、许可和免责声明继续适用。本文件须随源码及桌面安装包分发，不能用于将第三方代码重新标为本项目许可。

## Clawd on Desk — MIT License

- Upstream: <https://github.com/rullerzhou-afk/clawd-on-desk>
- Copyright (c) 2026 rullerzhou-afk
- Retained and modified MIT-era files, audited hashes, and license boundary commits are documented in [pc/docs/clawd-mit-provenance.md](pc/docs/clawd-mit-provenance.md).

EasyCoding Deck includes portions of Clawd on Desk published during the MIT period. The following upstream notice and permission grant apply to those portions, not to the project as a whole.

### MIT License

Copyright (c) 2026 rullerzhou-afk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Xiaomi MiLoCo

本项目的相关家居控制云协议实现参考 Xiaomi MiLoCo SDK `miloco-miot` 的协议代码，保留源文件中的 Copyright (C) 2025 Xiaomi Corporation 标识，不打包完整 Python SDK 或上游摄像头库。

- Upstream: <https://github.com/XiaoMi/xiaomi-miloco>
- License: Xiaomi Miloco License
- 本仓库保留的许可证副本：[Xiaomi-Miloco-LICENSE.md](pc/builtin-clawpkgs/wooden-fish/licenses/Xiaomi-Miloco-LICENSE.md)。

相关作品适用原始许可规定的使用与分发范围；致谢或非商业标识不替代额外授权，不授予第三方商标或商业使用权。用户账号及凭据不属于分发资源。

## FFmpeg 8.1.2 — LGPL-2.1-or-later

- Project: <https://ffmpeg.org/>
- Corresponding source: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- Source SHA-256: `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`
- License: GNU Lesser General Public License, version 2.1 or later.

桌面安装包使用独立调用的 FFmpeg 可执行文件。对应版本源码压缩包、LGPL 许可全文、来源信息和完整构建参数必须随安装包的 `tools/` 资源分发。具体输入、架构与哈希门禁见 [pc/docs/runtime-dependencies.md](pc/docs/runtime-dependencies.md)。不使用来源不明、GPL/nonfree 配置或缺少对应源码的二进制替代品。

## zlib — zlib License

- Project: <https://zlib.net/>
- Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler.

Windows FFmpeg 资源所包含的 zlib 继续适用其原始许可。打包时必须提供对应 `zlib.LICENSE`，不得移除或更改原始声明，不得误表来源；更改源码时须清楚标记。该软件按原始许可中的 as-is 条款提供。

## Node.js and desktop dependencies

Node.js 分发包的 Node.js 及第三方依赖适用其随包许可：<https://github.com/nodejs/node/blob/main/LICENSE>。React、Vite、Lucide、Tauri、Rust crates、bridge 的 `ws` 等依赖保持原始许可；准确版本以 `pc/package-lock.json`、bridge lockfile 与 `pc/src-tauri/Cargo.lock` 为准。发行者须保留随包版权与适用许可证，不能用本项目许可证覆盖这些权利。

### sysinfo — MIT

- Project: <https://github.com/GuillaumeGomez/sysinfo>
- Copyright (c) 2015 Guillaume Gomez.
- Preserved license: [sysinfo-LICENSE.txt](pc/builtin-clawpkgs/computer-status/licenses/sysinfo-LICENSE.txt).

该集成只启用 system/network 功能，不枚举进程或采集用户名、主机名。

### serialport — MPL-2.0

- Project: <https://github.com/serialport/serialport-rs>
- License: Mozilla Public License 2.0, <https://www.mozilla.org/MPL/2.0/>
- Versioned source: <https://crates.io/crates/serialport>，实际版本以 Cargo.lock 为准。

保留上游 MPL 声明；相应 covered library 的源码可按对应版本获得。项目自有条款不限制接收者依据 MPL 取得源码的权利。

### unescaper — MIT alternative

该间接依赖提供 GPL-3.0/MIT 双重许可；本项目选择其 MIT alternative，不改写上游 metadata 或另一许可的原文。原始版权和完整 MIT 文本随对应 crate 提供，实际版本以 Cargo.lock 为准：<https://crates.io/crates/unescaper>。

## ESP32-S3 firmware dependencies

ESP32-S3 固件使用 Espressif Arduino、ESP-IDF、pioarduino platform、ArduinoJson、LovyanGFX、SensorLib 及其依赖。版本与来源由 `firmware-s3/platformio.ini` 和对应依赖发行包确定；这些依赖保持各自原始许可，不改标为项目自有许可。

## Components, fonts, and media

组件包中的 `LICENSE.txt`、`THIRD_PARTY_NOTICES.txt` 与 `licenses/` 文件必须随包保留。具体素材和字体的授权范围以相应组件或资源目录的声明为准；下载或可公开访问不等于取得再分发权。

源码检查或构建成功不等于完成所有第三方软件、素材和字体的许可审核。发行者需按实际分发内容保留要求的版权、许可全文、来源和对应源码。
