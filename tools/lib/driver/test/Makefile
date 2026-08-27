CC ?= gcc

CFLAGS ?= -O2
CFLAGS += -Wall -Wextra -I../api -I..

TARGET := test
SRCS := test.c ../api/dma.c
OBJS := $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

test.o: test.c ../api/dma.h ../common.h
	$(CC) $(CFLAGS) -c -o $@ test.c

../api/dma.o: ../api/dma.c ../api/dma.h ../common.h
	$(CC) $(CFLAGS) -c -o $@ ../api/dma.c

clean:
	rm -f $(TARGET) test.o ../api/dma.o
