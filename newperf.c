#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../fio.h"
#include "tools/nvme.h"


#define FIO_HAS_MRT (FIO_IOOPS_VERSION >= 34)


static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static int curr_thread_num;
static uint16_t next_io_qid = 1;
static struct controller_data *g_ctrl;

static uint32_t next_sq_tail(const struct nvme_data *n)
{
	uint32_t tail = n->io_sq_tail + 1;

	if (tail == n->io_sq_depth)
		tail = 0;

	return tail;
}

static const char *target_name(struct thread_data *td, struct fio_file *f)
{
	if (f && f->file_name) return f->file_name;
	if (td->files_index && td->files[0] && td->files[0]->file_name)
		return td->files[0]->file_name;
	return td->o.filename;
}


static int nvme_setup(struct thread_data *td)
{
	struct fio_file *f;
	unsigned int i;
	int ret = 0;

	if (td->o.nr_files > 1) {
		log_err("fio: this ioengine does not support multiple files\n");
		return 1;
	}

	pthread_mutex_lock(&g_mutex);

	if (!g_ctrl) {
		g_ctrl = parse_target(target_name(td, NULL));
		if (!g_ctrl) {
			td_verror(td, EINVAL, "parse_target failed");
			pthread_mutex_unlock(&g_mutex);
			return 1;
		}

		if (!controller_init(g_ctrl)) {
			free(g_ctrl);
			g_ctrl = NULL;
			td_verror(td, EINVAL, "controller_init failed");
			pthread_mutex_unlock(&g_mutex);
			return 1;
		}
	}
	pthread_mutex_unlock(&g_mutex);

	for_each_file(td, f, i) {
		if (td_io_get_file_size(td, f)) {
			ret = 1;
			break;
		}
	}

	return ret;
}

static int nvme_init(struct thread_data *td)
{
	struct nvme_data *n;
	bool ret;

	if (!td->o.use_thread) {
		log_err("fio: this ioengine requires --thread=1\n");
		return 1;
	}

	if (td->o.nr_files > 1) {
		log_err("fio: this ioengine does not support multiple files\n");
		return 1;
	}

	n = calloc(1, sizeof(*n));
	if (!n) {
		td_verror(td, ENOMEM, "calloc");
		return 1;
	}

	pthread_mutex_lock(&g_mutex);

	if (!g_ctrl) {
		pthread_mutex_unlock(&g_mutex);
		free(n);
		td_verror(td, EINVAL, "nvme_init called before nvme_setup");
		return 1;
	}

	n->ctrl = g_ctrl;
	n->io_qid = next_io_qid++;
	curr_thread_num++;
	td->io_ops_data = n;

	n->io_sq_depth = td->o.iodepth + 1;
	n->io_cq_depth = td->o.iodepth + 1;
	if (n->io_sq_depth < 2) {
		n->io_sq_depth = 2;
		n->io_cq_depth = 2;
	}

	ret = create_io_queue_pair(n);

	if(!ret){
		curr_thread_num--;
		if (curr_thread_num == 0) {
			controller_deinit(g_ctrl);
			free(g_ctrl);
			g_ctrl = NULL;
			next_io_qid = 1;
		}
		pthread_mutex_unlock(&g_mutex);
		td->io_ops_data = NULL;
		free(n);
		td_verror(td, EINVAL, "create_io_queue_pair failed");
		return 1;
	}

	pthread_mutex_unlock(&g_mutex);

	return 0;
}


static void nvme_cleanup(struct thread_data *td)
{
	struct nvme_data *n = td->io_ops_data;

	if (!n)return;

	pthread_mutex_lock(&g_mutex);
		delete_io_queue_pair(n);

		if(--curr_thread_num == 0){
			controller_deinit(g_ctrl);
			free(g_ctrl);
			g_ctrl = NULL;
			next_io_qid = 1;
		}
			
	pthread_mutex_unlock(&g_mutex);

	free(n);
	td->io_ops_data = NULL;
}

static int nvme_size(struct thread_data *td, struct fio_file *f)
{
	uint64_t size;

	pthread_mutex_lock(&g_mutex);
	if (!g_ctrl) {
		td_verror(td, EINVAL, "nvme_get_file_size called before nvme_setup");
		pthread_mutex_unlock(&g_mutex);
		return 1;
	}

	size = nvme_get_file_size(g_ctrl);
	pthread_mutex_unlock(&g_mutex);

	if (size == 0) {
		td_verror(td, EINVAL, "nvme_get_file_size failed");
		return 1;
	}

	f->real_file_size = size;
	fio_file_set_size_known(f);
	return 0;
}

static int nvme_open_file(struct thread_data fio_unused *td,
				 struct fio_file fio_unused *f)
{
	return 0;
}

static int nvme_close_file(struct thread_data fio_unused *td,
				  struct fio_file fio_unused *f)
{
	return 0;
}

static enum fio_q_status nvme_queue(struct thread_data *td, struct io_u *io_u)
{
	uint16_t cid;
	uint16_t next_cid;
	struct nvme_data *n = td->io_ops_data;
	uint64_t offset;
	uint64_t len;

	if (next_sq_tail(n) == n->io_sq_head)
		return FIO_Q_BUSY;

	next_cid = n->io_cmd_id + 1;
	if (n->cmd_io_u[next_cid] != NULL)
		return FIO_Q_BUSY;

	if ((io_u->offset | io_u->xfer_buflen) & ((1ULL << n->ctrl->lba_shift) - 1)) 
	{
		io_u->error = EINVAL;
		return FIO_Q_COMPLETED;
	}

	offset = io_u->offset >> n->ctrl->lba_shift;
	len = io_u->xfer_buflen >> n->ctrl->lba_shift;

	switch (io_u->ddir) {
	case DDIR_READ:
	case DDIR_WRITE: {
		uintptr_t base = (uintptr_t)td->orig_buffer;
		uintptr_t buf = (uintptr_t)io_u->xfer_buf;
		uintptr_t phys_offset = buf - base;
		uint64_t data_phys_addr;

		if (len == 0) {
			io_u->error = EINVAL;
			return FIO_Q_COMPLETED;
		}

		data_phys_addr = n->io_u_data_phys_addr + phys_offset;
		if (io_u->ddir == DDIR_READ)
			cid = nvme_read(n, offset, len, data_phys_addr);
		else
			cid = nvme_write(n, offset, len, data_phys_addr);
		break;
	}
	case DDIR_TRIM:
		if (len == 0) {
			io_u->error = EINVAL;
			return FIO_Q_COMPLETED;
		}

#if FIO_HAS_MRT
		if (td->o.num_range == 1) {
			cid = nvme_trim(n, offset, len);
		} else {
			cid = nvme_multi_range_trim(n, io_u);
		}
#else
		cid = nvme_trim(n, offset, len);
#endif
		break;

	default:
		io_u->error = EINVAL;
		return FIO_Q_COMPLETED;
	}

	n->cmd_io_u[cid] = io_u;

	return FIO_Q_QUEUED;
}


static int nvme_getevents(struct thread_data *td, unsigned int fio_unused min,
				 unsigned int max,
				 const struct timespec fio_unused *t)
{
	struct timespec t0, t1;
	uint64_t timeout = 0;
	struct nvme_data *n = td->io_ops_data;

	int completions = 0;
	n->nr_events = 0;

	if (t) {
		timeout = t->tv_sec * 1000000000L + t->tv_nsec;
		clock_gettime(CLOCK_MONOTONIC_RAW, &t0);
	}

	for(;;){

		completions += poll_cq(n, max - completions);

		if(completions >= (int)min){
			break;
		}

		if (t) {
			uint64_t elapse;

			clock_gettime(CLOCK_MONOTONIC_RAW, &t1);
			elapse = ((t1.tv_sec - t0.tv_sec) * 1000000000L) + t1.tv_nsec - t0.tv_nsec;
			if (elapse > timeout) {
				break;
			}
		}
	}

	return completions;

}

static struct io_u *nvme_event(struct thread_data *td, int event)
{
	struct nvme_data *n = td->io_ops_data;

	struct io_u *io_u;

	if (!n || event < 0)
		return NULL;

	if (event >= n->nr_events)
		io_u = NULL;
	else
		io_u = n->events[event];

	return io_u;
}



static int nvme_alloc(struct thread_data *td, size_t total_sz)
{
	struct mem mem = {0};

	struct nvme_data *n = td->io_ops_data;

	if (!n || !n->ctrl) {
		td_verror(td, EINVAL, "nvme_alloc without initialized engine data");
		return 1;
	}

	if (!nvme_alloc_dma_mem(n->ctrl, total_sz, &mem)) {
		fprintf(stderr, "nvme_alloc_dma_mem failed\n");
		return 1;
	}

	n->io_u_data_phys_addr = mem.dma_addr;
	n->io_u_data_virt_addr = mem.user_virtaddr;
	n->io_u_data_mem = mem;

	td->orig_buffer = mem.user_virtaddr;

	return td->orig_buffer == NULL;

}

static void nvme_free(struct thread_data *td)
{
	struct nvme_data *n = td->io_ops_data;

	if (!n)
		return;

	nvme_free_dma_mem(&n->io_u_data_mem);

}


struct ioengine_ops ioengine = {

	.name		= "newperf",

	.version	= FIO_IOOPS_VERSION,
	.setup		= nvme_setup,
	.init		= nvme_init,

	.cleanup	= nvme_cleanup,
	.queue		= nvme_queue,
	.getevents	= nvme_getevents,
	.event		= nvme_event,
	.open_file	= nvme_open_file,
	.close_file	= nvme_close_file,
	.get_file_size	= nvme_size,

	.iomem_alloc = nvme_alloc,
	.iomem_free = nvme_free,

#if FIO_HAS_MRT
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN | FIO_DISKLESSIO | FIO_MULTI_RANGE_TRIM,
#else
	.flags			= FIO_RAWIO | FIO_NOEXTEND | FIO_NODISKUTIL | FIO_MEMALIGN | FIO_DISKLESSIO,
#endif
};
