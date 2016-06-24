/*
 * Copyright (c) 2015-2016 Cray Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef CT_UTILS_H
#define CT_UTILS_H

#ifndef CT_FIVERSION
#define CT_FIVERSION FI_VERSION(1,3)
#endif

static inline void ct_print_fi_error(const char *fi_fname, int ret_val)
{
	fprintf(stderr, "%s() ret=%d (%s)\n", fi_fname, ret_val,
		fi_strerror(ret_val));
	fflush(stdout);
}

static inline void ct_print_opts_usage(const char *opt, const char *desc)
{
	fprintf(stderr, " %-20s %s\n", opt, desc);
}

void ct_parseinfo(int op, char *optarg, struct fi_info *hints);

/* general utilities */
#include <sys/time.h>
static inline uint64_t get_time_usec(void)
{
	struct timeval tv;
	uint64_t usecs;

	gettimeofday(&tv, NULL);
	usecs = (tv.tv_sec * 1000000) + tv.tv_usec;
	return usecs;
}

/* out-of-band process management stuff */

void ctpm_Init(int *, char ***);
void ctpm_Abort(void);
void ctpm_Exit(void);
void ctpm_Rank(int *);
void ctpm_Finalize(void);
void ctpm_Barrier(void);
void ctpm_Job_size(int *);
void ctpm_Allgather(void *src, size_t, void *dest);
void ctpm_Bcast(void *, size_t);

#endif /* CT_UTILS_H */
