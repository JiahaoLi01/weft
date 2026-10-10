import type { ComponentType } from 'react';
import type { FloatingUIInstance } from '@/stores/floating-ui-store';
import { DamageNumber } from '@/components/floating-ui/DamageNumber';
import { HealthBar } from '@/components/floating-ui/HealthBar';
import { AbnormalFlagFloating } from '@/components/floating-ui/AbnormalFlagFloating';
import { QuestMarker, QuestMarkerEdgeIndicator } from '@/components/floating-ui/QuestMarker';

/**
 * Floating UI 类型 → React 组件 注册表 + off-screen policy。
 *
 * C++ 那边 `UWebUIFloatingManager::RegisterWidgetType(FName, VMClass)` 注册某个 widget
 * type 的 VM,这边对应同一个 type 注册它该用什么 React component 渲染 + 离屏时怎么处理。
 * Type 串两边必须严格一致(kebab-case 约定,FName ↔ JS string 来回不变形)。
 *
 * 加新类型的流程:
 *   1. C++:Spawn 前确保该 type 已 RegisterWidgetType。VM 可以是 nullptr(纯静态数据)
 *   2. JS:实现 React component(在 `components/floating-ui/` 下),加进这张表
 *      并指定 off-screen policy
 *   3. JS 端 component prop 拿到 FloatingUIInstance,从 `instance.data` 读 VM 字段
 *
 * 跟老的 WORLD_WIDGET_TYPES 是平行系统,数据不互通 —— 同一个项目里两边可以共存。
 */

// ─── Off-screen policy ────────────────────────────────────────────
//
// instance 离开 viewport 时,各 widget type 想要的反应不同。这里用 union string 枚举
// 表达,Overlay 按 policy 分发处理。新增 policy 时:
//   1. 加 string literal 到 OffscreenPolicy
//   2. 在 FloatingUIOverlay 的 switch 里加 case
//   3. 视情况在 FloatingUITypeDef 加该 policy 专属的回调字段

export type OffscreenPolicy =
  | 'hide'           // (默认)出屏直接不渲染。HP 条 / damage number 都用这个。
  | 'edge-clamp'     // 出屏时把组件 clamp 到 viewport 边缘,显示一个指向 off-screen 位置的小箭头。
                     // 典型场景:任务标记(quest marker)、远处队友 marker、目标点 ping。
                     // 配套配 `edgeIndicator` 字段告诉 Overlay 出屏时渲染什么(通常一个箭头组件)。
  | 'handoff'        // 出屏时把内容转交给另一个 UI 系统(例:floating subtitle → cinematic subtitle 区)。
                     // 期间不渲染 floating 实体,等回到屏内再恢复。需配合 `onOffscreen` /
                     // `onScreenAgain` 回调,接到对端系统的 store。
  | 'always-render'; // 即使出屏也按原始投影坐标渲染。仅在玩家"看到"出屏 UI 不违和的场景(例:
                     // 用 mod-replay 工具复盘时);demo 阶段几乎用不到,留作可能扩展。

export type FloatingUIComponent = ComponentType<{ instance: FloatingUIInstance }>;

/**
 * 一个 floating widget type 的完整定义。
 *
 *   component       on-screen 时(以及 always-render policy 的所有时刻)渲染用的组件
 *   offscreenPolicy 出屏行为,缺省 'hide'
 *   edgeIndicator   policy='edge-clamp' 时,出屏期渲染的"小箭头 / 缩略 marker"组件。
 *                   Overlay 会把它包在带旋转 / 位移的 wrapper 里(见
 *                   `components/floating-ui/OffscreenIndicator`),组件本身只画箭头本体。
 *                   policy != 'edge-clamp' 时该字段被忽略。
 *   onOffscreen     policy='handoff' 时,instance 从屏内进入屏外那一刻 fire 的回调。典型:
 *                   往 cinematic-subtitle store push 一条字幕条。每条 instance 在 on→off 转换
 *                   时调一次(不是每帧),消费者自己决定要不要持续展示。
 *   onScreenAgain   policy='handoff' 时,instance 从屏外回到屏内那一刻 fire 的回调。典型:
 *                   从 cinematic-subtitle store 移除对应字幕。
 *
 * onOffscreen / onScreenAgain 在其它 policy 下也允许配置(纯监听用),Overlay 也会 fire
 * transition —— 但 handoff 是它们最常见的使用场景。
 */
export interface FloatingUITypeDef {
  component: FloatingUIComponent;
  offscreenPolicy?: OffscreenPolicy;
  edgeIndicator?: FloatingUIComponent;
  onOffscreen?: (instance: FloatingUIInstance) => void;
  onScreenAgain?: (instance: FloatingUIInstance) => void;
}

export const FLOATING_UI_TYPES: Record<string, FloatingUITypeDef> = {
  // ─── 战斗伤害 / 治疗飘字 ────────────────────────────────────
  // 对应 C++ `UDamageNumberViewModel`,由 BEAC TryApplyModifier 自动 spawn
  // (见 `BattleEntityActorComponent::SpawnDamageNumberFloating`)。
  //
  // policy='hide':TTL ~1.75s,出屏期间不显示无伤大雅 —— 玩家此时根本看不到这块世界,
  // 飘字到底跳没跳就算了。等回到屏内一般飘字也已经 expire 了。
  'damage-number': {
    component: DamageNumber,
    offscreenPolicy: 'hide',
  },

  // ─── 浮空 HP 条(头顶跟随)──────────────────────────────────
  // 对应 C++ `UHealthBarViewModel`,由 BEAC.BeginPlay 自动 SpawnFromAnchor 持久持有
  // (TTL=-1),VM 自动绑 OnHealthChanged + MaxHealth attribute 变化。
  //
  // policy='hide':出屏不渲染。后续如想做"屏外队友 HP 提示"切到 edge-clamp + edgeIndicator
  // 自己加一个箭头组件即可。
  'health-bar': {
    component: HealthBar,
    offscreenPolicy: 'hide',
  },

  // ─── 异常状态「增 / 删」瞬时飘字 ──────────────────────────────
  // 对应 C++ `UAbnormalFlagFloatingViewModel`,由 BEAC.SpawnAbnormalFlagFloating 在 Buff
  // Register/Unregister 检测到 abnormal flag 跨越 0↔1 stack 边界时 spawn(SpawnAtLocation,
  // 短 TTL)。新增播上浮淡入,移除播删除线动画。
  //
  // policy='hide':跟 damage-number 一致 —— 出屏不渲染。飘字 ~1.6s 生命周期,出屏期间玩家
  // 看不到这块世界,漏掉无妨。
  'abnormal-flag': {
    component: AbnormalFlagFloating,
    offscreenPolicy: 'hide',
  },

  // ─── 任务 / 目标 marker(stub)─────────────────────────────
  // policy='edge-clamp':出屏时贴边 + 旋转箭头指向 off-screen 的实际位置。
  // C++ 端 `RegisterWidgetType("quest-marker", ...)` 待补,组件本体是 placeholder,
  // 等接入时改 ViewModel field schema 配齐即可。
  'quest-marker': {
    component: QuestMarker,
    offscreenPolicy: 'edge-clamp',
    edgeIndicator: QuestMarkerEdgeIndicator,
  },

  // ─── Floating subtitle(stub)──────────────────────────────
  // policy='handoff':出屏时把字幕转给 cinematic subtitle 区(屏底固定位置)。
  // 该 policy 完整工作需要 cinematic subtitle store(尚未实现) —— onOffscreen /
  // onScreenAgain 字段当前留空。等 cinematic subtitle 系统就绪时,把对应的 store
  // dispatch 接进来即可,C++ / Overlay / instance schema 都不用动。
  //
  // 'floating-subtitle': {
  //   component: FloatingSubtitle,
  //   offscreenPolicy: 'handoff',
  //   onOffscreen: (inst) => useCinematicSubtitleStore.getState().pushHandoff(inst.id, inst.data),
  //   onScreenAgain: (inst) => useCinematicSubtitleStore.getState().clearHandoff(inst.id),
  // },
};

/**
 * 拿一条 instance 的 type 定义(含 policy)。type 没注册返 undefined,Overlay 跳过该 instance
 * (跟旧行为一致:未注册 type → 不渲染)。
 */
export function getFloatingUITypeDef(type: string): FloatingUITypeDef | undefined {
  return FLOATING_UI_TYPES[type];
}
