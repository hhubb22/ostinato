import path from 'node:path'
import type { WebPreferences } from 'electron'

export function secureWebPreferences(preload: string): WebPreferences {
  return {
    preload,
    contextIsolation: true,
    nodeIntegration: false,
    sandbox: true,
  }
}

export function validatedDevelopmentUrl(value: string | undefined): string | undefined {
  if (!value) return undefined
  const url = new URL(value)
  if (url.protocol !== 'http:' ||
      (url.hostname !== '127.0.0.1' && url.hostname !== 'localhost') ||
      url.username || url.password || url.pathname !== '/' || url.search || url.hash) {
    throw new Error('development renderer URL must be a plain loopback HTTP origin')
  }
  return url.href
}

export function controllerPath(options: {
  isPackaged: boolean
  appPath: string
  resourcesPath: string
  developmentOverride?: string
}): string {
  if (options.isPackaged) {
    return path.join(options.resourcesPath, 'controller', 'ostinato-controller')
  }
  if (options.developmentOverride) {
    if (!path.isAbsolute(options.developmentOverride)) {
      throw new Error('development controller override must be absolute')
    }
    return options.developmentOverride
  }
  return path.resolve(options.appPath, '..', 'build-desktop', 'ostinato-controller')
}
