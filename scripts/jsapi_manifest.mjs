import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'

const [operation, binary, manifest, compiler] = process.argv.slice(2)
const root = process.env.PROJECT_ROOT || process.cwd()
const hash = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex')
const inputs = {}
function walk(directory) {
  for (const item of fs.readdirSync(directory, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name))) {
    const file = path.join(directory, item.name)
    if (item.isDirectory()) walk(file)
    else if (item.isFile() && /\.(?:cpp|c|h)$|CMakeLists\.txt$/.test(file)) inputs[path.relative(root, file)] = hash(file)
  }
}
walk(path.join(root, 'jsapi/src'))
walk(path.join(root, 'jsapi/iot-miniapp-sdk'))
inputs['jsapi/CMakeLists.txt'] = hash(path.join(root, 'jsapi/CMakeLists.txt'))
if (operation === 'write') {
  const data = { schema: 1, sha256: hash(binary), source_inputs: inputs,
    compiler, compiler_version: execFileSync(compiler, ['--version'], { encoding: 'utf8' }).trim() }
  fs.writeFileSync(`${manifest}.tmp`, JSON.stringify(data, null, 2))
  fs.renameSync(`${manifest}.tmp`, manifest)
} else if (operation === 'check') {
  const data = JSON.parse(fs.readFileSync(manifest))
  if (data.schema !== 1 || data.sha256 !== hash(binary) || JSON.stringify(data.source_inputs) !== JSON.stringify(inputs)) {
    throw new Error('JSAPI binary/source provenance mismatch')
  }
} else throw new Error('Expected write|check')
console.log(`JSAPI provenance ${operation} passed`)
