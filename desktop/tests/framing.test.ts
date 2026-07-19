import { describe, expect, it } from 'vitest'
import {
  BINARY_FRAME_TYPE,
  FrameDecoder,
  FrameProtocolError,
  JSON_FRAME_TYPE,
  MAX_FRAME_SIZE,
  encodeJsonFrame,
} from '../electron/framing'

describe('controller stdio framing', () => {
  it('decodes partial and coalesced frames', () => {
    const first = encodeJsonFrame({ first: true })
    const second = encodeJsonFrame({ second: 2 })
    const decoder = new FrameDecoder()
    expect(decoder.push(first.subarray(0, 3))).toEqual([])
    const frames = decoder.push(Buffer.concat([first.subarray(3), second]))
    expect(frames).toHaveLength(2)
    expect(frames.map((frame) => JSON.parse(frame.payload.toString()))).toEqual([
      { first: true },
      { second: 2 },
    ])
    expect(frames.every((frame) => frame.type === JSON_FRAME_TYPE)).toBe(true)
  })

  it('retains the reserved binary frame type without interpreting it', () => {
    const frame = Buffer.from([0, 0, 0, 4, BINARY_FRAME_TYPE, 1, 2, 3])
    expect(new FrameDecoder().push(frame)).toEqual([
      { type: BINARY_FRAME_TYPE, payload: Buffer.from([1, 2, 3]) },
    ])
  })

  it('rejects oversized and empty bodies before waiting for payload', () => {
    const oversized = Buffer.alloc(4)
    oversized.writeUInt32BE(MAX_FRAME_SIZE + 1)
    expect(() => new FrameDecoder().push(oversized)).toThrow(FrameProtocolError)
    expect(() => new FrameDecoder().push(Buffer.alloc(4))).toThrow(/type byte/)
  })
})
