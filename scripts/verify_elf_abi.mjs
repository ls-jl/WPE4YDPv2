import fs from 'node:fs'
import path from 'node:path'
import { execFileSync } from 'node:child_process'

const root = path.resolve(process.argv[2] || 'assets/wpe-runtime')
const closure = process.argv.includes('--closure')
const system = new Set(['libc.so.6', 'libm.so.6', 'libdl.so.2', 'libpthread.so.0',
  'librt.so.1', 'libresolv.so.2', 'libstdc++.so.6', 'libgcc_s.so.1', 'libatomic.so.1',
  'ld-linux-aarch64.so.1'])
const libraries = new Set()
const dependencies = []
const aliases = []
const validFiles = new Set()
const errors = []
let checked = 0
const above = (value, limit) => {
  const a = value.split('.').map(Number), b = limit.split('.').map(Number)
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    if ((a[i] || 0) !== (b[i] || 0)) return (a[i] || 0) > (b[i] || 0)
  }
  return false
}
function inspect(file) {
  if (fs.lstatSync(file).isSymbolicLink()) {
    if (!fs.existsSync(file)) errors.push(`broken runtime link: ${file}`)
    else aliases.push([path.basename(file), fs.realpathSync(file)])
    return
  }
  if (fs.statSync(file).isDirectory()) {
    for (const entry of fs.readdirSync(file)) inspect(path.join(file, entry))
    return
  }
  const fd = fs.openSync(file, 'r')
  const header = Buffer.alloc(64)
  const count = fs.readSync(fd, header, 0, header.length, 0)
  fs.closeSync(fd)
  if (count < 64 || !header.subarray(0, 4).equals(Buffer.from([127, 69, 76, 70]))) return
  if (header[4] !== 2 || header[5] !== 1 || header.readUInt16LE(18) !== 183) {
    errors.push(`not little-endian AArch64 ELF64: ${file}`)
    return
  }
  if (![2, 3].includes(header.readUInt16LE(16))) return
  checked++
  libraries.add(path.basename(file))
  validFiles.add(fs.realpathSync(file))
  const options = { encoding: 'utf8', env: { ...process.env, LC_ALL: 'C' } }
  const dynamic = execFileSync('readelf', ['-W', '-d', file], options)
  for (const match of dynamic.matchAll(/\(NEEDED\).*\[([^\]]+)\]/g)) dependencies.push([file, match[1]])
  const versions = execFileSync('readelf', ['-W', '--version-info', file], options)
  const needs = versions.slice(versions.indexOf('Version needs section'))
  for (const match of needs.matchAll(/Name: (GLIBCXX|GLIBC)_([\d.]+)/g)) {
    const limit = match[1] === 'GLIBCXX' ? '3.4.29' : '2.36'
    if (above(match[2], limit)) errors.push(`${file}: requires ${match[1]}_${match[2]}`)
  }
}
inspect(root)
for (const [name, target] of aliases) if (validFiles.has(target)) libraries.add(name)
if (!checked) errors.push('no runtime ELF files checked')
if (closure) {
  for (const [file, needed] of dependencies) {
    if (!system.has(needed) && !libraries.has(needed)) errors.push(`${file}: missing ${needed}`)
  }
}
if (errors.length) {
  console.error([...new Set(errors)].join('\n'))
  process.exitCode = 1
} else console.log(`ELF verification passed: ${checked} AArch64 executables/libraries; GLIBC<=2.36 GLIBCXX<=3.4.29 closure=${closure}`)
