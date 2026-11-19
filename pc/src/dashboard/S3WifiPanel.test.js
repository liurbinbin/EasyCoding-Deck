/*
 * [Input] S3WifiPanel source and DeviceContext source.
 * [Output] Static safety contract for native-owned WSS, USB-only provisioning, pairing UX, and secret handling.
 * [Pos] test node in pc/src/dashboard.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const panel = readFileSync(join(here, "S3WifiPanel.jsx"), "utf8");
const context = readFileSync(join(here, "../shell/DeviceContext.jsx"), "utf8");
const dashboard = readFileSync(join(here, "../DeviceDashboard.jsx"), "utf8");
const nativeLib = readFileSync(join(here, "../../src-tauri/src/lib.rs"), "utf8");
const nativeUsb = readFileSync(join(here, "../../src-tauri/src/usb_serial.rs"), "utf8");
const nativeWifi = readFileSync(join(here, "../../src-tauri/src/s3_wifi.rs"), "utf8");
const secureConfig = readFileSync(join(here, "../../src-tauri/src/secure_config.rs"), "utf8");

test("S3 Wi-Fi UI delegates every physical connection to native commands", () => {
  assert.doesNotMatch(panel, /new WebSocket|WebSocket\(/);
  assert.doesNotMatch(context, /new WebSocket|WebSocket\(/);
  for (const command of ["s3_wifi_discover", "s3_wifi_configure", "s3_wifi_status", "s3_wifi_scan_networks", "s3_wifi_connect", "s3_wifi_forget"]) {
    assert.match(context, new RegExp(`invoke\\(["']${command}["']`));
  }
});

test("saved S3 pairings remain reachable after a Wi-Fi-only cold start", () => {
  assert.match(dashboard, /const showS3WifiPanel = isS3Runtime \|\| !usb\.connected/);
  assert.match(dashboard, /s3Wifi\?\.pairings/);
  assert.match(dashboard, /showS3WifiPanel && <Card title="S3 Wi-Fi 传输"/);
});

test("S3 Wi-Fi provisioning is USB and runtime gated", () => {
  assert.match(panel, /usb\.transport === "usb"/);
  assert.match(panel, /usb\.runtime === "esp32-s3-tkeyboard"/);
  assert.match(context, /请先通过 USB 连接目标 S3/);
  assert.match(panel, /type="password"[^>]*maxLength=\{64\}/);
  assert.match(panel, /const usbPairing = pairings\.find/);
  assert.match(panel, /setSelectedBoard\(event\.target\.value\);[\s\S]*?setManualEndpoint\(""\)/);
  assert.match(panel, /扫描周边 Wi-Fi/);
  assert.match(panel, /setPassword\(""\)/);
});

test("S3 Wi-Fi panel exposes live device network and handshake state", () => {
  assert.match(panel, /设备网络状态/);
  assert.match(panel, /已配网，尚未连接路由器/);
  assert.match(panel, /WSS 已连接，正在发送认证 challenge/);
  assert.match(panel, /deviceStatus\.ssid/);
  assert.match(panel, /deviceStatus\.ip/);
  assert.match(panel, /refreshS3WifiStatus/);
  assert.match(context, /invoke\("s3_wifi_status"/);
  assert.match(nativeWifi, /network\/challenge-request/);
  assert.match(nativeWifi, /等待设备 challenge/);
  assert.match(nativeWifi, /等待认证回执/);
  assert.match(nativeWifi, /等待认证后 hello/);
});

test("S3 Wi-Fi native protocol stays aligned with the firmware contract", () => {
  assert.match(nativeLib, /ack\.get\("tlsFingerprintSha256"\)/);
  assert.match(nativeUsb, /"network\/scan-result"/);
  assert.match(nativeWifi, /const MAX_TEXT_BYTES: usize = 12 \* 1024/);
  assert.match(nativeWifi, /nonce\.len\(\) != 32/);
  assert.match(nativeWifi, /host\.replace\('%', "%25"\)/);
  assert.match(nativeUsb, /observe_serial_capture\(capture_active\.as_ref\(\), &topic, &payload\)/);
  assert.match(nativeWifi, /hachimodock\.s3\.v1/);
});

test("pairing is recoverable and transport switching is serialized", () => {
  const start = nativeLib.indexOf("async fn s3_wifi_configure(");
  const end = nativeLib.indexOf("async fn s3_wifi_status(", start);
  const configure = nativeLib.slice(start, end);
  assert.ok(start >= 0 && end > start);
  assert.ok(configure.indexOf("save_pending_pairing") < configure.indexOf("manager.configure_s3_wifi"));
  assert.ok(configure.indexOf("manager.configure_s3_wifi") < configure.indexOf("save_pairing"));
  assert.match(configure, /clear_pending_pairing/);
  assert.match(panel, /放弃 pending 配对/);
  assert.match(nativeWifi, /static PAIRING_UPDATES: Mutex/);
  assert.match(nativeWifi, /records\.len\(\) >= 16/);
  assert.doesNotMatch(nativeWifi, /Connector::Plain/);
  assert.match(nativeUsb, /let raw_psk =[\s\S]*?password\.as_bytes\(\)\.len\(\) == 64/);
  assert.doesNotMatch(nativeUsb, /let ssid = ssid\.trim\(\)/);
  const wifiStart = nativeUsb.indexOf("async fn connect_wifi_profile(");
  const wifiEnd = nativeUsb.indexOf("fn dispatch_wifi_message(", wifiStart);
  const wifiConnect = nativeUsb.slice(wifiStart, wifiEnd);
  assert.ok(wifiConnect.indexOf("crate::s3_wifi::connect") < wifiConnect.indexOf("existing.cancel_reader.store"));
  assert.match(nativeUsb, /device transport transition is in progress; request not submitted/);
  assert.match(nativeUsb, /TransportTransitionGuard::acquire/);
  assert.match(nativeUsb, /transport_transition_active\.load/);
  assert.match(nativeUsb, /crate::s3_wifi::discover\(Duration::from_millis\(1200\)\)/);
  assert.match(nativeUsb, /wifi_reconnect_active\.store\(false, Ordering::Release\);[\s\S]*?schedule_wifi_reconnect\(\)/);
  assert.doesNotMatch(nativeUsb, /let response_scope = status_snapshot\.try_lock\(\)/);
  assert.match(nativeLib, /s3_wifi_pairing_operation\(\)\.lock\(\)\.await/);
  assert.match(secureConfig, /s3-wifi-pending\.secure\.json/);
});

test("S3 Wi-Fi production connect never enables insecure development mode", () => {
  assert.match(context, /allowInsecureDevelopment: false/);
  assert.match(panel, /mDNS 结果仅用于发现/);
  assert.match(panel, /证书指纹、HMAC challenge、board、boot 和 connection generation/);
});
