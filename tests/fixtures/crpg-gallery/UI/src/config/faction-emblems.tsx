/**
 * 阵营纹章注册表 —— emblemId(来自 C++ UFactionDataSettings.EmblemId,经
 * setBattleSession 推送)→ 雕版风格 SVG 组件。
 *
 * 设计约定:
 *   - UE 只传 id 字符串,**图形资源完全由前端持有与解析**(美术换图只改这里)。
 *   - 纹章是 stroke 线稿,`stroke="currentColor"` —— 由父级 color 染色(阵营主题色 /
 *     黄铜默认),跟战斗 UI 的"细线雕版"语言一致;后续要换位图美术,把对应 entry
 *     的实现换成 <img src={...}> 即可,消费方接口不变。
 *   - 未注册 id / 空 id 走 FACTION_EMBLEM_FALLBACK(印章纹样)。
 *
 * 解析入口统一走 `resolveFaction(tag, directory)` —— 目录里查不到 tag 时降级:
 * 名字用 tag leaf 段,纹章用 fallback,颜色空(默认羊皮纸)。
 */

import type { FC } from 'react';
import type { BattleFactionInfo } from '@/stores/battle-store';

export interface FactionEmblemProps {
  /** 像素尺寸(方形),默认 48 */
  size?: number;
  className?: string;
}

// ─── 月桂环(laurel)—— 玩家方 / 同盟语义 ─────────────────────────
//
// 顶部开口的双枝月桂:两条镜像圆弧 + 沿弧分布的叶片短线(JS 计算位置,免手调
// path)。叶片外/内双向、靠枝端渐短,读作版画里的桂冠。

function LaurelEmblem({ size = 48, className }: FactionEmblemProps) {
  const cx = 16;
  const cy = 17;
  const r = 10;

  // 单侧叶片角度(度,SVG 坐标系 0°=右、90°=下;wreath 开口朝上 = 跳过 -90° 附近)。
  // 从底部(90°)向上到接近顶部(-60°),左侧镜像。
  const leafAngles = [62, 34, 6, -22, -50];

  const leaves: { x1: number; y1: number; x2: number; y2: number; w: number }[] = [];
  for (const side of [-1, 1] as const) {
    leafAngles.forEach((deg, i) => {
      const a = (deg * Math.PI) / 180;
      // side=-1 → 左半(角度镜像到 90°..270° 区间)
      const px = cx + side * r * Math.cos(a);
      const py = cy + r * Math.sin(a);
      // 叶片:从弧上的点向外侧偏切线方向伸出;越靠枝端(i 大)越短
      const len = 4.6 - i * 0.55;
      const outA = a - (side * 38 * Math.PI) / 180;
      leaves.push({
        x1: px,
        y1: py,
        x2: px + side * len * Math.cos(outA),
        y2: py + len * Math.sin(outA),
        w: 1.15,
      });
    });
  }

  // 圆弧端点(底部合拢 / 顶部开口两端)
  const topA = (-58 * Math.PI) / 180;
  const tipRx = cx + r * Math.cos(topA);
  const tipLx = cx - r * Math.cos(topA);
  const tipY = cy + r * Math.sin(topA);

  return (
    <svg width={size} height={size} viewBox="0 0 32 32" fill="none" className={className} aria-hidden>
      {/* 两条枝弧:从底部 90° 到 ±(-58°) */}
      <path
        d={`M ${cx} ${cy + r} A ${r} ${r} 0 0 1 ${tipLx.toFixed(2)} ${tipY.toFixed(2)}`}
        stroke="currentColor" strokeWidth="1.25" strokeLinecap="round"
      />
      <path
        d={`M ${cx} ${cy + r} A ${r} ${r} 0 0 0 ${tipRx.toFixed(2)} ${tipY.toFixed(2)}`}
        stroke="currentColor" strokeWidth="1.25" strokeLinecap="round"
      />
      {leaves.map((l, i) => (
        <path
          key={i}
          d={`M ${l.x1.toFixed(2)} ${l.y1.toFixed(2)} L ${l.x2.toFixed(2)} ${l.y2.toFixed(2)}`}
          stroke="currentColor" strokeWidth={l.w} strokeLinecap="round"
        />
      ))}
      {/* 顶部开口两侧的浆果点 */}
      <circle cx={tipLx.toFixed(2)} cy={(tipY - 1.6).toFixed(2)} r="1" fill="currentColor" />
      <circle cx={tipRx.toFixed(2)} cy={(tipY - 1.6).toFixed(2)} r="1" fill="currentColor" />
    </svg>
  );
}

// ─── 交叉军刀(sabers)—— 敌对 / 军事语义 ────────────────────────

function SabersEmblem({ size = 48, className }: FactionEmblemProps) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" fill="none" className={className} aria-hidden>
      {/* 两道弯刀刃,交叉 */}
      <path d="M 7.5 25.5 Q 13 19 24.5 6.5" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
      <path d="M 24.5 25.5 Q 19 19 7.5 6.5" stroke="currentColor" strokeWidth="1.4" strokeLinecap="round" />
      {/* 护手短杠(各自垂直于刃身近柄处) */}
      <path d="M 8.2 20.6 L 12.4 24.3" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" />
      <path d="M 23.8 20.6 L 19.6 24.3" stroke="currentColor" strokeWidth="1.2" strokeLinecap="round" />
      {/* 柄尾圆头 */}
      <circle cx="6.6" cy="26.4" r="1.2" fill="currentColor" />
      <circle cx="25.4" cy="26.4" r="1.2" fill="currentColor" />
    </svg>
  );
}

// ─── 印章纹样(seal)—— fallback / 中立 ─────────────────────────
//
// 双线菱形 + 角点 —— 呼应战斗面板的双线规 + 角饰语言,作为"未知阵营"的默认徽记。

function SealEmblem({ size = 48, className }: FactionEmblemProps) {
  return (
    <svg width={size} height={size} viewBox="0 0 32 32" fill="none" className={className} aria-hidden>
      <path d="M 16 4 L 28 16 L 16 28 L 4 16 Z" stroke="currentColor" strokeWidth="1.2" strokeLinejoin="round" />
      <path d="M 16 9 L 23 16 L 16 23 L 9 16 Z" stroke="currentColor" strokeWidth="0.9" strokeLinejoin="round" opacity="0.6" />
      <circle cx="16" cy="16" r="1.4" fill="currentColor" />
      {/* 四向小点 —— 罗盘 / 印章味 */}
      <circle cx="16" cy="1.8" r="0.8" fill="currentColor" opacity="0.7" />
      <circle cx="16" cy="30.2" r="0.8" fill="currentColor" opacity="0.7" />
      <circle cx="1.8" cy="16" r="0.8" fill="currentColor" opacity="0.7" />
      <circle cx="30.2" cy="16" r="0.8" fill="currentColor" opacity="0.7" />
    </svg>
  );
}

// ─── 注册表 ───────────────────────────────────────────────────────

export const FACTION_EMBLEMS: Record<string, FC<FactionEmblemProps>> = {
  laurel: LaurelEmblem,
  sabers: SabersEmblem,
  seal: SealEmblem,
};

export const FACTION_EMBLEM_FALLBACK: FC<FactionEmblemProps> = SealEmblem;

export function getFactionEmblem(emblemId: string): FC<FactionEmblemProps> {
  return FACTION_EMBLEMS[emblemId] ?? FACTION_EMBLEM_FALLBACK;
}

// ─── 阵营解析 ─────────────────────────────────────────────────────

export interface ResolvedFaction {
  name: string;
  emblemId: string;
  /** 主题色 hex;空串 = 调用方用默认色(羊皮纸) */
  color: string;
}

/**
 * tag → 可展示阵营数据。目录(store.session.factions)查不到时降级:
 * 名字用 tag 最后一段(`Npc.Test.Group.A` → `A`),纹章走 fallback,颜色默认。
 */
export function resolveFaction(
  tag: string,
  directory: Record<string, BattleFactionInfo>,
): ResolvedFaction {
  const info = directory[tag];
  const leaf = tag.split('.').pop() || tag;
  return {
    name: info?.name || leaf,
    emblemId: info?.emblemId || 'seal',
    color: info?.color || '',
  };
}
