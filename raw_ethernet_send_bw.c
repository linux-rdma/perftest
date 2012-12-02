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

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include </usr/include/netinet/ip.h>
#include <poll.h>
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "multicast_resources.h"
#include "perftest_communication.h"



#define VERSION 4.0

//#define DEBUG//if u want debug prints need to define that


#define INFO "INFO"
#define TRACE "TRACE"

#ifdef DEBUG
#define DEBUG_LOG(type,fmt, args...) fprintf(stderr,"file:%s: %d ""["type"]"fmt"\n",__FILE__,__LINE__,args)
#else
#define DEBUG_LOG(type,fmt, args...)
#endif

#ifdef _WIN32
#pragma warning( disable : 4242)
#pragma warning( disable : 4244)
#else
#define __cdecl
#endif

#define IP_ETHER_TYPE (0x800)
#define PRINT_ON (1)
#define PRINT_OFF (0)
#define SEC_TO_WAIT_FOR_PACKET (100)

cycles_t	*tposted;
cycles_t	*tcompleted;
volatile cycles_t		start_traffic = 0;
volatile cycles_t		end_traffic = 0;
volatile cycles_t		start_sample = 0;
volatile cycles_t		end_sample = 0;
struct perftest_parameters user_param;
//uint8_t 			raweth_mac[6] = { 0, 2, 201, 1, 1, 1}; //to this mac we send the RawEth trafic the first 0 stands for unicast this is the default 								       // for multicast use { 1, 2, 201, 1, 1, 1}
//uint64_t rcnt = 0, scnt = 0, ccnt = 0, global_cnt = 0;

/******************************************************************************
 *
 ******************************************************************************/
/*static void print_report(struct perftest_parameters *user_param) {

	double cycles_to_units;
	unsigned long tsize;	/* Transferred size, in megabytes */
	//int i, j, a, b;
	//int opt_posted = start_index, opt_completed = start_index;
	//cycles_t opt_delta = 0;
	//cycles_t t;

	//DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	//opt_delta = tcompleted[opt_posted] - tposted[opt_completed];

	//if (user_param->noPeak == OFF && user_param->test_type == ITERATIONS) {
		/* Find the peak bandwidth, unless asked not to in command line */
	/*	for (a = 0, i = start_index; a < user_param->iters; ++a, i = (i+1)%user_param->iters)
			for (b = a, j = i; b < user_param->iters; ++b, j = (j+1)%user_param->iters) {
				t = (tcompleted[j] - tposted[i]) / (b - a + 1);
				if (t < opt_delta) {
					opt_delta  = t;
					opt_posted = i;
					opt_completed = j;
				}
			}
	}*/

	/*cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f) * 1000000;
	//global_cnt = user_param->duplex ? 2*global_cnt : global_cnt;
	//tsize = user_param->duplex ? 2 : 1;

	tsize = user_param->size;
	if (user_param->test_type == ITERATIONS) {
		printf(REPORT_FMT_BW_RAW, (unsigned long) user_param->size,
			user_param->iters,
			(user_param->noPeak == OFF) * tsize * cycles_to_units / opt_delta / 0x100000,
			tsize * user_param->iters * cycles_to_units / (tcompleted[user_param->iters - 1] - tposted[0]) / 0x100000,
			( 1 + user_param->duplex) * user_param->iters * cycles_to_units / (tcompleted[user_param->iters - 1] - tposted[0])/ 1000000 );
	}else {
		printf(REPORT_FMT_BW_RAW, (unsigned long) user_param->size,
			global_cnt,
			0.00,
			tsize * global_cnt * cycles_to_units / (end_sample - start_sample) / 0x100000,
			global_cnt * cycles_to_units / (end_sample - start_sample) / 1000000);
	}
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
}*/


/******************************************************************************
 *
 ******************************************************************************/

void print_spec(struct ibv_flow_spec* spec)
{
	char str_ip_s[INET_ADDRSTRLEN];
	char str_ip_d[INET_ADDRSTRLEN];
	if(spec == NULL)
	{
		printf("error : spec is NULL\n");
		return;
	}
	inet_ntop(AF_INET, &spec->dst_ip, str_ip_d, INET_ADDRSTRLEN);
	printf("spec.dst_ip   : %s\n",str_ip_d);
	inet_ntop(AF_INET, &spec->src_ip, str_ip_s, INET_ADDRSTRLEN);
	printf("spec.src_ip   : %s\n",str_ip_s);
	printf("spec.dst_port : %d\n",ntohs(spec->dst_port));
	printf("spec.src_port : %d\n",ntohs(spec->src_port));
	printf("MAC attached  : %02X:%02X:%02X:%02X:%02X:%02X\n",
					spec->l2_id.eth.mac[0],
					spec->l2_id.eth.mac[1],
					spec->l2_id.eth.mac[2],
					spec->l2_id.eth.mac[3],
					spec->l2_id.eth.mac[4],
					spec->l2_id.eth.mac[5]);
}
/******************************************************************************
 *
 ******************************************************************************/
void print_ethernet_header(ETH_header* p_ethernet_header)
{

	if(NULL == p_ethernet_header)
	{
		fprintf(stderr, "ETH_header pointer is Null\n");
		return;
	}
	printf("**raw ethernet header****************************************\n\n");
	printf("--------------------------------------------------------------\n");
	printf("| Dest MAC         | Src MAC          | Packet Type          |\n");
	printf("|------------------------------------------------------------|\n");
	printf("|");
	printf(PERF_MAC_FMT,
			p_ethernet_header->dst_mac[0],
			p_ethernet_header->dst_mac[1],
			p_ethernet_header->dst_mac[2],
			p_ethernet_header->dst_mac[3],
			p_ethernet_header->dst_mac[4],
			p_ethernet_header->dst_mac[5]);
	printf("|");
	printf(PERF_MAC_FMT,
			p_ethernet_header->src_mac[0],
			p_ethernet_header->src_mac[1],
			p_ethernet_header->src_mac[2],
			p_ethernet_header->src_mac[3],
			p_ethernet_header->src_mac[4],
			p_ethernet_header->src_mac[5]);
	printf("|");
	char* eth_type = (ntohs(p_ethernet_header->eth_type) ==  IP_ETHER_TYPE ? "IP" : "DEFAULT");
	printf("%s(%-18X)|\n",eth_type,p_ethernet_header->eth_type);
	printf("|------------------------------------------------------------|\n\n");

}
/******************************************************************************
 *
 ******************************************************************************/
void print_ip_header(IP_V4_header* ip_header)
{
		char str_ip_s[INET_ADDRSTRLEN];
		char str_ip_d[INET_ADDRSTRLEN];
		if(NULL == ip_header)
		{
			fprintf(stderr, "IP_V4_header pointer is Null\n");
			return;
		}

		printf("**IP header**************\n");
		printf("|-----------------------|\n");
		printf("|Version   |%-12d|\n",ip_header->version);
		printf("|Ihl       |%-12d|\n",ip_header->ihl);
		printf("|TOS       |%-12d|\n",ip_header->tos);
		printf("|TOT LEN   |%-12d|\n",ntohs(ip_header->tot_len));
		printf("|ID        |%-12d|\n",ntohs(ip_header->id));
		printf("|Frag      |%-12d|\n",ntohs(ip_header->frag_off));
		printf("|TTL       |%-12d|\n",ip_header->ttl);
		printf("|protocol  |%-12s|\n",ip_header->protocol == UDP_PROTOCOL ? "UPD" : "EMPTY");
		printf("|Check sum |%-12X|\n",ntohs(ip_header->check));
		inet_ntop(AF_INET, &ip_header->saddr, str_ip_s, INET_ADDRSTRLEN);
		printf("|Source IP |%-12s|\n",str_ip_s);
		inet_ntop(AF_INET, &ip_header->daddr, str_ip_d, INET_ADDRSTRLEN);
		printf("|Dest IP   |%-12s|\n",str_ip_d);
		printf("|-----------------------|\n\n");
}
/******************************************************************************
 *
 ******************************************************************************/
void print_udp_header(UDP_header* udp_header)
{
		if(NULL == udp_header)
		{
			fprintf(stderr, "udp_header pointer is Null\n");
			return;
		}
		printf("**UDP header***********\n");
		printf("|---------------------|\n");
		printf("|Src  Port |%-10d|\n",ntohs(udp_header->uh_sport));
		printf("|Dest Port |%-10d|\n",ntohs(udp_header->uh_dport));
		printf("|Len       |%-10d|\n",ntohs(udp_header->uh_ulen));
		printf("|check sum |%-10d|\n",ntohs(udp_header->uh_sum));
		printf("|---------------------|\n");
}
/******************************************************************************
 *
 ******************************************************************************/

void print_pkt(void* pkt)
{

	if(NULL == pkt)
	{
		printf("pkt is null:error happened can't print packet\n");
		return;
	}
	print_ethernet_header((ETH_header*)pkt);
	if(user_param.is_client_ip && user_param.is_server_ip)
	{
		pkt = (void*)pkt + sizeof(ETH_header);
		print_ip_header((IP_V4_header*)pkt);
	}
	if(user_param.is_client_port && user_param.is_server_port)
	{
		pkt = pkt + sizeof(IP_V4_header);
		print_udp_header((UDP_header*)pkt);
	}
}
/******************************************************************************
 *
 ******************************************************************************/
void bulid_ptk_on_buffer(struct pingpong_context *ctx ,ETH_header* eth_header,struct pingpong_dest *my_dest,struct pingpong_dest *rem_dest,uint16_t eth_type,uint16_t ip_next_protocol,int print_flag,int sizePkt)
{
	void* header_buff = NULL;
	gen_eth_header(eth_header,my_dest->mac,rem_dest->mac,eth_type);
	if(user_param.is_client_ip && user_param.is_server_ip)
	{
		header_buff = (void*)eth_header + sizeof(ETH_header);
		gen_ip_header(header_buff,&my_dest->ip,&rem_dest->ip,ip_next_protocol,sizePkt);
	}
	if(user_param.is_client_port && user_param.is_server_port)
	{
		header_buff = header_buff + sizeof(IP_V4_header);
		gen_udp_header(header_buff,&my_dest->port,&rem_dest->port,my_dest->ip,rem_dest->ip,sizePkt);
	}

	if(print_flag == PRINT_ON)
	{
		print_pkt((void*)eth_header);
	}
}
/******************************************************************************
 *
 *
 ******************************************************************************/
void create_raw_eth_pkt_with_ip_header( struct pingpong_context *ctx ,  struct pingpong_dest	 *my_dest ,struct pingpong_dest	 *rem_dest) {
	int offset = 0;
	ETH_header* eth_header;
    uint16_t eth_type = (user_param.is_client_ip && user_param.is_server_ip ? IP_ETHER_TYPE : (ctx->size-RAWETH_ADDITTION));
    uint16_t ip_next_protocol = (user_param.is_client_port && user_param.is_server_port ? UDP_PROTOCOL : 0);
    DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	eth_header = (void*)ctx->buf;

	bulid_ptk_on_buffer(ctx ,eth_header,my_dest,rem_dest,eth_type,ip_next_protocol,PRINT_ON,ctx->size-RAWETH_ADDITTION);

	if (ctx->size <= (CYCLE_BUFFER / 2)) {
		while (offset < CYCLE_BUFFER-INC(ctx->size)) {
			offset += INC(ctx->size);
			eth_header = (void*)ctx->buf+offset;
			bulid_ptk_on_buffer(ctx ,eth_header,my_dest,rem_dest,eth_type,ip_next_protocol,PRINT_OFF,ctx->size-RAWETH_ADDITTION);
		}
	}
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
}
/******************************************************************************
 *
 ******************************************************************************/
void catch_alarm_eth(int sig) {
	switch (user_param.state) {
		case START_STATE:
			user_param.state = SAMPLE_STATE;
			user_param.tposted[0] = get_cycles();
			start_sample = user_param.tposted[0];
			alarm(user_param.duration - 2*(user_param.margin));
			break;
		case SAMPLE_STATE:
			user_param.state = STOP_SAMPLE_STATE;
			user_param.tcompleted[0] = get_cycles();
			end_sample = user_param.tcompleted[0];
			alarm(user_param.margin);
			break;
		case STOP_SAMPLE_STATE:
			user_param.state = END_STATE;
			break;
		default:
			fprintf(stderr,"unknown state\n");
	}
}
/******************************************************************************
 *
 ******************************************************************************/
static int send_set_up_connection(struct ibv_flow_spec* pspec,
								  struct pingpong_context *ctx,
								  struct perftest_parameters *user_parm,
								  struct pingpong_dest *my_dest,
								  struct pingpong_dest *rem_dest,
								  //struct mcast_parameters *mcg_params,
								  struct perftest_comm *comm) {
	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	/*if (user_parm->use_mcgIP && (user_parm->duplex || user_parm->machine == SERVER)) {

		mcg_params->user_mgid = user_parm->user_mgid;
		set_multicast_gid(mcg_params,ctx->qp[0]->qp_num,(int)user_parm->machine);
		if (set_mcast_group(ctx,user_parm,mcg_params)) {
			return 1;
		}*/

		/*while (i < user_parm->num_of_qps) {
			if (ibv_attach_mcast(ctx->qp[i],&mcg_params->mgid,mcg_params->mlid)) {
				fprintf(stderr, "Couldn't attach QP to MultiCast group");
				return 1;
			}
			i++;
		}

		mcg_params->mcast_state |= MCAST_IS_ATTACHED;
		my_dest->gid = mcg_params->mgid;
		my_dest->lid = mcg_params->mlid;
		my_dest->qpn = QPNUM_MCAST;

	} else {*/



		if (user_parm->gid_index != -1) {
			if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&my_dest->gid)) {
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return -1;
			}
		}
		// We do not fail test upon lid above RoCE.

		if (user_parm->gid_index < 0)
		{
			if (!my_dest->lid) {
				fprintf(stderr," Local lid 0x0 detected,without any use of gid. Is SM running?\n");
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return -1;
			}
		}
		my_dest->lid = ctx_get_local_lid(ctx->context,user_parm->ib_port);
		//DEBUG_LOG(INFO,"my_dest->lid : %d",my_dest->lid);
		my_dest->qpn = ctx->qp[0]->qp_num;



	if(user_parm->machine == SERVER || user_parm->duplex){

		if(user_parm->is_server_ip && user_parm->is_client_ip)
		{
			if(user_parm->machine == SERVER)
			{
				pspec->dst_ip = user_parm->server_ip;
				pspec->src_ip = user_parm->client_ip;
			}else{
				pspec->dst_ip = user_parm->client_ip;
				pspec->src_ip = user_parm->server_ip;
			}

			pspec->l2_id.eth.ethertype = htons(IP_ETHER_TYPE);
		}
		if(user_parm->is_server_port && user_parm->is_client_port)
		{
			if(user_parm->machine == SERVER)
			{
				pspec->dst_port = htons(user_parm->server_port);
				pspec->src_port = htons(user_parm->client_port);
			}else{
				pspec->dst_port = htons(user_parm->client_port);
				pspec->src_port = htons(user_parm->server_port);
			}
			pspec->l4_protocol = IBV_FLOW_L4_UDP;
		}

		if(user_parm->is_source_mac)
		{
			mac_from_user(pspec->l2_id.eth.mac , &(user_parm->source_mac[0]) , sizeof(user_parm->source_mac) );
		}
		else
		{
			mac_from_gid(pspec->l2_id.eth.mac, my_dest->gid.raw );//default option
		}
	}

	if(user_parm->machine == CLIENT || user_parm->duplex)
	{
		//set source mac
		if(user_parm->is_source_mac)
		{
			mac_from_user(my_dest->mac , &(user_parm->source_mac[0]) , sizeof(user_parm->source_mac) );
		}
		else
		{
			mac_from_gid(my_dest->mac, my_dest->gid.raw );//default option
		}
		//set dest mac
		mac_from_user(rem_dest->mac , &(user_parm->dest_mac[0]) , sizeof(user_parm->dest_mac) );
		if(user_parm->is_client_ip)
		{
			if(user_parm->machine == CLIENT)
			{
				my_dest->ip = user_parm->client_ip;
			}else{
				my_dest->ip = user_parm->server_ip;
			}
		}
		else
		{
			/// need to add default
		}
		//}
		if(user_parm->machine == CLIENT)
		{
			rem_dest->ip = user_parm->server_ip;
			my_dest->port = user_parm->client_port;
			rem_dest->port = user_parm->server_port;
		}
		if(user_parm->machine == SERVER && user_parm->duplex)
		{
			rem_dest->ip = user_parm->client_ip;
			my_dest->port = user_parm->server_port;
			rem_dest->port = user_parm->client_port;
		}

	#ifndef _WIN32
		my_dest->psn   = lrand48() & 0xffffff;
	#else
		my_dest->psn   = rand() & 0xffffff;
	#endif
	}

	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int pp_connect_ctx(struct pingpong_context *ctx,int my_psn,
             struct pingpong_dest *dest,
						  struct perftest_parameters *user_parm)
{
	struct ibv_qp_attr attr;
	memset(&attr, 0, sizeof attr);
	int i;
	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	attr.qp_state 		= IBV_QPS_RTR;
	attr.path_mtu       = user_parm->curr_mtu;
    attr.dest_qp_num    = dest->qpn;
	attr.rq_psn         = dest->psn;
	attr.ah_attr.dlid   = dest->lid;
	if (user_parm->connection_type == RC) {
		attr.max_dest_rd_atomic     = 1;
		attr.min_rnr_timer          = 12;
	}
	if (user_parm->gid_index < 0) {
		attr.ah_attr.is_global  = 0;
		attr.ah_attr.sl         = user_parm->sl;
	} else {
		attr.ah_attr.is_global  = 1;
		attr.ah_attr.grh.dgid   = dest->gid;
		attr.ah_attr.grh.sgid_index = user_parm->gid_index;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.sl         = 0;
	}
	attr.ah_attr.src_path_bits = 0;
	attr.ah_attr.port_num   = user_parm->ib_port;

	if (user_parm->connection_type == RC) {
		if (ibv_modify_qp(ctx->qp[0], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN             |
				  IBV_QP_MIN_RNR_TIMER      |
				  IBV_QP_MAX_DEST_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify RC QP to RTR\n");
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
		attr.timeout            = user_parm->qp_timeout;
		attr.retry_cnt          = 7;
		attr.rnr_retry          = 7;
	} else if (user_parm->connection_type == UC) {
		if (ibv_modify_qp(ctx->qp[0], &attr,
				  IBV_QP_STATE              |
				  IBV_QP_AV                 |
				  IBV_QP_PATH_MTU           |
				  IBV_QP_DEST_QPN           |
				  IBV_QP_RQ_PSN)) {
			fprintf(stderr, "Failed to modify UC QP to RTR\n");
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
	}

	else {
		for (i = 0; i < user_parm->num_of_qps; i++) {
			if (ibv_modify_qp(ctx->qp[i],&attr,IBV_QP_STATE )) {
				fprintf(stderr, "Failed to modify UD QP to RTR\n");
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}
		}
		if (user_parm->machine == CLIENT || user_parm->duplex) {
			ctx->ah[0] = ibv_create_ah(ctx->pd,&attr.ah_attr);
			if (!ctx->ah) {
				fprintf(stderr, "Failed to create AH for UD\n");
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}
		}
	}

	if (user_parm->machine == CLIENT || user_parm->duplex) {

		attr.qp_state 	    = IBV_QPS_RTS;
		attr.sq_psn 	    = my_psn;
		if (user_parm->connection_type == RC) {
			attr.max_rd_atomic  = 1;
			if (ibv_modify_qp(ctx->qp[0], &attr,
					IBV_QP_STATE              |
					IBV_QP_SQ_PSN             |
					IBV_QP_TIMEOUT            |
					IBV_QP_RETRY_CNT          |
					IBV_QP_RNR_RETRY          |
					IBV_QP_MAX_QP_RD_ATOMIC)) {
				fprintf(stderr, "Failed to modify RC QP to RTS\n");
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}

		} else if (user_parm->connection_type == UC || user_parm->connection_type == UD){
			if(ibv_modify_qp(ctx->qp[0],&attr,IBV_QP_STATE |IBV_QP_SQ_PSN)) {
				fprintf(stderr, "Failed to modify UC/UD QP to RTS\n");
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}
		}
		else {
			if (ibv_modify_qp(ctx->qp[0],&attr,IBV_QP_STATE )) {
				fprintf(stderr, "Failed to modify RawEth QP to RTS\n");
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}
		}
	}
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_bi_eth(struct pingpong_context *ctx,
				struct perftest_parameters *user_param)  {

	uint64_t				totscnt    = 0;
	uint64_t				totccnt    = 0;
	uint64_t				totrcnt    = 0;
	int 					i,index      = 0;
	int 					ne = 0;
	int						*rcnt_for_qp = NULL;
	int 					tot_iters = 0;
	int 					iters = 0;
	struct ibv_wc 			*wc          = NULL;
	struct ibv_wc 			*wc_tx		 = NULL;
	struct ibv_recv_wr      *bad_wr_recv = NULL;
	struct ibv_send_wr 		*bad_wr      = NULL;
	bool  					firstRx = true;

	ALLOCATE(wc,struct ibv_wc,user_param->rx_depth*user_param->num_of_qps);
	ALLOCATE(wc_tx,struct ibv_wc,user_param->tx_depth*user_param->num_of_qps);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	tot_iters = user_param->iters*user_param->num_of_qps;
	iters=user_param->iters;

	if (user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();


	if (ibv_req_notify_cq(ctx->recv_cq, 0)) {
		fprintf(stderr, " Couldn't request CQ notification\n");
		return FAILURE;
	}

	if((user_param->test_type == DURATION )&& (user_param->connection_type != RawEth || (user_param->machine == CLIENT && firstRx)))
	{
			firstRx = false;
			//duration_param=user_param;
			user_param->iters=0;
			user_param->state = START_STATE;
			signal(SIGALRM, catch_alarm_eth);
			alarm(user_param->margin);
	}
	while ((firstRx == false) && ((user_param->test_type == ITERATIONS && ( totccnt < tot_iters || totrcnt < tot_iters)) || (user_param->test_type == DURATION && user_param->state != END_STATE))) {

		for (index=0; index < user_param->num_of_qps; index++) {

			while ((user_param->test_type == ITERATIONS && (ctx->scnt[index] < iters)) || ((user_param->test_type == DURATION && user_param->state != END_STATE)&& ((ctx->scnt[index] - ctx->ccnt[index]) < user_param->tx_depth))) {

				if (user_param->state == END_STATE) break;

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1))
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}

				if (user_param->post_list == 1 && user_param->size <= (CYCLE_BUFFER / 2))
					increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],ctx->my_addr[index],0);

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->test_type == DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters ++ ;//user_param->iters += user_param->cq_mod;

				if (user_param->post_list == 1 && (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || (user_param->test_type == ITERATIONS && ctx->scnt[index] == iters-1)))
					ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
			}
		}

		if (user_param->use_event) {

			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr,"Failed to notify events to CQ");
				return 1;
			}
		}

		if ((user_param->test_type == ITERATIONS && (totrcnt < tot_iters)) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

			ne = ibv_poll_cq(ctx->recv_cq,user_param->rx_depth*user_param->num_of_qps,wc);
			//printf("ne recv: %d\n",ne);
			if (ne > 0) {
				if(user_param->connection_type == RawEth)
				{
					if (user_param->machine == SERVER && firstRx && user_param->test_type == DURATION) {
						firstRx = false;
						//duration_param=user_param;
						user_param->iters=0;
						user_param->state = START_STATE;
						signal(SIGALRM, catch_alarm_eth);
						alarm(user_param->margin);
					}
				}

				if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
					user_param->iters += user_param->cq_mod;

				for (i = 0; i < ne; i++) {
					if (user_param->state == END_STATE) break;
					if (wc[i].status != IBV_WC_SUCCESS)
						NOTIFY_COMP_ERROR_RECV(wc[i],(int)totrcnt);

					rcnt_for_qp[wc[i].wr_id]++;
					totrcnt++;

					if (user_param->test_type == DURATION || (user_param->test_type == ITERATIONS && (rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth <= iters))) {
						//ibv_post_recv(ctx->qp[i],&ctx->rwr[i],&bad_wr_recv)
						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id , rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (CYCLE_BUFFER / 2) && user_param->connection_type != RawEth)
							increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
											  user_param->size,
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth - 1,
											  ctx->my_addr[wc[i].wr_id],
											  user_param->connection_type);

						print_pkt((ETH_header*)ctx->rwr[wc[i].wr_id].sg_list->addr);
					}
				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}

		if ((user_param->test_type == ITERATIONS && (totccnt < tot_iters)) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

			ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth*user_param->num_of_qps,wc_tx);
			//printf("ne send: %d\n",ne);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {

					if (wc_tx[i].status != IBV_WC_SUCCESS)
						 NOTIFY_COMP_ERROR_SEND(wc_tx[i],(int)totscnt,(int)totccnt);

					totccnt += user_param->cq_mod;
					ctx->ccnt[(int)wc_tx[i].wr_id] += user_param->cq_mod;

					if (user_param->noPeak == OFF) {

						if ((user_param->test_type == ITERATIONS && (totccnt >= tot_iters - 1)))
							user_param->tcompleted[tot_iters - 1] = get_cycles();
						else
							user_param->tcompleted[totccnt-1] = get_cycles();
					}


				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}
	}

	if (user_param->noPeak == ON)
		user_param->tcompleted[0] = get_cycles();

	free(rcnt_for_qp);
	free(wc);
	free(wc_tx);
	return 0;
}

/*static int set_recv_wqes(struct pingpong_context *ctx,
						 struct perftest_parameters *user_param,
						 struct ibv_recv_wr *rwr,
						 struct ibv_sge *sge_list,
						 uint64_t *my_addr) {

	int					i,j;
	int 				duplex_ind;
	struct ibv_recv_wr  *bad_wr_recv;
	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	i = (user_param->duplex && user_param->use_mcg) ? 1 : 0;
	duplex_ind = (user_param->duplex && !user_param->use_mcg) ? 1 : 0;

	while (i < user_param->num_of_qps) {

		sge_list[i].addr = (uintptr_t)ctx->buf + (user_param->num_of_qps + i)*BUFF_SIZE(ctx->size);

		sge_list[i].length = (user_param->connection_type == RawEth) ? SIZE(user_param->connection_type,user_param->size - HW_CRC_ADDITION,1)
										 : SIZE(user_param->connection_type,user_param->size,(user_param->machine == SERVER || user_param->duplex));

		sge_list[i].lkey   = ctx->mr->lkey;
		rwr[i].sg_list     = &sge_list[i];
		rwr[i].wr_id       = i;
		rwr[i].next        = NULL;
		rwr[i].num_sge	   = MAX_RECV_SGE;
		my_addr[i]		   = sge_list[i].addr;

		for (j = 0; j < user_param->rx_depth; ++j) {

			if (ibv_post_recv(ctx->qp[i],&rwr[i],&bad_wr_recv)) {
				perror("ibv_post_recv");
				fprintf(stderr, "Couldn't post recv Qp = %d: counter=%d\n",i,j);
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}

			if (SIZE(user_param->connection_type,
					 user_param->size,
					(user_param->machine == SERVER ||
					 user_param->duplex)) <= (CYCLE_BUFFER / 2))

				increase_loc_addr(&sge_list[i],
								  SIZE(user_param->connection_type,
									   user_param->size,
									   (user_param->machine == SERVER ||
									   user_param->duplex)),
								  j,
								  my_addr[i],
								  user_param->connection_type);
		}

		i++;
	}
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
/*static void set_send_wqe(struct pingpong_context *ctx,int rem_qpn,
						 struct perftest_parameters *user_param,
						 struct ibv_sge *list,
						 struct ibv_send_wr *wr) {
	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	list->addr     = (uintptr_t)ctx->buf;
	list->lkey 	   = ctx->mr->lkey;

	wr->sg_list    = list;
	wr->num_sge    = 1;
	wr->opcode     = IBV_WR_SEND;
	wr->next       = NULL;
	wr->wr_id      = PINGPONG_SEND_WRID;
	wr->send_flags = IBV_SEND_SIGNALED;

	if (user_param->connection_type == UD) {
		wr->wr.ud.ah          = ctx->ah[0];
		wr->wr.ud.remote_qkey = DEF_QKEY;
		wr->wr.ud.remote_qpn  = rem_qpn;
	}
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
}*/



/****************************************************************************** 
 * Important note :
 * In case of UD/UC this is NOT the way to measureBW since we are running with
 * loop on the send side , while we should run on the recieve side or enable
 * retry in SW , Since the sender may be faster than the reciver.
 * Although	we had posted recieve it is not enough and might end this will
 * result in deadlock of test since both sides are stuck on poll cq.
 * In this test i do not solve this for the general test ,need to write
 * seperate test for UC/UD but in case the tx_depth is ~1/3 from the
 * number of iterations this should be ok .
 * Also note that the sender is limited in the number of send, ans
 * i try to make the reciver full .
 ******************************************************************************/
/*int run_iter_bi(struct pingpong_context *ctx,
				struct perftest_parameters *user_param,
				struct ibv_recv_wr *rwr,
				struct ibv_send_wr *wr,
				uint64_t *my_addr)  {
	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	int 					i,index      = 0;
	int 					ne = 0;
	int						*rcnt_for_qp = NULL;
	struct ibv_wc 			*wc          = NULL;
	struct ibv_wc 			*wc_tx		 = NULL;
	struct ibv_recv_wr      *bad_wr_recv = NULL;
	struct ibv_send_wr 		*bad_wr      = NULL;
	bool first_Rx = true;

	ALLOCATE(wc,struct ibv_wc,user_param->rx_depth*user_param->num_of_qps);
	ALLOCATE(wc_tx,struct ibv_wc,user_param->tx_depth*user_param->num_of_qps);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);

	wr->sg_list->length = (user_param->connection_type == RawEth) ? user_param->size - HW_CRC_ADDITION : user_param->size;
	wr->sg_list->addr   = (uintptr_t)ctx->buf;
	wr->send_flags = IBV_SEND_SIGNALED;

	if (user_param->size <= user_param->inline_size)
		wr->send_flags |= IBV_SEND_INLINE;

	if (user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	if (ibv_req_notify_cq(ctx->recv_cq, 0)) {
		fprintf(stderr, " Couldn't request CQ notification\n");
		return FAILURE;
	}

	while (ccnt < user_param->iters*user_param->num_of_qps || rcnt < user_param->iters*user_param->num_of_qps || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
		while ((scnt < user_param->iters || user_param->test_type == DURATION )&&((scnt - ccnt) < (user_param->tx_depth) )) {

			if (user_param->state == END_STATE) break;

			if ((scnt % user_param->cq_mod == 0 && user_param->cq_mod > 1))
				wr->send_flags &= ~IBV_SEND_SIGNALED;

			if (user_param->noPeak == OFF)
				user_param->tposted[scnt] = get_cycles();

			if (ibv_post_send(ctx->qp[0],wr,&bad_wr)) {
				fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
				return 1;
			}


			if (user_param->size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(wr->sg_list,user_param->size,scnt,(uintptr_t)ctx->buf,0);
			if (user_param->state == SAMPLE_STATE)
			{
				++global_cnt;
			}
			//fprintf(stderr, "ibv_post_send\n");
			++scnt;

			if (((scnt % user_param->cq_mod) == (user_param->cq_mod - 1)) || (user_param->test_type == ITERATIONS && scnt == user_param->iters-1))
				wr->send_flags |= IBV_SEND_SIGNALED;
		}



		if (rcnt < user_param->iters*user_param->num_of_qps || user_param->test_type == DURATION) {

			ne = ibv_poll_cq(ctx->recv_cq,user_param->tx_depth*user_param->num_of_qps,wc);
			//fprintf(stderr, "ibv_poll_cq ctx->recv_cq ne=%d\n",ne);
			if (ne > 0) {
				if(user_param->machine == SERVER){
					if(first_Rx && (user_param->test_type == DURATION))
					{
						user_param->state = START_STATE;
						signal(SIGALRM, catch_alarm_eth);
						alarm(user_param->margin);
						first_Rx = false;
						global_cnt = 0;
						start_traffic=get_cycles();
					}
				}
				if (user_param->state == SAMPLE_STATE)  //DURATION
						global_cnt += user_param->cq_mod;

				//fprintf(stderr, "ibv_poll_cq ctx->recv_cq ne=%d\n",ne);
				for (i = 0; i < ne; i++) {
					if (user_param->state == END_STATE) break;
					if (wc[i].status != IBV_WC_SUCCESS)
						NOTIFY_COMP_ERROR_RECV(wc[i],(int)rcnt);

					rcnt_for_qp[wc[i].wr_id]++;
					rcnt++;

					if (rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth <= user_param->iters || user_param->test_type == DURATION) {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id , rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						//fprintf(stderr, "ibv_post_recv\n");
						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (CYCLE_BUFFER / 2))
							increase_loc_addr(rwr[wc[i].wr_id].sg_list,
											  user_param->size,
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth - 1,
											  my_addr[wc[i].wr_id],
											  user_param->connection_type);
						print_pkt((ETH_header*)rwr[wc[i].wr_id].sg_list->addr);
					}
				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}



		if (ccnt < user_param->iters*user_param->num_of_qps || user_param->test_type == DURATION) {

			ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth*user_param->num_of_qps,wc_tx);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {

					if (wc_tx[i].status != IBV_WC_SUCCESS)
						 NOTIFY_COMP_ERROR_SEND(wc_tx[i],(int)scnt,(int)ccnt);

					ccnt += user_param->cq_mod;

					//ctx->ccnt[(int)wc_tx[i].wr_id] += user_param->cq_mod;
				}

			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}
	}

	if (user_param->noPeak == ON)
		user_param->tcompleted[0] = get_cycles();

	free(rcnt_for_qp);
	free(wc);
	free(wc_tx);
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}*/

/******************************************************************************
 *
 ******************************************************************************/
/*int run_iter_uni_server(struct pingpong_context *ctx,
						struct perftest_parameters *user_param,
						struct ibv_recv_wr *rwr,
						uint64_t *my_addr,
						struct ibv_sge* sge) {

	int 				rcnt = 0;
	int 				i, ne = 0 ;
	int                 *rcnt_for_qp = NULL;
	struct ibv_wc 		*wc          = NULL;
	struct ibv_recv_wr  *bad_wr_recv = NULL;
	bool first_Rx = true;
	//uint64_t my_addr;

	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	ALLOCATE(wc,struct ibv_wc,DEF_WC_SIZE);
	ALLOCATE(rcnt_for_qp,int,user_param->num_of_qps);
	memset(rcnt_for_qp,0,sizeof(int)*user_param->num_of_qps);
	while ((user_param->test_type == ITERATIONS && rcnt < user_param->iters*user_param->num_of_qps) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

			//requesting a CQ notification
		if (ibv_req_notify_cq(ctx->recv_cq, 0)) {
			fprintf(stderr, " Couldn't request CQ notification\n");
			return FAILURE;
		}

		if (user_param->state == END_STATE) goto deallocate;
		/*if (user_param->use_event) {
			if (ctx_notify_events(ctx->recv_cq,ctx->channel)) {
				fprintf(stderr ," Failed to notify events to CQ");
				goto err;
			}
		}*/

	/*	do {
			if (user_param->state == END_STATE) goto deallocate;
			ne = ibv_poll_cq(ctx->recv_cq,DEF_WC_SIZE,wc);
			//printf("ne = %d\n",ne);
			if (ne > 0) {
				if(first_Rx && (user_param->test_type == DURATION))
				{
					user_param->state = START_STATE;
					signal(SIGALRM, catch_alarm_eth);
					alarm(user_param->margin);
					first_Rx = false;
					global_cnt = 0;
					start_traffic=get_cycles();
				}
				for (i = 0; i < ne; i++) {

					if (user_param->state == END_STATE) goto deallocate;
					if (wc[i].status != IBV_WC_SUCCESS) {

						NOTIFY_COMP_ERROR_RECV(wc[i],rcnt_for_qp[0]);
					}

					rcnt_for_qp[wc[i].wr_id]++;
					/*if (user_param->test_type == ITERATIONS)
						tcompleted[rcnt++] = get_cycles();*/
				/*	if (user_param->state == SAMPLE_STATE || (user_param->state == END_STATE && global_cnt == 0)){
						++global_cnt;
					}
					if (rcnt + user_param->rx_depth*user_param->num_of_qps <= user_param->iters*user_param->num_of_qps ||
							user_param->test_type == DURATION) {

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id,rcnt_for_qp[wc[i].wr_id]);
							goto err;
						}
						if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (CYCLE_BUFFER / 2)) {
							increase_loc_addr(rwr[wc[i].wr_id].sg_list,
											  SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine),
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth,
											  my_addr[wc[i].wr_id],user_param->connection_type);
						}
					}
					//print_pkt((ETH_header*)rwr[wc[i].wr_id].sg_list->addr);
				}
			}
		} while (ne > 0);

		if (ne < 0) {
			fprintf(stderr, "Poll Recieve CQ failed %d\n", ne);
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
	}


	tposted[0] = tcompleted[0];
	goto deallocate;

err:
	free(wc);
	free(rcnt_for_qp);
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 1;
deallocate:
	free(wc);
	free(rcnt_for_qp);
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
/*int run_iter_uni_client(struct pingpong_context *ctx,
						struct perftest_parameters *user_param,
						struct ibv_send_wr *wr) {

	int 		       ne = 0;
	int 			   i    = 0;
	int                scnt = 0;
	int                ccnt = 0;
	struct ibv_wc      *tx_wc     = NULL;
	struct ibv_send_wr *bad_wr = NULL;

	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);
	ALLOCATE(tx_wc,struct ibv_wc,user_param->tx_depth*user_param->num_of_qps);
	// Set the lenght of the scatter in case of ALL option.
	wr->sg_list->length = (user_param->connection_type == RawEth) ? user_param->size - HW_CRC_ADDITION : user_param->size;
	wr->sg_list->addr   = (uintptr_t)ctx->buf;
	wr->send_flags = IBV_SEND_SIGNALED;
	if (user_param->size <= user_param->inline_size)
		wr->send_flags |= IBV_SEND_INLINE;
	while ((user_param->test_type == ITERATIONS && (scnt < user_param->iters || ccnt < user_param->iters)) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
		while ((scnt < user_param->iters || user_param->test_type == DURATION) && (scnt - ccnt) < user_param->tx_depth ) {
			if (user_param->state == END_STATE) break;
			if (scnt %  user_param->cq_mod == 0 && user_param->cq_mod > 1)
				wr->send_flags &= ~IBV_SEND_SIGNALED;
			if (user_param->test_type == ITERATIONS) {
				tposted[scnt] = get_cycles();
				}
			if (ibv_post_send(ctx->qp[0], wr, &bad_wr)) {
				perror("ibv_post_send");
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}
			if (user_param->size <= (CYCLE_BUFFER / 2))
				increase_loc_addr(wr->sg_list,user_param->size,scnt,(uintptr_t)ctx->buf,0);

			scnt++;
			if ((scnt % user_param->cq_mod) == (user_param->cq_mod - 1) || (user_param->test_type == ITERATIONS && scnt == (user_param->iters - 1)))
				wr->send_flags |= IBV_SEND_SIGNALED;
		}
		if (ccnt < user_param->iters || (user_param->test_type == DURATION && ccnt < scnt)) {
			if (user_param->state == END_STATE) break;
			//if (user_param->use_event) {
				/*if (ctx_notify_events(ctx->cq,ctx->channel)) { natali - remove from options
					fprintf(stderr , " Failed to notify events to CQ");
					return 1;
				}*/
			//}
			/*do {
				if (user_param->state == END_STATE) break;
				ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth*user_param->num_of_qps,tx_wc);
				if (ne > 0) {
					for (i = 0; i < ne; i++) {
						if (tx_wc[i].status != IBV_WC_SUCCESS)
							NOTIFY_COMP_ERROR_SEND(tx_wc[i],scnt,ccnt);
						ccnt += user_param->cq_mod;
						if (user_param->state == SAMPLE_STATE)  //DURATION
							global_cnt += user_param->cq_mod;
						if (user_param->test_type == ITERATIONS) {
							if (ccnt >= user_param->iters - 1)
								tcompleted[user_param->iters - 1] = get_cycles();
							else
								tcompleted[ccnt - 1] = get_cycles();
						}
					}
				}
			} while (ne > 0);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed\n");
				DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
				return 1;
			}
		}
	}

	if (user_param->size <= user_param->inline_size)
		wr->send_flags &= ~IBV_SEND_INLINE;

	free(tx_wc);
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}
/****************************************************************************** 
 *
 ******************************************************************************/


int __cdecl main(int argc, char *argv[]) {

	struct ibv_device		    *ib_dev = NULL;
	struct pingpong_context  	ctx;
	struct pingpong_dest	 	my_dest,rem_dest;
	struct perftest_comm		user_comm;
	//struct mcast_parameters     mcg_params;
	//struct ibv_sge          	list;
	//struct ibv_send_wr      	wr;
	//struct ibv_sge              *sge_list = NULL;
	struct ibv_flow_spec 		spec;
	//struct ibv_recv_wr      	*rwr = NULL;
	//int				            size_of_arr;
	//uint64_t                    *my_addr = NULL;
	//int                      	i = 0;
	//int     			size_min_pow=1;
	//int                 size_max_pow = 24;
	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

	/* init default values to user's parameters */
	memset(&ctx, 0,sizeof(struct pingpong_context));
	memset(&user_param, 0 , sizeof(struct perftest_parameters));
	//memset(&mcg_params, 0 , sizeof(struct mcast_parameters));
	memset(&user_comm, 0,sizeof(struct perftest_comm));
	memset(&my_dest, 0 , sizeof(struct pingpong_dest));
	memset(&rem_dest, 0 , sizeof(struct pingpong_dest));
	memset(&spec, 0, sizeof(struct ibv_flow_spec));


	user_param.verb    = SEND;
	user_param.tst     = BW;
	user_param.version = VERSION;

	// Configure the parameters values according to user arguments or defalut values.
	if (raw_eth_parser(&user_param,argv,argc)) {
		fprintf(stderr," Parser function exited with Error\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	//default value to spec
	spec.l2_id.eth.vlan_present = 0;
	spec.type = IBV_FLOW_ETH;
	spec.l4_protocol = IBV_FLOW_L4_NONE ;
	spec.l2_id.eth.port = user_param.ib_port;

	user_param.connection_type = RawEth;


	if(user_param.machine == SERVER /*|| user_param.duplex*/)
	{
		ctx.size = DEF_SIZE_BW;
	}

	// Finding the IB device selected (or defalut if no selected).
	ib_dev = ctx_find_dev(user_param.ib_devname);
	if (!ib_dev) {
		fprintf(stderr," Unable to find the Infiniband/RoCE deivce\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	// Getting the relevant context from the device
	ctx.context = ibv_open_device(ib_dev);
	if (!ctx.context) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	// See if MTU and link type are valid and supported.
	if (check_link_and_mtu(ctx.context,&user_param)) {
		fprintf(stderr, " Couldn't get context for the device\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	// Allocating arrays needed for the test.

	alloc_ctx(&ctx,&user_param);
	// Print basic test information.
	ctx_print_test_info(&user_param);

	//size_min_pow =  (int)UD_MSG_2_EXP(RAWETH_MIN_MSG_SIZE);
	//size_max_pow =  (int)UD_MSG_2_EXP(user_param.curr_mtu);

//i=size_min_pow;
//do {

	//if (user_param.all == ON)
	//{
	//		user_param.size = 1 << i;
	//		ctx.size = user_param.size;
	//		ctx.buff_size = BUFF_SIZE(ctx.size)*2*user_param.num_of_qps;
	//}


	// copy the rellevant user parameters to the comm struct
	if (create_comm_struct(&user_comm,&user_param)) {
		fprintf(stderr," Unable to create RDMA_CM resources\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	// create all the basic IB resources (data buffer, PD, MR, CQ and events channel)
	if (ctx_init(&ctx,&user_param)) {
		fprintf(stderr, " Couldn't create IB resources\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	// Set up the Connection.
	//set mac address by user choose
	if (send_set_up_connection(&spec,&ctx,&user_param,&my_dest,&rem_dest,&user_comm)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}

	if(user_param.machine == SERVER || user_param.duplex)
	{
		print_spec(&spec);
	}

	//attaching the qp to the spec
	if(user_param.machine == SERVER || user_param.duplex){
		if (ibv_attach_flow(ctx.qp[0], &spec, 0))
		{
			perror("error");
			fprintf(stderr, "Couldn't attach QP\n");
			return FAILURE;
		}
	}
	//raw_eth_print_pingpong_data();
	if(user_param.machine == CLIENT || user_param.duplex)
	{
		create_raw_eth_pkt_with_ip_header(&ctx, &my_dest , &rem_dest);
	}

	printf(RESULT_LINE);//change the printing of the test
	printf(RESULT_FMT);

	// Prepare IB resources for rtr/rts.
	if (pp_connect_ctx(&ctx,my_dest.psn,&rem_dest,&user_param)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

	/*if (user_param.use_event) {

		if (ibv_req_notify_cq(ctx.cq, 0)) {
			fprintf(stderr, " Couldn't request CQ notification\n");
			return 1;
			}
	}*/

	//size_of_arr = (user_param.duplex) ? 1 : user_param.num_of_qps;

//	ALLOCATE(tposted,cycles_t,user_param.iters*size_of_arr);
//	ALLOCATE(tcompleted,cycles_t,user_param.iters*size_of_arr);

//	if (user_param.machine == SERVER || user_param.duplex) {
//		ALLOCATE(rwr,struct ibv_recv_wr,user_param.num_of_qps);
//		ALLOCATE(sge_list,struct ibv_sge,user_param.num_of_qps);
//		ALLOCATE(my_addr ,uint64_t ,user_param.num_of_qps);
//	}

	if (user_param.machine == CLIENT || user_param.duplex) {
		ctx_set_send_wqes(&ctx,&user_param,&rem_dest);
	}

	if (user_param.machine == SERVER || user_param.duplex) {
		if (ctx_set_recv_wqes(&ctx,&user_param)) {
			fprintf(stderr," Failed to post receive recv_wqes\n");
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
		}
	}
	if (user_param.duplex) {
		if(run_iter_bi_eth(&ctx,&user_param))
				return 17;
			}else if (user_param.machine == CLIENT) {
				if(run_iter_bw(&ctx,&user_param)) {
					DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
					return 17;
				}
				} else	{
						if(run_iter_bw_server(&ctx,&user_param)) {
							DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
							return 17;
						}
					}
	print_report_bw(&user_param);

	if(user_param.machine == SERVER || user_param.duplex){
			if (ibv_detach_flow(ctx.qp[0], &spec, 0))
			{
				perror("error");
				fprintf(stderr, "Couldn't attach QP\n");
				return FAILURE;
			}
		}

	//if ((user_param.all == ON) && (i < size_max_pow) && (i >= size_min_pow))
	//{
	//	#ifndef _WIN32
	//			free(ctx.buf);
	//	#else
	//			posix_memfree(ctx.buf);
	//	#endif
	//}
	//i++;
//}while ((i >= size_min_pow) && (i <= size_max_pow) && (user_param.all == ON));
	/**************need to free memory*************/

	if (destroy_ctx(&ctx, &user_param)){
		fprintf(stderr,"Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
	}

	printf(RESULT_LINE);
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}








