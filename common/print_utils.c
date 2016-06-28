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

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <stdarg.h>

#include "print_utils.h"

#define INIT_BUF_LEN 	4096		       /* 4KB initial buf size */
#define MAX_BUF_LEN	4096 * 4096 * 64       /* 1GB max buf size */
#define STDOUT_FD 	1

/*******************************************************************************
 * HELPER MACROS
 ******************************************************************************/
#define __CHK_WRITE_ERR(ret, info)					\
	do {								\
		if ((int) (ret) == -1) {				\
			fprintf(stderr, "ERROR: write returned %s\n",	\
				strerror(errno));			\
			fini_info(info);				\
			abort();					\
		}							\
	} while(0)

/**
 * @var test_name  the name of the test.
 * @var csv_path   the full or relative path to the csv file.
 * @var csv_header the comma separated list of column headings.
 *
 * @var tbuf	   internal tabular buffer used to add lines of data to.
 * @var tbuf_sz	   the tabular buffer size.
 * @var tbuf_cnt   the number of valid bytes in tbuf.
 *
 * @var cbuf	   internal csv buffer used to add lines of data to.
 * @var cbuf_sz	   the csv buffer size.
 * @var cbuf_cnt   the number of valid bytes in cbuf.
 *
 * @var csv_fd	   the file descriptor for the csv file.
 */
struct info {
	char *test_name;
	char *csv_path;
	char *csv_header;

	char *tbuf;
	size_t tbuf_sz;
	size_t tbuf_cnt;

	char *cbuf;
	size_t cbuf_sz;
	size_t cbuf_cnt;

	int  csv_fd;
};

/*******************************************************************************
 * HELPER FNS
 ******************************************************************************/
/**
 * Double the size of the buf.
 * @var buf    pointer to the buffer to grow
 * @var cur_sz the current size of the buffer.
 *
 * @return the new size of the buffer on success; otherwise -1.
 */
inline int __grow_buf(void **buf, int cur_sz)
{
	char *tmp;

	if (cur_sz * 2 > MAX_BUF_LEN) {
		return -1;
	} else {
		tmp = realloc(*buf, cur_sz * 2);
		if (!tmp) {
			return -1;
		}
		*buf = tmp;
	}
	return cur_sz * 2;
}

/*******************************************************************************
 * API FNS
 ******************************************************************************/
inline info_t *init_info(char *test_name, char *csv_path, char *csv_header)
{
	int ret;
	info_t *info = calloc(1, sizeof(info_t));
	struct stat st;

	assert(info);

	if (test_name) {
		/* Ensure test name ends in newline */
		if (!strchr(test_name, '\n')) {
			info->test_name = malloc(strlen(test_name) + 2);
			memcpy(info->test_name, test_name, strlen(test_name) + 1);
			strncat(info->test_name, "\n", 1);
		} else {
			info->test_name = strdup(test_name);
		}

		ret = write(STDOUT_FD, info->test_name, strlen(test_name));

		__CHK_WRITE_ERR(ret, info);
	} else {
		fprintf(stderr, "ERROR in init_info, please provide a non-NULL "
			"test_name.\n");
		goto err;
	}

	if (csv_path) {
		info->csv_path = strdup(csv_path);
		if (!csv_header) {
			fprintf(stderr, "ERROR in init_info, please provide a "
				"non-NULL csv_header.\n");
			goto err;
		}

		info->csv_fd = open(csv_path, O_APPEND);

		/* Report error if error isn't that the file doesn't exist */
		if (info->csv_fd == -1 && errno != ENOENT) {
			fprintf(stderr, "Unable to open '%s', open returned %s\n",
				csv_path, strerror(errno));
			goto err;
		}

		/* Check if the file was opened */
		if (info->csv_fd != -1) {
			ret = fstat(info->csv_fd, &st);

			if (ret == -1) {
				fprintf(stderr, "Unable to stat info->csv_fd (%d), "
					"fstat returned %s\n", info->csv_fd,
					strerror(errno));
				goto err;
			}

			/* If the file size is zero, append the csv header */
			if (st.st_size == 0) {
				/* Ensure header end in newline */
				if (!strchr(csv_header, '\n')) {
					info->csv_header = malloc(strlen(csv_header) + 2);
					memcpy(info->csv_header, csv_header, strlen(csv_header) + 1);
					strncat(info->csv_header, "\n", 1);
				} else {
					info->csv_header = strdup(csv_header);
				}
				ret = write(info->csv_fd, info->csv_header, strlen(csv_header));

				__CHK_WRITE_ERR(ret, info);
			}
		}
	}

	info->tbuf_sz = INIT_BUF_LEN;
	info->tbuf = malloc(info->tbuf_sz);

	info->cbuf_sz = INIT_BUF_LEN;
	info->cbuf = malloc(info->cbuf_sz);

	return info;

err:
	fini_info(info);
	return NULL;
}

inline void add_line(info_t *info, char *fmt, ...)
{
	int ret, tbw, cbw, t;
	va_list vl;

	va_start(vl, fmt);
	tbw = vsnprintf(info->tbuf + info->tbuf_cnt,
		       info->tbuf_sz - info->tbuf_cnt, fmt, vl);

	/* check for exhausted tbuf */
	while (info->tbuf_sz - info->tbuf_cnt - tbw == 0) {
		ret = __grow_buf((void **) &info->tbuf, (int) info->tbuf_sz);
		if (ret == -1) {		/* can't grow, flush buffer */
			if (info->tbuf_cnt == 0) {
				fprintf(stderr, "ERROR: in add_line, va list "
					"too large or system out of memory.\n");
				fini_info(info);
				abort();
			}
			ret = write(STDOUT_FD, info->tbuf, info->tbuf_cnt);
			__CHK_WRITE_ERR(ret, info);

			info->tbuf_cnt = 0;
		} else {
			info->tbuf_sz = (size_t) ret;
		}
		va_start(vl, fmt);
		tbw = vsnprintf(info->tbuf + info->tbuf_cnt,
			       info->tbuf_sz - info->tbuf_cnt, fmt, vl);
	}

	t = info->tbuf_cnt;
	info->tbuf_cnt += (size_t) tbw;

	if (info->csv_fd != -1) {
		/* Append the tbuf values to the cbuf and convert to csv */
		while (t != info->tbuf_cnt) {
			for (cbw = info->cbuf_cnt;
			     cbw < info->cbuf_sz && t < info->tbuf_cnt; t++) {
				/* Hopefully the va list doesn't contain strings
				 * with spaces */
				if (isspace(info->tbuf[t])) {
					if (t + 1 < info->tbuf_sz &&
					    !isspace(info->tbuf[t + 1])) {
						info->cbuf[cbw++] = ',';
					}
					continue;
				}

				info->cbuf[cbw++] = info->tbuf[t];
			}

			info->cbuf_cnt = (size_t) cbw;

			/* Partial write */
			if (t != info->tbuf_cnt) {
				ret = __grow_buf((void **) &info->cbuf,
						 info->cbuf_sz);
				if (ret == -1) {
					ret = write(info->csv_fd, info->cbuf,
						    info->cbuf_cnt);
					__CHK_WRITE_ERR(ret, info);

					info->cbuf_cnt = 0;
				} else {
					info->cbuf_sz = (size_t) ret;
				}
			}
		}
	}
}

inline void print_data(info_t *info)
{
	int ret;

	/* write to table */
	if (info->tbuf_cnt) {
		ret = write(STDOUT_FD, info->tbuf, info->tbuf_cnt);
		__CHK_WRITE_ERR(ret, info);
	}

	/* write to csv */
	if (info->csv_fd != -1 && info->cbuf_cnt) {
		ret = write(info->csv_fd, info->cbuf, info->cbuf_cnt);
		__CHK_WRITE_ERR(ret, info);
	}
}

inline void fini_info(info_t *info)
{
	int ret;
	if (info->test_name) {
		free(info->test_name);
	}

	if (info->csv_path) {
		free(info->csv_path);
	}

	if (info->csv_header) {
		free(info->csv_header);
	}

	if (info->tbuf_cnt && info->tbuf) {
		ret = write(STDOUT_FD, info->tbuf, info->tbuf_cnt);
		__CHK_WRITE_ERR(ret, info);

		free(info->tbuf);
	}

	if (info->cbuf_cnt) {
		ret = write(info->csv_fd, info->cbuf, info->cbuf_cnt);
		__CHK_WRITE_ERR(ret, info);

		free(info->cbuf);
	}

	ret = close(info->csv_fd);

	if (ret == -1) {
		fprintf(stderr, "ERROR: close returned %s\n", strerror(errno));
	}

	free(info);
}
