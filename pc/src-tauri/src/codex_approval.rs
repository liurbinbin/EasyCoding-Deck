//! Bounded Windows UI Automation relay for native Codex approval prompts.

use crate::usb_serial::UsbSerialManager;
use serde::Deserialize;
use serde_json::{json, Value};
use std::fs;
use std::io::Write;
use std::process::Command;
use std::sync::{Mutex, OnceLock};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[cfg(windows)]
use std::os::windows::process::CommandExt;

const CREATE_NO_WINDOW: u32 = 0x08000000;

#[derive(Clone, Debug, Deserialize)]
#[serde(rename_all = "camelCase")]
struct PromptSnapshot {
    agent_id: String,
    agent_label: String,
    hwnd: i64,
    allow_name: String,
    deny_name: String,
    allow_runtime_id: String,
    deny_runtime_id: String,
    tool: String,
    #[serde(default)]
    hook_request_id: Option<String>,
}

#[derive(Clone)]
struct Pending {
    request_id: String,
    board: String,
    generation: u64,
    prompt: PromptSnapshot,
    usb: UsbSerialManager,
}

fn pending() -> &'static Mutex<Option<Pending>> {
    static PENDING: OnceLock<Mutex<Option<Pending>>> = OnceLock::new();
    PENDING.get_or_init(|| Mutex::new(None))
}

fn approval_trigger_from_payload(agent_id: &str, payload: &Value, now: u64) -> Option<String> {
    let event = payload["event"].as_str().unwrap_or_default();
    let state = payload["state"].as_str().unwrap_or_default();
    let approval_state = match agent_id {
        "codex" => state == "waiting_user"
            || event.starts_with("response_item:function_call")
            || event.starts_with("response_item:custom_tool_call"),
        "workbuddy" => state == "waiting_user" && event == "PermissionRequest",
        "zcode" => state == "waiting_user" && event == "PermissionRequest",
        "opencode" => state == "waiting_user" && event == "PermissionRequest",
        "hermes" => state == "waiting_user" && event == "pre_approval_request",
        // Prefer the native DSH relay. This trigger is a fallback for a DSH
        // installation that exposes only its desktop approval prompt.
        "deepseek-harness" => state == "waiting_user",
        _ => false,
    };
    if !approval_state {
        return None;
    }
    let timestamp = payload["tsMs"].as_u64()?;
    if timestamp > now.saturating_add(5_000) || now.saturating_sub(timestamp) > 30_000 {
        return None;
    }
    Some(format!("{agent_id}:{timestamp}:{event}:{state}"))
}

fn desktop_approval_trigger(agent_id: &str) -> Option<String> {
    let file_name = match agent_id {
        "codex" => "codex.json",
        "workbuddy" => "workbuddy.json",
        "deepseek-harness" => "deepseek-harness.json",
        "hermes" => "hermes.json",
        "zcode" => "zcode.json",
        "opencode" => "opencode.json",
        _ => return None,
    };
    let path = std::env::temp_dir().join("pet-manager-bridge-state").join(file_name);
    let payload: Value = serde_json::from_slice(&fs::read(path).ok()?).ok()?;
    let now = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_millis() as u64;
    approval_trigger_from_payload(agent_id, &payload, now)
}

fn blocking_hook_prompt(agent_id: &str) -> Option<PromptSnapshot> {
    if !matches!(agent_id, "workbuddy" | "zcode" | "opencode") {
        return None;
    }
    let payload: Value = serde_json::from_slice(&fs::read(
        std::env::temp_dir()
            .join("pet-manager-bridge-state")
            .join(format!("{agent_id}.json")),
    ).ok()?).ok()?;
    if payload["source"] != agent_id
        || payload["state"] != "waiting_user"
        || payload["event"] != "PermissionRequest"
    {
        return None;
    }
    let request_id = payload["approvalRequestId"].as_str()?;
    uuid::Uuid::parse_str(request_id).ok()?;
    let now = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_millis() as u64;
    let timestamp = payload["tsMs"].as_u64()?;
    if timestamp > now.saturating_add(5_000) || now.saturating_sub(timestamp) > 310_000 {
        return None;
    }
    let tool = payload["detail"]["toolName"].as_str()
        .map(str::trim).filter(|value| !value.is_empty() && value.len() <= 80)
        .unwrap_or(match agent_id { "zcode" => "ZCode action", "opencode" => "OpenCode action", _ => "WorkBuddy action" }).to_string();
    Some(PromptSnapshot {
        agent_id: agent_id.into(),
        agent_label: match agent_id { "zcode" => "ZCode", "opencode" => "OpenCode", _ => "WorkBuddy" }.into(),
        hwnd: 0,
        allow_name: "allow".into(),
        deny_name: "deny".into(),
        allow_runtime_id: request_id.into(),
        deny_runtime_id: request_id.into(),
        tool,
        hook_request_id: Some(request_id.into()),
    })
}

#[cfg(windows)]
fn powershell(script: &str, environment: &[(&str, String)]) -> Result<String, String> {
    let mut command = Command::new("powershell.exe");
    command.args(["-NoLogo", "-NoProfile", "-NonInteractive", "-Command", script]);
    command.creation_flags(CREATE_NO_WINDOW);
    for (name, value) in environment {
        command.env(name, value);
    }
    let output = command.output().map_err(|error| format!("Codex approval UI unavailable: {error}"))?;
    if !output.status.success() {
        return Err("Codex approval UI query failed".into());
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

#[cfg(windows)]
fn scan_prompt(agent_id: &str) -> Option<PromptSnapshot> {
    const SCRIPT: &str = r#"
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$agentId=$env:EASY_CODING_APPROVAL_AGENT
$processNames=switch($agentId){
  'claude-code'{@('claude');break}
  'workbuddy'{@('WorkBuddy');break}
  'deepseek-harness'{@('DeepSeek Harness');break}
  'hermes'{@('Hermes Agent');break}
  'zcode'{@('ZCode');break}
  default{@('ChatGPT','Codex')}
}
$agentLabel=switch($agentId){'claude-code'{'Claude'}'workbuddy'{'WorkBuddy'}'deepseek-harness'{'DeepSeek Harness'}'hermes'{'Hermes Agent'}'zcode'{'ZCode'}default{'Codex'}}
$allow='(?i)^(allow|approve)(\b|\s|$)|^(run|execute)\s+(command|tool)(\b|\s|$)|^(允许|批准|同意)(一次|写入|执行|修改|操作|本次|仅这次|始终|\s|$)|^(运行|执行)(命令|工具)'
$deny='(?i)^(deny|reject|cancel|no)(\b|\s|$)|^(拒绝|驳回|取消|不允许)'
foreach($process in @(Get-Process -Name $processNames -ErrorAction SilentlyContinue)) {
  $handle=[IntPtr]$process.MainWindowHandle
  if($handle -eq [IntPtr]::Zero){continue}
  try{$root=[System.Windows.Automation.AutomationElement]::FromHandle($handle)}catch{continue}
  if($null -eq $root){continue}
  $choices=@()
  foreach($controlType in @([System.Windows.Automation.ControlType]::Button,
      [System.Windows.Automation.ControlType]::ListItem,
      [System.Windows.Automation.ControlType]::RadioButton)){
    $choices+=@($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
      [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,$controlType)))
  }
  $allowButtons=@();$denyButtons=@()
  foreach($button in $choices){
    try{$name=([string]$button.Current.Name).Trim();$enabled=$button.Current.IsEnabled}catch{continue}
    if(-not $enabled){continue}
    if($name -match $allow -or ($agentId -eq 'hermes' -and $name -match '(?i)^(run|execute|运行|执行)$')){$allowButtons+=@($button)}
    if($name -match $deny){$denyButtons+=@($button)}
  }
  $allowButton=$null;$denyButton=$null;$best=[double]::PositiveInfinity
  foreach($candidateAllow in $allowButtons){foreach($candidateDeny in $denyButtons){
    try{$ar=$candidateAllow.Current.BoundingRectangle;$dr=$candidateDeny.Current.BoundingRectangle}catch{continue}
    $ay=$ar.Top+$ar.Height/2;$dy=$dr.Top+$dr.Height/2
    if([Math]::Abs($ay-$dy) -gt 140){continue}
    try{$allowName=([string]$candidateAllow.Current.Name).Trim()}catch{$allowName=''}
    # WorkBuddy exposes one-shot, session-wide, and deny as adjacent choices.
    # Keep the hardware Allow action least-privileged even though the persistent
    # choice is geometrically closer to Deny.
    $persistentPenalty=if($allowName -match '(?i)(always|session|始终|会话)'){1000}else{0}
    $score=$persistentPenalty+[Math]::Abs($ay-$dy)+[Math]::Abs(($ar.Left+$ar.Width/2)-($dr.Left+$dr.Width/2))/10
    if($score -lt $best){$best=$score;$allowButton=$candidateAllow;$denyButton=$candidateDeny}
  }}
  if($null -ne $allowButton -and $null -ne $denyButton){
    $texts=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
      [System.Windows.Automation.PropertyCondition]::new(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
        [System.Windows.Automation.ControlType]::Text))
    $tool="$agentLabel action"
    foreach($text in $texts){try{$value=([string]$text.Current.Name).Trim()}catch{continue};if($value.Length -gt 2 -and $value.Length -le 80 -and $value -match '(?i)(command|tool|execute|命令|工具|执行)'){$tool=$value;break}}
    [pscustomobject]@{agentId=$agentId;agentLabel=$agentLabel;hwnd=$handle.ToInt64();allowName=[string]$allowButton.Current.Name;denyName=[string]$denyButton.Current.Name;allowRuntimeId=[string]::Join('.',@($allowButton.GetRuntimeId()));denyRuntimeId=[string]::Join('.',@($denyButton.GetRuntimeId()));tool=$tool}|ConvertTo-Json -Compress
    exit 0
  }
}
"#;
    let output = powershell(
        SCRIPT,
        &[("EASY_CODING_APPROVAL_AGENT", agent_id.to_string())],
    )
    .ok()?;
    if output.is_empty() { None } else { serde_json::from_str(&output).ok() }
}

#[cfg(not(windows))]
fn scan_prompt(_agent_id: &str) -> Option<PromptSnapshot> { None }

fn find_prompt(agent_id: &str) -> Option<PromptSnapshot> {
    if matches!(agent_id, "workbuddy" | "zcode" | "opencode") {
        if let Some(prompt) = blocking_hook_prompt(agent_id) { return Some(prompt); }
    }
    scan_prompt(agent_id)
}

fn prompt_still_current(prompt: &PromptSnapshot) -> bool {
    if prompt.hook_request_id.is_some() {
        return blocking_hook_prompt(&prompt.agent_id).is_some_and(|current|
            current.hook_request_id == prompt.hook_request_id);
    }
    scan_prompt(&prompt.agent_id).is_some_and(|current|
        current.hwnd == prompt.hwnd && current.allow_name == prompt.allow_name
            && current.deny_name == prompt.deny_name
            && current.allow_runtime_id == prompt.allow_runtime_id
            && current.deny_runtime_id == prompt.deny_runtime_id)
}

fn write_hook_decision(request_id: &str, decision: &str) -> Result<(), String> {
    uuid::Uuid::parse_str(request_id).map_err(|_| "invalid approval request".to_string())?;
    let directory = std::env::temp_dir().join("pet-manager-approval-decisions");
    fs::create_dir_all(&directory).map_err(|error| format!("approval directory unavailable: {error}"))?;
    let path = directory.join(format!("{request_id}.json"));
    let mut file = fs::OpenOptions::new().write(true).create_new(true).open(&path)
        .map_err(|error| format!("approval decision already exists or is unavailable: {error}"))?;
    let body = serde_json::to_vec(&json!({"decision":decision}))
        .map_err(|error| format!("approval decision encoding failed: {error}"))?;
    file.write_all(&body).and_then(|_| file.sync_all())
        .map_err(|error| format!("approval decision write failed: {error}"))
}

#[cfg(windows)]
fn invoke_prompt(prompt: &PromptSnapshot, allow: bool) -> Result<(), String> {
    if let Some(request_id) = prompt.hook_request_id.as_deref() {
        return write_hook_decision(request_id, if allow { "allow" } else { "deny" });
    }
    const SCRIPT: &str = r#"
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$handle=[IntPtr][Int64]::Parse($env:HACHIMI_CODEX_APPROVAL_HWND)
$root=[System.Windows.Automation.AutomationElement]::FromHandle($handle)
if($null -eq $root){throw 'stale Codex window'}
$buttons=@()
foreach($controlType in @([System.Windows.Automation.ControlType]::Button,
    [System.Windows.Automation.ControlType]::ListItem,
    [System.Windows.Automation.ControlType]::RadioButton)){
  $buttons+=@($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
    [System.Windows.Automation.PropertyCondition]::new(
      [System.Windows.Automation.AutomationElement]::ControlTypeProperty,$controlType)))
}
$target=$null;$opposite=$null
foreach($button in $buttons){try{$name=[string]$button.Current.Name;$runtimeId=[string]::Join('.',@($button.GetRuntimeId()))}catch{continue};if($name -eq $env:HACHIMI_CODEX_APPROVAL_TARGET -and $runtimeId -eq $env:HACHIMI_CODEX_APPROVAL_TARGET_ID){$target=$button};if($name -eq $env:HACHIMI_CODEX_APPROVAL_OPPOSITE -and $runtimeId -eq $env:HACHIMI_CODEX_APPROVAL_OPPOSITE_ID){$opposite=$button}}
if($null -eq $target -or $null -eq $opposite){throw 'stale Codex approval prompt'}
$pattern=$null
if($target.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$pattern)){$pattern.Invoke();'OK';exit 0}
if($target.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern,[ref]$pattern)){$pattern.Select();'OK';exit 0}
if($target.TryGetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern,[ref]$pattern)){$pattern.DoDefaultAction();'OK';exit 0}
throw 'Desktop approval choice is not invokable'
"#;
    let (target, target_id, opposite, opposite_id) = if allow {
        (&prompt.allow_name, &prompt.allow_runtime_id, &prompt.deny_name, &prompt.deny_runtime_id)
    } else {
        (&prompt.deny_name, &prompt.deny_runtime_id, &prompt.allow_name, &prompt.allow_runtime_id)
    };
    let output = powershell(SCRIPT, &[
        ("HACHIMI_CODEX_APPROVAL_HWND", prompt.hwnd.to_string()),
        ("HACHIMI_CODEX_APPROVAL_TARGET", target.clone()),
        ("HACHIMI_CODEX_APPROVAL_TARGET_ID", target_id.clone()),
        ("HACHIMI_CODEX_APPROVAL_OPPOSITE", opposite.clone()),
        ("HACHIMI_CODEX_APPROVAL_OPPOSITE_ID", opposite_id.clone()),
    ])?;
    (output == "OK").then_some(()).ok_or_else(|| "Codex approval was not confirmed".into())
}

#[cfg(not(windows))]
fn invoke_prompt(_prompt: &PromptSnapshot, _allow: bool) -> Result<(), String> {
    Err("Codex native approval relay is only available on Windows".into())
}

pub fn handle_device_decision(payload: &Value) -> bool {
    let request_id = payload["requestId"].as_str().unwrap_or_default();
    let decision = payload["decision"].as_str().unwrap_or_default();
    if request_id.is_empty() || !matches!(decision, "allow" | "deny") {
        return false;
    }
    let claimed = pending().lock().ok().and_then(|mut slot| {
        if slot.as_ref().map(|item| item.request_id.as_str()) != Some(request_id) {
            return None;
        }
        slot.take()
    });
    let Some(item) = claimed else { return false; };
    if item.usb.approval_connection() != Some((item.board.clone(), item.generation))
        || crate::approval_agent_id() != item.prompt.agent_id
    {
        return true;
    }
    if let Err(error) = invoke_prompt(&item.prompt, decision == "allow") {
        eprintln!("[codex-approval] decision delivery failed: {error}");
    }
    let _ = item.usb.send_approval_bound(&item.board, item.generation, "approval/cancel",
        &json!({"requestId":item.request_id}));
    true
}

pub fn start(usb: UsbSerialManager) {
    thread::spawn(move || {
      let mut last_trigger = String::new();
      let mut scans_remaining = 0u8;
      let mut last_direct_scan = Instant::now() - Duration::from_secs(1);
      loop {
        thread::sleep(Duration::from_millis(500));
        let connection = usb.approval_connection();
        let active_agent = crate::approval_agent_id();
        let current = pending().lock().ok().and_then(|slot| slot.clone());
        if let Some(item) = current {
            if connection != Some((item.board.clone(), item.generation))
                || active_agent != item.prompt.agent_id
            {
                let _ = usb.send_approval_bound(&item.board, item.generation, "approval/cancel",
                    &json!({"requestId":item.request_id}));
                if let Ok(mut slot) = pending().lock() { *slot = None; }
                continue;
            }
            let still_visible = prompt_still_current(&item.prompt);
            if !still_visible {
                let _ = usb.send_approval_bound(&item.board, item.generation, "approval/cancel",
                    &json!({"requestId":item.request_id}));
                if let Ok(mut slot) = pending().lock() { *slot = None; }
            }
            continue;
        }
        if matches!(active_agent.as_str(), "claude-code" | "workbuddy" | "deepseek-harness" | "hermes" | "zcode" | "opencode")
            && last_direct_scan.elapsed() >= Duration::from_millis(900)
        {
            // These desktop clients do not provide a durable approval trigger:
            // WorkBuddy can overwrite PermissionRequest with PreToolUse, while
            // DSH may run without its native relay plugin. The process-scoped
            // UIA query still requires an exact allow/deny button pair.
            last_direct_scan = Instant::now();
            scans_remaining = 1;
        } else {
            if let Some(trigger) = desktop_approval_trigger(&active_agent) {
                if trigger != last_trigger {
                    last_trigger = trigger;
                    scans_remaining = 10;
                }
            }
        }
        if scans_remaining == 0 { continue; }
        scans_remaining -= 1;
        let (board, generation) = match connection { Some(value) => value, None => continue };
        if !matches!(active_agent.as_str(), "codex" | "claude-code" | "workbuddy" | "deepseek-harness" | "hermes" | "zcode" | "opencode") { continue; }
        if active_agent == "deepseek-harness" && crate::dsh_approval::has_pending() { continue; }
        let Some(prompt) = find_prompt(&active_agent) else { continue };
        let request_id = format!("approval-{}", uuid::Uuid::new_v4());
        let item = Pending { request_id: request_id.clone(), board: board.clone(), generation,
            prompt: prompt.clone(), usb: usb.clone() };
        if let Ok(mut slot) = pending().lock() { *slot = Some(item); } else { continue; }
        if usb.send_approval_bound(&board, generation, "approval/request", &json!({
            "requestId":request_id,"agent":prompt.agent_label,"tool":prompt.tool,
            "reason":format!("{} 请求执行：按设备上配置的确认键允许，返回键拒绝", prompt.agent_label),
            "timeoutMs":300000,
        })).is_err() {
            if let Ok(mut slot) = pending().lock() { *slot = None; }
        }
      }
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn prompt_snapshot_contract_is_bounded() {
        let value = json!({"agentId":"claude-code","agentLabel":"Claude","hwnd":42,"allowName":"Allow once","denyName":"Deny","allowRuntimeId":"42.1","denyRuntimeId":"42.2","tool":"Run command"});
        let prompt: PromptSnapshot = serde_json::from_value(value).unwrap();
        assert_eq!(prompt.hwnd, 42);
        assert_eq!(prompt.allow_name, "Allow once");
        assert_eq!(prompt.allow_runtime_id, "42.1");
        assert_eq!(prompt.agent_id, "claude-code");
    }

    #[test]
    fn stale_or_unrelated_bridge_state_cannot_trigger_uia() {
        let now = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_millis() as u64;
        assert!(now > 30_000);
        assert!(approval_trigger_from_payload("codex", &json!({
            "state":"waiting_user", "event":"response_item:function_call", "tsMs":now
        }), now).is_some());
        assert!(approval_trigger_from_payload("workbuddy", &json!({
            "state":"waiting_user", "event":"PermissionRequest", "tsMs":now
        }), now).is_some());
        assert!(approval_trigger_from_payload("opencode", &json!({
            "state":"waiting_user", "event":"PermissionRequest", "tsMs":now
        }), now).is_some());
        assert!(approval_trigger_from_payload("deepseek-harness", &json!({
            "state":"waiting_user", "event":"approval", "tsMs":now
        }), now).is_some());
        assert!(approval_trigger_from_payload("workbuddy", &json!({
            "state":"waiting_user", "event":"Notification", "tsMs":now
        }), now).is_none());
        assert!(approval_trigger_from_payload("workbuddy", &json!({
            "state":"waiting_user", "event":"PermissionRequest", "tsMs":now - 30_001
        }), now).is_none());
    }

    #[test]
    fn approval_allow_match_does_not_accept_generic_navigation_labels() {
        let source = include_str!("codex_approval.rs");
        let matcher = source.split("$allow='").nth(1).unwrap().split('\'').next().unwrap();
        assert!(!matcher.contains("continue"));
        assert!(!matcher.contains("confirm"));
        assert!(!matcher.contains("|yes"));
        assert!(source.contains("$agentId -eq 'hermes' -and $name -match '(?i)^(run|execute|运行|执行)$'"));
    }

    #[test]
    fn claude_approval_scan_is_agent_scoped() {
        let source = include_str!("codex_approval.rs");
        assert!(source.contains("Get-Process -Name $processNames"));
        assert!(source.contains("@('claude')"));
        assert!(source.contains("\"claude-code\" | \"workbuddy\" | \"deepseek-harness\" | \"hermes\" | \"zcode\""));
        assert!(source.contains("prompt.allow_runtime_id == item.prompt.allow_runtime_id"));
    }

    #[test]
    fn workbuddy_and_dsh_use_their_own_windows_and_localized_allow_once() {
        let source = include_str!("codex_approval.rs");
        assert!(source.contains("'workbuddy'{@('WorkBuddy')"));
        assert!(source.contains("'deepseek-harness'{@('DeepSeek Harness')"));
        assert!(source.contains("允许|批准|同意)(一次|写入|执行|修改|操作|本次|仅这次|始终|"));
        assert!(source.contains("crate::dsh_approval::has_pending()"));
        assert!(source.contains("[System.Windows.Automation.ControlType]::ListItem"));
        assert!(source.contains("[System.Windows.Automation.SelectionItemPattern]::Pattern"));
        assert!(source.contains("$persistentPenalty=if($allowName -match '(?i)(always|session|始终|会话)'){1000}else{0}"));
    }
}
