/* functable.h -- Struct containing function pointers to optimized functions
 * Copyright (C) 2017 Hans Kristian Rosbach
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

#ifndef FUNCTABLE_H_
#define FUNCTABLE_H_

#include "deflate.h"

uint32_t adler32_c(uint32_t adler, const unsigned char *buf, size_t len);

void functableInit();

struct functable_s {
    void     (* fill_window)    (deflate_state *s);
    Pos      (* insert_string)  (deflate_state *const s, const Pos str, unsigned int count);
    uint32_t (* adler32)        (uint32_t adler, const unsigned char *buf, size_t len);
};

extern struct functable_s functable;


#endif
