# Performance Scores

Measured on the reference platform (see [compatibility.md](compatibility.md)) with the NPU at up to 800 MHz and a three-core combined mask (`core_mask=7`), unless stated otherwise.

The reference configuration is the Orange Pi 5 Plus (Rockchip RK3588, 3 cores, 6 TOPS INT8) running mainline Linux 7.2.5 on Armbian 26.08.1 trixie (`aarch64`) with GCC 14.2.0 and `librknnrt.so` v2.3.2 (RKNN-Toolkit2 commit `59a913d`). Full hardware and kernel details are documented in [compatibility.md](compatibility.md).

All workloads run with CPU and NPU governors set to `performance` (NPU targeted at 800 MHz OPP).

## Workload Results

The workloads are the upstream RKNN-Toolkit2 demos, driven through `librknnrt.so`.

| Workload | Operator profile | Input / shape | Performance & Result | Independent Verification |
|---|---|---|---|---|
| `rknn_benchmark` | Continuous MobileNet-v1 INT8 | `1x224x224x3` INT8 | **613.300 FPS** / 1.63 ms (10,000 loops, mask 7) | Core 0/1/2 load: 75% / 70% / 69%; IRQs across all three cores; RC=0 |
| `rknn_mobilenet_demo` | Classification (Conv + Depthwise + Softmax) | `1x224x224x3` INT8 | Top-1 **class 156 @ 0.878906** | Expected classification verified; RC=0 |
| `rknn_yolov5_demo` | Multi-scale detection (FPN, C3/CSP, SiLU, anchor head) | `1x640x640x3` INT8 | **~24.8 ms** per frame | Bounding box coordinates and confidence verified |
| `rknn_dynshape_inference` | Dynamic-shape resizing (MobileNetV2 inverted residuals) | 256 / 224 / 160 INT8 | normal **233.434 / 237.540 / 355.049 FPS**; zero-copy **213.884 / 235.081 / 360.947 FPS** | Dynamic reallocation passes with zero memory fault |
| `rknn_matmul_api_demo` | MatMul engine (FP16 / INT8) | `128x256x512` & `4x64x32` | FP16->FP32 **5882.35 ops/s**; INT8->INT32 **5524.86 ops/s**; FP16->FP16 **12195.12 ops/s** | Three cores independently execute and produce single-step IRQs (mask 1/2/4) |
| Three-core submit + fence | Combined-mask graph submits; sync / NONBLOCK FENCE_OUT | masks 1/2/3/4/7, runtime ALL=65535 | explicit masks pass; **75/75** fence wrappers signalled, no timeout | DMA fence signaled across all cores |
| MEM_SYNC tolerance & concurrency | Cache maintenance on GEM objects | Over-range, zero-length, empty range | **160,000 ops / 0 failures**; empty range (`offset==size`) short-circuits to 0 | Clean kernel ring (zero EIO / GEM sync errors) |

A separate workload, the Ling-3.0-tiny W4A8 LLM, is recorded in
[ling-3-tiny-rknn.md](ling-3-tiny-rknn.md).

A non-zero FPS figure alone is not proof of execution. The acceptance procedure
in [validation.md](validation.md) lists the independent IRQ and per-core load
evidence to capture alongside these numbers.
