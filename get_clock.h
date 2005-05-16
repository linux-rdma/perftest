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
 */
#include <unistd.h>
#include <stdio.h>
#if defined (__x86_64__) || defined(__i386__)
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles()
{
	unsigned low, high;
	unsigned long long val;
	asm volatile ("rdtsc" : "=a" (low), "=d" (high));
	val = high;
	val = (val << 32) | low;
	return val;
}
#elif defined(__PPC64__)
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles()
{
	cycles_t ret;

	asm volatile ("mftb %0" : "=r" (ret) : );
	return ret;
}
#elif defined(__ia64__)
typedef unsigned long long cycles_t;
static inline cycles_t get_cycles()
{
	cycles_t ret;

	asm volatile ("mov %0=ar.itc" : "=r"(ret));
	return ret;
}
#else
#warning get_clock not implemented for this architecture
#endif

double get_cpu_mhz()
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
		rc = sscanf(buf, "cpu MHz : %lf", &m);
		if (rc != 1)
			continue;
		if (mhz == 0.0) {
			mhz = m;
			continue;
		}
		if (mhz != m) {
			fprintf(stderr,"Conflicting CPU frequency values "
					" detected: %lf != %lf\n",
					mhz, m);
			return 0.0;
		}
	}
	fclose(f);
	return mhz;
}
