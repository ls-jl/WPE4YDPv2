import path from 'node:path'
import crypto from 'node:crypto'
import fs from 'node:fs'
import { execFileSync, spawn } from 'node:child_process'

const archive = path.resolve(process.argv[2] || '8001779591038449.1_0_0.amr')
const entries = execFileSync('unzip', ['-Z1', archive], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 }).trim().split('\n')
const names = new Set(entries)
if (names.size !== entries.length) throw new Error('Duplicate entries in AMR')
const maliEntry = 'assets/wpe-runtime/gpu/mali/lib/libmali.so.1'
if (names.has(maliEntry)) {
  const source = JSON.parse(fs.readFileSync('licenses/mali/source.json'))
  for (const [name, expected] of Object.entries(source.notices)) {
    const entry = `assets/wpe-runtime/share/licenses/mali/${name}`
    if (!names.has(entry)) throw new Error(`Mali license missing from AMR: ${name}`)
    const bytes = execFileSync('unzip', ['-p', archive, entry])
    if (crypto.createHash('sha256').update(bytes).digest('hex') !== expected)
      throw new Error(`Mali license mismatch in AMR: ${name}`)
  }
}
const manifest = JSON.parse(execFileSync('unzip', ['-p', archive, 'assets/wpe-runtime/build-manifest.json'], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 }))
if (manifest.schema !== 2) throw new Error('Packaged runtime lacks source-bound provenance')
const artifacts = Object.fromEntries(Object.entries(manifest.artifacts).map(([file, value]) => [`assets/wpe-runtime/${file}`, value]))
const jsapi = JSON.parse(execFileSync('unzip', ['-p', archive, 'libs/arm64-orange/build-manifest.json'], { encoding: 'utf8' }))
artifacts['libs/arm64-orange/libjsapi_browser.so'] = jsapi.sha256
artifacts['libs/libjsapi_browser_12345.so'] = jsapi.sha256
for (const [file, expected] of Object.entries(artifacts)) {
  if (!names.has(file) || path.isAbsolute(file) || file.split('/').includes('..')) throw new Error(`Invalid or missing artifact: ${file}`)
  const child = spawn('unzip', ['-p', archive, file], { stdio: ['ignore', 'pipe', 'pipe'] })
  const done = new Promise((resolve, reject) => {
    child.on('error', reject)
    child.on('close', code => code === 0 ? resolve() : reject(new Error(`unzip failed: ${file}`)))
  })
  child.stderr.resume()
  const hash = crypto.createHash('sha256')
  for await (const bytes of child.stdout) hash.update(bytes)
  await done
  if (hash.digest('hex') !== expected) throw new Error(`Packaged hash mismatch: ${file}`)
}
console.log(`AMR native manifest verified: ${Object.keys(artifacts).length} artifacts; flavor=${manifest.package_flavor || 'gpu/private'}`)
