#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvme.h"




int main(){

    struct nvme_data n;
    struct controller_data ctrl;
    struct mem read_data, write_data;

    nvme_data_init(&n);
    memset(&ctrl, 0, sizeof(ctrl));
    n.ctrl = &ctrl;
    
    n.ctrl->bus = 5;
    n.ctrl->dev = 0;
    n.ctrl->func = 0;
    n.ctrl->nsid = 1;

    bool ok = controller_init(n.ctrl);
    if (!ok) {
        fprintf(stderr, "controller_init failed\n");
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    n.io_sq_depth = 1024;
    n.io_cq_depth = 1024;
    if (!create_io_queue_pair(&n)) {
        fprintf(stderr, "create_io_queue_pair failed\n");
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }


    if (!nvme_alloc_dma_mem(n.ctrl, 4096*10, &write_data)) {
        fprintf(stderr, "nvme_alloc_dma_mem failed\n");
        delete_io_queue_pair(&n);
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    if (!nvme_alloc_dma_mem(n.ctrl, 4096*10, &read_data)) {
        fprintf(stderr, "nvme_alloc_dma_mem failed\n");
        nvme_free_dma_mem(&write_data);
        delete_io_queue_pair(&n);
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }


    int j;

    uint8_t *p = (uint8_t*)write_data.user_virtaddr;

    for(j=0; j<100; ++j)p[j] = j;

    int cmd_id = nvme_write(&n, 0, 5, write_data.dma_addr);
    printf("nvme_write submitted: cid=%d\n", cmd_id);

    uint16_t completions = 0;
    
    for(;;){
       completions = poll_cq(&n, 1);
        if (completions != 0u) {
            break;
        }
    }

    cmd_id = nvme_read(&n, 0, 5, read_data.dma_addr);
    printf("nvme_read submitted: cid=%d\n", cmd_id);

    completions = 0;

    for(;;){
       completions = poll_cq(&n, 1);
        if (completions != 0u) {
            break;
        }
    }


    p = (uint8_t*)read_data.user_virtaddr;

    printf("%x, %x, %x, %x, %x\n", p[0],p[1],p[2],p[3],p[4]);

    cmd_id = nvme_trim(&n, 0, 5);
    
    completions = 0;

    for(;;){
       completions = poll_cq(&n, 1);
        if (completions != 0u) {
            break;
        }
    }

    cmd_id = nvme_read(&n, 0, 5, read_data.dma_addr);
    
    printf("nvme_read submitted: cid=%d\n", cmd_id);

    completions = 0;

    for(;;){
       completions = poll_cq(&n, 1);
        if (completions != 0u) {
            break;
        }
    }


    p = (uint8_t*)read_data.user_virtaddr;

    printf("%x, %x, %x, %x, %x\n", p[0],p[1],p[2],p[3],p[4]);

    nvme_free_dma_mem(&read_data);
    nvme_free_dma_mem(&write_data);

    if (!delete_io_queue_pair(&n)) {
        fprintf(stderr, "delete_io_queue_pair failed\n");
        controller_deinit(n.ctrl);
        return EXIT_FAILURE;
    }

    controller_deinit(n.ctrl);
    printf("controller_deinit completed\n");

    return EXIT_SUCCESS;


}
