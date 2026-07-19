import fs from 'node:fs'

fs.writeFileSync(new URL('../dist-electron/package.json', import.meta.url), '{"type":"commonjs"}\n')
