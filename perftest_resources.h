/*
 * Copyright (c) 2009 Mellanox Technologies Ltd.  All rights reserved.
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
 */

#ifndef PERFTEST_RESOURCES_H
#define PERFTEST_RESOURCES_H


// Files included for work.
#include <infiniband/verbs.h>


// Macro for allocating.
#define ALLOCATE(var,type,size)                                 \
    { if((var = (type*)malloc(sizeof(type)*(size))) == NULL)    \
        { fprintf(stderr, "Cannot Allocate\n"); exit(1);}}


// Message size we pass through sockets , without gid.
#define KEY_MSG_SIZE (sizeof "0000:000000:000000:00000000:0000000000000000")

// Message size we pass through sockets , with gid (For MGID in Mcast mode as well).
#define KEY_MSG_SIZE_GID (sizeof "0000:000000:000000:00000000:0000000000000000:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00")

// The Format of the message we pass through sockets , without passing Gid.
#define KEY_PRINT_FMT "%04x:%06x:%06x:%08x:%016Lx"

// The Format of the message we pass through sockets (With Gid).
#define KEY_PRINT_FMT_GID "%04x:%06x:%06x:%08x:%016Lx:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x"

// The print format of the pingpong_dest element.
#define ADDR_FMT " %s address: LID %#04x QPN %#06x PSN %#06x RKey %#08x VAddr %#016Lx\n"

// The print format of a global address or a multicast address.
#define GID_FMT " %s: %02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d\n"

// The type of the machine ( server or client actually).
typedef enum { SERVER , CLIENT } MachineType;

// The type of the machine ( server or client actually).
typedef enum { LOCAL , REMOTE } PrintDataSide;

/************************************************************************ 
 * Perftest resources Structures and data types.
 ************************************************************************/

struct pingpong_dest {

	int lid;
	int qpn;
	int psn;
	unsigned rkey;
	unsigned long long vaddr;
	union ibv_gid dgid;
};

struct pingpong_params {

	int sockfd;
    int conn_type;
	int use_index;
    int use_mcg;
    MachineType type;
    PrintDataSide side;
};


/************************************************************************ 
 * Perftest resources Methods and interface utilitizes.
 ************************************************************************/

/* 
 *
 */
uint16_t ctx_get_local_lid(struct ibv_context *context,int ib_port);

/* 
 *
 */
int ctx_client_connect(const char *servername, int port);

/* 
 *
 */
int ctx_server_connect(int port);

/* 
 *
 */
int ctx_hand_shake(struct pingpong_params  *params,
				   struct pingpong_dest *my_dest,
				   struct pingpong_dest *rem_dest);

/* 
 *
 */
void ctx_print_pingpong_data(struct pingpong_dest *png_element,
							 struct pingpong_params *params);

/*
 *
 */
int ctx_close_connection(struct pingpong_params  *params,
				         struct pingpong_dest *my_dest,
				         struct pingpong_dest *rem_dest);

#endif /* PERFTEST_RESOURCES_H */
