# Performance Scores

Measured on the reference platform (see [compatibility.md](compatibility.md)) with the NPU at up to 800 MHz and a three-core combined mask (`core_mask=7`), unless stated otherwise.

The reference configuration is the Orange Pi 5 Plus (Rockchip RK3588, 3 cores, 6 TOPS INT8) running mainline Linux 7.2.5 on Armbian 26.08.1 trixie (`aarch64`) with GCC 14.2.0 and `librknnrt.so` v2.3.2 (RKNN-Toolkit2 commit `59a913d`). Full hardware and kernel details are documented in [compatibility.md](compatibility.md).

All workloads run with CPU and NPU governors set to `performance` (NPU targeted at 800 MHz OPP).

> [!NOTE]
> Test environment: **2026-10-13**, DKMS **`0.9.8-7`**, driver commit **`842e9e93d721915fdbae63a1bd040f0f934096a8`**, module srcversion **`80D94120AADEE0271B5AFD8`**. The official demos were run through `test-suite/run-official-demo.sh` with CPU/NPU `performance` and `core_mask=7` unless noted.
>
> The demos report a per-process `Avg FPS`/`Average FPS`; the table uses:
> - **median of 4 process runs** for `rknn_benchmark`, dynamic-shape, and `128x256x512` MatMul;
> - **mean of 4 process-run averages** for YOLOv5 (each process run averages 10 loops);
> - **single-run values** for MobileNet and `4x64x32` MatMul.
>
> Ranges in the table are min–max across those runs. Rows without the above provenance are historical validation results.

## Workload Results

The workloads are the upstream RKNN-Toolkit2 demos, driven through `librknnrt.so`.

| Workload | Operator profile | Input / shape | Performance & Result | Independent Verification |
|---|---|---|---|---|
| `rknn_benchmark` | Continuous MobileNet-v1 INT8 | `1x224x224x3` INT8 | **452.7 FPS** / 2.21 ms (10,000 loops, mask 7; range 439.9–458.9 FPS) | Core 0/1/2 load: 74–92% / 68–86% / 67–84%; IRQs across all three cores; RC=0 |
| `rknn_mobilenet_demo` | Classification (Conv + Depthwise + Softmax) | `1x224x224x3` INT8 | Top-1 **class 156 @ 0.884766** (single run) | Expected classification verified; RC=0 |
| `rknn_yolov5_demo` | Multi-scale detection (FPN, C3/CSP, SiLU, anchor head) | `1x640x640x3` INT8 | **~27.2 ms** (mean of 4 process-run averages; range 25.5–29.1 ms) | Bounding box coordinates and confidence verified |
| `rknn_dynshape_inference` | Dynamic-shape resizing (MobileNetV2 inverted residuals) | 256 / 224 / 160 INT8 | normal **223.3 / 255.8 / 407.2 FPS**; zero-copy **251.4 / 288.7 / 409.8 FPS** (4-run medians) | Dynamic reallocation passes with zero memory fault |
| `rknn_matmul_api_demo` | MatMul engine (FP16 / INT8) | `128x256x512` & `4x64x32` | FP16->FP32 **5291.2 ops/s**; INT8->INT32 **4878.5 ops/s**; FP16->FP16 **11145.5 ops/s** (4x64x32: single run; 128x256x512: 4-run medians) | Three cores independently execute and produce single-step IRQs (mask 1/2/4) |
| Three-core submit + fence | Combined-mask graph submits; sync / NONBLOCK FENCE_OUT | masks 1/2/3/4/7, runtime ALL=65535 | explicit masks pass; **75/75** fence wrappers signalled, no timeout | DMA fence signaled across all cores |
| MEM_SYNC tolerance & concurrency | Cache maintenance on GEM objects | Over-range, zero-length, empty range | **160,000 ops / 0 failures**; empty range (`offset==size`) short-circuits to 0 | Clean kernel ring (zero EIO / GEM sync errors) |

Run-to-run ranges (minimum–maximum over four runs):

- Dynamic-shape normal: 214.9–306.8 / 215.3–326.7 / 306.9–486.0 FPS; zero-copy: 175.4–307.3 / 239.4–328.5 / 382.3–457.5 FPS.
- MatMul: FP16->FP32 4854.4–5917.2 ops/s; INT8->INT32 4717.0–5681.8 ops/s; FP16->FP16 8196.7–12048.2 ops/s.

A separate workload, the Ling-3.0-tiny W4A8 LLM, is recorded in
[ling-3-tiny-rknn.md](ling-3-tiny-rknn.md).

A non-zero FPS figure alone is not proof of execution. The acceptance procedure
in [validation.md](validation.md) lists the independent IRQ and per-core load
evidence to capture alongside these numbers.
