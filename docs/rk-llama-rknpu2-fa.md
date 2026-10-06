# rk-llama.cpp Flash-Attention (Qwen3-0.6B) Benchmark Result

Performance evaluation of the RKNPU2 hardware acceleration backend in `rk-llama.cpp` (`opi5-rknpu2-embed-opt` branch, commit `8cb318904`), executed on the reference platform.

- Upstream Project: <https://github.com/pty819/rk-llama.cpp/tree/opi5-rknpu2-embed-opt>
- Hardware & Platform: Orange Pi 5 Plus (Rockchip RK3588, 3 NPU cores, 6 TOPS INT8), mainline Linux 7.2.5. Full platform specifications are documented in [compatibility.md](compatibility.md).
- Runtime & Driver: `librknnrt.so` v2.3.2, mainline `rknpu` driver (`C0ED23E064B11DF515D80FF`).

---

## 1. Workload & Model Architecture

The workload evaluates the standalone NPU Flash-Attention acceleration engine tailored for **Qwen3-0.6B** decoder-only embedding models (such as `jina-embeddings-v5-small` / `v5-small-retrieval-Q8_0.gguf`).

| Architectural Parameter | Value | Description |
|---|---|---|
| Head Dimension ($D$) | 128 | Per-head vector dimension |
| Query Heads ($H$) | 16 | Number of attention query heads |
| Key/Value Heads ($H_{kv}$) | 8 | Grouped-query attention (GQA ratio 2:1) |
| Attention Softmax Scale | 0.125 ($1/\sqrt{128}$) | Standard query-key scaling factor |
| Acceleration API | `rknn_matmul_api` | Hardware INT8/FP16 native tensor matrix multiplication |
| Concurrency Dispatcher | `rknpu-fa` worker pool | Multi-threaded task dispatching across NPU hardware contexts |
| Cache Maintenance | `rknn_mem_sync` | Synchronous device-host memory synchronization |

### Evaluated Scheduling Strategies

- **Adaptive Tile Scheduling (`RKNPU_FA_ADAPTIVE=1`)**: Dynamically partitions attention computation into variable tile dimensions matching the sequence length, reducing redundant zero-padding and dispatch overhead on short/medium context lengths.
- **Baseline Fixed Tile Scheduling (`RKNPU_FA_ADAPTIVE=0`)**: Uses fixed static tile geometry across all token lengths.

---

## 2. Measured Benchmark Results

All benchmarks were executed pinned to the Cortex-A76 performance cluster (`taskset -c 4-7`) with CPU and NPU governors set to `performance` (NPU targeted at 800 MHz OPP). Latency represents the median execution time over 5 warm iterations.

| Pattern | Sequence Length ($N$) | Adaptive Tile (ms) | Baseline Fixed (ms) | Speedup | Max Absolute Error vs FP32 | Status |
|---|---|---|---|---|---|---|
| Causal | 53 | **0.7426** | 1.9184 | **2.58x** | 0.0002521 | PASS |
| Causal | 64 | **0.7931** | 1.8950 | **2.39x** | 0.0001713 | PASS |
| Causal | 65 | **1.1617** | 1.9195 | **1.65x** | 0.0001713 | PASS |
| Causal | 127 | **1.6103** | 2.3638 | **1.47x** | 0.0000938 | PASS |
| Causal | 128 | **1.5263** | 2.3524 | **1.54x** | 0.0001181 | PASS |
| Causal | 129 | 2.4909 | 2.4233 | 0.97x | 0.0001181 | PASS |
| Causal | 255 | 3.7863 | 3.2630 | 0.86x | 0.0000881 | PASS |
| Causal | 256 | 5.3141 | 3.3108 | 0.62x | 0.0000758 | PASS |
| Causal | 257 | **4.5087** | 6.0785 | **1.35x** | 0.0000758 | PASS |
| Causal | 547 | **9.9832** | 13.2958 | **1.33x** | 0.0000432 | PASS |
| Causal | 1023 | 22.2868 | 23.9143 | 1.07x | 0.0000319 | PASS |
| Causal | 2048 | 73.5814 | 76.2695 | 1.04x | 0.0000318 | PASS |
| Multi-Doc | 192 | **2.4550** | 3.5806 | **1.46x** | 0.0001885 | PASS |
| Multi-Doc | 387 | 7.6644 | 8.1821 | 1.07x | 0.0001110 | PASS |
| Sparse | 53 | **1.1781** | 2.7666 | **2.35x** | 0.0002927 | PASS |
| Sparse | 257 | **7.3094** | 9.1683 | **1.25x** | 0.0001003 | PASS |

### Numerical Stability & Accuracy

Every execution pass was verified against an unquantized FP32 CPU reference implementation:
- **Maximum Error Bound**: Across all evaluated sequence lengths and patterns, the maximum absolute error remained $\le 0.0002927$ (sub-$0.03\%$), well within the accepted tolerance bound of $0.006$ for half-precision floating-point arithmetic.
- **Finite Output Verification**: All output tensor elements verified finite with zero `NaN` or `Inf` occurrences.

---

## 3. Independent Verification Evidence

Hardware execution was independently validated without relying exclusively on binary exit codes:

1. **Device Enumeration**:
   `llama-bench --list-devices` reports active discovery of the Rockchip NPU hardware backend:
   ```text
   Available devices:
     RKNPU: Rockchip NPU (0 MiB, 0 MiB free)
   ```
2. **Hardware Interrupt Delta**:
   Hardware interrupt counters across `/proc/interrupts` confirmed balanced load and active execution across all three physical NPU cores throughout the benchmark run:
   - IRQ 112 (`fdab0000.npu`, Core 0): +8,185 interrupts
   - IRQ 113 (`fdac0000.npu`, Core 1): +7,227 interrupts
   - IRQ 114 (`fdad0000.npu`, Core 2): +7,436 interrupts
   - Total hardware interrupts generated: 22,848
3. **Kernel Stability**:
   - Zero kernel oops, fault, or IOMMU page fault entries observed in `dmesg`.
   - Kernel taint status remained unchanged at `4096` (`TAINT_OOT_MODULE`).

---

## 4. Reproduction

To reproduce the benchmark on a compatible system:

```bash
# Compare Adaptive Tile vs Baseline across all token lengths
./test-suite/run-rk-llama-bench.sh --compare

# Quick check on N=53 tokens
./test-suite/run-rk-llama-bench.sh --quick

# List detected NPU acceleration devices
./test-suite/run-rk-llama-bench.sh --devices
```
