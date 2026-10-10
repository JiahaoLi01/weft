import { sendToUE } from './ue-bridge';
import { useUIStore } from '@/stores/ui-store';
import { focusOn } from './focus';

export const pounds = (value: number) => `£${Math.round(value).toLocaleString('en-GB')}`;
/** Signed pounds for profits and changes. */
export const signedPounds = (value: number) => `${value < 0 ? '−' : value > 0 ? '+' : ''}£${Math.abs(Math.round(value)).toLocaleString('en-GB')}`;
/** Compact pounds for chart axes: £1.2k, £3.4M. */
export const shortPounds = (value: number) => {
  const a = Math.abs(value), s = value < 0 ? '−' : '';
  if (a >= 1e6) return `${s}£${(a / 1e6).toFixed(a >= 1e7 ? 0 : 1)}M`;
  if (a >= 1e3) return `${s}£${(a / 1e3).toFixed(a >= 1e4 ? 0 : 1)}k`;
  return `${s}£${Math.round(a)}`;
};
export const percent = (value: number) => `${(value * 100).toFixed(value > 0 && value < .1 ? 1 : 0)}%`;
export const price = (value: number) => `£${value >= 1 ? value.toFixed(2) : value.toFixed(3)}`;
export const count = (value: number) => Math.round(value).toLocaleString('en-GB');
export const decimal = (value: number) => value >= 100 ? count(value) : value >= 10 ? value.toFixed(1) : value.toFixed(2).replace(/\.?0+$/, '') || '0';
/** A quantity per unit of a product: three significant figures below one (0.195 吨 a bag, not 0.2). */
export const perUnit = (value: number) => value > 0 && value < 1 ? String(+value.toPrecision(3)) : decimal(value);
export const people = (value: number) => value >= 10000 ? `${(value / 10000).toFixed(value >= 1e6 ? 0 : 1)} 万` : count(value);

/** Every game-state action is a command; the UI never mutates the simulation. */
export const command = (type: string, target = '', args: Record<string, string> = {}) => sendToUE('command', { type, target, args });
/** A company named anywhere becomes the focus, shown in the business tree (another group's company is observed). */
export const openCompany = (id: string) => { focusOn('company', id); useUIStore.getState().setDock('business'); };
export const focusFacility = (id: string) => sendToUE('focus_facility', { id });
/** Colours the market-metrics layer by a subject and one of its measures, and brings the map to the front. */
/** selected (goods only): the 选中商品; '' = the player's own product of the good, 'none' = no product. */
export const setShareFocus = (kind: 'group' | 'product' | 'brand' | 'company' | 'good', id = '', measure = 'share', selected = '') => {
  sendToUE('set_share_focus', { kind, id, measure, selected });
  useUIStore.getState().setPanel('world');
};
/** Flies the camera to a 集散区 hub and brings the map to the front. */
export const focusMarket = (lon: number, lat: number) => {
  sendToUE('focus_market', { lon, lat });
  useUIStore.getState().setPanel('world');
};

export const measureNames: Record<string, string> = { share: '份额', units: '销量', revenue: '收入', price: '比价', stock: '库存', awareness: '知名度', loyalty: '忠诚度',
  main: '主力商品价格', delivered: '选中商品到户价格', sellers: '卖家' };
/** A measure's name for a focus: a product's own delivered price is just its 到户价. */
export const measureLabel = (kind: string | undefined, measure: string) => kind === 'product' && measure === 'delivered' ? '到户价' : measureNames[measure] ?? measure;
/** A measure's value as the map and the tables show it (share and standing are 0..1, the price index 1 = rivals' mean). */
export function formatMeasure(measure: string, value: number, unit = ''): string {
  if (value < 0 || !Number.isFinite(value)) return '—';
  switch (measure) {
    case 'share': case 'awareness': case 'loyalty': return `${(value * 100).toFixed(value > 0 && value < .1 ? 1 : 0)}%`;
    case 'price': return `${Math.round(value * 100)}%`;
    case 'revenue': return shortPounds(value);
    case 'units': return `${count(value)} 份`;
    case 'stock': return `${count(value)}${unit ? ' ' + unit : ''}`;
    case 'main': case 'delivered': return price(value);
    case 'sellers': return `${count(value)} 个`;
    default: return String(value);
  }
}

/** One-character glyph per facility type for markers and cards. */
export const facilityGlyph: Record<string, string> = {
  cotton_mill: '纺', flour_mill: '磨', rice_mill: '碾', farm: '农', granary: '粮', plantation: '园', filature: '丝', tea_factory: '茶', opium_factory: '烟', coal_mine: '煤', iron_mine: '矿',
  blast_furnace: '铁', timber_camp: '木', furniture_workshop: '具', coach_works: '车', engine_works: '机', shipyard: '船', armory: '械', press: '报',
  shipping_office: '航', carting_depot: '站', rail_depot: '轨', omnibus_depot: '巴', theatre: '戏', resort_hotel: '旅', escort_bureau: '镖', bank_office: '银', depot: '仓',
};

/** Tone of a works' status words (marker borders, badges). */
export const statusTone = (status = '') =>
  status.startsWith('受限') || status === '歉收将至' || status === '开仓平粜' ? 'limited' : status === '封存' || status.includes('未开工') || status.includes('没有产品') ? 'idle'
    : status.includes('安装') || status.includes('工程') ? 'project' : status === '产能爬坡' ? 'ramp' : 'steady';
