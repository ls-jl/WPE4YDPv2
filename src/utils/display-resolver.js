export const DEFAULT_URL = 'https://m.baidu.com/'
export const DEFAULT_VIEWPORT = '960x266'
export const DEFAULT_ROTATION = 270
export const DEFAULT_DRM_MODE = '480x960'
export const DEFAULT_BROWSER_MODE = 'native'

const ROTATE_270_MODE = 'rotate270'
const ROTATE_MODE_DELTA = 270
const HARD_FALLBACK_DISPLAY = {
  panelSize: DEFAULT_VIEWPORT,
  drmMode: DEFAULT_DRM_MODE,
  rotation: DEFAULT_ROTATION,
}

export function normalizeBrowserMode(value) {
  const mode = `${value || ''}`
  return mode === ROTATE_270_MODE ? ROTATE_270_MODE : DEFAULT_BROWSER_MODE
}

function normalizeRotationValue(value, fallback) {
  const rotation = Number(value)
  if (rotation === 0 || rotation === 90 || rotation === 180 || rotation === 270) return rotation
  return fallback
}

function rotateByDelta(rotation, delta) {
  return normalizeRotationValue((Number(rotation) + Number(delta)) % 360, normalizeRotationValue(rotation, 0))
}

function rotationDelta(fromRotation, toRotation) {
  const from = normalizeRotationValue(fromRotation, 0)
  const to = normalizeRotationValue(toRotation, from)
  return (to - from + 360) % 360
}

function parseSizeSpec(value) {
  if (!value) return null
  if (typeof value === 'object') {
    const width = Math.round(Number(value.width))
    const height = Math.round(Number(value.height))
    if (width >= 64 && height >= 64 && width <= 4096 && height <= 4096) {
      return { width, height }
    }
    return null
  }
  const match = `${value}`.trim().match(/^(\d+)\s*[xX,]\s*(\d+)$/)
  if (!match) return null
  const width = Math.round(Number(match[1]))
  const height = Math.round(Number(match[2]))
  if (width < 64 || height < 64 || width > 4096 || height > 4096) return null
  return { width, height }
}

function sizeSpec(size) {
  return size && size.width > 0 && size.height > 0 ? `${size.width}x${size.height}` : ''
}

function normalizeForRotation(size, rotationDeltaValue) {
  if (!size) return null
  const rotated = Number(rotationDeltaValue) === 90 || Number(rotationDeltaValue) === 270
  if (rotated) return { width: size.height, height: size.width }
  return { width: size.width, height: size.height }
}

function rotatedFramebufferSize(panel, rotation) {
  if (!panel) return null
  const swapped = Number(rotation) === 90 || Number(rotation) === 270
  return swapped
    ? { width: panel.height, height: panel.width }
    : { width: panel.width, height: panel.height }
}

function orientation(size) {
  if (!size || size.width === size.height) return 'square'
  return size.width > size.height ? 'landscape' : 'portrait'
}

function reconcilePanelOrientation(panel, rotation, drmSize) {
  if (!panel || !drmSize || orientation(drmSize) === 'square') {
    return { panel, corrected: false }
  }
  const framebuffer = rotatedFramebufferSize(panel, rotation)
  if (orientation(framebuffer) === orientation(drmSize)) {
    return { panel, corrected: false }
  }
  const swapped = { width: panel.height, height: panel.width }
  if (orientation(rotatedFramebufferSize(swapped, rotation)) === orientation(drmSize)) {
    return { panel: swapped, corrected: true }
  }
  return { panel, corrected: false }
}

function parseSystemDisplayConfig(raw) {
  if (!raw) return null
  let config = raw
  if (typeof raw === 'string') {
    try {
      config = JSON.parse(raw)
    } catch (err) {
      console.warn(`parse system display config failed ${err}`)
      return null
    }
  }
  return config && typeof config === 'object' ? config : null
}

function aspectDistance(a, b) {
  if (!a || !b || !a.width || !a.height || !b.width || !b.height) return 0
  const ratioA = a.width / a.height
  const ratioB = b.width / b.height
  if (!ratioA || !ratioB) return 0
  return Math.abs(ratioA - ratioB) / ratioB
}

function sameSize(a, b) {
  return !!(a && b && a.width === b.width && a.height === b.height)
}

function isDrmTransportSize(size, drmSize) {
  return !!(size && drmSize && sameSize(size, drmSize))
}

function isSwappedDrmSize(size, drmSize) {
  return !!(size && drmSize && size.width === drmSize.height && size.height === drmSize.width)
}

function isLikelyFullResolutionDrm(drmSize) {
  if (!drmSize) return false
  return Math.max(drmSize.width, drmSize.height) >= 1000 &&
    Math.min(drmSize.width, drmSize.height) >= 540
}

function falconEnv() {
  try {
    if (typeof $falcon !== 'undefined' && $falcon && $falcon.env) return $falcon.env
  } catch (err) {
    console.warn(`read falcon env failed ${err}`)
  }
  try {
    if (typeof globalThis !== 'undefined' && globalThis.$falcon && globalThis.$falcon.env) {
      return globalThis.$falcon.env
    }
  } catch (err) {
    console.warn(`read global falcon env failed ${err}`)
  }
  return {}
}

function appMeta() {
  try {
    if (typeof $falcon !== 'undefined' && $falcon && $falcon.__AppClazz && $falcon.__AppClazz.meta) {
      return $falcon.__AppClazz.meta
    }
  } catch (err) {
    console.warn(`read app meta failed ${err}`)
  }
  return {}
}

export function keyboardBackendOverride() {
  const props = appMeta().props || {}
  const value = `${props.keyboard_backend || 'auto'}`.toLowerCase()
  return value === 'textarea' || value === 'global' ? value : 'auto'
}

function deviceSkuKey() {
  const env = falconEnv()
  const custom = env && env.custom ? env.custom : {}
  const values = [
    custom.sku,
    custom.product,
    custom.model,
    env.deviceModel,
    env.model,
  ]
  for (let i = 0; i < values.length; i += 1) {
    const value = values[i] ? `${values[i]}`.toLowerCase() : ''
    if (value) return value
  }
  return ''
}

function pickSkuConfig(map) {
  if (!map || typeof map !== 'object') return null
  const sku = deviceSkuKey()
  if (sku) {
    const keys = Object.keys(map)
    for (let i = 0; i < keys.length; i += 1) {
      const key = `${keys[i]}`.toLowerCase()
      if (key !== 'default' && sku.indexOf(key) >= 0) return map[keys[i]]
    }
  }
  return map.default || null
}

function normalizeDisplayConfig(value) {
  const config = value && typeof value === 'object' ? value : {}
  const panel = parseSizeSpec(config.panelSize || config.viewport || config.panel)
  const drm = parseSizeSpec(config.drmMode || config.drm || config.screen)
  const rotation = Number(config.rotation)
  return {
    panelSize: sizeSpec(panel) || HARD_FALLBACK_DISPLAY.panelSize,
    drmMode: sizeSpec(drm) || HARD_FALLBACK_DISPLAY.drmMode,
    rotation: Number.isFinite(rotation) ? rotation : HARD_FALLBACK_DISPLAY.rotation,
  }
}

export function dataRootPath() {
  if (typeof $dataDir === 'string' && $dataDir) return $dataDir
  if (typeof $falcon !== 'undefined' && $falcon && typeof $falcon.$dataDir === 'string' && $falcon.$dataDir) {
    return $falcon.$dataDir
  }
  try {
    if (typeof globalThis !== 'undefined' && typeof globalThis.$dataDir === 'string' && globalThis.$dataDir) {
      return globalThis.$dataDir
    }
  } catch (err) {
    console.warn(`read data dir failed ${err}`)
  }
  return '/tmp/wpe-browser-data'
}

export function workspacePath(page) {
  if (page && typeof page.$workspace === 'string' && page.$workspace) return page.$workspace
  try {
    if (typeof globalThis !== 'undefined' && typeof globalThis.$workspace === 'string' && globalThis.$workspace) {
      return globalThis.$workspace
    }
  } catch (err) {
    console.warn(`read workspace failed ${err}`)
  }
  if (typeof $falcon !== 'undefined' && $falcon && typeof $falcon.$workspace === 'string' && $falcon.$workspace) {
    return $falcon.$workspace
  }
  if (typeof $workspace === 'string' && $workspace) return $workspace
  const dataRoot = dataRootPath()
  if (dataRoot.slice(-5) === '/data') return dataRoot.slice(0, -5) + '/a'
  return ''
}

export function browserDataPath(name) {
  return dataRootPath() + '/browser/' + name
}

function fallbackDisplayConfig() {
  const props = appMeta().props || {}
  const selected = pickSkuConfig(props.browser_display_sku)
  return normalizeDisplayConfig(selected)
}

function readSystemDisplayConfig(browserPlayer) {
  if (!browserPlayer.getSystemDisplayConfig) return null
  try {
    return parseSystemDisplayConfig(browserPlayer.getSystemDisplayConfig() || '')
  } catch (err) {
    console.warn(`read system display config failed ${err}`)
  }
  return null
}

function readDrmMode(browserPlayer, options, fallback) {
  const optionMode = parseSizeSpec(options && options.drmMode)
  if (optionMode) return sizeSpec(optionMode)
  try {
    if (browserPlayer.getDrmScreenSize) {
      const mode = parseSizeSpec(browserPlayer.getDrmScreenSize() || '')
      if (mode) return sizeSpec(mode)
    }
  } catch (err) {
    console.warn(`read drm mode failed ${err}`)
  }
  return fallback.drmMode || DEFAULT_DRM_MODE
}

function rejectPanelCandidate(size, fallbackPanel, drmSize) {
  if (!size) return 'invalid'
  if (isDrmTransportSize(size, drmSize)) return 'drm_transport'
  const distance = aspectDistance(size, fallbackPanel)
  if (distance > 0.30 && isSwappedDrmSize(size, drmSize) && isLikelyFullResolutionDrm(drmSize)) {
    return ''
  }
  if (fallbackPanel && distance > 0.30) return `aspect_conflict_${Math.round(distance * 100)}`
  return ''
}

async function nextTick() {
  return new Promise((resolve) => setTimeout(resolve, 0))
}

async function readHoleSize(component) {
  await nextTick()
  let rectResult = null
  try {
    const dom = component.$page && component.$page.$dom ? component.$page.$dom : null
    if (!dom || !dom.getComponentRect) return null
    rectResult = dom.getComponentRect(component.$refs.browserHole)
    if (rectResult && rectResult.then) {
      rectResult = await Promise.race([
        rectResult,
        new Promise((resolve) => setTimeout(() => resolve(null), 300)),
      ])
    }
  } catch (err) {
    console.warn(`get hole rect failed ${err}`)
  }
  const size = rectResult && (rectResult.size || rectResult)
  return parseSizeSpec(size)
}

function readEnvSize() {
  const env = falconEnv()
  return parseSizeSpec({
    width: env.deviceWidth,
    height: env.deviceHeight,
  })
}

export async function resolveDisplayConfig(component, browserPlayer, options) {
  const fallback = fallbackDisplayConfig()
  const browserMode = normalizeBrowserMode(options && options.browserMode)
  const systemConfig = readSystemDisplayConfig(browserPlayer)
  const legacyRotation = Number(options && options.rotation)
  const hasLegacyRotation = !(options && options.browserMode) && Number.isFinite(legacyRotation)
  if (systemConfig && Number(systemConfig.width) > 0 && Number(systemConfig.height) > 0) {
    const rawPanel = parseSizeSpec({
      width: systemConfig.width,
      height: systemConfig.height,
    })
    const nativeRotation = normalizeRotationValue(systemConfig.frameworkRotation, fallback.rotation)
    const nativeTouchRotation = normalizeRotationValue(systemConfig.touchRotation, nativeRotation)
    const rotation = hasLegacyRotation
      ? normalizeRotationValue(legacyRotation, nativeRotation)
      : (browserMode === ROTATE_270_MODE ? rotateByDelta(nativeRotation, ROTATE_MODE_DELTA) : nativeRotation)
    const drmMode = sizeSpec(parseSizeSpec(systemConfig.drmMode)) || readDrmMode(browserPlayer, options || {}, fallback)
    const drmSize = parseSizeSpec(drmMode)
    const relativeRotation = rotationDelta(nativeRotation, rotation)
    const orientedPanel = normalizeForRotation(rawPanel, relativeRotation)
    const reconciled = reconcilePanelOrientation(orientedPanel, rotation, drmSize)
    const panel = reconciled.panel
    const panelSize = sizeSpec(panel) || fallback.panelSize || HARD_FALLBACK_DISPLAY.panelSize
    const touchRotation = hasLegacyRotation
      ? normalizeRotationValue(systemConfig.touchRotation, rotation)
      : (browserMode === ROTATE_270_MODE ? rotateByDelta(nativeTouchRotation, ROTATE_MODE_DELTA) : nativeTouchRotation)
    const displaySource = `system_cfg:${browserMode}`
    console.warn(`display_resolve source=${displaySource} raw=${systemConfig.width}x${systemConfig.height} direction=${systemConfig.frameworkRotation} relative_rotation=${relativeRotation} video_direction=${systemConfig.videoRotation} tp_direction=${systemConfig.touchRotation} panel=${panelSize} drm_mode=${drmMode} viewport=${panelSize} rotation=${rotation} touch_rotation=${touchRotation} orientation_corrected=${reconciled.corrected ? 1 : 0}`)
    return {
      panelSize,
      drmMode,
      viewport: panelSize,
      rotation,
      touchRotation,
      touchDevice: systemConfig.touchDevice || '',
      touchOffsetX: Number(systemConfig.touchOffsetX) || 0,
      touchOffsetY: Number(systemConfig.touchOffsetY) || 0,
      fpsMax: Number(systemConfig.fpsMax) || 0,
      browserMode,
      displaySource,
    }
  }

  const optionRotation = Number(options && options.rotation)
  const rotation = Number.isFinite(optionRotation)
    ? normalizeRotationValue(optionRotation, fallback.rotation)
    : (browserMode === ROTATE_270_MODE ? rotateByDelta(fallback.rotation, ROTATE_MODE_DELTA) : fallback.rotation)
  const relativeRotation = options && options.browserMode
    ? (browserMode === ROTATE_270_MODE ? ROTATE_MODE_DELTA : 0)
    : rotationDelta(fallback.rotation, rotation)
  const drmMode = readDrmMode(browserPlayer, options || {}, fallback)
  const drmSize = parseSizeSpec(drmMode)
  const fallbackPanel = parseSizeSpec((options && options.panelSize) || fallback.panelSize) || parseSizeSpec(HARD_FALLBACK_DISPLAY.panelSize)
  const candidates = [
    { source: 'dom', size: await readHoleSize(component) },
    { source: 'env', size: readEnvSize() },
  ]
  if (isLikelyFullResolutionDrm(drmSize)) {
    candidates.push({ source: 'drm_mode', size: drmSize })
  }

  for (let i = 0; i < candidates.length; i += 1) {
    const candidate = candidates[i]
    const normalized = normalizeForRotation(candidate.size, relativeRotation)
    const reject = rejectPanelCandidate(normalized, fallbackPanel, drmSize)
    if (!reject) {
      const reconciled = reconcilePanelOrientation(normalized, rotation, drmSize)
      const panelSize = sizeSpec(reconciled.panel)
      console.warn(`display_resolve source=${candidate.source}:${browserMode} panel=${panelSize} drm_mode=${drmMode} viewport=${panelSize} rotation=${rotation} relative_rotation=${relativeRotation} orientation_corrected=${reconciled.corrected ? 1 : 0}`)
      return {
        panelSize,
        drmMode,
        viewport: panelSize,
        rotation,
        touchRotation: rotation,
        browserMode,
        displaySource: `${candidate.source}:${browserMode}`,
      }
    }
    if (candidate.size) {
      console.warn(`display_resolve reject source=${candidate.source} size=${sizeSpec(candidate.size)} normalized=${sizeSpec(normalized)} reason=${reject}`)
    }
  }

  const orientedFallback = normalizeForRotation(fallbackPanel, relativeRotation)
  const reconciledFallback = reconcilePanelOrientation(orientedFallback, rotation, drmSize)
  const panelSize = sizeSpec(reconciledFallback.panel) || HARD_FALLBACK_DISPLAY.panelSize
  const source = fallbackPanel ? 'app_config' : 'hard_fallback'
  console.warn(`display_resolve source=${source}:${browserMode} panel=${panelSize} drm_mode=${drmMode} viewport=${panelSize} rotation=${rotation} relative_rotation=${relativeRotation} orientation_corrected=${reconciledFallback.corrected ? 1 : 0}`)
  return {
    panelSize,
    drmMode,
    viewport: panelSize,
    rotation,
    touchRotation: rotation,
    browserMode,
    displaySource: `${source}:${browserMode}`,
  }
}

export async function browserOptions(component, browserPlayer, options) {
  let runtimePath = options.runtimePath || ''
  if (!runtimePath) {
    runtimePath = browserPlayer.prepareRuntime({
      workspace: workspacePath(component),
    })
  }
  const display = await resolveDisplayConfig(component, browserPlayer, options)
  return {
    runtimePath,
    workdir: options.workdir || browserDataPath(''),
    logPath: options.logPath || browserDataPath('wpe-drm.log'),
    url: options.url || DEFAULT_URL,
    viewport: options.viewport || display.viewport || DEFAULT_VIEWPORT,
    panelSize: options.panelSize || display.panelSize || DEFAULT_VIEWPORT,
    drmMode: options.drmMode || display.drmMode || DEFAULT_DRM_MODE,
    displaySource: display.displaySource || 'unknown',
    rotation: Number.isFinite(Number(display.rotation)) ? Number(display.rotation) : DEFAULT_ROTATION,
    browserMode: display.browserMode || normalizeBrowserMode(options.browserMode),
    touchRotation: Number.isFinite(Number(display.touchRotation)) ? Number(display.touchRotation) : Number(display.rotation),
    touchDevice: options.touchDevice || display.touchDevice || '',
    touchOffsetX: Number.isFinite(Number(display.touchOffsetX)) ? Number(display.touchOffsetX) : 0,
    touchOffsetY: Number.isFinite(Number(display.touchOffsetY)) ? Number(display.touchOffsetY) : 0,
    fpsMax: Number.isFinite(Number(display.fpsMax)) ? Number(display.fpsMax) : 0,
    drm: options.drm || '/dev/dri/card0',
    gpuMode: options.gpuMode || 'auto',
    useOverlay: true,
    overlayZpos: 0,
  }
}
