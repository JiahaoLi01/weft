import { Fragment, useState, type ReactNode } from 'react';
import { count, decimal } from '@/lib/format';
import type { StageChain, StageSlot } from '@/types';

// 工序（Processes.md 第 9 节）：一张工序流程表，一行一道工序，不套框。横条是每级能力：金色是瓶颈定下的那一截（上游等下游，
// 各道都只做这么多），淡色是闲着的能力。产品设计与改型里点做法就地换；产线只读。

/** One stage as the table shows it. */
type FlowRow = { id: string; name: string; way?: string; wayName?: string; manual?: boolean; capacity?: number; crew?: number; power?: string;
  powered?: boolean; detail?: string;
  /** 产线上：班组对这道做法的熟练度，和额定能力（capacity 是按熟练度算的实际能力）。 */
  skill?: number; rated?: number;
  /** What the stage works on first (上光 polishes what 抛光 made); bad: chosen without it. */
  note?: string; bad?: boolean };

/** Each stage's way in a chain of ids: its way there, else (a required stage) its opening way, else not done (''). */
export function chosenOf(slots: StageSlot[], chain: string[]): Record<string, string> {
  const out: Record<string, string> = {};
  for (const s of slots) out[s.id] = s.ways.find(w => chain.includes(w.id))?.id ?? (s.required ? s.opening ?? '' : '');
  return out;
}

/** The chain the choices make, in the stages' order: the commands' techs. */
export function chainOf(slots: StageSlot[], chosen: Record<string, string>) {
  return slots.map(s => chosen[s.id]).filter(Boolean).join(',');
}

/** 一句话：每级能力（瓶颈那道）、人手、要的动力。 */
export function chainSummary(chain: StageChain, unit: string, rated?: boolean) {
  const neck = chain.stages.filter(s => s.bottleneck).map(s => s.name).join('、');
  const parts = [`每级 ${decimal(chain.actual ?? chain.capacity)} ${unit}／日${neck ? `（瓶颈：${neck}）` : ''}`, `${count(chain.crew)} 人${chain.porters ? `（含搬运 ${count(chain.porters)}）` : ''}`];
  if (chain.needs) parts.push(`要${chain.needs}动力`);
  return `${rated ? '额定 ' : ''}${parts.join(' · ')}`;
}

/** The slowest stage's capacity, the widest bar, the people, and which stages are the bottleneck. */
function totals(rows: FlowRow[]) {
  const on = rows.filter(r => r.way);
  const neck = on.length ? Math.min(...on.map(r => r.capacity ?? 0)) : 0;
  return { neck, top: Math.max(1, ...on.map(r => r.capacity ?? 0)), crew: on.reduce((a, r) => a + (r.crew ?? 0), 0),
    necks: on.filter(r => (r.capacity ?? 0) <= neck).map(r => r.name) };
}

function StageFlow({ rows, unit, slots, chosen, onChange, footer }: { rows: FlowRow[]; unit: string; slots?: StageSlot[]; chosen?: Record<string, string>;
  onChange?: (stage: string, way: string) => void; footer?: ReactNode }) {
  const [open, setOpen] = useState('');
  const t = totals(rows);
  const edit = !!slots && !!chosen && !!onChange;
  const pick = (stage: string, way: string) => { onChange?.(stage, way); setOpen(''); };
  return <div className={`stage-flow ${edit ? 'editable' : ''}`}>
    <div className="flow-row flow-head"><span>工序</span><span>做法</span><span>每级能力（{unit}／日）</span><span /><span className="num">人手</span></div>
    {rows.map(r => {
      const slot = slots?.find(s => s.id === r.id);
      const neck = !!r.way && (r.capacity ?? 0) <= t.neck;
      const off = r.rated !== undefined && Math.abs(r.rated - (r.capacity ?? 0)) > .5;
      const tags = r.manual || r.power || r.powered === false || r.skill !== undefined;
      return <Fragment key={r.id}>
        <div className={`flow-row ${r.way ? '' : 'absent'} ${neck ? 'neck' : ''} ${open === r.id ? 'open' : ''}`}>
          <span className="flow-name">{r.name}</span>
          {edit ? <button className="flow-way" aria-expanded={open === r.id} onClick={() => setOpen(open === r.id ? '' : r.id)}>
              {r.way ? r.wayName : `＋ 加上${r.name}`}<i>{open === r.id ? '▴' : '▾'}</i></button>
            : <span className="flow-way">{r.way ? r.wayName : '不做'}</span>}
          <span className="flow-bar">{r.way && <>
            <span className="cap" style={{ width: `${((r.capacity ?? 0) / t.top) * 100}%` }} />
            <span className="used" style={{ width: `${(Math.min(r.capacity ?? 0, t.neck) / t.top) * 100}%` }} /></>}</span>
          <span className="flow-num cap">{r.way ? decimal(r.capacity ?? 0) : ''}</span>
          <span className="flow-num">{r.way ? count(r.crew ?? 0) : ''}</span>
          {(r.way && (tags || r.detail)) || r.note ? <span className="flow-detail">
            {r.manual && <i>硬手工</i>}{r.power && <i>要{r.power}</i>}{r.powered === false && <i className="warn">动力不够</i>}
            {r.way && r.skill !== undefined && <i className={r.skill < 60 ? 'warn' : ''}>熟练 {Math.round(r.skill)}{off ? ` · 额定 ${decimal(r.rated ?? 0)}` : ''}</i>}
            {r.way && r.detail}{r.note && <em className={r.bad ? 'bad' : ''}>{r.note}</em>}</span> : null}
        </div>
        {edit && open === r.id && slot && <div className="flow-picker" role="listbox" aria-label={slot.name}>
          {!slot.required && <button role="option" aria-selected={!chosen[slot.id]} className={!chosen[slot.id] ? 'chosen' : ''} onClick={() => pick(slot.id, '')}>
            <b>不做</b><span>{chosen[slot.id] ? '去掉这道工序' : '不做这道工序'}</span></button>}
          {slot.ways.map(w => <button key={w.id} role="option" aria-selected={chosen[slot.id] === w.id} disabled={!w.known}
            className={chosen[slot.id] === w.id ? 'chosen' : ''} onClick={() => pick(slot.id, w.id)}>
            <b>{w.name}</b>
            <span>{decimal(w.capacity)} {unit} · {count(w.crew)} 人{w.manual ? ' · 硬手工' : ''}{w.power ? ` · 要${w.power}` : ''}{w.detail ? ` · ${w.detail}` : ''}{w.known ? '' : ' · 未掌握'}</span>
          </button>)}
        </div>}
      </Fragment>;
    })}
    {footer && <div className="flow-foot">{footer}</div>}
  </div>;
}

/** 只读的工序流程：产线在这处工厂的实际每级能力（按班组对各道做法的熟练度）与人手，或产品的额定值。 */
export function ChainFlow({ chain, unit, footer }: { chain: StageChain; unit: string; footer?: ReactNode }) {
  const rows: FlowRow[] = chain.stages.map(s => ({ id: s.id, name: s.name, way: s.present ? s.way : undefined, wayName: s.wayName, manual: s.manual,
    capacity: s.actual ?? s.capacity, crew: s.crew, power: s.power, powered: s.powered, detail: s.detail, skill: s.skill, rated: s.actual !== undefined ? s.capacity : undefined }));
  return <StageFlow rows={rows} unit={unit} footer={footer} />;
}

/**
 * 产品设计与改型里的工序流程：点一道工序的做法，就地列出能换的做法（必需的从已掌握的里选，可选的多一项“不做”，没掌握的列出来
 * 不能选）；合计按额定值算，不含搬运，也不算水轮机、电力的加成。
 */
export function StageEditor({ slots, chosen, onChange, unit }: { slots: StageSlot[]; chosen: Record<string, string>; onChange: (stage: string, way: string) => void;
  unit: string }) {
  const rows: FlowRow[] = slots.map(s => {
    const w = s.ways.find(x => x.id === chosen[s.id]);
    const missing = s.needs.some(n => !chosen[n]);
    return { id: s.id, name: s.name, way: w?.id, wayName: w?.name, manual: w?.manual, capacity: w?.capacity, crew: w?.crew, power: w?.power, detail: w?.detail,
      note: missing ? `要先有${s.needNames}` : undefined, bad: missing && !!w };
  });
  const t = totals(rows);
  const strongest = slots.map(s => s.ways.find(x => x.id === chosen[s.id])).reduce<{ tier: number; power?: string } | undefined>(
    (a, w) => (w && w.tier > (a?.tier ?? 0) ? w : a), undefined);
  return <StageFlow rows={rows} unit={unit} slots={slots} chosen={chosen} onChange={onChange}
    footer={<>额定每级 <b>{decimal(t.neck)}</b> {unit}／日 · {count(t.crew)} 人{t.necks.length ? ` · 瓶颈：${t.necks.join('、')}` : ''}
      {strongest?.power ? ` · 要${strongest.power}动力` : ''}<em>不含搬运，也不算水轮机、电力的加成；做它的工厂要有够档的动力。</em></>} />;
}
