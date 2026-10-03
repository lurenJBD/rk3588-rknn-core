# Module Parameters & Debugfs

Parameters can be set at load time (`modprobe rknpu <param>=<value>`) or via
`/etc/modprobe.d/rknpu.conf`.

| Parameter | Type | Default | Description |
|---|---|---|---|
| `target_freq_mhz` | `int` | `800` | Target operating frequency in MHz (safe range 200-800). |
| `bypass_soft_reset` | `int` | `0` | Set to `1` to bypass the hardware soft reset on submit. **Warning:** with `1`, job timeout/abort recovery cannot quiesce the core (`rknpu_reset_begin()` returns `-EOPNOTSUPP`), so the core stays owned by the dead job and every later submit on it queues forever. Debug only. |
| `rknpu_debug_log` | `bool` | `false` | Verbose per-submit / per-IRQ logging; also togglable via `/sys/module/rknpu/parameters/rknpu_debug_log`. |
| `per_fd_domain` | `bool` | `true` | Per-FD IOMMU domain isolation for client processes. |
| `mem_profile` | `bool` | `false` | Load-time diagnostics: memory lookup/sync counters and timing at read-only debugfs `rknpu/mem_stats`. Leave disabled for performance measurements. |

Range-based cache sync is the default and has no enable/compatibility switch.
Imported DMA-BUF objects use their exporter's begin/end CPU-access protocol;
contiguous DMA allocations use the existing DMA range API.

## Debugfs

| Node | Contents |
|---|---|
| `/sys/kernel/debug/rknpu/load` | Per-core utilisation |
| `/sys/kernel/debug/rknpu/freq` | Current target frequency |
| `/sys/kernel/debug/rknpu/power` | Manual power control (`on` / `off`) |
| `/sys/kernel/debug/rknpu/mem_stats` | Memory lookup/sync counters (read-only; populated when `mem_profile=1`) |
