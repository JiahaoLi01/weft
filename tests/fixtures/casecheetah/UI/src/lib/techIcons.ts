// 科技树的图标（Art/Generated/techs，TechTree.md 第 7 节）：techs with a painted icon in UI/public/art/techs/<id>.png.
export const TECH_ICONS = new Set<string>([
  // 磨坊
  'water_millstones', 'automatic_mill', 'steam_millstones', 'steam_automatic_mill', 'high_grinding', 'roller_milling',
  'gradual_reduction', 'electric_mill', 'pneumatic_mill', 'automated_mill',
  'bolting_cloth', 'purifier', 'plansifter', 'natural_aging', 'flour_bleaching', 'enrichment',
  // 米行
  'hand_pounding', 'water_pounding', 'steam_rice_mill', 'engelberg_huller', 'rubber_roll_husker', 'abrasive_whitener', 'color_sorter',
  'rough_whitening', 'winnowing_fan', 'polishing', 'parboiling',
  // 粮仓：仓体、装卸、保粮、检验
  'sack_storehouse', 'bulk_bins', 'concrete_silos',
  'manual_porters', 'pulley_hoist', 'bucket_elevator', 'belt_conveyor', 'pneumatic_conveyor',
  'natural_care', 'ventilated_storehouse', 'fumigation', 'aeration_drying', 'controlled_atmosphere',
  'probe_sampling', 'moisture_tester', 'temperature_monitoring',
  // 基础学科
  'chemistry', 'refrigeration', 'electromagnetism', 'biochemistry', 'electronics',
]);
