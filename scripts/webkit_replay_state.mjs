import fs from 'node:fs'
import path from 'node:path'
import crypto from 'node:crypto'
import { execFileSync } from 'node:child_process'

const [operation, project, worktree] = process.argv.slice(2)
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex')
const git = args => execFileSync('git', ['-C', worktree, ...args], { maxBuffer: 64 * 1024 * 1024 })
const patchRoot = path.join(project, 'wpe-drm/webkit-patches')
const revision = fs.readFileSync(path.join(patchRoot, 'revision'), 'utf8').trim()
const inputHashes = []
for (const name of ['series', 'inputs']) {
  const contents = fs.readFileSync(path.join(patchRoot, name), 'utf8')
  inputHashes.push(contents)
  for (const line of contents.split('\n').map(x => x.trim()).filter(x => x && !x.startsWith('#'))) {
    inputHashes.push(digest(fs.readFileSync(path.join(name === 'series' ? patchRoot : project, line))))
  }
}
const state = {
  revision, inputs: digest(inputHashes.join('\n')),
  head: git(['rev-parse', 'HEAD']).toString().trim(),
  tracked: digest(git(['diff', '--no-ext-diff', '--binary', 'HEAD'])), untracked: {},
}
for (const file of git(['ls-files', '--others', '--exclude-standard', '-z']).toString().split('\0').filter(Boolean).sort()) {
  if (file === '.wpe-replay.json') continue
  state.untracked[file] = digest(fs.readFileSync(path.join(worktree, file)))
}
const filename = path.join(worktree, '.wpe-replay.json')
if (state.head !== revision) throw new Error('Worktree HEAD differs from pinned baseline')
if (operation === 'write') {
  fs.writeFileSync(filename, JSON.stringify(state, null, 2))
} else if (operation === 'check') {
  if (JSON.stringify(JSON.parse(fs.readFileSync(filename))) !== JSON.stringify(state)) {
    throw new Error('Existing worktree input/content fingerprint changed; preserving it')
  }
} else throw new Error('Expected write|check')
