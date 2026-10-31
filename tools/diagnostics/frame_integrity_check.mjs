import fs from 'node:fs'
import assert from 'node:assert/strict'

const [filename, rotationText = '0', insetText = '0'] = process.argv.slice(2)
if (!filename) throw new Error('Usage: frame_integrity_check.mjs OWNED_FRAME.ppm ROTATION TOP_INSET')
const rotation = Number(rotationText)
const inset = Number(insetText)
assert.ok([0, 90, 180, 270].includes(rotation), 'invalid rotation')
assert.ok(Number.isInteger(inset) && inset >= 0, 'invalid inset')
const bytes = fs.readFileSync(filename)
let offset = 0
function token() {
  while (offset < bytes.length) {
    if (bytes[offset] === 35) {
      while (offset < bytes.length && bytes[offset] !== 10) offset++
    } else if (bytes[offset] <= 32) offset++
    else break
  }
  const begin = offset
  while (offset < bytes.length && bytes[offset] > 32) offset++
  return bytes.subarray(begin, offset).toString('ascii')
}
assert.equal(token(), 'P6')
const width = Number(token())
const height = Number(token())
assert.equal(token(), '255')
assert.ok(width > 0 && height > 0 && bytes[offset] <= 32)
if (bytes[offset++] === 13 && bytes[offset] === 10) offset++
assert.equal(bytes.length - offset, width * height * 3, 'unexpected PPM payload')
const panelWidth = rotation === 90 || rotation === 270 ? height : width
const panelHeight = rotation === 90 || rotation === 270 ? width : height
const viewHeight = panelHeight - inset
assert.ok(viewHeight > 32 && panelWidth >= 80, 'frame too small')
function pixel(x, viewY) {
  let y = viewY + inset
  let outputX = x
  let outputY = y
  if (rotation === 90) { outputX = panelHeight - 1 - y; outputY = x }
  if (rotation === 180) { outputX = panelWidth - 1 - x; outputY = panelHeight - 1 - y }
  if (rotation === 270) { outputX = y; outputY = panelWidth - 1 - x }
  const index = offset + (outputY * width + outputX) * 3
  const color = [...bytes.subarray(index, index + 3)]
  if (color.every(value => value > 230)) return 1
  if (color.every(value => value < 25)) return 0
  throw new Error(`unrecognizable barcode pixel at ${x},${viewY}`)
}
const codes = [12, Math.floor(viewHeight / 2) + 4, viewHeight - 12].map(y => {
  const bits = Array.from({ length: 20 }, (_, i) => pixel(Math.floor((i + .5) * panelWidth / 20), y))
  assert.deepEqual(bits.slice(0, 2), [1, 0], 'missing start marker')
  assert.deepEqual(bits.slice(-2), [0, 1], 'missing end marker')
  return bits.slice(2, -2).reduce((value, bit) => (value << 1) | bit, 0)
})
assert.ok(codes.every(value => value === codes[0]), `mixed frame: ${codes.join(',')}`)
console.log(`Owned frame coherent: sequence=${codes[0]} rotation=${rotation} viewport=${panelWidth}x${viewHeight}`)
