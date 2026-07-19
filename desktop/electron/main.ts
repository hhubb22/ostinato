import path from 'node:path'
import {
  app,
  BrowserWindow,
  ipcMain,
  type IpcMainInvokeEvent,
} from 'electron'
import type { AppSnapshot } from '../shared/types'
import { validateEndpoint } from './schema'
import { controllerPath, secureWebPreferences, validatedDevelopmentUrl } from './security'
import { Sidecar, type ControllerEvent } from './sidecar'

let window: BrowserWindow | undefined
let sidecar: Sidecar | undefined
let quitting = false
let broadcastScheduled = false

const snapshot: AppSnapshot = {
  connection: { state: 'starting', message: 'Starting controller' },
  ports: [],
  stats: { sequence: 0, sampledAt: 0, ports: {} },
}

function trusted(event: IpcMainInvokeEvent): void {
  if (!window || event.sender !== window.webContents ||
      event.senderFrame !== window.webContents.mainFrame) {
    throw new Error('untrusted renderer IPC sender')
  }
}

function broadcast(): void {
  if (broadcastScheduled) return
  broadcastScheduled = true
  setImmediate(() => {
    broadcastScheduled = false
    if (window && !window.isDestroyed()) window.webContents.send('ostinato:snapshot', snapshot)
  })
}

function controllerEvent(message: ControllerEvent): void {
  if (message.event === 'status') {
    snapshot.connection = { ...message.payload, controllerPid: sidecar?.pid }
  } else if (message.event === 'ports') {
    snapshot.ports = message.payload.ports
  } else if (message.event === 'stats') {
    snapshot.stats = {
      sequence: message.payload.sequence,
      sampledAt: message.payload.sampledAt,
      ports: Object.fromEntries(message.payload.ports.map((value) => [value.id, value])),
    }
  } else {
    snapshot.connection = {
      state: 'error',
      message: `${message.payload.code}: ${message.payload.message}`,
      controllerPid: sidecar?.pid,
    }
  }
  broadcast()
}

function registerIpc(): void {
  ipcMain.handle('ostinato:get-snapshot', (event) => {
    trusted(event)
    return snapshot
  })
  ipcMain.handle('ostinato:connect', async (event, value: unknown) => {
    trusted(event)
    const endpoint = validateEndpoint(value)
    await sidecar?.command('connect', endpoint)
  })
  ipcMain.handle('ostinato:disconnect', async (event, ...args: unknown[]) => {
    trusted(event)
    if (args.length) throw new Error('disconnect takes no arguments')
    await sidecar?.command('disconnect')
  })
  ipcMain.handle('ostinato:reconnect', async (event, ...args: unknown[]) => {
    trusted(event)
    if (args.length) throw new Error('reconnect takes no arguments')
    await sidecar?.command('reconnect')
  })
}

async function createWindow(): Promise<void> {
  const preload = path.join(__dirname, 'preload.js')
  window = new BrowserWindow({
    width: 1180,
    height: 760,
    minWidth: 860,
    minHeight: 580,
    show: false,
    webPreferences: secureWebPreferences(preload),
  })
  window.webContents.setWindowOpenHandler(() => ({ action: 'deny' }))
  window.webContents.on('will-navigate', (event) => event.preventDefault())
  window.once('ready-to-show', () => window?.show())

  const developmentUrl = app.isPackaged
    ? undefined
    : validatedDevelopmentUrl(process.env.OSTINATO_DEV_URL)
  if (developmentUrl) await window.loadURL(developmentUrl)
  else await window.loadFile(path.join(app.getAppPath(), 'dist-renderer', 'index.html'))
}

app.whenReady().then(async () => {
  const executable = controllerPath({
    isPackaged: app.isPackaged,
    appPath: app.getAppPath(),
    resourcesPath: process.resourcesPath,
    developmentOverride: app.isPackaged ? undefined : process.env.OSTINATO_CONTROLLER_PATH,
  })
  sidecar = new Sidecar(executable)
  sidecar.on('event', controllerEvent)
  sidecar.on('protocolError', (message: string) => {
    snapshot.connection = { state: 'error', message, controllerPid: sidecar?.pid }
    broadcast()
  })
  sidecar.on('unexpectedExit', (message: string) => {
    snapshot.connection = { state: 'error', message }
    snapshot.ports = []
    snapshot.stats = { sequence: snapshot.stats.sequence + 1, sampledAt: Date.now(), ports: {} }
    broadcast()
  })
  try {
    sidecar.start()
  } catch (error) {
    snapshot.connection = { state: 'error', message: String(error) }
  }
  registerIpc()
  await createWindow()
  broadcast()
}).catch((error) => {
  console.error(error)
  app.exit(1)
})

app.on('window-all-closed', () => app.quit())
app.on('before-quit', (event) => {
  if (quitting) return
  event.preventDefault()
  quitting = true
  void sidecar?.stop().finally(() => app.quit())
})
