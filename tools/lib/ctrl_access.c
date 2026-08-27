#define _POSIX_C_SOURCE 200809L

#include "ctrl_access.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define PCI_SYSFS_BAR0_PATH "/sys/bus/pci/devices/0000:%02x:%02x.%u/resource0"

static int pci_bdf_is_valid(uint8_t dev, uint8_t func)
{
    return dev < 32u && func < 8u;
}

int open_pci_bar0(uint8_t bus, uint8_t dev, uint8_t func)
{
    char path[128];
    int ret;

    if (!pci_bdf_is_valid(dev, func)) {
        errno = EINVAL;
        print_errno("invalid PCI BDF");
        return -1;
    }

    ret = snprintf(path, sizeof(path), PCI_SYSFS_BAR0_PATH, bus, dev, func);
    if (ret < 0 || (size_t)ret >= sizeof(path)) {
        errno = ENAMETOOLONG;
        print_errno("failed to build PCI BAR0 path");
        return -1;
    }

    ret = open(path, O_RDWR | O_SYNC);
    if (ret < 0) {
        print_errno("failed to open PCI BAR0 resource");
    }

    return ret;
}

void *controller_bar0_map(uint8_t bus, uint8_t dev, uint8_t func, size_t size)
{
    int fd;
    void *virt_addr;

    if (size == 0u) {
        errno = EINVAL;
        print_errno("invalid BAR0 map size");
        return NULL;
    }

    fd = open_pci_bar0(bus, dev, func);
    if (fd < 0) {
        return NULL;
    }

    virt_addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (virt_addr == MAP_FAILED) {
        int saved_errno = errno;

        if (close(fd) < 0) {
            print_errno("failed to close PCI BAR0 resource");
        }
        errno = saved_errno;
        print_errno("failed to mmap PCI BAR0 resource");
        return NULL;
    }

    if (close(fd) < 0) {
        print_errno("failed to close PCI BAR0 resource");
    }

    return virt_addr;
}

int controller_bar0_unmap(void *bar0, size_t sz)
{
    if (bar0 == NULL || sz == 0u) {
        errno = EINVAL;
        print_errno("invalid BAR0 unmap argument");
        return -1;
    }

    if (munmap(bar0, sz) < 0) {
        print_errno("failed to unmap PCI BAR0 resource");
        return -1;
    }

    return 0;
}

int controller_reg_read32(void *bar0, uint32_t offset, uint32_t *value)
{
    volatile uint32_t *reg;

    if (bar0 == NULL || value == NULL || (offset & 0x3u) != 0u) {
        errno = EINVAL;
        print_errno("invalid controller register read argument");
        return -1;
    }

    reg = (volatile uint32_t *)((uint8_t *)bar0 + offset);
    *value = *reg;

    return 0;
}

int controller_reg_write32(void *bar0, uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg;

    if (bar0 == NULL || (offset & 0x3u) != 0u) {
        errno = EINVAL;
        print_errno("invalid controller register write argument");
        return -1;
    }

    reg = (volatile uint32_t *)((uint8_t *)bar0 + offset);
    *reg = value;

    return 0;
}
