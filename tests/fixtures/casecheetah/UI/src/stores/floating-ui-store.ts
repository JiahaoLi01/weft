import { create } from 'zustand';
import { onUECall } from '@/lib/ue-bridge';

/**
 * 世界浮标（移植自 CaseCat 的 floating UI）。UBloodMoneyFloatingUiSubsystem 每帧把实例投影成归一化视口坐标：
 *  - __floating_ui_batch：视口、镜头层级、[id, nx, ny, flags]（bit 0 在屏内，bit 1 在镜头后），载荷不变不推；
 *  - __floating_ui_data：类型与数据字典，只在变化时推。
 * 坐标约定与 CaseCat 相同：页面按 innerWidth 还原为视觉像素、再除 html zoom 得到逻辑坐标。
 */
export interface FloatingInstance { id: string; type: string; zOrder: number; data: Record<string, string> }
export interface FloatingPosition { nx: number; ny: number; onScreen: boolean; behind: boolean }

interface FloatingState {
  tier: string;
  paper: number;
  instances: FloatingInstance[];
  positions: Record<string, FloatingPosition>;
}

export const useFloatingUIStore = create<FloatingState>(() => ({ tier: 'near', paper: 0, instances: [], positions: {} }));

onUECall('__floating_ui_data', (data) => {
  useFloatingUIStore.setState({ instances: (data as { instances?: FloatingInstance[] }).instances ?? [] });
});
onUECall('__floating_ui_batch', (data) => {
  const batch = data as { tier?: string; paper?: number; items?: [string, number, number, number][] };
  const positions: Record<string, FloatingPosition> = {};
  for (const [id, nx, ny, flags] of batch.items ?? []) positions[id] = { nx, ny, onScreen: (flags & 1) !== 0, behind: (flags & 2) !== 0 };
  useFloatingUIStore.setState({ tier: batch.tier ?? 'near', paper: batch.paper ?? 0, positions });
});
