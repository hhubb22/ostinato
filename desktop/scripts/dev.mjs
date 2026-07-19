import { spawn } from 'node:child_process'
import process from 'node:process'

const children = []
const run = (command, args, env = process.env) => {
  const child = spawn(command, args, { stdio: 'inherit', env })
  children.push(child)
  return child
}

run('npm', ['exec', 'vite', '--', '--host', '127.0.0.1', '--port', '5173'])
for (let attempt = 0; attempt < 100; attempt += 1) {
  try {
    const response = await fetch('http://127.0.0.1:5173/')
    if (response.ok) break
  } catch {
    // Vite has not started listening yet.
  }
  await new Promise((resolve) => setTimeout(resolve, 100))
  if (attempt === 99) throw new Error('Vite did not become ready')
}

run('npm', ['exec', 'electron', '--', '.'], {
  ...process.env,
  OSTINATO_DEV_URL: 'http://127.0.0.1:5173/',
})

const stop = () => children.forEach((child) => child.kill('SIGTERM'))
process.on('SIGINT', stop)
process.on('SIGTERM', stop)
for (const child of children) child.once('exit', stop)
await Promise.all(children.map((child) => new Promise((resolve) => child.once('exit', resolve))))
