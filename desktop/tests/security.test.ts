import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import { controllerPath, secureWebPreferences, validatedDevelopmentUrl } from '../electron/security'

const __dirname = path.dirname(fileURLToPath(import.meta.url))

describe('Electron renderer security', () => {
  it('enforces isolated sandboxed renderer preferences', () => {
    expect(secureWebPreferences('/safe/preload.js')).toMatchObject({
      preload: '/safe/preload.js',
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    })
  })

  it('allows only a loopback HTTP development origin', () => {
    expect(validatedDevelopmentUrl('http://127.0.0.1:5173/')).toBe('http://127.0.0.1:5173/')
    expect(() => validatedDevelopmentUrl('https://example.com/')).toThrow(/loopback/)
    expect(() => validatedDevelopmentUrl('http://127.0.0.1:5173/path')).toThrow(/loopback/)
  })

  it('ignores executable overrides in production', () => {
    expect(controllerPath({
      isPackaged: true,
      appPath: '/app',
      resourcesPath: '/resources',
      developmentOverride: '/tmp/untrusted',
    })).toBe(path.join('/resources', 'controller', 'ostinato-controller'))
  })

  it('ships a CSP without remote scripts or unsafe evaluation', () => {
    const html = fs.readFileSync(path.resolve(__dirname, '..', 'index.html'), 'utf8')
    const csp = html.match(/content="([^"]*script-src[^"]*)"/)?.[1] ?? ''
    expect(csp).toContain("script-src 'self'")
    expect(csp).not.toContain('unsafe-inline')
    expect(csp).not.toContain('unsafe-eval')
    expect(csp).not.toMatch(/script-src[^;]*https?:/)
    expect(csp).toContain("object-src 'none'")
  })
})
