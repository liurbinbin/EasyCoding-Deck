import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "SerialLogPage.jsx"), "utf8");
const appSource = readFileSync(join(here, "..", "App.jsx"), "utf8");
const styles = readFileSync(join(here, "..", "styles.css"), "utf8");
const tauriLib = readFileSync(join(here, "..", "..", "src-tauri", "src", "lib.rs"), "utf8");
const localPermissions = JSON.parse(readFileSync(join(here, "..", "..", "src-tauri", "permissions", "local-app-commands.json"), "utf8"));

test("serial log viewer reads the Tauri-owned stream without opening a port", () => {
  assert.match(source, /invoke\("usb_get_serial_log"/);
  assert.match(source, /invoke\("usb_clear_serial_log"/);
  assert.doesNotMatch(source, /SerialPort|navigator\.serial|requestPort/);
});

test("serial log viewer is bounded and provides pause, copy, and clear controls", () => {
  assert.match(source, /MAX_VISIBLE_ROWS = 600/);
  assert.match(source, /setPaused/);
  assert.match(source, /clipboard\.writeText/);
  assert.match(source, /usb_clear_serial_log/);
  assert.match(source, /className="serial-log-page"/);
  assert.doesNotMatch(source, /modal-backdrop|role="dialog"|onClose/);
});

test("serial log viewer lets the user opt in or out of following the newest row", () => {
  assert.match(source, /followLatest/);
  assert.match(source, /aria-pressed=\{followLatest\}/);
  assert.match(source, /停止跟随最新日志/);
  assert.match(source, /if \(paused \|\| !followLatest/);
  assert.match(source, /const scrollToLatest = useCallback/);
  assert.match(source, /settledViewport\.scrollTop = settledViewport\.scrollHeight/);
  assert.match(source, /if \(!reset && incoming\.length === 0\) return current/);
  assert.match(source, /<footer className="serial-log-page__toolbar">/);
});

test("serial log toolbar stays outside the only scrolling viewport", () => {
  assert.match(appSource, /app-main--serial-log/);
  assert.match(appSource, /app-content--serial-log/);
  assert.match(styles, /\.app-main--serial-log\s*\{[\s\S]*?height: 100vh;[\s\S]*?overflow: hidden;/);
  assert.match(styles, /\.app-content--serial-log\s*\{[\s\S]*?overflow: hidden;/);
  assert.match(styles, /\.serial-log-page\s*\{[\s\S]*?min-height: 0;/);
  assert.match(styles, /\.serial-log-page__viewport\s*\{[\s\S]*?overflow: auto;/);
});

test("serial log commands are registered and allowed for the local app", () => {
  const allowed = localPermissions.permission[0].commands.allow;
  assert.ok(allowed.includes("usb_get_serial_log"));
  assert.ok(allowed.includes("usb_clear_serial_log"));
  assert.match(tauriLib, /usb_get_serial_log,/);
  assert.match(tauriLib, /usb_clear_serial_log,/);
});
