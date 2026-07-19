<script lang="ts">
  import { connection } from '../lib/stores'

  let host = '127.0.0.1'
  let port = 7878
  let actionError = ''
  let busy = false

  async function action(operation: () => Promise<void>): Promise<void> {
    busy = true
    actionError = ''
    try {
      await operation()
    } catch (error) {
      actionError = error instanceof Error ? error.message : String(error)
    } finally {
      busy = false
    }
  }
</script>

<section class="panel connection-panel" aria-labelledby="connection-title">
  <div class="panel-heading">
    <div>
      <p class="eyebrow">Controller sidecar</p>
      <h2 id="connection-title">Drone connection</h2>
    </div>
    <div class="status status-{$connection.state}" data-testid="connection-status">
      <span class="status-dot"></span>
      {$connection.state}
    </div>
  </div>

  <form on:submit|preventDefault={() => action(() => window.ostinato.connect({ host, port }))}>
    <label>
      Numeric IPv4 / IPv6
      <input data-testid="endpoint-host" bind:value={host} maxlength="64" spellcheck="false" />
    </label>
    <label>
      Port
      <input data-testid="endpoint-port" bind:value={port} type="number" min="1" max="65535" />
    </label>
    <button data-testid="connect" class="primary" type="submit" disabled={busy || $connection.state === 'connecting'}>
      Connect
    </button>
    <button data-testid="disconnect" type="button" disabled={busy || $connection.state !== 'connected'}
      on:click={() => action(() => window.ostinato.disconnect())}>
      Disconnect
    </button>
    <button data-testid="reconnect" type="button" disabled={busy || !$connection.endpoint}
      on:click={() => action(() => window.ostinato.reconnect())}>
      Reconnect
    </button>
  </form>

  <div class="connection-details">
    <span data-testid="connection-message">{$connection.message ?? 'Ready'}</span>
    {#if $connection.endpoint}
      <code>{$connection.endpoint.host}:{$connection.endpoint.port}</code>
    {/if}
    {#if $connection.controllerPid}
      <span class="pid">controller pid <strong data-testid="controller-pid">{$connection.controllerPid}</strong></span>
    {/if}
  </div>
  {#if actionError}<p class="inline-error" role="alert">{actionError}</p>{/if}
</section>
