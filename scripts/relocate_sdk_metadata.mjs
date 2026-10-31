import fs from 'node:fs'
import path from 'node:path'

const root = fs.realpathSync(process.argv[2])
if (root === '/' || !fs.existsSync(path.join(root, '.wpe-sdk-owned')) ||
    fs.readFileSync(path.join(root, '.wpe-sdk-owned'), 'utf8').trim() !== 'wpe-sdk-sysroot-v1') {
  throw new Error(`Refusing non-SDK root: ${root}`)
}
let count = 0
function visit(directory) {
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const file = path.join(directory, entry.name)
    if (entry.isDirectory()) visit(file)
    else if (entry.isFile() && entry.name.endsWith('.pc')) {
      const original = fs.readFileSync(file, 'utf8')
      const updated = original.replace(/(^|[=:\s]|-[IL])\/(usr|lib)(?=\/|\s|$)/gm, `$1${root}/$2`)
      fs.writeFileSync(file, updated)
      count++
    } else if (entry.isSymbolicLink()) {
      const target = fs.readlinkSync(file)
      if (target.startsWith('/')) {
        const relocated = path.join(root, target)
        if (!relocated.startsWith(`${root}/`)) throw new Error(`Unsafe SDK link: ${file}`)
        const temporary = `${file}.relocating-${process.pid}`
        fs.symlinkSync(path.relative(path.dirname(file), relocated), temporary)
        fs.renameSync(temporary, file)
      }
    }
  }
}
visit(root)
console.log(`Relocated ${count} target pkg-config files and rooted absolute symlinks`)
