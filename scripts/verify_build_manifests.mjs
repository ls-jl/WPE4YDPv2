import assert from 'node:assert/strict'
import crypto from 'node:crypto'
import fs from 'node:fs'

const runtimeManifestPath = 'assets/wpe-runtime/build-manifest.json'
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
assert.equal(runtimeManifest.schema, 1)
assert.equal(runtimeManifest.webkit_revision, fs.readFileSync(revisionPath, 'utf8').trim())
assert.equal(runtimeManifest.patch_series_sha256, patchSeriesHash())
verifyArtifacts(Object.fromEntries(Object.entries(runtimeManifest).map(([key, value]) => {
  if (key !== 'artifacts') return [key, value]
  return [key, Object.fromEntries(Object.entries(value).map(([path, hash]) => [`assets/wpe-runtime/${path}`, hash]))]
})))

if (fs.existsSync('release-manifest.json')) {
  const releaseManifest = JSON.parse(fs.readFileSync('release-manifest.json', 'utf8'))
  assert.equal(releaseManifest.schema, 1)
  assert.equal(releaseManifest.webkit_revision, runtimeManifest.webkit_revision)
  assert.equal(releaseManifest.patch_series_sha256, runtimeManifest.patch_series_sha256)
  verifyArtifacts(releaseManifest)
}

console.log('build manifest fixtures: ok')
