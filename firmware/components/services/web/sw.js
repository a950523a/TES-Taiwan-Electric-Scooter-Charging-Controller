const CACHE_NAME = 'tes-v2';
const SHELL = ['/', '/manifest.json', '/icon.svg'];

self.addEventListener('install', e => {
  e.waitUntil(
    caches.open(CACHE_NAME).then(c => c.addAll(SHELL))
  );
  self.skipWaiting();
});

self.addEventListener('activate', e => {
  e.waitUntil(
    caches.keys().then(keys =>
      Promise.all(keys.filter(k => k !== CACHE_NAME).map(k => caches.delete(k)))
    )
  );
  self.clients.claim();
});

self.addEventListener('fetch', e => {
  const path = new URL(e.request.url).pathname;

  // API endpoints: network-first, silent offline error (JS handles --)
  if (['/status', '/config', '/history', '/wifi/scan'].includes(path)) {
    e.respondWith(
      fetch(e.request).catch(() =>
        new Response('{"error":"offline"}', {headers: {'Content-Type': 'application/json'}})
      )
    );
    return;
  }

  // App shell: cache-first, network fallback, then offline fallback to cached /
  if (SHELL.includes(path)) {
    e.respondWith(
      caches.open(CACHE_NAME).then(async cache => {
        const cached = await cache.match(e.request);
        try {
          const fresh = await fetch(e.request);
          if (fresh.ok) cache.put(e.request, fresh.clone());
          return fresh;
        } catch (_) {
          // Offline: serve from cache (SW-registered context gets true offline support)
          return cached || new Response('Offline', {status: 503});
        }
      })
    );
    return;
  }

  e.respondWith(fetch(e.request));
});
