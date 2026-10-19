import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'

const [operation] = process.argv.slice(2)
const sdk = process.env.WPE_SDK_ROOT
const prefix = process.env.WEBRTC_PREFIX
const project = process.env.PROJECT_ROOT
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
const inputs = [process.env.CROSS_FILE, path.join(sdk, 'debian-packages.sha256'),
  path.join(sdk, 'local-inputs.sha256'),
  ...['media-sources.json', 'prepare_media_sdk.sh', 'build_media_runtime.sh'].map(p => path.join(project, 'scripts', p))]
for (const name of fs.readdirSync(path.join(project, 'wpe-drm/gstreamer-patches')).sort()) {
  if (name.endsWith('.patch')) inputs.push(path.join(project, 'wpe-drm/gstreamer-patches', name))
}
function inputName(file) {
  if (file === process.env.CROSS_FILE) return 'sdk/cross-file'
  if (file.startsWith(`${sdk}/`)) return `sdk/${path.relative(sdk, file)}`
  const recipe = file.match(/\/(scripts\/[^/]+|wpe-drm\/gstreamer-patches\/[^/]+)$/)
  if (!recipe) throw new Error(`Unrecognized SDK input: ${file}`)
  return recipe[1]
}
function sourceName(file) {
  if (file.startsWith(`${process.env.WPE_SOURCE_ROOT}/`)) return `sources/${path.relative(process.env.WPE_SOURCE_ROOT, file)}`
  const archiveRoot = path.join(process.env.WPE_DOWNLOADS_ROOT, 'media')
  if (file.startsWith(`${archiveRoot}/`)) return `archives/${path.relative(archiveRoot, file)}`
  throw new Error(`Unrecognized SDK source: ${file}`)
}
const current = { schema: 3, inputs: Object.fromEntries(inputs.map(file => [inputName(file), hash(file)])), sources: {}, files: {} }
function sourceTree(directory) {
  for (const item of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    if (item.name === '.git') continue
    const file = path.join(directory, item.name)
    if (item.isDirectory()) sourceTree(file)
    else if (item.isFile()) current.sources[sourceName(file)] = hash(file)
  }
}
sourceTree(process.env.WPE_SOURCE_ROOT)
for (const name of fs.readdirSync(path.join(process.env.WPE_DOWNLOADS_ROOT, 'media')).sort()) {
  if (/\.tar\.(gz|xz|bz2)$/.test(name)) {
    const file = path.join(process.env.WPE_DOWNLOADS_ROOT, 'media', name)
    current.sources[sourceName(file)] = hash(file)
  }
}
function walk(directory) {
  for (const item of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const file = path.join(directory, item.name)
    if (item.isDirectory()) walk(file)
    else if (item.isSymbolicLink()) current.files[path.relative(prefix, file)] = { link: fs.readlinkSync(file) }
    else if (item.isFile()) current.files[path.relative(prefix, file)] = { sha256: hash(file) }
  }
}
walk(prefix)
const filename = path.join(sdk, 'media-build-receipt.json')
if (operation === 'record') {
  fs.writeFileSync(`${filename}.tmp`, JSON.stringify(current, null, 2))
  fs.renameSync(`${filename}.tmp`, filename)
} else if (operation === 'check') {
  let expected = JSON.parse(fs.readFileSync(filename))
  if (expected.schema === 2) {
    const old = execFileSync('git', ['-C', project, 'show', '150b4b5:scripts/build_webrtc_runtime_pve.sh'], { encoding: 'utf8' })
    const oldHash = crypto.createHash('sha256').update(old).digest('hex')
    const oldInput = Object.entries(expected.inputs).find(([file]) => file.endsWith('/scripts/build_webrtc_runtime_pve.sh'))
    assert.equal(oldInput?.[1], oldHash, 'Legacy media recipe is not the verified baseline')
    const begin = old.indexOf('cat >"$WORK/bin/aarch64-cc"')
    const endMarker = 'node "$PROJECT_ROOT/scripts/sdk_receipt.mjs" record'
    const end = old.indexOf(endMarker, begin) + endMarker.length
    const module = fs.readFileSync(path.join(project, 'scripts/build_media_runtime.sh'), 'utf8')
    assert.equal(module.slice(module.indexOf('cat >"$WORK/bin/aarch64-cc"')).trim(), old.slice(begin, end).trim(), 'Media compilation changed; rebuild in a fresh SDK prefix')
    const normalizedInputs = Object.fromEntries(Object.entries(expected.inputs)
      .filter(([file]) => file !== oldInput[0]).map(([file, digest]) => [inputName(file), digest]))
    normalizedInputs['scripts/build_media_runtime.sh'] = current.inputs['scripts/build_media_runtime.sh']
    const normalized = { schema: 3, inputs: normalizedInputs,
      sources: Object.fromEntries(Object.entries(expected.sources).map(([file, digest]) => [sourceName(file), digest])), files: expected.files }
    assert.deepEqual(normalized, current, 'Legacy SDK source/artifact proof differs; preserving existing receipt')
    if (!fs.existsSync(`${filename}.schema2.bak`)) fs.copyFileSync(filename, `${filename}.schema2.bak`)
    fs.writeFileSync(`${filename}.tmp`, JSON.stringify(current, null, 2))
    fs.renameSync(`${filename}.tmp`, filename)
    expected = current
    console.log('Media receipt migrated after byte-identical recipe and complete artifact/source verification')
  }
  try {
    assert.deepEqual(expected, current)
  } catch {
    throw new Error('Media SDK inputs or artifacts changed; choose a fresh prefix and rebuild instead of reusing unknown binaries')
  }
} else throw new Error('Expected record|check')
console.log(`Media SDK receipt ${operation}: ${Object.keys(current.files).length} files`)
