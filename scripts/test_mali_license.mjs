import assert from 'node:assert/strict'
import { mkdtempSync, mkdirSync, cpSync, symlinkSync, writeFileSync, rmSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { resolve, join } from 'node:path'
import { verifyMaliRuntime } from './verify_mali_runtime.mjs'

const runtime = resolve('assets/wpe-runtime')
const tmp = mkdtempSync(join(tmpdir(), 'wpe-mali-license-'))
try {
  const lib = join(tmp, 'gpu/mali/lib')
  const notices = join(tmp, 'share/licenses/mali')
  mkdirSync(lib, { recursive: true })
  cpSync(join(runtime, 'share/licenses/mali'), notices, { recursive: true })
  for (const name of ['libmali.so.1', 'libEGL.so.1', 'libGLESv2.so.2', 'libgbm.so.1', 'libwpe-mali-gbm-compat.so'])
    symlinkSync(join(runtime, 'gpu/mali/lib', name), join(lib, name))
  verifyMaliRuntime(tmp)
  writeFileSync(join(lib, 'libmali_hook.so.1'), 'unreviewed firmware file')
  assert.throws(() => verifyMaliRuntime(tmp), /Unreviewed Mali file/)
  rmSync(join(lib, 'libmali_hook.so.1'))
  writeFileSync(join(notices, 'END_USER_LICENCE_AGREEMENT.txt'), 'incorrect license')
  assert.throws(() => verifyMaliRuntime(tmp), /notice missing or changed/)
  cpSync(join(runtime, 'share/licenses/mali/END_USER_LICENCE_AGREEMENT.txt'),
    join(notices, 'END_USER_LICENCE_AGREEMENT.txt'))
  rmSync(join(lib, 'libmali.so.1'))
  writeFileSync(join(lib, 'libmali.so.1'), 'unreviewed binary')
  assert.throws(() => verifyMaliRuntime(tmp), /licensed upstream hash/)
  console.log('Mali licensing regression tests passed')
} finally {
  rmSync(tmp, { recursive: true, force: true })
}
