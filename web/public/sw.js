// The site's Service Worker (README.md): the emulator's module, op1emu-web.<hash>.wasm,
// served from Cache Storage once it is there, so that Chrome keeps its
// compiled code (V8's wasm code cache keeps what it compiled of a response
// that comes from Cache Storage through a Service Worker, at its own URL; of
// one the page makes itself, nothing). The Worker asks for it to be cached
// (a "cache" message, after it started the emulator), and fetches it from
// then on: the first visit downloads the brotli-compressed .wasm.br instead.
// LLVM's deferred module, op1emu-web.<hash>.deferred.wasm, which the tier
// Worker (compile-worker.js) compiles on every start, is kept the first time
// it is fetched, for the same reason. Every other request goes to the
// network, as without it.

const CACHE = "op1emu-module";
const MODULE = /\/op1emu-web\.([0-9a-f]{12})(\.deferred)?\.wasm$/;

self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (event) => event.waitUntil(self.clients.claim()));

self.addEventListener("fetch", (event) => {
  const url = new URL(event.request.url);
  if (event.request.method !== "GET" || !MODULE.test(url.pathname)) return;
  const deferred = Boolean(MODULE.exec(url.pathname)[2]);
  event.respondWith((async () => {
    const cache = await caches.open(CACHE);
    const cached = await cache.match(url.href);
    if (cached) return cached;
    const response = await fetch(event.request);
    if (deferred && response.ok) event.waitUntil(cache.put(url.href, response.clone()).catch(() => {}));
    return response;
  })());
});

// { type: "cache", url }: that module kept, the others of other builds the
// cache holds dropped; { type: "cached", url } answers on the port whether
// it is there.
self.addEventListener("message", (event) => {
  const { type, url } = event.data ?? {};
  const port = event.ports[0];
  if (type === "cache") {
    event.waitUntil((async () => {
      const cache = await caches.open(CACHE);
      const build = MODULE.exec(new URL(url).pathname)?.[1];
      for (const request of await cache.keys())
        if (MODULE.exec(new URL(request.url).pathname)?.[1] !== build) await cache.delete(request);
      if (!(await cache.match(url))) await cache.add(url);
    })().catch((e) => console.warn("the module was not cached:", e)));
  } else if (type === "cached") {
    event.waitUntil((async () => port?.postMessage(Boolean(await (await caches.open(CACHE)).match(url))))());
  }
});
