// SPDX-License-Identifier: GPL-2.0+
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>
#include <assert.h>
#include "wd_zlib.h"

#define DEFLATE			0
#define INFLATE			1

#define ASIZE			(2*512*1024)	/*512K*/

#define SYS_ERR_COND(cond, msg, ...) \
do { \
	if (cond) { \
		if (errno) \
			perror(msg); \
		else \
			fprintf(stderr, msg, ##__VA_ARGS__); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

#define ALG_ZLIB		0
#define ALG_GZIP		1

#define HW_CTX_SIZE		(512*1024)

#define Z_OK			0
#define Z_STREAM_END		1
#define Z_STREAM_NULL		3
#define Z_STREAM_NO_FINSH	4
#define Z_ERRNO			(-1)

#define STREAM_CHUNK_OUT	(512*1024)

#define MIN_STREAM_CHUNK	512

#define swab32(x) \
	((((x) & 0x000000ff) << 24) | \
	(((x) & 0x0000ff00) <<  8) | \
	(((x) & 0x00ff0000) >>  8) | \
	(((x) & 0xff000000) >> 24))

#define cpu_to_be32(x)		swab32(x)

#define HZLIB_VERSION		"1.2.11"
#define ZLIB_HEAD_SIZE		2
#define GZIP_HEAD_SIZE		10
#define GZIP_TAIL_SIZE		8

struct hisi_qm_priv {
	__u16 sqe_size;
	__u16 op_type;
};

static int hw_init(z_stream *zstrm, int alg_type, int comp_optype);
static void hw_end(z_stream *zstrm);
static int hw_send_and_recv(z_stream *zstrm, int flush);

const static char zlib_head[ZLIB_HEAD_SIZE] = {0x78, 0x9c};
const static char gzip_head[GZIP_HEAD_SIZE] = {0x1f, 0x8b, 0x08, 0x00, 0x00,
					       0x00, 0x00, 0x00, 0x00, 0x03};
static int stream_chunk = 1024*512;

int hisi_deflateInit2_(z_stream *zstrm, int level, int method, int windowBits,
		       int memLevel, int strategy, const char *version,
		       int stream_size)
{
	int alg_type;
	int wrap = 0;

	if (windowBits < 0) { /* suppress zlib wrapper */
		wrap = 0;
		windowBits = -windowBits;
	}
	else if (windowBits > 15) {
		wrap = 2;		/* write gzip wrapper instead */
		windowBits -= 16;
	}
	if (wrap & 0x02)
		alg_type = ALG_GZIP;
	else
		alg_type = ALG_ZLIB;

	zstrm->total_in = 0;
	zstrm->total_out = 0;

	return hw_init(zstrm, alg_type, HW_DEFLATE);

}

int hisi_deflateInit_(z_stream *zstrm, int level,
		      const char *version, int stream_size)
{
	return hisi_deflateInit2_(zstrm, level, Z_DEFLATED, MAX_WBITS,
				  DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY,
				  version, stream_size);
}

int hisi_reset_hw_ctl(struct hw_ctl *hw_ctl)
{
	hw_ctl->stream_end = 0;
	hw_ctl->stream_pos = STREAM_NEW;
	hw_ctl->empty_in = hw_ctl->empty_out = 1;
	hw_ctl->full_in = hw_ctl->pending_out = 0;
	hw_ctl->avail_in = hw_ctl->avail_out = stream_chunk;
	hw_ctl->inlen = hw_ctl->outlen = 0;
	hw_ctl->next_in = hw_ctl->in;
	hw_ctl->next_out = hw_ctl->out;
}

static inline void hisi_load_from_stream(z_stream *zstrm, int length)
{
	struct hw_ctl *hw_ctl = (struct hw_ctl *)zstrm->reserved;

	memcpy(hw_ctl->next_in, zstrm->next_in, length);
	hw_ctl->next_in  += length;
	hw_ctl->inlen    += length;
	hw_ctl->avail_in -= length;
	zstrm->next_in   += length;
	zstrm->total_in  += length;
}

int hisi_flowctl(z_stream *zstrm, int flush)
{
	struct hw_ctl *hw_ctl = (struct hw_ctl *)zstrm->reserved;
	int len, offset;

	if (hw_ctl->pending_out && hw_ctl->outlen && zstrm->avail_out) {
		/* need move data out of OUT buffer */
		if (hw_ctl->outlen > zstrm->avail_out) {
			len = zstrm->avail_out;
			hw_ctl->empty_out = 0;
			hw_ctl->pending_out = 1;
		} else {
			len = hw_ctl->outlen;
			hw_ctl->empty_out = 1;
			hw_ctl->pending_out = 0;
		}
		memcpy(zstrm->next_out, hw_ctl->next_out - hw_ctl->outlen, len);
		hw_ctl->outlen = hw_ctl->outlen - len;
		zstrm->avail_out = zstrm->avail_out - len;
		zstrm->next_out += len;
if (hw_ctl->op_type == HW_DEFLATE)
fprintf(stderr, "#%s, %d, zstrm->next_out updated to 0x%x\n", __func__, __LINE__, zstrm->next_out);
		/* zstream OUT buffer is full */
		if (hw_ctl->pending_out)
			return Z_OK;
		else if (hw_ctl->stream_end) {
			hisi_reset_hw_ctl(hw_ctl);
			return Z_STREAM_END;
		}
	} else if (hw_ctl->stream_end) {
		hisi_reset_hw_ctl(hw_ctl);
	}
#if 0
	if (!zstrm->avail_in && hw_ctl->inlen && (flush != Z_FINISH)) {
		/* it must be the last frame */
		flush = Z_FINISH;
	}
#endif
	offset = hw_ctl->next_in - hw_ctl->in;
	if (!hw_ctl->full_in && (zstrm->avail_in || offset)) {
		if ((zstrm->avail_in + offset) > stream_chunk) {
			if (hw_ctl->avail_in > stream_chunk) {
				fprintf(stderr, "too much data in IN buffer\n");
				return Z_ERRNO;
			}
			len = stream_chunk - offset;
			if (zstrm->avail_in && (zstrm->avail_in < len)) {
				hisi_load_from_stream(zstrm, zstrm->avail_in);
				zstrm->avail_in = 0;
			} else if (zstrm->avail_in > len) {
				hisi_load_from_stream(zstrm, len);
				zstrm->avail_in -= len;
			}
		} else {
			hisi_load_from_stream(zstrm, zstrm->avail_in);
			zstrm->avail_in = 0;
		}
		if (hw_ctl->inlen < MIN_STREAM_CHUNK) {
			hw_ctl->empty_in = 0;
			hw_ctl->full_in = 0;
		} else {
			hw_ctl->empty_in = 0;
			hw_ctl->full_in = 1;
		}
	}
	/* cache data in IN buffer and wait for next operation */
	if (!hw_ctl->full_in && (flush != Z_FINISH))
		return Z_OK;
	if (!hw_ctl->empty_in && (flush == Z_FINISH) && hw_ctl->avail_out)
		return hw_send_and_recv(zstrm, flush);
	else if (hw_ctl->full_in && hw_ctl->avail_out)
		return hw_send_and_recv(zstrm, flush);
	else if (hw_ctl->empty_in && hw_ctl->empty_out && (flush == Z_FINISH))
		return Z_STREAM_END;
	fprintf(stderr, "wrong case in hisi_flowctl()\n");
	return Z_ERRNO;
}

int hisi_deflate(z_stream *zstrm, int flush)
{
	return hisi_flowctl(zstrm, flush);
}

int hisi_deflateEnd(z_stream *zstrm)
{
	hw_end(zstrm);
	return 0;
}

int hisi_deflateParams(z_stream *zstrm, int level, int strategy)
{
	fprintf(stderr, "#%s, %d, zstrm->total_out:%d, zstrm->next_out:0x%x\n", __func__, __LINE__, zstrm->total_out, zstrm->next_out);
	return 0;
}

int hisi_inflateInit2_(z_stream *zstrm, int windowBits,
		       const char *version, int stream_size)
{
	int wrap, alg_type;

	/* extract wrap request from windowBits parameter */
	if (windowBits < 0) {
		wrap = 0;
		windowBits = -windowBits;
	} else {
		wrap = (windowBits >> 4) + 5;

	}
	if (wrap & 0x01)
		alg_type = ALG_ZLIB;
	if (wrap & 0x02)
		alg_type = ALG_GZIP;

	zstrm->total_in = 0;
	zstrm->total_out = 0;

	return hw_init(zstrm, alg_type, HW_INFLATE);
}

int hisi_inflateInit_(z_stream *zstrm, const char *version, int stream_size)
{
	return inflateInit2_(zstrm, DEF_WBITS, version, stream_size);
}

int hisi_inflate(z_stream *zstrm, int flush)
{
	return hisi_flowctl(zstrm, flush);
}

int hisi_inflateEnd(z_stream *zstrm)
{
	hw_end(zstrm);
	return 0;
}

static int hw_init(z_stream *zstrm, int alg_type, int comp_optype)
{
	int ret = -1;
	size_t ss_region_size;
	struct hisi_qm_priv *priv;
	struct hw_ctl *hw_ctl;

	hw_ctl = calloc(1, sizeof(struct hw_ctl));
	if (!hw_ctl) {
		fputs("alloc hw_ctl fail!\n", stderr);
		return -1;
	}
	hw_ctl->q = calloc(1, sizeof(struct wd_queue));
	if (!hw_ctl->q) {
		fputs("alloc hw_ctl->q fail!\n", stderr);
		goto hwctl_free;
	}

	switch (alg_type) {
	case 0:
		hw_ctl->alg_type = HW_ZLIB;
		hw_ctl->q->capa.alg = "zlib";
		break;
	case 1:
		hw_ctl->alg_type = HW_GZIP;
		hw_ctl->q->capa.alg = "gzip";
		break;
	default:
		hw_ctl->alg_type = HW_ZLIB;
		hw_ctl->q->capa.alg = "zlib";
	}
	hw_ctl->q->capa.latency = 0;   /*todo..*/
	hw_ctl->q->capa.throughput = 0;
	priv = (struct hisi_qm_priv *)hw_ctl->q->capa.priv;
	priv->sqe_size = sizeof(struct hisi_zip_sqe);
	priv->op_type = hw_ctl->op_type = comp_optype;
	ret = wd_request_queue(hw_ctl->q);
	if (ret) {
		fprintf(stderr, "wd request queue fail, no hwacc!\n");
		goto hwctl_q_free;
	}

	ss_region_size = 4096+ASIZE*2+HW_CTX_SIZE;
#ifdef CONFIG_IOMMU_SVA
		hw_ctl->ss_buf = malloc(ss_region_size);
#else
		hw_ctl->ss_buf = wd_reserve_memory(hw_ctl->q, ss_region_size);
#endif
	if (!hw_ctl->ss_buf) {
		fprintf(stderr, "fail to reserve %ld dmabuf\n", ss_region_size);
		ret = -ENOMEM;
		goto release_q;
	}

	ret = smm_init(hw_ctl->ss_buf, ss_region_size, 0xF);
	if (ret)
		goto buf_free;

	hw_ctl->in = smm_alloc(hw_ctl->ss_buf, ASIZE);
	hw_ctl->out = smm_alloc(hw_ctl->ss_buf, ASIZE);
	hw_ctl->ctx_buf = smm_alloc(hw_ctl->ss_buf, HW_CTX_SIZE);
	hisi_reset_hw_ctl(hw_ctl);

	if (!hw_ctl->next_in || !hw_ctl->next_out) {
		dbg("not enough data ss_region memory for cache 1 (bs=%d)\n",
			ASIZE);
			goto buf_free;
	}

	if (hw_ctl->q->dev_flags & UACCE_DEV_NOIOMMU) {
		hw_ctl->in_pa = wd_get_pa_from_va(hw_ctl->q, hw_ctl->in);
		hw_ctl->out_pa = wd_get_pa_from_va(hw_ctl->q, hw_ctl->out);
		hw_ctl->ctx_buf = wd_get_pa_from_va(hw_ctl->q, hw_ctl->ctx_buf);
	} else {
		hw_ctl->in_pa = hw_ctl->in;
		hw_ctl->out_pa = hw_ctl->out;
	}

	zstrm->reserved = hw_ctl;

	return Z_OK;
buf_free:
#ifdef CONFIG_IOMMU_SVA
	if (hw_ctl->ss_buf)
		free(hw_ctl->ss_buf);
#endif
release_q:
	wd_release_queue(hw_ctl->q);
hwctl_q_free:
	free(hw_ctl->q);
hwctl_free:
	free(hw_ctl);

	return ret;
}

static void hw_end(z_stream *zstrm)
{
	struct hw_ctl *hw_ctl = (struct hw_ctl *)zstrm->reserved;

#ifdef CONFIG_IOMMU_SVA
	if (hw_ctl->ss_buf)
		free(hw_ctl->ss_buf);
#endif

	wd_release_queue(hw_ctl->q);
	free(hw_ctl->q);
	free(hw_ctl);
}

static int hw_send_and_recv(z_stream *zstrm, int flush)
{
	struct hisi_zip_sqe msg, *recv_msg;
	struct hw_ctl *hw_ctl = (struct hw_ctl *)zstrm->reserved;
	int ret = 0, len, flush_type;
	__u32 status, type;
	__u64 pa;

	if (hw_ctl->stream_pos && (hw_ctl->op_type == HW_INFLATE))
		hw_ctl->inlen -= zstrm->headlen;

	flush_type = (flush == Z_FINISH) ? HZ_FINISH : HZ_SYNC_FLUSH;

	memset(&msg, 0, sizeof(msg));
	msg.dw9 = hw_ctl->alg_type;
	msg.dw7 = (hw_ctl->stream_pos) ? HZ_STREAM_NEW : HZ_STREAM_OLD;
	msg.dw7 |= flush_type | HZ_STATEFUL;
	pa = (__u64)hw_ctl->next_in - (__u64)hw_ctl->in - hw_ctl->inlen +
	     (__u64)hw_ctl->in_pa;
	msg.source_addr_l = pa & 0xffffffff;
	msg.source_addr_h = pa >> 32;
	pa = (__u64)hw_ctl->next_out - (__u64)hw_ctl->out +
	     (__u64)hw_ctl->out_pa;
	msg.dest_addr_l = pa & 0xffffffff;
	msg.dest_addr_h = pa >> 32;
	msg.input_data_length = hw_ctl->inlen;
	msg.dest_avail_out = hw_ctl->avail_out;
	//if (hw_ctl->op_type == HW_INFLATE) {
		msg.stream_ctx_addr_l = (__u64)hw_ctl->ctx_buf & 0xffffffff;
		msg.stream_ctx_addr_h = (__u64)hw_ctl->ctx_buf >> 32;
	//}
	if (!hw_ctl->stream_pos) {
		msg.checksum = zstrm->adler;
		if (hw_ctl->alg_type == HW_GZIP)
			msg.isize = zstrm->total_in;
	}
/*
	msg.ctx_dw0 = hw_ctl->ctx_dw0;
	msg.ctx_dw1 = hw_ctl->ctx_dw1;
	msg.ctx_dw2 = hw_ctl->ctx_dw2;
*/
 #if 1
 	{
 		int i, len;
 		fprintf(stderr, "IN[%d]:", hw_ctl->inlen);
		len = hw_ctl->inlen;
		if (len > 512)
			len = 512;
		for (i = 0; i < len; i++) {
			fprintf(stderr, "%x ", *((unsigned char *)hw_ctl->next_in - hw_ctl->inlen + i));
 		}
 		fprintf(stderr, "\n");
 	}
#endif

	ret = wd_send(hw_ctl->q, &msg);
	if (ret == -EBUSY) {
		usleep(1);
		goto recv_again;
	}

	SYS_ERR_COND(ret, "send fail!\n");
recv_again:
	ret = wd_recv(hw_ctl->q, (void **)&recv_msg);
	if (ret == -EIO) {
		fputs(" wd_recv fail!\n", stderr);
		goto out;
	/* synchronous mode, if get none, then get again */
	} else if (ret == -EAGAIN)
		goto recv_again;
fprintf(stderr, "###msg dw7:0x%x, recv_msg dw7:0x%x\n", msg.dw7, recv_msg->dw7);
	status = recv_msg->dw3 & 0xff;
	type = recv_msg->dw9 & 0xff;
#if 1
 	{
 		int i, len;
 		fprintf(stderr, "OUT[%d]:", recv_msg->produced + hw_ctl->outlen);
		len = recv_msg->produced + hw_ctl->outlen;
		if (len > 512)
			len = 512;
		for (i = 0; i < len; i++) {
 			fprintf(stderr, "%x ", *((unsigned char *)hw_ctl->out + i));
 		}
 		fprintf(stderr, "\n");
	}
#endif
fprintf(stderr, "#%s, %d, status:0x%x, crc:0x%x, end_of_last_blk:0x%x\n", __func__, __LINE__, status, recv_msg->checksum, recv_msg->dw3 & 0x100);
	SYS_ERR_COND(status != 0 && status != 0x0d && status != 0x13,
		     "bad status (s=%d, t=%d)\n", status, type);
	hw_ctl->stream_pos = STREAM_OLD;
	hw_ctl->avail_out -= recv_msg->produced;
	hw_ctl->inlen -= recv_msg->consumed;
	hw_ctl->next_out += recv_msg->produced;
	hw_ctl->outlen += recv_msg->produced;
/*
	hw_ctl->ctx_dw0 = recv_msg->ctx_dw0;
	hw_ctl->ctx_dw1 = recv_msg->ctx_dw1;
	hw_ctl->ctx_dw2 = recv_msg->ctx_dw2;
*/
	zstrm->next_in += recv_msg->consumed;
	if (hw_ctl->op_type == HW_DEFLATE) {
		fprintf(stderr, "#%s, %d, crc:0x%x\n", __func__, __LINE__, zstrm->adler);
		if (hw_ctl->alg_type == HW_ZLIB) {
			hw_ctl->outlen -= 4;
			hw_ctl->next_out -= 4;
			zstrm->adler = adler32(zstrm->adler, hw_ctl->in,
					recv_msg->consumed);
		} else if (hw_ctl->alg_type == HW_GZIP) {
			hw_ctl->outlen -= 8;
			hw_ctl->next_out -= 8;
			zstrm->adler = crc32(zstrm->adler, hw_ctl->in,
					recv_msg->consumed);
		}
		fprintf(stderr, "#%s, %d, crc:0x%x\n", __func__, __LINE__, zstrm->adler);
	}
	if (zstrm->avail_in == 0) {
		hw_ctl->next_in = hw_ctl->in;
		hw_ctl->avail_in = stream_chunk;
		hw_ctl->inlen = 0;
		hw_ctl->empty_in = 1;
		hw_ctl->full_in = 0;
	}
	if (hw_ctl->outlen > zstrm->avail_out) {
		len = zstrm->avail_out;
		hw_ctl->pending_out = 1;
		hw_ctl->empty_out = 0;
	} else {
		len = hw_ctl->outlen;
		hw_ctl->pending_out = 0;
		hw_ctl->empty_out = 1;
	}
	memcpy(zstrm->next_out, hw_ctl->next_out - hw_ctl->outlen, len);
	zstrm->next_out += len;
if (hw_ctl->op_type == HW_DEFLATE)
fprintf(stderr, "#%s, %d, zstrm->next_out updated to 0x%x\n", __func__, __LINE__, zstrm->next_out);
	zstrm->avail_out -= len;
	zstrm->total_out += len;
	hw_ctl->outlen -= len;
	if (hw_ctl->empty_out) {
		hw_ctl->next_out = hw_ctl->out;
		hw_ctl->avail_out = stream_chunk;
	}

	if (ret == 0 && flush == Z_FINISH)
		ret = Z_STREAM_END;
	else if (ret == 0 &&  (recv_msg->dw3 & 0x1ff) == 0x113)
		ret = Z_STREAM_END;    /* decomp_is_end  region */
	if (ret == Z_STREAM_END) {
fprintf(stderr, "#%s, %d Z_STREAM_END\n", __func__, __LINE__);
		if (hw_ctl->pending_out) {
			hw_ctl->stream_end = 1;
			ret = Z_OK;
		} else {
			hw_ctl->stream_pos = STREAM_NEW;
		}
	}
out:
	return ret;
}
