/*
 * Copyright (c) 2017 Los Alamos National Security, LLC. All rights reserved.
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
/**************************************************************************/
#include <stdatomic.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include "ct_print_utils.h"
#include "ct_utils.h"

#if 1
#define dbg_printf(...)
#else
#define dbg_printf(...)		\
do {				\
	printf(__VA_ARGS__);	\
	fflush(stdout);		\
} while (0)
#endif

/*******************************************************************************
 * Begin tunables
 ******************************************************************************/
static atomic_int atomic_var;
static atomic_int dummy_atomic_var;
static int val = 0; // Needs to match above type
static int dummy_val = 0;

#define MEMORY_ORDER memory_order_acq_rel

#define FIELD_WIDTH 16 // output tunable
/*******************************************************************************
 * End tunables
 ******************************************************************************/

/*******************************************************************************
 * Wrappers to the 28 C11 atomic functions
 ******************************************************************************/
#define POST_FIX_INCREMENT dbg_printf("dummy_val = %d\n", dummy_val++);
#define INIT \
atomic_init(&atomic_var, 0);


#define THREAD_FENCE \
atomic_thread_fence(MEMORY_ORDER);


#define SIGNAL_FENCE \
atomic_signal_fence(MEMORY_ORDER);


#define IS_LOCK_FREE \
atomic_is_lock_free(&atomic_var);


#define STORE_EXPLICIT \
atomic_store_explicit(&atomic_var, ++val, memory_order_seq_cst);

#define STORE \
atomic_store(&atomic_var, ++val);


#define LOAD_EXPLICIT \
atomic_load_explicit(&atomic_var, ++val);

#define LOAD \
atomic_load(&atomic_var);


#define EXCHANGE_EXPLICIT \
atomic_exchange_explicit(&atomic_var, ++val, MEMORY_ORDER);

#define EXCHANGE \
atomic_exchange(&atomic_var, ++val);


// TODO: des, suc, fail
//#define COMPARE_EXCHANGE_STRONG_EXPLICIT \
//atomic_compare_exchange_strong_explicit(&atomic_var, ++val, des, suc, fail);
//
//#define COMPARE_EXCHANGE_STRONG \
//atomic_compare_exchange_strong(&atomic_var, ++val, des);
//
//
//#define COMPARE_EXCHANGE_WEAK_EXPLICIT \
//atomic_compare_exchange_weak_explicit(&atomic_var, ++val, des, suc, fail);
//
//#define COMPARE_EXCHANGE_WEAK \
//atomic_compare_exchange_weak_explicit(&atomic_var, ++val, des);


#define FETCH_ADD \
dbg_printf("atomic_var = %d\n", atomic_fetch_add(&atomic_var, 1));

#define FETCH_ADD_EXPLICIT \
dbg_printf("atomic_var = %d\n", atomic_fetch_add_explicit(&atomic_var, 1, MEMORY_ORDER));


#define FETCH_SUB \
dbg_printf("atomic_var = %d\n", atomic_fetch_sub(&atomic_var, 1));

#define FETCH_SUB_EXPLICIT \
dbg_printf("atomic_var = %d\n", atomic_fetch_sub_explicit(&atomic_var, 1, MEMORY_ORDER));


#define FETCH_OR \
atomic_fetch_or(&atomic_var, ++val);

#define FETCH_OR_EXPLICIT \
atomic_fetch_or_explicit(&atomic_var, ++val, MEMORY_ORDER);


#define FETCH_XOR \
atomic_fetch_xor(&atomic_var, ++val);

#define FETCH_XOR_EXPLICIT \
atomic_fetch_xor_explicit(&atomic_var, ++val, MEMORY_ORDER);


#define FETCH_AND \
atomic_fetch_and(&atomic_var, ++val);

#define FETCH_AND_EXPLICIT \
atomic_fetch_and_explicit(&atomic_var, ++val, MEMORY_ORDER);


#define FLAG_TEST_AND_SET \
atomic_flag_test_and_set(&atomic_var);

#define FLAG_TEST_AND_SET_EXPLICIT \
atomic_flag_test_and_set_explicit(&atomic_var, MEMORY_ORDER);


#define FLAG_CLEAR \
atomic_flag_clear(&atomic_var);

#define FLAG_CLEAR_EXPLICIT \
atomic_flag_clear_explicit(&atomic_var, memory_order_seq_cst);
/*******************************************************************************
 * End wrappers
 ******************************************************************************/

/*******************************************************************************
 * Begin test functions
 ******************************************************************************/
#define DO_TEST_LOOP(FN, ITERS)			\
	do {					\
		assert((ITERS));		\
		volatile uint64_t i = (ITERS);	\
		while (i--) { FN }		\
	} while (0)

#define RUN_TIMED_TEST(NAME, FN, ITERS)					\
	do {								\
		dbg_printf("Starting %s...\n", NAME);			\
		start_time = get_time_nsec();				\
		DO_TEST_LOOP(FN, iters);				\
		stop_time = get_time_nsec();				\
		printf("atomic_var = %d\n", atomic_fetch_add(&atomic_var, 0)); \
		printf("dummy_val = %d\n", dummy_val);				\
		ct_add_line(info, ":%s: %-lu\n", NAME, stop_time - start_time, FIELD_WIDTH); \
	} while (0)
/*******************************************************************************
 * End test functions
 ******************************************************************************/

/*******************************************************************************
 * Run tests
 ******************************************************************************/
int main(int argc, char **argv)
{
	char *test_name   = "---gnu_gcc_atomics---";
	char *csv_header  = ":functionName: runtime(ns)";
	char *csv_path	  = "./builtin_atomic_tests.csv"; // Touch to create
	ct_info_t *info;
	uint64_t iters, start_time, stop_time; // ~4*10^6 iterations

	info = ct_init_info(test_name, csv_path);

	if (argc != 2) {
		iters = 1 << 22;
	} else {
		iters = atoi(argv[1]);
	}

	ct_add_line(info, "IterationsOfEachFunction:%lu\n", iters);
	ct_add_line(info, "%-s\n", csv_header, FIELD_WIDTH);

	RUN_TIMED_TEST("C_postfix_increment", POST_FIX_INCREMENT, iters);
	RUN_TIMED_TEST("atomic_init", INIT, iters);
	RUN_TIMED_TEST("atomic_thread_fence", THREAD_FENCE, iters);
	RUN_TIMED_TEST("atomic_signal_fence", SIGNAL_FENCE, iters);
	RUN_TIMED_TEST("atomic_is_lock_free", IS_LOCK_FREE, iters);
	RUN_TIMED_TEST("atomic_store_explicit", STORE_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_store", STORE, iters);
	RUN_TIMED_TEST("atomic_load_explicit", LOAD_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_load", LOAD, iters);
	RUN_TIMED_TEST("atomic_exchange_explicit", EXCHANGE_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_exchange", EXCHANGE, iters);
	/* RUN_TIMED_TEST("atomic_compare_exchange_strong_explicit", */
	/* 	       COMPARE_EXCHANGE_STRONG_EXPLICIT, iters); */
	/* RUN_TIMED_TEST("atomic_compare_exchange_strong", */
	/* 	       COMPARE_EXCHANGE_STRONG, iters); */
	/* RUN_TIMED_TEST("atomic_compare_exchange_weak_explicit", */
	/* 	       COMPARE_EXCHANGE_WEAK_EXPLICIT, iters); */
	/* RUN_TIMED_TEST("atomic_compare_exchange_weak", COMPARE_EXCHANGE_WEAK, */
	/* 	       iters); */
	RUN_TIMED_TEST("atomic_fetch_add", FETCH_ADD, iters);
	RUN_TIMED_TEST("atomic_fetch_add_explicit", FETCH_ADD_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_fetch_sub", FETCH_SUB, iters);
	RUN_TIMED_TEST("atomic_fetch_sub_explicit", FETCH_SUB_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_fetch_or", FETCH_OR, iters);
	RUN_TIMED_TEST("atomic_fetch_or_explicit", FETCH_OR_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_fetch_xor", FETCH_XOR, iters);
	RUN_TIMED_TEST("atomic_fetch_xor_explicit", FETCH_XOR_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_fetch_and", FETCH_AND, iters);
	RUN_TIMED_TEST("atomic_fetch_and_explicit", FETCH_AND_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_flag_test_and_set", FLAG_TEST_AND_SET, iters);
	RUN_TIMED_TEST("atomic_flag_test_and_set_explicit",
		       FLAG_TEST_AND_SET_EXPLICIT, iters);
	RUN_TIMED_TEST("atomic_flag_clear", FLAG_CLEAR, iters);
	RUN_TIMED_TEST("atomic_flag_clear_explicit", FLAG_CLEAR_EXPLICIT, iters);

	atomic_init(&dummy_atomic_var, atomic_fetch_add(&atomic_var, 0));
	printf("dummy_atomic_var = %d\n", atomic_fetch_add(&dummy_atomic_var, 0));

	ct_fini_info(info);
}
