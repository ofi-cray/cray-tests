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

#include <ct_utils.h>
#include <gnix_vector.h>

/* Test variables */
static gnix_vector_t vec;
static gnix_vec_attr_t attr;
static void *tmp;

#define VEC_SZ  1024*1024*1024


static void setup(void)
{
        int ret;

	attr.vec_increase_step = 2;
	attr.vec_increase_type = GNIX_VEC_INCREASE_MULT;
	attr.vec_initial_size = VEC_SZ;
	attr.vec_internal_locking = GNIX_VEC_UNLOCKED;
	attr.vec_maximum_size = VEC_SZ;
}

static void teardown(void)
{
	int ret;

	ret = _gnix_vec_close(&vec);
	cr_assert(!ret, "_gnix_vec_close");
}

static void warmup(unsigned iters)
{
	int i;

	for (i = 0; i < iters; i++) {
		_gnix_vec_insert_at(&vec, 0xdeadbeef);
		_gnix_vec_at(&vec, &tmp, i);
		_gnix_vec_remove_at(&vec, i);
	}
}

static void ops_table_test(unsigned iters)
{
	int i;


	START_CLOCK;

	for (i = 0; i < iters; i++) {
		vec.ops->insert_at(&vec, 0xdeadbeef);
	}

	for (i = 0; i < iters; i++) {
		vec.ops->at(&vec, &tmp, i);
	}

	for (i = 0; i < iters; i++) {
		vec.ops->remove_at(&vec, i);
	}

	STOP_CLOCK;
	return;
}

static void no_ops_table_test(unsigned iters)
{
	START_CLOCK;

	for (i = 0; i < iters; i++) {
		_gnix_vec_insert_at(&vec, 0xdeadbeef);
	}

	for (i = 0; i < iters; i++) {
		_gnix_vec_at(&vec, &tmp, i);
	}

	for (i = 0; i < iters; i++) {
		_gnix_vec_remove_at(&vec, i);
	}

	STOP_CLOCK;
	return;
}

int main(int argc, char **argv)
{
	char *test_name   = "ops_table";
	char *csv_header  = "Date,runtime(s),cpu_time";
	char test_params[32];

	sprintf(test_params, "iters = %u", VEC_SZ);

	setup();

	warmup(VEC_SZ);

	ops_table_test(VEC_SZ);
	printf("%s\n%s\nruntime(s) = %-*gcpu_time = %-*g\n\n", test_name,
	       test_params, FIELD_WIDTH, WALL_CLOCK_TIME, FIELD_WIDTH, CPU_TIME);


	test_name = "no_ops_table";
	no_ops_table_test(VEC_SZ);
	printf("%s\n%s\nruntime(s) = %-*gcpu_time = %-*g\n\n", test_name,
	       test_params, FIELD_WIDTH, WALL_CLOCK_TIME, FIELD_WIDTH, CPU_TIME);

	teardown();
}
