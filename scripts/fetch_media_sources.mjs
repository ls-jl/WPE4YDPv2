import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'

const definitions = JSON.parse(fs.readFileSync(new URL('./media-sources.json', import.meta.url)))
const downloads = path.join(process.env.WPE_DOWNLOADS_ROOT, 'media')
const sources = process.env.WPE_SOURCE_ROOT
fs.mkdirSync(downloads, { recursive: true })
fs.mkdirSync(sources, { recursive: true })
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
const lockPath = path.join(downloads, 'sources.lock.json')
const lock = fs.existsSync(lockPath) ? JSON.parse(fs.readFileSync(lockPath)) : {}
for (const item of definitions) {
  const archive = path.join(downloads, path.basename(new URL(item.url).pathname))
  if (!fs.existsSync(archive)) {
    execFileSync('curl', ['--proto', '=https', '-fSL', '--retry', '3', item.url, '-o', `${archive}.part`], { stdio: 'inherit' })
    fs.renameSync(`${archive}.part`, archive)
  }
  const sha256 = hash(archive)
  if (lock[item.name] && (lock[item.name].sha256 !== sha256 || lock[item.name].url !== item.url)) {
    throw new Error(`Source lock mismatch: ${item.name}`)
  }
  lock[item.name] = { url: item.url, sha256 }
  fs.writeFileSync(`${lockPath}.tmp`, JSON.stringify(lock, null, 2))
  fs.renameSync(`${lockPath}.tmp`, lockPath)
  const destination = path.join(sources, item.name)
  if (fs.existsSync(destination)) {
    if (fs.readFileSync(path.join(destination, '.archive-sha256'), 'utf8').trim() !== sha256) {
      throw new Error(`Preserving unrecognized source tree: ${destination}`)
    }
    continue
  }
  const entries = execFileSync('tar', ['-tf', archive], { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 }).split('\n').filter(Boolean)
  if (entries.some(name => name.startsWith('/') || name.split('/').includes('..') || !name.startsWith(`${item.name}/`))) {
    throw new Error(`Unexpected source archive layout: ${item.name}`)
  }
  const temporary = fs.mkdtempSync(path.join(sources, '.extract-'))
  execFileSync('tar', ['-xf', archive, '-C', temporary], { stdio: 'inherit' })
  fs.writeFileSync(path.join(temporary, item.name, '.archive-sha256'), sha256)
  fs.renameSync(path.join(temporary, item.name), destination)
  fs.rmdirSync(temporary)
}
console.log(`Media sources verified: ${lockPath}`)
