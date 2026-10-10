/**
 * Speaker configuration — maps C++ speaker ID to display name (localizable) + color.
 *
 * C++ sends:  CallUI("showSubtitle", { speaker: "npc_aldric", content: "..." })
 * Frontend:   looks up "npc_aldric" here → display name + color
 *
 * To add a new speaker: add an entry below. The key must match the FName used in C++.
 * Colors are CSS color strings (hex, rgb, hsl, named colors all work).
 */

export interface SpeakerConfig {
  /** Display name per locale. Falls back to 'en' → key if missing. */
  name: Record<string, string>;
  /** CSS color for speaker name highlight. */
  color: string;
}

export const SPEAKERS: Record<string, SpeakerConfig> = {
  narrator: {
    name: { 'zh-CN': '旁白', en: 'Narrator' },
    color: '#a0a0a0',
  },
  npc_aldric: {
    name: { 'zh-CN': '奥尔德里克', en: 'Aldric' },
    color: '#d4a843',
  },
  ShiYe: {
    name: { 'zh-CN': '师爷', en: 'Elena' },
    color: '#7ab0ff',
  },
  XianZhang: {
    name: { 'zh-CN': '县长', en: 'Grimm' },
    color: '#86db67',
  },
  player: {
    name: { 'zh-CN': '主角', en: 'Player' },
    color: '#4caf4c',
  },
  Mabel: {
    name: { 'zh-CN': '梅贝儿', en: 'Mabel' },
    color: '#77339e'
  }
};

/**
 * Resolve speaker display name for current locale.
 * Falls back: exact locale → language prefix → 'en' → raw speaker key.
 */
export function getSpeakerName(speakerId: string, locale: string): string {
  const cfg = SPEAKERS[speakerId];
  if (!cfg) return speakerId;
  return cfg.name[locale] ?? cfg.name[locale.split('-')[0]] ?? cfg.name['en'] ?? speakerId;
}

export function getSpeakerColor(speakerId: string): string {
  return SPEAKERS[speakerId]?.color ?? '#f5e6c8';
}
