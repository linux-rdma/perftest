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

struct perftest_parameters* duration_param;

int check_flow_steering_support() {
	char* file_name = "/sys/module/mlx4_core/parameters/log_num_mgm_entry_size";
	FILE *fp;
	char line[4];
	fp = fopen(file_name, "r");      //open file , read only
	fgets(line,4,fp);
	int val = atoi(line);

	if (val >= 0) {
		fprintf(stderr,"flow steering is not supported.\n");
		fprintf(stderr," please run: echo options mlx4_core log_num_mgm_entry_size=-1 >> /etc/modprobe.d/mlnx.conf\n");
		fprintf(stderr," and restart the driver: /etc/init.d/openibd restart \n");
		fclose(fp);
		return 1;
	}

	fclose(fp); 
	return 0;
}


/******************************************************************************
 *
 ******************************************************************************/
static void mac_from_gid(uint8_t   *mac, uint8_t *gid, uint32_t port)
{
    memcpy(mac, gid + 8, 3);
    memcpy(mac + 3, gid + 13, 3);
    if(port==1) {
	mac[0] ^= 2;
    }
}

/******************************************************************************
 *
 ******************************************************************************/
static void mac_from_user(uint8_t   *mac, uint8_t *gid,int size )
{
    memcpy(mac,gid,size);
}

/******************************************************************************
 *
 ******************************************************************************/
static uint16_t ip_checksum	(void * buf,size_t 	  hdr_len)
{
       unsigned long sum = 0;
       const uint16_t *ip1;
        ip1 = buf;
        while (hdr_len > 1)
        {
                 sum += *ip1++;
                if (sum & 0x80000000)
                         sum = (sum & 0xFFFF) + (sum >> 16);
                 hdr_len -= 2;
        }
        while (sum >> 16)
                sum = (sum & 0xFFFF) + (sum >> 16);
        return(~sum);
 }

/******************************************************************************
 *
 ******************************************************************************/
void gen_ip_header(void* ip_header_buffer,uint32_t* saddr ,uint32_t* daddr , uint8_t protocol,int sizePkt, int tos)
{
	struct IP_V4_header ip_header;

	memset(&ip_header,0,sizeof(struct IP_V4_header));

	ip_header.version = 4;
	ip_header.ihl = 5;
	ip_header.tos = (tos == DEF_TOS)? 0 : tos;
	ip_header.tot_len = htons(sizePkt);
	ip_header.id = htons(0);
	ip_header.frag_off = htons(0);
	ip_header.ttl = DEFAULT_TTL;
	ip_header.protocol = protocol;
	ip_header.saddr = *saddr;
	ip_header.daddr = *daddr;
	ip_header.check = ip_checksum((void*)&ip_header,sizeof(struct IP_V4_header));

	memcpy(ip_header_buffer, &ip_header, sizeof(struct IP_V4_header));
}

/******************************************************************************
 *
 ******************************************************************************/
void gen_udp_header(void* UDP_header_buffer,int* sPort ,int* dPort,uint32_t saddr,uint32_t daddr,int sizePkt) {

	struct UDP_header udp_header;

	memset(&udp_header,0,sizeof(struct UDP_header));

	udp_header.uh_sport = htons(*sPort);
	udp_header.uh_dport = htons(*dPort);
	udp_header.uh_ulen = htons(sizePkt - sizeof(struct IP_V4_header));
	udp_header.uh_sum = 0;

	memcpy(UDP_header_buffer, &udp_header, sizeof(struct UDP_header));


}
/******************************************************************************
*
******************************************************************************/
void gen_tcp_header(void* TCP_header_buffer,int* sPort ,int* dPort) {

       struct TCP_header tcp_header;

       memset(&tcp_header,0,sizeof(struct TCP_header));

       tcp_header.th_sport = htons(*sPort);
       tcp_header.th_dport = htons(*dPort);
       tcp_header.th_doff = 5;
       tcp_header.th_window = htons(8192);
       memcpy(TCP_header_buffer, &tcp_header, sizeof(struct TCP_header));
}

/******************************************************************************
*
******************************************************************************/

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

#ifdef HAVE_RAW_ETH_EXP
void print_spec(struct ibv_exp_flow_attr* flow_rules,struct perftest_parameters* user_param)
{
	struct ibv_exp_flow_spec* spec_info = NULL;
#else
void print_spec(struct ibv_flow_attr* flow_rules,struct perftest_parameters* user_param)
{
        struct ibv_flow_spec* spec_info = NULL;
#endif
	void* header_buff = (void*)flow_rules;

	if (user_param->output != FULL_VERBOSITY)
	{
		return;
	}

	if (flow_rules == NULL) {
		printf("error : spec is NULL\n");
		return;
	}

#ifdef HAVE_RAW_ETH_EXP
	header_buff = header_buff + sizeof(struct ibv_exp_flow_attr);
	spec_info = (struct ibv_exp_flow_spec*)header_buff;
#else
        header_buff = header_buff + sizeof(struct ibv_flow_attr);
        spec_info = (struct ibv_flow_spec*)header_buff;
#endif
	printf("MAC attached  : %02X:%02X:%02X:%02X:%02X:%02X\n",
			spec_info->eth.val.dst_mac[0],
			spec_info->eth.val.dst_mac[1],
			spec_info->eth.val.dst_mac[2],
			spec_info->eth.val.dst_mac[3],
			spec_info->eth.val.dst_mac[4],
			spec_info->eth.val.dst_mac[5]);

	if (user_param->is_server_ip && user_param->is_client_ip) {
		char str_ip_s[INET_ADDRSTRLEN] = {0};
		char str_ip_d[INET_ADDRSTRLEN] = {0};
#ifdef HAVE_RAW_ETH_EXP
		header_buff = header_buff + sizeof(struct ibv_exp_flow_spec_eth);
		spec_info = (struct ibv_exp_flow_spec*)header_buff;
#else
                header_buff = header_buff + sizeof(struct ibv_flow_spec_eth);
                spec_info = (struct ibv_flow_spec*)header_buff;
#endif
		uint32_t dst_ip = spec_info->ipv4.val.dst_ip;
		uint32_t src_ip = spec_info->ipv4.val.src_ip;
		inet_ntop(AF_INET, &dst_ip, str_ip_d, INET_ADDRSTRLEN);
		printf("spec_info - dst_ip   : %s\n",str_ip_d);
		inet_ntop(AF_INET, &src_ip, str_ip_s, INET_ADDRSTRLEN);
		printf("spec_info - src_ip   : %s\n",str_ip_s);
	}

	if (user_param->is_server_port && user_param->is_client_port) {
#ifdef HAVE_RAW_ETH_EXP
		header_buff = header_buff + sizeof(struct ibv_exp_flow_spec_ipv4);
		spec_info = (struct ibv_exp_flow_spec*)header_buff;
#else
		header_buff = header_buff + sizeof(struct ibv_flow_spec_ipv4);
                spec_info = (struct ibv_flow_spec*)header_buff;
#endif
		printf("spec_info - dst_port : %d\n",ntohs(spec_info->tcp_udp.val.dst_port));
		printf("spec_info - src_port : %d\n",ntohs(spec_info->tcp_udp.val.src_port));
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
void print_ip_header(struct IP_V4_header* ip_header)
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
		if (ip_header->protocol)
            printf("|protocol  |%-12s|\n",ip_header->protocol == UDP_PROTOCOL ? "UDP" : "TCP");
        else
            printf("|protocol  |%-12s|\n","EMPTY");
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
void print_udp_header(struct UDP_header* udp_header)
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

void print_tcp_header(struct TCP_header* tcp_header)
{
   if(NULL == tcp_header)
   {
		   fprintf(stderr, "tcp_header pointer is Null\n");
		   return;
   }
   printf("**TCP header***********\n");
   printf("|---------------------|\n");
   printf("|Src  Port |%-10d|\n",ntohs(tcp_header->th_sport));
   printf("|Dest Port |%-10d|\n",ntohs(tcp_header->th_dport));
   printf("|offset    |%-10d|\n",tcp_header->th_doff);
   printf("|window    |%-10d|\n",ntohs(tcp_header->th_window));
   printf("|---------------------|\n");
}

/******************************************************************************
 *
 ******************************************************************************/

void print_pkt(void* pkt,struct perftest_parameters *user_param)
{

	if (user_param->output != FULL_VERBOSITY)
	{
		return;
	}

	if(NULL == pkt)
	{
		printf("pkt is null:error happened can't print packet\n");
		return;
	}
	print_ethernet_header((struct ETH_header*)pkt);
	if(user_param->is_client_ip || user_param->is_server_ip)
	{
		pkt = (void*)pkt + sizeof(struct ETH_header);
		print_ip_header((struct IP_V4_header*)pkt);
	}
	if(user_param->is_client_port && user_param->is_server_port)
	{
		pkt = pkt + sizeof(struct IP_V4_header);
		if (user_param->tcp)
			print_tcp_header((struct TCP_header*)pkt);
	    else
			print_udp_header((struct UDP_header*)pkt);
	}
}
/******************************************************************************
 *build single packet on ctx buffer
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
	if(user_param->is_client_ip || user_param->is_server_ip)
	{
		header_buff = (void*)eth_header + sizeof(struct ETH_header);
		gen_ip_header(header_buff,&my_dest_info->ip,&rem_dest_info->ip,ip_next_protocol,sizePkt, user_param->tos);
	}
	if(user_param->is_client_port && user_param->is_server_port)
	{
		header_buff = header_buff + sizeof(struct IP_V4_header);
		if (user_param->tcp)
			gen_tcp_header(header_buff,&my_dest_info->port,&rem_dest_info->port);
	    else
			gen_udp_header(header_buff,&my_dest_info->port,&rem_dest_info->port,my_dest_info->ip,rem_dest_info->ip,sizePkt);

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
	uint16_t ip_next_protocol = 0;
    uint16_t eth_type = (user_param->is_client_ip || user_param->is_server_ip ? IP_ETHER_TYPE : (ctx->size-RAWETH_ADDITION));
    if(user_param->is_client_port && user_param->is_server_port)
		ip_next_protocol = (user_param->tcp ? TCP_PROTOCOL : UDP_PROTOCOL);

    DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

    eth_header = (void*)ctx->buf;

    //build single packet on ctx buffer
	build_pkt_on_buffer(eth_header,my_dest_info,rem_dest_info,user_param,eth_type,ip_next_protocol,PRINT_ON,ctx->size-RAWETH_ADDITION);

	if (user_param->tst == BW) {
		//fill ctx buffer with same packets
		if (ctx->size <= (ctx->cycle_buffer / 2)) {
			while (offset < ctx->cycle_buffer-INC(ctx->size,ctx->cache_line_size)) {
				offset += INC(ctx->size,ctx->cache_line_size);
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
#ifdef HAVE_RAW_ETH_EXP
	int tot_size = sizeof(struct ibv_exp_flow_attr);
	tot_size += sizeof(struct ibv_exp_flow_spec_eth);
	if (is_ip_header)
		tot_size += sizeof(struct ibv_exp_flow_spec_ipv4);
	if (is_udp_header)
		tot_size += sizeof(struct ibv_exp_flow_spec_tcp_udp);
#else
        int tot_size = sizeof(struct ibv_flow_attr);
        tot_size += sizeof(struct ibv_flow_spec_eth);
        if (is_ip_header)
                tot_size += sizeof(struct ibv_flow_spec_ipv4);
        if (is_udp_header)
                tot_size += sizeof(struct ibv_flow_spec_tcp_udp);
#endif
	return tot_size;
}

/******************************************************************************
 *send_set_up_connection - init raw_ethernet_info and ibv_flow_spec to user args
 ******************************************************************************/
 int send_set_up_connection(
#ifdef HAVE_RAW_ETH_EXP
	struct ibv_exp_flow_attr **flow_rules,
#else
	struct ibv_flow_attr **flow_rules,
#endif
	struct pingpong_context *ctx,
	struct perftest_parameters *user_param,
	struct raw_ethernet_info* my_dest_info,
	struct raw_ethernet_info* rem_dest_info)
{

	union ibv_gid temp_gid;

	if (user_param->gid_index != -1) {
		if (ibv_query_gid(ctx->context,user_param->ib_port,user_param->gid_index,&temp_gid)) {
			DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
			return FAILURE;
		}
	}

	if (user_param->machine == SERVER || user_param->duplex) {

		void* header_buff;
#ifdef HAVE_RAW_ETH_EXP
		struct ibv_exp_flow_spec* spec_info;
		struct ibv_exp_flow_attr* attr_info;
#else
                struct ibv_flow_spec* spec_info;
                struct ibv_flow_attr* attr_info;
#endif
		int flow_rules_size;

		int is_ip = user_param->is_server_ip || user_param->is_client_ip;
		int is_port = user_param->is_server_port || user_param->is_client_port;

		flow_rules_size = calc_flow_rules_size(is_ip,is_port);

		ALLOCATE(header_buff,uint8_t,flow_rules_size);

		memset(header_buff, 0,flow_rules_size);
	#ifdef HAVE_RAW_ETH_EXP
		*flow_rules = (struct ibv_exp_flow_attr*)header_buff;
		attr_info = (struct ibv_exp_flow_attr*)header_buff;
	#else
                *flow_rules = (struct ibv_flow_attr*)header_buff;
                attr_info = (struct ibv_flow_attr*)header_buff;
	#endif
		attr_info->size = flow_rules_size;
		attr_info->priority = 0;
		attr_info->num_of_specs = 1 + is_ip + is_port;
		attr_info->port = user_param->ib_port;
		attr_info->flags = 0;
	#ifdef HAVE_RAW_ETH_EXP
		attr_info->type = IBV_EXP_FLOW_ATTR_NORMAL;
		header_buff = header_buff + sizeof(struct ibv_exp_flow_attr);
		spec_info = (struct ibv_exp_flow_spec*)header_buff;
		spec_info->eth.type = IBV_EXP_FLOW_SPEC_ETH;
		spec_info->eth.size = sizeof(struct ibv_exp_flow_spec_eth);
	#else
                attr_info->type = IBV_FLOW_ATTR_NORMAL;
                header_buff = header_buff + sizeof(struct ibv_flow_attr);
                spec_info = (struct ibv_flow_spec*)header_buff;
                spec_info->eth.type = IBV_FLOW_SPEC_ETH;
                spec_info->eth.size = sizeof(struct ibv_flow_spec_eth);
	#endif
		spec_info->eth.val.ether_type = 0;

		if(user_param->is_source_mac) {
			mac_from_user(spec_info->eth.val.dst_mac , &(user_param->source_mac[0]) , sizeof(user_param->source_mac));
		} else {
			mac_from_gid(spec_info->eth.val.dst_mac, temp_gid.raw, user_param->ib_port);
		}

		memset(spec_info->eth.mask.dst_mac, 0xFF,sizeof(spec_info->eth.mask.src_mac));
		if(user_param->is_server_ip || user_param->is_client_ip) {
		#ifdef HAVE_RAW_ETH_EXP
			header_buff = header_buff + sizeof(struct ibv_exp_flow_spec_eth);
			spec_info = (struct ibv_exp_flow_spec*)header_buff;
			spec_info->ipv4.type = IBV_EXP_FLOW_SPEC_IPV4;
			spec_info->ipv4.size = sizeof(struct ibv_exp_flow_spec_ipv4);
		#else
                        header_buff = header_buff + sizeof(struct ibv_flow_spec_eth);
                        spec_info = (struct ibv_flow_spec*)header_buff;
                        spec_info->ipv4.type = IBV_FLOW_SPEC_IPV4;
                        spec_info->ipv4.size = sizeof(struct ibv_flow_spec_ipv4);
		#endif
			if(user_param->machine == SERVER) {

				spec_info->ipv4.val.dst_ip = user_param->server_ip;
				spec_info->ipv4.val.src_ip = user_param->client_ip;

			} else{

				spec_info->ipv4.val.dst_ip = user_param->client_ip;
				spec_info->ipv4.val.src_ip = user_param->server_ip;
			}

			memset((void*)&spec_info->ipv4.mask.dst_ip, 0xFF,sizeof(spec_info->ipv4.mask.dst_ip));
			memset((void*)&spec_info->ipv4.mask.src_ip, 0xFF,sizeof(spec_info->ipv4.mask.src_ip));
		}

		if(user_param->is_server_port && user_param->is_client_port) {
		#ifdef HAVE_RAW_ETH_EXP
			header_buff = header_buff + sizeof(struct ibv_exp_flow_spec_ipv4);
			spec_info = (struct ibv_exp_flow_spec*)header_buff;
			spec_info->tcp_udp.type = (user_param->tcp) ? IBV_EXP_FLOW_SPEC_TCP : IBV_EXP_FLOW_SPEC_UDP;
			spec_info->tcp_udp.size = sizeof(struct ibv_exp_flow_spec_tcp_udp);
		#else
			header_buff = header_buff + sizeof(struct ibv_flow_spec_ipv4);
                        spec_info = (struct ibv_flow_spec*)header_buff;
                        spec_info->tcp_udp.type = (user_param->tcp) ? IBV_FLOW_SPEC_TCP : IBV_FLOW_SPEC_UDP;
                        spec_info->tcp_udp.size = sizeof(struct ibv_flow_spec_tcp_udp);
		#endif
			if(user_param->machine == SERVER) {

				spec_info->tcp_udp.val.dst_port = htons(user_param->server_port);
				spec_info->tcp_udp.val.src_port = htons(user_param->client_port);

			} else{
				spec_info->tcp_udp.val.dst_port = htons(user_param->client_port);
				spec_info->tcp_udp.val.src_port = htons(user_param->server_port);
			}

			memset((void*)&spec_info->tcp_udp.mask.dst_port, 0xFF,sizeof(spec_info->ipv4.mask.dst_ip));
			memset((void*)&spec_info->tcp_udp.mask.src_port, 0xFF,sizeof(spec_info->ipv4.mask.src_ip));
		}
	}

	if (user_param->machine == CLIENT || user_param->duplex) {

		//set source mac
		if(user_param->is_source_mac) {
			mac_from_user(my_dest_info->mac , &(user_param->source_mac[0]) , sizeof(user_param->source_mac) );

		} else {
			mac_from_gid(my_dest_info->mac, temp_gid.raw, user_param->ib_port);
		}

		//set dest mac
		mac_from_user(rem_dest_info->mac , &(user_param->dest_mac[0]) , sizeof(user_param->dest_mac) );

		if(user_param->is_client_ip) {
			if(user_param->machine == CLIENT) {
				my_dest_info->ip = user_param->client_ip;
			} else {
				my_dest_info->ip = user_param->server_ip;
			}
		}

		if(user_param->machine == CLIENT) {
			rem_dest_info->ip = user_param->server_ip;
			my_dest_info->port = user_param->client_port;
			rem_dest_info->port = user_param->server_port;
		}

		if(user_param->machine == SERVER && user_param->duplex) {
			rem_dest_info->ip = user_param->client_ip;
			my_dest_info->port = user_param->server_port;
			rem_dest_info->port = user_param->client_port;
		}
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int run_iter_fw(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	uint64_t		totscnt    = 0;
	uint64_t		totccnt    = 0;
	uint64_t		totrcnt    = 0;
	int			i,index      = 0;
	int			ne = 0;
	int			err = 0;
	uint64_t		*rcnt_for_qp = NULL;
	uint64_t		tot_iters = 0;
	uint64_t		iters = 0;
	struct ibv_wc		*wc = NULL;
	struct ibv_wc		*wc_tx = NULL;
	struct ibv_recv_wr	*bad_wr_recv = NULL;
#if defined(HAVE_VERBS_EXP)
	struct ibv_exp_send_wr	*bad_exp_wr = NULL;
#endif
	struct ibv_send_wr	*bad_wr = NULL;
	int			firstRx = 1;
    	int 			rwqe_sent = user_param->rx_depth;
	int			return_value = 0;

	ALLOCATE(wc,struct ibv_wc,CTX_POLL_BATCH);
	ALLOCATE(wc_tx,struct ibv_wc,CTX_POLL_BATCH);
	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);

	memset(rcnt_for_qp,0,sizeof(uint64_t)*user_param->num_of_qps);

	tot_iters = user_param->iters*user_param->num_of_qps;
	iters=user_param->iters;

	if (user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	if(user_param->test_type == DURATION && user_param->machine == CLIENT && firstRx) {
		firstRx = OFF;
		duration_param=user_param;
		user_param->iters=0;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);
	}

	while ((user_param->test_type == DURATION && user_param->state != END_STATE) || totccnt < tot_iters || totrcnt < tot_iters) {

		for (index=0; index < user_param->num_of_qps; index++) {

			while (((ctx->scnt[index] < iters) || ((firstRx == OFF) && (user_param->test_type == DURATION)))&&
					((ctx->scnt[index] - ctx->ccnt[index]) < user_param->tx_depth) && (rcnt_for_qp[index] - ctx->scnt[index] > 0)) {

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)) {
                                        #ifdef HAVE_VERBS_EXP
                                        if (user_param->use_exp == 1)
                                                ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_SEND_SIGNALED;
                                        else
                                        #endif
                                                ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
                                }

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && duration_param->state == END_STATE)
					break;
				switch_smac_dmac(ctx->wr[index*user_param->post_list].sg_list);

				#if defined(HAVE_VERBS_EXP)
                                if (user_param->use_exp == 1) {
                                        err = (ctx->exp_post_send_func_pointer)(ctx->qp[index],&ctx->exp_wr[index*user_param->post_list],&bad_exp_wr);
                                }
                                else {
                                        err = (ctx->post_send_func_pointer)(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr);
                                }
                                #else
                                        err = ibv_post_send(ctx->qp[index],&ctx->wr[index*user_param->post_list],&bad_wr);
                                #endif
				if(err) {
                                        fprintf(stderr,"Couldn't post send: qp %d scnt=%lu \n",index,ctx->scnt[index]);
                                        return_value = 1;
					goto cleaning;
                                }

				if (user_param->post_list == 1 && user_param->size <= (ctx->cycle_buffer / 2)) {
					#ifdef HAVE_VERBS_EXP
                                        if (user_param->use_exp == 1)
	                                        increase_loc_addr(ctx->exp_wr[index].sg_list,user_param->size,
										ctx->scnt[index],ctx->my_addr[index],0,ctx->cache_line_size,ctx->cycle_buffer);
                                        else
                                        #endif
                                                increase_loc_addr(ctx->wr[index].sg_list,user_param->size,
										ctx->scnt[index],ctx->my_addr[index],0,ctx->cache_line_size,ctx->cycle_buffer);
				}
				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 && (ctx->scnt[index]%user_param->cq_mod == user_param->cq_mod - 1 || (user_param->test_type == ITERATIONS && ctx->scnt[index] == iters-1))){
					#ifdef HAVE_VERBS_EXP
                                        if (user_param->use_exp == 1)
                                                ctx->exp_wr[index].exp_send_flags |= IBV_EXP_SEND_SIGNALED;
                                        else
                                        #endif
                                                ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
				}
			}
		}

		if (user_param->use_event) {

			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr,"Failed to notify events to CQ");
				return_value = 1;
				goto cleaning;
			}
		}

		if ((user_param->test_type == ITERATIONS && (totrcnt < tot_iters)) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
			ne = ibv_poll_cq(ctx->recv_cq,CTX_POLL_BATCH,wc);
			if (ne > 0) {
				if (user_param->machine == SERVER && firstRx && user_param->test_type == DURATION) {
					firstRx = OFF;
					duration_param=user_param;
					user_param->iters=0;
					duration_param->state = START_STATE;
					signal(SIGALRM, catch_alarm);
					alarm(user_param->margin);
				}

				for (i = 0; i < ne; i++) {
					if (wc[i].status != IBV_WC_SUCCESS) {
						NOTIFY_COMP_ERROR_RECV(wc[i],totrcnt);
					}

					rcnt_for_qp[wc[i].wr_id]++;
					totrcnt++;
				}
			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return_value = 1;
				goto cleaning;
			}
		}
		if ((totccnt < tot_iters) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
			ne = ibv_poll_cq(ctx->send_cq,CTX_POLL_BATCH,wc_tx);
			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					if (wc_tx[i].status != IBV_WC_SUCCESS)
						NOTIFY_COMP_ERROR_SEND(wc_tx[i],totscnt,totccnt);

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
				return_value = 1;
				goto cleaning;
			}
            while (rwqe_sent - totccnt < user_param->rx_depth) {    // Post more than buffer_size
    		    if (user_param->test_type==DURATION || rcnt_for_qp[0] + user_param->rx_depth <= user_param->iters) { 
					if (ibv_post_recv(ctx->qp[0],&ctx->rwr[0],&bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%lu\n",0,rcnt_for_qp[0]);
						return_value = 15;
						goto cleaning;
					}
					if (SIZE(user_param->connection_type,user_param->size,!(int)user_param->machine) <= (ctx->cycle_buffer / 2)) {
						increase_loc_addr(ctx->rwr[0].sg_list,
										user_param->size,
										rwqe_sent ,
										ctx->rx_buffer_addr[0],user_param->connection_type,
										ctx->cache_line_size,ctx->cycle_buffer);
					}
				}
                rwqe_sent++;
           }


		}
	}

	if (user_param->noPeak == ON && user_param->test_type == ITERATIONS)
		user_param->tcompleted[0] = get_cycles();

cleaning:
	free(rcnt_for_qp);
	free(wc);
	free(wc_tx);
	return return_value;
}
