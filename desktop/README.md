# Electron/Svelte validation slice

This directory is the stage 7.1 desktop technical slice. It runs beside the
existing Qt Widgets client; it does not replace or remove that client and does
not change the drone RPC schema.

## Architecture and ownership

```text
Svelte 5 renderer
  └─ window.ostinato (five typed operations only)
       └─ sandboxed preload/contextBridge
            └─ fixed Electron IPC channels + main-process validation
                 └─ supervised ostinato-controller child
                      └─ bounded framed stdin/stdout
                           └─ existing ostinato_client_core
                                └─ unchanged drone protobuf RPC
```

- `ostinato-controller` owns the `ClientSession` and publishes authoritative
  connection, port, and 500 ms stats snapshots. The renderer does not recreate
  connect/reconnect/apply rules.
- Electron main owns child creation, request deadlines, stderr logging, clean
  shutdown, TERM/KILL fallback, response correlation, controller output schema
  validation, and renderer sender/argument validation.
- The renderer splits connection, ports, stats, and selection into separate
  Svelte stores. A stats snapshot enters the stats store once; keyed rows then
  update from that batch.
- The 2,000-row performance view is labelled synthetic and has no path into the
  real controller stores. It mounts only a viewport and overscan window.

## Controller stdio protocol

Controller stdout contains frames only; diagnostics use stderr. Each frame is:

| Bytes | Meaning |
| --- | --- |
| 0..3 | unsigned big-endian body length |
| 4 | frame type (`1` JSON control, `2` reserved binary) |
| 5.. | payload |

The body length includes the one-byte type. It must be between 1 and 1,048,576
bytes. The decoder accepts partial and coalesced frames, rejects a bad length
before waiting for its payload, and disconnects on an oversized stream. JSON
requests have `{kind:"request", id, command, args}`; responses preserve `id`
and have a boolean `ok`; asynchronous messages have
`{kind:"event", event, payload}`. Only `connect`, `disconnect`, `reconnect`, and
`shutdown` are accepted. The binary type is reserved so future bulk payloads do
not require changing framing.

Both boundaries validate exact object shapes. Hosts must be numeric IPv4/IPv6,
ports are integral 1–65535 values, IDs and strings are bounded, port IDs are
unique uint32 values, and 64-bit counters cross JSON as decimal strings.

## Linux commands

From `desktop/`:

```bash
npm ci
npm run dev              # builds drone/controller, then Vite + Electron
npm run typecheck
npm run lint
npm run test:unit
npm run test:e2e         # xvfb + actual stage-A drone + Electron
npm run build
npm run package:linux    # release/linux-unpacked
npm run measure:linux    # measures that unpacked app through /proc
```

The CMake side can be built independently without Qt:

```bash
cmake -S .. -B ../build-desktop -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DOSTINATO_BUILD_CLIENT=OFF -DOSTINATO_BUILD_QT_TESTS=OFF
cmake --build ../build-desktop --parallel
ctest --test-dir ../build-desktop --output-on-failure
```

Development finds `../build-desktop/ostinato-controller` relative to the app;
`OSTINATO_CONTROLLER_PATH` is accepted only by an unpackaged development app.
The package always uses `resources/controller/ostinato-controller`, regardless
of environment. A Vite URL is likewise accepted only for an unpackaged app and
only as a plain loopback HTTP origin. Production loads its bundled `file:` page.

## Stage 7.1 Linux measurements

Measured 2026-07-19 in the x86-64 E2B orb under Xvfb. These are one-run,
coarse engineering checks, not benchmark claims. `measure:linux` starts an
actual drone without configuring or transmitting on a NIC, launches the
unpacked app, waits for the renderer, sums each Electron process-tree member's
`/proc/<pid>/smaps_rollup`, connects, observes another stats sequence, and
checks the controller PID after close.

| Check | Result |
| --- | ---: |
| Unpacked directory | 316,032,112 bytes (302 MiB) |
| `app.asar` | 4,882,800 bytes |
| packaged controller | 560,992 bytes |
| cold launch to renderer controls | 591 ms |
| disconnected idle, 6 processes | 561.7 MiB summed RSS / 273.6 MiB summed PSS |
| connected 500 ms updates, 6 processes | 583.2 MiB summed RSS / 284.5 MiB summed PSS |
| virtual model / mounted rows | 2,000 / 19 |
| observed virtual update-to-animation-frame | 2.2 ms |
| controller after app close | reaped |

The packaged runtime was Electron 41.5.2, embedded Node 24.15.0, and Chromium
146.0.7680.216. The build host used Node 20.9.0; the renderer used Svelte
5.56.6, Vite 6.4.3, and TypeScript 5.9.3.

The orb lacked raw-capture capability for normal NICs. The real drone still
started its unmodified service and exposed its usable `dbus-system` pcap port;
the E2E hydrated that port, observed multiple real stats RPC snapshots,
disconnected, reconnected and rehydrated, then closed without sending traffic.

## Security posture and current limits

- Renderer settings are `contextIsolation: true`, `nodeIntegration: false`, and
  `sandbox: true`; tests assert the settings at runtime and verify that renderer
  `process` and `require` are absent.
- Preload exposes no `ipcRenderer`, filesystem, child process, generic command,
  RPC method number, or arbitrary event name.
- CSP permits scripts from `self` only and forbids objects, base URL changes,
  ancestors, unsafe inline script, and unsafe evaluation. New windows and
  renderer navigation are denied.
- Shutdown is bounded. Controller RPC operations use finite deadlines, main
  requests use finite deadlines, Electron escalates shutdown from protocol to
  TERM/KILL, and Linux `PR_SET_PDEATHSIG(SIGKILL)` covers abrupt parent death.
  No detached thread or detached child is used.
- This slice was built and exercised only on Linux x86-64. Packaging paths and
  process signalling need explicit Windows/macOS implementation and testing.
  The current C++ controller target is POSIX-only because its underlying
  `ostinato_client_core` is POSIX-only. IPv6 input validation is covered, but
  this orb's actual drone smoke used IPv4 loopback.
