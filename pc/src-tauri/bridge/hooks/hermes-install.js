"use strict";

const { spawnSync } = require("node:child_process");

const MANAGED_NAME = "EasyCoding Deck";
const EVENTS = [
  "on_session_open", "pre_llm_call", "pre_tool_call", "post_tool_call",
  "on_stream_start", "on_stream_end", "pre_approval_request",
  "post_approval_response", "agent_loop_stopped", "on_session_end",
];

function mergeHermesTargets(current, port) {
  const entries = current == null ? [] : current;
  if (!Array.isArray(entries)) throw new Error("Hermes hooks.outbound must be a list");
  const managed = { url: `http://127.0.0.1:${Number(port) || 23333}/hermes-state`, events: EVENTS, name: MANAGED_NAME, timeout: 2 };
  return [
    ...entries.filter((entry) => entry?.name !== MANAGED_NAME && !String(entry?.url || "").includes("/hermes-state")),
    managed,
  ];
}

function runHermes(executable, args, options = {}) {
  const spawn = options.spawnSync || spawnSync;
  const result = spawn(executable, args, {
    encoding: "utf8",
    windowsHide: true,
    timeout: options.timeout || 10000,
    env: options.env || process.env,
  });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error((result.stderr || result.stdout || "Hermes config command failed").trim());
  return result.stdout.trim();
}

function isHermesUnsetError(error) {
  return /not found|unknown|(?:key\s+)?not set|is not set|missing/i.test(String(error?.message || error));
}

function syncHermesHooks(options = {}) {
  const executable = options.executable || "hermes.exe";
  let current = [];
  try {
    const raw = runHermes(executable, ["config", "get", "hooks.outbound", "--json"], options);
    current = raw ? JSON.parse(raw) : [];
  } catch (error) {
    if (!isHermesUnsetError(error)) throw error;
  }
  const next = mergeHermesTargets(current, options.port);
  if (JSON.stringify(current) === JSON.stringify(next)) return { updated: false };
  runHermes(executable, ["config", "set", "hooks.outbound", JSON.stringify(next)], options);
  runHermes(executable, ["gateway", "restart"], { ...options, timeout: options.restartTimeout || 30000 });
  return { updated: true, restarted: true };
}

module.exports = { EVENTS, MANAGED_NAME, isHermesUnsetError, mergeHermesTargets, syncHermesHooks };
