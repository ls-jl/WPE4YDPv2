import {
  browserDataPath,
  browserOptions,
  LAST_BROWSER_MODE,
  normalizeBrowserMode,
} from './display-resolver'

export function parseBrowserExitStatus(raw) {
  if (!raw) return { reason: 'exited', code: null, browserMode: '', message: '浏览器已退出' }
  try {
    const status = typeof raw === 'string' ? JSON.parse(raw) : raw
    const code = Number(status.code)
    const normalizedCode = Number.isFinite(code) ? code : null
    if (status.reason === 'rotation_change' && normalizedCode === 72) {
      return {
        reason: 'rotation_change',
        code: 72,
        browserMode: normalizeBrowserMode(status.browserMode),
        message: '正在应用显示方向',
      }
    }
    if (status.reason === 'user_shutdown') {
      return { reason: 'user_shutdown', code: normalizedCode, browserMode: '', message: '浏览器已关闭' }
    }
    if (status.reason === 'start_failed') {
      return { reason: 'start_failed', code: normalizedCode, browserMode: '', message: `浏览器启动失败（错误码 ${normalizedCode === null ? 'unknown' : normalizedCode}）` }
    }
    if (status.reason === 'crash') {
      return { reason: 'crash', code: normalizedCode, browserMode: '', message: `浏览器异常退出（错误码 ${normalizedCode === null ? 'unknown' : normalizedCode}）` }
    }
    return { reason: status.reason || 'exited', code: normalizedCode, browserMode: '', message: '浏览器已退出' }
  } catch (err) {
    console.warn(`parse browser exit status failed ${err}`)
    return { reason: 'invalid', code: null, browserMode: '', message: '浏览器已退出' }
  }
}

export function createBrowserLifecycleState() {
  return {
    running: false,
    starting: false,
    leaving: false,
    startToken: 0,
    watchdogTimer: null,
    browserModeOverride: '',
    rotationRestartCount: 0,
  }
}

export class BrowserLifecycle {
  constructor(component, browserPlayer, keyboardBridge) {
    this.component = component
    this.browserPlayer = browserPlayer
    this.keyboardBridge = keyboardBridge
  }

  get state() {
    return this.component.browser
  }

  pageOptions() {
    const options = this.component.pageOptions()
    if (!this.state.browserModeOverride) return options
    return Object.assign({}, options, {
      browserMode: this.state.browserModeOverride,
    })
  }

  workdir() {
    const options = this.pageOptions()
    return options.workdir || browserDataPath('')
  }

  requireWorkdir() {
    const workdir = this.workdir()
    if (!workdir) throw new Error('MiniApp 数据目录不可用，已阻止浏览器启动')
    return workdir
  }

  async resolveOptions() {
    return browserOptions(this.component, this.browserPlayer, this.pageOptions())
  }

  start() {
    if (this.state.starting || this.state.leaving) return
    try {
      this.requireWorkdir()
    } catch (err) {
      this.leaveFrame(err.message)
      return
    }
    try {
      if (this.browserPlayer.isBrowserRunning && this.browserPlayer.isBrowserRunning({ workdir: this.workdir() })) {
        this.state.running = true
        this.startWatchdog()
        if (this.keyboardBridge) this.keyboardBridge.startPolling()
        return
      }
    } catch (err) {
      console.warn(`check browser running failed ${err}`)
    }

    this.state.starting = true
    const token = this.state.startToken + 1
    this.state.startToken = token
    setTimeout(() => {
      if (token !== this.state.startToken || this.state.leaving) return
      this.resolveOptions().then((launchOptions) => {
        if (token !== this.state.startToken || this.state.leaving) return
        const pid = this.browserPlayer.startBrowser(launchOptions)
        this.state.running = Number(pid) > 0
        console.warn(`wpe browser pid ${pid}`)
        if (this.state.running) {
          this.startWatchdog()
          if (this.keyboardBridge) this.keyboardBridge.startPolling()
        } else {
          this.leaveFrame('start failed')
        }
      }).catch((err) => {
        console.warn(`start browser failed ${err}`)
        this.state.running = false
        this.leaveFrame(err && err.message ? err.message : 'start exception')
      }).then(() => {
        this.state.starting = false
      })
    }, 0)
  }

  startWatchdog() {
    if (this.state.watchdogTimer) return
    this.state.watchdogTimer = setInterval(() => this.watch(), 1000)
  }

  stopWatchdog() {
    if (this.state.watchdogTimer) {
      clearInterval(this.state.watchdogTimer)
      this.state.watchdogTimer = null
    }
  }

  watch() {
    if (this.state.starting || this.state.leaving) return
    let running = false
    try {
      running = this.browserPlayer.isBrowserRunning({ workdir: this.workdir() })
    } catch (err) {
      console.warn(`watch browser failed ${err}`)
    }
    if (running) {
      this.state.running = true
      return
    }
    this.state.running = false
    const status = this.consumeExitStatus()
    if (status.reason === 'rotation_change') {
      this.restartForRotation(status.browserMode)
      return
    }
    this.leaveFrame(status.message)
  }

  consumeExitStatus() {
    if (!this.browserPlayer.consumeBrowserExitStatus) return parseBrowserExitStatus('')
    try {
      const raw = this.browserPlayer.consumeBrowserExitStatus({ workdir: this.workdir() })
      return parseBrowserExitStatus(raw)
    } catch (err) {
      console.warn(`consume browser exit status failed ${err}`)
      return parseBrowserExitStatus('')
    }
  }

  restartForRotation(browserMode) {
    const mode = normalizeBrowserMode(browserMode)
    this.stopWatchdog()
    this.state.startToken += 1
    this.state.starting = false
    this.state.running = false
    this.state.browserModeOverride = mode
    this.state.rotationRestartCount += 1
    if (this.keyboardBridge) {
      this.keyboardBridge.stopPolling()
      this.keyboardBridge.cancelActiveRequest('rotation change')
    }
    console.warn(`wpe rotation restart mode=${mode} count=${this.state.rotationRestartCount}`)
    setTimeout(() => {
      if (!this.state.leaving && this.state.browserModeOverride === mode) this.start()
    }, 100)
  }

  stop() {
    this.stopWatchdog()
    this.state.startToken += 1
    this.state.starting = false
    this.state.running = false
    if (this.keyboardBridge) {
      this.keyboardBridge.stopPolling()
      this.keyboardBridge.cancelActiveRequest('stop browser')
    }
    const workdir = this.workdir()
    if (!workdir) return
    try {
      this.browserPlayer.stopBrowser({ workdir })
    } catch (err) {
      console.warn(`stop browser failed ${err}`)
    }
  }

  leaveFrame(reason) {
    if (this.state.leaving) return
    this.state.leaving = true
    console.warn(`wpe leave frame: ${reason}`)
    const options = this.pageOptions()
    this.stop()
    setTimeout(() => {
      try {
        $falcon.navTo(options.returnPage || 'index', {
          browserStatus: reason || 'stopped',
          browserMode: LAST_BROWSER_MODE,
        })
      } catch (err) {
        console.warn(`nav index failed ${err}`)
      }
    }, 100)
  }
}
