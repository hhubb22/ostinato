<script lang="ts">
  import { ports, selection, stats } from '../lib/stores'
</script>

<section class="panel ports-panel" aria-labelledby="ports-title">
  <div class="panel-heading">
    <div>
      <p class="eyebrow">Authoritative controller snapshot</p>
      <h2 id="ports-title">Ports</h2>
    </div>
    <div class="sample" data-testid="stats-sequence">
      sample {$stats.sequence} · {$stats.sampledAt ? new Date($stats.sampledAt).toLocaleTimeString() : 'not sampled'}
    </div>
  </div>

  <div class="table-wrap">
    <table>
      <thead>
        <tr>
          <th>ID</th><th>Name</th><th>Link</th><th>TX</th><th>Capture</th><th>Dirty</th>
          <th>RX packets</th><th>RX pps</th><th>TX packets</th><th>TX pps</th><th>Errors</th>
        </tr>
      </thead>
      <tbody data-testid="port-rows">
        {#each $ports as port (port.id)}
          {@const current = $stats.ports[port.id]}
          <tr class:selected={$selection === port.id} on:click={() => selection.set(port.id)}>
            <td><code>{port.id}</code></td>
            <td>{port.name || 'unnamed'}</td>
            <td><span class="link link-{current?.link ?? port.link}">{current?.link ?? port.link}</span></td>
            <td>{current?.transmit ?? port.transmit ? 'on' : 'off'}</td>
            <td>{current?.capture ?? port.capture ? 'on' : 'off'}</td>
            <td>{port.dirty ? 'yes' : 'no'}</td>
            <td>{current?.rxPkts ?? '—'}</td>
            <td>{current?.rxPps ?? '—'}</td>
            <td>{current?.txPkts ?? '—'}</td>
            <td>{current?.txPps ?? '—'}</td>
            <td>{current?.rxErrors ?? '—'}</td>
          </tr>
        {:else}
          <tr class="empty"><td colspan="11">Connect to a drone to hydrate its port snapshot.</td></tr>
        {/each}
      </tbody>
    </table>
  </div>
</section>
