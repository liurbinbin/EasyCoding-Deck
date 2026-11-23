export function deviceVoicePlatformWarning(agentId, platform) {
  return agentId === "mimocode" && platform !== "macos"
    ? "MiMoCode 的按键语音输入目前仅支持 macOS 终端；当前系统可使用状态跟随与设备实时对话。"
    : "";
}
export function deviceVoiceTransportReady(agentId, platform, cliReady) {
  const visibleDesktopAgents = new Set([
    "codex", "claude-code", "workbuddy", "deepseek-harness",
    "opencode", "hermes", "zcode",
  ]);
  // Visible desktop drafts do not use the Agent Session Bus transport. Native
  // ownership, foreground and writability checks still run before each use.
  return (["windows", "macos"].includes(platform) && visibleDesktopAgents.has(agentId))
    || cliReady === true;
}

