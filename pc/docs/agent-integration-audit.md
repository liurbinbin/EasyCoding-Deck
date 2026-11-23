# Agent 接入与兼容说明

本页区分状态接收、设备气泡、语音草稿、确认发送和桌面对话切换。它描述当前适配实现，不表示每个 Agent 版本、操作系统与硬件组合均已完成实机验收。

| Agent | 状态与气泡 | 按键语音路径 | 集成说明 |
| --- | --- | --- | --- |
| ChatGPT（Codex） | 日志事件及独立会话显示 | macOS / Windows 可见输入框 | 目标为当前可见且可写的编辑器；统计事件不冒充新回复 |
| Claude | 日志和 Hook；授权请求另走独立通道 | macOS / Windows 可见桌面输入框 | 权限气泡与状态 Hook 分开处理；默认不代替用户批准工具操作 |
| MiMoCode | 插件提供标题、内容与状态 | macOS 当前终端光标；Windows 尚未实现 | Windows 不支持该语音路径；检测到程序不表示可输入 |
| OpenClaw | 网关事件 | 已有 Agent Bus 路由 | 显示可取得的会话状态，心跳不刷新气泡时间，断开连接不残留旧气泡 |
| WorkBuddy | 独立 Hook 和会话状态 | macOS / Windows 当前可见输入框 | 追加语音草稿、确认发送及 Skill 安装；不读取私有历史数据库 |
| OpenCode | 本地官方插件事件 | 暂未接入桌面输入框 | 启用后自动同步独立状态插件；状态源固定为 `opencode`，不与 MiMoCode 混用 |
| Hermes Agent | 仅安装检测 | 未接入 | 当前桌面版未发现可验证的本地状态/审批 Hook，界面显示已安装但不允许跟随 |
| ZCode | 仅安装检测 | 未接入 | 当前桌面版未发现可验证的本地状态/审批 Hook，界面显示已安装但不允许跟随 |

| DeepSeek Harness | 插件提供会话状态；与桌面输入通道独立 | macOS / Windows 当前可见输入框 | 无需输入配对码；需在 DeepSeek 打开目标对话；输入不可用或目标不唯一时停止 |

## 输入与确认

语音松开后先形成草稿，用户确认才请求发送。桌面辅助功能必须能定位目标窗口和真实编辑器；检测到程序、状态事件正常或系统权限已授予，都不能单独证明编辑器可写。

录音固定本次目标。编辑器不可用、目标变化、写入或提交结果未知时，本次操作停止，不自动重放，也不改用最近历史会话。下一次新录音独立处理。界面读到的占位提示不属于用户输入。

WorkBuddy 与 DeepSeek 的设备气泡选择不会切换其桌面对话；请先在应用中打开目标对话。MiMoCode/OpenCode Windows 的语音尚未实现，设备实时对话属于另一条能力路径。详见 [WorkBuddy](workbuddy.md)、[DSH](../../integrations/dsh-easycoding-deck/README.md)。

## 权限边界

确认发送语音不等于批准 Agent 的工具操作。Claude 默认权限 Hook 将决定权留给 Claude；只有显式配置的允许/拒绝策略才能返回相应决定，未知策略不会回退为允许。协议依据见 [Claude PermissionRequest 文档](https://code.claude.com/docs/en/hooks#permissionrequest-decision-control)。

状态摘要来自各适配器可取得的事件；缺失信息不应伪装成完整回复。反馈时说明 Agent 版本、系统、目标对话是否可见以及失败阶段，不公开会话正文、路径或凭据。

WorkBuddy 权限请求使用专用 loopback HTTP Hook 阻塞等待设备决定。Bridge 只发布一次随机请求 ID，Rust 只为当前设备连接和当前 WorkBuddy 通道写入对应的一次性 allow/deny 决策文件；这条路径不依赖 Electron 审批菜单的具体行数或文字样式。原生 UI 自动化仅作为旧版本回退。
