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
#include <inttypes.h>
#include <netinet/ip.h>
#include <poll.h>
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "multicast_resources.h"
#include "perftest_communication.h"
#include "raw_ethernet_resources.h"
#include "raw_ethernet_steering.h"


/******************************************************************************
 *
 ******************************************************************************/
static void print_recv_convergence_note(struct perftest_parameters *user_param)
{
	int recv_qps = user_param->flows < user_param->num_of_qps ?
		       user_param->flows : user_param->num_of_qps;
	int flows_can_spread = !user_param->raw_mcast &&
			       !user_param->use_sniffer &&
			       !user_param->duplex &&
			       user_param->post_list == 1;

	if (user_param->raw_mcast || user_param->use_sniffer)
		recv_qps = 1;
	if (user_param->output != FULL_VERBOSITY ||
	    user_param->num_of_qps <= recv_qps)
		return;

	printf(" Note: %d send QPs converge on %d receive QP%s",
	       user_param->num_of_qps, recv_qps, recv_qps == 1 ? "" : "s");
	if (flows_can_spread) {
		const char *required_options =
			user_param->is_server_port && user_param->is_client_port ? "" :
			user_param->is_server_ip && user_param->is_client_ip ?
			" with -K/-k" : " with -J/-j and -K/-k";

		printf("; use --flows %d%s to spread receive traffic",
		       user_param->num_of_qps, required_options);
	}
	printf("\n");
}

static int set_receive_capacity(struct pingpong_context *ctx,
				struct perftest_parameters *user_param,
				struct raw_eth_steering_plan *steering)
{
	struct ibv_device_attr device_attr;
	uint64_t rules_per_owner;
	uint64_t qps_per_owner;
	uint64_t recv_qp_depth;
	uint64_t max_recv_qp_depth;
	int normal_rules = 0;
	int i;

	for (i = 0; i < steering->num_rules; i++) {
		if (steering->rules[i].attr->type == IBV_FLOW_ATTR_NORMAL)
			normal_rules++;
	}

	if (!steering->num_recv_qps) {
		fprintf(stderr, " Raw Ethernet receive steering has no owner QP\n");
		return FAILURE;
	}

	if (normal_rules) {
		rules_per_owner = ((uint64_t)normal_rules + steering->num_recv_qps - 1) /
				  steering->num_recv_qps;
		qps_per_owner = ((uint64_t)user_param->num_of_qps * rules_per_owner +
				 normal_rules - 1) / normal_rules;
	} else {
		qps_per_owner = user_param->num_of_qps;
	}
	recv_qp_depth = (uint64_t)user_param->rx_depth * qps_per_owner;

	if (ibv_query_device(ctx->context, &device_attr)) {
		fprintf(stderr, " Failed to query receive queue limits\n");
		return FAILURE;
	}

	max_recv_qp_depth = device_attr.max_qp_wr;
	if (max_recv_qp_depth >
	    (uint64_t)device_attr.max_cqe / steering->num_recv_qps)
		max_recv_qp_depth =
			(uint64_t)device_attr.max_cqe / steering->num_recv_qps;

	if (max_recv_qp_depth < (uint64_t)user_param->rx_depth) {
		fprintf(stderr,
			" RX depth %d exceeds the device receive limit of %" PRIu64
			" per owner QP; use a lower -r\n",
			user_param->rx_depth, max_recv_qp_depth);
		return FAILURE;
	}

	if (recv_qp_depth > max_recv_qp_depth) {
		fprintf(stderr,
			" Warning: limiting owner QP RX depth from %" PRIu64
			" to %" PRIu64 " due to device limits; use a lower -r"
			" to reduce requested receive headroom\n",
			recv_qp_depth, max_recv_qp_depth);
		recv_qp_depth = max_recv_qp_depth;
	}

	ctx->rposted = (int)recv_qp_depth;
	return SUCCESS;
}

int main(int argc, char *argv[])
{
	struct ibv_device		*ib_dev = NULL;
	struct pingpong_context		ctx;
	struct raw_ethernet_info	*my_dest_info = NULL;
	struct raw_ethernet_info	*rem_dest_info = NULL;
	int				ret_parser;
	struct perftest_parameters	user_param;
	struct raw_eth_steering_plan	steering = {0};
	int				is_receiver;
	int				flow_index, qp_index;

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

	is_receiver = user_param.machine == SERVER || user_param.duplex;

	/* Allocate user input dependable structs */
	MAIN_ALLOC(my_dest_info, struct raw_ethernet_info, user_param.num_of_qps, return_error);
	memset(my_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param.num_of_qps);
	MAIN_ALLOC(rem_dest_info, struct raw_ethernet_info, user_param.num_of_qps, free_my_dest);
	memset(rem_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param.num_of_qps);

	if (user_param.raw_mcast) {
		/* Transform IPv4 to Multicast MAC */
		user_param.dest_mac[0] = 0x01;
		user_param.dest_mac[1] = 0x00;
		user_param.dest_mac[2] = 0x5e;
		user_param.dest_mac[3] = (user_param.server_ip >> 8) & 0x7f;
		user_param.dest_mac[4] = (user_param.server_ip >> 16) & 0xff;
		user_param.dest_mac[5] = (user_param.server_ip >> 24) & 0xff;

		/* Multicast send so no response UDP port */
		user_param.client_port = 0;

	}

	/* Finding the IB device selected (or default if no selected). */
	ib_dev = ctx_find_dev(&user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto free_rem_dest;
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
	 * the function will print the relevant error info. */
	if (verify_params_with_device_context(ctx.context, &user_param)) {
		goto free_devname;
	}

	/* See if MTU and link type are valid and supported. */
	if (check_link_and_mtu(ctx.context, &user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto free_devname;
	}

	if (is_receiver) {
		if (raw_eth_steering_plan_build(&steering, &user_param))
			goto free_devname;
		ctx.active_recv_qps = steering.num_recv_qps;
		if (set_receive_capacity(&ctx, &user_param, &steering))
			goto free_steering;
	}

	/* Allocating arrays needed for the test. */
	if (alloc_ctx(&ctx,&user_param)){
		fprintf(stderr, "Couldn't allocate context\n");
		goto free_steering;
	}

	/* set mac address by user choose */
	for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++)
		send_set_up_connection(NULL, &ctx, &user_param,
				       &my_dest_info[qp_index], &rem_dest_info[qp_index]);

	/* Print basic test information. */
	ctx_print_test_info(&user_param);
	print_recv_convergence_note(&user_param);

	/* create all the basic IB resources (data buffer, PD, MR, CQ and events channel) */
	if (ctx_init(&ctx, &user_param)) {
		fprintf(stderr, " Couldn't create IB resources\n");
		dealloc_ctx(&ctx, &user_param);
		goto free_steering;
	}

	/* Keep every QP's generated flows in its own send-buffer region. */
	if (!user_param.mr_per_qp && user_param.flows > 1) {
		for (qp_index = 1; qp_index < user_param.num_of_qps; qp_index++)
			ctx.buf[qp_index] = (char *)ctx.buf[0] + qp_index * ctx.send_qp_buff_size;
	}

	if (is_receiver) {
		for (flow_index = 0; flow_index < steering.num_rules; flow_index++) {
			if (steering.rules[flow_index].attr->type == IBV_FLOW_ATTR_NORMAL)
				print_spec(steering.rules[flow_index].attr, &user_param);
		}
	}

	/* build raw Ethernet packets on ctx buffer */
	if ((user_param.machine == CLIENT || user_param.duplex) && !user_param.mac_fwd) {
		for (qp_index = 0; qp_index < user_param.num_of_qps; qp_index++) {
			create_raw_eth_pkt(&user_param, &ctx, (void*)ctx.buf[qp_index], &my_dest_info[qp_index], &rem_dest_info[qp_index]);
		}
	}

	if (is_receiver && raw_eth_steering_install_flows(&steering, ctx.qp))
		goto uninstall_steering;

	/* Prepare IB resources for rtr/rts. */
	if (ctx_connect(&ctx, NULL, &user_param, NULL)) {
		fprintf(stderr, " Unable to Connect the HCA's through the link\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto uninstall_steering;
	}

	if (is_receiver && user_param.raw_mcast &&
	    raw_eth_steering_install_mcast(&steering, ctx.qp))
		goto uninstall_steering;

	if (user_param.output == FULL_VERBOSITY) {
		printf(RESULT_LINE);
		if (user_param.raw_qos && user_param.report_min_bw)
			printf((user_param.report_fmt == MBS ? RESULT_FMT_QOS_MINBW : RESULT_FMT_G_QOS_MINBW));
		else if (user_param.raw_qos)
			printf((user_param.report_fmt == MBS ? RESULT_FMT_QOS : RESULT_FMT_G_QOS));
		else if (user_param.report_min_bw)
			printf((user_param.report_fmt == MBS ? RESULT_FMT_MINBW : RESULT_FMT_G_MINBW));
		else
			printf((user_param.report_fmt == MBS ? RESULT_FMT : RESULT_FMT_G));
		printf((user_param.cpu_util_data.enable ? RESULT_EXT_CPU_UTIL : RESULT_EXT));
	}

	if (user_param.test_method == RUN_REGULAR) {
		if (user_param.machine == CLIENT || user_param.duplex) {
			ctx_set_send_wqes(&ctx,	&user_param, NULL);
		}

		if (is_receiver) {
			if (ctx_set_recv_wqes(&ctx, &user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto uninstall_steering;
			}
		}

		if (user_param.mac_fwd) {

			if(run_iter_fw(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto uninstall_steering;
			}

		} else if (user_param.duplex) {

			if(run_iter_bi(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto uninstall_steering;
			}

		} else if (user_param.machine == CLIENT) {

			if(run_iter_bw(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto uninstall_steering;
			}

		} else {

			if(run_iter_bw_server(&ctx, &user_param)) {
				DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
				goto uninstall_steering;
			}
		}

		print_report_bw(&user_param, NULL);
	} else if (user_param.test_method == RUN_INFINITELY) {

		if (user_param.machine == CLIENT)
			ctx_set_send_wqes(&ctx, &user_param, NULL);

		else if (user_param.machine == SERVER) {

			if (ctx_set_recv_wqes(&ctx, &user_param)) {
				fprintf(stderr, "Failed to post receive recv_wqes\n");
				goto uninstall_steering;
			}
		}
		if (user_param.machine == CLIENT) {

			if(run_iter_bw_infinitely(&ctx, &user_param)) {
				fprintf(stderr, " Error occurred while running infinitely! aborting ...\n");
				goto uninstall_steering;
			}

		} else if (user_param.machine == SERVER) {

			if(run_iter_bw_infinitely_server(&ctx, &user_param)) {
				fprintf(stderr, " Error occurred while running infinitely on server! aborting ...\n");
				goto uninstall_steering;
			}
		}
	}

	if (raw_eth_steering_uninstall(&steering, ctx.qp))
		goto destroy_ctx;

	if (destroy_ctx(&ctx, &user_param)) {
		fprintf(stderr, "Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		goto free_steering;
	}

	raw_eth_steering_plan_free(&steering);
	free(my_dest_info);
	free(rem_dest_info);
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

uninstall_steering:
	raw_eth_steering_uninstall(&steering, ctx.qp);
destroy_ctx:
	destroy_ctx(&ctx, &user_param);
free_steering:
	raw_eth_steering_plan_free(&steering);
free_devname:
	free(user_param.ib_devname);
free_rem_dest:
	free(rem_dest_info);
free_my_dest:
	free(my_dest_info);
return_error:
	return FAILURE;
}
