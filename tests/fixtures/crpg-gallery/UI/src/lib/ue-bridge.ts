/**
 * UE Bridge — Type-safe communication layer between React UI and Unreal Engine.
 *
 * Communication channels (tried in order):
 *   1. WebSocket (ws://127.0.0.1:8765) — primary, works regardless of CEF CefApp
 *   2. CEF ue.interface binding — fallback, requires plugin's CefApp
 *   3. Console log — dev mode only
 *
 * Protocol (WebSocket JSON messages):
 *   JS → UE:  {"type":"event","name":"xxx","data":{...}}
 *   UE → JS:  {"type":"call","function":"xxx","data":{...}}
 */

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

/** Callback registered for UE→JS calls. */
type UECallback = (data: unknown) => void;

interface UEInterface {
  broadcast: (name: string, data?: string) => void;
  [key: string]: UECallback | object | undefined;
}

interface UEGlobal {
  interface: UEInterface;
}

declare global {
  interface Window {
    ue?: UEGlobal;
    ue5?: (name: string, data?: object, callback?: UECallback, timeout?: number) => void;
  }
}

// ---------------------------------------------------------------------------
// WebSocket connection
// ---------------------------------------------------------------------------

const WS_PORT = 8765;
const WS_URL = `ws://127.0.0.1:${WS_PORT}`;
const WS_RECONNECT_INTERVAL = 2000;

let ws: WebSocket | null = null;
let wsConnected = false;
let reconnectTimer: ReturnType<typeof setTimeout> | null = null;

/** Map of registered handler functions (UE → JS). */
const registeredHandlers = new Map<string, UECallback>();

function connectWebSocket(): void {
  if (ws?.readyState === WebSocket.OPEN || ws?.readyState === WebSocket.CONNECTING) {
    return;
  }

  try {
    ws = new WebSocket(WS_URL);
  } catch {
    scheduleReconnect();
    return;
  }

  ws.onopen = () => {
    wsConnected = true;
    console.log(`[UE Bridge] WebSocket connected to ${WS_URL}`);

    // Send ui_ready once connected
    sendToUE('ui_ready');
  };

  ws.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data as string) as {
        type: string;
        function?: string;
        data?: unknown;
      };

      if (msg.type === 'call' && msg.function) {
        const handler = registeredHandlers.get(msg.function);
        if (handler) {
          handler(msg.data);
        } else {
          console.warn(`[UE Bridge] No handler for "${msg.function}"`);
        }
      }
    } catch (e) {
      console.error('[UE Bridge] Failed to parse WS message:', e);
    }
  };

  ws.onclose = () => {
    wsConnected = false;
    console.log('[UE Bridge] WebSocket disconnected');
    scheduleReconnect();
  };

  ws.onerror = () => {
    // onclose will fire after this
  };
}

function scheduleReconnect(): void {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => {
    reconnectTimer = null;
    connectWebSocket();
  }, WS_RECONNECT_INTERVAL);
}

/** Initialize the WebSocket connection. Call once at app startup. */
export function initWebSocket(): void {
  // Ultralight 直连桥(window.ue.sendEvent,C++ 在 OnWindowObjectReady 注入)存在时
  // 跳过 WS:UE 侧并没有 :8765 服务端,连不上的 2s 重连循环只会往 Output Log 刷
  // "WebSocket network error",把真正的 JS 日志淹掉。WS 仅保留给浏览器 dev 模式
  // (window.ue.sendEvent 不存在)做替代宿主。
  if (typeof (window as unknown as { ue?: { sendEvent?: unknown } }).ue?.sendEvent === 'function') {
    return;
  }
  connectWebSocket();
}

/** Close WebSocket and stop reconnecting. */
export function destroyWebSocket(): void {
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  if (ws) {
    ws.onclose = null; // prevent reconnect
    ws.close();
    ws = null;
  }
  wsConnected = false;
}

// ---------------------------------------------------------------------------
// Runtime detection
// ---------------------------------------------------------------------------

/** True when WebSocket to UE is connected. */
export function isConnectedToUE(): boolean {
  return wsConnected;
}

/** True when running inside UE's CEF browser (legacy check). */
export function isInUnreal(): boolean {
  return wsConnected || (typeof window.ue === 'object' && typeof window.ue?.interface === 'object');
}

// ---------------------------------------------------------------------------
// JS → UE  (send events)
// ---------------------------------------------------------------------------

/**
 * Send a named event with optional JSON payload to Unreal Engine.
 * Channels tried in order: Ultralight direct → WebSocket → CEF → console.
 */
export function sendToUE(name: string, data?: Record<string, unknown>): void {
  // Primary: Ultralight direct bridge (injected by C++ via JSCore)
  if (typeof (window as any).ue?.sendEvent === 'function') {
    (window as any).ue.sendEvent(name, data ?? {});
    return;
  }

  // WebSocket
  if (ws?.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type: 'event', name, data: data ?? {} }));
    return;
  }

  // Fallback: CEF ue5() / ue.interface.broadcast
  if (typeof window.ue5 === 'function') {
    window.ue5(name, data);
    return;
  }
  if (typeof window.ue?.interface?.broadcast === 'function') {
    const jsonStr = data !== undefined ? JSON.stringify(data) : '';
    window.ue.interface.broadcast(name, jsonStr);
    return;
  }

  // Dev mode
  console.log(`[UE Bridge] sendToUE("${name}")`, data ?? '');
}

/**
 * Send an event to UE and wait for a response via WebSocket.
 */
export function requestFromUE<T = unknown>(
  name: string,
  data?: Record<string, unknown>,
  _timeoutMs: number = 3000,
): Promise<T> {
  return new Promise((_resolve, reject) => {
    if (!wsConnected) {
      reject(new Error('WebSocket not connected'));
      return;
    }

    // For now, simple fire-and-forget. Request/response can be added later.
    sendToUE(name, data);
    reject(new Error('requestFromUE not yet implemented over WebSocket'));
  });
}

// ---------------------------------------------------------------------------
// UE → JS  (register handlers)
// ---------------------------------------------------------------------------

/**
 * Register a handler that UE can invoke via WebSocket or CEF Call node.
 * Returns a cleanup function to unregister.
 */
export function onUECall(fnName: string, handler: UECallback): () => void {
  // Register in our WebSocket handler map
  registeredHandlers.set(fnName, handler);

  // Also register on ue.interface for CEF fallback
  if (typeof window.ue !== 'object') {
    (window as any).ue = { interface: {} };
  }
  if (typeof window.ue!.interface !== 'object') {
    window.ue!.interface = { broadcast: () => {} } as UEInterface;
  }
  window.ue!.interface[fnName] = handler;

  const cleanup = () => {
    registeredHandlers.delete(fnName);
    if (window.ue?.interface?.[fnName] === handler) {
      delete window.ue.interface[fnName];
    }
  };

  return cleanup;
}

/**
 * Remove all registered UE→JS handlers.
 */
export function clearAllHandlers(): void {
  for (const [fnName] of registeredHandlers) {
    if (window.ue?.interface?.[fnName]) {
      delete window.ue.interface[fnName];
    }
  }
  registeredHandlers.clear();
}

// ---------------------------------------------------------------------------
// Ultralight direct bridge: UE→JS dispatcher
// ---------------------------------------------------------------------------

// Register global dispatcher that C++ calls via View->EvaluateScript().
// C++ calls: window.__ue_dispatch('functionName', parsedJsonData)
(window as any).__ue_dispatch = (fnName: string, data: unknown) => {
  const handler = registeredHandlers.get(fnName);
  if (handler) {
    handler(data);
  } else {
    console.warn(`[UE Bridge] No handler for UE→JS call "${fnName}"`);
  }
};
