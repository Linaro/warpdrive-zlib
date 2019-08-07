#include "zbuild.h"
#include "zlib.h"
#include "zutil.h"
#include "deflate.h"
#include "wd_deflate.h"
#include "wd_detail.h"

static void reset_wd_param(struct wd_state *wd_state)
{
    struct hisi_param *param = &wd_state->param;

    param->stream_end = 0;
    param->stream_pos = STREAM_NEW;
    param->empty_in = param->empty_out = 1;
    param->full_in = param->pending_out = 0;
    param->avail_in = param->avail_out = STREAM_CHUNK;
    param->inlen = param->outlen = 0;
    param->next_in = param->in;
    param->next_out = param->out;
}

static inline void load_from_stream(PREFIX3(streamp) strm,
                                    struct hisi_param *param,
                                    int length)
{
    memcpy(param->next_in, strm->next_in, length);
    param->next_in  += length;
    param->inlen    += length;
    param->avail_in -= length;
    strm->next_in   += length;
    strm->total_in  += length;
}

static int hisi_send_and_recv(PREFIX3(streamp) strm,
			      struct wd_state *wd_state,
                              int flush)
{
    struct hisi_zip_sqe msg, *recv_msg;
    struct hisi_param *param = &wd_state->param;
    int ret = 0, len, flush_type;
    __u32 status, type;
    __u64 pa;

#if 0
    if (param->stream_pos && (param->op_type == HW_INFLATE))
        param->inlen -= strm->headlen;
#endif

    flush_type = (flush == Z_FINISH) ? HZ_FINISH : HZ_SYNC_FLUSH;

    memset(&msg, 0, sizeof(msg));
    msg.dw9 = param->alg_type;
    msg.dw7 = (param->stream_pos) ? HZ_STREAM_NEW : HZ_STREAM_OLD;
    msg.dw7 |= flush_type | HZ_STATEFUL;
    pa = (__u64)param->next_in - (__u64)param->in - param->inlen +
         (__u64)param->in_pa;
    msg.source_addr_l = pa & 0xffffffff;
    msg.source_addr_h = pa >> 32;
    pa = (__u64)param->next_out - (__u64)param->out +
         (__u64)param->out_pa;
    msg.dest_addr_l = pa & 0xffffffff;
    msg.dest_addr_h = pa >> 32;
    msg.input_data_length = param->inlen;
    msg.dest_avail_out = param->avail_out;
    msg.stream_ctx_addr_l = (__u64)param->ctx_buf & 0xffffffff;
    msg.stream_ctx_addr_h = (__u64)param->ctx_buf >> 32;
    if (!param->stream_pos) {
        msg.checksum = strm->adler;
        if (param->alg_type == HW_GZIP)
            msg.isize = strm->total_in;
    }
 #if 0
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

    ret = wd_send(&wd_state->q, &msg);
    if (ret == -EBUSY) {
        //usleep(1);
        goto recv_again;
    }

    if (ret)
        fprintf(stderr, "send fail!\n");
recv_again:
    ret = wd_recv(&wd_state->q, (void **)&recv_msg);
    if (ret == -EIO) {
        fputs(" wd_recv fail!\n", stderr);
        goto out;
    /* synchronous mode, if get none, then get again */
    } else if (ret == -EAGAIN)
        goto recv_again;
    status = recv_msg->dw3 & 0xff;
    type = recv_msg->dw9 & 0xff;
#if 1
    {
        int i, len;
        fprintf(stderr, "OUT[%d]:", recv_msg->produced + param->outlen);
        len = recv_msg->produced + param->outlen;
        if (len > 512)
            len = 512;
        for (i = 0; i < len; i++) {
            fprintf(stderr, "%x ", *((unsigned char *)param->out + i));
        }
        fprintf(stderr, "\n");
    }
#endif
    if (status != 0 && status != 0x0d && status != 0x13)
        fprintf(stderr, "bad status (s=%d, t=%d)\n", status, type);
    param->stream_pos = STREAM_OLD;
    param->avail_out -= recv_msg->produced;
    param->next_out  += recv_msg->produced;
    param->outlen    += recv_msg->produced;
    param->inlen    -= recv_msg->consumed;

    /* calculate CRC by software */
    if (param->alg_type == HW_ZLIB) {
        param->outlen -= 4;
        param->next_out -= 4;
        strm->adler = adler32(strm->adler, param->in, recv_msg->consumed);
    } else if (param->alg_type == HW_GZIP) {
        param->outlen -= 8;
        param->next_out -= 8;
        strm->adler = crc32(strm->adler, param->in, recv_msg->consumed);
    }
    if (strm->avail_in == 0) {
        param->next_in = param->in;
        param->avail_in = STREAM_CHUNK;
        param->inlen = 0;
        param->empty_in = 1;
        param->full_in = 0;
    }
    if (param->outlen > strm->avail_out) {
        len = strm->avail_out;
        param->pending_out = 1;
        param->empty_out = 0;
	strm->avail_out = 0;
    } else {
        len = param->outlen;
        param->pending_out = 0;
        param->empty_out = 1;
	strm->avail_out -= len;
    }
    memcpy(strm->next_out, param->next_out - param->outlen, len);
    param->avail_out -= len;
    strm->next_out += len;
    strm->total_out += len;
    param->outlen -= len;
    if (param->empty_out) {
        param->next_out = param->out;
        param->avail_out = STREAM_CHUNK;
    }

    if (ret == 0 && flush == Z_FINISH)
        ret = Z_STREAM_END;
    else if (ret == 0 &&  (recv_msg->dw3 & 0x1ff) == 0x113)
        ret = Z_STREAM_END;    /* decomp_is_end  region */
    if (ret == Z_STREAM_END) {
        if (param->pending_out) {
            param->stream_end = 1;
            ret = Z_OK;
        } else {
            param->stream_pos = STREAM_NEW;
        }
    }
out:
    return ret;
}

static inline int wd_are_params_ok(int level,
				   int strategy)
{
    return level && (strategy == Z_FIXED || strategy == Z_DEFAULT_STRATEGY);
}


int ZLIB_INTERNAL wd_deflate_params(PREFIX3(streamp) strm,
                                    int level,
                                    int strategy)
{
    int can_deflate = wd_are_params_ok(level, strategy);

    if (can_deflate)
	return Z_OK;

    return Z_STREAM_ERROR;
}

int ZLIB_INTERNAL wd_can_deflate(PREFIX3(streamp) strm)
{
    deflate_state *state = (deflate_state *)strm->state;

    /* Unsupported compression settings */
    if (!wd_are_params_ok(state->level, state->strategy))
        return 0;
    return 1;
}

int ZLIB_INTERNAL wd_deflate(PREFIX3(streamp) strm, int flush, block_state *result)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;
    int len;
    int offset;
    int ret;

    if (!param->hw_avail || !wd_can_deflate(strm))
        return 0;

    if (param->pending_out && param->outlen && strm->avail_out) {
        if (param->outlen > strm->avail_out) {
            len = strm->avail_out;
	    param->empty_out = 0;
	    param->pending_out = 1;
	} else {
            len = param->outlen;
	    param->empty_out = 1;
	    param->pending_out = 0;
	}
	memcpy(strm->next_out, param->next_out - param->outlen, len);
	param->outlen = param->outlen - len;
	strm->avail_out -= len;
	strm->next_out += len;
	/* stream OUT buffer is full */
	if (param->pending_out) {
            *result = need_more;
            return 1;
	} else if (param->stream_end) {
            reset_wd_param(wd_state);
	    *result = finish_done;
	    return 1;
	}
    } else if (param->stream_end) {
        reset_wd_param(wd_state);
    }
    offset = param->next_in - param->in;
    if (!param->full_in && (strm->avail_in || offset)) {
        if ((strm->avail_in + offset) > STREAM_CHUNK) {
            if (param->avail_in > STREAM_CHUNK) {
                fprintf(stderr, "Too much data in IN buffer.\n");
		return 1;
            }
            len = STREAM_CHUNK - offset;
            if (strm->avail_in && strm->avail_in < len) {
                load_from_stream(strm, param, strm->avail_in);
                strm->avail_in = 0;
            } else if (strm->avail_in > len) {
                load_from_stream(strm, param, len);
                strm->avail_in -= len;
            }
        } else {
            load_from_stream(strm, param, strm->avail_in);
            strm->avail_in = 0;
        }
	if (param->inlen < MIN_STREAM_CHUNK) {
            param->empty_in = 0;
	    param->full_in = 0;
        } else {
            param->empty_in = 0;
	    param->full_in = 1;
        }
    }
    if (!param->full_in && (flush != Z_FINISH)) {
        *result = need_more;
        return 1;
    }
    if (!param->empty_in && (flush == Z_FINISH) && param->avail_out) {
        ret = hisi_send_and_recv(strm, wd_state, flush);
	if (ret == Z_STREAM_END)
            *result = finish_done;
        else if (ret == Z_OK)
            *result = need_more;
        return 1;
    } else if (param->full_in && param->avail_out) {
        *result = block_done;
        hisi_send_and_recv(strm, wd_state, flush);
        return 1;
    } else if (param->empty_in && param->empty_out && (flush == Z_FINISH)) {
        *result = finish_done;
        return 1;
    }
    /* return 0 for unsupportd case. return 1 for hardware handling case. */
    return 1;
}

void ZLIB_INTERNAL wd_deflate_reset(PREFIX3(streamp) strm, uInt size)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;
    struct hisi_qm_priv *priv;
    struct wd_capa *capa = &wd_state->q.capa;
    int ret;
    size_t ss_region_size;

    if (state->wrap & 2) {
        param->alg_type = HW_GZIP;
	capa->alg = "gzip";
    } else {
        param->alg_type = HW_ZLIB;
	capa->alg = "zlib";
    }
    capa->latency = 0;   /*todo..*/
    capa->throughput = 0;
    priv = (struct hisi_qm_priv *)(char *)&capa->priv;
    priv->sqe_size = sizeof(struct hisi_zip_sqe);
    param->op_type = HW_DEFLATE;
    priv->op_type = HW_DEFLATE;
    ret = wd_request_queue(&wd_state->q);
    if (ret) {
        fprintf(stderr, "Fail to request wd queue without hwacc!\n");
        param->hw_avail = 0;
	return;
    }
    param->hw_avail = 1;

    ss_region_size = 4096 + ASIZE * 2 + HW_CTX_SIZE;
    param->ss_buf = wd_reserve_memory(&wd_state->q, ss_region_size);
    if (!param->ss_buf) {
        fprintf(stderr, "Fail to reserve %ld DMA buffer\n");
	goto out_queue;
    }

    ret = smm_init(param->ss_buf, ss_region_size, 0xf);
    if (ret)
        goto out_queue;

    param->in = smm_alloc(param->ss_buf, ASIZE);
    param->out = smm_alloc(param->ss_buf, ASIZE);
    param->ctx_buf = smm_alloc(param->ss_buf, HW_CTX_SIZE);
    reset_wd_param(wd_state);

    if (wd_state->q.dev_flags & UACCE_DEV_NOIOMMU) {
        param->in_pa   = wd_get_pa_from_va(&wd_state->q, param->in);
	param->out_pa  = wd_get_pa_from_va(&wd_state->q, param->out);
	param->ctx_buf = wd_get_pa_from_va(&wd_state->q, param->ctx_buf);
    } else {
	param->in_pa   = param->in;
        param->out_pa  = param->out;
    }
    return;

out_queue:
    wd_release_queue(&wd_state->q);
}

void ZLIB_INTERNAL wd_deflate_end(PREFIX3(streamp) strm)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);

    wd_release_queue(&wd_state->q);
}
