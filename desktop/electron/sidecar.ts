import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process'
import { EventEmitter } from 'node:events'
import fs from 'node:fs'
import { FrameDecoder, JSON_FRAME_TYPE, encodeJsonFrame } from './framing'
import {
  parseControllerEnvelope,
  type ControllerEvent,
  type ControllerResponse,
} from './schema'

interface PendingRequest {
  resolve: () => void
  reject: (error: Error) => void
  timer: NodeJS.Timeout
}

export class Sidecar extends EventEmitter {
  private child?: ChildProcessWithoutNullStreams
  private readonly decoder = new FrameDecoder()
  private readonly pending = new Map<string, PendingRequest>()
  private nextId = 1
  private stopping = false
  private exited?: Promise<void>

  constructor(private readonly executable: string) {
    super()
  }

  get pid(): number | undefined {
    return this.child?.pid
  }

  start(): void {
    if (this.child) throw new Error('controller already started')
    if (!fs.existsSync(this.executable)) {
      throw new Error(`controller executable not found: ${this.executable}`)
    }
    this.stopping = false
    const child = spawn(this.executable, [], {
      detached: false,
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    })
    this.child = child
    this.exited = new Promise((resolve) => child.once('exit', () => resolve()))
    child.stdout.on('data', (chunk: Buffer) => this.onStdout(chunk))
    child.stderr.on('data', (chunk: Buffer) => {
      const text = chunk.toString('utf8').trimEnd()
      if (text) console.error(`[ostinato-controller] ${text}`)
    })
    child.once('error', (error) => this.fail(error))
    child.once('exit', (code, signal) => {
      this.child = undefined
      const message = `controller exited (${signal ?? code ?? 'unknown'})`
      this.rejectPending(new Error(message))
      if (!this.stopping) this.emit('unexpectedExit', message)
    })
  }

  command(command: 'connect' | 'disconnect' | 'reconnect' | 'shutdown', args: object = {}): Promise<void> {
    const child = this.child
    if (!child || !child.stdin.writable) return Promise.reject(new Error('controller is not running'))
    const id = `main-${this.nextId++}`
    const frame = encodeJsonFrame({ kind: 'request', id, command, args })
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id)
        reject(new Error(`controller request timed out: ${command}`))
      }, 15_000)
      this.pending.set(id, { resolve, reject, timer })
      child.stdin.write(frame, (error) => {
        if (!error) return
        const request = this.pending.get(id)
        if (!request) return
        clearTimeout(request.timer)
        this.pending.delete(id)
        request.reject(error)
      })
    })
  }

  async stop(): Promise<void> {
    const child = this.child
    if (!child) return
    this.stopping = true
    try {
      await Promise.race([
        this.command('shutdown'),
        new Promise((_, reject) => setTimeout(() => reject(new Error('shutdown timeout')), 1500)),
      ])
    } catch {
      child.kill('SIGTERM')
    }
    if (this.child) {
      await Promise.race([this.exited, new Promise((resolve) => setTimeout(resolve, 1000))])
    }
    if (this.child) {
      child.kill('SIGKILL')
      await this.exited
    }
    this.rejectPending(new Error('controller stopped'))
  }

  private onStdout(chunk: Buffer): void {
    try {
      for (const frame of this.decoder.push(chunk)) {
        if (frame.type !== JSON_FRAME_TYPE) throw new Error('unexpected controller frame type')
        const envelope = parseControllerEnvelope(frame.payload.toString('utf8'))
        if (envelope.kind === 'response') this.onResponse(envelope)
        else this.emit('event', envelope)
      }
    } catch (error) {
      this.fail(error instanceof Error ? error : new Error(String(error)))
    }
  }

  private onResponse(response: ControllerResponse): void {
    const request = this.pending.get(response.id)
    if (!request) {
      this.fail(new Error('controller returned an unknown response id'))
      return
    }
    clearTimeout(request.timer)
    this.pending.delete(response.id)
    if (response.ok) request.resolve()
    else request.reject(new Error(`${response.error?.code}: ${response.error?.message}`))
  }

  private fail(error: Error): void {
    this.rejectPending(error)
    this.emit('protocolError', error.message)
    this.child?.kill('SIGTERM')
  }

  private rejectPending(error: Error): void {
    for (const request of this.pending.values()) {
      clearTimeout(request.timer)
      request.reject(error)
    }
    this.pending.clear()
  }
}

export type { ControllerEvent }
