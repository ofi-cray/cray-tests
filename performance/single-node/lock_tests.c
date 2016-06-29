/*
 * Copyright (c) 2016 Cray Inc. All rights reserved.
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

/******* Tunable defines for testing different lock implementations *******/
#define rlock_acquire  pthread_rwlock_rdlock
#define rwlock_acquire pthread_rwlock_wrlock
#define release        pthread_rwlock_unlock
#define lock_t	       pthread_rwlock_t
/**************************************************************************/
#define FIELD_WIDTH 16

#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

/* #include "print_utils.h" */
#include "ct_utils.h"

static pthread_barrier_t barrier;
static pthread_t *writers, *readers;
static lock_t lock;

/*******************************************************************************
 * Begin test functions
 ******************************************************************************/
void *reader(void *arg)
{
	unsigned nreads = *(unsigned *)arg, i;
	pthread_barrier_wait(&barrier);
	ct_start_clock();

	for (i = 0; i < nreads; i++) {
		rlock_acquire(&lock);
		/* read */
		release(&lock);
	}

	return NULL;
}

void *writer(void *arg)
{
	unsigned nwrites = *(unsigned *) arg, i;
	pthread_barrier_wait(&barrier);
	ct_start_clock();

	for (i = 0; i < nwrites; i++) {
		rwlock_acquire(&lock);
		/* write */
		release(&lock);
	}

	return NULL;
}

/* Fire up nreader/writer threads that do nreads_per_reader/writer before exiting */
void rwlock_test(unsigned nreaders, unsigned nreads_per_reader,
		 unsigned nwriters, unsigned nwrites_per_writer)
{
	int i, j;
	void *ret;

	if (nreaders) {
		readers = malloc(sizeof(pthread_t) * nreaders);
		assert(readers);
	}

	if (nwriters) {
		writers = malloc(sizeof(pthread_t) * nwriters);
		assert(writers);
	}

	/* Ensure readers and writers wait until all threads are up */
	pthread_barrier_init(&barrier, NULL, nreaders + nwriters);

	/* spawn read threads */
	for (i = 0; i < nreaders; i++) {
		assert(!pthread_create(readers + i, NULL, reader, &nreads_per_reader));
	}

	/* spawn write threads */
	for (j = 0; j < nwriters; j++) {
		assert(!pthread_create(writers + j, NULL, writer, &nwrites_per_writer));
	}

	/* join readers */
	for (i = 0; i < nreaders; i++) {
		assert(!pthread_join(readers[i], &ret));
		assert(!ret);
	}

	/* join writers */
	for (j = 0; j < nwriters; j++) {
		assert(!pthread_join(writers[j], &ret));
		assert(!ret);
	}

	ct_stop_clock();

	if (readers)
		free(readers);

	if (writers)
		free(writers);

	readers = writers = NULL;
}
/*******************************************************************************
 * End test functions
 ******************************************************************************/

/*******************************************************************************
 * Run tests
 ******************************************************************************/
int main(int argc, char **argv)
{
	char *test_name   = "rwlock";
	char *csv_header  = "Date,runtime(s),cpu_time";
	/* TODO: init_info(test_name, csv_header, arr_len) */

	/* n readers, n writers */
	char *test_params = "nreaders = 10, nreads_per_reader = 2^20, nwriters = 10,"
		" nwrites_per_writer = 2^20";
	rwlock_test(10, 1<<20, 10, 1<<20);

	/* TODO: print_utils should print the test_name & test_params followed by
	 * the formatted output.
	 * TODO: print_data(test_params);
	 */

	printf("%s\n%s\nruntime(s) = %-*gcpu_time = %-*g\n\n", test_name,
	       test_params, FIELD_WIDTH, ct_wall_clock_time(), FIELD_WIDTH, ct_cpu_time());

	/* x readers, 1 writers */
	test_params = "nreaders = 10, nreads_per_reader = 2^20, nwriters = 1,"
		" nwrites_per_writer = 2^20";

	rwlock_test(10, 1<<20, 1, 1<<20);

	printf("%s\n%s\nruntime(s) = %-*gcpu_time = %-*g\n\n", test_name,
	       test_params, FIELD_WIDTH, ct_wall_clock_time(), FIELD_WIDTH, ct_cpu_time());

	/* x readers, no writers */
	test_params = "nreaders = 10, nreads_per_reader = 2^20, nwriters = 0,"
		" nwrites_per_writer = 2^20";

	rwlock_test(10, 1<<20, 0, 1<<20);

	printf("%s\n%s\nruntime(s) = %-*gcpu_time = %-*g\n\n", test_name,
	       test_params, FIELD_WIDTH, ct_wall_clock_time(), FIELD_WIDTH, ct_cpu_time());

	/* TODO: fini_info */
}
