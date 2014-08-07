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
	struct ibv_flow			*flow_create_result = NULL;
	struct ibv_flow_attr		*flow_rules = NULL;
#endif	

	union ibv_gid mgid;

	/* init default values to user's parameters */
	memset(&ctx, 0,sizeof(struct pingpong_context));
	memset(&user_param, 0 , sizeof(struct perftest_parameters));
	memset(&my_dest_info, 0 , sizeof(struct raw_ethernet_info));
	memset(&rem_dest_info, 0 , sizeof(struct raw_ethernet_info));

	user_param.verb    = SEND;
	user_param.tst     = BW;
	strncpy(user_param.version, VERSION, sizeof(user_param.version));
	user_param.connection_type = RawEth;

	if (check_flow_steering_support()) {
	    return 1;
	}

	ret_parser = parser(&user_param,argv,argc);

	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT) {
			fprintf(stderr," Parser function exited with Error\n");
		}
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	//Multicast stuff
	if (user_param.raw_mcast)
	{
		//Transform IPv4 to Multicast MAC
		user_param.dest_mac[0] = 0x01;
		user_param.dest_mac[1] = 0x00;
		user_param.dest_mac[2] = 0x5e;
		user_param.dest_mac[3] = (user_param.server_ip >> 8) & 0x7f;
		user_param.dest_mac[4] = (user_param.server_ip >> 16) & 0xff;
		user_param.dest_mac[5] = (user_param.server_ip >> 24) & 0xff;

		/* Build up MGID (128bits, 16bytes) */
		memset (&mgid, 0, sizeof (union ibv_gid));
		memcpy (&mgid.raw[10], &user_param.dest_mac[0], 6);

		//Multicast send so no response UDP port
		user_param.client_port = 0;

	}

	if (user_param.use_rss)
	{
		//if num_of_qps is not even, set it to 2.
		//need to add a check if num_of_qps is in 2^n form.
		if (user_param.num_of_qps % 2)
		{
			user_param.num_of_qps = 2;
		}

		//add another one for rss parent QP
		user_param.num_of_qps += 1; //parent rss + 2^n child_rx
	}

	// Finding the IB device selected (or default if no selected).
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

	if ( !user_param.raw_mcast && (user_param.machine == SERVER || user_param.duplex)) {
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

	//build raw Ethernet packets on ctx buffer
	if((user_param.machine == CLIENT || user_param.duplex) && !user_param.mac_fwd){
		create_raw_eth_pkt(&user_param,&ctx, &my_dest_info , &rem_dest_info);
	}

	// Prepare IB resources for rtr/rts.
	if (user_param.work_rdma_cm == OFF) {
		if (ctx_connect(&ctx,NULL,&user_param,NULL)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
	}

	if (user_param.raw_mcast)
	{
		if (user_param.machine == SERVER)
		{	
			//join Multicast group by MGID
			ibv_attach_mcast(ctx.qp[0], &mgid, 0);
			printf(PERF_RAW_MGID_FMT,"MGID",
				mgid.raw[0], mgid.raw[1],
				mgid.raw[2], mgid.raw[3],
			    mgid.raw[4], mgid.raw[5],
			    mgid.raw[6], mgid.raw[7],
			   	mgid.raw[8], mgid.raw[9],
			    mgid.raw[10],mgid.raw[11],
			    mgid.raw[12],mgid.raw[13],
				mgid.raw[14],mgid.raw[15]);
		}
	}
	else
	{
		//attaching the qp to the spec
		if(user_param.machine == SERVER || user_param.duplex) {
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

		}
	}

	if (user_param.output == FULL_VERBOSITY) {
                printf(RESULT_LINE);
		if (user_param.raw_qos)
			printf((user_param.report_fmt == MBS ? RESULT_FMT_QOS : RESULT_FMT_G_QOS));
		else
			printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));
		printf((user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
	}

	if (user_param.test_method == RUN_REGULAR) {
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

		print_report_bw(&user_param,NULL);
	} else if (user_param.test_method == RUN_INFINITELY) {

		if (user_param.machine == CLIENT)
			ctx_set_send_wqes(&ctx,&user_param,NULL);

		else if (user_param.machine == SERVER) {

			if (ctx_set_recv_wqes(&ctx,&user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				return 1;
			}
		}

		if (user_param.machine == CLIENT) {

			if(run_iter_bw_infinitely(&ctx,&user_param)) {
				fprintf(stderr," Error occured while running infinitely! aborting ...\n");
				return 1;
			}

		} else if (user_param.machine == SERVER) {

			if(run_iter_bw_infinitely_server(&ctx,&user_param)) {
				fprintf(stderr," Error occured while running infinitely on server! aborting ...\n");
				return 1;
			}
		}
	}

	if(user_param.machine == SERVER || user_param.duplex) {

		if (user_param.raw_mcast)
		{
			if (ibv_detach_mcast(ctx.qp[0], &mgid,0))
			{
					perror("error");
					fprintf(stderr,"Couldn't leave multicast group\n");
			}
		}
		else
		{
			#ifdef HAVE_RAW_ETH_EXP
			if (user_param.use_promiscuous) {
				if (ibv_exp_destroy_flow(flow_promisc)) {
					perror("error");
					fprintf(stderr, "Couldn't Destory promisc flow\n");
					return FAILURE;
				}
			}
			#endif

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
		}
	}

	if (destroy_ctx(&ctx, &user_param)) {
		fprintf(stderr,"Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}
	//limit verifier
	if (!user_param.is_bw_limit_passed && (user_param.is_limit_bw == ON ) ) {
                fprintf(stderr,"Error: BW result is below bw limit\n");
                return 1;
        }

	if (user_param.output == FULL_VERBOSITY)
		printf(RESULT_LINE);

	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}
