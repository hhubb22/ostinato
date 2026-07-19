import { get } from 'svelte/store'
import { describe, expect, it } from 'vitest'
import type { AppSnapshot } from '../shared/types'
import { applySnapshot, connection, ports, selection, stats } from '../src/lib/stores'

describe('renderer domain stores', () => {
  it('applies one authoritative batch per domain without changing selection', () => {
    selection.set(99)
    let statsNotifications = 0
    const unsubscribe = stats.subscribe(() => { statsNotifications += 1 })
    const before = statsNotifications
    const snapshot: AppSnapshot = {
      connection: { state: 'connected', endpoint: { host: '::1', port: 7878 } },
      ports: [{ id: 4, name: 'eth0', link: 'up', transmit: false, capture: false, dirty: false }],
      stats: {
        sequence: 12,
        sampledAt: 123,
        ports: {
          4: {
            id: 4, link: 'up', transmit: false, capture: false,
            rxPkts: '1', rxBytes: '2', rxPps: '3', rxBps: '4',
            txPkts: '5', txBytes: '6', txPps: '7', txBps: '8',
            rxDrops: '0', rxErrors: '0',
          },
        },
      },
    }
    applySnapshot(snapshot)
    expect(statsNotifications - before).toBe(1)
    expect(get(connection).state).toBe('connected')
    expect(get(ports)[0].id).toBe(4)
    expect(get(stats).ports[4].txPps).toBe('7')
    expect(get(selection)).toBe(99)
    unsubscribe()
  })
})
