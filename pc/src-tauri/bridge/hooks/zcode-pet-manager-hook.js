"use strict";

const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");

const DEFAULT_PORT = 23333;
const MIN_PORT = 23333;
const MAX_PORT = 23337;
const runtimePath = process.env.PET_MANAGER_RUNTIME_PATH
  || path.join(os.homedir(), ".clawd", "runtime.json");

function compact(value, limit = 320) {
  const text = typeof value === "string" ? value.replace(/\s+/g, " ").trim() : "";
  return text.length > limit ? `${text.slice(0, limit - 3)}...` : text;
}

function candidatePorts() {
  const ports = [];
  const explicit = Number(process.env.PET_MANAGER_BRIDGE_PORT);
  if (Number.isInteger(explicit) && explicit >= MIN_PORT && explicit <= MAX_PORT) ports.push(explicit);
  try {
    const runtime = JSON.parse(fs.readFileSync(runtimePath, "utf8"));
    const port = Number(runtime?.port);
    if (Number.isInteger(port) && port >= MIN_PORT && port <= MAX_PORT) ports.push(port);
  } catch {}
  ports.push(DEFAULT_PORT);
  for (let port = MIN_PORT; port <= MAX_PORT; port += 1) ports.push(port);
  return [...new Set(ports)];
}

async function post(pathname, payload, timeoutMs) {
  let lastError = null;
  for (const port of candidatePorts()) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}${pathname}`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
        signal: AbortSignal.timeout(timeoutMs),
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      return await response.json().catch(() => ({}));
    } catch (error) {
      lastError = error;
    }
  }
  throw lastError || new Error("EasyCoding Deck Bridge unavailable");
}

function stateForHook(payload) {
  const event = String(payload?.hook_event_name || "");
  const state = event === "Stop"
    ? "done"
    : event === "PostToolUseFailure"
      ? "error"
      : event === "PermissionRequest"
        ? "waiting_user"
        : "working";
  const tool = compact(payload?.tool_name, 100);
  return {
    agent_id: "zcode",
    state,
    event,
    session_id: compact(payload?.session_id, 160) || undefined,
    cwd: compact(payload?.cwd, 500) || undefined,
    display_title: "ZCode",
    display_content: state === "done"
      ? compact(payload?.last_assistant_message, 320) || "已完成"
      : state === "error"
        ? compact(payload?.error, 320) || "执行失败"
        : state === "waiting_user"
          ? tool ? `等待授权：${tool}` : "等待授权"
          : tool ? `正在执行 ${tool}` : "正在处理",
    tool_name: tool || undefined,
  };
}

function permissionOutput(decision) {
  return {
    hookSpecificOutput: {
      hookEventName: "PermissionRequest",
      decision: decision === "allow"
        ? { behavior: "allow" }
        : { behavior: "deny", message: "Denied from EasyCoding Deck" },
    },
  };
}

async function main(input = process.stdin) {
  let raw = "";
  for await (const chunk of input) raw += chunk;
  const payload = JSON.parse(raw || "{}");
  if (payload?.hook_event_name === "PermissionRequest") {
    const reply = await post("/zcode-permission", payload, 305000);
    process.stdout.write(`${JSON.stringify(permissionOutput(reply?.decision))}\n`);
    return;
  }
  await post("/state", stateForHook(payload), 1500);
}

if (require.main === module) {
  main().catch((error) => {
    process.stderr.write(`${String(error?.message || error)}\n`);
    process.exitCode = 1;
  });
}

module.exports = { candidatePorts, permissionOutput, stateForHook };
