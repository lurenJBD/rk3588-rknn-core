# RK3588 RKNPU Linux Mainline Kernel Driver

An out-of-tree Linux kernel driver (`rknpu.ko`) for the Rockchip Neural
Processing Unit (RKNPU), targeting modern mainline Linux kernels. Developed and
validated on mainline Linux 7.2.5 aarch64.

> [!IMPORTANT]
> **The reference platform is the Orange Pi 5 Plus (Rockchip RK3588).** It is
> the only board the maintainer has verified. Other RK3588/RK3588S boards are
> **not guaranteed** out of the box — they may need matching device-tree power
> domains, regulators, resets and clocks. Third parties have reported success
> on the Orange Pi 5 (RK3588S); see
> [docs/compatibility.md](docs/compatibility.md). Other Rockchip SoCs
> (RK3568/RK3576/RV1106/RV1103) are not adapted.

## Reference environment

| Component | Value |
|---|---|
| Board / SoC | Orange Pi 5 Plus / Rockchip RK3588 |
| NPU | 3 cores, 6 TOPS INT8, 200-800 MHz DVFS |
| Distribution | Armbian 26.08.1 trixie (Debian 13.6, `aarch64`) |
| Kernel | mainline Linux 7.2.5 |
| Userspace runtime | `librknnrt.so` v2.3.2 (RKNN-Toolkit2 v2.3.2) |

Full details and the third-party board reports: [docs/compatibility.md](docs/compatibility.md).

## Key features

- **Mainline device-tree binding** (`rockchip,rk3588-rknn-core`), legacy BSP stubs stripped.
- **Three-core concurrency** with per-core time tracking and IRQ affinity routing to the A76 cluster.
- **800 MHz DVFS** via a standard devfreq device at `/sys/class/devfreq/fdab0000.npu/` (200-800 MHz).
- **Per-FD IOMMU isolation** with per-domain IOVA accounting, supporting models larger than the 32-bit IOVA space (validated with 4.1 GiB weights).
- **Native PC engine** with `PC_DMA_BASE = 0`, covering Conv, Depthwise, MatMul, GDN, LayerNorm and transformer projections.
- **Async submit & dma_fence** support (`RKNPU_JOB_NONBLOCK`).
- **Clean lifecycle**: balanced clocks/regulators on `rmmod`, kernel taint stays at `4096`.

## Validation

Compatibility and performance are validated with the **upstream
RKNN-Toolkit2 example demos** (`rknn_mobilenet_demo`, `rknn_yolov5_demo`,
`rknn_benchmark`, `rknn_matmul_api_demo`, `rknn_dynamic_shape_input_demo`) run
against the loaded module through `librknnrt.so`. The acceptance procedure is
in [docs/validation.md](docs/validation.md); the measured scores are in
[docs/scores.md](docs/scores.md); the LLM workload result is in
[docs/ling-3-tiny-rknn.md](docs/ling-3-tiny-rknn.md). The on-board test cases
and preparation scripts are in [test-suite/](test-suite/).

## Quick start

```sh
make -j$(nproc)
sudo make install && sudo depmod -a
sudo modprobe rknpu
cat /sys/kernel/debug/rknpu/load
```

Dependencies, verification steps and clean unload: [docs/build-and-install.md](docs/build-and-install.md).
Module parameters and debugfs nodes: [docs/module-parameters.md](docs/module-parameters.md).
Memory-bandwidth / mainline DMC background: [docs/memory-and-dmc.md](docs/memory-and-dmc.md).

A DKMS package that pins this repository is maintained at
<https://github.com/lurenJBD/rknpu-mainline-dkms>.

## Documentation

All detailed documents are indexed in [docs/README.md](docs/README.md).

## Acknowledgments

Thanks to the [rockchip-npu-notes](https://github.com/gregordinary/rockchip-npu-notes)
project for its hardware documentation and reverse-engineering of the RK3588
NPU (NVDLA-derived architecture, MRDMA behaviour, register-command paths and
clock subsystems).

## License

GPL-2.0 — see [LICENSE](LICENSE).
