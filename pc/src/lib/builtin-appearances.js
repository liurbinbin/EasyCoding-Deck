/**
 * [Input] Static appearance assets bundled under `pc/public`.
 * [Output] The twelve bundled appearance records, including Clawd's default state-specific WAV cues, that are prepended to app-local records.
 * [Pos] lib node in pc/src/lib
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

export const BUILTIN_TERRIER_APPEARANCE_ID = "builtin-terrier";

const BUNDLED_APPEARANCES = [
  {
    id: "c867abad-fabb-4704-b6ee-22d6fc0052fd",
    name: "你好世界",
    description: "内置的极客工作流动态形象。",
    model: "uploaded-mp4",
    created_at: "2026-11-12T06:29:38.711Z",
    families: ["welcome", "idle.playing", "idle.wandering", "working", "waiting_user", "done", "error", "touch.lick", "touch.what", "idle.default", "idle.jumping", "touch.right", "touch.left"],
  },
  { id: "codex-tiny-luffy", name: "Luffy", description: "Codex 内置像素形象。", created_at: "2026-11-12T02:50:42.300Z" },
  { id: "codex-ikun", name: "鸡哥ikun", description: "Codex 内置像素形象。", created_at: "2026-11-12T02:48:19.092Z" },
  { id: "codex-xiaoxin-pet", name: "小新", description: "Codex 内置像素形象。", created_at: "2026-11-12T02:46:06.251Z" },
  { id: "codex-clawd", name: "Clawd", description: "基于 Claude Code 像素 Clawd 动作的内置形象。", created_at: "2026-11-12T02:05:24.993Z" },
  { id: "codex-clawd-chef", name: "Clawd Chef", description: "Codex 内置像素形象。", created_at: "2026-11-12T02:04:28.807Z" },
  { id: "codex-hanbao", name: "憨包", description: "Codex 内置像素形象。", created_at: "2026-11-10T05:50:38.655Z" },
  { id: "codex-voira-deepseek", name: "DeepSeek · 蓝色大肥鱼", description: "Codex 内置像素形象。", created_at: "2026-11-10T03:35:48.174Z" },
  { id: "codex-naiwa-leslie-6ae3f2b8", name: "奶蛙", description: "Codex 内置像素形象。", created_at: "2026-11-10T02:06:31.103Z" },
  { id: "codex-naigou", name: "Milky Dog", description: "Codex 内置像素形象。", created_at: "2026-11-10T02:00:25.773Z" },
  { id: "codex-bsod", name: "BSOD", description: "Codex 内置像素形象。", created_at: "2026-11-10T01:11:23.778Z" },
];

const CODEX_FAMILIES = [
  "idle.default", "touch.right", "touch.left", "welcome", "idle.jumping",
  "error", "waiting_user", "working", "done",
];

const TERRIER_FAMILIES = [
  "welcome",
  "idle.playing",
  "idle.wandering",
  "working",
  "working.thinking",
  "working.typing",
  "working.browsing",
  "waiting_user",
  "done",
  "error",
  "touch.lick",
  "touch.what",
  "idle.begging",
  "idle.daydreaming",
  "idle.eating",
  "idle.reading",
  "idle.traveling",
];

const TERRIER_FAMILY_VIDEO_SOURCE = Object.freeze({
  working: "working.thinking",
});

export const DEFAULT_AUDIO_CUE_BY_FAMILY = Object.freeze({
  done: "done",
  error: "error",
  waiting_user: "waiting_user",
});

function terrierVideoSrc(family) {
  const sourceFamily = TERRIER_FAMILY_VIDEO_SOURCE[family] || family;
  return `/terrier-clips/${sourceFamily}.mp4`;
}

function terrierAudioCueSrc(cue) {
  return `/terrier-clips/${cue}.wav`;
}

export function defaultAudioCueSrcForFamily(family) {
  const cue = DEFAULT_AUDIO_CUE_BY_FAMILY[family];
  return cue ? terrierAudioCueSrc(cue) : "";
}

export function createBuiltinTerrierAppearance() {
  return {
    schema_version: 1,
    id: BUILTIN_TERRIER_APPEARANCE_ID,
    type: "builtin",
    name: "Clawd",
    description: "EasyCoding Deck 内置的橙色像素桌宠。",
    provider: "builtin",
    model: "clawd-v2",
    base_url: "",
    thinking_model: "",
    persona: { source: "clawd-v2" },
    source_image: "",
    source_image_src: "/terrier-clips/thumbs/welcome.jpg",
    source_mime: "image/jpeg",
    families: TERRIER_FAMILIES.map((family) => {
      const audioSrc = defaultAudioCueSrcForFamily(family);
      const audioCue = audioSrc
        ? {
            audioPath: audioSrc,
            audioSrc,
            audioDefault: true,
          }
        : {};
      return {
        family,
        ok: true,
        prompt: "builtin Clawd clip",
        videoPath: "",
        videoSrc: terrierVideoSrc(family),
        taskId: `builtin-terrier-${family}`,
        videoUrl: terrierVideoSrc(family),
        ...audioCue,
      };
    }),
    created_at: "2026-05-08T11:08:26.256Z",
    absolute_dir: "builtin://terrier-clips",
  };
}

function createBundledAppearance(definition) {
  const root = `/builtin-appearances/${definition.id}`;
  const families = definition.families || CODEX_FAMILIES;
  const hasPreview = definition.id !== "c867abad-fabb-4704-b6ee-22d6fc0052fd";
  return {
    schema_version: 1,
    id: definition.id,
    type: "builtin",
    name: definition.name,
    description: definition.description,
    provider: "builtin",
    model: definition.model || "codex-pet-v2",
    base_url: "",
    thinking_model: "",
    persona: { source: "builtin", pet_id: definition.id.replace(/^codex-/, "") },
    source_image: "",
    source_image_src: hasPreview ? `${root}/preview.png` : "",
    source_mime: hasPreview ? "image/png" : "",
    families: families.map((family) => ({
      family,
      ok: true,
      prompt: "bundled appearance clip",
      videoPath: "",
      videoSrc: `${root}/videos/${family}.mp4`,
      taskId: `builtin-${definition.id}-${family}`,
      videoUrl: `${root}/videos/${family}.mp4`,
    })),
    created_at: definition.created_at,
    absolute_dir: `builtin://builtin-appearances/${definition.id}`,
  };
}

export function listBuiltinAppearances() {
  return [
    createBuiltinTerrierAppearance(),
    ...BUNDLED_APPEARANCES.map(createBundledAppearance),
  ];
}

export function isBuiltinAppearanceId(id) {
  return listBuiltinAppearances().some((record) => record.id === id);
}

export function mergeBuiltinAppearances(records = []) {
  const builtin = listBuiltinAppearances();
  const builtinIds = new Set(builtin.map((record) => record.id));
  return [
    ...builtin,
    ...(records || []).filter((record) => !builtinIds.has(record?.id)),
  ];
}
