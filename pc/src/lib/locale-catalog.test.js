// 仅测试 UI 文案，不请求模型、不连接设备、不编译前端。
import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { createRequire } from "node:module";
import { translateText } from "./locale-catalog.js";
import { buildUsageHelp } from "./usage-help.js";

const require = createRequire(import.meta.url);
const { parse } = require("@babel/parser");
const hasChinese = text => /[一-鿿]/u.test(text);
const sourceFiles = [
  "CustomAvatarWizard.jsx", "AppearanceGallery.jsx", "AppearanceDetail.jsx", "ApiSettings.jsx",
  "LlmNetworkSettings.jsx", "TextModelSelect.jsx", "ImageModelSelect.jsx", "VideoGenerationSettings.jsx",
  "GenerationDiagnostics.jsx", "PersonaVoiceModal.jsx", "GlobalAudioCuesPanel.jsx", "UsageHelp.jsx",
  "dashboard/BoardButtonPanel.jsx", "dashboard/S3WifiPanel.jsx", "dashboard/S3HardwarePanel.jsx",
  "dashboard/VoiceAssistantPanel.jsx", "dashboard/DeviceVolumePanel.jsx", "dashboard/RealtimeChatPanel.jsx",
  "shell/PageOnboardingModal.jsx", "shell/ContextRail.jsx", "shell/FirmwareUpdateNavItem.jsx",
  "dashboard/FirmwareUpdateModal.jsx", "dashboard/DeviceDiagnosticsModal.jsx", "dashboard/SerialLogPage.jsx",
];
const visibleAttributes = new Set(["title", "subtitle", "placeholder", "aria-label", "label", "help", "summary", "description", "alt"]);

for (const file of sourceFiles) {
  test(`${file} 的固定 JSX 小字、标签和辅助属性有英文覆盖`, () => {
    const source = readFileSync(new URL(`../${file}`, import.meta.url), "utf8");
    const ast = parse(source, { sourceType: "module", plugins: ["jsx"] });
    const missing = new Set();
    function visit(node) {
      if (!node || typeof node !== "object") return;
      let value;
      if (node.type === "JSXText") value = node.value;
      if (node.type === "JSXAttribute" && visibleAttributes.has(node.name?.name) && node.value?.type === "StringLiteral") value = node.value.value;
      if (typeof value === "string" && hasChinese(value) && hasChinese(translateText(value))) missing.add(value.trim());
      for (const [key, value] of Object.entries(node)) {
        if (["loc", "start", "end", "extra", "comments", "tokens"].includes(key)) continue;
        if (Array.isArray(value)) value.forEach(visit);
        else if (value && typeof value === "object") visit(value);
      }
    }
    visit(ast);
    assert.deepEqual([...missing], [], `未覆盖文案：\n${[...missing].join("\n")}`);
  });
}

test("用户列出的硬件数量、状态及 Agent 扫描说明不能只翻译前半句", () => {
  for (const value of [
    "设备按钮配置与客户端一致。",
    "5 块 GC9107", "5 个带屏按键 · 非触摸", "ES7210 就绪", "14 灯 · QWIIC",
    "已检测到 8 个 Agent，设备端仅显示当前跟随的 Agent 状态，对话结束后设备气泡保留显示 60 秒",
    "按键 4 · 第 4 块屏幕下方 / 电源键", "按键 1 长按", "6 秒",
    "常用操作 · 按当前配置显示（待同步）",
    "mDNS 结果仅用于发现；连接仍会校验证书指纹、HMAC challenge、board、boot 和 connection generation。",
  ]) {
    assert.equal(hasChinese(translateText(value)), false, value);
  }
  assert.match(translateText("5 块 GC9107"), /5 GC9107/);
  assert.match(translateText("已检测到 8 个 Agent，设备端仅显示当前跟随的 Agent 状态，对话结束后设备气泡保留显示 60 秒"), /60 seconds/);
});

test("动态使用说明随真实按键映射翻译，包含多键、未绑定、关闭与组合句子", () => {
  for (const rows of [
    [],
    [
      { id: "voice", label: "按键 1 长按", defaultAction: "voice_ptt" },
      { id: "confirm", label: "按键 2 短按", defaultAction: "page_enter" },
      { id: "previous", label: "按键 3 短按", defaultAction: "session_previous" },
      { id: "back", label: "按键 4 短按", defaultAction: "page_back" },
      { id: "chat", label: "按键 5 长按", defaultAction: "realtime_chat" },
    ],
    [
      { id: "voice", label: "SW1 长按", defaultAction: "voice_ptt" },
      { id: "voice2", label: "SW2 长按", defaultAction: "voice_ptt" },
      { id: "confirm", label: "SW1 短按", defaultAction: "page_enter" },
      { id: "previous", label: "摇杆向左", defaultAction: "session_previous" },
      { id: "next", label: "摇杆向右", defaultAction: "session_next" },
      { id: "component", label: "SW2 短按", defaultAction: "component_center" },
    ],
  ]) {
    for (const enabled of [true, false]) {
      const help = buildUsageHelp(rows, {}, enabled, true);
      for (const value of Object.values(help).filter(value => typeof value === "string")) {
        assert.equal(hasChinese(translateText(value)), false, value);
      }
      const combined = `${help.voice} ${help.confirm}`;
      assert.equal(hasChinese(translateText(combined)), false, combined);
      assert.equal(hasChinese(translateText(`${help.select}；${help.enter}`)), false);
    }
  }
});

test("多行 JSX 小字正确翻译并保留外部空白", () => {
  const value = "\n  用设备麦克风给 Agent 输入文字，不会自动发送。没说完可以继续按住说，内容会接在后面。\n";
  const translated = translateText(value);
  assert.equal(hasChinese(translated), false);
  assert.ok(translated.startsWith("\n  "));
  assert.ok(translated.endsWith("\n"));
});

test("动态模板不更改 SSID、endpoint、模型 ID 或用户名称", () => {
  for (const value of ["我的 Wi-Fi", "wss://192.168.36.37:8443/device/v1", "ep-private-model-id", "用户写的中文指令", "我家的小猫"])
    assert.equal(translateText(value), value);
  assert.match(translateText("输入my-provider API Key"), /my-provider/);
});
