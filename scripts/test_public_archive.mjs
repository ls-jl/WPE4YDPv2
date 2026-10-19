import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { execFileSync, spawnSync } from 'node:child_process'

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-release-policy-'))
const verifier = path.resolve('scripts/verify_public_archive.mjs')
try {
  fs.writeFileSync(path.join(temporary, 'README.md'), 'public runtime fixture')
  const archive = path.join(temporary, 'fixture.amr')
  execFileSync('zip', ['-q', archive, 'README.md'], { cwd: temporary })
  assert.equal(spawnSync(process.execPath, [verifier, archive]).status, 0)
  const blob = 'assets/wpe-runtime/gpu/mali/lib/libmali.so.1'
  fs.mkdirSync(path.dirname(path.join(temporary, blob)), { recursive: true })
  fs.writeFileSync(path.join(temporary, blob), 'private fixture')
  execFileSync('zip', ['-q', archive, blob], { cwd: temporary })
  assert.equal(spawnSync(process.execPath, [verifier, archive]).status, 1)
  assert.equal(spawnSync(process.execPath, [verifier, path.join(temporary, 'missing.amr')]).status, 1)
  console.log('public archive fixtures passed: public, private GPU, missing archive')
} finally {
  fs.rmSync(temporary, { recursive: true, force: true })
}
