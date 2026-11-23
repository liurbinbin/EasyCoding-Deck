/**
 * [Input] Tauri-owned USB serial console snapshots and clear command.
 * [Output] Full-page live bounded RX/TX viewer without opening or competing for a serial port.
 * [Pos] Primary desktop navigation page for device serial diagnostics.
 * [Sync] If this file changes, update `pc/src/dashboard/.folder.md`.
 */

import React, { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { ArrowDownToLine, Clipboard, Pause, Play, RefreshCw, Trash2 } from "lucide-react";

const MAX_VISIBLE_ROWS = 600;

function timestamp(value) {
  const date = new Date(Number(value) || 0);
  return Number.isNaN(date.getTime()) ? "--:--:--.---" : date.toLocaleTimeString("zh-CN", {
    hour12: false,
    hour: "2-digit",
    minute: "2-digit",
    second: "2-digit",
    fractionalSecondDigits: 3,
  });
}

function printable(entry) {
  const direction = entry.direction === "tx" ? "TX" : "RX";
  return `${timestamp(entry.timestampMs)} ${direction} ${entry.port || "-"}@${entry.baud || 0} [${entry.phase || "-"}] ${entry.line || ""}`;
}

export default function SerialLogPage() {
  const [entries, setEntries] = useState([]);
  const [paused, setPaused] = useState(false);
  const [followLatest, setFollowLatest] = useState(true);
  const [error, setError] = useState("");
  const [copied, setCopied] = useState(false);
  const nextSequenceRef = useRef(0);
  const viewportRef = useRef(null);

  const load = useCallback(async ({ reset = false } = {}) => {
    try {
      const result = await invoke("usb_get_serial_log", {
        afterSequence: reset ? null : nextSequenceRef.current,
        limit: MAX_VISIBLE_ROWS,
      });
      const incoming = Array.isArray(result?.entries) ? result.entries : [];
      nextSequenceRef.current = Number(result?.nextSequence) || nextSequenceRef.current;
      setEntries((current) => {
        if (!reset && incoming.length === 0) return current;
        const merged = reset ? incoming : [...current, ...incoming];
        return merged.slice(-MAX_VISIBLE_ROWS);
      });
      setError("");
    } catch (loadError) {
      setError(typeof loadError === "string" ? loadError : String(loadError));
    }
  }, []);

  useEffect(() => {
    load({ reset: true });
  }, [load]);

  useEffect(() => {
    if (paused) return undefined;
    const id = window.setInterval(() => load(), 500);
    return () => window.clearInterval(id);
  }, [load, paused]);

  const scrollToLatest = useCallback(() => {
    window.requestAnimationFrame(() => {
      const viewport = viewportRef.current;
      if (!viewport) return;
      viewport.scrollTop = viewport.scrollHeight;
      window.requestAnimationFrame(() => {
        const settledViewport = viewportRef.current;
        if (settledViewport) settledViewport.scrollTop = settledViewport.scrollHeight;
      });
    });
  }, []);

  useEffect(() => {
    if (paused || !followLatest) return;
    scrollToLatest();
  }, [entries, followLatest, paused, scrollToLatest]);

  const toggleFollowLatest = () => {
    setFollowLatest((current) => {
      const next = !current;
      if (next) scrollToLatest();
      return next;
    });
  };

  const copy = async () => {
    await navigator.clipboard.writeText(entries.map(printable).join("\n"));
    setCopied(true);
    window.setTimeout(() => setCopied(false), 1200);
  };

  const clear = async () => {
    await invoke("usb_clear_serial_log");
    setEntries([]);
    nextSequenceRef.current = 0;
    await load({ reset: true });
  };

  return (
    <section className="serial-log-page" aria-labelledby="serial-log-title">
      <header className="serial-log-page__header">
        <div>
          <span className="serial-log-page__eyebrow">DEVICE CONSOLE</span>
          <h1 id="serial-log-title">串口日志</h1>
          <p>{entries.length} 条 · {paused ? "已暂停" : "实时"}</p>
        </div>
      </header>
      <div className="serial-log-page__viewport" ref={viewportRef}>
        {entries.map((entry) => (
          <div className="serial-log-page__row" data-direction={entry.direction} key={entry.sequence}>
            <span>{timestamp(entry.timestampMs)}</span>
            <strong>{entry.direction === "tx" ? "TX" : "RX"}</strong>
            <span>{entry.port || "-"}@{entry.baud || 0}</span>
            <code>{entry.line}</code>
          </div>
        ))}
        {!entries.length && !error && <div className="serial-log-page__empty">暂无串口日志</div>}
        {error && <div className="serial-log-page__error">{error}</div>}
      </div>
      <footer className="serial-log-page__toolbar">
        <div className="serial-log-page__actions">
          <button
            type="button"
            className={`serial-log-page__follow${followLatest ? " is-active" : ""}`}
            onClick={toggleFollowLatest}
            aria-pressed={followLatest}
            title={followLatest ? "停止跟随最新日志" : "跟随最新日志"}
          >
            <ArrowDownToLine size={15} />
            跟随最新
          </button>
          <button type="button" className="icon-btn" onClick={() => setPaused((value) => !value)} title={paused ? "继续刷新" : "暂停刷新"}>
            {paused ? <Play size={15} /> : <Pause size={15} />}
          </button>
          <button type="button" className="icon-btn" onClick={() => load({ reset: true })} title="刷新">
            <RefreshCw size={15} />
          </button>
          <button type="button" className="icon-btn" onClick={copy} disabled={!entries.length} title={copied ? "已复制" : "复制日志"}>
            <Clipboard size={15} />
          </button>
          <button type="button" className="icon-btn" onClick={clear} disabled={!entries.length} title="清空日志">
            <Trash2 size={15} />
          </button>
        </div>
      </footer>
    </section>
  );
}
