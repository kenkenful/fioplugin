CC ?= gcc
CFLAGS ?= -Wall -Wextra -Werror -Wno-unused-parameter -std=gnu11
CPPFLAGS ?= -D_GNU_SOURCE -include ../../../../config-host.h -I.. -I../lib -I../lib/driver/api -I../lib/driver
LDFLAGS ?=
LDLIBS ?=

TARGETS := test1 test2
UDMABUF_TEST_OBJS := udmabuf_test.o ../lib/udmabuf.o ../lib/util.o
UDMABUF_TEST2_OBJS := udmabuf_test2.o ../lib/udmabuf.o ../lib/util.o
TEST1_OBJS := test1.o ../nvme.o ../lib/ctrl_access.o ../lib/pci_access.o dma_api.o ../lib/util.o
TEST2_OBJS := test2.o ../nvme.o ../lib/ctrl_access.o dma_api.o ../lib/util.o

.PHONY: all clean test

all: $(TARGETS)

udmabuf_test: $(UDMABUF_TEST_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(UDMABUF_TEST_OBJS) $(LDLIBS)

udmabuf_test2: $(UDMABUF_TEST2_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(UDMABUF_TEST2_OBJS) $(LDLIBS)

test1: $(TEST1_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(TEST1_OBJS) $(LDLIBS)

test2: $(TEST2_OBJS)
	$(CC) $(LDFLAGS) -o $@ $(TEST2_OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

../lib/%.o: ../lib/%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

dma_api.o: ../lib/driver/api/dma.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPATH_MAX=4096 -c $< -o $@

../%.o: ../%.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test: udmabuf_test
	./udmabuf_test

clean:
	rm -f $(TARGETS) $(UDMABUF_TEST_OBJS) $(UDMABUF_TEST2_OBJS) $(TEST1_OBJS) $(TEST2_OBJS)
