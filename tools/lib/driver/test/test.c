#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "dma.h"

#define DEFAULT_DOMAIN 0
#define DEFAULT_BUS 5
#define DEFAULT_DEV 0
#define DEFAULT_FUNC 0
#define DEFAULT_LOOPS 100
#define BATCH_COUNT 10

static const size_t test_sizes[] = {
	1,
	64,
	512,
	4095,
	4096,
	4097,
	8192,
	16384,
	65536,
	1048576,
};

static int parse_ulong(const char *text, unsigned long *value)
{
	char *end = NULL;

	errno = 0;
	*value = strtoul(text, &end, 0);
	if (errno || end == text || *end != '\0' || *value == 0)
		return -1;

	return 0;
}

static int parse_bdf(const char *text, struct bdf *bdf)
{
	unsigned int domain;
	unsigned int bus;
	unsigned int dev;
	unsigned int func;
	int count;

	count = sscanf(text, "%x:%x:%x.%x", &domain, &bus, &dev, &func);
	if (count == 4) {
		if (bus > 0xff || dev > 0x1f || func > 0x7)
			return -1;
		bdf->domain = (int)domain;
		bdf->bus = (uint8_t)bus;
		bdf->dev = (uint8_t)dev;
		bdf->func = (uint8_t)func;
		return 0;
	}

	count = sscanf(text, "%x:%x.%x", &bus, &dev, &func);
	if (count == 3) {
		if (bus > 0xff || dev > 0x1f || func > 0x7)
			return -1;
		bdf->domain = 0;
		bdf->bus = (uint8_t)bus;
		bdf->dev = (uint8_t)dev;
		bdf->func = (uint8_t)func;
		return 0;
	}

	return -1;
}

static uint8_t pattern_byte(size_t offset, unsigned int loop, size_t size)
{
	return (uint8_t)((offset ^ loop ^ size) & 0xff);
}

static void fill_pattern(uint8_t *buf, size_t size, unsigned int loop)
{
	size_t i;

	for (i = 0; i < size; i++)
		buf[i] = pattern_byte(i, loop, size);
}

static int verify_pattern(uint8_t *buf, size_t size, unsigned int loop)
{
	size_t i;

	for (i = 0; i < size; i++) {
		uint8_t expected = pattern_byte(i, loop, size);

		if (buf[i] != expected) {
			fprintf(stderr,
				"verify failed: loop=%u size=%zu offset=%zu got=0x%02x expected=0x%02x\n",
				loop, size, i, buf[i], expected);
			return -1;
		}
	}

	return 0;
}

struct mapping {
	uint8_t *buf;
	size_t size;
	struct mem mem;
};

static void mapping_init(struct mapping *mapping)
{
	mapping->buf = NULL;
	mapping->size = 0;
	mapping->mem = (struct mem){ 0 };
}

static int alloc_one(int fd, struct mapping *mapping, unsigned int loop,
		     size_t size)
{
	struct mem mem;
	uint8_t *buf;

	mem = alloc_dma(fd, size);
	if (!mem.user_virtaddr) {
		perror("alloc_dma");
		return -1;
	}

	buf = mem.user_virtaddr;

	mapping->buf = buf;
	mapping->size = size;
	mapping->mem = mem;

	fill_pattern(buf, size, loop);
	if (verify_pattern(buf, size, loop) < 0)
		return -1;

	printf("[%5u] alloc   size=%8zu mapped=%8ld user=%p dma=0x%" PRIx64 " ok\n",
	       loop, size, mapping->mem.size, buf,
	       (uint64_t)mapping->mem.dma_addr);

	return 0;
}

static int free_one(struct mapping *mapping, unsigned int loop)
{
	uint64_t dma_addr;

	if (!mapping->buf)
		return 0;

	dma_addr = mapping->mem.dma_addr;
	free_dma(&mapping->mem);

	printf("[%5u] free    size=%8zu user=%p dma=0x%" PRIx64 "%s\n",
	       loop, mapping->size, mapping->buf, dma_addr, " ok");

	mapping_init(mapping);
	return 0;
}

int main(int argc, char **argv)
{
	unsigned long loops = DEFAULT_LOOPS;
	struct bdf bdf = {
		.domain = DEFAULT_DOMAIN,
		.bus = DEFAULT_BUS,
		.dev = DEFAULT_DEV,
		.func = DEFAULT_FUNC,
	};
	struct mapping mappings[BATCH_COUNT];
	int fd;
	unsigned long i;
	unsigned int j;
	int ret = EXIT_FAILURE;

	if (argc > 1 && parse_bdf(argv[1], &bdf) < 0) {
		fprintf(stderr, "invalid bdf: %s\n", argv[1]);
		fprintf(stderr, "usage: %s [domain:bus:dev.func] [loops]\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	if (argc > 2 && parse_ulong(argv[2], &loops) < 0) {
		fprintf(stderr, "invalid loop count: %s\n", argv[2]);
		return EXIT_FAILURE;
	}

	fd = dma_open_domain(bdf.domain, bdf.bus, bdf.dev, bdf.func);
	if (fd < 0) {
		perror("dma_open_domain");
		return EXIT_FAILURE;
	}

	printf("bdf    : %04x:%02x:%02x.%u\n",
	       bdf.domain, bdf.bus, bdf.dev, bdf.func);
	printf("loops  : %lu\n", loops);

	for (j = 0; j < BATCH_COUNT; j++)
		mapping_init(&mappings[j]);

	for (i = 0; i < loops; i += BATCH_COUNT) {
		unsigned int count = BATCH_COUNT;

		if (loops - i < BATCH_COUNT)
			count = (unsigned int)(loops - i);

		for (j = 0; j < count; j++) {
			size_t size;
			unsigned int loop = (unsigned int)(i + j);

			size = test_sizes[loop % (sizeof(test_sizes) /
						  sizeof(test_sizes[0]))];

			if (alloc_one(fd, &mappings[j], loop, size) < 0)
				goto out_cleanup;
		}

		for (j = 0; j < count; j++) {
			unsigned int loop = (unsigned int)(i + j);

			if (free_one(&mappings[j], loop) < 0)
				goto out_cleanup;
		}
	}

	printf("stress test: ok\n");
	ret = EXIT_SUCCESS;

out_cleanup:
	for (j = 0; j < BATCH_COUNT; j++)
		free_one(&mappings[j], (unsigned int)loops);

	if (close(fd) < 0) {
		perror("close");
		ret = EXIT_FAILURE;
	}

	return ret;
}
