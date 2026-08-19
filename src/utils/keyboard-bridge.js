import { KeyboardSession } from './keyboard'
import { keyboardBackendOverride } from './display-resolver'
import { normalizeKeyboardEvent, parseTextEditResult } from './keyboard-event'

const TEXTAREA_PROBE_MS = 1500
const BACKEND_SWITCH_SETTLE_MS = 250
const TEXTAREA_RETURN_CANCEL_MS = 800
const COMPLETION_TIMEOUT_MS = 15000
// Native keeps a keyboard request alive for up to 120 seconds. Remember a
// locally completed request beyond that window so a delayed/missing
// completion file cannot reopen the same keyboard session.
const COMPLETED_REQUEST_TTL_MS = 130000
const RESPONSE_RETRY_DELAYS_MS = [100, 250, 500, 1000, 1500, 2000, 2500]

export function createKeyboardBridgeState() {
  return {
    profileMode: 'auto',
    profileSource: 'probe',
    profileOverride: 'auto',
    phase: 'idle',
    session: null,
    pollTimer: null,
    pollTick: 0,
    activeRequest: null,
    activeBackend: '',
    attemptedBackends: {},
    probeTimer: null,
    backendSwitchTimer: null,
    returnCancelTimer: null,
    sequence: 0,
    lastUpdateText: null,
    completionDeadline: 0,
    responseRetryTimer: null,
    responseRetryAttempt: 0,
    pendingResponse: null,
    completedRequestIds: {},
    textareaVisible: false,
    textareaFocused: false,
    textareaBlurred: false,
    textareaValue: '',
    textareaInputType: 'ZhCNPreferred',
    textareaMaxlength: 512,
    textareaSeenInput: false,
  }
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
    if (!this.workdir()) {
      console.warn('keyboard bridge disabled: browser workdir is unavailable')
      return
    }
    this.readProfile()
    if (this.state.session) return
    this.state.session = new KeyboardSession({
      onConfirm: (text) => this.finishRequest(true, text),
      onCancel: () => this.finishRequest(false, ''),
    })
    this.state.session.mount()
  }

  teardown() {
    this.stopPolling()
    this.clearBackendTimers()
    this.clearResponseRetryTimer()
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
    this.state.profileOverride = keyboardBackendOverride()
    try {
      if (this.browserPlayer.getKeyboardProfile) {
        const raw = this.browserPlayer.getKeyboardProfile({
          workdir: this.workdir(),
          override: this.state.profileOverride,
        }) || ''
        profile = raw ? JSON.parse(raw) : null
      }
    } catch (err) {
      console.warn(`read keyboard profile failed ${err}`)
    }
    const mode = profile && `${profile.mode || ''}`
    this.state.profileMode = mode === 'textarea' || mode === 'global' ? mode : 'auto'
    this.state.profileSource = profile && profile.source ? `${profile.source}` : 'probe'
    console.warn(`keyboard profile mode=${this.state.profileMode} source=${this.state.profileSource} override=${this.state.profileOverride}`)
  }

  reportBackend(backend, successful, evidence) {
    if (!this.browserPlayer.reportKeyboardBackend || this.state.profileOverride !== 'auto') return
    try {
      this.browserPlayer.reportKeyboardBackend({
        workdir: this.workdir(),
        backend,
        successful: !!successful,
        evidence: evidence || '',
      })
    } catch (err) {
      console.warn(`report keyboard backend failed ${err}`)
    }
  }

  markBackendSuccess(backend, evidence) {
    if (!backend || this.state.activeBackend !== backend) return
    if (backend === 'textarea') this.clearProbeTimer()
    if (this.state.profileOverride === 'auto') {
      const shouldPersist = this.state.profileMode !== backend ||
        this.state.profileSource !== 'learned'
      this.state.profileMode = backend
      this.state.profileSource = 'learned'
      if (shouldPersist) this.reportBackend(backend, true, evidence)
    }
    console.warn(`keyboard backend ready backend=${backend} evidence=${evidence || ''}`)
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
      console.warn(`parse keyboard completion failed id=${request.id} ${err}`)
      return
    }
    if (!completion || `${completion.id || ''}` !== `${request.id}` || !completion.status) return
    try {
      if (this.browserPlayer.ackKeyboardCompletion) {
        this.browserPlayer.ackKeyboardCompletion({
          workdir: this.workdir(),
          id: request.id,
        })
      }
    } catch (err) {
      console.warn(`ack keyboard completion failed id=${request.id} ${err}`)
      return
    }
    console.warn(`keyboard completion id=${request.id} status=${completion.status} detail=${completion.detail || ''}`)
    this.resetActiveRequest(`native ${completion.status}`)
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

  preferredBackend() {
    return this.state.profileMode === 'global' ? 'global' : 'textarea'
  }

  openBackend(request, backend) {
    if (!this.isCurrentRequest(request.id)) return false
    this.state.activeBackend = backend
    this.state.attemptedBackends[backend] = true
    return backend === 'textarea' ? this.openTextarea(request) : this.openGlobal(request)
  }

  openTextarea(request) {
    const options = this.optionsForRequest(request)
    this.state.textareaValue = options.text
    this.state.textareaInputType = options.inputType
    this.state.textareaMaxlength = options.maxlength || 512
    this.state.textareaVisible = true
    this.state.textareaFocused = false
    this.state.textareaBlurred = false
    this.state.textareaSeenInput = false
    this.state.phase = 'opening'
    console.warn(`keyboard textarea request id=${request.id} kind=${request.kind || ''} type=${options.inputType}`)
    setTimeout(() => {
      if (!this.isCurrentRequest(request.id) || this.state.activeBackend !== 'textarea' ||
          this.state.phase !== 'opening') return
      this.state.textareaFocused = true
      try {
        const field = this.component.$refs.keyboardTextarea
        if (field && field.focus) field.focus()
      } catch (err) {
        console.warn(`keyboard textarea focus failed ${err}`)
        this.failBackend('textarea', 'focus_error')
        return
      }
      this.startTextareaProbe(request.id)
    }, 0)
    return true
  }

  startTextareaProbe(requestId) {
    this.clearProbeTimer()
    this.state.probeTimer = setTimeout(() => {
      this.state.probeTimer = null
      if (!this.isCurrentRequest(requestId) || this.state.activeBackend !== 'textarea') return
      console.warn(`keyboard textarea probe timeout id=${requestId}`)
      this.failBackend('textarea', 'probe_timeout')
    }, TEXTAREA_PROBE_MS)
  }

  openGlobal(request) {
    if (!this.state.session) return false
    this.closeTextarea()
    this.state.phase = 'opening'
    const uuid = this.state.session.open(this.optionsForRequest(request))
    if (uuid) {
      this.state.phase = 'active'
      this.markBackendSuccess('global', 'uuid')
    }
    console.warn(`keyboard global request id=${request.id} kind=${request.kind || ''} uuid=${uuid || ''}`)
    if (!uuid) this.failBackend('global', 'empty_uuid')
    return !!uuid
  }

  failBackend(backend, evidence) {
    const request = this.state.activeRequest
    if (!request || this.state.activeBackend !== backend || this.state.phase === 'responding') return
    this.reportBackend(backend, false, evidence)
    if (backend === 'global' && this.state.session && this.state.session.isActive()) {
      this.state.session.close()
    }
    if (backend === 'textarea') this.closeTextarea()
    const fallback = backend === 'textarea' ? 'global' : 'textarea'
    const allowFallback = this.state.profileOverride === 'auto' && !this.state.attemptedBackends[fallback]
    if (!allowFallback) {
      this.finishRequest(false, '')
      return
    }
    this.state.phase = 'opening'
    this.clearBackendSwitchTimer()
    this.state.backendSwitchTimer = setTimeout(() => {
      this.state.backendSwitchTimer = null
      if (!this.isCurrentRequest(request.id) || this.state.phase !== 'opening') return
      console.warn(`keyboard backend fallback from=${backend} to=${fallback} id=${request.id}`)
      this.openBackend(request, fallback)
    }, BACKEND_SWITCH_SETTLE_MS)
  }

  clearProbeTimer() {
    if (!this.state.probeTimer) return
    clearTimeout(this.state.probeTimer)
    this.state.probeTimer = null
  }

  clearBackendSwitchTimer() {
    if (!this.state.backendSwitchTimer) return
    clearTimeout(this.state.backendSwitchTimer)
    this.state.backendSwitchTimer = null
  }

  clearReturnCancelTimer() {
    if (!this.state.returnCancelTimer) return
    clearTimeout(this.state.returnCancelTimer)
    this.state.returnCancelTimer = null
  }

  clearBackendTimers() {
    this.clearProbeTimer()
    this.clearBackendSwitchTimer()
    this.clearReturnCancelTimer()
  }

  clearResponseRetryTimer() {
    if (!this.state.responseRetryTimer) return
    clearTimeout(this.state.responseRetryTimer)
    this.state.responseRetryTimer = null
  }

  closeTextarea() {
    this.clearProbeTimer()
    this.state.textareaFocused = false
    this.state.textareaVisible = false
    this.state.textareaBlurred = false
    this.state.textareaSeenInput = false
  }

  closeLocalKeyboard() {
    if (this.state.session && this.state.session.isActive()) this.state.session.close()
    this.closeTextarea()
  }

  onTextareaInput(value) {
    if (!this.state.activeRequest || this.state.activeBackend !== 'textarea' ||
        this.state.phase === 'responding') return
    const normalized = normalizeKeyboardEvent(value)
    if (!normalized.found) {
      console.warn('keyboard textarea input ignored: no text field')
      return
    }
    this.markBackendSuccess('textarea', 'input')
    this.state.phase = 'active'
    this.state.textareaValue = normalized.text
    this.state.textareaSeenInput = true
    if (this.state.lastUpdateText === normalized.text) return
    this.state.lastUpdateText = normalized.text
    this.state.sequence += 1
    this.updateRequest(normalized.text, this.state.sequence)
  }

  onTextareaConfirm(value) {
    if (!this.state.activeRequest || this.state.activeBackend !== 'textarea' ||
        this.state.phase === 'responding') return
    const normalized = normalizeKeyboardEvent(value)
    this.markBackendSuccess('textarea', 'confirm')
    const text = this.state.textareaSeenInput
      ? this.state.textareaValue
      : (normalized.found ? normalized.text : this.state.textareaValue)
    this.finishRequest(true, text)
  }

  onTextareaFinished(value) {
    if (!this.state.activeRequest || this.state.activeBackend !== 'textarea' ||
        this.state.phase === 'responding') return
    const result = parseTextEditResult(value)
    if (!result.terminal) {
      if (result.found) this.onTextareaInput(result.text)
      return
    }
    this.markBackendSuccess('textarea', 'textEditFinished')
    if (!result.confirmed) {
      this.finishRequest(false, '')
      return
    }
    const text = this.state.textareaSeenInput
      ? this.state.textareaValue
      : (result.found ? result.text : this.state.textareaValue)
    this.finishRequest(true, text)
  }

  onTextareaFocus() {
    this.state.textareaFocused = true
    this.state.textareaBlurred = false
    this.clearReturnCancelTimer()
    console.warn('keyboard textarea focused')
  }

  onTextareaBlur() {
    this.state.textareaFocused = false
    this.state.textareaBlurred = true
    console.warn('keyboard textarea blurred; waiting for page return or terminal event')
  }

  onPageHide() {
    if (!this.isActive()) return false
    if (this.state.activeBackend === 'textarea' &&
        (this.state.phase === 'opening' || this.state.phase === 'active')) {
      this.markBackendSuccess('textarea', 'page_hide')
      this.state.phase = 'active'
    }
    return true
  }

  onPageShow() {
    this.clearReturnCancelTimer()
    if (!this.state.activeRequest || this.state.activeBackend !== 'textarea' ||
        !this.state.textareaBlurred || this.state.phase === 'responding') return
    const requestId = this.state.activeRequest.id
    this.state.returnCancelTimer = setTimeout(() => {
      this.state.returnCancelTimer = null
      if (!this.isCurrentRequest(requestId) || this.state.textareaFocused ||
          !this.state.textareaBlurred || this.state.phase === 'responding') return
      console.warn(`keyboard textarea cancelled after page return id=${requestId}`)
      this.finishRequest(false, '')
    }, TEXTAREA_RETURN_CANCEL_MS)
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
      console.warn(`keyboard update id=${request.id} backend=${this.state.activeBackend} kind=${request.kind || ''} sequence=${sequence} bytes=${text.length}`)
    } catch (err) {
      console.warn(`update keyboard request failed ${err}`)
    }
  }

  pollRequest() {
    if (this.component.browser.leaving || this.state.phase !== 'idle') return
    if (!this.browserPlayer.pollKeyboardRequest || !this.state.session) return
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
    this.state.activeBackend = ''
    this.state.attemptedBackends = {}
    this.state.sequence = 0
    this.state.lastUpdateText = request.text == null ? '' : `${request.text}`
    this.state.completionDeadline = 0
    console.warn(`keyboard request id=${request.id} kind=${request.kind || ''} preferred=${this.preferredBackend()}`)
    this.openBackend(request, this.preferredBackend())
  }

  writeResponse(confirmed, text) {
    const request = this.state.activeRequest
    if (!request || !request.id) return false
    try {
      const result = this.browserPlayer.respondKeyboardRequest({
        workdir: this.workdir(),
        id: request.id,
        sequence: this.state.sequence,
        confirmed: !!confirmed,
        text: confirmed ? text : '',
      })
      if (result === false) return false
      console.warn(`keyboard ${confirmed ? 'confirmed' : 'cancelled'} id=${request.id} backend=${this.state.activeBackend} kind=${request.kind || ''} sequence=${this.state.sequence}`)
      return true
    } catch (err) {
      console.warn(`respond keyboard request failed ${err}`)
      return false
    }
  }

  publishPendingResponse() {
    const request = this.state.activeRequest
    const response = this.state.pendingResponse
    if (!request || !response || this.state.phase !== 'responding') return
    this.clearResponseRetryTimer()
    if (this.writeResponse(response.confirmed, response.text)) {
      this.state.pendingResponse = null
      this.state.responseRetryAttempt = 0
      this.closeLocalKeyboard()
      return
    }

    const attempt = this.state.responseRetryAttempt
    if (attempt >= RESPONSE_RETRY_DELAYS_MS.length) {
      console.warn(`keyboard terminal response abandoned id=${request.id} attempts=${attempt + 1}`)
      this.closeLocalKeyboard()
      this.resetActiveRequest('terminal response write failed')
      return
    }
    const delay = RESPONSE_RETRY_DELAYS_MS[attempt]
    this.state.responseRetryAttempt += 1
    console.warn(`keyboard terminal response retry id=${request.id} attempt=${attempt + 1} delay=${delay}`)
    this.state.responseRetryTimer = setTimeout(() => {
      this.state.responseRetryTimer = null
      this.publishPendingResponse()
    }, delay)
  }

  finishRequest(confirmed, value) {
    if (!this.state.activeRequest || this.state.phase === 'responding') return
    const normalized = normalizeKeyboardEvent(value)
    const text = normalized.found ? normalized.text : ''
    if (confirmed && text !== this.state.lastUpdateText) {
      this.state.lastUpdateText = text
      this.state.sequence += 1
    }
    this.state.phase = 'responding'
    this.state.completionDeadline = Date.now() + COMPLETION_TIMEOUT_MS
    this.clearBackendTimers()
    this.state.pendingResponse = { confirmed: !!confirmed, text }
    this.state.responseRetryAttempt = 0
    this.publishPendingResponse()
  }

  resetActiveRequest(reason, notify = true) {
    const request = this.state.activeRequest
    if (request && request.id) {
      this.state.completedRequestIds[request.id] = Date.now() + COMPLETED_REQUEST_TTL_MS
    }
    this.clearBackendTimers()
    this.clearResponseRetryTimer()
    this.closeLocalKeyboard()
    this.state.activeRequest = null
    this.state.activeBackend = ''
    this.state.attemptedBackends = {}
    this.state.sequence = 0
    this.state.lastUpdateText = null
    this.state.completionDeadline = 0
    this.state.responseRetryAttempt = 0
    this.state.pendingResponse = null
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
