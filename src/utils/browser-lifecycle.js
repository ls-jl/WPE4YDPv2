import { browserDataPath, browserOptions, DEFAULT_BROWSER_MODE } from './display-resolver'

export function createBrowserLifecycleState() {
  return {
    running: false,
    starting: false,
    leaving: false,
    startToken: 0,
    watchdogTimer: null,
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
    return this.component.pageOptions()
  }

  workdir() {
    const options = this.pageOptions()
    return options.workdir || browserDataPath('')
  }

  async resolveOptions() {
    return browserOptions(this.component, this.browserPlayer, this.pageOptions())
  }

  start() {
    if (this.state.starting || this.state.leaving) return
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
    this.leaveFrame('browser exited')
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
    try {
      this.browserPlayer.stopBrowser({ workdir: this.workdir() })
    } catch (err) {
      console.warn(`stop browser failed ${err}`)
    }
  }

  stopOnlyAndReturn() {
    const options = this.pageOptions()
    this.state.leaving = true
    console.warn('wpe stop-only frame')
    let status = '浏览器已停止'
    try {
      this.stop()
    } catch (err) {
      status = err && err.message ? err.message : `${err}`
      console.warn(`stop-only failed ${status}`)
    }
    setTimeout(() => {
      try {
        $falcon.navTo(options.returnPage || 'index', {
          browserStatus: status,
          browserMode: options.browserMode || DEFAULT_BROWSER_MODE,
        })
      } catch (err) {
        console.warn(`stop-only nav index failed ${err}`)
      }
    }, 100)
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
          browserMode: options.browserMode || DEFAULT_BROWSER_MODE,
        })
      } catch (err) {
        console.warn(`nav index failed ${err}`)
      }
    }, 100)
  }
}
