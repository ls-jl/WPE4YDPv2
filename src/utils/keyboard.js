import globalModule from 'global'
import { parseTextEditResult } from './keyboard-event'

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
    this.resultTimers = new Set()
    this.manager = null
    this.disposed = false
  }

  getManager() {
    if (!this.manager) this.manager = new globalModule.Global()
    return this.manager
  }

  mount() {
    if (this.editFinished) return
    this.disposed = false
    const gm = this.getManager()
    this.editFinished = (uuid, jsonData) => {
      if (!this.activeUuid || uuid !== this.activeUuid) {
        if (uuid) console.warn(`keyboard global result ignored uuid=${uuid} active=${this.activeUuid}`)
        return
      }
      const result = parseTextEditResult(jsonData)
      if (!result.terminal) return
      const timer = setTimeout(() => {
        this.resultTimers.delete(timer)
        if (this.disposed || !this.activeUuid || uuid !== this.activeUuid) return
        if (result.confirmed) {
          this.finish(result.found ? result.text : '')
        } else {
          this.cancel()
        }
      }, 0)
      this.resultTimers.add(timer)
    }
    if (gm.textEditFinished && gm.textEditFinished.on) {
      gm.textEditFinished.on(this.editFinished)
      console.warn('keyboard global listener mounted')
    } else {
      console.warn('keyboard global listener unavailable')
    }
  }

  unmount() {
    this.disposed = true
    const gm = this.manager
    if (gm && gm.textEditFinished && gm.textEditFinished.off && this.editFinished) {
      gm.textEditFinished.off(this.editFinished)
    }
    this.editFinished = null
    this.resultTimers.forEach(timer => clearTimeout(timer))
    this.resultTimers.clear()
    this.close()
    Object.keys(this.closeTimers).forEach(uuid => this.closeUuid(uuid, 'unmount'))
    this.manager = null
    this.closedUuids = {}
  }

  open(options = {}) {
    if (this.disposed) return ''
    if (this.activeUuid) return this.activeUuid
    this.stripNewlines = options.stripNewlines !== false
    this.closeDelayMs = options.closeDelayMs == null ? 350 : options.closeDelayMs
    const gm = this.getManager()
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
    this.activeUuid = ''
    const value = `${text || ''}`
    const cleaned = this.stripNewlines ? value.replace(/\n/g, '') : value
    this.deferClose(uuid)
    try {
      if (this.onConfirm) this.onConfirm(cleaned)
    } catch (err) {
      console.warn(`keyboard confirm handler failed ${err}`)
    }
  }

  cancel() {
    if (!this.activeUuid) return
    const uuid = this.activeUuid
    this.activeUuid = ''
    this.deferClose(uuid)
    try {
      if (this.onCancel) this.onCancel()
    } catch (err) {
      console.warn(`keyboard cancel handler failed ${err}`)
    }
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
    const closed = Object.keys(this.closedUuids)
    if (closed.length > 128) delete this.closedUuids[closed[0]]
    const gm = this.manager
    if (!gm) return
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
