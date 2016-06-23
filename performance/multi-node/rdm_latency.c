/*
 * Copyright (C) 2002-2012 the Network-Based Computing Laboratory
 * Copyright (c) 2013-2014 Intel Corporation.  All rights reserved.
 * Copyright (c) 2015-2016 Cray Inc.  All rights reserved.
 * Copyright (c) 2015 Los Alamos National Security, LLC. All rights reserved.
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

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>

#include "ft_utils.h"
#include "shared.h"

#define MAX_ALIGNMENT 65536
/*
 * restrict message size to less than the SMSG mailbox threshold
 * for now, which is 16384 - sizeof-of-gni-prov internal header
 */
#define MAX_MSG_SIZE (1<<13)
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

char s_buf_original[MYBUFSIZE];
char r_buf_original[MYBUFSIZE];

static int rx_depth = 512;

struct fi_info *fi, *hints;
struct fid_fabric *fab;
struct fid_domain *dom;
struct fid_ep *ep;
struct fid_av *av;
struct fid_cq *rcq, *scq;
struct fid_mr *mr;
struct fi_context fi_ctx_send;
struct fi_context fi_ctx_recv;
struct fi_context fi_ctx_av;

void *addrs;
fi_addr_t *fi_addrs;

int myid, numprocs;

void print_usage(void)
{
	if (!myid)
		ft_basic_usage(TEST_DESC);
}

static void free_ep_res(void)
{
	fi_close(&av->fid);
	fi_close(&rcq->fid);
	fi_close(&scq->fid);
}

static void cq_readerr(struct fid_cq *cq, const char *cq_str)
{
	struct fi_cq_err_entry cq_err;
	const char *err_str;
	int ret;

	ret = fi_cq_readerr(cq, &cq_err, 0);
	if (ret < 0) {
		FT_PRINTERR("fi_cq_readerr", ret);
	} else {
		err_str = fi_cq_strerror(cq, cq_err.prov_errno, cq_err.err_data,
					NULL, 0);
		fprintf(stderr, "%s: %d %s\n", cq_str, cq_err.err,
			fi_strerror(cq_err.err));
		fprintf(stderr, "%s: prov_err: %s (%d)\n", cq_str, err_str,
			cq_err.prov_errno);
	}
}

/*
 * fi_cq_err_entry can be cast to any CQ entry format.
 */
static int ft_wait_for_comp_omb(struct fid_cq *cq, int num_completions)
{
	struct fi_cq_err_entry comp;
	int ret;

	while (num_completions > 0) {
		ret = fi_cq_read(cq, &comp, 1);
		if (ret > 0) {
			num_completions--;
		} else if (ret < 0 && ret != -FI_EAGAIN) {
			if (ret == -FI_EAVAIL) {
				cq_readerr(cq, "cq");
			} else {
				FT_PRINTERR("fi_cq_read", ret);
			}
			return ret;
		}
	}
	return 0;
}

static int alloc_ep_res(void)
{
	struct fi_cq_attr cq_attr;
	struct fi_av_attr av_attr;
	int ret;

	memset(&cq_attr, 0, sizeof(cq_attr));
	cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	cq_attr.wait_obj = FI_WAIT_NONE;
	cq_attr.size = rx_depth;

	/* Open completion queue for send completions */
	ret = fi_cq_open(dom, &cq_attr, &scq, NULL);
	if (ret) {
		FT_PRINTERR("fi_cq_open", ret);
		goto err1;
	}

	/* Open completion queue for recv completions */
	ret = fi_cq_open(dom, &cq_attr, &rcq, NULL);
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
	ret = fi_av_open(dom, &av_attr, &av, NULL);
	if (ret) {
		FT_PRINTERR("fi_av_open", ret);
		 goto err3;
	 }

	return 0;

err3:
	fi_close(&rcq->fid);
err2:
	fi_close(&scq->fid);
err1:
	return ret;
}

static int bind_ep_res(void)
{
	int ret;

	/* Bind Send CQ with endpoint to collect send completions */
	ret = fi_ep_bind(ep, &scq->fid, FI_TRANSMIT);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	/* Bind Recv CQ with endpoint to collect recv completions */
	ret = fi_ep_bind(ep, &rcq->fid, FI_RECV);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	/* Bind AV with the endpoint to map addresses */
	ret = fi_ep_bind(ep, &av->fid, 0);
	if (ret) {
		FT_PRINTERR("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_enable(ep);
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

	/* Open endpoint */
	ret = fi_endpoint(dom, fi, &ep, NULL);
	if (ret) {
		FT_PRINTERR("fi_endpoint", ret);
		goto err3;
	}

	/* Allocate endpoint resources */
	ret = alloc_ep_res();
	if (ret)
		goto err4;

	/* Bind EQs and AVs with endpoint */
	ret = bind_ep_res();
	if (ret)
		goto err5;

	return 0;

err5:
	free_ep_res();
err4:
	fi_close(&ep->fid);
err3:
	fi_close(&dom->fid);
err2:
	fi_close(&fab->fid);
err1:
	return ret;
}

static int init_av(void)
{
	void *addr;
	size_t addrlen = 0;
	int ret;

	fi_getname(&ep->fid, NULL, &addrlen);
	addr = malloc(addrlen);
	assert(addr);
	ret = fi_getname(&ep->fid, addr, &addrlen);
	if (ret != 0) {
		FT_PRINTERR("fi_getname", ret);
		return ret;
	}

	addrs = malloc(numprocs * addrlen);
	assert(addrs);

	FT_Allgather(addr, addrlen, addrs);

	fi_addrs = malloc(numprocs * sizeof(fi_addr_t));
	assert(fi_addrs);

	/* Insert address to the AV and get the fabric address back */
	ret = fi_av_insert(av, addrs, numprocs, fi_addrs, 0, &fi_ctx_av);
	if (ret != numprocs) {
		FT_PRINTERR("fi_av_insert", ret);
		return ret;
	}

	free(addr);

	return 0;
}

int main(int argc, char *argv[])
{
	int i, peer;
	int size, align_size;
	char *s_buf, *r_buf;
	uint64_t t_start = 0, t_end = 0;
	int op, ret;
	ssize_t __attribute__((unused)) fi_rc;

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
	hints->caps		= FI_TAGGED | FI_DIRECTED_RECV;
	hints->mode		= FI_CONTEXT | FI_LOCAL_MR;

	if (numprocs != 2) {
		if (myid == 0) {
			fprintf(stderr, "This test requires exactly two processes\n");
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

	ret = init_av();
	if (ret) {
		fprintf(stderr, "Problem in AV initialization\n");
		return ret;
	}

	/* Data initialization */
	align_size = getpagesize();
	assert(align_size <= MAX_ALIGNMENT);

	s_buf = (char *) (((unsigned long) s_buf_original + (align_size - 1)) /
				align_size * align_size);

	r_buf = (char *) (((unsigned long) r_buf_original + (align_size - 1)) /
				align_size * align_size);

	if (myid == 0) {
		fprintf(stdout, HEADER);
		fprintf(stdout, "%-*s%*s\n", 10, "# Size", FIELD_WIDTH, "Latency (us)");
		fflush(stdout);
	}

	for (size = 0; size <= MAX_MSG_SIZE; size = (size ? size * 2 : 1)) {
		for (i = 0; i < size; i++) {
			s_buf[i] = 'a';
			r_buf[i] = 'b';
		}

		if (size > large_message_size) {
			loop = loop_large;
			skip = skip_large;
		}

		FT_Barrier();

		if (myid == 0) {
			peer = 1;
			for (i = 0; i < loop + skip; i++) {
				if (i == skip)
					t_start = get_time_usec();

				fi_rc = fi_tsend(ep, s_buf, size, NULL,
						fi_addrs[peer], 0xDEADBEEF, NULL);
				assert(!fi_rc);
				ft_wait_for_comp_omb(scq, 1);

				fi_rc = fi_trecv(ep, r_buf, size, NULL,
						fi_addrs[peer], 0xDEADBEEF, 0, NULL);
				assert(!fi_rc);
				ft_wait_for_comp_omb(rcq, 1);
			}

			t_end = get_time_usec();
		} else if (myid == 1) {
			peer = 0;
			for (i = 0; i < loop + skip; i++) {
				fi_rc = fi_trecv(ep, r_buf, size, NULL,
						fi_addrs[peer], 0xDEADBEEF, 0, NULL);
				assert(!fi_rc);
				ft_wait_for_comp_omb(rcq, 1);

				fi_rc = fi_tsend(ep, s_buf, size, NULL,
						fi_addrs[peer], 0xDEADBEEF, NULL);
				assert(!fi_rc);
				ft_wait_for_comp_omb(scq, 1);
			}
		}

		if (myid == 0) {
			double latency = (t_end - t_start) / (2.0 * loop);

			fprintf(stdout, "%-*d%*.*f\n", 10, size, FIELD_WIDTH,
					FLOAT_PRECISION, latency);
			fflush(stdout);
		}
	}

	FT_Barrier();

	free_ep_res();

	fi_close(&ep->fid);
	fi_close(&dom->fid);
	fi_close(&fab->fid);

	fi_freeinfo(hints);
	fi_freeinfo(fi);

	FT_Barrier();
	FT_Finalize();
	return 0;
}

/* vi:set sw=8 sts=8 */

