import { useMemo, useState } from 'react';
import * as d3 from 'd3';
import type { DistributionEntry, NpcRow, RumorRow } from '@/stores/rumor-test-store';

interface GroupShareProps {
  npcs: NpcRow[];
  rumors: RumorRow[];
  distributions: Record<string, DistributionEntry[]>;
}

/**
 * 群体抽样视图：选一个 tag（如 Npc.Class.Worker），对所有命中该 tag 的 NPC
 * 求 P(rumor) 平均，按概率降序展示。tag 选 "All" 时对全员求平均。
 *
 * 镜像 mini-app 模拟器的"群体抽样视图"——用来看不同社会群体当前最关注哪些 rumor。
 */
export function GroupShare({ npcs, rumors, distributions }: GroupShareProps) {
  // 收集所有出现过的 tag——下拉项动态生成。
  const allTags = useMemo(() => {
    const set = new Set<string>();
    for (const n of npcs) for (const t of n.tags) set.add(t);
    return Array.from(set).sort();
  }, [npcs]);

  const [filterTag, setFilterTag] = useState<string>('all');

  const matchingNpcs = useMemo(() => {
    if (filterTag === 'all') return npcs;
    return npcs.filter((n) => n.tags.includes(filterTag));
  }, [npcs, filterTag]);

  // 对 matchingNpcs 求 P(rumor) 平均。
  const avgDistribution = useMemo(() => {
    if (matchingNpcs.length === 0) return [];
    const sums: Record<string, number> = {};
    for (const npc of matchingNpcs) {
      const dist = distributions[npc.id] ?? [];
      for (const entry of dist) {
        sums[entry.rumorId] = (sums[entry.rumorId] ?? 0) + entry.p;
      }
    }
    const out = Object.entries(sums).map(([rumorId, sum]) => ({
      rumorId,
      avg: sum / matchingNpcs.length,
    }));
    return out.sort((a, b) => b.avg - a.avg);
  }, [matchingNpcs, distributions]);

  const colorOf = (rumorId: string) => {
    const idx = rumors.findIndex((r) => r.id === rumorId);
    return d3.schemeCategory10[Math.max(0, idx) % 10];
  };

  return (
    <div className="flex flex-col gap-3">
      {/* Tag 下拉 */}
      <div className="flex items-center gap-2 text-[10px]">
        <span className="text-zinc-500">Filter by tag:</span>
        <select
          value={filterTag}
          onChange={(e) => setFilterTag(e.target.value)}
          className="bg-zinc-900 border border-zinc-700 rounded px-2 py-0.5 text-zinc-300 font-mono text-[10px]"
        >
          <option value="all">All ({npcs.length})</option>
          {allTags.map((t) => {
            const count = npcs.filter((n) => n.tags.includes(t)).length;
            return (
              <option key={t} value={t}>
                {t.replace(/^Npc\./, '')} ({count})
              </option>
            );
          })}
        </select>
      </div>

      {/* 平均分布 */}
      {matchingNpcs.length === 0 ? (
        <div className="text-[10px] text-zinc-600 italic">No NPCs match this filter.</div>
      ) : avgDistribution.length === 0 ? (
        <div className="text-[10px] text-zinc-600 italic">
          Group has no active rumor distribution yet — Activate a rumor or wait for state push.
        </div>
      ) : (
        <div className="flex flex-col gap-1 text-[10px]">
          {avgDistribution.map((entry) => (
            <div key={entry.rumorId} className="flex items-center gap-2">
              <span
                className="inline-block w-2 h-2 rounded-sm shrink-0"
                style={{ backgroundColor: colorOf(entry.rumorId) }}
              />
              <span className="font-mono text-zinc-300 w-44 truncate">{entry.rumorId}</span>
              <div className="flex-1 h-2 bg-zinc-900 rounded overflow-hidden">
                <div
                  className="h-full"
                  style={{
                    width: `${entry.avg * 100}%`,
                    backgroundColor: colorOf(entry.rumorId),
                  }}
                />
              </div>
              <span className="font-mono text-zinc-500 w-12 text-right">
                {(entry.avg * 100).toFixed(1)}%
              </span>
            </div>
          ))}
        </div>
      )}

      {/* meta */}
      <div className="text-[9px] text-zinc-600">
        averaged over {matchingNpcs.length} NPC{matchingNpcs.length !== 1 ? 's' : ''}
      </div>
    </div>
  );
}
