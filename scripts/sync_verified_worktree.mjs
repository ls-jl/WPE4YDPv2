import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import assert from 'node:assert/strict'
import { execFileSync } from 'node:child_process'

const [project, source, destination] = process.argv.slice(2)
assert.ok(project && source && destination && source !== destination, 'Usage: sync_verified_worktree.mjs PROJECT REPLAY INCREMENTAL')
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex')
const git = (root, args) => execFileSync('git', ['-C', root, ...args], { maxBuffer: 64 * 1024 * 1024 })
const receipt = root => JSON.parse(fs.readFileSync(path.join(root, '.wpe-replay.json')))
function contents(root) {
  const untracked = {}
  for (const file of git(root, ['ls-files', '--others', '--exclude-standard', '-z']).toString().split('\0').filter(Boolean).sort()) {
    if (file !== '.wpe-replay.json') untracked[file] = sha(fs.readFileSync(path.join(root, file)))
  }
  return { tracked: sha(git(root, ['diff', '--no-ext-diff', '--binary', 'HEAD'])), untracked }
}
for (const root of [source, destination]) {
  const expected = receipt(root)
  assert.deepEqual(contents(root), { tracked: expected.tracked, untracked: expected.untracked }, `Preserving independently modified worktree: ${root}`)
}
assert.equal(receipt(source).revision, receipt(destination).revision)
const changed = root => new Set([
  ...git(root, ['diff', '--name-only', 'HEAD', '-z']).toString().split('\0'),
  ...Object.keys(contents(root).untracked),
].filter(Boolean))
const files = new Set([...changed(source), ...changed(destination)])
for (const file of files) {
  const input = path.join(source, file)
  const output = path.join(destination, file)
  if (!fs.existsSync(input)) fs.rmSync(output)
  else {
    if (fs.existsSync(output) && fs.readFileSync(input).equals(fs.readFileSync(output))
        && (fs.statSync(input).mode & 0o777) === (fs.statSync(output).mode & 0o777))
      continue;
    fs.mkdirSync(path.dirname(output), { recursive: true })
    fs.copyFileSync(input, output)
    fs.chmodSync(output, fs.statSync(input).mode & 0o777)
  }
}
assert.deepEqual(contents(source), contents(destination), 'Incremental source differs from independent replay')
execFileSync(process.execPath, [path.join(project, 'scripts/webkit_replay_state.mjs'), 'write', project, destination], { stdio: 'inherit' })
console.log(`Verified incremental source update: ${files.size} files`)
