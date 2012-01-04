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
#include <rdma/rdma_cma.h>
#include "perftest_parameters.h"
#include <stdint.h>
#include <math.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <byteswap.h>

#define CYCLE_BUFFER        (4096)
#define CACHE_LINE_SIZE     (64)
#define NUM_OF_RETRIES		(10)


// Outstanding reads for "read" verb only.
#define MAX_SEND_SGE        (1)
#define MAX_RECV_SGE        (1)
#define DEF_WC_SIZE         (1)
#define PL					(1)
#define ATOMIC_ADD_VALUE    (1)
#define ATOMIC_SWAP_VALUE   (0)

// Space for GRH when we scatter the packet in UD.
#define PINGPONG_SEND_WRID   (60)
#define PINGPONG_RDMA_WRID	 (3)
#define PINGPONG_READ_WRID	 (1)
#define PINGPONG_ATOMIC_WRID (22)
#define DEFF_QKEY            (0x11111111)

#define NOTIFY_COMP_ERROR_SEND(wc,scnt,ccnt)                     											\
	{ fprintf(stderr," Completion with error at client\n");      											\
	  fprintf(stderr," Failed status %d: wr_id %d syndrom 0x%x\n",wc.status,(int) wc.wr_id,wc.vendor_err);	\
	  fprintf(stderr, "scnt=%d, ccnt=%d\n",scnt, ccnt);	return 1;} 

#define NOTIFY_COMP_ERROR_RECV(wc,rcnt)                     											    \
	{ fprintf(stderr," Completion with error at server\n");      											\
	  fprintf(stderr," Failed status %d: wr_id %d syndrom 0x%x\n",wc.status,(int) wc.wr_id,wc.vendor_err);	\
	  fprintf(stderr," rcnt=%d\n",rcnt); return 1;} 

// Macro to determine packet size in case of UD. 
// The UD addition is for the GRH .
#define SIZE(type,size,valid) ((type == UD && valid) ? (size + UD_ADDITION) : (size))

// Macro to define the buffer size (according to "Nahalem" chip set).
// for small message size (under 4K) , we allocate 4K buffer , and the RDMA write
// verb will write in cycle on the buffer. this improves the BW in "Nahalem" systems.
#define BUFF_SIZE(size) ((size < CYCLE_BUFFER) ? (CYCLE_BUFFER) : (size))

// UD addition to the buffer.
#define IF_UD_ADD(type) ((type == UD) ? (CACHE_LINE_SIZE) : (0))

// Macro that defines the adress where we write in RDMA.
// If message size is smaller then CACHE_LINE size then we write in CACHE_LINE jumps.
#define INC(size) ((size > CACHE_LINE_SIZE) ? ((size%CACHE_LINE_SIZE == 0) ?  \
	       (size) : (CACHE_LINE_SIZE*(size/CACHE_LINE_SIZE+1))) : (CACHE_LINE_SIZE))

#define UD_MSG_2_EXP(size) ((log(size))/(log(2)))

/******************************************************************************
 * Perftest resources Structures and data types.
 ******************************************************************************/

struct pingpong_context {
	struct ibv_context 			*context;
	struct ibv_comp_channel 	*channel;
	struct ibv_pd      			*pd;
	struct ibv_mr      			*mr;
	struct ibv_cq      			*send_cq;
	struct ibv_cq      			*recv_cq;
	void               			*buf;
	struct ibv_ah				*ah;
	struct ibv_qp      			**qp;
	uint64_t           			size;
	int                			tx_depth;
    int                			*scnt;
    int                			*ccnt;
	struct rdma_event_channel 	*cm_channel;
	struct rdma_cm_id 		  	*cm_id_control;
	struct rdma_cm_id 		  	*cm_id;
};



/****************************************************************************** 
 * Perftest resources Methods and interface utilitizes.
 ******************************************************************************/

/* check_add_port
 *
 * Description : Creating a service struct from a given port and servername.
 *
 * Parameters : 
 *	service - an empty char** to contain the service name.	
 *  port - The selected port on which the server will listen.
 *  hints - The requested ai_* params for the connection.
 *  res - Holds the result.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int check_add_port(char **service,int port,
				   const char *servername,
				   struct addrinfo *hints,
				   struct addrinfo **res);

/* ctx_find_dev
 *
 * Description : Returns the device corresponding to ib_devname 
 *	or the first one found , in case ib_devname == NULL
 *
 * Parameters : 
 *	
 *	ib_devname - The name of the deivce requested or NULL for the first one.
 *
 * Return Value : the deivce or NULL in case of failure.
 */
struct ibv_device* ctx_find_dev(const char *ib_devname);

/* create_rdma_resources
 *
 * Description : Creates the rdma_cm_id and rdma_channel for the rdma_cm QPs.
 *
 * Parameters : 
 *	ctx - Resources sructure.
 * 	user_param - the perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int create_rdma_resources(struct pingpong_context *ctx,
                          struct perftest_parameters *user_param);


/* destroy_ctx
 *
 * Description : Deallocate all perftest resources.
 *
 * Parameters : 
 *	ctx - Resources sructure.
 * 	user_param - the perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int destroy_ctx(struct pingpong_context *ctx,
				struct perftest_parameters *user_parm);

/* ctx_init
 *
 * Description : 
 * 		Creates all the test resources.
 *		It creates Buffer, PD, MR, CQ QPs and moves the QPs to INIT.
 *
 * Parameters : 
 *	ctx - An empty resources sructure to fill inside the resources.
 * 	user_param - the perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int ctx_init(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* ctx_qp_create.
 *
 * Description : 
 *
 *	Creates a QP , according to the attributes given in param.
 *	The relevent attributes are tx_depth,rx_depth,inline_size and connection_type.
 *
 * Parameters :
 *
 *	pd      - The Protection domain , each the qp will be assigned to.
 *	send_cq - The CQ that will produce send CQE.
 *	recv_qp - The CQ that will produce recv CQE.
 *	param   - The parameters for the QP.
 *
 * Return Value : Adress of the new QP.
 */
struct ibv_qp* ctx_qp_create(struct pingpong_context *ctx,
							 struct perftest_parameters *user_param);

/* ctx_modify_qp_to_init.
 *
 * Description :
 *
 *	Modifies the given QP to INIT state , according to attributes in param.
 *  The relevent attributes are ib_port, connection_type and verb. 
 *
 * Parameters :
 *
 *	qp     - The QP that will be moved to INIT.
 *	param  - The parameters for the QP.
 *
 * Return Value : SUCCESS, FAILURE.
 *
 */
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param);

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

/* ctx_notify_events
 * 
 * Description : Prepare the test to work with events instead of polling the CQ.
 *	This is the way to work in un interipted mode.  
 * 
 * Parameters : 
 *  channel - (Mandotory) the created event channel.
 *         
 * Return Value : SUCCESS, FAILURE.
 */
inline int ctx_notify_events(struct ibv_comp_channel *channel);

/* increase_rem_addr.
 *
 * Description : 
 *	Increases the remote address in RDMA verbs by INC , 
 *  (at least 64 CACHE_LINE size) , so that the system will be a able to cahce the data
 *  in an orginzed way.
 * 
 *  Parameters : 
 *		wr - The send wqe.
 *		size - size of the message to send.
 *		scnt - The ammount of post_send or post_receive we called.
 *		prim_addr - The address of the original buffer.
 *
 * Return Value : SUCCESS, FAILURE.
 */
inline void increase_rem_addr(struct ibv_send_wr *wr,int size,int scnt,uint64_t prim_addr,VerbType verb);

/* increase_loc_addr.
 *
 * Description :
 * 	Increases the local address in all verbs , 
 *  (at least 64 CACHE_LINE size) , so that the system will be a able to cahce the data
 *  in an orginzed way.
 *
 * Parameters : 
 *		sg - The scatter element of the wqe.
 *		size - size of the message to send.
 *		rcnt - The ammount of post_send or post_receive we called.
 *		prim_addr - The address of the original buffer.
 *		server_is_ud - Indication to weather we are in UD mode.
 */
inline void increase_loc_addr(struct ibv_sge *sg,int size,int rcnt,
							  uint64_t prim_addr,int server_is_ud);

#endif /* PERFTEST_RESOURCES_H */
