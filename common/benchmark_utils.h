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
 * TODO: Print output to stdout?
 * For now, just cat the .csv file and pipe into column -t -s "," for console
 * output.
 *
 * TODO: Calculate the column number when opening a non-empty csv file.
 */

/*
 * Header format (info.hdr):
 * A comma separated list of labels.
 * For example: "label1,label2,...labeln".
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* TODO: Take a va list of parameters and build the header. */
#define BUILD_HDR(S1, S2) #S1 "," #S2

#define FLOAT_PRECISION 2
#define FIELD_WIDTH 31

/**
 * The information structure used throughout the procedures in this header.
 *
 * @var fname	   path to the given csv output file.
 * @var hdr	   space separated list of labels.
 * @var hdr_len	   length of the internally allocated header.
 * @var cols	   number of columns calculated from the hdr.
 * @var date_col   the column number of the date column.
 * @var fp	   file pointer to fname.
 * @var hdr_allocd bit to keep track of whether the hdr was allocd internally.
 */
static struct info_t {
	char *fname;
	struct stat stats;
	FILE *fp;

	char *hdr;
	size_t hdr_len;
	size_t cols;
	size_t date_col;

	char *date_str;
	int date_len;

	int hdr_allocd;
	int hdr_printed;
	int fname_exists;
} info;

static inline void fini_info();

/*******************************************************************************
 * Internal helper functions
 ******************************************************************************/
static inline void __set_cols()
{
	info.cols = 0;

	if (info.hdr) {
		char *tmp = strdup(info.hdr), *prev;
		while (tmp) {
			prev = tmp;
			tmp = strchr(tmp + 1, ',');

			if (tmp)
				*tmp = 0;

			if (!strcasecmp("date", prev)) {
				info.date_col = info.cols;
				info.date_len = strlen(info.date_str);
			}

			info.cols++;
		}
		free(tmp);
	}
}

/**
 * Get the format from an existing csv file specified by info.fname.
 */
static inline void __pull_and_set_hdr()
{
	char buf[4096] = {0};
	int cnt = 1;

	if (!info.hdr) {
		info.hdr_len = 4096;
		info.hdr = malloc(info.hdr_len);
		assert(info.hdr);
	} else {
		fprintf(stderr, "hdr is already set.\n");
		return;
	}

	while(fgets(buf, 4096, info.fp)) {
		if (strrchr(buf, '\n')) { /* end of first line */
			memcpy(info.hdr + (cnt - 1) * 4096, buf, strlen(buf));
			break;
		} else {
			info.hdr_len = ++cnt * 4096;
			info.hdr = realloc(info.hdr, info.hdr_len);
			memcpy(info.hdr + (cnt - 1) * 4096, buf, 4096);
		}
	}

	__set_cols();
}

#if 0
/**
 * Print the data to the given file specified by info.fname.
 */
static inline void __print_table(double data)
{
	static unsigned int col;

	if (!info.hdr_printed) {
		char *tmp = strdup(info.hdr), *ptr, *cur;


		int i = 0;

		for (cur = tmp; i < info.cols - 1; i++) {
			ptr = strchr(cur, ',');

			if (ptr)
				*ptr = 0;

			fprintf(stdout, "%-*s", FIELD_WIDTH, cur);

			cur += strlen(cur) + 1;
		}

		fprintf(stdout, "%s.\n", cur);

		free(tmp);
		info.hdr_printed = 1;
	}

	if (info.date_len && col == info.date_col) {
		time_t t;
		struct tm *lt;

		time(&t);
		lt = localtime(&t);

		if (lt) {	/* yy:mm:dd hh:mm:ss */
			fprintf(stdout, info.date_str, lt->tm_year,
				lt->tm_mon, lt->tm_mday, lt->tm_hour, lt->tm_min,
				lt->tm_sec);
			fprintf(stdout, ".");
		}
		col = (col + 1) % info.cols;
	}

	fprintf(stdout, col != (info.cols - 1) ? "%*.*lf" : "%*.*lf\n",
		FIELD_WIDTH, FLOAT_PRECISION, data);

	fflush(stdout);

	col = (col + 1) % info.cols;
}
#endif

/**
 * Print the data to the given file specified by info.fname.
 */
static inline void __print_csv(double data)
{
	static unsigned int col;

	if (info.stats.st_size == 0) {
		fprintf(info.fp, "%s\n", info.hdr);
		info.stats.st_size = strlen(info.hdr);
	}

	if (~info.date_col && col == info.date_col) {
		time_t t;
		struct tm *lt;

		time(&t);
		lt = localtime(&t);

		if (lt) {	/* yy:mm:dd hh:mm:ss */
			fprintf(info.fp, info.date_str, lt->tm_year % 100,
				lt->tm_mon, lt->tm_mday, lt->tm_hour, lt->tm_min,
				lt->tm_sec);
		}
	}

	fprintf(info.fp, col != (info.cols - 1) ? "%.*lf," : ".*lf\n",
		FLOAT_PRECISION, data);

	col = (col + 1) % info.cols;
}

/*******************************************************************************
 * Exposed utility fns
 ******************************************************************************/
/**
 * This function must be called before print_data() or fini_info().
 * Initialize the info structure, hdr or path must be non-NULL.
 * Note: The file pointed to by path must already exist for the
 * csv file to be written to.
 *
 * @assumptions The path must be non-NULL and valid, the header from an
 * existing csv file pointed to by path will always be used in place of the
 * hdr param
 *
 * @param hdr  the table header.
 * @param path the path to the csv file.
 * @return 0 on success, -1 on error.
 */
static inline int init_info(char *hdr, char *path)
{
	if (!hdr && !path) {
		fprintf(stdout, "ERROR: you must provide a non-NULL hdr or valid path.\n");
		return -1;
	}

	info.fname = path;
	info.hdr = hdr;
	info.fp = NULL;
	info.hdr_printed = 0;
	info.hdr_allocd = 0;
	info.date_col = ~0;
	info.date_str = "%d-%02d-%02d %02d:%02d:%02d,";

	if (hdr && !strchr(hdr, ',')) {
		fprintf(stdout, "ERROR: the hdr must be comma separated.\n");
		fini_info();
		return -1;
	}

	if (path) {
		info.fname_exists = ((stat(info.fname, &info.stats)) == 0);


		if (!info.fp) {
			info.fp = fopen(info.fname, "a+");
		}

		if (info.fname_exists) {
			__pull_and_set_hdr();
		} else {
			if (!hdr) {
				fprintf(stdout, "ERROR: you must provide a vali"
					"d header.\n");
				fini_info();
				return -1;
			}

			fprintf(info.fp, info.hdr);
		}
	} else {
		fprintf(stdout, "ERROR: You must provide a path to an existing csv.\n");
		fini_info();
		return -1;
	}

	__set_cols();

	return 0;
}

/**
 * Must be called before exiting the benchmark.
 */
static inline void fini_info()
{
	if (info.hdr && info.hdr_allocd) {
		free(info.hdr);
		info.hdr_allocd = 0;
	}
	info.hdr = NULL;

	if (info.fp) {
		fclose(info.fp);
	}
	info.fp = NULL;
	info.hdr_allocd = 0;
	info.fname = NULL;
}

static inline void print_data(double data)
{
	if (info.fp) {
		__print_csv(data);
	} else {
		if (!info.hdr) {
			fprintf(stdout, "ERROR: You must successfully call init_info()"
				" first.\n");
			return;
		}
	}

#if 0
	__print_table(data);
#endif
}
