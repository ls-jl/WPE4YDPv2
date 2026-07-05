export class BasePage extends $falcon.Page {
  /**
   * 构造函数,页面生命周期内只执行一次
   */
  constructor() {
    super()
  }

  /**
   * 页面生命周期:首次启动
   * @param Object options 页面启动参数
   */
  onLoad(options) {
    super.onLoad(options)
    this.options = options
  }

  /**
   * 页面生命周期:页面重新进入
   * 其他应用或者系统通过$falcon.navTo()方法重新启动页面.可以通过这个回调拿到新启动的参数
   * @param Object options 重新启动参数
   */
  onNewOptions(options) {
    super.onNewOptions(options)
    this.options = options
    if (this.$root && this.$root.onNewOptions) {
      this.$root.onNewOptions(options)
    }
  }

  /**
   * 页面生命周期:页面进入前台
   */
  onShow() {
    super.onShow()

    //onshow以后组件才创建,可以调用组件的方法
    if (this.$root.onShow) {
      this.$root.onShow()
    }
  }

  /**
   * 页面生命周期:页面进入后台
   */
  onHide() {
    super.onHide()
    if (this.$root.onHide) {
      this.$root.onHide()
    }
  }

  /**
   * 页面生命周期:页面卸载
   */
  onUnload() {
    super.onUnload()
    if (this.$root.onUnload) {
      this.$root.onUnload()
    }
  }

  beforeVueInstantiate(Vue) {
    try {
      Vue.prototype.$workspace = globalThis.$workspace
      Vue.prototype.$appid = globalThis.$appid
    } catch (err) {
      console.log(err)
    }
  }
}
