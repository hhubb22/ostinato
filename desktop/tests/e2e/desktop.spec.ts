import { _electron as electron, expect, test } from '@playwright/test'
import { spawn, type ChildProcess } from 'node:child_process'
import net from 'node:net'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const __dirname = path.dirname(fileURLToPath(import.meta.url))
const desktopRoot = path.resolve(__dirname, '../..')
const repositoryRoot = path.resolve(desktopRoot, '..')
const controller = path.join(repositoryRoot, 'build-desktop', 'ostinato-controller')
const droneExecutable = path.join(repositoryRoot, 'build-desktop', 'server', 'drone')

async function availablePort(): Promise<number> {
  return new Promise((resolve, reject) => {
    const server = net.createServer()
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => {
      const address = server.address()
      if (!address || typeof address === 'string') return reject(new Error('no test port'))
      server.close((error) => error ? reject(error) : resolve(address.port))
    })
  })
}

async function waitForListener(port: number, child: ChildProcess): Promise<void> {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    if (child.exitCode !== null) throw new Error(`drone exited during startup: ${child.exitCode}`)
    const connected = await new Promise<boolean>((resolve) => {
      const socket = net.connect({ host: '127.0.0.1', port })
      socket.once('connect', () => { socket.destroy(); resolve(true) })
      socket.once('error', () => resolve(false))
    })
    if (connected) return
    await new Promise((resolve) => setTimeout(resolve, 50))
  }
  throw new Error('drone did not listen')
}

async function stopChild(child: ChildProcess): Promise<void> {
  if (child.exitCode !== null || child.signalCode !== null) return
  child.kill('SIGTERM')
  await Promise.race([
    new Promise((resolve) => child.once('exit', resolve)),
    new Promise((resolve) => setTimeout(resolve, 1000)),
  ])
  if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL')
}

async function waitForReap(pid: number): Promise<void> {
  for (let attempt = 0; attempt < 100; attempt += 1) {
    try {
      process.kill(pid, 0)
    } catch (error: any) {
      if (error.code === 'ESRCH') return
      throw error
    }
    await new Promise((resolve) => setTimeout(resolve, 25))
  }
  throw new Error(`controller ${pid} remained alive after Electron close`)
}

test('real drone vertical slice, security, virtualization, and controller reap', async () => {
  const dronePort = await availablePort()
  const drone = spawn(droneExecutable, ['-d', '-p', String(dronePort)], {
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  let droneLogs = ''
  drone.stdout?.on('data', (value) => { droneLogs += value.toString() })
  drone.stderr?.on('data', (value) => { droneLogs += value.toString() })

  try {
    await waitForListener(dronePort, drone)
    const launchedAt = performance.now()
    const electronApp = await electron.launch({
      args: [desktopRoot],
      cwd: desktopRoot,
      env: {
        ...process.env,
        OSTINATO_CONTROLLER_PATH: controller,
      },
    })
    const page = await electronApp.firstWindow()
    await page.waitForURL(/dist-renderer\/index\.html/)
    await page.waitForLoadState('domcontentloaded')
    await expect(page).toHaveTitle('Ostinato Desktop')
    const launchMs = performance.now() - launchedAt

    const preferences = await electronApp.evaluate(({ BrowserWindow }) => {
      return BrowserWindow.getAllWindows()[0].webContents.getLastWebPreferences()
    })
    expect(preferences.contextIsolation).toBe(true)
    expect(preferences.nodeIntegration).toBe(false)
    expect(preferences.sandbox).toBe(true)
    expect(await page.evaluate(() => ({
      process: typeof (globalThis as any).process,
      require: typeof (globalThis as any).require,
      api: Object.keys(window.ostinato).sort(),
      csp: document.querySelector('meta[http-equiv="Content-Security-Policy"]')?.getAttribute('content'),
    }))).toEqual(expect.objectContaining({
      process: 'undefined',
      require: 'undefined',
      api: ['connect', 'disconnect', 'getSnapshot', 'reconnect', 'subscribeSnapshot'],
    }))

    await page.getByTestId('endpoint-host').fill('127.0.0.1')
    await page.getByTestId('endpoint-port').fill(String(dronePort))
    await page.getByTestId('connect').click()
    await expect(page.getByTestId('connection-status')).toContainText('connected', { timeout: 20_000 })
    const controllerPid = Number(await page.getByTestId('controller-pid').textContent())
    expect(controllerPid).toBeGreaterThan(1)
    await expect(page.getByTestId('port-rows').locator('tr:not(.empty)').first()).toBeVisible()

    const sequenceBefore = Number((await page.getByTestId('stats-sequence').textContent())?.match(/sample (\d+)/)?.[1])
    await expect.poll(async () => {
      return Number((await page.getByTestId('stats-sequence').textContent())?.match(/sample (\d+)/)?.[1])
    }, { timeout: 5000 }).toBeGreaterThan(sequenceBefore)

    await page.getByTestId('disconnect').click()
    await expect(page.getByTestId('connection-status')).toContainText('disconnected')
    await expect(page.getByTestId('port-rows').locator('tr:not(.empty)')).toHaveCount(0)
    await page.getByTestId('reconnect').click()
    await expect(page.getByTestId('connection-status')).toContainText('connected', { timeout: 20_000 })
    await expect(page.getByTestId('port-rows').locator('tr:not(.empty)').first()).toBeVisible()

    await page.getByTestId('performance-tab').click()
    await expect(page.getByText('Synthetic only')).toBeVisible()
    await expect(page.getByTestId('virtual-model-count')).toHaveText('2000')
    const initialDomRows = await page.getByTestId('synthetic-row').count()
    expect(initialDomRows).toBeLessThan(40)
    await page.getByTestId('virtual-viewport').evaluate((element) => { element.scrollTop = 30_000 })
    await page.waitForTimeout(1200)
    expect(await page.getByTestId('synthetic-row').count()).toBeLessThan(40)
    const updateMs = Number((await page.getByTestId('virtual-metrics').textContent())?.match(/frame ([\d.]+) ms/)?.[1])
    expect(updateMs).toBeLessThan(100)

    console.log(JSON.stringify({ launchMs: Math.round(launchMs), initialDomRows, updateMs, controllerPid }))
    await electronApp.close()
    await waitForReap(controllerPid)
  } catch (error) {
    throw new Error(`${String(error)}\nDrone output:\n${droneLogs}`)
  } finally {
    await stopChild(drone)
  }
})
