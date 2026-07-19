export type ConnectionState =
  | 'starting'
  | 'connecting'
  | 'connected'
  | 'disconnected'
  | 'error'

export interface Endpoint {
  host: string
  port: number
}

export interface ConnectionSnapshot {
  state: ConnectionState
  message?: string
  endpoint?: Endpoint
  controllerPid?: number
}

export type LinkState = 'unknown' | 'down' | 'up'

export interface PortSnapshot {
  id: number
  name: string
  link: LinkState
  transmit: boolean
  capture: boolean
  dirty: boolean
}

export interface PortStatsSnapshot {
  id: number
  link: LinkState
  transmit: boolean
  capture: boolean
  rxPkts: string
  rxBytes: string
  rxPps: string
  rxBps: string
  txPkts: string
  txBytes: string
  txPps: string
  txBps: string
  rxDrops: string
  rxErrors: string
}

export interface StatsSnapshot {
  sequence: number
  sampledAt: number
  ports: Record<number, PortStatsSnapshot>
}

export interface AppSnapshot {
  connection: ConnectionSnapshot
  ports: PortSnapshot[]
  stats: StatsSnapshot
}

export interface OstinatoDesktopApi {
  getSnapshot(): Promise<AppSnapshot>
  connect(endpoint: Endpoint): Promise<void>
  disconnect(): Promise<void>
  reconnect(): Promise<void>
  subscribeSnapshot(listener: (snapshot: AppSnapshot) => void): () => void
}
