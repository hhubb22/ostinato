import type { OstinatoDesktopApi } from '../shared/types'

declare global {
  interface Window {
    ostinato: OstinatoDesktopApi
  }
}

export {}
