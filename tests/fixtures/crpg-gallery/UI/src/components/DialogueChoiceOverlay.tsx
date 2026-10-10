import { useEffect, useRef, useState } from 'react';
import { Lock } from 'lucide-react';
import { onUECall, sendToUE } from '@/lib/ue-bridge';
import { useDialogueChoiceStore, type DialogueChoiceOption } from '@/stores/dialogue-choice-store';
import { getChoiceEmphasis } from '@/config/choice-emphasis';
import { useT } from '@/stores/i18n-store';
import { useUIInputStore } from '@/stores/ui-input-store';
import { useSubtitleStore } from '@/stores/subtitle-store';

/**
 * 对话分支选项面板 — 巫师 3 式的沉浸电影化选项列。
 *
 * 视觉：**字幕克隆** —— 文字的字体/字号/颜色/text-shadow 逐字段照搬
 * `.subtitle-text`（--font-subtitle、18px×fontScale、#fff、双层暗影，
 * 字号跟随字幕的 fontScale 设置），**选项集合整体**背后复用一层
 * `.subtitle-glow` 的羽化径向衬底（rgba(5,3,10) 椭圆渐隐 + blur(10px)，
 * 强度吃字幕的 bgOpacity 设置）——这是引擎内唯一多轮验证过的"亮场景
 * 可读"配方。装订线（左侧两端渐隐的黄铜竖线 + 端点菱形）保留增添质感；
 * 焦点提示 = 行前黄铜菱形标记 + 文字染金色主题强调色（--color-gold-light）。
 * 不做文字缩放——Ultralight 对 scale 过渡中的文字每帧重栅格化字形，
 * 是滚动卡顿的实测主源；纯颜色过渡字号恒定、字形缓存稳定。
 * 锚定屏幕中心偏右，文本**左对齐**。没有面板实体，行间空白点击穿透世界。
 * 历史衬底方案（别再试）：平涂发灰 / 深色 radial 成蛋 / 九宫格在
 * Ultralight 内渲染错位 / 纯 text-shadow 白背景下不够。
 *
 * ── iOS 时间轮式滚动 ───────────────────────────────────────────────
 * CRPG 选项可能非常多：超过 VIEW_ROWS 行时视口固定高度，内部列表用
 * transform: translateY 滚动（无原生滚动条）。键盘/手柄移动焦点时自动把
 * 焦点行滚到视口中心（两端 clamp），鼠标滚轮直接滚动列表。
 * 渐隐只发生在"还有内容可滚"的那一侧边缘（边缘渐隐带，提示可滚动方向）——
 * 不要做"离中心越远越暗"的全局衰减，稍微超行的列表会显得整体被压暗。
 *
 * C++（UDialogueUIDispatcher）推送：
 *   CallUI("showDialogueChoices", { options: [{ id, text, hint, emphasis, selectable }] })
 *   CallUI("hideDialogueChoices", {})
 * 选择回传：sendToUE('dialogue.choiceSelected', { id })。
 * 置灰选项（selectable=false）不发送；即使发了，C++ 侧 SelectOption 也会拒绝。
 *
 * ── 手柄 / 键盘导航 ────────────────────────────────────────────────
 * useGamepadInput（App 全局）把 UE 的 uiInput 语义事件（引擎内手柄/键盘经
 * EnhancedInput IMC）和浏览器 Gamepad API 统一合成为冒泡的 KeyboardEvent，
 * 这里只监听 document keydown：↑/↓ 移动焦点（含置灰项，便于阅读原因）、
 * Enter 确认、数字键 1-9 快选。鼠标 hover 与手柄焦点收敛到同一个 focusedId
 * （事实源在 focusedRef，同步更新——连续快速输入不能等 React 渲染回写）。
 * 面板是 always-mounted overlay（不进 page 栈），需显式
 * `ui.input.setExplicitActive` 声明输入接管权（同 BattleUnitPanel）。
 *
 * ── DOM 池（Ultralight 防掉帧，参照 SubtitleOverlay）────────────────
 * 面板根节点与固定 MAX_CHOICE_SLOTS 个选项行常驻挂载：空闲行 display:none；
 * 收起走 160ms 透明度过渡（state='exiting'）后才 display:none——瞬时隐藏
 * 会把最后一帧留在合成纹理上。hover/focus 用 JS state（CSS :hover 在
 * Ultralight 的悬浮层上会闪烁）。
 */

const MAX_CHOICE_SLOTS = 20;

// 与 dialogue-choice-store 的 EXIT_TRANSITION_MS 保持同步。
const PANEL_TRANSITION_MS = 160;

/** 基准行高（px），随字幕 fontScale 缩放。滚动偏移与轮式衰减全按实际行高计算。 */
const BASE_ROW_H = 40;
/** 视口最多同时展示的行数；超过进入轮式滚动。 */
const VIEW_ROWS = 7;
/**
 * 文本暗影 —— 基于字幕配方但第二层从 14px 收到 4px：Ultralight 无层缓存，
 * 滚动期间每帧对每个字形做 14px 半径模糊是 FPS 大跌的主要字形级开销
 * （字幕只有 1-2 行且静止，扛得住；选项 8 行且在滚）。
 */
const CHOICE_TEXT_SHADOW = '0 1px 4px rgba(0,0,0,0.9), 0 0 4px rgba(0,0,0,0.6)';

export function DialogueChoiceOverlay() {
  const options = useDialogueChoiceStore((s) => s.options);
  const panel = useDialogueChoiceStore((s) => s.panel);
  const show = useDialogueChoiceStore((s) => s.show);
  const hide = useDialogueChoiceStore((s) => s.hide);
  const subtitleSettings = useSubtitleStore((s) => s.settings);
  const t = useT();

  // 字号/衬底强度跟随字幕设置（完全照搬字幕观感）；行高随字号缩放。
  const fontScale = subtitleSettings.fontScale;
  const bgOpacity = subtitleSettings.bgOpacity;
  const rowH = Math.round(BASE_ROW_H * fontScale);
  const rowHRef = useRef(rowH);
  rowHRef.current = rowH;

  const [focusedId, setFocusedId] = useState<string | null>(null);
  const [pressedId, setPressedId] = useState<string | null>(null);
  const focusedRef = useRef<string | null>(null);
  const applyFocus = (id: string | null) => {
    focusedRef.current = id;
    setFocusedId(id);
  };

  // 轮式滚动偏移（px）。键盘/手柄移动焦点时居中焦点行，鼠标滚轮自由滚动。
  const [scrollOffset, setScrollOffset] = useState(0);

  // keydown 监听器常驻（依赖为空），经 ref 读取最新状态，避免反复挂卸。
  const optionsRef = useRef(options);
  const panelRef = useRef(panel);
  optionsRef.current = options;
  panelRef.current = panel;

  const viewRowCount = Math.min(options.length, VIEW_ROWS);
  const viewH = viewRowCount * rowH;
  const wheelMode = options.length > VIEW_ROWS;
  const maxOffset = Math.max(0, options.length * rowH - viewH);

  // 行高经 rowHRef 读取：keyboard 监听器是空依赖的稳定闭包，直接捕获 rowH 会过期。
  /** 把 offset clamp 进合法滚动范围（fit 模式下恒为 0）。 */
  const clampOffset = (v: number, count: number) => {
    const h = rowHRef.current;
    const max = Math.max(0, count * h - Math.min(count, VIEW_ROWS) * h);
    return Math.max(0, Math.min(v, max));
  };

  /** 让 index 行尽量滚到视口中心（两端 clamp，不留空白）。 */
  const centerOn = (index: number, count: number) => {
    const h = rowHRef.current;
    return clampOffset(index * h + h / 2 - (Math.min(count, VIEW_ROWS) * h) / 2, count);
  };

  useEffect(() => {
    const unsub1 = onUECall('showDialogueChoices', (data: unknown) => {
      const d = data as { options?: Partial<DialogueChoiceOption>[] };
      const normalized: DialogueChoiceOption[] = (d.options ?? []).map((o, i) => ({
        id: o.id ?? `__opt_${i}`,
        text: o.text ?? '',
        hint: o.hint ?? '',
        emphasis: o.emphasis ?? '',
        selectable: o.selectable !== false,
      }));
      setPressedId(null);
      setScrollOffset(0);
      // 手柄模式下焦点直接落到第一项，玩家按 D-pad 前就有锚点；
      // 鼠标模式不预置高亮（凭空高亮对鼠标用户是噪音）。
      applyFocus(
        useUIInputStore.getState().device === 'gamepad' && normalized.length > 0
          ? normalized[0].id
          : null,
      );
      show(normalized);
    });
    const unsub2 = onUECall('hideDialogueChoices', () => {
      applyFocus(null);
      setPressedId(null);
      hide();
    });
    return () => {
      unsub1();
      unsub2();
    };
  }, [show, hide]);

  // 选择回传（Enter / 数字键 / 点击共用）。
  const selectOption = (opt: DialogueChoiceOption | undefined) => {
    if (opt && opt.selectable) {
      sendToUE('dialogue.choiceSelected', { id: opt.id });
    }
  };

  // ── 键盘 / 手柄（经 useGamepadInput 合成的 KeyboardEvent 同样走这里）──
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (panelRef.current !== 'visible') return;
      const opts = optionsRef.current;
      if (opts.length === 0) return;

      if (e.key === 'ArrowUp' || e.key === 'ArrowDown') {
        e.preventDefault();
        const dir = e.key === 'ArrowDown' ? 1 : -1;
        const prev = focusedRef.current;
        const idx = prev !== null ? opts.findIndex((o) => o.id === prev) : -1;
        // 无焦点时：↓ 从第一项开始，↑ 从最后一项开始；有焦点时循环滚动。
        // 置灰项也可聚焦——玩家需要能读到"为什么不可选"的提示。
        const next = idx < 0
          ? (dir > 0 ? 0 : opts.length - 1)
          : (idx + dir + opts.length) % opts.length;
        applyFocus(opts[next].id);
        setScrollOffset(centerOn(next, opts.length));
        return;
      }

      if (e.key === 'Enter') {
        const focused = focusedRef.current;
        if (focused !== null) {
          e.preventDefault();
          selectOption(opts.find((o) => o.id === focused));
        }
        return;
      }

      // 数字键 1-9 快选（无需先聚焦）
      if (/^[1-9]$/.test(e.key)) {
        selectOption(opts[Number(e.key) - 1]);
      }
    };

    document.addEventListener('keydown', onKeyDown);
    return () => document.removeEventListener('keydown', onKeyDown);
  }, []);

  // 显式声明 UI navigation 输入接管权 —— always-mounted overlay 不进 page 栈，
  // stack-top 推断对它无效（同 BattleUnitPanel 的处理）。
  const needsInput = panel === 'visible' && options.length > 0;
  useEffect(() => {
    sendToUE('ui.input.setExplicitActive', { active: needsInput });
    return () => {
      sendToUE('ui.input.setExplicitActive', { active: false });
    };
  }, [needsInput]);

  // 固定槽位：options 之外的槽渲染为 display:none 的空行（常驻，不卸载）。
  const slots: (DialogueChoiceOption | null)[] = Array.from(
    { length: MAX_CHOICE_SLOTS },
    (_, i) => options[i] ?? null,
  );

  const focused = focusedId !== null ? options.find((o) => o.id === focusedId) : undefined;
  const focusedEmphasis = focused ? getChoiceEmphasis(focused.emphasis) : undefined;

  // 提示栏文案：手写 hint 优先，回退强调类别默认提示；置灰选项追加"条件不满足"。
  let hintText = '';
  if (focused) {
    hintText = focused.hint || (focusedEmphasis ? t(focusedEmphasis.defaultHintKey) : '');
    if (!focused.selectable) {
      hintText = hintText
        ? `${t('dialogue.choice.locked')} · ${hintText}`
        : t('dialogue.choice.locked');
    }
  }

  const visible = panel === 'visible';

  return (
    <div
      // 屏幕中下偏右（巫师 3 位置）：中心轴点下移到 62%。曾试过 bottom-[8%]
      // 底部锚定——对话相机多为 21:9 约束画幅，16:9 屏上有上下黑边
      // （约各 12%），底锚会让选项沉进下黑边，打回。中心轴点 + 62% 让选项
      // 落在 21:9 画幅内的中下带，且在不同画幅比下都不会出画。
      // 位移由内联 transform 负责——不要用 Tailwind 的 translate 工具类：
      // v4 走独立 `translate` 属性，会跟 transform 叠加成双倍位移
      className="fixed left-[55%] top-[62%] z-[55] w-[clamp(420px,40vw,640px)] pointer-events-none select-none"
      style={{
        display: panel === 'hidden' ? 'none' : 'block',
        opacity: visible ? 1 : 0,
        transform: `translateY(-50%) translateX(${visible ? 0 : 10}px)`,
        transition: `opacity ${PANEL_TRANSITION_MS}ms ease-out, transform ${PANEL_TRANSITION_MS}ms ease-out`,
      }}
    >
      {/* 内容容器。无面板实体：容器不设 cursor/onClick，行间空白的点击
          穿透到游戏世界（per-pixel 路由只在非默认 cursor 处吃点击）。 */}
      <div className="relative pointer-events-auto px-5 pt-3 pb-1.5">
        {/* 集合级衬底 —— 完全沿用字幕的居中椭圆渐隐（.subtitle-glow 配方，
            rgba(5,3,10) × bgOpacity 设置；多一档 65% 停靠补偿无 blur 的平滑感）。
            刻意不带字幕的 filter: blur(10px)：Ultralight 无层缓存，滚动期间
            每帧全区域高斯模糊是 FPS 大跌的元凶之一（字幕静止扛得住）。
            方向实验全部否决（保持居中，别再试）：左→右线性 = 上下硬边；
            阶梯羽化条 = 过渡生硬；`ellipse at 0% 50%` 左锚 = 整条左边界
            落在渐变最暗段 → span 左缘一条硬切线。 */}
        <span
          aria-hidden
          className="pointer-events-none absolute"
          style={{
            inset: '-18px -64px',
            background: `radial-gradient(ellipse at center, rgba(5,3,10,${0.7 * bgOpacity}) 0%, rgba(5,3,10,${0.25 * bgOpacity}) 50%, rgba(5,3,10,${0.1 * bgOpacity}) 65%, transparent 80%)`,
          }}
        />
        {/* 列装订线 —— 两端渐隐的黄铜竖线 + 端点菱形（雕版母题，OrnateDivider
            的竖直变体）。给选项列一个"这是一组"的结构暗示，保留质感。 */}
        <span aria-hidden className="pointer-events-none absolute left-[5px] top-1 bottom-1 w-px"
          style={{
            background:
              'linear-gradient(180deg, rgba(199,166,103,0) 0%, rgba(199,166,103,0.55) 14%, rgba(199,166,103,0.55) 86%, rgba(199,166,103,0) 100%)',
          }}
        />
        <span aria-hidden className="pointer-events-none absolute left-[3px] top-[10px]"
          style={{ width: 5, height: 5, transform: 'rotate(45deg)', background: 'rgba(199,166,103,0.75)' }}
        />
        <span aria-hidden className="pointer-events-none absolute left-[3px] bottom-[10px]"
          style={{ width: 5, height: 5, transform: 'rotate(45deg)', background: 'rgba(199,166,103,0.75)' }}
        />
        {/* 滚动视口：固定高度 + overflow hidden，内部 <ol> 用 translateY 滚动
            （无原生滚动条；鼠标滚轮直接改偏移，键盘/手柄导航自动居中焦点行）。 */}
        <div
          style={{ height: viewH, overflow: 'hidden', position: 'relative' }}
          onWheel={(e) => {
            const count = optionsRef.current.length;
            setScrollOffset((prev) => clampOffset(prev + e.deltaY * 0.4, count));
          }}
        >
          {/* 无 transition：滚轮/键盘事件直接落位。transition 会让每个输入事件
              拖出 140ms 的连续动画帧——Ultralight 无层缓存，动画期间每帧整视图
              重画，连续滚动 = 永远在动画中。快照式落位后脏帧只发生在事件当帧。 */}
          <ol style={{ transform: `translateY(${-scrollOffset}px)` }}>
            {slots.map((opt, i) => {
              if (opt === null) {
                // 空闲槽：常驻挂载，display:none（DOM 池，防移除掉帧）
                return <li key={i} style={{ display: 'none' }} />;
              }

              const emphasis = getChoiceEmphasis(opt.emphasis);
              const EmphasisIcon = emphasis?.icon;
              const isFocused = focusedId === opt.id;
              const isPressed = pressedId === opt.id;

              // 边缘渐隐带：只在"还有内容可滚"的那一侧边缘淡出（提示滚动方向）。
              // 不做"离中心越远越暗"的全局衰减——稍微超行时整个列表会显得被压暗。
              let falloff = 1;
              if (wheelMode) {
                const FADE_ZONE = rowH * 1.25;
                const rowCenter = i * rowH + rowH / 2 - scrollOffset;
                let f = 1;
                if (scrollOffset > 1) {
                  f = Math.min(f, Math.max(0, rowCenter / FADE_ZONE));
                }
                if (scrollOffset < maxOffset - 1) {
                  f = Math.min(f, Math.max(0, (viewH - rowCenter) / FADE_ZONE));
                }
                falloff = 0.25 + 0.75 * Math.min(1, f);
              }

              return (
                <li
                  key={i}
                  className={[
                    'flex items-center gap-2.5',
                    opt.selectable ? 'cursor-pointer' : 'cursor-not-allowed',
                  ].join(' ')}
                  style={{
                    height: rowH,
                    // 同样无 transition —— 边缘衰减随滚动偏移直接落位
                    opacity: falloff * (opt.selectable ? 1 : 0.55),
                  }}
                  onMouseEnter={() => applyFocus(opt.id)}
                  onMouseLeave={() => {
                    if (focusedRef.current === opt.id) {
                      applyFocus(null);
                    }
                    setPressedId((prev) => (prev === opt.id ? null : prev));
                  }}
                  onMouseDown={() => opt.selectable && setPressedId(opt.id)}
                  onMouseUp={() => setPressedId((prev) => (prev === opt.id ? null : prev))}
                  onClick={() => selectOption(opt)}
                >
                  {/* 焦点菱形标记 —— 无背景方案里替代"行高亮"的雕版指示物 */}
                  <span className="flex w-[12px] shrink-0 items-center justify-center">
                    <span
                      style={{
                        width: 6,
                        height: 6,
                        background: '#c7a667',
                        transform: `rotate(45deg) scale(${isFocused ? 1 : 0.3})`,
                        opacity: isFocused ? 1 : 0,
                        transition: 'transform 120ms ease-out, opacity 120ms ease-out',
                        boxShadow: '0 1px 2px rgba(0,0,0,0.8)',
                      }}
                    />
                  </span>

                  {/* 强调 icon 固定列宽 —— 左对齐时保证所有行文字起点一致 */}
                  <span className="flex w-[18px] shrink-0 items-center justify-center">
                    {EmphasisIcon && (
                      <EmphasisIcon
                        size={14}
                        strokeWidth={2}
                        style={{ color: emphasis!.color }}
                      />
                    )}
                  </span>

                  {/* 选项正文 —— .subtitle-text 逐字段克隆（字体/字号/字距/行高/
                      暗影全同字幕，字号跟随字幕 fontScale 设置）。
                      焦点提示 = 金色主题强调色（--color-gold-light，按压沉到
                      --color-gold）+ 行前菱形标记。不做任何 transform/字号变化：
                      Ultralight 对缩放过渡中的文字每帧重栅格化字形，是滚动/换焦
                      卡顿的主源（实测确认）；纯颜色过渡字号恒定，字形缓存稳定。
                      weight 500：中文命中 LXGW 家族里别名注册的 Noto W430
                      真字重实例（见 fonts.ts；600/500 都试过偏重，430 实测最佳，
                      CSS 槽位仍叫 500）；fontSynthesis none 让只有 400 字重的
                      EB Garamond / Shippori 回落真 Regular 而不是合成粗体
                      （英/日文保持原样）。 */}
                  <span
                    className="min-w-0 truncate font-[family-name:var(--font-subtitle)]"
                    style={{
                      fontSize: `${18 * fontScale}px`,
                      lineHeight: 1.5,
                      fontWeight: 500,
                      fontSynthesis: 'none',
                      letterSpacing: '0.5px',
                      color: !opt.selectable
                        ? 'rgba(255,255,255,0.85)'
                        : isPressed
                          ? 'var(--color-gold)'
                          : isFocused
                            ? 'var(--color-gold-light)'
                            : '#ffffff',
                      transition: 'color 100ms ease-out',
                      textShadow: CHOICE_TEXT_SHADOW,
                    }}
                  >
                    {opt.text}
                  </span>

                  {/* 置灰锁标 */}
                  {!opt.selectable && (
                    <Lock size={12} strokeWidth={2} className="shrink-0 text-brass/50" />
                  )}
                </li>
              );
            })}
          </ol>
        </div>

        {/* 提示行：固定高度防止 focus 切换时列表跳动；无提示时留白。
            pl 对齐强调 icon 列（marker 12 + gap 10）。 */}
        <div className="relative flex h-[26px] items-center gap-1.5 pl-[22px]">
          {focused && focusedEmphasis && (
            <span
              className="flex shrink-0 items-center gap-1 text-[11px]"
              style={{
                color: focusedEmphasis.color,
                textShadow: '0 1px 3px rgba(0,0,0,0.9)',
              }}
            >
              <focusedEmphasis.icon size={11} strokeWidth={2} />
              {t(focusedEmphasis.labelKey)}
            </span>
          )}
          {focused && focusedEmphasis && hintText && (
            <span className="text-[11px] text-brass/40">—</span>
          )}
          <span
            className="truncate text-[12px] italic font-[family-name:var(--font-subtitle)]"
            style={{ color: 'rgba(255,255,255,0.72)', textShadow: CHOICE_TEXT_SHADOW }}
          >
            {hintText}
          </span>
        </div>
      </div>
    </div>
  );
}
