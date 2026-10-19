import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'

const root = path.resolve(process.argv[2])
const sysroot = process.env.DEB_SYSROOT
const deviceCxx = process.env.DEVICE_CXX
const gpu = process.argv.includes('--gpu')
const files = new Map()
const cache = new Map()
const options = { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024, env: { ...process.env, LC_ALL: 'C' } }
for (const entry of fs.readdirSync(path.join(root, 'lib'))) {
  const file = path.join(root, 'lib', entry)
  if (fs.statSync(file).isFile() && entry.includes('.so')) files.set(entry, file)
}
if (gpu) {
  const directory = path.join(root, 'gpu/mali/lib')
  for (const entry of fs.readdirSync(directory)) {
    const file = path.join(directory, entry)
    if (fs.statSync(file).isFile() && entry.includes('.so')) files.set(entry, file)
  }
}
for (const library of ['libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0',
  'librt.so.1', 'libresolv.so.2', 'libgcc_s.so.1', 'libatomic.so.1', 'ld-linux-aarch64.so.1']) {
  if (files.has(library)) continue
  const found = ['lib/aarch64-linux-gnu', 'usr/lib/aarch64-linux-gnu', 'lib'].map(dir => path.join(sysroot, dir, library)).find(file => fs.existsSync(file))
  if (found) files.set(library, found)
}
files.set('libstdc++.so.6', path.join(deviceCxx, 'libstdc++.so.6.0.29'))
function inspect(file) {
  const canonical = fs.realpathSync(file)
  if (cache.has(canonical)) return cache.get(canonical)
  const dynamic = execFileSync('readelf', ['-W', '-d', file], options)
  const symbols = execFileSync('readelf', ['-W', '--dyn-syms', file], options)
  const result = { needed: [...dynamic.matchAll(/\(NEEDED\).*\[([^\]]+)\]/g)].map(m => m[1]), defined: new Set(), undefined: [] }
  for (const line of symbols.split('\n')) {
    const fields = line.trim().split(/\s+/)
    if (!/^\d+:$/.test(fields[0]) || fields.length < 8) continue
    const name = fields[7]
    if (fields[6] === 'UND') {
      if (fields[4] !== 'WEAK') result.undefined.push(name)
    } else if (['GLOBAL', 'WEAK', 'UNIQUE'].includes(fields[4])) {
      result.defined.add(name.replace('@@', '@'))
      result.defined.add(name.split('@')[0])
    }
  }
  cache.set(canonical, result)
  return result
}
const errors = []
const roots = ['lib/libWPEWebKit-2.0.so.1', 'wpe-drm-minimal',
  'libexec/wpe-webkit-2.0/WPEWebProcess', 'libexec/wpe-webkit-2.0/WPENetworkProcess',
  'lib/gstreamer-1.0/libgstwebrtc.so', 'lib/gstreamer-1.0/libgstdtls.so',
  'lib/gstreamer-1.0/libgstrockchipmpp.so']
for (const relative of roots) {
  const closure = new Map()
  function visit(file) {
    const canonical = fs.realpathSync(file)
    if (closure.has(canonical)) return
    const info = inspect(file)
    closure.set(canonical, info)
    for (const dependency of info.needed) {
      if (!files.has(dependency)) throw new Error(`Unresolved provider ${dependency} for ${file}`)
      visit(files.get(dependency))
    }
  }
  visit(path.join(root, relative))
  if (gpu) {
    visit(files.get('libwpe-mali-gbm-compat.so'))
  }
  const exports = new Set([...closure.values()].flatMap(info => [...info.defined]))
  for (const [file, info] of closure) {
    for (const symbol of info.undefined) if (!exports.has(symbol)) errors.push(`${relative}: ${path.basename(file)} needs ${symbol}`)
  }
}
if (errors.length) {
  console.error([...new Set(errors)].join('\n'))
  process.exitCode = 1
} else console.log(`Versioned symbol resolution passed for ${roots.length} runtime entrypoints against target libc and device libstdc++; profile=${gpu ? 'gpu' : 'cpu'}`)
