/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2006 Mellanox Technologies Ltd.  All rights reserved.
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

#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "perftest_communication.h"

/******************************************************************************
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	int ret_parser,i;
	struct ibv_device		   *ib_dev = NULL;
	struct pingpong_context    ctx;
	struct pingpong_dest       *my_dest = NULL;
	struct pingpong_dest	   *rem_dest = NULL;
	struct perftest_parameters user_param;
	struct perftest_comm	   user_comm;

	/* init default values to user's parameters */
	memset(&ctx,0,sizeof(struct pingpong_context));
	memset(&user_param , 0 , sizeof(struct perftest_parameters));
	memset(&user_comm,0,sizeof(struct perftest_comm));

	user_param.verb    = ATOMIC;
	user_param.tst     = BW;
	user_param.version = VERSION;

	ret_parser = parser(&user_param,argv,argc);
	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT)
			fprintf(stderr," Parser function exited with Error\n");
		return 1;
	}

	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev)
		return 7;

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

	ALLOCATE(my_dest , struct pingpong_dest , user_param.num_of_qps);
	memset(my_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);
	ALLOCATE(rem_dest , struct pingpong_dest , user_param.num_of_qps);
	memset(rem_dest, 0, sizeof(struct pingpong_dest)*user_param.num_of_qps);

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

		// create all the basic IB resources.
	    if (ctx_init(&ctx,&user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			return FAILURE;
	    }
	}

	// Set up the Connection.
	if (set_up_connection(&ctx,&user_param,my_dest)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return FAILURE;
	}

	// Print this machine QP information
	for (i=0; i < user_param.num_of_qps; i++)
		ctx_print_pingpong_data(&my_dest[i],&user_comm);

	// Init the connection and print the local data.
	if (establish_connection(&user_comm)) {
		fprintf(stderr," Unable to init the socket connection\n");
		return 1;
	}

	user_comm.rdma_params->side = REMOTE;

	for (i=0; i < user_param.num_of_qps; i++) {

		// shaking hands and gather the other side info.
		if (ctx_hand_shake(&user_comm,&my_dest[i],&rem_dest[i])) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1;
		}
		ctx_print_pingpong_data(&rem_dest[i],&user_comm);
	}

	if (user_param.work_rdma_cm == OFF) {

		if (ctx_connect(&ctx,rem_dest,&user_param,my_dest)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			return 1;
		}
	}

	// An additional handshake is required after moving qp to RTR.
	if (ctx_hand_shake(&user_comm,&my_dest[0],&rem_dest[0])) {
        fprintf(stderr,"Failed to exchange date between server and clients\n");
        return 1;
    }

	// For half duplex tests, server just waits for client to exit
	if (user_param.machine == SERVER && !user_param.duplex) {

		if (ctx_close_connection(&user_comm,&my_dest[0],&rem_dest[0])) {
			fprintf(stderr,"Failed to close connection between server and client\n");
			return 1;
		}

		printf(RESULT_LINE);
		return destroy_ctx(&ctx,&user_param);
	}

	if (user_param.use_event) {
		if (ibv_req_notify_cq(ctx.send_cq, 0)) {
			fprintf(stderr, "Couldn't request CQ notification\n");
			return 1;
		}
	}

	printf(RESULT_LINE);
	printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));

	ctx_set_send_wqes(&ctx,&user_param,rem_dest);

	if (user_param.test_method == RUN_REGULAR || user_param.test_method == RUN_ALL) {

		if(run_iter_bw(&ctx,&user_param)) {
			fprintf(stderr," Error occured in run_iter function\n");
			return 1;
		}

		print_report_bw(&user_param);

	} else if (user_param.test_method == RUN_INFINITELY) {

		if(run_iter_bw_infinitely(&ctx,&user_param)) {
			fprintf(stderr," Error occured while running infinitely! aborting ...\n");
			return 1;
		}
	}

	if (ctx_close_connection(&user_comm,&my_dest[0],&rem_dest[0])) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}

	//limit verifier
	//TODO: check which value should I return
	if ( !(user_param.is_msgrate_limit_passed && user_param.is_bw_limit_passed) )
		return 1;

	printf(RESULT_LINE);
	return destroy_ctx(&ctx,&user_param);
}
