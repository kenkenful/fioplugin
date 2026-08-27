#include "dma.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define GENPCI_DEV_DIR "/dev"
#define GENPCI_PREFIX "genpci"

static int is_genpci_name(const char *name)
{
	return strncmp(name, GENPCI_PREFIX, strlen(GENPCI_PREFIX)) == 0;
}

static int bdf_matches(const struct bdf *bdf, int domain, int bus, int dev,
		       int func)
{
	return bdf->domain == domain &&
	       bdf->bus == bus &&
	       bdf->dev == dev &&
	       bdf->func == func;
}

int dma_open(int bus, int dev, int func)
{
	return dma_open_domain(0, bus, dev, func);
}

int dma_open_domain(int domain, int bus, int dev, int func)
{
	DIR *dir;
	struct dirent *dent;
	int saved_errno = ENOENT;

	dir = opendir(GENPCI_DEV_DIR);
	if (!dir)
		return -1;

	while ((dent = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		struct bdf bdf = { 0 };
		int fd;

		if (!is_genpci_name(dent->d_name))
			continue;

		if (snprintf(path, sizeof(path), "%s/%s", GENPCI_DEV_DIR,
			     dent->d_name) >= (int)sizeof(path)) {
			saved_errno = ENAMETOOLONG;
			continue;
		}

		fd = open(path, O_RDWR);
		if (fd < 0) {
			saved_errno = errno;
			continue;
		}

		if (ioctl(fd, IOCTL_GET_BDF, &bdf) == 0 &&
		    bdf_matches(&bdf, domain, bus, dev, func)) {
			closedir(dir);
			return fd;
		}

		if (errno)
			saved_errno = errno;

		close(fd);
	}

	closedir(dir);
	errno = saved_errno;
	return -1;
}

struct mem alloc_dma(int fd, size_t len)
{
	struct mem mem = { 0 };
	void *virt;

	if (len == 0) {
		errno = EINVAL;
		return mem;
	}

	virt = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (virt == MAP_FAILED)
		return mem;

	if (ioctl(fd, IOCTL_GET_MEMFINFO, &mem) < 0) {
		int saved_errno = errno;

		munmap(virt, len);
		errno = saved_errno;
		return (struct mem){ 0 };
	}

	mem.fd = fd;
	mem.user_virtaddr = virt;

	return mem;
}

void free_dma(struct mem *mem)
{
	if (!mem || mem->user_virtaddr == NULL || mem->size <= 0)
		return;

	ioctl(mem->fd, IOCTL_RELEASE_MEM, mem);
	munmap(mem->user_virtaddr, (size_t)mem->size);
	*mem = (struct mem){ 0 };
}
