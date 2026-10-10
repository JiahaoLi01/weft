import { NestedTooltipDemo } from '@/components/NestedTooltipDemo';
import { BackButton } from '@/components/ui/BackButton';

export function TooltipDemo() {
  return (
    <div className="w-full h-full bg-zinc-950">
      <BackButton />
      <NestedTooltipDemo />
    </div>
  );
}
