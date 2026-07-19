import { execFileSync, spawn } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'
import { _electron as electron } from '@playwright/test'

if (process.platform !== 'linux') throw new Error('package verification is Linux-only')

const desktop = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const unpacked = path.join(desktop, 'release', 'linux-unpacked')
const packagedControllerDirectory = path.join(unpacked, 'resources', 'controller')
const packagedController = path.join(packagedControllerDirectory, 'ostinato-controller')
const packagedExecutable = path.join(unpacked, 'ostinato-desktop-slice')
const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'ostinato-package-proof-'))

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
  if (value.includes('not found')) throw new Error(`unresolved packaged dependency:\n${value}`)
  return {
    text: value,
    resolved: new Map(
      [...value.matchAll(/^\s*(\S+)\s+=>\s+(\S+)\s+\(/gm)]
        .map((match) => [match[1], match[2]]),
    ),
  }
}

function encode(value) {
  const payload = Buffer.from(JSON.stringify(value))
  const frame = Buffer.alloc(payload.length + 5)
  frame.writeUInt32BE(payload.length + 1, 0)
  frame[4] = 1
  payload.copy(frame, 5)
  return frame
}

async function waitFor(predicate, message, milliseconds = 5_000) {
  const deadline = Date.now() + milliseconds
  while (Date.now() < deadline) {
    const value = predicate()
    if (value) return value
    await new Promise((resolve) => setTimeout(resolve, 20))
  }
  throw new Error(message)
}

async function probeController(executable, home) {
  const child = spawn(executable, [], {
    cwd: path.dirname(executable),
    env: { HOME: home, LANG: 'C.UTF-8', PATH: '/usr/bin:/bin' },
    stdio: ['pipe', 'pipe', 'pipe'],
  })
  let buffered = Buffer.alloc(0)
  let stderr = ''
  const frames = []
  child.stderr.on('data', (chunk) => { stderr += chunk.toString() })
  child.stdout.on('data', (chunk) => {
    buffered = Buffer.concat([buffered, chunk])
    while (buffered.length >= 4) {
      const body = buffered.readUInt32BE(0)
      if (body < 1 || body > 1024 * 1024) throw new Error('invalid packaged controller frame')
      if (buffered.length < body + 4) return
      if (buffered[4] !== 1) throw new Error('unexpected packaged controller frame type')
      frames.push(JSON.parse(buffered.subarray(5, body + 4).toString()))
      buffered = buffered.subarray(body + 4)
    }
  })
  await waitFor(
    () => frames.find((frame) => frame.kind === 'event' && frame.event === 'status'),
    `isolated packaged controller did not start: ${stderr}`,
  )
  child.stdin.write(encode({ kind: 'request', id: 'package-stop', command: 'shutdown', args: {} }))
  const exit = await Promise.race([
    new Promise((resolve) => child.once('close', (code, signal) => resolve({ code, signal }))),
    new Promise((_, reject) => setTimeout(
      () => reject(new Error('isolated packaged controller shutdown timed out')),
      5_000,
    )),
  ])
  if (exit.code !== 0 || exit.signal !== null || buffered.length !== 0 ||
      !frames.some((frame) => frame.kind === 'response' &&
        frame.id === 'package-stop' && frame.ok === true)) {
    throw new Error(`isolated packaged controller probe failed: ${JSON.stringify({ exit, stderr, frames })}`)
  }
  return frames.length
}

function alive(pid) {
  try {
    process.kill(pid, 0)
    return true
  } catch (error) {
    if (error.code === 'ESRCH') return false
    throw error
  }
}

let packagedApp
try {
  if (!fs.existsSync(packagedController) || !fs.existsSync(packagedExecutable)) {
    throw new Error('linux-unpacked app/controller artifact is missing')
  }
  const dynamic = dynamicEntries(packagedController)
  if (!dynamic.runpath.split(':').includes('$ORIGIN')) {
    throw new Error('packaged controller lacks $ORIGIN RUNPATH')
  }
  const protobufSoname = dynamic.needed.find((name) => /^libprotobuf\.so\./.test(name))
  if (!protobufSoname) throw new Error('packaged controller lacks protobuf SONAME dependency')
  const linked = linkedLibraries(packagedController)
  for (const soname of [protobufSoname, 'libz.so.1']) {
    const resolved = linked.resolved.get(soname)
    if (!resolved || path.dirname(fs.realpathSync(resolved)) !==
        fs.realpathSync(packagedControllerDirectory)) {
      throw new Error(`${soname} is not self-contained in packaged resources`)
    }
  }

  const isolatedControllerDirectory = path.join(temporary, 'controller')
  fs.cpSync(packagedControllerDirectory, isolatedControllerDirectory, { recursive: true })
  const controllerFrames = await probeController(
    path.join(isolatedControllerDirectory, 'ostinato-controller'),
    temporary,
  )

  // Copy the complete unpacked artifact outside the checkout, launch with a
  // sanitized environment and no development override, then verify reap.
  const isolatedApp = path.join(temporary, 'linux-unpacked')
  fs.cpSync(unpacked, isolatedApp, { recursive: true })
  const isolatedExecutable = path.join(isolatedApp, 'ostinato-desktop-slice')
  const environment = {
    HOME: temporary,
    LANG: 'C.UTF-8',
    PATH: '/usr/bin:/bin',
    DISPLAY: process.env.DISPLAY,
    ...(process.env.XAUTHORITY ? { XAUTHORITY: process.env.XAUTHORITY } : {}),
    ...(process.env.XDG_RUNTIME_DIR ? { XDG_RUNTIME_DIR: process.env.XDG_RUNTIME_DIR } : {}),
  }
  packagedApp = await electron.launch({
    executablePath: isolatedExecutable,
    cwd: temporary,
    env: environment,
  })
  const page = await packagedApp.firstWindow()
  await page.waitForURL(/dist-renderer\/index\.html/)
  await page.getByTestId('endpoint-host').waitFor()
  const controllerPid = Number(await page.getByTestId('controller-pid').textContent())
  if (!Number.isSafeInteger(controllerPid) || controllerPid < 2) {
    throw new Error('isolated packaged app did not start its controller')
  }
  await packagedApp.close()
  packagedApp = undefined
  await waitFor(() => !alive(controllerPid), `packaged controller ${controllerPid} was not reaped`)

  console.log(JSON.stringify({
    runpath: dynamic.runpath,
    protobufSoname,
    protobufResolved: linked.resolved.get(protobufSoname),
    zlibResolved: linked.resolved.get('libz.so.1'),
    isolatedControllerFrames: controllerFrames,
    isolatedPackagedApp: 'started and closed outside checkout',
    controllerReaped: true,
    ldd: linked.text.trim().split('\n').map((line) => line.trim()),
  }, null, 2))
} finally {
  if (packagedApp) await packagedApp.close().catch(() => {})
  fs.rmSync(temporary, { recursive: true, force: true })
}
