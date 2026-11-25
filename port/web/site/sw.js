// The service worker, in the usual shape for a site that must start offline:
// - the game and the importer live in a folder named after their contents (build-<hash>/, by build.sh): those are
//   answered from the cache first, since a new build's page asks for another folder;
// - everything else (the page and its small scripts) from the network first, a few seconds at most, else the cache;
// - a new worker takes over at once (skipWaiting, clients.claim) and drops the other builds' caches.
// A tab that is playing keeps the game it loaded: its files are all in memory once it started.
'use strict';
const VERSION = '@VERSION@';
const FILES = ['./', ...@FILES@];
const CACHE = 'reg2-site-' + VERSION;
const NETWORK_WAIT_MS = 3000;

self.addEventListener('install', (event) => {
  event.waitUntil((async () => {
    const cache = await caches.open(CACHE);
    await cache.addAll(FILES.map((f) => new Request(f, { cache: 'reload' })));
    await self.skipWaiting();
  })());
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    for (const name of await caches.keys()) {
      if (name.startsWith('reg2-site-') && name !== CACHE) await caches.delete(name);
    }
    await self.clients.claim();
  })());
});

async function networkFirst(request) {
  const key = request.mode === 'navigate' ? './' : request;
  try {
    const response = await Promise.race([
      fetch(request),
      new Promise((_, reject) => setTimeout(() => reject(new Error('slow')), NETWORK_WAIT_MS)),
    ]);
    if (response.ok) (await caches.open(CACHE)).put(key, response.clone());
    return response;
  } catch {
    const cached = await caches.match(key, { ignoreSearch: true });
    if (cached) return cached;
    if (request.mode !== 'navigate') return Response.error();
    return new Response('<!doctype html><title>reG2</title><p>reG2 needs the network once to start.</p>',
      { status: 503, headers: { 'content-type': 'text/html' } });
  }
}

async function cacheFirst(request) {
  const cached = await caches.match(request);
  if (cached) return cached;
  const response = await fetch(request);
  if (response.ok) (await caches.open(CACHE)).put(request, response.clone());
  return response;
}

self.addEventListener('fetch', (event) => {
  const request = event.request;
  const url = new URL(request.url);
  if (request.method !== 'GET' || url.origin !== location.origin) return;
  event.respondWith(/\/build-[0-9a-f]+\//.test(url.pathname) ? cacheFirst(request) : networkFirst(request));
});
