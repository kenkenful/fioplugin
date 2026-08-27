#ifndef _DMA_H
#define _DMA_H

#include <stddef.h>

#include "common.h"

int dma_open(int bus, int dev, int func);
int dma_open_domain(int domain, int bus, int dev, int func);
struct mem alloc_dma(int fd, size_t len);
void free_dma(struct mem *mem);

#endif
