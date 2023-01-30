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



/******************************************************************************
 *
 ******************************************************************************/
int main(int argc, char *argv[])
{
	struct ibv_device		*ib_dev = NULL;
	struct pingpong_context		ctx;
	struct raw_ethernet_info	*my_dest_info = NULL;
	struct raw_ethernet_info	*rem_dest_info = NULL;
	int				ret_parser;
	struct perftest_parameters	user_param;

	struct ibv_flow			**flow_create_result;
	struct ibv_flow_attr		**flow_rules;
	struct ibv_flow 		**flow_promisc = NULL ;
	#ifdef HAVE_SNIFFER
	struct ibv_flow 		**flow_sniffer = NULL;
	#endif
	int 				flow_index, qp_index;
	union ibv_gid mgid;

	/* init default values to user's parameters */
	memset(&ctx, 0, sizeof(struct pingpong_context));
	memset(&user_param, 0 , sizeof(struct perftest_parameters));

	user_param.verb    = SEND;
	user_param.tst     = BW;
	strncpy(user_param.version, VERSION, sizeof(user_param.version));
	user_param.connection_type = RawEth;

	ret_parser = parser(&user_param, argv, argc);

	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT) {
			fprintf(stderr, " Parser function exited with Error\n");
		}
		DEBUG_LOG(TRACE,"<<<<<<%s", __FUNCTION__);
		goto return_error;
	}

	/* Allocate user input dependable structs */
	MAIN_ALLOC(my_dest_info, struct raw_ethernet_info, user_param.num_of_qps, return_error);
	memset(my_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param.num_of_qps);
	MAIN_ALLOC(rem_dest_info, struct raw_ethernet_info, user_param.num_of_qps, free_my_dest);
	memset(rem_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param.num_of_qps);

	MAIN_ALLOC(flow_create_result, struct ibv_flow*, user_param.flows * user_param.num_of_qps, free_rem_dest);
	MAIN_ALLOC(flow_rules, struct ibv_flow_attr*, user_param.flows * user_param.num_of_qps, free_flow_results);
	#ifdef HAVE_SNIFFER
	MAIN_ALLOC(flow_sniffer, struct ibv_flow*, user_param.num_of_qps, free_flow_rules);
	#endif
	MAIN_ALLOC(flow_promisc, struct ibv_flow*, user_param.num_of_qps, free_flow_sniffer);

	if (user_param.raw_mcast) {
		/* Transform IPv4 to Multicast MAC */
		user_param.dest_mac[0] = 0x01;
		user_param.dest_mac[1] = 0x00;
		user_param.dest_mac[2] = 0x5e;
		user_param.dest_mac[3] = (user_param.server_ip >> 8) & 0x7f;
		user_param.dest_mac[4] = (user_param.server_ip >> 16) & 0xff;
		user_param.dest_mac[5] = (user_param.server_ip >> 24) & 0xff;

		/* Build up MGID (128bits, 16bytes) */
		memset (&mgid, 0, sizeof (union ibv_gid));
		memcpy (&mgid.raw[10], &user_param.dest_mac[0], 6);

		/* Multicast send so no response UDP port */
		user_param.client_port = 0;

	}

	/* Finding the IB device selected (or default if no selected). */
	ib_dev = ctx_find_dev(&user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto free_mem;
	}

	if (check_flow_steering_support(user_param.ib_devname)) {
		goto free_devname;
	}

	/* Getting the relevant context from the device */
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto free_devname;
	}

	/* Verify user parameters that require the device context,
	 * the function will print the relevent error info. */
	if (verify_params_with_device_context(ctx.context, &user_param)) {
		goto free_devname;
	}

	/* See if MTU and link type are valid and supported. */
	if (check_link_and_mtu(ctx.context, &user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto free_devname;
	}

	/* Allocating arrays needed for the test. */
	if (alloc_ctx(&ctx,&user_param)){
		fprintf(stderr, "Couldn't allocate context\n");
		goto free_devname;
	}

	/* set mac address by user choose */
	for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
		if (send_set_up_connection(&flow_rules[qp_index * user_param.flows],
						&ctx, &user_param, &my_dest_info[qp_index], &rem_dest_info[qp_index])) {
			fprintf(stderr, " Unable to set up socket connection\n");
			dealloc_ctx(&ctx, &user_param);
			goto free_devname;
		}
	}

	/* Print basic test information. */
	ctx_print_test_info(&user_param);

	if ( !user_param.raw_mcast && (user_param.machine == SERVER || user_param.duplex)) {
		for (flow_index = 0; flow_index < user_param.flows; flow_index++)
			print_spec(flow_rules[flow_index], &user_param);
	}

	/* create all the basic IB resources (data buffer, PD, MR, CQ and events channel) */
	if (ctx_init(&ctx, &user_param)) {
		fprintf(stderr, " Couldn't create IB resources\n");
		dealloc_ctx(&ctx, &user_param);
		goto free_devname;
	}

	/* build raw Ethernet packets on ctx buffer */
	if ((user_param.machine == CLIENT || user_param.duplex) && !user_param.mac_fwd) {
		for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
			create_raw_eth_pkt(&user_param, &ctx, (void*)ctx.buf[qp_index], &my_dest_info[qp_index], &rem_dest_info[qp_index]);
		}
	}

	/* create flow rules for servers/duplex clients ,  that not test raw_mcast */
	if (!user_param.raw_mcast && (user_param.machine == SERVER || user_param.duplex)) {

		#if defined HAVE_SNIFFER
		if (user_param.use_sniffer) {
			struct ibv_flow_attr attr = {
				.type = IBV_FLOW_ATTR_SNIFFER,
				.num_of_specs = 0,
				.port = user_param.ib_port,
				.flags = 0
			};

			for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
				if ((flow_sniffer[qp_index] = ibv_create_flow(ctx.qp[qp_index], &attr)) == NULL) {
					perror("error");
					fprintf(stderr, "Couldn't attach SNIFFER rule QP\n");
				}
			}
		}
		#endif /* HAVE_SNIFFER */

			if (user_param.use_promiscuous) {
			struct ibv_flow_attr attr = {
				.type = IBV_FLOW_ATTR_ALL_DEFAULT,
				.num_of_specs = 0,
				.port = user_param.ib_port,
				.flags = 0
			};

			for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
				if ((flow_promisc[qp_index] = ibv_create_flow(ctx.qp[qp_index], &attr)) == NULL) {
					perror("error");
					fprintf(stderr, "Couldn't attach promiscuous rule QP\n");
				}
			}
		}

		/* attaching the qp to the spec */
		for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
			for (flow_index = 0; flow_index < user_param.flows; flow_index++) {
				flow_create_result[flow_index + qp_index * user_param.flows] =
					ibv_create_flow(ctx.qp[qp_index], flow_rules[(qp_index * user_param.flows) + flow_index]);

				if (!flow_create_result[flow_index + qp_index * user_param.flows]){
					perror("error");
					fprintf(stderr, "Couldn't attach QP\n");
					goto promisc_flow_destroy;
				}
			}
		}
	}

	/* Prepare IB resources for rtr/rts. */
	if (ctx_connect(&ctx, NULL, &user_param, NULL)) {
		fprintf(stderr, " Unable to Connect the HCA's through the link\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto destroy_ctx;
	}

	if (user_param.raw_mcast) {
		if (user_param.machine == SERVER) {
			/* join Multicast group by MGID */
			for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
				ibv_attach_mcast(ctx.qp[qp_index], &mgid, 0);
				printf(PERF_RAW_MGID_FMT, "MGID",
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
			ctx_set_send_wqes(&ctx,	&user_param, NULL);
		}

		if (user_param.machine == SERVER || user_param.duplex) {
			if (ctx_set_recv_wqes(&ctx, &user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto free_devname;
			}
		}

		if (user_param.mac_fwd) {

			if(run_iter_fw(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto free_devname;
			}

		} else if (user_param.duplex) {

			if(run_iter_bi(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto free_devname;
			}

		} else if (user_param.machine == CLIENT) {

			if(run_iter_bw(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto free_devname;
			}

		} else {

			if(run_iter_bw_server(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto free_devname;
			}
		}

		print_report_bw(&user_param, NULL);
	} else if (user_param.test_method == RUN_INFINITELY) {

		if (user_param.machine == CLIENT)
			ctx_set_send_wqes(&ctx, &user_param, NULL);

		else if (user_param.machine == SERVER) {

			if (ctx_set_recv_wqes(&ctx, &user_param)) {
				fprintf(stderr, "Failed to post receive recv_wqes\n");
				goto free_devname;
			}
		}
		if (user_param.machine == CLIENT) {

			if(run_iter_bw_infinitely(&ctx, &user_param)) {
				fprintf(stderr, " Error occurred while running infinitely! aborting ...\n");
				goto free_devname;
			}

		} else if (user_param.machine == SERVER) {

			if(run_iter_bw_infinitely_server(&ctx, &user_param)) {
				fprintf(stderr, " Error occurred while running infinitely on server! aborting ...\n");
				goto free_devname;
			}
		}
	}

	if(user_param.machine == SERVER || user_param.duplex) {
		/* destroy open flows */
		for (flow_index = 0; flow_index < user_param.flows; flow_index++) {
			for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
				if (ibv_destroy_flow(flow_create_result[flow_index + qp_index * user_param.flows])) {
					perror("error");
					fprintf(stderr, "Couldn't destroy flow\n");
					goto promisc_flow_destroy;
				}
			}
		}

		if (user_param.use_promiscuous) {
			for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
				if (ibv_destroy_flow(flow_promisc[qp_index])) {
					perror("error");
					fprintf(stderr, "Couldn't destroy flow\n");
					goto sniffer_flow_destroy;
				}
			}
		}

		#if defined HAVE_SNIFFER
		if (user_param.use_sniffer) {
			for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
				if (ibv_destroy_flow(flow_sniffer[qp_index])) {
					perror("error");
					fprintf(stderr, "Couldn't destroy sniffer flow\n");
					goto destroy_ctx;
				}
			}
		}
		#endif

		if (user_param.raw_mcast) {
			for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
				if (ibv_detach_mcast(ctx.qp[qp_index], &mgid, 0)) {
					perror("error");
					fprintf(stderr, "Couldn't leave multicast group\n");
				}
			}
		}
	}

	if (destroy_ctx(&ctx, &user_param)) {
		fprintf(stderr, "Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto free_mem;
	}

	free(my_dest_info);
	free(rem_dest_info);
	free(flow_rules);
	free(flow_promisc);
	#if defined HAVE_SNIFFER
	free(flow_sniffer);
	#endif
	free(user_param.ib_devname);

	/* limit verifier */
	if (!user_param.is_bw_limit_passed && (user_param.is_limit_bw == ON ) ) {
		fprintf(stderr, "Error: BW result is below bw limit\n");
		return FAILURE;
	}

	if (user_param.output == FULL_VERBOSITY)
		printf(RESULT_LINE);

	DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
	return SUCCESS;


promisc_flow_destroy:
	if (user_param.use_promiscuous) {
		for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
			if (ibv_destroy_flow(flow_promisc[qp_index])) {
				perror("error");
				fprintf(stderr, "Couldn't destroy flow\n");
				}
			}
		}
sniffer_flow_destroy: __attribute__((unused))
	#if defined HAVE_SNIFFER
	if (user_param.use_sniffer) {
		for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
			if ((ibv_destroy_flow(flow_sniffer[qp_index]))) {
				perror("error");
				fprintf(stderr, "Couldn't attach SNIFFER rule QP\n");
			}
		}
	}
	#endif
destroy_ctx:  __attribute__((unused))
	destroy_ctx(&ctx, &user_param);
free_devname:
	free(user_param.ib_devname);
free_mem:
	free(flow_promisc);
free_flow_sniffer: __attribute__((unused))
	#if defined HAVE_SNIFFER
	free(flow_sniffer);
	#endif
// cppcheck-suppress unusedLabelConfiguration
free_flow_rules:
	free(flow_rules);
free_flow_results:
	free(flow_create_result);
free_rem_dest:
	free(rem_dest_info);
free_my_dest:
	free(my_dest_info);
return_error:
	return FAILURE;
}
