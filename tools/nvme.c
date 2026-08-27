#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>


#include "../../fio.h"

#include "nvme.h"
#include "ctrl_access.h"
#include "util.h"

#define NVME_BAR0_MAP_SIZE 0x4000u

#define NVME_REG_CAP      0x0000u
#define NVME_REG_CC       0x0014u
#define NVME_REG_CSTS     0x001cu
#define NVME_REG_AQA      0x0024u
#define NVME_REG_ASQ      0x0028u
#define NVME_REG_ACQ      0x0030u
#define NVME_DOORBELL_BASE 0x1000u

#define NVME_CC_EN        (1u << 0)
#define NVME_CC_MPS_SHIFT 7u
#define NVME_CC_IOSQES    (6u << 16)
#define NVME_CC_IOCQES    (4u << 20)
#define NVME_CSTS_RDY     (1u << 0)
#define NVME_REG_INVALID32 0xffffffffu
#define NVME_REG_INVALID64 0xffffffffffffffffull

#define NVME_ADMIN_Q_DEPTH 32u
#define NVME_IO_QID 1u
#define NVME_ADMIN_SQ_ENTRY_SIZE 64u
#define NVME_ADMIN_CQ_ENTRY_SIZE 16u
#define NVME_IO_SQ_ENTRY_SIZE 64u
#define NVME_IO_CQ_ENTRY_SIZE 16u
#define NVME_IO_SQ_ENTRY_SHIFT 6u
#define NVME_IO_CQ_ENTRY_SHIFT 4u
#define NVME_ADMIN_DELETE_IO_SQ_OPCODE 0x00u
#define NVME_ADMIN_CREATE_IO_SQ_OPCODE 0x01u
#define NVME_ADMIN_DELETE_IO_CQ_OPCODE 0x04u
#define NVME_ADMIN_CREATE_IO_CQ_OPCODE 0x05u
#define NVME_ADMIN_IDENTIFY_OPCODE 0x06u
#define NVME_IO_WRITE_OPCODE 0x01u
#define NVME_IO_READ_OPCODE 0x02u
#define NVME_IO_DATASET_MANAGEMENT_OPCODE 0x09u
#define NVME_DSM_ATTR_DEALLOCATE (1u << 2)
#define NVME_IDENTIFY_CNS_NAMESPACE 0x00u
#define NVME_IDENTIFY_DATA_SIZE 4096u
#define NVME_IDENTIFY_NS_NSZE_OFFSET 0u
#define NVME_IDENTIFY_NS_FLBAS_OFFSET 26u
#define NVME_IDENTIFY_NS_LBAF_OFFSET 128u
#define NVME_LBAF_ENTRY_SIZE 4u
#define NVME_LBAF_LBADS_OFFSET 2u
#define NVME_QUEUE_PHYS_CONTIGUOUS 0x1u
#define NVME_PAGE_SIZE 4096u
#define NVME_PAGE_SHIFT 12u
#define NVME_PAGE_MASK (NVME_PAGE_SIZE - 1u)
#define NVME_PRP_LIST_ENTRY_SIZE 8u
#define NVME_PRP_LIST_ENTRY_SHIFT 3u
#define NVME_PRP_LIST_ENTRIES (1u << (NVME_PAGE_SHIFT - NVME_PRP_LIST_ENTRY_SHIFT))
#define NVME_CQE_PHASE_MASK 0x1u
#define NVME_CQE_SC_SHIFT 1u
#define NVME_CQE_SC_MASK 0xffu
#define NVME_CQE_SCT_SHIFT 9u
#define NVME_CQE_SCT_MASK 0x7u
#define NVME_READY_POLL_NS 1000000L
#define NVME_ADMIN_CMD_TIMEOUT_MS 60000u
#define NVME_ADMIN_CMD_POLL_NS 1000000L



struct nvme_command {
    uint8_t opcode;
    uint8_t flags;
    uint16_t command_id;
    uint32_t nsid;
    uint64_t reserved2;
    uint64_t metadata;
    uint64_t prp1;
    uint64_t prp2;
    uint64_t slba;
    uint32_t length;
    uint32_t dsmgmt;
    uint32_t reftag;
    uint16_t apptag;
    uint16_t appmask;
};

struct nvme_cq_entry{
    uint32_t cs;
    uint32_t rsvd;
    uint16_t sqhd;
    uint16_t sqid;
    uint16_t cid;
    union{
        uint16_t psf;
        struct{
            uint16_t p     : 1;
            uint16_t sc    : 8;
            uint16_t sct   : 3;
            uint16_t rsvd3 : 2;
            uint16_t m     : 1;
            uint16_t dnr   : 1;
        };
    };
};

struct nvme_dsm_range {
    uint32_t context_attributes;
    uint32_t length;
    uint64_t slba;
};

void nvme_data_init(struct nvme_data *n)
{
    memset(n, 0, sizeof(*n));
}

static struct controller_data *nvme_ctrl(struct nvme_data *n)
{
    return n->ctrl;
}

static void sleep_1ms(void)
{
    struct timespec req;

    req.tv_sec = 0;
    req.tv_nsec = NVME_READY_POLL_NS;
    nanosleep(&req, NULL);
}

static int nvme_reg_read64(void *bar0, uint32_t offset, uint64_t *value)
{
    uint32_t lo;
    uint32_t hi;

    if (controller_reg_read32(bar0, offset, &lo) < 0) {
        return -1;
    }

    if (controller_reg_read32(bar0, offset + 4u, &hi) < 0) {
        return -1;
    }

    *value = ((uint64_t)hi << 32) | lo;
    return 0;
}

static int nvme_reg_write64(void *bar0, uint32_t offset, uint64_t value)
{
    if (controller_reg_write32(bar0, offset, (uint32_t)value) < 0) {
        return -1;
    }

    if (controller_reg_write32(bar0, offset + 4u,
                               (uint32_t)(value >> 32)) < 0) {
        return -1;
    }

    return 0;
}

static void *dma_mem_virt(const struct mem *mem)
{
    return mem->user_virtaddr;
}

static uint64_t dma_mem_phys(const struct mem *mem)
{
    return mem->dma_addr;
}

static size_t dma_mem_size(const struct mem *mem)
{
    return (size_t)mem->size;
}

static bool nvme_dma_open_controller(struct controller_data *c)
{
    int fd;

    if (c->dma_fd_opened) {
        return true;
    }

    fd = dma_open((int)c->bus, (int)c->dev, (int)c->func);
    if (fd < 0) {
        print_errno("failed to open NVMe DMA device");
        return false;
    }

    c->dma_fd = fd;
    c->dma_fd_opened = true;
    return true;
}

bool nvme_alloc_dma_mem(struct controller_data *c, size_t size,
                        struct mem *mem)
{
    if (!nvme_dma_open_controller(c)) {
        return false;
    }

    *mem = alloc_dma(c->dma_fd, size);
    if (mem->user_virtaddr == NULL || mem->size <= 0) {
        print_errno("failed to allocate NVMe DMA memory");
        return false;
    }

    return true;
}

void nvme_free_dma_mem(struct mem *mem)
{
    free_dma(mem);
}

static void nvme_close_dma(struct controller_data *c)
{
    if (c->dma_fd_opened) {
        close(c->dma_fd);
        c->dma_fd = -1;
        c->dma_fd_opened = false;
    }
}

static void nvme_release_controller_dma(struct controller_data *c)
{
    free_dma(&c->admin_sq_mem);
    free_dma(&c->admin_cq_mem);
    nvme_close_dma(c);
}

static void nvme_unmap_controller_bar0(struct controller_data *c)
{
    if (c->bar0_virt != NULL) {
        controller_bar0_unmap(c->bar0_virt, NVME_BAR0_MAP_SIZE);
        c->bar0_virt = NULL;
    }
}

static inline void nvme_reg_write32_fast(void *bar0, uint32_t offset,
                                         uint32_t value)
{
    volatile uint32_t *reg;

    reg = (volatile uint32_t *)((uint8_t *)bar0 + offset);
    *reg = value;
}

static inline uint64_t nvme_page_offset(uint64_t addr)
{
    return addr & NVME_PAGE_MASK;
}

static inline uint64_t nvme_page_align_up(uint64_t addr)
{
    return (addr + NVME_PAGE_MASK) & ~(uint64_t)NVME_PAGE_MASK;
}

static inline void setup_prps(struct nvme_data *n, uint32_t sq_slot,
                              uint64_t dma_addr, uint64_t transfer_size,
                              uint64_t *prp1, uint64_t *prp2)
{
    uint64_t offset;
    uint64_t page_count;
    uint64_t total_size;
    uint64_t remaining_pages;
    uint64_t next_page;
    uint64_t *list;
    uint64_t list_offset;
    uint64_t list_phys_offset;

    *prp1 = dma_addr;
    *prp2 = 0u;

    offset = nvme_page_offset(dma_addr);
    total_size = offset + transfer_size;
    page_count = (total_size + NVME_PAGE_MASK) >> NVME_PAGE_SHIFT;
    if (page_count == 1u) {
        return;
    }

    next_page = nvme_page_align_up(dma_addr);
    if (page_count == 2u) {
        *prp2 = next_page;
        return;
    }

    remaining_pages = page_count - 1u;
    list_offset = (uint64_t)sq_slot << NVME_PAGE_SHIFT; /* 4KiBの倍数 */
    list = (uint64_t *)((uint8_t *)n->io_prp_list_virt_addr + list_offset);

    while (remaining_pages != 0u) {
        *list = next_page;
        ++list;
        next_page += NVME_PAGE_SIZE;
        --remaining_pages;
    }

    list_phys_offset = (uint64_t)sq_slot << NVME_PAGE_SHIFT;
    *prp2 = n->io_prp_list_phys_addr + list_phys_offset;
}

static uint32_t ready_timeout_ms(uint64_t cap)
{
    return (uint32_t)((cap >> 24) & 0xffu) * 500u;
}

static uint32_t cap_mqes_entries(uint64_t cap)
{
    return (uint32_t)(cap & 0xffffu) + 1u;
}

static uint32_t cap_mpsmin(uint64_t cap)
{
    return (uint32_t)((cap >> 48) & 0xfu);
}

static uint32_t cap_dstrd(uint64_t cap)
{
    return (uint32_t)((cap >> 32) & 0xfu);
}

static size_t round_up_page(size_t size)
{
    return (size + NVME_PAGE_SIZE - 1u) & ~(NVME_PAGE_SIZE - 1u);
}

static uint32_t admin_sq_tail_doorbell(uint64_t cap)
{
    (void)cap;
    return NVME_DOORBELL_BASE;
}

static uint32_t admin_cq_head_doorbell(uint64_t cap)
{
    uint32_t stride;

    stride = 4u << cap_dstrd(cap);
    return NVME_DOORBELL_BASE + stride;
}

static uint16_t nvme_io_qid(const struct nvme_data *n)
{
    return n->io_qid != 0u ? n->io_qid : NVME_IO_QID;
}

static uint32_t io_sq_tail_doorbell(uint64_t cap, uint16_t qid)
{
    uint32_t stride;

    stride = 4u << cap_dstrd(cap);
    return NVME_DOORBELL_BASE + (2u * qid * stride);
}

static uint32_t io_cq_head_doorbell(uint64_t cap, uint16_t qid)
{
    uint32_t stride;

    stride = 4u << cap_dstrd(cap);
    return NVME_DOORBELL_BASE + ((2u * qid + 1u) * stride);
}

static void memory_barrier(void)
{
#if defined(__GNUC__) || defined(__clang__)
    __sync_synchronize();
#endif
}

static uint64_t load_le64(const uint8_t *p)
{
    return ((uint64_t)p[0]) |
           ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static bool parse_ulong_field(const char **p, int base, unsigned long max,
                              char delimiter, unsigned long *value)
{
    char *end;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(*p, &end, base);
    if (errno != 0 || end == *p || parsed > max) {
        return false;
    }

    if (delimiter != '\0') {
        if (*end != delimiter) {
            return false;
        }
        *p = end + 1;
    } else {
        if (*end != '\0') {
            return false;
        }
        *p = end;
    }

    *value = parsed;
    return true;
}

struct controller_data *parse_target(const char *filename)
{
    struct controller_data *c;
    const char *p;
    unsigned long bus;
    unsigned long dev;
    unsigned long func;
    unsigned long nsid;

    if (filename == NULL) {
        return NULL;
    }

    p = filename;
    if (!parse_ulong_field(&p, 16, UINT16_MAX, ':', &bus)) {
        return NULL;
    }
    if (!parse_ulong_field(&p, 16, 31u, '.', &dev)) {
        return NULL;
    }
    if (!parse_ulong_field(&p, 16, 7u, ',', &func)) {
        return NULL;
    }
    if (!parse_ulong_field(&p, 10, UINT16_MAX, '\0', &nsid)) {
        return NULL;
    }
    if (nsid == 0u) {
        return NULL;
    }

    c = calloc(1u, sizeof(*c));
    if (c == NULL) {
        print_errno("failed to allocate controller data");
        return NULL;
    }

    c->bus = (uint16_t)bus;
    c->dev = (uint16_t)dev;
    c->func = (uint16_t)func;
    c->nsid = (uint16_t)nsid;
    return c;
}

static bool wait_ready(void *bar0, bool ready, uint32_t timeout_ms)
{
    uint32_t csts;
    uint32_t elapsed;
    uint32_t want;

    want = ready ? NVME_CSTS_RDY : 0u;
    printf("waiting for NVMe CSTS.RDY=%u timeout=%u ms\n",ready ? 1u : 0u, timeout_ms);

    for (elapsed = 0u; elapsed < timeout_ms; ++elapsed) {
        if (controller_reg_read32(bar0, NVME_REG_CSTS, &csts) < 0) {
            return false;
        }
        if (csts == NVME_REG_INVALID32) {
            errno = EIO;
            print_errno("NVMe CSTS read returned all ones");
            return false;
        }
        if ((csts & NVME_CSTS_RDY) == want) {
            printf("NVMe CSTS=0x%08x after %u ms\n", csts, elapsed);
            return true;
        }
        if ((elapsed % 1000u) == 0u) {
            printf("  still waiting: CSTS=0x%08x elapsed=%u ms\n",
                   csts, elapsed);
        }
        sleep_1ms();
    }

    errno = ETIMEDOUT;
    print_errno(ready ? "timeout waiting for NVMe ready" :
                         "timeout waiting for NVMe not ready");
    return false;
}

bool controller_init(struct controller_data *c)
{
    uint64_t cap;
    uint32_t cc;
    uint32_t aqa;
    uint32_t mpsmin;
    uint32_t timeout_ms;
    size_t admin_sq_size;
    size_t admin_cq_size;

    if (c == NULL) {
        errno = EINVAL;
        print_errno("invalid controller data");
        return false;
    }

    if (c->bar0_virt == NULL) {
        c->bar0_virt = controller_bar0_map((uint8_t)c->bus, (uint8_t)c->dev,
                                           (uint8_t)c->func,
                                           NVME_BAR0_MAP_SIZE);
        if (c->bar0_virt == NULL) {
            return false;
        }
    }

    if (nvme_reg_read64(c->bar0_virt, NVME_REG_CAP, &cap) < 0) {
        nvme_unmap_controller_bar0(c);
        return false;
    }
    if (cap == 0u) {
        errno = EIO;
        print_errno("NVMe CAP read returned zero");
        nvme_unmap_controller_bar0(c);
        return false;
    }
    if (cap == NVME_REG_INVALID64) {
        errno = EIO;
        print_errno("NVMe CAP read returned all ones");
        nvme_unmap_controller_bar0(c);
        return false;
    }
    timeout_ms = ready_timeout_ms(cap);
    if (timeout_ms == 0u) {
        errno = EIO;
        print_errno("NVMe CAP.TO is zero");
        nvme_unmap_controller_bar0(c);
        return false;
    }
    mpsmin = cap_mpsmin(cap);
    admin_sq_size = round_up_page(NVME_ADMIN_SQ_ENTRY_SIZE * NVME_ADMIN_Q_DEPTH);
    admin_cq_size = round_up_page(NVME_ADMIN_CQ_ENTRY_SIZE * NVME_ADMIN_Q_DEPTH);

    if (!nvme_alloc_dma_mem(c, admin_sq_size, &c->admin_sq_mem)) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    if (!nvme_alloc_dma_mem(c, admin_cq_size, &c->admin_cq_mem)) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    memset(dma_mem_virt(&c->admin_sq_mem), 0, dma_mem_size(&c->admin_sq_mem));
    memset(dma_mem_virt(&c->admin_cq_mem), 0, dma_mem_size(&c->admin_cq_mem));

    if (controller_reg_read32(c->bar0_virt, NVME_REG_CC, &cc) < 0) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }
    if (cc == NVME_REG_INVALID32) {
        errno = EIO;
        print_errno("NVMe CC read returned all ones");
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    if ((cc & NVME_CC_EN) != 0u) {
        cc &= ~NVME_CC_EN;
        if (controller_reg_write32(c->bar0_virt, NVME_REG_CC, cc) < 0) {
            nvme_unmap_controller_bar0(c);
            nvme_release_controller_dma(c);
            return false;
        }

        if (!wait_ready(c->bar0_virt, false, timeout_ms)) {
            nvme_unmap_controller_bar0(c);
            nvme_release_controller_dma(c);
            return false;
        }
    }

    aqa = (NVME_ADMIN_Q_DEPTH - 1u) | ((NVME_ADMIN_Q_DEPTH - 1u) << 16);
    if (controller_reg_write32(c->bar0_virt, NVME_REG_AQA, aqa) < 0) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    if (nvme_reg_write64(c->bar0_virt, NVME_REG_ASQ,
                         dma_mem_phys(&c->admin_sq_mem)) < 0) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    if (nvme_reg_write64(c->bar0_virt, NVME_REG_ACQ,
                         dma_mem_phys(&c->admin_cq_mem)) < 0) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    cc = (mpsmin << NVME_CC_MPS_SHIFT)|NVME_CC_IOSQES|NVME_CC_IOCQES|NVME_CC_EN;
    if (controller_reg_write32(c->bar0_virt, NVME_REG_CC, cc) < 0) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    if (!wait_ready(c->bar0_virt, true, timeout_ms)) {
        nvme_unmap_controller_bar0(c);
        nvme_release_controller_dma(c);
        return false;
    }

    c->admin_sq_tail = 0u;
    c->admin_cq_head = 0u;
    c->admin_cq_phase = 1u;
    c->admin_sq_phys_addr = dma_mem_phys(&c->admin_sq_mem);
    c->admin_cq_phys_addr = dma_mem_phys(&c->admin_cq_mem);
    c->admin_sq_virt_addr = c->admin_sq_mem.user_virtaddr;
    c->admin_cq_virt_addr = c->admin_cq_mem.user_virtaddr;
    c->admin_sq_depth = NVME_ADMIN_Q_DEPTH;
    c->admin_cq_depth = NVME_ADMIN_Q_DEPTH;
    c->admin_sq_tail_db = admin_sq_tail_doorbell(cap);
    c->admin_cq_head_db = admin_cq_head_doorbell(cap);
    if (c->lba_shift == 0) {
        c->lba_shift = 9;
    }

    return true;
}

void controller_deinit(struct controller_data *c)
{
    uint64_t cap;
    uint32_t cc;
    uint32_t timeout_ms;

    if (c == NULL) {
        errno = EINVAL;
        print_errno("invalid NVMe controller data");
        return;
    }

    if (c->bar0_virt != NULL) {
        if (nvme_reg_read64(c->bar0_virt, NVME_REG_CAP, &cap) == 0) {
            timeout_ms = ready_timeout_ms(cap);
            if (timeout_ms != 0u) {
                if (controller_reg_read32(c->bar0_virt, NVME_REG_CC,
                                          &cc) == 0) {
                    if ((cc & NVME_CC_EN) != 0u) {
                        cc &= ~NVME_CC_EN;
                        if (controller_reg_write32(c->bar0_virt, NVME_REG_CC,
                                                   cc) == 0) {
                            (void)wait_ready(c->bar0_virt, false, timeout_ms);
                        }
                    }
                }
            }
        }

        if (controller_bar0_unmap(c->bar0_virt, NVME_BAR0_MAP_SIZE) < 0) {
            print_errno("failed to unmap NVMe BAR0");
        }
        c->bar0_virt = NULL;
    }

    free_dma(&c->admin_sq_mem);
    free_dma(&c->admin_cq_mem);
    nvme_close_dma(c);

    c->admin_sq_tail = 0u;
    c->admin_cq_head = 0u;
    c->admin_cq_phase = 0u;
    c->admin_sq_phys_addr = 0u;
    c->admin_cq_phys_addr = 0u;
    c->admin_sq_virt_addr = NULL;
    c->admin_cq_virt_addr = NULL;
    c->admin_sq_depth = 0u;
    c->admin_cq_depth = 0u;
    c->admin_sq_tail_db = 0u;
    c->admin_cq_head_db = 0u;
    c->bar0_phys = 0u;
    c->size = 0u;
}

static bool wait_admin_completion(struct controller_data *c, uint16_t cmd_id,
                                  uint32_t timeout_ms)
{
    volatile struct nvme_cq_entry *cqe;
    uint32_t elapsed;
    uint16_t cid;
    uint16_t sc;
    uint16_t sct;

    for (elapsed = 0u; elapsed < timeout_ms; ++elapsed) {
        cqe = (volatile struct nvme_cq_entry *)c->admin_cq_virt_addr;
        cqe += c->admin_cq_head;

        if (cqe->p == c->admin_cq_phase) {
            cid = cqe->cid;
            sc = cqe->sc;
            sct = cqe->sct;

            if (cid != cmd_id) {
                errno = EIO;
                print_errno("unexpected NVMe admin completion command id");
                return false;
            }

            c->admin_cq_head++;
            if (c->admin_cq_head == c->admin_cq_depth) {
                c->admin_cq_head = 0u;
                c->admin_cq_phase ^= 1u;
            }

            if (controller_reg_write32(c->bar0_virt, c->admin_cq_head_db,
                                       c->admin_cq_head) < 0) {
                return false;
            }

            if (sct != 0u || sc != 0u) {
                errno = EIO;
                print_errno("NVMe admin command completed with error");
                return false;
            }

            return true;
        }

        sleep_1ms();
    }

    errno = ETIMEDOUT;
    print_errno("timeout waiting for NVMe admin completion");
    return false;
}



static bool submit_identify_namespace(struct controller_data *c,
                                      uint64_t identify_phys,
                                      uint16_t cmd_id)
{
    volatile uint32_t *cmd;
    volatile struct nvme_command *sqe;

    sqe = (volatile struct nvme_command *)c->admin_sq_virt_addr + c->admin_sq_tail;
    cmd = (volatile uint32_t *)sqe;

    memset((void *)cmd, 0, NVME_ADMIN_SQ_ENTRY_SIZE);
    cmd[0] = NVME_ADMIN_IDENTIFY_OPCODE | ((uint32_t)cmd_id << 16);
    cmd[1] = c->nsid;
    cmd[6] = (uint32_t)identify_phys;
    cmd[7] = (uint32_t)(identify_phys >> 32);
    cmd[10] = NVME_IDENTIFY_CNS_NAMESPACE;

    c->admin_sq_tail++;
    if (c->admin_sq_tail == c->admin_sq_depth) {
        c->admin_sq_tail = 0u;
    }

    memory_barrier();
    if (controller_reg_write32(c->bar0_virt, c->admin_sq_tail_db, c->admin_sq_tail) < 0) {
        return false;
    }

    return true;
}



static bool submit_admin_command(struct controller_data *c, uint8_t opcode,
                                 uint32_t nsid, uint64_t prp1,
                                 uint32_t cdw10, uint32_t cdw11,
                                 uint16_t cmd_id)
{
    volatile uint32_t *cmd;
    volatile struct nvme_command *sqe;

    sqe = (volatile struct nvme_command *)c->admin_sq_virt_addr + c->admin_sq_tail;
    cmd = (volatile uint32_t *)sqe;

    memset((void *)cmd, 0, NVME_ADMIN_SQ_ENTRY_SIZE);
    cmd[0] = opcode | ((uint32_t)cmd_id << 16);
    cmd[1] = nsid;
    cmd[6] = (uint32_t)prp1;
    cmd[7] = (uint32_t)(prp1 >> 32);
    cmd[10] = cdw10;
    cmd[11] = cdw11;

    c->admin_sq_tail++;
    if (c->admin_sq_tail == c->admin_sq_depth) {
        c->admin_sq_tail = 0u;
    }

    memory_barrier();
    if (controller_reg_write32(c->bar0_virt, c->admin_sq_tail_db, c->admin_sq_tail) < 0) {
        return false;
    }

    return true;
}

/*
    FIOプラグイン内部でジョブの数がけ呼ばれる関数
*/
bool create_io_queue_pair(struct nvme_data *n)
{
    struct controller_data *c;
    uint64_t cap;
    uint32_t timeout_ms;
    uint32_t max_io_depth;
    uint32_t cdw10;
    uint32_t cdw11;
    uint16_t cmd_id;
    uint16_t qid;
    uint32_t io_sq_depth;
    uint32_t io_cq_depth;
    size_t io_sq_size;
    size_t io_cq_size;
    size_t io_prp_list_size;

    if (n == NULL) {
        errno = EINVAL;
        print_errno("invalid NVMe data");
        return false;
    }

    c = nvme_ctrl(n);
    if (c->bar0_virt == NULL) {
        errno = EINVAL;
        print_errno("NVMe controller BAR0 is not mapped");
        return false;
    }
    if (c->admin_sq_virt_addr == NULL) {
        errno = EINVAL;
        print_errno("NVMe admin SQ is not initialized");
        return false;
    }
    if (c->admin_cq_virt_addr == NULL) {
        errno = EINVAL;
        print_errno("NVMe admin CQ is not initialized");
        return false;
    }

    if (!c->dma_fd_opened) {
        errno = EINVAL;
        print_errno("NVMe DMA allocator is not initialized");
        return false;
    }

    if (nvme_reg_read64(c->bar0_virt, NVME_REG_CAP, &cap) < 0) {
        return false;
    }

    qid = nvme_io_qid(n);
    timeout_ms = ready_timeout_ms(cap);
    if (timeout_ms == 0u) {
        errno = EIO;
        print_errno("NVMe CAP.TO is zero");
        return false;
    }

    max_io_depth = cap_mqes_entries(cap);

    io_sq_depth = n->io_sq_depth;
    if (io_sq_depth == 0u) {
        io_sq_depth = max_io_depth;
    }
    if (io_sq_depth > max_io_depth) {
        errno = EINVAL;
        print_errno("NVMe IO SQ depth exceeds CAP.MQES");
        return false;
    }

    io_cq_depth = n->io_cq_depth;
    if (io_cq_depth == 0u) {
        io_cq_depth = max_io_depth;
    }
    if (io_cq_depth > max_io_depth) {
        errno = EINVAL;
        print_errno("NVMe IO CQ depth exceeds CAP.MQES");
        return false;
    }

    printf("NVMe CAP=0x%016llx MQES=%u max_io_depth=%u "
           "io_sq_depth=%u io_cq_depth=%u\n",
           (unsigned long long)cap, (unsigned int)(cap & 0xffffu),
           max_io_depth, io_sq_depth, io_cq_depth);

    io_sq_size = round_up_page(NVME_IO_SQ_ENTRY_SIZE * (size_t)io_sq_depth);
    io_cq_size = round_up_page(NVME_IO_CQ_ENTRY_SIZE * (size_t)io_cq_depth);
    io_prp_list_size = NVME_PAGE_SIZE * (size_t)io_sq_depth;

    if (!nvme_alloc_dma_mem(c, io_cq_size, &n->io_cq_mem)) {
        return false;
    }

    if (!nvme_alloc_dma_mem(c, io_sq_size, &n->io_sq_mem)) {
        free_dma(&n->io_cq_mem);
        return false;
    }

    if (!nvme_alloc_dma_mem(c, io_prp_list_size, &n->io_prp_list_mem)) {
        free_dma(&n->io_sq_mem);
        free_dma(&n->io_cq_mem);
        return false;
    }

    memset(dma_mem_virt(&n->io_cq_mem), 0, dma_mem_size(&n->io_cq_mem));
    memset(dma_mem_virt(&n->io_sq_mem), 0, dma_mem_size(&n->io_sq_mem));
    memset(dma_mem_virt(&n->io_prp_list_mem), 0,
           dma_mem_size(&n->io_prp_list_mem));

    cmd_id = c->admin_cmd_id++;
    cdw10 = qid | ((uint32_t)(io_cq_depth - 1u) << 16);
    cdw11 = NVME_QUEUE_PHYS_CONTIGUOUS;

    if (!submit_admin_command(c, NVME_ADMIN_CREATE_IO_CQ_OPCODE, 0u,
                              dma_mem_phys(&n->io_cq_mem), cdw10, cdw11,
                              cmd_id)) {
        return false;
    }

    if (!wait_admin_completion(c, cmd_id, NVME_ADMIN_CMD_TIMEOUT_MS)) {
        return false;
    }

    cmd_id = c->admin_cmd_id++;
    cdw10 = qid | ((uint32_t)(io_sq_depth - 1u) << 16);
    cdw11 = ((uint32_t)qid << 16) | NVME_QUEUE_PHYS_CONTIGUOUS;

    if (!submit_admin_command(c, NVME_ADMIN_CREATE_IO_SQ_OPCODE, 0u,
                              dma_mem_phys(&n->io_sq_mem), cdw10, cdw11,
                              cmd_id)) {
        return false;
    }

    if (!wait_admin_completion(c, cmd_id, NVME_ADMIN_CMD_TIMEOUT_MS)) {
        return false;
    }

    n->io_sq_head = 0u;
    n->io_sq_tail = 0u;
    n->ic_cq_head = 0u;
    n->io_cq_pahse = 1u;
    n->io_sq_phys_addr = dma_mem_phys(&n->io_sq_mem);
    n->io_cq_phys_addr = dma_mem_phys(&n->io_cq_mem);
    n->io_prp_list_phys_addr = dma_mem_phys(&n->io_prp_list_mem);
    n->io_sq_virt_addr = n->io_sq_mem.user_virtaddr;
    n->io_cq_virt_addr = n->io_cq_mem.user_virtaddr;
    n->io_prp_list_virt_addr = n->io_prp_list_mem.user_virtaddr;
    n->io_sq_depth = io_sq_depth;
    n->io_cq_depth = io_cq_depth;
    n->io_sq_tail_db = io_sq_tail_doorbell(cap, qid);
    n->io_cq_head_db = io_cq_head_doorbell(cap, qid);

    return true;
}

bool delete_io_queue_pair(struct nvme_data *n)
{
    struct controller_data *c;
    uint64_t cap;
    uint32_t timeout_ms;
    uint32_t cdw10;
    uint16_t cmd_id;
    uint16_t qid;

    if (n == NULL) {
        errno = EINVAL;
        print_errno("invalid NVMe data");
        return false;
    }

    c = nvme_ctrl(n);
    if (c->bar0_virt == NULL) {
        errno = EINVAL;
        print_errno("NVMe controller BAR0 is not mapped");
        return false;
    }
    if (c->admin_sq_virt_addr == NULL) {
        errno = EINVAL;
        print_errno("NVMe admin SQ is not initialized");
        return false;
    }
    if (c->admin_cq_virt_addr == NULL) {
        errno = EINVAL;
        print_errno("NVMe admin CQ is not initialized");
        return false;
    }
    if (n->io_sq_virt_addr == NULL && n->io_cq_virt_addr == NULL &&
        n->io_u_data_virt_addr == NULL && n->io_u_data_phys_addr == 0u) {
        return true;
    }

    if (nvme_reg_read64(c->bar0_virt, NVME_REG_CAP, &cap) < 0) {
        return false;
    }

    qid = nvme_io_qid(n);
    timeout_ms = ready_timeout_ms(cap);
    if (timeout_ms == 0u) {
        errno = EIO;
        print_errno("NVMe CAP.TO is zero");
        return false;
    }

    if (n->io_sq_virt_addr != NULL) {
        cmd_id = c->admin_cmd_id++;
        cdw10 = qid;
        if (!submit_admin_command(c, NVME_ADMIN_DELETE_IO_SQ_OPCODE, 0u, 0u,
                                  cdw10, 0u, cmd_id)) {
            return false;
        }
        if (!wait_admin_completion(c, cmd_id, NVME_ADMIN_CMD_TIMEOUT_MS)) {
            return false;
        }
    }

    if (n->io_cq_virt_addr != NULL) {
        cmd_id = c->admin_cmd_id++;
        cdw10 = qid;
        if (!submit_admin_command(c, NVME_ADMIN_DELETE_IO_CQ_OPCODE, 0u, 0u,
                                  cdw10, 0u, cmd_id)) {
            return false;
        }
        if (!wait_admin_completion(c, cmd_id, NVME_ADMIN_CMD_TIMEOUT_MS)) {
            return false;
        }
    }

    n->io_sq_head = 0u;
    n->io_sq_tail = 0u;
    n->ic_cq_head = 0u;
    n->io_cq_pahse = 0u;
    n->io_sq_phys_addr = 0u;
    n->io_cq_phys_addr = 0u;
    n->io_prp_list_phys_addr = 0u;
    n->io_u_data_phys_addr = 0u;
    n->io_sq_virt_addr = NULL;
    n->io_cq_virt_addr = NULL;
    n->io_prp_list_virt_addr = NULL;
    n->io_u_data_virt_addr = NULL;
    free_dma(&n->io_sq_mem);
    free_dma(&n->io_cq_mem);
    free_dma(&n->io_prp_list_mem);
    free_dma(&n->io_u_data_mem);
    n->io_sq_depth = 0u;
    n->io_cq_depth = 0u;
    n->io_sq_tail_db = 0u;
    n->io_cq_head_db = 0u;

    return true;
}


static inline uint16_t submit_rw_command(struct nvme_data *n, uint8_t opcode,
                                         uint64_t offset, uint64_t size,
                                         uint64_t dma_addr)
{
    struct controller_data *c;
    volatile struct nvme_command *cmd;
    uint64_t transfer_size;
    uint64_t prp1;
    uint64_t prp2;
    uint32_t sq_slot;
    uint16_t cmd_id;

    c = nvme_ctrl(n);
    transfer_size = size << c->lba_shift;
    sq_slot = n->io_sq_tail;
    setup_prps(n, sq_slot, dma_addr, transfer_size, &prp1, &prp2);

    cmd_id = ++n->io_cmd_id;

    cmd = (volatile struct nvme_command *)n->io_sq_virt_addr + sq_slot;

    memset((void *)cmd, 0, sizeof(*cmd));
    cmd->opcode = opcode;
    cmd->command_id = cmd_id;
    cmd->nsid = c->nsid;
    cmd->prp1 = prp1;
    cmd->prp2 = prp2;
    cmd->slba = offset;
    cmd->length = (uint32_t)(size - 1u);

    ++n->io_sq_tail;
    if (n->io_sq_tail == n->io_sq_depth) {
        n->io_sq_tail = 0u;
    }

    memory_barrier();
    nvme_reg_write32_fast(c->bar0_virt, n->io_sq_tail_db, n->io_sq_tail);

    return cmd_id;
}

uint16_t nvme_read(struct nvme_data *n, uint64_t offset, uint64_t size, uint64_t dma_addr)
{
    return submit_rw_command(n, NVME_IO_READ_OPCODE, offset, size, dma_addr);
}

uint16_t nvme_write(struct nvme_data *n, uint64_t offset, uint64_t size, uint64_t dma_addr)
{
    return submit_rw_command(n, NVME_IO_WRITE_OPCODE, offset, size, dma_addr);
}

uint16_t nvme_trim(struct nvme_data *n, uint64_t offset, uint64_t size)
{
    struct controller_data *c;
    volatile uint32_t *cmd;
    volatile struct nvme_command *sqe;
    struct nvme_dsm_range *range;
    uint64_t range_offset;
    uint64_t range_phys;
    uint32_t sq_slot;
    uint16_t cmd_id;

    c = nvme_ctrl(n);
    sq_slot = n->io_sq_tail;
    
    range_offset = (uint64_t)sq_slot << NVME_PAGE_SHIFT; /* 1pageの倍数 */
    range = (struct nvme_dsm_range *)((uint8_t *)n->io_prp_list_virt_addr + range_offset);
    range_phys = n->io_prp_list_phys_addr + range_offset;
    
    range->context_attributes = 0u;
    range->length = (uint32_t)size;
    range->slba = offset;

    cmd_id = ++n->io_cmd_id;

    sqe = (volatile struct nvme_command *)n->io_sq_virt_addr + sq_slot;

    cmd = (volatile uint32_t *)sqe;

    memset((void *)cmd, 0, NVME_IO_SQ_ENTRY_SIZE);
    cmd[0] = NVME_IO_DATASET_MANAGEMENT_OPCODE | ((uint32_t)cmd_id << 16);
    cmd[1] = c->nsid;
    cmd[6] = (uint32_t)range_phys;
    cmd[7] = (uint32_t)(range_phys >> 32);
    cmd[10] = 0u;
    cmd[11] = NVME_DSM_ATTR_DEALLOCATE;

    ++n->io_sq_tail;
    if (n->io_sq_tail == n->io_sq_depth) {
        n->io_sq_tail = 0u;
    }

    memory_barrier();
    nvme_reg_write32_fast(c->bar0_virt, n->io_sq_tail_db, n->io_sq_tail);

    return cmd_id;
}

uint16_t nvme_multi_range_trim(struct nvme_data *n, struct io_u *io_u)
{
    struct controller_data *c;
    volatile uint32_t *cmd;
    volatile struct nvme_command *sqe;
    struct nvme_dsm_range *range;
    struct trim_range *tr;
    uint64_t range_offset;
    uint64_t range_phys;
    uint32_t sq_slot;
    uint32_t num_range;
    uint32_t i;
    uint16_t cmd_id;

    c = nvme_ctrl(n);
    sq_slot = n->io_sq_tail;

    range_offset = (uint64_t)sq_slot << NVME_PAGE_SHIFT; /* 4KiBの倍数 */
    range = (struct nvme_dsm_range *)((uint8_t *)n->io_prp_list_virt_addr + range_offset);
    range_phys = n->io_prp_list_phys_addr + range_offset;
    tr = (struct trim_range *)io_u->xfer_buf;
    num_range = io_u->number_trim;

    for (i = 0; i < num_range; i++) {
        range->context_attributes = 0u;
        range->length = (uint32_t)(tr->len >> c->lba_shift);
        range->slba = tr->start >> c->lba_shift;
        range++;
        tr++;
    }

    cmd_id = ++n->io_cmd_id;

    sqe = (volatile struct nvme_command *)n->io_sq_virt_addr + sq_slot;
    cmd = (volatile uint32_t *)sqe;

    memset((void *)cmd, 0, NVME_IO_SQ_ENTRY_SIZE);
    cmd[0] = NVME_IO_DATASET_MANAGEMENT_OPCODE | ((uint32_t)cmd_id << 16);
    cmd[1] = c->nsid;
    cmd[6] = (uint32_t)range_phys;
    cmd[7] = (uint32_t)(range_phys >> 32);
    cmd[10] = num_range - 1u;
    cmd[11] = NVME_DSM_ATTR_DEALLOCATE;

    ++n->io_sq_tail;
    if (n->io_sq_tail == n->io_sq_depth) {
        n->io_sq_tail = 0u;
    }

    memory_barrier();
    nvme_reg_write32_fast(c->bar0_virt, n->io_sq_tail_db, n->io_sq_tail);

    return cmd_id;
}

uint16_t poll_cq(struct nvme_data *n, int max)
{
    struct controller_data *c;
    volatile struct nvme_cq_entry *cq_base;
    volatile struct nvme_cq_entry *cqe;
    uint16_t count;
    uint32_t head;
    uint16_t sc;
    uint16_t sct;
    uint16_t psf;
    uint16_t max_count;
    uint8_t phase_tag;

    if (max <= 0) {
        return 0u;
    }

    if (max > UINT16_MAX) {
        max_count = UINT16_MAX;
    } else {
        max_count = (uint16_t)max;
    }

    c = nvme_ctrl(n);
    count = 0u;
    head = n->ic_cq_head;
    cq_base = (volatile struct nvme_cq_entry *)n->io_cq_virt_addr;
    cqe = cq_base + head;
    phase_tag = n->io_cq_pahse;

    for (;;) {
        struct io_u *io_u;

        if (count == max_count) {
            break;
        }

        psf = cqe->psf;

        if ((psf & NVME_CQE_PHASE_MASK) != phase_tag) {
            break;
        }

        sc = (psf >> NVME_CQE_SC_SHIFT) & NVME_CQE_SC_MASK;
        sct = (psf >> NVME_CQE_SCT_SHIFT) & NVME_CQE_SCT_MASK;
        if (sct != 0u || sc != 0u) {
            fprintf(stderr,
                    "NVMe IO command failed: cid=%u sqid=%u sqhd=%u "
                    "sct=%u sc=%u psf=0x%04x\n",
                    cqe->cid, cqe->sqid, cqe->sqhd, sct, sc, psf);
            exit(EXIT_FAILURE);
        }

        io_u = n->cmd_io_u[cqe->cid];
        if (io_u == NULL) {
            fprintf(stderr, "NVMe IO completion has unknown cid=%u\n",
                    cqe->cid);
            exit(EXIT_FAILURE);
        }
        n->events[n->nr_events + count] = io_u;
        n->cmd_io_u[cqe->cid] = NULL;
        n->io_sq_head = cqe->sqhd;

        ++head;
        ++count;
        ++cqe;

        if (head == n->io_cq_depth) {
            head = 0u;
            phase_tag ^= 1u;
            cqe = cq_base;
        }
    }

    if (count != 0u) {
        n->ic_cq_head = head;
        n->io_cq_pahse = phase_tag;
        n->nr_events += count;
        nvme_reg_write32_fast(c->bar0_virt, n->io_cq_head_db, head);
    }

    return count;
}

uint64_t nvme_get_file_size(struct controller_data *c)
{
    struct mem identify;
    uint8_t *data;
    uint64_t nsze;
    
    uint64_t cap;
    uint32_t timeout_ms;
    uint16_t cmd_id;
    uint8_t flbas;
    uint8_t lbaf_index;
    uint8_t lba_shift;
    size_t lbaf_offset;

    if (c == NULL || c->bar0_virt == NULL ||
        c->admin_sq_virt_addr == NULL || c->admin_cq_virt_addr == NULL) {
        errno = EINVAL;
        print_errno("invalid NVMe controller data");
        return 0u;
    }

    if (!c->dma_fd_opened) {
        errno = EINVAL;
        print_errno("NVMe DMA allocator is not initialized");
        return 0u;
    }

    if (nvme_reg_read64(c->bar0_virt, NVME_REG_CAP, &cap) < 0) {
        return 0u;
    }

    timeout_ms = ready_timeout_ms(cap);
    if (timeout_ms == 0u) {
        errno = EIO;
        print_errno("NVMe CAP.TO is zero");
        return 0u;
    }

    if (!nvme_alloc_dma_mem(c, NVME_IDENTIFY_DATA_SIZE, &identify)) {
        return 0u;
    }
    memset(dma_mem_virt(&identify), 0, dma_mem_size(&identify));

    cmd_id = c->admin_cmd_id++;

    if (!submit_identify_namespace(c, dma_mem_phys(&identify), cmd_id)) {
        free_dma(&identify);
        return 0u;
    }

    if (!wait_admin_completion(c, cmd_id, NVME_ADMIN_CMD_TIMEOUT_MS)) {
        free_dma(&identify);
        return 0u;
    }

    data = dma_mem_virt(&identify);
    nsze = load_le64(data + NVME_IDENTIFY_NS_NSZE_OFFSET);
    flbas = data[NVME_IDENTIFY_NS_FLBAS_OFFSET];
    lbaf_index = flbas & 0x0fu;
    
    lbaf_offset = NVME_IDENTIFY_NS_LBAF_OFFSET + (NVME_LBAF_ENTRY_SIZE * (size_t)lbaf_index);
    lba_shift = data[lbaf_offset + NVME_LBAF_LBADS_OFFSET];
    if (lba_shift >= 64u) {
        errno = EIO;
        print_errno("invalid NVMe namespace LBA shift");
        free_dma(&identify);
        return 0u;
    }

    c->lba_shift = lba_shift;
    c->size = nsze << lba_shift;
    free_dma(&identify);

    return c->size;
}
