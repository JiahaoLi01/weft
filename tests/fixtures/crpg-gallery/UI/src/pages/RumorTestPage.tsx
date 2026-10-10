import { useCallback, useEffect, useState } from 'react';
import { sendToUE } from '@/lib/ue-bridge';
import { useUEHandler } from '@/hooks/use-ue-handler';
import { IntensityChart } from '@/components/debug/IntensityChart';
import { RumorDetail } from '@/components/debug/RumorDetail';
import { NpcDistribution } from '@/components/debug/NpcDistribution';
import { GroupShare } from '@/components/debug/GroupShare';
import {
  useRumorTestStore,
  type RumorRow,
  type NpcRow,
  type UtteranceLog,
  type DistributionEntry,
} from '@/stores/rumor-test-store';

/**
 * Rumor 系统集成测试页（dev only）
 *
 * 三栏布局：
 *  · 左：所有 rumor 的 Intensity 条 + 激活/停用按钮
 *  · 中：所有 ARumorTestCRPGCharacter 列表，点 "Talk" 触发 EvaluateNpcUtterance
 *  · 右：最近的 utterance 日志
 *
 * 顶部：当前 sim tick + Advance Tick 按钮（推进 TimeOfDay → 触发 Service.AdvanceOneTick）。
 *
 * 协议对应 `ARumorTestGameMode`：
 *  JS → UE: rumortest_ready / rumortest_activate / rumortest_deactivate /
 *           rumortest_nudge / rumortest_evaluate / rumortest_advance
 *  UE → JS: rumorTestInit / rumorTestState / rumorTestUtterance
 */
export function RumorTestPage() {
  const npcs    = useRumorTestStore(s => s.npcs);
  const rumors  = useRumorTestStore(s => s.rumors);
  const simTick = useRumorTestStore(s => s.simTick);
  const log     = useRumorTestStore(s => s.log);
  const history = useRumorTestStore(s => s.history);
  const distributions     = useRumorTestStore(s => s.distributions);
  const selectedRumorId   = useRumorTestStore(s => s.selectedRumorId);
  const selectedNpcId     = useRumorTestStore(s => s.selectedNpcId);
  const applyInit         = useRumorTestStore(s => s.applyInit);
  const applyState        = useRumorTestStore(s => s.applyState);
  const appendUtterance   = useRumorTestStore(s => s.appendUtterance);
  const clearLog          = useRumorTestStore(s => s.clearLog);
  const clearHistory      = useRumorTestStore(s => s.clearHistory);
  const setSelectedRumor  = useRumorTestStore(s => s.setSelectedRumor);
  const setSelectedNpc    = useRumorTestStore(s => s.setSelectedNpc);

  // ── ViewModel 心跳（C++ 端 URumorDebugViewModel 通过 __vm_update 推送）──
  const [vmCounts, setVmCounts] = useState<{
    SimTick?: number;
    ActiveRumorCount?: number;
    VariantCount?: number;
    NpcCount?: number;
  }>({});

  useEffect(() => {
    // Cinematic / settings 等其他页面也用 __vm_update——多家共存通过 merge delta。
    const prev = window.__vm_update;
    window.__vm_update = (delta) => {
      // 链式调用，让其他页面（MVVMTest 等）也能继续收到。
      if (prev) try { prev(delta); } catch {}
      setVmCounts((prevState) => {
        const next: typeof prevState = { ...prevState };
        for (const [k, v] of Object.entries(delta ?? {})) {
          if (typeof v === 'number') {
            (next as Record<string, number>)[k] = v;
          }
        }
        return next;
      });
    };
    return () => { window.__vm_update = prev; };
  }, []);

  // ── UE → JS 处理器（页面挂载期间生效）──
  useUEHandler('rumorTestInit', useCallback((data: unknown) => {
    applyInit(data as {
      npcs: NpcRow[];
      rumors: RumorRow[];
      simTick: number;
      distributions?: Record<string, DistributionEntry[]>;
    });
  }, [applyInit]));

  useUEHandler('rumorTestState', useCallback((data: unknown) => {
    applyState(data as {
      rumors: RumorRow[];
      simTick: number;
      distributions?: Record<string, DistributionEntry[]>;
    });
  }, [applyState]));

  useUEHandler('rumorTestUtterance', useCallback((data: unknown) => {
    appendUtterance(data as Omit<UtteranceLog, 'ts'>);
  }, [appendUtterance]));

  // ── 挂载即向 UE 报到 ──
  useEffect(() => {
    sendToUE('rumortest_ready');
  }, []);

  // ── 操作回调 ──
  const handleAdvance     = useCallback(() => sendToUE('rumortest_advance'), []);
  const handleActivate    = useCallback((id: string) => sendToUE('rumortest_activate',   { rumorId: id }), []);
  const handleDeactivate  = useCallback((id: string) => sendToUE('rumortest_deactivate', { rumorId: id }), []);
  const handleNudge       = useCallback((id: string, delta: number) => sendToUE('rumortest_nudge', { rumorId: id, delta }), []);
  const handleTalk        = useCallback((id: string) => sendToUE('rumortest_evaluate', { npcId: id }), []);

  // 选中切换：再点一次取消选中。
  const toggleRumorSelection = useCallback((id: string) => {
    setSelectedRumor(selectedRumorId === id ? null : id);
  }, [selectedRumorId, setSelectedRumor]);

  const toggleNpcSelection = useCallback((id: string) => {
    setSelectedNpc(selectedNpcId === id ? null : id);
  }, [selectedNpcId, setSelectedNpc]);

  // 关闭按钮——发到 UE，让 dispatcher 走标准 Deactivate 路径。
  const handleClose = useCallback(() => sendToUE('rumortest_close'), []);

  // 用 selectedRumorId 找当前选中——可能因为 list 重排或激活失效而 stale。
  const selectedRumor = selectedRumorId ? rumors.find(r => r.id === selectedRumorId) ?? null : null;
  const selectedNpc = selectedNpcId ? npcs.find(n => n.id === selectedNpcId) ?? null : null;

  // VM 心跳优先（即时同步），fallback 到 store 里的 simTick。
  const headerSimTick    = vmCounts.SimTick    ?? simTick;
  const activeRumorCount = vmCounts.ActiveRumorCount ?? rumors.filter(r => r.active).length;
  const variantCount     = vmCounts.VariantCount     ?? rumors.filter(r => r.isVariant).length;
  const headerNpcCount   = vmCounts.NpcCount         ?? npcs.length;

  return (
    // bg-zinc-950/60 = 60% 不透明（40% 透明）——debug overlay 看清下层游戏画面。
    // 不放 BackButton——overlay 是由 cheat 命令 / X 按钮控制的，不走 page stack 导航。
    <div className="w-full h-full bg-zinc-950/60 text-zinc-200 flex flex-col overflow-hidden">

      {/* 顶部信息条（fixed） */}
      <header className="flex items-center justify-between px-6 py-3 border-b border-zinc-700 bg-zinc-900/80 shrink-0">
        <div className="flex items-baseline gap-4">
          <h1 className="text-lg font-bold">Rumor Debug</h1>
          <span className="text-[10px] uppercase tracking-widest text-zinc-500">
            overlay — close with ✕ or `RumorDebug` cheat
          </span>
        </div>

        {/* MVVM-bound 心跳标签 */}
        <div className="flex items-center gap-4 text-xs">
          <Counter label="tick"    value={headerSimTick}    color="text-emerald-400" />
          <Counter label="rumors"  value={activeRumorCount} color="text-amber-400" />
          <Counter label="variants" value={variantCount}    color="text-fuchsia-400" />
          <Counter label="npcs"    value={headerNpcCount}   color="text-sky-400" />

          <button
            className="px-2 py-1 text-[10px] text-zinc-500 hover:text-zinc-300
                       border border-zinc-700 rounded transition-colors"
            onClick={clearHistory}
            title="Clear chart history"
          >
            Clear chart
          </button>
          <button
            className="px-3 py-1.5 bg-emerald-700/40 border border-emerald-500 rounded text-xs
                       text-emerald-200 hover:bg-emerald-700/70 transition-colors"
            onClick={handleAdvance}
          >
            Advance Tick &rarr;
          </button>
          <button
            className="w-7 h-7 flex items-center justify-center bg-zinc-800/80 border border-zinc-700 rounded
                       text-zinc-400 hover:bg-rose-900/40 hover:border-rose-700 hover:text-rose-300
                       transition-colors text-sm"
            onClick={handleClose}
            title="Close debug overlay"
          >
            ✕
          </button>
        </div>
      </header>

      {/* 整页可纵向滚动——列表 + Inspector 都进得去。 */}
      <div className="flex-1 overflow-y-auto">
        {/* 强度时序图 */}
        <section className="px-4 py-3 border-b border-zinc-800 bg-zinc-900/50">
          <IntensityChart rumors={rumors} history={history} height={180} />
        </section>

        {/* 三栏主体——内置滚动；50vh 高度避免顶到 inspector */}
        <main className="grid grid-cols-3 gap-3 p-4" style={{ height: '50vh' }}>
          <RumorPanel
            rumors={rumors}
            selectedId={selectedRumorId}
            onSelect={toggleRumorSelection}
            onActivate={handleActivate}
            onDeactivate={handleDeactivate}
            onNudge={handleNudge}
          />
          <NpcPanel
            npcs={npcs}
            selectedId={selectedNpcId}
            onSelect={toggleNpcSelection}
            onTalk={handleTalk}
          />
          <LogPanel log={log} onClear={clearLog} />
        </main>

        {/* ── Inspector Row ── */}
        <section className="border-t border-zinc-800 bg-zinc-900/40 grid grid-cols-3 gap-3 p-4">
          {/* 选中 rumor 详情 */}
          <InspectorCard title="Selected Rumor" accent="amber"
            empty={!selectedRumor && 'Click any rumor card above to inspect.'}>
            {selectedRumor && (
              <RumorDetail
                rumor={selectedRumor}
                history={history[selectedRumor.id] ?? []}
                currentTick={simTick}
              />
            )}
          </InspectorCard>

          {/* 选中 NPC 分布 */}
          <InspectorCard title="Selected NPC" accent="emerald"
            empty={!selectedNpc && 'Click any NPC card above to inspect.'}>
            {selectedNpc && (
              <NpcDistribution
                npc={selectedNpc}
                distribution={distributions[selectedNpc.id] ?? []}
                rumors={rumors}
              />
            )}
          </InspectorCard>

          {/* 群体平均分布 */}
          <InspectorCard title="Group Share (avg over filter)" accent="sky">
            <GroupShare npcs={npcs} rumors={rumors} distributions={distributions} />
          </InspectorCard>
        </section>
      </div>
    </div>
  );
}

// MVVM 心跳显示——VM 直推，无需等 rumorTestState push。
function Counter({ label, value, color }: { label: string; value: number; color: string }) {
  return (
    <span className="text-zinc-500 flex items-baseline gap-1">
      {label}:
      <span className={`font-mono ${color}`}>{value}</span>
    </span>
  );
}

declare global {
  interface Window {
    __vm_update?: (delta: Record<string, unknown>) => void;
  }
}

// ============================================================
//   Inspector card wrapper
// ============================================================

interface InspectorCardProps {
  title: string;
  accent: 'amber' | 'emerald' | 'sky';
  empty?: string | false;
  children?: React.ReactNode;
}

function InspectorCard({ title, accent, empty, children }: InspectorCardProps) {
  const accentColor =
    accent === 'amber'   ? 'text-amber-400'   :
    accent === 'emerald' ? 'text-emerald-400' :
                           'text-sky-400';

  return (
    <section className="bg-zinc-900 rounded-lg border border-zinc-800 flex flex-col overflow-hidden">
      <h2 className={`px-3 py-2 text-xs font-semibold ${accentColor} border-b border-zinc-800
                      uppercase tracking-wider`}>
        {title}
      </h2>
      <div className="p-3">
        {empty ? (
          <div className="text-[11px] text-zinc-600 italic">{empty}</div>
        ) : children}
      </div>
    </section>
  );
}

// 失败原因 → 人类可读提示。后端送来的 reason 字段必须命中此表。
const REASON_TEXT: Record<string, string> = {
  subsystem_unavailable:        'Rumor subsystem not available.',
  missing_npc_id:               'Missing npcId in request.',
  speaker_not_found:            'Speaker actor not found in world.',
  no_active_rumor:              'No active rumor — click Activate on at least one rumor first.',
  no_expressions_on_any_rumor:  'Active rumors have empty Expressions array — open the asset and add a RumorExpression.',
  zero_score_for_speaker:       'All rumors scored 0 for this speaker (weight log-sum underflow?).',
};

// ============================================================
//   Rumors panel
// ============================================================

interface RumorPanelProps {
  rumors: RumorRow[];
  selectedId: string | null;
  onSelect: (id: string) => void;
  onActivate: (id: string) => void;
  onDeactivate: (id: string) => void;
  onNudge: (id: string, delta: number) => void;
}

function RumorPanel({ rumors, selectedId, onSelect, onActivate, onDeactivate, onNudge }: RumorPanelProps) {
  return (
    <section className="bg-zinc-900 rounded-lg border border-zinc-800 flex flex-col overflow-hidden">
      <h2 className="px-3 py-2 text-xs font-semibold text-amber-400 border-b border-zinc-800
                     uppercase tracking-wider">
        Rumors ({rumors.length})
      </h2>
      <div className="flex-1 overflow-y-auto p-2 flex flex-col gap-2">
        {rumors.length === 0 ? (
          <div className="text-xs text-zinc-600 italic p-3">No rumors loaded.</div>
        ) : (
          rumors.map(r => (
            <RumorRowItem key={r.id} rumor={r}
                          selected={r.id === selectedId}
                          onSelect={onSelect}
                          onActivate={onActivate}
                          onDeactivate={onDeactivate}
                          onNudge={onNudge} />
          ))
        )}
      </div>
    </section>
  );
}

interface RumorRowProps {
  rumor: RumorRow;
  selected: boolean;
  onSelect: (id: string) => void;
  onActivate: (id: string) => void;
  onDeactivate: (id: string) => void;
  onNudge: (id: string, delta: number) => void;
}

// 阻断按钮点击冒泡到 row（避免按按钮顺带触发 onSelect）。
function stop(e: React.MouseEvent) { e.stopPropagation(); }

function RumorRowItem({ rumor, selected, onSelect, onActivate, onDeactivate, onNudge }: RumorRowProps) {
  const kind: 'baseline' | 'variant' | 'event' =
    rumor.isBaseline ? 'baseline' : rumor.isVariant ? 'variant' : 'event';

  const kindColor =
    kind === 'baseline' ? 'text-sky-400 border-sky-700' :
    kind === 'variant'  ? 'text-fuchsia-400 border-fuchsia-700' :
                          'text-amber-400 border-amber-700';

  // Activate 用 defId（指向资产）；Deactivate/Nudge 用 runtime id（变体可能 != defId）。
  const activateId = rumor.defId || rumor.id;

  // ── 未激活：灰显，仅露 Activate ──
  if (!rumor.active) {
    return (
      <div
        className={`rounded border px-2 py-1.5 bg-zinc-950 cursor-pointer
                    ${selected ? 'border-amber-400 opacity-100' : 'border-zinc-800 opacity-60'}`}
        onClick={() => onSelect(rumor.id)}
      >
        <div className="flex items-baseline justify-between gap-2 mb-1.5">
          <div className="flex items-baseline gap-2 min-w-0">
            <span className={`text-[9px] uppercase font-semibold ${kindColor.split(' ')[0]}`}>
              {kind}
            </span>
            <span className="text-xs text-zinc-400 font-mono truncate">{rumor.id}</span>
          </div>
          <span className="text-[9px] uppercase tracking-wider text-zinc-600 shrink-0">
            inactive
          </span>
        </div>
        <button
          className="w-full px-2 py-1 bg-emerald-700/20 border border-emerald-800 rounded text-[10px]
                     text-emerald-300 hover:bg-emerald-700/50 transition-colors"
          onClick={(e) => { stop(e); onActivate(activateId); }}
          title="Create runtime entry from this Definition"
        >
          Activate
        </button>
      </div>
    );
  }

  // ── 已激活：完整 UI ──
  // 强度可视化：以 5.0 为满量程（事件谣言典型 amplitude，超过后 clamp）。
  const fillPct = Math.min(100, (rumor.intensity / 5) * 100);
  const ringClass = selected ? 'ring-2 ring-amber-400' : '';

  return (
    <div
      className={`rounded border px-2 py-1.5 bg-zinc-950 cursor-pointer ${kindColor.split(' ')[1]} ${ringClass}`}
      onClick={() => onSelect(rumor.id)}
    >
      <div className="flex items-baseline justify-between gap-2 mb-1">
        <div className="flex items-baseline gap-2 min-w-0">
          <span className={`text-[9px] uppercase font-semibold ${kindColor.split(' ')[0]}`}>
            {kind}
          </span>
          <span className="text-xs text-zinc-200 font-mono truncate">{rumor.id}</span>
        </div>
        <span className="text-[10px] font-mono text-zinc-400 shrink-0">
          {rumor.intensity.toFixed(2)}
        </span>
      </div>

      {/* 强度条 */}
      <div className="h-1.5 bg-zinc-800 rounded overflow-hidden mb-1.5">
        <div
          className="h-full bg-amber-500/70 transition-all duration-300"
          style={{ width: `${fillPct}%` }}
        />
      </div>

      {/* 操作按钮 */}
      <div className="flex gap-1 text-[10px]">
        <button
          className="px-1.5 py-0.5 bg-zinc-800 border border-zinc-700 rounded text-zinc-400
                     hover:text-emerald-300 hover:border-emerald-700 transition-colors"
          onClick={(e) => { stop(e); onActivate(activateId); }}
          title="Reset to Definition default"
        >
          Reset
        </button>
        <button
          className="px-1.5 py-0.5 bg-zinc-800 border border-zinc-700 rounded text-zinc-400
                     hover:text-rose-300 hover:border-rose-700 transition-colors"
          onClick={(e) => { stop(e); onDeactivate(rumor.id); }}
        >
          Remove
        </button>
        <button
          className="px-1.5 py-0.5 bg-zinc-800 border border-zinc-700 rounded text-zinc-400
                     hover:text-sky-300 hover:border-sky-700 transition-colors"
          onClick={(e) => { stop(e); onNudge(rumor.id, +1); }}
          title="Nudge intensity +1"
        >
          +1
        </button>
        <button
          className="px-1.5 py-0.5 bg-zinc-800 border border-zinc-700 rounded text-zinc-400
                     hover:text-sky-300 hover:border-sky-700 transition-colors"
          onClick={(e) => { stop(e); onNudge(rumor.id, -1); }}
          title="Nudge intensity -1"
        >
          -1
        </button>
      </div>

      {kind === 'variant' && rumor.parentId && (
        <div className="text-[9px] text-zinc-500 mt-1 truncate">
          parent: <span className="font-mono">{rumor.parentId}</span>
        </div>
      )}
    </div>
  );
}

// ============================================================
//   NPCs panel
// ============================================================

interface NpcPanelProps {
  npcs: NpcRow[];
  selectedId: string | null;
  onSelect: (id: string) => void;
  onTalk: (id: string) => void;
}

function NpcPanel({ npcs, selectedId, onSelect, onTalk }: NpcPanelProps) {
  return (
    <section className="bg-zinc-900 rounded-lg border border-zinc-800 flex flex-col overflow-hidden">
      <h2 className="px-3 py-2 text-xs font-semibold text-emerald-400 border-b border-zinc-800
                     uppercase tracking-wider">
        NPCs ({npcs.length})
      </h2>
      <div className="flex-1 overflow-y-auto p-2 flex flex-col gap-2">
        {npcs.length === 0 ? (
          <div className="text-xs text-zinc-600 italic p-3">
            No ARumorTestCRPGCharacter actors found in level.
          </div>
        ) : (
          npcs.map(n => {
            const selected = n.id === selectedId;
            return (
              <div
                key={n.id}
                className={`rounded border px-2 py-2 bg-zinc-950 cursor-pointer
                            ${selected ? 'border-emerald-400 ring-2 ring-emerald-400' : 'border-zinc-800'}`}
                onClick={() => onSelect(n.id)}
              >
                <div className="flex items-baseline justify-between gap-2 mb-1">
                  <span className="text-xs font-semibold text-zinc-200 truncate">
                    {n.displayName}
                  </span>
                  <button
                    className="px-2 py-0.5 bg-emerald-700/30 border border-emerald-700 rounded text-[10px]
                               text-emerald-300 hover:bg-emerald-700/60 transition-colors shrink-0"
                    onClick={(e) => { stop(e); onTalk(n.id); }}
                  >
                    Talk
                  </button>
                </div>
                <div className="text-[9px] text-zinc-600 font-mono mb-1">{n.id}</div>
                <div className="flex flex-wrap gap-0.5">
                  {n.tags.map(t => (
                    <span key={t} className="px-1 py-0 bg-zinc-800 border border-zinc-700 rounded
                                              text-[9px] text-zinc-400 font-mono">
                      {t.replace(/^Npc\./, '')}
                    </span>
                  ))}
                </div>
              </div>
            );
          })
        )}
      </div>
    </section>
  );
}

// ============================================================
//   Log panel
// ============================================================

function LogPanel({ log, onClear }: { log: UtteranceLog[]; onClear: () => void }) {
  return (
    <section className="bg-zinc-900 rounded-lg border border-zinc-800 flex flex-col overflow-hidden">
      <header className="px-3 py-2 border-b border-zinc-800 flex items-center justify-between">
        <h2 className="text-xs font-semibold text-sky-400 uppercase tracking-wider">
          Utterance Log ({log.length})
        </h2>
        <button
          className="text-[10px] text-zinc-500 hover:text-zinc-300 transition-colors"
          onClick={onClear}
        >
          Clear
        </button>
      </header>
      <div className="flex-1 overflow-y-auto p-2 flex flex-col-reverse gap-2">
        {log.length === 0 ? (
          <div className="text-xs text-zinc-600 italic p-3">
            No utterances yet — click Talk on an NPC.
          </div>
        ) : (
          log.map((entry, i) => (
            <div
              key={`${entry.ts}-${i}`}
              className={`rounded border px-2 py-1.5 ${entry.ok
                ? 'border-zinc-800 bg-zinc-950'
                : 'border-rose-900 bg-rose-950/30'}`}
            >
              <div className="flex items-baseline justify-between gap-2 text-[9px] text-zinc-500 mb-1">
                <span className="font-mono">tick {entry.simTick}</span>
                <span>{new Date(entry.ts).toLocaleTimeString()}</span>
              </div>
              {entry.ok ? (
                <>
                  <div className="text-xs text-zinc-200 mb-1">{entry.text}</div>
                  <div className="text-[9px] text-zinc-500 font-mono truncate">
                    {entry.npcId} → {entry.rumorId} ({entry.expressionId})
                  </div>
                </>
              ) : (
                <>
                  <div className="text-[10px] text-rose-400 mb-0.5">
                    {REASON_TEXT[entry.reason ?? ''] ?? `Failed: ${entry.reason ?? 'unknown'}`}
                  </div>
                  <div className="text-[9px] text-rose-500/70 font-mono truncate">
                    speaker: {entry.npcId}
                  </div>
                </>
              )}
            </div>
          ))
        )}
      </div>
    </section>
  );
}
