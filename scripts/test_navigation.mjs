import assert from 'node:assert/strict'
import fs from 'node:fs'

const source = fs.readFileSync('src/utils/navigation.js', 'utf8')
const moduleUrl = `data:text/javascript;base64,${Buffer.from(source).toString('base64')}`
const { DEFAULT_BROWSER_URL, normalizeLaunchUrl } = await import(moduleUrl)

assert.equal(normalizeLaunchUrl(''), DEFAULT_BROWSER_URL)
assert.equal(normalizeLaunchUrl('baidu'), DEFAULT_BROWSER_URL)
assert.equal(normalizeLaunchUrl('bing'), 'https://www.bing.com/')
assert.equal(normalizeLaunchUrl('example.com/path'), 'https://example.com/path')
assert.equal(normalizeLaunchUrl('测试 搜索'), 'https://m.baidu.com/s?word=%E6%B5%8B%E8%AF%95%20%E6%90%9C%E7%B4%A2')
assert.equal(normalizeLaunchUrl('https://example.com/'), 'https://example.com/')
assert.equal(normalizeLaunchUrl('about:blank'), 'about:blank')
assert.equal(normalizeLaunchUrl('file:///tmp/test.html'), 'file:///tmp/test.html')
assert.equal(normalizeLaunchUrl('baiduboxapp://search'), DEFAULT_BROWSER_URL)
assert.equal(normalizeLaunchUrl('intent://game'), DEFAULT_BROWSER_URL)
assert.equal(normalizeLaunchUrl('javascript:alert(1)'), DEFAULT_BROWSER_URL)
assert.equal(normalizeLaunchUrl('bad://scheme'), DEFAULT_BROWSER_URL)

console.log('launcher navigation fixtures passed: 12')
