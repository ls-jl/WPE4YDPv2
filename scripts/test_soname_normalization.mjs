import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-soname-'))
try {
  const library = path.join(directory, 'lib')
  fs.mkdirSync(library)
  fs.writeFileSync(path.join(directory, '.wpe-bundle-staging'), 'wpe-bundle-staging-v1\n')
  const source = path.join(directory, 'test.c')
  fs.writeFileSync(source, 'int example(void) { return 1; }\n')
  const versioned = path.join(library, 'libexample.so.1.2')
  execFileSync('cc', ['-shared', '-fPIC', source, '-Wl,-soname,libexample.so.1', '-o', versioned])
  fs.copyFileSync(versioned, path.join(library, 'libexample.so.1'))
  execFileSync(process.execPath, ['scripts/materialize_sonames.mjs', library])
  assert.ok(fs.existsSync(path.join(library, 'libexample.so.1')))
  assert.ok(!fs.existsSync(versioned))
  fs.writeFileSync(source, 'int example(void) { return 2; }\n')
  execFileSync('cc', ['-shared', '-fPIC', source, '-Wl,-soname,libexample.so.1', '-o', versioned])
  const failed = spawnSync(process.execPath, ['scripts/materialize_sonames.mjs', library], { encoding: 'utf8' })
  assert.notEqual(failed.status, 0)
  assert.match(failed.stderr, /Conflicting runtime versions/)
  assert.ok(fs.existsSync(versioned), 'conflict must not delete input libraries')
  fs.rmSync(path.join(directory, '.wpe-bundle-staging'))
  const noMarker = spawnSync(process.execPath, ['scripts/materialize_sonames.mjs', library], { encoding: 'utf8' })
  assert.notEqual(noMarker.status, 0)
  assert.match(noMarker.stderr, /staging-only/)
  console.log('SONAME tests passed: exact duplicate cleanup, conflicts and staging ownership')
} finally {
  fs.rmSync(directory, { recursive: true, force: true })
}
