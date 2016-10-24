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

/*
 * These comments and  code were derived from the DARPA HPCS program.  Contact
 * David Koester <dkoester@mitre.org> or Bob Lucas <rflucas@isi.edu>
 * if you have questions.
 *
 * GUPS (Giga UPdates per Second) is a measurement that profiles the memory
 * architecture of a system and is a measure of performance similar to MFLOPS.
 * The HPCS HPCchallenge RandomAccess benchmark is intended to exercise the
 * GUPS capability of a system, much like the LINPACK benchmark is intended to
 * exercise the MFLOPS capability of a computer.  In each case, we would
 * expect these benchmarks to achieve close to the "peak" capability of the
 * memory system. The extent of the similarities between RandomAccess and
 * LINPACK are limited to both benchmarks attempting to calculate a peak system
 * capability.
 *
 * GUPS is calculated by identifying the number of memory locations that can be
 * randomly updated in one second, divided by 1 billion (1e9). The term "randomly"
 * means that there is little relationship between one address to be updated and
 * the next, except that they occur in the space of one half the total system
 * memory.  An update is a read-modify-write operation on a table of 64-bit words.
 * An address is generated, the value at that address read from memory, modified
 * by an integer operation (add, and, or, xor) with a literal value, and that
 * new value is written back to memory.
 *
 * We are interested in knowing the GUPS performance of both entire systems and
 * system subcomponents --- e.g., the GUPS rating of a distributed memory
 * multiprocessor the GUPS rating of an SMP node, and the GUPS rating of a
 * single processor.  While there is typically a scaling of FLOPS with processor
 * count, a similar phenomenon may not always occur for GUPS.
 *
 * Select the memory size to be the power of two such that 2^n <= 1/2 of the
 * total memory.  Each CPU operates on its own address stream, and the single
 * table may be distributed among nodes. The distribution of memory to nodes
 * is left to the implementer.  A uniform data distribution may help balance
 * the workload, while non-uniform data distributions may simplify the
 * calculations that identify processor location by eliminating the requirement
 * for integer divides. A small (less than 1%) percentage of missed updates
 * are permitted.
 *
 * When implementing a benchmark that measures GUPS on a distributed memory
 * multiprocessor system, it may be required to define constraints as to how
 * far in the random address stream each node is permitted to "look ahead".
 * Likewise, it may be required to define a constraint as to the number of
 * update messages that can be stored before processing to permit multi-level
 * parallelism for those systems that support such a paradigm.  The limits on
 * "look ahead" and "stored updates" are being implemented to assure that the
 * benchmark meets the intent to profile memory architecture and not induce
 * significant artificial data locality. For the purpose of measuring GUPS,
 * we will stipulate that each thread is permitted to look ahead no more than
 * 1024 random address stream samples with the same number of update messages
 * stored before processing.
 *
 * The supplied MPI-1 code generates the input stream {A} on all processors
 * and the global table has been distributed as uniformly as possible to
 * balance the workload and minimize any Amdahl fraction.  This code does not
 * exploit "look-ahead".  Addresses are sent to the appropriate processor
 * where the table entry resides as soon as each address is calculated.
 * Updates are performed as addresses are received.  Each message is limited
 * to a single 64 bit long integer containing element ai from {A}.
 * Local offsets for T[ ] are extracted by the destination processor.
 *
 * If the number of processors is equal to a power of two, then the global
 * table can be distributed equally over the processors.  In addition, the
 * processor number can be determined from that portion of the input stream
 * that identifies the address into the global table by masking off log2(p)
 * bits in the address.
 *
 * If the number of processors is not equal to a power of two, then the global
 * table cannot be equally distributed between processors.  In the MPI-1
 * implementation provided, there has been an attempt to minimize the differences
 * in workloads and the largest difference in elements of T[ ] is one.  The
 * number of values in the input stream generated by each processor will be
 * related to the number of global table entries on each processor.
 *
 * The MPI-1 version of RandomAccess treats the potential instance where the
 * number of processors is a power of two as a special case, because of the
 * significant simplifications possible because processor location and local
 * offset can be determined by applying masks to the input stream values.
 * The non power of two case uses an integer division to determine the processor
 * location.  The integer division will be more costly in terms of machine
 * cycles to perform than the bit masking operations
 *
 * For additional information on the GUPS metric, the HPCchallenge RandomAccess
 * Benchmark,and the rules to run RandomAccess or modify it to optimize
 * performance -- see http://icl.cs.utk.edu/hpcc/
 */


#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdint.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_atomic.h>

#include "ct_utils.h"

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
#define MAX_ALIGNMENT 65536
/* #define MAX_MSG_SIZE (1<<22) */
#define MAX_MSG_SIZE (8*1024) /* Current GNI provider max send size */
#define MYBUFSIZE (MAX_MSG_SIZE + MAX_ALIGNMENT)

#define TEST_DESC "Libfabric Random Access Test"
#define HEADER "# " TEST_DESC " \n"
#ifndef FIELD_WIDTH
#   define FIELD_WIDTH 20
#endif
#ifndef FLOAT_PRECISION
#   define FLOAT_PRECISION 2
#endif

/* Random number generator */
#define POLY 0x0000000000000007UL
#define PERIOD 1317624576693539401L

/* Define 64-bit types and corresponding format strings
 * for printf() and constants
 */
#define FSTR64 "%ld"
#define FSTRU64 "%lu"
#define ZERO64B 0LL
#define HEAD_RANK (rank == 0)
#define UNLOCKED 0
#define LOCKED(rank) (rank + 1)

#define CTPM_SIMPLE_TYPE_FUNC(LABEL, type, op) \
		static type ctpm_##LABEL (void *src, int total_ranks) \
		 \
		{ \
			type *vals; \
			int i; \
			type ret; \
			\
			vals = calloc(total_ranks, sizeof(type)); \
			assert(vals); \
			\
			ctpm_Allgather(src, sizeof(type), vals); \
			\
			ret = vals[0]; \
			for (i = 1; i < total_ranks; i++) \
				ret = ret op vals[i]; \
			\
			ctpm_Barrier(); \
			\
			free(vals); \
			\
			return ret; \
		}

#define SCRATCH_SIZE 8

/* global variables */
double gups;
double errors_fraction;
double check_time;
int failure;
uint32_t scratch[SCRATCH_SIZE];

/* Allocate main table (in global memory) */
uint64_t *hpcc_table;
uint64_t *hpcc_lock;

static uint64_t global_start_my_proc;

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
	struct fid_mr *l_lock_mr;
	struct fid_mr *l_scratch_mr;
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
	local_lock_key = fi_mr_key(data->l_lock_mr);

	DEBUG("Exchanging registration data\n");

	ctpm_Barrier();

	DEBUG("Allgather 1\n");
	/* first, table addr */
	ctpm_Allgather(&local_table_key, sizeof(local_table_key), keys);

	for (i = 0; i < num_ranks; i++)
		peers[i].data.r_table_key = keys[i];

	ctpm_Barrier();

	DEBUG("Allgather 2\n");
	ctpm_Allgather(&local_lock_key, sizeof(local_lock_key), keys);

	for (i = 0; i < num_ranks; i++)
		peers[i].data.r_lock_key = keys[i];

	ctpm_Barrier();

	DEBUG("Allgather 3\n");

	DEBUG("Sending table %p, size=%d, dest=%p\n", hpcc_table, sizeof(void *),
			addresses);
	ctpm_Allgather(&hpcc_table, sizeof(void *), addresses);
	ctpm_Barrier();

	for (i = 0; i < num_ranks; i++)
		peers[i].data.r_table_addr = addresses[i];

	ctpm_Barrier();

	DEBUG("Allgather 4\n");DEBUG("Sending lock %p, size=%d, dest=%p\n",
			hpcc_lock, sizeof(void *), addresses);
	ctpm_Allgather(&hpcc_lock, sizeof(void *), addresses);

	for (i = 0; i < num_ranks; i++)
		peers[i].data.r_lock_addr = addresses[i];

	ctpm_Barrier();
	DEBUG("Finished exchanging data\n");
}

/* function declarations */
CTPM_SIMPLE_TYPE_FUNC(int_sum_all, int, +);
CTPM_SIMPLE_TYPE_FUNC(int64_sum_all, int64_t, +);

static inline void cq_readerr(struct fid_cq *cq, const char *cq_str)
{
	struct fi_cq_err_entry cq_err;
	const char *err_str;
	int ret;

	ret = fi_cq_readerr(cq, &cq_err, 0);
	if (unlikely(ret < 0)) {
		ct_print_fi_error("fi_cq_readerr", ret);
		return;
	}

	err_str = fi_cq_strerror(cq, cq_err.prov_errno, cq_err.err_data, NULL, 0);
	ERROR("%s: %d %s\n", cq_str, cq_err.err, fi_strerror(cq_err.err));
	ERROR("%s: prov_err: %s (%d)\n", cq_str, err_str, cq_err.prov_errno);
}

/*
 * fi_cq_err_entry can be cast to any CQ entry format.
 */
static inline int wait_for_comp(struct fid_cq *cq, int num_completions)
{
	struct fi_cq_err_entry comp;
	int ret;

	while (num_completions > 0) {
		ret = fi_cq_read(cq, &comp, 1);
		if (ret > 0) {
			num_completions--;
		} else if (unlikely(ret < 0 && ret != -FI_EAGAIN)) {
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

static inline void lf_atomic_xor(void *addr, uint64_t val, int remote_rank)
{
	ssize_t rc;
	struct peer *p = &prov.peers[remote_rank];
	struct fid_ep *ep = p->ep;
	struct fid_cq *scq = p->scq;
	fi_addr_t dest_addr = prov.fi_addrs[remote_rank];
	uint64_t rem_addr, key;
	void *tmp = &scratch[0];
	app_data_t *data = &prov.data;

	// assume same sized tables with identical offsets
	rem_addr = (uint64_t)(p->data.r_table_addr);
	rem_addr += ((uint64_t)addr) - ((uint64_t)hpcc_table);
	key = p->data.r_table_key;
	*(uint64_t *)tmp = val;

	rc = fi_atomic(ep, tmp, 1, fi_mr_desc(data->l_scratch_mr), dest_addr,
			rem_addr, key, FI_UINT64, FI_BXOR, NULL);

	ASSERT_MSG(!rc, "failed to xor a value, rc=%d", rc);

	wait_for_comp(scq, 1);
}

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
	ret = fi_getinfo(CT_FIVERSION, NULL, NULL, flags, fabric->hints,
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

static int64_t generate_initial_xor_val(uint64_t seed)
{
	/* int64_t i, j; */
	int i, j;
	uint64_t m2[64];
	uint64_t temp, ret;

	/* This section of code is legacy code from when seed used to be a
	 * signed int
	 while (n < 0)
	 n += PERIOD;
	 */
	while (seed > PERIOD)
		seed -= PERIOD;
	if (seed == 0)
		return 0x1;

	temp = 0x1;
	for (i = 0; i < 64; i++) {
		m2[i] = temp;
		temp = (temp << 1) ^ ((int64_t)temp < 0 ? POLY : 0);
		temp = (temp << 1) ^ ((int64_t)temp < 0 ? POLY : 0);
	}

	for (i = 62; i >= 0; i--)
		if ((seed >> i) & 1)
			break;

	ret = 0x2;

	while (i > 0) {
		temp = 0;
		for (j = 0; j < 64; j++)
			if ((ret >> j) & 1)
				temp ^= m2[j];
		ret = temp;
		i -= 1;
		if ((seed >> i) & 1)
			ret = (ret << 1) ^ ((int64_t)ret < 0 ? POLY : 0);
	}

	return ret;
}

static void update_table(uint64_t *table, uint64_t table_size,
		uint64_t min_local_table_size, uint64_t top, int remainder,
		int64_t iterations)
{
	uint64_t i;
	int index;
	uint64_t ran, remote_val, global_offset;
	int remote_pe;
	int global_start_at_pe;

	ctpm_Barrier();

	ran = saved_initial_ran_val;

	for (i = 0; i < iterations; i++) {
		ran = (ran << 1) ^ ((int64_t)ran < ZERO64B ? POLY : ZERO64B);
		global_offset = ran & (table_size - 1);
		if (global_offset < top) {
			remote_pe = global_offset / (min_local_table_size + 1);
			global_start_at_pe = (min_local_table_size + 1) * remote_pe;
		} else {
			remote_pe = (global_offset - remainder) / min_local_table_size;
			global_start_at_pe = min_local_table_size * remote_pe + remainder;
		}
		index = global_offset - global_start_at_pe;

		lf_atomic_xor((void *)&table[index], ran, remote_pe);
	}

	ctpm_Barrier();
}

static int random_access(void)
{
	int64_t i;
	static int64_t num_errors, global_num_errors;
	int remainder; /* Number of processors with (LocalTableSize + 1) entries */
	uint64_t num_top_table_entries; /* Number of table entries in top of Table */
	int64_t local_table_size; /* Local table width */
	uint64_t min_local_table_size; /* Integer ratio TableSize/num_ranks */
	uint64_t log_table_size;
	uint64_t table_size;
	double real_time; /* Real time to update table */
	double total_mem;
	static int send_abort, recv_abort;
	uint64_t default_num_updates; /* Number of updates to table
	 (suggested: 4x number of table entries) */
	uint64_t actual_num_updates; /* actual number of updates to table - may be
	 smaller than NumUpdates_Default due to
	 execution time bounds */
	int64_t proc_num_updates; /* number of updates per processor */
	FILE *outFile = NULL;
	double MyGUPS;
	double *local_gups_score;
	int rc;

	local_gups_score = &MyGUPS;

	if (HEAD_RANK) {
		outFile = stdout;
		setbuf(outFile, NULL);
	}

	total_mem = tunables.total_mem_opt ? tunables.total_mem_opt : 200000; /* max single node memory */
	total_mem *= num_ranks; /* max memory in num_ranks nodes */

	total_mem /= sizeof(uint64_t);

	/* calculate TableSize --- the size of update array (must be a power of 2) */
	for (total_mem *= 0.5, log_table_size = 0, table_size = 1; total_mem >= 1.0;
			total_mem *= 0.5, log_table_size++, table_size <<= 1)
		; /* EMPTY */

	/*
	 * Calculate local table size, etc.
	 */
	min_local_table_size = table_size / num_ranks;

	/* Number of processors with (LocalTableSize + 1) entries */
	remainder = table_size - (min_local_table_size * num_ranks);

	/* Number of table entries in top of Table */
	num_top_table_entries = (min_local_table_size + 1) * remainder;

	/* Local table size */
	if (rank < remainder) {
		local_table_size = min_local_table_size + 1;
		global_start_my_proc = ((min_local_table_size + 1) * rank);
	} else {
		local_table_size = min_local_table_size;
		global_start_my_proc = ((min_local_table_size * rank) + remainder);
	}

	send_abort = 0;
	hpcc_table = malloc(local_table_size * sizeof(uint64_t));
	if (!hpcc_table)
		send_abort = 1;

	hpcc_lock = malloc(sizeof(uint64_t) * num_ranks);
	if (!hpcc_lock)
		send_abort = 1;

	ctpm_Barrier();
	recv_abort = ctpm_int_sum_all(&send_abort, num_ranks);
	ctpm_Barrier();

	if (recv_abort > 0) {
		if (HEAD_RANK)
			fprintf(outFile, "Failed to allocate memory for the main table.\n");
		/* check all allocations in case there are new added and their order changes */
		if (hpcc_table)
			free(hpcc_table);
		goto failed_table;
	}

	for (i = 0; i < num_ranks; i++)
		hpcc_lock[i] = UNLOCKED;

	/* register memory */
	rc = fi_mr_reg(prov.dom, hpcc_table, local_table_size * sizeof(uint64_t),
			FI_REMOTE_READ | FI_REMOTE_WRITE | FI_SEND | FI_RECV | FI_READ
					| FI_WRITE, 0, 0, 0, &prov.data.l_table_mr, NULL);
	if (rc != FI_SUCCESS)
		DEBUG("rc=%d\n", rc);
	assert(rc == FI_SUCCESS);

	rc = fi_mr_reg(prov.dom, hpcc_lock, sizeof(uint64_t) * num_ranks,
			FI_REMOTE_READ | FI_REMOTE_WRITE | FI_SEND | FI_RECV | FI_READ
					| FI_WRITE, 0, 0, 0, &prov.data.l_lock_mr, NULL);
	assert(rc == FI_SUCCESS);

	rc = fi_mr_reg(prov.dom, scratch, sizeof(uint32_t) * SCRATCH_SIZE,
			FI_REMOTE_READ | FI_REMOTE_WRITE | FI_SEND | FI_RECV | FI_READ
					| FI_WRITE, 0, 0, 0, &prov.data.l_scratch_mr, NULL);
	assert(rc == FI_SUCCESS);

	/* distribute memory keys */
	exchange_registration_data(&prov);

	/* Default number of global updates to table: 4x number of table entries */
	default_num_updates = 4 * table_size;
	proc_num_updates = 4 * local_table_size;
	actual_num_updates = default_num_updates;

	if (HEAD_RANK) {
		fprintf(outFile, "Running on %d processors\n", num_ranks);
		fprintf(outFile,
				"Total Main table size = 2^" FSTR64 " = " FSTR64 " words\n",
				log_table_size, table_size);
		fprintf(outFile,
				"PE Main table size = (2^" FSTR64 ")/%d  = " FSTR64 " words/PE MAX\n",
				log_table_size, num_ranks, local_table_size);

		fprintf(outFile,
				"Default number of updates (RECOMMENDED) = " FSTR64 "\n",
				default_num_updates);
	}

	/* Initialize main table */
	for (i = 0; i < local_table_size; i++)
		hpcc_table[i] = i + global_start_my_proc;

	saved_initial_ran_val = generate_initial_xor_val(4 * global_start_my_proc);

	/* start timed section */
	real_time = -usec_time();
	update_table(hpcc_table, table_size, min_local_table_size,
			num_top_table_entries, remainder, proc_num_updates);
	real_time += usec_time();
	/* End timed section */

	/* Print timing results */
	if (HEAD_RANK) {
		*local_gups_score = 1e-9 * actual_num_updates / (real_time * 1.0);
		fprintf(outFile, "Real time used = %.6f seconds\n", real_time);
		fprintf(outFile, "%.9f Billion(10^9) Updates    per second [GUP/s]\n",
				*local_gups_score);
		fprintf(outFile, "%.9f Billion(10^9) Updates/PE per second [GUP/s]\n",
				*local_gups_score / num_ranks);
	}
	/* distribute result to all nodes */
	ctpm_Barrier();
	ctpm_Bcast(local_gups_score, sizeof(double));
	ctpm_Barrier();

	/* Verification phase */

	/* Begin timing here */

	real_time = -usec_time();

	update_table(hpcc_table, table_size, min_local_table_size,
			num_top_table_entries, remainder, proc_num_updates);

	num_errors = 0;
	for (i = 0; i < local_table_size; i++) {
		if (hpcc_table[i] != i + global_start_my_proc)
			num_errors++;
	}

	real_time += usec_time();

	ctpm_Barrier();
	global_num_errors = ctpm_int64_sum_all((void *)&num_errors, num_ranks);
	ctpm_Barrier();

	/* End timed section */

	if (HEAD_RANK) {
		fprintf(outFile, "Verification:  Real time used = %.6f seconds\n",
				real_time);
		fprintf(outFile,
				"Found " FSTR64 " errors in " FSTR64 " locations (%s).\n",
				global_num_errors, table_size,
				(global_num_errors <= 0.01 * table_size) ? "passed" : "failed");
		if (global_num_errors > 0.01 * table_size)
			failure = 1;
	}
	/* End verification phase */

	free(hpcc_table);
	free(hpcc_lock);
	failed_table:

	if (HEAD_RANK)
		if (outFile != stderr)
			fclose(outFile);

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

	ctpm_Init(&argc, &argv);
	ctpm_Rank(&rank);
	ctpm_Job_size(&num_ranks);

	prov.hints = fi_allocinfo();
	if (!prov.hints)
		return EXIT_FAILURE;

	hints = prov.hints;

	while ((op = getopt(argc, argv, "hm:n:" CT_STD_OPTS)) != -1) {
		switch(op) {
		default:
			ct_parse_std_opts(op, optarg, hints);
			break;
		/*
		 * memory per PE (used for determining table size)
		 */
		case 'm':
			tunables.total_mem_opt = atoll(optarg);
			if (tunables.total_mem_opt <= 0) {
				print_usage();
				return -1;
			}
			break;

			/*
			 * num updates/PE
			 */
		case 'n':
			tunables.num_updates_opt = atoi(optarg);
			if (tunables.num_updates_opt <= 0) {
				print_usage();
				return -1;
			}
			break;
		case '?':
		case 'h':
			print_usage();
			return EXIT_FAILURE;
		}
	}

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

	// barrier and finalize
	ctpm_Barrier();

	ctpm_Finalize();

	return 0;
}

/* vi:set sw=8 sts=8 */
