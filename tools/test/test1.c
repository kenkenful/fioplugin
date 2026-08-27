#define _POSIX_C_SOURCE 199309L

#include "nvme.h"
#include "pci_access.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define READ_TEST_LOOP_COUNT 2000u
#define READ_TEST_BUFFER_COUNT 8u
#define READ_TEST_PROGRESS_INTERVAL 10u
#define READ_TEST_VERBOSE_AFTER 100u
#define READ_TEST_CQE_POLL_INTERVAL 1000000u
#define TEST_IO_SQ_DEPTH 64u
#define TEST_IO_CQ_DEPTH 256u

static int parse_u16(const char *arg, uint16_t *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(arg, &end, 0);
    if (errno != 0 || *end != '\0' || parsed > UINT16_MAX) {
        return -1;
    }

    *value = (uint16_t)parsed;
    return 0;
}

static int parse_u64(const char *arg, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(arg, &end, 0);
    if (errno != 0 || *end != '\0') {
        return -1;
    }

    *value = (uint64_t)parsed;
    return 0;
}

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s <bus> <dev> <func> [nsid] [offset_lba size_lba]\n",
            prog);
    fprintf(stderr, "example: %s 0x01 0x00 0x0 1 0 8\n", prog);
}

static void wait_after_submit(void)
{
    struct timespec req;

    req.tv_sec = 0;
    req.tv_nsec = 1000000L;
    nanosleep(&req, NULL);
}

static void free_read_buffers(struct mem *mem_v, uint32_t count)
{
    uint32_t i;

    if (mem_v == NULL) {
        return;
    }

    for (i = 0u; i < count; ++i) {
        free_dma(&mem_v[i]);
    }
    free(mem_v);
}

int main(int argc, char **argv)
{
    struct nvme_data n;
    struct controller_data ctrl;
    struct mem *read_mem_v;
    uint64_t capacity;
    uint64_t read_offset_lba;
    uint64_t read_size_lba;
    uint64_t read_dma_size;
    uint64_t namespace_lba_count;
    uint64_t current_offset_lba;
    uint64_t last_offset_lba;
    uint16_t read_cmd_id;
    uint16_t completions;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t nsid;
    uint32_t read_slot_count;
    uint32_t submitted;
    uint32_t completed;
    uint32_t loop_index;
    uint32_t buffer_index;
    uint32_t sq_index;
    uint32_t cqe_poll_count;
    volatile uint32_t *sqe;
    volatile uint32_t *cqe;
    unsigned int i;
    unsigned int first_byte_count;
    bool ok;

    if (argc != 4 && argc != 5 && argc != 6 && argc != 7) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    memset(&n, 0, sizeof(n));
    memset(&ctrl, 0, sizeof(ctrl));
    n.ctrl = &ctrl;
    read_mem_v = NULL;
    if (parse_u16(argv[1], &n.ctrl->bus) < 0 ||
        parse_u16(argv[2], &n.ctrl->dev) < 0 ||
        parse_u16(argv[3], &n.ctrl->func) < 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    nsid = 1u;
    read_offset_lba = 0u;
    read_size_lba = 1u;
    if (argc == 5 && parse_u16(argv[4], &nsid) < 0) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    if (argc == 6) {
        if (parse_u64(argv[4], &read_offset_lba) < 0 ||
            parse_u64(argv[5], &read_size_lba) < 0) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (argc == 7) {
        if (parse_u16(argv[4], &nsid) < 0 ||
            parse_u64(argv[5], &read_offset_lba) < 0 ||
            parse_u64(argv[6], &read_size_lba) < 0) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }
    if (read_size_lba == 0u) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }
    n.ctrl->nsid = nsid;

    printf("controller_init: bus=0x%02x dev=0x%02x func=0x%x nsid=%u "
           "read_offset_lba=%llu read_size_lba=%llu\n",
           n.ctrl->bus, n.ctrl->dev, n.ctrl->func, n.ctrl->nsid,
           (unsigned long long)read_offset_lba,
           (unsigned long long)read_size_lba);

    if (pci_config_read((uint8_t)n.ctrl->bus, (uint8_t)n.ctrl->dev,
                        (uint8_t)n.ctrl->func, 0x00u, &vendor_id,
                        sizeof(vendor_id)) == 0 &&
        pci_config_read((uint8_t)n.ctrl->bus, (uint8_t)n.ctrl->dev,
                        (uint8_t)n.ctrl->func, 0x02u, &device_id,
                        sizeof(device_id)) == 0 &&
        pci_config_read((uint8_t)n.ctrl->bus, (uint8_t)n.ctrl->dev,
                        (uint8_t)n.ctrl->func, 0x04u, &command,
                        sizeof(command)) == 0) {
        printf("PCI config: vendor=0x%04x device=0x%04x command=0x%04x\n",
               vendor_id, device_id, command);
    }

    ok = controller_init(n.ctrl);
    if (!ok) {
        fprintf(stderr, "controller_init failed\n");
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    printf("controller_init succeeded\n");
    printf("  bar0_virt=%p\n", n.ctrl->bar0_virt);
    printf("  admin_sq_phys=0x%016llx virt=0x%016llx depth=%u\n",
           (unsigned long long)n.ctrl->admin_sq_phys_addr,
           (unsigned long long)n.ctrl->admin_sq_virt_addr,
           n.ctrl->admin_sq_depth);
    printf("  admin_cq_phys=0x%016llx virt=0x%016llx depth=%u phase=%u\n",
           (unsigned long long)n.ctrl->admin_cq_phys_addr,
           (unsigned long long)n.ctrl->admin_cq_virt_addr,
           n.ctrl->admin_cq_depth, n.ctrl->admin_cq_phase);

    n.io_sq_depth = TEST_IO_SQ_DEPTH;
    n.io_cq_depth = TEST_IO_CQ_DEPTH;
    if (!create_io_queue_pair(&n)) {
        fprintf(stderr, "create_io_queue_pair failed\n");
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    printf("create_io_queue_pair succeeded\n");
    printf("  io_sq_phys=0x%016llx virt=0x%016llx depth=%u\n",
           (unsigned long long)n.io_sq_phys_addr,
           (unsigned long long)n.io_sq_virt_addr,
           n.io_sq_depth);
    printf("  io_cq_phys=0x%016llx virt=0x%016llx depth=%u phase=%u\n",
           (unsigned long long)n.io_cq_phys_addr,
           (unsigned long long)n.io_cq_virt_addr,
           n.io_cq_depth, n.io_cq_pahse);

    capacity = nvme_get_file_size(n.ctrl);
    if (capacity == 0u) {
        fprintf(stderr, "nvme_get_file_size failed\n");
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }
    printf("  namespace_size=%llu bytes lba_shift=%d\n",
           (unsigned long long)capacity, n.ctrl->lba_shift);
    namespace_lba_count = capacity >> n.ctrl->lba_shift;
    if (read_offset_lba > namespace_lba_count ||
        read_size_lba > namespace_lba_count - read_offset_lba) {
        fprintf(stderr,
                "read range exceeds namespace: offset_lba=%llu size_lba=%llu "
                "namespace_lba_count=%llu\n",
                (unsigned long long)read_offset_lba,
                (unsigned long long)read_size_lba,
                (unsigned long long)namespace_lba_count);
        delete_io_queue_pair(&n);
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }
    read_dma_size = read_size_lba << n.ctrl->lba_shift;
    if (read_dma_size > SIZE_MAX) {
        fprintf(stderr, "read DMA size is too large: %llu bytes\n",
                (unsigned long long)read_dma_size);
        delete_io_queue_pair(&n);
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    if (n.io_sq_depth <= 1u) {
        fprintf(stderr, "IO SQ depth is too small: depth=%u\n",
                n.io_sq_depth);
        delete_io_queue_pair(&n);
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    read_slot_count = READ_TEST_BUFFER_COUNT;
    read_mem_v = calloc(read_slot_count, sizeof(*read_mem_v));
    if (read_mem_v == NULL) {
        fprintf(stderr, "failed to allocate read buffer table\n");
        delete_io_queue_pair(&n);
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    for (submitted = 0u; submitted < read_slot_count; ++submitted) {
        read_mem_v[submitted] = alloc_dma(n.ctrl->dma_fd, (size_t)read_dma_size);
        if (read_mem_v[submitted].user_virtaddr == NULL ||
            read_mem_v[submitted].size <= 0) {
            fprintf(stderr, "read buffer allocation failed\n");
            free_read_buffers(read_mem_v, submitted);
            delete_io_queue_pair(&n);
            controller_deinit(n.ctrl);
            return EXIT_FAILURE;
        }
        memset(read_mem_v[submitted].user_virtaddr, 0,
               (size_t)read_mem_v[submitted].size);
    }

    printf("nvme_read loop start: loops=%u total=%u queue_depth=%u "
           "offset_lba=%llu size_lba=%llu dma_size=%llu\n",
           READ_TEST_LOOP_COUNT, READ_TEST_LOOP_COUNT, n.io_sq_depth,
           (unsigned long long)read_offset_lba,
           (unsigned long long)read_size_lba,
           (unsigned long long)read_dma_size);

    submitted = 0u;
    completed = 0u;
    current_offset_lba = read_offset_lba;
    last_offset_lba = namespace_lba_count - read_size_lba;

    for (loop_index = 0u; loop_index < READ_TEST_LOOP_COUNT; ++loop_index) {
        buffer_index = loop_index % read_slot_count;
        read_cmd_id = nvme_read(&n, current_offset_lba, read_size_lba,
                                read_mem_v[buffer_index].dma_addr);
        printf("nvme_read submitted: loop=%u cid=%u sq_tail=%u cq_head=%u "
               "cq_phase=%u offset_lba=%llu size_lba=%llu dma=0x%016llx\n",
               loop_index, read_cmd_id, n.io_sq_tail, n.ic_cq_head,
               n.io_cq_pahse,
               (unsigned long long)current_offset_lba,
               (unsigned long long)read_size_lba,
               (unsigned long long)read_mem_v[buffer_index].dma_addr);
        if (current_offset_lba >= last_offset_lba) {
            current_offset_lba = read_offset_lba;
        } else {
            current_offset_lba += read_size_lba;
        }
        if (n.io_sq_tail == 0u) {
            sq_index = n.io_sq_depth - 1u;
        } else {
            sq_index = n.io_sq_tail - 1u;
        }
        sqe = (volatile uint32_t *)(uintptr_t)n.io_sq_virt_addr;
        sqe += (size_t)sq_index * 16u;
        printf("nvme_read sqe: index=%u dw0=%08x dw1=%08x "
               "dw6=%08x dw7=%08x dw10=%08x dw11=%08x dw12=%08x\n",
               sq_index, sqe[0], sqe[1], sqe[6], sqe[7],
               sqe[10], sqe[11], sqe[12]);
        fflush(stdout);
        wait_after_submit();
        ++submitted;

        cqe_poll_count = 0u;
        for (;;) {
            completions = poll_cq(&n, 1);
            if (completions != 0u) {
                if (completions != 1u) {
                    fprintf(stderr,
                            "poll_cq returned unexpected completions: "
                            "loop=%u completions=%u\n",
                            loop_index, completions);
                    free_read_buffers(read_mem_v, read_slot_count);
                    delete_io_queue_pair(&n);
                    controller_deinit(n.ctrl);
                    return EXIT_FAILURE;
                }

                ++completed;
                if (completed >= READ_TEST_VERBOSE_AFTER ||
                    (completed % READ_TEST_PROGRESS_INTERVAL) == 0u) {
                    printf("nvme_read progress: loop=%u completed=%u "
                           "sq_tail=%u cq_head=%u cq_phase=%u cid=%u\n",
                           loop_index, completed, n.io_sq_tail,
                           n.ic_cq_head, n.io_cq_pahse, read_cmd_id);
                    fflush(stdout);
                }
                break;
            }

            ++cqe_poll_count;
            if (loop_index >= READ_TEST_VERBOSE_AFTER &&
                (cqe_poll_count % READ_TEST_CQE_POLL_INTERVAL) == 0u) {
                cqe = (volatile uint32_t *)(uintptr_t)n.io_cq_virt_addr;
                cqe += (size_t)n.ic_cq_head * 4u;
                printf("nvme_read cqe: loop=%u cid=%u cq_head=%u "
                       "cq_phase=%u dw0=%08x dw1=%08x dw2=%08x dw3=%08x\n",
                       loop_index, read_cmd_id, n.ic_cq_head,
                       n.io_cq_pahse, cqe[0], cqe[1], cqe[2], cqe[3]);
                fflush(stdout);
            }
        }
    }

    printf("nvme_read loop completed: submitted=%u completed=%u first16=",
           submitted, completed);
    first_byte_count = read_dma_size < 16u ? (unsigned int)read_dma_size : 16u;
    for (i = 0u; i < first_byte_count; ++i) {
        printf("%02x", ((uint8_t *)read_mem_v[0].user_virtaddr)[i]);
    }
    printf("\n");

    free_read_buffers(read_mem_v, read_slot_count);
    if (!delete_io_queue_pair(&n)) {
        fprintf(stderr, "delete_io_queue_pair failed\n");
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    controller_deinit(n.ctrl);
    printf("controller_deinit completed\n");

    return EXIT_SUCCESS;
}
