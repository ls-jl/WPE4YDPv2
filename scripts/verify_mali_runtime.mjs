import { createHash } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { readFileSync, readdirSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const pinned = JSON.parse(readFileSync(resolve(root, 'licenses/mali/source.json')))
const hash = file => createHash('sha256').update(readFileSync(file)).digest('hex')

export function verifyMaliRuntime(runtime) {
  const lib = resolve(runtime, 'gpu/mali/lib')
  const notices = resolve(runtime, 'share/licenses/mali')
  if (hash(resolve(lib, 'libmali.so.1')) !== pinned.sha256)
    throw new Error('Mali binary does not match licensed upstream hash')
  for (const [name, expected] of Object.entries(pinned.notices)) {
    if (hash(resolve(notices, name)) !== expected)
      throw new Error(`Mali notice missing or changed: ${name}`)
  }
  if (hash(resolve(notices, 'source.json')) !== hash(resolve(root, 'licenses/mali/source.json')))
    throw new Error('Mali provenance mismatch')
  if (hash(resolve(notices, 'WRAPPERS-MIT.txt')) !== hash(resolve(root, 'LICENSE')))
    throw new Error('Mali facade license mismatch')
  const allowed = ['libmali.so.1', 'libEGL.so.1', 'libGLESv2.so.2',
    'libgbm.so.1', 'libwpe-mali-gbm-compat.so']
  for (const name of readdirSync(lib)) {
    if (!allowed.includes(name)) throw new Error(`Unreviewed Mali file: ${name}`)
  }
  for (const name of allowed.slice(1, 4)) {
    const dynamic = execFileSync('readelf', ['-d', resolve(lib, name)], { encoding: 'utf8' })
    if (!dynamic.includes(`Library soname: [${name}]`)
        || !dynamic.includes('Shared library: [libmali.so.1]'))
      throw new Error(`Invalid Mali ABI facade: ${name}`)
  }
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  if (!process.argv[2]) throw new Error('Usage: verify_mali_runtime.mjs RUNTIME')
  verifyMaliRuntime(process.argv[2])
  console.log('Mali upstream binary, notices, provenance and ABI facades verified')
}
