import { useEffect, useCallback } from 'react';
import { onUECall } from '@/lib/ue-bridge';

/**
 * Register a UE→JS handler scoped to a component's lifetime.
 * Handler is added on mount and removed on unmount.
 *
 * Usage:
 *   useUEHandler('setHealth', (data) => {
 *     const { current, max } = data as { current: number; max?: number };
 *     useGameStore.getState().setHealth(current, max);
 *   });
 */
export function useUEHandler(name: string, handler: (data: unknown) => void): void {
  // Stable reference — caller should wrap handler in useCallback if it has deps
  const stableHandler = useCallback(handler, [handler]);

  useEffect(() => {
    const cleanup = onUECall(name, stableHandler);
    return cleanup;
  }, [name, stableHandler]);
}
