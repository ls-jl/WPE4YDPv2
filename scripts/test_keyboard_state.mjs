import assert from 'node:assert/strict'
import fs from 'node:fs'

const moduleURL = source => `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`
const eventURL = moduleURL(fs.readFileSync('src/utils/keyboard-event.js', 'utf8'))
let uuidCounter = 0
const managers = []
globalThis.__keyboardFixtureGlobal = class {
  constructor() {
    this.closed = []
    this.textEditFinished = { on: fn => { this.listener = fn }, off: () => { this.listener = null } }
    managers.push(this)
  }
  startTextEdit() { return `uuid-${++uuidCounter}` }
  closeTextEdit(uuid) { this.closed.push(uuid) }
}
const sessionURL = moduleURL(fs.readFileSync('src/utils/keyboard.js', 'utf8')
  .replace("import globalModule from 'global'", 'const globalModule = { Global: globalThis.__keyboardFixtureGlobal }')
  .replace("'./keyboard-event'", JSON.stringify(eventURL)))
const { KeyboardSession } = await import(sessionURL)
const bridgeURL = moduleURL(fs.readFileSync('src/utils/keyboard-bridge.js', 'utf8')
  .replace("'./keyboard'", JSON.stringify(sessionURL))
  .replace("import { keyboardBackendOverride } from './display-resolver'", "const keyboardBackendOverride = () => 'auto'")
  .replace("'./keyboard-event'", JSON.stringify(eventURL)))
const { KeyboardBridge, createKeyboardBridgeState } = await import(bridgeURL)
const delay = ms => new Promise(resolve => setTimeout(resolve, ms))

function bridgeFixture() {
  const component = { keyboard: createKeyboardBridgeState(), browser: { leaving: false } }
  const bridge = new KeyboardBridge(component, {}, () => '/tmp/keyboard-fixture', () => true)
  Object.assign(component.keyboard, {
    activeRequest: { id: 'request-1' }, activeBackend: 'textarea',
    textareaSeenInput: true, textareaValue: '12345', phase: 'active',
  })
  bridge.markBackendSuccess = () => {}
  const completions = []
  bridge.finishRequest = (confirmed, text) => completions.push({ confirmed, text })
  return { bridge, component, completions }
}
for (const text of ['123456', '']) {
  const { bridge, completions } = bridgeFixture()
  bridge.onTextareaConfirm({ detail: { value: text } })
  assert.deepEqual(completions, [{ confirmed: true, text }], 'terminal text is authoritative including empty')
}
{
  const { bridge, completions } = bridgeFixture()
  bridge.onTextareaConfirm({})
  assert.equal(completions[0].text, '12345', 'missing terminal text uses last input')
}
{
  const { bridge, completions } = bridgeFixture()
  bridge.onTextareaFinished({ editConfirmed: true, text: '123456' })
  assert.equal(completions[0].text, '123456')
}
{
  let confirmed = ''
  let cancelled = 0
  const session = new KeyboardSession({ onConfirm: text => { confirmed = text; session.close() }, onCancel: () => cancelled++ })
  session.mount()
  const manager = managers.at(-1)
  const uuid = session.open({ closeDelayMs: 30 })
  manager.listener(uuid, { text: 'partial' })
  await delay(10)
  assert.equal(cancelled, 0, 'nonterminal event cannot cancel global session')
  manager.listener(uuid, { editConfirmed: true, text: 'complete' })
  await delay(10)
  assert.equal(confirmed, 'complete')
  assert.deepEqual(manager.closed, [], 'reentrant bridge.close must not bypass deferred native close')
  await delay(40)
  assert.deepEqual(manager.closed, [uuid])
  session.unmount()
  assert.deepEqual(manager.closed, [uuid], 'unmount must not close UUID twice')
  const next = new KeyboardSession()
  next.mount()
  assert.notEqual(managers.at(-1), manager, 'native keyboard object belongs to page/session')
  next.unmount()
}
{
  const { bridge, component } = bridgeFixture()
  component.keyboard.activeBackend = 'global'
  component.keyboard.session = { open: () => 'accepted-uuid', isActive: () => false }
  const evidence = []
  bridge.markBackendSuccess = (backend, reason) => evidence.push(reason)
  bridge.openGlobal(component.keyboard.activeRequest)
  assert.deepEqual(evidence, [], 'a UUID alone is not evidence of keyboard visibility')
  assert.equal(component.keyboard.probeTimer, null, 'silent legacy IMEs must not be closed while typing')
  bridge.clearBackendTimers()
}
{
  let session
  session = new KeyboardSession({ onConfirm: () => session.unmount() })
  session.mount()
  const manager = managers.at(-1)
  const uuid = session.open({ closeDelayMs: 30 })
  session.finish('done')
  await delay(40)
  assert.deepEqual(manager.closed, [uuid], 'reentrant unload flushes pending close exactly once')
}
delete globalThis.__keyboardFixtureGlobal
console.log('keyboard state fixtures passed: terminal text, empty, nonterminal, close ownership, backend evidence')
