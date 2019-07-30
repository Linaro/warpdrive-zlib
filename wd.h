// SPDX-License-Identifier: GPL-2.0
#ifndef __WD_H
#define __WD_H
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <limits.h>
#include "uacce.h"

#define SYS_VAL_SIZE		16
#define PATH_STR_SIZE		256
#define MAX_ATTR_STR_SIZE	256
#define WD_NAME_SIZE		64

typedef int bool;

#ifndef true
#define true 1
#endif

#ifndef false
#define false 0
#endif

#ifndef WD_ERR
#ifndef WITH_LOG_FILE
#define WD_ERR(format, args...) fprintf(stderr, format, ##args)
#else
extern FILE *flog_fd;
#define WD_ERR(format, args...)				\
	if (!flog_fd)					\
		flog_fd = fopen(WITH_LOG_FILE, "a+");	\
	if (flog_fd)					\
		fprintf(flog_fd, format, ##args);	\
	else						\
		fprintf(stderr, "log %s not exists!",	\
			WITH_LOG_FILE);
#endif
#endif

#ifdef DEBUG_LOG
#define dbg(msg, ...) fprintf(stderr, msg, ##__VA_ARGS__)
#else
#define dbg(msg, ...)
#endif

static inline void wd_reg_write(void *reg_addr, uint32_t value)
{
	*((volatile uint32_t *)reg_addr) = value;
	__sync_synchronize();
}

static inline uint32_t wd_reg_read(void *reg_addr)
{
	uint32_t temp;

	temp = *((volatile uint32_t *)reg_addr);
	__sync_synchronize();

	return temp;
}

#define WD_CAPA_PRIV_DATA_SIZE	64

#define alloc_obj(objp) do { \
	objp = malloc(sizeof(*objp)); \
	memset(objp, 0, sizeof(*objp)); \
} while (0)

#define free_obj(objp) do { \
	if (objp) \
		free(objp); \
} while (0)

/* Capabilities */
struct wd_capa {
	char *alg;
	int throughput;
	int latency;
	__u32 flags;
	__u8 priv[WD_CAPA_PRIV_DATA_SIZE];/* For algorithm parameters */
};

struct wd_queue {
	struct wd_capa capa;
	char *hw_type;
	int hw_type_id;
	void *priv; /* private data used by the drv layer */
	void *dev_info;
	int fd;
	int iommu_type;
	char dev_path[PATH_STR_SIZE];
	void *ss_va;
	void *ss_pa;
	int dev_flags;
	unsigned long qfrs_offset[UACCE_QFRT_MAX];
};

static inline void *wd_get_pa_from_va(struct wd_queue *q, void *va)
{
	return va - q->ss_va + q->ss_pa;
}

static inline void *wd_get_va_from_pa(struct wd_queue *q, void *pa)
{
	return pa - q->ss_pa + q->ss_va;
}

extern int wd_request_queue(struct wd_queue *q);
extern void wd_release_queue(struct wd_queue *q);
extern int wd_send(struct wd_queue *q, void *req);
extern int wd_recv(struct wd_queue *q, void **resp);
extern void wd_flush(struct wd_queue *q);
extern int wd_recv_sync(struct wd_queue *q, void **resp, __u16 ms);
extern void *wd_reserve_memory(struct wd_queue *q, size_t size);
extern int wd_share_reserved_memory(struct wd_queue *q,
				    struct wd_queue *target_q);
#endif
