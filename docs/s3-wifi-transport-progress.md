# EasyCoding Deck Wi-Fi 传输与验收

> 状态：最新 USB/WSS 音频与独立 DMA 任务修复已完成源码及非编译契约检查；历史版本曾构建通过，最新固件和 PC 均未编译，由用户验证；实机网络、频率、音频、功耗和声学验收待执行
> 分支：`feat/s3-wifi-transport`  
> 基线提交：`4c30d64`（`chore: snapshot current workspace baseline`）  
> 建立日期：2026年11月12日  
> 目标验收日期：2026年11月13日

本文档是本任务的唯一进度账本。每完成一个可审查步骤，就更新对应复选框、证据和偏差说明；代码实现、测试结果与未完成风险不得只留在会话中。

## 1. 目标与边界

### 目标

- 保留 ESP32-S3 T-Keyboard Voice 的现有 USB CDC/JSONL 行为。
- 增加由 PC 原生 Rust 层管理的 Wi-Fi WebSocket/JSONL 设备 transport。
- 通过 USB 完成首次 Wi-Fi 配网和显式配对。
- USB 与 Wi-Fi 共享设备协议、连接代次、请求归属、音频和审批生命周期。
- 任一时刻只有一个经过准入的业务连接 owner；切换 transport 不重放未知结果的副作用请求。
- Wi-Fi 控制、输入事件、状态、上行录音、下行播放和 S3 形象事务具备明确的 owner、断线和资源准入语义。

### 本轮不做

- 不把 Agent Session Bus、hook HTTP、DSH 或其他 PC loopback 服务暴露到局域网。
- 不让 S3 直接连接 Codex、Claude、DSH、云模型或任意互联网服务。
- 不实现 BLE 音频、SoftAP captive portal 或无线 OTA。
- 不跨 transport 热迁移录音、播放、审批或形象写入事务。
- 不将 `boardDeviceId`、`debugBootId`、FNV checksum 或 mDNS TXT 当作认证凭据。

## 2. 已确认基线

- [x] 从 `main` 创建 `feat/s3-wifi-transport` 分支。
- [x] 保留并提交原有源码、固件、文档和素材改动。
- [x] 将本地生成的 `/outputs/` 保留在磁盘但排除出版本控制。
- [x] 运行 `node pc/scripts/check-source-safety.mjs`，基线通过 3197 个路径检查。
- [x] 确认暂存区不含 `.env`、`.pem`、`.key`、`.p12`、`.pfx` 等敏感文件。
- [x] 基线提交：`4c30d64`。

## 3. 架构决策

### 3.1 连接拓扑

```text
React UI
  │ existing Tauri IPC
  ▼
Rust DeviceConnection owner
  ├─ USB CDC adapter
  └─ Wi-Fi WebSocket adapter ── authenticated LAN connection ── ESP32-S3
                                                                │
                                                     ProtocolRouter / Sink
                                                                │
                                           Audio / Input / Avatar / Power
```

- PC 主动连接 S3 提供的局域网 WebSocket 服务，避免为 PC Agent 服务增加 LAN listener。
- mDNS 和手动 IP 只产生未授信候选；只有配对校验成功后才能成为业务连接。
- Wi-Fi transport 使用现有 `{topic,payload}` 业务语义；USB wire protocol 保持兼容。
- PC 注入 transport-neutral native scope，远端 payload 无权自报或覆盖该 scope。

### 3.2 首次配网和认证

- 配网、忘记配对和凭据轮换仅允许通过精确 USB owner 发起。
- S3 为每次配对保存独立随机 secret；PC 使用现有 secure config/系统凭据路径保存。
- 网络握手使用随机 challenge 与 HMAC-SHA256，防止被动复制 token 和重放旧握手。
- 首版仅允许一个已配对 PC、一个活动 WebSocket 客户端。
- 正式发布链路只允许 WSS：设备生成并持久化每设备 P-256 identity，PC 固定 USB 配置响应中的叶证书 SHA-256；明文 `ws://` 在 debug/release 均被拒绝。

### 3.3 事务和切换

- transport 切换总是创建新的 connection generation。
- 旧 generation 的 ACK、音频、审批决定和断开回调不得影响新连接。
- 请求超时或 transport 断开属于结果未知；不得自动重放副作用操作。
- 切换时终止旧录音、播放、审批和形象 staging；只重新发布可替换的最新状态。

## 4. 实施清单

### A. 工具链与门禁

状态标记：`[x]` 已完成并有自动化证据；`[~]` 源码/自动化部分完成但仍缺实机或环境证据；`[ ]` 待执行。

- [x] 锁定经验证的 ESP32-S3 PlatformIO platform、Arduino core 和库版本。
- [x] 为 CI 增加独立 transport contract 与 `pio run -d firmware-s3 -e t_keyboard_voice` job。
- [~] 增加 HMAC 行为向量、Rust waiter/writer 单元测试和 S3 源码契约；firmware 队列/切换仍缺原生 host runtime harness。
- [~] 桌面、DSH 与源码安全检查分别记录；Windows ACL 和符号链接权限等环境限制不作为功能通过的证据。

### B. S3 transport-neutral 协议层

- [x] 引入 `TransportContext`，包含 transport kind、connection generation、认证 peer 和 owner 状态。
- [x] 引入统一协议路由，从 USB 和 Wi-Fi 输入完整 envelope。
- [x] 引入统一 `ProtocolTransport` sink，main、audio、input、board、power 不再直接输出到 `Serial`。
- [x] USB CDC adapter 保持现有 JSONL、hello、heartbeat 和音频业务语义。
- [x] 为 RX/TX 设置 12288-byte 上限、有界队列、慢连接和非法 frame 处理。
- [x] 通用 ACK 明确为 `stage: received, completed: false`，完成由 service-specific response 证明。

### C. S3 调度与资源安全

- [x] 下行播放改为可取消的小块 I2S 输出，协议 loop 不再阻塞整段音频时长。
- [x] 16 kHz mono 到 48 kHz stereo 转换改为固定小块 buffer。
- [x] 下行 `audio/play_chunk` 严格校验 seq、长度和重复/乱序，错误终态不再回报成功。
- [x] 控制与 bulk 数据使用有界独立队列；owner 切换清除旧 generation 待发帧。
- [x] 形象 staging 绑定 connection generation 和 transfer owner，并在切换/断线 abort。
- [x] 形象更新与长音频播放实施双向 admission，避免超过 8 MiB PSRAM。
- [~] 已增加 queue high-water、loop 延迟和 heap/PSRAM 最低值；实机音频丢帧统计仍待压力验收。

### D. S3 Wi-Fi 与配对

- [x] 增加 Wi-Fi STA 状态机、指数退避重连和关机/睡眠兼容。
- [x] 增加当前 USB owner 限定的 scan/config/status/forget；首次 P-256/X.509 identity 生成使用低优先级 24 KiB 独立 task 和 heap buffer，完成时重新校验精确 USB generation；重新 config 轮换 pair secret，forget 后下次 config 生成新的 TLS identity。
- [x] 凭据和配对 secret 不进入 Serial console、transfer log 或 diagnostics。
- [x] 增加 `_hachimodock-s3._tcp.local` mDNS 服务和手动 WSS endpoint。
- [x] 增加单客户端 WSS 服务、ping/pong、消息分片、subprotocol 和大小限制；ESP-IDF 自动 upgrade 后由 PC 首帧触发 fd+epoch session 初始化及 challenge，异步 frame 未被 HTTP server 接受时保持 pending 并重试。
- [x] 增加 128-bit nonce + HMAC-SHA256、10 秒握手期限、失败计数和 30 秒 lease。
- [x] 认证完成前禁止业务 dispatcher、录音、审批、配置写入和重启。
- [x] hello 准确声明 USB/WSS、认证、schema 8 和独立 wire protocol capability。

### E. PC 原生统一连接层

- [x] 将当前连接 owner、generation、准入和入站分发扩展为 USB/Wi-Fi 共用。
- [x] 保留 USB 串口发现、probe、pacing 和 partial-write 语义。
- [x] 增加 mDNS/manual candidate、证书 pinning WSS、HMAC、ping/pong、有界队列和退避重连。
- [x] 通用 request waiter 固定 board、boot、generation 和 transport owner。
- [x] S3 形象整个事务固定同一 generation，禁止跨链路续传。
- [x] Rust 注入 `_nativeDeviceScope`，并保留受控 `_nativeUsbScope` 兼容层；远端同名字段被覆盖。
- [x] 普通语音、实时会话、输入、遥测和 waiter 统一验证 native scope；Wi-Fi capture 状态同步到原生 admission。
- [x] 配对 secret 使用现有 AES-256-GCM/Windows credential vault 或 owner-only macOS key 存储，并支持撤销与轮换。
- [x] 自动连接与切换只发布一个 active owner；旧 reader、waiter 和 reconnect generation 被退役。

### F. React/Tauri 接口与 UX

- [x] 原生 device status 保留准确的 `transport`、`authenticated`、endpoint、decimal-string generation。
- [x] `DeviceContext` reachability 由统一 native status 决定，不再硬编码 USB。
- [x] 增加扫描、配网、发现、手动 endpoint、连接和忘记配对 UI；Wi-Fi-only 冷启动仍显示入口。
- [x] 新 Tauri command 同步 handler 和 `permissions/local-app-commands.json`。
- [x] 新命令仅加入 bundled local main UI permission，远程 community frame 无新增权限。
- [x] generation 切换清理旧 waiter、capture 状态、审批/音频 owner 和设备遥测 scope。

### G. 测试与验收

- [~] USB 回归：现有源码契约、PC build、Rust check 和 S3 build 通过；仍需真实 USB smoke。
- [~] 网络 parser：消息上限、text-only、分片和有界队列已有实现/契约；仍需真实慢客户端与 fuzz。
- [~] 认证：固定 HMAC 向量、WSS-only、证书 pinning 和身份一致性有测试；错误 secret/replay 需实机负向测试。
- [x] 代次：waiter 的旧 generation/跨 transport 拒绝、USB bind rotation、旧 TX 清理已有自动化契约。
- [~] 音频：seq/长度/Base64/错误终态、200 ms 预缓冲、4 秒有界流缓冲、独立音频 task、双交替输出块、40 ms TX-only DMA、显式 PLL 160 MHz 时钟、USB/WSS ACK 节流已有实现/源码契约；最新版本未编译，USB/Wi-Fi 声学、频率及长时压力仍需实机。
- [~] 资源：admission 与构建资源用量已确认；最大形象+播放+WSS 仍需实机 PSRAM 压力测试。
- [x] 隔离：新增的是 Rust outbound device client；Agent Bus、hook server、DSH 和 WebView CSP 未开放 LAN。
- [~] PC 聚焦测试、Web/CSP、Rust check 和 Rust test compile 通过；全量测试仅剩既有 Windows symlink/ACL 环境限制。
- [~] S3 transport contracts 通过；PlatformIO 构建仅覆盖历史版本，最新音频任务和 DMA 改动按用户要求未编译。
- [ ] 实机 Wi-Fi、音频、功耗和声学验收；没有硬件证据，不声称通过。

## 5. 关键验收条件

1. 未配对的 LAN 客户端不能读取麦克风、触发按键语义、批准请求、修改配置或重启设备。
2. USB 与 Wi-Fi 同时可见时，设备只有一个业务 owner，事件不重复。
3. transport 切换后，旧 generation 的消息不能完成新 generation 的请求。
4. 长音频播放期间，ping/pong、KEY4、flush、审批期限和断线清理保持响应。
5. 最大形象更新与长音频播放不会无保护地同时占用超过可用 PSRAM。
6. PC 不向 LAN 暴露 Agent Bus、hook HTTP、DSH 或任意 Tauri/WebView IPC。
7. 所有未完成、跳过和仅静态验证的项目在本文档中明确标记。

## 6. 证据记录

| 日期 | 证据 | 结果 |
|---|---|---|
| 2026年11月12日 | `git status --short --untracked-files=all` | 识别出源码、素材、完整 `firmware-s3/` 与本地 `outputs/` |
| 2026年11月12日 | `node pc/scripts/check-source-safety.mjs` | 通过，3197 个路径 |
| 2026年11月12日 | 敏感文件名检查 | 暂存区未包含凭据/私钥文件 |
| 2026年11月12日 | `git commit 4c30d64` | 建立可审查基线，532 个文件 |
| 2026年11月12日 | PlatformIO 6.1.19 基线构建 | 通过；platform 55.3.311、Arduino 3.3.11、IDF 5.5.5，Flash 18.5%、静态 RAM 9.1% |
| 2026年11月12日 | `.github/workflows/source-checks.yml` | 新增独立 S3 PlatformIO CI build job |
| 2026年11月12日 | 统一离线验收基线 | 未通过：Windows symlink 测试因 `EPERM` 失败；runner 子进程未发现 npm/python3；workflow 合成 crate 暴露原有 `component_library` import 缺口。报告：`outputs/s3-wifi-baseline/framework-offline-7nIQ4b/report.json` |
| 2026年11月12日 | `npm test` 基线 | 1857 项：1854 通过、3 失败；2 项为原有源码契约期待值不一致，1 项为 Windows symlink `EPERM` |
| 2026年11月12日 | `npm run test:bridge` 基线 | 245 项：230 通过、12 失败、3 跳过；11 项为 Windows ACL 校验环境失败，1 项为 symlink `EPERM` |
| 2026年11月12日 | Rust lib test 基线 | 编译失败：4 处原有测试引用不存在的 `volcengine_asr::configure_storage_dir`；另有 16 条 warning |
| 2026年11月12日 | Source-safety 自测 | 6/6 通过 |
| 2026年11月12日 | Web build 与 release CSP 基线 | `npm run build:web` 与 `npm run check:release-csp` 通过 |
| 2026年11月12日 | Windows desktop resource preflight | 使用 Node 26.7.0、FFmpeg 8.1.2、对应许可证和源码归档的隔离副本后，`npm run prepare:desktop` 通过 |
| 2026年11月12日 | 最终 S3 transport contracts | 9/9 通过；覆盖 HMAC 向量、WSS session epoch、USB short write、scan topic、owner rotation、旧 TX 清理、playback/capture failure 与精确依赖 |
| 2026年11月12日 | 最终 S3 PlatformIO build | 通过；Flash 42.3%（1330563 bytes）、静态 RAM 17.3%（56640 bytes） |
| 2026年11月12日 | PC Rust | `cargo check` 通过；`cargo test --lib --no-run` 通过；当前 Windows 会话启动 test binary 时发生 `STATUS_ENTRYPOINT_NOT_FOUND`，未伪报单元测试已执行 |
| 2026年11月12日 | Wi-Fi 前端与跨层聚焦测试 | 31/31 通过；`npm run build:web` 与 release CSP 通过 |
| 2026年11月12日 | 许可证重新收集 | 离线 collector 基于双平台 Cargo metadata 与 npm locks 收集 454 packages、835 texts，coverage 完整；仅 symlink 负向测试受 Windows `EPERM` 限制 |
| 2026年11月12日 | 最终 source-safety | publication check 通过 3210 个路径，source-safety 自测 6/6 通过 |
| 2026年11月12日 | 最终 `npm test` | 1865 项：1864 通过；唯一失败为当前 Windows 会话无 symlink 权限导致的 `EPERM`，无功能断言失败 |
| 2026年11月12日 | 最终 bridge 基线复核 | 245 项：230 通过、12 失败、3 跳过；失败仍为既有 11 项 Windows ACL 环境限制与 1 项 symlink `EPERM` |
| 2026年11月12日 | 四轮高强度 correctness review | owner/generation、播放终态与 DMA、冷启动入口、capture/backpressure、recoverable pairing、WSS session/cleanup epoch、异步发送、endpoint 提交、旧 TX、challenge delivery/retry、配对并发、重连发现/lost wakeup、明文 WS、帧边界、IPv6 scope、SSID/PSK、USB short write、README 与测试覆盖问题均已修正并复测 |
| 2026年11月12日 | Windows release build | 按审核资源和 VS 2022 x64 环境完成 `npm run build:win`；release compile 9m10s，NSIS/MSI 两个 bundle 成功 |
| 2026年11月12日 | NSIS SHA-256 | `EasyCoding Deck_0.1.199_x64-setup.exe`：`D7421BEB0EDE2FDDDA0143336A3F6D9F4CE595731522E923823EA3513079F04B` |
| 2026年11月12日 | MSI SHA-256 | `EasyCoding Deck_0.1.199_x64_en-US.msi`：`23146649ADE89C8E93C0534871CB0551913EBEB7389FEB3FC04794E8CF99DD20` |
| 2026年11月13日 | 隔离分支后续修正的语义合入 | 保留 bounded async WSS send、session epoch、DMA drain、capture backpressure 和 owner/generation 修正；仅合入 HTTP 超时，以及播放 chunk/结束拒绝的显式失败回执。按当前执行约束未重新运行编译或测试，待用户自行验证。 |
| 2026年11月13日 | 首次配网崩溃诊断与源码修复 | 崩溃日志 ELF SHA-256 与本地 `firmware.elf` 完全匹配；addr2line 确认 `loopTask` 在 `mbedtls_x509write_crt_set_subject_key_identifier` 中栈溢出。TLS identity 生成已移至低优先级 24 KiB 独立 task，输出 buffer/context 移至 heap，完成后在主循环重新校验 USB generation 再落盘；编译期最低栈约束和 `tlsIdentityStackMinFreeBytes` 运行时水位用于防止回归。按当前执行约束未重新编译或测试。 |
| 2026年11月13日 | WSS 认证超时与状态可见性修复 | 核对 ESP-IDF `httpd_uri.c` 后确认 framework 会自动完成 upgrade 并跳过应用 URI handler；旧实现因此从未在 handshake 时初始化 session，第一条 frame 又被 `HTTP_GET` 分支误吞。现改为 PC 首帧 `network/challenge-request` 到达时建立 fd+epoch session，并在同一次 handler 调用中消费该 frame，再发送/重试 challenge。PC 错误区分 challenge/auth-ack/hello 阶段，面板显示配网、路由连接、IP、RSSI、WSS client/challenge 和认证状态。按当前执行约束未重新编译或测试。 |
| 2026年11月13日 | S3 USB/Wi-Fi 实时对话播放修复 | S3 下行播放改为首个有效 chunk 起播和 128000-byte 有界 ring buffer，缓冲超过 1.5 秒目标水位时主动播放排空再准入下一块，移除 15 秒整段缓存限制；Base64 采用 mbedTLS 两阶段长度校验并返回脱敏分类元数据。WSS 入站队列保留背压帧，异步发送饱和返回 `Busy` 并保留协议队首；PC S3 USB/Wi-Fi 播放提前量降为 600 ms、WSS writer 有界等待 750 ms、普通 ACK 5 秒过期且不重发，错误提示按当前 transport 显示。S3 契约 12/12；PlatformIO 6.1.19 构建与 source-safety 证据属于此前 2 秒缓冲版本，不覆盖随后 4 秒缓冲/主动排空增量；PC 核心改动曾通过 `cargo check`，随后最终 PC 增量按用户要求不再运行编译测试。Rust test binary 仍受当前 Windows `STATUS_ENTRYPOINT_NOT_FOUND` 环境限制。 |

| 2026年11月13日 | USB Base64 长度错误、Wi-Fi 断续与无法结束的后续修复 | 核对本机 HWCDC ISR：8192-byte RX 满时会丢收到的字节；改为 32 KiB RX、16 KiB 读取预算及协议队列满时停止读取。PC S3 USB/WSS 使用 50 ms PCM、四块未确认窗口和 600 ms 时间提前量。删除协议回调中的主动排空循环，新增独立 priority-4 音频任务、128000-byte PSRAM SPSC 流缓冲、200 ms 起播/100 ms 欠载恢复预缓冲、两块 1920-byte 内部 DMA-capable 展开块、8×240-frame TX-only DMA（40 ms），取消后先等待任务退出再切换/清理。S3 显式 PLL 160 MHz、48 kHz、256× MCLK；驱动时钟及缓冲指标进入脱敏诊断，不声称物理频率已校准。WSS 收发拆为独立有界任务，短暂入站背压保留帧；连接退役结束本地会话并标记 `connection_retired`，不伪造设备 idle、不向后继连接发旧终止命令；disable 独立停止扬声器。非编译检查：S3 Python 源码契约 15/15、Node 聚焦回归 20/20、source-safety 3213 路径、`git diff --check` 通过。新增 Rust ACK 窗口单元用例未运行。所有最新固件/PC 代码均未编译，DMA 稳定性、USB/WSS 长回复与停止响应、GPIO16/17/18 物理频率由用户实机验证。 |
| 2026年11月13日 | Wi-Fi 欢迎语后必断线根因修复 | 设备 WebSocket 处理器在 `httpd_ws_recv_frame(...,0)` 返回 `HTTPD_SOCK_ERR_TIMEOUT` 时返回 `ESP_FAIL`，httpd 随即关闭会话；PC 在播放结束后不再下发数据，约一个接收超时后必然断开，重连产生新连接代次，实时对话报「设备连接已变化」。现空闲超时与 PONG 发送失败均保持会话，真正 socket 错误仍由后续接收判定；PC ping 间隔改为 4 秒。按用户要求未运行编译或测试，由用户实机验证。 |
| 2026年11月16日 | Wi-Fi 实时对话间歇性连接变化二次修复 | 追踪到两个独立断线源：WebSocket 控制 Ping 可能被 HTTP framework 消费，固件只按应用帧刷新 30 秒 lease；以及设备异步 WSS 发送回调将瞬时 `ESP_ERR_TIMEOUT`/`ESP_ERR_NO_MEM` 当成致命关闭。PC 现在每 4 秒发送应用 `system/heartbeat`，固件 lease 延长为 120 秒并仅将非瞬时发送错误关闭；S3 播放改为 3200-byte/100 ms、8 块窗口匹配设备 8 个异步发送槽，PC 入站缓冲扩为 128、分发等待 30 秒。按用户要求未编译、未测试，由用户验证。 |

## 7. 偏差与未决项

- 工具链已锁定为本机与 CI 验证组合：PlatformIO 6.1.19、`espressif32@55.3.311`、Arduino 3.3.11、ESP-IDF 5.5.5、ArduinoJson 7.4.3、LovyanGFX 1.3.0。
- 固件采用每设备自生成 P-256 证书与 PC 叶证书 SHA-256 pinning；NVS 物理提取风险仍依赖产品量产时启用 ESP32 flash encryption/secure boot，不在本轮软件 transport 范围内。
- 历史版本完成过 S3 固件构建和 PC `cargo check`；Rust 聚焦测试仅编译过，test binary 启动受 Windows `STATUS_ENTRYPOINT_NOT_FOUND` 限制。最新 USB/WSS ACK 节流、独立 DMA 播放任务和停止生命周期修复只通过非编译源码契约；用户要求今后全部编译自行完成，不运行 PlatformIO、cargo build/check/test、npm build 等命令。最新固件与 PC 均待用户编译验证，实机 USB/Wi-Fi 长回复、停止响应、GPIO 时钟频率、声学和功耗仍需单独留证。
- 功能改动前的统一离线验收不是绿色基线：Windows 当前会话没有创建 symlink 权限，runner 的隔离 PATH 未发现 npm/python3，且 workflow 合成测试暴露原有 `component_library` import 缺口；这些必须与本任务新增回归分开报告。
- bridge 全量测试仍有 11 项 Windows ACL 环境失败和 1 项 symlink `EPERM`；本任务没有放宽 ACL、loopback 或 production 安全行为。
