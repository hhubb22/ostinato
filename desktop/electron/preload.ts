import { contextBridge, ipcRenderer } from 'electron'
import type { AppSnapshot, Endpoint, OstinatoDesktopApi } from '../shared/types'

const api: OstinatoDesktopApi = {
  getSnapshot: () => ipcRenderer.invoke('ostinato:get-snapshot'),
  connect: (endpoint: Endpoint) => ipcRenderer.invoke('ostinato:connect', endpoint),
  disconnect: () => ipcRenderer.invoke('ostinato:disconnect'),
  reconnect: () => ipcRenderer.invoke('ostinato:reconnect'),
  subscribeSnapshot: (listener: (snapshot: AppSnapshot) => void) => {
    const handler = (_event: Electron.IpcRendererEvent, snapshot: AppSnapshot) => listener(snapshot)
    ipcRenderer.on('ostinato:snapshot', handler)
    return () => ipcRenderer.removeListener('ostinato:snapshot', handler)
  },
}

contextBridge.exposeInMainWorld('ostinato', Object.freeze(api))
