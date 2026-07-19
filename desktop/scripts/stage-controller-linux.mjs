import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'

if (process.platform !== 'linux') throw new Error('controller staging is Linux-only')

const desktop = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const source = path.resolve(desktop, '..', 'build-desktop', 'ostinato-controller')
const destination = path.join(desktop, 'staged-controller')
const stagedController = path.join(destination, 'ostinato-controller')

function output(command, args) {
  return execFileSync(command, args, { encoding: 'utf8' })
}

function dynamicEntries(executable) {
  const value = output('readelf', ['-d', executable])
  return {
    needed: [...value.matchAll(/\(NEEDED\).*?\[([^\]]+)\]/g)].map((match) => match[1]),
    runpath: value.match(/\((?:RUNPATH|RPATH)\).*?\[([^\]]+)\]/)?.[1] ?? '',
  }
}

function linkedLibraries(executable) {
  const value = output('ldd', [executable])
  if (value.includes('not found')) throw new Error(`unresolved controller dependency:\n${value}`)
  return [...value.matchAll(/^\s*(\S+)\s+=>\s+(\S+)\s+\(/gm)]
    .map((match) => ({ soname: match[1], resolved: match[2] }))
}

if (!fs.existsSync(source)) throw new Error(`controller binary not found: ${source}`)
const sourceDynamic = dynamicEntries(source)
if (!sourceDynamic.runpath.split(':').includes('$ORIGIN')) {
  throw new Error('controller must have a $ORIGIN RUNPATH before staging')
}
const protobufSoname = sourceDynamic.needed.find((name) => /^libprotobuf\.so\./.test(name))
if (!protobufSoname) throw new Error('controller does not declare a protobuf SONAME')

fs.rmSync(destination, { recursive: true, force: true })
fs.mkdirSync(destination, { recursive: true })
fs.copyFileSync(source, stagedController)
fs.chmodSync(stagedController, 0o755)

// protobuf is an application dependency and zlib is its non-glibc runtime
// dependency on the supported Linux build host. glibc, libstdc++, libgcc and
// the ELF loader remain the documented Linux baseline.
const bundled = linkedLibraries(source).filter(({ soname }) =>
  /^libprotobuf\.so\./.test(soname) || /^libz\.so\./.test(soname))
if (!bundled.some(({ soname }) => soname === protobufSoname)) {
  throw new Error(`ldd did not resolve required ${protobufSoname}`)
}
for (const library of bundled) {
  const target = path.join(destination, library.soname)
  fs.copyFileSync(fs.realpathSync(library.resolved), target)
  fs.chmodSync(target, 0o644)
  // RUNPATH is not inherited. Giving each private shared object its own
  // $ORIGIN lookup keeps recursive dependencies inside the staged directory.
  execFileSync('patchelf', ['--set-rpath', '$ORIGIN', target])
}

const stagedLdd = output('ldd', [stagedController])
for (const library of bundled) {
  const expected = fs.realpathSync(path.join(destination, library.soname))
  const resolved = linkedLibraries(stagedController)
    .find(({ soname }) => soname === library.soname)?.resolved
  if (!resolved || fs.realpathSync(resolved) !== expected) {
    throw new Error(`${library.soname} did not resolve from staged controller directory`)
  }
}

console.log(JSON.stringify({
  controller: stagedController,
  runpath: dynamicEntries(stagedController).runpath,
  protobufSoname,
  bundled: bundled.map(({ soname }) => soname),
  ldd: stagedLdd.trim().split('\n').map((line) => line.trim()),
}, null, 2))
