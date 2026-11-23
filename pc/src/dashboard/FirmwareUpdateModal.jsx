/**
 * [Input] open/onClose props, expected board identity, current ESP32-P4/ESP32-S3
 *         firmware version, native file picker or bundled factory image,
 *         USB download-port scan, native OTA/factory-flash commands, and
 *         firmware progress events.
 * [Output] A/B OTA dialog for existing firmware plus a first-install factory
 *          flashing dialog for blank ESP32-S3 devices, with bounded progress
 *          and explicit reboot state.
 * [Pos] component node in pc/src/dashboard
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { open as openDialog } from "@tauri-apps/plugin-dialog";
import { CheckCircle2, FileUp, LoaderCircle, RefreshCw, X } from "lucide-react";

const INITIAL_PROGRESS = { stage: "idle", bytesSent: 0, bytesTotal: 0, percent: 0 };

const STAGE_LABELS = {
  begin: "正在创建备用固件槽",
  recover: "旧版固件中断，正在自动恢复升级",
  upload: "正在传输并逐块校验",
  verify: "正在校验镜像完整性",
  reboot: "镜像完整性通过，正在等待设备重启",
  validate: "设备已重连，正在确认新版本有效",
  factory_prepare: "正在准备 USB 工厂刷写",
  factory_flash: "正在刷写完整工厂镜像",
  factory_verify: "正在校验闪存内容",
  factory_reboot: "刷写完成，正在重启设备",
};

function fileNameFromPath(path) {
  return String(path || "").split(/[\\/]/).filter(Boolean).at(-1) || "";
}

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
}

export default function FirmwareUpdateModal({
  open,
  onClose,
  currentFirmware = "",
  expectedBoardDeviceId,
  factoryMode = false,
}) {
  const [firmwarePath, setFirmwarePath] = useState("");
  const [progress, setProgress] = useState(INITIAL_PROGRESS);
  const [pending, setPending] = useState(false);
  const [result, setResult] = useState(null);
  const [error, setError] = useState("");
  const [factoryPort, setFactoryPort] = useState("");
  const [factoryPorts, setFactoryPorts] = useState([]);
  const [factoryScanBusy, setFactoryScanBusy] = useState(false);

  useEffect(() => {
    if (!open) return undefined;
    setFirmwarePath("");
    setProgress(INITIAL_PROGRESS);
    setPending(false);
    setResult(null);
    setError("");
    let cancelled = false;
    let unlisten = null;
    setFactoryPort("");
    setFactoryPorts([]);
    if (factoryMode) {
      setFactoryScanBusy(true);
      invoke("usb_scan_devices")
        .then((devices) => {
          const ports = (Array.isArray(devices) ? devices : [])
            .map((device) => String(device?.portName || "").trim())
            .filter(Boolean);
          if (!cancelled) {
            setFactoryPorts(ports);
            if (ports.length === 1) setFactoryPort(ports[0]);
          }
        })
        .catch((scanError) => {
          if (!cancelled) setError(`扫描下载串口失败：${scanError}`);
        })
        .finally(() => {
          if (!cancelled) setFactoryScanBusy(false);
        });
    }
    listen("usb-firmware-update-progress", (event) => {
      const payload = event?.payload || {};
      setProgress({
        stage: String(payload.stage || "upload"),
        bytesSent: Number(payload.bytesSent) || 0,
        bytesTotal: Number(payload.bytesTotal) || 0,
        percent: Math.max(0, Math.min(100, Number(payload.percent) || 0)),
      });
    }).then((stop) => {
      if (cancelled) stop();
      else unlisten = stop;
    });
    return () => {
      cancelled = true;
      unlisten?.();
    };
  }, [factoryMode, open]);

  const selectedName = useMemo(() => fileNameFromPath(firmwarePath), [firmwarePath]);

  const selectFirmware = async () => {
    setError("");
    const selected = await openDialog({
      multiple: false,
      directory: false,
      filters: [{ name: "固件镜像", extensions: ["bin"] }],
    });
    if (typeof selected === "string") setFirmwarePath(selected);
  };

  const startUpdate = async () => {
    if (!firmwarePath || pending) return;
    setPending(true);
    setResult(null);
    setError("");
    setProgress({ ...INITIAL_PROGRESS, stage: "begin" });
    try {
      const nextResult = await invoke("usb_update_firmware", {
        firmwarePath,
        expectedBoardDeviceId,
      });
      setResult(nextResult);
      setProgress((current) => ({ ...current, stage: "reboot", percent: 100 }));
    } catch (updateError) {
      setError(typeof updateError === "string" ? updateError : String(updateError));
    } finally {
      setPending(false);
    }
  };

  const startFactoryFlash = async () => {
    if (pending) return;
    setPending(true);
    setResult(null);
    setError("");
    setProgress({ ...INITIAL_PROGRESS, stage: "factory_prepare" });
    try {
      const nextResult = await invoke("usb_flash_bundled_factory_firmware", {
        portName: factoryPort || null,
      });
      setResult(nextResult);
      setProgress((current) => ({ ...current, stage: "factory_reboot", percent: 100 }));
    } catch (flashError) {
      setError(typeof flashError === "string" ? flashError : String(flashError));
    } finally {
      setPending(false);
    }
  };

  if (!open) return null;

  const close = () => {
    if (!pending) onClose?.();
  };
  const stageLabel = STAGE_LABELS[progress.stage] || "等待选择固件";

  return (
    <div className="modal-backdrop" role="presentation" onMouseDown={close}>
      <div
        className="modal-card firmware-update-modal"
        role="dialog"
        aria-modal="true"
        aria-label="固件升级"
        onMouseDown={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div>
            <h3 className="modal-title">固件升级</h3>
            <p className="firmware-update-modal__subtitle">
              当前版本：<code>{currentFirmware || "未知"}</code>
            </p>
          </div>
          <button type="button" className="icon-btn" onClick={close} disabled={pending} aria-label="关闭">
            <X size={16} />
          </button>
        </div>

        <div className="modal-body firmware-update-modal__body">
          {factoryMode ? (
            <div className="firmware-update-modal__factory">
              <div className="firmware-update-modal__factory-copy">
                <strong>首次安装设备固件</strong>
                <small>将完整 factory.bin 写入 ESP32-S3，包括 bootloader、分区表和应用。新设备无需先运行 EasyCoding Deck 固件。</small>
              </div>
              <label className="firmware-update-modal__port">
                <span>下载串口</span>
                <select
                  value={factoryPort}
                  onChange={(event) => setFactoryPort(event.target.value)}
                  disabled={pending || factoryScanBusy}
                >
                  <option value="">自动选择唯一串口</option>
                  {factoryPorts.map((port) => <option key={port} value={port}>{port}</option>)}
                </select>
              </label>
              <button
                type="button"
                className="firmware-update-modal__picker"
                onClick={() => {
                  setFactoryScanBusy(true);
                  invoke("usb_scan_devices")
                    .then((devices) => {
                      const ports = (Array.isArray(devices) ? devices : [])
                        .map((device) => String(device?.portName || "").trim())
                        .filter(Boolean);
                      setFactoryPorts(ports);
                      if (ports.length === 1) setFactoryPort(ports[0]);
                    })
                    .catch((scanError) => setError(`扫描下载串口失败：${scanError}`))
                    .finally(() => setFactoryScanBusy(false));
                }}
                disabled={pending || factoryScanBusy}
              >
                <RefreshCw size={20} className={factoryScanBusy ? "spin" : ""} />
                <span>
                  <strong>{factoryScanBusy ? "正在扫描串口…" : "重新扫描下载串口"}</strong>
                  <small>{factoryPorts.length ? factoryPorts.join("、") : "如果设备没有应用，请按住 BOOT 再按一下 RESET"}</small>
                </span>
              </button>
            </div>
          ) : (
            <button
              type="button"
              className="firmware-update-modal__picker"
              onClick={selectFirmware}
              disabled={pending}
            >
              <FileUp size={20} />
              <span>
                <strong>{selectedName || "选择 firmware.bin"}</strong>
                <small>{firmwarePath || "支持 ESP32-S3 factory.bin 或 ESP-IDF app .bin 镜像"}</small>
              </span>
            </button>
          )}

          {(pending || result) && (
            <div className="firmware-update-modal__progress" aria-live="polite">
              <div className="firmware-update-modal__progress-meta">
                <span>{pending ? <LoaderCircle className="spin" size={15} /> : <CheckCircle2 size={15} />}</span>
                <strong>{result
                  ? (factoryMode ? "工厂固件已写入设备" : "新固件已在设备上确认有效")
                  : stageLabel}</strong>
                <span>{progress.percent}%</span>
              </div>
              <div className="firmware-update-modal__progress-track">
                <span style={{ width: `${progress.percent}%` }} />
              </div>
              <small>{formatBytes(progress.bytesSent)} / {formatBytes(progress.bytesTotal)}</small>
            </div>
          )}

          {result && (
            <div className="firmware-update-modal__result" data-tone="success">
              {factoryMode
                ? <>工厂镜像已写入，设备正在重启。新设备重启后会重新枚举 USB，EasyCoding Deck 将自动尝试连接。版本 <code>{result.version || "未知"}</code>。</>
                : <>设备已重连，新版本 <code>{result.version || "未知"}</code> 已在
                  <code>{result.targetPartition || "备用槽"}</code> 确认为 <code>{result.imageState || "valid"}</code>。</>}
            </div>
          )}
          {error && <div className="firmware-update-modal__result" data-tone="error">{error}</div>}
          {!pending && !result && !error && (
            <p className="firmware-update-modal__notice">
              {factoryMode
                ? "首次刷写需要下载模式串口。请连接数据线；若未发现串口，按住 BOOT 再按一下 RESET，然后重新扫描。"
                : "升级写入备用槽，不会覆盖当前可启动版本。传输开始后请保持 USB 连接和设备供电。"}
            </p>
          )}
        </div>

        <div className="modal-footer firmware-update-modal__footer">
          <button type="button" className="btn-secondary" onClick={close} disabled={pending}>
            {result ? "完成" : "取消"}
          </button>
          {!result && (
            <button
              type="button"
              className="btn-primary"
              onClick={factoryMode ? startFactoryFlash : startUpdate}
              disabled={factoryMode ? pending || factoryScanBusy : !firmwarePath || pending}
            >
              {pending ? "升级中..." : factoryMode ? "刷写内置工厂固件" : "开始升级"}
            </button>
          )}
        </div>
      </div>
    </div>
  );
}
