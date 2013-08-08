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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>

#include "get_clock.h"
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "perftest_communication.h"

/******************************************************************************
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

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

	ctx_set_send_wqes(&ctx,&user_param,&rem_dest);
	printf(RESULT_LINE);
	printf("%s",(user_param.test_type == ITERATIONS) ? RESULT_FMT_LAT : RESULT_FMT_LAT_DUR);

	if(run_iter_lat(&ctx,&user_param))
		return 17;

	user_param.test_type == ITERATIONS ? print_report_lat(&user_param) : print_report_lat_duration(&user_param);

	if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
	 	fprintf(stderr,"Failed to close connection between server and client\n");
	 	return 1;
	}

	printf(RESULT_LINE);
	return 0;
}
