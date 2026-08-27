#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdint.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/memfd.h>
#include <linux/udmabuf.h>


#define TEST_PREFIX	"drivers/dma-buf/udmabuf"
#define NUM_PAGES       3
#define HUGETLB_PAGE_SIZE (2UL * 1024UL * 1024UL)
#define PAGEMAP_PATH	"/proc/self/pagemap"
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
	struct udmabuf_create create;
	int devfd, memfd, buf, ret;
	off_t size;
	void *mem;
	uint8_t *page;
	long page_size;
	size_t i;

	devfd = open("/dev/udmabuf", O_RDWR);
	if (devfd < 0) {
		printf("%s: [skip,no-udmabuf: Unable to access DMA buffer device file]\n",
		       TEST_PREFIX);
		exit(77);
	}

	memfd = memfd_create("udmabuf-test",
			     MFD_ALLOW_SEALING | MFD_HUGETLB | MFD_HUGE_2MB);
	if (memfd < 0) {
		printf("%s: [skip,no-hugetlb-memfd: %s]\n",
		       TEST_PREFIX, strerror(errno));
		exit(77);
	}

	ret = fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK);
	if (ret < 0) {
		printf("%s: [skip,fcntl-add-seals]\n", TEST_PREFIX);
		exit(77);
	}


	size = (off_t)HUGETLB_PAGE_SIZE * NUM_PAGES;
	page_size = sysconf(_SC_PAGESIZE);
	if (page_size <= 0) {
		perror("sysconf");
		exit(1);
	}

	ret = ftruncate(memfd, size);
	if (ret == -1) {
		printf("%s: [FAIL,memfd-truncate]\n", TEST_PREFIX);
		exit(1);
	}

	mem = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED,
		   memfd, 0);
	if (mem == MAP_FAILED) {
		printf("%s: [skip,no-hugetlb-pages: %s]\n",
		       TEST_PREFIX, strerror(errno));
		exit(77);
	}

	page = mem;
	/* Fault in every base page contained by the HugeTLB mapping. */
	for (i = 0u; i < (size_t)size; i += (size_t)page_size) {
		page[i] = 0u;
	}

	memset(&create, 0, sizeof(create));



	/* should work */
	create.memfd  = memfd;
	create.offset = 0;
	create.size   = size;
	buf = ioctl(devfd, UDMABUF_CREATE, &create);
	if (buf < 0) {
		printf("%s: [FAIL,test-4: %s]\n", TEST_PREFIX, strerror(errno));
		exit(1);
	}

	printf("page addresses:\n");
	for (i = 0u; i < NUM_PAGES; ++i) {
		void *virt = page + (i * HUGETLB_PAGE_SIZE);
		uintptr_t phys = virt_to_phys(virt);

		if (phys == 0u) {
			printf("  page=%zu virt=%p phys=<unavailable>\n", i, virt);
		} else {
			printf("  page=%zu virt=%p phys=0x%016llx\n",
			       i, virt, (unsigned long long)phys);
		}
	}

	fprintf(stderr, "%s: ok\n", TEST_PREFIX);
	munmap(mem, (size_t)size);
	close(buf);
	close(memfd);
	close(devfd);
	return 0;
}
