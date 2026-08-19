import assert from 'node:assert/strict'
import fs from 'node:fs'

let source = fs.readFileSync(new URL('../src/utils/browser-lifecycle.js', import.meta.url), 'utf8')
source = source.replace(
  /import \{[\s\S]*?\} from '\.\/display-resolver'/,
  `const LAST_BROWSER_MODE = 'last'
const browserDataPath = () => ''
const browserOptions = async () => ({})
const normalizeBrowserMode = (value) => ['native', 'rotate90', 'rotate180', 'rotate270'].includes(String(value)) ? String(value) : 'native'`,
)
const lifecycle = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`)

assert.deepEqual(lifecycle.parseBrowserExitStatus(''), {
  reason: 'exited', code: null, browserMode: '', message: '浏览器已退出',
})
assert.deepEqual(lifecycle.parseBrowserExitStatus(JSON.stringify({
  reason: 'rotation_change', code: 72, browserMode: 'rotate90',
})), {
  reason: 'rotation_change', code: 72, browserMode: 'rotate90', message: '正在应用显示方向',
})
assert.equal(lifecycle.parseBrowserExitStatus(JSON.stringify({
  reason: 'rotation_change', code: 72, browserMode: 'invalid',
})).browserMode, 'native')
assert.equal(lifecycle.parseBrowserExitStatus(JSON.stringify({
  reason: 'user_shutdown', code: 74,
})).reason, 'user_shutdown')
assert.equal(lifecycle.parseBrowserExitStatus(JSON.stringify({
  reason: 'start_failed', code: 91,
})).message, '浏览器启动失败（错误码 91）')
assert.equal(lifecycle.parseBrowserExitStatus('{bad json').reason, 'invalid')

console.log('browser lifecycle exit fixtures passed: 6')
