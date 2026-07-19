export const MAX_FRAME_SIZE = 1024 * 1024
export const JSON_FRAME_TYPE = 1
export const BINARY_FRAME_TYPE = 2

export interface Frame {
  type: number
  payload: Buffer
}

export class FrameProtocolError extends Error {}

export class FrameDecoder {
  private buffered = Buffer.alloc(0)

  push(chunk: Buffer): Frame[] {
    if (chunk.length > 0) {
      this.buffered = this.buffered.length === 0
        ? Buffer.from(chunk)
        : Buffer.concat([this.buffered, chunk])
    }

    const frames: Frame[] = []
    let offset = 0
    while (this.buffered.length - offset >= 4) {
      const bodyLength = this.buffered.readUInt32BE(offset)
      if (bodyLength < 1) {
        throw new FrameProtocolError('frame body must contain a type byte')
      }
      if (bodyLength > MAX_FRAME_SIZE) {
        throw new FrameProtocolError('frame exceeds configured maximum')
      }
      if (this.buffered.length - offset - 4 < bodyLength) break
      frames.push({
        type: this.buffered[offset + 4],
        payload: Buffer.from(this.buffered.subarray(offset + 5, offset + 4 + bodyLength)),
      })
      offset += 4 + bodyLength
    }
    if (offset > 0) this.buffered = Buffer.from(this.buffered.subarray(offset))
    if (this.buffered.length > MAX_FRAME_SIZE + 4) {
      throw new FrameProtocolError('frame buffer exceeds configured maximum')
    }
    return frames
  }
}

export function encodeJsonFrame(value: unknown): Buffer {
  const payload = Buffer.from(JSON.stringify(value), 'utf8')
  const bodyLength = payload.length + 1
  if (bodyLength > MAX_FRAME_SIZE) {
    throw new FrameProtocolError('outgoing frame exceeds configured maximum')
  }
  const output = Buffer.allocUnsafe(bodyLength + 4)
  output.writeUInt32BE(bodyLength, 0)
  output[4] = JSON_FRAME_TYPE
  payload.copy(output, 5)
  return output
}
