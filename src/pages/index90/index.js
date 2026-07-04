import { BasePage } from '../../base-page.js'
import IndexComponent from '../index/index.vue'

class PageIndexAlias extends BasePage {
  onLoad(options) {
    super.onLoad(Object.assign({}, options || {}, { miniappRotation: 90 }))
    this.setRootComponent(IndexComponent)
  }
}

export default PageIndexAlias
