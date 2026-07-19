<script lang="ts">
  import { onMount } from 'svelte'
  import ConnectionPanel from './components/ConnectionPanel.svelte'
  import PortTable from './components/PortTable.svelte'
  import VirtualStats from './components/VirtualStats.svelte'
  import { applySnapshot } from './lib/stores'

  let page: 'live' | 'performance' = 'live'

  onMount(() => {
    let disposed = false
    window.ostinato.getSnapshot().then((value) => {
      if (!disposed) applySnapshot(value)
    })
    const unsubscribe = window.ostinato.subscribeSnapshot(applySnapshot)
    return () => {
      disposed = true
      unsubscribe()
    }
  })
</script>

<header class="app-header">
  <div>
    <p class="product">OSTINATO</p>
    <h1>Desktop controller validation</h1>
  </div>
  <nav aria-label="Views">
    <button data-testid="live-tab" class:active={page === 'live'} on:click={() => { page = 'live' }}>Live ports</button>
    <button data-testid="performance-tab" class:active={page === 'performance'} on:click={() => { page = 'performance' }}>
      Virtual table lab
    </button>
  </nav>
</header>

<main>
  {#if page === 'live'}
    <ConnectionPanel />
    <PortTable />
  {:else}
    <VirtualStats />
  {/if}
</main>
