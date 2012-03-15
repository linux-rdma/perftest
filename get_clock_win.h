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
 * $Id: get_clock.h 1955 2007-02-19 14:46:04Z sleybo $
 *
 * Author: Michael S. Tsirkin <mst@mellanox.co.il>
 */

#ifndef GET_CLOCK_H
#define GET_CLOCK_H

#include "complib/cl_timer.h"

typedef uint64_t cycles_t;

static int __cdecl cycles_compare(const void * aptr, const void * bptr)
{
	const cycles_t *a = aptr;
	const cycles_t *b = bptr;
	if (*a < *b) return -1;
	if (*a > *b) return 1;
	return 0;

}
/*
 * When there is an
 *	odd number of samples, the median is the middle number.
 * 	even number of samples, the median is the mean of the
 *		two middle numbers.
 *
 */
inline cycles_t get_median(int n, cycles_t delta[])
{
	if (n % 2)
		return(delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}


static inline cycles_t get_cycles()
{
	return cl_get_tick_count();
}

static double get_cpu_mhz(void)
{
	return (double)cl_get_tick_freq();
}

#endif
