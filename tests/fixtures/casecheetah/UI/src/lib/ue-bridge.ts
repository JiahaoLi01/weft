/**
 * UE 桥 —— React 与 Unreal 之间的通信层。
 *
 * 两个方向：
 *   JS → UE : sendToUE(name, data)  →  window.ue.sendEvent（C++ 在页面脚本执行前，
 *             于 OnWindowObjectReady 注入这个全局对象）
 *   UE → JS : C++ 调 UWebWidgetInstance::CallUI(fn, json)，它经 JavaScriptCore 直接调用
 *             window.__ue_dispatch(fn, data)（数据按 JSON 解析，不当脚本源码），随后补一个微任务
 *             检查点，React 排在微任务里的渲染才会执行；本模块把它路由到 onUECall 注册的处理器。
 *
 * 浏览器里跑（npm run dev）时 window.ue 不存在，sendToUE 退化为 console 输出，
 * onUECall 依然可用 —— 可以在浏览器控制台手动调 window.__ue_dispatch(...) 调试页面。
 */

type UEHandler = (data: unknown) => void;

declare global {
  interface Window {
    ue?: { sendEvent?: (name: string, data?: unknown) => void };
    __ue_dispatch?: (fnName: string, data: unknown) => void;
  }
}

/** UE → JS 的处理器表，键是 C++ 侧 CallUI 的函数名。 */
const handlers = new Map<string, UEHandler>();

/** 是否运行在 UE 宿主里（而不是浏览器 dev 模式）。 */
export function isInUnreal(): boolean {
  return typeof window.ue?.sendEvent === 'function';
}

/** JS → UE：发送具名事件。 */
export function sendToUE(name: string, data?: Record<string, unknown>): void {
  if (typeof window.ue?.sendEvent === 'function') {
    window.ue.sendEvent(name, data ?? {});
    return;
  }
  console.log(`[UE Bridge] sendToUE("${name}")`, data ?? {});
}

/**
 * UE → JS：注册一个可被 C++ CallUI 调用的处理器，返回注销函数。
 * 同名重复注册会覆盖（React StrictMode 下会挂载两次，覆盖语义正是想要的）。
 */
export function onUECall(fnName: string, handler: UEHandler): () => void {
  handlers.set(fnName, handler);
  return () => {
    if (handlers.get(fnName) === handler) {
      handlers.delete(fnName);
    }
  };
}

// C++ 的 CallUI 直接调用它；必须在模块加载时就挂上，
// 否则 CallUI 找不到这个函数，调用被静默丢弃。
window.__ue_dispatch = (fnName: string, data: unknown) => {
  const handler = handlers.get(fnName);
  if (handler) {
    handler(data);
  } else {
    console.warn(`[UE Bridge] 没有处理器响应 UE→JS 调用 "${fnName}"`);
  }
};
