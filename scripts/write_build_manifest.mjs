import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'
import assert from 'node:assert/strict'
import { captureNativeSourceInputs } from './native_source_inputs.mjs'

const [output, stage, cacheFile, revision, seriesHash] = process.argv.slice(2)
const root = process.env.PROJECT_ROOT || process.cwd()
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
const configuration = {}
for (const line of fs.readFileSync(cacheFile, 'utf8').split('\n')) {
  const match = line.match(/^([^#/:][^:]*):[^=]+=(.*)$/)
  if (match) configuration[match[1]] = match[2]
}
const buildFlags = {}
for (const [key, value] of Object.entries(configuration)) {
  if (/^(ENABLE_|USE_)/.test(key)) buildFlags[key] = value === 'ON' ? true : value === 'OFF' ? false : value
}
const sources = captureNativeSourceInputs(root)
const captured = path.join(stage, '.source-inputs.json')
assert.ok(fs.existsSync(captured), 'Missing start-of-build source snapshot')
assert.deepEqual(JSON.parse(fs.readFileSync(captured)), sources,
  'Native inputs changed during compilation; preserve the stage and rebuild')
const artifacts = {}
function recordArtifacts(directory) {
  for (const item of fs.readdirSync(directory, { withFileTypes: true })) {
    const name = path.join(directory, item.name)
    if (item.isDirectory()) recordArtifacts(name)
    else if (item.isFile()) {
      const fd = fs.openSync(name, 'r')
      const magic = Buffer.alloc(4)
      fs.readSync(fd, magic, 0, 4, 0)
      fs.closeSync(fd)
      if (magic.equals(Buffer.from([0x7f, 0x45, 0x4c, 0x46]))) artifacts[path.relative(stage, name)] = digest(name)
    }
  }
}
recordArtifacts(stage)
const compiler = configuration.CMAKE_CXX_COMPILER
if (!compiler) throw new Error('Missing actual CMake compiler identity')
const dependencyInputs = {}
for (const name of ['local-inputs.sha256', 'debian-packages.sha256']) {
  const file = path.join(process.env.WPE_SDK_ROOT, name)
  dependencyInputs[name] = digest(file)
}
const manifest = {
  schema: 2, webkit_revision: revision, patch_series_sha256: seriesHash,
  build_flags: buildFlags, source_inputs: sources, dependency_inputs: dependencyInputs,
  toolchain: { compiler, version: execFileSync(compiler, ['--version'], { encoding: 'utf8' }).trim(),
    c_flags: configuration.CMAKE_C_FLAGS, cxx_flags: configuration.CMAKE_CXX_FLAGS,
    link_flags: configuration.CMAKE_SHARED_LINKER_FLAGS, sysroot: configuration.CMAKE_SYSROOT,
    pgo: { file: configuration.PGO_PROFILE_PATH, sha256: digest(configuration.PGO_PROFILE_PATH) } },
  artifacts,
}
fs.writeFileSync(`${output}.tmp`, JSON.stringify(manifest, null, 2), { mode: 0o600 })
fs.renameSync(`${output}.tmp`, output)
console.log(`Build manifest: ${output}`)
