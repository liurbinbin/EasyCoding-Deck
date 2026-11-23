// 翻译监听不得阻塞原生启动握手和配置读取；DOM 调度由独立兼容层负责。
import React, { createContext, useContext, useEffect, useMemo, useSyncExternalStore } from "react";
import { observeLocaleDom } from "./locale-dom.js";
import { translateText } from "./locale-catalog.js";
export { translateText } from "./locale-catalog.js";

export const LOCALE_STORAGE_KEY = "easycoding-deck-locale";
export const LOCALES = Object.freeze({ ZH: "zh-CN", EN: "en-US" });

let locale = LOCALES.ZH;
try {
  const saved = globalThis.localStorage?.getItem(LOCALE_STORAGE_KEY);
  locale = saved === LOCALES.EN || saved === LOCALES.ZH
    ? saved
    : (globalThis.navigator?.language?.toLowerCase().startsWith("en") ? LOCALES.EN : LOCALES.ZH);
} catch {
  locale = LOCALES.ZH;
}

const listeners = new Set();
const subscribe = listener => {
  listeners.add(listener);
  return () => listeners.delete(listener);
};
const getSnapshot = () => locale;

export function setLocale(nextLocale) {
  const next = nextLocale === LOCALES.EN ? LOCALES.EN : LOCALES.ZH;
  if (next === locale) return;
  locale = next;
  try { globalThis.localStorage?.setItem(LOCALE_STORAGE_KEY, next); } catch { /* 存储不可用时仍可切换当前界面。 */ }
  listeners.forEach(listener => listener());
}

export function localize(value) {
  return locale === LOCALES.EN ? translateText(value) : value;
}

export function useLocale() {
  return useSyncExternalStore(subscribe, getSnapshot, getSnapshot);
}

export const LocaleContext = createContext({ locale, setLocale, isEnglish: locale === LOCALES.EN });

export function LocaleProvider({ children }) {
  const currentLocale = useLocale();
  const value = useMemo(() => ({
    locale: currentLocale,
    setLocale,
    isEnglish: currentLocale === LOCALES.EN,
  }), [currentLocale]);
  useEffect(() => observeLocaleDom(document.getElementById("root"), currentLocale, translateText), [currentLocale]);
  return React.createElement(LocaleContext.Provider, { value }, children);
}

export function useLocaleContext() {
  return useContext(LocaleContext);
}
