<template>
  <div class="launcher-page">
    <div class="launcher-shell">
      <div class="topbar">
        <text class="title">WPE Browser</text>
        <text class="status">{{ statusText }}</text>
      </div>

      <div class="content">
        <text class="message">{{ messageText }}</text>
        <text v-if="detailText" class="detail">{{ detailText }}</text>

        <div class="mode-block">
          <text class="section-label">显示模式</text>
          <div class="mode-row">
            <text :class="selectedMode === 'last' ? 'mode-option mode-option-active mode-option-last' : 'mode-option mode-option-last'" @click="setMode('last')">沿用上次</text>
            <text :class="selectedMode === 'native' ? 'mode-option mode-option-active' : 'mode-option'" @click="setMode('native')">原生</text>
            <text :class="selectedMode === 'rotate90' ? 'mode-option mode-option-active' : 'mode-option'" @click="setMode('rotate90')">90°</text>
            <text :class="selectedMode === 'rotate180' ? 'mode-option mode-option-active' : 'mode-option'" @click="setMode('rotate180')">180°</text>
            <text :class="selectedMode === 'rotate270' ? 'mode-option mode-option-active' : 'mode-option'" @click="setMode('rotate270')">270°</text>
          </div>
        </div>

        <div class="button-row">
          <text class="primary-button" @click="launchBrowser()">{{ busy ? '启动中' : '启动浏览器' }}</text>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import { DEFAULT_BROWSER_URL, normalizeLaunchUrl } from '../../utils/navigation'
import { normalizeBrowserLaunchMode } from '../../utils/display-resolver'

const DEFAULT_BROWSER_MODE = 'last'

export default {
  name: 'index',
  data() {
    return {
      busy: false,
      statusText: 'Ready',
      messageText: '准备启动 Direct WPE 浏览器',
      detailText: '',
      selectedMode: DEFAULT_BROWSER_MODE,
    }
  },
  mounted() {
    this.applyPageOptions(this.pageOptions(), true)
  },
  onShow() {
    this.applyPageOptions(this.pageOptions(), true)
  },
  onNewOptions(options) {
    this.applyPageOptions(options || {}, true)
  },
  methods: {
    pageOptions() {
      return this.$page && this.$page.options ? this.$page.options : {}
    },
    applyPageOptions(options, resetTransient) {
      const pageOptions = options || {}
      const wasLaunching = this.busy || this.messageText === '正在进入浏览器'
      this.selectedMode = normalizeBrowserLaunchMode(pageOptions.browserMode || this.selectedMode)
      if (resetTransient || this.busy) {
        this.busy = false
        this.statusText = 'Ready'
        this.messageText = '准备启动 Direct WPE 浏览器'
      }
      if (pageOptions.browserStatus) {
        this.detailText = `${pageOptions.browserStatus}`
      } else if (resetTransient && wasLaunching) {
        this.detailText = ''
      }
    },
    setMode(mode) {
      this.selectedMode = normalizeBrowserLaunchMode(mode)
      this.detailText = ''
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
            return normalizeLaunchUrl(decodeURIComponent(`${raw}`))
          } catch (err) {
            return normalizeLaunchUrl(raw)
          }
        }
      }
      return DEFAULT_BROWSER_URL
    },
    launchBrowser() {
      if (this.busy) return
      this.busy = true
      this.statusText = 'Starting'
      this.messageText = '正在进入浏览器'
      this.detailText = ''

      try {
        $falcon.navTo('frame', {
          url: this.readInitialUrl(),
          browserMode: this.selectedMode,
          returnPage: 'index',
        })
      } catch (err) {
        const message = err && err.message ? err.message : `${err}`
        this.statusText = 'Error'
        this.messageText = '浏览器启动失败'
        this.detailText = message
        console.warn(`launch browser nav failed ${message}`)
        this.busy = false
      }
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
  align-items: center;
  justify-content: center;
  overflow: hidden;
}

.launcher-shell {
  background-color: #0c1014;
  color: #f5f7fa;
  width: 100vw;
  height: 100vh;
}

.topbar {
  height: 38px;
  padding-left: 12px;
  padding-right: 12px;
  flex-direction: row;
  align-items: center;
  justify-content: space-between;
  background-color: #151b22;
}

.title {
  font-size: 18px;
  color: #ffffff;
}

.status {
  font-size: 14px;
  color: #8fd0ff;
}

.content {
  padding-left: 10px;
  padding-right: 10px;
  padding-top: 8px;
}

.message {
  font-size: 18px;
  color: #ffffff;
}

.detail {
  margin-top: 8px;
  font-size: 15px;
  color: #ffb4a8;
}

.mode-block {
  margin-top: 8px;
}

.section-label {
  font-size: 13px;
  color: #9fb1c0;
}

.mode-row,
.button-row {
  flex-direction: row;
  align-items: center;
}

.mode-row {
  margin-top: 5px;
}

.mode-option {
  margin-right: 6px;
  width: 60px;
  height: 30px;
  line-height: 30px;
  text-align: center;
  border-radius: 6px;
  font-size: 14px;
  color: #d9e6f2;
  background-color: #26323d;
}

.mode-option-last {
  width: 88px;
}

.mode-option-active {
  color: #081018;
  background-color: #8fd0ff;
}

.primary-button {
  margin-top: 10px;
  margin-right: 8px;
  width: 112px;
  height: 36px;
  line-height: 36px;
  text-align: center;
  border-radius: 6px;
  font-size: 15px;
}

.primary-button {
  color: #081018;
  background-color: #79d66b;
}

</style>
