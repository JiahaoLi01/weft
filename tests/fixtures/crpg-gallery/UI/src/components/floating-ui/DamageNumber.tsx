import { motion } from 'framer-motion';
import type { FloatingUIInstance } from '@/stores/floating-ui-store';
import { useDamageFlagLabel } from '@/i18n/battle-labels';

/**
 * Floating damage number —— `damage-number` widget type 的渲染。
 *
 * 数据(从 instance.data 读,跟 `UDamageNumberViewModel` 反射对齐):
 *   - Amount       float    数值绝对值,渲染时 round 成整数
 *   - bHeal        bool     true → 绿色字(治疗);false → 白 / 红色(伤害,跟元素绑)
 *   - bCritical    bool     暴击 → 字号更大 + outline glow + 落地时一条横贯数字的闪光带
 *                           (0.1s 纵向塌缩,类 Apex 碎甲径向闪光;无文字标签)
 *   - DamageType   number   EDamageType 枚举的 int 值,索引到 DAMAGE_TYPE_COLOR 表
 *
 * 动画:Tobias Ahlin Moving Letters #4 风格,**整体**(不是 per-char)pop-in → hold → scale-out。
 * 原始时间线被压短(用户偏好"更快")—— 原 800ms in + 500ms 间隔 + 600ms out 总 1.9s 太长,
 * 我们 0.9s 跑完,跟 C++ 端 TTL=1.0s 几乎对齐(留 0.1s 给 AnimatePresence 兜底)。
 *
 *   阶段          | 时间段       | scale  | opacity | easing
 *   --------------|--------------|--------|---------|----------
 *   pop-in        | 0 → 0.153s   | 0.2→1  | 0→1     | easeOut
 *   hold          | 0.153 → 0.648s | 1    | 1       | linear
 *   scale-out     | 0.648 → 0.9s | 1→3    | 1→0     | easeInExpo (cubic-bezier 0.7,0,0.84,0)
 *
 * **不切 per-char**:用户 quote 的 ML4 timeline 是 letters-1/2/3 三组整体动画,不是逐字符
 * stagger;我之前理解错了。
 *
 * Outer `FloatingUIOverlay` 还包了一层 motion.div 做 AnimatePresence enter/exit fade,与
 * 这里的内部 opacity 是相乘关系。整个生命周期:
 *   外层:  enter 0.15s fade-in → hold → exit 0.15s fade-out(C++ remove 触发)
 *   内层:  本组件的 ML4 0.9s timeline
 *   组合:  开头几乎对齐都是 0;中段都是 1;t=0.9 后内层已经 opacity=0,外层 exit 看不见。
 *
 * 用 framer-motion 而不是 anime.js:跟项目其他组件统一栈,bundle 小一点。
 */

// 元素颜色表 —— 跟 `EDamageType` 声明顺序对齐(C++ 那边 enum 索引 → JS 这边数组索引)。
const DAMAGE_TYPE_COLOR: readonly string[] = [
  '#a8ff60', // Acid     - 强酸,绿
  '#e8e8e8', // Bludgeoning - 钝,白灰
  '#7ec8ff', // Cold      - 冰蓝
  '#ff7043', // Fire      - 火橙
  '#fff176', // Lightning - 雷黄
  '#9c6dff', // Necrotic  - 腐紫
  '#ffd6a0', // Piercing  - 穿刺,淡橘
  '#aef58a', // Poison    - 毒绿
  '#ff9eea', // Psychic   - 心灵粉
  '#fff0a8', // Radiant   - 辐光金
  '#ffe0e0', // Slashing  - 挥砍,粉白
  '#b0d4ff', // Thunder   - 雷霆,亮蓝紫
] as const;

const HEAL_COLOR = '#7dffaa';
const FALLBACK_COLOR = '#ffffff';

// 被 modifier 增幅/削弱时,主数字旁显示"被划掉的原值 + 升降箭头"。配色走古典克制(无霓虹):
//   原值 = **元素色**(跟主数字同色),靠删除线 + 小字号表达"已作废",而非靠灰掉 —— 用户要求被取消
//          的伤害也带颜色,跟元素绑定读起来更连贯(火橙的原值划掉、火橙的实际值居中)。
//   箭头 = 暖琥珀↑(增幅)/ 冷灰↓(削弱),方向 + 色相双重表达增/减。
const ARROW_AMPLIFIED = '#e6b85c';
const ARROW_REDUCED = '#9aa7ad';

// 伤害飘字的屏幕偏移 —— 头顶有血条会遮住居中飘字,整体右移避开;再叠一个**按实例 id 确定**的
// 随机散布(同一处连续命中不会叠成一坨)。确定性散布:从 id 哈希出,跨帧稳定不抖。
const DMG_BASE_RIGHT_PX = 52; // 右移基量(约半个血条宽 + 余量),清开头顶血条
const DMG_RAND_X_PX = 18;     // 水平随机 ±
const DMG_RAND_Y_PX = 26;     // 垂直随机 ±(垂直多散一点,时间序列命中更不易叠)

/** FNV-1a 字符串哈希 → uint32。用于从 instance.id 派生稳定的伪随机偏移(同 id 跨帧一致)。 */
function hashStringU32(s: string): number {
  let h = 2166136261;
  for (let i = 0; i < s.length; i++) {
    h ^= s.charCodeAt(i);
    h = Math.imul(h, 16777619);
  }
  return h >>> 0;
}

// easeInExpo 的标准 cubic-bezier(easings.net),framer-motion 没内建,得手写。
// 形状:前半几乎不动,后半 exponential 加速 —— 配合 scale 1→3 给出"爆开"的视觉感。
const EASE_IN_EXPO: [number, number, number, number] = [0.7, 0, 0.84, 0];

interface DamageNumberProps {
  instance: FloatingUIInstance;
}

/**
 * 修正原值徽标 —— 主数字旁的"被划掉小号原值 + 升降箭头",仅在被 modifier 增幅/削弱时显示。
 *
 * 删除线动画**仿照 AbnormalFlagFloating 的移除态**:一条线从左到右 scaleX 0→1 划过(GPU 合成,
 * 零 layout),delay 一拍让读序是「原值出现 → 被划掉」。绝对定位在主数字右侧,**不进主数字的
 * flex 流**,所以主数字仍居中在飘字锚点上。
 */
function OriginalStrikeBadge({ originalText, amplified, color }: { originalText: string; amplified: boolean; color: string }) {
  const arrowColor = amplified ? ARROW_AMPLIFIED : ARROW_REDUCED;
  // 实心三角(无 stroke,Ultralight 下比描边箭头更稳):增幅↑ 顶点朝上,削弱↓ 顶点朝下。
  const arrowPath = amplified ? 'M5 1.5 L9 8.5 L1 8.5 Z' : 'M1 1.5 L9 1.5 L5 8.5 Z';
  return (
    <div
      className="pointer-events-none"
      style={{
        position: 'absolute',
        left: 'calc(100% + 0.1em)',
        top: '50%',
        transform: 'translateY(-50%)',
        display: 'flex',
        alignItems: 'center',
        gap: '0.12em',
        // 主数字的 ~40%,小但可读;跟主数字一起被父层 burst-out 放大飘走。
        fontSize: '0.4em',
        whiteSpace: 'nowrap',
      }}
      aria-hidden
    >
      <svg
        viewBox="0 0 10 10"
        style={{ width: '1em', height: '1em', flexShrink: 0, filter: 'drop-shadow(0 1px 1px rgba(0,0,0,0.9))' }}
      >
        <path d={arrowPath} fill={arrowColor} />
      </svg>
      <span
        style={{
          position: 'relative',
          // 元素色(同主数字)—— 被划掉的原值也带颜色,靠删除线 + 小字号表达"作废"。
          color,
          // subtitle 混合栈:数字走 Cinzel,跟主数字同族但不强行继承外层 Cinzel Decorative。
          fontFamily: 'var(--font-subtitle)',
          fontWeight: 500,
          letterSpacing: '0.02em',
          textShadow: '0 1px 2px rgba(0,0,0,0.85), 0 0 3px rgba(0,0,0,0.6)',
        }}
      >
        {originalText}
        {/* 删除线 —— scaleX 0→1 从左划过(仿 AbnormalFlagFloating 移除态)。 */}
        <motion.span
          aria-hidden
          style={{
            position: 'absolute',
            left: 0,
            right: 0,
            top: '50%',
            height: 2,
            transformOrigin: 'left center',
            background: color,
            borderRadius: 1,
            boxShadow: '0 0 1px rgba(0,0,0,0.9)',
          }}
          initial={{ scaleX: 0, opacity: 0.95 }}
          animate={{ scaleX: 1 }}
          transition={{ duration: 0.3, delay: 0.18, ease: 'easeOut' }}
        />
      </span>
    </div>
  );
}

export function DamageNumber({ instance }: DamageNumberProps) {
  // 从 instance.data 读 VM 字段。C++ 反射用 raw key 名,跟 VM property 同名。
  const amount = Number(instance.data.Amount ?? 0);
  const bHeal = Boolean(instance.data.bHeal);
  const bCritical = Boolean(instance.data.bCritical);
  const damageType = Number(instance.data.DamageType ?? 0);
  const bOverPenetrated = Boolean(instance.data.bOverPenetrated);
  const bFailedPenetrate = Boolean(instance.data.bFailedPenetrate);

  // 修正原值:被 buff modifier 增幅 / 削弱时,显示被划掉的原始量 + 升降箭头。
  // bModified 由 C++ 端排除 HP 钳制后给出(过量击杀/治疗不算修正);这里再做一次"四舍五入后
  // 确实不同"的防御 —— 避免 30.4→30.0 这种取整后视觉一样却画线。amplified 直接比可见的两个数,
  // 箭头方向永远和"原值 vs 实际值"一致(增幅↑ / 削弱↓)。
  const originalAmount = Number(instance.data.OriginalAmount ?? 0);
  const bModified = Boolean(instance.data.bModified);
  const showOriginal = bModified && Math.round(originalAmount) !== Math.round(amount);
  const amplified = amount > originalAmount;

  // 屏幕偏移:整体右移避开头顶血条遮挡,再叠按 instance.id 派生的确定性散布(同一处连续命中错开,
  // 不叠成一坨)。从 id 哈希取两路 [0,1) → 映射到 ±range,跨帧稳定不抖(纯函数,不用 state/random)。
  const idHash = hashStringU32(instance.id);
  const randUnitX = (idHash & 0xffff) / 0xffff;          // 低 16 位
  const randUnitY = ((idHash >>> 16) & 0xffff) / 0xffff; // 高 16 位
  const offsetX = DMG_BASE_RIGHT_PX + (randUnitX * 2 - 1) * DMG_RAND_X_PX;
  const offsetY = (randUnitY * 2 - 1) * DMG_RAND_Y_PX;

  // 装甲穿透标签 —— 互斥的两个 flag,二选一。OverPenetrated 优先(同时为 true 是策划配错,
  // 但渲染端做防御:优先穿透,不双标)。`useDamageFlagLabel` 缺翻译返 null,自动安静失败。
  const getFlagLabel = useDamageFlagLabel();
  const flagLabel = bOverPenetrated
    ? getFlagLabel('OverPenetrated')
    : bFailedPenetrate
    ? getFlagLabel('FailedPenetrate')
    : null;

  const text = Math.round(amount).toString();
  const originalText = Math.round(originalAmount).toString();
  const color = bHeal
    ? HEAL_COLOR
    : (DAMAGE_TYPE_COLOR[damageType] ?? FALLBACK_COLOR);

  // 普通 / 暴击两档字号。Cinzel Decorative 是 display caps face,小字号下细节噪点多读不清,
  // 整体放大让"装饰雕刻感"读得出来,也让屏幕中央 4-5m 外的飘字仍清晰。暴击 ~1.6x 区分。
  const fontSize = bCritical ? '64px' : '40px';
  // 暴击保留一个柔光 halo,强化"重要这下命中"的视觉提示;非暴击完全无 shadow。
  const textShadow = bCritical ? `0 0 6px ${color}, 0 0 12px ${color}` : undefined;

  return (
    // 外层做静态屏幕偏移(右移避血条 + 确定性随机散布)。内层 motion.div 负责 pop/scale 动画 ——
    // 拆两层因为 framer 用 transform 跑 scale 动画,静态 translate 不能跟它共用同一元素。
    // 偏移在 overlay 的缩放空间内(overlay 外层已 translate(-50%,-50%) + scale),所以会随距离一起缩,
    // 跟头顶血条同比例,远近都一致地避开。
    <div style={{ transform: `translate(${offsetX}px, ${offsetY}px)` }}>
    <motion.div
      className="select-none pointer-events-none"
      style={{
        // 显式用 Cinzel Decorative,不走 var(--font-display)。`--font-display` 默认是 Noto
        // Serif SC,数字优先走 NotoSerif,Cinzel Decorative 只在 subtitle-mixed stack 里
        // unicode-range 限定到数字时才用上。伤害飘字只渲染数字,这里直接锚定 Cinzel
        // Decorative,fallback 留 serif 兜底。Cinzel Decorative 的 @font-face 在 startup
        // 时由 restoreFontsFromStorage 全部预注册,所以这里能直接 by-name 引用。
        fontFamily: "'Cinzel Decorative', serif",
        // font-weight 600 = Cinzel Decorative Black 真字重(fonts.ts 已经把 Bold / Black
        // 的 @font-face 注册好),不再走浏览器的合成 bold。
        fontWeight: 600,
        fontSize,
        color,
        textShadow,
        whiteSpace: 'nowrap',
      }}
      initial={{ opacity: 0, scale: 0.2 }}
      animate={{
        // 4 个 keyframe:start → pop-in 完 → hold 结束 → scale-out 完
        opacity: [0, 1, 1, 0],
        scale:   [0.2, 1, 1, 3],
      }}
      // Per-property transition —— framer-motion 顶层 `ease` 会把整段 easing 同时应用到
      // opacity 和 scale。easeInExpo 让 opacity 在 out 段最后一刻才掉,跟 scale 1→3 的
      // burst 视觉叠在一起像"瞬间消失"。把 opacity 拆出来用 easeOut(早开始 / 慢结束)
      // 才能看到清晰的 fade,scale 保留 easeInExpo 给爆开感。
      //
      // 时间点 [0, 0.15, 0.76, 1] × duration 1.65s:
      //   pop-in     0      → 0.247s    (0.2→1 / 0→1)
      //   hold       0.247  → 1.254s    (1 / 1)       ≈ 1.0s 常态停留
      //   scale-out  1.254  → 1.65s     (1→3 / 1→0)   ≈ 0.4s 淡出
      //
      // C++ 端 TTL 配合改成 1.75s(BattleEntityActorComponent::SpawnDamageNumberFloating),
      // 留 0.1s buffer 让 framer-motion 完整跑完再被 manager 删 entry。
      transition={{
        opacity: {
          duration: 1.65,
          times: [0, 0.1, 0.76, 1],
          ease: ['easeOut', 'linear', 'easeOut'],
        },
        scale: {
          duration: 1.65,
          times: [0, 0.1, 0.76, 1],
          ease: ['easeOut', 'linear', EASE_IN_EXPO],
        },
      }}
    >
      <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', lineHeight: 1 }}>
        {/* 主数字 wrapper:relative,让"修正原值徽标"绝对定位在它右侧而**不挤动主数字居中**。 */}
        <div style={{ position: 'relative', display: 'inline-flex' }}>
          <span>{text}</span>
          {bCritical && (
            // 暴击闪光 —— 仿 Apex 碎甲特效:一条横贯数字的亮色光带,瞬时出现(opacity 0→1
            // 无过渡)后 0.1s 内纵向塌缩到 0(scaleY→0),读作一次径向闪光的"快门"。是叠在
            // 数字上方的独立 bloom 层,不是文字本身发光,也不加文字标签。
            // Ultralight 安全路径:光带 = **不透明**双层圆角矩形(外层元素色 + 内层白热
            // 核心),避开 rgba 背景的像素噪点;塌缩走 transform scaleY(GPU 合成);bloom
            // 用小半径 box-shadow(大半径 shadow 在 Ultralight 下有噪点/性能问题)。
            // 尺寸全部用 em,跟随暴击字号缩放;左右各外扩 0.75em,光带比数字宽一截。
            // 时序:delay 0.1s 瞬时点亮,0.1~0.2s 塌缩 —— pop-in 落地(0.165s)正在窗口
            // 中间,闪光压在"砸实"那一拍上。
            <motion.div
              aria-hidden
              style={{
                position: 'absolute',
                left: '-0.75em',
                right: '-0.75em',
                top: '50%',
                y: '-50%',
                height: '0.32em',
                borderRadius: '0.16em',
                background: color,
                boxShadow: `0 0 8px ${color}`,
                pointerEvents: 'none',
                display: 'flex',
                alignItems: 'center',
                justifyContent: 'center',
              }}
              // 用整段 keyframes + times 表达"延迟 0.1s → 瞬时点亮 → 0.1s 塌缩",跟主数字
              // 动画同一种写法。**不要**改回 per-property `delay`:framer 会先把终值
              // scaleY(0) 写进 inline style,delay 期间 WAAPI 不填充效果,光带在等待期
              // 就渲染成塌缩完的状态,闪光整个丢失(实测踩过)。
              // times:0→0.49 隐藏(opacity 0);0.49→0.5 瞬时点亮(~2ms,读作 Apex 的
              // "瞬时出现");0.5→1 scaleY 1→0 塌缩(easeIn:先慢后快,快门感)。
              initial={{ opacity: 0, scaleY: 1 }}
              animate={{ opacity: [0, 0, 1, 1], scaleY: [1, 1, 1, 0] }}
              transition={{ duration: 0.2, times: [0, 0.49, 0.5, 1], ease: ['linear', 'linear', 'easeIn'] }}
            >
              {/* 白热核心 —— 中段更亮,给"闪光"而非"色条"的读感;随外层一起塌缩。 */}
              <div
                style={{
                  width: '60%',
                  height: '0.16em',
                  borderRadius: '0.08em',
                  background: '#ffffff',
                  boxShadow: '0 0 6px #ffffff',
                }}
              />
            </motion.div>
          )}
          {showOriginal && <OriginalStrikeBadge originalText={originalText} amplified={amplified} color={color} />}
        </div>
        {flagLabel && (
          <span
            style={{
              // 标签字号:数字的 ~30%,小但仍能读;非暴击 12px / 暴击 ~19px。
              fontSize: bCritical ? '0.3em' : '0.32em',
              // 字重 500(SemiBold)—— 比之前 400 略粗,小字号 + 宽字距下笔画太细容易糊。
              // 主数字 600 仍然权重更高,视觉主次没变。
              fontWeight: 500,
              // **不**靠 opacity 降级表达"次要" —— 那样在元素饱和色 + 杂背景上糊得没法读。
              // 改用字号 / 字重 / 字距差异表达主次,opacity 保持 1.0 让标签清晰。
              opacity: 1,
              // letterSpacing 跟主项目的 small-caps 风格一致(0.22em wide),
              // 中文字符在这种宽字距下读着像"刻字提示"。
              letterSpacing: '0.22em',
              textTransform: 'uppercase',
              // 标签字体回到 subtitle stack(混合 Cinzel 数字 + Garamond 拉丁 + CJK 衬线),
              // **不**继承外层 Cinzel Decorative —— 那个是 display 大写专用,中文会 fallback 到
              // 兜底字体不好看。subtitle stack 通过 unicode-range 自动按字符切字体。
              fontFamily: 'var(--font-subtitle)',
              // 上下间距:跟主数字之间留 ~6px,不挨太近也不脱节
              marginTop: bCritical ? '6px' : '4px',
              // 颜色跟主数字同元素色
              color,
              // 暗色多层 shadow —— 飘字背景是世界场景,可能撞到亮草地 / 反光地砖 / 火光,
              // 双层暗影把字符跟背景剥离开。1px 紧 shadow 给硬边,4px 软 shadow 给"浮起来"
              // 的对比层。不用 outline / -webkit-text-stroke 因为 Ultralight 对 stroke
              // 渲染粗细不稳。
              textShadow: '0 1px 2px rgba(0,0,0,0.85), 0 0 4px rgba(0,0,0,0.65)',
            }}
          >
            {flagLabel}
          </span>
        )}
      </div>
    </motion.div>
    </div>
  );
}
