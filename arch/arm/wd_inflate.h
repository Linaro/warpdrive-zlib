#ifndef WD_INFLATE_H
#define WD_INFLATE_H

#include "inflate.h"
#include "wd_common.h"

typedef enum {
    WD_INFLATE_CONTINUE,
    WD_INFLATE_BREAK,
    WD_INFLATE_SOFTWARE,
} wd_inflate_action;

int ZLIB_INTERNAL wd_can_inflate(PREFIX3(streamp) strm);
int ZLIB_INTERNAL wd_inflate_disable(PREFIX3(streamp) strm);
wd_inflate_action ZLIB_INTERNAL wd_inflate(PREFIX3(streamp) strm, int flush, int *ret);
void ZLIB_INTERNAL wd_inflate_reset(PREFIX3(streamp) strm, uInt size);

#define INFLATE_RESET_KEEP_HOOK(strm) \
    wd_inflate_reset((strm), sizeof(struct inflate_state))

#define INFLATE_PRIME_HOOK(strm, bits, value) \
    do { if (wd_inflate_disable((strm))) return Z_STREAM_ERROR; } while (0)

#define INFLATE_TYPEDO_HOOK(strm, flush) \
    if (wd_can_inflate((strm))) { \
        wd_inflate_action action; \
\
        RESTORE(); \
        action = wd_inflate((strm), (flush), &ret); \
        LOAD(); \
        if (action == WD_INFLATE_CONTINUE) \
            break; \
        else if (action == WD_INFLATE_BREAK) \
            goto inf_leave; \
    }

/* Memory management for the inflate state. Useful for allocating arch-specific extension blocks. */
#  define ZALLOC_STATE(strm, items, size) wd_alloc_state(strm, items, size)
#  define ZFREE_STATE(strm, addr) ZFREE(strm, addr)
#  define ZCOPY_STATE(dst, src, size) wd_copy_state(dst, src, size)
/* Memory management for the window. Useful for allocation the aligned window. */
#  define ZALLOC_WINDOW(strm, items, size) ZALLOC(strm, items, size)
#  define ZFREE_WINDOW(strm, addr) ZFREE(strm, addr)
/* Returns whether zlib-ng should compute a checksum. Set to 0 if arch-specific inflation code already does that. */
#  define INFLATE_NEED_CHECKSUM(strm) 1
/* Returns whether zlib-ng should update a window. Set to 0 if arch-specific inflation code already does that. */
#  define INFLATE_NEED_UPDATEWINDOW(strm) 1
/* Invoked at the beginning of inflateMark(). Useful for updating arch-specific pointers and offsets. */
#  define INFLATE_MARK_HOOK(strm) do {} while (0)

#endif /* WD_INFLATE_H */
