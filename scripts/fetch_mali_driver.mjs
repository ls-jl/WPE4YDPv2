import { createHash } from 'node:crypto'
import { execFileSync } from 'node:child_process'
import { readFileSync, existsSync, mkdirSync, renameSync, rmSync } from 'node:fs'
import { dirname, resolve } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..')
const source = JSON.parse(readFileSync(resolve(root, 'licenses/mali/source.json')))
const hash = path => createHash('sha256').update(readFileSync(path)).digest('hex')
for (const [name, expected] of Object.entries(source.notices)) {
  if (hash(resolve(root, 'licenses/mali', name)) !== expected)
    throw new Error(`Mali notice checksum mismatch: ${name}`)
}
const directory = process.argv[2]
if (!directory) throw new Error('Usage: node scripts/fetch_mali_driver.mjs CACHE_DIRECTORY')
mkdirSync(directory, { recursive: true })
const output = resolve(directory, 'libmali.so.1')
if (existsSync(output)) {
  if (hash(output) !== source.sha256) throw new Error(`Mali cache checksum mismatch: ${output}`)
} else {
  const temporary = `${output}.${process.pid}.tmp`
  try {
    execFileSync('curl', ['--fail', '--location', '--silent', '--show-error',
      '--proto', '=https', '--tlsv1.2', '--max-time', '180', '--retry', '2',
      `https://raw.githubusercontent.com/JeffyCN/mirrors/${source.revision}/${source.path}`,
      '--output', temporary], { stdio: 'inherit' })
    if (hash(temporary) !== source.sha256) throw new Error('Downloaded Mali checksum mismatch')
    renameSync(temporary, output)
  } finally {
    rmSync(temporary, { force: true })
  }
}
console.log(output)
