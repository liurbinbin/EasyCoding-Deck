import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const firmware = readFileSync(join(here, "../../firmware-s3/src/main.cpp"), "utf8");
const inputConfig = readFileSync(join(here, "../../firmware-s3/src/input_config.cpp"), "utf8");
const audio = readFileSync(join(here, "../../firmware-s3/src/audio_service.cpp"), "utf8");
const board = readFileSync(join(here, "../../firmware-s3/src/board_runtime.cpp"), "utf8");
const avatar = readFileSync(join(here, "../../firmware-s3/src/avatar_service.cpp"), "utf8");
const tauri = readFileSync(join(here, "../src-tauri/src/lib.rs"), "utf8");
const codexApproval = readFileSync(join(here, "../src-tauri/src/codex_approval.rs"), "utf8");
const codexComposer = readFileSync(join(here, "../src-tauri/src/codex_composer.rs"), "utf8");
const serial = readFileSync(join(here, "../src-tauri/src/usb_serial.rs"), "utf8");
const dashboard = readFileSync(join(here, "DeviceDashboard.jsx"), "utf8");
const assignment = readFileSync(join(here, "lib/desktop-pet-assignment.js"), "utf8");
const playback = readFileSync(join(here, "../src-tauri/src/usb_serial/playback_test.rs"), "utf8");
const realtime = readFileSync(join(here, "../src-tauri/src/realtime_chat.rs"), "utf8");
const acks = readFileSync(join(here, "../src-tauri/src/realtime_audio_ack.rs"), "utf8");

test("S3 state transitions retain visible alerts and schedule distinct local cues", () => {
  assert.match(firmware, /next_state == "WAITING"[\s\S]*notifyEvent\("waiting_user"\)/);
  assert.match(firmware, /next_state == "DONE"[\s\S]*notifyEvent\("done"\)/);
  assert.match(firmware, /next_state == "ERROR"[\s\S]*notifyEvent\("error"\)/);
  assert.match(firmware, /state_hold_until_ms = millis\(\) \+ 5000/);
  assert.match(audio, /strcmp\(event, "done"\)/);
  assert.match(audio, /strcmp\(event, "error"\)/);
});

test("S3 approval is modal and follows the configured confirm and cancel actions", () => {
  assert.match(firmware, /approvalDecisionForShortEvent\(event\)/);
  assert.match(firmware, /decision > 0\) sendApprovalDecision\(true\)/);
  assert.match(firmware, /decision < 0\) sendApprovalDecision\(false\)/);
  assert.match(inputConfig, /page_enter[\s\S]*page_back/);
  assert.match(inputConfig, /exclusive_approval_action/);
  assert.match(inputConfig, /bindings_\[i\]\.action = "disabled"/);
  assert.match(firmware, /if \(approval_active\)[\s\S]*return true;/);
  assert.match(firmware, /sendMessage\("approval\/decision"/);
  assert.match(board, /actionFor\(event\)/);
  assert.match(board, /drawApprovalKey\(displays_\[index\], kGreen, "ALLOW", index \+ 1\)/);
  assert.match(board, /drawApprovalKey\(displays_\[index\], kRed, "DENY", index \+ 1\)/);
  assert.match(board, /"PRESS NOW"/);
  assert.match(board, /drawRoundRect\(6, 6, 116, 116, 14, accent\)/);
  assert.match(board, /drawRoundRect\(7, 7, 114, 114, 13, accent\)/);
  assert.doesNotMatch(board, /drawRoundRect\(3, 3, 122, 122, 17, accent\)/);
  assert.doesNotMatch(board, /drawRect\(2, 2, 124, 124, accent\)/);
});

test("Codex approval allow is exact and cannot match generic navigation controls", () => {
  const allowMatcher = codexApproval.match(/\$allow='([^']+)'/)?.[1] || "";
  assert.doesNotMatch(allowMatcher, /continue|confirm|\|yes/i);
  assert.match(codexApproval, /\$agentId -eq 'hermes' -and \$name -match '\(\?i\)\^\(run\|execute\|运行\|执行\)\$'/);
  assert.match(codexApproval, /allowRuntimeId/);
  assert.match(codexApproval, /HACHIMI_CODEX_APPROVAL_TARGET_ID/);
  assert.match(codexApproval, /GetRuntimeId/);
});

test("Claude approval is scanned only for the bound agent and keeps exact button identity", () => {
  assert.match(codexApproval, /"claude-code" \| "workbuddy" \| "deepseek-harness"/);
  assert.match(codexApproval, /@\('claude'\)/);
  assert.match(codexApproval, /Get-Process -Name \$processNames/);
  assert.match(codexApproval, /prompt\.allow_runtime_id == item\.prompt\.allow_runtime_id/);
  assert.match(codexApproval, /"agent":prompt\.agent_label/);
});

test("WorkBuddy and DeepSeek approvals use bound native windows and configured S3 decisions", () => {
  assert.match(codexApproval, /'workbuddy'\{@\('WorkBuddy'\)/);
  assert.match(codexApproval, /'deepseek-harness'\{@\('DeepSeek Harness'\)/);
  assert.match(codexApproval, /workbuddy\.json/);
  assert.match(codexApproval, /approvalRequestId/);
  assert.match(codexApproval, /pet-manager-approval-decisions/);
  assert.match(codexApproval, /write_hook_decision/);
  assert.match(codexApproval, /deepseek-harness\.json/);
  assert.match(codexApproval, /PermissionRequest/);
  assert.match(codexApproval, /"claude-code" \| "workbuddy" \| "deepseek-harness" \| "hermes" \| "zcode"/);
  assert.match(codexApproval, /last_direct_scan\.elapsed\(\) >= Duration::from_millis\(900\)/);
  assert.match(codexApproval, /WorkBuddy can overwrite PermissionRequest with PreToolUse/);
  assert.match(codexApproval, /允许\|批准\|同意\)\(一次\|写入\|执行\|修改\|操作\|本次\|仅这次\|始终/);
  assert.match(codexApproval, /ControlType\]::ListItem/);
  assert.match(codexApproval, /SelectionItemPattern\]::Pattern/);
  assert.match(codexApproval, /LegacyIAccessiblePattern\]::Pattern/);
  assert.match(codexApproval, /crate::dsh_approval::has_pending\(\)/);
  assert.match(codexApproval, /active_agent != item\.prompt\.agent_id/);
  assert.match(codexApproval, /approval_agent_id\(\) != item\.prompt\.agent_id/);
  assert.match(tauri, /codex_approval::handle_device_decision\(&payload\)[\s\S]*dsh_approval::handle_device_decision\(&payload\)/);
});

test("ordinary Confirm and Cancel operate the current visible typed composer", () => {
  assert.match(tauri, /extract_visible_composer_button_action/);
  assert.match(tauri, /VisibleComposerButtonAction::Submit/);
  assert.match(tauri, /VisibleComposerButtonAction::Cancel/);
  assert.match(tauri, /inspect_task_draft/);
  assert.match(tauri, /TEXT_INPUT_EMPTY/);
  assert.match(tauri, /TYPE TEXT FIRST/);
  assert.match(tauri, /DRAFT CLEARED/);
  assert.match(codexComposer, /dispatch_task_from_draft/);
  assert.match(codexComposer, /clear_task_draft/);
  assert.match(codexComposer, /'clear_task_draft' \{[\s\S]*Assert-TaskDraftSnapshot \$state \$command\.draftSnapshot[\s\S]*DESKTOP_TASK_CLEAR_UNCONFIRMED/);
  assert.match(tauri, /bridge\.clear_task_draft\(&snapshot\)/);
  assert.doesNotMatch(tauri, /bridge\.update_confirmed\(1, ""\)/);
  assert.match(codexComposer, /@\('codex','claude','workbuddy','deepseek-harness','opencode','hermes','zcode'\)/);
  assert.match(codexComposer, /Get-TaskDraftSnapshot \$state \$false/);
  assert.match(codexComposer, /Set-ComposerText \$state '' \$false \$true/);
  assert.match(codexComposer, /Invoke-SendButton \$state \$true \(\$state\.Agent -eq 'codex'\)/);
  assert.match(codexComposer, /return 'enter'/);
  assert.match(codexComposer, /\$state\.CurrentTask -and \$state\.Agent -eq 'claude'[\s\S]*PressEnter\(\)[\s\S]*return 'enter'/);
  assert.match(codexComposer, /Assert-TaskDraftSnapshot[\s\S]*Get-TaskWindowIdentity \$state\.Root \$state\.Agent/);
});

test("current Claude actions restore an existing minimized window without launching Claude", () => {
  assert.match(codexComposer, /function Get-RestoredClaudeWindows/);
  assert.match(codexComposer, /if \(\$agent -eq 'claude'\) \{\s*\$windows = @\(Get-RestoredClaudeWindows\)/);
  assert.match(codexComposer, /if \(\$desktopProcesses\.Count -eq 0\) \{ return @\(\) \}/);
  assert.doesNotMatch(
    codexComposer.match(/function Get-CurrentVisibleTarget[\s\S]*?function Assert-WorkBuddyComposerCurrent/)?.[0] || "",
    /Get-OrLaunchClaudeWindows|Start-Process/,
  );
});

test("S3 WAV transport batches acknowledgements but keeps final byte proof", () => {
  assert.match(playback, /const CHUNKS_PER_BATCH: usize = 8/);
  assert.match(playback, /send_confirmed_batch/);
  assert.match(playback, /receivedBytes[\s\S]*pcm\.len\(\)/);
  assert.match(playback, /writtenBytes[\s\S]*pcm\.len\(\) \* 6/);
});

test("S3 realtime conversation is half duplex, session bound, and resumes capture", () => {
  // 版本只校验格式；半双工行为由下方精确 capability 和会话契约断言保证。
  assert.match(firmware, /kFirmwareVersion\[\]\s*=\s*"\d+\.\d+\.\d+-s3-wifi"/);
  assert.match(firmware, /realtimeConversation.*s3-half-duplex-v1/);
  assert.match(firmware, /requestTopic.*topic/);
  assert.match(audio, /handleConversation\(JsonObjectConst payload\)/);
  assert.match(audio, /conversationSessionId.*conversation_session_/);
  assert.match(audio, /suspendConversationCapture\(\)/);
  assert.match(audio, /resumeConversationCapture\(\)/);
  assert.match(audio, /audio\/query.*sendStatus\("query"\)/);
  assert.match(audio, /conversation_flush/);
});

test("S3 capture mixes both onboard microphones and detects MIC3 beyond the AUX hint pin", () => {
  assert.match(audio, /pinMode\(tkeyboard::pins::aux_detect, INPUT_PULLUP\)/);
  assert.match(audio, /capture_uses_mic3_ = digitalRead\(tkeyboard::pins::aux_detect\) == LOW/);
  assert.match(audio, /configureCaptureCodec\(true\)/);
  assert.match(audio, /auxRms\(capture_raw_\)/);
  assert.match(audio, /kAuxSignalFramesRequired/);
  assert.match(audio, /include_aux \? 0x02 : 0x00/);
  assert.match(audio, /include_aux \? 0x00 : 0xFF/);
  assert.match(audio, /upperSample\(left\)[\s\S]*upperSample\(right\)[\s\S]*lowerSample\(left\)/);
  assert.match(audio, /sum \/ \(include_aux \? 3 : 2\)/);
  assert.match(audio, /captureInputMode/);
  assert.match(firmware, /captureChannels"\] = 3/);
});

test("S3 PTT and configured action keys own their corresponding displays", () => {
  assert.match(firmware, /setVoiceCaptureActive\(true\)/);
  assert.match(board, /"SPEAKING"/);
  assert.match(board, /drawVoiceCaptureFrame\(false\)/);
  assert.match(board, /shortKeyForAction\("page_enter"\)[\s\S]*"SEND"/);
  assert.match(board, /shortKeyForAction\("page_back"\)[\s\S]*"CANCEL"/);
});

test("S3 keeps Agent work status on screen five when screen two is SEND", () => {
  assert.match(board, /drawPanel\(displays_\[4\], state_color, state\.c_str\(\), agent, "AGENT \/ TASK"\)/);
  assert.match(board, /drawActionKey\(displays_\[confirm_key - 1\], kGreen, "SEND"/);
  assert.doesNotMatch(board, /drawPanel\(displays_\[1\], state_color, state\.c_str\(\), agent/);
  assert.match(board, /edge_light_target_mask_ = \(1U << 2\) \| \(1U << 4\)/);
});

test("S3 edge lights clear their path and leave no completed-state rail", () => {
  assert.match(board, /draw_trail\(edge_light_frame_ - 1, true\)/);
  assert.match(board, /erase \? kBackground : scale565\(edge_light_color_, brightness\)/);
  assert.match(board, /edge_light_complete_ = true/);
  assert.doesNotMatch(board, /const uint16_t settled/);
  assert.doesNotMatch(board, /drawRoundRect\([^\n]*edge_light_color_/);
  assert.doesNotMatch(board, /drawRoundRect\([^\n]*kBackground/);
});

test("S3 realtime playback accepts both completion receipts and surfaces playback failure", () => {
  assert.match(realtime, /topic == "audio\/diagnostic" \|\| topic == "audio\/play-state"/);
  assert.match(realtime, /state_complete[\s\S]*writtenBytes/);
  assert.match(realtime, /device_playback_failed/);
});

test("native realtime audio scope binds the negotiated runtime instead of P4 only", () => {
  assert.match(realtime, /runtime:\s*usb_status\.runtime\.clone\(\)/);
  assert.match(realtime, /scope\.runtime/);
  assert.match(acks, /esp32-s3-tkeyboard/);
  assert.doesNotMatch(acks, /incoming==.*Some\("esp-p4"\)/);
});

test("S3 appearance gallery exports and transactionally activates bounded RGB565 animation", () => {
  assert.match(firmware, /s3Avatar.*s3-avatar-rgb565-v3/);
  assert.match(firmware, /display\/avatar_ack/);
  assert.match(avatar, /128U \* 128U \* 2U/);
  assert.match(avatar, /kMaxTotalFrames = 84/);
  assert.match(avatar, /MALLOC_CAP_SPIRAM/);
  assert.match(avatar, /running_checksum_ != expected_checksum_/);
  assert.match(board, /drawFullFrame\(avatar_\.frame\(\)\)/);
  assert.match(serial, /fps=\{sampling_fps:\.6\},scale=128:128/);
  assert.match(serial, /display\/avatar_begin/);
  assert.match(serial, /display\/avatar_chunk/);
  assert.match(serial, /display\/avatar_end/);
  assert.match(tauri, /UsbAppearanceSyncRuntime::Esp32S3/);
  assert.match(dashboard, /s3-avatar-rgb565-v3/);
  assert.match(avatar, /LittleFS\.rename/);
  assert.match(avatar, /selectState/);
  assert.match(serial, /"approval"\.into\(\)/);
  assert.match(assignment, /isS3AvatarRuntime[\s\S]*esp32-s3-tkeyboard[\s\S]*if \(!skipAppearanceSync && !isS3AvatarRuntime\)[\s\S]*reconcileDesktopPetAppearance/);
});

test("S3 avatar actions sample six frames across the full source timeline", () => {
  assert.match(serial, /probe_s3_video/);
  assert.match(serial, /allocate_s3_frames/);
  assert.match(serial, /MAX_TOTAL_FRAMES: u32 = 84/);
  assert.match(serial, /S3_MAX_FRAMES_PER_STATE: u32 = 6/);
  assert.match(serial, /probe\.frames\.min\(S3_MAX_FRAMES_PER_STATE\)/);
  assert.match(serial, /probe\.duration_ms\.div_ceil\(frame_count as u32\)/);
  assert.match(avatar, /kMaxFramesPerState = 84/);
  assert.match(avatar, /kMaxTotalFrames = 84/);
  assert.match(serial, /pad=128:128:[^"\n]*color=black/);
  assert.match(serial, /selected\.len\(\) >= 14/);
  assert.match(firmware, /component_center[\s\S]*selectNextAvatarState\(\)[\s\S]*return true/);
  assert.match(avatar, /selectNextState/);
  assert.doesNotMatch(board, /renderSystem\(system_dirty_\)/);
  assert.match(board, /drawPanel\(displays_\[4\], state_color, state\.c_str\(\), agent, "AGENT \/ TASK"\)/);
  assert.doesNotMatch(board, /millis\(\) - last_render_ < 10000/);
});

test("screen one uses a dedicated uninterrupted full-frame transaction", () => {
  const display = readFileSync(join(here, "../../firmware-s3/src/voice_display.cpp"), "utf8");
  assert.match(board, /displays_\[0\]\.drawFullFrame\(avatar_\.frame\(\)\)/);
  assert.match(display, /startWrite\(\)[\s\S]*setAddrWindow\(0, 0, 128, 128\)[\s\S]*writePixels\(pixels, 128 \* 128, true\)[\s\S]*endWrite\(\)/);
});

test("five-screen layout uses bounded wrapping instead of fixed byte truncation", () => {
  assert.match(board, /drawWrappedText/);
  assert.match(board, /panel\.textWidth\(candidate\) > kPanelTextWidth/);
  assert.match(board, /value \+= "\.\.\."/);
  assert.doesNotMatch(board, /substring\(0, 18\)/);
  for (const label of ["STATUS", "ACTION", "MESSAGE", "CONTROLS"]) {
    assert.match(board, new RegExp(`"${label}"`));
  }
});

test("information panels keep text inside the rounded display safe area", () => {
  assert.match(board, /kPanelTextLeft = 11/);
  assert.match(board, /kPanelTextWidth = 106/);
  assert.match(board, /drawString\(title, kPanelTextLeft, 10\)/);
  assert.match(board, /drawString\(value, kPanelTextLeft/);
  assert.match(board, /drawString\(bounded, kPanelTextLeft, 109\)/);
  assert.doesNotMatch(board, /drawString\(title, 7, 9\)/);
});
