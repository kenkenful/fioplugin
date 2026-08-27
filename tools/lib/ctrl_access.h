#ifndef CTRL_ACCESS_H
#define CTRL_ACCESS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int open_pci_bar0(uint8_t bus, uint8_t dev, uint8_t func);
void *controller_bar0_map(uint8_t bus, uint8_t dev, uint8_t func, size_t size);
int controller_bar0_unmap(void *bar0, size_t sz);
int controller_reg_read32(void *bar0, uint32_t offset, uint32_t *value);
int controller_reg_write32(void *bar0, uint32_t offset, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
