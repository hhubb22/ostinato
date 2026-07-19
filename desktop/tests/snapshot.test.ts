import { describe, expect, it } from 'vitest'
import type { AppSnapshot } from '../shared/types'
import { failControllerSnapshot } from '../electron/snapshot'

describe('controller terminal state', () => {
  it('makes an output error visible and clears stale ports and stats', () => {
    const snapshot: AppSnapshot = {
      connection: { state: 'connected', controllerPid: 99 },
      ports: [{
        id: 7,
        name: 'stale',
        link: 'up',
        transmit: false,
        capture: false,
        dirty: false,
      }],
      stats: {
        sequence: 10,
        sampledAt: 20,
        ports: {
          7: {
            id: 7,
            link: 'up',
            transmit: false,
            capture: false,
            rxPkts: '1',
            rxBytes: '2',
            rxPps: '3',
            rxBps: '4',
            txPkts: '5',
            txBytes: '6',
            txPps: '7',
            txBps: '8',
            rxDrops: '0',
            rxErrors: '0',
          },
        },
      },
    }

    failControllerSnapshot(snapshot, 'outgoing-frame-too-large', 99)

    expect(snapshot.connection).toMatchObject({
      state: 'error',
      message: 'outgoing-frame-too-large',
      controllerPid: 99,
    })
    expect(snapshot.ports).toEqual([])
    expect(snapshot.stats).toMatchObject({ sequence: 11, ports: {} })
  })
})
