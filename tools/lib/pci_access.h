#ifndef PCI_ACCESS_H
#define PCI_ACCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCIE_CONFIG_SPACE_SIZE 4096u

int pci_config_read(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset,
                    void *buf, size_t sz);
int pci_config_write(uint8_t bus, uint8_t dev, uint8_t func, uint16_t offset,
                     const void *buf, size_t sz);

#ifdef __cplusplus
}
#endif

#endif
