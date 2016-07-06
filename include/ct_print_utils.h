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
 *
 */

#ifndef _CT_PRINT_UTILS_H_
#define _CT_PRINT_UTILS_H_

typedef struct info ct_info_t;

/**
 * This function must be called before any other function.
 *
 * The csv_header and csv_path are not required but if the csv_path is non-NULL
 * the csv_header must be non-NULL as well.
 *
 * If you want the csv file to be appended to make sure it has been created
 * before calling init_info(), this file will not be truncated, only be appended
 * to.
 *
 * This fn allocates the info structure, opens the csv file if it exists,
 * and initializes the info structure.
 *
 * @param test_name  the name of the test.
 * @param csv_path   the full or relative path to the csv file.
 * @param csv_header the comma separated list of column headings.
 *
 * @return an instance of the info structure.
 */
ct_info_t *ct_init_info(char *test_name, char *csv_path);

/**
 * Adds a line of data to the the internal print buffers.
 * If the csv file doesn't exist no data is written to it's
 * internal buffer.
 *
 * It is required that the fmt buffer ends in at least one newline.
 *
 * @param info the info instance returned by init_info().
 * @param fmt  C format string specifying the output format.
 * @param ...  optional varargs list that accompanies the fmt string.
 */
void ct_add_line(ct_info_t *info, char *fmt, ...);

/**
 * Flushes the non-empty internal buffer(s) causing formatted data to be
 * written to the screen and/or the csv file.
 *
 * @param info the info instance returned by init_info().
 */
void ct_print_data(ct_info_t *info);

/**
 * This function should be called before exiting the benchmark.
 *
 * It flushes the internal buffers if needed and cleans up memory/fds allocated
 * by init_info.
 *
 * @param info the info instance returned by init_info().
 */
void ct_fini_info(ct_info_t *info);
#endif	/* _CT_PRINT_UTILS_H_ */
