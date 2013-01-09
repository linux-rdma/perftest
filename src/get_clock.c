/*
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
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
 * $Id$
 *
 * Author: Michael S. Tsirkin <mst@mellanox.co.il>
 */

/* #define DEBUG 1 */
/* #define DEBUG_DATA 1 */
/* #define GET_CPU_MHZ_FROM_PROC 1 */

/* For gettimeofday */
#define _BSD_SOURCE
#include <sys/time.h>

#include <unistd.h>
#include <stdio.h>
#include "get_clock.h"

#ifndef DEBUG
#define DEBUG 0
#endif
#ifndef DEBUG_DATA
#define DEBUG_DATA 0
#endif

#define MEASUREMENTS 200
#define USECSTEP 10
#define USECSTART 100

/*
 Use linear regression to calculate cycles per microsecond.
 http://en.wikipedia.org/wiki/Linear_regression#Parameter_estimation
*/
static double sample_get_cpu_mhz(void)
{
	struct timeval tv1, tv2;
	cycles_t start;
	double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
	double tx, ty;
	int i;

	/* Regression: y = a + b x */
	long x[MEASUREMENTS];
	cycles_t y[MEASUREMENTS];
	double a; /* system call overhead in cycles */
	double b; /* cycles per microsecond */
	double r_2;

	for (i = 0; i < MEASUREMENTS; ++i) {
		start = get_cycles();

		if (gettimeofday(&tv1, NULL)) {
			fprintf(stderr, "gettimeofday failed.\n");
			return 0;
		}

		do {
			if (gettimeofday(&tv2, NULL)) {
				fprintf(stderr, "gettimeofday failed.\n");
				return 0;
			}
		} while ((tv2.tv_sec - tv1.tv_sec) * 1000000 +
			(tv2.tv_usec - tv1.tv_usec) < USECSTART + i * USECSTEP);

		x[i] = (tv2.tv_sec - tv1.tv_sec) * 1000000 +
			tv2.tv_usec - tv1.tv_usec;
		y[i] = get_cycles() - start;
		if (DEBUG_DATA)
			fprintf(stderr, "x=%ld y=%Ld\n", x[i], (long long)y[i]);
	}

	for (i = 0; i < MEASUREMENTS; ++i) {
		tx = x[i];
		ty = y[i];
		sx += tx;
		sy += ty;
		sxx += tx * tx;
		syy += ty * ty;
		sxy += tx * ty;
	}
	
	b = (MEASUREMENTS * sxy - sx * sy) / (MEASUREMENTS * sxx - sx * sx);
	a = (sy - b * sx) / MEASUREMENTS;

	if (DEBUG)
		fprintf(stderr, "a = %g\n", a);
	if (DEBUG)
		fprintf(stderr, "b = %g\n", b);
	if (DEBUG)
		fprintf(stderr, "a / b = %g\n", a / b);
	r_2 = (MEASUREMENTS * sxy - sx * sy) * (MEASUREMENTS * sxy - sx * sy) /
		(MEASUREMENTS * sxx - sx * sx) /
		(MEASUREMENTS * syy - sy * sy);

	if (DEBUG)
		fprintf(stderr, "r^2 = %g\n", r_2);
	if (r_2 < 0.9) {
		fprintf(stderr,"Correlation coefficient r^2: %g < 0.9\n", r_2);
		return 0;
	}

	return b;
}

static double proc_get_cpu_mhz(int no_cpu_freq_fail)
{
	FILE* f;
	char buf[256];
	double mhz = 0.0;

	f = fopen("/proc/cpuinfo","r");
	if (!f)
		return 0.0;
	while(fgets(buf, sizeof(buf), f)) {
		double m;
		int rc;

#if defined (__ia64__)
		/* Use the ITC frequency on IA64 */
		rc = sscanf(buf, "itc MHz : %lf", &m);
#elif defined (__PPC__) || defined (__PPC64__)
		/* PPC has a different format as well */
		rc = sscanf(buf, "clock : %lf", &m);
#else
		rc = sscanf(buf, "cpu MHz : %lf", &m);
#endif

		if (rc != 1)
			continue;

		if (mhz == 0.0) {
			mhz = m;
			continue;
		}
		if (mhz != m) {
			fprintf(stderr, "Conflicting CPU frequency values"
				" detected: %lf != %lf\n", mhz, m);
			if (no_cpu_freq_fail) {
				fprintf(stderr, "Test integrity may be harmed !\n");
			}else{
				return 0.0;
			}
			continue;
		}
	}
	fclose(f);
	return mhz;
}


double get_cpu_mhz(int no_cpu_freq_fail)
{
	double sample, proc, delta;
	sample = sample_get_cpu_mhz();
	proc = proc_get_cpu_mhz(no_cpu_freq_fail);

	if (!proc || !sample)
		return 0;

	delta = proc > sample ? proc - sample : sample - proc;
	if (delta / proc > 0.01) {
			fprintf(stderr, "Warning: measured timestamp frequency "
					"%g differs from nominal %g MHz\n",
					sample, proc);
			return sample;
	}
	return proc;
}
