import globalModule from 'global'
import { parseTextEditResult } from './keyboard-event'

let globalManager = null

export function getGlobalModule() {
  if (!globalManager) {
    globalManager = new globalModule.Global()
  }
  return globalManager
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
    this.closedUuids = {}
    this.closeTimers = {}
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
          this.finish(result.found ? result.text : '')
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
    if (!uuid || this.closedUuids[uuid] || this.closeTimers[uuid]) return
    this.closeTimers[uuid] = setTimeout(() => {
      delete this.closeTimers[uuid]
      this.closeUuid(uuid, 'deferred')
    }, this.closeDelayMs)
  }

  closeUuid(uuid, reason) {
    if (!uuid || this.closedUuids[uuid]) return
    if (this.closeTimers[uuid]) {
      clearTimeout(this.closeTimers[uuid])
      delete this.closeTimers[uuid]
    }
    this.closedUuids[uuid] = true
    setTimeout(() => { delete this.closedUuids[uuid] }, 60000)
    const gm = getGlobalModule()
    if (!gm.closeTextEdit) return
    try {
      gm.closeTextEdit(uuid)
      console.warn(`keyboard global closeTextEdit uuid=${uuid} reason=${reason || ''}`)
    } catch (err) {
      console.warn(`keyboard closeTextEdit failed ${err}`)
    }
  }

  close() {
    const uuid = this.activeUuid
    if (!uuid) return
    this.activeUuid = ''
    this.closeUuid(uuid, 'session close')
  }

  isActive() {
    return !!this.activeUuid
  }
}
