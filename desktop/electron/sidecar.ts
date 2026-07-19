import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process'
import { EventEmitter } from 'node:events'
import fs from 'node:fs'
import { FrameDecoder, JSON_FRAME_TYPE, encodeJsonFrame } from './framing'
import {
  parseControllerEnvelope,
  type ControllerEvent,
  type ControllerResponse,
} from './schema'

type ControllerCommand = 'connect' | 'disconnect' | 'reconnect' | 'shutdown'

interface PendingRequest {
  resolve: () => void
  reject: (error: Error) => void
  timer?: NodeJS.Timeout
}

export interface SidecarOptions {
  arguments?: readonly string[]
  deadlines?: Partial<Record<ControllerCommand, number>>
  termGraceMs?: number
  killGraceMs?: number
}

const DEFAULT_DEADLINES: Record<ControllerCommand, number> = {
  // ClientSession hydration performs several independently bounded RPCs per
  // port, so connect/reconnect need an aggregate product deadline above one
  // RPC timeout. Other state transitions should complete promptly.
  connect: 60_000,
  reconnect: 60_000,
  disconnect: 5_000,
  shutdown: 1_500,
}

export class Sidecar extends EventEmitter {
  private child?: ChildProcessWithoutNullStreams
  private readonly decoder = new FrameDecoder()
  private readonly pending = new Map<string, PendingRequest>()
  private readonly arguments: readonly string[]
  private readonly deadlines: Record<ControllerCommand, number>
  private readonly termGraceMs: number
  private readonly killGraceMs: number
  private nextId = 1
  private stopping = false
  private terminating = false
  private terminalSettled = false
  private failureReported = false
  private controlInFlight = false
  private shutdownInFlight?: Promise<void>
  private spawnError?: Error
  private terminationReason?: Error
  private termination?: Promise<void>
  private closed?: Promise<void>
  private settleClosed?: () => void

  constructor(private readonly executable: string, options: SidecarOptions = {}) {
    super()
    this.arguments = options.arguments ?? []
    this.deadlines = { ...DEFAULT_DEADLINES, ...options.deadlines }
    this.termGraceMs = options.termGraceMs ?? 1_000
    this.killGraceMs = options.killGraceMs ?? 1_000
  }

  get pid(): number | undefined {
    return this.child?.pid
  }

  start(): void {
    if (this.child || this.closed) throw new Error('controller already started')
    if (!fs.existsSync(this.executable)) {
      throw new Error(`controller executable not found: ${this.executable}`)
    }
    this.stopping = false
    const child = spawn(this.executable, [...this.arguments], {
      detached: false,
      stdio: ['pipe', 'pipe', 'pipe'],
      windowsHide: true,
    })
    this.child = child
    this.closed = new Promise((resolve) => { this.settleClosed = resolve })
    child.stdout.on('data', (chunk: Buffer) => this.onStdout(chunk))
    child.stderr.on('data', (chunk: Buffer) => {
      const text = chunk.toString('utf8').trimEnd()
      if (text) console.error(`[ostinato-controller] ${text}`)
    })
    child.stdin.on('error', (error) => {
      if (!this.terminating && !this.stopping) this.fail(error)
    })
    // A spawn failure emits `error`, then `close`, but is not guaranteed to
    // emit `exit`. Record it here; close remains the single terminal/reap path.
    child.once('error', (error) => {
      this.spawnError = error
      console.error(`[ostinato-controller] spawn failed: ${error.message}`)
    })
    child.once('close', (code, signal) => {
      const detail = this.spawnError
        ? `controller failed to start: ${this.spawnError.message}`
        : `controller closed (${signal ?? code ?? 'unknown'})`
      this.settleTerminal(child, new Error(detail))
    })
  }

  command(command: ControllerCommand, args: object = {}): Promise<void> {
    if (command === 'shutdown') {
      if (!this.shutdownInFlight) {
        this.shutdownInFlight = this.sendRequest(command, args)
          .finally(() => { this.shutdownInFlight = undefined })
      }
      return this.shutdownInFlight
    }
    if (this.controlInFlight) {
      return Promise.reject(new Error('controller busy'))
    }
    this.controlInFlight = true
    return this.sendRequest(command, args)
      .finally(() => { this.controlInFlight = false })
  }

  async stop(): Promise<void> {
    if (!this.child) return
    this.stopping = true
    try {
      await this.command('shutdown')
    } catch {
      // A failed or timed-out shutdown is escalated below.
    }
    if (this.child && !await this.waitForTerminal(this.termGraceMs)) {
      await this.terminate(new Error('controller shutdown did not close'))
    }
    if (this.termination) await this.termination
    this.rejectPending(new Error('controller stopped'))
  }

  private sendRequest(command: ControllerCommand, args: object): Promise<void> {
    const child = this.child
    if (!child || !child.stdin.writable || this.terminating) {
      return Promise.reject(new Error('controller is not running'))
    }
    const id = `main-${this.nextId++}`
    let frame: Buffer
    try {
      frame = encodeJsonFrame({ kind: 'request', id, command, args })
    } catch (error) {
      return Promise.reject(error instanceof Error ? error : new Error(String(error)))
    }
    const response = new Promise<void>((resolve, reject) => {
      const request: PendingRequest = { resolve, reject }
      request.timer = setTimeout(() => {
        if (!this.pending.has(id)) return
        request.timer = undefined
        const error = new Error(`controller request timed out: ${command}`)
        this.reportFailure(error)
        void this.terminate(error)
      }, this.deadlines[command])
      this.pending.set(id, request)
    })
    void this.writeFrame(child, frame).catch((error: Error) => {
      if (!this.pending.has(id)) return
      this.reportFailure(error)
      void this.terminate(error)
    })
    return response
  }

  private writeFrame(child: ChildProcessWithoutNullStreams, frame: Buffer): Promise<void> {
    return new Promise((resolve, reject) => {
      let callbackComplete = false
      let drainComplete = true
      let settled = false
      const finish = (error?: Error | null): void => {
        if (settled) return
        if (error) {
          settled = true
          child.stdin.off('drain', onDrain)
          reject(error)
        } else if (callbackComplete && drainComplete) {
          settled = true
          resolve()
        }
      }
      const onDrain = (): void => {
        drainComplete = true
        finish()
      }
      const accepted = child.stdin.write(frame, (error) => {
        callbackComplete = true
        finish(error)
      })
      if (!accepted) {
        // At most one control frame plus one shutdown frame can be queued. A
        // false write is not followed by another control write until drain.
        drainComplete = false
        child.stdin.once('drain', onDrain)
      }
    })
  }

  private onStdout(chunk: Buffer): void {
    if (this.terminating) return
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
    if (this.terminating) return
    const request = this.pending.get(response.id)
    if (!request) {
      this.fail(new Error('controller returned an unknown response id'))
      return
    }
    if (request.timer) clearTimeout(request.timer)
    this.pending.delete(response.id)
    if (response.ok) request.resolve()
    else request.reject(new Error(`${response.error?.code}: ${response.error?.message}`))
  }

  private fail(error: Error): void {
    if (this.terminating) return
    this.reportFailure(error)
    void this.terminate(error)
  }

  private reportFailure(error: Error): void {
    if (this.failureReported || this.stopping) return
    this.failureReported = true
    this.emit('protocolError', error.message)
  }

  private terminate(reason: Error): Promise<void> {
    if (this.termination) return this.termination
    this.terminating = true
    this.terminationReason = reason
    const child = this.child
    this.termination = (async () => {
      if (!child) return
      child.kill('SIGTERM')
      if (await this.waitForTerminal(this.termGraceMs)) return
      child.kill('SIGKILL')
      if (await this.waitForTerminal(this.killGraceMs)) return
      // Node normally emits close after SIGKILL. This final settlement keeps
      // Electron quit bounded even if the platform fails to report it.
      this.settleTerminal(child, new Error(`${reason.message}; close not observed after SIGKILL`))
    })()
    return this.termination
  }

  private async waitForTerminal(milliseconds: number): Promise<boolean> {
    if (!this.child || this.terminalSettled) return true
    const closed = this.closed
    if (!closed) return true
    return Promise.race([
      closed.then(() => true),
      new Promise<boolean>((resolve) => setTimeout(() => resolve(false), milliseconds)),
    ])
  }

  private settleTerminal(child: ChildProcessWithoutNullStreams, error: Error): void {
    if (this.terminalSettled) return
    this.terminalSettled = true
    if (this.child === child) this.child = undefined
    const terminalError = this.terminationReason ?? error
    this.rejectPending(terminalError)
    this.settleClosed?.()
    this.settleClosed = undefined
    if (!this.stopping && !this.terminating) this.emit('unexpectedExit', error.message)
  }

  private rejectPending(error: Error): void {
    for (const request of this.pending.values()) {
      if (request.timer) clearTimeout(request.timer)
      request.reject(error)
    }
    this.pending.clear()
  }
}

export type { ControllerEvent }
