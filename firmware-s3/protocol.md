# T-Keyboard Voice transport protocol

## Envelope and bounds

USB CDC uses UTF-8 JSON Lines. WSS uses one UTF-8 WebSocket text message per envelope:

```json
{"topic":"network/status-query","payload":{"requestId":"req-1"}}
```

Maximum serialized envelope size is 12288 bytes. WebSocket fragments are accumulated only up to that limit. Binary frames, malformed JSON and messages from an unauthenticated or retired connection are rejected.

## Trusted connection scope

`transport kind`, `peer id`, `connection generation` and authentication state are local metadata. A remote JSON object cannot provide or override them. Device-generated asynchronous events are sent only to the current owner. Responses generated while dispatching a request return to that request's connection.

USB `bind` establishes a fresh owner generation. A successful Wi-Fi authentication establishes another generation. Owner replacement or disconnect aborts the old generation's capture, playback, approval and avatar staging state.

## USB-only provisioning

### `network/scan`

Request payload contains `requestId`. An accepted scan first emits `network/scan-state` with `state: "scanning"`; the terminal `network/scan-result` uses the same request ID and contains only SSID, RSSI, channel and authentication mode. Busy, rejected and failed scans also terminate with `network/scan-result`.

### `network/config`

```json
{
  "topic": "network/config",
  "payload": {
    "requestId": "cfg-1",
    "ssid": "example",
    "password": "not-returned-by-device",
    "pairSecret": "64 hexadecimal characters"
  }
}
```

The command is accepted only over authenticated USB. First-time P-256/X.509 identity generation runs asynchronously on a bounded dedicated task; the immediate generic `protocol/ack` remains receipt-only and the terminal result is `network/config-ack`. Before persistence, completion revalidates the exact USB connection generation. Concurrent config/forget requests terminate with `provisioning_busy`. `network/config-ack` returns `configured`, `secure` and `tlsFingerprintSha256`, but never returns the password, pair secret or private key.

### `network/forget`

Accepted only over authenticated USB. It clears SSID/password, pair secret, TLS certificate/private key and active network sessions.

### `network/status-query`

Accepted over USB or the active authenticated Wi-Fi owner. `network/status` reports connection state, address, RSSI, WSS port, service name, certificate fingerprint, `clientConnected`, `challengePending` and `authenticated`. After an identity-generation attempt it also reports `tlsIdentityStackMinFreeBytes`, the task's historical minimum free stack in bytes for regression diagnosis.

## WSS authentication

Endpoint: `/device/v1`  
Subprotocol: `hachimodock.s3.v1`  
Service: `_hachimodock-s3._tcp.local`  
Port: 8443

The PC first pins the self-signed leaf certificate SHA-256 fingerprint received over USB. After WebSocket upgrade:

1. ESP-IDF completes the WebSocket upgrade before invoking the application handler. The PC therefore sends `network/challenge-request` as its first frame; when that frame reaches the handler, the device creates an fd+epoch session, consumes the same frame and sends `network/challenge`. The adapter keeps retrying until the HTTP server accepts the asynchronous challenge frame, and merely queuing a protocol message is not treated as delivery. The challenge contains `boardDeviceId`, `bootId`, a 128-bit random nonce and a 10-second expiry.
2. PC sends `network/auth` with `requestId`, `clientId`, nonce and proof.
3. Proof is the hexadecimal HMAC-SHA256 of the exact UTF-8 bytes below, keyed by the 32-byte decoded pair secret:

```text
hachimodock-wifi-v1\n<boardDeviceId>\n<bootId>\n<clientId>\n<nonce>
```

4. Device performs a constant-time proof comparison. Three failures close the session.
5. Successful `network/auth-ack` includes the connection generation; the device then sends `hello` over the new owner.

mDNS fields, board ID, boot ID, FNV checksums and the certificate fingerprint are not client credentials.

## ACK semantics

`protocol/ack` is a receipt only:

```json
{
  "topic": "protocol/ack",
  "payload": {
    "requestId": "r1",
    "requestTopic": "audio/play_chunk",
    "ok": true,
    "stage": "received",
    "completed": false,
    "code": "received"
  }
}
```

Execution, persistence and playback completion are proven by their service-specific response or diagnostic. Timeout or disconnect is an unknown result and must not trigger automatic replay of side effects.

## Audio and avatar ownership

- `audio/play_chunk.seq` starts at 0 and increments by one. Missing, duplicate and out-of-order chunks abort playback.
- PCM is 16 kHz mono S16LE. Each chunk is at most 4096 decoded bytes and must contain standard padded Base64 whose encoded and decoded lengths exactly match the declared even `bytes` value.
- S3 initializes the codec at `audio/play_begin`. A dedicated priority-4 audio task consumes a fixed 128000-byte PSRAM stream buffer, starts after 200 ms of prebuffer (or `play_end` for a shorter reply), and rebuffers 100 ms after underrun. The dispatcher never waits for playback drain; it validates and admits complete chunks only. The buffer is retained for reuse across replies. PCM is released as the task consumes it, not retained for the whole reply.
- `audio/play_end.bytes` must equal the accumulated decoded byte count. The stream has a bounded five-minute total byte limit, but no longer has the old 15-second whole-reply allocation limit.
- Sequence, size, ring-capacity and Base64 failures emit `audio/error` followed by terminal `audio/play-state` with `ok: false`; the playback buffer is released. Base64 errors include only declared/encoded/decoded lengths, mbedTLS status and transport kind, never PCM or Base64 contents.
- Stale chunks and invalid or inactive `audio/play_end` requests receive an explicit failed `audio/play-state`. An inactive `audio/play_flush` is an idempotent success.
- Playback uses two internal DMA-capable 1920-byte staging blocks (160 mono samples expanded to 10 ms of 48 kHz stereo) and a TX-only native I2S channel with 8 descriptors × 240 frames = 7680 bytes / 40 ms. DMA auto-clear prevents repeating stale speech on underrun. `play_end` completion waits the full DMA duration plus 10 ms; flush/cancel waits for the worker to quiesce before changing the shared codec or freeing the decode block.
- The S3 clock source is explicitly PLL 160 MHz (not APLL), with 48 kHz stereo, 16-bit slots and 256× MCLK: nominal MCLK 12.288 MHz, BCLK 1.536 MHz, WS 48 kHz. `audio/status` reports driver source/MCLK/BCLK and DMA sizes, plus buffered bytes and underruns; these are driver calculations, not physical frequency measurements.
- PC S3 playback uses 3200-byte / 100 ms chunks, at most eight unacknowledged chunks (matching the device's eight asynchronous WSS send slots and covering the 600 ms lead) and a 600 ms time lead over USB or WSS. USB RX is 32 KiB with a 16 KiB poll budget; it stops consuming when the protocol queue is full. Base64 length failures remain fatal and report lengths, never guessed padding or automatic replay. The WSS server uses 5 s receive / 10 s send timeouts and four sockets so a single Wi-Fi retransmission cannot be misread as a dead session.
- `audio/conversation` disable independently cancels playback without resuming capture, even if a preceding flush did not arrive.
- Wi-Fi inbound queues retain pending frames while the protocol queue is full. Outbound async-send saturation returns `Busy` and keeps the protocol queue head; only a fatal send result releases the owner.
- An asynchronous WSS send callback error is treated as an unknown socket/frame result: the device retires and closes that session, releases its owner, and lets the PC reconnect. It never replays the frame. Keeping a failed async socket alive would strand ACK waiters and block the next challenge behind a dead fd.
- The device validates the existing WebSocket fd before admitting a new client. If HTTPD has not yet run `sessionFree` but the old fd is no longer `HTTPD_WS_CLIENT_WEBSOCKET`, the stale session is cleared and the new challenge is admitted; a live old client remains rejected by the single-owner rule.
- The PC sends an application `system/heartbeat` text envelope every 4 s in addition to normal WebSocket traffic. This refreshes the device's authenticated lease even when half-duplex realtime chat is waiting for the user; WebSocket control Ping alone may be consumed by the HTTP framework before the application handler sees it. The authenticated lease is 120 s as a failure safety net, not the normal keepalive path.
- Avatar begin records the current connection generation. Chunk/end from another generation aborts staging.
- Avatar staging and playback begin are mutually exclusive to bound PSRAM use.
