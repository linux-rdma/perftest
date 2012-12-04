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
#include <getopt.h>
#include </usr/include/netinet/ip.h>
#include <poll.h>
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "multicast_resources.h"
#include "perftest_communication.h"



#define VERSION 1.0

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
 *build single packet on ctx buffer
 *
 ******************************************************************************/
void bulid_ptk_on_buffer(ETH_header* eth_header,
						 struct raw_ethernet_info *my_dest_info,
						 struct raw_ethernet_info *rem_dest_info,
						 uint16_t eth_type,
						 uint16_t ip_next_protocol,
						 int print_flag,
						 int sizePkt)
{
	void* header_buff = NULL;
	gen_eth_header(eth_header,my_dest_info->mac,rem_dest_info->mac,eth_type);
	if(user_param.is_client_ip && user_param.is_server_ip)
	{
		header_buff = (void*)eth_header + sizeof(ETH_header);
		gen_ip_header(header_buff,&my_dest_info->ip,&rem_dest_info->ip,ip_next_protocol,sizePkt);
	}
	if(user_param.is_client_port && user_param.is_server_port)
	{
		header_buff = header_buff + sizeof(IP_V4_header);
		gen_udp_header(header_buff,&my_dest_info->port,&rem_dest_info->port,my_dest_info->ip,rem_dest_info->ip,sizePkt);
	}

	if(print_flag == PRINT_ON)
	{
		print_pkt((void*)eth_header);
	}
}
/******************************************************************************
 *create_raw_eth_pkt - build raw Ethernet packet by user arguments
 *
 ******************************************************************************/
void create_raw_eth_pkt( struct pingpong_context *ctx ,  struct raw_ethernet_info	 *my_dest_info ,struct raw_ethernet_info	 *rem_dest_info) {
	int offset = 0;
	ETH_header* eth_header;
    uint16_t eth_type = (user_param.is_client_ip && user_param.is_server_ip ? IP_ETHER_TYPE : (ctx->size-RAWETH_ADDITTION));
    uint16_t ip_next_protocol = (user_param.is_client_port && user_param.is_server_port ? UDP_PROTOCOL : 0);
    DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

    eth_header = (void*)ctx->buf;

    //build single packet on ctx buffer
	bulid_ptk_on_buffer(eth_header,my_dest_info,rem_dest_info,eth_type,ip_next_protocol,PRINT_ON,ctx->size-RAWETH_ADDITTION);

	//fill ctx buffer with same packets
	if (ctx->size <= (CYCLE_BUFFER / 2)) {
		while (offset < CYCLE_BUFFER-INC(ctx->size)) {
			offset += INC(ctx->size);
			eth_header = (void*)ctx->buf+offset;
			bulid_ptk_on_buffer(eth_header,my_dest_info,rem_dest_info,eth_type,ip_next_protocol,PRINT_OFF,ctx->size-RAWETH_ADDITTION);
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
 *send_set_up_connection - init raw_ethernet_info and ibv_flow_spec to user args
 ******************************************************************************/
static int send_set_up_connection(struct ibv_flow_spec* pspec,
								  struct pingpong_context *ctx,
								  struct perftest_parameters *user_parm,
								  struct pingpong_dest *my_dest,
								  struct pingpong_dest *rem_dest,
								  struct raw_ethernet_info* my_dest_info,
								  struct raw_ethernet_info* rem_dest_info,
								  struct perftest_comm *comm) {
	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

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
	my_dest->qpn = ctx->qp[0]->qp_num;

	if(user_parm->machine == SERVER || user_parm->duplex){

		//default value to spec
		pspec->l2_id.eth.vlan_present = 0;
		pspec->type = IBV_FLOW_ETH;
		pspec->l4_protocol = IBV_FLOW_L4_NONE;
		pspec->l2_id.eth.port = user_param.ib_port;

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
			mac_from_user(my_dest_info->mac , &(user_parm->source_mac[0]) , sizeof(user_parm->source_mac) );
		}
		else
		{
			mac_from_gid(my_dest_info->mac, my_dest->gid.raw );//default option
		}
		//set dest mac
		mac_from_user(rem_dest_info->mac , &(user_parm->dest_mac[0]) , sizeof(user_parm->dest_mac) );

		if(user_parm->is_client_ip)
		{
			if(user_parm->machine == CLIENT)
			{
				my_dest_info->ip = user_parm->client_ip;
			}else{
				my_dest_info->ip = user_parm->server_ip;
			}
		}

		if(user_parm->machine == CLIENT)
		{
			rem_dest_info->ip = user_parm->server_ip;
			my_dest_info->port = user_parm->client_port;
			rem_dest_info->port = user_parm->server_port;
		}

		if(user_parm->machine == SERVER && user_parm->duplex)
		{
			rem_dest_info->ip = user_parm->client_ip;
			my_dest_info->port = user_parm->server_port;
			rem_dest_info->port = user_parm->client_port;
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


	//if (ibv_req_notify_cq(ctx->recv_cq, 0)) {
	//	fprintf(stderr, " Couldn't request CQ notification\n");
	//	return FAILURE;
	//}

	if((user_param->test_type == DURATION )&& (user_param->connection_type != RawEth || (user_param->machine == CLIENT && firstRx)))
	{
			firstRx = false;
			//duration_param=user_param;
			user_param->iters=0;
			user_param->state = START_STATE;
			signal(SIGALRM, catch_alarm_eth);
			alarm(user_param->margin);
	}
	while ( totccnt < tot_iters || totrcnt < tot_iters || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

		for (index=0; index < user_param->num_of_qps; index++) {

			while (((ctx->scnt[index] < iters) || ((firstRx == false) && (user_param->test_type == DURATION)))&& ((ctx->scnt[index] - ctx->ccnt[index]) < user_param->tx_depth)) {

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1))
					ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && user_param->state == END_STATE)
					break;

				if (ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr)) {
					fprintf(stderr,"Couldn't post send: qp %d scnt=%d \n",index,ctx->scnt[index]);
					return 1;
				}

				if (user_param->post_list == 1 && user_param->size <= (CYCLE_BUFFER / 2))
					increase_loc_addr(ctx->wr[index].sg_list,user_param->size,ctx->scnt[index],ctx->my_addr[index],0);

				//print_pkt((ETH_header*)ctx->wr[index].sg_list->addr);

				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				//if (user_param->test_type == DURATION && user_param->state == SAMPLE_STATE)
				//	user_param->iters ++ ;//user_param->iters += user_param->cq_mod;

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
			//printf("Rx_def = %d",user_param->rx_depth);
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

				//if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
				//	user_param->iters += user_param->cq_mod;

				for (i = 0; i < ne; i++) {
					if (user_param->state == END_STATE) break;
					if (wc[i].status != IBV_WC_SUCCESS)
						NOTIFY_COMP_ERROR_RECV(wc[i],(int)totrcnt);

					rcnt_for_qp[wc[i].wr_id]++;
					totrcnt++;

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
										user_param->iters++;

					if (user_param->test_type == DURATION ||(rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth <= iters)) {
						//ibv_post_recv(ctx->qp[i],&ctx->rwr[i],&bad_wr_recv)

						if (ibv_post_recv(ctx->qp[wc[i].wr_id],&ctx->rwr[wc[i].wr_id],&bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%d\n",(int)wc[i].wr_id , rcnt_for_qp[wc[i].wr_id]);
							return 15;
						}

						if ((SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine)) <= (CYCLE_BUFFER / 2)){

							increase_loc_addr(ctx->rwr[wc[i].wr_id].sg_list,
											  user_param->size,
											  rcnt_for_qp[wc[i].wr_id] + user_param->rx_depth-1,
											  ctx->my_addr[wc[i].wr_id],
											  user_param->connection_type);
							//printf("increase_loc_addr: %d\n",i);
						}
					}
					//printf("wc[i].wr_id = %d\n",wc[i].wr_id);
					print_pkt((ETH_header*)ctx->rwr[wc[i].wr_id].sg_list->addr);
				}
				//printf("wc[i].wr_id = %d\n",wc[i].wr_id);
			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}

		if ((totccnt < tot_iters) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {

			ne = ibv_poll_cq(ctx->send_cq,user_param->tx_depth*user_param->num_of_qps,wc_tx);
			//printf("ne send: %d\n",ne);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					//printf("ne send: %d\n",ne);
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

					if (user_param->test_type==DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters += user_param->cq_mod;
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

/******************************************************************************
 *
 ******************************************************************************/
int __cdecl main(int argc, char *argv[]) {

	struct ibv_device		    *ib_dev = NULL;
	struct pingpong_context  	ctx;
	struct raw_ethernet_info	my_dest_info,rem_dest_info;
	struct pingpong_dest 		my_dest,rem_dest;
	struct perftest_comm		user_comm;
	struct ibv_flow_spec 		spec;
	int							ret_parser;

	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

	/* init default values to user's parameters */
	memset(&ctx, 0,sizeof(struct pingpong_context));
	memset(&user_param, 0 , sizeof(struct perftest_parameters));
	memset(&user_comm, 0,sizeof(struct perftest_comm));
	memset(&my_dest, 0 , sizeof(struct pingpong_dest));
	memset(&rem_dest, 0 , sizeof(struct pingpong_dest));
	memset(&my_dest_info, 0 , sizeof(struct raw_ethernet_info));
	memset(&rem_dest_info, 0 , sizeof(struct raw_ethernet_info));
	memset(&spec, 0, sizeof(struct ibv_flow_spec));


	user_param.verb    = SEND;
	user_param.tst     = BW;
	user_param.version = VERSION;
	user_param.connection_type = RawEth;

	ret_parser = parser(&user_param,argv,argc);
	if (ret_parser) {
		if (ret_parser != VERSION_EXIT && ret_parser != HELP_EXIT) { 
			fprintf(stderr," Parser function exited with Error\n");
		}
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
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

	// create all the basic IB resources (data buffer, PD, MR, CQ and events channel)
	if (ctx_init(&ctx,&user_param)) {
		fprintf(stderr, " Couldn't create IB resources\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return FAILURE;
	}

	// Set up the Connection.
	//set mac address by user choose
	if (send_set_up_connection(&spec,&ctx,&user_param,&my_dest,&rem_dest,&my_dest_info,&rem_dest_info,&user_comm)) {
		fprintf(stderr," Unable to set up socket connection\n");
		return 1;
	}

	if(user_param.machine == SERVER || user_param.duplex){
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
	//build raw Ethernet packets on ctx buffer
	if(user_param.machine == CLIENT || user_param.duplex)
	{
		create_raw_eth_pkt(&ctx, &my_dest_info , &rem_dest_info);
	}

	printf(RESULT_LINE);//change the printing of the test
	printf(RESULT_FMT);

	// Prepare IB resources for rtr/rts.
	if (ctx_connect(&ctx,&rem_dest,&user_param,&my_dest)) {
		fprintf(stderr," Unable to Connect the HCA's through the link\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
		return 1;
	}

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

	if (destroy_ctx(&ctx, &user_param)){
		fprintf(stderr,"Failed to destroy_ctx\n");
		DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return 1;
	}

	printf(RESULT_LINE);
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
	return 0;
}








