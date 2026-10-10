/**
 * 技能图标注册表 v3 —— **版画墨章(woodcut stamp)**:实心剪影 + 负空间镂刻 + 淡墨二印。
 *
 * 设计语言(整套图标共享,新增图标请遵守):
 *
 *   1. **墨层(Ink)**:fill=currentColor 的实心剪影 —— 云、雷霆、火焰、步枪、日轮…
 *      直白象形,靠"形"而不是"线"表意。小尺寸下实心形状的辨识度天生强于线稿。
 *      内部细节用**负空间**刻出(fillRule=evenodd 的镂空子路径:火焰的内焰、日轮里的
 *      眼睛、创生之星中央的真空)—— 木刻版画的"留白即笔触"。
 *   2. **淡墨层(Wash,opacity 0.38)**:第二次过版的浅印 —— 烟雾的墨晕、劈斩的轨迹、
 *      滚雷的回响、光晕的环。氛围与运动感都放这层,不和主形争抢注意力。
 *
 *   - 24×24 viewBox;无外框(卡片本身就是画框,剪影自己呼吸)。
 *   - 颜色单一 currentColor,靠 fill / opacity 分层 —— 卡片三态(金亮/羊皮/置灰)自动继承。
 *   - 淡墨层里的描边控制在 0.9,实心形不受 Ultralight 上采样变粗的影响(这正是选
 *     剪影路线的技术红利,2026-07-09 线稿版实测会被上采样加粗)。
 *   - 实心小圆点(瞳孔 / 彗核 / 剑柄)每图至多两处。
 *
 * Key = `UAbilityDefinition::IconName`(FName,kebab-case),由 BattleUIDispatcher 随
 * abilities payload 的 `icon` 字段下发。未命中注册表 → 默认纹章(DefaultSigil)。
 * variant 的 icon 为空时,调用方(VariantCard)先回退父技能图标再回退纹章。
 *
 * 后续若切换到 PNG 美术资源:保持本文件的 key 与 `AbilityIcon` 签名不变,把对应
 * 条目的实现换成 <img src=...> 即可,资产端 IconName 与 call site 都不用动。
 */

import type { FC, ReactNode } from 'react';

export type AbilityIconGlyph = FC<{ className?: string }>;

/** 共享 SVG 外壳 —— 默认实心墨层参数(fill=currentColor,无描边)。 */
function Plate({ children, className }: { children: ReactNode; className?: string }) {
  return (
    <svg
      viewBox="0 0 24 24"
      fill="currentColor"
      stroke="none"
      className={className}
      aria-hidden
    >
      {children}
    </svg>
  );
}

/** 淡墨二印层 —— 0.45 透明度的浅印(0.38 在游戏里偏闷,提亮一档)。子元素自带 fill / stroke 声明。 */
function Wash({ children }: { children: ReactNode }) {
  return <g opacity={0.45}>{children}</g>;
}

/** 淡墨描边的公共 props(轨迹 / 回响 / 环这类线性淡印用)。 */
const washStroke = {
  fill: 'none',
  stroke: 'currentColor',
  strokeWidth: 0.9,
  strokeLinecap: 'round',
  strokeLinejoin: 'round',
} as const;

/** 实心雨滴(顶点朝上的泪滴)。 */
function Drop({ x, y }: { x: number; y: number }) {
  return (
    <path d={`M${x} ${y} c-.85 1.5 -1.35 2.3 -1.35 3.05 a1.35 1.35 0 0 0 2.7 0 c0 -.75 -.5 -1.55 -1.35 -3.05 Z`} />
  );
}

/** 三张祈雨系图标共用的实心云拱。 */
const CLOUD = 'M6.2 13.5 A3 3 0 0 1 7.1 7.9 A4.4 4.4 0 0 1 15.6 6.9 A3.7 3.7 0 0 1 18.8 13.5 Z';

/**
 * 单子(Monad)—— **世界观固定符号,不可自造变体**:圆点居中 + 同心环(circumpunct)。
 * 任何图标里出现"单子"概念(单子囚笼 / 后续单子系技能)一律用这个组件表示。
 * r 是外环半径;环厚与瞳点按比例缩放,r < 2 时环会糊,别在小于 2 的场合引用。
 */
function Monad({ cx, cy, r }: { cx: number; cy: number; r: number }) {
  const inner = r * 0.62;
  return (
    <>
      <path
        fillRule="evenodd"
        d={`M${cx} ${cy - r} a${r} ${r} 0 1 0 .01 0 Z
            M${cx} ${cy - inner} a${inner} ${inner} 0 1 1 -.01 0 Z`}
      />
      <circle cx={cx} cy={cy} r={r * 0.34} />
    </>
  );
}

// ─── 图标 ─────────────────────────────────────────────────────────

/** 祈雨术 —— 墨:云拱 + 三滴坠雨;淡墨:地面涟漪。 */
const Rainmaking: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d={CLOUD} />
    <Drop x={8.6} y={14.6} />
    <Drop x={12} y={15.4} />
    <Drop x={15.4} y={14.6} />
    <Wash>
      <path {...washStroke} d="M7 21.4 q2.4 1.3 4.6 0 M12.6 22.2 q2.4 1.3 4.6 0" />
    </Wash>
  </Plate>
);

/** 纾困祈雨术(变体)—— 墨:云拱 + 垂直细雨;淡墨:风平浪静的水平面。 */
const RainSoothe: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d={CLOUD} />
    <g {...washStroke} opacity={1} strokeWidth={1.4}>
      <path d="M8.6 15.2 v3 M12 16 v3.4 M15.4 15.2 v3" />
    </g>
    <Wash>
      <path {...washStroke} d="M6.8 21.6 Q9.4 20.6 12 21.6 T17.2 21.6" />
    </Wash>
  </Plate>
);

/** 急迫祈雨术(变体)—— 墨:云拱坠下实心雷霆;淡墨:两侧斜雨。 */
const RainUrgent: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d={CLOUD} />
    <path d="M13.4 13.8 L10.6 18 H12.8 L11.2 21.8 L16 16.2 H13.6 L14.8 13.8 Z" />
    <Wash>
      <path {...washStroke} d="M8.4 15.4 l-1.3 2.8 M17 15.4 l-1.3 2.8" />
    </Wash>
  </Plate>
);

/** 祛云术(变体)—— 墨:放射日轮;淡墨:正在消散的残云。 */
const SkyClear: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <circle cx={14.8} cy={9.6} r={3.1} />
    <g {...washStroke} opacity={1} strokeWidth={1.4}>
      <path d="M14.8 4.6 v-1.8 M18.7 6.1 l1.3 -1.3 M20.2 9.6 h1.8 M18.7 13.1 l1.3 1.3 M10.9 6.1 L9.6 4.8" />
    </g>
    <Wash>
      <path fill="currentColor" d="M4.2 17.4 A2.5 2.5 0 0 1 5 12.6 A3.5 3.5 0 0 1 11.6 11.7 A2.9 2.9 0 0 1 14.2 17.4 Z" />
      <path {...washStroke} d="M4.6 20 h2.6 M8.4 21.6 h2" />
    </Wash>
  </Plate>
);

/** 雷声滚滚 —— 墨:完整的实心雷霆;淡墨:两侧滚雷回响弧。 */
const Thunder: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M13.9 2.8 L6.9 13.2 H10.7 L8.6 21.4 L17.4 10.4 H13.4 L16.4 2.8 Z" />
    <Wash>
      <path {...washStroke} d="M4 14.6 a8.4 8.4 0 0 0 2 4 M20 14.6 a8.4 8.4 0 0 1 -2 4" />
    </Wash>
  </Plate>
);

/** 火球术 —— 墨:爆轰(不规则锯齿爆裂星,白热核心以负空间镂出)+ 两粒飞溅火星;
    淡墨:扩散中的冲击波断环。这批美术是**爆燃**且杀伤范围大,图标要有相称的冲击力 ——
    尖峰长短刻意不均,violence 靠"不规则"传达,规整的星形反而温顺。 */
const Fireball: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M12 2 L13.7 7.9 L17.7 6.3 L15.9 10.4 L21.5 12 L16.1 13.5 L18.1 17.1 L13.8 15.8 L12 21.8 L10.3 16.1 L6.6 17.4 L8.1 13.6 L2.8 12 L7.9 10.3 L6.5 6.5 L10.4 8.1 Z
         M12 9.4 a2.6 2.6 0 1 0 0 5.2 a2.6 2.6 0 1 0 0 -5.2 Z"
    />
    <circle cx={19.8} cy={4.4} r={0.7} />
    <circle cx={4.4} cy={18.8} r={0.6} />
    <Wash>
      <path {...washStroke} d="M12 1 A11 11 0 0 1 23 12 M12 23 A11 11 0 0 1 1 12" />
    </Wash>
  </Plate>
);

/** 北苏格兰高地火球术(变体)—— 墨:实心彗核 + 收束的拖尾撇锋;淡墨:第二道浅拖尾。 */
const FireballHighland: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <circle cx={16.4} cy={7.6} r={3.6} />
    <path d="M13.3 9.9 C9.6 11.9 6.6 14.8 3.8 19 C7.6 16.4 11.2 14.4 15.2 13.1 C14.4 12.2 13.7 11.1 13.3 9.9 Z" />
    <Wash>
      <path {...washStroke} d="M12.4 6.4 C9.8 7.2 7.4 8.4 5.2 10 M16.2 12.9 C14.2 15 12.6 17.2 11.3 19.8" />
    </Wash>
  </Plate>
);

/** 卡珊德拉心灵之火(变体)—— 墨:实心眼(负空间虹膜 + 悬浮瞳点)上方燃起跳动的火舌;
    淡墨:灵感刻线。火苗刻意**不对称**:左侧小舌内折出缺口、主尖右倾、再加一缕脱离的
    侧焰 —— 光滑对称的泪滴轮廓会被误读成水滴(2026-07-09 美术反馈),脱离的小火苗是
    最强的"火"信号。 */
const FireMind: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M9.9 8.9 C9.4 7 10.1 5.6 11 4.6 C11.1 5.5 11.4 6.1 12 6.5 C11.8 4.9 12.3 3.4 13.5 2.2 C13.5 3.9 14.2 4.9 14.7 6 C15.2 7.2 15 8.3 14.4 9.1 C13.1 10.1 10.7 10 9.9 8.9 Z" />
    <path d="M15.9 5.6 C16.5 6.5 16.6 7.5 16 8.5 C15.3 7.9 15.1 6.9 15.3 5.9 Z" />
    <path
      fillRule="evenodd"
      d="M6.4 15.4 Q12 11.4 17.6 15.4 Q12 19.4 6.4 15.4 Z
         M12 13.5 a1.9 1.9 0 1 0 0 3.8 a1.9 1.9 0 1 0 0 -3.8 Z"
    />
    <circle cx={12} cy={15.4} r={0.8} />
    <Wash>
      <path {...washStroke} d="M7.2 10.8 l1.7 1.1 M17 11 l-1.6 1" />
    </Wash>
  </Plate>
);

/** 以太创生 —— 墨:八芒创生之星,中央真空以负空间镂出;淡墨:虚线轨道环。 */
const AetherGenesis: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M21 12 L15.4 13.4 L18.4 18.4 L13.4 15.4 L12 21 L10.6 15.4 L5.6 18.4 L8.6 13.4 L3 12 L8.6 10.6 L5.6 5.6 L10.6 8.6 L12 3 L13.4 8.6 L18.4 5.6 L15.4 10.6 Z
         M12 9.8 a2.2 2.2 0 1 0 0 4.4 a2.2 2.2 0 1 0 0 -4.4 Z"
    />
    <Wash>
      <circle {...washStroke} cx={12} cy={12} r={10.6} strokeDasharray="1.7 2.8" />
    </Wash>
  </Plate>
);

/** V&C "主教座堂" 杠杆步枪 —— 墨:整支步枪的实心剪影(枪管条 + 机匣块 + 斜削枪托)+
    杠杆护圈;淡墨:哥特尖拱窗封印(Cathedral 铭牌之意)。 */
const CathedralRifle: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M2.6 10.2 H14 V9.5 H17.7 L22.8 14.6 L20.4 16.9 L17 12.7 H14 V11.4 H2.6 Z" />
    <path d="M4.4 10.2 V9 H5.4 V10.2 Z" />
    <path fill="none" stroke="currentColor" strokeWidth={1.2} d="M17.7 9.5 L18.9 8.1" strokeLinecap="round" />
    <circle fill="none" stroke="currentColor" strokeWidth={1.2} cx={15.5} cy={14.8} r={2} />
    <Wash>
      <path {...washStroke} d="M6.6 20.6 v-1.9 q0 -1.6 1.2 -2.2 q1.2 .6 1.2 2.2 v1.9 z" />
    </Wash>
  </Plate>
);

/** 声子势井 —— 墨:中央实心揭示之眼(负空间瞳孔)+ 共振环;淡墨:外圈断弧与向心坍缩线。 */
const PhononWell: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M8.6 12 Q12 9.6 15.4 12 Q12 14.4 8.6 12 Z
         M12 11 a1 1 0 1 0 0 2 a1 1 0 1 0 0 -2 Z"
    />
    <circle fill="none" stroke="currentColor" strokeWidth={1.2} cx={12} cy={12} r={4.7} />
    <Wash>
      <path {...washStroke} d="M12 4.8 A7.2 7.2 0 0 1 19.2 12 M12 19.2 A7.2 7.2 0 0 1 4.8 12" />
      <path {...washStroke} d="M12 1.9 A10.1 10.1 0 0 0 1.9 12 M12 22.1 A10.1 10.1 0 0 0 22.1 12" />
      <path {...washStroke} d="M6.3 6.3 L7.8 7.8 M17.7 6.3 L16.2 7.8 M6.3 17.7 L7.8 16.2 M17.7 17.7 L16.2 16.2" />
    </Wash>
  </Plate>
);

/** 烟雾弹 —— 墨:实心弹体 + 顶盖;淡墨:整团墨晕状的烟云(实心浅印)+ 引烟。 */
const SmokeGrenade: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <circle cx={10.5} cy={16} r={4.7} />
    <path d="M8.9 11.2 h3.2 v-1.6 h-3.2 Z" />
    <Wash>
      <path fill="currentColor" d="M13.9 10.2 A2.6 2.6 0 0 1 15.2 5.6 A3.1 3.1 0 0 1 21.1 6.3 A2.4 2.4 0 0 1 20.1 10.2 Z" />
      <circle fill="currentColor" cx={13.4} cy={3.9} r={1.2} />
      <path {...washStroke} d="M10.5 9.6 C10.7 8.2 11.6 7.2 12.9 6.6" />
    </Wash>
  </Plate>
);

/** 活体光晕 —— 墨:实心日轮,全视之眼以负空间镂出(瞳点悬浮)+ 八向实心光锋。 */
const LivingGlare: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M12 7.1 a4.9 4.9 0 1 0 0.01 0 Z
         M8.6 12 Q12 9.6 15.4 12 Q12 14.4 8.6 12 Z"
    />
    <circle cx={12} cy={12} r={1} />
    <path d="M12 2.6 L13 5.2 H11 Z M12 21.4 L13 18.8 H11 Z M2.6 12 L5.2 11 V13 Z M21.4 12 L18.8 11 V13 Z" />
    <path d="M18.6 5.4 L17.9 7.3 L16.7 6.1 Z M5.4 5.4 L6.1 7.3 L7.3 6.1 Z M18.6 18.6 L17.9 16.7 L16.7 17.9 Z M5.4 18.6 L6.1 16.7 L7.3 17.9 Z" />
    <Wash>
      <circle {...washStroke} cx={12} cy={12} r={10} />
    </Wash>
  </Plate>
);

/** 横扫 —— 墨:弯刀刃身的实心撇锋 + 护手与柄;淡墨:虚线劈斩轨迹。 */
const Sweep: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M5.2 18.8 C8.6 16.6 13 12.4 16.9 7 C17.5 6.2 18.3 5.2 19 4.4 C18.6 5.6 18.2 6.6 17.6 7.7 C14.4 13.2 10 17.4 5.9 19.6 Z" />
    <path d="M7.9 15.5 L10.5 18.1 L9.7 18.9 L7.1 16.3 Z" />
    <path d="M8 16.6 L9.4 18 L7.3 20.1 L5.9 18.7 Z" />
    <circle cx={5.6} cy={20.4} r={1} />
    <Wash>
      <path {...washStroke} d="M19.8 5.6 A11.4 11.4 0 0 1 14 20.4" strokeDasharray="2.4 2" />
    </Wash>
  </Plate>
);

// ─── 步枪弹药族(V&C 杠杆步枪的 variants)────────────────────────
//
// 三种弹药共用"竖立枪弹(弹头 + 弹壳,负空间留装配缝)"母题成一族,靠**弹头处理**区分;
// 再装填以环形箭头为主形。弹壳统一:M9.4 13.2 → 20.4 圆角底 + 底缘横条。

/** 弹壳(三种弹药共用):圆角底的直壳 + 底缘。 */
function CartridgeCase() {
  return (
    <>
      <path d="M9.4 13.2 H14.6 V19.4 Q14.6 20.3 13.7 20.3 H10.3 Q9.4 20.3 9.4 19.4 Z" />
      <path d="M9 21.2 h6 v1.1 h-6 Z" />
    </>
  );
}

/** 全金属被甲弹(步枪变体)—— 墨:尖头枪弹;淡墨:被射线洞穿的装甲板(断线)+ 崩落火星。 */
const AmmoFmj: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M9.4 12.4 C9.4 8.6 10.6 5.9 12 3.6 C13.4 5.9 14.6 8.6 14.6 12.4 Z" />
    <CartridgeCase />
    <Wash>
      <path {...washStroke} d="M4.2 5.8 H8.6 M15.4 5.8 H19.8" />
      <path {...washStroke} d="M8.9 4.2 l-1.2 -1.1 M15.1 4.2 l1.2 -1.1" />
    </Wash>
  </Plate>
);

/** 再装填(步枪变体)—— 墨:环形装填箭头 + 中央竖立弹药;淡墨:第二道浅弧。 */
const Reload: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fill="none"
      stroke="currentColor"
      strokeWidth={1.4}
      strokeLinecap="round"
      d="M12 4.5 A7.5 7.5 0 1 1 4.5 12"
    />
    <path d="M12 2.7 L15.2 4.5 L12 6.3 Z" />
    <path d="M10.6 10.8 C10.6 8.9 11.3 7.7 12 6.9 C12.7 7.7 13.4 8.9 13.4 10.8 Z" />
    <path d="M10.6 11.4 H13.4 V16 Q13.4 16.8 12.7 16.8 H11.3 Q10.6 16.8 10.6 16 Z" />
    <Wash>
      <path {...washStroke} d="M5.6 8.2 A7.5 7.5 0 0 1 8.5 5.3" />
    </Wash>
  </Plate>
);

/** 十字空尖弹(步枪变体)—— 墨:平头弹,尖端以负空间镂出十字空腔;淡墨:扩张阻滞刻线。 */
const AmmoHollowpoint: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M9.4 12.4 V8.8 Q9.4 6.2 10.7 5.2 H13.3 Q14.6 6.2 14.6 8.8 V12.4 Z
         M11.5 6 h1 v3.8 h-1 Z
         M10.1 7.4 h3.8 v1 h-3.8 Z"
    />
    <CartridgeCase />
    <Wash>
      <path {...washStroke} d="M7.6 4.6 l-1.5 -1.3 M16.4 4.6 l1.5 -1.3 M6.4 8 l-1.8 -.4 M17.6 8 l1.8 -.4" />
    </Wash>
  </Plate>
);

/** 神经毒素弹(步枪变体)—— 墨:尖头弹,弹体以负空间镂出毒滴;淡墨:两侧垂坠的滴液纹。 */
const AmmoNeurotoxin: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M9.4 12.4 C9.4 8.6 10.6 5.9 12 3.6 C13.4 5.9 14.6 8.6 14.6 12.4 Z
         M12 7.4 c-.7 1.2 -1.1 1.9 -1.1 2.55 a1.1 1.1 0 0 0 2.2 0 c0 -.65 -.4 -1.35 -1.1 -2.55 Z"
    />
    <CartridgeCase />
    <Wash>
      <path {...washStroke} d="M6.8 12.6 q-.8 1.3 0 2.6 M17.2 12.6 q.8 1.3 0 2.6" />
      <circle fill="currentColor" cx={6.8} cy={17} r={0.7} />
      <circle fill="currentColor" cx={17.2} cy={17} r={0.7} />
    </Wash>
  </Plate>
);

/** 燃烧弹 —— 墨:落地火海(池面 + 三簇不对称火舌)+ 坠落中的弹体;淡墨:投掷轨迹与池沿光晕。 */
const Firebomb: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <ellipse cx={11.4} cy={19} rx={7.8} ry={2.1} />
    <path d="M6.9 18.6 C6.5 17 7.1 15.8 8.1 14.9 C8.3 16 8.9 16.8 8.9 17.9 Z" />
    <path d="M10.6 18.4 C10 15.8 10.7 13.4 12.1 11.6 C12.2 13.4 13.1 14.6 13 16.4 C12.9 17.4 12.1 18.4 10.6 18.4 Z" />
    <path d="M14.8 18.6 C14.4 16.8 15 15.4 16.2 14.2 C16.2 15.6 16.8 16.4 16.6 17.8 Z" />
    <circle cx={16.6} cy={6.4} r={1.7} />
    <path d="M17.5 5 l1.1 -1.2" fill="none" stroke="currentColor" strokeWidth={1.2} strokeLinecap="round" />
    <Wash>
      <path {...washStroke} d="M3 3.6 Q9.4 2.2 14.9 6.2" strokeDasharray="2.2 1.8" />
      <path {...washStroke} d="M2.6 19 a8.9 3 0 0 0 8.8 2.4" />
    </Wash>
  </Plate>
);

/** Anvil "战壕清道夫" 12 号口径 —— 墨:与步枪同一"长枪侧影"语法(枪口朝左、枪托后掠),
    霰弹枪辨识签名 = 枪管下并行的弹仓管 + 包住它的粗泵动前托;扳机护圈负空间环。
    淡墨:铁砧厂徽(呼应品牌名,同步枪的尖拱窗封印)。 */
const AnvilShotgun: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M1.8 7.6 H14.4 V9.2 H1.8 Z" />
    <path d="M2.6 10 H12.8 V11.3 H2.6 Z" />
    <path d="M4.6 9.6 H9.4 V12.6 Q9.4 13.4 8.6 13.4 H5.4 Q4.6 13.4 4.6 12.6 Z" />
    <path d="M14.4 7 H17.8 V12 H14.4 Z" />
    <path d="M17.6 7.4 L22.2 9.4 V12.4 L19.8 13.2 L17.6 11.6 Z" />
    <path
      fillRule="evenodd"
      d="M16 12.2 a1.7 1.7 0 1 0 .01 0 Z
         M16 13.3 a.62 .62 0 1 1 -.01 0 Z"
    />
    <Wash>
      <path fill="currentColor" d="M3.6 16.6 H9.4 L8.3 17.9 H7.1 L7.8 19.4 H4.9 L5.6 17.9 H4.6 Z" />
    </Wash>
  </Plate>
);

// ─── 霰弹枪弹头族(Anvil 战壕清道夫的 variants)──────────────────
//
// 与步枪的尖头铜壳弹拉开:共用"平顶圆柱弹壳 + 铜底座(负空间底火)"母题,
// 膛体上按弹种镂刻 —— 火焰 / 独头弹丸 / 哨眼。

/** 霰弹壳(三种弹头共用):平顶膛体在各图标内单独画(要挖不同的负空间),这里是铜底座。 */
function ShellBase() {
  return (
    <path
      fillRule="evenodd"
      d="M8.8 16.8 H15.2 V19.4 H8.8 Z
         M12 17.4 a.7 .7 0 1 0 0 1.4 a.7 .7 0 1 0 0 -1.4 Z"
    />
  );
}

/** 高爆烧夷弹(霰弹变体)—— 墨:弹壳膛体镂刻火焰;淡墨:锥形扩散线(锥形范围杀伤)。 */
const ShellIncendiary: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M9.2 5.4 H14.8 V16.2 H9.2 Z
         M12 7.2 C10.9 8.8 10.6 10.2 11.3 11.6 C11.7 12.4 12.7 12.4 13.1 11.5 C13.7 10.1 13.2 8.6 12 7.2 Z"
    />
    <ShellBase />
    <Wash>
      <path {...washStroke} d="M9.8 4.4 L7 1.8 M12 4.2 V1.2 M14.2 4.4 L17 1.8" />
    </Wash>
  </Plate>
);

/** 独头弹(霰弹变体)—— 墨:弹壳膛体镂刻单颗独头弹丸;淡墨:向上的远距直射线。 */
const ShellSlug: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M9.2 5.4 H14.8 V16.2 H9.2 Z
         M12 6.8 a2.1 2.6 0 1 0 0 5.2 a2.1 2.6 0 1 0 0 -5.2 Z"
    />
    <ShellBase />
    <Wash>
      <path {...washStroke} d="M12 4.2 V1 M10.9 2.1 L12 .9 L13.1 2.1" />
    </Wash>
  </Plate>
);

/** 心灵哨弹(霰弹变体)—— 墨:弹壳膛体镂刻哨眼(揭示语义)+ 悬浮瞳点;淡墨:哨戒 ping 弧。 */
const ShellPsychic: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M9.2 5.4 H14.8 V16.2 H9.2 Z
         M9.7 9.6 Q12 7.9 14.3 9.6 Q12 11.3 9.7 9.6 Z"
    />
    <circle cx={12} cy={9.6} r={0.7} />
    <ShellBase />
    <Wash>
      <path {...washStroke} d="M8.4 3.6 A5 5 0 0 1 15.6 3.6" />
      <path {...washStroke} d="M10 4.7 A2.9 2.9 0 0 1 14 4.7" />
    </Wash>
  </Plate>
);

/** 以太光矛 —— 墨:斜贯的引导光束 + 命中处不规则爆裂;淡墨:被洞穿的装甲板断线
    (反重甲语义,同全金属被甲弹的板线 idiom)+ 源头辉光刻线。 */
const AetherLance: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M3.2 4.8 L4.8 3.2 L18.6 13.4 L16.9 15.1 Z" />
    <path d="M19.2 12.4 L20 14.4 L22.2 14.2 L20.6 15.7 L21.8 17.6 L19.6 16.7 L18.5 18.7 L18.3 16.5 L16.2 16 L18.1 15 Z" />
    <Wash>
      <path {...washStroke} d="M13.8 20.6 H16.8 M21.4 20.6 H23.2" />
      <path {...washStroke} d="M2.2 7.2 l1.4 -.5 M6.9 1.7 l-.5 1.4" />
    </Wash>
  </Plate>
);

/** 荆棘术 —— 墨:环形荆棘缠索(束缚语义),外刺不对称、内刺伤及被缠者;淡墨:抽出的卷须。 */
const Thorns: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M12 5.8 a7.2 7.2 0 1 0 .01 0 Z
         M12 8.4 a4.6 4.6 0 1 1 -.01 0 Z"
    />
    <path d="M10.8 6.4 L12 3.3 L13.2 6.4 Z" />
    <path d="M16.4 7.6 L19.3 5.9 L18.1 9.1 Z" />
    <path d="M18.9 12 L22 13 L18.9 14.2 Z" />
    <path d="M16.6 18.3 L18.9 20.7 L15.5 19.9 Z" />
    <path d="M7.3 18.2 L5.2 20.7 L8.5 19.8 Z" />
    <path d="M5.1 12.2 L2 13.2 L5.1 14.3 Z" />
    <path d="M11 8.7 L12 10.9 L13 8.7 Z" />
    <path d="M16.3 12.2 L14.2 13.1 L16.3 14 Z" />
    <Wash>
      <path {...washStroke} d="M6.2 6.4 C5.2 5.2 5.4 4 6.6 3.4" />
      <path {...washStroke} d="M18.6 20 C19.9 20.7 21 20.2 21.3 19" />
    </Wash>
  </Plate>
);

/** 单子囚笼 —— 非拟物:球形反心灵力场。墨:边界厚环 + 赤道椭圆(圆升维成球的语法)
    + 四方向内压楔(场向内收束 = 囚禁),中心是单子固定符号;淡墨:被场偏折的心灵波纹。 */
const MonadCage: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M12 3.4 a8.6 8.6 0 1 0 .01 0 Z
         M12 4.9 a7.1 7.1 0 1 1 -.01 0 Z"
    />
    {/* 球体线格:竖向经线 + 下前方纬线弧(赤道横穿中心会跟圆心单子拼成"眼睛",禁) */}
    <ellipse cx={12} cy={12} rx={3.4} ry={8.5} fill="none" stroke="currentColor" strokeWidth={0.9} />
    <path fill="none" stroke="currentColor" strokeWidth={0.9} strokeLinecap="round"
      d="M4.5 13.4 A 8.4 4.4 0 0 0 19.5 13.4" />
    <path d="M11.2 4.9 H12.8 L12 6.7 Z" />
    <path d="M19.1 11.2 V12.8 L17.3 12 Z" />
    <path d="M12.8 19.1 H11.2 L12 17.3 Z" />
    <path d="M4.9 12.8 V11.2 L6.7 12 Z" />
    <Monad cx={12} cy={12} r={2.7} />
    <Wash>
      <path {...washStroke} d="M1.4 8.2 C2.6 9 2.8 10.4 1.8 11.6" />
      <path {...washStroke} d="M22.6 8.2 C21.4 9 21.2 10.4 22.2 11.6" />
    </Wash>
  </Plate>
);

/** 王车易位 —— 墨:车(城齿塔)与王(十字冠)对置错位 + 双向交换弧箭(换位语义);
    淡墨:对角棋盘格。 */
const Castling: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    {/* 车(rook)—— 左下 */}
    <path d="M3.8 8.8 h1.3 v1.5 h1 v-1.5 h1.3 v1.5 h1 v-1.5 h1.3 v3.1 h-5.9 Z" />
    <path d="M4.6 11.9 h4.3 l.5 4.6 h-5.3 Z" />
    <path d="M3.9 16.5 h5.7 v2 h-5.7 Z" />
    {/* 王(king)—— 右上,十字冠 */}
    <path d="M16.9 3 h1.4 v3.2 h-1.4 Z" />
    <path d="M15.9 3.9 h3.4 v1.2 h-3.4 Z" />
    <circle cx={17.6} cy={7.7} r={1.5} />
    <path d="M16.4 9.1 h2.4 l.7 5.3 h-3.8 Z" />
    <path d="M15.5 14.4 h4.2 v1.9 h-4.2 Z" />
    {/* 交换弧箭:上弧指向王,下弧指向车 */}
    <path fill="none" stroke="currentColor" strokeWidth={1} strokeLinecap="round"
      d="M6.4 6.6 Q9.2 2.8 13 3.4" />
    <path d="M12.8 2.5 L14.9 3.6 L12.7 4.4 Z" />
    <path fill="none" stroke="currentColor" strokeWidth={1} strokeLinecap="round"
      d="M17.5 17.5 Q14.6 20.8 11 20" />
    <path d="M11.3 19 L9.1 19.8 L11.2 20.8 Z" />
    <Wash>
      <path fill="currentColor" d="M11.6 16.4 h1.7 v1.7 h-1.7 Z M13.3 14.7 h1.7 v1.7 h-1.7 Z" />
    </Wash>
  </Plate>
);

/** 哨兵使魔 —— 墨:上空翱翔的使魔鸟(海燕式 M 形双翼 + 垂身楔尾)+ 下方大哨眼
    (实心杏眼 + 负空间虹膜 + 悬浮瞳点,同卡珊德拉心灵之火的眼语法 —— 共享视野属揭示族);
    淡墨:自翼下汇入眼睛的两道虚线视线束 —— 它的所见即你的所见。 */
const SentinelFamiliar: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M12 8.6 C10.8 6.9 8.9 5.6 6.2 5 C7 6.8 8.3 8.3 10.1 9.4 C10.8 9.8 11.4 9.9 12 9.8 C12.6 9.9 13.2 9.8 13.9 9.4 C15.7 8.3 17 6.8 17.8 5 C15.1 5.6 13.2 6.9 12 8.6 Z" />
    <path d="M12 8.8 C12.7 9.6 13 10.5 12.9 11.6 L12 13.2 L11.1 11.6 C11 10.5 11.3 9.6 12 8.8 Z" />
    <path
      fillRule="evenodd"
      d="M6.8 16.4 Q12 12.9 17.2 16.4 Q12 19.9 6.8 16.4 Z
         M12 14.55 a1.85 1.85 0 1 0 .01 0 Z"
    />
    <circle cx={12} cy={16.4} r={0.8} />
    <Wash>
      <path {...washStroke} strokeDasharray="1.6 1.5" d="M8.4 9.2 L7.4 13.4 M15.6 9.2 L16.6 13.4" />
    </Wash>
  </Plate>
);

/** 昭昭天命 —— 墨:实心冠冕(负空间束带缝 + 冠心宝珠)+ 五向实心光锋(LivingGlare 光锋
    语法)—— 冠 = 天命,放射 = 昭昭其明,也是"向所有目标揭示自己"的代价写照;
    淡墨:虚线世界环,光锋恰好触环 —— 光芒抵达所有人。 */
const ManifestDestiny: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M7.4 16.6 V10 L10.1 12.4 L12 8.4 L13.9 12.4 L16.6 10 V16.6 Z
         M8.3 14.5 H15.7 V15.3 H8.3 Z"
    />
    <circle cx={12} cy={13.2} r={0.8} />
    <path d="M12 2.2 L13 4.9 H11 Z" />
    <path d="M18.9 4.2 L17.9 6.7 L16.2 5 Z" />
    <path d="M5.1 4.2 L6.1 6.7 L7.8 5 Z" />
    <path d="M21.6 11.6 L18.9 10.7 V12.5 Z" />
    <path d="M2.4 11.6 L5.1 10.7 V12.5 Z" />
    <Wash>
      <circle {...washStroke} cx={12} cy={11.8} r={9.7} strokeDasharray="1.7 2.8" />
    </Wash>
  </Plate>
);

/** 双生飞弹 —— 墨:一对自两侧下角向顶心收束的粗体飞镖(三角镖头 + 宽杆),左镖实心
    (黯蚀),右镖空心描边(光耀)—— 明暗孪生;镖尖几乎相触 = 命中同一目标(目盲的
    达成条件),相触点上方一点细十字爆花;淡墨:尾端拖速线。 */
const TwinBolts: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path d="M11.2 4.4 L11.5 8.6 L8.3 7.2 Z" />
    <path d="M4 19 L9 7.5 L10.8 8.3 L5.8 19.8 Z" />
    <path
      fill="none"
      stroke="currentColor"
      strokeWidth={1.1}
      strokeLinejoin="round"
      d="M12.8 4.4 L15.7 7.2 L15 7.5 L20 19 L18.2 19.8 L13.2 8.3 L12.5 8.6 Z"
    />
    <path fill="none" stroke="currentColor" strokeWidth={0.9} strokeLinecap="round" d="M12 1.2 V2.9 M11.15 2.05 H12.85" />
    <Wash>
      <path {...washStroke} d="M5.4 21.2 l-.6 1.6 M18.6 21.2 l.6 1.6" />
    </Wash>
  </Plate>
);

// ─── 沉默族(沉默术及其 variants)──────────────────────────────────
//
// 族徽 = **钟**(贝尔失语术的 Bell 双关顺势成全族母题):实心钟身 + 分缝钟唇。
// "沉默"统一用两个记号表达 —— 钟舌脱落(敲不响)与声弧被切断/压平。

/** 沉默术钟身(全族共用):提钮 + 钟肩长裙,钟口平沿。 */
function BellBody() {
  return (
    <>
      <circle cx={12} cy={3} r={1.05} />
      <path d="M12 3.8 C9.2 4.2 7.8 6.7 7.8 10 C7.8 12 7.2 13.3 6.2 14.3 H17.8 C16.8 13.3 16.2 12 16.2 10 C16.2 6.7 14.8 4.2 12 3.8 Z" />
    </>
  );
}

/** 沉默术 —— 墨:钟 + 分缝钟唇,钟舌**脱落坠地**(敲不响的钟);淡墨:两侧被掐断的
    声弧残段 + 底部一道平直的无声线。 */
const Silence: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <BellBody />
    <path d="M5.8 15.1 H18.2 V16.2 H5.8 Z" />
    <circle cx={12} cy={19} r={1.1} />
    <Wash>
      <path {...washStroke} d="M3.6 8.6 a8.8 8.8 0 0 0 -1 3 M20.4 8.6 a8.8 8.8 0 0 1 1 3" />
      <path {...washStroke} d="M8.6 21.8 h6.8" />
    </Wash>
  </Plate>
);

/** 普利茅茨沉默术(变体)—— 墨:隔音球界(厚环)罩住小钟,钟舌整个不存在(场内出不了声);
    淡墨:界内平直的无声线 + 右上一道**止步于球面**的雷霆折线(雷霆伤害 -100%)。 */
const SilencePlymouth: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M12 3.4 a8.6 8.6 0 1 0 .01 0 Z
         M12 4.9 a7.1 7.1 0 1 1 -.01 0 Z"
    />
    <circle cx={12} cy={6.6} r={0.75} />
    <path d="M12 7.1 C10.1 7.4 9.1 9.1 9.1 11.3 C9.1 12.6 8.7 13.5 8 14.2 H16 C15.3 13.5 14.9 12.6 14.9 11.3 C14.9 9.1 13.9 7.4 12 7.1 Z" />
    <path d="M7.7 14.9 H16.3 V15.7 H7.7 Z" />
    <Wash>
      <path {...washStroke} d="M9.4 17.6 h5.2" />
      <path {...washStroke} d="M22.6 1.4 L20.8 3 L21.7 3.4 L18.6 5.6" />
    </Wash>
  </Plate>
);

/** 贝尔失语术(变体)—— 墨:法球横贯钟身(负空间射孔 + 入射杆与出射锋,穿深 0~120 的
    极穿透),钟舌被击落;淡墨:入射侧速线 + 坠舌下的闷响弧。 */
const SilenceAphasia: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <circle cx={12} cy={3} r={1.05} />
    <path
      fillRule="evenodd"
      d="M12 3.8 C9.2 4.2 7.8 6.7 7.8 10 C7.8 12 7.2 13.3 6.2 14.3 H17.8 C16.8 13.3 16.2 12 16.2 10 C16.2 6.7 14.8 4.2 12 3.8 Z
         M7.8 8.9 H16.2 V10.1 H7.8 Z"
    />
    <path d="M5.8 15.1 H18.2 V16.2 H5.8 Z" />
    <path d="M1.2 8.9 H7.3 V10.1 H1.2 Z" />
    <path d="M16.7 7.6 L21.6 9.5 L16.7 11.4 Z" />
    <circle cx={12} cy={18.8} r={1} />
    <Wash>
      <path {...washStroke} d="M1.6 6.6 h2.4 M1.6 12.4 h2.4" />
      <path {...washStroke} d="M9.8 21.4 q2.2 1 4.4 0" />
    </Wash>
  </Plate>
);

/** 冰霜护盾 —— 墨:实心盾章,负空间镂出六芒霜星;淡墨:右下角一簇舔盾的火舌与热浪
    (>10 火焰伤害即碎的弱点写在图标上)。 */
const FrostShield: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M12 2.8 L19.6 5.2 V11.6 C19.6 16.6 16.6 19.8 12 21.6 C7.4 19.8 4.4 16.6 4.4 11.6 V5.2 Z
         M12 7.2 L11.4 10.4 L8.4 9.3 L10.8 11.4 L8.4 13.5 L11.4 12.4 L12 15.6 L12.6 12.4 L15.6 13.5 L13.2 11.4 L15.6 9.3 L12.6 10.4 Z"
    />
    <Wash>
      <path fill="currentColor" d="M18.2 21.2 C17.8 19.7 18.3 18.5 19.4 17.5 C19.5 18.7 20.1 19.4 20 20.5 Z" />
      <path {...washStroke} d="M16.2 22.4 q1.8 .9 3.8 .5" />
    </Wash>
  </Plate>
);

/** 默认纹章(fallback)—— 墨:实心金字塔,全视之眼以负空间镂出;淡墨:环形轨道。 */
const DefaultSigil: AbilityIconGlyph = ({ className }) => (
  <Plate className={className}>
    <path
      fillRule="evenodd"
      d="M12 4.6 L20.4 19.4 H3.6 Z
         M8.9 15.4 Q12 13.2 15.1 15.4 Q12 17.6 8.9 15.4 Z"
    />
    <circle cx={12} cy={15.4} r={0.8} />
    <Wash>
      <circle {...washStroke} cx={12} cy={13} r={9.4} strokeDasharray="1.7 2.8" />
    </Wash>
  </Plate>
);

// ─── 注册表 ───────────────────────────────────────────────────────
//
// Key 与 `UAbilityDefinition::IconName` 对齐(kebab-case)。批量赋值见
// `Content/Python/assign_ability_icon_names.py`(按 DisplayName 匹配)。

const ABILITY_ICONS: Record<string, AbilityIconGlyph> = {
  'rainmaking': Rainmaking,
  'rain-soothe': RainSoothe,
  'rain-urgent': RainUrgent,
  'sky-clear': SkyClear,
  'thunder': Thunder,
  'fireball': Fireball,
  'fireball-highland': FireballHighland,
  'fire-mind': FireMind,
  'aether-genesis': AetherGenesis,
  'cathedral-rifle': CathedralRifle,
  'ammo-fmj': AmmoFmj,
  'reload': Reload,
  'ammo-hollowpoint': AmmoHollowpoint,
  'ammo-neurotoxin': AmmoNeurotoxin,
  'firebomb': Firebomb,
  'anvil-shotgun': AnvilShotgun,
  'shell-incendiary': ShellIncendiary,
  'shell-slug': ShellSlug,
  'shell-psychic': ShellPsychic,
  'aether-lance': AetherLance,
  'thorns': Thorns,
  'monad-cage': MonadCage,
  // 旧 key 别名(资产改名"心灵囚笼→单子囚笼"过渡期已赋值的 IconName 不断链)
  'mind-cage': MonadCage,
  'castling': Castling,
  'phonon-well': PhononWell,
  'smoke-grenade': SmokeGrenade,
  'living-glare': LivingGlare,
  'sweep': Sweep,
  // 2026-07-16 批次
  'sentinel-familiar': SentinelFamiliar,
  'manifest-destiny': ManifestDestiny,
  'twin-bolts': TwinBolts,
  'silence': Silence,
  'silence-plymouth': SilencePlymouth,
  'silence-aphasia': SilenceAphasia,
  'frost-shield': FrostShield,
};

/**
 * 技能图标组件。回退链:name → fallbackName(variant 用父技能图标)→ 默认纹章。
 * 空串 / undefined / 未注册的 key 一律走回退,不会渲染裂图。
 */
export function AbilityIcon({
  name,
  fallbackName,
  className,
}: {
  name?: string;
  fallbackName?: string;
  className?: string;
}) {
  const Glyph =
    (name ? ABILITY_ICONS[name] : undefined) ??
    (fallbackName ? ABILITY_ICONS[fallbackName] : undefined) ??
    DefaultSigil;
  return <Glyph className={className} />;
}
