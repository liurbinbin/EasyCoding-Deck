/*
 * [Input] Bounded serial probe/connected RX and TX observations.
 * [Output] In-memory, sequence-addressable serial console snapshots for the local desktop UI.
 * [Pos] Debug observability helper; never opens a second serial handle or persists payloads.
 */

use serde::Serialize;
use std::collections::VecDeque;
use std::sync::{Mutex, OnceLock};
use std::time::{SystemTime, UNIX_EPOCH};

const MAX_ENTRIES: usize = 600;
const MAX_LINE_BYTES: usize = 4096;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Entry {
    sequence: u64,
    timestamp_ms: u128,
    direction: &'static str,
    phase: &'static str,
    port: String,
    baud: u32,
    line: String,
    truncated: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    entries: Vec<Entry>,
    next_sequence: u64,
}

#[derive(Default)]
struct State {
    next_sequence: u64,
    entries: VecDeque<Entry>,
}

static STATE: OnceLock<Mutex<State>> = OnceLock::new();

fn state() -> &'static Mutex<State> {
    STATE.get_or_init(|| Mutex::new(State::default()))
}

fn bounded_line(line: &str) -> (String, bool) {
    let line = line.trim_end_matches(['\r', '\n']);
    if line.len() <= MAX_LINE_BYTES {
        return (line.to_string(), false);
    }
    let mut end = MAX_LINE_BYTES;
    while !line.is_char_boundary(end) {
        end -= 1;
    }
    (format!("{} ... [truncated, {} bytes total]", &line[..end], line.len()), true)
}

pub fn record(direction: &'static str, phase: &'static str, port: &str, baud: u32, line: &str) {
    let Ok(mut state) = state().lock() else { return; };
    state.next_sequence = state.next_sequence.saturating_add(1);
    let (line, truncated) = bounded_line(line);
    let entry = Entry {
        sequence: state.next_sequence,
        timestamp_ms: SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_millis(),
        direction,
        phase,
        port: port.to_string(),
        baud,
        line,
        truncated,
    };
    state.entries.push_back(entry);
    while state.entries.len() > MAX_ENTRIES {
        state.entries.pop_front();
    }
}

pub fn snapshot(after_sequence: Option<u64>, limit: Option<usize>) -> Snapshot {
    let Ok(state) = state().lock() else {
        return Snapshot { entries: Vec::new(), next_sequence: 0 };
    };
    let after = after_sequence.unwrap_or(0);
    let limit = limit.unwrap_or(250).clamp(1, MAX_ENTRIES);
    let mut entries: Vec<_> = state.entries.iter().filter(|entry| entry.sequence > after).cloned().collect();
    if entries.len() > limit {
        entries.drain(..entries.len() - limit);
    }
    Snapshot { entries, next_sequence: state.next_sequence }
}

pub fn clear() {
    if let Ok(mut state) = state().lock() {
        state.entries.clear();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bounds_and_sequences_console_rows() {
        clear();
        record("rx", "probe", "COM1", 115200, "hello\n");
        record("tx", "connected", "COM1", 115200, &"x".repeat(MAX_LINE_BYTES + 10));
        let rows = snapshot(None, None).entries;
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].line, "hello");
        assert!(rows[1].truncated);
        assert!(rows[1].sequence > rows[0].sequence);
    }
}
