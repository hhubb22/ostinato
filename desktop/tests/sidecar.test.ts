import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import process from 'node:process'
import { afterEach, describe, expect, it } from 'vitest'
import { Sidecar } from '../electron/sidecar'

const fakeController = String.raw`
const mode = process.argv[1]
let buffered = Buffer.alloc(0)
const send = (value) => {
  const payload = Buffer.from(JSON.stringify(value))
  const frame = Buffer.alloc(payload.length + 5)
  frame.writeUInt32BE(payload.length + 1, 0)
  frame[4] = 1
  payload.copy(frame, 5)
  process.stdout.write(frame)
}
send({ kind: 'event', event: 'status', payload: { state: 'disconnected', message: 'fake ready' } })
if (mode === 'delayed') process.on('SIGTERM', () => {})
process.stdin.on('data', (chunk) => {
  buffered = Buffer.concat([buffered, chunk])
  while (buffered.length >= 4) {
    const size = buffered.readUInt32BE(0)
    if (buffered.length < size + 4) return
    const request = JSON.parse(buffered.subarray(5, size + 4).toString())
    buffered = buffered.subarray(size + 4)
    if (request.command === 'shutdown') {
      send({ kind: 'response', id: request.id, ok: true })
      setImmediate(() => process.exit(0))
    } else {
      setTimeout(() => send({ kind: 'response', id: request.id, ok: true }),
        mode === 'delayed' ? 300 : 80)
    }
  }
})
`

const temporaryDirectories: string[] = []

afterEach(() => {
  for (const directory of temporaryDirectories.splice(0)) {
    fs.rmSync(directory, { recursive: true, force: true })
  }
})

async function bounded<T>(promise: Promise<T>, milliseconds = 1_000): Promise<T> {
  return Promise.race([
    promise,
    new Promise<T>((_, reject) => setTimeout(
      () => reject(new Error(`operation exceeded ${milliseconds} ms`)),
      milliseconds,
    )),
  ])
}

function alive(pid: number): boolean {
  try {
    process.kill(pid, 0)
    return true
  } catch (error) {
    return (error as NodeJS.ErrnoException).code !== 'ESRCH'
  }
}

describe('supervised controller sidecar', () => {
  it('turns a delayed request into one timeout and reaps the controller session', async () => {
    const sidecar = new Sidecar(process.execPath, {
      arguments: ['-e', fakeController, 'delayed'],
      deadlines: { connect: 30 },
      termGraceMs: 30,
      killGraceMs: 200,
    })
    const errors: string[] = []
    sidecar.on('protocolError', (message: string) => errors.push(message))
    sidecar.start()
    const pid = sidecar.pid
    expect(pid).toBeTypeOf('number')

    await expect(bounded(sidecar.command('connect', {
      host: '127.0.0.1',
      port: 7878,
    }))).rejects.toThrow('controller request timed out: connect')

    expect(errors).toEqual(['controller request timed out: connect'])
    expect(sidecar.pid).toBeUndefined()
    expect(alive(pid as number)).toBe(false)
    await new Promise((resolve) => setTimeout(resolve, 350))
    expect(errors).toHaveLength(1)
    expect(errors.join(' ')).not.toContain('unknown response id')
  })

  it('allows only one control request in flight and does not queue callers', async () => {
    const sidecar = new Sidecar(process.execPath, {
      arguments: ['-e', fakeController, 'normal'],
      deadlines: { disconnect: 1_000 },
      termGraceMs: 100,
      killGraceMs: 100,
    })
    const protocolErrors: string[] = []
    sidecar.on('protocolError', (message: string) => protocolErrors.push(message))
    sidecar.start()

    const first = sidecar.command('disconnect')
    const concurrent = Array.from(
      { length: 50 },
      () => sidecar.command('reconnect'),
    )
    const results = await Promise.allSettled(concurrent)
    expect(results.every((result) =>
      result.status === 'rejected' && result.reason instanceof Error &&
      result.reason.message === 'controller busy')).toBe(true)
    await first
    expect(protocolErrors).toEqual([])
    await bounded(sidecar.stop())
  })

  it('bounds close when an existing controller file cannot be spawned', async () => {
    const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'ostinato-sidecar-spawn-'))
    temporaryDirectories.push(directory)
    const fixtures = [
      { name: 'not-executable', bytes: Buffer.from('#!/bin/sh\nexit 0\n'), mode: 0o644 },
      { name: 'invalid-elf', bytes: Buffer.from('\x7fELF invalid executable'), mode: 0o755 },
    ]

    for (const fixture of fixtures) {
      const executable = path.join(directory, fixture.name)
      fs.writeFileSync(executable, fixture.bytes, { mode: fixture.mode })
      const sidecar = new Sidecar(executable, {
        deadlines: { shutdown: 50 },
        termGraceMs: 50,
        killGraceMs: 100,
      })
      sidecar.start()
      await new Promise((resolve) => setTimeout(resolve, 25))
      await bounded(sidecar.stop(), 500)
      expect(sidecar.pid).toBeUndefined()
    }
  })
})
