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
            <text :class="selectedMode === 'native' ? 'mode-option mode-option-active' : 'mode-option'" @click="setMode('native')">原生模式</text>
            <text :class="selectedMode === 'rotate270' ? 'mode-option mode-option-active' : 'mode-option'" @click="setMode('rotate270')">横屏旋转</text>
          </div>
        </div>

        <textarea
          ref="keyboardProbeField"
          class="keyboard-probe-field"
          :value="keyboardProbeText"
          :focus="keyboardProbeFocused"
          placeholder="键盘测试"
          placeholderColor="#878A99"
          cursorColor="#FF683D"
          maxlength="128"
          @textEditFinished="onKeyboardProbeTextEditFinished"
          @textChanged="onKeyboardProbeTextChanged"
          @focus="onKeyboardProbeFocus"
          @blur="onKeyboardProbeBlur"
        ></textarea>

        <div class="button-row">
          <text class="primary-button" @click="launchBrowser()">{{ busy ? '启动中' : '启动浏览器' }}</text>
          <text class="secondary-button" @click="openKeyboardProbe()">键盘测试</text>
        </div>
      </div>
    </div>
  </div>
</template>

<script>
import globalModule from 'global'

const DEFAULT_URL = 'https://m.baidu.com/'
const DEFAULT_BROWSER_MODE = 'native'
const BROWSER_MODES = ['native', 'rotate270']

let keyboardProbeGlobal = null

function getKeyboardProbeGlobal() {
  if (!keyboardProbeGlobal) {
    keyboardProbeGlobal = new globalModule.Global()
  }
  return keyboardProbeGlobal
}

function normalizeBrowserMode(value) {
  const mode = `${value || ''}`
  return BROWSER_MODES.indexOf(mode) >= 0 ? mode : DEFAULT_BROWSER_MODE
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
      selectedMode: DEFAULT_BROWSER_MODE,
      keyboardProbeUuid: '',
      keyboardProbeHandler: null,
      keyboardProbeTimer: null,
      keyboardProbeFocused: false,
      keyboardProbeText: '',
    }
  },
  mounted() {
    this.applyPageOptions()
    this.bindKeyboardProbe()
  },
  beforeDestroy() {
    if (this.keyboardProbeTimer) {
      clearTimeout(this.keyboardProbeTimer)
      this.keyboardProbeTimer = null
    }
    this.unbindKeyboardProbe()
  },
  methods: {
    pageOptions() {
      return this.$page && this.$page.options ? this.$page.options : {}
    },
    applyPageOptions() {
      const options = this.pageOptions()
      this.selectedMode = normalizeBrowserMode(options.browserMode || this.selectedMode)
      if (options.browserStatus) {
        this.statusText = 'Ready'
        this.messageText = '准备启动 Direct WPE 浏览器'
        this.detailText = `${options.browserStatus}`
      }
    },
    setMode(mode) {
      this.selectedMode = normalizeBrowserMode(mode)
      this.detailText = ''
    },
    bindKeyboardProbe() {
      try {
        const gm = getKeyboardProbeGlobal()
        this.keyboardProbeHandler = (uuid, jsonData) => {
          if (`${uuid || ''}` !== `${this.keyboardProbeUuid || ''}`) return
          let result = null
          try {
            result = JSON.parse(jsonData || '{}')
          } catch (err) {
            this.detailText = `键盘回调解析失败 ${err}`
            return
          }
          if (result && result.editConfirmed) {
            this.detailText = `键盘返回: ${result.text || ''}`
            this.keyboardProbeUuid = ''
          }
        }
        if (gm.textEditFinished) {
          gm.textEditFinished.on(this.keyboardProbeHandler)
          console.warn('index keyboard probe listener mounted')
        }
      } catch (err) {
        this.detailText = `键盘监听失败 ${err}`
      }
    },
    unbindKeyboardProbe() {
      try {
        const gm = getKeyboardProbeGlobal()
        if (gm.textEditFinished && this.keyboardProbeHandler) {
          gm.textEditFinished.off(this.keyboardProbeHandler)
        }
      } catch (err) {
        console.warn(`index keyboard probe unbind failed ${err}`)
      }
      this.keyboardProbeHandler = null
      this.keyboardProbeUuid = ''
    },
    openKeyboardProbe() {
      this.keyboardProbeFocused = true
      this.detailText = 'textarea focus 键盘请求'
      console.warn('index keyboard textarea focus request')
      return
    },
    onKeyboardProbeTextEditFinished(result) {
      const text = this.parseKeyboardProbeResult(result)
      this.keyboardProbeText = text
      this.keyboardProbeFocused = false
      this.detailText = `textarea 键盘返回: ${text || '空'}`
      console.warn(`index keyboard textarea finished ${text || ''}`)
    },
    onKeyboardProbeTextChanged(result) {
      this.keyboardProbeText = this.parseKeyboardProbeResult(result)
      console.warn(`index keyboard textarea changed ${this.keyboardProbeText || ''}`)
    },
    onKeyboardProbeFocus() {
      console.warn('index keyboard textarea focused')
    },
    onKeyboardProbeBlur() {
      this.keyboardProbeFocused = false
      console.warn('index keyboard textarea blurred')
    },
    parseKeyboardProbeResult(result) {
      if (!result) return ''
      if (typeof result === 'string') {
        try {
          const parsed = JSON.parse(result)
          return parsed.text || (parsed.records && parsed.records[0] && parsed.records[0].text) || parsed.contents || result
        } catch (err) {
          return result
        }
      }
      if (typeof result === 'object') {
        return result.text || (result.records && result.records[0] && result.records[0].text) || result.contents || ''
      }
      return `${result}`
    },
    openKeyboardProbeViaGlobal() {
      try {
        const request = {
          uuid: `index_keyboard_probe_${Date.now()}`,
          contents: '',
          text: '',
          placeholder: '键盘测试',
          placeholderColor: '#878A99',
          autofocus: true,
          maxlength: 128,
          showCursor: true,
          cursorIndex: 0,
          cursorColor: '#FF683D',
          cursorSize: 3,
          confirmButtonDisabledOnTextEmpty: false,
          inputType: 'ZhCNPreferred',
          keyboardType: 'normal',
          multiLinesEditVisible: false,
          capsLockSwitchOn: false,
          micInputVisible: false,
          isNetworkConnected: true,
          enterButtonText: '确认',
        }
        const falcon = typeof $falcon !== 'undefined' ? $falcon : null
        if (falcon && falcon.trigger) {
          this.keyboardProbeUuid = request.uuid
          falcon.trigger('requestIMAppShow', request)
          this.detailText = `falcon 键盘请求 ${this.keyboardProbeUuid}`
          console.warn(`index keyboard falcon request uuid ${this.keyboardProbeUuid}`)
          return
        }
        const gm = getKeyboardProbeGlobal()
        if (!gm.startTextEdit) {
          this.detailText = 'startTextEdit 不存在'
          return
        }
        this.keyboardProbeUuid = gm.startTextEdit(JSON.stringify({
          text: '',
          placeholder: '键盘测试',
          placeholderColor: '#878A99',
          autofocus: true,
          maxlength: 128,
          showCursor: true,
          cursorColor: '#FF683D',
          cursorSize: 3,
          confirmButtonDisabledOnTextEmpty: false,
          inputType: 'ZhCNPreferred',
          multiLinesEditVisible: false,
          capsLockSwitchOn: false,
          enterButtonText: '确认',
        })) || ''
        this.detailText = this.keyboardProbeUuid ? `键盘请求 ${this.keyboardProbeUuid}` : '键盘请求失败'
        console.warn(`index keyboard probe uuid ${this.keyboardProbeUuid}`)
      } catch (err) {
        this.detailText = `键盘启动失败 ${err}`
        console.warn(`index keyboard probe failed ${err}`)
      }
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
  margin-right: 8px;
  width: 96px;
  height: 30px;
  line-height: 30px;
  text-align: center;
  border-radius: 6px;
  font-size: 14px;
  color: #d9e6f2;
  background-color: #26323d;
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

.secondary-button {
  margin-top: 10px;
  margin-right: 8px;
  width: 112px;
  height: 36px;
  line-height: 36px;
  text-align: center;
  border-radius: 6px;
  font-size: 15px;
  color: #d9e6f2;
  background-color: #26323d;
}

.keyboard-probe-field {
  margin-top: 4px;
  width: 260px;
  height: 42px;
  padding: 4px 8px;
  border-radius: 4px;
  opacity: 1;
  color: #ffffff;
  background-color: #18212a;
  font-size: 18px;
}

</style>
