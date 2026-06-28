import { BasePage } from '../../base-page.js'
import IndexComponent from './index.vue'

class PageIndex extends BasePage {
  onLoad(options) {
    super.onLoad(options)
    this.setRootComponent(IndexComponent)
  }
}

export default PageIndex
