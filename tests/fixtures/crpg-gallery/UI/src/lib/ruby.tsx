/**
 * Inline ruby (furigana) parser for hand-written subtitle scripts.
 *
 * ── Author syntax ──────────────────────────────────────────────────
 *   [漢字]{かんじ}         — whole-string ruby; <rt> covers the full base
 *   [漢字]{かん|じ}        — per-character alignment via '|' splits
 *   [漢字]!{かん|じ}       — same as above but flagged as a "key word"
 *
 * The optional `!` between `]` and `{` marks the annotation as important.
 * The runtime CSS uses this flag to filter visibility:
 *
 *   off   — every <rt> is hidden (kanji-only display)
 *   key   — only key-word rt visible
 *   all   — every rt visible
 *
 * ── Per-character alignment ────────────────────────────────────────
 * When the reading contains '|' AND the segment count matches the base's
 * codepoint count, we emit interleaved <rt> per base char so the browser
 * pairs each kana-group above the corresponding kanji exactly:
 *
 *   <ruby>漢<rt>かん</rt>字<rt>じ</rt></ruby>
 *
 * If counts mismatch (writer typo, mixed widths) we degrade to a single
 * <rt> over the whole base. Better to lose precise alignment than to
 * silently drop characters.
 *
 * ── Why a parser, not dangerouslySetInnerHTML ─────────────────────
 * Subtitle content nominally comes from C++ but ultimately reflects
 * author-written script files; a regex-driven structured parse keeps the
 * rendering path safe even if a future content pipeline ever ingests
 * untrusted strings.
 */

import { Fragment, type ReactNode } from 'react';

export type RubyToken =
  | { kind: 'text'; text: string }
  | {
      kind: 'ruby';
      base: string;
      /** Reading split on '|' — length 1 = whole-string; length N matching
       *  base codepoint count = per-char alignment. */
      readings: string[];
      important: boolean;
    };

const RUBY_RE = /\[([^\]\n]+)\](!?)\{([^\}\n]+)\}/g;

/** Split a string into alternating text and ruby tokens. */
export function parseRuby(input: string): RubyToken[] {
  const tokens: RubyToken[] = [];
  let last = 0;
  RUBY_RE.lastIndex = 0;
  let m: RegExpExecArray | null;
  while ((m = RUBY_RE.exec(input)) !== null) {
    if (m.index > last) {
      tokens.push({ kind: 'text', text: input.slice(last, m.index) });
    }
    const [, base, bang, reading] = m;
    tokens.push({
      kind: 'ruby',
      base,
      readings: reading.split('|'),
      important: bang === '!',
    });
    last = m.index + m[0].length;
  }
  if (last < input.length) {
    tokens.push({ kind: 'text', text: input.slice(last) });
  }
  return tokens;
}

/**
 * Render a content string into React nodes, expanding inline ruby syntax
 * into <ruby> elements. Per-character alignment is preferred when '|' splits
 * the reading into the same number of segments as the base has codepoints;
 * otherwise we fall back to a single <rt> over the whole base.
 *
 * Used by both the cinematic subtitle overlay (where the surrounding CSS
 * filters <rt> visibility based on `data-furigana-mode`) and the Settings
 * panel (where it provides a live demo of each density mode in the option
 * descriptions).
 */
export function renderRuby(content: string): ReactNode[] {
  return parseRuby(content).map((tok, i) => {
    if (tok.kind === 'text') return tok.text;
    const className = tok.important ? 'ruby-key' : 'ruby-normal';
    const baseChars = Array.from(tok.base);
    const perChar =
      tok.readings.length > 1 && tok.readings.length === baseChars.length;
    if (perChar) {
      return (
        <ruby key={i} className={className}>
          {baseChars.map((ch, j) => (
            <Fragment key={j}>
              {ch}
              <rt>{tok.readings[j]}</rt>
            </Fragment>
          ))}
        </ruby>
      );
    }
    return (
      <ruby key={i} className={className}>
        {tok.base}
        <rt>{tok.readings.join('')}</rt>
      </ruby>
    );
  });
}
