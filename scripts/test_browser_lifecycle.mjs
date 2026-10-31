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

const delay = ms => new Promise(resolve => setTimeout(resolve, ms))
function controller() {
  const component = {
    pageVisible: true,
    browser: lifecycle.createBrowserLifecycleState(),
    pageOptions: () => ({ workdir: '/tmp/lifecycle-fixture' }),
  }
  const calls = []
  const native = {
    isBrowserRunning: () => false,
    stopBrowser: () => calls.push('stop'),
    startBrowser: () => { calls.push('start'); return 123 },
  }
  const instance = new lifecycle.BrowserLifecycle(component, native, null)
  instance.leaveFrame = reason => calls.push(`leave:${reason}`)
  return { instance, component, calls }
}

{
  const { instance, component, calls } = controller()
  let rejectOld
  instance.resolveOptions = () => new Promise((resolve, reject) => { rejectOld = reject })
  instance.start()
  await delay(10)
  instance.stop()
  component.browser.running = true
  component.browser.starting = true
  rejectOld(new Error('stale rejection'))
  await delay(10)
  assert.deepEqual(calls, ['stop'], 'stale failures must not leave the new session')
  assert.equal(component.browser.running, true)
  assert.equal(component.browser.starting, true, 'stale finally must not clear new startup')
}
{
  const { instance, component, calls } = controller()
  instance.restartForRotation('rotate90')
  component.pageVisible = false
  instance.stop()
  await delay(130)
  assert.deepEqual(calls, ['stop'], 'rotation timer must not start after stop/hide')
}
{
  const { instance, component, calls } = controller()
  component.pageVisible = false
  instance.start()
  await delay(10)
  assert.deepEqual(calls, [], 'a hidden page cannot start WPE')
}
{
  const { instance, component, calls } = controller()
  const navigations = []
  globalThis.$falcon = { navTo: (...args) => navigations.push(args) }
  instance.leaveFrame = lifecycle.BrowserLifecycle.prototype.leaveFrame.bind(instance)
  instance.leaveFrame('closed')
  component.pageVisible = false
  instance.stop()
  await delay(130)
  assert.equal(navigations.length, 0, 'hidden or unloaded page cannot navigate later')
  component.pageVisible = true
  instance.scheduleReturn()
  await delay(130)
  assert.equal(navigations.length, 1, 'show resumes pending exit instead of restarting WPE')
  assert.equal(navigations[0][1].browserStatus, 'closed')
  assert.deepEqual(calls, ['stop', 'stop'])
  delete globalThis.$falcon
}
console.log('browser lifecycle async fixtures passed: 4')
