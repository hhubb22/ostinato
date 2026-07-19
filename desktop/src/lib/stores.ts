import { writable } from 'svelte/store'
import type {
  AppSnapshot,
  ConnectionSnapshot,
  PortSnapshot,
  StatsSnapshot,
} from '../../shared/types'

export const connection = writable<ConnectionSnapshot>({
  state: 'starting',
  message: 'Waiting for controller snapshot',
})
export const ports = writable<PortSnapshot[]>([])
export const stats = writable<StatsSnapshot>({ sequence: 0, sampledAt: 0, ports: {} })
export const selection = writable<number | undefined>(undefined)

export function applySnapshot(snapshot: AppSnapshot): void {
  // Each controller update enters each domain store once. In particular, one
  // stats frame produces one store notification regardless of field count.
  connection.set(snapshot.connection)
  ports.set(snapshot.ports)
  stats.set(snapshot.stats)
}
