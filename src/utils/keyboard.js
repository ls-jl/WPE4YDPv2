import globalModule from 'global'

let globalManager = null

export function getGlobalModule() {
  if (!globalManager) {
    globalManager = new globalModule.Global()
  }
  return globalManager
}

export class KeyboardSession {
  constructor({ onConfirm, onCancel } = {}) {
    this.inputTaskUuid = ''
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
    }
  }

  unmount() {
    const gm = getGlobalModule()
    if (gm.textEditFinished && this.editFinished) {
      gm.textEditFinished.off(this.editFinished)
    }
    this.close()
    this.editFinished = null
  }

  open(options = {}) {
    const gm = getGlobalModule()
    if (!gm.startTextEdit) {
      console.log('keyboard startTextEdit unavailable')
      return ''
    }
    if (this.inputTaskUuid) return this.inputTaskUuid
    this.stripNewlines = options.stripNewlines !== false
    this.closeDelayMs = options.closeDelayMs == null ? 350 : options.closeDelayMs
    const maxlength = options.maxlength == null ? 100 : Math.max(1, Math.min(Number(options.maxlength) || 100, 100))
    const config = {
      text: options.text || '',
      placeholder: options.placeholder || '',
      placeholderColor: options.placeholderColor || '#878A99',
      autofocus: true,
      maxlength,
      showCursor: true,
      cursorColor: options.cursorColor || '#FF683D',
      cursorSize: options.cursorSize || 3,
      confirmButtonDisabledOnTextEmpty: options.confirmButtonDisabledOnTextEmpty !== false,
      inputType: options.inputType || 'ZhCNPreferred',
      multiLinesEditVisible: !!options.multiLinesEditVisible,
      capsLockSwitchOn: false,
      enterButtonText: options.enterButtonText || '确认',
      returnButtonVisible: options.returnButtonVisible !== false,
      closeButtonVisible: options.closeButtonVisible !== false,
      micInputVisible: !!options.micInputVisible,
    }
    try {
      this.inputTaskUuid = gm.startTextEdit(JSON.stringify(config)) || ''
      console.log(`keyboard startTextEdit uuid ${this.inputTaskUuid}`)
    } catch (err) {
      console.log(`keyboard startTextEdit failed ${err}`)
      this.inputTaskUuid = ''
    }
    return this.inputTaskUuid
  }

  finish(text) {
    const uuid = this.inputTaskUuid
    const value = `${text || ''}`
    const cleaned = this.stripNewlines ? value.replace(/\n/g, '') : value
    if (this.onConfirm) {
      this.onConfirm(cleaned)
    }
    this.deferClose(uuid)
    if (this.inputTaskUuid === uuid) {
      this.inputTaskUuid = ''
    }
  }

  cancel() {
    const uuid = this.inputTaskUuid
    if (this.onCancel) {
      this.onCancel()
    }
    if (this.inputTaskUuid === uuid) {
      this.inputTaskUuid = ''
    }
  }

  deferClose(uuid) {
    if (!uuid) return
    setTimeout(() => {
      const gm = getGlobalModule()
      if (gm.closeTextEdit) {
        try {
          gm.closeTextEdit(uuid)
        } catch (err) {
          console.log(`keyboard closeTextEdit failed ${err}`)
        }
      }
    }, this.closeDelayMs)
  }

  close() {
    const gm = getGlobalModule()
    if (gm.closeTextEdit && this.inputTaskUuid) {
      try {
        gm.closeTextEdit(this.inputTaskUuid)
      } catch (err) {
        console.log(`keyboard close failed ${err}`)
      }
    }
    this.inputTaskUuid = ''
  }

  isActive() {
    return !!this.inputTaskUuid
  }
}
