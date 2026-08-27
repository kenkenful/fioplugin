#include "udmabuf.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_ALLOC_SIZE 4096u
#define PAGE_SIZE 4096u

static int parse_size(const char *arg, size_t *size)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(arg, &end, 0);
    if (errno != 0 || *end != '\0' || value == 0ul ||
        (value % PAGE_SIZE) != 0ul) {
        return -1;
    }

    *size = (size_t)value;
    return 0;
}

int main(int argc, char **argv)
{
    struct dma_mem mem;
    uint8_t *bytes;
    size_t size = DEFAULT_ALLOC_SIZE;

    if (argc > 2) {
        fprintf(stderr, "usage: %s [size]\n", argv[0]);
        return EXIT_FAILURE;
    }

    if (argc == 2 && parse_size(argv[1], &size) < 0) {
        fprintf(stderr, "size must be a non-zero multiple of %u\n", PAGE_SIZE);
        return EXIT_FAILURE;
    }

    if (udmabuf_allocator_init() < 0) {
        fprintf(stderr, "udmabuf_allocator_init failed\n");
        return EXIT_FAILURE;
    }

    if (udmabuf_alloc(size, &mem) < 0) {
        fprintf(stderr, "udmabuf_alloc failed\n");
        udmabuf_allocator_deinit();
        return EXIT_FAILURE;
    }

    if (udmabuf_alloc(size, &mem) < 0) {
        fprintf(stderr, "udmabuf_alloc failed\n");
        udmabuf_allocator_deinit();
        return EXIT_FAILURE;
    }

    bytes = mem.virt;
    bytes[0] = 0x5au;
    bytes[mem.size - 1u] = 0xa5u;

    printf("udmabuf allocation succeeded\n");
    printf("  virt=%p\n", mem.virt);
    printf("  phys=0x%016llx\n", (unsigned long long)mem.phys);
    printf("  size=%zu\n", mem.size);
    printf("  first=0x%02x last=0x%02x\n", bytes[0], bytes[mem.size - 1u]);

    udmabuf_allocator_deinit();
    return EXIT_SUCCESS;
}
