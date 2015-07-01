/*
 * Copyright (C) 2002-2012 the Network-Based Computing Laboratory
 * Copyright (c) 2013-2014 Intel Corporation.  All rights reserved.
 * Copyright (c) 2015 Cray Inc.  All rights reserved.
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

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <sys/time.h>
#include <string.h>
#include <pthread.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>

#include "ft_utils.h"
#include "shared.h"

#define MAX_ALIGNMENT 65536
/* #define MAX_MSG_SIZE (1<<22) */
#define MAX_MSG_SIZE (8*1024) /* Current GNI provider max send size */
#define MYBUFSIZE (MAX_MSG_SIZE + MAX_ALIGNMENT)

#define TEST_DESC "Libfabric Latency Test"
#define HEADER "# " TEST_DESC " \n"
#ifndef FIELD_WIDTH
#   define FIELD_WIDTH 20
#endif
#ifndef FLOAT_PRECISION
#   define FLOAT_PRECISION 2
#endif

int skip = 1000;
int loop = 10000;
int skip_large = 10;
int loop_large = 100;
int large_message_size = 8192;

static int rx_depth = 512;

struct per_thread_data {
	pthread_t thread;
	int tid; /* thread id */
	struct fid_ep *ep;
	struct fid_av *av;
	struct fid_cq *rcq, *scq;
	struct fid_mr *r_mr, *l_mr;
	struct fi_context fi_ctx_send;
	struct fi_context fi_ctx_recv;
	struct fi_context fi_ctx_av;
	char s_buf_original[MYBUFSIZE];
	char r_buf_original[MYBUFSIZE];
	char *s_buf;
	char *r_buf;
	void *addrs;
	fi_addr_t *fi_addrs;
	double latency;
};

struct per_iteration_data {
	union {
		struct {
			uint32_t thread_id;
			uint32_t message_size;
		};
		void *data;
	};
};

pthread_barrier_t barr;
struct per_thread_data *thread_data;
struct fi_info *fi, *hints;
struct fid_fabric *fab;
struct fid_domain *dom;

int myid, numprocs;

struct test_tunables {
	int threads;
};

enum {
	ENV_TYPE_INT = 1,
	ENV_TYPE_BOOL,
	ENV_TYPE_STR,
};

enum {
	RDM_BW_THREADS = 0,
	RDM_MAX_ENV_CNT
};

struct environ_var {
	const char *name;
	int type;
	union _defaults {
		void *addr;
		int int_val;
		int bool_val;
		char *str_val;
	} def; /* defaults */
	union _tunables {
		void **addr;
		int *int_val;
		int *bool_val;
		char **str_val;
	} out;
};

struct test_tunables tunables;
struct environ_var env_vars[RDM_MAX_ENV_CNT] = {
		{ .name = "RDM_BW_THREADS",
		  .type = ENV_TYPE_INT,
		  .def.int_val = 1,
		  .out.addr = (void *) &tunables.threads,
		},
};

pthread_mutex_t mutex;

static void get_environment_variables(void)
{
	struct environ_var *var;
	char *value = NULL, *tmp, *end;
	int i;

	for (i = 0; i < RDM_MAX_ENV_CNT; i++) {
		var = &env_vars[i];
		if (!var->def.addr)
			continue;

		switch (var->type) {
		case ENV_TYPE_INT:
			*var->out.int_val = var->def.int_val;
			break;
		case ENV_TYPE_BOOL:
			*var->out.bool_val = (var->def.bool_val != 0);
			break;
		case ENV_TYPE_STR:
			strcpy(*var->out.str_val, var->def.str_val);
			break;
		}

		value = getenv(var->name);
		if (!value)
			continue;

		tmp = strdup(value);
		if (!tmp)
			continue;

		switch (var->type) {
		case ENV_TYPE_INT:
			*var->out.int_val = strtol(tmp, &end, 10);
			if (errno == EINVAL || errno == ERANGE)
				*var->out.int_val = var->def.int_val;
			break;
		case ENV_TYPE_BOOL:
			*var->out.bool_val = strtol(tmp, &end, 10);
			if (errno == EINVAL || errno == ERANGE)
				*var->out.bool_val = var->def.bool_val;

			*var->out.bool_val = (*var->out.bool_val != 0);
			break;
		case ENV_TYPE_STR:
			strcpy(*var->out.str_val, tmp);
			break;
		}

		free(tmp);
	}
}

void print_usage(void)
{
	if (!myid)
		ft_basic_usage(TEST_DESC);
}

static void free_ep_res(struct per_thread_data *ptd)
{
	fi_close(&ptd->av->fid);
	fi_close(&ptd->rcq->fid);
	fi_close(&ptd->scq->fid);
}

static int alloc_ep_res(struct per_thread_data *ptd)
{
	struct fi_cq_attr cq_attr;
	struct fi_av_attr av_attr;
	int ret;

	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	cq_attr.wait_obj = FI_WAIT_NONE;
	cq_attr.size = rx_depth;

	/* Open completion queue for send completions */
	ret = fi_cq_open(dom, &cq_attr, &ptd->scq, NULL);
	if (ret) {
		FT_PRINTERR("fi_cq_open", ret);
		goto err1;
	}

	/* Open completion queue for recv completions */
	ret = fi_cq_open(dom, &cq_attr, &ptd->rcq, NULL);
	if (ret) {
		FT_PRINTERR("fi_cq_open", ret);
		goto err2;
	}

	memset(&av_attr, 0, sizeof(av_attr));
	av_attr.type = fi->domain_attr->av_type ?
			fi->domain_attr->av_type : FI_AV_MAP;
	av_attr.count = 2;
	av_attr.name = NULL;

	/* Open address vector (AV) for mapping address */
	ret = fi_av_open(dom, &av_attr, &ptd->av, NULL);
	if (ret) {
		FT_PRINTERR("fi_av_open", ret);
		 goto err3;
	 }

	return 0;

err3:
	fi_close(&ptd->rcq->fid);
err2:
	fi_close(&ptd->scq->fid);
err1:
	return ret;
}

static int bind_ep_res(struct per_thread_data *ptd)
{
	int ret;

	/* Bind Send CQ with endpoint to collect send completions */
	ret = fi_ep_bind(ptd->ep, &ptd->scq->fid, FI_SEND|FI_WRITE);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	/* Bind Recv CQ with endpoint to collect recv completions */
	ret = fi_ep_bind(ptd->ep, &ptd->rcq->fid, FI_RECV);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	/* Bind AV with the endpoint to map addresses */
	ret = fi_ep_bind(ptd->ep, &ptd->av->fid, 0);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_enable(ptd->ep);
	if (ret) {
		FT_PRINTERR("fi_enable", ret);
		return ret;
	 }

	return ret;
}

static int init_fabric(void)
{
	int ret;
	uint64_t flags = 0;

	/* Get fabric info */
	ret = fi_getinfo(FT_FIVERSION, NULL, NULL, flags, hints, &fi);
	if (ret) {
		FT_PRINTERR("fi_getinfo", ret);
		return ret;
	}

	/* Open fabric */
	ret = fi_fabric(fi->fabric_attr, &fab, NULL);
	if (ret) {
		FT_PRINTERR("fi_fabric", ret);
		goto err1;
	}

	/* Open domain */
	ret = fi_domain(fab, fi, &dom, NULL);
	if (ret) {
		FT_PRINTERR("fi_domain", ret);
		goto err2;
	}

	return 0;

err2:
	fi_close(&fab->fid);
err1:
	return ret;
}

static int init_endpoint(struct per_thread_data *ptd)
{
	int ret;

	/* Open endpoint */
	ret = fi_endpoint(dom, fi, &ptd->ep, NULL);
	if (ret) {
		FT_PRINTERR("fi_endpoint", ret);
		goto err3;
	}

	/* Allocate endpoint resources */
	ret = alloc_ep_res(ptd);
	if (ret)
		goto err4;

	/* Bind EQs and AVs with endpoint */
	ret = bind_ep_res(ptd);
	if (ret)
		goto err5;

	return 0;

err5:
	free_ep_res(ptd);
err4:
	fi_close(&ptd->ep->fid);
err3:
	fi_close(&dom->fid);
	return ret;
}

static int init_av(struct per_thread_data *ptd)
{
	void *addr;
	size_t addrlen = 0;
	int ret;

	fi_getname(&ptd->ep->fid, NULL, &addrlen);
	addr = malloc(addrlen);
	assert(addr);
	ret = fi_getname(&ptd->ep->fid, addr, &addrlen);
	if (ret != 0) {
		FT_PRINTERR("fi_getname", ret);
		return ret;
	}

	ptd->addrs = malloc(numprocs * addrlen);
	assert(ptd->addrs);

	FT_Allgather(addr, addrlen, ptd->addrs);

	ptd->fi_addrs = malloc(numprocs * sizeof(fi_addr_t));
	assert(ptd->fi_addrs);

	/* Insert address to the AV and get the fabric address back */
	ret = fi_av_insert(ptd->av, ptd->addrs, numprocs,
			   ptd->fi_addrs, 0, &ptd->fi_ctx_av);
	if (ret != numprocs) {
		FT_PRINTERR("fi_av_insert", ret);
		return ret;
	}

	free(addr);

	return 0;
}

int init_per_thread_data(struct per_thread_data *ptd)
{
	int align_size;
	int ret;

	ret = init_endpoint(ptd);
	if (ret) {
		fprintf(stderr, "Problem in endpoint initialization\n");
		return ret;
	}

	ret = init_av(ptd);
	if (ret) {
		fprintf(stderr, "Problem in AV initialization\n");
		return ret;
	}

	/* Data initialization */
	align_size = getpagesize();
	assert(align_size <= MAX_ALIGNMENT);

	ptd->s_buf = (char *) (((unsigned long)
			ptd->s_buf_original + (align_size - 1)) /
			align_size * align_size);
	ptd->r_buf = (char *) (((unsigned long)
			ptd->r_buf_original + (align_size - 1)) /
			align_size * align_size);

	return 0;
}

int fini_per_thread_data(struct per_thread_data *ptd)
{
	fi_close(&ptd->l_mr->fid);
	fi_close(&ptd->r_mr->fid);

	free_ep_res(ptd);

	fi_close(&ptd->ep->fid);

	return 0;
}

void *thread_fn(void *data)
{
	int i, peer;
	int size;
	ssize_t fi_rc;
	uint64_t t_start = 0, t_end = 0;
	struct per_thread_data *ptd;
	struct per_iteration_data it;

	it.data = data;
	size = it.message_size;

	if (it.thread_id >= tunables.threads)
		return (void *)-EINVAL;

	ptd = &thread_data[it.thread_id];

#ifdef THREAD_SYNC
	if (!it.thread_id)
		FT_Barrier();
	pthread_barrier_wait(&barr);
#endif

	if (myid == 0) {
		peer = 1;
		for (i = 0; i < loop + skip; i++) {
			if (i == skip)
				t_start = get_time_usec();

			fi_rc = fi_send(ptd->ep, ptd->s_buf, size, NULL,
					ptd->fi_addrs[peer], NULL);
			assert(!fi_rc);
			wait_for_data_completion(ptd->scq, 1);

			fi_rc = fi_recv(ptd->ep, ptd->r_buf, size, NULL,
					ptd->fi_addrs[peer], NULL);
			assert(!fi_rc);
			wait_for_data_completion(ptd->rcq, 1);
		}

		t_end = get_time_usec();
	} else if (myid == 1) {
		peer = 0;
		for (i = 0; i < loop + skip; i++) {
			fi_rc = fi_recv(ptd->ep, ptd->r_buf, size, NULL,
					ptd->fi_addrs[peer], NULL);
			assert(!fi_rc);
			wait_for_data_completion(ptd->rcq, 1);

			fi_rc = fi_send(ptd->ep, ptd->s_buf, size, NULL,
					ptd->fi_addrs[peer], NULL);
			assert(!fi_rc);
			wait_for_data_completion(ptd->scq, 1);
		}
	}

#ifdef THREAD_SYNC
	if (!it.thread_id)
		FT_Barrier();
	pthread_barrier_wait(&barr);
#endif

	ptd->latency = (t_end - t_start) / (2.0 * loop);

	return NULL;
}

int main(int argc, char *argv[])
{
	int i, j;
	int size;
	int op, ret;
	struct per_iteration_data iter_key;
	struct per_thread_data *ptd;
	double min_lat, max_lat, sum_lat;

	get_environment_variables();
	printf("%i thread_data\n", tunables.threads);

	thread_data = calloc(tunables.threads, sizeof(struct per_thread_data));
	if (!thread_data) {
		fprintf(stderr,
			"Could not allocate memory for per thread struct\n");
		return -1;
	}

	FT_Init(&argc, &argv);
	FT_Rank(&myid);
	FT_Job_size(&numprocs);

	hints = fi_allocinfo();
	if (!hints)
		return -1;

	while ((op = getopt(argc, argv, "h" INFO_OPTS)) != -1) {
		switch (op) {
		default:
			ft_parseinfo(op, optarg, hints);
			break;
		case '?':
		case 'h':
			print_usage();
			return EXIT_FAILURE;
		}
	}

	hints->ep_attr->type	= FI_EP_RDM;
	hints->caps		= FI_MSG | FI_DIRECTED_RECV;
	hints->mode		= FI_CONTEXT | FI_LOCAL_MR;

	if (numprocs != 2) {
		if (myid == 0) {
			fprintf(stderr,
				"This test requires exactly two processes\n");
		}
		FT_Finalize();
		return -1;
	}

	/* Fabric initialization */
	ret = init_fabric();
	if (ret) {
		fprintf(stderr, "Problem in fabric initialization\n");
		return ret;
	}

	for (i = 0; i < tunables.threads; i++) {
		init_per_thread_data(&thread_data[i]);
	}

	pthread_barrier_init(&barr, NULL, tunables.threads);

	if (myid == 0) {
		fprintf(stdout, HEADER);
		fprintf(stdout, "%-*s%*s%*s%*s\n", 10, "# Size",
			FIELD_WIDTH, "Latency (us)",
			FIELD_WIDTH, "Min Lat (us)",
			FIELD_WIDTH, "Max Lat (us)");
		fflush(stdout);
	}

	for (size = 1; size <= MAX_MSG_SIZE; size = (size ? size * 2 : 1)) {
		for (i = 0; i < tunables.threads; i++) {
			ptd = &thread_data[i];

			for (j = 0; j < size; j++) {
				ptd->s_buf[j] = 'a';
				ptd->r_buf[j] = 'b';
			}

			if (size > large_message_size) {
				loop = loop_large;
				skip = skip_large;
			}
		}

		iter_key.message_size = size;

		FT_Barrier();

		for (i = 0; i < tunables.threads; i++) {
			iter_key.thread_id = i;
			ret = pthread_create(&thread_data[i].thread, NULL,
					thread_fn, iter_key.data);
			if (ret != 0) {
				printf("couldn't create thread %i\n", i);
				pthread_exit(NULL);
			}
		}

		for (i = 0; i < tunables.threads; i++)
			pthread_join(thread_data[i].thread, NULL);

		FT_Barrier();

		if (myid == 0) {
			min_lat = max_lat = sum_lat = thread_data[0].latency;
			for (i = 1; i < tunables.threads; i++) {
				if (thread_data[i].latency < min_lat) {
					min_lat = thread_data[i].latency;
				} else if (thread_data[i].latency > max_lat) {
					max_lat = thread_data[i].latency;
				}
				sum_lat += thread_data[i].latency;
			}
			fprintf(stdout, "%-*d%*.*f%*.*f%*.*f\n", 10, size,
				FIELD_WIDTH, FLOAT_PRECISION,
				sum_lat / tunables.threads,
				FIELD_WIDTH, FLOAT_PRECISION, min_lat,
				FIELD_WIDTH, FLOAT_PRECISION, max_lat);
			fflush(stdout);
		}
	}

	FT_Barrier();

	for (i = 0; i < tunables.threads; i++) {
		fini_per_thread_data(&thread_data[i]);
	}

	fi_close(&dom->fid);
	fi_close(&fab->fid);

	fi_freeinfo(hints);
	fi_freeinfo(fi);

	FT_Barrier();
	FT_Finalize();
	return 0;
}

/* vi:set sw=8 sts=8 */

