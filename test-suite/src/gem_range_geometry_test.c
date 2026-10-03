/* Offline geometry test: actual driver helper, modeled SG/DMA callbacks. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SHIFT 12
#define PAGE_SIZE (1UL << PAGE_SHIFT)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define offset_in_page(x) ((x) & (PAGE_SIZE - 1))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define RKNPU_MEM_SYNC_TO_DEVICE 1
#define RKNPU_MEM_SYNC_FROM_DEVICE 2
#define DMA_BIDIRECTIONAL 0
struct device { int unused; };
struct page { unsigned long pfn; };
static struct page pages[1024];
#define page_to_pfn(p) ((p)->pfn)
#define pfn_to_page(n) (&pages[n])
struct scatterlist {
    struct page *page;
    unsigned long offset, length, dma_address, dma_length;
    bool end;
    struct scatterlist *next;
};
struct sg_table { struct scatterlist *sgl; int orig_nents; };
#define sg_page(s) ((s)->page)
#define sg_phys(s) (page_to_pfn(sg_page(s)) * PAGE_SIZE + (s)->offset)
#define sg_dma_address(s) ((s)->dma_address)
#define sg_dma_len(s) ((s)->dma_length)
#define for_each_sgtable_sg(t, s, i) \
    for ((i) = 0, (s) = (t)->sgl; (i) < (t)->orig_nents; (i)++, (s) = (s)->next)
static void sg_init_table(struct scatterlist *sg, unsigned n)
{
    memset(sg, 0, sizeof(*sg) * n);
    for (unsigned i = 0; i < n; i++) {
        sg[i].next = i + 1 < n ? &sg[i + 1] : NULL;
        sg[i].end = i + 1 == n;
    }
}
static void sg_set_page(struct scatterlist *sg, struct page *p,
                        unsigned long len, unsigned long off)
{
    assert(off < PAGE_SIZE);
    sg->page = p; sg->length = len; sg->offset = off;
}
static void sg_mark_end(struct scatterlist *sg) { sg->end = true; }

/* A flat byte-address oracle, built from the original fragmented object. */
static unsigned long oracle[512 * 1024], expected_offset, expected_size;
static unsigned long visited[2], batches[2];
static void capture(struct scatterlist *sg, unsigned n, unsigned direction)
{
    assert(n && n <= 16);
    batches[direction]++;
    for (unsigned i = 0; i < n; i++) {
        assert(sg && sg->length && sg->end == (i + 1 == n));
        assert(sg_dma_address(sg) == sg_phys(sg));
        assert(sg_dma_len(sg) == sg->length);
        for (unsigned long j = 0; j < sg->length; j++) {
            assert(visited[direction] < expected_size);
            assert(sg_phys(sg) + j == oracle[expected_offset + visited[direction]]);
            visited[direction]++;
        }
        sg = sg->next;
    }
}
static void dma_sync_sg_for_device(struct device *d, struct scatterlist *sg,
                                    unsigned n, int dir)
{ (void)d; assert(dir == DMA_BIDIRECTIONAL); capture(sg, n, 0); }
static void dma_sync_sg_for_cpu(struct device *d, struct scatterlist *sg,
                               unsigned n, int dir)
{ (void)d; assert(dir == DMA_BIDIRECTIONAL); capture(sg, n, 1); }

#include "gem-range-under-test.inc"

int main(void)
{
    struct device dev = {0};
    struct scatterlist input[35], saved[35];
    struct sg_table table = { input, ARRAY_SIZE(input) };
    unsigned long total = 0;
    for (unsigned i = 0; i < ARRAY_SIZE(pages); i++) pages[i].pfn = i;
    sg_init_table(input, ARRAY_SIZE(input));
    for (unsigned i = 0; i < ARRAY_SIZE(input); i++) {
        /* Distinct physical runs; lengths exceed one page, offsets vary. */
        sg_set_page(&input[i], &pages[1 + i * 20],
                    257 + (i * 7919) % 16384, (i * 137) % PAGE_SIZE);
        for (unsigned long j = 0; j < input[i].length; j++)
            oracle[total++] = sg_phys(&input[i]) + j;
    }
    memcpy(saved, input, sizeof(input));
    srand(3588);
    for (unsigned trial = 0; trial < 300; trial++) {
        unsigned long off = (unsigned)rand() % total;
        unsigned long len = 1 + (unsigned)rand() % (total - off);
        if (trial == 0) { off = 0; len = total; }
        if (trial == 1) { off = total - 1; len = 1; }
        if (trial == 2) { off = 4095; len = 4098; }
        if (trial == 3) { off = 0; len = 1; }
        for (unsigned flags = 1; flags <= 3; flags++) {
            expected_offset = off; expected_size = len;
            memset(visited, 0, sizeof(visited));
            memset(batches, 0, sizeof(batches));
            assert(rknpu_gem_sync_sg_range(&dev, &table, off, len, flags) == 0);
            assert(visited[0] == (flags & 1 ? len : 0));
            assert(visited[1] == (flags & 2 ? len : 0));
            if (!trial) assert(batches[flags == 2] == 3);
            assert(!memcmp(saved, input, sizeof(input)));
        }
    }
    /* A short table is an internal error, never a successful partial sync. */
    expected_offset = 0; expected_size = total;
    memset(visited, 0, sizeof(visited));
    assert(rknpu_gem_sync_sg_range(&dev, &table, 0, total + 1, 3) == -EIO);
    assert(!memcmp(saved, input, sizeof(input)));
    puts("PASS: 900 ranges/directions, unaligned edges, multipage segments, 16-entry batches, short-table error, immutable source SG");
    return 0;
}
