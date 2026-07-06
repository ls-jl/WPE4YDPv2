import { KeyboardSession } from './keyboard'

export function createKeyboardBridgeState() {
  return {
    profileMode: 'globalOnly',
    profileReason: 'default',
    session: null,
    pollTimer: null,
    pollTick: 0,
    activeUntil: 0,
    activeRequest: null,
    textareaVisible: false,
    textareaFocused: false,
    textareaValue: '',
    textareaInputType: 'ZhCNPreferred',
    textareaMaxlength: 512,
    textareaActive: false,
    textareaSeenInput: false,
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

export class KeyboardBridge {
  constructor(component, browserPlayer, workdirGetter, runningGetter) {
    this.component = component
    this.browserPlayer = browserPlayer
    this.workdirGetter = workdirGetter
    this.runningGetter = runningGetter
  }

  get state() {
    return this.component.keyboard
  }

  workdir() {
    return this.workdirGetter ? this.workdirGetter() : ''
  }

  setup() {
    this.readProfile()
    if (this.state.session || this.state.profileMode !== 'globalOnly') return
    this.state.session = new KeyboardSession({
      onConfirm: (text) => {
        const value = this.state.textareaSeenInput ? this.state.textareaValue : text
        this.finishRequest(true, value)
      },
      onCancel: () => this.finishRequest(false, ''),
    })
    this.state.session.mount()
  }

  teardown() {
    this.stopPolling()
    this.cancelActiveRequest('teardown')
    this.closeTextarea()
    if (this.state.session) {
      this.state.session.unmount()
      this.state.session = null
    }
  }

  readProfile() {
    let profile = null
    try {
      if (this.browserPlayer.getKeyboardProfile) {
        const raw = this.browserPlayer.getKeyboardProfile() || ''
        profile = raw ? JSON.parse(raw) : null
      }
    } catch (err) {
      console.warn(`read keyboard profile failed ${err}`)
    }
    this.state.profileMode = profile && profile.mode === 'textareaOnly' ? 'textareaOnly' : 'globalOnly'
    this.state.profileReason = profile && profile.reason ? `${profile.reason}` : 'default'
    console.warn(`keyboard profile mode=${this.state.profileMode} reason=${this.state.profileReason}`)
  }

  startPolling() {
    if (this.state.pollTimer) return
    this.state.pollTimer = setInterval(() => this.onPollTick(), 200)
    this.pollRequest()
  }

  stopPolling() {
    if (this.state.pollTimer) {
      clearInterval(this.state.pollTimer)
      this.state.pollTimer = null
    }
  }

  onPollTick() {
    this.state.pollTick += 1
    const active = Date.now() < this.state.activeUntil
    if (active || this.state.pollTick % 3 === 0) {
      this.pollRequest()
    }
  }

  optionsForRequest(request) {
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
  }

  openTextarea(request) {
    if (!request || !request.id) return false
    const options = this.optionsForRequest(request)
    this.state.textareaValue = options.text || ''
    this.state.textareaInputType = options.inputType || 'ZhCNPreferred'
    this.state.textareaMaxlength = options.maxlength || 512
    this.state.textareaVisible = true
    this.state.textareaFocused = false
    this.state.textareaActive = true
    this.state.textareaSeenInput = false
    console.warn(`keyboard textarea request id=${request.id} kind=${request.kind || ''} type=${this.state.textareaInputType}`)
    setTimeout(() => {
      if (!this.state.activeRequest || this.state.activeRequest.id !== request.id) return
      this.state.textareaFocused = true
      try {
        const field = this.component.$refs.keyboardTextarea
        if (field && field.focus) field.focus()
      } catch (err) {
        console.warn(`keyboard textarea focus failed ${err}`)
      }
    }, 0)
    return true
  }

  closeTextarea() {
    this.state.textareaFocused = false
    this.state.textareaActive = false
    this.state.textareaVisible = false
    this.state.textareaSeenInput = false
  }

  openGlobal(request) {
    if (!request || !request.id || !this.state.session) return false
    const options = this.optionsForRequest(request)
    this.closeTextarea()
    const uuid = this.state.session.open(options)
    console.warn(`keyboard global request id=${request.id} kind=${request.kind || ''} uuid=${uuid || ''}`)
    return !!uuid
  }

  onTextareaInput(value) {
    this.state.textareaValue = normalizeKeyboardText(value)
    this.state.textareaSeenInput = true
    console.warn(`keyboard textarea input ${this.state.textareaValue || ''}`)
    this.updateRequest(this.state.textareaValue)
  }

  onTextareaConfirm(value) {
    if (!this.state.activeRequest) return
    const text = this.state.textareaSeenInput ? this.state.textareaValue : normalizeKeyboardText(value)
    this.state.textareaValue = text
    console.warn(`keyboard textarea confirm ${text || ''}`)
    this.finishRequest(true, text)
    if (this.state.session && this.state.session.isActive()) {
      this.state.session.close()
    }
    this.closeTextarea()
  }

  onTextareaFocus() {
    this.state.textareaActive = true
    console.warn('keyboard textarea focused')
  }

  onTextareaBlur() {
    this.state.textareaFocused = false
    console.warn('keyboard textarea blurred')
    setTimeout(() => {
      if (!this.state.activeRequest) return
      if (this.state.session && this.state.session.isActive()) return
      if (this.state.textareaFocused) return
      console.warn(`keyboard textarea cancel id=${this.state.activeRequest.id}`)
      this.finishRequest(false, '')
      this.closeTextarea()
    }, 250)
  }

  updateRequest(text) {
    const request = this.state.activeRequest
    if (!request || !request.id || !this.browserPlayer.updateKeyboardRequest) return
    try {
      this.browserPlayer.updateKeyboardRequest({
        workdir: this.workdir(),
        id: request.id,
        text: `${text || ''}`,
      })
      console.warn(`keyboard update id=${request.id} kind=${request.kind || ''} bytes=${`${text || ''}`.length}`)
    } catch (err) {
      console.warn(`update keyboard request failed ${err}`)
    }
  }

  pollRequest() {
    if (this.component.browser.leaving || this.state.activeRequest) return
    if (!this.browserPlayer.pollKeyboardRequest) return
    if (this.state.profileMode === 'globalOnly' && !this.state.session) return
    if (this.runningGetter && !this.runningGetter()) return

    let raw = ''
    try {
      raw = this.browserPlayer.pollKeyboardRequest({ workdir: this.workdir() }) || ''
    } catch (err) {
      console.warn(`poll keyboard request failed ${err}`)
      return
    }
    if (!raw) return
    this.state.activeUntil = Date.now() + 10000

    let request = null
    try {
      request = JSON.parse(raw)
    } catch (err) {
      console.warn(`parse keyboard request failed ${err}`)
      return
    }
    if (!request || !request.id) return

    this.state.activeRequest = request
    console.warn(`keyboard request id=${request.id} kind=${request.kind || ''} mode=${this.state.profileMode}`)
    if (this.state.profileMode === 'textareaOnly') {
      if (!this.openTextarea(request)) {
        this.finishRequest(false, '')
      }
      return
    }
    if (!this.openGlobal(request)) {
      this.finishRequest(false, '')
    }
  }

  finishRequest(confirmed, text) {
    const request = this.state.activeRequest
    if (!request || !request.id) return
    this.state.activeRequest = null
    this.state.activeUntil = Date.now() + 10000
    try {
      this.browserPlayer.respondKeyboardRequest({
        workdir: this.workdir(),
        id: request.id,
        confirmed: !!confirmed,
        text: confirmed ? `${text || ''}` : '',
      })
      console.warn(`keyboard ${confirmed ? 'confirmed' : 'cancelled'} id=${request.id} kind=${request.kind || ''}`)
    } catch (err) {
      console.warn(`respond keyboard request failed ${err}`)
    }
    this.closeTextarea()
  }

  cancelActiveRequest(reason) {
    if (this.state.activeRequest) {
      console.warn(`keyboard cancel active reason=${reason || ''} id=${this.state.activeRequest.id}`)
      this.finishRequest(false, '')
    }
    if (this.state.session && this.state.session.isActive()) {
      this.state.session.close()
    }
    this.closeTextarea()
  }

  isActive() {
    return !!((this.state.session && this.state.session.isActive()) || this.state.textareaActive)
  }
}
