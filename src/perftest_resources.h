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

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>
#if defined(__FreeBSD__)
#include <infiniband/byteswap.h>
#else
#include <byteswap.h>
#endif
#include <math.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include "perftest_parameters.h"

#define NUM_OF_RETRIES		(10)

/* Outstanding reads for "read" verb only. */
#define MAX_SEND_SGE		(1)
#define MAX_RECV_SGE		(1)
#define CTX_POLL_BATCH		(16)
#define PL			(1)
#define ATOMIC_ADD_VALUE	(1)
#define ATOMIC_SWAP_VALUE	(0)

/* Space for GRH when we scatter the packet in UD. */
#define PINGPONG_SEND_WRID	(60)
#define PINGPONG_RDMA_WRID	(3)
#define PINGPONG_READ_WRID	(1)
#define PINGPONG_ATOMIC_WRID	(22)
#define DEFF_QKEY		(0x11111111)

#define ERROR_MSG_SIZE (128)

#ifdef HAVE_XRCD
#define SERVER_FD "/tmp/xrc_domain_server"
#define CLIENT_FD "/tmp/xrc_domain_client"
#endif


#define NOTIFY_COMP_ERROR_SEND(wc,scnt,ccnt)                     											\
	{ fprintf(stderr," Completion with error at client\n");      											\
	  fprintf(stderr," Failed status %d: wr_id %d syndrom 0x%x\n",wc.status,(int) wc.wr_id,wc.vendor_err);	\
	  fprintf(stderr, "scnt=%lu, ccnt=%lu\n",scnt, ccnt); }

#define NOTIFY_COMP_ERROR_RECV(wc,rcnt)                     											    \
	{ fprintf(stderr," Completion with error at server\n");      											\
	  fprintf(stderr," Failed status %d: wr_id %d syndrom 0x%x\n",wc.status,(int) wc.wr_id,wc.vendor_err);	\
	  fprintf(stderr," rcnt=%lu\n",rcnt); }

/* Macro to determine packet size in case of UD. The UD addition is for the GRH . */
#define SIZE(type,size,valid) ((type == UD && valid) ? (size + UD_ADDITION) : (size))

/* Macro to define the buffer size (according to "Nahalem" chip set).
 * for small message size (under 4K) , we allocate 4K buffer , and the RDMA write
 * verb will write in cycle on the buffer. this improves the BW in "Nahalem" systems.
 */
#define BUFF_SIZE(size,cycle_buffer) ((size < cycle_buffer) ? (cycle_buffer) : (size))

/* UD addition to the buffer. */
#define IF_UD_ADD(type,cache_line_size) ((type == UD) ? (cache_line_size) : (0))

#define ROUND_UP(value, alignment) (((value) % (alignment) == 0) ?  \
		(value) : ((alignment) * ((value) / (alignment) + 1)))

/* Macro that defines the address where we write in RDMA.
 * If message size is smaller then CACHE_LINE size then we write in CACHE_LINE jumps.
 */
#define INC(size, cache_line_size) ((size > cache_line_size) ? \
		ROUND_UP(size, cache_line_size) : (cache_line_size))

#define MSG_SZ_2_EXP(size) ((log(size)) / (log(2)))

#define MASK_IS_SET(mask, attr)      (((mask)&(attr))!=0)

/******************************************************************************
 * Perftest resources Structures and data types.
 ******************************************************************************/

/* Represents RDMA CM node with needed information */
struct cma_node {
	struct rdma_cm_id *cma_id;
	uint32_t remote_qpn;
	uint32_t remote_qkey;
	int id;
	int connected;
};

/* Represents RDMA CM management needed information */
struct cma {
	struct rdma_event_channel* channel;
	struct rdma_addrinfo* rai;
	struct cma_node* nodes;
	int connection_index;
	int connects_left;
	int disconnects_left;
};

struct pingpong_context {
	struct cma cma_master;
	struct rdma_event_channel		*cm_channel;
	struct rdma_cm_id			*cm_id_control;
	struct rdma_cm_id			*cm_id;
	struct ibv_context			*context;
	struct ibv_comp_channel			*channel;
	struct ibv_pd				*pd;
	struct ibv_mr				**mr;
	struct ibv_cq				*send_cq;
	struct ibv_cq				*recv_cq;
	void					**buf;
	struct ibv_ah				**ah;
	struct ibv_qp				**qp;
	#ifdef HAVE_IBV_WR_API
	struct ibv_qp_ex			**qpx;
	int (*new_post_send_work_request_func_pointer) (struct pingpong_context *ctx, int index,
		struct perftest_parameters *user_param);
	#endif
	struct ibv_srq				*srq;
	struct ibv_sge				*sge_list;
	struct ibv_sge				*recv_sge_list;
	struct ibv_send_wr			*wr;
	struct ibv_recv_wr			*rwr;
	uint64_t				size;
	uint64_t				*my_addr;
	uint64_t				*rx_buffer_addr;
	uint64_t				*rem_addr;
	uint64_t				buff_size;
	uint64_t				send_qp_buff_size;
	uint64_t				flow_buff_size;
	int					tx_depth;
	int					huge_shmid;
	uint64_t				*scnt;
	uint64_t				*ccnt;
	int					is_contig_supported;
	uint32_t                                *ctrl_buf;
	uint32_t                                *credit_buf;
	struct ibv_mr                           *credit_mr;
	struct ibv_sge                          *ctrl_sge_list;
	struct ibv_send_wr                      *ctrl_wr;
	int                                     send_rcredit;
	int                                     credit_cnt;
	int					cache_line_size;
	int					cycle_buffer;
	#ifdef HAVE_XRCD
	struct ibv_xrcd				*xrc_domain;
	int 					fd;
	#endif
	#if defined(HAVE_VERBS_EXP)
        drv_exp_post_send_func			exp_post_send_func_pointer;
        drv_post_send_func			post_send_func_pointer;
	drv_poll_cq_func			poll_cq_func_pointer;
	struct ibv_exp_dct			**dct;
	struct ibv_exp_send_wr			*exp_wr;
	#endif
	#ifdef HAVE_ACCL_VERBS
	struct ibv_exp_res_domain		*res_domain;
	struct ibv_exp_cq_family		*send_cq_family;
	struct ibv_exp_cq_family		*recv_cq_family;
	struct ibv_exp_qp_burst_family		**qp_burst_family;
	#endif

};

 struct pingpong_dest {
	int 				lid;
	int 				out_reads;
	int 				qpn;
	int 				psn;
	unsigned			rkey;
	unsigned long long		vaddr;
	union ibv_gid			gid;
	unsigned			srqn;
	int				gid_index;
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
 *	or the first one found , in case ib_devname == NULL.
 *	Also sets the actual device name selected.
 *
 * Parameters :
 *
 *	ib_devname - The name of the device requested or NULL for the first one.
 *
 * Return Value : the device or NULL in case of failure.
 */
struct ibv_device* ctx_find_dev(char **ib_devname);

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

/* destroy_rdma_resources
 *
 * Description : Destroys the rdma_cm_id and rdma_channel for the rdma_cm QPs.
 *
 * Parameters :
 *	ctx - Resources sructure.
 * 	user_param - the perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int destroy_rdma_resources(struct pingpong_context *ctx,
                          struct perftest_parameters *user_param);

/* alloc_ctx
 *
 * Description : allocate all perftest resources.
 *
 * Parameters :
 *	ctx - Resources sructure.
 * 	user_param - the perftest parameters.
 */
void alloc_ctx(struct pingpong_context *ctx,struct perftest_parameters *user_param);

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
				struct perftest_parameters *user_param);

/* verify_params_with_device_context
 *
 * Description :
 * 		Verify user params that require information from the ibv_context
 *
 * Parameters :
 *	context - ibv_context
 * 	user_param - the perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int verify_params_with_device_context(struct ibv_context *ctx,
				      struct perftest_parameters *user_param);


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
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param, uint64_t init_flag);

/* ctx_connect.
 *
 * Description :
 *
 *	Modifies the given QP to RTR and then RTS states, given it's transport type and feature.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *  dest    - pingpong_dest struct of the remote side.
 *	user_param  - user_parameters struct for this test.
 *  my_dest - pingpong_dest struct of this side.
 *
 * Return Value : SUCCESS, FAILURE.
 *
 */
int ctx_connect(struct pingpong_context *ctx,
				struct pingpong_dest *dest,
				struct perftest_parameters *user_param,
				struct pingpong_dest *my_dest);

/* ctx_set_send_exp_wqes.
 *
 * Description :
 *
 *	Prepare the exp send work request templates for all QPs
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *  rem_dest   - pingpong_dest struct of the remote side.
 *
 */
void ctx_set_send_exp_wqes(struct pingpong_context *ctx,
					   struct perftest_parameters *user_param,
					   struct pingpong_dest *rem_dest);


/* ctx_set_send_regwqes.
 *
 * Description :
 *
 *	Prepare the regular send work request templates for all QPs
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *  rem_dest   - pingpong_dest struct of the remote side.
 *
 */
void ctx_set_send_reg_wqes(struct pingpong_context *ctx,
					   struct perftest_parameters *user_param,
					   struct pingpong_dest *rem_dest);

/* ctx_set_send_wqes.
 *
 * Description :
 *
 *	Prepare the send work request templates for all QPs
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *  rem_dest   - pingpong_dest struct of the remote side.
 *
 */
void ctx_set_send_wqes(struct pingpong_context *ctx,
					   struct perftest_parameters *user_param,
					   struct pingpong_dest *rem_dest);


/* ctx_set_recv_wqes.
 *
 * Description :
 *
 *	Prepare the receives work request templates for all QPs in SEND receive test.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *
 */
int ctx_set_recv_wqes(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* ctx_alloc_credit
 *
 * Description :
 *
 * 	Allocate resources to support the credit exchange mechanism,
 * 	which allows ib_send_bw to work with iWARP
 *
 * Parameters :
 *
 *	ctx        - test context
 *  	user_param - user parameters struct for this test
 *  	my_dest    - pingpong_dest struct of the this side
 *
 *  	my_dest is updated to store the credit buf vaddr and rkey
 *  	which need to be exchanged with the remote side
 *  	to enable RDMA WRITE op
 */
int ctx_alloc_credit(struct pingpong_context *ctx,
				struct perftest_parameters *user_param,
				struct pingpong_dest *my_dest);
/* ctx_set_credit_wqes
 *
 * Description :
 *
 * 	Prepare the send credit work request templates for all QPs
 * 	RDMA WRITE op is used for sending credit
 * 	Credit exchange is necessary for ib_send_bw to work with iWARP
 *
 * Parameters :
 *
 *	ctx        - test context
 *  	user_param - user parameters struct for this test
 *  	rem_dest   - pingpong_dest struct of the remote side.
 */
int ctx_set_credit_wqes(struct pingpong_context *ctx,
				struct perftest_parameters *user_param,
				struct pingpong_dest *rem_dest);
/* run_iter_bw.
 *
 * Description :
 *
 *	The main testing method in BW tests.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *
 */
int run_iter_bw(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* run_iter_bw_infinitely
 *
 * Description :
 *
 *	Infinite BW method, that prints BW every 5 seconds and never stops.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *
 */
int run_iter_bw_infinitely(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* run_iter_bw_infinitely_server
 *
 * Description :
 *
 *	Infinite on SEND server BW method, will not print anything and will post receive wqes for each incoming packet.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *
 */
int run_iter_bw_infinitely_server(struct pingpong_context *ctx, struct perftest_parameters *user_param);

/* run_iter_bw_server.
 *
 * Description :
 *
 *	The main testing method for Receiver in SEND test.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *
 */
int run_iter_bw_server(struct pingpong_context *ctx, struct perftest_parameters *user_param);

/* run_iter_bi.
 *
 * Description :
 *
 *	The main testing method for bidirrectional.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 *
 */
int run_iter_bi(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* run_iter_lat_write
 *
 * Description :
 *
 *  This is the latency test function for WRITE verb.
 *
 * Parameters :
 *
 *	ctx     - Test Context.
 *	user_param  - user_parameters struct for this test.
 */
int run_iter_lat_write(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* run_iter_lat
 *
 * Description :
 *
 *  This is the latency test function for READ or ATOMIC verb latency tests.
 *
 * Parameters :
 *
 *  ctx     - Test Context.
 *  user_param  - user_parameters struct for this test.
 */
int run_iter_lat(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* run_iter_lat_send
 *
 * Description :
 *
 *  This is the latency test function for SEND verb latency test.
 *
 * Parameters :
 *
 *  ctx     - Test Context.
 *  user_param  - user_parameters struct for this test.
 */
int run_iter_lat_send(struct pingpong_context *ctx, struct perftest_parameters *user_param);

/* run_iter_lat_burst
 *
 * Description :
 *
 *  This is the latency test function for SEND verb latency test in burst mode
 *
 * Parameters :
 *
 *  ctx     - Test Context.
 *  user_param  - user_parameters struct for this test.
 */

int run_iter_lat_burst(struct pingpong_context *ctx, struct perftest_parameters *user_param);

/* run_iter_lat_burst_server
 *
 * Description :
 *
 *  This is the latency test function for server side latency test in burst mode
 *
 * Parameters :
 *
 *  ctx     - Test Context.
 *  user_param  - user_parameters struct for this test.
 */

int run_iter_lat_burst_server(struct pingpong_context *ctx, struct perftest_parameters *user_param);

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
uint16_t ctx_get_local_lid(struct ibv_context *context, int ib_port);

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
static __inline int ctx_notify_events(struct ibv_comp_channel *channel)
{

	struct ibv_cq       *ev_cq;
	void                *ev_ctx;

	if (ibv_get_cq_event(channel,&ev_cq,&ev_ctx)) {
		fprintf(stderr, "Failed to get cq_event\n");
		return 1;
	}

	ibv_ack_cq_events(ev_cq,1);

	if (ibv_req_notify_cq(ev_cq, 0)) {
		fprintf(stderr, "Couldn't request CQ notification\n");
		return 1;
	}
	return 0;
}

#if defined(HAVE_VERBS_EXP)
static __inline void increase_exp_rem_addr(struct ibv_exp_send_wr *wr,int size,uint64_t scnt,uint64_t prim_addr,VerbType verb, int cache_line_size, int cycle_buffer)
{
	if (verb == ATOMIC)
		wr->wr.atomic.remote_addr += INC(size,cache_line_size);

	else
		wr->wr.rdma.remote_addr += INC(size,cache_line_size);

	if ( ((scnt+1) % (cycle_buffer/ INC(size,cache_line_size))) == 0) {

		if (verb == ATOMIC)
			wr->wr.atomic.remote_addr = prim_addr;

		else
			wr->wr.rdma.remote_addr = prim_addr;
	}
}
#endif
static __inline void increase_rem_addr(struct ibv_send_wr *wr,int size,uint64_t scnt,uint64_t prim_addr,VerbType verb, int cache_line_size, int cycle_buffer)
{
	if (verb == ATOMIC)
		wr->wr.atomic.remote_addr += INC(size,cache_line_size);

	else
		wr->wr.rdma.remote_addr += INC(size,cache_line_size);

	if ( ((scnt+1) % (cycle_buffer/ INC(size,cache_line_size))) == 0) {

		if (verb == ATOMIC)
			wr->wr.atomic.remote_addr = prim_addr;

		else
			wr->wr.rdma.remote_addr = prim_addr;
	}
}

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
static __inline void increase_loc_addr(struct ibv_sge *sg,int size,uint64_t rcnt,uint64_t prim_addr,int server_is_ud, int cache_line_size, int cycle_buffer)
{
	sg->addr  += INC(size,cache_line_size);

	if ( ((rcnt+1) % (cycle_buffer/ INC(size,cache_line_size))) == 0 )
		sg->addr = prim_addr;

}

/* catch_alarm.
 *
 * Description :
 * 	Signal catcher for duration feature.
 *  run_iter_bw in start will set it to triger at MARGIN (parameter), it will then start counting packets
 *  and triger it back to SAMPLE TIME, in that time the test will count packets and completion and
 *  will calculate BW accordingley.
 *
 */
void catch_alarm(int sig);

void check_alive(int sig);

/* catch_alarm.
 *
 * Description :
 * 	Signal catcher for run_infinitely feature.
 *  Will be triggered every 5 sec and measure BW in this time frame.
 *
 */
void catch_alarm_infintely();

/* handle_signal_print_thread
*
* Description :
* 	Handle thread creation for signal catching in run_infinitely mode
*
*/
void *handle_signal_print_thread(void *sig_mask);

/* ctx_modify_dc_qp_to_init.
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
int ctx_modify_dc_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *user_param);

int perform_warm_up(struct pingpong_context *ctx,struct perftest_parameters *user_param);

#ifdef HAVE_MASKED_ATOMICS
struct ibv_qp* ctx_atomic_qp_create(struct pingpong_context *ctx,
					struct perftest_parameters *user_param);
int check_masked_atomics_support(struct pingpong_context *ctx);
#endif

#if defined (HAVE_PACKET_PACING_EXP) || defined (HAVE_PACKET_PACING)
int check_packet_pacing_support(struct pingpong_context *ctx);
#if defined (HAVE_PACKET_PACING_EXTENSION_EXP)
int check_packet_pacing_extension_support(struct pingpong_context *ctx);
#endif
#endif

#ifdef HAVE_ACCL_VERBS
struct ibv_exp_res_domain* create_res_domain(struct pingpong_context *ctx,
						struct perftest_parameters *user_param);
#endif

int create_reg_qp_main(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int i, int num_of_qps);

#ifdef HAVE_VERBS_EXP
int create_exp_qp_main(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int i, int num_of_qps);
#endif

int create_qp_main(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int i, int num_of_qps);

#ifdef HAVE_VERBS_EXP
struct ibv_qp* ctx_exp_qp_create(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int qp_index);
#endif

int modify_qp_to_init(struct pingpong_context *ctx,
                struct perftest_parameters *user_param, int qp_index, int num_of_qps);


/* create_single_mr
 *
 * Description :
 *
 *	Creates a single MR for a specific QP index.
 *
 *	Parameters :
 *      	ctx - Resources sructure.
 *		user_param - the perftest parameters.
 *		qp_index  - QP index to register a MR
 *
 * Return Value : SUCCESS, FAILURE.
 *
 */
int create_single_mr(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int qp_index);

/* create_mr
 *
 * Description :
 *
 *	Creates Memory Regions for the test.
 *	Takes into consideration all user parameters and test type.
 *
 *	Parameters :
 *      	ctx - Resources sructure.
 *		user_param - the perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 *
 */
int create_mr(struct pingpong_context *ctx,
		struct perftest_parameters *user_param);

/* alloc_hugapage_region
 *
 * Description :
 *
 *	Creates hugepage memory Regions for the test.
 *
 *	Parameters :
 *      	ctx - Resources sructure.
 *
 * Return Value : SUCCESS, FAILURE.
 *
 */
int alloc_hugepage_region (struct pingpong_context *ctx, int qp_index);

/* run_iter_fs_rate
 *
 * Description :
 *
 *	The main testing method for Flow steering creation
 *
 * Parameters :
 *
 *	ctx		- Test Context.
 *	user_param	- user_parameters struct for this test.
 *
 */
int run_iter_fs(struct pingpong_context *ctx, struct perftest_parameters *user_param);

/* rdma_cm_allocate_nodes:
*
* Description:
*
*    Allocates RDMA CM nodes.
*    Allocates memory for the structures, creates RDMA CM IDs for every node.
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
int rdma_cm_allocate_nodes(struct pingpong_context *ctx,
	struct perftest_parameters *user_param, struct rdma_addrinfo *hints);

/* rdma_cm_destroy_qps:
*
* Description:
*
*    Destroys all RDMA CM QPs.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*
* Return value:
*    None.
*
*/
void rdma_cm_destroy_qps(struct pingpong_context *ctx,
	struct perftest_parameters *user_param);

/* rdma_cm_destroy_cma:
*
* Description:
*
*    Destroys RDMA CM resources.
*    Destroys RDMA CM IDs for every QP, event channel,
*    RDMA address info and frees RDMA CM nodes memory.
*
* Parameters:
*
*    ctx - Application contexts.
*    user_param - User parameters from the parser.
*
* Return value:
*    None.
*
*/
int rdma_cm_destroy_cma(struct pingpong_context *ctx,
	struct perftest_parameters *user_param);

/* error_handler:
*
* Description:
*
*    Error handler.
*    Prints error message with ERRNO string and returns FAILURE.
*
* Parameters:
*
*    error_message - Error message.
*
* Return value:
*    Failure: FAILURE(1).
*
*/
int error_handler(char *error_message);

#endif /* PERFTEST_RESOURCES_H */
