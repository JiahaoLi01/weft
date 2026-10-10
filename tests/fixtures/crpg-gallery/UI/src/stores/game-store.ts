import { create } from 'zustand';

/**
 * Core game state store.
 * UE pushes updates here via the UE Bridge → onUECall handlers.
 * React components subscribe to slices they need.
 */

export interface GameState {
  // Connection
  connected: boolean;

  // Player vitals
  health: number;
  maxHealth: number;
  mana: number;
  maxMana: number;
  stamina: number;
  maxStamina: number;

  // Player info
  playerName: string;
  level: number;

  // Actions
  setConnected: (connected: boolean) => void;
  setHealth: (health: number, maxHealth?: number) => void;
  setMana: (mana: number, maxMana?: number) => void;
  setStamina: (stamina: number, maxStamina?: number) => void;
  setPlayerInfo: (name: string, level: number) => void;
}

export const useGameStore = create<GameState>((set) => ({
  // Initial state
  connected: false,
  health: 100,
  maxHealth: 100,
  mana: 50,
  maxMana: 50,
  stamina: 80,
  maxStamina: 80,
  playerName: 'Adventurer',
  level: 1,

  // Actions
  setConnected: (connected) => set({ connected }),

  setHealth: (health, maxHealth) =>
    set((s) => ({ health, maxHealth: maxHealth ?? s.maxHealth })),

  setMana: (mana, maxMana) =>
    set((s) => ({ mana, maxMana: maxMana ?? s.maxMana })),

  setStamina: (stamina, maxStamina) =>
    set((s) => ({ stamina, maxStamina: maxStamina ?? s.maxStamina })),

  setPlayerInfo: (playerName, level) => set({ playerName, level }),
}));
