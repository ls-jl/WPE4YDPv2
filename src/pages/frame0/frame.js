import { BasePage } from '../../base-page.js'
import FrameComponent from '../frame/frame.vue'

class PageFrameAlias extends BasePage {
  onLoad(options) {
    super.onLoad(Object.assign({}, options || {}, { miniappRotation: 0 }))
    this.setRootComponent(FrameComponent)
  }
}

export default PageFrameAlias
