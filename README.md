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

All benchmarks below were executed on an **Orange Pi 5 Plus (RK3588)** running mainline Linux **7.2.5** with the NPU operating at **800 MHz**:

| Benchmark / Demo | Model / Operator Profile | Input Shape & Precision | Measured Performance | Accuracy & Result |
|---|---|---|---|---|
| **`rknn_mobilenet_demo`** | Standard Classification (Conv + Depthwise + Softmax) | `[1, 224, 224, 3]` INT8 | **3.10 ms** latency (**~322.8 FPS** peak) | **PASS** (Top-1: Class 156 @ 0.884766) |
| **`rknn_yolov5_demo`** | Multi-Scale Object Detection (FPN, C3/CSP, SiLU, Anchor Head) | `[1, 640, 640, 3]` INT8 | **25.12 ms** latency (**~40 FPS**) | **PASS** (bus @ 0.69, person @ 0.88/0.87/0.84) |
| **`rknn_dynshape_inference`** | Dynamic Shape Resizing (MobileNet V2 Inverted Residuals) | Dynamic 3 shapes: 256x256 / 224x224 / 160x160 INT8 | 256x256 @ 129.3 FPS (peak 185.2 FPS / 5.40 ms)<br>224x224 @ 191.4 FPS (peak 227.1 FPS / 4.40 ms)<br>160x160 @ **311.9 FPS** (peak 375.4 FPS / 2.66 ms) | **PASS** (Smooth dynamic shape switching, Top-1: Class 155) |
| **`rknn_matmul_api_demo`** | Hardware Matrix Multiplication (MatMul Engine) | Matrix Dimension `128x256x512` | FP16 $\to$ FP16: **0.09 ms** (11,236 ops/s, peak 13,333 ops/s)<br>INT8 $\to$ INT32: **0.22 ms** (4,525 ops/s, peak 5,618 ops/s)<br>FP16 $\to$ FP32: **0.16 ms** (6,250 ops/s) | **PASS** (Bit-exact output matching reference calculations) |
| **`rknn_benchmark`** | 100-loop continuous stress benchmark (`rknn_create_mem_demo`) | `[1, 224, 224, 3]` INT8 | Latency **3.10 ms** (Average **~280 FPS**, peak **322.8 FPS**) | **PASS** (0 drops, 0 timeouts, 100% IRQs handled on CPU4..7) |
| **`mindnano-infer`** | 7.9B-parameter LLM (Ling-3.0-tiny W4A8, 4.4 GiB package) | 4K context, 3-core concurrent decoding | Weight load: **4.1 GiB in 33.17s** (~135 MB/s IOVA)<br>Throughput: **11.0 – 13.6 tokens/sec** | **PASS** (45,224 IRQs across Cores 0/1/2, 0 timeouts, 3 complete conversations generated) |

---

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
- **Fastpath Driver Mitigations**: Because mainline lacks dynamic DMC QoS bias, driver-level software overhead directly impacts effective inference latency. This driver incorporates dedicated submit fastpaths:
  1. **Zero-Allocation Stack Reuse**: Bypasses `kmemdup`/`kfree` for blocking submissions to eliminate kernel allocator contention.
  2. **Task Token Hot-Cache**: Directly resolves repeated task objects without O(N) IDR scans.
  3. **IOMMU Detach Early-Exit**: Eliminates global mutex lock contention across inactive subcores during inference cycles.

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
