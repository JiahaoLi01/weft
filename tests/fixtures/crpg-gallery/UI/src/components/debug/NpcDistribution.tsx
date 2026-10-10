import * as d3 from 'd3';
import type { DistributionEntry, NpcRow, RumorRow } from '@/stores/rumor-test-store';

interface NpcDistributionProps {
  npc: NpcRow;
  distribution: DistributionEntry[];
  rumors: RumorRow[];
}

/**
 * 选中 NPC 的 P(rumor=i) 分布——横向 stacked bar + 排序后的细分列表。
 * 用 schemeCategory10 给每个 rumor 分配稳定颜色（按 rumor.id 在数组中位置）。
 */
export function NpcDistribution({ npc, distribution, rumors }: NpcDistributionProps) {
  // 颜色映射：用 rumors 数组顺序保证稳定。
  const colorOf = (rumorId: string) => {
    const idx = rumors.findIndex((r) => r.id === rumorId);
    return d3.schemeCategory10[Math.max(0, idx) % 10];
  };

  // 按概率降序的副本——visual list 用。
  const sorted = [...distribution].sort((a, b) => b.p - a.p);

  if (distribution.length === 0) {
    return (
      <div className="text-[10px] text-zinc-600 italic">
        No active rumor for this NPC (all weights × intensities ≤ 0).
      </div>
    );
  }

  return (
    <div className="flex flex-col gap-3">
      {/* 标签徽章 */}
      <div className="flex flex-wrap gap-0.5">
        {npc.tags.map((t) => (
          <span key={t} className="px-1 py-0 bg-zinc-800 border border-zinc-700 rounded
                                    text-[9px] text-zinc-400 font-mono">
            {t.replace(/^Npc\./, '')}
          </span>
        ))}
      </div>

      {/* Stacked bar */}
      <div className="flex h-4 rounded overflow-hidden bg-zinc-900 border border-zinc-800">
        {sorted.map((entry) => (
          <div
            key={entry.rumorId}
            className="h-full"
            style={{
              width: `${entry.p * 100}%`,
              backgroundColor: colorOf(entry.rumorId),
            }}
            title={`${entry.rumorId}: ${(entry.p * 100).toFixed(1)}%`}
          />
        ))}
      </div>

      {/* 排序后的细分列表 */}
      <div className="flex flex-col gap-0.5 text-[10px]">
        {sorted.map((entry) => (
          <div key={entry.rumorId} className="flex items-center gap-2">
            <span
              className="inline-block w-2 h-2 rounded-sm shrink-0"
              style={{ backgroundColor: colorOf(entry.rumorId) }}
            />
            <span className="font-mono text-zinc-300 flex-1 truncate">{entry.rumorId}</span>
            <span className="font-mono text-zinc-500 w-14 text-right">
              {(entry.p * 100).toFixed(1)}%
            </span>
          </div>
        ))}
      </div>
    </div>
  );
}
