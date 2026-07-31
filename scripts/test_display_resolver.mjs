import assert from 'node:assert/strict'
import fs from 'node:fs'

const source = fs.readFileSync(new URL('../src/utils/display-resolver.js', import.meta.url), 'utf8')
const display = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`)

function player(config) {
  return {
    getSystemDisplayConfig() {
      return JSON.stringify(config)
    },
    getDrmScreenSize() {
      return config.drmMode || ''
    },
  }
}

const portrait = {
  width: 568,
  height: 1210,
  frameworkRotation: 180,
  videoRotation: 90,
  touchRotation: 180,
  touchDevice: '/dev/input/by-path/touchscreen',
  drmMode: '568x1210',
}

const nativePortrait = await display.resolveDisplayConfig({}, player(portrait), {
  browserMode: 'native',
})
assert.equal(nativePortrait.panelSize, '568x1210')
assert.equal(nativePortrait.viewport, '568x1210')
assert.equal(nativePortrait.rotation, 180)
assert.equal(nativePortrait.touchRotation, 180)
assert.equal(nativePortrait.touchDevice, portrait.touchDevice)

const rotatedPortrait = await display.resolveDisplayConfig({}, player(portrait), {
  browserMode: 'rotate270',
})
assert.equal(rotatedPortrait.panelSize, '1210x568')
assert.equal(rotatedPortrait.rotation, 90)
assert.equal(rotatedPortrait.touchRotation, 90)

const legacyLandscape = await display.resolveDisplayConfig({}, player({
  width: 266,
  height: 960,
  frameworkRotation: 270,
  touchRotation: 270,
  drmMode: '480x960',
}), { browserMode: 'native' })
assert.equal(legacyLandscape.panelSize, '960x266')
assert.equal(legacyLandscape.drmMode, '480x960')
assert.equal(legacyLandscape.rotation, 270)
assert.equal(legacyLandscape.touchRotation, 270)

assert.equal(display.normalizeBrowserMode('rotate270'), 'rotate270')
assert.equal(display.normalizeBrowserMode('invalid'), 'native')

console.log('display resolver fixtures passed: 4')
