/*
 * Copyright (c) 2005 Mellanox Technologies.  All rights reserved.
 * Copyright (c) 1996-2003 Intel Corporation. All rights reserved. 
 *
 * This software is available to you under the OpenIB.org BSD license
 * below:
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "getopt.h"

char *optarg;
int optind = 1;
int opterr = 1;
int optopt = '?';

int getopt(int argc, char * const argv[], char const *opts)
{
	char *loc;

	optarg = NULL;

	if (optind == 0) {
		optind = opterr = 1;
		optopt = '?';
	}

	if (optind >= argc) {
		return EOF;
	}

	if (argv[optind][0] != '-' && argv[optind][0] != '/') {
		return EOF;
	}

	if (!strcmp(argv[optind], "--")) {
		optind++;
		return '?';
	}

	optopt = argv[optind][1];

	loc = strchr(opts, optopt);
	if (loc == NULL) {
		return '?';
	}

	if (loc[1] != ':') {
		goto out;
	}

	/* process switch argument */
	if (argv[optind][2] != '\0') {
		optarg = &argv[optind][2];
		goto out;
	}

	/* switch argument is optional (::) - be careful */
	if (loc[2] == ':' ) {
		if ((argv[optind+1] == NULL)) {
			/* handle EOL without optional arg */
			optarg = NULL;
			goto out;
		}
		if (argv[optind+1] && (argv[optind+1][0] == '-' || argv[optind+1][0] == '/'))
			goto out;
	}
 
	optarg = argv[++optind];
	if (!optarg || !(*optarg)) {
		return '?';
	}

out:
	optind++;
	return optopt;
}

int getopt_long(int argc, char * const argv[], char const *opts,
				const struct option *longopts, int *longindex)
{
	char arg[256];
	char *str;
#ifdef _WIN32
	char *next_token;
#endif
	int i;

	if (optind == 0) {
		optarg = NULL;
		optind = opterr = 1;
		optopt = '?';
	}

	if (optind == argc) {
		return EOF;
	}

	if (argv[optind][0] != '-' && argv[optind][0] != '/') {
		return EOF;
	}

	if (!strcmp(argv[optind], "--")) {
		optind++;
		return '?';
	}

	if (argv[optind][1] != '-') {
		return getopt(argc, argv, opts);
	}

#ifndef _WIN32
	strcpy(arg, &argv[optind][2]);
	str = strtok(arg, "=");
#else
	strcpy_s(arg, sizeof(arg), &argv[optind][2]);
	str = strtok_s(arg, "=", &next_token);
#endif

	for (i = 0; longopts[i].name; i++) {

		if (strcmp(str, longopts[i].name)) {
			continue;
		}

		if (longindex != NULL) {
			*longindex = i;
		}

		if (longopts[i].flag != NULL) {
			*(longopts[i].flag) = longopts[i].val;
		}

		switch (longopts[i].has_arg) {
		case required_argument:
#ifndef _WIN32
			optarg = strtok(NULL, "=");
#else
			optarg = strtok_s(NULL, "=", &next_token);
#endif
			if (optarg != NULL) {
				break;
			}

			if (++optind == argc || argv[optind][0] == '-') {
				return '?';
			}

			optarg = argv[optind];
			break;
		case optional_argument:
#ifndef _WIN32
			optarg = strtok(NULL, "=");
#else
			optarg = strtok_s(NULL, "=", &next_token);
#endif
			if (optarg != NULL) {
				break;
			}

			if (optind + 1 == argc || argv[optind + 1][0] == '-') {
				break;
			}

			optarg = argv[++optind];
			break;
		default:
			break;
		}

		optind++;
		if (longopts[i].flag == 0) {
			return (longopts[i].val);
		} else {
			return 0;
		}
	}
	return '?';
}
