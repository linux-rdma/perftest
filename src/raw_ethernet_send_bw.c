/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005 Mellanox Technologies Ltd.  All rights reserved.
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
#include <signal.h>
#include <getopt.h>
#include </usr/include/netinet/ip.h>
#include <poll.h>
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "multicast_resources.h"
#include "perftest_communication.h"
#include "raw_ethernet_resources.h"



/******************************************************************************
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	struct ibv_device			*ib_dev = NULL;
	struct pingpong_context		ctx;
	struct raw_ethernet_info	my_dest_info,rem_dest_info;
	int							ret_parser;
	struct perftest_parameters	user_param;
	struct ibv_flow				*flow_create_result = NULL;
	struct ibv_flow_attr		*flow_rules = NULL;

	/* init default values to user's parameters */
	memset(&ctx, 0,sizeof(struct pingpong_context));
	memset(&user_param, 0 , sizeof(struct perftest_parameters));
	memset(&my_dest_info, 0 , sizeof(struct raw_ethernet_info));
	memset(&rem_dest_info, 0 , sizeof(struct raw_ethernet_info));

	user_param.verb    = SEND;
	user_param.tst     = BW;
	user_param.version = VERSION;
	user_param.connection_type = RawEth;

	ret_parser = parser(&user_param,argv,argc);

	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT) {
			fprintf(stderr," Parser function exited with Error\n");
		}
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}
	// Finding the IB device selected (or default if no selected).
	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE deivce\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
 		return 1;
	}
	// Getting the relevant context from the device
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}
	// See if MTU and link type are valid and supported.
	if (check_link_and_mtu(ctx.context,&user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}
	// Allocating arrays needed for the test.
	alloc_ctx(&ctx,&user_param);
	// Print basic test information.
	ctx_print_test_info(&user_param);

	//set mac address by user choose
	if (send_set_up_connection(&flow_rules,&ctx,&user_param,&my_dest_info,&rem_dest_info)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}

	if(user_param.machine == SERVER || user_param.duplex) {
		print_spec(flow_rules,&user_param);
	}

	// Create (if necessary) the rdma_cm ids and channel.
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

		} else if (rdma_server_connect(&ctx,&user_param)) {
			fprintf(stderr,"Unable to perform rdma_client function\n");
			return FAILURE;
		}

	} else {

		// create all the basic IB resources (data buffer, PD, MR, CQ and events channel)
		if (ctx_init(&ctx,&user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			return FAILURE;
		}
	}

	//attaching the qp to the spec
	if(user_param.machine == SERVER || user_param.duplex) {
		flow_create_result = ibv_create_flow(ctx.qp[0], flow_rules);
		if (!flow_create_result){
			perror("error");
			fprintf(stderr, "Couldn't attach QP\n");
			return FAILURE;
		}
	}

	//build raw Ethernet packets on ctx buffer
	if((user_param.machine == CLIENT || user_param.duplex) && !user_param.mac_fwd){
		create_raw_eth_pkt(&user_param,&ctx, &my_dest_info , &rem_dest_info);
	}

	printf(RESULT_LINE);//change the printing of the test
	printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));

	// Prepare IB resources for rtr/rts.
	if (user_param.work_rdma_cm == OFF) {
		if (ctx_connect(&ctx,NULL,&user_param,NULL)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
	}

	if (user_param.machine == CLIENT || user_param.duplex) {
		ctx_set_send_wqes(&ctx,&user_param,NULL);
	}

	if (user_param.machine == SERVER || user_param.duplex) {
		if (ctx_set_recv_wqes(&ctx,&user_param)) {
			fprintf(stderr," Failed to post receive recv_wqes\n");
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
	}

	if (user_param.mac_fwd) {

		if(run_iter_fw(&ctx,&user_param)) {
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return FAILURE;
		}

	} else if (user_param.duplex) {

		if(run_iter_bi(&ctx,&user_param)) {
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return FAILURE;
		}

	} else if (user_param.machine == CLIENT) {

		if(run_iter_bw(&ctx,&user_param)) {
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return FAILURE;
		}

	} else {

		if(run_iter_bw_server(&ctx,&user_param)) {
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 17;
		}
	}

	print_report_bw(&user_param);

	if(user_param.machine == SERVER || user_param.duplex) {

		if (ibv_destroy_flow(flow_create_result)) {
			perror("error");
			fprintf(stderr, "Couldn't Destory flow\n");
			return FAILURE;
		}
		free(flow_rules);
	}

	if (destroy_ctx(&ctx, &user_param)) {
		fprintf(stderr,"Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	//limit verifier
	//TODO: check which value should I return
	if ( !(user_param.is_msgrate_limit_passed && user_param.is_bw_limit_passed) )
		return 1;

	printf(RESULT_LINE);
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}

