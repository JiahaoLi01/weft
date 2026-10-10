import { Eye, EyeOff, Anchor, Activity } from 'lucide-react';

/**
 * 异常状态 flag → icon 映射。
 *
 * C++ `UHealthBarViewModel::AbnormalFlags` 推一个逗号分隔的 leaf 名串 —— BEAC
 * `AbnormalTagContainer` 里 `Battle.AbnormalFlag.*` tag 的最后一段(如 "Reveal,Unmovable")。
 * HealthBar 在名字与血条之间按这张表渲染对应 icon;表里没有的 flag 回退到 FALLBACK。
 *
 * key 用 tag 的 leaf 名,大小写跟 C++ tag 定义一致(见 BattleTags.cpp 的 FAbnormalFlagTags)。
 * 加新 flag:C++ 定义好 tag 后,这里补一行 leaf→icon;美术想换图也只动这张表。
 *
 * 用 lucide-react 矢量图标(跟战斗面板的资源 / buff icon 同一套),无需美术资源。
 */

// 所有 lucide 图标共享同一组件签名,用 `typeof Eye` 作类型即可,不依赖 LucideIcon 类型导出。
type IconComponent = typeof Eye;

export const ABNORMAL_FLAG_ICONS: Record<string, IconComponent> = {
  Reveal: Eye, // 被标记 / 强制可见
  Unmovable: Anchor, // 定身 / 不可移动
  Blind: EyeOff, // 目盲 —— 与 Reveal 的 Eye 成对(看见 ⇄ 看不见)
};

/** 未映射的 flag 用的兜底 icon —— 一个中性的「状态」标记。 */
export const ABNORMAL_FLAG_FALLBACK: IconComponent = Activity;

/** 把 C++ 推来的逗号分隔串解析成 leaf 名数组(去空白 / 去空项 / 去重)。 */
export function parseAbnormalFlags(raw: unknown): string[] {
  if (typeof raw !== 'string' || raw.length === 0) return [];
  const seen = new Set<string>();
  const out: string[] = [];
  for (const part of raw.split(',')) {
    const leaf = part.trim();
    if (leaf && !seen.has(leaf)) {
      seen.add(leaf);
      out.push(leaf);
    }
  }
  return out;
}
