import { create } from 'zustand';
import { onUECall } from '@/lib/ue-bridge';

/**
 * Floating UI Store —— 跟 C++ `UWebUIFloatingManager` 配对的前端状态。
 *
 * C++ side: 每 tick 算一个完整批次,调
 *   CallUI("__floating_ui_batch", { instances: [...], viewport: { width, height } })
 *
 * 每条 instance 由 manager 持有的 FGuid 作 stable key,可在多帧之间 reconcile。
 *
 * 跟老的 `world-widget-store` 是**并行系统**,互不干涉:
 *   - 老 WorldWidget 仍由 `UWorldWidgetController` 推 `__world_widget_batch`,
 *     渲染在 `WorldWidgetOverlay`
 *   - 新 FloatingUI 推这条 `__floating_ui_batch`,渲染在 `FloatingUIOverlay`
 *   - 两个 overlay 都挂在 App.tsx 里,zIndex 一样(都是"游戏世界叠层");FLOATING 在
 *     OBJECT WIDGET 之上是因为先 mount 的在后(z-index 同 → DOM 顺序决定)
 *
 * 等 floating 这套验证稳定后,老 WorldWidget 可以自然退役 —— 但 Phase 1 不动它。
 */

export interface FloatingUIInstance {
  /** FGuid 字符串("Digits with hyphens" 格式,小写带连字符) */
  id: string;
  /** widget type —— 跟 FLOATING_UI_TYPES 表的 key 对应 */
  type: string;
  /**
   * 屏幕坐标 (Ultralight View CSS 空间);overlay 渲染时再除以 uiScale。
   *
   * 重要:这是 **raw 投影坐标**,C++ 端不做 clamp —— 当 instance 出 viewport / 在镜头后方时,
   * x / y 可能落在 [0, viewport.width/height] 范围外。各 off-screen policy 自行决定怎么用
   * 这个坐标(hide 直接忽略 / edge-clamp 用它算"贴边方向" / 等等)。
   */
  x: number;
  y: number;
  /** 距离缩放后的最终 transform scale */
  scale: number;
  /**
   * = onScreen && inRange,兼容字段。新 policy 走下面三 raw flag,这条是给 'hide' policy
   * 直接读的简便短路。
   */
  visible: boolean;
  /** 是否落在 viewport 矩形内 + 不在镜头后方。 */
  onScreen: boolean;
  /**
   * 是否在镜头后方。`behindCamera=true` 时 ProjectWorldLocationToScreen 给出的坐标是
   * "翻转"的(像 viewport 中心点对面),edge-clamp policy 需要反向处理才能得到正确的
   * "该往哪个边缘指"方向。
   */
  behindCamera: boolean;
  /** 距离 <= instance 的 MaxVisibleDistance。 */
  inRange: boolean;
  /** 同 zIndex 层内的相对前后 */
  zOrder: number;
  /** instance 到相机距离(world units) */
  distance: number;
  /**
   * TTL 信息(可选,仅 instance 有 TTL 时才出现)。前端可以用 elapsed/ttl 算 normalize 进度
   * 做 fade out、进度条之类的纯前端动画。
   */
  ttl?: number;
  elapsed?: number;
  /**
   * VM FieldNotify 字段反射序列化的扁平 record。typed 值(number / boolean / string)。
   * 消费组件按各自的 type 已知 schema 解读,这里不做强类型(各 component 自己 coerce)。
   */
  data: Record<string, unknown>;
}

/** Viewport 信息 —— edge-clamp policy 用这个算屏幕矩形 / 中心 / 边缘交点。 */
export interface FloatingUIViewport {
  width: number;
  height: number;
}

interface FloatingUIState {
  instances: FloatingUIInstance[];
  viewport: FloatingUIViewport;
  setBatch: (payload: { instances: FloatingUIInstance[]; viewport?: FloatingUIViewport }) => void;
}

export const useFloatingUIStore = create<FloatingUIState>((set) => ({
  instances: [],
  // 默认占位 viewport,首批 batch 前用不到 —— 没 instance 时 edge-clamp 也不会查它。
  // 0 而不是 window.innerWidth:Ultralight 总尺寸跟游戏 viewport 可能不一致(letterbox /
  // 缩放),用 C++ 端推过来的"权威"viewport 更稳。
  viewport: { width: 0, height: 0 },

  setBatch: ({ instances, viewport }) => set((state) => ({
    instances,
    // viewport 缺省时保留旧值 —— C++ 端理论上每帧都发,但 wire 失败兜底不至于把已知 viewport 清零
    viewport: viewport ?? state.viewport,
  })),
}));

// ===== UE → JS handler(模块加载时一次性注册,跟 ui-store 等其它 store 同 pattern)=====

onUECall('__floating_ui_batch', (data: unknown) => {
  const payload = data as { instances?: FloatingUIInstance[]; viewport?: FloatingUIViewport };
  useFloatingUIStore.getState().setBatch({
    instances: payload?.instances ?? [],
    viewport: payload?.viewport,
  });
});
