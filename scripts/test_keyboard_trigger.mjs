import assert from 'node:assert/strict'
import fs from 'node:fs'
import vm from 'node:vm'

function extractKeyboardScript() {
  const source = fs.readFileSync('wpe-drm/wpe-drm-minimal.c', 'utf8')
  const functionStart = source.indexOf('static void setup_keyboard_user_script')
  const functionEnd = source.indexOf('\nstatic gboolean remove_dir_contents', functionStart)
  assert.ok(functionStart >= 0 && functionEnd > functionStart, 'keyboard script function not found')
  const block = source.slice(functionStart, functionEnd)
  const assignment = block.indexOf('const char *source =')
  assert.ok(assignment >= 0, 'keyboard script assignment not found')
  const literals = []
  for (const line of block.slice(assignment).split('\n')) {
    const match = line.match(/^\s*("(?:\\.|[^"\\])*")\s*;?\s*$/)
    if (match) literals.push(JSON.parse(match[1]))
  }
  assert.ok(literals.length > 20, 'keyboard script extraction incomplete')
  return literals.join('')
}

class FakeElement {
  constructor(tagName, attributes = {}) {
    this.tagName = tagName.toUpperCase()
    this.attributes = { ...attributes }
    this.type = attributes.type || ''
    this.value = ''
    this.disabled = false
    this.readOnly = false
    this.isContentEditable = false
    this.parentElement = null
  }

  getAttribute(name) {
    return Object.prototype.hasOwnProperty.call(this.attributes, name)
      ? this.attributes[name]
      : null
  }

  setAttribute(name, value) {
    this.attributes[name] = `${value}`
  }

  dispatchEvent() {
    return true
  }

  focus() {}

  setSelectionRange() {}
}

function createHarness(script, commands = []) {
  const listeners = new Map()
  const messages = []
  const document = {
    activeElement: null,
    documentElement: { contains: () => true },
    addEventListener(name, callback) {
      if (!listeners.has(name)) listeners.set(name, [])
      listeners.get(name).push(callback)
    },
    querySelector() {
      return null
    },
    createEvent() {
      return {
        initEvent() {},
      }
    },
  }
  const context = {
    console,
    document,
    navigator: { language: 'zh-CN' },
    setTimeout,
    clearTimeout,
    Promise,
    Date,
    encodeURIComponent,
    decodeURIComponent,
    isFinite,
    parseInt,
    HTMLInputElement: function HTMLInputElement() {},
    HTMLTextAreaElement: function HTMLTextAreaElement() {},
    InputEvent: function InputEvent() {},
  }
  context.window = context
  context.window.webkit = {
    messageHandlers: {
      haasKeyboard: {
        postMessage(message) {
          messages.push(message)
          const operation = new URLSearchParams(message).get('op')
          if (operation === 'open') return 'op=opened&id=test-request'
          if (operation === 'wait') return commands.length ? commands.shift() : new Promise(() => {})
          return 'op=invalid'
        },
      },
    },
  }
  vm.runInNewContext(script, context, { filename: 'haas-keyboard-user-script.js' })

  return {
    document,
    messages,
    dispatch(name, target, path = [target], isTrusted = true) {
      target.ownerDocument = document
      target.isConnected = true
      const event = {
        type: name,
        target,
        isTrusted,
        composedPath: () => path,
      }
      for (const callback of listeners.get(name) || []) callback(event)
    },
    async flush(ms = 5) {
      await new Promise((resolve) => setTimeout(resolve, ms))
    },
  }
}

const keyboardScript = extractKeyboardScript()
new Function(keyboardScript)

{
  const harness = createHarness(keyboardScript)
  const otp = new FakeElement('input', {
    type: 'text',
    inputmode: 'numeric',
    autocomplete: 'one-time-code',
    placeholder: '请输入验证码',
  })
  harness.document.activeElement = otp
  harness.dispatch('focusin', otp)
  await harness.flush()
  assert.equal(harness.messages.length, 0, 'script autofocus must not open keyboard')
}

{
  const harness = createHarness(keyboardScript)
  const wrapper = new FakeElement('div')
  const otp = new FakeElement('input', {
    type: 'text',
    inputmode: 'numeric',
    autocomplete: 'one-time-code',
    placeholder: '请输入验证码',
  })
  otp.parentElement = wrapper
  harness.dispatch('pointerdown', wrapper)
  harness.document.activeElement = otp
  // Site code calls focus() from a trusted touch handler. WebKit may mark the
  // resulting focusin as synthetic, so trust comes from the preceding touch.
  harness.dispatch('focusin', otp, [otp, wrapper], false)
  await harness.flush()

  const openMessages = harness.messages.filter((message) =>
    new URLSearchParams(message).get('op') === 'open')
  assert.equal(openMessages.length, 1)
  const request = new URLSearchParams(openMessages[0])
  assert.equal(request.get('trigger'), 'focusin')
  assert.equal(request.get('inputType'), 'Number')
  assert.equal(request.get('maxlength'), '8')
  assert.equal(request.get('inputmode'), 'numeric')

  harness.dispatch('click', otp, [otp, wrapper])
  await harness.flush()
  assert.equal(harness.messages.filter((message) =>
    new URLSearchParams(message).get('op') === 'open').length, 1,
  'focusin and click from one activation must be deduplicated')
}

{
  const harness = createHarness(keyboardScript)
  const input = new FakeElement('input', {
    type: 'tel',
    maxlength: '6',
  })
  harness.dispatch('click', input)
  await harness.flush()
  const request = new URLSearchParams(harness.messages[0])
  assert.equal(request.get('trigger'), 'click')
  assert.equal(request.get('inputType'), 'Number')
  assert.equal(request.get('maxlength'), '6')
}

{
  const harness = createHarness(keyboardScript, ['op=commit&id=test-request&sequence=1&text=123456'])
  const input = new FakeElement('input', { type: 'tel' })
  harness.document.documentElement.contains = () => false // Shadow DOM boundary.
  harness.dispatch('click', input)
  await harness.flush(130)
  const ack = harness.messages.map(message => new URLSearchParams(message)).find(message => message.has('ackSequence'))
  assert.equal(input.value, '123456')
  assert.equal(ack.get('applied'), '1')
  assert.equal(ack.get('alive'), '1')
}
{
  const harness = createHarness(keyboardScript, ['op=commit&id=test-request&sequence=1&text=wrong-target'])
  const input = new FakeElement('input')
  const unrelated = new FakeElement('input')
  harness.dispatch('click', input)
  await harness.flush(10)
  input.isConnected = false
  unrelated.ownerDocument = harness.document
  unrelated.isConnected = true
  harness.document.activeElement = unrelated
  await harness.flush(130)
  const ack = harness.messages.map(message => new URLSearchParams(message)).find(message => message.has('ackSequence'))
  assert.equal(ack.get('applied'), '0', 'detached target cannot report success')
  assert.equal(ack.get('alive'), '0')
  assert.equal(unrelated.value, '', 'must not fall back to unrelated focused control')
}
console.log('keyboard trigger fixtures passed: activation, OTP, Shadow DOM writeback, detached target')
