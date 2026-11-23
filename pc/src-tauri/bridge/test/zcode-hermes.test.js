"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const test = require("node:test");

const { EVENTS: HERMES_EVENTS, isHermesUnsetError, mergeHermesTargets, syncHermesHooks } = require("../hooks/hermes-install.js");
const { syncZCodeHooks } = require("../hooks/zcode-install.js");
const { permissionOutput, stateForHook } = require("../hooks/zcode-pet-manager-hook.js");
const { hermesStatePayload } = require("../packages/clawd-backend-service/src/headless-mqtt.js");
const { SessionMetricsTracker } = require("../packages/clawd-backend-service/src/status-metrics.js");

test("Hermes target merge preserves user webhooks and replaces only EasyCoding Deck", () => {
  const current = [
    { name: "User webhook", url: "https://example.invalid/events", events: ["on_session_end"] },
    { name: "EasyCoding Deck", url: "http://127.0.0.1:23334/hermes-state", events: [] },
  ];
  const next = mergeHermesTargets(current, 23335);
  assert.deepEqual(next[0], current[0]);
  assert.deepEqual(next[1], {
    name: "EasyCoding Deck",
    url: "http://127.0.0.1:23335/hermes-state",
    events: HERMES_EVENTS,
    timeout: 2,
  });
  assert.deepEqual(mergeHermesTargets(null, 23333).map((entry) => entry.name), ["EasyCoding Deck"]);
});

test("Hermes missing config value is treated as an empty outbound hook list", () => {
  assert.equal(isHermesUnsetError(new Error("Config key not set: hooks.outbound")), true);
  assert.equal(isHermesUnsetError(new Error("permission denied")), false);
});

test("Hermes restarts its gateway only after managed webhook configuration changes", () => {
  const calls = [];
  const spawnSync = (_executable, args) => {
    calls.push(args);
    if (args[0] === "config" && args[1] === "get") {
      return { status: 0, stdout: "[]", stderr: "" };
    }
    return { status: 0, stdout: "", stderr: "" };
  };
  assert.deepEqual(syncHermesHooks({ executable: "hermes", port: 23333, spawnSync }), {
    updated: true,
    restarted: true,
  });
  assert.deepEqual(calls.map((args) => args.slice(0, 2)), [
    ["config", "get"],
    ["config", "set"],
    ["gateway", "restart"],
  ]);

  calls.length = 0;
  const managed = JSON.stringify(mergeHermesTargets([], 23333));
  const unchangedSpawn = (_executable, args) => {
    calls.push(args);
    return { status: 0, stdout: args[0] === "config" && args[1] === "get" ? managed : "", stderr: "" };
  };
  assert.deepEqual(syncHermesHooks({ executable: "hermes", port: 23333, spawnSync: unchangedSpawn }), {
    updated: false,
  });
  assert.equal(calls.length, 1);
});

test("Hermes completes only when its agent loop stops", () => {
  assert.equal(hermesStatePayload({ hook_event_name: "on_stream_end" }).state, "speaking");
  assert.equal(hermesStatePayload({ hook_event_name: "agent_loop_stopped" }).state, "done");
  assert.equal(hermesStatePayload({ hook_event_name: "agent_loop_stopped", status: "error" }).state, "error");
});

test("Hermes starts a second turn after the previous terminal state", () => {
  const tracker = new SessionMetricsTracker();
  const apply = (hookEvent, nowMs) => {
    const payload = hermesStatePayload({ hook_event_name: hookEvent, session_id: "hermes-session" });
    return tracker.apply({
      source: "hermes",
      sessionId: payload.session_id,
      event: payload.event,
      state: payload.state,
      display: { title: payload.display_title, content: payload.display_content },
    }, nowMs);
  };

  assert.equal(apply("pre_llm_call", 100).state, "working");
  assert.equal(apply("agent_loop_stopped", 200).state, "done");
  const secondTurn = apply("pre_llm_call", 300);
  assert.equal(secondTurn.state, "working");
  assert.equal(secondTurn.display.content, "正在处理");
  assert.equal(tracker.sessionStatuses("hermes", 300)[0].updatedAt, 300);
});

test("ZCode hook install preserves user hooks, writes all events, and is idempotent", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "easycoding-zcode-test-"));
  try {
    const configPath = path.join(root, "config.json");
    const userHook = { matcher: "shell", hooks: [{ type: "command", command: "user-hook" }] };
    fs.writeFileSync(configPath, `${JSON.stringify({ hooks: { events: { PreToolUse: [userHook] } } }, null, 2)}\n`);
    const options = { configPath, nodeBin: "C:\\runtime\\node.exe", scriptPath: "C:\\runtime\\zcode-hook.js" };
    assert.equal(syncZCodeHooks(options).updated, true);
    const installed = JSON.parse(fs.readFileSync(configPath, "utf8"));
    assert.deepEqual(installed.hooks.events.PreToolUse[0], userHook);
    assert.equal(installed.hooks.events.PermissionRequest[0].hooks[0].timeoutMs, 310000);
    assert.equal(Object.hasOwn(installed.hooks.events.PermissionRequest[0], "matcher"), false);
    assert.equal(installed.hooks.events.Stop[0].hooks[0].timeoutMs, 3000);
    assert.equal(syncZCodeHooks(options).updated, false);
    assert.equal(fs.existsSync(`${configPath}.easycoding-backup`), true);
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("ZCode migrates only the managed empty matcher", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "easycoding-zcode-migrate-"));
  try {
    const configPath = path.join(root, "config.json");
    const managed = { matcher: "", hooks: [{ type: "process", command: "old-node", args: ["C:\\runtime\\zcode-pet-manager-hook.js"] }] };
    const user = { matcher: "", hooks: [{ type: "command", command: "user-hook" }] };
    fs.writeFileSync(configPath, `${JSON.stringify({ hooks: { events: { Stop: [managed, user] } } }, null, 2)}\n`);
    syncZCodeHooks({ configPath, nodeBin: "node", scriptPath: "C:\\runtime\\zcode-pet-manager-hook.js" });
    const entries = JSON.parse(fs.readFileSync(configPath, "utf8")).hooks.events.Stop;
    assert.equal(Object.hasOwn(entries[0], "matcher"), false);
    assert.equal(entries[1].matcher, "");
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("ZCode PermissionRequest emits strict allow and deny hook responses", () => {
  assert.deepEqual(permissionOutput("allow"), {
    hookSpecificOutput: { hookEventName: "PermissionRequest", decision: { behavior: "allow" } },
  });
  assert.deepEqual(permissionOutput("deny"), {
    hookSpecificOutput: {
      hookEventName: "PermissionRequest",
      decision: { behavior: "deny", message: "Denied from EasyCoding Deck" },
    },
  });
  assert.equal(stateForHook({ hook_event_name: "PermissionRequest" }).state, "waiting_user");
  assert.equal(stateForHook({ hook_event_name: "Stop" }).state, "done");
});
