
#include "zbuild.h"
#include "wd_common.h"

void ZLIB_INTERNAL wd_reset_param(struct wd_state *wd_state)
{
    struct hisi_param *param = &wd_state->param;

    param->stream_end = 0;
    param->stream_pos = STREAM_NEW;
    param->empty_in = param->empty_out = 1;
    param->full_in = param->pending = 0;
    param->avail_in = param->avail_out = STREAM_CHUNK;
    param->stalled_size = param->pending_size = 0;
    param->next_in = param->in;
    param->next_out = param->out;
}

void ZLIB_INTERNAL *wd_alloc_state(PREFIX3(streamp) strm, uInt items, uInt size)
{
    return ZALLOC(strm, ALIGN_UP(items * size, 8) + sizeof(struct wd_state), sizeof(unsigned char));
}

void ZLIB_INTERNAL wd_copy_state(void *dst, const void *src, uInt size)
{
    memcpy(dst, src, ALIGN_UP(size, 8) + sizeof(struct wd_state));
}
