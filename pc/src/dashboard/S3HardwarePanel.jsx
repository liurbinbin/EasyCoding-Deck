/**
 * [Input] ESP32-S3 USB status/capabilities and live power/audio USB events.
 * [Output] Compact five-screen, input, audio, power and peripheral health overview.
 * [Pos] T-Keyboard Voice hardware boundary panel in the device dashboard.
 */
import React, { useEffect, useMemo, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { BatteryCharging, CircuitBoard, Keyboard, Mic2, Monitor } from "lucide-react";

const ROLE_LABELS = { status: "连接", agent: "Agent", voice: "语音", tool: "工具", system: "系统" };

function StatusItem({ icon: Icon, label, value }) {
  return <div className="s3-hardware__item"><Icon size={16} /><span>{label}</span><strong>{value}</strong></div>;
}

export default function S3HardwarePanel({ usb }) {
  const [power, setPower] = useState(null);
  const [audio, setAudio] = useState(null);
  const capabilities = usb?.capabilities || {};
  const topology = capabilities.displayTopology || {};
  const roles = Array.isArray(topology.roles) ? topology.roles : [];

  useEffect(() => {
    let disposed = false;
    let stop = () => {};
    listen("usb-message", (event) => {
      if (disposed) return;
      if (event?.payload?.topic === "power/status") setPower(event.payload.payload || null);
      if (event?.payload?.topic === "audio/status") setAudio(event.payload.payload || null);
    }).then((unlisten) => { if (disposed) unlisten(); else stop = unlisten; }).catch(() => {});
    return () => { disposed = true; stop(); };
  }, [usb?.connectionId]);

  const battery = useMemo(() => {
    if (!power) return "等待遥测";
    if (Number.isFinite(Number(power.batteryPercent))) return `${Number(power.batteryPercent)}%${power.charging ? " · 充电中" : ""}`;
    return power.vbusPresent ? "USB 供电" : power.batteryPresent ? "电池已连接" : "未检测到电池";
  }, [power]);

  return <div className="s3-hardware">
    <div className="s3-hardware__screens" aria-label="五屏状态">
      {(roles.length ? roles : ["status", "agent", "voice", "tool", "system"]).map((role, index) => (
        <div className="s3-hardware__screen" key={`${role}-${index}`}>
          <span>LCD {index + 1}</span><strong>{ROLE_LABELS[role] || role}</strong><small>{topology.width || 128}x{topology.height || 128}</small>
        </div>
      ))}
    </div>
    <div className="s3-hardware__grid">
      <StatusItem icon={Monitor} label="显示" value={`${topology.count || 5} 块 GC9107`} />
      <StatusItem icon={Keyboard} label="输入" value={`${capabilities.input?.keys || 5} 个带屏按键 · 非触摸`} />
      <StatusItem icon={Mic2} label="音频" value={audio?.active ? "正在采集" : capabilities.audio?.captureAvailable ? "ES7210 就绪" : "不可用"} />
      <StatusItem icon={BatteryCharging} label="电源" value={battery} />
      <StatusItem icon={CircuitBoard} label="外设" value={`${capabilities.peripherals?.ws2812 || 0} 灯 · QWIIC`} />
    </div>
  </div>;
}
