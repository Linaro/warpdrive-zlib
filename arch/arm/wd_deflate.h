#ifndef WD_DEFLATE_H
#define WD_DEFLATE_H

int ZLIB_INTERNAL wd_can_deflate(PREFIX3(streamp) strm);
int ZLIB_INTERNAL wd_deflate(PREFIX3(streamp) strm, int flush, block_state *result);
int ZLIB_INTERNAL wd_deflate_params(PREFIX3(streamp) strm,
                                    int level,
                                    int strategy
                                    );
void ZLIB_INTERNAL wd_deflate_reset(PREFIX3(streamp) strm, uInt size);

#define DEFLATE_HOOK wd_deflate

#define DEFLATE_RESET_KEEP_HOOK(strm) \
    wd_deflate_reset((strm), sizeof(deflate_state))

#define DEFLATE_END_HOOK wd_deflate_end

#define DEFLATE_PARAMS_HOOK(strm, level, strategy)      \
    do {                                                \
    	int err;                                        \
    	                                                \
    	err = wd_deflate_params(strm, level, strategy);	\
    	if (err == Z_STREAM_ERROR)			\
            return err;                                 \
    } while (0)

/* Memory management for the deflate state. Useful for allocating arch-specific extension blocks. */
#  define ZALLOC_STATE(strm, items, size) wd_alloc_state(strm, items, size)
#  define ZFREE_STATE(strm, addr) ZFREE(strm, addr)
#  define ZCOPY_STATE(dst, src, size) wd_copy_state(dst, src, size)
/* Memory management for the window. Useful for allocation the aligned window. */
#  define ZALLOC_WINDOW(strm, items, size) ZALLOC(strm, items, size)
#  define TRY_FREE_WINDOW(strm, addr) TRY_FREE(strm, addr)
/* Invoked at the beginning of deflateSetDictionary(). Useful for checking arch-specific window data. */
#  define DEFLATE_SET_DICTIONARY_HOOK(strm, dict, dict_len) do {} while (0)
/* Invoked at the beginning of deflateGetDictionary(). Useful for adjusting arch-specific window data. */
#  define DEFLATE_GET_DICTIONARY_HOOK(strm, dict, dict_len) do {} while (0)
/* Adjusts the upper bound on compressed data length based on compression parameters and uncompressed data length.
 * Useful when arch-specific deflation code behaves differently than regular zlib-ng algorithms. */
#  define DEFLATE_BOUND_ADJUST_COMPLEN(strm, complen, sourceLen) do {} while (0)
/* Returns whether an optimistic upper bound on compressed data length should *not* be used.
 * Useful when arch-specific deflation code behaves differently than regular zlib-ng algorithms. */
#  define DEFLATE_NEED_CONSERVATIVE_BOUND(strm) 0
/* Returns whether zlib-ng should compute a checksum. Set to 0 if arch-specific deflation code already does that. */
#  define DEFLATE_NEED_CHECKSUM(strm) 1
/* Returns whether reproducibility parameter can be set to a given value. */
#  define DEFLATE_CAN_SET_REPRODUCIBLE(strm, reproducible) 1
#endif /* WD_DEFLATE_H */
