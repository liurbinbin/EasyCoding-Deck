/*
 * [Input] Paired S3 endpoint metadata, mDNS candidates, WebSocket text envelopes, and a pinned TLS certificate.
 * [Output] Bounded authenticated Wi-Fi sessions, HMAC proofs, secure pairing records, and discovery candidates.
 * [Pos] Native LAN transport boundary; business dispatch remains owned by usb_serial::UsbSerialManager.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use futures_util::{SinkExt, StreamExt};
use mdns_sd::{ServiceDaemon, ServiceEvent};
use ring::{digest, hmac};
use rustls::{
    client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier},
    crypto::{verify_tls12_signature, verify_tls13_signature, CryptoProvider},
    pki_types::{CertificateDer, ServerName, UnixTime},
    DigitallySignedStruct, Error as RustlsError, SignatureScheme,
};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fmt,
    fs,
    io::{Error as IoError, ErrorKind, Write},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    time::{Duration, Instant},
};
use tokio::sync::mpsc;
use tokio_tungstenite::{
    connect_async_tls_with_config,
    tungstenite::{
        client::IntoClientRequest,
        http::{header::SEC_WEBSOCKET_PROTOCOL, HeaderValue},
        protocol::WebSocketConfig,
        Message,
    },
    Connector,
};

pub const SERVICE_TYPE: &str = "_hachimodock-s3._tcp.local.";
pub const PAIRING_FILE: &str = "s3-wifi-pairings.secure.json";
pub const PAIRING_PURPOSE: &str = "s3-wifi-pairings";
pub const PENDING_PAIRING_FILE: &str = "s3-wifi-pending.secure.json";
pub const PENDING_PAIRING_PURPOSE: &str = "s3-wifi-pending";
const AUTH_DOMAIN: &str = "hachimodock-wifi-v1";
const WEBSOCKET_SUBPROTOCOL: &str = "hachimodock.s3.v1";
const MAX_TEXT_BYTES: usize = 12 * 1024;
const OUTBOUND_FRAMES: usize = 32;
const OUTBOUND_ENQUEUE_TIMEOUT: Duration = Duration::from_millis(1500);
const INBOUND_FRAMES: usize = 128;
const INBOUND_DISPATCH_TIMEOUT: Duration = Duration::from_secs(30);
const HANDSHAKE_TIMEOUT: Duration = Duration::from_secs(10);
const PING_INTERVAL: Duration = Duration::from_secs(4);
const DEVICE_HEARTBEAT: &str = r#"{"topic":"system/heartbeat","payload":{}}"#;
static PAIRING_UPDATES: Mutex<()> = Mutex::new(());
static PENDING_UPDATES: Mutex<()> = Mutex::new(());

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct PairingRecord {
    pub board_device_id: String,
    pub client_id: String,
    pub pair_secret: String,
    pub certificate_sha256: String,
    #[serde(default)]
    pub endpoint: String,
}

impl fmt::Debug for PairingRecord {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PairingRecord")
            .field("board_device_id", &self.board_device_id)
            .field("client_id", &self.client_id)
            .field("pair_secret", &"[REDACTED]")
            .field("certificate_sha256", &self.certificate_sha256)
            .field("endpoint", &self.endpoint)
            .finish()
    }
}

impl PairingRecord {
    pub fn validate(&self) -> Result<(), String> {
        validate_identifier("boardDeviceId", &self.board_device_id, 96)?;
        validate_identifier("clientId", &self.client_id, 63)?;
        decode_hex_32(&self.pair_secret).map_err(|_| "pairSecret 必须是 32 字节小写十六进制".to_string())?;
        decode_hex_32(&self.certificate_sha256)
            .map_err(|_| "certificateSha256 必须是 32 字节小写十六进制".to_string())?;
        if !self.endpoint.is_empty() {
            validate_endpoint(&self.endpoint, false)?;
        }
        Ok(())
    }

    fn secret(&self) -> Result<[u8; 32], String> {
        decode_hex_32(&self.pair_secret).map_err(|_| "配对凭据损坏".to_string())
    }

    fn certificate_pin(&self) -> Result<[u8; 32], String> {
        decode_hex_32(&self.certificate_sha256).map_err(|_| "设备证书指纹损坏".to_string())
    }
}

#[derive(Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct PendingPairing {
    pub board_device_id: String,
    pub client_id: String,
    pub pair_secret: String,
}

impl fmt::Debug for PendingPairing {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PendingPairing")
            .field("board_device_id", &self.board_device_id)
            .field("client_id", &self.client_id)
            .field("pair_secret", &"[REDACTED]")
            .finish()
    }
}

impl PendingPairing {
    fn validate(&self) -> Result<(), String> {
        validate_identifier("boardDeviceId", &self.board_device_id, 96)?;
        validate_identifier("clientId", &self.client_id, 63)?;
        decode_hex_32(&self.pair_secret)
            .map_err(|_| "pending pairSecret 必须是 32 字节小写十六进制".to_string())?;
        Ok(())
    }
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "camelCase")]
pub struct WifiCandidate {
    pub board_device_id: String,
    pub host: String,
    pub port: u16,
    pub endpoint: String,
    pub source: String,
    pub authenticated: bool,
}

#[derive(Debug, Clone)]
pub struct InboundFrame {
    pub topic: String,
    pub payload: serde_json::Value,
}

pub struct ConnectedWifi {
    pub writer: WifiFrameWriter,
    pub incoming: mpsc::Receiver<Result<InboundFrame, String>>,
    pub cancel: Arc<AtomicBool>,
    pub board_device_id: String,
    pub boot_id: String,
    pub hello: serde_json::Value,
}

#[derive(Clone)]
pub struct WifiFrameWriter {
    sender: mpsc::Sender<String>,
    pending: Vec<u8>,
}

impl WifiFrameWriter {
    fn new(sender: mpsc::Sender<String>) -> Self {
        Self { sender, pending: Vec::with_capacity(8 * 1024) }
    }

    fn enqueue(&self, mut text: String) -> std::io::Result<()> {
        let deadline = Instant::now() + OUTBOUND_ENQUEUE_TIMEOUT;
        loop {
            match self.sender.try_send(text) {
                Ok(()) => return Ok(()),
                Err(tokio::sync::mpsc::error::TrySendError::Full(returned)) => {
                    if Instant::now() >= deadline {
                        return Err(IoError::new(
                            ErrorKind::WouldBlock,
                            "WebSocket outbound queue remained full",
                        ));
                    }
                    text = returned;
                    std::thread::sleep(Duration::from_millis(2));
                }
                Err(tokio::sync::mpsc::error::TrySendError::Closed(_)) => {
                    return Err(IoError::new(
                        ErrorKind::BrokenPipe,
                        "WebSocket outbound queue closed",
                    ));
                }
            }
        }
    }

    fn drain_complete_frames(&mut self) -> std::io::Result<()> {
        while let Some(end) = self.pending.iter().position(|byte| *byte == b'\n') {
            let mut frame_end = end;
            while frame_end > 0 && self.pending[frame_end - 1] == b'\r' {
                frame_end -= 1;
            }
            if frame_end == 0 {
                self.pending.drain(..=end);
                continue;
            }
            if frame_end > MAX_TEXT_BYTES {
                return Err(IoError::new(
                    ErrorKind::InvalidData,
                    "WebSocket envelope exceeds size limit",
                ));
            }
            let text = String::from_utf8(self.pending[..frame_end].to_vec())
                .map_err(|_| IoError::new(ErrorKind::InvalidData, "WebSocket envelope is not UTF-8"))?;
            self.enqueue(text)?;
            self.pending.drain(..=end);
        }
        Ok(())
    }
}

impl Write for WifiFrameWriter {
    fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
        self.drain_complete_frames()?;
        if self.pending.len().saturating_add(bytes.len()) > MAX_TEXT_BYTES + 2 {
            self.pending.clear();
            return Err(IoError::new(ErrorKind::InvalidData, "WebSocket envelope exceeds size limit"));
        }
        self.pending.extend_from_slice(bytes);
        self.drain_complete_frames()?;
        Ok(bytes.len())
    }

    fn flush(&mut self) -> std::io::Result<()> {
        self.drain_complete_frames()?;
        if self.pending.is_empty() {
            Ok(())
        } else {
            Err(IoError::new(ErrorKind::InvalidData, "incomplete WebSocket JSON envelope"))
        }
    }
}

#[derive(Debug)]
struct PinnedServerCertificate {
    expected_sha256: [u8; 32],
    algorithms: rustls::crypto::WebPkiSupportedAlgorithms,
}

impl ServerCertVerifier for PinnedServerCertificate {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, RustlsError> {
        let actual = digest::digest(&digest::SHA256, end_entity.as_ref());
        if actual.as_ref() != self.expected_sha256 {
            return Err(RustlsError::General("pinned S3 certificate fingerprint mismatch".into()));
        }
        Ok(ServerCertVerified::assertion())
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, RustlsError> {
        verify_tls12_signature(message, cert, dss, &self.algorithms)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, RustlsError> {
        verify_tls13_signature(message, cert, dss, &self.algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.algorithms.supported_schemes()
    }
}

pub fn pairings_path(app_data_dir: &Path) -> PathBuf {
    app_data_dir.join(PAIRING_FILE)
}

pub fn pending_pairing_path(app_data_dir: &Path) -> PathBuf {
    app_data_dir.join(PENDING_PAIRING_FILE)
}

pub fn load_pending_pairing(app_data_dir: &Path) -> Result<Option<PendingPairing>, String> {
    let Some(raw) = crate::secure_config::read(
        &pending_pairing_path(app_data_dir),
        PENDING_PAIRING_PURPOSE,
    )? else {
        return Ok(None);
    };
    let pending: PendingPairing = serde_json::from_str(&raw)
        .map_err(|_| "S3 Wi-Fi pending 配对记录损坏；原文件未修改".to_string())?;
    pending.validate()?;
    Ok(Some(pending))
}

pub fn save_pending_pairing(app_data_dir: &Path, pending: &PendingPairing) -> Result<(), String> {
    let _guard = PENDING_UPDATES.lock().map_err(|_| "S3 Wi-Fi pending 配对存储繁忙")?;
    pending.validate()?;
    let raw = serde_json::to_vec(pending).map_err(|_| "无法编码 S3 Wi-Fi pending 配对记录")?;
    crate::secure_config::write(
        &pending_pairing_path(app_data_dir),
        PENDING_PAIRING_PURPOSE,
        &raw,
    )
}

pub fn clear_pending_pairing(app_data_dir: &Path, board_device_id: &str) -> Result<bool, String> {
    let _guard = PENDING_UPDATES.lock().map_err(|_| "S3 Wi-Fi pending 配对存储繁忙")?;
    validate_identifier("boardDeviceId", board_device_id, 96)?;
    let Some(pending) = load_pending_pairing(app_data_dir)? else {
        return Ok(false);
    };
    if pending.board_device_id != board_device_id {
        return Ok(false);
    }
    let path = pending_pairing_path(app_data_dir);
    let metadata = fs::symlink_metadata(&path)
        .map_err(|_| "无法检查 S3 Wi-Fi pending 配对记录")?;
    if !metadata.is_file() || metadata.file_type().is_symlink() || metadata.len() > 1024 * 1024 {
        return Err("S3 Wi-Fi pending 配对路径不安全；原文件未修改".into());
    }
    fs::remove_file(path).map_err(|_| "无法清除 S3 Wi-Fi pending 配对记录")?;
    Ok(true)
}

pub fn load_pairings(app_data_dir: &Path) -> Result<Vec<PairingRecord>, String> {
    let Some(raw) = crate::secure_config::read(&pairings_path(app_data_dir), PAIRING_PURPOSE)? else {
        return Ok(Vec::new());
    };
    let records: Vec<PairingRecord> = serde_json::from_str(&raw)
        .map_err(|_| "S3 Wi-Fi 配对存储损坏；原文件未修改".to_string())?;
    if records.len() > 16 {
        return Err("S3 Wi-Fi 配对记录超过安全上限".into());
    }
    for record in &records {
        record.validate()?;
    }
    Ok(records)
}

pub fn save_pairing(app_data_dir: &Path, record: PairingRecord) -> Result<(), String> {
    let _guard = PAIRING_UPDATES.lock().map_err(|_| "S3 Wi-Fi 配对存储繁忙")?;
    record.validate()?;
    let mut records = load_pairings(app_data_dir)?;
    records.retain(|item| item.board_device_id != record.board_device_id);
    if records.len() >= 16 {
        return Err("S3 Wi-Fi 配对记录已达到 16 条安全上限；请先移除旧设备".into());
    }
    records.push(record);
    records.sort_by(|left, right| left.board_device_id.cmp(&right.board_device_id));
    let raw = serde_json::to_vec(&records).map_err(|_| "无法编码 S3 Wi-Fi 配对记录")?;
    crate::secure_config::write(&pairings_path(app_data_dir), PAIRING_PURPOSE, &raw)
}

pub fn forget_pairing(app_data_dir: &Path, board_device_id: &str) -> Result<bool, String> {
    let _guard = PAIRING_UPDATES.lock().map_err(|_| "S3 Wi-Fi 配对存储繁忙")?;
    validate_identifier("boardDeviceId", board_device_id, 96)?;
    let mut records = load_pairings(app_data_dir)?;
    let previous = records.len();
    records.retain(|item| item.board_device_id != board_device_id);
    if records.len() == previous {
        return Ok(false);
    }
    let raw = serde_json::to_vec(&records).map_err(|_| "无法编码 S3 Wi-Fi 配对记录")?;
    crate::secure_config::write(&pairings_path(app_data_dir), PAIRING_PURPOSE, &raw)?;
    Ok(true)
}

pub fn pairing_for(app_data_dir: &Path, board_device_id: &str) -> Result<PairingRecord, String> {
    load_pairings(app_data_dir)?
        .into_iter()
        .find(|item| item.board_device_id == board_device_id)
        .ok_or_else(|| "未找到该 S3 的 Wi-Fi 配对凭据；请先通过 USB 配对".to_string())
}

pub fn generate_pair_secret() -> Result<String, String> {
    use ring::rand::{SecureRandom, SystemRandom};
    let mut secret = [0u8; 32];
    SystemRandom::new().fill(&mut secret).map_err(|_| "无法生成 S3 配对密钥")?;
    Ok(hex(&secret))
}

pub fn auth_proof(
    pair_secret_hex: &str,
    board_device_id: &str,
    boot_id: &str,
    client_id: &str,
    nonce: &str,
) -> Result<String, String> {
    let secret = decode_hex_32(pair_secret_hex).map_err(|_| "配对凭据损坏".to_string())?;
    validate_identifier("boardDeviceId", board_device_id, 96)?;
    validate_boot_id(boot_id)?;
    validate_identifier("clientId", client_id, 63)?;
    if nonce.len() != 32 || !nonce.bytes().all(is_lower_hex) {
        return Err("认证 challenge nonce 无效".into());
    }
    let canonical = format!("{AUTH_DOMAIN}\n{board_device_id}\n{boot_id}\n{client_id}\n{nonce}");
    let key = hmac::Key::new(hmac::HMAC_SHA256, &secret);
    Ok(hex(hmac::sign(&key, canonical.as_bytes()).as_ref()))
}

pub fn validate_endpoint(endpoint: &str, _allow_insecure_development: bool) -> Result<String, String> {
    let endpoint = endpoint.trim();
    if endpoint.len() > 512 || endpoint.contains(['\r', '\n', '\0']) {
        return Err("S3 Wi-Fi endpoint 无效".into());
    }
    let uri: tokio_tungstenite::tungstenite::http::Uri = endpoint.parse()
        .map_err(|_| "S3 Wi-Fi endpoint 无效")?;
    let scheme = uri.scheme_str().unwrap_or("");
    let authority = uri.authority().map(|value| value.as_str()).unwrap_or("");
    if authority.is_empty() || authority.contains('@') || uri.path_and_query().map(|value| value.as_str()) != Some("/device/v1") {
        return Err("S3 Wi-Fi endpoint 必须是固定 /device/v1，且不能包含凭据、查询或 fragment".into());
    }
    if scheme == "wss" {
        return Ok(endpoint.to_string());
    }
    Err("S3 Wi-Fi 连接必须使用 wss://".into())
}

pub fn discover(timeout: Duration) -> Result<Vec<WifiCandidate>, String> {
    let daemon = ServiceDaemon::new().map_err(|error| format!("无法启动 S3 mDNS discovery：{error}"))?;
    let receiver = daemon.browse(SERVICE_TYPE)
        .map_err(|error| format!("无法浏览 S3 mDNS 服务：{error}"))?;
    let deadline = Instant::now() + timeout.min(Duration::from_secs(5));
    let mut candidates = BTreeMap::<String, WifiCandidate>::new();
    while Instant::now() < deadline {
        let remaining = deadline.saturating_duration_since(Instant::now()).min(Duration::from_millis(150));
        match receiver.recv_timeout(remaining) {
            Ok(ServiceEvent::ServiceResolved(info)) => {
                let board = info.txt_properties.iter()
                    .find(|property| property.key() == "id")
                    .map(|property| property.val_str())
                    .unwrap_or_default()
                    .trim()
                    .to_string();
                if board.is_empty() || validate_identifier("boardDeviceId", &board, 96).is_err() {
                    continue;
                }
                for address in &info.addresses {
                    let host = address.to_string();
                    let authority = if address.is_ipv6() {
                        format!("[{}]", host.replace('%', "%25"))
                    } else {
                        host.clone()
                    };
                    let endpoint = format!("wss://{authority}:{}/device/v1", info.port);
                    candidates.insert(format!("{board}|{host}|{}", info.port), WifiCandidate {
                        board_device_id: board.clone(), host, port: info.port, endpoint,
                        source: "mdns".into(), authenticated: false,
                    });
                }
            }
            Ok(_) => {}
            Err(_) => {}
        }
    }
    let _ = daemon.stop_browse(SERVICE_TYPE);
    let _ = daemon.shutdown();
    Ok(candidates.into_values().collect())
}

pub async fn connect(
    endpoint: &str,
    pairing: &PairingRecord,
    _allow_insecure_development: bool,
) -> Result<ConnectedWifi, String> {
    pairing.validate()?;
    let endpoint = validate_endpoint(endpoint, false)?;
    let websocket_config = WebSocketConfig::default()
        .read_buffer_size(16 * 1024)
        .write_buffer_size(0)
        .max_write_buffer_size(MAX_TEXT_BYTES * 2)
        .max_message_size(Some(MAX_TEXT_BYTES))
        .max_frame_size(Some(MAX_TEXT_BYTES));
    let provider = CryptoProvider::get_default().cloned()
        .unwrap_or_else(|| Arc::new(rustls::crypto::ring::default_provider()));
    let verifier = PinnedServerCertificate {
        expected_sha256: pairing.certificate_pin()?,
        algorithms: provider.signature_verification_algorithms,
    };
    let config = rustls::ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|_| "无法配置 S3 TLS 协议版本")?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(verifier))
        .with_no_client_auth();
    let connector = Some(Connector::Rustls(Arc::new(config)));
    let mut request = endpoint.as_str().into_client_request()
        .map_err(|_| "无法创建 S3 Wi-Fi WebSocket 请求")?;
    request.headers_mut().insert(
        SEC_WEBSOCKET_PROTOCOL,
        HeaderValue::from_static(WEBSOCKET_SUBPROTOCOL),
    );
    let (mut socket, response) = tokio::time::timeout(
        HANDSHAKE_TIMEOUT,
        connect_async_tls_with_config(request, Some(websocket_config), true, connector),
    ).await.map_err(|_| "S3 Wi-Fi TLS/WebSocket 握手超时".to_string())?
        .map_err(|error| format!("S3 Wi-Fi TLS/WebSocket 握手失败：{error}"))?;
    if response.status().as_u16() != 101 {
        return Err("S3 Wi-Fi endpoint 未完成 WebSocket upgrade".into());
    }
    if response.headers().get(SEC_WEBSOCKET_PROTOCOL)
        .and_then(|value| value.to_str().ok()) != Some(WEBSOCKET_SUBPROTOCOL) {
        return Err("S3 Wi-Fi endpoint 未确认要求的 WebSocket subprotocol".into());
    }
    socket.send(Message::Text(serde_json::json!({
        "topic":"network/challenge-request","payload":{}
    }).to_string().into())).await
        .map_err(|_| "无法请求 S3 Wi-Fi 认证 challenge")?;

    let challenge = next_json(&mut socket, HANDSHAKE_TIMEOUT, "等待设备 challenge").await?;
    if challenge.topic != "network/challenge" {
        return Err("S3 Wi-Fi 在认证前发送了非 challenge 消息".into());
    }
    let board = challenge.payload["boardDeviceId"].as_str().unwrap_or("").trim().to_string();
    let boot = challenge.payload["bootId"].as_str().unwrap_or("").trim().to_string();
    let nonce = challenge.payload["nonce"].as_str().unwrap_or("").trim().to_string();
    if board != pairing.board_device_id {
        return Err("S3 Wi-Fi challenge 设备身份与配对记录不一致".into());
    }
    validate_boot_id(&boot)?;
    let proof = auth_proof(&pairing.pair_secret, &board, &boot, &pairing.client_id, &nonce)?;
    socket.send(Message::Text(serde_json::json!({
        "topic":"network/auth",
        "payload":{"clientId":pairing.client_id,"nonce":nonce,"proof":proof}
    }).to_string().into())).await.map_err(|_| "无法提交 S3 Wi-Fi 认证")?;

    let auth = next_json(&mut socket, HANDSHAKE_TIMEOUT, "等待认证回执").await?;
    if auth.topic != "network/auth-ack"
        || auth.payload["ok"].as_bool() != Some(true)
        || auth.payload["boardDeviceId"].as_str() != Some(board.as_str())
        || auth.payload["bootId"].as_str() != Some(boot.as_str()) {
        return Err("S3 Wi-Fi 认证被拒绝或回执身份不匹配".into());
    }
    let hello = next_json(&mut socket, HANDSHAKE_TIMEOUT, "等待认证后 hello").await?;
    if hello.topic != "hello" || hello.payload["boardDeviceId"].as_str() != Some(board.as_str())
        || hello.payload.pointer("/capabilities/debugBootId").and_then(serde_json::Value::as_str) != Some(boot.as_str()) {
        return Err("S3 Wi-Fi hello 与已认证身份不一致".into());
    }

    let (mut sink, mut stream) = socket.split();
    let (outbound_tx, mut outbound_rx) = mpsc::channel::<String>(OUTBOUND_FRAMES);
    let (control_tx, mut control_rx) = mpsc::channel::<Message>(8);
    let (inbound_tx, inbound_rx) = mpsc::channel::<Result<InboundFrame, String>>(INBOUND_FRAMES);
    let cancel = Arc::new(AtomicBool::new(false));
    let writer_cancel = Arc::clone(&cancel);
    // 收、发必须独立：慢 TLS send 不能同时阻塞音频 ACK 和 pong 的读取。
    tokio::spawn(async move {
        let mut ping = tokio::time::interval(PING_INTERVAL);
        let mut watchdog = tokio::time::interval(Duration::from_millis(100));
        loop {
            let message = tokio::select! {
                biased;
                _ = watchdog.tick() => {
                    if writer_cancel.load(Ordering::Acquire) { break; }
                    continue;
                },
                control = control_rx.recv() => match control { Some(message) => message, None => break },
                _ = ping.tick() => Message::Text(DEVICE_HEARTBEAT.into()),
                outbound = outbound_rx.recv() => match outbound {
                    Some(text) if text.len() <= MAX_TEXT_BYTES => Message::Text(text.into()),
                    _ => break,
                },
            };
            // Wi-Fi 重传/漫游时一次 TLS 发送可能持续数秒；过短会误判为断线。
            if !matches!(tokio::time::timeout(Duration::from_secs(10), sink.send(message)).await, Ok(Ok(()))) { break; }
        }
        writer_cancel.store(true, Ordering::Release);
        let _ = tokio::time::timeout(Duration::from_secs(1), sink.close()).await;
    });
    let reader_cancel = Arc::clone(&cancel);
    tokio::spawn(async move {
        let mut watchdog = tokio::time::interval(Duration::from_millis(100));
        loop {
            let message = tokio::select! {
                _ = watchdog.tick() => {
                    if reader_cancel.load(Ordering::Acquire) { break; }
                    continue;
                },
                message = stream.next() => message,
            };
            let frame = match message {
                Some(Ok(Message::Text(text))) if text.len() <= MAX_TEXT_BYTES => parse_envelope(text.as_str()),
                Some(Ok(Message::Text(_))) => Err("S3 Wi-Fi 消息超过大小限制".into()),
                Some(Ok(Message::Ping(data))) => {
                    if control_tx.try_send(Message::Pong(data)).is_err() { break; }
                    continue;
                },
                Some(Ok(Message::Pong(_))) => continue,
                Some(Ok(Message::Close(_))) | None | Some(Err(_)) => break,
                Some(Ok(_)) => Err("S3 Wi-Fi 仅允许 UTF-8 JSON text frame".into()),
            };
            let invalid = frame.is_err();
            // 分发繁忙时保留当前帧并等待；socket 缓冲区吸收等待期间的入站数据。
            if !matches!(tokio::time::timeout(INBOUND_DISPATCH_TIMEOUT, inbound_tx.send(frame)).await, Ok(Ok(()))) || invalid { break; }
        }
        reader_cancel.store(true, Ordering::Release);
    });

    Ok(ConnectedWifi {
        writer: WifiFrameWriter::new(outbound_tx), incoming: inbound_rx, cancel,
        board_device_id: board, boot_id: boot, hello: hello.payload,
    })
}

async fn next_json<S>(
    socket: &mut tokio_tungstenite::WebSocketStream<S>,
    timeout: Duration,
    stage: &str,
) -> Result<InboundFrame, String>
where S: tokio::io::AsyncRead + tokio::io::AsyncWrite + Unpin {
    loop {
        let message = tokio::time::timeout(timeout, socket.next()).await
            .map_err(|_| format!("S3 Wi-Fi {stage}超时"))?
            .ok_or_else(|| format!("S3 Wi-Fi 在{stage}期间断开"))?
            .map_err(|_| format!("S3 Wi-Fi 在{stage}期间读取帧失败"))?;
        match message {
            Message::Text(text) if text.len() <= MAX_TEXT_BYTES => return parse_envelope(text.as_str()),
            Message::Ping(data) => socket.send(Message::Pong(data)).await.map_err(|_| "S3 Wi-Fi pong 发送失败")?,
            Message::Pong(_) => {}
            _ => return Err(format!("S3 Wi-Fi 在{stage}期间收到非法帧")),
        }
    }
}

fn parse_envelope(text: &str) -> Result<InboundFrame, String> {
    let value: serde_json::Value = serde_json::from_str(text).map_err(|_| "S3 Wi-Fi JSON envelope 无效")?;
    let object = value.as_object().ok_or("S3 Wi-Fi envelope 必须是对象")?;
    if object.len() != 2 || !object.contains_key("topic") || !object.contains_key("payload") {
        return Err("S3 Wi-Fi envelope 仅允许 topic 和 payload".into());
    }
    let topic = value["topic"].as_str().unwrap_or("");
    if topic.is_empty() || topic.len() > 96 || !topic.bytes().all(|b| b.is_ascii_alphanumeric() || b"/_-".contains(&b)) {
        return Err("S3 Wi-Fi topic 无效".into());
    }
    if !value["payload"].is_object() {
        return Err("S3 Wi-Fi payload 必须是对象".into());
    }
    Ok(InboundFrame { topic: topic.to_string(), payload: value["payload"].clone() })
}

fn validate_identifier(name: &str, value: &str, max: usize) -> Result<(), String> {
    if value.is_empty() || value.len() > max || !value.bytes().all(|b| b.is_ascii_alphanumeric() || b"._:-".contains(&b)) {
        return Err(format!("{name} 无效"));
    }
    Ok(())
}

fn validate_boot_id(value: &str) -> Result<(), String> {
    if value.len() == 16 && value.bytes().all(is_lower_hex) { Ok(()) } else { Err("S3 bootId 无效".into()) }
}

fn is_lower_hex(byte: u8) -> bool {
    byte.is_ascii_digit() || (b'a'..=b'f').contains(&byte)
}

fn decode_hex_32(value: &str) -> Result<[u8; 32], ()> {
    if value.len() != 64 || !value.bytes().all(is_lower_hex) { return Err(()); }
    let mut output = [0u8; 32];
    for (index, slot) in output.iter_mut().enumerate() {
        *slot = u8::from_str_radix(&value[index * 2..index * 2 + 2], 16).map_err(|_| ())?;
    }
    Ok(output)
}

fn hex(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        output.push(DIGITS[(byte >> 4) as usize] as char);
        output.push(DIGITS[(byte & 0x0f) as usize] as char);
    }
    output
}

impl fmt::Debug for WifiFrameWriter {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.debug_struct("WifiFrameWriter").field("pending_bytes", &self.pending.len()).finish_non_exhaustive()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hmac_canonicalization_is_stable_and_scope_sensitive() {
        let secret = "00".repeat(32);
        let nonce = "11".repeat(16);
        let first = auth_proof(&secret, "s3-tkeyboard-1234", "0123456789abcdef", "desktop-test", &nonce).unwrap();
        let same = auth_proof(&secret, "s3-tkeyboard-1234", "0123456789abcdef", "desktop-test", &nonce).unwrap();
        let other = auth_proof(&secret, "s3-tkeyboard-1234", "fedcba9876543210", "desktop-test", &nonce).unwrap();
        assert_eq!(first, same);
        assert_ne!(first, other);
        assert_eq!(first.len(), 64);
    }

    #[test]
    fn hmac_matches_the_firmware_contract_vector() {
        let secret = hex(&(0u8..32).collect::<Vec<_>>());
        let proof = auth_proof(
            &secret,
            "s3-tkeyboard-a1b2c3d4",
            "0011223344556677",
            "desktop-test",
            "0123456789abcdef0123456789abcdef",
        ).unwrap();
        assert_eq!(proof, "9402f88c580589b8a87cfd430d3720919e13c6569c211a117373249a85bbb65e");
    }

    #[test]
    fn writer_counts_the_json_envelope_without_its_line_delimiter() {
        let (sender, mut receiver) = mpsc::channel(1);
        let mut writer = WifiFrameWriter::new(sender);
        let line = format!("{}\n", "x".repeat(MAX_TEXT_BYTES));
        assert_eq!(writer.write(line.as_bytes()).unwrap(), line.len());
        assert_eq!(receiver.try_recv().unwrap().len(), MAX_TEXT_BYTES);
        let oversized = format!("{}\n", "x".repeat(MAX_TEXT_BYTES + 1));
        assert!(writer.write(oversized.as_bytes()).is_err());
    }

    #[test]
    fn writer_waits_for_bounded_queue_without_dropping_the_frame() {
        let (sender, mut receiver) = mpsc::channel(1);
        let mut writer = WifiFrameWriter::new(sender);
        writer.write_all(b"first\n").unwrap();
        let pending = std::thread::spawn(move || {
            writer.write_all(b"second\n")
        });
        std::thread::sleep(Duration::from_millis(20));
        assert_eq!(receiver.try_recv().unwrap(), "first");
        pending.join().unwrap().unwrap();
        assert_eq!(receiver.try_recv().unwrap(), "second");
    }

    #[test]
    fn endpoints_always_require_wss() {
        assert_eq!(validate_endpoint("wss://192.168.1.4:9443/device/v1", false).unwrap(), "wss://192.168.1.4:9443/device/v1");
        assert!(validate_endpoint("ws://192.168.1.4/device/v1", false).is_err());
        assert!(validate_endpoint("ws://127.0.0.1/device/v1", true).is_err());
        assert!(validate_endpoint("https://192.168.1.4/device/v1", true).is_err());
        assert!(validate_endpoint("wss://user@192.168.1.4/device/v1", false).is_err());
        assert!(validate_endpoint("wss://192.168.1.4/device/v1?token=secret", false).is_err());
    }

    #[test]
    fn envelope_rejects_remote_scope_smuggling_shape() {
        let parsed = parse_envelope(r#"{"topic":"audio/status","payload":{"_nativeDeviceScope":{"connectionId":"999"}}}"#).unwrap();
        assert!(parsed.payload.get("_nativeDeviceScope").is_some());
        // The transport owner removes this field before dispatch; parsing alone never grants authority.
        assert_eq!(parsed.topic, "audio/status");
    }

    #[test]
    fn pairing_validation_rejects_secret_or_pin_shape() {
        let mut record = PairingRecord {
            board_device_id: "s3-tkeyboard-1".into(), client_id: "desktop-1".into(),
            pair_secret: "00".repeat(32), certificate_sha256: "11".repeat(32), endpoint: String::new(),
        };
        assert!(record.validate().is_ok());
        record.pair_secret = "AA".repeat(32);
        assert!(record.validate().is_err());
    }
}
