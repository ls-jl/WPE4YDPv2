export const DEFAULT_BROWSER_URL = 'https://m.baidu.com/'

const ALLOWED_SCHEMES = ['http', 'https', 'about', 'file']

export function normalizeLaunchUrl(url) {
  const value = `${url || ''}`.trim()
  if (!value) return DEFAULT_BROWSER_URL
  if (value === 'baidu') return DEFAULT_BROWSER_URL
  if (value === 'bing') return 'https://www.bing.com/'

  const schemeMatch = value.match(/^([a-z][a-z0-9+.-]*):/i)
  if (schemeMatch) {
    const scheme = schemeMatch[1].toLowerCase()
    return ALLOWED_SCHEMES.indexOf(scheme) >= 0 ? value : DEFAULT_BROWSER_URL
  }
  if (value.indexOf('://') >= 0) return DEFAULT_BROWSER_URL
  if (/\s/.test(value) || value.indexOf('.') < 0) {
    return `https://m.baidu.com/s?word=${encodeURIComponent(value)}`
  }
  return `https://${value}`
}
