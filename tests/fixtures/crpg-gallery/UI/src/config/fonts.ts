/**
 * Font configuration table.
 *
 * Central registry of every font the UI can use. Each entry is a FontConfig:
 *   - id:     stable key (kept short / kebab-case); referenced from settings.
 *   - label:  localizable display name.
 *   - stack:  CSS font-family value (with web-safe fallbacks).
 *   - source: optional self-hosted file; when present, index.css (or this
 *             module at runtime) is expected to register a matching
 *             @font-face so the name in `stack` resolves locally.
 *
 * The FONT_SLOTS table maps the CSS variables declared in core/theme.css
 *   --font-ui, --font-display, --font-subtitle, --font-mono
 * to the ids that are legal for that slot, plus a default. `applyFont(slot,id)`
 * mutates the custom property on document.documentElement, which cascades to
 * every element that references the variable (base body, Tailwind utilities,
 * inline styles via `font-[family-name:var(--font-display)]`, ...).
 *
 * Adding a new font:
 *   1. Drop the TTF into src/assets/fonts/<lang>/  — pick the language folder
 *      based on what script the font is *primarily* designed to serve:
 *        General/  跨语言(目前只有 Cinzel Decorative,unicode-range 锁在 ASCII 数字)
 *        English/  拉丁系(EB Garamond / IM Fell English / Libre Caslon / 各种花体手写)
 *        Chinese/  中文(LXGW NeoZhiSong Plus / Noto Serif SC)
 *        Japanese/ 日文 kana(Shippori Mincho)
 *      字体多了之后语言分桶比扁平目录好找,也方便按语言裁剪打包(未来可能按 locale
 *      做按需加载)。
 *   2. Add a new entry here with a `source` pointing at the file.
 *   3. List its id in FONT_SLOTS.<slot>.allowedIds.
 *   4. (If self-hosted) add a matching @font-face in index.css, OR rely on
 *      ensureFontFaceInjected() below to lazily register it on first use.
 */

// ── Types ───────────────────────────────────────────────────────

export interface FontSource {
  /** Module-relative or absolute URL (bundled assets resolve via Vite). */
  url: string;
  /** e.g. '200 900' for variable fonts, '400' for static. */
  weight?: string;
  /** 'normal' | 'italic' | 'oblique'. */
  style?: string;
  /**
   * Optional `unicode-range` descriptor — restricts which glyphs this face
   * can supply. Used to scope a display face to a subset (e.g. only digits)
   * so the rest of a mixed stack falls through to a more readable face.
   * Example: 'U+0030-0039' for ASCII digits only.
   */
  unicodeRange?: string;
}

export interface FontConfig {
  id: string;
  label: Record<string, string>;
  stack: string;
  /**
   * Self-hosted file(s). Pass an array to register multiple weights/styles
   * under the same `font-family` (e.g. Regular + Bold + Black) — each entry
   * becomes its own @font-face rule.
   */
  source?: FontSource | FontSource[];
}

export type FontSlot =
  "ui" | "display" | "mono" | "subtitle" | "handWriteTrademarkUpscale";

export interface FontSlotConfig {
  /** CSS custom property written by applyFont(). */
  cssVar: string;
  /** id used when nothing has been persisted yet. */
  defaultId: string;
  /** Whitelist of font ids that may be assigned to this slot. */
  allowedIds: string[];
}

// ── Font catalog ────────────────────────────────────────────────
//
// The asset URL for Noto Serif SC is imported so Vite can fingerprint it in
// production builds. Other fonts that rely on system availability have no
// `source` field — they just hit whatever the OS provides.

// Vite handles the ?url suffix by returning the final bundled path.
//
// We ship the *static* (weight-400, instanced) build of Noto Serif SC, NOT
// the upstream variable font. Ultralight 1.4 (WebKit 615.1) mis-parses the
// HVAR table on large CJK variable fonts: glyph outlines get the right
// per-weight strokes but advance widths fall back to a default master,
// causing characters to overlap or render with zero advance — most visibly
// on settings labels like "辅助功能" rendering as "辅功能" in the game build.
//
// To regenerate this file from the upstream NotoSerifSC-Variable.ttf:
//   pip install fonttools
//   python -c "from fontTools.ttLib import TTFont; \
//              from fontTools.varLib.instancer import instantiateVariableFont; \
//              instantiateVariableFont(TTFont('NotoSerifSC-Variable.ttf'), \
//                {'wght': 400}).save('NotoSerifSC-Regular.ttf')"
//
// Side benefit: the static instance is ~14 MB vs 24 MB for the variable
// source — meaningful savings on first-load time too.
// --------------------------------------------------------------------------
// 字体资源按语言归类的目录约定:
//   assets/fonts/General/    —— 跨语言通用(目前只有 Cinzel Decorative,因为它的
//                                 unicode-range 锁在 ASCII 数字,不属于哪种语言)
//   assets/fonts/Chinese/    —— 中文字形主用(LXGW NeoZhiSong Plus / Noto Serif SC)
//   assets/fonts/English/    —— 拉丁字形(EB Garamond / IM Fell / Libre Caslon
//                                 及英文手写花体若干种)
//   assets/fonts/Japanese/   —— 日文 kana(Shippori Mincho)
//
// 新增字体时按这条 invariant 放对位置;manual scan 翻 fonts.ts 一眼就能看出
// 一个 stack 跨了几种语言的资源。
// --------------------------------------------------------------------------

import notoSerifScUrl from "@/assets/fonts/Chinese/NotoSerifSC-Regular.ttf?url";
// W430 静态实例 —— 同上配方 {'wght': 430} 产出（注意 instantiateVariableFont
// 默认 inplace=False，要接住返回值再 save）。给混合字幕栈的中文提供真字重加粗
// （LXGW 新致宋只有单字重），对话选项 fontWeight:500 时命中它。
// 字重调参史：SemiBold(600) 过重 → Medium(500) 仍略重 → 430 是实测最佳。
// CSS 侧仍注册为 500（整百字重，避开 Ultralight 对非常规数字字重匹配的
// 未知行为），只是该 face 的字形文件是 430 实例。
import notoSerifScW430Url from "@/assets/fonts/Chinese/NotoSerifSC-W430.ttf?url";

// Cinzel Decorative — display face restricted to ASCII digits (U+0030-0039)
// inside the subtitle stack. Letters and punctuation fall through to the
// next face (EB Garamond) so the subtitle reads as classical literature
// rather than carved stone. All three weights (Regular / Bold / Black) are
// registered so consumers (e.g. damage-number floating UI) can pick a real
// heavy face instead of relying on the browser's synthetic-bold rasterizer.
import cinzelDecorativeRegularUrl from "@/assets/fonts/General/Cinzel_Decorative/CinzelDecorative-Regular.ttf?url";
import cinzelDecorativeBoldUrl from "@/assets/fonts/General/Cinzel_Decorative/CinzelDecorative-Bold.ttf?url";
import cinzelDecorativeBlackUrl from "@/assets/fonts/General/Cinzel_Decorative/CinzelDecorative-Black.ttf?url";

// EB Garamond — Latin body face for the subtitle stack. Garamond revivals
// have been the workhorse typeface of literary printing for four centuries;
// EB Garamond is the open-source standard and reads cleanly at subtitle
// sizes. Like Noto Serif SC we ship the static Regular instance to side-
// step the Ultralight HVAR mis-parse on variable fonts.
import ebGaramondRegularUrl from "@/assets/fonts/English/EB_Garamond/static/EBGaramond-Regular.ttf?url";

// LXGW Neo Zhi Song Plus (霞鹜新致宋 Plus) — CJK serif used for the subtitle
// stack instead of Noto Serif SC. From the LXGW author (also behind WenKai
// and XinMing); a refined modern Ming with quieter stroke contrast that
// avoids the "Noto Serif everywhere" look while keeping the literary feel
// the design called for. Single-weight, SIL OFL.
import lxgwNeoZhiSongPlusUrl from "@/assets/fonts/Chinese/LXGW_NeoZhiSong_Plus/LXGWNeoZhiSongPlus.ttf?url";

// Shippori Mincho (しっぽり明朝) — Japanese mincho serif. Scoped via
// unicode-range to kana-only blocks so Japanese hiragana / katakana render
// in a Japanese-designed face while shared kanji continue to come from
// LXGW. This keeps Chinese players from seeing Japanese kanji-form
// variants (骨, 直, 入 etc.) in CJK contexts. SIL OFL.
import shipporiMinchoRegularUrl from "@/assets/fonts/Japanese/Shippori_Mincho/ShipporiMincho-Regular.ttf?url";
import tangerineRegularUrl from "@/assets/fonts/English/Tangerine/Tangerine-Regular.ttf?url";
import tangerineBoldUrl from "@/assets/fonts/English/Tangerine/Tangerine-Bold.ttf?url";

export const FONTS: Record<string, FontConfig> = {
  "noto-serif-sc": {
    id: "noto-serif-sc",
    label: { "zh-CN": "思源宋体", en: "Noto Serif SC" },
    stack: "'Noto Serif SC', 'Cinzel', 'Times New Roman', serif",
    source: { url: notoSerifScUrl, weight: "400", style: "normal" },
  },
  // Standalone Cinzel Decorative face. NOT exposed in any slot's allowedIds —
  // it exists only so its @font-face is registered at startup, letting the
  // 'noto-cinzel-mixed' stack reference it by name. The unicode-range pins
  // it to ASCII digits only; Latin letters fall through to EB Garamond so
  // English copy reads as literary serif, not chiseled display caps. Bold /
  // Black weights live in the same folder if a heavier digit face is later
  // needed.
  "cinzel-decorative": {
    id: "cinzel-decorative",
    label: { "zh-CN": "Cinzel Decorative", en: "Cinzel Decorative" },
    stack: "'Cinzel Decorative', serif",
    source: [
      // 三档字重共用同一 family-name "Cinzel Decorative",CSS `font-weight` 解析时
      // 浏览器按数字 close-match 选最接近的 face:400 → Regular, 700 → Bold, 900 → Black。
      // unicode-range 全部仍然限制在 ASCII 数字范围,letters / 标点继续 fall-through。
      {
        url: cinzelDecorativeRegularUrl,
        weight: "400",
        style: "normal",
        unicodeRange: "U+0030-0039", // ASCII digits 0-9 only
      },
      {
        url: cinzelDecorativeBoldUrl,
        weight: "700",
        style: "normal",
        unicodeRange: "U+0030-0039",
      },
      {
        url: cinzelDecorativeBlackUrl,
        weight: "900",
        style: "normal",
        unicodeRange: "U+0030-0039",
      },
    ],
  },
  tangerine: {
    id: "tangerine",
    label: { "zh-CN": "tangerine", en: "tangerine" },
    stack: "tangerine, Cinzel",
    source: [
      {
        url: tangerineRegularUrl,
        weight: "400",
        style: "normal",
      },
      {
        url: tangerineBoldUrl,
        weight: "700",
        style: "normal",
      },
    ],
  },
  // EB Garamond — Latin literary serif. Used both as a building block of the
  // subtitle stack AND as a standalone choice for the UI / display slots.
  // The stack chains LXGW → Noto Serif SC after Garamond so CJK glyphs (which
  // Garamond doesn't carry) fall through to a Chinese serif rather than the
  // system default — important because UI labels are bilingual.
  "eb-garamond": {
    id: "eb-garamond",
    label: { "zh-CN": "EB Garamond", en: "EB Garamond" },
    stack:
      "'EB Garamond', 'LXGW NeoZhiSong Plus', 'Noto Serif SC', 'Times New Roman', serif",
    source: [{ url: ebGaramondRegularUrl, weight: "400", style: "normal" }],
  },
  // LXGW Neo Zhi Song Plus — registered standalone for the subtitle stack's
  // CJK glyphs. Not exposed as a slot choice directly.
  "lxgw-neozhisong-plus": {
    id: "lxgw-neozhisong-plus",
    label: { "zh-CN": "霞鹜新致宋 Plus", en: "LXGW Neo Zhi Song Plus" },
    stack: "'LXGW NeoZhiSong Plus', 'Noto Serif SC', serif",
    source: [
      { url: lxgwNeoZhiSongPlusUrl, weight: "400", style: "normal" },
      // 500 是「家族别名」：LXGW 新致宋只有单字重，这里把 Noto Serif SC 的
      // W430 静态实例注册进同一 family-name 的 500 槽位，让混合栈的中文在
      // font-weight:500 时命中真字重（而不是合成粗体）。字形从新致宋切到
      // 思源宋的差异在加粗语境下可接受；对话选项面板是首个消费方。
      { url: notoSerifScW430Url, weight: "500", style: "normal" },
    ],
  },
  // Shippori Mincho — Japanese mincho serif scoped to kana blocks. The
  // unicode-range covers Hiragana (U+3040-309F), Katakana (U+30A0-30FF),
  // Katakana Phonetic Extensions (U+31F0-31FF), Halfwidth Katakana
  // (U+FF65-FF9F), and the kana CJK iteration marks (U+3031-3035). Kanji
  // is intentionally excluded so LXGW remains the single source of truth
  // for shared CJK glyph forms.
  "shippori-mincho": {
    id: "shippori-mincho",
    label: {
      "zh-CN": "日文明朝 (Shippori)",
      en: "Shippori Mincho",
      ja: "しっぽり明朝",
    },
    stack: "'Shippori Mincho', serif",
    source: [
      {
        url: shipporiMinchoRegularUrl,
        weight: "400",
        style: "normal",
        unicodeRange:
          "U+3031-3035, U+3040-309F, U+30A0-30FF, U+31F0-31FF, U+FF65-FF9F",
      },
    ],
  },
  // Mixed-script subtitle face: per-glyph fallback resolves digits via the
  // unicode-range-scoped Cinzel Decorative, Latin letters & punctuation via
  // EB Garamond, kana via Shippori Mincho, CJK kanji via LXGW Neo Zhi Song
  // Plus, with Noto Serif SC as a safety net for anything else. No `source`
  // of its own — composites the five faces registered separately above.
  "subtitle-mixed": {
    id: "subtitle-mixed",
    label: {
      "zh-CN": "特调混合字体",
      en: "LXGW + Garamond + Shippori (kana)",
      ja: "LXGW + Garamond + しっぽり明朝（仮名）",
    },
    stack:
      "'Cinzel Decorative', 'EB Garamond', 'Shippori Mincho', 'LXGW NeoZhiSong Plus', 'Noto Serif SC', 'Times New Roman', serif",
  },
  // "default" —— subtitle-mixed 的别名副本。那套五路混合栈(数字 Cinzel、拉丁 EB
  // Garamond、kana Shippori、汉字 LXGW、兜底 Noto Serif SC)是项目里最常用的正文字体,
  // 复制一份命名为 "default" 方便通用 UI(浮空血条名等)直接引用,语义上就是"默认字体"。
  // 同样无 source —— 复用上面五个已注册的 face。改动时与 subtitle-mixed 的 stack 保持一致。
  default: {
    id: "default",
    label: { "zh-CN": "默认", en: "Default" },
    stack:
      "'Cinzel Decorative', 'EB Garamond', 'Shippori Mincho', 'LXGW NeoZhiSong Plus', 'Noto Serif SC', 'Times New Roman', serif",
  },
  "system-sans": {
    id: "system-sans",
    label: { "zh-CN": "系统无衬线", en: "System Sans" },
    stack: "'Segoe UI', system-ui, -apple-system, sans-serif",
  },
  "system-serif": {
    id: "system-serif",
    label: { "zh-CN": "系统衬线", en: "System Serif" },
    stack: "'Times New Roman', Georgia, serif",
  },
  consolas: {
    id: "consolas",
    label: { "zh-CN": "Consolas 等宽", en: "Consolas" },
    stack: "'Consolas', 'Courier New', monospace",
  },
};

export const FONT_SLOTS: Record<FontSlot, FontSlotConfig> = {
  ui: {
    cssVar: "--font-ui",
    defaultId: "subtitle-mixed",
    allowedIds: [
      "system-sans",
      "noto-serif-sc",
      "eb-garamond",
      "system-serif",
      "subtitle-mixed",
    ],
  },
  display: {
    cssVar: "--font-display",
    defaultId: "noto-serif-sc",
    allowedIds: ["noto-serif-sc", "eb-garamond", "system-serif", "system-sans"],
  },
  mono: {
    cssVar: "--font-mono",
    defaultId: "consolas",
    allowedIds: ["consolas"],
  },
  // Subtitle slot — defaults to the four-way mixed stack (Cinzel Decorative
  // for digits, EB Garamond for Latin, LXGW Neo Zhi Song Plus for CJK).
  // Players who prefer a single face can pick Noto Serif SC alone or fall
  // back to system fonts.
  subtitle: {
    cssVar: "--font-subtitle",
    defaultId: "subtitle-mixed",
    allowedIds: [
      "subtitle-mixed",
      "noto-serif-sc",
      "system-serif",
      "system-sans",
    ],
  },
  handWriteTrademarkUpscale: {
    cssVar: "--font-handWrite-trademark-upscale",
    defaultId: "tangerine",
    allowedIds: ["tangerine"],
  },
};

// ── Runtime helpers ─────────────────────────────────────────────

/** Resolve a localized label for a font id. Falls back: locale → lang prefix → en → id. */
export function getFontLabel(id: string, locale: string): string {
  const cfg = FONTS[id];
  if (!cfg) return id;
  return (
    cfg.label[locale] ??
    cfg.label[locale.split("-")[0]] ??
    cfg.label["en"] ??
    id
  );
}

/**
 * Ensure a self-hosted font file is registered with the document. Idempotent.
 * index.css declares Noto Serif SC up front; this helper covers any font we
 * add later via FONTS[...].source without needing to touch the CSS file.
 */
const injectedFontIds = new Set<string>();
export function ensureFontFaceInjected(id: string): void {
  if (injectedFontIds.has(id)) return;
  const cfg = FONTS[id];
  if (!cfg || !cfg.source) return;

  // Extract the primary family name from the stack — the quoted first token.
  const match = cfg.stack.match(/^\s*['"]?([^'",]+?)['"]?\s*(?:,|$)/);
  const family = match ? match[1] : cfg.id;

  const sources = Array.isArray(cfg.source) ? cfg.source : [cfg.source];
  const css = sources
    .map(
      (src) =>
        `@font-face{font-family:'${family}';` +
        `src:url('${src.url}') format('truetype');` +
        `font-weight:${src.weight ?? "400"};` +
        `font-style:${src.style ?? "normal"};` +
        (src.unicodeRange ? `unicode-range:${src.unicodeRange};` : "") +
        `font-display:block;}`,
    )
    .join("");

  const style = document.createElement("style");
  style.dataset.font = id;
  style.textContent = css;
  document.head.appendChild(style);
  injectedFontIds.add(id);
}

/** Apply a font choice to the CSS variable for a slot. Persists to localStorage. */
export function applyFont(slot: FontSlot, fontId: string): void {
  const cfg = FONTS[fontId];
  const slotCfg = FONT_SLOTS[slot];
  if (!cfg || !slotCfg) return;

  ensureFontFaceInjected(fontId);
  document.documentElement.style.setProperty(slotCfg.cssVar, cfg.stack);
  try {
    localStorage.setItem(`font.${slot}`, fontId);
  } catch {
    /* ignore */
  }
}

/** Read the persisted font id for a slot, falling back to the slot default. */
export function getSavedFontId(slot: FontSlot): string {
  const slotCfg = FONT_SLOTS[slot];
  try {
    const saved = localStorage.getItem(`font.${slot}`);
    if (saved && slotCfg.allowedIds.includes(saved)) return saved;
  } catch {
    /* ignore */
  }
  return slotCfg.defaultId;
}

/** Apply every slot's persisted font on app startup. Call once. */
export function restoreFontsFromStorage(): void {
  // Pre-register every font that has a `source` regardless of which slot is
  // currently using it. This lets a stack reference a face from another
  // entry (e.g. 'noto-cinzel-mixed' depending on 'cinzel-decorative' AND
  // 'noto-serif-sc' being live in the document) without requiring the
  // dependency to be the active selection of any slot.
  Object.keys(FONTS).forEach((id) => ensureFontFaceInjected(id));

  (Object.keys(FONT_SLOTS) as FontSlot[]).forEach((slot) => {
    applyFont(slot, getSavedFontId(slot));
  });
}
