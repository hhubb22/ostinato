import { isIP } from 'node:net'
import type {
  ConnectionSnapshot,
  Endpoint,
  LinkState,
  PortSnapshot,
  PortStatsSnapshot,
} from '../shared/types'

export interface ControllerResponse {
  kind: 'response'
  id: string
  ok: boolean
  error?: { code: string; message: string }
}

export type ControllerEvent =
  | { kind: 'event'; event: 'status'; payload: Omit<ConnectionSnapshot, 'controllerPid'> }
  | { kind: 'event'; event: 'ports'; payload: { ports: PortSnapshot[] } }
  | {
      kind: 'event'
      event: 'stats'
      payload: { sequence: number; sampledAt: number; ports: PortStatsSnapshot[] }
    }
  | { kind: 'event'; event: 'protocolError'; payload: { code: string; message: string } }

export type ControllerEnvelope = ControllerResponse | ControllerEvent

function record(value: unknown, context: string): Record<string, unknown> {
  if (!value || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error(`${context} must be an object`)
  }
  return value as Record<string, unknown>
}

function exactKeys(
  value: Record<string, unknown>,
  required: string[],
  optional: string[] = [],
): void {
  const accepted = new Set([...required, ...optional])
  if (required.some((key) => !(key in value)) ||
      Object.keys(value).some((key) => !accepted.has(key))) {
    throw new Error('object fields do not match schema')
  }
}

function string(value: unknown, context: string, maximum = 1024): string {
  if (typeof value !== 'string' || value.length > maximum) {
    throw new Error(`${context} must be a bounded string`)
  }
  return value
}

function finiteNumber(value: unknown, context: string): number {
  if (typeof value !== 'number' || !Number.isFinite(value)) {
    throw new Error(`${context} must be a finite number`)
  }
  return value
}

function boolean(value: unknown, context: string): boolean {
  if (typeof value !== 'boolean') throw new Error(`${context} must be boolean`)
  return value
}

function link(value: unknown): LinkState {
  if (value !== 'unknown' && value !== 'down' && value !== 'up') {
    throw new Error('invalid link state')
  }
  return value
}

function port(value: unknown): PortSnapshot {
  const input = record(value, 'port')
  exactKeys(input, ['id', 'name', 'link', 'transmit', 'capture', 'dirty'])
  const id = finiteNumber(input.id, 'port id')
  if (!Number.isSafeInteger(id) || id < 0 || id > 0xffffffff) {
    throw new Error('port id must be uint32')
  }
  return {
    id,
    name: string(input.name, 'port name', 4096),
    link: link(input.link),
    transmit: boolean(input.transmit, 'transmit'),
    capture: boolean(input.capture, 'capture'),
    dirty: boolean(input.dirty, 'dirty'),
  }
}

function counter(input: Record<string, unknown>, key: string): string {
  const value = string(input[key], key, 32)
  if (!/^\d+$/.test(value)) throw new Error(`${key} must be an unsigned decimal string`)
  return value
}

function stats(value: unknown): PortStatsSnapshot {
  const input = record(value, 'port stats')
  exactKeys(input, [
    'id', 'link', 'transmit', 'capture', 'rx_pkts', 'rx_bytes', 'rx_pps', 'rx_bps',
    'tx_pkts', 'tx_bytes', 'tx_pps', 'tx_bps', 'rx_drops', 'rx_errors',
  ])
  const id = finiteNumber(input.id, 'stats port id')
  if (!Number.isSafeInteger(id) || id < 0 || id > 0xffffffff) {
    throw new Error('stats port id must be uint32')
  }
  return {
    id,
    link: link(input.link),
    transmit: boolean(input.transmit, 'stats transmit'),
    capture: boolean(input.capture, 'stats capture'),
    rxPkts: counter(input, 'rx_pkts'),
    rxBytes: counter(input, 'rx_bytes'),
    rxPps: counter(input, 'rx_pps'),
    rxBps: counter(input, 'rx_bps'),
    txPkts: counter(input, 'tx_pkts'),
    txBytes: counter(input, 'tx_bytes'),
    txPps: counter(input, 'tx_pps'),
    txBps: counter(input, 'tx_bps'),
    rxDrops: counter(input, 'rx_drops'),
    rxErrors: counter(input, 'rx_errors'),
  }
}

export function validateEndpoint(value: unknown): Endpoint {
  const input = record(value, 'endpoint')
  exactKeys(input, ['host', 'port'])
  const host = string(input.host, 'host', 64)
  const endpointPort = finiteNumber(input.port, 'port')
  if (isIP(host) === 0 || !Number.isInteger(endpointPort) ||
      endpointPort < 1 || endpointPort > 65535) {
    throw new Error('numeric IPv4/IPv6 host and port 1-65535 required')
  }
  return { host, port: endpointPort }
}

export function parseControllerEnvelope(json: string): ControllerEnvelope {
  if (Buffer.byteLength(json, 'utf8') > 1024 * 1024) throw new Error('JSON exceeds maximum')
  const input = record(JSON.parse(json), 'controller envelope')
  const kind = string(input.kind, 'kind', 16)
  if (kind === 'response') {
    exactKeys(input, ['kind', 'id', 'ok'], ['error'])
    const id = string(input.id, 'response id', 64)
    const ok = boolean(input.ok, 'response ok')
    if (ok && input.error !== undefined) throw new Error('successful response has error')
    if (!ok) {
      const error = record(input.error, 'response error')
      exactKeys(error, ['code', 'message'])
      return {
        kind,
        id,
        ok,
        error: {
          code: string(error.code, 'error code', 64),
          message: string(error.message, 'error message', 4096),
        },
      }
    }
    return { kind, id, ok }
  }
  if (kind !== 'event') throw new Error('unknown envelope kind')
  exactKeys(input, ['kind', 'event', 'payload'])
  const event = string(input.event, 'event', 32)
  const payload = record(input.payload, 'event payload')
  if (event === 'status') {
    exactKeys(payload, ['state'], ['message', 'endpoint'])
    const state = string(payload.state, 'connection state', 16)
    if (!['starting', 'connecting', 'connected', 'disconnected', 'error'].includes(state)) {
      throw new Error('invalid connection state')
    }
    return {
      kind,
      event,
      payload: {
        state: state as ConnectionSnapshot['state'],
        ...(payload.message === undefined ? {} : { message: string(payload.message, 'message', 4096) }),
        ...(payload.endpoint === undefined ? {} : { endpoint: validateEndpoint(payload.endpoint) }),
      },
    }
  }
  if (event === 'ports') {
    exactKeys(payload, ['ports'])
    if (!Array.isArray(payload.ports) || payload.ports.length > 65536) {
      throw new Error('ports must be a bounded array')
    }
    const ports = payload.ports.map(port)
    if (new Set(ports.map((item) => item.id)).size !== ports.length) {
      throw new Error('duplicate port id')
    }
    return { kind, event, payload: { ports } }
  }
  if (event === 'stats') {
    exactKeys(payload, ['sequence', 'sampledAt', 'ports'])
    if (!Array.isArray(payload.ports) || payload.ports.length > 65536) {
      throw new Error('stats ports must be a bounded array')
    }
    const ports = payload.ports.map(stats)
    return {
      kind,
      event,
      payload: {
        sequence: finiteNumber(payload.sequence, 'sequence'),
        sampledAt: finiteNumber(payload.sampledAt, 'sampledAt'),
        ports,
      },
    }
  }
  if (event === 'protocolError') {
    exactKeys(payload, ['code', 'message'])
    return {
      kind,
      event,
      payload: {
        code: string(payload.code, 'protocol error code', 64),
        message: string(payload.message, 'protocol error message', 4096),
      },
    }
  }
  throw new Error('unknown controller event')
}
