import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'

const apply = process.argv.includes('--apply')
const root = execFileSync('git', ['rev-parse', '--show-toplevel'], { encoding: 'utf8' }).trim()
process.chdir(root)
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
const entries = execFileSync('git', ['ls-files', '-s', '-z'], { maxBuffer: 16 * 1024 * 1024 })
  .toString().split('\0').filter(Boolean)
const files = []
const errors = []
for (const entry of entries) {
  const [meta, name] = entry.split('\t')
  const [mode, oid, stage] = meta.split(' ')
  if (stage !== '0' || !fs.existsSync(name)) { errors.push(`missing/conflicted: ${name}`); continue }
  const stat = fs.lstatSync(name)
  if (mode === '120000') {
    const target = execFileSync('git', ['cat-file', 'blob', oid]).toString()
    if (stat.isSymbolicLink()) {
      if (fs.readlinkSync(name) !== target) errors.push(`modified link: ${name}`)
      continue
    }
    const destination = path.resolve(path.dirname(name), target)
    if (!stat.isFile() || !fs.existsSync(destination) || hash(name) !== hash(destination)) {
      errors.push(`link contents differ: ${name}`)
      continue
    }
    files.push({ name, action: 'symlink', target, beforeMode: stat.mode & 0o777, sha256: hash(name) })
  } else if (stat.isFile()) {
    const expected = mode === '100755' ? 0o755 : 0o644
    if ((stat.mode & 0o777) !== expected) files.push({ name, action: 'chmod', mode: expected,
      beforeMode: stat.mode & 0o777, sha256: hash(name) })
  }
}
for (const name of ['assets/wpe-runtime/run.sh', 'assets/wpe-runtime/libexec/wpe-gpu-probe']) {
  if (fs.existsSync(name) && !(fs.statSync(name).mode & 0o111)) {
    files.push({ name, action: 'chmod', mode: 0o755, beforeMode: fs.statSync(name).mode & 0o777, sha256: hash(name) })
  }
}
if (errors.length) throw new Error(errors.join('\n'))
console.log(JSON.stringify({ apply, repairs: files.length, links: files.filter(f => f.action === 'symlink').length }, null, 2))
if (apply) {
  const directory = path.join('.rollback', `migration-${Date.now()}`)
  fs.mkdirSync(directory, { recursive: true, mode: 0o700 })
  const state = execFileSync('git', ['status', '--porcelain=v1']).toString()
  fs.writeFileSync(path.join(directory, 'before.json'), JSON.stringify({ state, files }, null, 2), { mode: 0o600 })
  for (const item of files) {
    if (hash(item.name) !== item.sha256) throw new Error(`changed during repair: ${item.name}`)
    if (item.action === 'symlink') {
      const temporary = `${item.name}.migration-${process.pid}`
      fs.symlinkSync(item.target, temporary)
      fs.renameSync(temporary, item.name)
    } else fs.chmodSync(item.name, item.mode)
    if (hash(item.name) !== item.sha256) throw new Error(`content verification failed: ${item.name}`)
  }
  console.log(`Verified migration repair; original metadata: ${directory}/before.json`)
}
