// 源码契约与数值边界检查，不替代 Rust/固件运行或实机声学验证。
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
const read = (name) => readFileSync(new URL(name, import.meta.url), "utf8");
const rtc = read("../src-tauri/src/realtime_chat.rs");
const wifi = read("../src-tauri/src/s3_wifi.rs");

test("S3 USB/WSS share short PCM chunks and an ACK-bounded burst", () => {
  assert.match(rtc, /S3_PLAY_CHUNK_BYTES: usize = 3200/);
  assert.match(rtc, /S3_PLAYBACK_ACK_WINDOW: usize = 8/);
  assert.match(read("../src-tauri/src/s3_wifi.rs"), /DEVICE_HEARTBEAT/);
  assert.match(rtc, /let chunk_bytes = if s3 \{ S3_PLAY_CHUNK_BYTES \} else \{ PLAY_CHUNK_BYTES \}/);
  const chunk = rtc.split("let flush_chunk =")[1].split("while let Ok((expected, cmd))")[0];
  assert.match(chunk, /pending_playback_chunks\(&session.id, turn_id\)/);
  assert.match(chunk, /session.cancelled.load/);
  assert.match(chunk, /playback_ack_timeout/);
  assert.ok(chunk.indexOf("pending_playback_chunks") < chunk.indexOf("send_session_board("));
  for (const bytes of [2, 128, 640, 1600, 3198, 3200, 4096]) {
    const pcm = Buffer.alloc(bytes, 127);
    const encoded = pcm.toString("base64");
    assert.equal(encoded.length, Math.ceil(bytes / 3) * 4);
    assert.deepEqual(Buffer.from(encoded, "base64"), pcm);
  }
});

test("slow WSS writes do not share the read loop and ingress backpressure retains the frame", () => {
  const io = wifi.split("let (mut sink, mut stream) = socket.split();")[1].split("Ok(ConnectedWifi")[0];
  assert.equal((io.match(/tokio::spawn\(async move/g) || []).length, 2);
  const [writer, reader] = io.split("let reader_cancel");
  assert.match(writer, /timeout\(Duration::from_secs\(10\), sink.send/);
  assert.doesNotMatch(writer, /stream.next/);
  assert.match(reader, /stream.next/);
  assert.doesNotMatch(reader, /sink.send|inbound_tx.try_send/);
  assert.match(reader, /timeout\(INBOUND_DISPATCH_TIMEOUT, inbound_tx.send\(frame\)\)/);
});

test("retired connections finish locally without fabricating an idle receipt", () => {
  const refresh = rtc.split("fn refresh_device_stop_metadata")[1].split("fn active_session")[0];
  assert.match(refresh, /else if !transport_current\(session\)/);
  assert.match(refresh, /status.active=false;status.state="ended"/);
  assert.match(refresh, /"connection_retired"/);
  const finish = rtc.split("async fn finish(")[1].split("// ---- preview")[0];
  assert.ok(finish.indexOf("if !transport_current(session)") < finish.indexOf("attempt_terminal_controls(session"));
  assert.match(finish, /"stop_unconfirmed"/);
  assert.match(finish, /"connection_retired"/);
  const panel = read("./dashboard/RealtimeChatPanel.jsx");
  assert.match(panel, /stopping: "正在停止"/);
  assert.match(panel, /stop_unconfirmed: "设备停止待确认"/);
});
