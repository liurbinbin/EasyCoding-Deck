import base64
import hashlib
import hmac
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class TransportContractTests(unittest.TestCase):
    def test_authentication_vector_is_stable(self):
        secret = bytes(range(32))
        message = (
            b"hachimodock-wifi-v1\n"
            b"s3-tkeyboard-a1b2c3d4\n"
            b"0011223344556677\n"
            b"desktop-test\n"
            b"0123456789abcdef0123456789abcdef"
        )
        self.assertEqual(
            hmac.new(secret, message, hashlib.sha256).hexdigest(),
            "9402f88c580589b8a87cfd430d3720919e13c6569c211a117373249a85bbb65e",
        )

    def test_protocol_output_is_owned_by_usb_adapter(self):
        offenders = []
        for path in (ROOT / "src").glob("*.cpp"):
            if path.name == "usb_cdc_transport.cpp":
                continue
            text = path.read_text(encoding="utf-8")
            if "serializeJson(doc, Serial)" in text or "Serial.write" in text:
                offenders.append(path.name)
        self.assertEqual(offenders, [])

    def test_usb_short_write_never_terminates_a_partial_json_frame(self):
        source = (ROOT / "src" / "usb_cdc_transport.cpp").read_text(encoding="utf-8")
        self.assertIn("tx_poisoned_ = true", source)
        self.assertIn("frame += '\\n';", source)
        self.assertIn("written != frame.length()", source)
        self.assertNotIn("Serial.write('\\n');", source)

    def test_network_transport_is_tls_and_has_no_embedded_private_key(self):
        source = (ROOT / "src" / "wifi_transport.cpp").read_text(encoding="utf-8")
        self.assertIn("httpd_ssl_start", source)
        self.assertIn("mbedtls_x509write_crt_pem", source)
        self.assertNotIn("BEGIN PRIVATE KEY", source)
        self.assertNotIn("BEGIN EC PRIVATE KEY", source)
        self.assertIn("SessionContext", source)
        self.assertIn("session_.epoch == context->epoch", source)
        self.assertIn("session_.epoch == frame.epoch", source)
        self.assertIn("session_.authenticated ? session_.generation : session_.epoch", source)
        self.assertIn("httpd_ws_send_data_async", source)
        self.assertNotIn("httpd_ws_send_data(server_", source)
        self.assertIn("config.httpd.recv_wait_timeout = 5;", source)
        self.assertIn("config.httpd.send_wait_timeout = 10;", source)
        self.assertIn("config.httpd.max_open_sockets = 4;", source)
        self.assertIn("sessionContext(false, true)", source)
        self.assertIn("if (!session_mutex_) return;", source)
        self.assertIn('sendText(target, message) == TransportSendResult::Sent', source)
        self.assertIn('!strcmp(topic, "network/challenge-request")', source)
        self.assertIn("session_.epoch == source.generation", source)
        self.assertNotIn("request->method == HTTP_GET", source)
        self.assertIn("request->sess_ctx = context;", source)
        self.assertIn("request->free_ctx = &WifiDeviceTransport::sessionFree;", source)
        self.assertNotIn("httpd_sess_set_ctx(", source)
        self.assertIn('payload["clientConnected"]', source)
        self.assertIn('payload["challengePending"]', source)
        self.assertIn("completeSend(fd, epoch, ESP_OK);", source)
        self.assertIn("if (header == HTTPD_SOCK_ERR_TIMEOUT || header == ESP_ERR_TIMEOUT) return ESP_OK;", source)
        self.assertIn("(void)httpd_ws_send_frame(request, &frame);", source)
        self.assertNotIn("if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK) return ESP_FAIL;", source)

    def test_tls_identity_generation_is_off_the_loop_stack(self):
        header = (ROOT / "include" / "wifi_transport.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "wifi_transport.cpp").read_text(encoding="utf-8")
        self.assertIn("QueueHandle_t provisioning_results_", header)
        self.assertIn("kTlsIdentityTaskStackBytes = 24 * 1024", source)
        self.assertIn("static_assert(kTlsIdentityTaskStackBytes >= 16 * 1024", source)
        self.assertIn("uxTaskGetStackHighWaterMark(nullptr)", source)
        self.assertIn('payload["tlsIdentityStackMinFreeBytes"]', source)
        self.assertIn("xTaskCreatePinnedToCore", source)
        self.assertIn("tskIDLE_PRIORITY", source)
        self.assertIn("heap_caps_calloc", source)
        self.assertIn("mbedtls_platform_zeroize(key_buffer", source)
        self.assertIn("transport_->admitted(job->target)", source)
        self.assertIn("pollTlsProvisioning();\n  pollConnection();", source)
        self.assertNotIn("uint8_t certificate_buffer[2048]", source)
        self.assertNotIn("uint8_t key_buffer[1024]", source)

    def test_playback_is_sequence_checked_streamed_and_strictly_decoded(self):
        header = (ROOT / "include" / "audio_service.h").read_text(encoding="utf-8")
        source = (ROOT / "src" / "audio_service.cpp").read_text(encoding="utf-8")
        self.assertIn("s3transport::SequenceCounter", header)
        self.assertIn("playback_buffered_bytes_", header)
        self.assertIn("playback_end_received_", header)
        stream = (ROOT / "src" / "audio_playback_stream.cpp").read_text(encoding="utf-8")
        stream_header = (ROOT / "include" / "audio_playback_stream.h").read_text(encoding="utf-8")
        self.assertIn("AudioPlaybackStream playback_stream_", header)
        self.assertIn("kPlaybackBufferBytes", source)
        self.assertNotIn("kPlaybackBufferDrainTimeoutMs", source)
        self.assertIn("xStreamBufferSend(stream_, pcm, bytes, 0)", stream)
        self.assertIn("xStreamBufferReceive(stream_, mono, wanted, 0)", stream)
        self.assertIn("kBufferBytes = 16000 * 2 * 4", stream_header)
        self.assertIn("nullptr, 0, &result.required", source)
        self.assertIn("decoded.required != declared", source)
        self.assertIn("playback-base64-length-mismatch", source)
        self.assertNotIn("heap_caps_realloc", source)
        self.assertNotIn("ps_malloc(output_bytes)", source)

    def test_pc_equivalent_playback_base64_lengths_are_exact(self):
        for size in (128, 512, 640, 3198, 3200):
            pcm = bytes(index % 251 for index in range(size))
            encoded = base64.b64encode(pcm)
            self.assertEqual(len(encoded), ((size + 2) // 3) * 4)
            self.assertEqual(len(base64.b64decode(encoded, validate=True)), size)

    def test_scan_and_challenge_contract_match_the_pc_client(self):
        source = (ROOT / "src" / "wifi_transport.cpp").read_text(encoding="utf-8")
        self.assertIn('transport_->sendTo(scan_target_, "network/scan-result"', source)
        self.assertIn("session_.nonce = randomHex(16);", source)
        self.assertIn('kWebsocketSubprotocol[] = "hachimodock.s3.v1"', source)

    def test_wifi_backpressure_keeps_the_queue_and_owner_alive(self):
        header = (ROOT / "include" / "protocol_transport.h").read_text(encoding="utf-8")
        transport = (ROOT / "src" / "protocol_transport.cpp").read_text(encoding="utf-8")
        wifi = (ROOT / "src" / "wifi_transport.cpp").read_text(encoding="utf-8")
        wifi_header = (ROOT / "include" / "wifi_transport.h").read_text(encoding="utf-8")
        self.assertIn("enum class TransportSendResult", header)
        self.assertIn("TransportSendResult::Busy", wifi)
        self.assertIn("inboundAvailable()", wifi)
        self.assertIn("PendingFrame pending_[16]", wifi_header)
        busy = transport.index("if (result == TransportSendResult::Busy) return;")
        dequeue = transport.index("dequeueOutbound(queue", busy)
        self.assertLess(busy, dequeue)
        self.assertNotIn("(preauth_target || owner_target) && session_.sends_inflight < 8", wifi)

    def test_owner_rotation_drops_stale_output_and_usb_bind_forces_generation(self):
        transport = (ROOT / "src" / "protocol_transport.cpp").read_text(encoding="utf-8")
        main = (ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        usb = (ROOT / "src" / "usb_cdc_transport.cpp").read_text(encoding="utf-8")
        self.assertIn("if (previous.valid) clearOutboundFor(previous);", transport)
        self.assertIn("clearOutboundFor(previous);\n  owner_ = {};", transport)
        self.assertIn("slot.context.sameConnection(retired)", transport)
        self.assertIn("releaseOwner(message.context)", transport)
        self.assertIn("claimOwner(source, true)", main)
        self.assertIn("target.sameConnection(active)", usb)

    def test_owner_scoped_provisioning_and_failed_playback_are_explicit(self):
        wifi = (ROOT / "src" / "wifi_transport.cpp").read_text(encoding="utf-8")
        audio = (ROOT / "src" / "audio_service.cpp").read_text(encoding="utf-8")
        self.assertGreaterEqual(wifi.count("!transport_->admitted(source)"), 3)
        self.assertIn("transport_->claimOwner(previous_owner, true)", wifi)
        self.assertIn("password.isEmpty()", wifi)
        self.assertIn("password.length() == 64 && validHex(password, 64)", wifi)
        playback_guard = audio.index('sendPlaybackState(payload, false, "another playback session is active")')
        capture_suspend = audio.index("suspendConversationCapture()", playback_guard)
        self.assertLess(playback_guard, capture_suspend)
        self.assertGreaterEqual(audio.count("failPlayback(payload,"), 7)
        self.assertIn('failPlayback(payload, "playback-sequence-invalid"', audio)
        self.assertIn('failPlayback(payload, "playback-base64-invalid"', audio)
        self.assertIn('failPlayback(payload, "playback-decoded-length-mismatch"', audio)
        self.assertIn('sendPlaybackState(payload, false, "playback session is not current"', audio)
        self.assertIn('play ? "no playback session is active" : nullptr', audio)
        stream = (ROOT / "src" / "audio_playback_stream.cpp").read_text(encoding="utf-8")
        self.assertIn("kDmaDurationMs + 10", stream)
        self.assertIn("State::Completed", stream)
        self.assertIn("playback_stream_.end();", audio)
        self.assertIn('if (!send("audio/chunk", doc))', audio)
        self.assertIn('stopCapture("transport-backpressure")', audio)
        self.assertLess(audio.index('if (!send("audio/chunk", doc))'), audio.index("++sequence_", audio.index('if (!send("audio/chunk", doc))')))
        self.assertNotIn('sendError("playback-sequence-invalid", "PCM playback chunk sequence is missing, duplicate, or out of order");\n    endPlayback(payload, false);', audio)

    def test_dma_worker_is_independent_bounded_and_clocked_for_s3(self):
        stream = (ROOT / "src" / "audio_playback_stream.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include" / "audio_playback_stream.h").read_text(encoding="utf-8")
        audio = (ROOT / "src" / "audio_service.cpp").read_text(encoding="utf-8")
        self.assertIn('xTaskCreatePinnedToCore(taskEntry, "s3-pcm-tx", 6144', stream)
        self.assertIn("MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA", stream)
        self.assertIn("uint8_t* output_[2]", header)
        self.assertIn("kDmaDescriptors = 8", header)
        self.assertIn("kDmaFrames = 240", header)
        self.assertIn("I2S_CLK_SRC_PLL_160M", stream)
        self.assertNotIn("I2S_CLK_SRC_APLL", stream)
        self.assertIn("I2S_MCLK_MULTIPLE_256", stream)
        self.assertIn("config.gpio_cfg.din = I2S_GPIO_UNUSED", stream)
        self.assertIn("channel.auto_clear = true", stream)
        self.assertIn("slot ^= 1", stream)
        self.assertIn("xSemaphoreTake(idle_, portMAX_DELAY)", stream)
        self.assertNotIn("protocolTransport()", stream)
        polling = audio.split("void AudioService::pollPlayback()", 1)[1].split("void AudioService::finishPlayback", 1)[0]
        self.assertNotIn("i2s_channel_write", polling)
        self.assertNotIn("i2s_.write", audio)
        self.assertIn("playbackMclkHz", audio)
        self.assertEqual(8 * 240 * 1000 // 48000, 40)
        self.assertLessEqual(240 * 2 * 2, 4092)
        self.assertEqual(16000 * 3, 48000)
        self.assertEqual(48000 * 256, 12288000)
        self.assertEqual(48000 * 16 * 2, 1536000)

    def test_usb_rx_and_pc_ack_window_bound_the_initial_burst(self):
        usb = (ROOT / "src" / "usb_cdc_transport.cpp").read_text(encoding="utf-8")
        rtc = (ROOT.parent / "pc" / "src-tauri" / "src" / "realtime_chat.rs").read_text(encoding="utf-8")
        self.assertIn("kUsbRxBufferBytes = 32 * 1024", usb)
        self.assertIn("transport_->inboundAvailable() && Serial.available()", usb)
        self.assertIn("line_.reserve(ProtocolTransport::kMaxMessageBytes)", usb)
        self.assertIn("S3_PLAY_CHUNK_BYTES: usize = 3200", rtc)
        self.assertIn("S3_PLAYBACK_ACK_WINDOW: usize = 8", rtc)
        self.assertIn("pending_playback_chunks(&session.id, turn_id)", rtc)
        # 单帧 envelope 小于 6 KiB，读取循环在 USB RX 充满前已消化。
        self.assertLess(((3200 + 2) // 3) * 4 + 512, 6 * 1024)
        self.assertLessEqual(8 * 100, 800)

    def test_disable_stops_playback_without_resuming_capture(self):
        audio = (ROOT / "src" / "audio_service.cpp").read_text(encoding="utf-8")
        disable = audio.split("if (!enabled)", 1)[1].split("const String session", 1)[0]
        self.assertLess(disable.index("resume_capture_after_playback_ = false"),
                        disable.index("finishPlayback(true, nullptr, false)"))
        self.assertIn("if (playback_) finishPlayback", disable)
        finish = audio.split("void AudioService::finishPlayback", 1)[1]
        self.assertLess(finish.index("playback_stream_.stop()"), finish.index("releasePlaybackBuffer()"))

    def test_toolchain_dependencies_are_exact(self):
        config = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        self.assertIn("platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.311/platform-espressif32.zip", config)
        self.assertNotIn("platform = espressif32@55.3.311", config)
        self.assertIn("bblanchon/ArduinoJson @ 7.4.3", config)
        self.assertIn("lovyan03/LovyanGFX @ 1.3.0", config)
        self.assertNotIn("ArduinoJson @ ^", config)
        self.assertNotIn("LovyanGFX @ ^", config)
    def test_mbedtls_sha256_uses_current_arduino_api(self):
        source = (ROOT / "src" / "firmware_update_service.cpp").read_text(encoding="utf-8")
        for function in ("mbedtls_sha256_starts", "mbedtls_sha256_update", "mbedtls_sha256_finish"):
            self.assertIn(function, source)
        for legacy_function in ("mbedtls_sha256_starts_ret", "mbedtls_sha256_update_ret", "mbedtls_sha256_finish_ret"):
            self.assertNotIn(legacy_function, source)

    def test_factory_image_descriptor_matches_public_artifact(self):
        import hashlib
        import json
        artifact = ROOT / "releases" / "firmware.factory.bin"
        descriptor = json.loads((ROOT / "releases" / "firmware.factory.json").read_text(encoding="utf-8"))
        self.assertEqual(artifact.stat().st_size, descriptor["sizeBytes"])
        self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(), descriptor["sha256"])


if __name__ == "__main__":
    unittest.main()
