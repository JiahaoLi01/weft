import { useEffect, useRef, useState, lazy, Suspense } from 'react';
import * as SpatialNav from '@noriginmedia/norigin-spatial-navigation';
import { useUEBridge } from '@/hooks/use-ue-bridge';
import { useGamepadInput } from '@/hooks/use-gamepad-input';
import { useUIStore } from '@/stores/ui-store';
import { useSettingsStore } from '@/stores/settings-store';
import { useUIInputStore, type UIInputBindings, type UIInputDevice, type ControllerFamily } from '@/stores/ui-input-store';
import { sendToUE, onUECall } from '@/lib/ue-bridge';
import { SubtitleOverlay } from '@/components/SubtitleOverlay';
import { DialogueChoiceOverlay } from '@/components/DialogueChoiceOverlay';
import { WorldWidgetOverlay } from '@/components/WorldWidgetOverlay';
import { FloatingUIOverlay } from '@/components/FloatingUIOverlay';
import { BattleUnitPanel } from '@/components/BattleUnitPanel';
import { BattleUnitStatusPanel } from '@/components/BattleUnitStatusPanel';
import { BattleSessionOverlay } from '@/components/battle/BattleSessionOverlay';
import { TargetingHoverTooltip } from '@/components/battle/TargetingHoverTooltip';
import { MovePreviewTooltip } from '@/components/battle/MovePreviewTooltip';
import { PinnedTooltipProvider, PinnedTooltipLayer } from '@/components/NestedTooltip';
import { BATTLE_KEYWORDS } from '@/config/battle-keywords';
import { useBattleStore, type BattleAbilityRow, type BattleResource, type BattleBuff, type BattleAttribute, type BattleFactionInfo } from '@/stores/battle-store';
import { usePick17Store, type Pick17Draft as Pick17DraftPayload } from '@/stores/pick17-store';
// Battle start waits on this page; keep it in the main bundle so file:// chunk loading cannot delay it.
import { Pick17Draft } from '@/pages/Pick17Draft';
import { restoreFontsFromStorage } from '@/config/fonts';
import '@/stores/world-widget-store';
import '@/stores/floating-ui-store';

// Apply persisted font choices before React paints — runs once at module load.
// Locale restore is handled inside the i18n-store's initial state.
restoreFontsFromStorage();

// Initialize spatial-navigation before any component mounts. The lib reads
// keyboard events (ArrowUp/Down/Left/Right/Enter) at the document level and
// resolves "next focusable in this direction" by element bounding box. Our
// useGamepadInput hook below feeds the same keyboard events from the
// Gamepad API, so the entire navigation runtime stays single-source.
SpatialNav.init({
  // Throttle the focus engine slightly so very fast key repeats don't ping-
  // pong focus through dozens of items per second.
  throttle: 50,
  throttleKeypresses: true,
});

// Dev-only: expose the singleton on window so the running module instance
// is reachable from preview_eval / DevTools. Mirrors the __subtitleStore
// pattern used elsewhere in the codebase.
if (import.meta.env.DEV) {
  (window as unknown as { __spatialNav?: typeof SpatialNav }).__spatialNav = SpatialNav;
}

// ── UE → JS: input bindings + device type ──────────────────────────
//
// UE owns the EnhancedInput Mapping Context — it knows which keys are
// bound to which UI action. UE pushes this map on input-controller
// activation (and would push again on rebind), and pushes the active
// input device whenever the player flips between gamepad and keyboard.
// We just stash both into the store; components that render input-hint
// pills subscribe and re-render automatically.
//
// Subscribed at module load (outside React tree) so a late-arriving
// payload still lands in the store regardless of which page is mounted.
onUECall('uiInputBindings', (data: unknown) => {
  // Trust the C++ shape (controlled wire format) — JSON.parse already
  // happened in the bridge layer. Cast and store.
  useUIInputStore.getState().setBindings(data as UIInputBindings);
});

// ── Battle: SelectedUnit panel ────────────────────────────────
//
// 选中战斗单位的 panel 是跨页存在的 — 玩家可能在打开战斗主菜单 / Settings 时
// 被选中变化击中,handler 不能依赖某个 page 是否在 stack 上。所以这里在模块
// 加载时全局注册一次,handler 直接写 store。BattleUnitPanel 同样是 always-mounted
// overlay(见下面 JSX),store 没数据时 return null 不渲染。
onUECall('setSelectedBattleUnit', (data: unknown) => {
  // 接收 C++ `BuildSelectedBattleUnitJson` 推过来的完整 payload。所有字段都 forward
  // 到 store,store.setSelectedUnit 内部用 ?? [] 给 resources / buffs 兜底默认值。
  const d = data as {
    name: string | null;
    abilities: BattleAbilityRow[];
    resources?: BattleResource[];
    buffs?: BattleBuff[];
    attributes?: BattleAttribute[];
    currentHealth?: number;
  };
  useBattleStore.getState().setSelectedUnit(d);
});

// Pick 17 演示模式:开局技能构筑草案。UPick17BattleSchema 在战斗开局先
// PushUI('pick17') 拉起选择页,再推 payload。handler 必须全局注册,不能等页面挂载。
onUECall('setPick17Draft', (data: unknown) => {
  const draft = data as Pick17DraftPayload;
  console.log('[Pick17] setPick17Draft received', {
    totalPoints: draft?.totalPoints,
    characterCount: draft?.characters?.length ?? 0,
    optionCount: draft?.characters?.reduce((count, character) => count + (character.options?.length ?? 0), 0) ?? 0,
  });
  usePick17Store.getState().setDraft(draft);
});

// 瞄准期(Battle.Targeting state)推送 — 面板在 active 时切到"目标 X/Y"显示。
// 全局注册:玩家可能在打开其它 page 时进入瞄准,面板状态不能依赖某个 page 是否在 stack。
onUECall('setBattleTargeting', (data: unknown) => {
  const d = data as { active?: boolean; picked?: number; required?: number };
  useBattleStore.getState().setTargeting({
    active: !!d.active,
    picked: d.picked ?? 0,
    required: d.required ?? 0,
  });
});

// Battle session 开关 + 阵营静态目录。BattleUIDispatcher OnActivated 推 {active:true,
// factions:[...]},OnDeactivated 推 {active:false}。BattleSessionOverlay 按 active 挂载
// 战斗常驻控件(End Turn / TurnBanner);factions 是 tag → {name, emblemId, color} 目录,
// TurnBanner 等按 tag 解析展示数据(纹章图形由前端 faction-emblems 注册表持有)。
onUECall('setBattleSession', (data: unknown) => {
  const d = data as { active?: boolean; factions?: BattleFactionInfo[] };
  useBattleStore.getState().setBattleSession({ active: !!d.active, factions: d.factions ?? [] });
});

// 当前回合数。C++ 在 SyncCurrentState 开局补推一次 + Battle.Event.RoundAdvanced 桥接维护。
// 持久存 store.currentRound(不再是一次性飘字信号)—— TurnBanner 在轮次推进时连同阵营
// 信息一起展示,旧 RoundAnnouncement 飘字已被它取代。
onUECall('setBattleRound', (data: unknown) => {
  const d = data as { round?: number };
  if (typeof d.round === 'number')
  {
    useBattleStore.getState().setCurrentRound(d.round);
  }
});

// 轮次推进:推哪个阵营在行动 + 在 TurnOrder 中的下标。SyncCurrentState 开局补一次,
// 后续 Battle.Event.TurnAdvanced 桥接维护。factionTag 空 string = "没轮次概念"
// (战斗未开始 / TurnOrder 空) → EndTurnButton 用这个 disabled。
onUECall('setBattleTurn', (data: unknown) => {
  const d = data as { factionTag?: string; turnIndex?: number };
  useBattleStore.getState().setCurrentTurn({
    factionTag: d.factionTag ?? '',
    turnIndex: d.turnIndex ?? 0,
  });
});

// Hover 校验:PC.BattleTargetingState 每帧 raycast cursor + ValidateTarget,节流后 push 这里。
// active=false 时 UI 隐藏 tooltip;active=true + valid=false 显示原因 tooltip;active=true + valid=true
// (后续可加正向"可选"提示,目前 tooltip 只关注 invalid)。
onUECall('setTargetingHover', (data: unknown) => {
  const d = data as { active?: boolean; valid?: boolean; reasonTag?: string; actorName?: string };
  useBattleStore.getState().setTargetingHover({
    active: !!d.active,
    valid: !!d.valid,
    reasonTag: d.reasonTag ?? '',
    actorName: d.actorName ?? '',
  });
});

// Reject pulse:玩家点了无效目标的一次性反馈。Tooltip 收到后闪一下红 + 抖动。trigger 自增在 store
// 内部自管让动画能 replay,同一 reason 多次点击也能重复触发。
onUECall('setTargetingRejectPulse', (data: unknown) => {
  const d = data as { reasonTag?: string };
  useBattleStore.getState().pushTargetingRejectPulse(d.reasonTag ?? '');
});

// 移动距离预览(BattleMoveController 推):cursor 跟随 tooltip 显示米数 + 红绿状态。
// active=false 时 store 接收但 UI 端 MovePreviewTooltip 返 null。
onUECall('setBattleMovePreview', (data: unknown) => {
  const d = data as { active?: boolean; reachable?: boolean; costCm?: number; availableCm?: number };
  useBattleStore.getState().setMovePreview({
    active: !!d.active,
    reachable: !!d.reachable,
    costCm: d.costCm ?? 0,
    availableCm: d.availableCm ?? 0,
  });
});

onUECall('uiInputDevice', (data: unknown) => {
  const d = data as { device?: UIInputDevice; controllerFamily?: ControllerFamily };
  if (d.device === 'gamepad' || d.device === 'keyboardMouse') {
    useUIInputStore.getState().setDevice(d.device);
  }
  // Family is sent in the same payload to keep the wire surface small and
  // because device + family transitions are usually correlated (player
  // unplugs Xbox → plugs PS → triggers BOTH a probe device flip and a
  // hardware-detection family change). Validate before writing to the
  // store to keep the type union honest.
  const fams: ControllerFamily[] = ['xbox', 'playstation', 'switch', 'generic', 'unknown'];
  if (d.controllerFamily && fams.includes(d.controllerFamily)) {
    useUIInputStore.getState().setControllerFamily(d.controllerFamily);
  }
});

// ── UI stack → UE notifier ──────────────────────────────────────────
//
// C++ owns the gamepad UI-navigation IMC and gates it on whatever sits at
// the top of the JS UI stack (see `UWebUIInputController`). We push a
// notification on every stack mutation (push, pop, replace, clear, AND
// in-page goBack() — anything that changes the store) so C++ stays in
// sync without having to reason about which call paths originated where.
//
// Subscribed at module load (outside React's tree) because Zustand's
// `subscribe` lives forever and we want the notification regardless of
// which component is mounted.
let lastSentTop: string | null = null;
useUIStore.subscribe((state) => {
  const top = state.stack.length > 0 ? state.stack[state.stack.length - 1] : null;
  if (top === lastSentTop) {
    // Stack mutated but the top page didn't change (e.g., popped a hidden
    // entry beneath the active page). C++ only cares about top.
    return;
  }
  lastSentTop = top;
  sendToUE('ui_stack', { depth: state.stack.length, top: top ?? '' });
});

// ── Lazy-loaded pages (only loaded when pushed onto stack) ──
const TestMenu        = lazy(() => import('@/pages/TestMenu').then(m => ({ default: m.TestMenu })));
const HUD             = lazy(() => import('@/pages/HUD').then(m => ({ default: m.HUD })));
const Settings        = lazy(() => import('@/pages/Settings').then(m => ({ default: m.Settings })));
const TestGalleryPage = lazy(() => import('@/pages/TestGalleryPage').then(m => ({ default: m.TestGalleryPage })));
const TooltipDemo     = lazy(() => import('@/pages/TooltipDemo').then(m => ({ default: m.TooltipDemo })));
const MVVMTest        = lazy(() => import('@/pages/MVVMTest').then(m => ({ default: m.MVVMTest })));
const RumorTestPage   = lazy(() => import('@/pages/RumorTestPage').then(m => ({ default: m.RumorTestPage })));
const CheatPanel      = lazy(() => import('@/pages/CheatPanel').then(m => ({ default: m.CheatPanel })));
const Market          = lazy(() => import('@/pages/Market').then(m => ({ default: m.Market })));
const SmokeBuffLab    = lazy(() => import('@/pages/SmokeBuffLab').then(m => ({ default: m.SmokeBuffLab })));
/** Map page ID → React component. Add new pages here. */
const PAGE_MAP: Record<string, React.ComponentType> = {
  'test-menu':    TestMenu,
  'hud':          HUD,
  'settings':     Settings,
  'test-gallery': TestGalleryPage,
  'tooltip-demo': TooltipDemo,
  'mvvm-test':    MVVMTest,
  'rumor-test':   RumorTestPage,
  'cheat-panel':  CheatPanel,
  'market':       Market,
  'smoke-buff-lab': SmokeBuffLab,  // dev:special buff 烟雾调参实验室
  'pick17':         Pick17Draft,   // Pick 17 演示模式:开局技能构筑选择(UPick17BattleSchema 推送)
};

/**
 * Main UI — blank canvas.
 *
 * Renders nothing by default. C++ drives page display via:
 *   CallUI("pushUI",    { page: "test-menu" })
 *   CallUI("popUI",     {})
 *   CallUI("replaceUI", { page: "hud" })
 *   CallUI("clearUI",   {})
 *
 * SubtitleOverlay is always mounted (not in the stack).
 */
function App() {
  useUEBridge();
  useGamepadInput();   // poll Gamepad API → synthesize key events

  const stack = useUIStore(s => s.stack);
  const uiScale = useSettingsStore(s => s.uiScale);

  useEffect(() => {
    document.documentElement.style.zoom = String(uiScale);
  }, [uiScale]);

  // ── Force-clear pulse on page-stack drain ────────────────────────────
  //
  // Ultralight 的 GPU 渲染纹理是持久化的——当 stack 从非空变空时，React 卸载页
  // 后新一帧没有覆盖 viewport 的 draw command，上一帧的像素就赖在纹理上（典型
  // 表现：关掉 debug overlay 后 UI 仍可见但不可交互）。
  //
  // 这里检测"上一次非空、本次空"的转换瞬间，bump key 让一个 100ms 的全屏 fade
  // 元素重新挂载——动画期间持续 draw 全屏 → Ultralight 重画整张纹理 → 旧帧像素
  // 被覆盖。100ms 后 opacity:0，与 transparent body 视觉等价。
  const [forceClearKey, setForceClearKey] = useState(0);
  const prevStackLen = useRef(stack.length);
  useEffect(() => {
    if (prevStackLen.current > 0 && stack.length === 0) {
      setForceClearKey(k => k + 1);
    }
    prevStackLen.current = stack.length;
  }, [stack.length]);

  return (
    // PinnedTooltipProvider 包在最外层 —— pin 行为是"玩家想留住这条术语随时看",
    // 跨页保留更符合直觉(切到 Settings / Inventory pin 的窗口不消失),Provider+Layer
    // 一处包定全局最简。BATTLE_KEYWORDS 模块常量,Provider 不会因 prop 变化 re-render。
    // 后续要把 keyword 从 C++ DataAsset 推过来时,这里改成订阅 store 拿 Map 即可。
    <PinnedTooltipProvider keywords={BATTLE_KEYWORDS}>
      <div className="relative w-full h-full">
        {/* Force-clear pulse: 当 page 栈刚刚从非空变空，挂一帧瞬时遮罩跑 fade-out
            动画——强制 Ultralight 重画 viewport，逐出上一帧残留像素。详见上面的 useEffect。 */}
        {forceClearKey > 0 && stack.length === 0 && (
          <div
            key={`force-clear-${forceClearKey}`}
            className="absolute inset-0 pointer-events-none animate-webui-clear-pulse"
            style={{ zIndex: 1 }}
            aria-hidden
          />
        )}

        {/* Stacked pages — all rendered simultaneously, z-index by stack order */}
        {stack.map((pageId, index) => {
          const PageComponent = PAGE_MAP[pageId];
          if (!PageComponent) return null;
          return (
            <div
              key={pageId}
              className="absolute inset-0"
              style={{ zIndex: 10 + index * 10 }}
            >
              <Suspense fallback={null}>
                <PageComponent />
              </Suspense>
            </div>
          );
        })}

        {/* Always-mounted overlays, independent of page stack */}
        <WorldWidgetOverlay />
        {/* FloatingUIOverlay — 新的"屏幕投影到世界点"叠层。跟 WorldWidget 并行,
            后者老 HP 条仍在用,新东西(伤害飘字、quest marker、头顶 HP 条)走这个新系统。
            本层 zIndex=0(见组件内注释):刻意低于下面始终挂载的战斗 HUD 面板
            (BattleUnitPanel / StatusPanel 等),这样头顶血条不会盖住技能/状态面板。 */}
        <FloatingUIOverlay />
        <SubtitleOverlay />
        {/* 对话分支选项面板 —— DialogueUIDispatcher 推 showDialogueChoices/
            hideDialogueChoices，选择经 dialogue.choiceSelected 回传 C++。
            巫师 3 式：屏幕中心偏右一整块无边框黑色半透明矩形，文本左对齐，
            选中文字放大；选项超过 7 行进入 iOS 时间轮式滚动（焦点自动居中 +
            远端衰减 + 滚轮）。支持手柄/键盘导航（↑↓/Enter/数字键），
            常驻挂载（DOM 池，见组件头注释）。 */}
        <DialogueChoiceOverlay />
        <BattleUnitPanel />
        {/* 左下角"状态面":角色名 + buff 列表。跟右下角的 BattleUnitPanel 配对,共享同一个
            selectedUnit store,会同步进出场。两个 panel 故意拆开 —— 右下是操作面(技能 /
            资源 / 瞄准),左下是状态面(身份 / buff),视觉上分列两端便于读取。 */}
        <BattleUnitStatusPanel />
        {/* 战斗会话常驻控件组(End Turn 按钮 / TurnBanner 回合横幅)—— C++
            BattleUIDispatcher 就绪(OnActivated)推 setBattleSession 后才挂载,
            战斗外不存在于 DOM。新战斗常驻控件挂进 overlay 内部,别散装挂回这里。 */}
        <BattleSessionOverlay />
        {/* 瞄准期"不可选中原因"tooltip —— 跟随鼠标,边缘自动反向 anchor 避免溢出。
            store.targetingHover.active=false 时自动隐藏(返 null);active=true + valid=false 时
            显示原因文案。reject pulse 触发时叠加一次红色 shake 反馈。 */}
        <TargetingHoverTooltip />
        {/* 移动 hover 距离预览 —— BattleMoveController 推,跟随鼠标显示"X.XX m"+ 红/绿。
            active=false 时 return null。3D 路径线仍然由 C++ 端 DrawDebugLine 画。 */}
        <MovePreviewTooltip />
        {/* Pinned tooltip windows — portal-rendered, sits above everything else.
            Empty (returns null) when no tooltips are pinned, so zero cost when unused. */}
        <PinnedTooltipLayer />
      </div>
    </PinnedTooltipProvider>
  );
}

export default App;
