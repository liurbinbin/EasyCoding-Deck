# README 五屏键盘演示 / Keyboard demo

中英文 README 共用 `readme-startup.gif`：仅含五屏键盘，1000×510、约 6.4 秒。虽然保留原文件名，当前内容已改为直接展示动画，不再重现启动阶段。

**屏幕 1 从第一帧直接播放用户指定的 `working.mp4`，不显示黑屏、HACHIMI 或其他初始化文字。** 其余屏幕维持确认、消息、返回及 Agent 状态，不包含桌面应用总览。

这是设备 UI 模拟，非实机录像或启动耗时测试；假定已同步所选形象，并使用桌面端默认键位（K2 确认、K4 返回）。它不表示刚刷写设备的默认画面，也未修改实际固件启动流程。

The shared GIF shows only the five-screen keyboard. Screen 1 plays the selected working animation immediately, with no dark-screen or boot-text prelude. The actual boot sequence is intentionally omitted. This is a UI simulation, not a hardware recording or timing benchmark.

## 素材来源与处理

- 实际输入：[`pc/public/builtin-appearances/c867abad-fabb-4704-b6ee-22d6fc0052fd/videos/working.mp4`](../pc/public/builtin-appearances/c867abad-fabb-4704-b6ee-22d6fc0052fd/videos/working.mp4)，即用户指定的现有视频。
- 资源记录：[`manifest.json`](../pc/public/builtin-appearances/c867abad-fabb-4704-b6ee-22d6fc0052fd/manifest.json)，形象名称为「你好世界」，类型为用户上传视频形象。生成器只读上述视频，不读取记录中的用户路径或 `videoUrl`。
- 原片为 480×480、10 fps、约 0.8 秒。等比缩小至 120×120，不拉伸或裁切；按原帧率循环八次，生成约 6.4 秒的整机预览。
- 使用现有 FFmpeg 转换为临时 GIF，再确定性解码其动画帧，逐帧合成到屏幕 1；GIF 第 0 帧就使用原视频首帧，没有初始化片头。
- 不下载外部素材、不重绘角色、不把第三方形象标为项目原创。素材的原始权利及许可不因转码而改变；参见 [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)。本次演示不等于完成独立素材许可审核。

The source is the user-selected `c867abad-fabb-4704-b6ee-22d6fc0052fd/videos/working.mp4`, recorded as an uploaded video for the “你好世界” avatar. It is scaled without stretching or cropping, then looped at the original 10 fps. Existing third-party rights remain applicable; this conversion is not a license audit.

## 显示边界

- 确认、消息、返回和 Agent 状态区域参考 `firmware-s3/src/board_runtime.cpp` 的 `renderStatus()`、`drawBoundActionKeys()` 的屏幕职责。
- 为直接展示动画效果，本预览不模拟固件初始化和连接时序。
- 设备实际读取已同步到 LittleFS 的 RGB565 帧，不直接解码 MP4 或 GIF。这里的 GIF 只是 README 展示格式，不是新增固件能力。

## 更新方式

需要仓库已有 Playwright、本机浏览器和 FFmpeg。从仓库根目录运行：

```sh
node pc/scripts/capture-readme-demo.mjs
```

- 模板：[`pc/tests/readme-demo.html`](../pc/tests/readme-demo.html)，只含五屏设备。直接打开可查看布局；尚未注入视频时显示明确占位，不请求网络。
- 生成器：[`pc/scripts/capture-readme-demo.mjs`](../pc/scripts/capture-readme-demo.mjs)，使用全新的 browser context 离线渲染，禁止 Service Worker 和 HTTP/WebSocket 请求，不访问 Tauri、设备、音频或私人配置。
- `--keep-frames` 保留临时 GIF、逐帧 PNG 与内嵌视频动画的 `demo.html`，脚本打印目录，可在本地浏览器查看。默认自动清理中间文件，只更新最终 GIF。
- `PET_TEST_BROWSER`、`PET_TEST_FFMPEG` 可指定已有工具绝对路径。缺工具或源视频时停止，不安装或下载替代品。完整生成、检查成功后才更新 GIF，不以旧输出冒充成功。

Regenerate using the command above. `--keep-frames` retains temporary animation frames and a standalone HTML preview. The generator uses only existing local tools, with no compilation, services, or hardware interaction.

## 非编译验证

```sh
node --check pc/scripts/capture-readme-demo.mjs
node pc/scripts/check-source-safety.mjs
git diff --check
```

生成器检查屏幕 1 从第 0 帧就有指定动画、所有帧均无初始化文字、动画具有不同帧、五屏数量与 K2/K4 映射、无布局溢出、零业务网络请求和零页面错误。导出后还需目视核对首帧及连续帧。

不运行 Vite、PlatformIO、Cargo 或 npm build，不连接设备、不刷写。上述检查不是实际应用或硬件验收。
