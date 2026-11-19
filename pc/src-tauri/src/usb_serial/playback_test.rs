//! Exact-board, capability-gated one-off WAV playback for hardware testing.
use super::*;
use base64::engine::general_purpose::STANDARD as B64;

const ENGINE: &str = "usb-pcm16-test-v1";
const RESPONSE_TOPIC: &str = "audio/play-state";
const ACK_TOPIC: &str = "protocol/ack";
const MAX_PCM_BYTES: usize = 16_000 * 2 * 15;
const CHUNK_BYTES: usize = 128;
const CHUNKS_PER_BATCH: usize = 8;

#[derive(Clone)]
struct ActivePlayback {
    board: String,
    session: String,
    cancelled: Arc<AtomicBool>,
}

fn active() -> &'static Mutex<Option<ActivePlayback>> {
    static ACTIVE: OnceLock<Mutex<Option<ActivePlayback>>> = OnceLock::new();
    ACTIVE.get_or_init(|| Mutex::new(None))
}

fn board(value: &str) -> Result<(), String> {
    if value.is_empty() || value.len() > 64 || value.trim() != value || value.chars().any(char::is_control) {
        return Err("invalid expectedBoardDeviceId".into());
    }
    Ok(())
}

fn wav_pcm(bytes: &[u8]) -> Result<Vec<u8>, String> {
    if bytes.len() < 44 || &bytes[..4] != b"RIFF" || &bytes[8..12] != b"WAVE" {
        return Err("请选择有效的 WAV 文件".into());
    }
    let mut at = 12usize;
    let mut format = None;
    let mut data = None;
    while at + 8 <= bytes.len() {
        let size = u32::from_le_bytes(bytes[at + 4..at + 8].try_into().unwrap()) as usize;
        let start = at + 8;
        let end = start.checked_add(size).filter(|end| *end <= bytes.len()).ok_or("WAV 数据块损坏")?;
        match &bytes[at..at + 4] {
            b"fmt " if size >= 16 => {
                format = Some((
                    u16::from_le_bytes(bytes[start..start + 2].try_into().unwrap()),
                    u16::from_le_bytes(bytes[start + 2..start + 4].try_into().unwrap()),
                    u32::from_le_bytes(bytes[start + 4..start + 8].try_into().unwrap()),
                    u16::from_le_bytes(bytes[start + 14..start + 16].try_into().unwrap()),
                ));
            }
            b"data" => data = Some(bytes[start..end].to_vec()),
            _ => {}
        }
        at = end.checked_add(size & 1).ok_or("WAV 数据块溢出")?;
    }
    if format != Some((1, 1, 16_000, 16)) {
        return Err("WAV 必须转换为 16 kHz、单声道、16 位 PCM".into());
    }
    let pcm = data.ok_or("WAV 缺少音频数据")?;
    if pcm.is_empty() || pcm.len() & 1 != 0 || pcm.len() > MAX_PCM_BYTES {
        return Err("WAV 播放长度须在 0 到 15 秒之间".into());
    }
    Ok(pcm)
}

fn validate_connection(manager: &UsbSerialManager, connection: &UsbConnection, expected: &str, generation: u64) -> Result<(), String> {
    if generation == 0 || connection.connection_id != generation || connection.board_device_id != expected
        || connection.cancel_reader.load(Ordering::Acquire) || !manager.inbound_owner.is_current(generation)
        || connection.capabilities["audioTestPlayback"] != ENGINE
        || connection.capabilities["audio"]["playbackAvailable"] != true {
        return Err("当前设备不支持 WAV 硬件播放测试".into());
    }
    Ok(())
}

fn clear_active(session: &str) {
    if let Ok(mut slot) = active().lock() {
        if slot.as_ref().is_some_and(|value| value.session == session) {
            *slot = None;
        }
    }
}

fn send_confirmed_batch(
    manager: &UsbSerialManager,
    session: &str,
    chunks: &[(usize, &[u8])],
    expected: &str,
    generation: u64,
) -> Result<(), String> {
    let mut pending = Vec::with_capacity(chunks.len());
    for (seq, chunk) in chunks {
        let request = format!("wav-chunk-{}-{seq}", uuid::Uuid::new_v4().simple());
        let (sender, receiver) = mpsc::channel();
        manager.device_response_waiters.lock().map_err(|_| "WAV 播放确认队列不可用")?.push(DeviceResponseWaiter {
            request_id: request.clone(), response_topic: ACK_TOPIC.into(), scope: None, sender,
        });
        pending.push((request, receiver, *seq, *chunk));
    }
    for (request, _, seq, chunk) in &pending {
        if let Err(error) = manager.send_inner("audio/play_chunk", &serde_json::json!({
            "requestId": request, "sessionId": session, "seq": seq,
            "bytes": chunk.len(), "data": B64.encode(chunk),
        }), true, Some(expected), Some(generation), None) {
            for (request, _, _, _) in &pending {
                manager.remove_device_response_waiter(request, ACK_TOPIC);
            }
            return Err(error);
        }
    }
    for (request, receiver, _, _) in &pending {
        let receipt = receiver.recv_timeout(Duration::from_secs(3));
        manager.remove_device_response_waiter(request, ACK_TOPIC);
        let receipt = match receipt {
            Ok(receipt) => receipt,
            Err(_) => {
                for (pending_request, _, _, _) in &pending {
                    manager.remove_device_response_waiter(pending_request, ACK_TOPIC);
                }
                return Err("设备未确认 WAV 数据批次".into());
            }
        };
        if receipt["requestId"] != request.as_str() || receipt["ok"] != true {
            for (pending_request, _, _, _) in &pending {
                manager.remove_device_response_waiter(pending_request, ACK_TOPIC);
            }
            return Err(receipt["code"].as_str().unwrap_or("设备拒绝 WAV 数据").to_string());
        }
    }
    Ok(())
}

impl UsbSerialManager {
    pub(crate) fn play_wav_test(&self, expected: &str, wav: &[u8]) -> Result<serde_json::Value, String> {
        board(expected)?;
        let pcm = wav_pcm(wav)?;
        let _bulk = self.asset_transfer_guard.try_lock().map_err(|_| "设备正在执行其他传输，请稍后再试")?;
        let generation = {
            let connection = self.connection.lock().map_err(|_| "USB connection unavailable")?;
            let connection = connection.as_ref().ok_or("USB is not connected")?;
            validate_connection(self, connection, expected, connection.connection_id)?;
            connection.connection_id
        };
        let session = format!("wav-test-{}", uuid::Uuid::new_v4());
        let request = format!("wav-begin-{}", uuid::Uuid::new_v4());
        let cancelled = Arc::new(AtomicBool::new(false));
        {
            let mut slot = active().lock().map_err(|_| "WAV 播放状态不可用")?;
            if slot.is_some() { return Err("已有 WAV 正在播放".into()); }
            *slot = Some(ActivePlayback { board: expected.into(), session: session.clone(), cancelled: cancelled.clone() });
        }
        let (sender, receiver) = mpsc::channel();
        self.device_response_waiters.lock().map_err(|_| "WAV 播放确认队列不可用")?.push(DeviceResponseWaiter {
            request_id: request.clone(), response_topic: RESPONSE_TOPIC.into(), scope: None, sender,
        });
        let begin = self.send_inner("audio/play_begin", &serde_json::json!({
            "requestId": request, "sessionId": session, "sampleRate": 16000, "bytes": pcm.len(),
        }), true, Some(expected), Some(generation), None);
        let receipt = match begin {
            Ok(()) => receiver.recv_timeout(Duration::from_secs(5)).map_err(|_| "设备未确认 WAV 播放开始".to_string()),
            Err(error) => Err(error),
        };
        self.remove_device_response_waiter(&request, RESPONSE_TOPIC);
        let receipt = match receipt {
            Ok(value) if value["ok"] == true && value["boardDeviceId"] == expected
                && value["sessionId"] == session && value["requestId"] == request => value,
            Ok(value) => {
                let error = value["error"].as_str().unwrap_or("设备拒绝 WAV 播放").to_string();
                clear_active(&session);
                return Err(error);
            }
            Err(error) => { clear_active(&session); return Err(error); }
        };
        let _ = receipt;
        let transfer = (|| {
            let chunks = pcm.chunks(CHUNK_BYTES).enumerate().collect::<Vec<_>>();
            for batch in chunks.chunks(CHUNKS_PER_BATCH) {
                if cancelled.load(Ordering::Acquire) { return Err("WAV 播放已停止".to_string()); }
                send_confirmed_batch(self, &session, batch, expected, generation)?;
            }
            if !cancelled.load(Ordering::Acquire) {
                let request = format!("wav-end-{}", uuid::Uuid::new_v4().simple());
                let (sender, receiver) = mpsc::channel();
                self.device_response_waiters.lock().map_err(|_| "WAV 播放确认队列不可用")?.push(DeviceResponseWaiter {
                    request_id: request.clone(), response_topic: RESPONSE_TOPIC.into(), scope: None, sender,
                });
                let sent = self.send_inner("audio/play_end", &serde_json::json!({
                    "requestId":request, "sessionId":session,
                }), true, Some(expected), Some(generation), None);
                let receipt = match sent {
                    Ok(()) => receiver.recv_timeout(Duration::from_secs(20))
                        .map_err(|_| "设备未确认 WAV 实际播放结果".to_string()),
                    Err(error) => Err(error),
                };
                self.remove_device_response_waiter(&request, RESPONSE_TOPIC);
                let receipt = receipt?;
                if receipt["requestId"] != request || receipt["boardDeviceId"] != expected
                    || receipt["sessionId"] != session || receipt["ok"] != true {
                    return Err(receipt["error"].as_str().unwrap_or("设备 WAV 播放失败").to_string());
                }
                if receipt["receivedBytes"] != pcm.len() || receipt["writtenBytes"] != pcm.len() * 6 {
                    return Err("设备报告的 WAV 接收或 I2S 写入字节数不完整".into());
                }
            }
            Ok(serde_json::json!({"ok":true,"boardDeviceId":expected,"durationMs":pcm.len() as u64 * 1000 / 32_000,"bytes":pcm.len()}))
        })();
        if transfer.is_err() && !cancelled.load(Ordering::Acquire) {
            let _ = self.send_to_board(expected, "audio/play_flush", &serde_json::json!({"sessionId":session}));
        }
        clear_active(&session);
        transfer
    }

    pub(crate) fn stop_wav_test(&self, expected: &str) -> Result<(), String> {
        board(expected)?;
        let playback = active().lock().map_err(|_| "WAV 播放状态不可用")?.clone();
        let Some(playback) = playback else { return Ok(()); };
        if playback.board != expected { return Err("WAV 播放设备已经改变".into()); }
        playback.cancelled.store(true, Ordering::Release);
        self.send_to_board(expected, "audio/play_flush", &serde_json::json!({"sessionId":playback.session}))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn wav(data: usize) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend(b"RIFF"); bytes.extend(((36 + data) as u32).to_le_bytes()); bytes.extend(b"WAVEfmt ");
        bytes.extend(16u32.to_le_bytes()); bytes.extend(1u16.to_le_bytes()); bytes.extend(1u16.to_le_bytes());
        bytes.extend(16_000u32.to_le_bytes()); bytes.extend(32_000u32.to_le_bytes()); bytes.extend(2u16.to_le_bytes()); bytes.extend(16u16.to_le_bytes());
        bytes.extend(b"data"); bytes.extend((data as u32).to_le_bytes()); bytes.resize(44 + data, 0); bytes
    }
    #[test] fn accepts_bounded_board_profile() { assert_eq!(wav_pcm(&wav(32_000)).unwrap().len(), 32_000); }
    #[test] fn rejects_wrong_profile_and_oversize() {
        let mut wrong = wav(100); wrong[24] = 0x80; assert!(wav_pcm(&wrong).is_err());
        assert!(wav_pcm(&wav(MAX_PCM_BYTES + 2)).is_err());
    }
}
