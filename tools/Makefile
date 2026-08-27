CC ?= gcc
CFLAGS ?= -Wall -Wextra -Wno-unused-parameter -std=gnu11
CPPFLAGS ?= -D_GNU_SOURCE -include ../../../config-host.h -I. -Ilib -Ilib/driver/api -Ilib/driver
LDFLAGS ?=
LDLIBS ?=

TARGET := nvme_test
OBJS := nvme_test.o nvme.o lib/ctrl_access.o lib/pci_access.o dma_api.o lib/util.o

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

lib/%.o: lib/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

dma_api.o: lib/driver/api/dma.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPATH_MAX=4096 -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS) test.o
