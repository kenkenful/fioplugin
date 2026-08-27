CC ?= gcc
MAKE ?= make

NEWPERF := newperf.o

NEWPERF_CPPFLAGS := \
	-D_GNU_SOURCE \
	-include ../../config-host.h \
	-Itools \
	-Itools/lib \
	-Itools/lib/driver/api \
	-Itools/lib/driver

NEWPERF_CFLAGS := -Wall -O2 -g
NEWPERF_LDFLAGS := -shared -rdynamic -fPIC

NEWPERF_SRCS := \
	newperf.c \
	tools/nvme.c \
	tools/lib/ctrl_access.c \
	tools/lib/pci_access.c \
	tools/lib/util.c \
	tools/lib/driver/api/dma.c

.PHONY: all tools tools-test clean

all: $(NEWPERF) tools tools-test

$(NEWPERF): $(NEWPERF_SRCS) tools/nvme.h
	$(CC) $(NEWPERF_CFLAGS) $(NEWPERF_CPPFLAGS) \
		$(NEWPERF_LDFLAGS) \
		-o $@ \
		$(NEWPERF_SRCS)

tools:
	$(MAKE) -C tools

tools-test:
	$(MAKE) -C tools/test

clean:
	rm -f $(NEWPERF)
	$(MAKE) -C tools clean
	$(MAKE) -C tools/test clean
