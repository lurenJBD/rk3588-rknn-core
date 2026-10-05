# Idle per-FD domain reclamation and GEM mmap cleanup

Closed-client per-FD IOMMU page tables no longer persist until module removal after their working set drains. Domain IDs 16-63 are reclaimed when no DRM FD owns the ID, the allocator is empty and no core holds an active job reference. Empty domains remain cached for an open client session. IDs 0-15 retain the SDK's existing caching behavior.

Reclamation is attempted after the last IOVA reservation release, job-domain detach and DRM postclose. Exported dma-bufs and VMAs retain GEM references, which retain their allocator reservation, so closing the original DRM FD cannot reclaim their domain prematurely. A new allocation racing cleanup pins the domain through the allocator lock. The lock order is `iommu_domain_lock` then `iommu_shared_mm_lock`; native mapping/unmapping finishes before its reservation is released.

An idle sticky attachment is detached before its domain is freed. The small empty IOVA allocator remains initialized for reuse; page-table roots and retained lower-level tables are freed. Domain lookup in attach is delayed until after the unlocked domain-switch wait, so reclamation cannot leave a cached target pointer dangling while attach sleeps. Normal module shutdown retains its existing teardown sweep.

The read-only `iommu_domains` debugger entry reports allocated core bits, active references, FD owners, live reservations/bytes and the number of freed per-core domains. It is available through the existing proc/debugfs debugger interface.

The unused `rknpu_gem_mmap` and `rknpu_gem_prime_mmap` wrappers and declarations are removed. The live object callback rejects imported objects before modifying VMA cache attributes or invoking native DMA mapping. This preserves the current ABI: MEM_MAP already rejects imported handles through DRM's dumb-map helper; imported-object re-export returns the original dma-buf, whose exporter performs its mmap. This is defensive cleanup, not a claim of a reachable existing userspace wrong-mapping vulnerability.

Regression commands:

```sh
python3 test-suite/scripts/test-domain-reclaim.py src/rknpu_iommu.c
python3 test-suite/scripts/test-mmap-import-guard.py src/rknpu_gem.c
python3 test-suite/scripts/compare-driver-uapi-abi.py OLD/include/rknpu_ioctl.h src/include/rknpu_ioctl.h
cc -O2 -I/usr/include/libdrm -Isrc/include test-suite/src/domain-mmap-hardware.c -o /tmp/domain-mmap-test
sudo /tmp/domain-mmap-test
```

The hardware test must run with other NPU clients stopped. It discovers the NPU DRM device, checks repeated client close and empty-domain reclamation, exported dma-buf and VMA retention, ID reuse with an old exported GEM still alive, and imported handle rejection plus exporter-backed mmap. Mock tests execute the production reclamation/release/attach/detach code, including target reclamation during the unlocked attach wait, allocation races and injected mapping failures. ASan/UBSan runs and both 6.18/7.1 kernel builds are part of validation. UAPI layout is unchanged.
