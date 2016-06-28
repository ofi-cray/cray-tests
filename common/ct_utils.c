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

#include <sys/time.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <malloc.h>
#include <sched.h>
#include <sys/utsname.h>
#include <dlfcn.h>

#include "pmi.h"
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_rma.h>
#include "ct_utils.h"
#include "pmi.h"

#ifndef CRAY_PMI_COLL

static int myRank;
static char *kvsName;
static int debug;

#define ENCODE_LEN(_len)	(2 * _len + 1)

static char *encode(char *buf, size_t len)
{
	int i;
	char *ebuf;

	ebuf = calloc(ENCODE_LEN(len), 1);
	assert(ebuf);

	for (i = 0 ; i < len; i++) {
		ebuf[(2*i)] = (buf[i] & 0xF) + 'a';
		ebuf[(2*i)+1] = ((buf[i] >> 4) & 0xF) + 'a';
	}

	ebuf[2*len] = '\0';

	return ebuf;
}

static char *decode(char *ebuf, size_t *outlen)
{
	int i;
	char *buf;
	int len;

	len = strlen(ebuf);

	buf = malloc(len/2);
	assert(buf);

	for (i = 0; i < len/2; i++) {
		buf[i] = (((ebuf[(2*i)+1] - 'a') << 4) | (ebuf[(2*i)] - 'a'));
	}

	*outlen = len;
	return buf;
}

static void gni_pmi_send(char *kvs, void *buffer, size_t len)
{
	char *data;
	char key[64];
	int __attribute__((unused)) rc;

	data = encode(buffer, len);

	snprintf(key, 64, "%s_rank%d", kvs, myRank);
	rc = PMI_KVS_Put(kvsName, key, data);

	if (debug) {
		fprintf(stderr, "[%d]PMI_KVS_Put key: %s data %s, rc %d\n",
			myRank, key, data, rc);
	}

	assert(rc == PMI_SUCCESS);

	rc = PMI_KVS_Commit(kvsName);
	assert(rc == PMI_SUCCESS);

	free(data);
}

static void gni_pmi_receive(char *kvs, int rank, void *buffer, size_t len)
{
	char *data;
	char key[64];
	char *keyval = calloc(ENCODE_LEN(len), 1);
	int __attribute__((unused)) rc;
	size_t outlen;

	snprintf(key, 64, "%s_rank%d", kvs, rank);

	rc = PMI_KVS_Get(kvsName, key, keyval, ENCODE_LEN(len));

	if (debug) {
		fprintf(stderr, "[%d]PMI_KVS_Get key: %s keyval %s, rc %d\n",
			myRank, key, keyval, rc);
	}

	assert(rc == PMI_SUCCESS);

	data = decode(keyval, &outlen);
	assert(data != NULL);

	memcpy(buffer, data, outlen < len ? outlen : len);

	free(keyval);
	free(data);
}

int PMI_Allgather(void *src, void *targ, size_t len_per_rank)
{
	static int cnt;
	int i, nranks;
	char *ptr;
	char idstr[64];

	snprintf(idstr, 64, "allg%d", cnt++);
	gni_pmi_send(idstr, src, len_per_rank);
	PMI_Barrier();
	PMI_Get_size(&nranks);

	for (i = 0; i < nranks; i++) {
		ptr = ((char *)targ) + (i*len_per_rank);
		gni_pmi_receive(idstr, i, ptr, len_per_rank);
	}

	return PMI_SUCCESS;
}

int PMI_Bcast(void *buf, int len)
{
	static int cnt;
	char idstr[64];

	snprintf(idstr, 64, "bcst%d", cnt++);

	if (!myRank) {
		gni_pmi_send(idstr, buf, len);
		PMI_Barrier();
	} else {
		PMI_Barrier();
		gni_pmi_receive(idstr, 0, buf, len);
	}

	PMI_Barrier();

	return PMI_SUCCESS;
}

static void pmi_coll_init(void)
{
	int len;
	int rank;
	int __attribute__((unused)) rc;

	rc = PMI_Get_rank(&rank);
	assert(rc == PMI_SUCCESS);

	myRank = rank;

	rc = PMI_KVS_Get_name_length_max(&len);
	assert(rc == PMI_SUCCESS);

	kvsName = calloc(len, sizeof(char));
	rc = PMI_KVS_Get_my_name(kvsName, len);
	assert(rc == PMI_SUCCESS);

	PMI_Barrier();
}
#else
#define pmi_coll_init()
#endif /* CRAY_PMI_COLL */

static void allgather(void *in, void *out, int len)
{
	static int *ivec_ptr, already_called, job_size;
	int i, __attribute__((unused)) rc;
	int my_rank;
	char *tmp_buf, *out_ptr;

	if (!already_called) {
		rc = PMI_Get_size(&job_size);
		assert(rc == PMI_SUCCESS);
		rc = PMI_Get_rank(&my_rank);
		assert(rc == PMI_SUCCESS);

		ivec_ptr = (int *)malloc(sizeof(int) * job_size);
		assert(ivec_ptr != NULL);

		rc = PMI_Allgather(&my_rank, ivec_ptr, sizeof(int));
		assert(rc == PMI_SUCCESS);

		already_called = 1;
	}

	tmp_buf = malloc(job_size * len);
	assert(tmp_buf);

	rc = PMI_Allgather(in, tmp_buf, len);
	assert(rc == PMI_SUCCESS);

	out_ptr = out;

	for (i = 0; i < job_size; i++) {
		memcpy(&out_ptr[len * ivec_ptr[i]], &tmp_buf[i * len], len);
	}

	free(tmp_buf);
}

void ctpm_Exit(void)
{
	PMI_Abort(0, "Terminating application successfully");
}

void ctpm_Abort(void)
{
	PMI_Abort(-1, "pmi abort called");
}

void ctpm_Barrier(void)
{
	int __attribute__((unused)) rc;

	rc = PMI_Barrier();
	assert(rc == PMI_SUCCESS);
}

void ctpm_Init(int *argc, char ***argv)
{
	int __attribute__((unused)) rc;
	int first_spawned;
	void *libpmi_handle;

	libpmi_handle = dlopen("libpmi.so.0", RTLD_LAZY | RTLD_GLOBAL);
	if (libpmi_handle == NULL) {
		perror("Unabled to open libpmi.so check your LD_LIBRARY_PATH");
		abort();
	}

	rc = PMI_Init(&first_spawned);
	assert(rc == PMI_SUCCESS);

	pmi_coll_init();
}

void ctpm_Rank(int *rank)
{
	int __attribute__((unused)) rc;

	rc = PMI_Get_rank(rank);
	assert(rc == PMI_SUCCESS);
}

void ctpm_Finalize(void)
{
	PMI_Finalize();
}

void ctpm_Job_size(int *nranks)
{
	int __attribute__((unused)) rc;

	rc = PMI_Get_size(nranks);
	assert(rc == PMI_SUCCESS);
}

void ctpm_Allgather(void *src, size_t len_per_rank, void *targ)
{
	allgather(src, targ, len_per_rank);
}

void ctpm_Bcast(void *buffer, size_t len)
{
	int __attribute__((unused)) rc;

	rc = PMI_Bcast(buffer, len);
	assert(rc == PMI_SUCCESS);
}

