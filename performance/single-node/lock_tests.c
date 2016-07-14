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
/* #define rlock_acquire  pthread_rwlock_rdlock */
/* #define rwlock_acquire pthread_rwlock_wrlock */
/* #define release        pthread_rwlock_unlock */
/* #define lock_t	       pthread_rwlock_t */
#define rlock_acquire  pthread_mutex_lock
#define rwlock_acquire pthread_mutex_lock
#define release        pthread_mutex_unlock
#define lock_t	       pthread_mutex_t
/**************************************************************************/
#define FIELD_WIDTH 16

#include <pthread.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "ct_print_utils.h"
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
	int p;
	volatile int tmp = 0;

	pthread_barrier_wait(&barrier);
	ct_start_clock();

	for (i = 0; i < nreads; i++) {
		rlock_acquire(&lock);
		while (++tmp % 64);
		release(&lock);
	}

	return NULL;
}

void *writer(void *arg)
{
	unsigned nwrites = *(unsigned *) arg, i;
	volatile int tmp = 0;

	pthread_barrier_wait(&barrier);
	ct_start_clock();


	for (i = 0; i < nwrites; i++) {
		rwlock_acquire(&lock);
		while (++tmp % 128);
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
	char *test_name   = "---pthread_mutex(glibc-2.11.3)---";
	char *csv_header  = "runtime(s),cpu_time";
	char *csv_path	  = "./lock-tests.csv";
	ct_info_t *info;

	info = ct_init_info(test_name, csv_path);

	ct_add_line(info, "%-*s%-*s\n", FIELD_WIDTH, "runtime(s)", FIELD_WIDTH,
		    "cpu_time");

	/* n readers, n writers */
	rwlock_test(1<<7, 1<<14, 1<<7, 1<<14);
	ct_add_line(info, "%-*g%-*g\n", FIELD_WIDTH,
		    ct_wall_clock_time(), FIELD_WIDTH, ct_cpu_time());

	/* x readers, 1 writers */
	rwlock_test(1<<7, 1<<14, 1<<0, 1<<14);
	ct_add_line(info, "%-*g%-*g\n", FIELD_WIDTH,
		    ct_wall_clock_time(), FIELD_WIDTH, ct_cpu_time());

	/* x readers, no writers */
	rwlock_test(1<<7, 1<<14, 0, 1<<14);
	ct_add_line(info, "%-*g%-*g\n", FIELD_WIDTH,
		    ct_wall_clock_time(), FIELD_WIDTH, ct_cpu_time());

	ct_fini_info(info);
}
