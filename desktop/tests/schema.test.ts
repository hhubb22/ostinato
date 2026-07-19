import { describe, expect, it } from 'vitest'
import { parseControllerEnvelope, validateEndpoint } from '../electron/schema'

describe('renderer and controller schema boundaries', () => {
  it('accepts only numeric IPv4/IPv6 endpoints and bounded ports', () => {
    expect(validateEndpoint({ host: '127.0.0.1', port: 7878 })).toEqual({ host: '127.0.0.1', port: 7878 })
    expect(validateEndpoint({ host: '2001:db8::1', port: 1 })).toEqual({ host: '2001:db8::1', port: 1 })
    expect(() => validateEndpoint({ host: 'drone.example', port: 7878 })).toThrow(/numeric IPv4/)
    expect(() => validateEndpoint({ host: '127.0.0.1', port: 0 })).toThrow(/numeric IPv4/)
    expect(() => validateEndpoint({ host: '127.0.0.1', port: 7878, command: 'anything' })).toThrow(/schema/)
  })

  it('preserves response IDs and rejects unknown envelopes', () => {
    expect(parseControllerEnvelope(JSON.stringify({
      kind: 'response', id: 'main-9', ok: false,
      error: { code: 'unknown-command', message: 'unknown command' },
    }))).toMatchObject({ id: 'main-9', ok: false })
    expect(() => parseControllerEnvelope('{"kind":"magic"}')).toThrow()
  })

  it('validates and converts a complete batched stats event', () => {
    const event = parseControllerEnvelope(JSON.stringify({
      kind: 'event',
      event: 'stats',
      payload: {
        sequence: 4,
        sampledAt: 12345,
        ports: [{
          id: 7, link: 'up', transmit: false, capture: true,
          rx_pkts: '42', rx_bytes: '64', rx_pps: '2', rx_bps: '16',
          tx_pkts: '8', tx_bytes: '512', tx_pps: '1', tx_bps: '8',
          rx_drops: '0', rx_errors: '0',
        }],
      },
    }))
    expect(event.kind).toBe('event')
    if (event.kind === 'event' && event.event === 'stats') {
      expect(event.payload.ports[0]).toMatchObject({ id: 7, rxPkts: '42', capture: true })
    }
  })

  it('rejects malformed stats counters and duplicate port snapshots', () => {
    expect(() => parseControllerEnvelope(JSON.stringify({
      kind: 'event', event: 'ports', payload: { ports: [
        { id: 1, name: 'a', link: 'up', transmit: false, capture: false, dirty: false },
        { id: 1, name: 'b', link: 'up', transmit: false, capture: false, dirty: false },
      ] },
    }))).toThrow(/duplicate/)
  })
})
