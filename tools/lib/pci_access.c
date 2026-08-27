#define _POSIX_C_SOURCE 200809L

#include "pci_access.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define PCI_SYSFS_CONFIG_PATH "/sys/bus/pci/devices/0000:%02x:%02x.%u/config"

static int pci_bdf_is_valid(uint8_t dev, uint8_t func)
{
    return dev < 32u && func < 8u;
}

static int pci_config_range_is_valid(uint16_t offset, size_t sz)
{
    return offset < PCIE_CONFIG_SPACE_SIZE &&
           sz <= PCIE_CONFIG_SPACE_SIZE - offset;
}

static int open_pci_config(uint8_t bus, uint8_t dev, uint8_t func)
{
    char path[128];
    int fd;
    int ret;

    if (!pci_bdf_is_valid(dev, func)) {
        errno = EINVAL;
        print_errno("invalid PCI BDF");
        return -1;
    }

    ret = snprintf(path, sizeof(path), PCI_SYSFS_CONFIG_PATH, bus, dev, func);
    if (ret < 0 || (size_t)ret >= sizeof(path)) {
        errno = ENAMETOOLONG;
        print_errno("failed to build PCI config path");
        return -1;
    }

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0 && errno == EACCES) {
        fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        print_errno("failed to open PCI config");
    }

    return fd;
}

int pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset,
                    void *buf, size_t sz)
{
    int fd;
    uint8_t *pos;
    size_t done;

    if ((buf == NULL && sz > 0u) || !pci_config_range_is_valid(offset, sz)) {
        errno = EINVAL;
        print_errno("invalid PCI config read argument");
        return -1;
    }

    fd = open_pci_config(bus, dev, func);
    if (fd < 0) {
        return -1;
    }

    pos = buf;
    done = 0u;
    while (done < sz) {
        ssize_t nread = pread(fd, pos + done, sz - done,
                              (off_t)offset + (off_t)done);

        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            print_errno("failed to read PCI config");
            close(fd);
            return -1;
        }
        if (nread == 0) {
            close(fd);
            errno = EIO;
            print_errno("unexpected EOF while reading PCI config");
            return -1;
        }

        done += (size_t)nread;
    }

    if (close(fd) < 0) {
        print_errno("failed to close PCI config");
        return -1;
    }

    return 0;
}

int pci_config_write(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset,
                     const void *buf, size_t sz)
{
    int fd;
    const uint8_t *pos;
    size_t done;

    if ((buf == NULL && sz > 0u) || !pci_config_range_is_valid(offset, sz)) {
        errno = EINVAL;
        print_errno("invalid PCI config write argument");
        return -1;
    }

    fd = open_pci_config(bus, dev, func);
    if (fd < 0) {
        return -1;
    }

    pos = buf;
    done = 0u;
    while (done < sz) {
        ssize_t nwritten = pwrite(fd, pos + done, sz - done,
                                  (off_t)offset + (off_t)done);

        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            print_errno("failed to write PCI config");
            close(fd);
            return -1;
        }
        if (nwritten == 0) {
            close(fd);
            errno = EIO;
            print_errno("unexpected EOF while writing PCI config");
            return -1;
        }

        done += (size_t)nwritten;
    }

    if (close(fd) < 0) {
        print_errno("failed to close PCI config");
        return -1;
    }

    return 0;
}
