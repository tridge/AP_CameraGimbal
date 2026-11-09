// Page side of the video viewer's handover (CONTRACTS.md websplat.request): receive the request
// and File from window.opener, and copy the File into OPFS while hashing it.
import { trustedOrigin, isLocalOrigin } from './request.js';
import { CHUNK } from './storage/hash.js';
import { dir, injectedQuota, list, readFile, remove, writeFile } from './storage/opfs.js';

export class HandoverError extends Error {
  name = 'HandoverError';
}

const post = (msg, origin) => window.opener?.postMessage(msg, origin);

// the opener's origin is unknown until it answers: local development pages post to any origin
const readyOrigin = () => (isLocalOrigin(location.origin) ? '*' : location.origin);

/** Tell the opener the page is ready (or why not) and wait for its websplat.request message.
 * Resolves {data, origin}; throws HandoverError without an opener or after waitMs. Messages from
 * other windows or origins, of other types or with another nonce are ignored. */
export function receiveRequest(nonce, { waitMs = 30000 } = {}) {
  if (!window.opener) return Promise.reject(new HandoverError('the page has no opener'));
  return new Promise((resolve, reject) => {
    const done = () => {
      clearTimeout(timer);
      window.removeEventListener('message', onMessage);
    };
    const onMessage = (e) => {
      if (e.source !== window.opener || !trustedOrigin(e.origin, location.origin)) return;
      if (e.data?.type !== 'websplat.request' || e.data.nonce !== nonce) return;
      done();
      resolve({ data: e.data, origin: e.origin });
    };
    const timer = setTimeout(() => {
      done();
      reject(new HandoverError(`no request from the video viewer within ${waitMs / 1000} s`));
    }, waitMs);
    window.addEventListener('message', onMessage);
    post({ type: 'websplat.ready', nonce }, readyOrigin());
  });
}

export const reply = (origin, msg) => post(msg, origin);

/** Device check failed: tell the opener (it waits for websplat.ready otherwise). */
export const refuse = (nonce, reason) =>
  window.opener && post({ type: 'websplat.rejected', nonce, reason }, readyOrigin());

const hex = (buf) => [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, '0')).join('');
const incomingLock = (name) => `websplat-incoming-${name}`;

/** Copy `file` to OPFS incoming/<name> while hashing it (sha256c, as hashBlob chunkedAlways),
 * so the hash is that of the bytes copied, then run use({path, hash}). A Web Lock is held until
 * use() returns (removeStaleIncoming() deletes only copies whose tab is gone); the copy is
 * removed afterwards unless use() moved it. */
export function withStagedCopy(file, name, use, { onProgress = () => {} } = {}) {
  const path = `incoming/${name}`;
  return navigator.locks.request(incomingLock(name), async () => {
    const n = Math.max(1, Math.ceil(file.size / CHUNK));
    const digests = new Uint8Array(n * 32);
    async function* chunks() {
      for (let k = 0; k < n; k++) {
        const end = Math.min(file.size, (k + 1) * CHUNK);
        const part = await file.slice(k * CHUNK, end).arrayBuffer();
        digests.set(new Uint8Array(await crypto.subtle.digest('SHA-256', part)), k * 32);
        onProgress(end, file.size);
        yield part;
      }
    }
    try {
      try {
        await writeFile(path, chunks());
      } catch (e) {
        if (e?.name === 'NotReadableError' || e?.name === 'NotFoundError')
          throw new HandoverError(
            `${file.name} can no longer be read (moved or changed since the viewer opened it)`,
          );
        throw e;
      }
      const hash = 'sha256c:' + hex(await crypto.subtle.digest('SHA-256', digests));
      return await use({ path, hash });
    } finally {
      await remove(path).catch(() => {});
    }
  });
}

export async function removeStaleIncoming() {
  for (const e of await list('incoming')) {
    await navigator.locks.request(incomingLock(e.name), { ifAvailable: true }, async (lock) => {
      if (lock) await remove(`incoming/${e.name}`).catch(() => {});
    });
  }
}

/** Move an OPFS file (FileSystemHandle.move where available, else copy and delete). */
export async function moveFile(from, to) {
  const split = (p) => [p.slice(0, p.lastIndexOf('/')), p.slice(p.lastIndexOf('/') + 1)];
  const [fromDir, fromName] = split(from);
  const [toDir, toName] = split(to);
  const handle = await (await dir(fromDir)).getFileHandle(fromName);
  injectedQuota(to);
  const dest = await dir(toDir, { create: true });
  await remove(to);
  if (typeof handle.move === 'function') {
    try {
      await handle.move(dest, toName);
      return;
    } catch {
      // fall back to a copy
    }
  }
  await writeFile(to, await readFile(from));
  await remove(from);
}
