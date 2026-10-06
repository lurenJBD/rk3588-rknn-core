# On-board test suite

Test cases, runners and preparation scripts used to validate `rknpu` on real
RK3588 hardware. Nothing here is needed to build or install the module.

The `.rknn` models, sample images and RKNN-Toolkit2 demo binaries are
third-party material. `prepare-test-assets.sh` stages them from your own
toolkit checkout and builds the rest into `assets/`.

## Prepare

```sh
./prepare-test-assets.sh                 # stage models/images + build everything
./prepare-test-assets.sh --skip-demos    # driver tests only, no toolkit builds
./prepare-test-assets.sh --toolkit ../../rknn-toolkit2 --force
```

The script looks for `rknn-toolkit2` next to this repository (then inside the
repository, then inside this suite); `--toolkit` / `$RKNN_TOOLKIT2` overrides
it. The toolkit is not a submodule — pin the revision you test with.

## Run

```sh
bash run-official-demo.sh status

# MobileNet benchmark, three-core mask, 10000 loops
bash run-official-demo.sh --watch-load benchmark \
    assets/models/mobilenet_v1.rknn assets/images/dog_224x224.jpg 10000 7
```

The runner uses `assets/lib/librknnrt.so`; override with `RKNN_RUNTIME=`.
Every workload is bounded by `timeout 60s`. A non-zero FPS figure alone is not
proof of execution: also record `/proc/interrupts` NPU counters and the
per-core `load=` samples. The acceptance procedure is in
[`../docs/validation.md`](../docs/validation.md).

## Layout

| Path | Contents |
|---|---|
| `prepare-test-assets.sh` | Stages models/images and builds every tool into `assets/` |
| `run-official-demo.sh` | Runs the official toolkit demos against the loaded module |
| `run-rk-llama-bench.sh` | Runs third-party `rk-llama.cpp` Flash-Attention benchmarks |
| `src/` | Test-case sources (driver ioctl tests, demo source) |
| `scripts/` | Self-contained helper scripts |
| `assets/` | Generated test assets and third-party checkouts (`bin/ lib/ models/ ...`) |

## Third-party Flash-Attention benchmark (`rk-llama.cpp`)

`run-rk-llama-bench.sh` drives end-to-end embedding benchmarks on `jina-embeddings-v5-small` (`v5-small-retrieval-Q8_0.gguf`, Qwen3-0.6B architecture) via a third-party fork of `llama.cpp` (`rk-llama.cpp`, branch `opi5-rknpu2-embed-opt`):
<https://github.com/pty819/rk-llama.cpp/tree/opi5-rknpu2-embed-opt>

The benchmark evaluates NPU hardware Flash-Attention acceleration (`rknn_matmul_api`) comparing `RKNPU_FA=1` (NPU Flash-Attention) against `RKNPU_FA=0` (CPU Flash-Attention) across token lengths 53 to 2048, and verifies 1024-dimensional embedding cosine similarity ($\ge 0.996$).

Because it is third-party material, it is not staged by `prepare-test-assets.sh`. Clone the repository manually into `assets/rk-llama.cpp`:

```sh
git clone -b opi5-rknpu2-embed-opt https://github.com/pty819/rk-llama.cpp assets/rk-llama.cpp
```

Then run the benchmark (the model `v5-small-retrieval-Q8_0.gguf` and benchmark binaries are automatically prepared on first run):

```sh
./run-rk-llama-bench.sh --compare      # end-to-end model benchmark across token lengths 53..2048
./run-rk-llama-bench.sh --quick        # quick run on N=53 tokens
./run-rk-llama-bench.sh --microbench   # standalone kernel adaptive tile microbenchmark
./run-rk-llama-bench.sh --devices      # list detected NPU devices
```

Full benchmark scores, token scaling tables and verification details are documented in [`../docs/rk-llama-rknpu2-fa.md`](../docs/rk-llama-rknpu2-fa.md).

## Driver-level ioctl tests

Built by `prepare-test-assets.sh` into `assets/bin/`. They operate on
`/dev/dri/renderD128` (fallback `renderD129`) and need the loaded module.

| Source | Purpose |
|---|---|
| `src/gem_lifecycle_test.c` | GEM create / map / destroy basics |
| `src/gem_iommu_isolation_test.c` | Per-FD IOMMU domain isolation |
| `src/gem_multicore_map_sync_test.c` | Per-core mapping and MEM_SYNC across core masks |
| `src/dma_token_lookup_test.c` | DMA-token vs handle lookup; stale-token rejection |
| `src/submit_validation_test.c` | Submit path and invalid task-range rejection |
| `src/gem_destroy_release_test.c` | MEM_DESTROY / release lifetime |
| `src/mem_sync_stress_test.c` | MEM_SYNC tolerance (over-range / zero-length) + concurrency stress |
| `src/dma_fence_shim.c` | `LD_PRELOAD` ioctl shim to observe dma_fence signalling |
| `src/rknn_create_mem_demo.cpp` | Source of the `rknn_create_mem_demo` graph workload |

They build against the driver UAPI header in this repository
(`../src/include/rknpu_ioctl.h`); the UAPI must match the loaded module.

## Offline model tests

These extract the current driver helpers from `../src/rknpu_gem.c` and compile
them against a userspace model with ASan/UBSan:

```sh
python3 scripts/test-gem-range-geometry.py --output ./assets/bin/gem-range
python3 scripts/test-gem-token-lifetime.py --output ./assets/bin/gem-token
```

| Source | Covers |
|---|---|
| `src/gem_range_geometry_test.c` | Range-sync physical byte coverage: unaligned edges, multipage SG, 16-entry batches, short-table error |
| `src/gem_token_lifetime_test.c` | GEM token index/lifetime: handle reuse, domain collision, ownership, alias dedup, rollback, concurrent readers |

These validate logic only — not real DRM, DMA coherency, kernel locking or NPU
numerical correctness.

## Scripts

| Script | Purpose |
|---|---|
| `scripts/compare-driver-uapi-abi.py` | Compile two UAPI headers and diff their ABI (header paths as args) |
| `scripts/test-gem-range-geometry.py` | Offline range-sync coverage test |
| `scripts/test-gem-token-lifetime.py` | Offline GEM token lifetime test |

## Notes

- The runner sets all CPU policies and the NPU devfreq governor to
  `performance` and leaves them there; restore them yourself if needed.
- `rknn_yolov5_demo` reads its labels from a hard-coded relative path
  `./model/coco_80_labels_list.txt` and **segfaults (exit 139) if it is
  missing** — an upstream demo bug, not a driver fault. `prepare-test-assets.sh`
  stages that file for exactly this reason; run the demo from a directory that
  has `model/coco_80_labels_list.txt`.
- Hardware workloads require explicit user authorization.
