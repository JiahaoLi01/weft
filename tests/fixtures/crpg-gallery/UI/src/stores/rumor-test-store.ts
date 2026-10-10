import { create } from 'zustand';

/**
 * RumorTest store — purely test/dev. Holds the snapshot pushed from C++ each sim tick,
 * plus a rolling log of utterances the player has triggered.
 *
 * Wire format mirrors `ARumorTestGameMode::BuildRumorsJson` / `BuildNpcsJson`:
 *  - `rumorTestInit`      → replaces npcs + rumors + simTick
 *  - `rumorTestState`     → replaces rumors + simTick
 *  - `rumorTestUtterance` → appends to log
 */

export interface RumorRow {
  /** Runtime id（变体形如 "Rumor_01_v1"）；inactive 时等于 defId。 */
  id: string;

  /** Definition 的资产 FName。变体没有自己的 def，留空。 */
  defId: string;

  /** 是否已激活——false 时是 Definition 占位，没有运行时态。 */
  active: boolean;

  isBaseline: boolean;
  isVariant: boolean;
  parentId: string;
  birthTick: number;
  amplitude: number;
  intensity: number;

  // ── Definition 曲线参数（前端算 λ(t) 用）──
  shape: number;
  scale: number;
  decayRate: number;
  variantRate: number;

  /** Tag → 对数线性系数（来自 Definition.Weights）。 */
  weights: Record<string, number>;

  /** otherRumorId → 耦合系数（来自 Definition.IncomingCoupling）。 */
  coupling: Record<string, number>;
}

/** 单个 NPC 在某 tick 的 P(rumor=i) 分布项。 */
export interface DistributionEntry {
  rumorId: string;
  p: number;
}

export interface NpcRow {
  id: string;
  displayName: string;
  tags: string[];
}

export interface UtteranceLog {
  ts: number;        // Date.now()
  simTick: number;
  ok: boolean;
  reason?: string;   // ok=false 时的失败原因（subsystem_unavailable / no_active_rumor / no_expressions_on_any_rumor / ...）
  npcId: string;
  rumorId: string;
  expressionId: string;
  text: string;
}

/** 折线图用：按 tick 采样的强度点。 */
export interface IntensityPoint {
  tick: number;
  intensity: number;
}

/** 历史窗口长度：超过会从队首丢弃，避免 store 与 d3 渲染膨胀。 */
const HISTORY_LIMIT = 200;

interface RumorTestState {
  npcs: NpcRow[];
  rumors: RumorRow[];
  simTick: number;
  log: UtteranceLog[];

  /** 每条 rumor 的 (tick, intensity) 历史——applyState 每次推进 append 一个点。 */
  history: Record<string, IntensityPoint[]>;

  /** 每个 NPC 的当前 P(rumor) 分布（C++ 每 tick 推一份）。 */
  distributions: Record<string, DistributionEntry[]>;

  // 选中态——驱动 Inspector 行展示的子面板。
  selectedRumorId: string | null;
  selectedNpcId: string | null;

  applyInit: (data: {
    npcs: NpcRow[];
    rumors: RumorRow[];
    simTick: number;
    distributions?: Record<string, DistributionEntry[]>;
  }) => void;
  applyState: (data: {
    rumors: RumorRow[];
    simTick: number;
    distributions?: Record<string, DistributionEntry[]>;
  }) => void;
  appendUtterance: (entry: Omit<UtteranceLog, 'ts'>) => void;
  clearLog: () => void;
  clearHistory: () => void;
  setSelectedRumor: (id: string | null) => void;
  setSelectedNpc: (id: string | null) => void;
}

function appendHistory(
  prev: Record<string, IntensityPoint[]>,
  rumors: RumorRow[],
  tick: number,
): Record<string, IntensityPoint[]> {
  const next: Record<string, IntensityPoint[]> = { ...prev };
  for (const r of rumors) {
    if (!r.active) continue;          // 只追踪已激活的——inactive 不画
    const arr = next[r.id] ?? [];
    // 同一 tick 重复推（rumorTestState 多次调用）只更新最后一点，避免锯齿。
    const last = arr[arr.length - 1];
    if (last && last.tick === tick) {
      next[r.id] = [...arr.slice(0, -1), { tick, intensity: r.intensity }];
    } else {
      const appended = [...arr, { tick, intensity: r.intensity }];
      next[r.id] = appended.length > HISTORY_LIMIT ? appended.slice(-HISTORY_LIMIT) : appended;
    }
  }
  return next;
}

export const useRumorTestStore = create<RumorTestState>((set) => ({
  npcs: [],
  rumors: [],
  simTick: 0,
  log: [],
  history: {},
  distributions: {},
  selectedRumorId: null,
  selectedNpcId: null,

  applyInit: (data) =>
    set({
      npcs: data.npcs ?? [],
      rumors: data.rumors ?? [],
      simTick: data.simTick ?? 0,
      distributions: data.distributions ?? {},
      // Init 也录一笔历史——保证第一帧就有点可画。
      history: appendHistory({}, data.rumors ?? [], data.simTick ?? 0),
    }),

  applyState: (data) =>
    set((s) => ({
      rumors: data.rumors ?? [],
      simTick: data.simTick ?? 0,
      distributions: data.distributions ?? s.distributions,
      history: appendHistory(s.history, data.rumors ?? [], data.simTick ?? 0),
    })),

  appendUtterance: (entry) =>
    set((s) => ({
      // 保留最近 50 条——再多 UI 滚动列表会卡。
      log: [...s.log.slice(-49), { ...entry, ts: Date.now() }],
    })),

  clearLog: () => set({ log: [] }),
  clearHistory: () => set({ history: {} }),
  setSelectedRumor: (id) => set({ selectedRumorId: id }),
  setSelectedNpc: (id) => set({ selectedNpcId: id }),
}));
