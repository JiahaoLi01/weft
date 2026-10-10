import { create } from 'zustand';

// ===== Types =====

/** 一条对话选项 —— 与 UDialogueUIDispatcher::PushBranchOptionsToUI 的 wire 格式对应。 */
export interface DialogueChoiceOption {
  id: string;
  text: string;
  /** 作者手写的 hover 提示；空串 = 无（UI 回退到强调类别的默认提示）。 */
  hint: string;
  /** 强调类别叶名（Danger/OneChance/...）；空串 = 无强调。 */
  emphasis: string;
  /** false = 可选条件不满足：置灰展示，点击不发送。 */
  selectable: boolean;
}

/**
 * 面板可见状态。参照 subtitle-store 的经验：hidden ↔ visible 之间必须经过
 * 一段短 CSS 过渡（exiting），否则 Ultralight 合成纹理上会留下最后一帧。
 * 面板根节点常驻挂载，hidden 时 display:none。
 */
export type DialogueChoicePanelState = 'hidden' | 'visible' | 'exiting';

interface DialogueChoiceState {
  options: DialogueChoiceOption[];
  panel: DialogueChoicePanelState;

  show: (options: DialogueChoiceOption[]) => void;
  hide: () => void;
  /** Internal: exiting → hidden（CSS 过渡结束后清空）。 */
  finishExit: () => void;
}

// 与 DialogueChoiceOverlay 的 transition 时长保持同步。
const EXIT_TRANSITION_MS = 160;
const EXIT_SAFETY_MS = 25;

// 防止 hide→show 快速交替时旧的 finishExit 定时器误杀新面板。
let exitTimer: ReturnType<typeof setTimeout> | null = null;

export const useDialogueChoiceStore = create<DialogueChoiceState>((set, get) => ({
  options: [],
  panel: 'hidden',

  show: (options) => {
    if (exitTimer !== null) {
      clearTimeout(exitTimer);
      exitTimer = null;
    }
    set({ options, panel: 'visible' });
  },

  hide: () => {
    if (get().panel !== 'visible') {
      return;
    }
    set({ panel: 'exiting' });
    exitTimer = setTimeout(() => {
      exitTimer = null;
      get().finishExit();
    }, EXIT_TRANSITION_MS + EXIT_SAFETY_MS);
  },

  finishExit: () => {
    set((s) => (s.panel === 'exiting' ? { panel: 'hidden', options: [] } : s));
  },
}));
