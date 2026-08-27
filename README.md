# newperf fio ioengine

`newperf` is an external fio ioengine for direct NVMe performance measurement.
It bypasses the Linux block layer and talks to an NVMe controller through BAR0,
admin queues, I/O submission queues, I/O completion queues, and DMA memory.

This engine is intended for learning and controlled performance experiments.
It is not a general-purpose storage driver.

## Warning

This program directly controls the NVMe controller.

- Use a test device only.
- Do not use a device that contains important data.
- `write` and `trim` workloads can destroy data.
- Do not run this while another driver is actively using the same controller.
- Multiple fio files are not supported.
- All jobs must target the same controller/namespace. The engine assumes this by operation rule.

## Build

Build from the `engines` directory.

```bash
cd /home/ttt/fio/engines
make
```

This builds:

```text
newperf.o
tools/nvme_test
tools/test/test1
tools/test/test2
```

`newperf.o` is built as a shared external fio ioengine using:

```bash
gcc -Wall -O2 -g -D_GNU_SOURCE -include ../config-host.h \
  -shared -rdynamic -fPIC \
  -Itools -Itools/lib -Itools/lib/driver/api -Itools/lib/driver \
  -o newperf.o \
  newperf.c \
  tools/nvme.c \
  tools/lib/ctrl_access.c \
  tools/lib/pci_access.c \
  tools/lib/util.c \
  tools/lib/driver/api/dma.c
```

Clean build files:

```bash
make clean
make
```

## Target Format

Use the fio `filename` option to specify the NVMe target.

```text
<bus>:<device>.<function>,<namespace-id>
```

Example:

```text
05:00.0,1
```

fio treats `:` as a filename separator, so escape it:

```bash
--filename='05\:00.0,1'
```

## Basic Read Test

Run from the `engines` directory:

```bash
sudo ../fio \
  --name=newperf-read \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=read \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

`--thread=1` is required. The engine uses shared controller state in-process and creates one SQ/CQ pair per fio thread.

## Write Test

Danger: this overwrites data.

```bash
sudo ../fio \
  --name=newperf-write \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=write \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

## Trim Test

Danger: this deallocates LBAs.

```bash
sudo ../fio \
  --name=newperf-trim \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=trim \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

Multi-range trim is supported when the fio version supports `FIO_MULTI_RANGE_TRIM`.

```bash
sudo ../fio \
  --name=newperf-multi-trim \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --rw=trim \
  --bs=4k \
  --iodepth=32 \
  --num_range=8 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

## Multiple Jobs

Use fio threads, not processes.

```bash
sudo ../fio \
  --name=newperf-read \
  --ioengine=./newperf.o \
  --filename='05\:00.0,1' \
  --thread=1 \
  --numjobs=4 \
  --rw=read \
  --bs=4k \
  --iodepth=32 \
  --size=1G \
  --direct=1 \
  --time_based=1 \
  --runtime=10
```

The design is:

```text
1 controller shared by all fio threads
1 I/O SQ per fio thread
1 I/O CQ per fio thread
```

Queue IDs are assigned per fio thread.

## Current Limitations

- `--thread=1` is required.
- `nr_files > 1` is rejected.
- All jobs are expected to use the same target.
- The I/O queue depth is configured as `iodepth + 1`.
- I/O offset and size must be aligned to the namespace LBA size.
- This engine assumes fio-provided I/O buffers are allocated through the engine DMA allocator.

## Callback Flow

The engine follows this fio callback order:

```text
setup
get_file_size
init
queue / getevents / event
cleanup
```

`setup` initializes the shared controller.
`get_file_size` reads the namespace size and marks the fio file size as known.
`init` allocates per-thread queue state and creates one SQ/CQ pair.

## Troubleshooting

If fio says multiple files are not supported, check the escaped colon:

```bash
--filename='05\:00.0,1'
```

If fio reports a symbol lookup error, rebuild `newperf.o` with `make` from the `engines` directory.

If DMA allocation fails, reduce `iodepth` or fio buffer size.

If the controller does not become ready, confirm that the target PCI address is correct and that no other driver is controlling the device.
