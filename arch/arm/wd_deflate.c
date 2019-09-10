#include "zbuild.h"
#include "zlib.h"
#include "zutil.h"
#include "deflate.h"
#include "wd_deflate.h"
#include "wd_detail.h"

static inline void load_from_stream(PREFIX3(streamp) strm,
                                    struct hisi_param *param,
                                    int length)
{
    memcpy(param->next_in, strm->next_in, length);
    param->next_in  += length;
    param->stalled_size    += length;
    param->avail_in -= length;
    strm->next_in   += length;
    strm->total_in  += length;
    strm->avail_in  -= length;
}

static int hisi_deflate(PREFIX3(streamp) strm,
			      struct wd_state *wd_state,
                              int flush)
{
    struct hisi_zip_sqe msg, *recv_msg;
    struct hisi_param *param = &wd_state->param;
    int ret = 0, len, flush_type;
    __u32 status, type;
    __u64 pa;

    flush_type = (flush == Z_FINISH) ? HZ_FINISH : HZ_SYNC_FLUSH;

    memset(&msg, 0, sizeof(msg));
    msg.dw9 = param->alg_type;
    msg.dw7 = (param->stream_pos) ? HZ_STREAM_NEW : HZ_STREAM_OLD;
    msg.dw7 |= flush_type | HZ_STATEFUL;
    pa = (__u64)param->next_in - (__u64)param->in - param->stalled_size +
         (__u64)param->in_pa;
    msg.source_addr_l = pa & 0xffffffff;
    msg.source_addr_h = pa >> 32;
    pa = (__u64)param->next_out - (__u64)param->out +
         (__u64)param->out_pa;
    msg.dest_addr_l = pa & 0xffffffff;
    msg.dest_addr_h = pa >> 32;
    msg.input_data_length = param->stalled_size;
    msg.dest_avail_out = param->avail_out;
    msg.stream_ctx_addr_l = (__u64)param->ctx_pa & 0xffffffff;
    msg.stream_ctx_addr_h = (__u64)param->ctx_pa >> 32;
    if (!param->stream_pos) {
        msg.checksum = strm->adler;
        if (param->alg_type == HW_GZIP)
            msg.isize = strm->total_in;
    }
 #if 0
    {
        int i, len;
        fprintf(stderr, "IN[%d]:", hw_ctl->stalled_size);
        len = hw_ctl->stalled_size;
        if (len > 512)
        len = 512;
        for (i = 0; i < len; i++) {
            fprintf(stderr, "%x ", *((unsigned char *)hw_ctl->next_in - hw_ctl->stalled_size + i));
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
        fprintf(stderr, "OUT[%d]:", recv_msg->produced + param->pending_size);
        len = recv_msg->produced + param->pending_size;
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
    param->pending_size    += recv_msg->produced;
    param->stalled_size    -= recv_msg->consumed;

    /* calculate CRC by software */
    if (param->alg_type == HW_ZLIB) {
        param->pending_size -= 4;
        param->next_out -= 4;
#ifdef ZLIB_COMPAT
        strm->adler = adler32(strm->adler, param->in, recv_msg->consumed);
#else
        strm->adler = zng_adler32(strm->adler, param->in, recv_msg->consumed);
#endif
    } else if (param->alg_type == HW_GZIP) {
        param->pending_size -= 8;
        param->next_out -= 8;
#ifdef ZLIB_COMPAT
        strm->adler = crc32(strm->adler, param->in, recv_msg->consumed);
#else
        strm->adler = zng_crc32(strm->adler, param->in, recv_msg->consumed);
#endif
    }
    if (strm->avail_in == 0) {
        param->next_in = param->in;
        param->avail_in = STREAM_CHUNK;
        param->stalled_size = 0;
        param->empty_in = 1;
        param->full_in = 0;
    }
    if (param->pending_size > strm->avail_out) {
        len = strm->avail_out;
        param->pending = 1;
        param->empty_out = 0;
	strm->avail_out = 0;
    } else {
        len = param->pending_size;
        param->pending = 0;
        param->empty_out = 1;
	strm->avail_out -= len;
    }
    memcpy(strm->next_out, param->next_out - param->pending_size, len);
    param->avail_out -= len;
    strm->next_out += len;
    strm->total_out += len;
    param->pending_size -= len;
    if (param->empty_out) {
        param->next_out = param->out;
        param->avail_out = STREAM_CHUNK;
    }

    if (ret == 0 && flush == Z_FINISH)
        ret = Z_STREAM_END;
    else if (ret == 0 &&  (recv_msg->dw3 & 0x1ff) == 0x113)
        ret = Z_STREAM_END;    /* decomp_is_end  region */
    if (ret == Z_STREAM_END) {
        if (param->pending) {
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
    if ((level <= 3) || (strategy != Z_DEFAULT_STRATEGY))
	return 0;
    return 1;
}


int ZLIB_INTERNAL wd_can_deflate(PREFIX3(streamp) strm)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);

    struct hisi_param *param = &wd_state->param;

    if (param->hw_avail && param->hw_enabled) {
        if (wd_are_params_ok(state->level, state->strategy))
            return 1;
    }
    return 0;
}

int ZLIB_INTERNAL wd_deflate_enable(PREFIX3(streamp) strm)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    if (param->hw_avail) {
        param->hw_enabled = 1;
        return 0;
    }
    return -EINVAL;
}

int ZLIB_INTERNAL wd_deflate_disable(PREFIX3(streamp) strm)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    param->hw_enabled = 0;
    fprintf(stderr, "Deflate by software now.\n");
    return 0;
}

int ZLIB_INTERNAL wd_deflate_need_flush(PREFIX3(streamp) strm)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    if (param->pending && param->pending_size && strm->avail_out)
        return 1;
    return 0;
}

void ZLIB_INTERNAL wd_deflate_flush_pending(PREFIX3(streamp) strm,
                                            block_state *result)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;
    int flush_size;

    if (param->pending_size > strm->avail_out) {
        /* There's no enough room to flush. Only flush partial data. */
        flush_size = strm->avail_out;
        param->empty_out = 0;
        param->pending = 1;
    } else {
        /* Flush all. */
        flush_size = param->pending_size;
        param->empty_out = 1;
        param->pending = 0;
    }
    memcpy(strm->next_out, param->next_out - param->pending_size, flush_size);
    param->pending_size = param->pending_size - flush_size;
    strm->avail_out -= flush_size;
    strm->next_out += flush_size;
    if (param->pending) {
        /* Only part data is flushed. */
        *result = need_more;
    } else if (param->stream_end) {
        /* All of dta is flush. */
        wd_reset_param(wd_state);
        *result = finish_done;
    }
}

int ZLIB_INTERNAL wd_deflate_params(PREFIX3(streamp) strm,
                                    int level,
                                    int strategy)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;
    block_state result;

    if (wd_can_deflate(strm) && (level <= 3)) {
        if (wd_deflate_need_flush(strm))
            wd_deflate_flush_pending(strm, &result);
        wd_deflate_disable(strm);
    }
    if (wd_can_deflate(strm) && !wd_are_params_ok(level, strategy)) {
        wd_deflate_disable(strm);
    }
    return Z_OK;
}

int ZLIB_INTERNAL wd_deflate(PREFIX3(streamp) strm,
                             int flush,
                             block_state *result)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;
    int len;
    int ret;

    if (state == Z_NO_COMPRESSION) {
        return 0;
    }

    if (!param->hw_avail || !wd_can_deflate(strm))
        return 0;

    if (wd_deflate_need_flush(strm)) {
        wd_deflate_flush_pending(strm, result);
        return 1;
    }
    param->stalled_size = param->next_in - param->in;
    if (!param->full_in && (strm->avail_in || param->stalled_size)) {
#if 0
        if ((strm->avail_in + stalled_size) > STREAM_CHUNK) {
            len = STREAM_CHUNK - stalled_size;
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
	if (param->stalled_size < MIN_STREAM_CHUNK) {
            param->empty_in = 0;
	    param->full_in = 0;
        } else {
            param->empty_in = 0;
	    param->full_in = 1;
        }
#else
        if (strm->avail_in)
            load_from_stream(strm, param, strm->avail_in);
        if (param->stalled_size) {
            param->empty_in = 0;
            param->full_in = 0;
        }
#endif
    }
    if (!param->full_in && (flush != Z_FINISH)) {
        *result = need_more;
        return 1;
    }
    if (!param->empty_in && (flush == Z_FINISH) && param->avail_out) {
        ret = hisi_deflate(strm, wd_state, flush);
	if (ret == Z_STREAM_END)
            *result = finish_done;
        else if (ret == Z_OK)
            *result = need_more;
        return 1;
    } else if (param->full_in && param->avail_out) {
        ret = hisi_deflate(strm, wd_state, flush);
        if (ret == Z_STREAM_END)
            *result = finish_done;
        else if (ret == Z_OK)
            *result = need_more;
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

    if (state->level <= 3) {
        fprintf(stderr, "Can't support fast level. Use software instead.\n");
	wd_deflate_disable(strm);
	param->hw_avail = 0;
	return;
    }
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
    wd_deflate_enable(strm);

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
    wd_reset_param(wd_state);

    if (wd_state->q.dev_flags & UACCE_DEV_NOIOMMU) {
        param->in_pa   = wd_get_pa_from_va(&wd_state->q, param->in);
	param->out_pa  = wd_get_pa_from_va(&wd_state->q, param->out);
	param->ctx_pa  = wd_get_pa_from_va(&wd_state->q, param->ctx_buf);
    } else {
	param->in_pa   = param->in;
        param->out_pa  = param->out;
        param->ctx_pa  = param->ctx_buf;
    }
    return;

out_queue:
    wd_release_queue(&wd_state->q);
}

void ZLIB_INTERNAL wd_deflate_end(PREFIX3(streamp) strm)
{
    deflate_state *state = (deflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    if (param->hw_avail)
        wd_release_queue(&wd_state->q);
    smm_free(param->ss_buf, param->in);
    smm_free(param->ss_buf, param->out);
    smm_free(param->ss_buf, param->ctx_buf);
}
