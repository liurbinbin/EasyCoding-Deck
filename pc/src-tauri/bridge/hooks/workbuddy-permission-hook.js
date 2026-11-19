#!/usr/bin/env node
"use strict";

const { getPortCandidates, readRuntimePort } = require("./server-config");

async function readInput(input = process.stdin) {
  let bytes = 0;
  const chunks = [];
  for await (const chunk of input) {
    bytes += chunk.length;
    if (bytes > 1024 * 1024) throw new Error("WorkBuddy permission payload is too large");
    chunks.push(chunk);
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}");
}

function normalizePayload(payload) {
  const value = payload && typeof payload === "object" ? payload : {};
  return {
    ...value,
    hook_event_name: value.hook_event_name || value.hookEventName || value.event || "PermissionRequest",
  };
}

async function requestDecision(payload, options = {}) {
  const ports = getPortCandidates(options.port, { runtimePort: options.runtimePort ?? readRuntimePort() });
  let lastError;
  for (const port of ports) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/workbuddy-permission`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(normalizePayload(payload)),
        signal: AbortSignal.timeout(options.timeoutMs || 305000),
      });
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const body = await response.json();
      return body && typeof body === "object" ? body : {};
    } catch (error) {
      lastError = error;
    }
  }
  throw lastError || new Error("EasyCoding Deck Bridge unavailable");
}

async function main() {
  const payload = await readInput();
  const response = await requestDecision(payload);
  process.stdout.write(`${JSON.stringify(response)}\n`);
}

if (require.main === module) {
  main().catch((error) => {
    process.stderr.write(`${String(error?.message || error)}\n`);
    process.stdout.write("{}\n");
  });
}

module.exports = { normalizePayload, readInput, requestDecision };
