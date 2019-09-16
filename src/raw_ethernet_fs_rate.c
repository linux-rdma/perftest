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
	struct report_options		report;

	/* init default values to user's parameters */
	memset(&ctx, 0, sizeof(struct pingpong_context));
	memset(&user_param, 0, sizeof(struct perftest_parameters));

	user_param.tst     = FS_RATE;
	user_param.verb    = SEND;
	strncpy(user_param.version, VERSION, sizeof(user_param.version));
	user_param.connection_type = RawEth;
	user_param.r_flag  = &report;

	ret_parser = parser(&user_param, argv, argc);

	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT) {
			fprintf(stderr, " Parser function exited with Error\n");
		}
		DEBUG_LOG(TRACE,"<<<<<<%s", __FUNCTION__);
		return FAILURE;
	}
	user_param.machine = SERVER;

	/* Allocate user input dependable structs */
	ALLOCATE(my_dest_info, struct raw_ethernet_info, user_param.num_of_qps);
	memset(my_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param.num_of_qps);
	ALLOCATE(rem_dest_info, struct raw_ethernet_info, user_param.num_of_qps);
	memset(rem_dest_info, 0, sizeof(struct raw_ethernet_info) * user_param.num_of_qps);

	/* Finding the IB device selected (or default if no selected). */
	ib_dev = ctx_find_dev(&user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr, "Unable to find the Infiniband/RoCE device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		return FAILURE;
	}

	if (check_flow_steering_support(user_param.ib_devname)) {
		return FAILURE;
	}

	/* Getting the relevant context from the device */
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, "Couldn't get context for the device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		return FAILURE;
	}

	/* See if MTU and link type are valid and supported. */
	if (check_link_and_mtu(ctx.context, &user_param)) {
		fprintf(stderr, "Couldn't get context for the device\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		return FAILURE;
	}

	/* Allocating arrays needed for the test. */
	alloc_ctx(&ctx, &user_param);

	/* create all the basic IB resources (data buffer, PD, MR, CQ and events channel) */
	if (ctx_init(&ctx, &user_param)) {
		fprintf(stderr, "Couldn't create IB resources\n");
		return FAILURE;
	}

	/* Print basic test information. */
	ctx_print_test_info(&user_param);

	if(run_iter_fs(&ctx, &user_param)){
		fprintf(stderr, "Unable to run iter fs rate\n");
		return FAILURE;
	}

	print_report_fs_rate(&user_param);

	if (destroy_ctx(&ctx, &user_param)) {
		fprintf(stderr, "Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
		return FAILURE;
	}

	if (user_param.output == FULL_VERBOSITY)
		printf(RESULT_LINE);

	DEBUG_LOG(TRACE, "<<<<<<%s", __FUNCTION__);
	return SUCCESS;
}
