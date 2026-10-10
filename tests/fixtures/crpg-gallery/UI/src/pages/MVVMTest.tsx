import { useState, useCallback, useEffect, useRef } from 'react';
import { sendToUE } from '@/lib/ue-bridge';
import { useUEHandler } from '@/hooks/use-ue-handler';
import { BackButton } from '@/components/ui/BackButton';

/**
 * MVVM test page — displays ViewModel data pushed from C++ via __vm_update,
 * and buttons that send events back to UE.
 */
export function MVVMTest() {
  const [vmData, setVmData] = useState<Record<string, number>>({});
  const [log, setLog] = useState<string[]>(['UI ready']);
  const [pingCount, setPingCount] = useState(0);
  const setVmDataRef = useRef(setVmData);
  setVmDataRef.current = setVmData;

  const appendLog = useCallback((msg: string) => {
    setLog(prev => [...prev.slice(-50), `[${new Date().toLocaleTimeString()}] ${msg}`]);
  }, []);

  // __vm_update is called by C++ via View->EvaluateScript(), not through
  // the __ue_dispatch handler system. Must set it on window directly.
  useEffect(() => {
    const prev = window.__vm_update;
    window.__vm_update = (delta: Record<string, unknown>) => {
      setVmDataRef.current(d => {
        const next = { ...d };
        for (const [k, v] of Object.entries(delta)) {
          if (typeof v === 'number') {
            next[k] = v;
          }
        }
        return next;
      });
    };
    return () => { window.__vm_update = prev; };
  }, []);

  // UE → JS: receive CallUI dispatches
  useUEHandler('pingReply', useCallback((data: unknown) => {
    appendLog(`UE→pingReply: ${JSON.stringify(data)}`);
  }, [appendLog]));

  const sendPing = useCallback(() => {
    const count = pingCount + 1;
    setPingCount(count);
    sendToUE('test_ping', { count, ts: Date.now() });
    appendLog(`Sent test_ping #${count}`);
  }, [pingCount, appendLog]);

  const sendHello = useCallback(() => {
    sendToUE('hello', { msg: 'Hello from JS!' });
    appendLog('Sent hello');
  }, [appendLog]);

  return (
    <div className="w-full h-full flex items-center justify-center bg-zinc-950">
      <BackButton />

      <div className="bg-zinc-900 border border-zinc-700 rounded-lg p-6 w-[420px]">
        <h1 className="text-lg font-bold text-zinc-200 mb-1">MVVM Test</h1>
        <p className="text-[10px] text-zinc-500 mb-4">ViewModel push + JS↔UE events</p>

        {/* ViewModel data */}
        <div className="mb-4">
          <h2 className="text-xs font-semibold text-sky-400 mb-2">ViewModel (MVVM)</h2>
          {Object.keys(vmData).length === 0 ? (
            <div className="text-xs text-zinc-600 italic">Waiting for ViewModel data...</div>
          ) : (
            Object.entries(vmData).map(([key, val]) => (
              <div key={key} className="flex justify-between text-sm py-0.5 border-b border-zinc-800">
                <span className="text-zinc-500">{key}</span>
                <span className="text-zinc-200 font-mono">{typeof val === 'number' ? val.toFixed(1) : String(val)}</span>
              </div>
            ))
          )}
          {vmData.TestFloatValue !== undefined && (
            <div className="mt-1 h-1.5 bg-zinc-800 rounded-full overflow-hidden">
              <div
                className="h-full bg-sky-500 rounded-full transition-all duration-300"
                style={{ width: `${Math.min(100, vmData.TestFloatValue)}%` }}
              />
            </div>
          )}
        </div>

        {/* Event buttons */}
        <div className="mb-4">
          <h2 className="text-xs font-semibold text-emerald-400 mb-2">Events</h2>
          <div className="flex gap-2">
            <button
              className="px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-xs text-zinc-300
                         hover:bg-zinc-700 transition-colors"
              onClick={sendPing}
            >
              Send test_ping
            </button>
            <button
              className="px-3 py-1.5 bg-zinc-800 border border-zinc-700 rounded text-xs text-zinc-300
                         hover:bg-zinc-700 transition-colors"
              onClick={sendHello}
            >
              Send hello
            </button>
          </div>
        </div>

        {/* Log */}
        <div>
          <h2 className="text-xs font-semibold text-amber-400 mb-2">UE ↔ JS Log</h2>
          <div className="bg-zinc-950 rounded p-2 max-h-32 overflow-y-auto font-mono text-[10px] text-emerald-400/80">
            {log.map((entry, i) => (
              <div key={i}>{entry}</div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}

// Type declaration for __vm_update
declare global {
  interface Window {
    __vm_update?: (delta: Record<string, unknown>) => void;
  }
}
