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

#if defined(__FreeBSD__)
#include <sys/types.h>
#include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <netinet/ip.h>
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
	struct ibv_exp_flow		**flow_create_result;
	struct ibv_exp_flow_attr	**flow_rules;
	struct ibv_exp_flow 		*flow_promisc = NULL;
	#else
	struct ibv_flow			**flow_create_result;
	struct ibv_flow_attr		**flow_rules;
	struct ibv_flow 		*flow_promisc = NULL;
	#endif
	struct report_options		report;
	int				i;

	/* allocate memory space for user parameters &*/
	memset(&ctx,		0, sizeof(struct pingpong_context));
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&my_dest_info, 0 , sizeof(struct raw_ethernet_info));
	memset(&rem_dest_info, 0 , sizeof(struct raw_ethernet_info));

	/* init default values to user's parameters that's relvant for this test:
	 * Raw Ethernet Send Latency Test
	 */
	user_param.verb    = SEND;
	user_param.tst     = LAT_BY_BW;
	strncpy(user_param.version, VERSION, sizeof(user_param.version));
	user_param.connection_type = RawEth;
	user_param.r_flag  = &report;


	/* Configure the parameters values according to user
	   arguments or default values. */
	ret_parser = parser(&user_param, argv,argc);

	/* check for parsing errors */
	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT)
			fprintf(stderr," Parser function exited with Error\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}
	#ifdef HAVE_RAW_ETH_EXP
	ALLOCATE(flow_create_result, struct ibv_exp_flow*, user_param.flows);
	ALLOCATE(flow_rules, struct ibv_exp_flow_attr*, user_param.flows);
	#else
	ALLOCATE(flow_create_result, struct ibv_flow*, user_param.flows);
	ALLOCATE(flow_rules, struct ibv_flow_attr*, user_param.flows);
	#endif


	/*this is a bidirectional test, so we need to let the init functions
	 * think we are in duplex mode
	 */
	user_param.duplex  = 1;

	/* Find the selected IB device (or default if the user didn't select one). */
	ib_dev = ctx_find_dev(&user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	if (check_flow_steering_support(user_param.ib_devname)) {
		return FAILURE;
	}

	/* Getting the relevant context from the device */
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	/* Verify user parameters that require the device context,
	 * the function will print the relevent error info. */
	if (verify_params_with_device_context(ctx.context, &user_param)) {
		return FAILURE;
	}

	/* See if MTU and link type are valid and supported. */
	if (check_link_and_mtu(ctx.context, &user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	/* Allocating arrays needed for the test. */
	alloc_ctx(&ctx, &user_param);

	/*set up the connection, return the required flow rules (notice that user_param->duplex == TRUE)
	 * so the function will setup like it's a bidirectional test
	 */
	if (send_set_up_connection(flow_rules, &ctx, &user_param, &my_dest_info, &rem_dest_info)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return FAILURE;
	}

	/* Print basic test information. */
	ctx_print_test_info(&user_param);

	for (i = 0; i < user_param.flows; i++)
		print_spec(flow_rules[i], &user_param);

	/* initalize IB resources (data buffer, PD, MR, CQ and events channel) */
	if (ctx_init(&ctx, &user_param)) {
		fprintf(stderr, " Couldn't create IB resources\n");
		return FAILURE;
	}


	/* attaching the qp to the spec */
	for (i = 0; i < user_param.flows; i++) {
		#ifdef HAVE_RAW_ETH_EXP
		flow_create_result[i] = ibv_exp_create_flow(ctx.qp[0], flow_rules[i]);
		#else
		flow_create_result[i] = ibv_create_flow(ctx.qp[0], flow_rules[i]);
		#endif

		if (!flow_create_result[i]){
			perror("error");
			fprintf(stderr, "Couldn't attach QP\n");
			return FAILURE;
		}
	}

	if (user_param.use_promiscuous) {
		#ifdef HAVE_RAW_ETH_EXP
		struct ibv_exp_flow_attr attr = {
			.type = IBV_EXP_FLOW_ATTR_ALL_DEFAULT,
			.num_of_specs = 0,
			.port = user_param.ib_port,
			.flags = 0
		};

		if ((flow_promisc = ibv_exp_create_flow(ctx.qp[0], &attr)) == NULL) {
			perror("error");
			fprintf(stderr, "Couldn't attach promiscuous rule QP\n");
		}
		#else
		struct ibv_flow_attr attr = {
			.type = IBV_FLOW_ATTR_ALL_DEFAULT,
			.num_of_specs = 0,
			.port = user_param.ib_port,
			.flags = 0
		};

		if ((flow_promisc = ibv_create_flow(ctx.qp[0], &attr)) == NULL) {
			perror("error");
			fprintf(stderr, "Couldn't attach promiscuous rule QP\n");
		}
		#endif
	}

	/* build ONE Raw Ethernet packets on ctx buffer */
	create_raw_eth_pkt(&user_param, &ctx, (void*)ctx.buf[0], &my_dest_info , &rem_dest_info);

	if (user_param.output == FULL_VERBOSITY) {
		printf(RESULT_LINE);
		printf("%s",(user_param.test_type == ITERATIONS) ? RESULT_FMT_LAT : RESULT_FMT_LAT_DUR);
		printf((user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
	}

	/* modify QPs to rtr/rts */
	if (ctx_connect(&ctx, NULL, &user_param, NULL)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	ctx_set_send_wqes(&ctx,&user_param,NULL);

	if (ctx_set_recv_wqes(&ctx,&user_param)) {
		fprintf(stderr," Failed to post receive recv_wqes\n");
		return FAILURE;
	}

	/* latency test function for SEND verb latency test. */
	if (user_param.machine == CLIENT) {
		if (run_iter_lat_burst(&ctx, &user_param))
			return FAILURE;
	}
	else {
		if (run_iter_lat_burst_server(&ctx, &user_param))
			return FAILURE;
	}

	/* print report (like print_report_bw) in the correct format
	 * (as set before: FMT_LAT or FMT_LAT_DUR)
	 */
	if (user_param.machine == CLIENT)
		print_report_lat(&user_param);

	/* destroy promisc flow */
	if (user_param.use_promiscuous) {
		#ifdef HAVE_RAW_ETH_EXP
		if (ibv_exp_destroy_flow(flow_promisc)) {
		#else
		if (ibv_destroy_flow(flow_promisc)) {
		#endif
			perror("error");
			fprintf(stderr, "Couldn't destroy promisc flow\n");
			return FAILURE;
		}
	}

	/* destroy flow */
	for (i = 0; i < user_param.flows; i++) {
		#ifdef HAVE_RAW_ETH_EXP
		if (ibv_exp_destroy_flow(flow_create_result[i])) {
		#else
		if (ibv_destroy_flow(flow_create_result[i])) {
		#endif
			perror("error");
			fprintf(stderr, "Couldn't destroy flow\n");
			return FAILURE;
		}

		free(flow_rules[i]);
	}

	/* Deallocate all perftest resources. */
	if (destroy_ctx(&ctx, &user_param)) {
		fprintf(stderr,"Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	if (user_param.output == FULL_VERBOSITY)
		printf(RESULT_LINE);

	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return SUCCESS;
}
