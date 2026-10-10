import { useEffect, useRef } from 'react';
import { AnimatePresence, motion } from 'framer-motion';
import { useFloatingUIStore, type FloatingUIInstance } from '@/stores/floating-ui-store';
import { useSettingsStore } from '@/stores/settings-store';
import { getFloatingUITypeDef, type FloatingUITypeDef, type OffscreenPolicy } from '@/config/floating-ui-types';
import { clampToViewportEdge } from '@/components/floating-ui/offscreen-clamp';

/**
 * 渲染所有当前 floating UI instance —— C++ `UWebUIFloatingManager` 每 tick 推一个完整
 * 批次,这里按 instance.id 做 keyed render,AnimatePresence 自动处理"id 消失时的 exit
 * 动画"(C++ 删除 instance → 下一帧 batch 里没这条 → AnimatePresence 还会保留 DOM 跑
 * 完 exit transition 才真正卸载)。
 *
 * 挂在 App.tsx 里,跟 WorldWidgetOverlay 同层级(zIndex: 5),属于"游戏世界叠层"。
 *
 * 坐标转换链(必须跟 `UWebUIFloatingManager::FlushBatch` 同步):
 *   UE 逻辑 viewport 像素 × DPIScale_UE (C++ 已应用) = CSS 像素
 *   CSS 像素 × html.zoom (= uiScale) = 视觉屏幕像素
 * 我们要让视觉像素 == 原始逻辑像素,所以这里再除一次 uiScale 抵消 html.zoom。
 *
 * ── Off-screen policy dispatch ────────────────────────────────────────────
 *
 * 每个 widget type 在 `floating-ui-types.ts` 配 `offscreenPolicy`,Overlay 按 policy
 * 分发处理:
 *
 *   hide          (默认)出屏 / 远距离 → 不渲染。damage number / HP bar 都用这条。
 *   edge-clamp    on-screen 在 raw pos 渲染 component;off-screen 切到 edgeIndicator,
 *                 用 `clampToViewportEdge` 算贴边坐标 + 箭头旋转 deg。
 *   handoff       on-screen 时正常渲染;on→off 转换时 fire `onOffscreen` 回调
 *                 (通常把内容转给另一个 UI 系统,如 cinematic subtitle),off 期间不渲染。
 *                 off→on 转换时 fire `onScreenAgain` 收回。
 *   always-render 永远在 raw pos 渲染,不管 onScreen / inRange。罕用,留给特殊调试 / 工具场景。
 *
 * 加新 policy 时同步更新这里的 switch + `floating-ui-types.ts` 的 OffscreenPolicy union。
 */
export function FloatingUIOverlay() {
  const instances = useFloatingUIStore((s) => s.instances);
  const viewport = useFloatingUIStore((s) => s.viewport);
  const uiScale = useSettingsStore((s) => s.uiScale);
  const inv = uiScale > 0 ? 1 / uiScale : 1;

  // 上一帧每条 instance 的 onScreen + inRange 综合状态 —— 用来在每帧 batch 之间检测
  // on↔off 转换,fire `onOffscreen` / `onScreenAgain` 回调。useRef 而非 useState:
  // 这是"frame-bridge"数据,不参与渲染输出,不需要触发 re-render。
  //
  // 不存"完全消失"的 instance —— C++ 端 despawn 后 batch 不再包含它,我们直接从 map 抹掉。
  // 那条不算"出屏",是真没了,不 fire onOffscreen。
  const prevOnScreenRef = useRef<Map<string, boolean>>(new Map());

  // Batch 变化时检测 transition + fire 回调。effect 而不是渲染期 inline —— 回调可能
  // dispatch 到别的 store,渲染期 dispatch 违反 React 规则;且 effect 保证只在 commit
  // 之后跑(避免 batch 在中间帧 race 时双发)。
  useEffect(() => {
    const newMap = new Map<string, boolean>();
    for (const inst of instances) {
      const def = getFloatingUITypeDef(inst.type);
      if (!def) continue;
      const isOnScreen = inst.onScreen && inst.inRange;
      newMap.set(inst.id, isOnScreen);

      const wasOnScreen = prevOnScreenRef.current.get(inst.id);
      // 第一帧出现:wasOnScreen === undefined,不算 transition(避免新 spawn 就立刻
      // fire onScreenAgain 的语义混乱)。后续帧才比对。
      if (wasOnScreen === undefined) continue;
      if (wasOnScreen && !isOnScreen) def.onOffscreen?.(inst);
      if (!wasOnScreen && isOnScreen) def.onScreenAgain?.(inst);
    }
    prevOnScreenRef.current = newMap;
  }, [instances]);

  return (
    <div
      className="absolute inset-0 overflow-hidden pointer-events-none"
      // zIndex:0 —— 世界叠层,刻意低于始终挂载的战斗 HUD 面板(BattleUnitPanel /
      // BattleUnitStatusPanel 等是 position:absolute 无显式 z = auto,与本层同级靠 DOM
      // 顺序决定,本 overlay 在它们之前挂载 → 被压在下面)。0 仍创建 stacking context,
      // 把内部各 instance 的 zOrder 关在本层内,不会与 HUD 面板交错。头顶血条因此不再
      // 盖住技能/状态面板。
      style={{ zIndex: 0 }}
    >
      <AnimatePresence>
        {instances.map((inst) => {
          const def = getFloatingUITypeDef(inst.type);
          if (!def) return null;
          const policy: OffscreenPolicy = def.offscreenPolicy ?? 'hide';
          return renderInstance(inst, def, policy, viewport, inv);
        })}
      </AnimatePresence>
    </div>
  );
}

/**
 * 单条 instance 的 policy 分发渲染。抽出来是为了让 Overlay body 干净,所有 policy 分支
 * 的 transform / 显隐 / 组件选择逻辑集中在这里。
 *
 * 注意:**所有 policy 都同样 mount 一个 motion.div 包装**,只是里面的 transform / 渲染
 * 内容不同。这样 framer-motion 的 AnimatePresence 在 instance.id 消失时一致地跑 exit
 * 动画(damage number 飘走、quest marker 淡出),而不是 unmount 时硬切。
 *
 * 唯一例外:'handoff' 在 off-screen 时返 null —— 该期间 instance 转给别的系统接管,
 * floating overlay 不渲染。再回到屏内时这条 id 又出现,AnimatePresence 会跑 enter 动画。
 */
function renderInstance(
  inst: FloatingUIInstance,
  def: FloatingUITypeDef,
  policy: OffscreenPolicy,
  viewport: { width: number; height: number },
  inv: number,
) {
  const Component = def.component;
  const visibleHere = inst.onScreen && inst.inRange;  // 跟 inst.visible 等价,内部用变量更清晰

  switch (policy) {
    case 'hide': {
      // 最常见 policy:visible 时按 raw pos 渲染,否则什么都不画。
      // display:none 而不是直接 return null —— 让 AnimatePresence 继续跟踪 id 生命周期,
      // 后续 visible 切回 true 时不会被当作"新 spawn"(避免重复入场动画)。
      return (
        <motion.div
          key={inst.id}
          className="absolute top-0 left-0"
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: 0.15, ease: 'easeOut' }}
          style={{
            transform: `translate3d(${inst.x * inv}px, ${inst.y * inv}px, 0) translate(-50%, -50%) scale(${inst.scale})`,
            display: visibleHere ? 'block' : 'none',
            zIndex: inst.zOrder,
          }}
        >
          <Component instance={inst} />
        </motion.div>
      );
    }

    case 'edge-clamp': {
      // 跟 'hide' 不一样:即使 onScreen=false 也渲染(切到 edgeIndicator)。
      // 唯一隐藏条件:`inRange=false` —— 距离超 MaxVisibleDistance 时连箭头都不显示。
      // 这是合理的:玩家根本不知道 5km 外有个 quest marker,屏幕边缘画一个箭头会让人困惑
      // ("点哪去?")。inRange 由 instance 各自 MaxVisibleDistance 控制。
      if (!inst.inRange) {
        return (
          <motion.div
            key={inst.id}
            className="absolute top-0 left-0"
            style={{ display: 'none' }}
          />
        );
      }

      if (inst.onScreen) {
        // 屏内:按正常 component 在 raw pos 渲染
        return (
          <motion.div
            key={inst.id}
            className="absolute top-0 left-0"
            initial={{ opacity: 0 }}
            animate={{ opacity: 1 }}
            exit={{ opacity: 0 }}
            transition={{ duration: 0.15, ease: 'easeOut' }}
            style={{
              transform: `translate3d(${inst.x * inv}px, ${inst.y * inv}px, 0) translate(-50%, -50%) scale(${inst.scale})`,
              zIndex: inst.zOrder,
            }}
          >
            <Component instance={inst} />
          </motion.div>
        );
      }

      // 屏外:用 edgeIndicator(没配则回退到主 component)
      const EdgeComponent = def.edgeIndicator ?? def.component;

      // Clamp 在 CSS 像素空间(viewport 已含 DPI scale),最后 *inv 一次给 overlay 局部坐标
      const clamp = clampToViewportEdge(
        inst.x,
        inst.y,
        inst.behindCamera,
        viewport.width,
        viewport.height,
      );

      return (
        <motion.div
          key={inst.id}
          className="absolute top-0 left-0"
          initial={{ opacity: 0, scale: 0.6 }}
          animate={{ opacity: 1, scale: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: 0.2, ease: 'easeOut' }}
          style={{
            // 外层 wrapper:负责把箭头**位置**移到贴边坐标。translate(-50%,-50%) 把组件
            // 自身中心对齐到点。
            transform: `translate3d(${clamp.x * inv}px, ${clamp.y * inv}px, 0) translate(-50%, -50%)`,
            zIndex: inst.zOrder,
          }}
        >
          {/* 内层 wrapper:负责**旋转**。把"位置"跟"朝向"两个 transform 拆到两层是为了
              不互相干扰 —— translate(-50%) 之类的居中要在旋转之前(rotate 会改坐标系)。 */}
          <div style={{ transform: `rotate(${clamp.rotationDeg}deg)` }}>
            <EdgeComponent instance={inst} />
          </div>
        </motion.div>
      );
    }

    case 'handoff': {
      // 屏内正常渲染;屏外返 null,内容由 onOffscreen 回调转给的对端系统(例 cinematic
      // subtitle store)负责显示。回调 fire 在 useEffect 里,这里只控渲染。
      if (!visibleHere) {
        // 返 null 而不是 display:none —— handoff 的语义就是"这个 instance 不再属于
        // floating UI 范畴",彻底卸载让对端接管显示资源(避免 floating 这边还占着
        // DOM 节点 / framer-motion 跑动画)。AnimatePresence 会跑 exit 动画再消失。
        return null;
      }
      return (
        <motion.div
          key={inst.id}
          className="absolute top-0 left-0"
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: 0.2, ease: 'easeOut' }}
          style={{
            transform: `translate3d(${inst.x * inv}px, ${inst.y * inv}px, 0) translate(-50%, -50%) scale(${inst.scale})`,
            zIndex: inst.zOrder,
          }}
        >
          <Component instance={inst} />
        </motion.div>
      );
    }

    case 'always-render': {
      // 任何时候都按 raw pos 渲染。Behind-camera + 大屏外坐标可能很离谱,但 always-render
      // 的设计意图就是"消费者知道自己在干嘛"(调试 / 工具),不做防呆。
      return (
        <motion.div
          key={inst.id}
          className="absolute top-0 left-0"
          initial={{ opacity: 0 }}
          animate={{ opacity: 1 }}
          exit={{ opacity: 0 }}
          transition={{ duration: 0.15, ease: 'easeOut' }}
          style={{
            transform: `translate3d(${inst.x * inv}px, ${inst.y * inv}px, 0) translate(-50%, -50%) scale(${inst.scale})`,
            zIndex: inst.zOrder,
          }}
        >
          <Component instance={inst} />
        </motion.div>
      );
    }
  }
}
