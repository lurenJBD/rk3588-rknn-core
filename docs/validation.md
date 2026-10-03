# Acceptance Test Flow

Compatibility and performance acceptance runs use the upstream RKNN-Toolkit2
example demos, driven through the closed-source runtime `librknnrt.so`. The
driver itself is only the kernel side (`rknpu.ko`); userspace comes from the
toolkit.

| Item | Value |
|---|---|
| Toolkit repository | <https://github.com/airockchip/rknn-toolkit2> |
| Runtime library | `rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so` |
| Runtime version | `librknnrt` 2.3.2 (RKNN-Toolkit2 v2.3.2) |
| Demos | `rknn_mobilenet_demo`, `rknn_yolov5_demo`, `rknn_benchmark`, `rknn_matmul_api_demo`, `rknn_dynamic_shape_input_demo` |

The toolkit is an external dependency and is **not** vendored here as a
submodule. Pin the revision you test with and record it with your results; the
revision behind the published scores is noted in [scores.md](scores.md).

## Procedure

1. **Build and load the driver** — see [build-and-install.md](build-and-install.md).
2. **Get the toolkit**:
   ```sh
   git clone https://github.com/airockchip/rknn-toolkit2
   ```
   The runtime lives in
   `rknn-toolkit2/rknpu2/runtime/Linux/librknn_api/aarch64/`. Verify it with
   `strings .../librknnrt.so | grep 'librknnrt version'`.
3. **Stage the test assets and build the tools.** The models, images and demo
   binaries are third-party material; stage them into `test-suite/assets/`:
   ```sh
   cd test-suite
   ./prepare-test-assets.sh            # finds rknn-toolkit2 next to the repo
   # or: ./prepare-test-assets.sh --toolkit ../../rknn-toolkit2
   ```
4. **Run the maintained runner** (defaults to `assets/lib` for the runtime;
   override with `RKNN_RUNTIME=`):
   ```sh
   bash run-official-demo.sh status
   bash run-official-demo.sh --watch-load benchmark \
       assets/models/mobilenet_v1.rknn assets/images/dog_224x224.jpg 10000 7
   ```
   Other modes: `graph`, `matmul`.
5. **Record independent evidence.** A non-zero FPS figure is not proof of
   execution. Capture before/after `/proc/interrupts` NPU counters and the
   per-core `load=` samples from `/sys/kernel/debug/rknpu/load`, and keep the
   kernel taint at `0` or `4096`.

## Core-mask controls

`AUTO=0` selects one least-loaded core; it does **not** split or round-robin.
The combined masks are `3` (Core 0 + 1) and `7` (Core 0 + 1 + 2). Shorter
single-core runs confirm the mask is honoured:

```sh
bash run-official-demo.sh --watch-load benchmark \
    assets/models/mobilenet_v1.rknn assets/images/dog_224x224.jpg 1000 1  # core 0
```

A combined mask does not by itself guarantee saturation: a serial workload may
leave cores idle between tasks.

## Driver-level ioctl tests

The toolkit-independent tests (GEM lifetime, IOMMU isolation, MEM_SYNC
tolerance, multicore masks, DMA-token handling) live in
[`../test-suite/src/`](../test-suite/src/); see
[`../test-suite/README.md`](../test-suite/README.md). They need only the loaded
module and the UAPI header in this repository.

## Not covered

These runs do not cover every failure mode: `FENCE_IN`, fault injection, and
out-of-tree SoCs are out of scope. The driver validates shared task-buffer IOVA
and per-core task ranges for combined masks; missing multicore ranges are
rejected rather than inferred.
