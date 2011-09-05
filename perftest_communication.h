/*
 * Copyright (c) 2011 Mellanox Technologies Ltd.  All rights reserved.
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
 * Author: Ido Shamay <idos@dev.mellanox.co.il>
 *
 * Description : ...
 *  
 * Methods : ...
 */

#ifndef PERFTEST_COMMUNICATION_H
#define PERFTEST_COMMUNICATION_H

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include "perftest_resources.h"

#define KEY_MSG_SIZE 	 (50)   // Message size without gid.
#define KEY_MSG_SIZE_GID (98)   // Message size with gid (MGID as well).
#define SYNC_SPEC_ID	 (5)

// The Format of the message we pass through sockets , without passing Gid.
#define KEY_PRINT_FMT "%04x:%04x:%06x:%06x:%08x:%016Lx"

// The Format of the message we pass through sockets (With Gid).
#define KEY_PRINT_FMT_GID "%04x:%04x:%06x:%06x:%08x:%016Lx:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x"

// The Basic print format for all verbs.
#define BASIC_ADDR_FMT " %s address: LID %#04x QPN %#06x PSN %#06x"

// Addition format string for READ - the outstanding reads.
#define READ_FMT       " OUT %#04x"

// The print format of the pingpong_dest element for RDMA verbs.
#define RDMA_FMT       " RKey %#08x VAddr %#016Lx"

// The print format of a global address or a multicast address.
#define PERF_GID_FMT " %s: %02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d\n"

struct pingpong_dest {
	int 			   lid;
	int 			   out_reads;
	int 			   qpn;
	int 			   psn;  
	unsigned           rkey;
	unsigned long long vaddr;
	union ibv_gid      gid;
};

struct perftest_comm {
	struct pingpong_context    *rdma_ctx;
	struct perftest_parameters *rdma_params;
};

/* create_comm_struct
 *
 * Description : Creating the communication struct for Etherent or rdma_cm options.
 *
 * Parameters : 
 *	comm - An empty Communication struct.
 *  user_param -  Perftest parameters.
 *
 * Return Value : SUCCESS,FAILURE.
 */
int create_comm_struct (struct perftest_comm *comm,
						struct perftest_parameters *user_param);


/* set_up_connection .
 *
 * Description : Fills the my_dest with all of the machine proporties.
 *  
 *
 * Parameters :
 *  ctx 		- Pingoong context after the ctx_init function.
 *  user_parm   - Perftest parameters.
 *  my_dest		- An empty pingpong_dest struct.
 *
 * Return Value : SUCCESS,FAILURE.
 */
int set_up_connection(struct pingpong_context *ctx,
					  struct perftest_parameters *user_parm,
					  struct pingpong_dest *my_dest);

/* establish_connection .
 *
 * Description :
 *
 *  Connect the client the a well known server to a requested port.
 *  It assumes the Server is waiting for request on the port.
 *  It uses Ethernet sockets or rdma_cm as mentioned in use_rdma_cm.
 *
 * Parameters : 
 *		comm - The communication struct with all the data.
 *
 * Return Value : SUCCESS,FAILURE.
 */
int establish_connection(struct perftest_comm *comm);

/* rdma_client_connect .
 *
 * Description : Connects the client to a QP on the other machine with rdma_cm.
 *
 * Parameters : 
 *		ctx - An empty resources struct to fill the resources created for this QP.
 *		user_param - Perftest parameters.
 *
 * Return Value : SUCCESS,FAILURE.
 */
int rdma_client_connect(struct pingpong_context *ctx,
						struct perftest_parameters *user_param);

/* rdma_server_connect .
 *
 * Description : Assinging a server to listen on a rdma_cm port and connect to it.
 *
 * Parameters : 
 *		ctx - An empty resources struct to fill the resources created for this QP.
 *		user_param - Perftest parameters.
 *
 * Return Value : SUCCESS,FAILURE.
 */
int rdma_server_connect(struct pingpong_context *ctx,
						struct perftest_parameters *user_param);
/* ctx_hand_shake .
 *
 * Description :
 *
 *  Exchanging the data , represented in struct pingpong_dest , between 
 *  a server and client that performed the ctx_server/clinet_connect.
 *  The method fills in rem_dest the remote machine data , and passed the data
 *  in my_dest to other machine.  
 *
 * Parameters : 
 *
 *  params   - The parameters needed for this method. Are mentioned above ,and
 *             contains standard IB info. (exists on perftest).
 *  my_dest  - Contains the data you want to pass to the other side.
 *  rem_dest - The other side data.
 *
 * Return Value : 0 upon success. -1 if it fails.
 */
int ctx_hand_shake(struct perftest_comm *comm,
				   struct pingpong_dest *my_dest,
				   struct pingpong_dest *rem_dest);

/* ctx_print_pingpong_data.
 *
 * Description :
 *
 *  Prints the data stored in the struct pingpong_dest.
 *
 * Parameters : 
 *
 *  params  - The parameters of the machine.
 *  element - The element to print.           
 */
void ctx_print_pingpong_data(struct pingpong_dest *element,
							 struct perftest_comm *comm);

/* ctx_close_connection .
 *
 * Description :
 *
 *  Close the connection between the 2 machines.
 *  It performs an handshake to ensure the 2 sides are there.
 *
 * Parameters : 
 *
 *  params            - The parameters of the machine
 *  my_dest ,rem_dest - The 2 sides that ends the connection.
 *
 * Return Value : 0 upon success. -1 if it fails.
 */
int ctx_close_connection(struct perftest_comm *comm,
				         struct pingpong_dest *my_dest,
				         struct pingpong_dest *rem_dest);

#endif /* PERFTEST_COMMUNICATION_H */
