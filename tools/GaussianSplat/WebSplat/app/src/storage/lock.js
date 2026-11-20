// Single-tab ownership of a job through the Web Locks API.
export class JobLockedError extends Error {
  name = 'JobLockedError';
}

// resolves to { release() } or throws JobLockedError if another tab holds the job
export async function acquireJobLock(jobId) {
  if (!navigator.locks) throw new Error('this browser has no Web Locks API');
  let release;
  const held = new Promise((r) => (release = r));
  const got = await new Promise((resolve, reject) => {
    navigator.locks
      .request(`websplat-job-${jobId}`, { mode: 'exclusive', ifAvailable: true }, (lock) => {
        resolve(!!lock);
        return lock ? held : undefined;
      })
      .catch(reject);
  });
  if (!got) throw new JobLockedError('this job is open in another tab or window');
  return { release };
}
