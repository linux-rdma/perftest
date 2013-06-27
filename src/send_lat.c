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
#include "multicast_resources.h"
#include "perftest_communication.h"

/******************************************************************************
 *
 ******************************************************************************/
static int set_mcast_group(struct pingpong_context *ctx,
						   struct perftest_parameters *user_parm,
						   struct mcast_parameters *mcg_params) {

	int i;
	struct ibv_port_attr port_attr;

	if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&mcg_params->port_gid)) {
			return 1;
	}

	if (ibv_query_pkey(ctx->context,user_parm->ib_port,DEF_PKEY_IDX,&mcg_params->pkey)) {
		return 1;
	}

	if (ibv_query_port(ctx->context,user_parm->ib_port,&port_attr)) {
		return 1;
	}
	mcg_params->sm_lid  = port_attr.sm_lid;
	mcg_params->sm_sl   = port_attr.sm_sl;
	mcg_params->ib_port = user_parm->ib_port;
	mcg_params->user_mgid = user_parm->user_mgid;
	set_multicast_gid(mcg_params,ctx->qp[0]->qp_num,(int)user_parm->machine);

	if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
		// Request for Mcast group create registery in SM.
		if (join_multicast_group(SUBN_ADM_METHOD_SET,mcg_params)) {
			fprintf(stderr," Failed to Join Mcast request\n");
			return 1;
		}
	}

	for (i=0; i < user_parm->num_of_qps; i++) {

		if (ibv_attach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
			fprintf(stderr, "Couldn't attach QP to MultiCast group");
			return 1;
		}
	}
	mcg_params->mcast_state |= MCAST_IS_ATTACHED;

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int destroy_mcast_group(struct pingpong_context *ctx,
							   struct perftest_parameters *user_parm,
						       struct mcast_parameters *mcg_params) {
	int i;

	for (i = 0; i < user_parm->num_of_qps; i++) {
		if (ibv_detach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
			fprintf(stderr, "Couldn't deattach QP from MultiCast group\n");
			return 1;
		}
	}
	if (!strcmp(link_layer_str(user_parm->link_type),"IB")) {
		// Removal Request for Mcast group in SM.
		if (join_multicast_group(SUBN_ADM_METHOD_DELETE,mcg_params)) {
			fprintf(stderr,"Couldn't Unregister the Mcast group on the SM\n");
			return 1;
		}
	}

	mcg_params->mcast_state &= ~MCAST_IS_ATTACHED;
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int send_set_up_connection(struct pingpong_context *ctx,
								  struct perftest_parameters *user_parm,
								  struct pingpong_dest *my_dest,
								  struct mcast_parameters *mcg_params,
								  struct perftest_comm *comm) {

	if (user_parm->use_mcg) {

		if (set_mcast_group(ctx,user_parm,mcg_params)) {
			return 1;
		}

		my_dest->gid = mcg_params->mgid;
		my_dest->lid = mcg_params->mlid;
		my_dest->qpn = QPNUM_MCAST;

	} else {
		if (user_parm->gid_index != -1) {
			if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&my_dest->gid)) {
				return -1;
			}
		}
		my_dest->lid   	   = ctx_get_local_lid(ctx->context,user_parm->ib_port);
		my_dest->qpn   	   = ctx->qp[0]->qp_num;
	}

	my_dest->psn       = lrand48() & 0xffffff;

	// We do not fail test upon lid above RoCE.
	if (user_parm->gid_index < 0) {
		if (!my_dest->lid) {
			fprintf(stderr," Local lid 0x0 detected,without any use of gid. Is SM running?\n");
			return -1;
		}
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int send_destroy_ctx_resources(struct pingpong_context    *ctx,
								 struct perftest_parameters *user_parm,
								 struct mcast_parameters    *mcg_params)  {

	int i,test_result = 0;

	if (user_parm->use_mcg) {
		if (destroy_mcast_group(ctx,user_parm,mcg_params)) {
			fprintf(stderr, "failed to destroy MultiCast resources\n");
			test_result = 1;
		}
	}

	if (user_parm->connection_type == UD) {

		if (ibv_destroy_ah(ctx->ah[0])) {
			fprintf(stderr, "failed to destroy AH\n");
			test_result = 1;
		}
		free(ctx->ah);
	}

	for(i = 0; i < user_parm->num_of_qps; i++) {
		if (ibv_destroy_qp(ctx->qp[i])) {
			fprintf(stderr, "failed to destroy QP\n");
			test_result = 1;
		}
	}
	free(ctx->qp);

	if (ibv_destroy_cq(ctx->send_cq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}

	if (ibv_destroy_cq(ctx->recv_cq)) {
		fprintf(stderr, "failed to destroy CQ\n");
		test_result = 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "failed to deregister MR\n");
		test_result = 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "failed to deallocate PD\n");
		test_result = 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "failed to destroy channel \n");
			test_result = 1;
		}
	}

	if (user_parm->work_rdma_cm == OFF) {

		if (ibv_close_device(ctx->context)) {
			fprintf(stderr, "failed to close device context\n");
			test_result = 1;
		}
	}
	return test_result;
}

/******************************************************************************
 *
 ******************************************************************************/
int main(int argc, char *argv[]) {

	int                        i = 0;
	int                        size_max_pow = 24;
	int						   ret_val;
	struct report_options      report;
	struct pingpong_context    ctx;
	struct pingpong_dest	   my_dest,rem_dest;
	struct mcast_parameters	   mcg_params;
	struct ibv_device          *ib_dev = NULL;
	struct perftest_parameters user_param;
	struct perftest_comm	   user_comm;

	/* init default values to user's parameters */
	memset(&ctx,		0, sizeof(struct pingpong_context));
	memset(&user_param, 0, sizeof(struct perftest_parameters));
	memset(&user_comm , 0, sizeof(struct perftest_comm));
	memset(&mcg_params, 0, sizeof(struct mcast_parameters));
	memset(&my_dest, 0 , sizeof(struct pingpong_dest));
	memset(&rem_dest, 0 , sizeof(struct pingpong_dest));

	user_param.verb    = SEND;
	user_param.tst     = LAT;
	user_param.version = VERSION;
	user_param.r_flag  = &report;

	// Configure the parameters values according to user arguments or defalut values.
	ret_val = parser(&user_param,argv,argc);
	if (ret_val) {
		if (ret_val != VERSION_EXIT && ret_val != HELP_EXIT)
			fprintf(stderr," Parser function exited with Error\n");
		return 1;
	}

	//Checking that the user did not run with RawEth. for this we have raw_etherent_bw test.
	if (user_param.connection_type == RawEth) {
        fprintf(stderr," This test cannot run Raw Ethernet QPs (you have chosen RawEth as connection type\n");
        return FAILURE;
    }

	// Finding the IB device selected (or defalut if no selected).
	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE deivce\n");
		return 1;
	}

	mcg_params.ib_devname = ibv_get_device_name(ib_dev);

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
	if (send_set_up_connection(&ctx,&user_param,&my_dest,&mcg_params,&user_comm)) {
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

		// Prepare IB resources for rtr/rts.
		if (ctx_connect(&ctx,&rem_dest,&user_param,&my_dest)) {
			fprintf(stderr," Unable to Connect the HCA's through the link\n");
			return 1;
		}
	}

	// shaking hands and gather the other side info.
	if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to exchange date between server and clients\n");
		return 1;
	}

    if (user_param.use_event) {

		if (ibv_req_notify_cq(ctx.send_cq, 0)) {
			fprintf(stderr, "Couldn't request RCQ notification\n");
			return 1;
		}

		if (ibv_req_notify_cq(ctx.recv_cq, 0)) {
			fprintf(stderr, "Couldn't request RCQ notification\n");
			return 1;
		}
    }

	printf(RESULT_LINE);
	printf("%s",(user_param.test_type == ITERATIONS) ? RESULT_FMT_LAT : RESULT_FMT_LAT_DUR);

	ctx_set_send_wqes(&ctx,&user_param,&rem_dest);

	if (user_param.test_method == RUN_ALL) {

		if (user_param.connection_type == UD)
			size_max_pow =  (int)UD_MSG_2_EXP(MTU_SIZE(user_param.curr_mtu)) + 1;

		for (i = 1; i < size_max_pow ; ++i) {

			user_param.size = (uint64_t)1 << i;

			// Post recevie recv_wqes fo current message size
			if (ctx_set_recv_wqes(&ctx,&user_param)) {
				fprintf(stderr," Failed to post receive recv_wqes\n");
				return 1;
			}

			// Sync between the client and server so the client won't send packets
			// Before the server has posted his receive wqes (in UC/UD it will result in a deadlock).
			if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
				fprintf(stderr,"Failed to exchange date between server and clients\n");
				return 1;
			}

			if(run_iter_lat_send(&ctx, &user_param))
				return 17;

			user_param.test_type == ITERATIONS ? print_report_lat(&user_param) : print_report_lat_duration(&user_param);
		}

	} else {

		// Post recevie recv_wqes fo current message size
		if (ctx_set_recv_wqes(&ctx,&user_param)) {
			fprintf(stderr," Failed to post receive recv_wqes\n");
			return 1;
		}

		// Sync between the client and server so the client won't send packets
		// Before the server has posted his receive wqes (in UC/UD it will result in a deadlock).
		if (ctx_hand_shake(&user_comm,&my_dest,&rem_dest)) {
			fprintf(stderr,"Failed to exchange date between server and clients\n");
			return 1;
		}

		if(run_iter_lat_send(&ctx, &user_param))
			return 17;

		user_param.test_type == ITERATIONS ? print_report_lat(&user_param) : print_report_lat_duration(&user_param);
	}

	if (ctx_close_connection(&user_comm,&my_dest,&rem_dest)) {
		fprintf(stderr,"Failed to close connection between server and client\n");
		return 1;
	}

	printf(RESULT_LINE);

	return send_destroy_ctx_resources(&ctx,&user_param,&mcg_params);
}
