interface VitalBarProps {
  label: string;
  current: number;
  max: number;
  color: string;
  bgColor?: string;
}

export function VitalBar({ label, current, max, color, bgColor = 'bg-panel' }: VitalBarProps) {
  const pct = max > 0 ? Math.min((current / max) * 100, 100) : 0;

  return (
    <div className="flex flex-col gap-0.5">
      <div className="flex justify-between text-xs text-parchment-dark">
        <span>{label}</span>
        <span>{Math.round(current)} / {max}</span>
      </div>
      <div className={`h-3 rounded-sm ${bgColor} overflow-hidden border border-panel-border`}>
        <div
          className={`h-full ${color} transition-all duration-300 ease-out`}
          style={{ width: `${pct}%` }}
        />
      </div>
    </div>
  );
}
