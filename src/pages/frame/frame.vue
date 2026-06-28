<template>
  <div class="frame-page"></div>
</template>

<script>
import { browserPlayer } from 'browser'

const DEFAULT_URL = 'https://m.baidu.com/'
const DEFAULT_VIEWPORT = '960x266'
const DEFAULT_ROTATION = 270

function dataRootPath() {
  if (typeof $dataDir === 'string' && $dataDir) return $dataDir
  if (typeof $falcon !== 'undefined' && $falcon && typeof $falcon.$dataDir === 'string' && $falcon.$dataDir) {
    return $falcon.$dataDir
  }
  try {
    if (typeof globalThis !== 'undefined' && typeof globalThis.$dataDir === 'string' && globalThis.$dataDir) {
      return globalThis.$dataDir
    }
  } catch (err) {
    console.warn(`read data dir failed ${err}`)
  }
  return '/tmp/wpe-browser-data'
}

function workspacePath(page) {
  if (page && typeof page.$workspace === 'string' && page.$workspace) return page.$workspace
  try {
    if (typeof globalThis !== 'undefined' && typeof globalThis.$workspace === 'string' && globalThis.$workspace) {
      return globalThis.$workspace
    }
  } catch (err) {
    console.warn(`read workspace failed ${err}`)
  }
  if (typeof $falcon !== 'undefined' && $falcon && typeof $falcon.$workspace === 'string' && $falcon.$workspace) {
    return $falcon.$workspace
  }
  if (typeof $workspace === 'string' && $workspace) return $workspace
  const dataRoot = dataRootPath()
  if (dataRoot.slice(-5) === '/data') return dataRoot.slice(0, -5) + '/a'
  return ''
}

function browserDataPath(name) {
  return dataRootPath() + '/browser/' + name
}

export default {
  name: 'frame',
  data() {
    return {
      browserRunning: false,
      browserStarting: false,
      watchdogTimer: null,
      leavingFrame: false,
      startToken: 0,
    }
  },
  mounted() {
    this.startBrowser()
  },
  methods: {
    browserWorkdir() {
      const options = this.$page && this.$page.options ? this.$page.options : {}
      return options.workdir || browserDataPath('')
    },
    browserOptions() {
      const options = this.$page && this.$page.options ? this.$page.options : {}
      let runtimePath = options.runtimePath || ''
      if (!runtimePath) {
        runtimePath = browserPlayer.prepareRuntime({
          workspace: workspacePath(this),
          dataDir: dataRootPath(),
        })
      }
      return {
        runtimePath,
        workdir: this.browserWorkdir(),
        logPath: options.logPath || browserDataPath('wpe-drm.log'),
        url: options.url || DEFAULT_URL,
        viewport: options.viewport || DEFAULT_VIEWPORT,
        rotation: Number(options.rotation || DEFAULT_ROTATION),
        drm: options.drm || '/dev/dri/card0',
      }
    },
    startBrowser() {
      if (this.browserStarting || this.leavingFrame) return
      try {
        if (browserPlayer.isBrowserRunning && browserPlayer.isBrowserRunning({ workdir: this.browserWorkdir() })) {
          this.browserRunning = true
          this.startWatchdog()
          return
        }
      } catch (err) {
        console.warn(`check browser running failed ${err}`)
      }

      this.browserStarting = true
      const token = this.startToken + 1
      this.startToken = token
      setTimeout(() => {
        if (token !== this.startToken || this.leavingFrame) return
        try {
          const pid = browserPlayer.startBrowser(this.browserOptions())
          this.browserRunning = Number(pid) > 0
          console.warn(`wpe browser pid ${pid}`)
          if (this.browserRunning) {
            this.startWatchdog()
          } else {
            this.leaveFrame('start failed')
          }
        } catch (err) {
          console.warn(`start browser failed ${err}`)
          this.browserRunning = false
          this.leaveFrame(err && err.message ? err.message : 'start exception')
        } finally {
          this.browserStarting = false
        }
      }, 0)
    },
    startWatchdog() {
      this.stopWatchdog()
      this.watchdogTimer = setInterval(() => {
        if (this.browserStarting || this.leavingFrame) return
        let running = false
        try {
          running = browserPlayer.isBrowserRunning({ workdir: this.browserWorkdir() })
        } catch (err) {
          console.warn(`watch browser failed ${err}`)
        }
        if (running) {
          this.browserRunning = true
          return
        }
        this.browserRunning = false
        this.leaveFrame('browser exited')
      }, 1000)
    },
    stopWatchdog() {
      if (this.watchdogTimer) {
        clearInterval(this.watchdogTimer)
        this.watchdogTimer = null
      }
    },
    stopBrowser() {
      this.stopWatchdog()
      this.startToken += 1
      this.browserStarting = false
      this.browserRunning = false
      try {
        browserPlayer.stopBrowser({ workdir: this.browserWorkdir() })
      } catch (err) {
        console.warn(`stop browser failed ${err}`)
      }
    },
    leaveFrame(reason) {
      if (this.leavingFrame) return
      this.leavingFrame = true
      console.warn(`wpe leave frame: ${reason}`)
      this.stopBrowser()
      setTimeout(() => {
        try {
          $falcon.navTo('index', { browserStatus: reason || 'stopped' })
        } catch (err) {
          console.warn(`nav index failed ${err}`)
        }
      }, 100)
    },
    onShow() {
      if (!this.leavingFrame) {
        this.startBrowser()
      }
    },
    onHide() {
      console.warn('wpe frame onHide')
      this.stopBrowser()
    },
    onUnload() {
      console.warn('wpe frame onUnload')
      this.stopBrowser()
    },
  },
}
</script>

<style lang="less" scoped>
.frame-page {
  width: 100vw;
  height: 100vh;
  background-color: #000000;
}
</style>
