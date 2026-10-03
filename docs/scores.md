# Performance Scores

Measured on the reference platform (see
[compatibility.md](compatibility.md)) with the NPU at up to 800 MHz and a
three-core mask, unless stated otherwise.

The workloads are the upstream RKNN-Toolkit2 demos, driven through
`librknnrt.so`. Toolkit revision: `airockchip/rknn-toolkit2`, `librknnrt`
2.3.2 (`59a913d172e7f5ff03c9076e2ec7b1b1288ffd08`).

| Workload | Operator profile | Input / shape | Performance |
|---|---|---|---|
| `rknn_benchmark` | Continuous MobileNet-v1 INT8 | `1x224x224x3` INT8 | **459.828 FPS** / 2.17 ms (10,000 loops, mask 7) |
| `rknn_mobilenet_demo` | Classification (Conv + Depthwise + Softmax) | `1x224x224x3` INT8 | Top-1 **class 156 @ 0.884766** |
| `rknn_yolov5_demo` | Multi-scale detection (FPN, C3/CSP, SiLU, anchor head) | `1x640x640x3` INT8 | **~24.8 ms** per frame |
| `rknn_dynshape_inference` | Dynamic-shape resizing (MobileNetV2 inverted residuals) | 256 / 224 / 160 INT8 | normal **233.434 / 237.540 / 355.049 FPS**; zero-copy **213.884 / 235.081 / 360.947 FPS** |
| `rknn_matmul_api_demo` | MatMul engine | `128x256x512` | FP16->FP32 **5882.35 ops/s**; INT8->INT32 **5524.86 ops/s**; FP16->FP16 **12195.12 ops/s** |
| Three-core submit + fence | Combined-mask graph submits; sync / NONBLOCK FENCE_OUT | masks 1/2/3/4/7, runtime ALL=65535 | explicit masks pass; **75/75** fence wrappers signalled, no timeout |

A separate workload, the Ling-3.0-tiny W4A8 LLM, is recorded in
[ling-3-tiny-rknn.md](ling-3-tiny-rknn.md).

A non-zero FPS figure alone is not proof of execution. The acceptance procedure
in [validation.md](validation.md) lists the independent IRQ and per-core load
evidence to capture alongside these numbers.
