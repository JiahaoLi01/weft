import { useState, useCallback } from 'react';
import { ItemCard, PinnedTooltipProvider, PinnedTooltipLayer, type KeywordDef, type ItemDef } from './NestedTooltip';
import { DEFAULT_SMOKE_PARAMS, type SmokeParams } from './SmokeBg';

// ===== Mock Keyword Database =====

const KEYWORDS: KeywordDef[] = [
  // ── Terms (default wiki-style tooltip) ──
  {
    id: 'fire_damage',
    name: 'Fire Damage',
    description: 'Elemental damage that ignites targets. Scaled by {elemental_mastery}. Can trigger {burn}.',
    color: 'text-orange-400',
  },
  {
    id: 'arcane_damage',
    name: 'Arcane Damage',
    description: 'Pure magical damage that bypasses physical armor. Resisted only by magic resistance.',
    color: 'text-indigo-400',
  },
  {
    id: 'elemental_mastery',
    name: 'Elemental Mastery',
    description: 'Increases all elemental damage and {elemental_reaction} damage by 2% per point.',
    color: 'text-violet-400',
  },
  {
    id: 'elemental_reaction',
    name: 'Elemental Reaction',
    description: 'Triggered when two different elements interact. Deals bonus damage based on {elemental_mastery}.',
    color: 'text-sky-400',
  },
  {
    id: 'mana',
    name: 'Mana',
    description: 'Magical energy consumed to cast spells. Regenerates over time. Low mana causes {exhaustion}.',
    color: 'text-blue-400',
  },

  // ── Equipment (rarity smoke card) ──
  {
    id: 'fire_amulet',
    name: 'Ember Amulet',
    description: 'Grants +15% {fire_damage}. While equipped, {burn} duration is doubled.',
    color: 'text-amber-400',
    category: 'equipment',
    icon: '\u{1F4FF}',
    rarity: 'rare',
    stats: { 'Fire Damage': '+15%', 'Burn Duration': '+100%' },
  },

  // ── Spells (equipment renderer, spell category) ──
  {
    id: 'fireball',
    name: 'Fireball',
    description: 'Hurls a ball of fire dealing {fire_damage} in an area. Applies {burn} for 4 seconds.',
    color: 'text-orange-300',
    category: 'spell',
    icon: '\u{1F525}',
    rarity: 'epic',
    stats: { 'Damage': '120-180', 'Mana Cost': '35', 'Range': '25m', 'Cooldown': '8s' },
  },

  // ── Status effects (colored accent bar) ──
  {
    id: 'burn',
    name: 'Burn',
    description: 'Deals {fire_damage} over time for the duration. Removes {fortify}. Stacks up to 3 times.',
    color: 'text-red-400',
    category: 'status',
    icon: '\u{1F525}',
  },
  {
    id: 'stun',
    name: 'Stun',
    description: 'Target cannot move or act for the duration. Breaks on damage. Grants brief {fortify} after recovery.',
    color: 'text-yellow-400',
    category: 'status',
    icon: '\u{26A1}',
  },
  {
    id: 'fortify',
    name: 'Fortify',
    description: 'Reduces all incoming damage by 25%. Prevents {stun} while active.',
    color: 'text-emerald-400',
    category: 'status',
    icon: '\u{1F6E1}',
  },
  {
    id: 'exhaustion',
    name: 'Exhaustion',
    description: 'Movement speed reduced by 30%. Cannot sprint. Cleared when {mana} reaches full.',
    color: 'text-zinc-400',
    category: 'status',
    icon: '\u{1F4A4}',
  },
];

const KEYWORD_MAP = new Map(KEYWORDS.map(k => [k.id, k]));

// ===== Mock Item Data =====

const ITEMS: ItemDef[] = [
  {
    id: 'flamebrand',
    name: 'Flamebrand Sword',
    icon: '\u{1F5E1}',
    rarity: 'legendary',
    description: 'Deals {fire_damage} on hit. Critical strikes apply {burn} for 3 seconds. Pairs well with {fire_amulet}.',
    stats: {
      'Attack Power': '+142',
      'Fire Damage': '+38',
      'Critical Rate': '+12%',
    },
  },
  {
    id: 'arcane_blast',
    name: 'Arcane Blast',
    icon: '\u{1F4DC}',
    rarity: 'epic',
    description: 'Consumes 45 {mana} to deal {arcane_damage}. Can be combo-cast after {fireball} for {elemental_reaction}.',
    stats: {
      'Damage': '180-220',
      'Mana Cost': '45',
      'Cooldown': '6s',
    },
  },
  {
    id: 'wardens_resolve',
    name: "Warden's Resolve",
    icon: '\u{1F6E1}',
    rarity: 'rare',
    description: 'Blocks the next attack. If the blocked attack would apply {stun}, reflects it back to the attacker. Grants {fortify} for 5 seconds.',
    stats: {
      'Block Value': '350',
      'Duration': '3s',
      'Cooldown': '12s',
    },
  },
];

// ===== Demo Component =====

export function NestedTooltipDemo() {
  const [smoke, setSmoke] = useState<SmokeParams>({ ...DEFAULT_SMOKE_PARAMS });
  const [panelOpen, setPanelOpen] = useState(true);

  const s = useCallback((key: keyof SmokeParams, min: number, max: number, step: number) => (
    <label key={key} className="flex items-center gap-2">
      <span className="text-[10px] text-zinc-400 w-24 shrink-0">{key}</span>
      <input type="range" min={min} max={max} step={step} value={smoke[key]}
        onChange={e => setSmoke(prev => ({ ...prev, [key]: parseFloat(e.target.value) }))}
        className="w-24 h-1 accent-zinc-400 cursor-pointer" />
      <span className="text-[10px] text-zinc-300 w-10 text-right font-mono">
        {smoke[key].toFixed(2)}
      </span>
    </label>
  ), [smoke]);

  return (
    <PinnedTooltipProvider keywords={KEYWORD_MAP}>
      <div className="w-full h-full flex flex-col items-center justify-center gap-6 p-8">
        {/* Smoke tuning panel — top right */}
        <div className="fixed top-3 right-3 z-[1000] bg-zinc-800 border border-zinc-700 rounded-lg
                        px-3 py-2 select-none max-h-[90vh] overflow-y-auto">
          <div className="flex items-center justify-between mb-2 cursor-pointer"
               onClick={() => setPanelOpen(p => !p)}>
            <span className="text-[10px] text-zinc-400 font-bold">SMOKE PARAMS</span>
            <span className="text-[10px] text-zinc-500">{panelOpen ? '\u25B2' : '\u25BC'}</span>
          </div>
          {panelOpen && (
            <div className="flex flex-col gap-1.5">
              {s('cloudScale', 0.5, 10, 0.1)}
              {s('noiseScale', 0.5, 10, 0.1)}
              {s('cloudSpeedX', -30, 30, 1)}
              {s('cloudSpeedY', -30, 30, 1)}
              {s('noiseSpeedX', -30, 30, 1)}
              {s('noiseSpeedY', -30, 30, 1)}
              <div className="h-px bg-zinc-700 my-1" />
              {s('gamma', 0.1, 3, 0.05)}
              {s('noiseStrength', 0, 1, 0.05)}
              {s('fadePower', 0.1, 5, 0.1)}
              <button className="mt-1 text-[10px] text-zinc-500 hover:text-zinc-300 text-left"
                onClick={() => setSmoke({ ...DEFAULT_SMOKE_PARAMS })}>
                Reset defaults
              </button>
            </div>
          )}
        </div>

        {/* Title */}
        <div className="text-center mb-2">
          <h1 className="text-xl font-bold text-zinc-200 mb-1">Nested Tooltip Demo</h1>
          <p className="text-xs text-zinc-500">
            Hover keywords for tooltips — hold 3s to pin as floating window
          </p>
        </div>

        {/* Item Cards */}
        <div className="flex gap-6 flex-wrap justify-center">
          {ITEMS.map(item => (
            <ItemCard key={item.id} item={item} keywords={KEYWORD_MAP} smokeParams={smoke} />
          ))}
        </div>

        {/* Legend */}
        <div className="flex flex-wrap gap-3 mt-4">
          {KEYWORDS.map(kw => (
            <span key={kw.id} className={`text-[10px] ${kw.color} opacity-60`}>
              {kw.name}
            </span>
          ))}
        </div>
      </div>

      {/* Pinned floating tooltip windows */}
      <PinnedTooltipLayer />
    </PinnedTooltipProvider>
  );
}
