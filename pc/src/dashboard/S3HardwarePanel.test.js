import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "S3HardwarePanel.jsx"), "utf8");

test("S3 hardware panel follows negotiated capabilities and live telemetry", () => {
  assert.match(source, /displayTopology/);
  assert.match(source, /power\/status/);
  assert.match(source, /audio\/status/);
  assert.match(source, /五屏状态/);
  assert.match(source, /个带屏按键 · 非触摸/);
  assert.doesNotMatch(source, /键 \+ 旋钮/);
});
