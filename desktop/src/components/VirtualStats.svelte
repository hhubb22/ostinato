<script lang="ts">
  import { onMount } from 'svelte'

  const rowCount = 2000
  const rowHeight = 30
  const viewportHeight = 420
  const overscan = 5
  const rows = Array.from({ length: rowCount }, (_, index) => ({
    id: index + 1,
    name: `synthetic-stream-${String(index + 1).padStart(4, '0')}`,
  }))

  let scrollTop = 0
  let tick = 0
  let updateMs = 0
  $: start = Math.max(0, Math.floor(scrollTop / rowHeight) - overscan)
  $: end = Math.min(rowCount, Math.ceil((scrollTop + viewportHeight) / rowHeight) + overscan)
  $: visible = rows.slice(start, end)

  onMount(() => {
    const timer = window.setInterval(() => {
      const started = performance.now()
      tick += 1
      requestAnimationFrame(() => { updateMs = performance.now() - started })
    }, 500)
    return () => window.clearInterval(timer)
  })
</script>

<section class="panel synthetic" aria-labelledby="synthetic-title">
  <div class="panel-heading">
    <div>
      <p class="eyebrow"><span class="synthetic-badge">Synthetic only</span> · isolated performance harness</p>
      <h2 id="synthetic-title">2,000-row virtual stream stats</h2>
    </div>
    <div class="sample" data-testid="virtual-metrics">
      model <strong data-testid="virtual-model-count">{rowCount}</strong> · DOM
      <strong data-testid="virtual-dom-count">{visible.length}</strong> · frame {updateMs.toFixed(1)} ms
    </div>
  </div>
  <p class="perf-note">
    This generated data never enters the controller or real-drone stores. Scroll while 500 ms updates continue;
    only the visible window plus five overscan rows on each side is mounted.
  </p>
  <div class="virtual-header" aria-hidden="true">
    <span>ID</span><span>Stream</span><span>RX packets</span><span>TX packets</span><span>Rate</span>
  </div>
  <div
    class="virtual-viewport"
    data-testid="virtual-viewport"
    on:scroll={(event) => { scrollTop = event.currentTarget.scrollTop }}
  >
    <div class="virtual-spacer" style:height={`${rowCount * rowHeight}px`}>
      <div class="virtual-window" style:transform={`translateY(${start * rowHeight}px)`}>
        {#each visible as row (row.id)}
          <div class="virtual-row" data-testid="synthetic-row" style:height={`${rowHeight}px`}>
            <code>{row.id}</code>
            <span>{row.name}</span>
            <span>{row.id * 1000 + tick * 17}</span>
            <span>{row.id * 710 + tick * 11}</span>
            <span>{(row.id * 13 + tick) % 10000} pps</span>
          </div>
        {/each}
      </div>
    </div>
  </div>
</section>
