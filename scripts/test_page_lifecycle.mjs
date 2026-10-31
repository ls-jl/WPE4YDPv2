import fs from 'node:fs'
import vm from 'node:vm'
import assert from 'node:assert/strict'

const calls = []
class Page {
  constructor(marker) { this.marker = marker }
  onLoad() {}
  onNewOptions() {}
  onShow() { calls.push('baseShow') }
  onHide() { calls.push('baseHide') }
  onUnload() { calls.push('baseUnload') }
}
const context = vm.createContext({ $falcon: { Page } })
vm.runInContext(fs.readFileSync('src/base-page.js', 'utf8').replace('export class BasePage', 'class BasePage') + '\nthis.BasePage = BasePage', context)
const page = new context.BasePage('inherited argument')
assert.equal(page.marker, 'inherited argument')
for (const method of ['onShow', 'onHide', 'onUnload']) page[method]()
assert.deepEqual(calls, ['baseShow', 'baseHide', 'baseUnload'])
page.$root = {
  onShow() { calls.push('rootShow') },
  onHide() { calls.push('rootHide') },
  onUnload() { calls.push('rootUnload') },
  onNewOptions(options) { calls.push(options.value) },
}
for (const method of ['onShow', 'onHide', 'onUnload']) page[method]()
page.onNewOptions({ value: 'updated' })
assert.deepEqual(calls.slice(3), ['baseShow', 'rootShow', 'baseHide', 'rootHide', 'baseUnload', 'rootUnload', 'updated'])
console.log('Page lifecycle passed: early hooks, inherited construction and mounted forwarding')

const displaySource = fs.readFileSync('src/utils/display-resolver.js', 'utf8')
const display = await import(`data:text/javascript;base64,${Buffer.from(displaySource).toString('base64')}`)
const navigationSource = fs.readFileSync('src/utils/navigation.js', 'utf8')
const navigation = await import(`data:text/javascript;base64,${Buffer.from(navigationSource).toString('base64')}`)
Object.assign(context, {
  normalizeBrowserLaunchMode: display.normalizeBrowserLaunchMode,
  DEFAULT_BROWSER_URL: navigation.DEFAULT_BROWSER_URL,
  normalizeLaunchUrl: navigation.normalizeLaunchUrl,
  console,
})
const navigations = []
context.$falcon.navTo = (...args) => navigations.push(args)
const launcherScript = fs.readFileSync('src/pages/index/index.vue', 'utf8')
  .match(/<script>([\s\S]*?)<\/script>/)[1]
  .replace(/^import .*$/gm, '')
  .replace('export default', 'this.Launcher =')
vm.runInContext(launcherScript, context)
const launcherPage = new context.BasePage()
const root = Object.assign(context.Launcher.data(), { $page: launcherPage })
for (const [name, method] of Object.entries(context.Launcher.methods)) root[name] = method.bind(root)
launcherPage.$root = root
context.Launcher.mounted.call(root)
root.launchBrowser()
assert.equal(root.busy, true)
assert.equal(navigations.length, 1)
launcherPage.onNewOptions({ browserStatus: '浏览器已关闭', browserMode: 'last' })
assert.equal(root.busy, false, 'cached launcher must reset via actual page onNewOptions forwarding')
assert.equal(root.statusText, 'Ready')
assert.equal(root.detailText, '浏览器已关闭')
root.launchBrowser()
launcherPage.onShow()
assert.equal(root.busy, false, 'cached launcher must reset via actual page onShow forwarding')
assert.equal(root.statusText, 'Ready')
assert.equal(navigations.length, 2, 'return hooks must not restart the browser')
console.log('Launcher lifecycle passed: cached return, shutdown status and no automatic restart')
