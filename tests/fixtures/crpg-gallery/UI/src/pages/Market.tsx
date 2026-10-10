import { useCallback, useEffect, useMemo, useState } from 'react';
import { sendToUE } from '@/lib/ue-bridge';
import { useUEHandler } from '@/hooks/use-ue-handler';
import { useUIStore } from '@/stores/ui-store';
import { useMarketStore, type MarketSnapshot, type MarketStock } from '@/stores/market-store';
import { CandlestickChart } from '@/components/market/CandlestickChart';
import { useMarketPalette } from '@/hooks/use-market-color';
import type { MarketPalette } from '@/config/market-colors';

/**
 * 模拟股市 K 线图（dev only）。
 *
 * 协议：
 *   UE → JS: loadMarket（{currentDay, stocks:[{id,sector,price,lot,shares,trades,bars}]}）
 *   JS → UE: market_request（挂载即取）/ market_advance（{days}）/ market_add_attractor（{kind,scope,...}）
 *           / market_trade（{stockId, shares, side:"buy"|"sell"}，卖出超持仓=卖空）
 *
 * 注意：本页**只用按钮/步进器，不用任何可编辑 `<input>`** —— Ultralight 下原生输入框交互会硬崩
 * （项目主 UI 同理避开，用 slider/toggle/自定义控件）。
 */
const CANDLE_OPTIONS = [
  { label: '日', days: 1 },
  { label: '周', days: 7 },
  { label: '旬', days: 10 },
];

const ADVANCE_PRESETS = [1, 7, 30, 90, 365];

// 作用域预设（对应 demo 的板块 tag；空 = 全市场）。
const SCOPES = [
  { label: '全市场', tag: '' },
  { label: '银行', tag: 'Market.Eco.Bank' },
  { label: '军工', tag: 'Market.Eco.Industry.Arms' },
  { label: '航运', tag: 'Market.Eco.Industry.Shipping' },
  { label: '农业', tag: 'Market.Eco.Agriculture' },
  { label: '黄金', tag: 'Market.Eco.Commodity.Gold' },
];

const KINDS = [
  { id: 'soft_target', label: '软·拉向目标价' },
  { id: 'sector_trend', label: '软·板块趋势' },
  { id: 'instant_return', label: '硬·单日涨跌' },
  { id: 'pin_price', label: '硬·钉死价格' },
];

const btn = 'px-2 py-1 rounded border border-zinc-700 text-zinc-300 hover:text-zinc-100 hover:border-zinc-500 cursor-pointer transition-colors';
const stepBtn = 'w-6 h-6 flex items-center justify-center rounded border border-zinc-700 text-zinc-400 hover:text-zinc-100 hover:border-zinc-500 cursor-pointer text-xs';
const pill = (active: boolean) =>
  `px-2 py-1 rounded border cursor-pointer transition-colors whitespace-nowrap text-xs ${
    active ? 'border-amber-500 text-amber-300 bg-amber-700/20' : 'border-zinc-700 text-zinc-400 hover:text-zinc-200'
  }`;

export function Market() {
  const currentDay = useMarketStore((s) => s.currentDay);
  const stocks = useMarketStore((s) => s.stocks);
  const selectedId = useMarketStore((s) => s.selectedId);
  const candleDays = useMarketStore((s) => s.candleDays);
  const setSnapshot = useMarketStore((s) => s.setSnapshot);
  const setSelected = useMarketStore((s) => s.setSelected);
  const setCandleDays = useMarketStore((s) => s.setCandleDays);
  const goBack = useUIStore((s) => s.goBack);
  const palette = useMarketPalette();

  const [showAttractor, setShowAttractor] = useState(false);

  useUEHandler(
    'loadMarket',
    useCallback((data: unknown) => setSnapshot(data as MarketSnapshot), [setSnapshot]),
  );

  useEffect(() => {
    sendToUE('market_request');
  }, []);

  const selected = selectedId ? stocks.find((s) => s.id === selectedId) ?? null : null;

  // 成交日 → bars 下标：bars 连续且以 currentDay 结尾（固定历史 / 实时演化 / 读档重放都满足）。
  const tradeMarks = useMemo(() => {
    if (!selected || selected.trades.length === 0) return undefined;
    const firstBarDay = currentDay - selected.bars.length + 1;
    return selected.trades.map(([day, shares, price]) => ({ barIdx: day - firstBarDay, shares, price }));
  }, [selected, currentDay]);

  return (
    <div className="w-full h-full bg-zinc-950 text-zinc-200 flex flex-col overflow-hidden">
      {/* 顶栏 */}
      <header className="flex items-center justify-between px-6 py-3 border-b border-zinc-700 bg-zinc-900 shrink-0">
        <div className="flex items-baseline gap-4">
          <h1 className="text-lg font-bold">模拟股市 · K 线</h1>
          <span className="text-[10px] uppercase tracking-widest text-zinc-500">day {currentDay} · 滚轮缩放 · 拖拽平移</span>
        </div>
        <div className="flex items-center gap-3">
          <div className="flex items-center gap-1">
            {CANDLE_OPTIONS.map((o) => (
              <button key={o.days} className={pill(candleDays === o.days)} onClick={() => setCandleDays(o.days)}>
                {o.label}
              </button>
            ))}
          </div>
          <button
            className="w-7 h-7 flex items-center justify-center bg-zinc-800 border border-zinc-700 rounded
                       text-zinc-400 hover:bg-rose-900/40 hover:border-rose-700 hover:text-rose-300
                       transition-colors text-sm cursor-pointer"
            onClick={goBack}
            title="关闭"
          >
            ✕
          </button>
        </div>
      </header>

      {/* 调试工具栏：推进市场（预设按钮，无输入框）+ 添加吸引子 */}
      <div className="flex items-center gap-2 px-6 py-2 border-b border-zinc-800 bg-zinc-900 text-xs shrink-0">
        <span className="text-zinc-500">推进市场</span>
        {ADVANCE_PRESETS.map((d) => (
          <button key={d} className={btn} onClick={() => sendToUE('market_advance', { days: d })}>
            +{d}日
          </button>
        ))}
        <div className="w-px h-4 bg-zinc-700 mx-2" />
        <button
          className={`${btn} ${showAttractor ? 'border-amber-500 text-amber-300' : ''}`}
          onClick={() => setShowAttractor((v) => !v)}
        >
          {showAttractor ? '收起吸引子' : '添加吸引子 ▾'}
        </button>
      </div>

      {showAttractor && <AttractorForm defaultScope={selected?.sector ?? ''} currentDay={currentDay} />}

      <div className="flex-1 flex overflow-hidden">
        {/* 股票列表 */}
        <aside className="w-56 shrink-0 border-r border-zinc-800 bg-zinc-900 overflow-y-auto p-2 flex flex-col gap-1">
          {stocks.length === 0 ? (
            <div className="text-xs text-zinc-600 italic p-3">无股票数据。</div>
          ) : (
            stocks.map((s) => {
              const sel = s.id === selectedId;
              const first = s.bars[0]?.[3] ?? s.price; // 首日收盘
              const chg = first > 0 ? (s.price / first - 1) * 100 : 0;
              const up = chg >= 0;
              return (
                <button
                  key={s.id}
                  onClick={() => setSelected(s.id)}
                  className={`text-left rounded border px-2 py-1.5 cursor-pointer transition-colors ${
                    sel ? 'border-amber-500 bg-zinc-800' : 'border-zinc-800 bg-zinc-950 hover:border-zinc-600'
                  }`}
                >
                  <div className="flex items-baseline justify-between gap-2">
                    <span className="text-xs font-mono text-zinc-200 truncate">{s.id}</span>
                    <span className={`text-xs font-mono ${up ? palette.upClass : palette.downClass}`}>{s.price.toFixed(1)}</span>
                  </div>
                  <div className="flex items-baseline justify-between gap-2 mt-0.5">
                    <span className="text-[9px] text-zinc-500 truncate">
                      {s.sector.replace(/^Market\./, '')}
                      {s.shares !== 0 && <span className="ml-1 text-amber-500/90">持{s.shares}</span>}
                    </span>
                    <span className={`text-[9px] font-mono opacity-80 ${up ? palette.upClass : palette.downClass}`}>
                      {up ? '+' : ''}
                      {chg.toFixed(1)}%
                    </span>
                  </div>
                </button>
              );
            })
          )}
        </aside>

        {/* 图表 */}
        <main className="flex-1 overflow-auto p-4">
          {selected ? (
            <>
              <div className="flex items-baseline gap-4 mb-3">
                <h2 className="text-base font-mono text-zinc-100">{selected.id}</h2>
                <span className="text-xs text-zinc-500">{selected.sector}</span>
                <span className="text-sm font-mono text-zinc-200">{selected.price.toFixed(2)}</span>
                <span className="text-[10px] text-zinc-600">{selected.bars.length} 天历史</span>
              </div>
              <div className="bg-zinc-900 border border-zinc-800 rounded-lg p-3 overflow-hidden">
                <CandlestickChart bars={selected.bars} candleDays={candleDays} trades={tradeMarks} />
              </div>
            </>
          ) : (
            <div className="text-sm text-zinc-600 italic p-6">左侧选一支股票查看 K 线。</div>
          )}
        </main>

        {/* 交易 + 持仓（券商面板） */}
        <TradeSidebar selected={selected} stocks={stocks} palette={palette} />
      </div>
    </div>
  );
}

// ============================================================
//   交易 + 持仓侧栏（券商面板）
// ============================================================

function TradeSidebar({
  selected,
  stocks,
  palette,
}: {
  selected: MarketStock | null;
  stocks: MarketStock[];
  palette: MarketPalette;
}) {
  const [lots, setLots] = useState(1);

  const holdings = stocks.filter((s) => s.shares !== 0);
  const totalValue = holdings.reduce((sum, s) => sum + s.shares * s.price, 0);

  const trade = (side: 'buy' | 'sell') => {
    if (!selected || lots <= 0) return;
    // shares 已乘成股数；side 决定走 BuyStock / SellStock（卖出超持仓 = 卖空）。
    sendToUE('market_trade', { stockId: selected.id, shares: lots * selected.lot, side });
  };

  // 选中股当日涨跌 → 价格上色（与左侧列表一致）。
  const first = selected ? selected.bars[0]?.[3] ?? selected.price : 0;
  const up = selected ? selected.price >= first : true;
  const priceColor = up ? palette.upClass : palette.downClass;

  return (
    <aside className="w-72 shrink-0 border-l border-zinc-800 bg-zinc-900 overflow-y-auto p-3 flex flex-col gap-5">
      {/* 交易票据 */}
      <section>
        <h3 className="text-xs font-semibold text-zinc-300 mb-2">交易</h3>
        {selected ? (
          <div className="flex flex-col gap-2">
            <div className="flex items-baseline justify-between">
              <span className="font-mono text-sm text-zinc-100">{selected.id}</span>
              <span className={`font-mono text-sm ${priceColor}`}>{selected.price.toFixed(2)}</span>
            </div>
            <div className="text-[11px] text-zinc-500">
              持仓 <span className="font-mono text-zinc-300">{selected.shares}</span> 股
              {selected.shares < 0 && <span className="text-amber-500/90"> (卖空)</span>}
              {' · 市值 '}
              <span className="font-mono text-zinc-300">{(selected.shares * selected.price).toFixed(0)}</span>
            </div>

            <Stepper
              label={`手数（1 手 = ${selected.lot} 股）`}
              value={lots}
              set={(n) => setLots(Math.max(1, Math.round(n)))}
              step={1}
              big={10}
            />
            <div className="text-[11px] text-zinc-500">
              ≈ <span className="font-mono text-zinc-300">{(lots * selected.lot * selected.price).toFixed(0)}</span>
              {` （${lots * selected.lot} 股）`}
            </div>

            <div className="flex gap-2 mt-1">
              <button
                onClick={() => trade('buy')}
                style={{ color: palette.upHex, borderColor: palette.upHex }}
                className="flex-1 px-3 py-2 rounded border bg-transparent hover:bg-white/5 cursor-pointer text-sm font-medium transition-colors"
              >
                买入
              </button>
              <button
                onClick={() => trade('sell')}
                style={{ color: palette.downHex, borderColor: palette.downHex }}
                className="flex-1 px-3 py-2 rounded border bg-transparent hover:bg-white/5 cursor-pointer text-sm font-medium transition-colors"
              >
                卖出
              </button>
            </div>
            <p className="text-[10px] text-zinc-600 leading-relaxed">
              卖出超过持仓即转为卖空（持仓变负）。可买 / 可卖空的校验在 C++ 端（CanBuy / CanSell），目前恒通过。
            </p>
          </div>
        ) : (
          <p className="text-[11px] text-zinc-600 italic">左侧选一支股票。</p>
        )}
      </section>

      {/* 交易记录（选中股，新→旧）——与 K 线上的 B/S 点一一对应 */}
      {selected && selected.trades.length > 0 && (
        <section>
          <div className="flex items-baseline justify-between mb-2">
            <h3 className="text-xs font-semibold text-zinc-300">交易记录 · {selected.id}</h3>
            <span className="text-[10px] text-zinc-600">{selected.trades.length} 笔</span>
          </div>
          <div className="flex flex-col gap-1 max-h-48 overflow-y-auto">
            {[...selected.trades].reverse().slice(0, 20).map(([day, shares, price], idx) => {
              const buy = shares > 0;
              return (
                <div
                  key={`${day}-${idx}`}
                  className="grid grid-cols-[auto_auto_1fr_auto] items-baseline gap-2 text-[11px] rounded px-1.5 py-1 bg-zinc-950 border border-zinc-800"
                >
                  <span className="font-mono text-zinc-500">D{day}</span>
                  <span className={`font-semibold ${buy ? palette.upClass : palette.downClass}`}>{buy ? '买' : '卖'}</span>
                  <span className="font-mono text-zinc-300 text-right">{Math.abs(shares)} 股</span>
                  <span className="font-mono text-zinc-400 w-14 text-right">@{price.toFixed(1)}</span>
                </div>
              );
            })}
          </div>
        </section>
      )}

      {/* 持仓总览 */}
      <section>
        <div className="flex items-baseline justify-between mb-2">
          <h3 className="text-xs font-semibold text-zinc-300">持仓 ({holdings.length})</h3>
          <span className="text-[11px] font-mono text-zinc-400">总市值 {totalValue.toFixed(0)}</span>
        </div>
        {holdings.length === 0 ? (
          <p className="text-[11px] text-zinc-600 italic">暂无持仓。买入后在此显示。</p>
        ) : (
          <div className="flex flex-col gap-1">
            {holdings.map((s) => {
              const value = s.shares * s.price;
              const short = s.shares < 0;
              return (
                <div
                  key={s.id}
                  className="grid grid-cols-[1fr_auto_auto] items-baseline gap-2 text-[11px] rounded px-1.5 py-1 bg-zinc-950 border border-zinc-800"
                >
                  <span className="font-mono text-zinc-300 truncate">{s.id}</span>
                  <span className={`font-mono ${short ? 'text-amber-500/90' : 'text-zinc-400'}`}>{s.shares}</span>
                  <span className="font-mono text-zinc-200 w-16 text-right">{value.toFixed(0)}</span>
                </div>
              );
            })}
          </div>
        )}
      </section>
    </aside>
  );
}

// ============================================================
//   添加吸引子表单（纯按钮 / 步进器，无可编辑 input）
// ============================================================

function Field({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <label className="flex flex-col gap-0.5">
      <span className="text-[10px] text-zinc-500">{label}</span>
      {children}
    </label>
  );
}

function Stepper({
  label,
  value,
  set,
  step,
  big,
  fixed = 0,
}: {
  label: string;
  value: number;
  set: (n: number) => void;
  step: number;
  big: number;
  fixed?: number;
}) {
  const apply = (delta: number) => set(Number((value + delta).toFixed(Math.max(fixed, 0))));
  return (
    <Field label={label}>
      <div className="flex items-center gap-1">
        <button className={stepBtn} onClick={() => apply(-big)} title={`-${big}`}>
          «
        </button>
        <button className={stepBtn} onClick={() => apply(-step)}>
          −
        </button>
        <span className="w-14 text-center font-mono text-zinc-200">{value.toFixed(fixed)}</span>
        <button className={stepBtn} onClick={() => apply(step)}>
          +
        </button>
        <button className={stepBtn} onClick={() => apply(big)} title={`+${big}`}>
          »
        </button>
      </div>
    </Field>
  );
}

function AttractorForm({ defaultScope, currentDay }: { defaultScope: string; currentDay: number }) {
  const [kind, setKind] = useState('soft_target');
  const [scope, setScope] = useState(defaultScope);
  const [fromDay, setFromDay] = useState(currentDay);
  const [untilDay, setUntilDay] = useState(currentDay + 60);
  const [triggerDay, setTriggerDay] = useState(currentDay + 1);
  const [targetPrice, setTargetPrice] = useState(120);
  const [pullStrength, setPullStrength] = useState(0.08);
  const [dailyTrendPct, setDailyTrendPct] = useState(-1); // %
  const [percentChange, setPercentChange] = useState(-20); // %
  const [pinnedPrice, setPinnedPrice] = useState(100);

  const isSoft = kind === 'soft_target' || kind === 'sector_trend';

  const submit = () => {
    const payload: Record<string, unknown> = { kind, scope: scope.trim() };
    if (kind === 'soft_target') {
      Object.assign(payload, { targetPrice, pullStrength, fromDay, untilDay });
    } else if (kind === 'sector_trend') {
      Object.assign(payload, { dailyTrendPercent: dailyTrendPct / 100, fromDay, untilDay });
    } else if (kind === 'instant_return') {
      Object.assign(payload, { percentChange: percentChange / 100, triggerDay });
    } else if (kind === 'pin_price') {
      Object.assign(payload, { pinnedPrice, triggerDay });
    }
    sendToUE('market_add_attractor', payload);
  };

  return (
    <div className="flex flex-wrap items-end gap-3 px-6 py-3 border-b border-zinc-800 bg-zinc-900 text-xs">
      <Field label="类型">
        <div className="flex gap-1">
          {KINDS.map((k) => (
            <button key={k.id} type="button" onClick={() => setKind(k.id)} className={pill(kind === k.id)}>
              {k.label}
            </button>
          ))}
        </div>
      </Field>

      <Field label="作用域">
        <div className="flex gap-1">
          {SCOPES.map((s) => (
            <button key={s.label} type="button" onClick={() => setScope(s.tag)} className={pill(scope === s.tag)}>
              {s.label}
            </button>
          ))}
        </div>
      </Field>

      {isSoft ? (
        <>
          <Stepper label="起始天" value={fromDay} set={setFromDay} step={1} big={30} />
          <Stepper label="结束天" value={untilDay} set={setUntilDay} step={1} big={30} />
        </>
      ) : (
        <Stepper label="触发天" value={triggerDay} set={setTriggerDay} step={1} big={30} />
      )}

      {kind === 'soft_target' && (
        <>
          <Stepper label="目标价" value={targetPrice} set={setTargetPrice} step={5} big={50} />
          <Stepper label="牵引强度" value={pullStrength} set={setPullStrength} step={0.01} big={0.05} fixed={2} />
        </>
      )}
      {kind === 'sector_trend' && <Stepper label="每日趋势 %" value={dailyTrendPct} set={setDailyTrendPct} step={0.1} big={1} fixed={1} />}
      {kind === 'instant_return' && <Stepper label="单日涨跌 %" value={percentChange} set={setPercentChange} step={1} big={5} />}
      {kind === 'pin_price' && <Stepper label="钉死价" value={pinnedPrice} set={setPinnedPrice} step={5} big={50} />}

      <button
        onClick={submit}
        className="px-3 py-1.5 rounded border border-emerald-700 bg-emerald-700/20 text-emerald-300
                   hover:bg-emerald-700/40 cursor-pointer transition-colors"
      >
        添加 + 刷新
      </button>
    </div>
  );
}
