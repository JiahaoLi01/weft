import { useUIStore } from '@/stores/ui-store';

export function BackButton() {
  const pop = useUIStore(s => s.pop);
  const hasStack = useUIStore(s => s.stack.length > 1);

  if (!hasStack) return null;

  return (
    <button
      className="fixed top-3 left-3 z-[999] flex items-center gap-1.5 px-3 py-1.5
                 bg-zinc-800 border border-zinc-700 rounded-lg
                 text-xs text-zinc-400 hover:text-zinc-200 transition-colors select-none"
      onClick={pop}
    >
      <span className="text-sm">&larr;</span>
      Back
    </button>
  );
}
