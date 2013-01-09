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

/******************************************************************************
 *
 ******************************************************************************/

void print_spec(struct ibv_flow_spec* spec)
{
	char str_ip_s[INET_ADDRSTRLEN] = {0};
	char str_ip_d[INET_ADDRSTRLEN] = {0};
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
	printf("%-22s|\n",eth_type);
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

void print_pkt(void* pkt,struct perftest_parameters *user_param)
{

	if(NULL == pkt)
	{
		printf("pkt is null:error happened can't print packet\n");
		return;
	}
	print_ethernet_header((ETH_header*)pkt);
	if(user_param->is_client_ip && user_param->is_server_ip)
	{
		pkt = (void*)pkt + sizeof(ETH_header);
		print_ip_header((IP_V4_header*)pkt);
	}
	if(user_param->is_client_port && user_param->is_server_port)
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
						 struct perftest_parameters *user_param,
						 uint16_t eth_type,
						 uint16_t ip_next_protocol,
						 int print_flag,
						 int sizePkt)
{
	void* header_buff = NULL;
	gen_eth_header(eth_header,my_dest_info->mac,rem_dest_info->mac,eth_type);
	if(user_param->is_client_ip && user_param->is_server_ip)
	{
		header_buff = (void*)eth_header + sizeof(ETH_header);
		gen_ip_header(header_buff,&my_dest_info->ip,&rem_dest_info->ip,ip_next_protocol,sizePkt);
	}
	if(user_param->is_client_port && user_param->is_server_port)
	{
		header_buff = header_buff + sizeof(IP_V4_header);
		gen_udp_header(header_buff,&my_dest_info->port,&rem_dest_info->port,my_dest_info->ip,rem_dest_info->ip,sizePkt);
	}

	if(print_flag == PRINT_ON)
	{
		print_pkt((void*)eth_header,user_param);
	}
}
/******************************************************************************
 *create_raw_eth_pkt - build raw Ethernet packet by user arguments
 *
 ******************************************************************************/
void create_raw_eth_pkt( struct perftest_parameters *user_param,
						 struct pingpong_context 	*ctx ,
						 struct raw_ethernet_info	*my_dest_info,
						 struct raw_ethernet_info	*rem_dest_info) {
	int offset = 0;
	ETH_header* eth_header;
    uint16_t eth_type = (user_param->is_client_ip && user_param->is_server_ip ? IP_ETHER_TYPE : (ctx->size-RAWETH_ADDITTION));
    uint16_t ip_next_protocol = (user_param->is_client_port && user_param->is_server_port ? UDP_PROTOCOL : 0);
    DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

    eth_header = (void*)ctx->buf;

    //build single packet on ctx buffer
	bulid_ptk_on_buffer(eth_header,my_dest_info,rem_dest_info,user_param,eth_type,ip_next_protocol,PRINT_ON,ctx->size-RAWETH_ADDITTION);

	//fill ctx buffer with same packets
	if (ctx->size <= (CYCLE_BUFFER / 2)) {
		while (offset < CYCLE_BUFFER-INC(ctx->size)) {
			offset += INC(ctx->size);
			eth_header = (void*)ctx->buf+offset;
			bulid_ptk_on_buffer(eth_header,my_dest_info,rem_dest_info,user_param,eth_type,ip_next_protocol,PRINT_OFF,ctx->size-RAWETH_ADDITTION);
		}
	}
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
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
		pspec->l2_id.eth.port = user_parm->ib_port;

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
int __cdecl main(int argc, char *argv[]) {

	struct ibv_device		    *ib_dev = NULL;
	struct pingpong_context  	ctx;
	struct raw_ethernet_info	my_dest_info,rem_dest_info;
	struct pingpong_dest 		my_dest,rem_dest;
	struct perftest_comm		user_comm;
	struct ibv_flow_spec 		spec;
	int							ret_parser;
	struct perftest_parameters user_param;

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
		if (ibv_attach_flow(ctx.qp[0], &spec, 0)){
			perror("error");
			fprintf(stderr, "Couldn't attach QP\n");
			return FAILURE;
		}
	}
	//build raw Ethernet packets on ctx buffer
	if(user_param.machine == CLIENT || user_param.duplex){
		create_raw_eth_pkt(&user_param,&ctx, &my_dest_info , &rem_dest_info);
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
		if(run_iter_bi(&ctx,&user_param))
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








