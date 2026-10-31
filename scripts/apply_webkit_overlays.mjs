import fs from 'node:fs'
import path from 'node:path'
import assert from 'node:assert/strict'

const [project, worktree, mode = 'apply'] = process.argv.slice(2)
assert.ok(project && worktree && ['apply', 'check'].includes(mode), 'Usage: apply_webkit_overlays.mjs PROJECT WORKTREE [apply|check]')
const mapping = JSON.parse(fs.readFileSync(path.join(project, 'wpe-drm/webkit-overlays.json')))
const destinations = new Set()
const safe = name => !path.isAbsolute(name) && !name.split('/').includes('..')
for (const [source, targets] of Object.entries(mapping)) {
  for (const destination of Array.isArray(targets) ? targets : [targets]) {
  assert.ok(safe(source) && source.startsWith('wpe-drm/'), `invalid overlay source: ${source}`)
  assert.ok(safe(destination) && destination.startsWith('Source/'), `invalid overlay destination: ${destination}`)
  assert.ok(!destinations.has(destination), `duplicate overlay: ${destination}`)
  destinations.add(destination)
  const input = path.join(project, source)
  const output = path.join(worktree, destination)
  if (mode === 'apply') {
    fs.mkdirSync(path.dirname(output), { recursive: true })
    fs.copyFileSync(input, output)
  }
  assert.ok(fs.readFileSync(input).equals(fs.readFileSync(output)), `overlay mismatch: ${destination}`)
  }
}
console.log(`WebKit overlays ${mode}: ${destinations.size} files`)
