import { BasePage } from '../../base-page.js'
import FrameComponent from './frame.vue'

class PageFrame extends BasePage {
  onLoad(options) {
    super.onLoad(options)
    this.setRootComponent(FrameComponent)
  }
}

export default PageFrame
