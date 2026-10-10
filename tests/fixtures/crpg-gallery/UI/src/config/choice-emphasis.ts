import {
  Skull,
  Dices,
  RotateCcw,
  Heart,
  Crown,
  Coins,
  HeartCrack,
  Star,
  type LucideIcon,
} from 'lucide-react';
import type { LocaleKeys } from '@/i18n';

/**
 * 对话选项强调类别注册表。
 *
 * key 对应 C++ 侧 GameplayTag `Gameplay.CinematicDialogue.Choice.Color.<Key>`
 * 的叶名（DialogueUIDispatcher 序列化时只传叶名）。CDSL 剧本通过
 * `[COLOR: Danger]` 等短名标注；条目含义见 cdsl-commands.md 的 COLOR 一节。
 *
 * defaultHintKey：当选项没有作者手写的 `[HINT]` 时，hover 提示回退到该
 * 类别的默认说明文案。
 */
export interface ChoiceEmphasisDef {
  icon: LucideIcon;
  /** 强调色（不透明 hex —— Ultralight 下半透明背景会出噪点，前景色无碍但保持统一） */
  color: string;
  labelKey: LocaleKeys;
  defaultHintKey: LocaleKeys;
}

export const CHOICE_EMPHASIS: Record<string, ChoiceEmphasisDef> = {
  /** 危险：该选项会产生不可逆的后果 */
  Danger:    { icon: Skull,      color: '#c05252', labelKey: 'dialogue.emphasis.danger',     defaultHintKey: 'dialogue.hint.danger' },
  /** 一次机会：带检定，且只有一次机会 */
  OneChance: { icon: Dices,      color: '#cfa63a', labelKey: 'dialogue.emphasis.one_chance', defaultHintKey: 'dialogue.hint.one_chance' },
  /** 可反复尝试：带检定，可反复尝试 */
  Retry:     { icon: RotateCcw,  color: '#6f9cc4', labelKey: 'dialogue.emphasis.retry',      defaultHintKey: 'dialogue.hint.retry' },
  /** 亲密：推进与角色的亲密关系 */
  Intimacy:  { icon: Heart,      color: '#cd7f9d', labelKey: 'dialogue.emphasis.intimacy',   defaultHintKey: 'dialogue.hint.intimacy' },
  /** 优势：在这个对白中享有优势 */
  Advantage: { icon: Crown,      color: '#8fac60', labelKey: 'dialogue.emphasis.advantage',  defaultHintKey: 'dialogue.hint.advantage' },
  /** 交易：打开交易面板 */
  Trade:     { icon: Coins,      color: '#c08f45', labelKey: 'dialogue.emphasis.trade',      defaultHintKey: 'dialogue.hint.trade' },
  /** 冒犯：可能损害与交谈者的关系 */
  Offense:   { icon: HeartCrack, color: '#c47a50', labelKey: 'dialogue.emphasis.offense',    defaultHintKey: 'dialogue.hint.offense' },
  /** 重要：关联被追踪的任务或主线故事 */
  Important: { icon: Star,       color: '#a184c2', labelKey: 'dialogue.emphasis.important',  defaultHintKey: 'dialogue.hint.important' },
};

export function getChoiceEmphasis(key: string): ChoiceEmphasisDef | undefined {
  return CHOICE_EMPHASIS[key];
}
