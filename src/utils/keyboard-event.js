const MAX_EVENT_DEPTH = 5

function hasOwn(value, key) {
  return !!(value && Object.prototype.hasOwnProperty.call(value, key))
}

function decodeKeyboardValue(value, depth) {
  if (depth > MAX_EVENT_DEPTH) return { found: false, text: '' }
  if (typeof value === 'string') {
    const trimmed = value.trim()
    if (trimmed.charAt(0) === '{' || trimmed.charAt(0) === '[') {
      try {
        const decoded = decodeKeyboardValue(JSON.parse(value), depth + 1)
        if (decoded.found) return decoded
      } catch (err) {
        // A normal input string may begin with a brace.
      }
    }
    return { found: true, text: value }
  }
  if (typeof value === 'number' || typeof value === 'boolean') {
    return { found: true, text: `${value}` }
  }
  if (!value || typeof value !== 'object') return { found: false, text: '' }
  if (Array.isArray(value)) {
    for (let i = 0; i < value.length; i += 1) {
      const result = decodeKeyboardValue(value[i], depth + 1)
      if (result.found) return result
    }
    return { found: false, text: '' }
  }

  const directKeys = ['value', 'text', 'contents']
  for (let i = 0; i < directKeys.length; i += 1) {
    const key = directKeys[i]
    if (!hasOwn(value, key)) continue
    const result = decodeKeyboardValue(value[key], depth + 1)
    if (result.found) return result
  }

  const wrappers = ['detail', 'target', 'currentTarget']
  for (let i = 0; i < wrappers.length; i += 1) {
    const key = wrappers[i]
    if (!hasOwn(value, key)) continue
    const result = decodeKeyboardValue(value[key], depth + 1)
    if (result.found) return result
  }

  if (Array.isArray(value.records)) {
    for (let i = 0; i < value.records.length; i += 1) {
      const result = decodeKeyboardValue(value.records[i], depth + 1)
      if (result.found) return result
    }
  }
  return { found: false, text: '' }
}

export function normalizeKeyboardEvent(value) {
  return decodeKeyboardValue(value, 0)
}

function decodeTextEditResult(value, depth) {
  if (depth > MAX_EVENT_DEPTH) return null
  let payload = value
  if (typeof payload === 'string') {
    try {
      payload = JSON.parse(payload || '{}')
    } catch (err) {
      return null
    }
  }
  if (!payload || typeof payload !== 'object') return null
  if (hasOwn(payload, 'editConfirmed')) {
    const text = normalizeKeyboardEvent(payload)
    const confirmed = payload.editConfirmed === true ||
      payload.editConfirmed === 1 ||
      `${payload.editConfirmed}`.toLowerCase() === 'true'
    return {
      terminal: true,
      confirmed,
      found: text.found,
      text: text.text,
    }
  }
  const wrappers = Array.isArray(payload)
    ? payload
    : ['detail', 'target', 'currentTarget', 'value', 'records']
  for (let i = 0; i < wrappers.length; i += 1) {
    const nested = Array.isArray(payload) ? wrappers[i] : payload[wrappers[i]]
    if (!Array.isArray(payload) && !hasOwn(payload, wrappers[i])) continue
    const result = decodeTextEditResult(nested, depth + 1)
    if (result) return result
  }
  return null
}

export function parseTextEditResult(value) {
  const result = decodeTextEditResult(value, 0)
  if (result) return result
  const text = normalizeKeyboardEvent(value)
  return {
    terminal: false,
    confirmed: false,
    found: text.found,
    text: text.text,
  }
}
