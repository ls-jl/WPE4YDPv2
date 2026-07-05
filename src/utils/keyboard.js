import globalModule from 'global'

let globalManager = null

export function getGlobalModule() {
  if (!globalManager) {
    globalManager = new globalModule.Global()
  }
  return globalManager
}

function normalizeText(value) {
  if (value && typeof value === 'object') {
    if (typeof value.value === 'string') return value.value
    if (typeof value.text === 'string') return value.text
    if (typeof value.contents === 'string') return value.contents
    if (value.records && value.records[0] && typeof value.records[0].text === 'string') {
      return value.records[0].text
    }
  }
  return typeof value === 'string' ? value : ''
}

function parseTextEditResult(jsonData) {
  try {
    const result = JSON.parse(jsonData || '{}')
    return {
      confirmed: !!(result && result.editConfirmed),
      text: normalizeText(result && result.text),
    }
  } catch (err) {
    console.warn(`keyboard parse result failed ${err}`)
  }
  return { confirmed: false, text: '' }
}

function clampMaxlength(value) {
  let maxlength = value == null ? 100 : Number(value)
  if (!Number.isFinite(maxlength)) maxlength = 100
  if (maxlength !== -1) maxlength = Math.max(1, Math.min(maxlength || 100, 4096))
  return maxlength
}

export class KeyboardSession {
  constructor({ onConfirm, onCancel } = {}) {
    this.activeUuid = ''
    this.editFinished = null
    this.onConfirm = onConfirm
    this.onCancel = onCancel
    this.stripNewlines = true
    this.closeDelayMs = 350
  }

  mount() {
    if (this.editFinished) return
    const gm = getGlobalModule()
    this.editFinished = (uuid, jsonData) => {
      if (!this.activeUuid || uuid !== this.activeUuid) {
        if (uuid) console.warn(`keyboard global result ignored uuid=${uuid} active=${this.activeUuid}`)
        return
      }
      const result = parseTextEditResult(jsonData)
      setTimeout(() => {
        if (!this.activeUuid || uuid !== this.activeUuid) return
        if (result.confirmed) {
          this.finish(result.text || '')
        } else {
          this.cancel()
        }
      }, 0)
    }
    if (gm.textEditFinished && gm.textEditFinished.on) {
      gm.textEditFinished.on(this.editFinished)
      console.warn('keyboard global listener mounted')
    } else {
      console.warn('keyboard global listener unavailable')
    }
  }

  unmount() {
    const gm = getGlobalModule()
    if (gm.textEditFinished && gm.textEditFinished.off && this.editFinished) {
      gm.textEditFinished.off(this.editFinished)
    }
    this.editFinished = null
    this.close()
  }

  open(options = {}) {
    if (this.activeUuid) return this.activeUuid
    this.stripNewlines = options.stripNewlines !== false
    this.closeDelayMs = options.closeDelayMs == null ? 350 : options.closeDelayMs
    const gm = getGlobalModule()
    if (!gm.startTextEdit) {
      console.warn('keyboard global startTextEdit unavailable')
      return ''
    }

    const config = {
      text: `${options.text || ''}`,
      placeholder: options.placeholder || '',
      autofocus: true,
      maxlength: clampMaxlength(options.maxlength),
      showCursor: true,
      cursorColor: options.cursorColor || '#008CFF',
      cursorSize: options.cursorSize || 3,
      confirmButtonDisabledOnTextEmpty: options.confirmButtonDisabledOnTextEmpty !== false,
      inputType: options.inputType || 'ZhCNPreferred',
      multiLinesEditVisible: !!options.multiLinesEditVisible,
      capsLockSwitchOn: false,
      enterButtonText: options.enterButtonText || '确认',
    }

    try {
      this.activeUuid = gm.startTextEdit(JSON.stringify(config)) || ''
      console.warn(`keyboard global startTextEdit string uuid ${this.activeUuid || 'empty'}`)
    } catch (err) {
      this.activeUuid = ''
      console.warn(`keyboard global startTextEdit string failed ${err}`)
    }
    return this.activeUuid
  }

  finish(text) {
    if (!this.activeUuid) return
    const uuid = this.activeUuid
    const value = `${text || ''}`
    const cleaned = this.stripNewlines ? value.replace(/\n/g, '') : value
    try {
      if (this.onConfirm) this.onConfirm(cleaned)
    } catch (err) {
      console.warn(`keyboard confirm handler failed ${err}`)
    }
    this.deferClose(uuid)
    this.activeUuid = ''
  }

  cancel() {
    if (!this.activeUuid) return
    const uuid = this.activeUuid
    try {
      if (this.onCancel) this.onCancel()
    } catch (err) {
      console.warn(`keyboard cancel handler failed ${err}`)
    }
    this.deferClose(uuid)
    this.activeUuid = ''
  }

  deferClose(uuid) {
    if (!uuid) return
    setTimeout(() => {
      const gm = getGlobalModule()
      if (gm.closeTextEdit) {
        try {
          gm.closeTextEdit(uuid)
          console.warn(`keyboard global closeTextEdit uuid ${uuid}`)
        } catch (err) {
          console.warn(`keyboard closeTextEdit failed ${err}`)
        }
      }
    }, this.closeDelayMs)
  }

  close() {
    const uuid = this.activeUuid
    if (!uuid) return
    const gm = getGlobalModule()
    if (gm.closeTextEdit) {
      try {
        gm.closeTextEdit(uuid)
        console.warn(`keyboard global close uuid ${uuid}`)
      } catch (err) {
        console.warn(`keyboard close failed ${err}`)
      }
    }
    this.activeUuid = ''
  }

  isActive() {
    return !!this.activeUuid
  }
}
