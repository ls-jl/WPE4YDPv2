import globalModule from 'global'

let globalManager = null

export function getGlobalModule() {
  if (!globalManager) {
    globalManager = new globalModule.Global()
  }
  return globalManager
}

function getDefaultAmModule() {
  try {
    if (typeof globalThis !== 'undefined' && globalThis.$am) return globalThis.$am
  } catch (err) {
    void err
  }
  try {
    if (typeof $am !== 'undefined' && $am) return $am
  } catch (err) {
    void err
  }
  try {
    if (typeof $falcon === 'function') {
      const am = $falcon('getAmModule')
      if (am) return am
    }
  } catch (err) {
    void err
  }
  return null
}

function getDefaultFalconModule() {
  try {
    if (typeof globalThis !== 'undefined' && globalThis.$falcon) return globalThis.$falcon
  } catch (err) {
    void err
  }
  try {
    if (typeof $falcon !== 'undefined' && $falcon) return $falcon
  } catch (err) {
    void err
  }
  return null
}

function parseAmTextResult(data) {
  if (data == null) return { text: '', uuid: '', editConfirmed: true }
  if (typeof data === 'object') {
    return {
      text: data.text != null ? data.text : (data.contents != null ? data.contents : ''),
      uuid: data.uuid != null ? `${data.uuid}` : '',
      editConfirmed: data.editConfirmed !== false,
    }
  }
  const raw = `${data}`
  try {
    const parsed = JSON.parse(raw || '{}')
    if (parsed && typeof parsed === 'object') {
      return {
        text: parsed.text != null ? parsed.text : (parsed.contents != null ? parsed.contents : raw),
        uuid: parsed.uuid != null ? `${parsed.uuid}` : '',
        editConfirmed: parsed.editConfirmed !== false,
      }
    }
  } catch (err) {
    void err
  }
  return { text: raw, uuid: '', editConfirmed: true }
}

export class KeyboardSession {
  constructor({ onConfirm, onCancel, getAm, getFalcon } = {}) {
    this.inputTaskUuid = ''
    this.editFinished = null
    this.amEditFinished = null
    this.amEditClosed = null
    this.amPanelVisibleChanged = null
    this.falconEditFinished = null
    this.falconEditClosed = null
    this.falconPanelVisibleChanged = null
    this.onConfirm = onConfirm
    this.onCancel = onCancel
    this.getAm = getAm || getDefaultAmModule
    this.getFalcon = getFalcon || getDefaultFalconModule
    this.stripNewlines = true
    this.closeDelayMs = 350
    this.backend = ''
  }

  mount() {
    if (this.editFinished || this.amEditFinished) return
    const am = this.resolveAmModule()
    if (am && am.on) {
      this.amEditFinished = (data) => {
        if (!this.inputTaskUuid) return
        const result = parseAmTextResult(data)
        if (result.uuid && result.uuid !== this.inputTaskUuid) {
          console.warn(`keyboard am result ignored uuid=${result.uuid} active=${this.inputTaskUuid}`)
          return
        }
        setTimeout(() => {
          if (result.editConfirmed) {
            this.finish(result.text || '')
          } else {
            this.cancel()
          }
        }, 0)
      }
      this.amEditClosed = () => {
        if (!this.inputTaskUuid || this.backend !== 'am') return
        setTimeout(() => this.cancel(), 0)
      }
      this.amPanelVisibleChanged = (visible) => {
        console.warn(`keyboard am panel visible ${visible}`)
      }
      try {
        am.on('sendTextEditFinishedSignal', this.amEditFinished)
        am.on('im===editClosed===called', this.amEditClosed)
        am.on('sendApolloTextEditClosedSignal', this.amEditClosed)
        am.on('sendImPanelVisbileChangedSignal', this.amPanelVisibleChanged)
        console.warn('keyboard am listeners mounted')
      } catch (err) {
        console.warn(`keyboard am listener mount failed ${err}`)
        this.amEditFinished = null
        this.amEditClosed = null
        this.amPanelVisibleChanged = null
      }
    }
    const falcon = this.resolveFalconModule()
    if (falcon && falcon.on) {
      this.falconEditFinished = (data) => {
        if (!this.inputTaskUuid) return
        const result = parseAmTextResult(data)
        if (result.uuid && result.uuid !== this.inputTaskUuid) {
          console.warn(`keyboard falcon result ignored uuid=${result.uuid} active=${this.inputTaskUuid}`)
          return
        }
        setTimeout(() => {
          if (result.editConfirmed) {
            this.finish(result.text || '')
          } else {
            this.cancel()
          }
        }, 0)
      }
      this.falconEditClosed = () => {
        if (!this.inputTaskUuid || this.backend !== 'falcon') return
        setTimeout(() => this.cancel(), 0)
      }
      this.falconPanelVisibleChanged = (visible) => {
        console.warn(`keyboard falcon panel visible ${visible}`)
      }
      try {
        falcon.on('textEditFinished', this.falconEditFinished)
        falcon.on('apolloTextEditClosed', this.falconEditClosed)
        falcon.on('imPanelVisbileChanged', this.falconPanelVisibleChanged)
        console.warn('keyboard falcon listeners mounted')
      } catch (err) {
        console.warn(`keyboard falcon listener mount failed ${err}`)
        this.falconEditFinished = null
        this.falconEditClosed = null
        this.falconPanelVisibleChanged = null
      }
    }
    const gm = getGlobalModule()
    this.editFinished = (uuid, jsonData) => {
      if (uuid !== this.inputTaskUuid) return
      let result = null
      try {
        result = JSON.parse(jsonData || '{}')
      } catch (err) {
        console.log(`keyboard parse error ${err}`)
        return
      }
      setTimeout(() => {
        if (result && result.editConfirmed) {
          this.finish(result.text || '')
        } else {
          this.cancel()
        }
      }, 0)
    }
    if (gm.textEditFinished) {
      gm.textEditFinished.on(this.editFinished)
      console.warn('keyboard global listener mounted')
    }
  }

  unmount() {
    const am = this.resolveAmModule()
    if (am && am.off) {
      try {
        if (this.amEditFinished) am.off('sendTextEditFinishedSignal', this.amEditFinished)
        if (this.amEditClosed) {
          am.off('im===editClosed===called', this.amEditClosed)
          am.off('sendApolloTextEditClosedSignal', this.amEditClosed)
        }
        if (this.amPanelVisibleChanged) {
          am.off('sendImPanelVisbileChangedSignal', this.amPanelVisibleChanged)
        }
      } catch (err) {
        console.warn(`keyboard am listener unmount failed ${err}`)
      }
    }
    this.amEditFinished = null
    this.amEditClosed = null
    this.amPanelVisibleChanged = null
    const falcon = this.resolveFalconModule()
    if (falcon && falcon.off) {
      try {
        if (this.falconEditFinished) falcon.off('textEditFinished', this.falconEditFinished)
        if (this.falconEditClosed) falcon.off('apolloTextEditClosed', this.falconEditClosed)
        if (this.falconPanelVisibleChanged) {
          falcon.off('imPanelVisbileChanged', this.falconPanelVisibleChanged)
        }
      } catch (err) {
        console.warn(`keyboard falcon listener unmount failed ${err}`)
      }
    }
    this.falconEditFinished = null
    this.falconEditClosed = null
    this.falconPanelVisibleChanged = null
    const gm = getGlobalModule()
    if (gm.textEditFinished && this.editFinished) {
      gm.textEditFinished.off(this.editFinished)
    }
    this.close()
    this.editFinished = null
  }

  resolveAmModule() {
    try {
      const am = this.getAm ? this.getAm() : null
      if (am && am.trigger && am.on) return am
    } catch (err) {
      console.warn(`keyboard get am failed ${err}`)
    }
    return getDefaultAmModule()
  }

  resolveFalconModule() {
    try {
      const falcon = this.getFalcon ? this.getFalcon() : null
      if (falcon && falcon.trigger && falcon.on) return falcon
    } catch (err) {
      console.warn(`keyboard get falcon failed ${err}`)
    }
    const falcon = getDefaultFalconModule()
    if (falcon && falcon.trigger && falcon.on) return falcon
    return null
  }

  open(options = {}) {
    if (this.inputTaskUuid) return this.inputTaskUuid
    this.stripNewlines = options.stripNewlines !== false
    this.closeDelayMs = options.closeDelayMs == null ? 350 : options.closeDelayMs
    let maxlength = options.maxlength == null ? 100 : Number(options.maxlength)
    if (!Number.isFinite(maxlength)) maxlength = 100
    if (maxlength !== -1) maxlength = Math.max(1, Math.min(maxlength || 100, 4096))
    const config = {
      uuid: '',
      text: options.text || '',
      defaultText: options.text || '',
      contents: options.text || '',
      placeholder: options.placeholder || '',
      placeholderColor: options.placeholderColor || '#878A99',
      autofocus: true,
      maxlength,
      showCursor: true,
      cursorIndex: `${options.text || ''}`.length,
      cursorColor: options.cursorColor || '#FF683D',
      cursorSize: options.cursorSize || 3,
      confirmButtonDisabledOnTextEmpty: options.confirmButtonDisabledOnTextEmpty !== false,
      inputType: options.inputType || 'ZhCNPreferred',
      keyboardType: options.multiLinesEditVisible ? 'multi_lines_edit' : 'normal',
      multiLinesEditVisible: !!options.multiLinesEditVisible,
      capsLockSwitchOn: false,
      micInputVisible: false,
      isNetworkConnected: true,
      enterButtonText: options.enterButtonText || '确认',
    }
    const am = this.resolveAmModule()
    if (am && am.trigger) {
      const uuid = `wpe_keyboard_${Date.now()}_${Math.floor(Math.random() * 100000)}`
      const amConfig = {
        uuid,
        contents: config.contents,
        defaultText: config.defaultText,
        text: config.text,
        placeholder: config.placeholder,
        placeholderColor: config.placeholderColor,
        autofocus: config.autofocus,
        maxlength: config.maxlength,
        showCursor: config.showCursor,
        cursorIndex: config.cursorIndex,
        cursorColor: config.cursorColor,
        cursorSize: config.cursorSize,
        confirmButtonDisabledOnTextEmpty: config.confirmButtonDisabledOnTextEmpty,
        inputType: config.inputType,
        keyboardType: config.keyboardType,
        multiLinesEditVisible: config.multiLinesEditVisible,
        capsLockSwitchOn: config.capsLockSwitchOn,
        enterButtonText: config.enterButtonText,
        micInputVisible: config.micInputVisible,
        isNetworkConnected: config.isNetworkConnected,
      }
      try {
        this.inputTaskUuid = uuid
        this.backend = 'am'
        am.trigger('requestIMAppShow', amConfig)
        console.warn(`keyboard am requestIMAppShow uuid ${uuid} type=${amConfig.inputType}`)
        return this.inputTaskUuid
      } catch (err) {
        console.warn(`keyboard am requestIMAppShow failed ${err}`)
        this.inputTaskUuid = ''
        this.backend = ''
      }
    } else {
      console.warn('keyboard am unavailable, fallback falcon')
    }
    const falcon = this.resolveFalconModule()
    if (falcon && falcon.trigger) {
      const uuid = `wpe_keyboard_${Date.now()}_${Math.floor(Math.random() * 100000)}`
      const falconConfig = {
        uuid,
        contents: config.contents,
        defaultText: config.defaultText,
        text: config.text,
        placeholder: config.placeholder,
        placeholderColor: config.placeholderColor,
        autofocus: config.autofocus,
        maxlength: config.maxlength,
        showCursor: config.showCursor,
        cursorIndex: config.cursorIndex,
        cursorColor: config.cursorColor,
        cursorSize: config.cursorSize,
        confirmButtonDisabledOnTextEmpty: config.confirmButtonDisabledOnTextEmpty,
        inputType: config.inputType,
        keyboardType: config.keyboardType,
        multiLinesEditVisible: config.multiLinesEditVisible,
        capsLockSwitchOn: config.capsLockSwitchOn,
        enterButtonText: config.enterButtonText,
        micInputVisible: config.micInputVisible,
        isNetworkConnected: config.isNetworkConnected,
      }
      try {
        this.inputTaskUuid = uuid
        this.backend = 'falcon'
        falcon.trigger('requestIMAppShow', falconConfig)
        console.warn(`keyboard falcon requestIMAppShow uuid ${uuid} type=${falconConfig.inputType}`)
        return this.inputTaskUuid
      } catch (err) {
        console.warn(`keyboard falcon requestIMAppShow failed ${err}`)
        this.inputTaskUuid = ''
        this.backend = ''
      }
    }
    console.warn('keyboard fallback global startTextEdit')
    const gm = getGlobalModule()
    if (!gm.startTextEdit) {
      console.warn('keyboard startTextEdit unavailable')
      return ''
    }
    try {
      this.backend = 'global'
      config.uuid = `wpe_keyboard_${Date.now()}_${Math.floor(Math.random() * 100000)}`
      this.inputTaskUuid = gm.startTextEdit(config) || ''
      console.warn(`keyboard startTextEdit object uuid ${this.inputTaskUuid}`)
      if (!this.inputTaskUuid) {
        throw new Error('empty uuid')
      }
    } catch (err) {
      console.warn(`keyboard startTextEdit object failed ${err}`)
      try {
        this.backend = 'global'
        this.inputTaskUuid = gm.startTextEdit(JSON.stringify(config)) || ''
        console.warn(`keyboard startTextEdit string uuid ${this.inputTaskUuid}`)
      } catch (err2) {
        console.warn(`keyboard startTextEdit string failed ${err2}`)
        this.inputTaskUuid = ''
        this.backend = ''
      }
    }
    return this.inputTaskUuid
  }

  finish(text) {
    const uuid = this.inputTaskUuid
    const value = `${text || ''}`
    const cleaned = this.stripNewlines ? value.replace(/\n/g, '') : value
    try {
      if (this.onConfirm) {
        this.onConfirm(cleaned)
      }
    } catch (err) {
      console.warn(`keyboard confirm handler failed ${err}`)
    }
    if (this.backend === 'global') {
      this.deferClose(uuid)
    }
    if (this.inputTaskUuid === uuid) {
      this.inputTaskUuid = ''
    }
    this.backend = ''
  }

  cancel() {
    const uuid = this.inputTaskUuid
    try {
      if (this.onCancel) {
        this.onCancel()
      }
    } catch (err) {
      console.warn(`keyboard cancel handler failed ${err}`)
    }
    if (this.inputTaskUuid === uuid) {
      this.inputTaskUuid = ''
    }
    this.backend = ''
  }

  deferClose(uuid) {
    if (!uuid) return
    setTimeout(() => {
      const gm = getGlobalModule()
      if (gm.closeTextEdit) {
        try {
          gm.closeTextEdit(uuid)
        } catch (err) {
          console.warn(`keyboard closeTextEdit failed ${err}`)
        }
      }
    }, this.closeDelayMs)
  }

  close() {
    if (this.backend === 'am' || this.backend === 'falcon') {
      const isFalcon = this.backend === 'falcon'
      const falcon = isFalcon ? this.resolveFalconModule() : null
      const am = this.resolveAmModule()
      const target = isFalcon ? falcon : am
      if (target && target.trigger) {
        try {
          target.trigger('requestIMAppClose', {})
          console.warn(`keyboard ${this.backend} close uuid ${this.inputTaskUuid}`)
        } catch (err) {
          console.warn(`keyboard ${this.backend} close failed ${err}`)
        }
      }
      this.inputTaskUuid = ''
      this.backend = ''
      return
    }
    const gm = getGlobalModule()
    if (gm.closeTextEdit && this.inputTaskUuid) {
      try {
        gm.closeTextEdit(this.inputTaskUuid)
      } catch (err) {
        console.warn(`keyboard close failed ${err}`)
      }
    }
    this.inputTaskUuid = ''
    this.backend = ''
  }

  isActive() {
    return !!this.inputTaskUuid
  }
}
