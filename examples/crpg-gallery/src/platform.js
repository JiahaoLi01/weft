// Authored test host only. No fake DOM, CSS, Canvas or WebGL APIs.
(() => {
  const events=[];
  const capture=(name,data)=>events.push({name:String(name),data:JSON.parse(JSON.stringify(data ?? {}))});
  window.ue={sendEvent:capture,interface:{broadcast:(name,data)=>capture(name,data)}};
  globalThis.__ceGalleryBridge=events;
  // Defense in depth: an original bridge edit must not connect to a live game.
  globalThis.WebSocket=class DisabledGameSocket {
    constructor(){throw new Error('CE_GALLERY_NETWORK: game socket disabled');}
  };
})();
