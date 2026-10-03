# Hardware & Kernel Compatibility

## Reference platform

| Component / Layer | Value |
|---|---|
| Board | Orange Pi 5 Plus (16 GB LPDDR4x) |
| SoC | Rockchip RK3588 (4x Cortex-A76 + 4x Cortex-A55) |
| NPU | 3 cores, 6 TOPS INT8, 200-800 MHz DVFS |
| Distribution | Armbian 26.08.1 trixie (Debian GNU/Linux 13.6, `aarch64`) |
| Kernel | mainline Linux 7.2.5 |
| Toolchain | GCC 14.2.0 / binutils 2.42 |
| Userspace runtime | `librknnrt` 2.3.2 (RKNN-Toolkit2 v2.3.2) |

The scores in [scores.md](scores.md) and the procedure in
[validation.md](validation.md) refer to this platform.

## Third-party boards

These configurations were reported by external users and are **not** reproduced
by the maintainer; treat them as `user report` evidence.

| Board | SoC | Kernel | Runtime | Observed result |
|---|---|---|---|---|
| Orange Pi 5 | RK3588S | mainline 7.1.8-edge-rockchip64 (Armbian 26.8.3) | `librknnrt` 2.3.2 | `rknn_matmul` correct on all three cores (cosine 1.0 vs CPU); llama.cpp embeddings cosine >= 0.996 |
| Orange Pi 5 | RK3588S | mainline 6.18 / 7.1 | `librknnrt` 2.3.2 | 24/7 llama.cpp embedding service runs correctly under watchdog restarts and many matmul contexts per process |
| RK3588 (Rockchip 5 Pro / Orange Pi 5 class) | RK3588 | mainline 7.1.8-edge-rockchip64 | `librknnrt` 2.3.2 | Concurrent-load embedding service stable; over-range `MEM_SYNC` requests are clamped instead of failing |

### RK3588S notes

- The RK3588S NPU device-tree nodes are symmetric on the Orange Pi 5 — power
  domains 9/10/11, IOMMUs enabled, all fed from `vdd_npu_s0` — and **no overlay
  is needed**.
- Switching from the in-tree `rocket` driver at runtime works when `rocket` is
  unbound first.
- The three-core matmul path and long-running multi-context inference both work
  on RK3588S with `librknnrt` 2.3.2.

## Other Rockchip SoCs

The driver's `of_device_id` table also lists `rockchip,rk3568-rknn-core` and
`rockchip,rk3576-rknn-core` with their own register configs, but those SoCs are
not validated and are out of scope for the claims here. RV1106/RV1103 are not
supported. The validated target is the mainline RK3588 NPU binding
(`rockchip,rk3588-rknn-core`, three NPU power domains, per-core IOMMU groups).
