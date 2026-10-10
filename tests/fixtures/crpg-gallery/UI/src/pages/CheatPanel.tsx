import { useState, useCallback, useEffect, useMemo } from 'react';
import { sendToUE } from '@/lib/ue-bridge';
import { useUIStore } from '@/stores/ui-store';
import { useUEHandler } from '@/hooks/use-ue-handler';

/**
 * 单个参数描述 —— 来自 C++ `BuildCheatsJson` 的 type 字段。
 * UE 反射对应：FStrProperty / FNameProperty / FIntProperty / FFloatProperty / FBoolProperty
 */
interface CheatParam {
  name: string;
  type: 'string' | 'name' | 'int' | 'float' | 'bool' | 'unknown';
}

interface CheatCommand {
  name: string;
  category: string;
  params: CheatParam[];
}

interface CheatsPayload {
  commands: CheatCommand[];
}

// 项目里 EB Garamond 在 fonts.ts 已注册 @font-face；维多利亚风格的衬线字体，
// 用在 cheat 面板的函数名 + 参数 label + 参数输入框，与游戏整体字体调性一致。
// CJK fall-through 链按 fonts.ts['eb-garamond'].stack 对齐。
const GARAMOND_STACK = "'EB Garamond', 'LXGW NeoZhiSong Plus', 'Noto Serif SC', 'Times New Roman', serif";

/**
 * Dev cheat panel —— 半透明 WebUI 覆盖层，列出 PlayerController.CheatClass 上所有
 * `UFUNCTION(Exec)`。每条命令给一行：命令名 + 按参数生成的 input + Run 按钮。
 *
 * 触发：控制台输入 `OpenCheatPanel`（前提 `EnableCheats` 已激活，非 shipping）。
 * 协议：C++ 通过 `loadCheats` event push 命令列表；点击 Run → `sendToUE('cheat_invoke', { commandLine })`
 * → 走 `PlayerController.ConsoleCommand` 路由到 cheat 函数。
 */
export function CheatPanel() {
  const [commands, setCommands] = useState<CheatCommand[]>([]);
  const [argsByCmd, setArgsByCmd] = useState<Record<string, Record<string, string>>>({});
  const pop = useUIStore(s => s.pop);

  useUEHandler(
    'loadCheats',
    useCallback((data: unknown) => {
      const payload = data as CheatsPayload;
      setCommands(payload.commands ?? []);
    }, []),
  );

  // mount 时主动向 C++ 请求最新命令列表。
  // 避免 lazy-loaded 组件在 C++ 推 loadCheats 之前还没 mount → handler 没注册 → 消息丢失的 race。
  // 同样的模式可参考 Settings 页 ↔ request_settings。
  useEffect(() => {
    sendToUE('request_cheats');
  }, []);

  // 按 Category 分组；Category 是 C++ `UFUNCTION` metadata，缺省为 "Misc"
  const grouped = useMemo(() => {
    const groups = new Map<string, CheatCommand[]>();
    for (const cmd of commands) {
      const cat = cmd.category || 'Misc';
      if (!groups.has(cat)) groups.set(cat, []);
      groups.get(cat)!.push(cmd);
    }
    return [...groups.entries()].sort(([a], [b]) => a.localeCompare(b));
  }, [commands]);

  const updateArg = (cmdName: string, paramName: string, value: string) => {
    setArgsByCmd(prev => ({
      ...prev,
      [cmdName]: { ...(prev[cmdName] ?? {}), [paramName]: value },
    }));
  };

  /**
   * 拼装控制台命令行。约定：
   *  - string/name 含空格 → 加双引号
   *  - 数值类型 → 原样
   *  - bool → "true" / "false"（PC.ConsoleCommand 接受这两个字面值）
   *  - 空字符串 → 留空，UE 会拿默认值
   */
  const buildCommandLine = (cmd: CheatCommand): string => {
    const args = argsByCmd[cmd.name] ?? {};
    const parts: string[] = [cmd.name];
    for (const p of cmd.params) {
      const raw = (args[p.name] ?? '').trim();
      if (raw === '') {
        // 不传参，让 UE 用 default
        continue;
      }
      if ((p.type === 'string' || p.type === 'name') && raw.includes(' ')) {
        parts.push(`"${raw}"`);
      } else {
        parts.push(raw);
      }
    }
    return parts.join(' ');
  };

  const runCommand = (cmd: CheatCommand) => {
    const commandLine = buildCommandLine(cmd);
    sendToUE('cheat_invoke', { commandLine });
  };

  return (
    <div
      className="absolute inset-0 flex items-center justify-center"
      style={{ background: 'rgba(0, 0, 0, 0.55)' }}
    >
      <div className="w-[min(960px,92vw)] max-h-[88vh] bg-zinc-900/95 border border-zinc-700 rounded-lg shadow-2xl flex flex-col">
        {/* Header */}
        <div className="flex items-center justify-between px-5 py-3 border-b border-zinc-700">
          <div>
            <h1 className="text-lg font-bold text-zinc-100">Cheat Panel</h1>
            <p className="text-xs text-zinc-500 mt-0.5">
              {commands.length} commands · auto-discovered from CheatManager via reflection
            </p>
          </div>
          <button
            type="button"
            onClick={pop}
            className="w-8 h-8 flex items-center justify-center rounded text-zinc-400 hover:text-zinc-100 hover:bg-zinc-700"
            aria-label="Close"
          >
            ✕
          </button>
        </div>

        {/* Body */}
        <div className="flex-1 overflow-y-auto p-5 space-y-6">
          {grouped.length === 0 && (
            <div className="text-center text-zinc-500 text-sm py-12">
              No cheat commands. Make sure `EnableCheats` is active and the CheatClass exposes UFUNCTION(Exec) methods.
            </div>
          )}

          {grouped.map(([category, cmds]) => (
            <section key={category}>
              <h2 className="text-xs font-semibold uppercase tracking-wider text-zinc-400 mb-2">
                {category}
              </h2>
              <div className="space-y-1.5">
                {cmds.map(cmd => (
                  <div
                    key={cmd.name}
                    className="flex items-center gap-3 px-3 py-2 rounded bg-zinc-800/60 hover:bg-zinc-800 border border-zinc-700/50"
                  >
                    <div
                      className="text-base text-zinc-100 min-w-[200px]"
                      style={{ fontFamily: GARAMOND_STACK }}
                    >
                      {cmd.name}
                    </div>
                    <div className="flex-1 flex items-center flex-wrap gap-2">
                      {cmd.params.length === 0 ? (
                        <span className="text-xs text-zinc-500 italic">no args</span>
                      ) : (
                        cmd.params.map(p => (
                          <ParamInput
                            key={p.name}
                            param={p}
                            value={argsByCmd[cmd.name]?.[p.name] ?? ''}
                            onChange={v => updateArg(cmd.name, p.name, v)}
                          />
                        ))
                      )}
                    </div>
                    <button
                      type="button"
                      onClick={() => runCommand(cmd)}
                      className="px-3 py-1 text-xs font-medium rounded bg-emerald-600 hover:bg-emerald-500 text-white"
                    >
                      Run
                    </button>
                  </div>
                ))}
              </div>
            </section>
          ))}
        </div>

        {/* Footer */}
        <div className="px-5 py-2.5 border-t border-zinc-700 text-[11px] text-zinc-500">
          Routed via <span className="font-mono">PlayerController.ConsoleCommand</span> ·{' '}
          <span className="font-mono">EnableCheats</span> must already be active.
        </div>
      </div>
    </div>
  );
}

interface ParamInputProps {
  param: CheatParam;
  value: string;
  onChange: (v: string) => void;
}

function ParamInput({ param, value, onChange }: ParamInputProps) {
  if (param.type === 'bool') {
    return (
      <label
        className="flex items-center gap-1.5 text-sm text-zinc-300"
        style={{ fontFamily: GARAMOND_STACK }}
      >
        <input
          type="checkbox"
          checked={value === 'true' || value === '1'}
          onChange={e => onChange(e.target.checked ? 'true' : 'false')}
          className="rounded"
        />
        {param.name}
      </label>
    );
  }

  const isNumber = param.type === 'int' || param.type === 'float';
  return (
    <div className="flex items-center gap-1.5">
      <label
        className="text-xs text-zinc-400"
        style={{ fontFamily: GARAMOND_STACK }}
      >
        {param.name}
      </label>
      <input
        type={isNumber ? 'number' : 'text'}
        step={param.type === 'float' ? '0.1' : '1'}
        value={value}
        onChange={e => onChange(e.target.value)}
        placeholder={param.type}
        className="px-2 py-1 text-sm bg-zinc-900 border border-zinc-600 rounded text-zinc-100 placeholder:text-zinc-500 focus:outline-none focus:border-emerald-500 min-w-[120px]"
        style={{ fontFamily: GARAMOND_STACK }}
      />
    </div>
  );
}
