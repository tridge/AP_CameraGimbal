import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

export default function setup() {
  execFileSync(
    process.env.PYTHON || 'python3',
    [fileURLToPath(new URL('../../scripts/generate-fixtures.py', import.meta.url))],
    { stdio: 'inherit', env: { ...process.env, PYTHONDONTWRITEBYTECODE: '1' } },
  );
  if (process.env.VIEWER_REAL_THERMAL)
    execFileSync(
      process.env.PYTHON || 'python3',
      [
        fileURLToPath(new URL('../../scripts/generate-fixtures.py', import.meta.url)),
        '--reference',
        process.env.VIEWER_REAL_THERMAL,
      ],
      { stdio: 'inherit', env: { ...process.env, PYTHONDONTWRITEBYTECODE: '1' } },
    );
}
