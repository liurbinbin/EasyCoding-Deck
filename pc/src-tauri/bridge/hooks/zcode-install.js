"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PermissionRequest", "PostToolUse", "PostToolUseFailure", "Stop"];
const MARKER = "zcode-pet-manager-hook.js";

function resolveZCodeConfig(options = {}) {
  const env = options.env || process.env;
  const home = options.home || env.HOME || env.USERPROFILE || os.homedir();
  return options.configPath || path.join(home, ".zcode", "cli", "config.json");
}

function syncZCodeHooks(options = {}) {
  const configPath = resolveZCodeConfig(options);
  let original = null;
  try { original = fs.readFileSync(configPath, "utf8"); } catch (error) { if (error.code !== "ENOENT") throw error; }
  const config = original == null ? {} : JSON.parse(original);
  if (!config || typeof config !== "object" || Array.isArray(config)) throw new Error("ZCode config must be an object");
  if (config.hooks != null && (typeof config.hooks !== "object" || Array.isArray(config.hooks))) throw new Error("Invalid ZCode hooks config");
  config.hooks ||= {};
  if (config.hooks.events != null && (typeof config.hooks.events !== "object" || Array.isArray(config.hooks.events))) throw new Error("Invalid ZCode hook events");
  config.hooks.events ||= {};
  config.hooks.enabled = true;
  const command = options.nodeBin || process.execPath;
  const script = options.scriptPath || path.join(__dirname, MARKER);
  let changed = original == null;
  for (const event of EVENTS) {
    const entries = config.hooks.events[event] ||= [];
    if (!Array.isArray(entries)) throw new Error(`Invalid ZCode ${event} hook list`);
    let own;
    let ownEntry;
    for (const entry of entries) {
      if (!Array.isArray(entry?.hooks)) continue;
      own = entry.hooks.find((hook) => hook?.type === "process"
        && hook.args?.some((arg) => String(arg) === script || String(arg).includes(MARKER)));
      if (own) { ownEntry = entry; break; }
    }
    const wanted = {
      type: "process",
      command,
      args: [script],
      timeoutMs: event === "PermissionRequest" ? 310000 : 3000,
      statusMessage: event === "PermissionRequest" ? "等待 EasyCoding Deck 审批" : "同步 EasyCoding Deck 状态",
    };
    if (own) {
      if (JSON.stringify(own) !== JSON.stringify(wanted)) { Object.keys(own).forEach((key) => delete own[key]); Object.assign(own, wanted); changed = true; }
      if (ownEntry?.matcher === "") { delete ownEntry.matcher; changed = true; }
    } else {
      entries.push({ hooks: [wanted] });
      changed = true;
    }
  }
  if (!changed && original === `${JSON.stringify(config, null, 2)}\n`) return { updated: false, configPath };
  fs.mkdirSync(path.dirname(configPath), { recursive: true });
  if (original != null) {
    try { fs.writeFileSync(`${configPath}.easycoding-backup`, original, { flag: "wx", mode: 0o600 }); }
    catch (error) { if (error.code !== "EEXIST") throw error; }
  }
  const temporary = `${configPath}.easycoding-${process.pid}.tmp`;
  try {
    fs.writeFileSync(temporary, `${JSON.stringify(config, null, 2)}\n`, { flag: "wx", mode: 0o600 });
    fs.renameSync(temporary, configPath);
  } finally { try { fs.unlinkSync(temporary); } catch {} }
  return { updated: true, configPath };
}

module.exports = { EVENTS, MARKER, resolveZCodeConfig, syncZCodeHooks };
