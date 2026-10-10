import { create } from 'zustand';

/** 一支股票的快照（对应 C++ `UMarketService::BuildMarketSnapshotJson`）。 */
/** 逐日 OHLC：[open, high, low, close]。 */
export type DailyBar = [number, number, number, number];

/** 一笔成交：[成交日, 股数(正买负卖), 成交价]。 */
export type TradeRecord = [number, number, number];

export interface MarketStock {
  id: string;
  sector: string;
  /** 当前价。 */
  price: number;
  /** 一手股数（下单粒度，默认 100）。 */
  lot: number;
  /** 当前持仓股数（可负 = 卖空）。 */
  shares: number;
  /** 逐笔成交（旧 → 新）。画 K 线 B/S 点 + 交易记录列表。 */
  trades: TradeRecord[];
  /** 逐日 OHLC（旧 → 新）。前端按 candleDays 聚合成蜡烛；candleDays=1 直接用。 */
  bars: DailyBar[];
}

/** loadMarket 的整包 payload。 */
export interface MarketSnapshot {
  currentDay: number;
  stocks: MarketStock[];
}

interface MarketState {
  currentDay: number;
  stocks: MarketStock[];
  selectedId: string | null;
  /** 每根蜡烛聚合多少天（1=日、7=周、10=旬）。 */
  candleDays: number;

  setSnapshot: (snap: MarketSnapshot) => void;
  setSelected: (id: string | null) => void;
  setCandleDays: (n: number) => void;
}

/**
 * 模拟股市的前端状态。C++ 通过 `loadMarket` 推快照（cheat `OpenMarketUI` / `MarketSimulateDays`），
 * 由 Market 页注册的 handler 写进来。纯展示，无回写。
 */
export const useMarketStore = create<MarketState>((set) => ({
  currentDay: 0,
  stocks: [],
  selectedId: null,
  candleDays: 5,

  setSnapshot: (snap) =>
    set((prev) => {
      // 兜底字段：防止前端/ C++ 版本不一致时炸掉（旧 C++ 不发 lot/shares/trades → 默认 100/0/[]；不发 bars → 空图）。
      const stocks = (snap?.stocks ?? []).map((s) => ({
        ...s,
        bars: s.bars ?? [],
        lot: s.lot ?? 100,
        shares: s.shares ?? 0,
        trades: s.trades ?? [],
      }));
      // 保留当前选中（若还在），否则默认选第一支。
      const keep = prev.selectedId && stocks.some((s) => s.id === prev.selectedId);
      return {
        currentDay: snap?.currentDay ?? 0,
        stocks,
        selectedId: keep ? prev.selectedId : stocks[0]?.id ?? null,
      };
    }),

  setSelected: (id) => set({ selectedId: id }),
  setCandleDays: (n) => set({ candleDays: Math.max(1, Math.floor(n)) }),
}));
