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
#include <sys/time.h>
#include <string.h>
#include <sys/uio.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>

#include "ct_utils.h"

#define DEFAULT_WINDOW       (64)

#define IOV_CNT		     8 	/* Note this is hard coded to the current gni
				 * max iov limit */
#define ITERS_SMALL          (100)
#define WARMUP_ITERS_SMALL   (10)
#define ITERS_LARGE          (20)
#define WARMUP_ITERS_LARGE   (2)
#define LARGE_THRESHOLD      (8192)

#define WINDOW_SIZES {8, 16, 32, 64, 128}
#define WINDOW_SIZES_COUNT   (5)

#define MAX_MSG_SIZE         (1<<22)
#define MAX_ALIGNMENT        (65536)

#define TEST_DESC "Libfabric Multiple Bandwidth and Message Rate Test"
#define HEADER "# " TEST_DESC " \n"
#define SEND_RECV_DESC   "# fi_send  -> fi_recv\n"
#define SENDV_RECV_DESC  "# fi_sendv -> fi_recv  (Scatter/Gather)\n"
#define SEND_RECVV_DESC  "# fi_send  -> fi_recvv (Scatter)\n"
#define SENDV_RECVV_DESC "# fi_sendv -> fi_recvv (Scatter/Scatter)\n"
#ifndef FIELD_WIDTH
#   define FIELD_WIDTH 20
#endif
#ifndef FLOAT_PRECISION
#   define FLOAT_PRECISION 2
#endif

enum send_recv_type_e {
	SEND_RECV, SENDV_RECVV, SEND_RECVV, SENDV_RECV, FIN
};

int loop = 100;
int window_size = 64;
int skip = 10;

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
	if (!myid) {
		fprintf(stderr, "\n%s\n", TEST_DESC);
		fprintf(stderr, "\nOptions:\n");
		ct_print_opts_usage("-r <0,1> ", "Print uni-directional message rate (default 1)");
		ct_print_opts_usage("-p <pairs>", "Number of pairs involved (default np / 2)");
		ct_print_opts_usage("-w <window>", "Number of messages sent before "
				    "acknowldgement (64, 10) [cannot be used with -v]");
		ct_print_opts_usage("-v", "Vary the window size (default no) "
				    "[cannot be used with -w]");
		ct_print_std_usage();
	}
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
		ct_print_fi_error("fi_cq_open", ret);
		goto err1;
	}

	/* Open completion queue for recv completions */
	ret = fi_cq_open(dom, &cq_attr, &rcq, NULL);
	if (ret) {
		ct_print_fi_error("fi_cq_open", ret);
		goto err2;
	}

	memset(&av_attr, 0, sizeof(av_attr));
	av_attr.type = fi->domain_attr->av_type ?
			fi->domain_attr->av_type : FI_AV_TABLE;
	av_attr.count = numprocs;
	av_attr.name = NULL;

	/* Open address vector (AV) for mapping address */
	ret = fi_av_open(dom, &av_attr, &av, NULL);
	if (ret) {
		ct_print_fi_error("fi_av_open", ret);
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
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	/* Bind Recv CQ with endpoint to collect recv completions */
	ret = fi_ep_bind(ep, &rcq->fid, FI_RECV);
	if (ret) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	/* Bind AV with the endpoint to map addresses */
	ret = fi_ep_bind(ep, &av->fid, 0);
	if (ret) {
		ct_print_fi_error("fi_ep_bind", ret);
		return ret;
	}

	ret = fi_enable(ep);
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

	/* Open domain */
	ret = fi_domain(fab, fi, &dom, NULL);
	if (ret) {
		ct_print_fi_error("fi_domain", ret);
		goto err2;
	}

	/* Open endpoint */
	ret = fi_endpoint(dom, fi, &ep, NULL);
	if (ret) {
		ct_print_fi_error("fi_endpoint", ret);
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
		ct_print_fi_error("fi_getname", ret);
		return ret;
	}

	addrs = malloc(numprocs * addrlen);
	assert(addrs);

	ctpm_Allgather(addr, addrlen, addrs);

	fi_addrs = malloc(numprocs * sizeof(fi_addr_t));
	assert(fi_addrs);

	/* Insert address to the AV and get the fabric address back */
	ret = fi_av_insert(av, addrs, numprocs, fi_addrs, 0, &fi_ctx_av);
	if (ret != numprocs) {
		ct_print_fi_error("fi_av_insert", ret);
		return ret;
	}

	free(addr);

	return 0;
}

double calc_bw(int rank, int num_pairs, int window_size, struct iovec *s_iov,
	       struct iovec *r_iov, int iov_cnt, char *s_buf, int s_buf_len,
	       char *r_buf, int r_buf_len, enum send_recv_type_e type)
{
	uint64_t t_start = 0, t_end = 0, t = 0, maxtime = 0, *ts;
	double bw = 0;
	int c, i, j, target;
	int loop, skip;
	size_t cum_size = 0;
	int mult = (DEFAULT_WINDOW / window_size) > 0 ? (DEFAULT_WINDOW /
			window_size) : 1;
	int __attribute__((unused)) fi_rc;
	char r_fin[4] = {'b', 'b', 'b', 'b'};
	char s_fin[4] = {'a', 'a', 'a', 'a'};

	for (c = 0; c < iov_cnt; c++) {
		for (i = 0; i < s_iov[c].iov_len; i++) {
			((char *) s_iov[c].iov_base)[i] = 'a';
		}

		for (i = 0; i < r_iov[c].iov_len; i++) {
			((char *) r_iov[c].iov_base)[i] = 'b';
		}

		/* Size will be all the receiver can hold */
		cum_size += r_iov[c].iov_len;
	}

	for (c = 0; c < s_buf_len; c++) {
		s_buf[c]= 'a';
	}

	for (c = 0; c < r_buf_len; c++) {
		r_buf[c]= 'b';
	}

	if (cum_size > LARGE_THRESHOLD) {
		loop = ITERS_LARGE * mult;
		skip = WARMUP_ITERS_LARGE * mult;
	} else {
		loop = ITERS_SMALL * mult;
		skip = WARMUP_ITERS_SMALL * mult;
	}

	ctpm_Barrier();

	if (rank < num_pairs) {
		target = rank + num_pairs;

		for (i = 0; i < loop + skip; i++) {
			if (i == skip) {
				ctpm_Barrier();
				t_start = get_time_usec();
			}

			for (j = 0; j < window_size; j++) {
				switch (type) {
				case SEND_RECV:
					for (c = 0; c < iov_cnt; c++) {
						fi_rc = fi_send(ep, s_iov[c].iov_base,
								s_iov[c].iov_len, NULL,
								fi_addrs[target],
								NULL);
						assert(!fi_rc);
					}
					break;
				case SENDV_RECVV:
				case SENDV_RECV:
					fi_rc = fi_sendv(ep, s_iov, (void **) NULL,
							 iov_cnt, fi_addrs[target],
							 NULL);
					assert(!fi_rc);
					break;
				case SEND_RECVV:
					fi_rc = fi_send(ep, s_buf,
							s_buf_len, NULL,
							fi_addrs[target],
							NULL);
					assert(!fi_rc);
					break;
				default:
					abort();
				}
			}

			wait_for_comp(scq, type == SEND_RECV ? window_size * iov_cnt : window_size);
			fi_rc = fi_recv(ep, r_fin, 4, NULL,
					fi_addrs[target], NULL);
			assert(!fi_rc);
			wait_for_comp(rcq, 1);
		}

		t_end = get_time_usec();
		t = t_end - t_start;
	} else if (rank < num_pairs * 2) {
		target = rank - num_pairs;

		for (i = 0; i < loop + skip; i++) {
			if (i == skip) {
				ctpm_Barrier();
			}

			for (j = 0; j < window_size; j++) {
				switch (type) {
				case SEND_RECV:
					for (c = 0; c < iov_cnt; c++) {
						fi_rc = fi_recv(ep, r_iov[c].iov_base,
								r_iov[c].iov_len, NULL,
								fi_addrs[target],
								NULL);
						assert(!fi_rc);
					}
					break;
				case SENDV_RECVV:
				case SEND_RECVV:

					fi_rc = fi_recvv(ep, r_iov, (void **) NULL, iov_cnt,
							 fi_addrs[target], NULL);
					assert(!fi_rc);
					break;
				case SENDV_RECV:
					fi_rc = fi_recv(ep, r_buf,
							r_buf_len, NULL,
							fi_addrs[target],
							NULL);
					assert(!fi_rc);
					break;
				default:
					abort();
				}
			}

			wait_for_comp(rcq, type == 0 ? window_size * iov_cnt : window_size);
			fi_rc = fi_send(ep, s_fin, 4, NULL,
					fi_addrs[target], NULL);
			assert(!fi_rc);
			wait_for_comp(scq, 1);
		}
	} else {
		ctpm_Barrier();
	}

	ts = malloc(sizeof(t) * numprocs);
	assert(ts);
	ctpm_Allgather(&t, sizeof(t), ts);
	if (!myid) {
		for (i = 0; i < numprocs; i++) {
			if (ts[i] > maxtime) {
				maxtime = ts[i];
			}
		}
	}
	free(ts);

	if (rank == 0) {
		double tmp = num_pairs * cum_size / 1e6;

		tmp = tmp * loop * window_size;
		bw = tmp / (maxtime / 1e6);

		return bw;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int op, ret;

	struct iovec s_iov[IOV_CNT], r_iov[IOV_CNT];
	char *s_buf, *r_buf;
	int align_size;
	int pairs, print_rate;
	int window_varied;
	int c, j;
	int curr_size;
	enum send_recv_type_e type;

	ctpm_Init(&argc, &argv);
	ctpm_Rank(&myid);
	ctpm_Job_size(&numprocs);

	/* default values */
	pairs            = numprocs / 2;
	window_size      = DEFAULT_WINDOW;
	window_varied    = 0;
	print_rate       = 1;

	hints = fi_allocinfo();
	if (!hints)
		return -1;

	while ((op = getopt(argc, argv, "hp:w:vr:" CT_STD_OPTS)) != -1) {
		switch (op) {
		default:
			ct_parse_std_opts(op, optarg, hints);
			break;
		case 'p':
			pairs = atoi(optarg);
			if (pairs > (numprocs / 2)) {
				print_usage();
				return EXIT_FAILURE;
			}
			break;
		case 'w':
			window_size = atoi(optarg);
			break;
		case 'v':
			window_varied = 1;
			break;
		case 'r':
			print_rate = atoi(optarg);
			if (0 != print_rate && 1 != print_rate) {
				print_usage();
				return EXIT_FAILURE;
			}
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

	if (numprocs < 2) {
		if (!myid) {
			fprintf(stderr, "This test requires at least two processes\n");
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

	ret = init_av();
	if (ret) {
		fprintf(stderr, "Problem in AV initialization\n");
		return ret;
	}

	/* Data initialization */
	align_size = getpagesize();
	assert(align_size <= MAX_ALIGNMENT);

	/* Allocate page aligned buffers */
	for (c = 0; c < IOV_CNT; c++) {
		assert(!posix_memalign(&s_iov[c].iov_base, align_size, MAX_MSG_SIZE));
		assert(!posix_memalign(&r_iov[c].iov_base, align_size, MAX_MSG_SIZE));
	}

	assert(!posix_memalign((void **)&s_buf, align_size, MAX_MSG_SIZE * IOV_CNT));
	assert(!posix_memalign((void **)&r_buf, align_size, MAX_MSG_SIZE * IOV_CNT));

	for (type = 0; type < FIN; type++) {
		if (!myid) {
			fprintf(stdout, HEADER);
			switch (type) {
			case SEND_RECV:
				fprintf(stdout, SEND_RECV_DESC);
				break;
			case SENDV_RECVV:
				fprintf(stdout, SENDV_RECVV_DESC);
				break;
			case SEND_RECVV:
				fprintf(stdout, SEND_RECVV_DESC);
				break;
			case SENDV_RECV:
				fprintf(stdout, SENDV_RECV_DESC);
				break;
			default:
				abort();
			}

			if (window_varied) {
				fprintf(stdout, "# [ pairs: %d ] [ window size: varied ]\n", pairs);
				fprintf(stdout, "\n# Uni-directional Bandwidth (MB/sec)\n");
			} else {
				fprintf(stdout, "# [ pairs: %d ] [ window size: %d ]\n", pairs,
					window_size);
				if (print_rate) {
					fprintf(stdout, "%-*s%*s%*s%*s\n", 10, "# Size", FIELD_WIDTH,
						"Iov count", FIELD_WIDTH, "MB/s", FIELD_WIDTH, "Messages/s");
				} else {
					fprintf(stdout, "%-*s%*s%*s\n", 10, "# Size", FIELD_WIDTH,
						"Iov count", FIELD_WIDTH, "MB/s");
				}
			}
			fflush(stdout);
		}

		if (window_varied) {
			int window_array[] = WINDOW_SIZES;
			double **bandwidth_results;
			int log_val = 1, tmp_message_size = MAX_MSG_SIZE;
			int i, j;

			for (i = 0; i < WINDOW_SIZES_COUNT; i++) {
				if (window_array[i] > window_size) {
					window_size = window_array[i];
				}
			}

			while (tmp_message_size >>= 1) {
				log_val++;
			}

			bandwidth_results = (double **)malloc(sizeof(double *) * log_val);

			for (i = 0; i < log_val; i++) {
				bandwidth_results[i] = (double *)malloc(sizeof(double) *
									WINDOW_SIZES_COUNT);
			}

			if (!myid) {
				fprintf(stdout, "#      ");

				for (i = 0; i < WINDOW_SIZES_COUNT; i++) {
					fprintf(stdout, "  %10d", window_array[i]);
				}

				fprintf(stdout, "\n");
				fflush(stdout);
			}

			for (j = 0, curr_size = 1; curr_size <= MAX_MSG_SIZE; curr_size *= 2, j++) {
				if (!myid) {
					fprintf(stdout, "%-7d", curr_size);
				}

				for (i = 0; i < WINDOW_SIZES_COUNT; i++) {
					for (c = 0; c < IOV_CNT; c++) {
						r_iov[c].iov_len = s_iov[c].iov_len = curr_size;
						bandwidth_results[j][i] = calc_bw(myid, pairs,
										  window_array[i], s_iov, r_iov, c + 1,
										  s_buf, (c + 1) * curr_size, r_buf,
										  (c + 1) * curr_size, type);

						if (!myid) {
							fprintf(stdout, "%*d  %10.*f", FIELD_WIDTH, c + 1,
								FLOAT_PRECISION,
								bandwidth_results[j][i]);
						}

						fprintf(stdout, c == IOV_CNT - 1 ? "\n" : "");
					}
				}

				if (!myid) {
					fprintf(stdout, "\n");
					fflush(stdout);
				}
			}

			if (!myid && print_rate) {
				fprintf(stdout, "\n# Message Rate Profile\n");
				fprintf(stdout, "#      ");

				for (i = 0; i < WINDOW_SIZES_COUNT; i++) {
					fprintf(stdout, "  %10d", window_array[i]);
				}

				fprintf(stdout, "\n");
				fflush(stdout);

				for (c = 0; c < IOV_CNT; c++) {
					for (j = 0, curr_size = 1; curr_size <= MAX_MSG_SIZE; curr_size *= 2) {
						fprintf(stdout, "%-7d,%*d", curr_size * (c + 1), FIELD_WIDTH, c + 1);

						for (i = 0; i < WINDOW_SIZES_COUNT; i++) {
							double rate = 1e6 * bandwidth_results[j][i] / (curr_size * (c + 1));

							fprintf(stdout, "  %10.2f", rate);
						}

						fprintf(stdout, "\n");
						fflush(stdout);
						j++;
					}
				}
			}
		} else {
			/* Just one window size */
			for (curr_size = 1; curr_size <= MAX_MSG_SIZE; curr_size *= 2) {
				double bw, rate;

				for (c = 0; c < IOV_CNT; c++) {
					r_iov[c].iov_len = s_iov[c].iov_len = curr_size;
					bw = calc_bw(myid, pairs, window_size, s_iov, r_iov, c + 1,
						     s_buf, (c + 1) * curr_size, r_buf,
						     (c + 1) * curr_size, type);

					if (!myid) {
						rate = 1e6 * bw / (curr_size * (c + 1));

						if (print_rate) {
							fprintf(stdout, "%-*d%*d%*.*f%*.*f\n", 10, curr_size * (c + 1),
								FIELD_WIDTH, c + 1, FIELD_WIDTH,
								FLOAT_PRECISION, bw, FIELD_WIDTH,
								FLOAT_PRECISION, rate);
							fflush(stdout);
						} else {
							fprintf(stdout, "%-*d%*d%*.*f\n", 10, curr_size * (c + 1), FIELD_WIDTH,
								FIELD_WIDTH, c + 1, FLOAT_PRECISION, bw);
							fflush(stdout);
						}
					}
					fprintf(stdout, c == IOV_CNT - 1 ? "\n" : "");
				}
			}
		}
	}

	ctpm_Barrier();

	free_ep_res();

	fi_close(&ep->fid);
	fi_close(&dom->fid);
	fi_close(&fab->fid);

	fi_freeinfo(hints);
	fi_freeinfo(fi);

	ctpm_Barrier();
	ctpm_Finalize();

	for (c = 0; c < IOV_CNT; c++) {
		free(r_iov[c].iov_base);
		free(s_iov[c].iov_base);
	}

	return 0;
}

/* vi:set sw=8 sts=8 */
