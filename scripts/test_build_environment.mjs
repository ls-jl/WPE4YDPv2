import assert from 'node:assert/strict'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { execFileSync, spawnSync } from 'node:child_process'

const root = process.cwd()
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-build-env-'))
try {
  const cleanEnvironment = { PATH: process.env.PATH, HOME: process.env.HOME }
  const resolved = execFileSync('bash', ['-c',
    'source scripts/build-env.sh; printf "%s\\n" "$PROJECT_ROOT" "$WPE_SDK_ROOT" "$JOBS" "$LINK_JOBS" "$THINLTO_JOBS"'],
  { cwd: root, env: cleanEnvironment, encoding: 'utf8' }).trim().split('\n')
  assert.equal(fs.realpathSync(resolved[0]), fs.realpathSync(root))
  assert.deepEqual(resolved.slice(1), ['/srv/wpe-build/sdk', '6', '1', '2'])
  const override = execFileSync('bash', ['-c', 'source scripts/build-env.sh; printf "%s|%s" "$WPE_BUILD_ROOT" "$JOBS"'],
    { env: { ...cleanEnvironment, WPE_BUILD_ROOT: '/custom/build', JOBS: '3' }, encoding: 'utf8' })
  assert.equal(override, '/custom/build|3')

  const sdk = path.join(temporary, 'sdk', 'target')
  fs.mkdirSync(path.join(sdk, 'usr/lib/pkgconfig'), { recursive: true })
  fs.writeFileSync(path.join(sdk, '.wpe-sdk-owned'), 'wpe-sdk-sysroot-v1\n')
  const pc = path.join(sdk, 'usr/lib/pkgconfig/example.pc')
  fs.writeFileSync(pc, 'prefix=/usr\nName: fixture\nVersion: 1\nCflags: -I/usr/include\nLibs: -L/usr/lib -lexample\n')
  fs.symlinkSync('/usr/lib/pkgconfig/example.pc', path.join(sdk, 'example.pc'))
  execFileSync(process.execPath, ['scripts/relocate_sdk_metadata.mjs', sdk])
  const first = fs.readFileSync(pc, 'utf8')
  assert.ok(first.includes(`prefix=${sdk}/usr`))
  assert.ok(first.includes(`-I${sdk}/usr/include`))
  assert.ok(first.includes(`-L${sdk}/usr/lib`))
  assert.equal(fs.readlinkSync(path.join(sdk, 'example.pc')), 'usr/lib/pkgconfig/example.pc')
  execFileSync(process.execPath, ['scripts/relocate_sdk_metadata.mjs', sdk])
  assert.equal(fs.readFileSync(pc, 'utf8'), first, 'metadata relocation is idempotent')

  const base = path.join(temporary, 'base')
  fs.mkdirSync(base)
  execFileSync('git', ['init', '-q', base])
  execFileSync('git', ['-C', base, '-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '--allow-empty', '-qm', 'fixture'])
  const project = path.join(temporary, 'project')
  fs.mkdirSync(path.join(project, 'wpe-drm/webkit-patches'), { recursive: true })
  const revision = execFileSync('git', ['-C', base, 'rev-parse', 'HEAD'], { encoding: 'utf8' })
  fs.writeFileSync(path.join(project, 'wpe-drm/webkit-patches/revision'), revision)
  const worktree = path.join(temporary, 'existing')
  execFileSync('git', ['-C', base, 'worktree', 'add', '--detach', '-q', worktree])
  fs.writeFileSync(path.join(worktree, 'keep.txt'), 'uncommitted work')
  const attempt = spawnSync('bash', ['scripts/prepare_webkit_worktree.sh'], {
    env: { ...cleanEnvironment, PROJECT_ROOT: project, WEBKIT_BASE: base, WEBKIT_WORKTREE: worktree }, encoding: 'utf8',
  })
  assert.notEqual(attempt.status, 0)
  assert.equal(fs.readFileSync(path.join(worktree, 'keep.txt'), 'utf8'), 'uncommitted work')
  console.log('build environment fixtures passed: paths, overrides, SDK metadata, worktree preservation')
} finally {
  fs.rmSync(temporary, { recursive: true, force: true })
}
