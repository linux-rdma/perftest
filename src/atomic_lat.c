/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
 * Copyright (c) 2005 Hewlett Packard, Inc (Grant Grundler)
 * Copyright (c) 2009 HNR Consulting.  All rights reserved.
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

#if HAVE_CONFIG_H
#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "get_clock_win.h"
#else
#include <unistd.h>
#include <malloc.h>
#include "get_clock.h"
#endif

#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "perftest_communication.h"

cycles_t *tstamp;

#ifdef _WIN32
#pragma warning( disable : 4242)
#pragma warning( disable : 4244)
#else
#define __cdecl
#endif

#ifndef _WIN32
/*
 * When there is an
 *	odd number of samples, the median is the middle number.
 * 	even number of samples, the median is the mean of the
 *		two middle numbers.
 *
 */
static inline cycles_t get_median(int n, cycles_t delta[])
{
	if ((n - 1) % 2)
		return(delta[n / 2] + delta[n / 2 - 1]) / 2;
	else
		return delta[n / 2];
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int cycles_compare(const void * aptr, const void * bptr)
{
	const cycles_t *a = aptr;
	const cycles_t *b = bptr;
	if (*a < *b) return -1;
	if (*a > *b) return 1;
	return 0;

}
#endif

/****************************************************************************** 
 *
 ******************************************************************************/
static void print_report(struct perftest_parameters *user_param) {

	double cycles_to_units;
	cycles_t median;
	int i;
	const char* units;
	cycles_t *delta = malloc((user_param->iters - 1) * sizeof *delta);

	if (!delta) {
		perror("malloc");
		return;
	}

	for (i = 0; i < user_param->iters - 1; ++i)
		delta[i] = tstamp[i + 1] - tstamp[i];


	if (user_param->r_flag->cycles) {
		cycles_to_units = 1;
		units = "cycles";
	} else {
#ifndef _WIN32
		cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
#else
		cycles_to_units = get_cpu_mhz()/1000000;
#endif
		units = "usec";
	}

	if (user_param->r_flag->unsorted) {
		printf("#, %s\n", units);
		for (i = 0; i < user_param->iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	qsort(delta, user_param->iters - 1, sizeof *delta, cycles_compare);

	if (user_param->r_flag->histogram) {
		printf("#, %s\n", units);
		for (i = 0; i < user_param->iters - 1; ++i)
			printf("%d, %g\n", i + 1, delta[i] / cycles_to_units );
	}

	median = get_median(user_param->iters - 1, delta);
	printf(REPORT_FMT_LAT,(unsigned long)user_param->size,user_param->iters,delta[0] / cycles_to_units ,
	       delta[user_param->iters - 2] / cycles_to_units ,median / cycles_to_units );

	free(delta);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int run_iter(struct pingpong_context *ctx, 
			 struct perftest_parameters *user_param) {

	int 					scnt = 0;
	int                     ne;
	struct ibv_send_wr 		*bad_wr = NULL;
	struct ibv_wc 			wc;

	while (scnt < user_param->iters) {
	
		tstamp[scnt] = get_cycles();
		if (ibv_post_send(ctx->qp[0],&ctx->wr[0],&bad_wr)) {
			fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
			return 11;
		}

		scnt++;
	
		if (user_param->use_event) {
			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return 1;
			}
		}

		do {
			ne = ibv_poll_cq(ctx->send_cq, 1, &wc);
			if(ne > 0) { 
				if (wc.status != IBV_WC_SUCCESS) 
					NOTIFY_COMP_ERROR_SEND(wc,scnt,scnt);
			}
		} while (!user_param->use_event && ne == 0);

		if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return 12;
		}
		
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int __cdecl main(int argc, char *argv[]) {

	int ret_parser;
	struct report_options       report;
	struct pingpong_context     ctx;
	struct pingpong_dest        my_dest,rem_dest;
	struct ibv_device           *ib_dev;
	struct perftest_parameters  user_param;
	struct perftest_comm		user_comm;

	/* init default values to user's parameters */
	memset(&ctx,0,sizeof(struct pingpong_context));
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));
	memset(&my_dest,0,sizeof(struct pingpong_dest));
	memset(&rem_dest,0,sizeof(struct pingpong_dest));

	user_param.verb    = ATOMIC;
	user_param.tst     = LAT;
	user_param.r_flag  = &report;
	user_param.version = VERSION;

	ret_parser = parser(&user_param,argv,argc);
	if (ret_parser) { 
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT)
			fprintf(stderr," Parser function exited with Error\n");
		return 1;
	}

	// Finding the IB device selected (or defalut if no selected).
	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE deivce\n");
		return FAILURE;
	}

	// Getting the relevant context from the device
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		return 1;
	}

	// See if MTU and link type are valid and supported.
	if (check_link_and_mtu(ctx.context,&user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		return FAILURE;
	}

	// Print basic test information.
	ctx_print_test_info(&user_param);

	// Allocating arrays needed for the test.
	alloc_ctx(&ctx,&user_param);

	// copy the rellevant user parameters to the comm struct + creating rdma_cm resources.
	if (create_comm_struct(&user_comm,&user_param)) { 
		fprintf(stderr," Unable to create RDMA_CM resources\n");
		return 1;
	}

	// Create (if nessacery) the rdma_cm ids and channel.
	if (user_param.work_rdma_cm == ON) {

		if (create_rdma_resources(&ctx,&user_param)) {
			fprintf(stderr," Unable to create the rdma_resources\n");
			return FAILURE;
	    }

		 if (user_param.machine == CLIENT) {

			if (rdma_client_connect(&ctx,&user_param)) {
				fprintf(stderr,"Unable to perform rdma_client function\n");
				return FAILURE;
			}
		
		} else {

			if (rdma_server_connect(&ctx,&user_param)) {
				fprintf(stderr,"Unable to perform rdma_client function\n");
				return FAILURE;
			}
		}

	} else {

		// create all the basic IB resources (data buffer, PD, MR, CQ and events channel)
	    if (ctx_init(&ctx,&user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			return FAILURE;
	    }
	}

	// Set up the Connection.
	if (set_up_connection(&ctx,&user_param,&my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	} 

	ctx_print_pingpong_data(&my_dest,&user_comm);

	// Init the connection and print the local data.
	if (establish_connection(&user_comm)) {
		fprintf(stderr," Unable to init the socket connection\n");
		return 1;
	}

	// shaking hands and gather the other side info.
	if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to exchange date between server and clients\n");
		return 1;
	}

	user_comm.rdma_params->side = REMOTE;
	ctx_print_pingpong_data(&rem_dest,&user_comm);

	if (user_param.work_rdma_cm == OFF) {

		if (ctx_connect(&ctx,&rem_dest,&user_param,&my_dest)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			return 1;
		}
	}

	// An additional handshake is required after moving qp to RTR.
	if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
    }

	// Only Client post read request. 
	if (user_param.machine == SERVER) {

		if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
		 	fprintf(stderr,"Failed to close connection between server and client\n");
		 	return 1;
		}
		printf(RESULT_LINE);
		return 0;

	}

	if (user_param.use_event) {
		if (ibv_req_notify_cq(ctx.send_cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		} 
	}

	ALLOCATE(tstamp,cycles_t,user_param.iters);
	ctx_set_send_wqes(&ctx,&user_param,&rem_dest);
	printf(RESULT_LINE);
	printf(RESULT_FMT_LAT);

	if(run_iter(&ctx,&user_param))
		return 17;

	print_report(&user_param);

	if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
	 	fprintf(stderr,"Failed to close connection between server and client\n");
	 	return 1;
	}
	
	printf(RESULT_LINE);
	free(tstamp);
	return 0;
}
