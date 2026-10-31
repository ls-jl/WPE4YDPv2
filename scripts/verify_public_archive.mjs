import { execFileSync } from 'node:child_process'

const archive = process.argv[2] || '8001779591038449.1_0_0.amr'
try {
  const entries = execFileSync('unzip', ['-Z1', archive], { encoding: 'utf8', maxBuffer: 8 * 1024 * 1024 }).trim().split('\n')
  if (!entries.length || !entries[0]) throw new Error('empty archive')
  const forbidden = entries.filter(name =>
    name.startsWith('/') || name.includes('\\') || name.split('/').includes('..') ||
    /(?:^|\/)gpu\/(?:mali|modules)(?:\/|$)/i.test(name) ||
    /(?:^|\/)libmali[^/]*\.so(?:\.|$)/i.test(name) || /\.ko$/i.test(name))
  if (forbidden.length) {
    throw new Error(`public release contains private GPU/kernel assets or unsafe paths:\n${forbidden.join('\n')}`)
  }
  console.log(`Public archive policy passed: ${archive}`)
} catch (error) {
  console.error(`Public release blocked: ${error.message}`)
  process.exitCode = 1
}
