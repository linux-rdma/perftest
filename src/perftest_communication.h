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

#include <netinet/in.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include "perftest_resources.h"

/* Macro for 64 bit variables to switch to/from net */
#if BYTE_ORDER == BIG_ENDIAN
#define ntoh_64(x) (x)
#define hton_64(x) (x)
#define ntoh_double(x) (x)
#define hton_double(x) (x)
#elif BYTE_ORDER == LITTLE_ENDIAN
#define ntoh_64(x) bswap_64(x)
#define hton_64(x) bswap_64(x)
#define ntoh_double(x) bswap_double(x)
#define hton_double(x) bswap_double(x)
#else
#error "Must set BYTE_ORDER"
#endif

/* long is 64-bit in LP64 mode, 32-bit in LLP64 mode. */
#if defined(_LP64) || defined(__LP64__)
#define ntoh_long(x) ntoh_64(x)
#define hton_long(x) hton_64(x)
#else
#define ntoh_long(x) ntohl(x)
#define hton_long(x) htonl(x)
#endif

/* int is 32-bit in both LP64 and LLP64 modes. */
#define ntoh_int(x) (int) ntohl((uint32_t) (x))
#define hton_int(x) (int) htonl((uint32_t) (x))

#define KEY_MSG_SIZE 	 (59)   /* Message size without gid. */
#define KEY_MSG_SIZE_GID (108)   /* Message size with gid (MGID as well). */
#define SYNC_SPEC_ID	 (5)

/* The Format of the message we pass through sockets , without passing Gid. */
#define KEY_PRINT_FMT "%04x:%04x:%06x:%06x:%08x:%016llx:%08x"

/* The Format of the message we pass through sockets (With Gid). */
#define KEY_PRINT_FMT_GID "%04x:%04x:%06x:%06x:%08x:%016llx:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%08x:"

/* The Basic print format for all verbs. */
#define BASIC_ADDR_FMT " %s address: LID %#04x QPN %#06x PSN %#06x"

/* Addition format string for READ - the outstanding reads. */
#define READ_FMT       " OUT %#04x"

/* The print format of the pingpong_dest element for RDMA verbs. */
#define RDMA_FMT       " RKey %#08x VAddr %#016llx"

/* The print number of SRQ in case of XRC */
#define XRC_FMT 	   " SRQn %#08x"
#define DC_FMT         " SRQn %#08x"

/* The print format of a global address or a multicast address. */
#define PERF_GID_FMT " %s: %02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d:%02d\n"

/* The print format of a global address or a multicast address. */
#define PERF_RAW_MGID_FMT " %s: %02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x\n"

struct perftest_comm {
	struct pingpong_context    *rdma_ctx;
	struct counter_context     *counter_ctx;
	struct perftest_parameters *rdma_params;
};

/* bswap_double
 *
 * Description : swap byte order for double.
 *
 * Parameters :
 *  x -	input double variable
 *
 *  Return Value : double after byte order being swapped.
 */
double bswap_double(double x);


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
 *  user_param   - Perftest parameters.
 *  my_dest		- An empty pingpong_dest struct.
 *
 * Return Value : SUCCESS,FAILURE.
 */
int set_up_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
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

/* retry_rdma_connect .
 *
 * Description : Retries rdma_client_connect() because the listener may not be ready
 *               when the rdma client attempts to connect
 *
 * Parameters :
 *		ctx - An empty resources struct to fill the resources created for this QP.
 *		user_param - Perftest parameters.
 *
 * Return Value : SUCCESS,FAILURE.
 */
int retry_rdma_connect(struct pingpong_context *ctx,
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

/* ctx_xchg_data .
 *
 * Description :
 *
 *  Exchanging data between
 *  a server and client after performing ctx_server/client_connect.
 *  The method fills in rem_data the remote machine data , and passed the data
 *  in my_dest to other machine.
 *
 * Parameters :
 *
 *  comm   - contains connections info
 *  my_data  - Contains the data you want to pass to the other side.
 *  rem_data - The other side data.
 *  size - size of my_data (after casting is made)
 *
 * Return Value : 0 upon success. -1 if it fails.
 */
int ctx_xchg_data( struct perftest_comm *comm,
		void *my_data,
		void *rem_data,int size);

/* ethernet_write_data .
 *
 * Description :
 *
 *  Sends data that is written in msg using ethernet
 *  This functions can send any basic type (int,float,double,char*,string, etc..).
 *	If you need to send a struct, decoder and encoder must be implemented to convert
 *  the struct to a string
 *
 * Parameters :
 *
 *  comm   - contains connections info
 *  msg    - message that will be sent
 *  size   - size of the message
 * Return Value : 0 upon success. -1 if it fails.
 */
int ethernet_write_data(struct perftest_comm *comm, char *msg, size_t size);

/* ethernet_read_data .
 *
 * Description :
 *
 *  Read data from remote machine using ethernet.
 *
 * Parameters :
 *
 *  comm   - contains connections info
 *  recv_msg    - function will return, in this argument, the message from remote machine
 *  size   - size of the message
 * Return Value : 0 upon success. -1 if it fails.
 */
int ethernet_read_data(struct perftest_comm *comm, char *recv_msg, size_t size);

/* rdma_write_data .
 *
 * Description :
 *
 *  Sends data that to remote machine using RDMA.
 *  This functions can send any variable type
 *
 * Parameters :
 *
 *  data   - data that will be sent
 *  comm   - contains connections info
 *  size    - size of data
 * Return Value : 0 upon success. -1 if it fails.
 */
int rdma_write_data(void *data,	struct perftest_comm *comm, int size);

/* rdma_read_data .
 *
 * Description :
 *
 *  Reads  data from remote machine using RDMA.
 *  This functions can read any variable type
 *
 * Parameters :
 *
 *  data   - data that will be sent
 *  comm   - contains connections info
 *  size    - size of data
 * Return Value : 0 upon success. -1 if it fails.
 * Return Value : 0 upon success. -1 if it fails.
 */
int rdma_read_data(void *data, struct perftest_comm *comm, int size);

/* ctx_xchg_data .
 *
 * Description :
 *
 *  Implements ctx_xchg_data for ethernet
 *
 * Parameters :
 *
 *  comm   - contains connections info
 *  my_data  - Contains the data you want to pass to the other side.
 *  rem_data - The other side data.
 *  size - size of my_data (after casting is made)
 *
 * Return Value : 0 upon success. -1 if it fails.
 */
int ctx_xchg_data_ethernet( struct perftest_comm *comm, void *my_data, void *rem_data,int size);

/* ctx_xchg_data .
 *
 * Description :
 *
 *  Implements ctx_xchg_data for RDMA
 *
 * Parameters :
 *
 *  comm   - contains connections info
 *  my_data  - Contains the data you want to pass to the other side.
 *  rem_data - The other side data.
 *  size - size of my_data (after casting is made)
 *
 * Return Value : 0 upon success. -1 if it fails.
 */
int ctx_xchg_data_rdma( struct perftest_comm *comm, void *my_data, void *rem_data,int size);

/* ctx_xchg_data .
 *
 * Description :
 *
 *  Exchanging bw reports between
 *  a server and client after performing ctx_server/client_connect.
 *  The method fills in rem_data the remote machine data , and passed the data
 *  in my_dest to other machine.
 *
 * Parameters :
 *
 *  comm   - contains connections info
 *  my_bw_rep  - Contains the data you want to pass to the other side.
 *  rem_bw_rep - The other side data.
 *
 * Return Value : 0 upon success. -1 if it fails.
 */
void xchg_bw_reports (struct perftest_comm *comm, struct bw_report_data *my_bw_rep,
		struct bw_report_data *rem_bw_rep, float remote_version);

/* exchange_versions.
 *
 * Description :
 * 	Exchange versions between sides.
 *
 */
void exchange_versions (struct perftest_comm *user_comm, struct perftest_parameters *user_param);

/* check_sys_data.
 *
 * Description :
 * 	Exchange system data between sides.
 *
 */
void check_sys_data(struct perftest_comm *user_comm, struct perftest_parameters *user_param);

/* check_mtu
 *
 * Description : Configures test MTU.
 *
 * Parameters :
 *
 *	 context    - Context of the device.
 *	 user_param - Perftest parameters.
 *   user_comm	- user communication struct.
 * Return Value : SUCCESS, FAILURE.
 */
int check_mtu(struct ibv_context *context,struct perftest_parameters *user_param, struct perftest_comm *user_comm);

int ctx_check_gid_compatibility(struct pingpong_dest *my_dest,
		struct pingpong_dest *rem_dest);

/* rdma_cm_get_rdma_address:
*
* Description:
*
*    Provides transport independent address translation.
*    Resolves the destination node, service address and returns
*    information required to establish device communication.
*
* Parameters:
*
*    user_param - User parameters from the parser.
*    hints - RDMA address information.
*    rai - Returned RDMA address information.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_get_rdma_address(struct perftest_parameters *user_param,
		struct rdma_addrinfo *hints, struct rdma_addrinfo **rai);

/* rdma_cm_request_ud_connection_parameters:
*
* Description:
*
*    Request UD connection parameters in server side.
*    Doing the request by posting WQE to the receive queue, to
*    get the remote side connection parameters, remote QP number and QP key,
*    after calling rdma_accept RDMA CM API function. Initializing the
*    local QP number for the connection parameter.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    conn_param - RDMA connection parameters.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_request_ud_connection_parameters(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct rdma_conn_param *conn_param);

/* rdma_cm_initialize_ud_connection_parameters:
*
* Description:
*
*    Initializes UD connection parameters in server side.
*    Initializes remote node connection parameters needed for the received
*    connection, remote QP number and QP key.
*    Doing that by creating address handle from received WQE
*    sent by the remote side.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_initialize_ud_connection_parameters(struct pingpong_context *ctx,
		struct perftest_parameters *user_param);

/* rdma_cm_send_ud_connection_parameters:
*
* Description:
*
*    Sends UD connection parameters to the server side.
*    Sends client's QP number to the server side as an immediate data
*    in the WQE. Using the remote QP number and QP key received from
*    RDMA CM connect event.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    connection_index - Connection index number.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_send_ud_connection_parameters(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int connection_index);

/* rdma_cm_establish_ud_connection:
*
* Description:
*
*    Establishes UD connection parameters in client side.
*    Initializes remote node connection parameters needed for the received
*    connection, by using the remote QP number and QP key received from
*    RDMA CM connect event. Creates address handle by the received connect
*    event UD address handle parameters. Sends to the remote node UD
*    parameters.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    event - RDMA CM event.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_establish_ud_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_event *event);

/* rdma_cm_connect_error:
*
* Description:
*
*    Represents connection error.
*    Decreases the connections left parameter in the cma_master structure.
*
* Parameters:
*
*    ctx - Application contexts.
*
* Return value:
*    None.
*
*/
void rdma_cm_connect_error(struct pingpong_context *ctx);

/* rdma_cm_address_handler:
*
* Description:
*
*    Resolves RDMA route to the destination address in order
*    to establish a connection, after receiving RDMA_CM_EVENT_ADDR_RESOLVED
*    event from RDMA CM API.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    cma_id - RDMA CM ID.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_address_handler(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_id *cma_id);

/* rdma_cm_route_handler:
*
* Description:
*
*    Initializes context for the test per connection in the client side
*    and connects through rdma_connect RDMA CM API function. After receiving
*    RDMA_CM_EVENT_ADDR_RESOLVED event from RDMA CM API.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    cma_id - RDMA CM ID.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_route_handler(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_id *cma_id);

/* rdma_cm_connection_request_handler:
*
* Description:
*
*    Initializes context for the test per connection in the server side
*    and connects through rdma_connect RDMA CM API function. After receiving
*    RDMA_CM_EVENT_CONNECT_REQUEST event from RDMA CM API.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    cma_id - RDMA CM ID.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_connection_request_handler(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_id *cma_id);

/* rdma_cm_connection_established_handler:
*
* Description:
*
*    Establishes connection in the client side by updating the connections
*    and disconnections left counters in cma_master structure. Includes
*    establishing UD connection, in case of UD connection chosen.
*    After receiving RDMA_CM_EVENT_ESTABLISHED event from RDMA CM API.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    event - RDMA CM event.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_connection_established_handler(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_event *event);

/* rdma_cm_event_error_handler:
*
* Description:
*
*    Handles RDMA CM events error by printing the event and the error.
*    Calling rdma_cm_connect_error function to update connection left.
*
* Parameters:
*
*    ctx - Application contexts.
*    event - RDMA CM event.
*
* Return value:
*    FAILURE(1).
*
*/
int rdma_cm_event_error_handler(struct pingpong_context *ctx,
		struct rdma_cm_event *event);

/* rdma_cm_disconnect_handler:
*
* Description:
*
*    Disconnects RDMA CM connection after receiving RDMA_CM_EVENT_DISCONNECTED
*    event from RDMA CM API. Doing that by decreasing the connections
*    left parameter in the cma_master structure.
*
* Parameters:
*
*    ctx - Application contexts.
*
* Return value:
*    None.
*
*/
void rdma_cm_disconnect_handler(struct pingpong_context *ctx);

/* rdma_cm_events_dispatcher:
*
* Description:
*
*    RDMA CM Events dispatcher.
*    Receives events and directs to the appropriate function
*    to handle the event.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    cma_id - RDMA CM ID.
*    event - RDMA CM event.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_events_dispatcher(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_id *cma_id,
		struct rdma_cm_event *event);

/* rdma_cm_connect_events:
*
* Description:
*
*    RDMA CM connects events.
*    Waits on events from rdma_get_cm_event RDMA CM API function, receives
*    the events, forwards to the dispatcher to handle the event, finally ACKs
*    the event to the RDMA CM API.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    cma_id - RDMA CM ID.
*    event - RDMA CM event.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_connect_events(struct pingpong_context *ctx,
		struct perftest_parameters *user_param);

/* rdma_cm_disconnect_nodes:
*
* Description:
*
*    Disconnects RDMA CM nodes.
*    Disconnects the connection by calling rdma_disconnect RDMA CM API
*    function for every RDMA CM ID and handling the disconnects events.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_disconnect_nodes(struct pingpong_context *ctx,
		struct perftest_parameters *user_param);

/* rdma_cm_server_connection:
*
* Description:
*
*    Initializes server side RDMA CM connection.
*    Entry point to the server connection, server will connect and create
*    all the needed structures for the requested number of QPs(connections).
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    hints - RDMA address information.
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_server_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_addrinfo *hints);

/* _rdma_cm_client_connection:
*
* Description:
*
*    Initializes client side RDMA CM connection.
*    Entry point to the client connection, client will connect and create
*    all the needed structures for the requested number of QPs(connections).
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    hints - RDMA address information.
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int _rdma_cm_client_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_addrinfo *hints);

/* _rdma_cm_client_connection:
*
* Description:
*
*    Wrapper function to rdma_cm_client_connection.
*    Here the client will attempt to make several calls to
*    rdma_cm_client_connection, in case the server is not ready at start.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    hints - RDMA address information.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int rdma_cm_client_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_addrinfo *hints);

/* create_rdma_cm_connection:
*
* Description:
*
*    Creates RDMA CM connection.
*    Entry point to create RDMA CM contexts and communication
*    for server and client.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*    comm - Communication information.
*    my_dest - Local node destination communication information.
*    rem_dest - Remote node destination communication information.
*
* Return value:
*    rc - On success: SUCCESS(0), on failure: FAILURE(1).
*
*/
int create_rdma_cm_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct perftest_comm *comm,
		struct pingpong_dest *my_dest, struct pingpong_dest *rem_dest);


#endif /* PERFTEST_COMMUNICATION_H */



