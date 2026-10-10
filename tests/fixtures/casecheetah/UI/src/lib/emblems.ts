// 设施徽记（HierarchyUI.md 第 9 节）：the facility types whose painted emblem is in `public/art/buildings/<type>.png` (cut from
// the sheets in Art/Generated/buildings by `Scripts/slice_icon_sheets.py buildings`). The others keep their type character.
export const emblemTypes = new Set<string>([
  'armory', 'bank_office', 'blast_furnace', 'building', 'carting_depot', 'coach_works', 'coal_mine',
  'construction', 'cotton_mill', 'depot', 'engine_works', 'escort_bureau', 'farm', 'filature',
  'flour_mill', 'furniture_workshop', 'granary', 'iron_mine', 'omnibus_depot', 'opium_factory',
  'plantation', 'port', 'press', 'rail_depot', 'resort_hotel', 'rice_mill', 'rolling_stock_works',
  'shipping_office', 'shipyard', 'tea_factory', 'theatre', 'timber_camp',
]);

export const emblemArt = (typeId: string) => emblemTypes.has(typeId) ? `art/buildings/${typeId}.png` : undefined;
