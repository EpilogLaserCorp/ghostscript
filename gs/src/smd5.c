/* Copyright (C) 2000 Aladdin Enterprises.  All rights reserved.
  
  This software is licensed to a single customer by Artifex Software Inc.
  under the terms of a specific OEM agreement.
*/

/*$RCSfile$ $Revision$ */
/* MD5Encode filter */
#include "memory_.h"
#include "strimpl.h"
#include "smd5.h"

/* ------ MD5Encode ------ */

private_st_MD5E_state();

/* Initialize the state. */
private int
s_MD5E_init(stream_state * st)
{
    stream_MD5E_state *const ss = (stream_MD5E_state *) st;

    md5_init(&ss->md5);
    return 0;
}

/* Process a buffer. */
private int
s_MD5E_process(stream_state * st, stream_cursor_read * pr,
	       stream_cursor_write * pw, bool last)
{
    stream_MD5E_state *const ss = (stream_MD5E_state *) st;
    int status = 0;

    if (pr->ptr < pr->limit) {
	md5_append(&ss->md5, pr->ptr + 1, pr->limit - pr->ptr);
	pr->ptr = pr->limit;
    }
    if (last) {
	if (pw->limit - pw->ptr >= 16) {
	    md5_finish(&ss->md5, pw->ptr + 1);
	    pw->ptr += 16;
	    status = EOFC;
	} else
	    status = 1;
    }
    return status;
}

/* Stream template */
const stream_template s_MD5E_template = {
    &st_MD5E_state, s_MD5E_init, s_MD5E_process, 1, 16
};
