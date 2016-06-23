/*
 * Copyright (c) 2016 Los Alamos National Security, LLC. All rights reserved.
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

#ifndef FT_TBARRIER_H
#define FT_TBARRIER_H

#include <stdlib.h>
#include <stdatomic.h>

typedef struct {
	int phase[2];
	int slot;
	int njoiners;
	atomic_int *counter[2];
	atomic_int *signal[2];
} fabtests_tbar_t;

static void tbarrier(fabtests_tbar_t *tbar)
{
	int njoiners;
	int val;

	njoiners = atomic_fetch_add(tbar->counter[tbar->slot],1);

	/*
	 * if I'm the last one to join, reset counter to 0
	 * and toggle signal variable
	 */

	if ((njoiners + 1) ==  tbar->njoiners) {
		atomic_store(tbar->counter[tbar->slot], 0);
		atomic_store(tbar->signal[tbar->slot],
			     1 - tbar->phase[tbar->slot]);
	} else {
		do {
			val = atomic_load(tbar->signal[tbar->slot]);
		} while (val == tbar->phase[tbar->slot]);
	}

	tbar->phase[tbar->slot] = 1 - tbar->phase[tbar->slot];
	tbar->slot = 1 - tbar->slot;

}

static void tbarrier_init(fabtests_tbar_t *tbar, int njoiners,
			  atomic_int *counter, atomic_int *signal)
{
	tbar->njoiners = njoiners;
	tbar->counter[0] = &counter[0];
	tbar->counter[1] = &counter[1];
	tbar->signal[0] = &signal[0];
	tbar->signal[1] = &signal[1];
	tbar->phase[0] = tbar->phase[1] = 0;
	tbar->slot = 0;
}

#endif /* FT_TBARRIER_H */
