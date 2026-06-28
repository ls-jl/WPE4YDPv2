<template>
  <div class="launcher-page">
    <div class="topbar">
      <text class="title">WPE Browser</text>
      <text class="status">{{ statusText }}</text>
    </div>

    <div class="content">
      <text class="message">{{ messageText }}</text>
      <text v-if="detailText" class="detail">{{ detailText }}</text>
      <text class="primary-button" @click="launchBrowser">{{ busy ? '启动中' : '启动浏览器' }}</text>
      <text class="secondary-button" @click="stopBrowser">停止浏览器</text>
    </div>
  </div>
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

function normalizeUrl(url) {
  const value = `${url || ''}`.trim()
  if (!value) return DEFAULT_URL
  if (value === 'baidu') return DEFAULT_URL
  if (value === 'bing') return 'https://www.bing.com/'
  if (value.indexOf('://') >= 0 || value.indexOf('about:') === 0 || value.indexOf('file:') === 0) return value
  if (/\s/.test(value) || value.indexOf('.') < 0) {
    return `https://m.baidu.com/s?word=${encodeURIComponent(value)}`
  }
  return `https://${value}`
}

export default {
  name: 'index',
  data() {
    return {
      busy: false,
      statusText: 'Ready',
      messageText: '准备启动 Direct WPE 浏览器',
      detailText: '',
      autoLaunchDone: false,
    }
  },
  mounted() {
    this.cleanupBrowser()
    this.scheduleAutoLaunch()
  },
  methods: {
    scheduleAutoLaunch() {
      if (this.autoLaunchDone) return
      this.autoLaunchDone = true
      setTimeout(() => this.launchBrowser(), 80)
    },
    readInitialUrl() {
      const page = this.$page || {}
      const app = typeof $falcon !== 'undefined' && $falcon.$app
      const candidates = [
        page.loadOptions,
        page.newOptions,
        page.options,
        app && app.launchOptions,
      ]
      for (let i = 0; i < candidates.length; i += 1) {
        const options = candidates[i] || {}
        const raw = options.url || options.href || options.u
        if (raw) {
          try {
            return normalizeUrl(decodeURIComponent(`${raw}`))
          } catch (err) {
            return normalizeUrl(raw)
          }
        }
      }
      return DEFAULT_URL
    },
    cleanupBrowser() {
      try {
        if (browserPlayer && browserPlayer.stopBrowser) {
          browserPlayer.stopBrowser({ workdir: browserDataPath('') })
        }
      } catch (err) {
        console.warn(`cleanup browser failed ${err}`)
      }
    },
    stopBrowser() {
      this.cleanupBrowser()
      this.statusText = 'Stopped'
      this.messageText = '浏览器已停止'
      this.detailText = ''
    },
    launchBrowser() {
      if (this.busy) return
      this.busy = true
      this.statusText = 'Preparing'
      this.messageText = '正在准备浏览器运行时'
      this.detailText = ''

      setTimeout(() => {
        try {
          const workspace = workspacePath(this)
          const dataDir = dataRootPath()
          const runtimePath = browserPlayer.prepareRuntime({
            workspace,
            dataDir,
          })
          const workdir = browserDataPath('')
          const logPath = browserDataPath('wpe-drm.log')
          this.statusText = 'Starting'
          this.messageText = '正在进入浏览器'
          $falcon.navTo('frame', {
            runtimePath,
            workdir,
            logPath,
            url: this.readInitialUrl(),
            viewport: DEFAULT_VIEWPORT,
            rotation: DEFAULT_ROTATION,
          })
        } catch (err) {
          const message = err && err.message ? err.message : `${err}`
          this.statusText = 'Error'
          this.messageText = '浏览器启动失败'
          this.detailText = message
          console.warn(`launch browser failed ${message}`)
        } finally {
          this.busy = false
        }
      }, 0)
    },
  },
}
</script>

<style lang="less" scoped>
.launcher-page {
  width: 100vw;
  height: 100vh;
  background-color: #0c1014;
  color: #f5f7fa;
}

.topbar {
  height: 48px;
  padding-left: 18px;
  padding-right: 18px;
  flex-direction: row;
  align-items: center;
  justify-content: space-between;
  background-color: #151b22;
}

.title {
  font-size: 20px;
  color: #ffffff;
}

.status {
  font-size: 15px;
  color: #8fd0ff;
}

.content {
  padding-left: 28px;
  padding-right: 28px;
  padding-top: 28px;
}

.message {
  font-size: 22px;
  color: #ffffff;
}

.detail {
  margin-top: 14px;
  font-size: 15px;
  color: #ffb4a8;
}

.primary-button,
.secondary-button {
  margin-top: 22px;
  width: 180px;
  height: 42px;
  line-height: 42px;
  text-align: center;
  border-radius: 6px;
  font-size: 17px;
}

.primary-button {
  color: #081018;
  background-color: #79d66b;
}

.secondary-button {
  color: #d9e6f2;
  background-color: #26323d;
}
</style>
