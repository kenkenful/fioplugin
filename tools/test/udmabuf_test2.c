#include "udmabuf.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define TEST_ALLOC_SIZE (1024u * 1024u)
#define PAGE_SIZE 4096u
#define PAGEMAP_PATH "/proc/self/pagemap"
#define PAGEMAP_PRESENT_BIT (1ull << 63)
#define PAGEMAP_PFN_MASK ((1ull << 55) - 1ull)

static uintptr_t virt_to_phys(void *virt)
{
    uintptr_t virt_addr;
    uint64_t entry;
    uint64_t pfn;
    long page_size;
    off_t offset;
    ssize_t read_size;
    int fd;

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        perror("sysconf");
        return 0u;
    }

    fd = open(PAGEMAP_PATH, O_RDONLY);
    if (fd < 0) {
        perror("open pagemap");
        return 0u;
    }

    virt_addr = (uintptr_t)virt;
    offset = (off_t)((virt_addr / (uintptr_t)page_size) * sizeof(entry));
    if (lseek(fd, offset, SEEK_SET) < 0) {
        perror("lseek pagemap");
        close(fd);
        return 0u;
    }

    read_size = read(fd, &entry, sizeof(entry));
    close(fd);
    if (read_size != (ssize_t)sizeof(entry)) {
        perror("read pagemap");
        return 0u;
    }

    if ((entry & PAGEMAP_PRESENT_BIT) == 0u) {
        return 0u;
    }

    pfn = entry & PAGEMAP_PFN_MASK;
    return (uintptr_t)((pfn * (uint64_t)page_size) +
                       (virt_addr % (uintptr_t)page_size));
}

int main(void)
{
    struct dma_mem mem;
    uintptr_t phys;
    uintptr_t prev_phys;
    uint8_t *page;
    size_t page_count;
    size_t i;

    if (udmabuf_allocator_init() < 0) {
        fprintf(stderr, "udmabuf_allocator_init failed\n");
        return EXIT_FAILURE;
    }

    if (udmabuf_alloc(TEST_ALLOC_SIZE, &mem) < 0) {
        fprintf(stderr, "udmabuf_alloc failed\n");
        udmabuf_allocator_deinit();
        return EXIT_FAILURE;
    }

    printf("udmabuf 1M allocation succeeded\n");
    printf("  virt=%p\n", mem.virt);
    printf("  phys=0x%016llx\n", (unsigned long long)mem.phys);
    printf("  requested_size=%u\n", TEST_ALLOC_SIZE);
    printf("  allocated_size=%zu\n", mem.size);

    page = mem.virt;
    page_count = TEST_ALLOC_SIZE / PAGE_SIZE;
    prev_phys = 0u;

    for (i = 0u; i < page_count; ++i) {
        phys = virt_to_phys(page + (i * PAGE_SIZE));
        if (phys == 0u) {
            fprintf(stderr, "failed to translate page=%zu\n", i);
            udmabuf_allocator_deinit();
            return EXIT_FAILURE;
        }

        if (i == 0u) {
            if (phys != (uintptr_t)mem.phys) {
                fprintf(stderr,
                        "page 0 phys mismatch: mem.phys=0x%016llx "
                        "pagemap=0x%016llx\n",
                        (unsigned long long)mem.phys,
                        (unsigned long long)phys);
                udmabuf_allocator_deinit();
                return EXIT_FAILURE;
            }
        } else if (phys != prev_phys + PAGE_SIZE) {
            fprintf(stderr,
                    "non-contiguous page: page=%zu prev=0x%016llx "
                    "phys=0x%016llx\n",
                    i, (unsigned long long)prev_phys,
                    (unsigned long long)phys);
            udmabuf_allocator_deinit();
            return EXIT_FAILURE;
        }

        if (i < 4u || i + 4u >= page_count) {
            printf("  page=%zu phys=0x%016llx\n",
                   i, (unsigned long long)phys);
        }

        prev_phys = phys;
    }

    printf("physical pages are contiguous: pages=%zu\n", page_count);

    udmabuf_allocator_deinit();
    return EXIT_SUCCESS;
}
