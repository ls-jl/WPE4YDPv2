import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'

const directory = fs.realpathSync(process.argv[2])
const project = process.env.PROJECT_ROOT || process.cwd()
const installedSources = path.join(project, 'assets/wpe-runtime/lib')
if (fs.existsSync(installedSources) && directory === fs.realpathSync(installedSources))
  throw new Error('SONAME normalization is staging-only; never mutate project runtime sources')
const marker = path.join(path.dirname(directory), '.wpe-bundle-staging')
if (!fs.existsSync(marker) || fs.readFileSync(marker, 'utf8').trim() !== 'wpe-bundle-staging-v1') {
  throw new Error('SONAME normalization is staging-only')
}
const bundle = path.dirname(directory)
const options = { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024, env: { ...process.env, LC_ALL: 'C' } }
const hashes = new Map()
const hash = file => {
  const canonical = fs.realpathSync(file)
  if (!hashes.has(canonical)) hashes.set(canonical, crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex'))
  return hashes.get(canonical)
}
const groups = new Map()
const needed = new Set()
const isELF = file => {
  const header = Buffer.alloc(4)
  const fd = fs.openSync(file, 'r')
  fs.readSync(fd, header, 0, 4, 0)
  fs.closeSync(fd)
  return header.equals(Buffer.from([127, 69, 76, 70]))
}
function inspectTree(root) {
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const file = path.join(root, entry.name)
    if (entry.isDirectory()) inspectTree(file)
    else if (entry.isFile() && isELF(file)) {
      const dynamic = execFileSync('readelf', ['-W', '-d', file], options)
      for (const match of dynamic.matchAll(/\(NEEDED\).*\[([^\]]+)\]/g)) needed.add(match[1])
    }
  }
}
inspectTree(bundle)
function inspectNames(root) {
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const file = path.join(root, entry.name)
    if (entry.isDirectory()) inspectNames(file)
    else if (/\.(?:c|cpp|h|sh|js|vue)$/.test(entry.name)) {
      const code = fs.readFileSync(file, 'utf8')
      for (const match of code.matchAll(/["']([^"'\s/]+\.so(?:\.[0-9]+)*)["']/g)) needed.add(match[1])
    }
  }
}
for (const folder of ['src', 'jsapi/src', 'wpe-drm', 'scripts']) inspectNames(path.join(project, folder))
for (const entry of fs.readdirSync(directory).sort()) {
  if (!/\.so(?:\.|$)/.test(entry)) continue
  const file = path.join(directory, entry)
  if (!fs.existsSync(file) || !fs.statSync(file).isFile() || !isELF(file)) continue
  if (!fs.realpathSync(file).startsWith(`${directory}/`)) throw new Error(`External runtime symlink: ${file}`)
  const dynamic = execFileSync('readelf', ['-W', '-d', file], options)
  const soname = dynamic.match(/\(SONAME\).*\[([^\]]+)\]/)?.[1]
  if (!soname || path.basename(soname) !== soname) throw new Error(`Missing or unsafe SONAME: ${entry}`)
  const group = groups.get(soname) || []
  group.push(file)
  groups.set(soname, group)
}
let removed = 0
let bytes = 0
for (const [soname, files] of groups) {
  if (new Set(files.map(hash)).size !== 1)
    throw new Error(`Conflicting runtime versions for ${soname}: ${files.map(file => path.basename(file)).join(', ')}`)
}
for (const [soname, files] of groups) {
  const canonical = path.join(directory, soname)
  const temporary = `${canonical}.staging-${process.pid}`
  if (!fs.existsSync(canonical) || fs.lstatSync(canonical).isSymbolicLink()) {
    fs.copyFileSync(files[0], temporary)
    fs.chmodSync(temporary, fs.statSync(files[0]).mode & 0o777)
    fs.renameSync(temporary, canonical)
  }
  for (const file of files) {
    const name = path.basename(file)
    if (name === soname) continue
    if (needed.has(name)) {
      if (fs.lstatSync(file).isSymbolicLink()) {
        const alias = `${file}.staging-${process.pid}`
        fs.copyFileSync(canonical, alias)
        fs.renameSync(alias, file)
      }
      continue
    }
    if (fs.lstatSync(file).isFile()) bytes += fs.statSync(file).size
    fs.unlinkSync(file)
    removed++
  }
}
console.log(`Canonical SONAME runtime: removed=${removed} redundant_bytes=${bytes}; module directories preserved`)
