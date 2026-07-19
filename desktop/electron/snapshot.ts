import type { AppSnapshot } from '../shared/types'

export function failControllerSnapshot(
  snapshot: AppSnapshot,
  message: string,
  controllerPid?: number,
): void {
  snapshot.connection = {
    state: 'error',
    message,
    ...(controllerPid === undefined ? {} : { controllerPid }),
  }
  snapshot.ports = []
  snapshot.stats = {
    sequence: snapshot.stats.sequence + 1,
    sampledAt: Date.now(),
    ports: {},
  }
}
