import assert from 'node:assert/strict'
import fs from 'node:fs'

const source = fs.readFileSync(new URL('../src/utils/keyboard-event.js', import.meta.url), 'utf8')
const keyboard = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`)

const fixtures = [
  ['direct string', 'hello', true, 'hello'],
  ['detail value', { detail: { value: '中文' } }, true, '中文'],
  ['target empty', { target: { value: '' } }, true, ''],
  ['current target text', { currentTarget: { text: 'abc' } }, true, 'abc'],
  ['records', { records: [{ text: 'record' }] }, true, 'record'],
  ['root array', [{ detail: { value: 'array' } }], true, 'array'],
  ['json wrapper', '{"detail":{"value":"json"}}', true, 'json'],
  ['malformed object', { detail: { payload: 1 } }, false, ''],
]

for (const [name, input, found, text] of fixtures) {
  const result = keyboard.normalizeKeyboardEvent(input)
  assert.equal(result.found, found, `${name}: found`)
  assert.equal(result.text, text, `${name}: text`)
  assert.notEqual(result.text, '[object Object]', `${name}: object coercion`)
}

assert.deepEqual(
  keyboard.parseTextEditResult({ detail: { editConfirmed: true, text: '' } }),
  { terminal: true, confirmed: true, found: true, text: '' },
)
assert.deepEqual(
  keyboard.parseTextEditResult('{"editConfirmed":false,"text":"ignored"}'),
  { terminal: true, confirmed: false, found: true, text: 'ignored' },
)
assert.deepEqual(
  keyboard.parseTextEditResult({ records: [{ editConfirmed: 'true', value: 'record-ok' }] }),
  { terminal: true, confirmed: true, found: true, text: 'record-ok' },
)

console.log(`keyboard event fixtures passed: ${fixtures.length + 3}`)
