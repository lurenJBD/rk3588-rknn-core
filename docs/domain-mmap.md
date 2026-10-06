# Idle Per-FD Domain Reclamation and GEM mmap Cleanup

Summary of the IOMMU domain lifecycle management and GEM mmap cleanup in the mainline `rknpu` driver.

---

## 1. Idle Per-FD Domain Reclamation

Historically, per-FD IOMMU page tables allocated by closed clients persisted until kernel module removal. The driver now reclaims inactive domains to prevent page-table resource leakage during long-running sessions:

- **Reclaimed Range**: Domain IDs 16–63 are eligible for reclamation when:
  1. No DRM file descriptor (`rknpu_fpriv`) owns the ID.
  2. The per-domain IOVA allocator is empty (no active buffers).
  3. No NPU hardware core holds an active job reference.
- **Cached Range**: Domain IDs 0–15 retain SDK caching behavior.
- **Client Cache**: Empty domains remain cached for active client sessions until postclose.

---

## 2. Lifetime & Synchronization Discipline

- **Reclamation Triggers**: Attempted after the last IOVA reservation release, job-domain detach, and DRM postclose.
- **Exported Buffer Protection**: Exported dma-bufs and user VMAs retain GEM references, preventing premature domain reclamation while external handles or mappings remain alive.
- **Lock Ordering**: Strict hierarchy: `iommu_domain_lock` followed by `iommu_shared_mm_lock`. Native mapping/unmapping completes before reservation release.
- **Attach Race Prevention**: Domain lookup in `attach` is deferred until after the unlocked domain-switch wait, ensuring reclamation cannot leave a dangling target pointer during sleep.
- **Teardown**: Idle sticky attachments are detached before freeing root page tables and lower-level pagetables.

---

## 3. GEM mmap Defensive Cleanup

- **Dead Wrappers Removed**: Removed unused `rknpu_gem_mmap` and `rknpu_gem_prime_mmap` wrappers and declarations.
- **Import Guard**: The object callback rejects imported dma-buf objects before modifying VMA cache attributes or calling native DMA mapping.
- **ABI Preservation**: Imported handles continue to be rejected through DRM's dumb-map helper (`MEM_MAP`). Re-exporting an imported GEM returns the original dma-buf, whose exporter performs the mmap.

---

## 4. Diagnostics & Debugging

The read-only debugfs node `iommu_domains` (under `/sys/kernel/debug/rknpu/iommu_domains`) provides runtime visibility:
- Allocated core bitmasks and active references
- DRM FD ownership
- Live IOVA reservations and allocated byte counts
- Total reclaimed/freed per-core domains counter

---

## 5. Verification & Test Suite

The test cases in `test-suite/` validate reclamation logic, concurrency races, and ABI integrity:

```sh
# Mock unit tests for domain reclamation and mmap guards
python3 test-suite/scripts/test-domain-reclaim.py src/rknpu_iommu.c
python3 test-suite/scripts/test-mmap-import-guard.py src/rknpu_gem.c

# Hardware verification (run on reference board with other clients stopped)
cc -O2 -I/usr/include/libdrm -Isrc/include test-suite/src/domain-mmap-hardware.c -o /tmp/domain-mmap-test
sudo /tmp/domain-mmap-test
```
