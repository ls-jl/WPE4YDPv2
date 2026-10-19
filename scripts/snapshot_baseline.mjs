import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'

const revision = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim()
const destination = path.resolve('.rollback', `stability-${revision.slice(0, 12)}`)
if (fs.existsSync(destination)) throw new Error(`Baseline already exists: ${destination}`)
fs.mkdirSync(destination, { recursive: true, mode: 0o700 })
const files = {}
function snapshot(source) {
  const target = path.join(destination, source)
  const stat = fs.lstatSync(source)
  if (stat.isSymbolicLink()) {
    fs.mkdirSync(path.dirname(target), { recursive: true })
    fs.symlinkSync(fs.readlinkSync(source), target)
    return
  }
  if (stat.isDirectory()) {
    fs.mkdirSync(target, { recursive: true })
    for (const name of fs.readdirSync(source)) snapshot(path.join(source, name))
    return
  }
  fs.mkdirSync(path.dirname(target), { recursive: true })
  fs.copyFileSync(source, target, fs.constants.COPYFILE_FICLONE)
  fs.chmodSync(target, stat.mode & 0o777)
  const hash = filename => crypto.createHash('sha256').update(fs.readFileSync(filename)).digest('hex')
  const sha256 = hash(source)
  if (hash(target) !== sha256) throw new Error(`Backup mismatch: ${source}`)
  files[source] = { size: stat.size, sha256 }
}
for (const source of ['assets/wpe-runtime', 'libs', '8001779591038449.1_0_0.amr', 'release-manifest.json']) snapshot(source)
const privatePackage = '.cache/releases/gpu/8001779591038449.1_0_0.amr'
if (fs.existsSync(privatePackage)) snapshot(privatePackage)
fs.writeFileSync(path.join(destination, 'baseline.json'), JSON.stringify({ revision, files }, null, 2), { mode: 0o600 })
console.log(`Verified baseline: ${destination} (${Object.keys(files).length} files)`)
