#ifndef _COMMON_H
#define _COMMON_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct mem {
	int fd;
	__u64 kernel_virtaddr;
	void *user_virtaddr;
	__u64 dma_addr;
	long size;
};

struct bdf {
	int domain;
	__u8 bus;
	__u8 dev;
	__u8 func;
};

#define NVME 'N'
#define IOCTL_GET_MEMFINFO _IOR(NVME, 2, struct mem *)
#define IOCTL_GET_BDF      _IOR(NVME, 3, struct bdf *)
#define IOCTL_RELEASE_MEM  _IOW(NVME, 4, struct mem *)

#endif
