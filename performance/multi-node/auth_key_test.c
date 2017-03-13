/* -*- mode: C; tab-width: 2; indent-tabs-mode: nil; -*- */

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

#include <config.h>

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdint.h>
#include <sys/uio.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_ext_gni.h>

#include "ct_utils.h"
#include "rdmacred.h"

#define PRINT(stream, fmt, args...) \
	do { \
		fprintf(stream, fmt, ##args); \
		fflush(stream); \
	} while (0)
#define VERBOSE(fmt, args...) PRINT(stdout,"rank %d: " fmt, rank, ##args)
#define ERROR(fmt, args...) PRINT(stderr, fmt, ##args)
#if 1
# define DEBUG(fmt, args...) ;
#else
# define DEBUG(fmt, args...) \
	do { \
		ERROR("[%s:%d] " fmt, __FILE__, __LINE__, ##args); \
		fflush(stderr); \
	} while (0)
#endif

#define GNIX_EP_RDM_PRIMARY_CAPS                                               \
	(FI_MSG | FI_RMA | FI_TAGGED | FI_ATOMICS |                            \
	 FI_DIRECTED_RECV | FI_READ |                                          \
	 FI_WRITE | FI_SEND | FI_RECV | FI_REMOTE_READ | FI_REMOTE_WRITE)

/* Program-level defines */

#define TEST_DESC "Libfabric Authorization Key Test"
#define HEADER "# " TEST_DESC " \n"
#ifndef FIELD_WIDTH
#   define FIELD_WIDTH 20
#endif
#ifndef FLOAT_PRECISION
#   define FLOAT_PRECISION 2
#endif

#define HEAD_RANK (rank == 0)

/* global variables */
static struct fi_gni_auth_key auth_key;

/* Allocate main table (in global memory) */
uint64_t *local_data_table;

static int rx_depth = 512;
static int thread_safe = 1; // default

struct peer_data {
	void *r_table_addr;
	uint64_t r_table_key;
	void *r_lock_addr;
	uint64_t r_lock_key;
};

struct peer {
	struct fid_ep *ep;
	struct fid_cq *scq;
	struct fid_cq *rcq;
	struct peer_data data;
};

typedef struct application_data {
	struct fid_mr *l_table_mr;
	struct fid_mr *l_auth_key_mr;
} app_data_t;

typedef struct fabric {
	struct fi_info *hints;
	struct fi_info *info;
	struct fid_fabric *fab;
	struct fid_domain *dom;
	struct fid_av *av;
	void *addrs;
	fi_addr_t *fi_addrs;
	app_data_t data;
	struct peer *peers;
} fabric_t;

fabric_t prov;

int rank, num_ranks;
uint64_t saved_initial_ran_val;

struct test_tunables {
	uint64_t total_mem_opt;
	int num_updates_opt;
	int use_drc;
};

static struct test_tunables tunables = {0};

static double usec_time(void)
{
	struct timeval tp;
	gettimeofday(&tp, NULL);
	return tp.tv_sec + tp.tv_usec / (double)1.0e6;
}

static inline void exchange_registration_data(fabric_t *fabric)
{
	uint64_t *keys;
	void **addresses;
	uint64_t local_table_key, local_lock_key;
	int i;
	app_data_t *data = &fabric->data;
	struct peer *peers = fabric->peers;

	keys = calloc(num_ranks, sizeof(*keys));
	assert(keys);

	addresses = calloc(num_ranks, sizeof(*addresses));
	assert(addresses);

	local_table_key = fi_mr_key(data->l_table_mr);

	DEBUG("Exchanging registration data\n");

	ctpm_Barrier();

	DEBUG("Allgather 1\n");
	/* first, table addr */
	ctpm_Allgather(&local_table_key, sizeof(local_table_key), keys);

	for (i = 0; i < num_ranks; i++)
		peers[i].data.r_table_key = keys[i];

	ctpm_Barrier();

	DEBUG("Allgather 2\n");

	DEBUG("Sending table %p, size=%d, dest=%p\n", local_data_table, sizeof(void *),
			addresses);
	ctpm_Allgather(&local_data_table, sizeof(void *), addresses);
	ctpm_Barrier();

	for (i = 0; i < num_ranks; i++)
		peers[i].data.r_table_addr = addresses[i];

	ctpm_Barrier();

	ctpm_Barrier();
	DEBUG("Finished exchanging data\n");
}

/* function declarations */
CT_ALLREDUCE_FUNC(int_sum, int, +);
CT_ALLREDUCE_FUNC(int64_sum, int64_t, +);

static inline void print_usage(void)
{
	if (!rank) {
		ERROR("\n%s\n", TEST_DESC);
		ERROR("\nOptions:\n");

		ct_print_opts_usage("-h", "display this help message");
		ct_print_opts_usage("-m", "memory in bytes per PE");
		ct_print_opts_usage("-n", "number of updates per PE");
		ct_print_std_usage();
	}
}

static inline void free_ep_res(fabric_t *fabric, int remote_rank)
{
	struct peer *p = &fabric->peers[remote_rank];
	int ret;

	ret = fi_close(&p->rcq->fid);
	if (ret != FI_SUCCESS)
		ct_print_fi_error("fi_close: rcq", ret);
	else
		p->rcq = NULL;

	ret = fi_close(&p->scq->fid);
	if (ret != FI_SUCCESS)
		ct_print_fi_error("fi_close: scq", ret);
	else
		p->scq = NULL;
}

static inline int alloc_ep_res(fabric_t *fabric, int remote_rank)
{
	struct peer *p = &fabric->peers[remote_rank];
	struct fi_cq_attr cq_attr;
	int ret;

	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	cq_attr.wait_obj = FI_WAIT_NONE;
	cq_attr.size = rx_depth;

	/* Open completion queue for send completions */
	ret = fi_cq_open(fabric->dom, &cq_attr, &p->scq, NULL);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_cq_open", ret);
		goto err_scq_open;
	}

	/* Open completion queue for recv completions */
	ret = fi_cq_open(fabric->dom, &cq_attr, &p->rcq, NULL);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_cq_open", ret);
		goto err_rcq_open;
	}

	return 0;

	err_rcq_open: fi_close(&p->scq->fid);
	p->scq = NULL;
	err_scq_open: return ret;
}

static inline int bind_ep_res(fabric_t *fabric, int remote_rank)
{
	struct peer *p = &fabric->peers[remote_rank];
	int ret;

	/* Bind Send CQ with endpoint to collect send completions */
	ret = fi_ep_bind(p->ep, &p->scq->fid, FI_TRANSMIT);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	/* Bind Recv CQ with endpoint to collect recv completions */
	ret = fi_ep_bind(p->ep, &p->rcq->fid, FI_RECV);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	/* Bind AV with the endpoint to map addresses */
	ret = fi_ep_bind(p->ep, &fabric->av->fid, 0);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_enable(p->ep);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_enable", ret);
		return ret;
	}

	return ret;
}

static inline int init_fabric(fabric_t *fabric)
{
	int ret;
	uint64_t flags = 0;

	/* Get fabric info */
	ret = fi_getinfo(fi_version(), NULL, NULL, flags, fabric->hints,
			&fabric->info);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_getinfo", ret);
		return ret;
	}

	/* Open fabric */
	ret = fi_fabric(fabric->info->fabric_attr, &fabric->fab, NULL);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_fabric", ret);
		goto err_fabric_open;
	}

	/* Open domain */
	ret = fi_domain(fabric->fab, fabric->info, &fabric->dom, NULL);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_domain", ret);
		goto err_domain_open;
	}

	return 0;

	err_domain_open: fi_close(&fabric->fab->fid);
	err_fabric_open: return ret;
}

static int init_endpoint(fabric_t *fabric, int remote_rank)
{
	struct peer *p = &fabric->peers[remote_rank];
	int ret;
	int i;

	/* Open endpoint */
	ret = fi_endpoint(fabric->dom, fabric->info, &p->ep, NULL);
	if (ret) {
		ct_print_fi_error("fi_endpoint", ret);
		goto err_fi_endpoint;
	}

	/* Allocate endpoint resources */
	ret = alloc_ep_res(fabric, remote_rank);
	if (ret)
		goto err_alloc_ep_res;

	/* Bind EQs and AVs with endpoint */
	ret = bind_ep_res(fabric, remote_rank);
	if (ret)
		goto err_bind_ep_res;

	return 0;

	err_bind_ep_res: free_ep_res(fabric, remote_rank);
	err_alloc_ep_res: fi_close(&p->ep->fid);
	err_fi_endpoint: return ret;
}

static int init_av(fabric_t *fabric)
{
	void *addr;
	size_t addrlen = 0;
	int ret;
	int i;
	struct peer *peers = fabric->peers;

	/* use first ep since they will all have the same source addr */
	for (i = 0; i < num_ranks; i++)
		if (fabric->peers[i].ep)
			break;
	assert(i < num_ranks);

	fi_getname(&peers[i].ep->fid, NULL, &addrlen);

	addr = malloc(addrlen);
	assert(addr);

	ret = fi_getname(&peers[i].ep->fid, addr, &addrlen);
	if (ret != 0) {
		ct_print_fi_error("fi_getname", ret);
		free(addr);
		return ret;
	}

	fabric->addrs = calloc(num_ranks, addrlen);
	assert(fabric->addrs);

	ctpm_Allgather(addr, addrlen, fabric->addrs);

	fabric->fi_addrs = calloc(num_ranks, sizeof(fi_addr_t));
	assert(fabric->fi_addrs);

	/* Insert address to the AV and get the fabric address back */
	ret = fi_av_insert(fabric->av, fabric->addrs, num_ranks, fabric->fi_addrs,
			0, NULL);
	if (ret != num_ranks) {
		ct_print_fi_error("fi_av_insert", ret);
		return ret;
	}

	free(addr);

	return 0;
}


static int random_access(void)
{
	int64_t local_table_size; /* Local table width */
	static int send_abort, recv_abort;
	int rc;
	struct iovec mr_iov = {0};
	struct fi_mr_attr attr = {
		.mr_iov = &mr_iov,
		.iov_count = 1,
		.access = (FI_REMOTE_READ | FI_REMOTE_WRITE | FI_SEND |
				FI_RECV | FI_READ | FI_WRITE),
		.offset = 0,
		.requested_key = 0,
		.context = NULL,
		.auth_key = NULL,
		.auth_key_size = 0,
	};

	local_table_size = 1 << 18;

	send_abort = 0;
	local_data_table = calloc(local_table_size, sizeof(uint64_t));
	if (!local_data_table)
		send_abort = 1;

	ctpm_Barrier();
	recv_abort = ct_allreduce_int_sum(&send_abort, num_ranks);
	ctpm_Barrier();

	if (recv_abort > 0) {
		if (HEAD_RANK)
			ERROR("Failed to allocate memory for the main table.\n");
		/* check all allocations in case there are new added and their order changes */
		if (local_data_table)
			free(local_data_table);
		return -1;
	}

	mr_iov.iov_base = local_data_table;
	mr_iov.iov_len = local_table_size * sizeof(uint64_t);

	/* register memory */
	rc = fi_mr_regattr(prov.dom, &attr, 0, &prov.data.l_table_mr);
	if (rc != FI_SUCCESS)
		DEBUG("rc=%d\n", rc);
	assert(rc == FI_SUCCESS);

	/* register memory with new authorization key */
	attr.auth_key = (uint8_t *) &auth_key;
	attr.auth_key_size = sizeof(struct fi_gni_raw_auth_key);

	rc = fi_mr_regattr(prov.dom, &attr, 0, &prov.data.l_auth_key_mr);
	if (rc != FI_SUCCESS)
		DEBUG("rc=%d\n", rc);
	assert(rc == FI_SUCCESS);

	exchange_registration_data(&prov);

	rc = fi_close(&prov.data.l_table_mr->fid);
	assert(rc == FI_SUCCESS);

	rc = fi_close(&prov.data.l_auth_key_mr->fid);
	assert(rc == FI_SUCCESS);

	ctpm_Barrier();

	return 0;
}

static inline int init_gnix_prov(fabric_t *fabric)
{
	int ret;
	int i;
	struct fi_av_attr av_attr;

	/* Fabric initialization */
	ret = init_fabric(fabric);
	if (unlikely(ret)) {
		ERROR("Problem in fabric initialization\n");
		return ret;
	}

	fabric->peers = calloc(num_ranks, sizeof(*fabric->peers));
	assert(fabric->peers);

	// Set AV attributes
	memset(&av_attr, 0, sizeof(av_attr));
	av_attr.type =
			fabric->info->domain_attr->av_type ?
					fabric->info->domain_attr->av_type : FI_AV_MAP;
	av_attr.count = num_ranks;
	av_attr.name = NULL;

	/* Open address vector (AV) for mapping address */
	ret = fi_av_open(fabric->dom, &av_attr, &fabric->av, NULL);
	if (unlikely(ret)) {
		ct_print_fi_error("fi_av_open", ret);
		return ret;
	}

	// initialize all endpoints
	for (i = 0; i < num_ranks; i++) {
		ret = init_endpoint(fabric, i);
		if (ret) {
			ERROR("Problem in endpoint initialization\n");
			return ret;
		}
	}

	// init av
	ret = init_av(fabric);
	if (unlikely(ret)) {
		ERROR("Problem in AV initialization\n");
		return ret;
	}

	return 0;
}

static inline void fini_gnix_prov(fabric_t *fabric)
{
	int ret;
	int i;

	if (fabric->info)
		free(fabric->info);
	if (fabric->hints)
		free(fabric->hints);

	// close all endpoints
	for (i = 0; i < num_ranks; i++) {
		free_ep_res(fabric, i);

		fi_close(&fabric->peers[i].ep->fid);
	}

	if (fabric->av) {
		ret = fi_close(&fabric->av->fid);
		if (ret != FI_SUCCESS)
			ct_print_fi_error("fi_close: av", ret);
	}

	if (fabric->dom) {
		fi_close(&fabric->dom->fid);
		if (ret != FI_SUCCESS)
			ct_print_fi_error("fi_close: dom", ret);
	}

	if (fabric->fab) {
		fi_close(&fabric->fab->fid);
		if (ret != FI_SUCCESS)
			ct_print_fi_error("fi_close: fab", ret);
	}

	if (fabric->peers)
		free(fabric->peers);
	if (fabric->addrs)
		free(fabric->addrs);
}

int main(int argc, char **argv)
{
	int i, j;
	int size;
	int op, ret;
	struct fi_info *hints;
	int credential;
	int *credential_arr;
	drc_info_handle_t info;

	ctpm_Init(&argc, &argv);
	ctpm_Rank(&rank);
	ctpm_Job_size(&num_ranks);

	prov.hints = fi_allocinfo();
	if (!prov.hints)
		return EXIT_FAILURE;

	hints = prov.hints;
	hints->domain_attr->mr_mode = FI_MR_VIRT_ADDR | FI_MR_ALLOCATED | FI_MR_PROV_KEY;

	while ((op = getopt(argc, argv, "hm:n:d" CT_STD_OPTS)) != -1) {
		switch(op) {
		default:
			ct_parse_std_opts(op, optarg, hints);
			break;
		case '?':
		case 'h':
			print_usage();
			return EXIT_FAILURE;
		}
	}

	/* setup authorization key */
	auth_key.type = GNIX_AKT_RAW;

	if (HEAD_RANK) {
		ret = drc_acquire(&credential, 0);
		if (ret) {
			ERROR("failed to acquire credential, ret=%d\n", ret);
			exit(-1);
		}
	}

	ctpm_Bcast(&credential, sizeof(int));

	ret = drc_access(credential, 0, &info);
	if (ret) {
		ERROR("failed to access credential, ret=%d\n", ret);
		if (HEAD_RANK) {
			ret = drc_release(credential, 0);
			if (ret)
				ERROR("failed to release credential, ret=%d\n", ret);
		}
		exit(-1);
	}

	auth_key.raw.protection_key = drc_get_first_cookie(info);
	VERBOSE("Using protection key %08x.\n", auth_key.raw.protection_key);

	// modify hints as needed prior to fabric initialization
	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_TAGGED | FI_DIRECTED_RECV;
	hints->mode = FI_CONTEXT | FI_LOCAL_MR;
	hints->caps |= GNIX_EP_RDM_PRIMARY_CAPS;

	if (!thread_safe) {
		hints->domain_attr->threading = FI_THREAD_COMPLETION;
		hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
		hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
	}

	// initialize the gnix provider fabric
	init_gnix_prov(&prov);

	// run random access test
	ctpm_Barrier();
	random_access();
	ctpm_Barrier();

	// close the gnix provider fabric
	fini_gnix_prov(&prov);

	if (HEAD_RANK)
		ret = drc_release(credential, 0);

	// barrier and finalize
	ctpm_Barrier();

	ctpm_Finalize();

	return 0;
}

/* vi:set sw=8 sts=8 */
