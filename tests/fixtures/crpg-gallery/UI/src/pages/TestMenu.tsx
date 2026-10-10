import { useUIStore } from '@/stores/ui-store';

interface TestEntry {
  page: string;
  title: string;
  desc: string;
  color: string; // border + accent color class
}

const TESTS: TestEntry[] = [
  {
    page: 'test-gallery',
    title: 'Feature Tests',
    desc: 'CSS, D3 charts, Three.js, Framer Motion, React Spring, Lucide icons',
    color: 'border-amber-500 text-amber-400',
  },
  {
    page: 'tooltip-demo',
    title: 'Tooltip Tests',
    desc: 'Nested tooltips, pin-to-float, drag, smoke background, DPI scaling',
    color: 'border-purple-500 text-purple-400',
  },
  {
    page: 'mvvm-test',
    title: 'MVVM Tests',
    desc: 'ViewModel binding, FieldNotify push, JS↔UE events, CallUI echo',
    color: 'border-sky-500 text-sky-400',
  },
  {
    page: 'settings',
    title: 'Settings',
    desc: 'Audio, video, input, accessibility — JSON schema driven UI',
    color: 'border-emerald-500 text-emerald-400',
  },
];

/**
 * Dev-only test hub — a grid of links into the feature-test pages.
 * Not the real game main menu; page id is `test-menu` in the UI stack.
 */
export function TestMenu() {
  const push = useUIStore(s => s.push);

  return (
    <div className="w-full h-full flex items-center justify-center">
      <div className="flex flex-col items-center gap-8">
        {/* Title */}
        <div className="text-center mb-2">
          <div className="text-[10px] uppercase tracking-[0.3em] text-zinc-500 mb-2">
            CRPGUltralight
          </div>
          <h1 className="text-3xl font-bold text-zinc-200">
            WebUI Test Hub
          </h1>
          <p className="text-xs text-zinc-500 mt-2">
            Select a test suite to enter
          </p>
        </div>

        {/* Test entries */}
        <div className="flex flex-col gap-3 w-96">
          {TESTS.map(t => (
            <button
              key={t.page}
              // 显式 cursor-pointer:虽然 Tailwind v4 preflight 会给 <button> 默认设
              // cursor: pointer,但这里观察到 Ultralight 报 kCursor_Pointer(默认箭头)
              // 进而被 UMG 判成非交互区。原因可能是 preflight 没被 import 或被项目 css
              // 覆盖。显式写出来最安全,也让 per-pixel hit-test 一目了然能识别。
              className={`group relative text-left rounded-lg border px-5 py-4
                         bg-zinc-900 hover:bg-zinc-800 transition-all duration-200
                         cursor-pointer select-none
                         ${t.color.split(' ')[0]}`}
              onClick={() => push(t.page)}
            >
              <div className={`text-base font-semibold mb-0.5 ${t.color.split(' ')[1]}`}>
                {t.title}
              </div>
              <div className="text-[11px] text-zinc-500 group-hover:text-zinc-400 transition-colors">
                {t.desc}
              </div>
              <div className="absolute right-4 top-1/2 -translate-y-1/2 text-zinc-600
                              group-hover:text-zinc-400 transition-colors">
                &rarr;
              </div>
            </button>
          ))}
        </div>

        {/* Footer */}
        <div className="text-[9px] uppercase tracking-widest text-zinc-700 mt-4">
          Build 0.6.1 &middot; UE 5.6 &middot; Ultralight 1.4.0
        </div>
      </div>
    </div>
  );
}
