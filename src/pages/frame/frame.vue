<template>
  <div class="frame-page">
    <hole ref="browserHole" class="browser-hole"></hole>
    <textarea
      v-if="keyboard.textareaVisible"
      ref="keyboardTextarea"
      class="keyboard-bridge-textarea"
      :value="keyboard.textareaValue"
      :focus="keyboard.textareaFocused"
      :maxlength="keyboard.textareaMaxlength"
      :inputType="keyboard.textareaInputType"
      :showCursor="true"
      :softInputEnable="true"
      placeholder="请输入内容"
      placeholderColor="#878A99"
      cursorColor="#008CFF"
      :cursorSize="3"
      @input="onKeyboardTextareaInput"
      @confirm="onKeyboardTextareaConfirm"
      @textChanged="onKeyboardTextareaInput"
      @textEditFinished="onKeyboardTextareaFinished"
      @focus="onKeyboardTextareaFocus"
      @blur="onKeyboardTextareaBlur"
    ></textarea>
  </div>
</template>

<script>
import { browserPlayer } from 'browser'
import { BrowserLifecycle, createBrowserLifecycleState } from '../../utils/browser-lifecycle'
import { browserDataPath } from '../../utils/display-resolver'
import { KeyboardBridge, createKeyboardBridgeState } from '../../utils/keyboard-bridge'

export default {
  name: 'frame',
  data() {
    return {
      browser: createBrowserLifecycleState(),
      keyboard: createKeyboardBridgeState(),
      browserLifecycle: null,
      keyboardBridge: null,
      pageVisible: true,
      keyboardHideTimer: null,
    }
  },
  mounted() {
    this.ensureControllers()
    if (this.browserWorkdir()) {
      this.keyboardBridge.setup()
    }
    this.browserLifecycle.start()
  },
  beforeDestroy() {
    this.teardownControllers()
  },
  methods: {
    ensureControllers() {
      if (!this.keyboardBridge) {
        this.keyboardBridge = new KeyboardBridge(
          this,
          browserPlayer,
          () => this.browserWorkdir(),
          () => this.browser.running || this.browser.starting,
        )
      }
      if (!this.browserLifecycle) {
        this.browserLifecycle = new BrowserLifecycle(this, browserPlayer, this.keyboardBridge)
      }
    },
    teardownControllers() {
      this.clearKeyboardHideTimer()
      if (this.browserLifecycle) {
        this.browserLifecycle.stop()
      }
      if (this.keyboardBridge) {
        this.keyboardBridge.teardown()
      }
      if (this.browserLifecycle) {
        this.browserLifecycle.stopWatchdog()
      }
    },
    pageOptions() {
      return this.$page && this.$page.options ? this.$page.options : {}
    },
    browserWorkdir() {
      const options = this.pageOptions()
      return options.workdir || browserDataPath('')
    },
    onKeyboardTextareaInput(value) {
      this.ensureControllers()
      this.keyboardBridge.onTextareaInput(value)
    },
    onKeyboardTextareaConfirm(value) {
      this.ensureControllers()
      this.keyboardBridge.onTextareaConfirm(value)
    },
    onKeyboardTextareaFinished(value) {
      this.ensureControllers()
      this.keyboardBridge.onTextareaFinished(value)
    },
    onKeyboardTextareaFocus() {
      this.ensureControllers()
      this.keyboardBridge.onTextareaFocus()
    },
    onKeyboardTextareaBlur() {
      this.ensureControllers()
      this.keyboardBridge.onTextareaBlur()
    },
    clearKeyboardHideTimer() {
      if (!this.keyboardHideTimer) return
      clearTimeout(this.keyboardHideTimer)
      this.keyboardHideTimer = null
    },
    onKeyboardBridgeInactive() {
      this.clearKeyboardHideTimer()
      this.keyboardHideTimer = setTimeout(() => {
        this.keyboardHideTimer = null
        if (this.pageVisible || this.keyboardBridge.isActive()) return
        console.warn('wpe hidden after keyboard closed; stopping browser')
        this.browserLifecycle.stop()
      }, 15000)
    },
    onShow() {
      this.ensureControllers()
      this.pageVisible = true
      this.clearKeyboardHideTimer()
      this.keyboardBridge.onPageShow()
      this.keyboardBridge.reconcileActiveRequest()
      if (!this.browser.leaving) {
        this.browserLifecycle.start()
      }
    },
    onHide() {
      console.warn('wpe frame onHide')
      this.ensureControllers()
      this.pageVisible = false
      if (this.keyboardBridge.onPageHide()) {
        console.warn('wpe frame onHide ignored while keyboard active')
        return
      }
      this.browserLifecycle.stop()
    },
    onUnload() {
      console.warn('wpe frame onUnload')
      this.ensureControllers()
      this.pageVisible = false
      this.clearKeyboardHideTimer()
      this.teardownControllers()
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

.browser-hole {
  width: 100vw;
  height: 100vh;
}

.keyboard-bridge-textarea {
  position: absolute;
  left: 0;
  top: 0;
  width: 2px;
  height: 2px;
  opacity: 0.01;
  color: transparent;
  background-color: transparent;
  font-size: 1px;
}
</style>
