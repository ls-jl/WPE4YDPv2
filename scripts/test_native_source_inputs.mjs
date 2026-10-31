import fs from 'node:fs'
import path from 'node:path'
import os from 'node:os'
import assert from 'node:assert/strict'
import { captureNativeSourceInputs } from './native_source_inputs.mjs'

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-build-inputs-'))
try {
  fs.cpSync('scripts', path.join(directory, 'scripts'), { recursive: true })
  fs.mkdirSync(path.join(directory, 'wpe-drm'))
  const source = path.join(directory, 'wpe-drm/source.c')
  fs.writeFileSync(source, 'int value = 1;\n')
  const before = captureNativeSourceInputs(directory)
  fs.writeFileSync(path.join(directory, 'wpe-drm/notes.md'), 'Build notes\n')
  assert.deepEqual(captureNativeSourceInputs(directory), before)
  fs.writeFileSync(source, 'int value = 2;\n')
  assert.notDeepEqual(captureNativeSourceInputs(directory), before)
  fs.writeFileSync(source, 'int value = 1;\n')
  fs.writeFileSync(path.join(directory, 'wpe-drm/new-header.h'), '#pragma once\n')
  assert.notDeepEqual(captureNativeSourceInputs(directory), before)
  console.log('Native build input tests passed: changed code, added headers and documentation exclusion')
} finally {
  fs.rmSync(directory, { recursive: true, force: true })
}
