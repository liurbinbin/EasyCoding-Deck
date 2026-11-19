/*
 * [Input] Transport-neutral device status and S3 Wi-Fi actions from DeviceContext.
 * [Output] USB-only provisioning, live device network/handshake status, authenticated WSS connect, discovery/manual endpoint, and pairing revocation UI.
 * [Pos] dashboard node for the ESP32-S3 transport surface.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useEffect, useMemo, useState } from "react";
import Button from "../shell/Button.jsx";
import { useDeviceContext } from "../shell/DeviceContext.jsx";

export default function S3WifiPanel() {
  const {
    usb,
    s3Wifi,
    refreshS3Wifi,
    refreshS3WifiStatus,
    configureS3Wifi,
    scanS3WifiNetworks,
    connectS3Wifi,
    forgetS3Wifi,
  } = useDeviceContext();
  const [ssid, setSsid] = useState("");
  const [password, setPassword] = useState("");
  const [manualEndpoint, setManualEndpoint] = useState("");
  const [selectedBoard, setSelectedBoard] = useState("");
  const [networks, setNetworks] = useState([]);
  const [pending, setPending] = useState("");
  const [message, setMessage] = useState("");

  useEffect(() => {
    void refreshS3Wifi().catch(() => {});
  }, [refreshS3Wifi]);

  const pairingEntries = Array.isArray(s3Wifi?.pairings) ? s3Wifi.pairings : [];
  const pendingPairing = pairingEntries.find((entry) => entry?.pending === true) || null;
  const pairings = pairingEntries.filter((entry) => entry?.pending !== true);
  const candidates = Array.isArray(s3Wifi?.candidates) ? s3Wifi.candidates : [];
  const usbPairing = pairings.find((entry) => entry.boardDeviceId === usb.boardDeviceId) || null;
  const boardId = selectedBoard || usbPairing?.boardDeviceId || pairings[0]?.boardDeviceId || "";
  const pairing = useMemo(
    () => pairings.find((entry) => entry.boardDeviceId === boardId) || null,
    [boardId, pairings],
  );
  const candidate = candidates.find((entry) => entry.boardDeviceId === boardId) || null;
  const endpoint = manualEndpoint.trim() || candidate?.endpoint || pairing?.endpoint || "";
  const usbProvisioning = usb.connected && usb.transport === "usb"
    && usb.runtime === "esp32-s3-tkeyboard" && Boolean(usb.boardDeviceId);
  const wifiConnected = usb.connected && usb.transport === "wifi-websocket" && usb.authenticated;
  const deviceStatus = s3Wifi?.deviceStatus || null;
  const deviceStatusText = !deviceStatus
    ? (usbProvisioning || wifiConnected ? "正在读取设备网络状态" : "连接设备后可查看实时网络状态")
    : deviceStatus.provisioning
      ? "正在生成安全身份"
      : !deviceStatus.configured
        ? "尚未配网"
        : !deviceStatus.connected
          ? "已配网，尚未连接路由器"
          : !deviceStatus.serverRunning
            ? "已连接路由器，WSS 服务正在启动"
            : deviceStatus.authenticated
              ? "Wi-Fi 已连接并通过认证"
              : deviceStatus.challengePending
                ? "WSS 已连接，正在发送认证 challenge"
                : deviceStatus.clientConnected
                  ? "WSS 已连接，正在等待认证"
                  : "已连接路由器，等待桌面端连接";
  const deviceStatusDetails = deviceStatus
    ? [
        deviceStatus.ssid ? `SSID ${deviceStatus.ssid}` : "",
        deviceStatus.ip ? `IP ${deviceStatus.ip}` : "",
        Number.isFinite(deviceStatus.rssi) ? `${deviceStatus.rssi} dBm` : "",
      ].filter(Boolean).join(" · ")
    : "";

  useEffect(() => {
    if (!usb.connected || usb.runtime !== "esp32-s3-tkeyboard" || !usb.boardDeviceId) return undefined;
    const timer = window.setInterval(() => {
      void refreshS3WifiStatus(usb.boardDeviceId).catch(() => {});
    }, 5000);
    return () => window.clearInterval(timer);
  }, [refreshS3WifiStatus, usb.boardDeviceId, usb.connected, usb.runtime]);

  const run = async (name, action) => {
    setPending(name);
    setMessage("");
    try {
      const result = await action();
      setMessage(result || "操作完成");
    } catch (error) {
      setMessage(error?.message || String(error));
    } finally {
      setPending("");
    }
  };

  return (
    <div className="settings-grid s3-wifi-panel">
      <div className="device-row">
        <div>
          <strong>当前链路</strong>
          <p className="muted">
            {wifiConnected
              ? `已认证 Wi-Fi · ${usb.endpoint}`
              : usb.connected
                ? `USB · ${usb.portName}`
                : "设备未连接"}
          </p>
        </div>
        <Button
          variant="ghost"
          size="small"
          loading={pending === "refresh" || s3Wifi?.pending === true}
          onClick={() => run("refresh", async () => { await refreshS3Wifi(); return "设备状态、局域网候选与配对记录已刷新"; })}
        >
          刷新状态与发现
        </Button>
      </div>

      <div className="device-row" role="status">
        <div>
          <strong>设备网络状态</strong>
          <p className="muted">{deviceStatusText}</p>
          {deviceStatusDetails && <p className="muted">{deviceStatusDetails}</p>}
          {s3Wifi?.statusError && <p className="muted">状态读取失败：{s3Wifi.statusError}</p>}
        </div>
      </div>

      {pendingPairing && (
        <div className="device-row" role="status">
          <div>
            <strong>未完成的安全配对</strong>
            <p className="muted">
              {pendingPairing.boardDeviceId} 的 pair secret 已安全保存。重新连接该设备可恢复配网，或显式放弃这条 pending 记录。
            </p>
          </div>
          <Button
            variant="ghost"
            size="small"
            loading={pending === "discard-pending"}
            onClick={() => run("discard-pending", async () => {
              await forgetS3Wifi(pendingPairing.boardDeviceId);
              return "未完成的本机配对记录已移除";
            })}
          >
            放弃 pending 配对
          </Button>
        </div>
      )}

      {usbProvisioning && (
        <div className="settings-grid">
          <div className="button-row">
            <Button
              variant="ghost"
              loading={pending === "scan"}
              onClick={() => run("scan", async () => {
                const result = await scanS3WifiNetworks(usb.boardDeviceId);
                const rows = Array.isArray(result?.networks) ? result.networks : [];
                setNetworks(rows);
                if (rows.length > 0 && !ssid) setSsid(String(rows[0]?.ssid || ""));
                return rows.length > 0 ? `发现 ${rows.length} 个 Wi-Fi 网络` : "未发现可用 Wi-Fi 网络";
              })}
            >
              扫描周边 Wi-Fi
            </Button>
          </div>
          {networks.length > 0 && (
            <label>
              <span>扫描结果</span>
              <select value={ssid} onChange={(event) => setSsid(event.target.value)}>
                {networks.map((network, index) => (
                  <option key={`${network.ssid || "hidden"}-${network.channel || 0}-${index}`} value={network.ssid || ""}>
                    {network.ssid || "隐藏网络"} · {network.rssi ?? "?"} dBm · {network.auth || "unknown"}
                  </option>
                ))}
              </select>
            </label>
          )}
          <label>
            <span>Wi-Fi SSID</span>
            <input value={ssid} maxLength={32} autoComplete="off" onChange={(event) => setSsid(event.target.value)} />
          </label>
          <label>
            <span>Wi-Fi 密码</span>
            <input type="password" value={password} maxLength={64} autoComplete="new-password" onChange={(event) => setPassword(event.target.value)} />
          </label>
          <Button
            loading={pending === "configure"}
            disabled={!ssid}
            onClick={() => run("configure", async () => {
              try {
                await configureS3Wifi({ boardDeviceId: usb.boardDeviceId, ssid, password });
              } finally {
                setPassword("");
              }
              setSelectedBoard(usb.boardDeviceId);
              setManualEndpoint("");
              return "配网与配对信息已通过 USB 写入；密码未保留在界面中";
            })}
          >
            通过 USB 配网并配对
          </Button>
        </div>
      )}

      {pairings.length > 0 && (
        <div className="settings-grid">
          <label>
            <span>已配对 S3</span>
            <select value={boardId} onChange={(event) => {
              setSelectedBoard(event.target.value);
              setManualEndpoint("");
            }}>
              {pairings.map((entry) => <option key={entry.boardDeviceId} value={entry.boardDeviceId}>{entry.boardDeviceId}</option>)}
            </select>
          </label>
          <label>
            <span>手动 WSS endpoint（mDNS 不可用时）</span>
            <input
              value={manualEndpoint}
              placeholder={candidate?.endpoint || pairing?.endpoint || "wss://192.168.1.50:8443/device/v1"}
              autoComplete="off"
              onChange={(event) => setManualEndpoint(event.target.value)}
            />
          </label>
          <div className="button-row">
            <Button
              loading={pending === "connect"}
              disabled={!boardId || !endpoint}
              onClick={() => run("connect", async () => {
                try {
                  await connectS3Wifi({ boardDeviceId: boardId, endpoint });
                } finally {
                  await refreshS3WifiStatus(boardId).catch(() => null);
                }
                return "已切换到经过认证的 Wi-Fi transport";
              })}
            >
              连接 Wi-Fi
            </Button>
            <Button
              variant="ghost"
              loading={pending === "forget"}
              disabled={!boardId}
              onClick={() => run("forget", async () => {
                await forgetS3Wifi(boardId);
                setSelectedBoard("");
                setManualEndpoint("");
                return "本机配对已移除；通过 USB 连接时设备端凭据也已撤销";
              })}
            >
              忘记配对
            </Button>
          </div>
        </div>
      )}

      {(message || s3Wifi?.error) && <p role="status" className="muted">{message || s3Wifi.error}</p>}
      <p className="muted">mDNS 结果仅用于发现；连接仍会校验证书指纹、HMAC challenge、board、boot 和 connection generation。</p>
    </div>
  );
}
