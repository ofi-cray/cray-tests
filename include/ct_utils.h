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
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef CT_UTILS_H
#define CT_UTILS_H

static clock_t start, end;
static pthread_mutex_t clock_lock;
static char clock_started, is_clock_lock_init;

#ifndef CT_FIVERSION
#define CT_FIVERSION FI_VERSION(1,3)
#endif

/* Branch predictor hints */
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

/*  Expand macro argument "something" to a string literal. */
#define STRINGIFY_HELPER(something)  #something
#define STRINGIFY(something)         STRINGIFY_HELPER(something)

/* Min and max */
#ifndef MIN
#define MIN(x, y)	(((x) < (y))?(x):(y))
#endif
#ifndef MAX
#define MAX(x, y)	(((x) > (y))?(x):(y))
#endif

#ifndef FLOOR
#define FLOOR(a,b)      ((a) - ((a)%(b)))
#endif
#ifndef CEILING
#define CEILING(a,b)    ((a) <= 0 ? 0 :  (FLOOR((a)-1,b) + (b)))
#endif

#include "rdma/fi_errno.h"
#include "rdma/fabric.h"

static inline void ct_print_fi_error(const char *fi_fname, int ret_val)
{
	fprintf(stderr, "%s() ret=%d (%s)\n", fi_fname, ret_val,
		fi_strerror(-ret_val));
	fflush(stdout);
}

#define CT_STD_OPTS "p:"

static inline void ct_print_opts_usage(const char *opt, const char *desc)
{
	fprintf(stderr, " %-20s %s\n", opt, desc);
}

static inline void ct_print_std_usage()
{
	ct_print_opts_usage("-p <provider>", "specified provider (e.g., gni)");
}

#include <string.h>
static inline void
ct_parse_std_opts(int op, char *optarg, struct fi_info *hints)
{
	switch(op) {
	case 'p':
		if (hints) {
			if (!hints->fabric_attr) {
				hints->fabric_attr =
					malloc(sizeof(struct fi_fabric_attr));
			}
			assert(hints->fabric_attr);
			hints->fabric_attr->prov_name = strdup(optarg);
		}
		break;
	}
}

/* general utilities */
#include <sys/time.h>
#include <time.h>
#include <stdint.h>

/* Branch predictor hints */
#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

#ifdef NDEBUG
#define ASSERT_MSG(cond, fmt, args...) do { } while (0)
#else
#include <assert.h>
#include <stdio.h>
#define ASSERT_MSG(cond, fmt, args...) \
	do { \
		if (unlikely(!(cond))) { \
			fprintf(stderr, "[%s:%d] " fmt "\n", __func__, __LINE__, ##args); \
			fflush(stderr); \
			assert(cond); \
		} \
	} while (0)
#endif

static inline uint64_t get_time_usec(void)
{
	struct timeval tv;
	uint64_t usecs;

	gettimeofday(&tv, NULL);
	usecs = (tv.tv_sec * 1000000) + tv.tv_usec;
	return usecs;
}

static inline uint64_t get_time_nsec(void)
{
#ifdef CLOCK_MONOTONIC
	struct timespec tp;
	uint64_t nsecs;

	clock_gettime(CLOCK_MONOTONIC, &tp);
	nsecs = (tp.tv_sec * 1000000000) + tp.tv_nsec;
	return nsecs;
#else
	return get_time_usec() * 1000;
#endif
}


/* thread safe clock start and clock stop */
static inline void ct_start_clock()
{
	if (!is_clock_lock_init) {
		pthread_mutex_init(&clock_lock, NULL);
		is_clock_lock_init = 1;
	}

	if (!pthread_mutex_lock(&clock_lock)) {
		if (!clock_started) {
			start = clock();
			clock_started = 1;
		}
	}
	pthread_mutex_unlock(&clock_lock);
}

static inline void ct_stop_clock()
{
	if (!pthread_mutex_lock(&clock_lock)) {
		if (clock_started) {
			end = clock();
			clock_started = 0;
		}
	}
	pthread_mutex_unlock(&clock_lock);

	if (is_clock_lock_init) {
		pthread_mutex_destroy(&clock_lock);
		is_clock_lock_init = 0;
	}
}

static inline double ct_cpu_time()
{
	return (double) end - (double) start;
}

static inline double ct_wall_clock_time()
{
	return ct_cpu_time() / CLOCKS_PER_SEC;
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
