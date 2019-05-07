#ifndef __WD_ZLIB_H
#define __WD_ZLIB_H

#include "zutil.h"
#include "wd.h"
#include "zip_usr_if.h"
#include "smm.h"


struct hw_ctl {
	struct wd_queue *q;
	unsigned empty_in : 1;		/* IN buffer is empty */
	unsigned empty_out : 1;		/* OUT buffer is empty */
	unsigned full_in : 1;		/* IN buffer is ready */
	unsigned pending_out : 1;	/* pending data for OUT buffer */
	unsigned stream_pos : 1;	/* STREAM_NEW or STREAM_OLD */
	unsigned stream_end : 1;
	int alg_type;
	int op_type;
	int avail_in;			/* number of bytes availiable in IN buffer */
	int avail_out;
	int inlen;			/* data cached in IN buffer */
	int outlen;			/* data cached in OUT buffer */
	void *in;
	void *out;
	void *in_pa;
	void *out_pa;
	void *next_in;
	void *next_out;
	void *ctx_buf;
	int ctx_dw0;
	int ctx_dw1;
	int ctx_dw2;
	void *ss_buf;
};

extern int hisi_deflateInit2_(z_stream *zstrm, int level, int method,
		     int windowBits, int memLevel, int strategy,
		     const char *version, int stream_size);
extern int hisi_deflate(z_stream *zstrm, int flush);
extern int hisi_deflateEnd(z_stream *zstrm);
extern int hisi_deflateParams(z_stream *zstrm, int level, int strategy);


extern int hisi_inflateInit2_(z_stream *zstrm, int windowBits,
		     const char *version, int stream_size);
extern int hisi_inflate(z_stream *zstrm, int flush);
extern int hisi_inflateEnd(z_stream *zstrm);

#endif
