import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import assert from 'node:assert/strict'
import { execFileSync, spawnSync } from 'node:child_process'

const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'wpe-frame-integrity-'))
try {
  for (const rotation of [0, 90, 180, 270]) {
    for (const inset of [0, 44, 80]) {
      const panelWidth = 266
      const panelHeight = 960
      const width = rotation === 90 || rotation === 270 ? panelHeight : panelWidth
      const height = rotation === 90 || rotation === 270 ? panelWidth : panelHeight
      const pixels = Buffer.alloc(width * height * 3, 100)
      const viewHeight = panelHeight - inset
      function draw(y, sequence) {
        const bits = [1, 0, ...Array.from({ length: 16 }, (_, i) => sequence >> (15 - i) & 1), 0, 1]
        for (let x = 0; x < panelWidth; x++) {
          let outputX = x
          let outputY = y + inset
          if (rotation === 90) { outputX = panelHeight - 1 - y - inset; outputY = x }
          if (rotation === 180) { outputX = panelWidth - 1 - x; outputY = panelHeight - 1 - y - inset }
          if (rotation === 270) { outputX = y + inset; outputY = panelWidth - 1 - x }
          const bit = bits[Math.min(19, Math.floor(x * 20 / panelWidth))]
          pixels.fill(bit ? 255 : 0, (outputY * width + outputX) * 3, (outputY * width + outputX) * 3 + 3)
        }
      }
      const ys = [12, Math.floor(viewHeight / 2) + 4, viewHeight - 12]
      const file = path.join(directory, 'capture.ppm')
      const save = () => fs.writeFileSync(file, Buffer.concat([Buffer.from(`P6\n${width} ${height}\n255\n`), pixels]))
      ys.forEach(y => draw(y, 12345))
      save()
      execFileSync(process.execPath, ['tools/diagnostics/frame_integrity_check.mjs', file, `${rotation}`, `${inset}`])
      draw(ys[1], 12346)
      save()
      const result = spawnSync(process.execPath, ['tools/diagnostics/frame_integrity_check.mjs', file, `${rotation}`, `${inset}`], { encoding: 'utf8' })
      assert.notEqual(result.status, 0)
      assert.match(result.stderr, /mixed frame/)
    }
  }
  console.log('frame integrity diagnostics: four rotations, insets and mixed frames passed')
} finally {
  fs.rmSync(directory, { recursive: true, force: true })
}
