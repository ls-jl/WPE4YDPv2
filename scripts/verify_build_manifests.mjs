import assert from 'node:assert/strict'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'

const runtimeRoot = process.env.WPE_VERIFY_RUNTIME || 'assets/wpe-runtime'
const runtimeManifestPath = path.join(runtimeRoot, 'build-manifest.json')
const revisionPath = 'wpe-drm/webkit-patches/revision'
const seriesPath = 'wpe-drm/webkit-patches/series'

function sha256File(path) {
  return crypto.createHash('sha256').update(fs.readFileSync(path)).digest('hex')
}

function patchSeriesHash() {
  const series = fs.readFileSync(seriesPath, 'utf8')
  const hashes = []
  for (const rawLine of series.split('\n')) {
    const patch = rawLine.trim()
    if (!patch || patch.startsWith('#')) continue
    hashes.push(sha256File(`wpe-drm/webkit-patches/${patch}`))
  }
  let material = `${series}${hashes.join('\n')}\n`
  const inputsPath = 'wpe-drm/webkit-patches/inputs'
  if (fs.existsSync(inputsPath)) {
    const inputs = fs.readFileSync(inputsPath, 'utf8')
    const inputHashes = []
    for (const rawLine of inputs.split('\n')) {
      const input = rawLine.trim()
      if (!input || input.startsWith('#')) continue
      inputHashes.push(sha256File(input))
    }
    material += `${inputs}${inputHashes.join('\n')}\n`
  }
  return crypto.createHash('sha256').update(material).digest('hex')
}

function verifyArtifacts(manifest) {
  for (const [path, expected] of Object.entries(manifest.artifacts || {})) {
    assert.ok(fs.existsSync(path), `manifest artifact missing: ${path}`)
    assert.equal(sha256File(path), expected, `manifest hash mismatch: ${path}`)
  }
}

const runtimeManifest = JSON.parse(fs.readFileSync(runtimeManifestPath, 'utf8'))
assert.ok([1, 2].includes(runtimeManifest.schema))
if (runtimeManifest.schema === 2) {
  assert.ok(runtimeManifest.toolchain?.version, 'missing actual toolchain provenance')
  assert.ok(Object.keys(runtimeManifest.source_inputs || {}).length, 'missing source inputs')
  for (const [path, expected] of Object.entries(runtimeManifest.source_inputs)) {
    assert.equal(sha256File(path), expected, `source changed since native build: ${path}`)
  }
}
assert.equal(runtimeManifest.webkit_revision, fs.readFileSync(revisionPath, 'utf8').trim())
assert.equal(runtimeManifest.patch_series_sha256, patchSeriesHash())
verifyArtifacts(Object.fromEntries(Object.entries(runtimeManifest).map(([key, value]) => {
  if (key !== 'artifacts') return [key, value]
  return [key, Object.fromEntries(Object.entries(value).map(([file, hash]) => [path.join(runtimeRoot, file), hash]))]
})))

if (!process.argv.includes('--runtime-only') && fs.existsSync('release-manifest.json')) {
  const releaseManifest = JSON.parse(fs.readFileSync('release-manifest.json', 'utf8'))
  assert.equal(releaseManifest.schema, 1)
  assert.equal(releaseManifest.webkit_revision, runtimeManifest.webkit_revision)
  assert.equal(releaseManifest.patch_series_sha256, runtimeManifest.patch_series_sha256)
  verifyArtifacts(releaseManifest)
}

console.log('build manifest fixtures: ok')
