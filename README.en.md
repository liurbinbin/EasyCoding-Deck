<div align="center">
  <h1>EasyCoding Deck</h1>
  <p><strong>A five-screen, five-key desktop interface for AI Agents</strong></p>
  <p>Connect over USB or secure Wi-Fi to follow tasks, dictate to desktop Agents and talk with your desktop companion.</p>
  <p><a href="README.md">简体中文</a> · <a href="#quick-start">Quick start</a> · <a href="pc/README.md">Desktop development</a> · <a href="firmware-s3/README.md">Device firmware</a></p>
</div>

## Overview

**EasyCoding Deck** pairs a desktop client with ESP32-S3 firmware for **LILYGO T-Keyboard Voice V0.3**. The computer handles Agent integration, speech recognition, conversation and speech synthesis. The device handles five displays, keys, audio capture and playback, status LEDs and power management.

## Features

| Feature | How it works |
| --- | --- |
| Agent status | Follow working, waiting, completion and error states through companion animations, screen effects and status LEDs. |
| Five-key controls | Configure short-press, long-press and hold actions for sessions, confirmation, cancellation and voice. |
| Push-to-talk dictation | Hold the voice key to record, release to transcribe into an Agent draft, then confirm separately to send. |
| Real-time conversation | Talk through computer-hosted ASR, a conversation model and TTS, with device subtitles and audio playback. |
| Custom companions | Convert media into 128×128 RGB565 animations with up to 14 action states persisted across power cycles. |
| Approval input | Supported adapters show ALLOW/DENY and accept a decision from the configured confirm/back keys. |
| USB / Wi-Fi | USB CDC for wired operation and initial provisioning; paired LAN WSS for status, input, audio and avatar transfers. |
| Power and diagnostics | Check battery, charging and connection status, adjust volume, and configure power-off or deep sleep. |

Status, draft insertion, confirmed sending and tool approval are separate capabilities. See the [Agent integration matrix](pc/docs/agent-integration-audit.md). Device features are enabled from the actual capabilities in its `hello` response.

## Hardware

| Module | Configuration |
| --- | --- |
| MCU | ESP32-S3 N16R8, 240 MHz |
| Storage | 16 MiB Flash, 8 MiB OPI PSRAM |
| Displays | Five GC9107 panels, 128×128 each, shared SPI with individual chip selects |
| Input | Five physical keys |
| Capture | ES7210, MIC1 + MIC2 by default; a third channel when AUX is detected |
| Playback | ES8389; 16 kHz mono PCM16 input, 48 kHz stereo output |
| Status LEDs | 14 WS2812 LEDs |
| Power | AXP2101, battery and charging telemetry, power-off and deep sleep |
| Transport | Native USB CDC and 2.4 GHz Wi-Fi / WSS |

See the [board definition](firmware-s3/boards/t_keyboard_voice_esp32s3.json) and [pin_config.h](firmware-s3/include/pin_config.h).

### Display layout

1. **Screen 1:** companion avatar and animation.
2. **Screen 2:** current key-action hints.
3. **Screen 3:** messages, dictation feedback and conversation subtitles.
4. **Screen 4:** control hints, including default back/cancel actions.
5. **Screen 5:** current Agent and task status.

Screen content follows key bindings and interaction state. Approval prompts appear on the panels mapped to confirm and back.

## Quick start

### 1. Connect over USB

1. Prepare a matching EasyCoding Deck desktop client and device firmware. Connect with a data-capable USB cable.
2. Launch the full desktop application. Confirm the device connection and check firmware and audio status.
3. Select the Agent to follow. For dictation, open the target conversation and grant required accessibility permissions.
4. Enter speech and conversation credentials in settings. Configure the companion and voice, then synchronize its avatar.
5. Validate keys, status and short dictation before optionally configuring Wi-Fi.

Supply your own service API keys. Speech, model and media-generation usage may incur charges.

### 2. Default controls

Persisted user bindings take precedence. K1–K5 correspond to SW1–SW5 in the protocol.

| Gesture | Default action |
| --- | --- |
| K1 short press | Confirm / send; allow a pending approval |
| Hold K1, then release | Start capture after approximately 500 ms; release to finish and transcribe |
| K2 short press | Previous session |
| K3 short press | Next session |
| K4 short press | Back / cancel; deny a pending approval |
| K4 long press, then release | Power off after the default 3 seconds; configurable from 2–10 seconds or as deep sleep |
| K5 short press | Cycle companion animation states |
| K5 long press, then release | Start / stop real-time conversation |

K4 long-press belongs to power management. Confirming a voice draft is different from approving an Agent tool request.

### 3. Configure Wi-Fi

1. Keep the device connected over USB and open **S3 Wi-Fi transport** on the device home page.
2. Scan nearby networks or enter an SSID, provide the 2.4 GHz password, and provision/pair over USB.
3. Ensure the computer and device can reach each other on the LAN. Check IP, RSSI and network status.
4. Select the paired device. If mDNS is unavailable, enter `wss://<device-IP>:8443/device/v1`.
5. Connect and wait for an authenticated connection. Battery or external power is still required after removing USB.

Pairing uses a certificate SHA-256 fingerprint obtained over USB and HMAC-SHA256 challenge-response. Discovery is not authentication; do not disable certificate checks. The desktop client must remain running over Wi-Fi.

Only one business connection owner is active at a time. Switching connections or disconnecting terminates current capture, playback, approval and uncommitted avatar transfers. Transactions do not migrate between connections, and unknown-result writes are not automatically replayed. Use USB to forget device-side provisioning credentials.

## Repository structure

```text
.
├── pc/                         Desktop client, Agent bridge and resource tools
│   ├── src/                    React UI and frontend tests
│   ├── src-tauri/              Rust native layer and Node bridge
│   ├── public/                 Avatar, audio and static resources
│   ├── builtin-clawpkgs/       Bundled component resources
│   ├── builtin-catalog.json    Component order and avatar metadata
│   ├── runtime-core/           Bounded C cores for desktop previews
│   ├── scripts/                Resource preparation, packaging and checks
│   ├── skills/                 Component authoring tools and contracts
│   └── tests/                  Fixtures and acceptance tools
├── firmware-s3/                ESP32-S3 device firmware
│   ├── src/                    Display, input, audio, network and power
│   ├── include/                Module interfaces and pin definitions
│   ├── boards/                 Board configuration
│   ├── tests/                  Transport contract tests
│   └── tools/                  Serial hardware-check tools
├── integrations/               Desktop application integration plugins
├── server/community/web/       Static community frontend
├── docs/                       Architecture, networking and acceptance records
└── assets/                     Project images
```

React submits actions and renders state. Rust owns device identity, USB/WSS, connection generations, receipts and audio lifecycles. The Node bridge handles local Agent integration. Agent Session Bus, hook HTTP and DSH services remain loopback-only.

## Development

### Desktop client

Requirements: Node.js 22+, Rust stable and Tauri 2 platform dependencies. Windows needs MSVC C++ Build Tools and WebView2; macOS needs Xcode Command Line Tools.

```sh
cd pc
npm ci
npm run dev:web
```

`dev:web` is UI preview only. Full desktop development also requires relocatable Node, audited FFmpeg and bridge dependencies. See the [desktop guide](pc/README.md) and [native runtime resources](pc/docs/runtime-dependencies.md).

### Device firmware

Build from the repository root:

```sh
pio run -d firmware-s3 -e t_keyboard_voice
```

Toolchains and dependencies are pinned in `firmware-s3/platformio.ini`. USB monitoring uses **115200 baud**; upload speed is **921600 baud**. See [partitions.csv](firmware-s3/partitions.csv) for the Flash layout and back up needed data before flashing.

### Non-compiling checks

Run from the repository root:

```sh
python -I firmware-s3/tests/test_transport_contract.py
node --test pc/src/dashboard/S3WifiPanel.test.js pc/src/s3-realtime-streaming.test.js
node pc/scripts/check-source-safety.mjs
```

These checks do not connect to hardware or establish firmware-build, performance or acoustic acceptance. See the [firmware guide](firmware-s3/README.md) for hardware-check tools and their side effects.

## FAQ

**Can I use it without Wi-Fi?** Yes. USB supports normal device operation and initial provisioning.

**Can I close the client over Wi-Fi?** Live status, recognition, conversation and synthesis require the computer-side client to keep running.

**Can I talk during speech playback?** Conversation is half-duplex. Capture pauses during playback and resumes after the reply.

**Does the device play MP4 directly?** The computer converts avatar media into RGB565 frames before synchronizing it to the device.

**Why is a discovered device unreachable?** Check USB pairing, LAN reachability, port 8443 and authentication status. Manual IP entry handles discovery, not authentication.

**How should I report a problem?** Include client and firmware versions, operating system, transport, reproduction steps and sanitized errors. Report security issues privately under [SECURITY.md](SECURITY.md). Do not publish keys, Wi-Fi passwords, pairing secrets, recordings or private conversations.

## License and acknowledgements

See [LICENSE](LICENSE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Thanks to project contributors and to LILYGO, Espressif, Arduino, LovyanGFX, ArduinoJson, SensorLib, Tauri, React, Node.js, FFmpeg, Lucide and Clawd on Desk for hardware, tools and software foundations.
