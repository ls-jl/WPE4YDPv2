<template>
  <div class="frame-page">
    <hole ref="browserHole" class="browser-hole"></hole>
    <textarea
      v-if="keyboardTextareaVisible"
      ref="keyboardTextarea"
      class="keyboard-bridge-textarea"
      :value="keyboardTextareaValue"
      :focus="keyboardTextareaFocused"
      :maxlength="keyboardTextareaMaxlength"
      :inputType="keyboardTextareaInputType"
      :showCursor="true"
      :softInputEnable="true"
      placeholder="请输入内容"
      placeholderColor="#878A99"
      cursorColor="#008CFF"
      :cursorSize="3"
      @input="onKeyboardTextareaInput"
      @confirm="onKeyboardTextareaConfirm"
      @textChanged="onKeyboardTextareaInput"
      @textEditFinished="onKeyboardTextareaConfirm"
      @focus="onKeyboardTextareaFocus"
      @blur="onKeyboardTextareaBlur"
    ></textarea>
  </div>
</template>

<script>
import { browserPlayer } from 'browser'
import { KeyboardSession } from '../../utils/keyboard'

const DEFAULT_URL = 'https://m.baidu.com/'
const DEFAULT_VIEWPORT = '960x266'
const DEFAULT_ROTATION = 270
const DEFAULT_DRM_MODE = '480x960'
const DEFAULT_BROWSER_MODE = 'native'
const ROTATE_270_MODE = 'rotate270'
const ROTATE_MODE_DELTA = 270
const HARD_FALLBACK_DISPLAY = {
  panelSize: DEFAULT_VIEWPORT,
  drmMode: DEFAULT_DRM_MODE,
  rotation: DEFAULT_ROTATION,
}

function normalizeBrowserMode(value) {
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

function normalizeForRotation(size, rotation) {
  if (!size) return null
  const rotated = Number(rotation) === 90 || Number(rotation) === 270
  if (rotated && size.width < size.height) {
    return { width: size.height, height: size.width }
  }
  return { width: size.width, height: size.height }
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
  if (!config || typeof config !== 'object') return null
  return config
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
  if (!size || !drmSize) return false
  return sameSize(size, drmSize)
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

function normalizeKeyboardText(value) {
  if (value && typeof value === 'object') {
    if (typeof value.value === 'string') return value.value
    if (typeof value.text === 'string') return value.text
    if (typeof value.contents === 'string') return value.contents
    if (value.records && value.records[0] && typeof value.records[0].text === 'string') {
      return value.records[0].text
    }
  }
  if (typeof value === 'string') {
    try {
      const parsed = JSON.parse(value)
      return normalizeKeyboardText(parsed) || value
    } catch (err) {
      return value
    }
  }
  return value == null ? '' : `${value}`
}

function dataRootPath() {
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

function workspacePath(page) {
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

function browserDataPath(name) {
  return dataRootPath() + '/browser/' + name
}

export default {
  name: 'frame',
  data() {
    return {
      browserRunning: false,
      browserStarting: false,
      watchdogActive: false,
      leavingFrame: false,
      startToken: 0,
      keyboardSession: null,
      pollTimer: null,
      keyboardGlobalOpenTimer: null,
      pollTick: 0,
      keyboardActiveUntil: 0,
      activeKeyboardRequest: null,
      keyboardTextareaVisible: false,
      keyboardTextareaFocused: false,
      keyboardTextareaValue: '',
      keyboardTextareaInputType: 'ZhCNPreferred',
      keyboardTextareaMaxlength: 512,
      keyboardTextareaActive: false,
      keyboardTextareaSeenInput: false,
    }
  },
  mounted() {
    this.setupKeyboard()
    this.startBrowser()
    this.startPolling()
  },
  beforeDestroy() {
    this.teardownKeyboard()
  },
  methods: {
    pageOptions() {
      return this.$page && this.$page.options ? this.$page.options : {}
    },
    browserWorkdir() {
      const options = this.pageOptions()
      return options.workdir || browserDataPath('')
    },
    nextTick() {
      return new Promise((resolve) => setTimeout(resolve, 0))
    },
    fallbackDisplayConfig() {
      const props = appMeta().props || {}
      const selected = pickSkuConfig(props.browser_display_sku)
      return normalizeDisplayConfig(selected)
    },
    readSystemDisplayConfig() {
      if (!browserPlayer.getSystemDisplayConfig) return null
      try {
        return parseSystemDisplayConfig(browserPlayer.getSystemDisplayConfig() || '')
      } catch (err) {
        console.warn(`read system display config failed ${err}`)
      }
      return null
    },
    readDrmMode(options, fallback) {
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
    },
    rejectPanelCandidate(size, source, fallbackPanel, drmSize) {
      if (!size) return 'invalid'
      if (isDrmTransportSize(size, drmSize)) return 'drm_transport'
      const distance = aspectDistance(size, fallbackPanel)
      if (distance > 0.30 && isSwappedDrmSize(size, drmSize) && isLikelyFullResolutionDrm(drmSize)) {
        return ''
      }
      if (fallbackPanel && distance > 0.30) return `aspect_conflict_${Math.round(distance * 100)}`
      return ''
    },
    async readHoleSize() {
      await this.nextTick()
      let rectResult = null
      try {
        const dom = this.$page && this.$page.$dom ? this.$page.$dom : null
        if (!dom || !dom.getComponentRect) return null
        rectResult = dom.getComponentRect(this.$refs.browserHole)
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
    },
    readEnvSize() {
      const env = falconEnv()
      return parseSizeSpec({
        width: env.deviceWidth,
        height: env.deviceHeight,
      })
    },
    async resolveDisplayConfig(options) {
      const fallback = this.fallbackDisplayConfig()
      const browserMode = normalizeBrowserMode(options && options.browserMode)
      const systemConfig = this.readSystemDisplayConfig()
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
        const panel = normalizeForRotation(rawPanel, rotation)
        const panelSize = sizeSpec(panel) || fallback.panelSize || HARD_FALLBACK_DISPLAY.panelSize
        const drmMode = sizeSpec(parseSizeSpec(systemConfig.drmMode)) || this.readDrmMode(options || {}, fallback)
        const touchRotation = hasLegacyRotation
          ? normalizeRotationValue(systemConfig.touchRotation, rotation)
          : (browserMode === ROTATE_270_MODE ? rotateByDelta(nativeTouchRotation, ROTATE_MODE_DELTA) : nativeTouchRotation)
        const displaySource = `system_cfg:${browserMode}`
        console.warn(`display_resolve source=${displaySource} raw=${systemConfig.width}x${systemConfig.height} direction=${systemConfig.frameworkRotation} video_direction=${systemConfig.videoRotation} tp_direction=${systemConfig.touchRotation} panel=${panelSize} drm_mode=${drmMode} viewport=${panelSize} rotation=${rotation} touch_rotation=${touchRotation}`)
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
      const drmMode = this.readDrmMode(options || {}, fallback)
      const drmSize = parseSizeSpec(drmMode)
      const fallbackPanel = parseSizeSpec((options && options.panelSize) || fallback.panelSize) || parseSizeSpec(HARD_FALLBACK_DISPLAY.panelSize)
      const candidates = [
        { source: 'dom', size: await this.readHoleSize() },
        { source: 'env', size: this.readEnvSize() },
      ]
      if (isLikelyFullResolutionDrm(drmSize)) {
        candidates.push({ source: 'drm_mode', size: drmSize })
      }

      for (let i = 0; i < candidates.length; i += 1) {
        const candidate = candidates[i]
        const normalized = normalizeForRotation(candidate.size, rotation)
        const reject = this.rejectPanelCandidate(normalized, candidate.source, fallbackPanel, drmSize)
        if (!reject) {
          const panelSize = sizeSpec(normalized)
          console.warn(`display_resolve source=${candidate.source}:${browserMode} panel=${panelSize} drm_mode=${drmMode} viewport=${panelSize} rotation=${rotation}`)
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

      const panelSize = sizeSpec(fallbackPanel) || HARD_FALLBACK_DISPLAY.panelSize
      const source = fallbackPanel ? 'app_config' : 'hard_fallback'
      console.warn(`display_resolve source=${source}:${browserMode} panel=${panelSize} drm_mode=${drmMode} viewport=${panelSize} rotation=${rotation}`)
      return {
        panelSize,
        drmMode,
        viewport: panelSize,
        rotation,
        touchRotation: rotation,
        browserMode,
        displaySource: `${source}:${browserMode}`,
      }
    },
    async browserOptions() {
      const options = this.pageOptions()
      let runtimePath = options.runtimePath || ''
      if (!runtimePath) {
        runtimePath = browserPlayer.prepareRuntime({
          workspace: workspacePath(this),
          dataDir: dataRootPath(),
        })
      }
      const display = await this.resolveDisplayConfig(options)
      return {
        runtimePath,
        workdir: this.browserWorkdir(),
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
        useOverlay: true,
        overlayZpos: 0,
      }
    },
    setupKeyboard() {
      if (this.keyboardSession) return
      this.keyboardSession = new KeyboardSession({
        onConfirm: (text) => {
          const value = this.keyboardTextareaSeenInput ? this.keyboardTextareaValue : text
          this.finishKeyboardRequest(true, value)
        },
        onCancel: () => this.finishKeyboardRequest(false, ''),
      })
      this.keyboardSession.mount()
    },
    teardownKeyboard() {
      this.stopPolling()
      this.cancelActiveKeyboardRequest('teardown')
      this.closeKeyboardTextarea()
      if (this.keyboardSession) {
        this.keyboardSession.unmount()
        this.keyboardSession = null
      }
    },
    // 单一 200ms tick 承担键盘桥轮询与 watchdog，替代原先两个独立定时器：
    // 键盘桥近期有活动时保持 200ms 灵敏度，空闲降到 600ms（C++ 侧每次轮询都是一次目录扫描）；
    // watchdog 维持原有 1s 节奏。
    startPolling() {
      if (this.pollTimer) return
      this.pollTimer = setInterval(() => this.onPollTick(), 200)
      this.pollKeyboardRequest()
    },
    stopPolling() {
      if (this.pollTimer) {
        clearInterval(this.pollTimer)
        this.pollTimer = null
      }
    },
    onPollTick() {
      this.pollTick += 1
      const keyboardActive = Date.now() < this.keyboardActiveUntil
      if (keyboardActive || this.pollTick % 3 === 0) {
        this.pollKeyboardRequest()
      }
      if (this.watchdogActive && this.pollTick % 5 === 0) {
        this.watchBrowser()
      }
    },
    keyboardOptionsForRequest(request) {
      const kind = request && request.kind ? `${request.kind}` : ''
      const text = request && request.text != null ? `${request.text}` : ''
      const placeholder = request && request.placeholder ? `${request.placeholder}` : (kind === 'web_input' ? '请输入内容' : '输入网址或搜索')
      const inputType = request && request.inputType ? `${request.inputType}` : (kind === 'web_input' ? 'ZhCNPreferred' : 'EnUSPreferred')
      let maxlength = request && request.maxlength != null ? Number(request.maxlength) : 512
      if (!Number.isFinite(maxlength)) maxlength = 512
      return {
        text,
        placeholder,
        maxlength,
        inputType,
        multiLinesEditVisible: !!(request && request.multiLinesEditVisible),
        stripNewlines: !(request && request.multiLinesEditVisible),
        confirmButtonDisabledOnTextEmpty: false,
        enterButtonText: '确认',
      }
    },
    openKeyboardTextarea(request) {
      if (!request || !request.id) return false
      const options = this.keyboardOptionsForRequest(request)
      this.keyboardTextareaValue = options.text || ''
      this.keyboardTextareaInputType = options.inputType || 'ZhCNPreferred'
      this.keyboardTextareaMaxlength = options.maxlength || 512
      this.keyboardTextareaVisible = true
      this.keyboardTextareaFocused = false
      this.keyboardTextareaActive = true
      this.keyboardTextareaSeenInput = false
      console.warn(`keyboard textarea request id=${request.id} kind=${request.kind || ''} type=${this.keyboardTextareaInputType}`)
      setTimeout(() => {
        if (!this.activeKeyboardRequest || this.activeKeyboardRequest.id !== request.id) return
        this.keyboardTextareaFocused = true
        try {
          const field = this.$refs.keyboardTextarea
          if (field && field.focus) field.focus()
        } catch (err) {
          console.warn(`keyboard textarea focus failed ${err}`)
        }
      }, 0)
      return true
    },
    closeKeyboardTextarea() {
      this.keyboardTextareaFocused = false
      this.keyboardTextareaActive = false
      this.keyboardTextareaVisible = false
      this.keyboardTextareaSeenInput = false
    },
    clearKeyboardGlobalOpenTimer() {
      if (this.keyboardGlobalOpenTimer) {
        clearTimeout(this.keyboardGlobalOpenTimer)
        this.keyboardGlobalOpenTimer = null
      }
    },
    scheduleGlobalKeyboardOpen(request, options, textareaStarted) {
      this.clearKeyboardGlobalOpenTimer()
      this.keyboardGlobalOpenTimer = setTimeout(() => {
        this.keyboardGlobalOpenTimer = null
        if (!this.activeKeyboardRequest || this.activeKeyboardRequest.id !== request.id) return
        const uuid = this.keyboardSession.open(options)
        if (!uuid && !textareaStarted) {
          this.finishKeyboardRequest(false, '')
        }
      }, 80)
    },
    onKeyboardTextareaInput(value) {
      this.keyboardTextareaValue = normalizeKeyboardText(value)
      this.keyboardTextareaSeenInput = true
      console.warn(`keyboard textarea input ${this.keyboardTextareaValue || ''}`)
      this.updateKeyboardRequest(this.keyboardTextareaValue)
    },
    onKeyboardTextareaConfirm(value) {
      if (!this.activeKeyboardRequest) return
      const text = this.keyboardTextareaSeenInput ? this.keyboardTextareaValue : normalizeKeyboardText(value)
      this.keyboardTextareaValue = text
      console.warn(`keyboard textarea confirm ${text || ''}`)
      this.finishKeyboardRequest(true, text)
      if (this.keyboardSession && this.keyboardSession.isActive()) {
        this.keyboardSession.close()
      }
      this.closeKeyboardTextarea()
    },
    onKeyboardTextareaFocus() {
      this.keyboardTextareaActive = true
      console.warn('keyboard textarea focused')
    },
    onKeyboardTextareaBlur() {
      this.keyboardTextareaFocused = false
      console.warn('keyboard textarea blurred')
      setTimeout(() => {
        if (!this.activeKeyboardRequest) return
        if (this.keyboardSession && this.keyboardSession.isActive()) return
        if (this.keyboardTextareaFocused) return
        console.warn(`keyboard textarea cancel id=${this.activeKeyboardRequest.id}`)
        this.finishKeyboardRequest(false, '')
        this.closeKeyboardTextarea()
      }, 250)
    },
    updateKeyboardRequest(text) {
      const request = this.activeKeyboardRequest
      if (!request || !request.id || !browserPlayer.updateKeyboardRequest) return
      try {
        browserPlayer.updateKeyboardRequest({
          workdir: this.browserWorkdir(),
          id: request.id,
          text: `${text || ''}`,
        })
        console.warn(`keyboard update id=${request.id} kind=${request.kind || ''} bytes=${`${text || ''}`.length}`)
      } catch (err) {
        console.warn(`update keyboard request failed ${err}`)
      }
    },
    pollKeyboardRequest() {
      if (this.leavingFrame || this.activeKeyboardRequest) return
      if (!browserPlayer.pollKeyboardRequest || !this.keyboardSession) return
      if (!this.browserRunning && !this.browserStarting) return

      let raw = ''
      try {
        raw = browserPlayer.pollKeyboardRequest({ workdir: this.browserWorkdir() }) || ''
      } catch (err) {
        console.warn(`poll keyboard request failed ${err}`)
        return
      }
      if (!raw) return
      this.keyboardActiveUntil = Date.now() + 10000

      let request = null
      try {
        request = JSON.parse(raw)
      } catch (err) {
        console.warn(`parse keyboard request failed ${err}`)
        return
      }
      if (!request || !request.id) return

      this.activeKeyboardRequest = request
      console.warn(`keyboard request id=${request.id} kind=${request.kind || ''}`)
      const options = this.keyboardOptionsForRequest(request)
      const textareaStarted = this.openKeyboardTextarea(request)
      this.scheduleGlobalKeyboardOpen(request, options, textareaStarted)
    },
    finishKeyboardRequest(confirmed, text) {
      const request = this.activeKeyboardRequest
      if (!request || !request.id) return
      this.clearKeyboardGlobalOpenTimer()
      this.activeKeyboardRequest = null
      this.keyboardActiveUntil = Date.now() + 10000
      try {
        browserPlayer.respondKeyboardRequest({
          workdir: this.browserWorkdir(),
          id: request.id,
          confirmed: !!confirmed,
          text: confirmed ? `${text || ''}` : '',
        })
        console.warn(`keyboard ${confirmed ? 'confirmed' : 'cancelled'} id=${request.id} kind=${request.kind || ''}`)
      } catch (err) {
        console.warn(`respond keyboard request failed ${err}`)
      }
      this.closeKeyboardTextarea()
    },
    cancelActiveKeyboardRequest(reason) {
      this.clearKeyboardGlobalOpenTimer()
      if (this.activeKeyboardRequest) {
        console.warn(`keyboard cancel active reason=${reason || ''} id=${this.activeKeyboardRequest.id}`)
        this.finishKeyboardRequest(false, '')
      }
      if (this.keyboardSession && this.keyboardSession.isActive()) {
        this.keyboardSession.close()
      }
      this.closeKeyboardTextarea()
    },
    startBrowser() {
      if (this.browserStarting || this.leavingFrame) return
      try {
        if (browserPlayer.isBrowserRunning && browserPlayer.isBrowserRunning({ workdir: this.browserWorkdir() })) {
          this.browserRunning = true
          this.startWatchdog()
          return
        }
      } catch (err) {
        console.warn(`check browser running failed ${err}`)
      }

      this.browserStarting = true
      const token = this.startToken + 1
      this.startToken = token
      setTimeout(() => {
        if (token !== this.startToken || this.leavingFrame) return
        this.browserOptions().then((launchOptions) => {
          if (token !== this.startToken || this.leavingFrame) return
          const pid = browserPlayer.startBrowser(launchOptions)
          this.browserRunning = Number(pid) > 0
          console.warn(`wpe browser pid ${pid}`)
          if (this.browserRunning) {
            this.startWatchdog()
          } else {
            this.leaveFrame('start failed')
          }
        }).catch((err) => {
          console.warn(`start browser failed ${err}`)
          this.browserRunning = false
          this.leaveFrame(err && err.message ? err.message : 'start exception')
        }).then(() => {
          this.browserStarting = false
        })
      }, 0)
    },
    startWatchdog() {
      this.watchdogActive = true
    },
    stopWatchdog() {
      this.watchdogActive = false
    },
    watchBrowser() {
      if (this.browserStarting || this.leavingFrame) return
      let running = false
      try {
        running = browserPlayer.isBrowserRunning({ workdir: this.browserWorkdir() })
      } catch (err) {
        console.warn(`watch browser failed ${err}`)
      }
      if (running) {
        this.browserRunning = true
        return
      }
      this.browserRunning = false
      this.leaveFrame('browser exited')
    },
    stopBrowser() {
      this.stopWatchdog()
      this.startToken += 1
      this.browserStarting = false
      this.browserRunning = false
      this.cancelActiveKeyboardRequest('stop browser')
      try {
        browserPlayer.stopBrowser({ workdir: this.browserWorkdir() })
      } catch (err) {
        console.warn(`stop browser failed ${err}`)
      }
    },
    leaveFrame(reason) {
      if (this.leavingFrame) return
      this.leavingFrame = true
      console.warn(`wpe leave frame: ${reason}`)
      const options = this.pageOptions()
      this.stopBrowser()
      setTimeout(() => {
        try {
          $falcon.navTo(options.returnPage || 'index', {
            browserStatus: reason || 'stopped',
            browserMode: options.browserMode || DEFAULT_BROWSER_MODE,
          })
        } catch (err) {
          console.warn(`nav index failed ${err}`)
        }
      }, 100)
    },
    onShow() {
      if (!this.leavingFrame) {
        this.startBrowser()
      }
    },
    onHide() {
      console.warn('wpe frame onHide')
      if ((this.keyboardSession && this.keyboardSession.isActive()) || this.keyboardTextareaActive) {
        console.warn('wpe frame onHide ignored while keyboard active')
        return
      }
      this.stopBrowser()
    },
    onUnload() {
      console.warn('wpe frame onUnload')
      this.stopBrowser()
      this.teardownKeyboard()
    },
  },
}
</script>

<style lang="less" scoped>
.frame-page {
  width: 100vw;
  height: 100vh;
  background-color: #000000;
}

.browser-hole {
  width: 100vw;
  height: 100vh;
}

.keyboard-bridge-textarea {
  position: absolute;
  left: 0;
  top: 0;
  width: 2px;
  height: 2px;
  opacity: 0.01;
  color: transparent;
  background-color: transparent;
  font-size: 1px;
}
</style>
