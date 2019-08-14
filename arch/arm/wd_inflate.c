#include "zbuild.h"
#include "zlib.h"
#include "zutil.h"
#include "inftrees.h"
#include "inflate.h"
#include "wd_detail.h"
#include "wd_inflate.h"

static inline void load_from_stream(PREFIX3(streamp) strm,
                                    struct hisi_param *param,
                                    int length)
{
    memcpy(param->next_in, strm->next_in, length);
    param->next_in  += length;
    param->stalled_size    += length;
    param->avail_in -= length;
    param->saved_avail_in = length;
    strm->next_in   += length;
    strm->total_in  += length;
    strm->avail_in  -= length;
}

static inline void restore_to_stream(PREFIX3(streamp) strm,
                                     struct hisi_param *param)
{
    strm->next_in   -= param->saved_avail_in;
    strm->total_in  -= param->saved_avail_in;
    strm->avail_in  += param->saved_avail_in;
    param->saved_avail_in = 0;
}

static int hisi_inflate(PREFIX3(streamp) strm,
			      struct wd_state *wd_state,
                              int flush,
                              wd_inflate_action *action)
{
    struct hisi_zip_sqe msg, *recv_msg;
    struct hisi_param *param = &wd_state->param;
    int ret = 0, len, flush_type;
    __u32 status, type;
    __u64 pa;

    memset(&msg, 0, sizeof(msg));
    msg.dw9 = param->alg_type;  // PBUFFER
    msg.dw7 = (param->stream_pos) ? HZ_STREAM_NEW : HZ_STREAM_OLD;
    msg.dw7 |= HZ_STATEFUL;
    msg.dw3 = END_OF_LAST_BLK | SQE_STATUS_MASK;
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
    msg.stream_ctx_addr_l = (__u64)param->ctx_buf & 0xffffffff;
    msg.stream_ctx_addr_h = (__u64)param->ctx_buf >> 32;

 #if 0
    {
        int i, len, offset;
        fprintf(stderr, "+IN[%d]:", param->stalled_size);
        len = param->stalled_size;
        if (len > 256)
            len = 256;
        for (i = 0; i < len; i++) {
            fprintf(stderr, "%x ", *((unsigned char *)param->next_in - param->stalled_size + i));
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "-IN[%d]:", param->stalled_size);
        len = param->stalled_size;
        if (len > 256)
            len = 256;
	offset = param->stalled_size + 2 - 256;
	if (i < param->next_in - param->in) {
            for (i = offset; i < offset + len; i++) {
                fprintf(stderr, "%x ", *((unsigned char *)param->next_in - param->stalled_size + i));
            }
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
        *action = WD_INFLATE_BREAK;
        goto out;
    /* synchronous mode, if get none, then get again */
    } else if (ret == -EAGAIN)
        goto recv_again;
    status = recv_msg->dw3 & 0xff;
    type = recv_msg->dw9 & 0xff;
#if 0
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
    if (status != 0 && status != 0x0d && status != 0x13) {
        fprintf(stderr, "Bad status (s=%d, t=%d). Inflate by software now.\n",
                status, type);
        *action = WD_INFLATE_SOFTWARE;
	restore_to_stream(strm, param);
        wd_inflate_disable(strm);
        ret = -EINVAL;
        goto out;
    }

    *action                 = WD_INFLATE_END;
    param->stream_pos       = STREAM_OLD;
    param->saved_avail_in   = 0;
    param->avail_out       -= recv_msg->produced;
    param->next_out        += recv_msg->produced;
    param->pending_size    += recv_msg->produced;
    param->stalled_size    -= recv_msg->consumed;

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
    else if (!ret && (recv_msg->dw3 & END_OF_LAST_BLK)) {
        ret = Z_STREAM_END;
    }
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

int ZLIB_INTERNAL wd_can_inflate(PREFIX3(streamp) strm)
{
    struct inflate_state *state = (struct inflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    if (param->hw_avail && param->hw_enabled)
        return 1;
    return 0;
}

int ZLIB_INTERNAL wd_inflate_enable(PREFIX3(streamp) strm)
{
    struct inflate_state *state = (struct inflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    if (param->hw_avail) {
        param->hw_enabled = 1;
        return 0;
    }
    return -EINVAL;
}

int ZLIB_INTERNAL wd_inflate_disable(PREFIX3(streamp) strm)
{
    struct inflate_state *state = (struct inflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    param->hw_enabled = 0;
    return 0;
}

int ZLIB_INTERNAL wd_inflate_need_flush(PREFIX3(streamp) strm)
{
    struct inflate_state *state = (struct inflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;

    if (param->pending && param->pending_size && strm->avail_out)
        return 1;
    return 0;
}

wd_inflate_action ZLIB_INTERNAL wd_inflate_flush_pending(PREFIX3(streamp) strm,
                                                         int *ret)
{
    struct inflate_state *state = (struct inflate_state *)strm->state;
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
    strm->total_out += flush_size;
    if (!param->pending && param->stream_end) {
        /* All of dta is flush. */
        wd_reset_param(wd_state);
	*ret = Z_STREAM_END;
    }
    return WD_INFLATE_END;
}

wd_inflate_action ZLIB_INTERNAL wd_inflate(PREFIX3(streamp) strm,
                                           int flush,
					   int *ret)
{
    struct inflate_state *state = (struct inflate_state *)strm->state;
    struct wd_state *wd_state = GET_WD_STATE(state);
    struct hisi_param *param = &wd_state->param;
    wd_inflate_action action;
    int len;
    int avail_in;

    if (flush == Z_BLOCK || flush == Z_TREES) {
        /* HW accelerator does not support stopping on block boundaries */
        if (wd_inflate_disable(strm)) {
            *ret = Z_STREAM_ERROR;
            return WD_INFLATE_BREAK;
        } else {
            *ret = Z_OK;
            return WD_INFLATE_SOFTWARE;
	}
    }

    if (wd_inflate_need_flush(strm))
        return wd_inflate_flush_pending(strm, ret);

    param->stalled_size = param->next_in - param->in;
    if (!param->full_in && (strm->avail_in || param->stalled_size)) {
        if (strm->avail_in)
            load_from_stream(strm, param, strm->avail_in);
        if (param->stalled_size) {
            param->empty_in = 0;
            param->full_in = 0;
        }
    }
    if (!param->full_in && !strm->avail_in) {
       /* Fail to get more data, trigger to inflate */
       *ret = hisi_inflate(strm, wd_state, flush, &action);
       if ((*ret < 0) && (action == WD_INFLATE_SOFTWARE)) {
           *ret = Z_OK;
       } else if (*ret == Z_OK) {
          if (!param->pending)
              *ret = Z_STREAM_END;
          action = WD_INFLATE_END;
       }
       return action;
    }
    if (!param->empty_in && (flush == Z_FINISH) && param->avail_out) {
        *ret = hisi_inflate(strm, wd_state, flush, &action);
	return action;
    } else if (param->full_in && param->avail_out) {
        *ret = hisi_inflate(strm, wd_state, flush, &action);
	return action;
    } else if (param->empty_in && param->empty_out && (flush == Z_FINISH)) {
        *ret = Z_STREAM_END;
        return WD_INFLATE_END;
    }
    *ret = Z_DATA_ERROR;
    return WD_INFLATE_END;
}

void ZLIB_INTERNAL wd_inflate_reset(PREFIX3(streamp) strm, uInt size)
{
    struct inflate_state *state = (struct inflate_state *)strm->state;
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
    param->op_type = HW_INFLATE;
    priv->op_type = HW_INFLATE;
    ret = wd_request_queue(&wd_state->q);
    if (ret) {
        fprintf(stderr, "Fail to request wd queue without hwacc!\n");
        param->hw_avail = 0;
	return;
    }
    param->hw_avail = 1;
    param->hw_enabled = 1;

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
	param->ctx_buf = wd_get_pa_from_va(&wd_state->q, param->ctx_buf);
    } else {
	param->in_pa   = param->in;
        param->out_pa  = param->out;
    }
    return;

out_queue:
    param->hw_enabled = 0;
    wd_release_queue(&wd_state->q);
}
