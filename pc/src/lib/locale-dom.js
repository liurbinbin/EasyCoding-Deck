// 兼容翻译层：只写入发生变化的内容，并隔离自身 DOM 更新，避免阻塞启动。
const textValues = new WeakMap();
const attributeValues = new WeakMap();
const attributes = ["placeholder", "title", "aria-label", "aria-description", "alt", "label"];

function shouldSkip(element, forAttributes = false) {
  return !element
    || element.closest("[data-locale-ignore], script, style, code, pre")
    || (!forAttributes && element.closest("textarea"))
    || element.isContentEditable;
}

function resolveValue(values, key, current, english, translate) {
  const previous = values.get(key);
  // 保存实际写入值，而不是重新推断当前语言，确保英中切换能恢复原文。
  const original = previous && current === previous.rendered ? previous.original : current;
  const rendered = english ? translate(original) : original;
  values.set(key, { original, rendered });
  return rendered;
}

export function translateLocaleDom(root, locale, translate) {
  if (!root) return;
  const document = root.ownerDocument;
  if (document.documentElement.lang !== locale) document.documentElement.lang = locale;
  const english = locale === "en-US";
  const walker = document.createTreeWalker(root, 4); // NodeFilter.SHOW_TEXT
  let node;
  while ((node = walker.nextNode())) {
    if (shouldSkip(node.parentElement)) continue;
    const current = node.nodeValue || "";
    const next = resolveValue(textValues, node, current, english, translate);
    if (current !== next) node.nodeValue = next;
  }
  for (const element of root.querySelectorAll("*")) {
    if (shouldSkip(element, true)) continue;
    let values = attributeValues.get(element);
    if (!values) {
      values = new Map();
      attributeValues.set(element, values);
    }
    for (const attribute of attributes) {
      if (!element.hasAttribute(attribute)) {
        values.delete(attribute);
        continue;
      }
      const current = element.getAttribute(attribute) || "";
      const next = resolveValue(values, attribute, current, english, translate);
      if (current !== next) element.setAttribute(attribute, next);
    }
  }
}

export function observeLocaleDom(root, locale, translate) {
  if (!root) return () => {};
  const view = root.ownerDocument.defaultView;
  let stopped = false;
  let frame = null;
  const options = {
    childList: true,
    subtree: true,
    characterData: true,
    attributes: true,
    attributeFilter: attributes,
  };
  const observer = new view.MutationObserver(() => {
    if (stopped || frame !== null) return;
    // 使用帧任务合并外部更新，避免 MutationObserver 微任务链饿死 IPC 和渲染。
    frame = view.requestAnimationFrame(() => {
      frame = null;
      if (!stopped) apply();
    });
  });
  function apply() {
    // 翻译产生的变更不进入监听队列；恢复监听前仍完成当前所有 DOM 的处理。
    observer.disconnect();
    try {
      translateLocaleDom(root, locale, translate);
    } finally {
      if (!stopped) observer.observe(root, options);
    }
  }
  apply();
  return () => {
    stopped = true;
    observer.disconnect();
    if (frame !== null) view.cancelAnimationFrame(frame);
    frame = null;
  };
}
