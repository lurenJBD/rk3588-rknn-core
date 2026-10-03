# Memory Bandwidth & Mainline DMC Facts

## 1. Mainline DMC status

- As tracked by the Collabora [Rockchip 3588 upstream enablement notes](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md),
  mainline Linux does not yet merge or support the RK3588 Dynamic Memory
  Controller devfreq driver (`rockchip,rk3588-dmc`).
- Consequence: without the in-kernel DMC devfreq governor, dynamic DDR
  frequency scaling and the vendor dynamic memory-QoS bias toward the NPU/GPU
  (present in the BSP 6.1 kernel) are absent.

## 2. Physical memory baseline

- The LPDDR4X controller is initialised by bootloader/TF-A and locked at its
  maximum physical frequency of **2112 MHz** (quad-channel 16-bit, ~**33.8 GB/s**
  theoretical peak).
- Hardware PMU verification on the reference board:
  ```sh
  perf stat -a -e rockchip_ddr/cycles/ sleep 1
  # ~2,112,000,000 cycles / 1.00 s = 2112 MHz
  ```
- Measured host memory throughput:
  - `tinymembench`: NEON `memset` ~**31.4 GB/s** (~93% of peak); single-core NEON copy ~**12.5 GB/s**.
  - `sysbench` memory: 4-thread write across A76 cores ~**45.2 GB/s** (cache-assisted); single-core sustained write ~**11.7 GB/s**.

## 3. Engineering impact for NPU / LLM decode

- LLM autoregressive decoding is strictly memory-bandwidth-bound: weights are
  streamed from DRAM once per token.
- Because mainline lacks dynamic DMC QoS bias, driver-level software overhead
  directly affects inference latency. The driver therefore uses:
  1. **Zero-allocation stack reuse** — avoids `kmemdup`/`kfree` on the blocking submit path.
  2. **Referenced token lookup** — normal tokens use DRM handle lookup; legacy DMA tokens acquire a live reference under the index lock and verify file ownership; no stale task-object cache is retained.
  3. **IOMMU detach early-exit** — avoids global-mutex work for inactive subcores during inference.
  4. **Range-based cache sync** — pages-backed buffers sync the requested offset/size via stack SG batches; imported DMA-BUFs use the exporter CPU-access protocol; contiguous DMA allocations use the DMA range API. Missing mappings or incomplete backing return an error instead of silently succeeding.
