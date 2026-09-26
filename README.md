# RK3588 RKNPU Linux Mainline Kernel Driver

An out-of-tree Linux kernel driver (`rknpu.ko`) for the Rockchip Neural Processing Unit (RKNPU) core, engineered specifically for modern mainline Linux kernels (tested and validated on mainline kernel `7.2.5` aarch64).

---

## ⚠️ Hardware Compatibility Disclaimer

> [!IMPORTANT]
> **This driver has ONLY been tested and verified on the Orange Pi 5 Plus (Rockchip RK3588).**
> - **Verified Baseline**: **Armbian 26.08.1 trixie** (Debian GNU/Linux 13.6 `trixie`, aarch64) running mainline kernel **Linux 7.2.5**.
> - **Other RK3588 Boards**: Other RK3588/RK3588S boards (e.g., Radxa Rock 5B, FriendlyELEC NanoPC-T6, etc.) are **not guaranteed** to work out-of-the-box. They may require compatible device tree configurations for NPU power domains, regulator rails, reset lines, and clocks.
> - **Other Rockchip SoCs**: Other Rockchip SoCs (such as RK3568, RK3576, RV1106, RV1103) have **NOT** been specifically adapted or validated with this driver codebase.

---

## Verified System & Hardware Environment

All testing, performance benchmarks, and LLM load validations were conducted on the following verified reference setup:

| Component / Layer | Verified Specification & Environment |
|---|---|
| **Hardware Board** | **Orange Pi 5 Plus** (16 GB LPDDR4x) |
| **Target SoC** | **Rockchip RK3588** (Octa-core: 4x Cortex-A76 @ 2.4 GHz + 4x Cortex-A55 @ 1.8 GHz) |
| **NPU Cores** | 3 independent cores (Core 0, Core 1, Core 2; 6 TOPS @ INT8, 800 MHz DVFS) |
| **Linux Distribution** | **Armbian 26.08.1 trixie** (Debian GNU/Linux 13 / `trixie`, `aarch64`) |
| **Kernel Version** | **Mainline Linux 7.2.5** (`#1 SMP PREEMPT CST 2026`, upstream tree) |
| **Device Tree Compatible** | Standard upstream mainline string: `rockchip,rk3588-rknn-core` |
| **Toolchain & Compiler** | GCC 14.2.0 (`Debian 14.2.0-19`) / GNU ld (Binutils) 2.42 |
| **Userspace Runtime** | Rockchip `librknnrt.so` **v2.3.2** / RKNN-Toolkit2 **v2.3.2** |

---

## Key Features & Mainline Adaptations

- **Mainline Device Tree Compatibility**: Binds directly to the upstream mainline device tree compatible string `rockchip,rk3588-rknn-core`. Legacy BSP-specific naming schemes and dead stubs have been completely stripped.
- **Three-Core NPU Concurrency**: Fully drives Core 0, Core 1, and Core 2 in parallel with per-core time tracking, decoupled hardware counters, and automated IRQ affinity routing to high-performance Cortex-A76 cores (CPU4..7).
- **800 MHz DVFS Performance Release**: Integrated standard Linux devfreq driver under `/sys/class/devfreq/fdab0000.npu/` supporting dynamic OPP scaling between 200 MHz and 800 MHz without requiring external voltage step-up.
- **IOMMU Isolation & 4+ GiB Model Support**: Features per-domain IOVA accounting and memory pool isolation, overcoming the 32-bit IOVA space barrier and enabling seamless loading of large language models (tested with 4.1 GiB weights).
- **Native Program Counter (PC) Engine**: Operates with `PC_DMA_BASE = 0` hardware protocol, fully supporting Conv, Depthwise, MatMul, GDN, LayerNorm, and transformer linear projections.
- **Async Submit & Fence Support**: Implements non-blocking job submissions (`RKNPU_JOB_NONBLOCK`) and Linux `dma_fence` synchronization for pipelined inference.
- **Kernel Hygiene & Zero-Leak Teardown**: Clean module lifecycle (`rmmod rknpu`) with clocks and regulators strictly balanced to 0, zero memory leaks, and kernel taint preserved at standard out-of-tree `4096` (`TAINT_OOT_MODULE`).

---

## Official RKNN & Benchmark Validation Results

All results below were measured on an Orange Pi 5 Plus (RK3588), running
mainline Linux 7.2.5 with the NPU at up to 800 MHz. The table records the
maintained result for each workload; when a newer validation supersedes an
older value, the older value is replaced rather than listed as a separate
status entry.

| Benchmark / Demo | Model / Operator Profile | Input Shape & Precision | Measured Performance | Accuracy & Result |
|---|---|---|---|---|
| **`rknn_mobilenet_demo`** | Standard Classification (Conv + Depthwise + Softmax) | `[1, 224, 224, 3]` INT8 | **397.478 FPS** / 2.52 ms, 10,000 loops with mask 7 | **PASS** (Top-1: Class 156; Core 0/1/2 IRQs observed) |
| **`rknn_yolov5_demo`** | Multi-Scale Object Detection (FPN, C3/CSP, SiLU, Anchor Head) | `[1, 640, 640, 3]` INT8 | **22.106 ms** mean, 10 loops; first run 24.551 ms | **PASS** (bus/person detections correct) |
| **`rknn_dynshape_inference`** | Dynamic Shape Resizing (MobileNet V2 Inverted Residuals) | 256x256 / 224x224 / 160x160 INT8 | Normal mask 7: **195.549 / 256.997 / 372.367 FPS**; zero-copy mask 7: **209.797 / 250.447 / 457.208 FPS** | **PASS** (all shapes classify class 155) |
| **`rknn_matmul_api_demo`** | Hardware Matrix Multiplication (MatMul Engine) | Matrix dimension `128x256x512` | FP16→FP32 **6097.56 ops/s**; INT8→INT32 **4484.31 ops/s**; FP16→FP16 **12345.68 ops/s** | **PASS** (demo reference comparisons passed) |
| **`rknn_benchmark`** | Continuous `rknn_create_mem_demo` workload | `[1, 224, 224, 3]` INT8 | **397.478 FPS**, 10,000 loops with mask 7 | **PASS** (no timeout; hardware IRQs observed) |
| **Three-core submit and fence validation** | Combined-mask graph submits and synchronous / NONBLOCK FENCE_OUT wrappers | Core masks 1/2/3/4/7 and runtime ALL=65535 | Explicit masks passed; fence wrappers **75/75** signalled with no timeout | **PASS** (selected-core IRQ routing and task ranges validated) |
| **`mindnano-infer`** | Ling3-v6 W4A8 LLM | 8K context, 128 input / 64 output | Median TTFT **829.875 ms**; decode **11.455 tok/s**; RSS **5767 MiB** | **PASS** (self-check passed; consistent output; all three cores generated IRQs) |

The driver validates shared task-buffer IOVA and per-core task ranges for
combined masks. `AUTO=0` selects one least-loaded core and does not split or
round-robin serial submissions. Missing multicore ranges are rejected rather
than inferred. These tests do not cover every failure mode, FENCE_IN, or fault
injection.

## Memory Bandwidth & Mainline DMC Architecture Facts

### 1. Mainline Linux DMC Status
- **Upstream Driver Status**: As tracked by the Collabora [Rockchip 3588 Upstream Enablement Notes](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588/-/blob/main/mainline-status.md), mainline Linux currently does **not** yet merge or support the RK3588 Dynamic Memory Controller driver (`rockchip,rk3588-dmc` devfreq).
- **Behavioral Impact**: Without the in-kernel DMC devfreq governor, dynamic frequency scaling of the DDR bus and vendor-specific dynamic memory bus QoS priority steering (which dynamically biases DDR bandwidth towards the NPU/GPU under load in BSP 6.1) are absent in mainline Linux.

### 2. Physical Memory Hardware Baseline
- **Firmware-Locked Peak Frequency**: On the Orange Pi 5 Plus reference board, the LPDDR4X memory controller is initialized by bootloader/TF-A firmware and locked at its maximum physical frequency of **2112 MHz** (quad-channel 16-bit, theoretical peak bandwidth ~**33.8 GB/s**).
- **Hardware PMU Verification**: Confirmed via on-chip Rockchip DDR PMU cycle counters:
  ```bash
  perf stat -a -e rockchip_ddr/cycles/ sleep 1
  # Measured: ~2,112,000,000 cycles / 1.00s = 2112 MHz
  ```
- **Measured Host Memory Throughput**:
  - `tinymembench`: Standard `memset` / NEON fill reaches **31.4 GB/s** (~93% of theoretical peak); single-core NEON copy achieves **12.5 GB/s**.
  - `sysbench` memory: 4-thread parallel write across Cortex-A76 cores reaches **45.2 GB/s** (cache-assisted) and single-core sustained write at **11.7 GB/s**.

### 3. Engineering Implications for NPU & LLM Decode
- **DRAM Bandwidth Bounds**: Large Language Model decoding (autoregressive token generation) is strictly memory-bandwidth-bound, as weights must be streamed from DRAM once per token.
- **Driver Memory and Submit Paths**: Because mainline lacks dynamic DMC QoS bias, driver-level software overhead directly impacts effective inference latency. The driver therefore uses:
  1. **Zero-Allocation Stack Reuse**: Bypasses `kmemdup`/`kfree` for blocking submissions to reduce allocator overhead.
  2. **Referenced Token Lookup**: Normal tokens use DRM handle lookup. Legacy DMA tokens acquire a live reference under the index lock and verify file ownership; stale task-object caches are not retained.
  3. **IOMMU Detach Early-Exit**: Avoids global mutex work for inactive subcores during inference cycles.
  4. **Range-Based Cache Synchronization**: Pages-backed buffers synchronize the requested offset and size using stack SG batches, including full-buffer requests. Imported DMA-BUF objects use their exporter CPU-access protocol, while contiguous DMA allocations use the DMA range API. Missing mappings or incomplete backing return an error instead of silently succeeding or falling back to whole-buffer synchronization.

---

## Dependencies (Debian / Ubuntu)

Before compiling the kernel module, ensure that your system has the standard build toolchain and kernel headers matching your running kernel:

```bash
sudo apt update
sudo apt install -y \
    build-essential \
    gcc \
    make \
    bc \
    bison \
    flex \
    libssl-dev \
    libelf-dev \
    pahole \
    linux-headers-$(uname -r)
```

Optional (recommended for automated DKMS management):
```bash
sudo apt install -y dkms
```

---

## Build and Installation

### 1. Build the Kernel Module

Clone the repository and compile using `make`:

```bash
git clone https://github.com/lurenJBD/rk3588-rknn-core.git
cd rk3588-rknn-core
make -j$(nproc)
```

Upon successful compilation, `rknpu.ko` will be generated in the repository root directory.

### 2. Install the Module

Install the module into `/lib/modules/$(uname -r)/extra/` and update module dependencies:

```bash
sudo make install
sudo depmod -a
```

### 3. Load and Verify

Load the driver module:

```bash
sudo modprobe rknpu
```

Verify that the driver probes successfully:

```bash
# Check dmesg output
sudo dmesg | grep -i rknpu

# Verify module information
modinfo rknpu

# Verify devfreq dynamic frequency node (200 - 800 MHz)
cat /sys/class/devfreq/fdab0000.npu/cur_freq

# Verify active NPU core load (supports Core 0, 1, 2)
cat /sys/kernel/debug/rknpu/load
```

---

## Driver Module Parameters

The module provides the following runtime parameters (configurable via `modprobe rknpu <param>=<value>` or `/etc/modprobe.d/rknpu.conf`):

| Parameter | Type | Default | Description |
|---|---|---|---|
| `target_freq_mhz` | `int` | `800` | Target operational frequency in MHz (safe baseline range: 200–800 MHz). |
| `bypass_soft_reset` | `int` | `0` | Set to `1` to bypass hardware soft reset upon submit (default `0`). |
| `rknpu_debug_log` | `bool` | `false` | Enable verbose per-submit/per-IRQ debug logging (can be toggled via `/sys/module/rknpu/parameters/rknpu_debug_log`). |
| `per_fd_domain` | `bool` | `true` | Enable per-FD IOMMU domain isolation for client processes. |
| `mem_profile` | `bool` | `false` | Load-time diagnostics: memory lookup/sync counters and timing at read-only debugfs `rknpu/mem_stats`. Keep disabled for performance measurements. |

Range-based cache sync is the default and has no enable/compatibility switch.
Imported DMA-BUF objects use their exporter's begin/end CPU-access protocol;
contiguous DMA allocations use their existing DMA range API.

---

## Clean Unload

The driver supports clean unloading without leaving residue clocks or dangling workqueues:

```bash
sudo rmmod rknpu
```

Verify that the kernel remains clean and untainted:

```bash
# Tainted value should remain 4096 (OOT module flag only; no DIE / BUG bits)
cat /proc/sys/kernel/tainted
```

---

## Acknowledgments

Special thanks and acknowledgment to the [rockchip-npu-notes](https://github.com/gregordinary/rockchip-npu-notes) project for its comprehensive hardware documentation and reverse-engineering insights into the Rockchip RK3588 NPU (including NVDLA-derived architectures, MRDMA behaviors, register command paths, and clock subsystems).

---

## License

This project is licensed under the **GPL-2.0 License** - see the [LICENSE](LICENSE) file for details.
