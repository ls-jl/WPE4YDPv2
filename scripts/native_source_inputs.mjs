import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import assert from 'node:assert/strict'
import { fileURLToPath } from 'node:url'

export function captureNativeSourceInputs(root) {
  const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
  const inputs = {}
  function walk(directory) {
    for (const entry of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
      const file = path.join(directory, entry.name)
      if (entry.isDirectory()) walk(file)
      else if (entry.isFile() && /\.(?:c|cpp|h|sh|mjs|patch|json)$/.test(file))
        inputs[path.relative(root, file)] = hash(file)
    }
  }
  walk(path.join(root, 'wpe-drm'))
  for (const name of ['build-env.sh', 'build_webrtc_runtime_pve.sh', 'build_media_runtime.sh',
    'prepare_webkit_worktree.sh', 'apply_webkit_overlays.mjs', 'prepare_sdk.sh', 'prepare_media_sdk.sh',
    'prepare_abi_overlay.sh', 'media-sources.json', 'relocate_sdk_metadata.mjs',
    'fetch_media_sources.mjs', 'sdk_receipt.mjs', 'webkit_replay_state.mjs',
    'native_source_inputs.mjs', 'write_build_manifest.mjs', 'generate_build_manifest.sh'])
    inputs[`scripts/${name}`] = hash(path.join(root, 'scripts', name))
  return inputs
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const [operation, filename] = process.argv.slice(2)
  const inputs = captureNativeSourceInputs(process.env.PROJECT_ROOT || process.cwd())
  if (operation === 'record') fs.writeFileSync(filename, JSON.stringify(inputs, null, 2), { mode: 0o600 })
  else if (operation === 'check') assert.deepEqual(JSON.parse(fs.readFileSync(filename)), inputs,
    'Native source changed during compilation; refusing to attest old binaries')
  else throw new Error('Expected record|check INPUTS_FILE')
  console.log(`Native source inputs ${operation}: ${Object.keys(inputs).length} files`)
}
