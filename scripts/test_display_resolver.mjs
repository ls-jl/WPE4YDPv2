import assert from 'node:assert/strict'
import fs from 'node:fs'

const source = fs.readFileSync(new URL('../src/utils/display-resolver.js', import.meta.url), 'utf8')
const display = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`)

assert.equal(display.dataRootPath(), '', 'missing MiniApp data directory must fail closed')
assert.equal(display.browserDataPath('wpe-drm.log'), '', 'browser path must not use shared /tmp storage')

function player(config, persistedMode = 'native') {
  return {
    getSystemDisplayConfig() {
      return JSON.stringify(config)
    },
    getDrmScreenSize() {
      return config.drmMode || ''
    },
    getDisplayMode() {
      return persistedMode
    },
  }
}

function parseSize(spec) {
  const match = `${spec}`.match(/^(\d+)x(\d+)$/)
  assert.ok(match, `invalid size ${spec}`)
  return { width: Number(match[1]), height: Number(match[2]) }
}

function framebufferSize(panelSpec, rotation) {
  const panel = parseSize(panelSpec)
  return rotation === 90 || rotation === 270
    ? { width: panel.height, height: panel.width }
    : panel
}

function orientation(size) {
  if (size.width === size.height) return 'square'
  return size.width > size.height ? 'landscape' : 'portrait'
}

function assertOpposite(a, b, message) {
  assert.equal((Number(a) + 180) % 360, Number(b), message)
}

async function resolveModes(config) {
  const entries = await Promise.all([
    'native',
    'rotate90',
    'rotate180',
    'rotate270',
  ].map(async (mode) => [mode, await display.resolveDisplayConfig({}, player(config), {
    browserMode: mode,
  })]))
  return Object.fromEntries(entries)
}

async function assertTwoTemplateDevice(name, config, expected) {
  const modes = await resolveModes(config)
  const drm = parseSize(config.drmMode)

  assert.equal(modes.native.panelSize, expected.nativePanel, `${name}: native panel`)
  assert.equal(modes.rotate180.panelSize, expected.nativePanel, `${name}: rotate180 must reuse native panel`)
  assert.equal(modes.native.viewport, modes.rotate180.viewport, `${name}: native/180 viewport`)
  assert.equal(modes.native.layoutTemplate, 'native', `${name}: native layout template`)
  assert.equal(modes.rotate180.layoutTemplate, 'native', `${name}: rotate180 layout template`)
  assert.equal(modes.native.layoutRotation, modes.rotate180.layoutRotation, `${name}: native/180 layout rotation`)

  assert.equal(modes.rotate90.panelSize, expected.rotatedPanel, `${name}: rotate90 panel`)
  assert.equal(modes.rotate270.panelSize, expected.rotatedPanel, `${name}: rotate270 panel`)
  assert.equal(modes.rotate90.viewport, modes.rotate270.viewport, `${name}: 90/270 viewport`)
  assert.equal(modes.rotate90.layoutTemplate, 'rotate270', `${name}: rotate90 layout template`)
  assert.equal(modes.rotate270.layoutTemplate, 'rotate270', `${name}: rotate270 layout template`)
  assert.equal(modes.rotate90.layoutRotation, modes.rotate270.layoutRotation, `${name}: 90/270 layout rotation`)

  assertOpposite(modes.native.rotation, modes.rotate180.rotation, `${name}: native/180 output rotation`)
  assertOpposite(modes.native.touchRotation, modes.rotate180.touchRotation, `${name}: native/180 touch rotation`)
  assertOpposite(modes.rotate90.rotation, modes.rotate270.rotation, `${name}: 90/270 output rotation`)
  assertOpposite(modes.rotate90.touchRotation, modes.rotate270.touchRotation, `${name}: 90/270 touch rotation`)

  for (const [mode, resolved] of Object.entries(modes)) {
    assert.equal(
      orientation(framebufferSize(resolved.panelSize, resolved.rotation)),
      orientation(drm),
      `${name}: ${mode} framebuffer orientation must match DRM envelope`,
    )
    assert.equal(resolved.touchDevice, config.touchDevice, `${name}: ${mode} touch device`)
  }
  return modes
}

const legacyX7 = await assertTwoTemplateDevice('266x960-x7', {
  width: 266,
  height: 960,
  frameworkRotation: 270,
  videoRotation: 270,
  touchRotation: 270,
  touchDevice: '/dev/input/by-path/hyn_ts',
  drmMode: '480x960',
}, {
  nativePanel: '960x266',
  rotatedPanel: '266x960',
})
assert.equal(legacyX7.native.rotation, 270)
assert.equal(legacyX7.rotate90.rotation, 0)
assert.equal(legacyX7.rotate180.rotation, 90)
assert.equal(legacyX7.rotate270.rotation, 180)

const wideX7 = await assertTwoTemplateDevice('936x280-x7', {
  width: 936,
  height: 280,
  frameworkRotation: 270,
  videoRotation: 270,
  touchRotation: 270,
  touchDevice: '/dev/input/by-path/sitronix_ts_spi',
  drmMode: '280x936',
}, {
  nativePanel: '936x280',
  rotatedPanel: '280x936',
})
assert.equal(wideX7.native.rotation, 270)
assert.equal(wideX7.rotate90.rotation, 0)
assert.equal(wideX7.rotate180.rotation, 90)
assert.equal(wideX7.rotate270.rotation, 180)

const portraitX7 = await assertTwoTemplateDevice('568x1210-x7', {
  width: 568,
  height: 1210,
  frameworkRotation: 180,
  videoRotation: 90,
  touchRotation: 180,
  touchDevice: '/dev/input/by-path/touchscreen',
  drmMode: '568x1210',
}, {
  nativePanel: '568x1210',
  rotatedPanel: '1210x568',
})
assert.equal(portraitX7.native.rotation, 180)
assert.equal(portraitX7.rotate90.rotation, 270)
assert.equal(portraitX7.rotate180.rotation, 0)
assert.equal(portraitX7.rotate270.rotation, 90)

assert.equal(display.browserLayoutTemplate('native'), 'native')
assert.equal(display.browserLayoutTemplate('rotate180'), 'native')
assert.equal(display.browserLayoutTemplate('rotate90'), 'rotate270')
assert.equal(display.browserLayoutTemplate('rotate270'), 'rotate270')
assert.equal(display.browserLayoutDelta('rotate90'), 270)
assert.equal(display.browserModeDelta('rotate90'), 90)
assert.equal(display.normalizeBrowserMode('invalid'), 'native')
assert.equal(display.normalizeBrowserLaunchMode('last'), 'last')
assert.equal(display.normalizeBrowserLaunchMode(), 'last')
assert.equal(display.resolvePersistedBrowserMode({
  getDisplayMode() { return 'rotate180' },
}, { browserMode: 'last', workdir: '/tmp/browser' }), 'rotate180')
assert.equal(display.resolvePersistedBrowserMode({
  getDisplayMode() { return 'invalid' },
}, { browserMode: 'last', workdir: '/tmp/browser' }), 'native')

const directStart = await display.browserOptions({ $workspace: '/pkg' }, player({
  width: 936,
  height: 280,
  frameworkRotation: 270,
  touchRotation: 270,
  touchDevice: '/dev/input/by-path/sitronix_ts_spi',
  drmMode: '280x936',
}), {
  browserMode: 'rotate90',
  runtimePath: '/pkg/assets/wpe-runtime',
  workdir: '/data/browser',
})
const menuRestart = await display.browserOptions({ $workspace: '/pkg' }, player({
  width: 936,
  height: 280,
  frameworkRotation: 270,
  touchRotation: 270,
  touchDevice: '/dev/input/by-path/sitronix_ts_spi',
  drmMode: '280x936',
}, 'rotate90'), {
  browserMode: 'last',
  runtimePath: '/pkg/assets/wpe-runtime',
  workdir: '/data/browser',
})
for (const key of ['browserMode', 'panelSize', 'viewport', 'drmMode', 'layoutTemplate', 'layoutRotation', 'rotation', 'touchRotation']) {
  assert.equal(menuRestart[key], directStart[key], `launcher/menu restart must share ${key}`)
}

console.log('display resolver fixtures passed: 3 devices x 4 modes, two locked layout templates')
