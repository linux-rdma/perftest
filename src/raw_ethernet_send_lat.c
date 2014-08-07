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


/*
* Main function. implements raw_ethernet_send_lat
*/
int main(int argc, char *argv[])
{

	struct ibv_device		*ib_dev = NULL;
	struct pingpong_context		ctx;
	struct raw_ethernet_info	my_dest_info,rem_dest_info;
	int				ret_parser;
	struct perftest_parameters	user_param;
#ifdef HAVE_RAW_ETH_EXP
	struct ibv_exp_flow		*flow_create_result = NULL;
	struct ibv_exp_flow_attr	*flow_rules = NULL;
	struct ibv_exp_flow 		*flow_promisc = NULL;
#else
	struct ibv_flow		*flow_create_result = NULL;
	struct ibv_flow_attr	*flow_rules = NULL;
#endif
	struct report_options		report;

	//allocate memory space for user parameters
	memset(&ctx,		0, sizeof(struct pingpong_context));
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&my_dest_info, 0 , sizeof(struct raw_ethernet_info));
	memset(&rem_dest_info, 0 , sizeof(struct raw_ethernet_info));

	/* init default values to user's parameters that's relvant for this test:
	* Raw Ethernet Send Latency Test
	*/
	user_param.verb    = SEND;
	user_param.tst     = LAT;
	strncpy(user_param.version, VERSION, sizeof(user_param.version));
	user_param.connection_type = RawEth;
	user_param.r_flag  = &report;

	if (check_flow_steering_support()) {
            return 1;
        }

	/* Configure the parameters values according to user
												arguments or default values. */
	ret_parser = parser(&user_param, argv,argc);

	//check for parsing errors
	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT)
			fprintf(stderr," Parser function exited with Error\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	//this is a bidirectional test, so we need to let the init functions
	//to think we are in duplex mode
	//TODO: ask Ido if that's ok, or should I add another field in user_param
	user_param.duplex  = 1;


	// Find the selected IB device (or default if the user didn't select one).
	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE device\n");
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
	if (check_link_and_mtu(ctx.context, &user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	// Allocating arrays needed for the test.
	alloc_ctx(&ctx, &user_param);

	// Print basic test information.
	ctx_print_test_info(&user_param);

	//set up the connection, return the required flow rules (notice that user_param->duplex == TRUE)
	//so the function will setup like it's a bidirectional test
	if (send_set_up_connection(&flow_rules, &ctx, &user_param, &my_dest_info, &rem_dest_info)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}

	//print specifications of the test
	print_spec(flow_rules,&user_param);

	// Create (if necessary) the rdma_cm ids and channel.
	if (user_param.work_rdma_cm == ON) {

		//create resources
		if (create_rdma_resources(&ctx, &user_param)) {
			fprintf(stderr," Unable to create the rdma_resources\n");
			return FAILURE;
		}

		if (user_param.machine == CLIENT) {

			//Connects the client to a QP on the other machine with rdma_cm
			if (rdma_client_connect(&ctx, &user_param)) {
				fprintf(stderr,"Unable to perform rdma_client function\n");
				return FAILURE;
			}

		} else if (rdma_server_connect(&ctx, &user_param)) {
			//Assigning a server to listen on rdma_cm port and connect to it.
			fprintf(stderr,"Unable to perform rdma_server function\n");
			return FAILURE;
		}

	} else {

		// initalize IB resources (data buffer, PD, MR, CQ and events channel)
		if (ctx_init(&ctx, &user_param)) {
			fprintf(stderr, " Couldn't create IB resources\n");
			return FAILURE;
		}
	}


	//attaching the qp to the spec
#ifdef HAVE_RAW_ETH_EXP
	flow_create_result = ibv_exp_create_flow(ctx.qp[0], flow_rules);
#else
	flow_create_result = ibv_create_flow(ctx.qp[0], flow_rules);
#endif
	if (!flow_create_result){
		perror("error");
		fprintf(stderr, "Couldn't attach QP\n");
		return FAILURE;
	}

#ifdef HAVE_RAW_ETH_EXP
	if (user_param.use_promiscuous) {
		struct ibv_exp_flow_attr attr = {
			.type = IBV_EXP_FLOW_ATTR_ALL_DEFAULT,
			.num_of_specs = 0,
			.port = user_param.ib_port,
			.flags = 0
		};

		if ((flow_promisc = ibv_exp_create_flow(ctx.qp[0], &attr)) == NULL) {
			perror("error");
			fprintf(stderr, "Couldn't attach promiscous rule QP\n");
		}
	}
#endif

	//build ONE Raw Ethernet packets on ctx buffer
	create_raw_eth_pkt(&user_param,&ctx, &my_dest_info , &rem_dest_info);

	if (user_param.output == FULL_VERBOSITY) {
                printf(RESULT_LINE);
                printf("%s",(user_param.test_type == ITERATIONS) ? RESULT_FMT_LAT : RESULT_FMT_LAT_DUR);
                printf((user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
        }

	// Prepare IB resources for rtr(ready to read)/rts(ready to send)
	if (user_param.work_rdma_cm == OFF) {
		if (ctx_connect(&ctx, NULL, &user_param, NULL)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
	}


	//Post Send send_wqes for current message size
	ctx_set_send_wqes(&ctx,&user_param,NULL);

	// Post receive recv_wqes for current message size
	if (ctx_set_recv_wqes(&ctx,&user_param)) {
		fprintf(stderr," Failed to post receive recv_wqes\n");
		return 1;
	}

	//latency test function for SEND verb latency test.
	if (run_iter_lat_send(&ctx, &user_param))
	{
		return 17;
	}

	//print report (like print_report_bw) in the correct format
	// (as set before: FMT_LAT or FMT_LAT_DUR)
	user_param.test_type == ITERATIONS ? print_report_lat(&user_param) :
										print_report_lat_duration(&user_param);

	//destory promisc flow
#ifdef HAVE_RAW_ETH_EXP
	if (user_param.use_promiscuous) {
		if (ibv_exp_destroy_flow(flow_promisc)) {
			perror("error");
			fprintf(stderr, "Couldn't Destory promisc flow\n");
			return FAILURE;
		}
	}
#endif


	//destroy flow
#ifdef HAVE_RAW_ETH_EXP
	if (ibv_exp_destroy_flow(flow_create_result)) {
#else
	if (ibv_destroy_flow(flow_create_result)) {
#endif
		perror("error");
		fprintf(stderr, "Couldn't Destory flow\n");
		return FAILURE;
	}
	free(flow_rules);



	//Deallocate all perftest resources.
	if (destroy_ctx(&ctx, &user_param)) {
		fprintf(stderr,"Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	if (user_param.output == FULL_VERBOSITY)
		printf(RESULT_LINE);

	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;

}
