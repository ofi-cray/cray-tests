/*
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
#include <string.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>
#include <stdatomic.h>

#include <pthread.h>

#include "ct_utils.h"
#include "ct_tbarrier.h"

#define MAX_ALIGNMENT 65536
#define MAX_MSG_SIZE (1<<22)
#define MYBUFSIZE (MAX_MSG_SIZE + MAX_ALIGNMENT)

#define TEST_DESC "Libfabric Bandwidth Test"
#define HEADER "# " TEST_DESC " \n"
#ifndef FIELD_WIDTH
#   define FIELD_WIDTH 20
#endif
#ifndef FLOAT_PRECISION
#   define FLOAT_PRECISION 2
#endif

int loop = 1000;
int window_size = 64;
int skip = 10;

int loop_large = 20;
int window_size_large = 64;
int skip_large = 2;

int large_message_size = 8192;

static int rx_depth = 512;
atomic_int tbar_counter[2] __attribute__ ((aligned (64)));
atomic_int tbar_signal[2] __attribute__ ((aligned (64)));

typedef struct buf_desc {
	uint64_t addr;
	uint64_t key;
} buf_desc_t;


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
	buf_desc_t *rbuf_descs;
	double latency;
	uint64_t bytes_sent;
	uint64_t time_start;
	uint64_t time_end;
	fabtests_tbar_t tbar;
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


static pthread_barrier_t thread_barrier;
struct per_thread_data *thread_data;
struct fi_info *fi, *hints;
struct fid_fabric *fab;
struct fid_domain *dom;
static int thread_safe = 1;

int myid, numprocs;

struct test_tunables {
	int threads;
};

static struct test_tunables tunables;
static pthread_mutex_t mutex;

void print_usage(void)
{
	if (!myid) {
		fprintf(stderr, "\n%s\n", TEST_DESC);
		fprintf(stderr, "\nOptions:\n");
		ct_print_opts_usage("-l <loops>", "number of loops to measure");
		ct_print_opts_usage("-s <skip>", "number of loops to skip");
		ct_print_opts_usage("-i <iterations>", "iterations per loop");
		ct_print_std_usage();
	}
}

static void cq_readerr(struct fid_cq *cq, const char *cq_str)
{
	struct fi_cq_err_entry cq_err;
	const char *err_str;
	int ret;

	ret = fi_cq_readerr(cq, &cq_err, 0);
	if (ret < 0) {
		ct_print_fi_error("fi_cq_readerr", ret);
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
static int wait_for_comp(struct fid_cq *cq, int num_completions)
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
				ct_print_fi_error("fi_cq_read", ret);
			}
			return ret;
		}
	}
	return 0;
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
		ct_print_fi_error("fi_cq_open", ret);
		goto err1;
	}

	/* Open completion queue for recv completions */
	ret = fi_cq_open(dom, &cq_attr, &ptd->rcq, NULL);
	if (ret) {
		ct_print_fi_error("fi_cq_open", ret);
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
		ct_print_fi_error("fi_av_open", ret);
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
	ret = fi_ep_bind(ptd->ep, &ptd->scq->fid, FI_TRANSMIT);
	if (ret) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	/* Bind Recv CQ with endpoint to collect recv completions */
	ret = fi_ep_bind(ptd->ep, &ptd->rcq->fid, FI_RECV);
	if (ret) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	/* Bind AV with the endpoint to map addresses */
	ret = fi_ep_bind(ptd->ep, &ptd->av->fid, 0);
	if (ret) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_enable(ptd->ep);
	if (ret) {
		ct_print_fi_error("fi_enable", ret);
		return ret;
	 }

	return ret;
}

static int init_fabric(void)
{
	int ret;
	uint64_t flags = 0;

	/* Get fabric info */
	ret = fi_getinfo(CT_FIVERSION, NULL, NULL, flags, hints, &fi);
	if (ret) {
		ct_print_fi_error("fi_getinfo", ret);
		return ret;
	}

	/* Open fabric */
	ret = fi_fabric(fi->fabric_attr, &fab, NULL);
	if (ret) {
		ct_print_fi_error("fi_fabric", ret);
		goto err1;
	}


	if (!thread_safe) {
		fi->domain_attr->threading = FI_THREAD_COMPLETION;
		fi->domain_attr->data_progress = FI_PROGRESS_MANUAL;
		fi->domain_attr->control_progress = FI_PROGRESS_MANUAL;
	}

	/* Open domain */
	ret = fi_domain(fab, fi, &dom, NULL);
	if (ret) {
		ct_print_fi_error("fi_domain", ret);
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
		ct_print_fi_error("fi_endpoint", ret);
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
		ct_print_fi_error("fi_getname", ret);
		return ret;
	}

	ptd->addrs = malloc(numprocs * addrlen);
	assert(ptd->addrs);

	ctpm_Allgather(addr, addrlen, ptd->addrs);

	ptd->fi_addrs = malloc(numprocs * sizeof(fi_addr_t));
	assert(ptd->fi_addrs);

	/* Insert address to the AV and get the fabric address back */
	ret = fi_av_insert(ptd->av, ptd->addrs, numprocs, ptd->fi_addrs, 0, &ptd->fi_ctx_av);
	if (ret != numprocs) {
		ct_print_fi_error("fi_av_insert", ret);
		return ret;
	}

	free(addr);

	return 0;
}

int init_per_thread_data(struct per_thread_data *ptd)
{
	int align_size;
	int ret;
	buf_desc_t lbuf_desc;

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

	ptd->s_buf = (char *) (((unsigned long) ptd->s_buf_original + (align_size - 1)) /
				align_size * align_size);
	ptd->r_buf = (char *) (((unsigned long) ptd->r_buf_original + (align_size - 1)) /
				align_size * align_size);

	ret = fi_mr_reg(dom, ptd->r_buf, MYBUFSIZE, FI_REMOTE_WRITE, 0, 0, 0, &ptd->r_mr, NULL);
	if (ret) {
		ct_print_fi_error("fi_mr_reg", ret);
		return -1;
	}

	lbuf_desc.addr = (uint64_t) ptd->r_buf;
	lbuf_desc.key = fi_mr_key(ptd->r_mr);

	ptd->rbuf_descs = (buf_desc_t *) malloc(numprocs * sizeof(buf_desc_t));

	/* Distribute memory keys */
	ctpm_Allgather(&lbuf_desc, sizeof(lbuf_desc), ptd->rbuf_descs);

	ret = fi_mr_reg(dom, ptd->s_buf, MYBUFSIZE, FI_WRITE, 0, 0, 0, &ptd->l_mr, NULL);
	if (ret) {
		ct_print_fi_error("fi_mr_reg", ret);
		return -1;
	}

	return 0;
}

int fini_per_thread_data(struct per_thread_data * ptd)
{
	assert(ptd != NULL);

	if (&ptd->l_mr->fid != NULL)
		fi_close(&ptd->l_mr->fid);

	if (&ptd->r_mr->fid != NULL)
		fi_close(&ptd->r_mr->fid);

	free_ep_res(ptd);

	fi_close(&ptd->ep->fid);

	return 0;
}

void *thread_fn(void *data)
{
	int i, j, peer;
	int size;
	ssize_t __attribute__((unused)) fi_rc;
	struct per_thread_data *ptd;
	struct per_iteration_data it;
	uint64_t t_start = 0, t_end = 0;

	it.data = data;
	size = it.message_size;

	if (it.thread_id >= tunables.threads)
		return (void *)-EINVAL;

	ptd = &thread_data[it.thread_id];
	ptd->bytes_sent = 0;

        ct_tbarrier(&ptd->tbar);

	if (myid == 0) {
		peer = 1;

		for (i = 0; i < loop + skip; i++) {
			if (i == skip) {  /* warm up loop */
				t_start = get_time_usec();
				ptd->bytes_sent = 0;
			}

			for (j = 0; j < window_size; j++) {
				fi_rc = fi_write(ptd->ep, ptd->s_buf, size, ptd->l_mr,
						ptd->fi_addrs[peer],
						ptd->rbuf_descs[peer].addr,
						ptd->rbuf_descs[peer].key,
						(void *)(intptr_t)j);
				assert(fi_rc==FI_SUCCESS);
				ptd->bytes_sent += size;
			}

			wait_for_comp(ptd->scq, window_size);
		}

		fi_rc = fi_send(ptd->ep, ptd->s_buf, 4, NULL,
				ptd->fi_addrs[peer],
				NULL);
		assert(!fi_rc);
		wait_for_comp(ptd->scq, 1);

		fi_rc = fi_recv(ptd->ep, ptd->s_buf, 4, NULL,
				ptd->fi_addrs[peer],
				NULL);
		assert(!fi_rc);
		wait_for_comp(ptd->rcq, 1);

		t_end = get_time_usec();
	} else if (myid == 1) {
		peer = 0;

		fi_rc = fi_recv(ptd->ep, ptd->s_buf, 4, NULL,
				ptd->fi_addrs[peer],
				NULL);
		assert(!fi_rc);
		wait_for_comp(ptd->rcq, 1);

		fi_rc = fi_send(ptd->ep, ptd->s_buf, 4, NULL,
				ptd->fi_addrs[peer],
				NULL);
		assert(!fi_rc);
		wait_for_comp(ptd->scq, 1);
	}

        ct_tbarrier(&ptd->tbar);

	ptd->latency = (t_end - t_start) / (double)(loop * window_size);
	ptd->time_start = t_start;
	ptd->time_end = t_end;

	return NULL;
}

int main(int argc, char *argv[])
{
	int op, ret;
	int i, j, size;
	struct per_iteration_data iter_key;
	struct per_thread_data *ptd;
	double min_lat, max_lat, sum_lat;
	uint64_t time_start, time_end;
	uint64_t bytes_sent;
	double mbps;

	pthread_mutex_init(&mutex, NULL);
	tunables.threads = 1;

	ctpm_Init(&argc, &argv);
	ctpm_Rank(&myid);
	ctpm_Job_size(&numprocs);

	hints = fi_allocinfo();
	if (!hints) {
		perror("malloc failed\n");
		return -1;
	}

	while ((op = getopt(argc, argv, "hmt:i:l:s:" CT_STD_OPTS)) != -1) {
		switch (op) {
		default:
			ct_parse_std_opts(op, optarg, hints);
			break;
		case 'l':  // loops
			loop = atoi(optarg);
			if (loop <= 0) {
				print_usage();
				return EXIT_FAILURE;
			}

			loop_large = loop / 5;
			if (loop_large == 0)
				loop_large = 1;
			break;
		case 'm':
			thread_safe = 0;
			break;
		case 's':  // skips
			skip = atoi(optarg);
			if (skip <= 0) {
				print_usage();
				return EXIT_FAILURE;
			}

			skip_large = skip / 5;
			if (skip_large == 0)
				skip_large = 1;
			break;
		case 't':
			tunables.threads = atoi(optarg);
			if (tunables.threads <= 0) {
				print_usage();
				return EXIT_FAILURE;
			}
			break;
		case 'i':  // window size or iterations per loop
			window_size = atoi(optarg);
			if (window_size <= 0) {
				print_usage();
				return EXIT_FAILURE;
			}
			window_size_large = window_size;
			break;
		case '?':
		case 'h':
			print_usage();
			return EXIT_FAILURE;
		}
	}

	pthread_barrier_init(&thread_barrier, NULL, tunables.threads);

	hints->ep_attr->type	= FI_EP_RDM;
	hints->caps		= FI_MSG | FI_DIRECTED_RECV | FI_RMA;
	hints->mode		= FI_CONTEXT | FI_LOCAL_MR;
	hints->domain_attr->mr_mode = FI_MR_BASIC;

	if (numprocs != 2) {
		if (myid == 0) {
			fprintf(stderr, "This test requires exactly two processes\n");
		}
		ctpm_Finalize();
		return -1;
	}

	/* Fabric initialization */
	ret = init_fabric();
	if (ret) {
		fprintf(stderr, "Problem in fabric initialization\n");
		return ret;
	}

	if (myid == 0)
		printf("%i threads\n", tunables.threads);
	thread_data = calloc(tunables.threads, sizeof(struct per_thread_data));
	if (!thread_data) {
		fprintf(stderr, "Could not allocate memory for per thread struct\n");
		return -1;
	}

	for (i = 0; i < tunables.threads; i++) {
		init_per_thread_data(&thread_data[i]);
		ct_tbarrier_init(&thread_data[i].tbar, tunables.threads,
				 tbar_counter, tbar_signal);
	}

	if (myid == 0) {
		fprintf(stdout, HEADER);
		fprintf(stdout, "%-*s%*s%*s%*s%*s\n", 10, "# Size",
			FIELD_WIDTH, "Bandwidth (MB/s)",
			FIELD_WIDTH, "Latency (us)",
			FIELD_WIDTH, "Min Lat (us)",
			FIELD_WIDTH, "Max Lat (us)");
		fflush(stdout);
	}

	/* Bandwidth test */
	for (size = 1; size <= MAX_MSG_SIZE; size *= 2) {
		/* reset data per thread */
		for (i = 0; i < tunables.threads; i++) {
			ptd = &thread_data[i];

			/* touch the data */
			for (j = 0; j < size; j++) {
				ptd->s_buf[j] = 'a';
				ptd->r_buf[j] = 'b';
			}

			if (size > large_message_size) {
				loop = loop_large;
				skip = skip_large;
				window_size = window_size_large;
			}
		}

		iter_key.message_size = size;

		ctpm_Barrier();

		/* threaded section */
		for (i = 0; i < tunables.threads; i++) {
			iter_key.thread_id = i;
			ret = pthread_create(&thread_data[i].thread, NULL,
					thread_fn, iter_key.data);
			if (ret != 0) {
				printf("couldn't create thread %i\n", i);
				pthread_exit(NULL); /* a more robust exit would be nice here */
			}
		}

		for (i = 0; i < tunables.threads; i++)
			pthread_join(thread_data[i].thread, NULL);

		ctpm_Barrier();

		if (myid == 0) {
			min_lat = max_lat = sum_lat = thread_data[0].latency;
			bytes_sent = thread_data[0].bytes_sent;
			time_start = thread_data[0].time_start;
			time_end = thread_data[0].time_end;

			for (i = 1; i < tunables.threads; i++) {
				if (thread_data[i].latency < min_lat) {
					min_lat = thread_data[i].latency;
				} else if (thread_data[i].latency > max_lat) {
					max_lat = thread_data[i].latency;
				}
				sum_lat += thread_data[i].latency;

				bytes_sent += thread_data[i].bytes_sent;

				if (thread_data[i].time_start < time_start)
					time_start = thread_data[i].time_start;

				if (thread_data[i].time_end > time_end)
					time_end = thread_data[i].time_end;
			}

			mbps = ((bytes_sent * 1.0) / (1024. * 1024.)) / ((time_end - time_start) / (1.0 * 1e6));

			fprintf(stdout, "%-*d%*.*f%*.*f%*.*f%*.*f\n", 10, size,
				FIELD_WIDTH, FLOAT_PRECISION, mbps,
				FIELD_WIDTH, FLOAT_PRECISION,
				sum_lat / tunables.threads,
				FIELD_WIDTH, FLOAT_PRECISION, min_lat,
				FIELD_WIDTH, FLOAT_PRECISION, max_lat);
			fflush(stdout);
		}

	}

	for (i = 0; i < tunables.threads; i++) {
		fini_per_thread_data(&thread_data[i]);
	}

	/* end of threaded section */
	fi_close(&dom->fid);
	fi_close(&fab->fid);

	fi_freeinfo(hints);
	fi_freeinfo(fi);

	ctpm_Barrier();
	ctpm_Finalize();

	pthread_exit(NULL);
}

/* vi:set sw=8 sts=8 */

