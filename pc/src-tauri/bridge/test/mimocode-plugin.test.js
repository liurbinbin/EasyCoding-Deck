"use strict";

/*
 * [Input] Temporary MiMoCode config roots and synthetic official plugin events.
 * [Output] Regression coverage for managed plugin installation and EasyCoding Deck state payloads.
 * [Pos] Node tests for the MiMoCode status integration.
 */

const test = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const http = require("node:http");
const os = require("node:os");
const path = require("node:path");

const {
  MANAGED_PLUGIN_FILE_NAME,
  OPENCODE_PLUGIN_FILE_NAME,
  resolveMiMoCodeConfigDir,
  resolveOpenCodeConfigDir,
  syncMiMoCodePlugin,
  syncOpenCodePlugin,
} = require("../hooks/mimocode-install");

const PLUGIN_SOURCE = path.join(__dirname, "..", "hooks", "mimocode-pet-manager-plugin.js");

test("MiMoCode plugin sync is idempotent and preserves user plugins", () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "mimocode-plugin-sync-"));
  try {
    const configDir = resolveMiMoCodeConfigDir({ home, env: {} });
    const pluginDir = path.join(configDir, "plugin");
    fs.mkdirSync(pluginDir, { recursive: true });
    const userPlugin = path.join(pluginDir, "user-plugin.js");
    fs.writeFileSync(userPlugin, "export const UserPlugin = async () => ({});\n", "utf8");

    const first = syncMiMoCodePlugin({ configDir, sourcePath: PLUGIN_SOURCE });
    assert.equal(first.added, 1);
    assert.equal(first.updated, 0);
    assert.equal(fs.existsSync(path.join(pluginDir, MANAGED_PLUGIN_FILE_NAME)), true);
    assert.equal(fs.readFileSync(userPlugin, "utf8"), "export const UserPlugin = async () => ({});\n");

    const second = syncMiMoCodePlugin({ configDir, sourcePath: PLUGIN_SOURCE });
    assert.equal(second.added, 0);
    assert.equal(second.updated, 0);
    assert.equal(second.skipped, 1);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("OpenCode plugin uses its own config root and Agent identity", async () => {
  const home = fs.mkdtempSync(path.join(os.tmpdir(), "opencode-plugin-sync-"));
  try {
    const configDir = resolveOpenCodeConfigDir({ home, env: {} });
    const result = syncOpenCodePlugin({ configDir, sourcePath: PLUGIN_SOURCE });
    assert.equal(result.added, 1);
    const obsoleteDir = path.join(configDir, "plugin");
    fs.mkdirSync(obsoleteDir, { recursive: true });
    fs.writeFileSync(path.join(obsoleteDir, MANAGED_PLUGIN_FILE_NAME), "obsolete");
    const updated = syncOpenCodePlugin({ configDir, sourcePath: PLUGIN_SOURCE });
    assert.equal(updated.removed, 1);
    const installed = path.join(configDir, "plugins", OPENCODE_PLUGIN_FILE_NAME);
    assert.equal(fs.existsSync(installed), true);
    assert.equal(fs.existsSync(path.join(obsoleteDir, MANAGED_PLUGIN_FILE_NAME)), false);
    const source = fs.readFileSync(installed, "utf8");
    assert.match(source, /OPEN_CODE_PLUGIN/);
    assert.match(source, /AGENT_ID = OPEN_CODE_PLUGIN \? "opencode" : "mimocode"/);
    assert.match(source, /export default \{/);
    assert.match(source, /id: "easycoding-deck"/);
    assert.match(source, /ctx\.event\.subscribe/);
  } finally {
    fs.rmSync(home, { recursive: true, force: true });
  }
});

test("OpenCode V2 default definition subscribes to lifecycle events", async () => {
  const source = fs.readFileSync(PLUGIN_SOURCE, "utf8");
  const installedUrl = "file:///C:/Users/test/.config/opencode/plugins/opencode-pet-manager.js";
  const dataUrl = `data:text/javascript;base64,${Buffer.from(
    `${source.replace("import.meta.url", JSON.stringify(installedUrl))}\n// v2-test-${Date.now()}`,
    "utf8",
  ).toString("base64")}`;
  const module = await import(dataUrl);
  const events = [];
  const definition = module.default;
  assert.equal(definition.id, "easycoding-deck");
  const dispose = definition.setup({
    location: { directory: "D:/repo" },
    event: {
      subscribe: async function* () {
        events.push("subscribed");
      },
    },
  });
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(events, ["subscribed"]);
  assert.equal(typeof dispose, "function");
  dispose();
});

test("OpenCode V2 lifecycle publishes working, waiting, and done states", async () => {
  const received = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      received.push(JSON.parse(Buffer.concat(chunks).toString("utf8")));
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end('{"ok":true}');
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const previousPort = process.env.PET_MANAGER_BRIDGE_PORT;
  process.env.PET_MANAGER_BRIDGE_PORT = String(server.address().port);
  try {
    const source = fs.readFileSync(PLUGIN_SOURCE, "utf8");
    const installedUrl = "file:///C:/Users/test/.config/opencode/plugins/opencode-pet-manager.js";
    const dataUrl = `data:text/javascript;base64,${Buffer.from(
      `${source.replace("import.meta.url", JSON.stringify(installedUrl))}\n// v2-events-${Date.now()}`,
      "utf8",
    ).toString("base64")}`;
    const module = await import(dataUrl);
    const sessionID = "opencode-v2-session";
    const events = [
      { type: "session.created", location: { directory: "D:/repo" }, data: { sessionID, title: "V2 task" } },
      { type: "session.execution.started", location: { directory: "D:/repo" }, data: { sessionID } },
      { type: "session.text.delta", location: { directory: "D:/repo" }, data: { sessionID, delta: "已" } },
      { type: "session.text.ended", location: { directory: "D:/repo" }, data: { sessionID, text: "已完成 V2 任务" } },
      { type: "permission.asked", location: { directory: "D:/repo" }, data: { sessionID, action: "shell" } },
      { type: "session.execution.succeeded", location: { directory: "D:/repo" }, data: { sessionID } },
    ];
    const dispose = module.default.setup({
      location: { directory: "D:/repo" },
      event: {
        subscribe: async function* () {
          for (const event of events) yield event;
        },
      },
    });
    await waitFor(() => received.length >= 3);
    dispose();
    assert.deepEqual(received.slice(0, 3).map((item) => item.state), ["working", "waiting_user", "done"]);
    assert.ok(received.slice(0, 3).every((item) => item.agent_id === "opencode"));
    assert.equal(received[1].display_content, "等待授权：shell");
    assert.equal(received[2].display_content, "已完成 V2 任务");
  } finally {
    if (previousPort === undefined) delete process.env.PET_MANAGER_BRIDGE_PORT;
    else process.env.PET_MANAGER_BRIDGE_PORT = previousPort;
    await new Promise((resolve) => server.close(resolve));
  }
});

test("OpenCode V2 permission request waits for hardware and replies through the official API", async () => {
  const received = [];
  const replies = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      received.push({ url: req.url, body: JSON.parse(Buffer.concat(chunks).toString("utf8")) });
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(req.url === "/opencode-permission" ? '{"decision":"allow"}' : '{"ok":true}');
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const previousPort = process.env.PET_MANAGER_BRIDGE_PORT;
  process.env.PET_MANAGER_BRIDGE_PORT = String(server.address().port);
  try {
    const source = fs.readFileSync(PLUGIN_SOURCE, "utf8");
    const installedUrl = "file:///C:/Users/test/.config/opencode/plugins/opencode-pet-manager.js";
    const dataUrl = `data:text/javascript;base64,${Buffer.from(
      `${source.replace("import.meta.url", JSON.stringify(installedUrl))}\n// v2-permission-${Date.now()}`,
      "utf8",
    ).toString("base64")}`;
    const module = await import(dataUrl);
    const dispose = module.default.setup({
      location: { directory: "D:/repo" },
      event: {
        subscribe: async function* () {
          yield { type: "permission.asked", data: { sessionID: "session-1", requestID: "request-1", action: "bash" } };
        },
      },
      permission: { reply: async (value) => replies.push(value) },
    });
    await waitFor(() => replies.length === 1);
    dispose();
    assert.deepEqual(replies[0], { sessionID: "session-1", requestID: "request-1", decision: "once" });
    assert.equal(received.some((entry) => entry.url === "/opencode-permission"), true);
    assert.equal(received.find((entry) => entry.url === "/opencode-permission").body.tool_name, "bash");
  } finally {
    if (previousPort === undefined) delete process.env.PET_MANAGER_BRIDGE_PORT;
    else process.env.PET_MANAGER_BRIDGE_PORT = previousPort;
    await new Promise((resolve) => server.close(resolve));
  }
});

test("MiMoCode official lifecycle events publish working, waiting, and done states", async () => {
  const received = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (chunk) => chunks.push(chunk));
    req.on("end", () => {
      received.push(JSON.parse(Buffer.concat(chunks).toString("utf8")));
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end('{"ok":true}');
    });
  });
  await new Promise((resolve) => server.listen(0, "127.0.0.1", resolve));
  const address = server.address();
  const previousPort = process.env.PET_MANAGER_BRIDGE_PORT;
  process.env.PET_MANAGER_BRIDGE_PORT = String(address.port);

  try {
    const source = fs.readFileSync(PLUGIN_SOURCE, "utf8");
    const dataUrl = `data:text/javascript;base64,${Buffer.from(
      `${source}\n// test-${Date.now()}`,
      "utf8",
    ).toString("base64")}`;
    const module = await import(dataUrl);
    const hooks = await module.PetManagerMiMoCodePlugin({ directory: "D:/repo" });
    const sessionID = "mimo-session-1";

    await hooks.event({
      event: {
        type: "session.updated",
        properties: {
          sessionID,
          info: {
            id: sessionID,
            title: "接入 MiMoCode 状态",
            directory: "D:/repo",
          },
        },
      },
    });
    await hooks["session.pre"]({ sessionID, agentID: "build" }, {});
    await hooks.event({
      event: {
        type: "permission.asked",
        properties: {
          id: "permission-1",
          sessionID,
          permission: "bash",
          patterns: [],
          metadata: {},
          always: [],
        },
      },
    });
    await hooks.event({
      event: {
        type: "message.part.updated",
        properties: {
          sessionID,
          part: {
            type: "text",
            text: "MiMoCode 状态桥接完成。",
          },
        },
      },
    });
    await hooks.event({
      event: {
        type: "session.idle",
        properties: { sessionID },
      },
    });

    await waitFor(() => received.length >= 3);
    assert.deepEqual(received.map((item) => item.state), ["working", "waiting_user", "done"]);
    assert.ok(received.every((item) => item.agent_id === "mimocode"));
    assert.ok(received.every((item) => item.session_id === sessionID));
    assert.equal(received[2].display_content, "MiMoCode 状态桥接完成。");
  } finally {
    if (previousPort === undefined) delete process.env.PET_MANAGER_BRIDGE_PORT;
    else process.env.PET_MANAGER_BRIDGE_PORT = previousPort;
    await new Promise((resolve) => server.close(resolve));
  }
});

async function waitFor(predicate, timeoutMs = 3000) {
  const started = Date.now();
  while (!predicate()) {
    if (Date.now() - started > timeoutMs) throw new Error("timed out waiting for plugin state");
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
}
