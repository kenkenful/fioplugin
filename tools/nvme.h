#ifndef NVME_H
#define NVME_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "dma.h"

/*　コントローラにつき1つもつ構造体 */
struct controller_data {
    uint16_t bus;
    uint16_t dev;
    uint16_t func;
    uint16_t nsid;

    uint64_t bar0_phys;
    void*    bar0_virt;

    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint8_t  admin_cq_phase;

    uint64_t admin_sq_phys_addr;
    void *admin_sq_virt_addr;
    struct mem admin_sq_mem;
    
    uint64_t admin_cq_phys_addr;
    void *admin_cq_virt_addr;
    struct mem admin_cq_mem;

    uint16_t admin_sq_depth;
    uint16_t admin_cq_depth;

    uint16_t admin_cmd_id;

    uint32_t admin_sq_tail_db;
    uint32_t admin_cq_head_db;

    int lba_shift;
    uint64_t size;

    int dma_fd;
    bool dma_fd_opened;
};

/* スレッドごとにもつ構造体 */
struct nvme_data{
    struct controller_data *ctrl;

    uint32_t io_sq_head;
    uint32_t io_sq_tail;
    uint32_t ic_cq_head;

    uint8_t io_cq_pahse;

    uint64_t io_sq_phys_addr;
    void *io_sq_virt_addr;
    struct mem io_sq_mem;
    
    uint64_t io_cq_phys_addr;
    void *io_cq_virt_addr;
    struct mem io_cq_mem;
    
    /* prp listのメモリを予め取得しておいて使いまわす. サイズは、1page(4KiB)*io_sq_depth */
    uint64_t io_prp_list_phys_addr;  /* prp listの物理アドレス */  
    void *io_prp_list_virt_addr;     /* prp listの仮想アドレス */
    struct mem io_prp_list_mem;      /* prp listのmem構造体 */

    /* iomem_alloc, iomem_freeコールバックで設定される */
    uint64_t io_u_data_phys_addr;
    void *io_u_data_virt_addr;
    struct mem io_u_data_mem;


    uint32_t io_sq_depth;
    uint32_t io_cq_depth;
    uint16_t io_qid;

    uint16_t io_cmd_id;

    uint32_t io_sq_tail_db;
    uint32_t io_cq_head_db;

    struct io_u *cmd_io_u[65536];  
    struct io_u *events[65536];
    int nr_events;

};

bool controller_init(struct controller_data *c);
void controller_deinit(struct controller_data *c);
void nvme_data_init(struct nvme_data *n);
bool nvme_alloc_dma_mem(struct controller_data *c, size_t size, struct mem *mem);
void nvme_free_dma_mem(struct mem *mem);
uint64_t nvme_get_file_size(struct controller_data *n);
struct controller_data *parse_target(const char *filename);
bool create_io_queue_pair(struct nvme_data *n);
bool delete_io_queue_pair(struct nvme_data *n);

uint16_t nvme_read(struct nvme_data *n, uint64_t offset, uint64_t size, uint64_t dma_addr);
uint16_t nvme_write(struct nvme_data *n, uint64_t offset, uint64_t size, uint64_t dma_addr);
uint16_t nvme_trim(struct nvme_data *n, uint64_t offset, uint64_t size);
uint16_t nvme_multi_range_trim(struct nvme_data *n, struct io_u *io_u);

uint16_t poll_cq(struct nvme_data *n, int max);

#endif
