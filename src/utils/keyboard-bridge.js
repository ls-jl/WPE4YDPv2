import { KeyboardSession } from './keyboard'

export function createKeyboardBridgeState() {
  return {
    profileMode: 'globalOnly',
    profileReason: 'default',
    phase: 'idle',
    session: null,
    pollTimer: null,
    pollTick: 0,
    activeRequest: null,
    sequence: 0,
    lastUpdateText: null,
    completionDeadline: 0,
    completedRequestIds: {},
    textareaVisible: false,
    textareaFocused: false,
    textareaValue: '',
    textareaInputType: 'ZhCNPreferred',
    textareaMaxlength: 512,
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
  if (typeof value === 'string' && value.trim().charAt(0) === '{') {
    try {
      return normalizeKeyboardText(JSON.parse(value))
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
      onConfirm: (text) => this.finishRequest(true, text),
      onCancel: () => this.finishRequest(false, ''),
    })
    this.state.session.mount()
  }

  teardown() {
    this.stopPolling()
    if (this.state.activeRequest && this.state.phase !== 'responding') {
      this.writeResponse(false, '')
    }
    this.closeLocalKeyboard()
    this.resetActiveRequest('teardown', false)
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
    if (!this.state.pollTimer) return
    clearInterval(this.state.pollTimer)
    this.state.pollTimer = null
  }

  onPollTick() {
    this.state.pollTick += 1
    this.pruneCompletedRequests()
    if (this.state.activeRequest) {
      this.pollCompletion()
      if (this.state.phase === 'responding' && this.state.completionDeadline > 0 &&
          Date.now() >= this.state.completionDeadline) {
        console.warn(`keyboard completion timeout id=${this.state.activeRequest.id}`)
        this.resetActiveRequest('completion timeout')
      }
      return
    }
    if (this.state.pollTick % 3 === 0) this.pollRequest()
  }

  reconcileActiveRequest() {
    if (this.state.activeRequest) {
      this.pollCompletion()
      return false
    }
    this.pollRequest()
    return false
  }

  pollCompletion() {
    const request = this.state.activeRequest
    if (!request || !request.id || !this.browserPlayer.pollKeyboardCompletion) return
    let raw = ''
    try {
      raw = this.browserPlayer.pollKeyboardCompletion({
        workdir: this.workdir(),
        id: request.id,
      }) || ''
    } catch (err) {
      console.warn(`poll keyboard completion failed ${err}`)
      return
    }
    if (!raw) return
    let completion = null
    try {
      completion = JSON.parse(raw)
    } catch (err) {
      console.warn(`parse keyboard completion failed ${err}`)
      return
    }
    if (!completion || `${completion.id || ''}` !== `${request.id}`) return
    console.warn(`keyboard completion id=${request.id} status=${completion.status || 'unknown'} detail=${completion.detail || ''}`)
    this.resetActiveRequest(`native ${completion.status || 'completed'}`)
  }

  optionsForRequest(request) {
    const kind = request && request.kind ? `${request.kind}` : ''
    const text = request && request.text != null ? `${request.text}` : ''
    const placeholder = request && request.placeholder
      ? `${request.placeholder}`
      : (kind === 'web_input' ? '请输入内容' : '输入网址或搜索')
    const inputType = request && request.inputType
      ? `${request.inputType}`
      : (kind === 'web_input' ? 'ZhCNPreferred' : 'EnUSPreferred')
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
    const options = this.optionsForRequest(request)
    this.state.textareaValue = options.text
    this.state.textareaInputType = options.inputType
    this.state.textareaMaxlength = options.maxlength || 512
    this.state.textareaVisible = true
    this.state.textareaFocused = false
    this.state.textareaSeenInput = false
    this.state.phase = 'opening'
    console.warn(`keyboard textarea request id=${request.id} kind=${request.kind || ''} type=${options.inputType}`)
    setTimeout(() => {
      if (!this.isCurrentRequest(request.id) || this.state.phase !== 'opening') return
      this.state.textareaFocused = true
      this.state.phase = 'active'
      try {
        const field = this.component.$refs.keyboardTextarea
        if (field && field.focus) field.focus()
      } catch (err) {
        console.warn(`keyboard textarea focus failed ${err}`)
        this.finishRequest(false, '')
      }
    }, 0)
    return true
  }

  openGlobal(request) {
    if (!this.state.session) return false
    this.closeTextarea()
    this.state.phase = 'opening'
    const uuid = this.state.session.open(this.optionsForRequest(request))
    if (uuid) this.state.phase = 'active'
    console.warn(`keyboard global request id=${request.id} kind=${request.kind || ''} uuid=${uuid || ''}`)
    return !!uuid
  }

  closeTextarea() {
    this.state.textareaFocused = false
    this.state.textareaVisible = false
    this.state.textareaSeenInput = false
  }

  closeLocalKeyboard() {
    if (this.state.session && this.state.session.isActive()) this.state.session.close()
    this.closeTextarea()
  }

  onTextareaInput(value) {
    if (!this.state.activeRequest || this.state.phase === 'responding') return
    const text = normalizeKeyboardText(value)
    this.state.textareaValue = text
    this.state.textareaSeenInput = true
    if (this.state.lastUpdateText === text) return
    this.state.lastUpdateText = text
    this.state.sequence += 1
    this.updateRequest(text, this.state.sequence)
  }

  onTextareaConfirm(value) {
    if (!this.state.activeRequest || this.state.phase === 'responding') return
    const eventText = normalizeKeyboardText(value)
    const text = this.state.textareaSeenInput ? this.state.textareaValue : eventText
    this.finishRequest(true, text)
  }

  onTextareaFocus() {
    if (this.state.phase === 'opening') this.state.phase = 'active'
    console.warn('keyboard textarea focused')
  }

  onTextareaBlur() {
    this.state.textareaFocused = false
    console.warn('keyboard textarea blurred')
    const requestId = this.state.activeRequest ? this.state.activeRequest.id : ''
    setTimeout(() => {
      if (!requestId || !this.isCurrentRequest(requestId)) return
      if (this.state.phase === 'responding' || this.state.textareaFocused) return
      console.warn(`keyboard textarea cancel id=${requestId}`)
      this.finishRequest(false, '')
    }, 300)
  }

  updateRequest(text, sequence) {
    const request = this.state.activeRequest
    if (!request || !request.id || !this.browserPlayer.updateKeyboardRequest) return
    try {
      this.browserPlayer.updateKeyboardRequest({
        workdir: this.workdir(),
        id: request.id,
        sequence,
        text,
      })
      console.warn(`keyboard update id=${request.id} kind=${request.kind || ''} sequence=${sequence} bytes=${text.length}`)
    } catch (err) {
      console.warn(`update keyboard request failed ${err}`)
    }
  }

  pollRequest() {
    if (this.component.browser.leaving || this.state.phase !== 'idle') return
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
    let request = null
    try {
      request = JSON.parse(raw)
    } catch (err) {
      console.warn(`parse keyboard request failed ${err}`)
      return
    }
    if (!request || !request.id || this.state.completedRequestIds[request.id]) return

    this.state.activeRequest = request
    this.state.sequence = 0
    this.state.lastUpdateText = request.text == null ? '' : `${request.text}`
    this.state.completionDeadline = 0
    console.warn(`keyboard request id=${request.id} kind=${request.kind || ''} mode=${this.state.profileMode}`)
    const opened = this.state.profileMode === 'textareaOnly'
      ? this.openTextarea(request)
      : this.openGlobal(request)
    if (!opened) this.finishRequest(false, '')
  }

  writeResponse(confirmed, text) {
    const request = this.state.activeRequest
    if (!request || !request.id) return false
    try {
      this.browserPlayer.respondKeyboardRequest({
        workdir: this.workdir(),
        id: request.id,
        sequence: this.state.sequence,
        confirmed: !!confirmed,
        text: confirmed ? text : '',
      })
      console.warn(`keyboard ${confirmed ? 'confirmed' : 'cancelled'} id=${request.id} kind=${request.kind || ''} sequence=${this.state.sequence}`)
      return true
    } catch (err) {
      console.warn(`respond keyboard request failed ${err}`)
      return false
    }
  }

  finishRequest(confirmed, value) {
    if (!this.state.activeRequest || this.state.phase === 'responding') return
    const text = normalizeKeyboardText(value)
    if (confirmed && text !== this.state.lastUpdateText) {
      this.state.lastUpdateText = text
      this.state.sequence += 1
    }
    this.state.phase = 'responding'
    this.state.completionDeadline = Date.now() + 5000
    this.writeResponse(confirmed, text)
    this.closeLocalKeyboard()
  }

  resetActiveRequest(reason, notify = true) {
    const request = this.state.activeRequest
    if (request && request.id) this.state.completedRequestIds[request.id] = Date.now() + 10000
    this.closeLocalKeyboard()
    this.state.activeRequest = null
    this.state.sequence = 0
    this.state.lastUpdateText = null
    this.state.completionDeadline = 0
    this.state.phase = 'idle'
    console.warn(`keyboard session reset reason=${reason || ''}`)
    if (notify) this.notifyInactive()
  }

  pruneCompletedRequests() {
    const now = Date.now()
    Object.keys(this.state.completedRequestIds).forEach((id) => {
      if (this.state.completedRequestIds[id] <= now) delete this.state.completedRequestIds[id]
    })
  }

  isCurrentRequest(id) {
    return !!(this.state.activeRequest && `${this.state.activeRequest.id}` === `${id}`)
  }

  notifyInactive() {
    if (!this.component || !this.component.onKeyboardBridgeInactive) return
    setTimeout(() => this.component.onKeyboardBridgeInactive(), 0)
  }

  cancelActiveRequest(reason) {
    if (!this.state.activeRequest) {
      this.closeLocalKeyboard()
      return
    }
    console.warn(`keyboard cancel active reason=${reason || ''} id=${this.state.activeRequest.id}`)
    if (this.state.phase !== 'responding') this.writeResponse(false, '')
    this.resetActiveRequest(reason || 'cancelled')
  }

  isActive() {
    return this.state.phase !== 'idle' ||
      !!(this.state.session && this.state.session.isActive()) ||
      this.state.textareaVisible
  }
}
