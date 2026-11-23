// 真实浏览器回归，不启动桌面应用、不访问设备，也不执行编译。
import test, { before, after } from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";
import { ENGLISH_DETAILS } from "../src/lib/locale-en.js";
import { buildUsageHelp } from "../src/lib/usage-help.js";

const asModule = source => `data:text/javascript;base64,${Buffer.from(source).toString("base64")}`;
const detailsUrl = asModule(readFileSync(new URL("../src/lib/locale-en.js", import.meta.url), "utf8"));
const dynamicUrl = asModule(readFileSync(new URL("../src/lib/locale-dynamic.js", import.meta.url), "utf8"));
const catalogUrl = asModule(readFileSync(new URL("../src/lib/locale-catalog.js", import.meta.url), "utf8")
  .replace('"./locale-en.js"', JSON.stringify(detailsUrl))
  .replace('"./locale-dynamic.js"', JSON.stringify(dynamicUrl)));

const domSource = readFileSync(new URL("../src/lib/locale-dom.js", import.meta.url), "utf8");
const moduleUrl = `data:text/javascript;base64,${Buffer.from(domSource).toString("base64")}`;

test("完整实际词库的多行小字、动态按键、textarea 提示与选项可来回切换", { timeout: 20000 }, async () => {
  const help = buildUsageHelp([
    { id: "voice", label: "按键 1 长按", defaultAction: "voice_ptt" },
    { id: "confirm", label: "按键 2 短按", defaultAction: "page_enter" },
    { id: "chat", label: "按键 5 长按", defaultAction: "realtime_chat" },
  ]);
  const copy = [...Object.keys(ENGLISH_DETAILS), `${help.voice} ${help.confirm}`, help.chat];
  await withPage(async page => {
    const result = await page.evaluate(async ({ url, copy }) => {
      const { translateText } = await import(url);
      const root = document.getElementById("root");
      root.replaceChildren();
      for (const text of copy) {
        const paragraph = document.createElement("p");
        paragraph.textContent = `\n  ${text}\n`;
        root.append(paragraph);
      }
      const editor = document.createElement("textarea");
      editor.placeholder = "输入要发送到当前会话的测试文本";
      editor.value = "继续当前任务。设备";
      root.append(editor);
      const userName = document.createElement("strong");
      userName.setAttribute("data-locale-ignore", "true");
      userName.textContent = "设备";
      root.append(userName);
      const select = document.createElement("select");
      const group = document.createElement("optgroup");
      group.label = "当前音色";
      const option = document.createElement("option");
      option.value = "power_off";
      option.textContent = "关机断电";
      group.append(option);
      select.append(group);
      root.append(select);
      let stop = localeDom.observeLocaleDom(root, "en-US", translateText);
      const remaining = [...root.querySelectorAll("p")].map(node => node.textContent).filter(text => /[一-鿿]/u.test(text));
      const englishPlaceholder = editor.placeholder;
      const optionText = option.textContent;
      const groupLabel = group.label;
      stop();
      for (let i = 0; i < 3; i++) {
        stop = localeDom.observeLocaleDom(root, "zh-CN", translateText);
        stop();
        stop = localeDom.observeLocaleDom(root, "en-US", translateText);
        stop();
      }
      stop = localeDom.observeLocaleDom(root, "zh-CN", translateText);
      stop();
      return {
        remaining, englishPlaceholder, optionText, groupLabel,
        restored: [...root.querySelectorAll("p")].every((node, index) => node.textContent === `\n  ${copy[index]}\n`),
        value: editor.value, name: userName.textContent, optionValue: option.value,
        restoredPlaceholder: editor.placeholder,
      };
    }, { url: catalogUrl, copy });
    assert.deepEqual(result.remaining, []);
    assert.equal(result.restored, true);
    assert.equal(result.englishPlaceholder, "Enter test text to send to the current session");
    assert.equal(result.restoredPlaceholder, "输入要发送到当前会话的测试文本");
    assert.equal(result.optionText, "Power off");
    assert.equal(result.optionValue, "power_off");
    assert.equal(result.groupLabel, "Current voice");
    assert.equal(result.value, "继续当前任务。设备");
    assert.equal(result.name, "设备");
  });
});

let browser;
before(async () => {
  // 复用进程但逐用例隔离 context，避免频繁启动系统 Edge 引起调度抖动。
  browser = await chromium.launch({
    headless: true,
    args: ["--disable-background-timer-throttling", "--disable-renderer-backgrounding", "--disable-backgrounding-occluded-windows"],
    ...(process.platform === "win32" ? { channel: "msedge" } : {}),
  });
});
after(async () => { await browser?.close(); });

async function withPage(check) {
  const context = await browser.newContext();
  try {
    const page = await context.newPage();
    await page.setContent('<div id="root"><span id="status">正在打开管理端…</span><button title="设备" aria-label="设备">设备</button><input placeholder="设备"></div>');
    await page.evaluate(async (url) => {
      window.localeDom = await import(url);
      window.translate = value => ({ "设备": "Device", "正在打开管理端…": "Opening…", "已连接": "Connected" })[value] || value;
    }, moduleUrl);
    await check(page);
  } finally {
    await context.close();
  }
}

for (const locale of ["zh-CN", "en-US"]) {
  test(`${locale}: 翻译监听允许异步启动和定时器继续执行，且无自触发循环`, { timeout: 15000 }, async () => {
    await withPage(async page => {
      const result = await page.evaluate(async locale => {
        const root = document.getElementById("root");
        let mutations = 0;
        const counter = new MutationObserver(records => { mutations += records.length; });
        counter.observe(root, { subtree: true, childList: true, characterData: true, attributes: true });
        const stop = localeDom.observeLocaleDom(root, locale, translate);
        await new Promise(resolve => setTimeout(resolve, 20));
        // 模拟原生配置响应：异步从加载文案切换到设备页面。
        root.querySelector("#status").textContent = "已连接";
        await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        const settled = mutations;
        await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
        const result = { text: root.querySelector("#status").textContent, settled, final: mutations };
        stop();
        counter.disconnect();
        return result;
      }, locale);
      assert.equal(result.text, locale === "en-US" ? "Connected" : "已连接");
      assert.equal(result.final, result.settled, "空闲期间不能继续生成 DOM 变更");
      assert.ok(result.final < 20, `DOM 变更不应失控：${result.final}`);
    });
  });
}

test("重复翻译零写入；英中切换恢复文字及 placeholder、title、ARIA", { timeout: 15000 }, async () => {
  await withPage(async page => {
    const result = await page.evaluate(async () => {
      const root = document.getElementById("root");
      localeDom.translateLocaleDom(root, "en-US", translate);
      let mutations = 0;
      const counter = new MutationObserver(records => { mutations += records.length; });
      counter.observe(root, { subtree: true, characterData: true, attributes: true });
      localeDom.translateLocaleDom(root, "en-US", translate);
      await Promise.resolve();
      const repeatedWrites = mutations;
      counter.disconnect();
      for (let i = 0; i < 3; i++) {
        localeDom.translateLocaleDom(root, "zh-CN", translate);
        localeDom.translateLocaleDom(root, "en-US", translate);
      }
      localeDom.translateLocaleDom(root, "zh-CN", translate);
      return {
        repeatedWrites,
        text: root.querySelector("button").textContent,
        title: root.querySelector("button").title,
        aria: root.querySelector("button").getAttribute("aria-label"),
        placeholder: root.querySelector("input").placeholder,
      };
    });
    assert.deepEqual(result, { repeatedWrites: 0, text: "设备", title: "设备", aria: "设备", placeholder: "设备" });
  });
});

test("动态属性更新可以翻译且停止监听会取消待执行任务", { timeout: 15000 }, async () => {
  await withPage(async page => {
    const result = await page.evaluate(async () => {
      const root = document.getElementById("root");
      const button = root.querySelector("button");
      let stop = localeDom.observeLocaleDom(root, "en-US", translate);
      button.title = "已连接";
      await new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve)));
      const translated = button.title;
      button.title = "设备";
      await Promise.resolve();
      stop();
      await new Promise(resolve => setTimeout(resolve, 100));
      const afterStop = button.title;
      stop = localeDom.observeLocaleDom(root, "zh-CN", translate);
      const restored = button.title;
      stop();
      return { translated, afterStop, restored };
    });
    assert.deepEqual(result, { translated: "Connected", afterStop: "设备", restored: "设备" });
  });
});

test("React 异步启动与页面增删不会被翻译监听阻塞", { timeout: 15000 }, async () => {
  await withPage(async page => {
    await page.addScriptTag({ path: fileURLToPath(new URL("../node_modules/react/umd/react.development.js", import.meta.url)) });
    await page.addScriptTag({ path: fileURLToPath(new URL("../node_modules/react-dom/umd/react-dom.development.js", import.meta.url)) });
    const errors = [];
    page.on("pageerror", error => errors.push(error.message));
    await page.evaluate(() => {
      const container = document.getElementById("root");
      container.replaceChildren();
      const { createElement: h, useEffect, useState } = React;
      function App() {
        const [loaded, setLoaded] = useState(false);
        useEffect(() => {
          if (loaded) return undefined;
          const timer = setTimeout(() => setLoaded(true), 30);
          return () => clearTimeout(timer);
        }, [loaded]);
        return loaded
          ? h("section", { id: "device" }, h("h1", null, "设备"), h("button", { onClick: () => setLoaded(false) }, "已连接"))
          : h("div", { id: "loading" }, "正在打开管理端…");
      }
      ReactDOM.createRoot(container).render(h(React.StrictMode, null, h(App)));
      window.stopLocale = localeDom.observeLocaleDom(container, "en-US", translate);
    });
    await page.waitForFunction(() => document.querySelector("#device h1")?.textContent === "Device");
    await page.locator("#device button").click();
    await page.waitForFunction(() => document.querySelector("#device h1")?.textContent === "Device");
    await page.evaluate(() => window.stopLocale());
    assert.deepEqual(errors, []);
  });
});
