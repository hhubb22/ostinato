import { spawn } from 'node:child_process'
import fs from 'node:fs'
import net from 'node:net'
import path from 'node:path'
import process from 'node:process'
import { fileURLToPath } from 'node:url'
import { _electron as electron } from '@playwright/test'

const desktop = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')
const repository = path.resolve(desktop, '..')
const executable = path.join(desktop, 'release', 'linux-unpacked', 'ostinato-desktop-slice')
const droneExecutable = path.join(repository, 'build-desktop', 'server', 'drone')

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds))

async function availablePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer()
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => {
      const address = server.address()
      server.close((error) => error ? reject(error) : resolve(address.port))
    })
  })
}

async function waitForListener(port, child) {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (child.exitCode !== null) throw new Error('drone exited during startup')
    if (await new Promise((resolve) => {
      const socket = net.connect({ host: '127.0.0.1', port })
      socket.once('connect', () => { socket.destroy(); resolve(true) })
      socket.once('error', () => resolve(false))
    })) return
    await delay(50)
  }
  throw new Error('drone did not listen')
}

function processTree(root) {
  const result = []
  const visit = (pid) => {
    if (result.includes(pid)) return
    result.push(pid)
    try {
      const children = fs.readFileSync(`/proc/${pid}/task/${pid}/children`, 'utf8').trim()
      if (children) children.split(/\s+/).map(Number).forEach(visit)
    } catch {
      // A process may exit while its descendants are sampled.
    }
  }
  visit(root)
  return result
}

function memory(root) {
  let rssKiB = 0
  let pssKiB = 0
  const pids = processTree(root)
  for (const pid of pids) {
    try {
      const rollup = fs.readFileSync(`/proc/${pid}/smaps_rollup`, 'utf8')
      rssKiB += Number(rollup.match(/^Rss:\s+(\d+)/m)?.[1] ?? 0)
      pssKiB += Number(rollup.match(/^Pss:\s+(\d+)/m)?.[1] ?? 0)
    } catch {
      // A process may exit while its rollup is sampled.
    }
  }
  return { processes: pids.length, rssMiB: +(rssKiB / 1024).toFixed(1), pssMiB: +(pssKiB / 1024).toFixed(1) }
}

const port = await availablePort()
const drone = spawn(droneExecutable, ['-d', '-p', String(port)], { stdio: 'ignore' })
let app
try {
  await waitForListener(port, drone)
  const started = performance.now()
  app = await electron.launch({ executablePath: executable })
  const page = await app.firstWindow()
  await page.waitForURL(/dist-renderer\/index\.html/)
  await page.getByTestId('endpoint-host').waitFor()
  const coldStartMs = Math.round(performance.now() - started)
  const runtime = await app.evaluate(() => ({
    electron: process.versions.electron,
    node: process.versions.node,
    chrome: process.versions.chrome,
  }))
  await delay(1000)
  const idle = memory(app.process().pid)

  await page.getByTestId('endpoint-host').fill('127.0.0.1')
  await page.getByTestId('endpoint-port').fill(String(port))
  await page.getByTestId('connect').click()
  await page.getByTestId('connection-status').getByText('connected', { exact: false }).waitFor({ timeout: 20_000 })
  const controllerPid = Number(await page.getByTestId('controller-pid').textContent())
  const firstSequence = Number((await page.getByTestId('stats-sequence').textContent()).match(/sample (\d+)/)?.[1])
  for (let attempt = 0; attempt < 20; attempt += 1) {
    await delay(100)
    const current = Number((await page.getByTestId('stats-sequence').textContent()).match(/sample (\d+)/)?.[1])
    if (current > firstSequence) break
  }
  const updating = memory(app.process().pid)
  await page.getByTestId('performance-tab').click()
  await delay(1200)
  const virtualDomRows = await page.getByTestId('synthetic-row').count()
  const virtualFrameMs = Number((await page.getByTestId('virtual-metrics').textContent()).match(/frame ([\d.]+) ms/)?.[1])

  await app.close()
  app = undefined
  await delay(100)
  let controllerReaped = false
  try { process.kill(controllerPid, 0) } catch (error) { controllerReaped = error.code === 'ESRCH' }
  console.log(JSON.stringify({
    runtime,
    coldStartMs,
    idle,
    updating,
    virtualModelRows: 2000,
    virtualDomRows,
    virtualFrameMs,
    controllerReaped,
  }, null, 2))
} finally {
  if (app) await app.close().catch(() => {})
  if (drone.exitCode === null) drone.kill('SIGTERM')
}
