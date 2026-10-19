import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'

const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-abi-'))
const verifier = path.resolve('scripts/verify_elf_abi.mjs')
try {
  const wrong = Buffer.alloc(64)
  wrong.set([127, 69, 76, 70, 2, 1])
  wrong.writeUInt16LE(3, 16)
  wrong.writeUInt16LE(62, 18)
  const foreign = path.join(temporary, 'host.so')
  fs.writeFileSync(foreign, wrong)
  assert.equal(spawnSync(process.execPath, [verifier, foreign]).status, 1)
  fs.unlinkSync(foreign)
  const toolchain = process.env.JSAPI_TOOLCHAIN_ROOT || '/srv/wpe-build/sdk/toolchains/aarch64--glibc--stable-2018.11-1'
  const cc = path.join(toolchain, 'bin/aarch64-buildroot-linux-gnu-gcc')
  if (fs.existsSync(cc)) {
    fs.writeFileSync(path.join(temporary, 'provider.c'), 'int provider(void) { return 7; }\n')
    fs.writeFileSync(path.join(temporary, 'consumer.c'), 'extern int provider(void); int consume(void) { return provider(); }\n')
    execFileSync(cc, ['-fPIC', '-shared', path.join(temporary, 'provider.c'), '-Wl,-soname,libprovider.so.1', '-o', path.join(temporary, 'libprovider.so')])
    execFileSync(cc, ['-fPIC', '-shared', path.join(temporary, 'consumer.c'), `-L${temporary}`, '-lprovider', '-o', path.join(temporary, 'libconsumer.so')])
    assert.equal(spawnSync(process.execPath, [verifier, temporary, '--closure']).status, 1,
      'an embedded SONAME alone does not make a loadable file')
    fs.symlinkSync('libprovider.so', path.join(temporary, 'libprovider.so.1'))
    assert.equal(spawnSync(process.execPath, [verifier, temporary, '--closure']).status, 0)
    fs.unlinkSync(path.join(temporary, 'libprovider.so'))
    assert.equal(spawnSync(process.execPath, [verifier, temporary, '--closure']).status, 1)
    console.log('ELF fixtures passed: foreign architecture, AArch64 ABI, missing dependency')
  } else console.log('ELF foreign architecture fixture passed; cross compiler fixture skipped (SDK absent)')
} finally {
  fs.rmSync(temporary, { recursive: true, force: true })
}
