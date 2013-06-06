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

 #include "raw_ethernet_resources.h"




 
 /*****************************************************************************
 * generates a new ethernet header
 *****************************************************************************/
void gen_eth_header(struct ETH_header* eth_header,uint8_t* src_mac,
											uint8_t* dst_mac, uint16_t eth_type)
{
	memcpy(eth_header->src_mac, src_mac, 6);
	memcpy(eth_header->dst_mac, dst_mac, 6);
	eth_header->eth_type = htons(eth_type);

}
 

/******************************************************************************
 * print test specification
 ******************************************************************************/

void print_spec(struct ibv_flow_attr* flow_rules,struct perftest_parameters* user_parm)
{
	struct ibv_flow_spec* spec_info = NULL;

	void* header_buff = (void*)flow_rules;

	if (flow_rules == NULL) {
		printf("error : spec is NULL\n");
		return;
	}

	header_buff = header_buff + sizeof(struct ibv_flow_attr);
	spec_info = (struct ibv_flow_spec*)header_buff;
	printf("MAC attached  : %02X:%02X:%02X:%02X:%02X:%02X\n",
			spec_info->eth.val.dst_mac[0],
			spec_info->eth.val.dst_mac[1],
			spec_info->eth.val.dst_mac[2],
			spec_info->eth.val.dst_mac[3],
			spec_info->eth.val.dst_mac[4],
			spec_info->eth.val.dst_mac[5]);

	if (user_parm->is_server_ip && user_parm->is_client_ip) {
		char str_ip_s[INET_ADDRSTRLEN] = {0};
		char str_ip_d[INET_ADDRSTRLEN] = {0};
		header_buff = header_buff + sizeof(struct ibv_flow_spec_eth);
		spec_info = (struct ibv_flow_spec*)header_buff;
		uint32_t dst_ip = ntohl(spec_info->ipv4.val.dst_ip);
		uint32_t src_ip = ntohl(spec_info->ipv4.val.src_ip);
		inet_ntop(AF_INET, &dst_ip, str_ip_d, INET_ADDRSTRLEN);
		printf("spec_info - dst_ip   : %s\n",str_ip_d);
		inet_ntop(AF_INET, &src_ip, str_ip_s, INET_ADDRSTRLEN);
		printf("spec_info - src_ip   : %s\n",str_ip_s);
	}

	if (user_parm->is_server_port && user_parm->is_client_port) {

		header_buff = header_buff + sizeof(struct ibv_flow_spec_ipv4);
		spec_info = (struct ibv_flow_spec*)header_buff;

		printf("spec_info - dst_port : %d\n",spec_info->tcp_udp.val.dst_port);
		printf("spec_info - src_port : %d\n",spec_info->tcp_udp.val.src_port);
	}

}
/******************************************************************************
 *
 ******************************************************************************/
void print_ethernet_header(struct ETH_header* p_ethernet_header)
{

	if (NULL == p_ethernet_header)
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
		if (NULL == ip_header)
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
		printf("|protocol  |%-12s|\n",ip_header->protocol == UDP_PROTOCOL ? "UDP" : "EMPTY");
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
	print_ethernet_header((struct ETH_header*)pkt);
	if(user_param->is_client_ip && user_param->is_server_ip)
	{
		pkt = (void*)pkt + sizeof(struct ETH_header);
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
void build_pkt_on_buffer(struct ETH_header* eth_header,
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
		header_buff = (void*)eth_header + sizeof(struct ETH_header);
		gen_ip_header(header_buff,&my_dest_info->ip,&rem_dest_info->ip,ip_next_protocol,sizePkt);
	}
	if(user_param->is_client_port && user_param->is_server_port)
	{
		header_buff = header_buff + sizeof(IP_V4_header);
		gen_udp_header(header_buff,&my_dest_info->port,&rem_dest_info->port,
									my_dest_info->ip,rem_dest_info->ip,sizePkt);
	}

	if(print_flag == PRINT_ON)
	{
		print_pkt((void*)eth_header,user_param);
	}
}
/******************************************************************************
 *create_raw_eth_pkt - build raw Ethernet packet by user arguments
 *on bw test, build one packet and duplicate it on the buffer
 *on lat test, build only one packet on the buffer (for the ping pong method)
 ******************************************************************************/
void create_raw_eth_pkt( struct perftest_parameters *user_param,
						 struct pingpong_context 	*ctx ,
						 struct raw_ethernet_info	*my_dest_info,
						 struct raw_ethernet_info	*rem_dest_info) 
{
	int offset = 0;
	struct ETH_header* eth_header;
    uint16_t eth_type = (user_param->is_client_ip && user_param->is_server_ip ? IP_ETHER_TYPE : (ctx->size-RAWETH_ADDITION));
    uint16_t ip_next_protocol = (user_param->is_client_port && user_param->is_server_port ? UDP_PROTOCOL : 0);
    DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

    eth_header = (void*)ctx->buf;

    //build single packet on ctx buffer
	build_pkt_on_buffer(eth_header,my_dest_info,rem_dest_info,user_param,eth_type,ip_next_protocol,PRINT_ON,ctx->size-RAWETH_ADDITION);

	if (user_param->tst == BW) {
		//fill ctx buffer with same packets
		if (ctx->size <= (CYCLE_BUFFER / 2)) {
			while (offset < CYCLE_BUFFER-INC(ctx->size)) {
				offset += INC(ctx->size);
				eth_header = (void*)ctx->buf+offset;
				build_pkt_on_buffer(eth_header,my_dest_info,rem_dest_info,
										user_param,eth_type,ip_next_protocol,
											PRINT_OFF,ctx->size-RAWETH_ADDITION);
			}
		}
	}
	
	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
}

/******************************************************************************
 calc_flow_rules_size
 ******************************************************************************/
int calc_flow_rules_size(int is_ip_header,int is_udp_header)
{
	int tot_size = sizeof(struct ibv_flow_attr);
	tot_size += sizeof(struct ibv_flow_spec_eth);
	if (is_ip_header)
		tot_size += sizeof(struct ibv_flow_spec_ipv4);
	if (is_udp_header)
		tot_size += sizeof(struct ibv_flow_spec_tcp_udp);
	return tot_size;
}

/******************************************************************************
 *send_set_up_connection - init raw_ethernet_info and ibv_flow_spec to user args
 ******************************************************************************/
 int send_set_up_connection(
	struct ibv_flow_attr **flow_rules,
	struct pingpong_context *ctx,
	struct perftest_parameters *user_parm,
	struct raw_ethernet_info* my_dest_info,
	struct raw_ethernet_info* rem_dest_info)
{

	union ibv_gid temp_gid;

	if (user_parm->gid_index != -1) {
		if (ibv_query_gid(ctx->context,user_parm->ib_port,user_parm->gid_index,&temp_gid)) {
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return FAILURE;
		}
	}

	if (user_parm->machine == SERVER || user_parm->duplex) {

		void* header_buff;
		struct ibv_flow_spec* spec_info;
		struct ibv_flow_attr* attr_info;
		int flow_rules_size;

		int is_ip = user_parm->is_server_ip || user_parm->is_client_ip;
		int is_port = user_parm->is_server_port || user_parm->is_client_port;

		flow_rules_size = calc_flow_rules_size(is_ip,is_port);

		ALLOCATE(header_buff,uint8_t,flow_rules_size);

		memset(header_buff, 0,flow_rules_size);
		*flow_rules = (struct ibv_flow_attr*)header_buff;
		attr_info = (struct ibv_flow_attr*)header_buff;
		attr_info->comp_mask = 0;
		attr_info->type = IBV_FLOW_ATTR_NORMAL;
		attr_info->size = flow_rules_size;
		attr_info->priority = 0;
		attr_info->num_of_specs = 1 + is_ip + is_port;
		attr_info->port = user_parm->ib_port;
		attr_info->flags = 0;
		header_buff = header_buff + sizeof(struct ibv_flow_attr);
		spec_info = (struct ibv_flow_spec*)header_buff;
		spec_info->eth.type = IBV_FLOW_SPEC_ETH;
		spec_info->eth.size = sizeof(struct ibv_flow_spec_eth);
		spec_info->eth.val.ether_type = 0;

		if(user_parm->is_source_mac) {
			mac_from_user(spec_info->eth.val.dst_mac , &(user_parm->source_mac[0]) , sizeof(user_parm->source_mac));
		} else {
			mac_from_gid(spec_info->eth.val.dst_mac, temp_gid.raw);
		}

		memset(spec_info->eth.mask.dst_mac, 0xFF,sizeof(spec_info->eth.mask.src_mac));
		if(user_parm->is_server_ip && user_parm->is_client_ip) {

			header_buff = header_buff + sizeof(struct ibv_flow_spec_eth);
			spec_info = (struct ibv_flow_spec*)header_buff;
			spec_info->ipv4.type = IBV_FLOW_SPEC_IPV4;
			spec_info->ipv4.size = sizeof(struct ibv_flow_spec_ipv4);

			if(user_parm->machine == SERVER) {

				spec_info->ipv4.val.dst_ip = htonl(user_parm->server_ip);
				spec_info->ipv4.val.src_ip = htonl(user_parm->client_ip);

			} else{

				spec_info->ipv4.val.dst_ip = htonl(user_parm->client_ip);
				spec_info->ipv4.val.src_ip = htonl(user_parm->server_ip);
			}

			memset((void*)&spec_info->ipv4.mask.dst_ip, 0xFF,sizeof(spec_info->ipv4.mask.dst_ip));
			memset((void*)&spec_info->ipv4.mask.src_ip, 0xFF,sizeof(spec_info->ipv4.mask.src_ip));
		}

		if(user_parm->is_server_port && user_parm->is_client_port) {

			header_buff = header_buff + sizeof(struct ibv_flow_spec_ipv4);
			spec_info = (struct ibv_flow_spec*)header_buff;
			spec_info->tcp_udp.type = IBV_FLOW_SPEC_UDP;
			spec_info->tcp_udp.size = sizeof(struct ibv_flow_spec_tcp_udp);

			if(user_parm->machine == SERVER) {

				spec_info->tcp_udp.val.dst_port = user_parm->server_port;
				spec_info->tcp_udp.val.src_port = user_parm->client_port;

			} else{
				spec_info->tcp_udp.val.dst_port = user_parm->client_port;
				spec_info->tcp_udp.val.src_port = user_parm->server_port;
			}

			memset((void*)&spec_info->tcp_udp.mask.dst_port, 0xFF,sizeof(spec_info->ipv4.mask.dst_ip));
			memset((void*)&spec_info->tcp_udp.mask.src_port, 0xFF,sizeof(spec_info->ipv4.mask.src_ip));
		}
	}

	if (user_parm->machine == CLIENT || user_parm->duplex) {

		//set source mac
		if(user_parm->is_source_mac) {
			mac_from_user(my_dest_info->mac , &(user_parm->source_mac[0]) , sizeof(user_parm->source_mac) );

		} else {
			mac_from_gid(my_dest_info->mac, temp_gid.raw );
		}

		//set dest mac
		mac_from_user(rem_dest_info->mac , &(user_parm->dest_mac[0]) , sizeof(user_parm->dest_mac) );

		if(user_parm->is_client_ip) {
			if(user_parm->machine == CLIENT) {
				my_dest_info->ip = user_parm->client_ip;
			} else {
				my_dest_info->ip = user_parm->server_ip;
			}
		}

		if(user_parm->machine == CLIENT) {
			rem_dest_info->ip = user_parm->server_ip;
			my_dest_info->port = user_parm->client_port;
			rem_dest_info->port = user_parm->server_port;
		}

		if(user_parm->machine == SERVER && user_parm->duplex) {
			rem_dest_info->ip = user_parm->client_ip;
			my_dest_info->port = user_parm->server_port;
			rem_dest_info->port = user_parm->client_port;
		}
	}
	return 0;
}

