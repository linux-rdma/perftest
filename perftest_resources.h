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
 *
 * Description :
 *
 *  This API gathres the Socket interface methods for all perftest benchmarks 
 *  and can be used for any benchmark for IB.
 *  It passes messages between 2 end points through sockets interface methods,
 *  while passing the rellevant information for the IB entities. 
 *  
 * Methods :
 *
 *  ctx_get_local_lid  - Receives the Local id from the subnet manager.
 *  ctx_client_connect - Connects the client through sockets interface.
 *  ctx_server_connect - Connects the Server to client through sockets.
 *  ctx_hand_shake     - Passes the data between 2 end points machines.
 *  ctx_print_pingpong_data - Prints the data that was passed. 
 *  ctx_close_connection    - Closing the sockets interface.
 */

#ifndef PERFTEST_RESOURCES_H
#define PERFTEST_RESOURCES_H


// Files included for work.
#include <infiniband/verbs.h>

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

// The link layer of the current port.
typedef enum { FAILURE = -1 , IB = 1 , ETH = 2 } LinkType;

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

/* set_link_layer.
 *
 * Description : Determines the link layer type (IB or ETH).
 *
 * Parameters : 
 *
 *  context - The context of the HCA device.
 *  ib_port - The port of the HCA (1 or 2).
 *
 * Return Value : IB or ETH in case of success , FAILURE otherwise.
 */
LinkType set_link_layer(struct ibv_context *context,int ib_port);

/* ctx_get_local_lid .
 *
 * Description :
 *
 *  This method find and returns the local Id in IB subnet manager of
 *  the selected port and HCA given.The lid identifies the port.
 *
 * Parameters : 
 *
 *  context - the context of the HCA device.
 *  ib_port - The port of the HCA (1 or 2).
 *
 * Return Value : The Lid itself. (No error values).
 */
uint16_t ctx_get_local_lid(struct ibv_context *context,int ib_port);

/* ctx_client_connect .
 *
 * Description :
 *
 *  Connect the client the a well known server to a requested port.
 *  It assumes the Server is waiting for request on the port.
 *
 * Parameters : 
 *
 *  servername - The server name (according to DNS) or IP.
 *  port       - The port that the server is listening to.
 *
 * Return Value : The file descriptor selected in the PDT for the socket.
 */
int ctx_client_connect(const char *servername, int port);

/* ctx_server_connect .
 *
 * Description :
 *
 *  Instructs a machine to listen on a requested port.
 *  when running this command the machine will wait for 1 client to
 *  contant it , on the selected port , with the ctx_client_connect method.
 *
 * Parameters : 
 *
 *  port - The port which the machine will listen.
 *
 * Return Value : The new file descriptor selected in the PDT for the socket.
 */
int ctx_server_connect(int port);

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
int ctx_hand_shake(struct pingpong_params  *params,
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
							 struct pingpong_params *params);

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
int ctx_close_connection(struct pingpong_params  *params,
				         struct pingpong_dest *my_dest,
				         struct pingpong_dest *rem_dest);


#endif /* PERFTEST_RESOURCES_H */
