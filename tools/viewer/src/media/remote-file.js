// Downloads a ?url= recording or log into OPFS (in-memory Blob fallback) and returns a File
// for the same open path as the file chooser.
export const VIDEO_EXTENSIONS = ['.mp4', '.ts', '.mts', '.mkv', '.mov'];
export const LOG_EXTENSIONS = ['.bin', '.tlog'];
const DIRECTORY = 'url-downloads';
const LOCK_PREFIX = 'apcg-url-download:';
const UNLOCKED_MAX_AGE_MS = 24 * 3600 * 1000;
export const MEMORY_LIMIT = 1e9;

export function parseRemoteUrl(text, base) {
  let url;
  try {
    url = new URL(text, base);
  } catch {
    throw new Error(`Invalid URL: ${text}`);
  }
  if (!['http:', 'https:'].includes(url.protocol))
    throw new Error(`Only http and https URLs can be opened: ${text}`);
  if (url.protocol === 'http:' && new URL(base || url).protocol === 'https:')
    throw new Error(`An https page cannot download from http (mixed content); use https: ${text}`);
  return url;
}

export function remoteName(url) {
  const last = url.pathname.split('/').filter(Boolean).at(-1) || url.hostname;
  try {
    return decodeURIComponent(last);
  } catch {
    return last;
  }
}

// An unknown extension is rejected; no extension is left to content detection.
export function checkExtension(name, extensions, kind) {
  const match = /\.[A-Za-z0-9]{1,5}$/.exec(name);
  if (match && !extensions.includes(match[0].toLowerCase()))
    throw new Error(`${name} is not a supported ${kind} (${extensions.join(', ')}).`);
}

export function progressText(got, total, elapsedMs) {
  const mb = (n) => (n / 1e6).toFixed(1);
  if (!total || got > total) return `${mb(got)} MB`;
  let text = `${Math.floor((100 * got) / total)}% · ${mb(got)} / ${mb(total)} MB`;
  const rate = got / Math.max(elapsedMs, 1);
  if (got && elapsedMs > 1000) {
    const s = Math.round((total - got) / rate / 1000);
    text += ` · ${s >= 60 ? `${Math.floor(s / 60)} min ${s % 60} s` : `${s} s`} left`;
  }
  return text;
}

function corsError(name, url) {
  const origin = globalThis.location?.origin || 'this viewer';
  return new Error(
    `Could not download ${name} from ${url.host}: network failure or the server does not allow ` +
      `cross-origin access. The server must send Access-Control-Allow-Origin for ${origin}.`,
  );
}

const isQuota = (e) => e?.name === 'QuotaExceededError';
const storageFull = (name) =>
  new Error(
    `Not enough browser storage to download ${name}; free disk space or download it and open it locally.`,
  );

// null when OPFS is unavailable (e.g. private browsing); storage-full is an error
async function downloadDirectory(storage) {
  if (!storage?.getDirectory) return null;
  try {
    const root = await storage.getDirectory();
    return await root.getDirectoryHandle(DIRECTORY, { create: true });
  } catch (e) {
    if (isQuota(e)) throw e;
    return null;
  }
}

// Holds the named lock until the returned release() is called.
function hold(locks, name) {
  return new Promise((locked) => {
    locks.request(name, () => new Promise((release) => locked(release)));
  });
}

// Deletes downloads whose lock no page holds; each file's lock is taken while deleting it.
export async function pruneDownloads({ storage = globalThis.navigator?.storage, locks } = {}) {
  locks ??= globalThis.navigator?.locks;
  const dir = await downloadDirectory(storage);
  if (!dir) return;
  const names = [];
  for await (const [name, handle] of dir.entries()) if (handle.kind === 'file') names.push(name);
  for (const name of names) {
    if (locks) {
      // Chrome lists an open writable's swap file as NAME.crswap
      const owner = name.replace(/\.crswap$/, '');
      await locks.request(LOCK_PREFIX + owner, { ifAvailable: true }, async (lock) => {
        if (lock) await dir.removeEntry(name).catch(() => {});
      });
    } else {
      const file = await (await dir.getFileHandle(name)).getFile().catch(() => null);
      if (!file || Date.now() - file.lastModified > UNLOCKED_MAX_AGE_MS)
        await dir.removeEntry(name).catch(() => {});
    }
  }
}

/**
 * Resolves to {file, release}; release() lets a later prune delete the OPFS copy.
 * onProgress receives (text, fraction|null).
 */
export async function downloadFile(
  url,
  {
    extensions,
    kind,
    signal,
    onProgress,
    fetch = globalThis.fetch,
    storage = globalThis.navigator?.storage,
    locks = globalThis.navigator?.locks,
    memoryLimit = MEMORY_LIMIT,
  },
) {
  const name = remoteName(url);
  checkExtension(name, extensions, kind);
  let response,
    blocked = false;
  const onViolation = (e) => {
    if (e.effectiveDirective === 'connect-src' && e.blockedURI?.startsWith(url.origin))
      blocked = true;
  };
  globalThis.document?.addEventListener('securitypolicyviolation', onViolation);
  try {
    response = await fetch(url, { signal, mode: 'cors', credentials: 'omit' });
  } catch (e) {
    if (e?.name === 'AbortError') throw e;
    await new Promise((resolve) => setTimeout(resolve, 50)); // violation events are queued
    if (blocked)
      throw new Error(
        `Could not download ${name}: this viewer's Content-Security-Policy (connect-src) does not allow ${url.origin}.`,
      );
    throw corsError(name, url);
  } finally {
    globalThis.document?.removeEventListener('securitypolicyviolation', onViolation);
  }
  let reader = null,
    dir = null,
    entry = null,
    writable = null,
    release = () => {};
  const chunks = [];
  try {
    if (!response.ok)
      throw new Error(
        `Could not download ${name}: HTTP ${response.status}${response.statusText ? ' ' + response.statusText : ''}.`,
      );
    const type = response.headers.get('content-type') || '';
    if (/^text\/html\b/i.test(type))
      throw new Error(`Could not open ${name}: the server returned a web page, not a ${kind}.`);
    if (!response.body)
      throw new Error(
        `Could not open ${name}: the server returned no data (HTTP ${response.status}).`,
      );
    const total = Number(response.headers.get('content-length')) || 0;
    const lastModified = Date.parse(response.headers.get('last-modified') || '') || 0;

    await pruneDownloads({ storage, locks }).catch(() => {});
    signal?.throwIfAborted();
    dir = await downloadDirectory(storage);
    if (dir) {
      const candidate = `${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 8)}`;
      // lock before creating, so a concurrent prune cannot take the new file
      if (locks) release = await hold(locks, LOCK_PREFIX + candidate);
      entry = candidate;
      try {
        const handle = await dir.getFileHandle(entry, { create: true });
        writable = (await handle.createWritable?.()) || null;
      } catch (e) {
        if (isQuota(e)) throw e;
      }
      if (!writable) {
        // OPFS without createWritable: use memory
        await dir.removeEntry(entry).catch(() => {});
        entry = null;
        release();
        release = () => {};
      }
    }
    if (writable && total) {
      const estimate = await storage.estimate?.().catch(() => null);
      if (estimate?.quota && estimate.quota - (estimate.usage || 0) < total)
        throw storageFull(name);
    }
    const tooLarge = () =>
      new Error(
        `${name} is too large to download without browser file storage (limit ${(memoryLimit / 1e9).toFixed(1)} GB); download it and open it locally.`,
      );
    if (!writable && total > memoryLimit) throw tooLarge();
    reader = response.body.getReader();
    const start = Date.now();
    let got = 0,
      reported = 0;
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      got += value.length;
      if (writable) await writable.write(value);
      else if (got > memoryLimit) throw tooLarge();
      else chunks.push(value);
      if (Date.now() - reported > 200) {
        reported = Date.now();
        onProgress?.(progressText(got, total, reported - start), total ? got / total : null);
      }
    }
    onProgress?.(progressText(got, total, Date.now() - start), total ? got / total : null);
    if (total && got < total && !response.headers.get('content-encoding'))
      throw new Error(`Download of ${name} ended early (${got} of ${total} bytes).`);
    let data;
    if (writable) {
      await writable.close();
      writable = null;
      signal?.throwIfAborted();
      data = await (await dir.getFileHandle(entry)).getFile();
    } else data = new Blob(chunks);
    chunks.length = 0;
    signal?.throwIfAborted();
    return {
      file: new File([data], name, { type: data.type || type, lastModified }),
      release,
    };
  } catch (e) {
    chunks.length = 0;
    (reader ? reader.cancel() : response.body?.cancel())?.catch(() => {});
    if (writable) await writable.abort().catch(() => {});
    if (entry) await dir.removeEntry(entry).catch(() => {});
    release();
    if (isQuota(e)) throw storageFull(name);
    if (e instanceof TypeError) throw new Error(`Download of ${name} failed: ${e.message}`);
    throw e;
  }
}

// Opens a download. On success it becomes slot's copy and the previous one is released;
// if open throws, the download is released and the previous copy stays owned.
export async function handOver(slot, download, open) {
  try {
    await open(download.file);
  } catch (e) {
    download.release();
    throw e;
  }
  const previous = slot.release;
  slot.release = download.release;
  previous?.();
}
