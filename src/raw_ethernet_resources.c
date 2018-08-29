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
#if defined(__FreeBSD__)
#include <sys/types.h>
#include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <netinet/ip.h>
#include <poll.h>
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "multicast_resources.h"
#include "perftest_communication.h"
#include "raw_ethernet_resources.h"
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

struct perftest_parameters* duration_param;

int check_flow_steering_support(char *dev_name)
{
	char* file_name = "/sys/module/mlx4_core/parameters/log_num_mgm_entry_size";
	char* openibd_path = "/etc/init.d/openibd";
	FILE *fp;
	char line[4];
	int is_flow_steering_supported = 0;

	if (strstr(dev_name, "mlx5") != NULL)
		return 0;

	fp = fopen(file_name, "r");
	if (fp == NULL)
		return 0;
	if (fgets(line, 4, fp) == NULL)
		return 0;

	int val = atoi(line);
	if (val >= 0) {
		fprintf(stderr,"flow steering is not supported.\n");
		fprintf(stderr," please run: echo options mlx4_core log_num_mgm_entry_size=-1 >> /etc/modprobe.d/mlnx.conf\n");
		if (access(openibd_path, F_OK) != -1)
			fprintf(stderr," and restart the driver: %s restart \n", openibd_path);
		else
			fprintf(stderr," and restart the driver: modprobe -r mlx4_core; modprobe mlx4_core \n");
		is_flow_steering_supported =  1;
	}

	fclose(fp);
	return is_flow_steering_supported;
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
	while (hdr_len > 1) {
		sum += *ip1++;
		if (sum & 0x80000000)
			sum = (sum & 0xFFFF) + (sum >> 16);
		hdr_len -= 2;
	}
	while (sum >> 16)
		sum = (sum & 0xFFFF) + (sum >> 16);

	return(~sum);
}

void gen_ipv6_header(void* ip_header_buffer, uint8_t* saddr, uint8_t* daddr,
		     uint8_t protocol, int pkt_size, int hop_limit, int tos, int is_l4,
		     int is_tcp)
{
	struct IP_V6_header ip_header;

	memset(&ip_header,0,sizeof(struct IP_V6_header));

	ip_header.version = 6;
	ip_header.nexthdr = protocol ? protocol : DEFAULT_IPV6_NEXT_HDR;
	ip_header.hop_limit = hop_limit;
	ip_header.payload_len = htons(pkt_size - sizeof(struct IP_V6_header));
	if (is_l4) {
		if (is_tcp)
			ip_header.payload_len -= sizeof(struct TCP_header);
		else
			ip_header.payload_len -= sizeof(struct UDP_header);
	}
	memcpy(&ip_header.saddr, saddr, sizeof(ip_header.saddr));
	memcpy(&ip_header.daddr, daddr, sizeof(ip_header.daddr));

	memcpy(ip_header_buffer, &ip_header, sizeof(struct IP_V6_header));
}

/******************************************************************************
 *
 ******************************************************************************/
void gen_ipv4_header(void* ip_header_buffer, uint32_t* saddr, uint32_t* daddr,
		     uint8_t protocol, int pkt_size, int ttl, int tos, int flows_offset)
{
	struct IP_V4_header ip_header;

	memset(&ip_header,0,sizeof(struct IP_V4_header));

	ip_header.version = 4;
	ip_header.ihl = 5;
	ip_header.tos = (tos == DEF_TOS)? 0 : tos;
	ip_header.tot_len = htons(pkt_size);
	ip_header.id = htons(0);
	ip_header.frag_off = htons(0);
	ip_header.ttl = ttl;
	ip_header.protocol = protocol;
	ip_header.saddr = *saddr;
	ip_header.daddr = *daddr;
	ip_header.check = ip_checksum((void*)&ip_header,sizeof(struct IP_V4_header));

	memcpy(ip_header_buffer, &ip_header, sizeof(struct IP_V4_header));
}

/******************************************************************************
 *
 ******************************************************************************/
void gen_udp_header(void* UDP_header_buffer, int src_port, int dst_port, int pkt_size)
{
	struct UDP_header udp_header;

	memset(&udp_header,0,sizeof(struct UDP_header));

	udp_header.uh_sport = htons(src_port);
	udp_header.uh_dport = htons(dst_port);
	udp_header.uh_ulen = htons(pkt_size - sizeof(struct IP_V4_header));
	udp_header.uh_sum = 0;

	memcpy(UDP_header_buffer, &udp_header, sizeof(struct UDP_header));


}
/******************************************************************************
 *
 ******************************************************************************/
void gen_tcp_header(void* TCP_header_buffer,int src_port ,int dst_port)
{
	struct TCP_header tcp_header;

	memset(&tcp_header,0,sizeof(struct TCP_header));

	tcp_header.th_sport = htons(src_port);
	tcp_header.th_dport = htons(dst_port);
	tcp_header.th_doff = 5;
	tcp_header.th_window = htons(8192);
	memcpy(TCP_header_buffer, &tcp_header, sizeof(struct TCP_header));
}

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

static int get_ip_size(int is_ipv6)
{
#ifdef HAVE_IPV6
	if (is_ipv6)
#ifdef HAVE_RAW_ETH_EXP
		return sizeof(struct ibv_exp_flow_spec_ipv6_ext);
#else
		return sizeof(struct ibv_flow_spec_ipv6);
#endif
#endif /* HAVE_IPV6 */

#ifdef HAVE_RAW_ETH_EXP
#ifdef HAVE_IPV4_EXT
		return sizeof(struct ibv_exp_flow_spec_ipv4_ext);
#else
		return sizeof(struct ibv_exp_flow_spec_ipv4);
#endif /* HAVE_IPV4_EXT */
#else
#ifdef HAVE_IPV4_EXT
		return sizeof(struct ibv_flow_spec_ipv4_ext);
#else
		return sizeof(struct ibv_flow_spec_ipv4);
#endif /* HAVE_IPV4_EXT */
#endif
}

/******************************************************************************
 * print test specification
 ******************************************************************************/
#ifdef HAVE_RAW_ETH_EXP
void print_spec(struct ibv_exp_flow_attr* flow_rules,struct perftest_parameters* user_param)
{
	struct ibv_exp_flow_spec* spec_info = NULL;
	#ifdef HAVE_IPV6
	struct ibv_exp_flow_spec_ipv6_ext *ipv6_spec;
	#endif
#else
void print_spec(struct ibv_flow_attr* flow_rules,struct perftest_parameters* user_param)
{
		struct ibv_flow_spec* spec_info = NULL;
		#ifdef HAVE_IPV6
		struct ibv_flow_spec_ipv6 *ipv6_spec;
		#endif
#endif
		void* header_buff = (void*)flow_rules;
		int ip_size = get_ip_size(user_param->raw_ipv6);

		if (user_param->output != FULL_VERBOSITY) {
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
			#ifdef HAVE_IPV6
			char str_ip6_s[INET6_ADDRSTRLEN] = {0};
			char str_ip6_d[INET6_ADDRSTRLEN] = {0};
			#endif
			#ifdef HAVE_RAW_ETH_EXP
			header_buff = header_buff + sizeof(struct ibv_exp_flow_spec_eth);
			spec_info = (struct ibv_exp_flow_spec*)header_buff;
			#ifdef HAVE_IPV6
			ipv6_spec = &spec_info->ipv6_ext;
			#endif
			#else
			header_buff = header_buff + sizeof(struct ibv_flow_spec_eth);
			spec_info = (struct ibv_flow_spec*)header_buff;
			#ifdef HAVE_IPV6
			ipv6_spec = &spec_info->ipv6;
			#endif
			#endif
			if (!user_param->raw_ipv6) {
				uint32_t dst_ip = spec_info->ipv4.val.dst_ip;
				uint32_t src_ip = spec_info->ipv4.val.src_ip;
				inet_ntop(AF_INET, &dst_ip, str_ip_d, INET_ADDRSTRLEN);
				printf("spec_info - dst_ip   : %s\n",str_ip_d);
				inet_ntop(AF_INET, &src_ip, str_ip_s, INET_ADDRSTRLEN);
				printf("spec_info - src_ip   : %s\n",str_ip_s);
			}
			else {
				#ifdef HAVE_IPV6
				void *dst_ip = ipv6_spec->val.dst_ip;
				void *src_ip = ipv6_spec->val.src_ip;
				inet_ntop(AF_INET6, dst_ip, str_ip6_d, INET6_ADDRSTRLEN);
				printf("spec_info - dst_ip   : %s\n",str_ip6_d);
				inet_ntop(AF_INET6, src_ip, str_ip6_s, INET6_ADDRSTRLEN);
				printf("spec_info - src_ip   : %s\n",str_ip6_s);
				#endif
			}
		}

		if (user_param->is_server_port && user_param->is_client_port) {
			header_buff = header_buff + ip_size;
			spec_info = header_buff;
			printf("spec_info - dst_port : %d\n",ntohs(spec_info->tcp_udp.val.dst_port));
			printf("spec_info - src_port : %d\n",ntohs(spec_info->tcp_udp.val.src_port));
		}

	}

static char *etype_str(uint16_t etype)
{
	if (etype == IP_ETHER_TYPE)
		return "IPv4";
	else if (etype == IP6_ETHER_TYPE)
		return "IPv6";
	else return "DEFAULT";
}

/******************************************************************************
 *
 ******************************************************************************/
void print_ethernet_header(void* in_ethernet_header)
{
	struct ETH_header* p_ethernet_header = in_ethernet_header;
	if (NULL == p_ethernet_header) {
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
	char* eth_type = etype_str((ntohs(p_ethernet_header->eth_type)));
	printf("%-22s|\n",eth_type);
	printf("|------------------------------------------------------------|\n\n");

}
/******************************************************************************
*
******************************************************************************/
void print_ethernet_vlan_header(void* in_ethernet_header)
{
	struct ETH_vlan_header* p_ethernet_header = in_ethernet_header;
        if (NULL == p_ethernet_header) {
                fprintf(stderr, "ETH_header pointer is Null\n");
                return;
        }

        printf("**raw ethernet header****************************************\n\n");
        printf("----------------------------------------------------------------------------\n");
        printf("| Dest MAC         | Src MAC          |    vlan tag    |   Packet Type     |\n");
        printf("|--------------------------------------------------------------------------|\n");
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
        printf("   0x%08x   ",ntohl(p_ethernet_header->vlan_header));

        printf("|");
        char* eth_type = (ntohs(p_ethernet_header->eth_type) ==  IP_ETHER_TYPE ? "IP" : "DEFAULT");
        printf("%-19s|\n",eth_type);
        printf("|--------------------------------------------------------------------------|\n\n");

}
/******************************************************************************
 *
 ******************************************************************************/
void print_ip_header(struct IP_V4_header* ip_header)
{
	char str_ip_s[INET_ADDRSTRLEN];
	char str_ip_d[INET_ADDRSTRLEN];

	if (NULL == ip_header) {
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
void print_ip6_header(struct IP_V6_header* ip_header)
{
	char str_ip_s[INET6_ADDRSTRLEN];
	char str_ip_d[INET6_ADDRSTRLEN];

	if (NULL == ip_header) {
		fprintf(stderr, "IP_V6_header pointer is Null\n");
		return;
	}

	printf("***************IPv6 header*************\n");
	printf("|-------------------------------------|\n");
	printf("|Version   |%-26d|\n",ip_header->version);
	printf("|Hop Limit |%-26d|\n",ip_header->hop_limit);

	if (ip_header->nexthdr)
		printf("|protocol  |%-26s|\n",ip_header->nexthdr == UDP_PROTOCOL ? "UDP" : "TCP");
	else
		printf("|protocol  |%-26s|\n","EMPTY");
	inet_ntop(AF_INET6, &ip_header->saddr, str_ip_s, INET6_ADDRSTRLEN);
	printf("|Source IP |%-26s|\n",str_ip_s);
	inet_ntop(AF_INET6, &ip_header->daddr, str_ip_d, INET6_ADDRSTRLEN);
	printf("|Dest IP   |%-26s|\n",str_ip_d);
	printf("|-------------------------------------|\n\n");
}

/******************************************************************************
 *
 ******************************************************************************/
void print_udp_header(struct UDP_header* udp_header)
{
	if(NULL == udp_header) {
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
	if(NULL == tcp_header) {
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
		return;

	if(NULL == pkt) {
		printf("pkt is null:error happened can't print packet\n");
		return;
	}

	user_param->print_eth_func((struct ETH_header*)pkt);
	if(user_param->is_client_ip || user_param->is_server_ip) {
		pkt = (void*)pkt + sizeof(struct ETH_header);
		if (user_param->raw_ipv6)
			print_ip6_header((struct IP_V6_header*)pkt);
		else
			print_ip_header((struct IP_V4_header*)pkt);
	}

	if(user_param->is_client_port && user_param->is_server_port) {
		if (user_param->raw_ipv6)
			pkt = pkt + sizeof(struct IP_V6_header);
		else
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
			 uint16_t eth_type, uint16_t ip_next_protocol,
			 int print_flag, int pkt_size, int flows_offset)
{
	void* header_buff = NULL;
	int have_ip_header = user_param->is_client_ip || user_param->is_server_ip;
	int is_udp_or_tcp = user_param->is_client_port && user_param->is_server_port;
	int eth_header_size = sizeof(struct ETH_header);
	static uint32_t vlan_pcp = 0;

	if(user_param->vlan_pcp==VLAN_PCP_VARIOUS) {
		vlan_pcp++;
	} else {
		vlan_pcp = user_param->vlan_pcp;
	}

	gen_eth_header(eth_header, my_dest_info->mac, rem_dest_info->mac, eth_type);

	if(user_param->vlan_en) {
		struct ETH_vlan_header *p_eth_vlan = (struct ETH_vlan_header *)eth_header;
		p_eth_vlan->eth_type = eth_header->eth_type;
		p_eth_vlan->vlan_header = htonl(VLAN_TPID << 16 |
								((vlan_pcp & 0x7) << 13) | VLAN_VID | VLAN_CFI << 12);
		eth_header_size = sizeof(struct ETH_vlan_header);
		pkt_size -=4;
	}

	if(have_ip_header) {
		int offset = is_udp_or_tcp ? 0 : flows_offset;

		header_buff = (void*)eth_header + eth_header_size;
		if (user_param->raw_ipv6)
			gen_ipv6_header(header_buff, my_dest_info->ip6,
					rem_dest_info->ip6, ip_next_protocol,
					pkt_size, user_param->hop_limit, user_param->tos,
					is_udp_or_tcp, user_param->tcp);
		else
			gen_ipv4_header(header_buff, &my_dest_info->ip, &rem_dest_info->ip,
					ip_next_protocol, pkt_size, user_param->hop_limit, user_param->tos, offset);
	}

	if(is_udp_or_tcp) {
		if (user_param->raw_ipv6)
			header_buff = header_buff + sizeof(struct IP_V6_header);
		else
			header_buff = header_buff + sizeof(struct IP_V4_header);
		if (user_param->tcp)
			gen_tcp_header(header_buff, my_dest_info->port + flows_offset,
				       rem_dest_info->port + flows_offset);
		else
			gen_udp_header(header_buff, my_dest_info->port + flows_offset,
				       rem_dest_info->port+ flows_offset, pkt_size);

	}

	if(print_flag == PRINT_ON) {
		print_pkt((void*)eth_header, user_param);
	}
}

/******************************************************************************
 *Create_raw_eth_pkt - build raw Ethernet packet by user arguments.
 *On bw test, build one packet and duplicate it on the buffer per QP.
 *Alternatively, build multiple packets according to number of flows,
 * again per QP
 *On lat test, build only one packet on the buffer (for the ping pong method)
 ******************************************************************************/
void create_raw_eth_pkt( struct perftest_parameters *user_param,
		struct pingpong_context 	*ctx,
		void		 		*buf,
		struct raw_ethernet_info	*my_dest_info,
		struct raw_ethernet_info	*rem_dest_info)
{
	int pkt_offset = 0;
	int flow_limit = 0;
	int i = 0;
	int print_flag = (user_param->vlan_pcp==VLAN_PCP_VARIOUS) ? PRINT_ON:PRINT_OFF;
	struct ETH_header* eth_header;
	uint16_t vlan_tag_size = user_param->vlan_en ? 4 : 0;
	uint16_t ip_next_protocol = 0;
	uint16_t eth_type = user_param->is_ethertype ? user_param->ethertype :
		(user_param->is_client_ip || user_param->is_server_ip ?
		 (user_param->raw_ipv6) ? IP6_ETHER_TYPE :
		 IP_ETHER_TYPE : (ctx->size-RAWETH_ADDITION-vlan_tag_size));
	if(user_param->is_client_port && user_param->is_server_port)
		ip_next_protocol = (user_param->tcp ? TCP_PROTOCOL : UDP_PROTOCOL);

	DEBUG_LOG(TRACE,">>>>>>%s",__FUNCTION__);

	eth_header = buf;

	if (user_param->tst == BW || user_param->tst == LAT_BY_BW) {
		/* fill ctx buffer with different packets according to flows_offset */
		for (i = 0; i < user_param->flows; i++) {
			print_flag = PRINT_ON;
			pkt_offset = ctx->flow_buff_size * i; /* update the offset to next flow */
			flow_limit = ctx->flow_buff_size * (i + 1);
			eth_header = (void*)buf + pkt_offset;/* update the eth_header to next flow */
			/* fill ctx buffer with same packets */
			while ((flow_limit - INC(ctx->size, ctx->cache_line_size)) >= pkt_offset) {
				build_pkt_on_buffer(eth_header, my_dest_info, rem_dest_info,
						    user_param, eth_type, ip_next_protocol,
						    print_flag, ctx->size - RAWETH_ADDITION, i);
				print_flag = PRINT_OFF;
				pkt_offset += INC(ctx->size, ctx->cache_line_size);/* update the offset to next packet in same flow */
				eth_header = (void*)buf + pkt_offset;/* update the eth_header to next packet in same flow */
			}
		}
	} else if (user_param->tst == LAT) {
		/* fill ctx buffer with different packets according to flows_offset */
		for (i = 0; i < user_param->flows; i++) {
			pkt_offset = ctx->flow_buff_size * i;
			eth_header = (void*)buf + pkt_offset;
			build_pkt_on_buffer(eth_header, my_dest_info, rem_dest_info,
					    user_param, eth_type, ip_next_protocol,
					    PRINT_ON, ctx->size - RAWETH_ADDITION, i);

		}
	}

	DEBUG_LOG(TRACE,"<<<<<<%s",__FUNCTION__);
}

/******************************************************************************
  calc_flow_rules_size
 ******************************************************************************/
int calc_flow_rules_size(struct perftest_parameters *user_param, int is_ip_header,int is_udp_header)
{
	#ifdef HAVE_RAW_ETH_EXP
	int tot_size = sizeof(struct ibv_exp_flow_attr);
	tot_size += sizeof(struct ibv_exp_flow_spec_eth);
	if (is_ip_header)
		tot_size += get_ip_size(user_param->raw_ipv6);
	if (is_udp_header)
		tot_size += sizeof(struct ibv_exp_flow_spec_tcp_udp);
	#else
	int tot_size = sizeof(struct ibv_flow_attr);
	tot_size += sizeof(struct ibv_flow_spec_eth);
	if (is_ip_header)
		tot_size += get_ip_size(user_param->raw_ipv6);
	if (is_udp_header)
		tot_size += sizeof(struct ibv_flow_spec_tcp_udp);
	#endif

	return tot_size;
}

#ifdef HAVE_RAW_ETH_EXP
static void fill_ip_common(struct ibv_exp_flow_spec* spec_info,
			   struct perftest_parameters *user_param)
{
	#ifdef HAVE_IPV6
	struct ibv_exp_flow_spec_ipv6_ext *ipv6_spec = &spec_info->ipv6_ext;
	static const char ipv6_mask[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
					 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	#endif
	#ifdef HAVE_IPV4_EXT
	struct ibv_exp_flow_spec_ipv4_ext *ipv4_spec = &spec_info->ipv4_ext;
	#else
	struct ibv_exp_flow_spec_ipv4 *ipv4_spec = &spec_info->ipv4;
	#endif /* HAVE_IPV4_EXT */
#else
static void fill_ip_common(struct ibv_flow_spec* spec_info,
			   struct perftest_parameters *user_param)
{
	#ifdef HAVE_IPV6
	struct ibv_flow_spec_ipv6 *ipv6_spec = &spec_info->ipv6;
	static const char ipv6_mask[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
					 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
	#endif
	#ifdef HAVE_IPV4_EXT
	struct ibv_flow_spec_ipv4_ext *ipv4_spec = &spec_info->ipv4_ext;
	#else
	struct ibv_flow_spec_ipv4 *ipv4_spec = &spec_info->ipv4;
	#endif
#endif

	if(user_param->machine == SERVER) {
		if (user_param->raw_ipv6) {
			#ifdef HAVE_IPV6
			memcpy(ipv6_spec->val.dst_ip,
			       user_param->server_ip6, 16);
			memcpy(ipv6_spec->val.src_ip,
			       user_param->client_ip6, 16);
			if (user_param->tos != DEF_TOS) {
				ipv6_spec->val.traffic_class =
					user_param->tos;
				ipv6_spec->mask.traffic_class =
					0xff;
			}
			if (user_param->flow_label) {
				ipv6_spec->val.flow_label =
					htonl(user_param->flow_label);
				ipv6_spec->mask.flow_label =
					htonl(0xfffff);
			}
			memcpy((void*)&ipv6_spec->mask.dst_ip, ipv6_mask, 16);
			memcpy((void*)&ipv6_spec->mask.src_ip, ipv6_mask, 16);
			#endif
		} else {
			ipv4_spec->val.dst_ip = user_param->server_ip;
			ipv4_spec->val.src_ip = user_param->client_ip;
			memset((void*)&ipv4_spec->mask.dst_ip, 0xFF,sizeof(ipv4_spec->mask.dst_ip));
			memset((void*)&ipv4_spec->mask.src_ip, 0xFF,sizeof(ipv4_spec->mask.src_ip));
			#ifdef HAVE_IPV4_EXT
			if (user_param->tos != DEF_TOS) {
				ipv4_spec->val.tos = user_param->tos;
				ipv4_spec->mask.tos = 0xff;
			}
			#endif
		}
	}
}

#ifdef HAVE_RAW_ETH_EXP
static void fill_exp_ip_spec(struct ibv_exp_flow_spec* spec_info,
			     struct perftest_parameters *user_param)
{
	if (user_param->raw_ipv6) {
		#ifdef HAVE_IPV6
		spec_info->ipv6.type = IBV_EXP_FLOW_SPEC_IPV6_EXT;
		spec_info->ipv6.size = sizeof(struct ibv_exp_flow_spec_ipv6_ext);
		#endif
	} else {
		#ifdef HAVE_IPV4_EXT
		spec_info->ipv4.type = IBV_EXP_FLOW_SPEC_IPV4_EXT;
		spec_info->ipv4.size = sizeof(struct ibv_exp_flow_spec_ipv4_ext);
		#else
		spec_info->ipv4.type = IBV_EXP_FLOW_SPEC_IPV4;
		spec_info->ipv4.size = sizeof(struct ibv_exp_flow_spec_ipv4);
		#endif
	}
	fill_ip_common(spec_info, user_param);
}
#else
static void fill_ip_spec(struct ibv_flow_spec* spec_info,
			 struct perftest_parameters *user_param)
{
	if (user_param->raw_ipv6) {
		#ifdef HAVE_IPV6
		spec_info->ipv6.type = IBV_FLOW_SPEC_IPV6;
		spec_info->ipv6.size = sizeof(struct ibv_flow_spec_ipv6);
		#endif
	} else {
		#ifdef HAVE_IPV4_EXT
		spec_info->ipv4.type = IBV_FLOW_SPEC_IPV4_EXT;
		spec_info->ipv4.size = sizeof(struct ibv_flow_spec_ipv4_ext);
		#else
		spec_info->ipv4.type = IBV_FLOW_SPEC_IPV4;
		spec_info->ipv4.size = sizeof(struct ibv_flow_spec_ipv4);
		#endif
	}
	fill_ip_common(spec_info, user_param);
}
#endif

int set_up_flow_rules(
		#ifdef HAVE_RAW_ETH_EXP
		struct ibv_exp_flow_attr **flow_rules,
		#else
		struct ibv_flow_attr **flow_rules,
		#endif
		struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		int local_port,
		int remote_port)
{
	#ifdef HAVE_RAW_ETH_EXP
	struct ibv_exp_flow_spec* spec_info;
	struct ibv_exp_flow_attr* attr_info;
	#else
	struct ibv_flow_spec* spec_info;
	struct ibv_flow_attr* attr_info;
	#endif

	void* header_buff;
	int flow_rules_size;
	int is_ip = user_param->is_server_ip || user_param->is_client_ip;
	int is_port = user_param->is_server_port || user_param->is_client_port;

	flow_rules_size = calc_flow_rules_size(user_param, is_ip,is_port);

	ALLOCATE(header_buff, uint8_t, flow_rules_size);

	memset(header_buff, 0, flow_rules_size);

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

	mac_from_user(spec_info->eth.val.dst_mac, &(user_param->source_mac[0]), sizeof(user_param->source_mac));

	memset(spec_info->eth.mask.dst_mac, 0xFF,sizeof(spec_info->eth.mask.src_mac));
	if(user_param->is_server_ip || user_param->is_client_ip) {
		#ifdef HAVE_RAW_ETH_EXP
		header_buff = header_buff + sizeof(struct ibv_exp_flow_spec_eth);
		spec_info = (struct ibv_exp_flow_spec*)header_buff;
		fill_exp_ip_spec(spec_info, user_param);
		#else
		header_buff = header_buff + sizeof(struct ibv_flow_spec_eth);
		spec_info = (struct ibv_flow_spec*)header_buff;
		fill_ip_spec(spec_info, user_param);
		#endif
	}

	if(user_param->is_server_port && user_param->is_client_port) {
		header_buff = header_buff + get_ip_size(user_param->raw_ipv6);
		#ifdef HAVE_RAW_ETH_EXP
		spec_info = (struct ibv_exp_flow_spec*)header_buff;
		spec_info->tcp_udp.type = (user_param->tcp) ? IBV_EXP_FLOW_SPEC_TCP : IBV_EXP_FLOW_SPEC_UDP;
		spec_info->tcp_udp.size = sizeof(struct ibv_exp_flow_spec_tcp_udp);
		#else
		spec_info = (struct ibv_flow_spec*)header_buff;
		spec_info->tcp_udp.type = (user_param->tcp) ? IBV_FLOW_SPEC_TCP : IBV_FLOW_SPEC_UDP;
		spec_info->tcp_udp.size = sizeof(struct ibv_flow_spec_tcp_udp);
		#endif

		if(user_param->machine == SERVER) {
			spec_info->tcp_udp.val.dst_port = htons(local_port);
			spec_info->tcp_udp.val.src_port = htons(remote_port);
		} else {
			spec_info->tcp_udp.val.src_port = htons(local_port);
			spec_info->tcp_udp.val.dst_port = htons(remote_port);
		}

		memset((void*)&spec_info->tcp_udp.mask.dst_port, 0xFF,sizeof(spec_info->ipv4.mask.dst_ip));
		memset((void*)&spec_info->tcp_udp.mask.src_port, 0xFF,sizeof(spec_info->ipv4.mask.src_ip));
	}

	if (user_param->is_ethertype) {
		spec_info->eth.val.ether_type = htons(user_param->ethertype);
		spec_info->eth.mask.ether_type = 0xffff;
	}
	return 0;
}

/******************************************************************************
 *set_fs_rate_rules - init flow struct for FS rate test
 ******************************************************************************/
int set_up_fs_rules(
		#ifdef HAVE_RAW_ETH_EXP
		struct ibv_exp_flow_attr **flow_rules,
		#else
		struct ibv_flow_attr **flow_rules,
		#endif
		struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		uint64_t allocated_flows) {


	int				local_port = 0;
	int				remote_port = 0;
	int				last_local_index = 0;
	int 				flow_index = 0;
	int				qp_index = 0;
	int				allowed_server_ports = MAX_FS_PORT - user_param->server_port;

	for (qp_index = 0; qp_index < user_param->num_of_qps; qp_index++) {
		for (flow_index = 0; flow_index < allocated_flows; flow_index++) {
			if (set_up_flow_rules(&flow_rules[(qp_index * allocated_flows) + flow_index],
					      ctx, user_param, local_port, remote_port)) {
				fprintf(stderr, "Unable to set up flow rules\n");
	                        return FAILURE;
			}
			if (flow_index <= allowed_server_ports) {
				local_port = user_param->local_port + flow_index;
				remote_port = user_param->remote_port;
				last_local_index = flow_index;
			} else {
				local_port = user_param->local_port;
				remote_port = user_param->remote_port + flow_index - last_local_index;
			}
		}
	}
	return SUCCESS;
}

/******************************************************************************2
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
		struct raw_ethernet_info *my_dest_info,
		struct raw_ethernet_info *rem_dest_info)
{

	int flow_index;

	if (user_param->machine == SERVER || user_param->duplex) {
		for (flow_index = 0; flow_index < user_param->flows; flow_index++)
			set_up_flow_rules(&flow_rules[flow_index], ctx,
					  user_param, user_param->server_port + flow_index, user_param->client_port + flow_index);
	}

	if (user_param->machine == CLIENT || user_param->duplex) {

		/* set source mac */
		mac_from_user(my_dest_info->mac , &(user_param->source_mac[0]) , sizeof(user_param->source_mac) );

		/* set dest mac */
		mac_from_user(rem_dest_info->mac , &(user_param->dest_mac[0]) , sizeof(user_param->dest_mac) );

		if(user_param->is_client_ip) {
			if(user_param->machine == CLIENT) {
				if (!user_param->raw_ipv6)
					my_dest_info->ip = user_param->client_ip;
				else
					memcpy(my_dest_info->ip6,
					       &(user_param->client_ip6[0]),
					       sizeof(user_param->client_ip6));
			} else {
				if (!user_param->raw_ipv6)
					my_dest_info->ip = user_param->server_ip;
				else
					memcpy(my_dest_info->ip6,
					       &(user_param->server_ip6[0]),
					       sizeof(user_param->server_ip6));
			}
		}

		if(user_param->machine == CLIENT) {
			if (!user_param->raw_ipv6)
				rem_dest_info->ip = user_param->server_ip;
			else {
				memcpy(rem_dest_info->ip6,
				       &(user_param->server_ip6[0]),
				       sizeof(user_param->server_ip6));
			}
			my_dest_info->port = user_param->client_port;
			rem_dest_info->port = user_param->server_port;
		}

		if(user_param->machine == SERVER && user_param->duplex) {
			if (!user_param->raw_ipv6)
				rem_dest_info->ip = user_param->client_ip;
			else
				memcpy(rem_dest_info->ip6,
				       &(user_param->client_ip6[0]),
				       sizeof(user_param->client_ip6));
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
	int			i;
	int			index      = 0;
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
	int			wc_id;

	ALLOCATE(wc, struct ibv_wc, CTX_POLL_BATCH);
	ALLOCATE(wc_tx, struct ibv_wc, CTX_POLL_BATCH);
	ALLOCATE(rcnt_for_qp,uint64_t,user_param->num_of_qps);

	memset(wc, 0, sizeof(struct ibv_wc));
	memset(wc_tx, 0, sizeof(struct ibv_wc));
	memset(rcnt_for_qp, 0, sizeof(uint64_t) * user_param->num_of_qps);

	tot_iters = (uint64_t)user_param->iters * user_param->num_of_qps;
	iters = user_param->iters;

	if (user_param->noPeak == ON)
		user_param->tposted[0] = get_cycles();

	if(user_param->test_type == DURATION && user_param->machine == CLIENT && firstRx) {
		firstRx = OFF;
		duration_param = user_param;
		user_param->iters = 0;
		duration_param->state = START_STATE;
		signal(SIGALRM, catch_alarm);
		alarm(user_param->margin);
	}

	while ((user_param->test_type == DURATION && user_param->state != END_STATE) || totccnt < tot_iters || totrcnt < tot_iters) {

		for (index = 0; index < user_param->num_of_qps; index++) {

			while (((ctx->scnt[index] < iters) || ((firstRx == OFF) && (user_param->test_type == DURATION))) &&
					((ctx->scnt[index] - ctx->ccnt[index]) < user_param->tx_depth) && (rcnt_for_qp[index] - ctx->scnt[index] > 0)) {

				if (user_param->post_list == 1 && (ctx->scnt[index] % user_param->cq_mod == 0 && user_param->cq_mod > 1)) {
					#ifdef HAVE_VERBS_EXP
					#ifdef HAVE_ACCL_VERBS
					if (user_param->verb_type == ACCL_INTF)
						ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_QP_BURST_SIGNALED;
					else {
					#endif
						if (user_param->use_exp == 1)
							ctx->exp_wr[index].exp_send_flags &= ~IBV_EXP_SEND_SIGNALED;
						else
					#endif
							ctx->wr[index].send_flags &= ~IBV_SEND_SIGNALED;
					#ifdef HAVE_ACCL_VERBS
					}
					#endif
				}

				if (user_param->noPeak == OFF)
					user_param->tposted[totscnt] = get_cycles();

				if (user_param->test_type == DURATION && duration_param->state == END_STATE)
					break;
				switch_smac_dmac(ctx->wr[index*user_param->post_list].sg_list);

				#ifdef HAVE_VERBS_EXP
				#ifdef HAVE_ACCL_VERBS
				if (user_param->verb_type == ACCL_INTF) {
					struct ibv_sge *sg_l = ctx->exp_wr[index*user_param->post_list].sg_list;
					err = ctx->qp_burst_family[index]->send_burst(ctx->qp[index], sg_l, 1, ctx->exp_wr[index].exp_send_flags);
				} else {
				#endif
					if (user_param->use_exp == 1) {
						err = (ctx->exp_post_send_func_pointer)(ctx->qp[index],
							&ctx->exp_wr[index*user_param->post_list], &bad_exp_wr);
					}
					else {
						err = (ctx->post_send_func_pointer)(ctx->qp[index],
							&ctx->wr[index*user_param->post_list],&bad_wr);
					}
				#ifdef HAVE_ACCL_VERBS
				}
				#endif
				#else
				err = ibv_post_send(ctx->qp[index], &ctx->wr[index*user_param->post_list], &bad_wr);
				#endif
				if(err) {
					fprintf(stderr, "Couldn't post send: qp %d scnt=%lu \n", index, ctx->scnt[index]);
					return_value = FAILURE;
					goto cleaning;
				}

				if (user_param->post_list == 1 && user_param->size <= (ctx->cycle_buffer / 2)) {
					#ifdef HAVE_VERBS_EXP
					if (user_param->use_exp == 1)
						increase_loc_addr(ctx->exp_wr[index].sg_list, user_param->size,
								  ctx->scnt[index], ctx->my_addr[index], 0,
								  ctx->cache_line_size, ctx->cycle_buffer);
					else
					#endif
						increase_loc_addr(ctx->wr[index].sg_list, user_param->size,
								  ctx->scnt[index], ctx->my_addr[index], 0,
								  ctx->cache_line_size, ctx->cycle_buffer);
				}
				ctx->scnt[index] += user_param->post_list;
				totscnt += user_param->post_list;

				if (user_param->post_list == 1 &&
					(ctx->scnt[index]%user_param->cq_mod == (user_param->cq_mod - 1) ||
						(user_param->test_type == ITERATIONS && ctx->scnt[index] == (iters - 1)))) {
					#ifdef HAVE_VERBS_EXP
					#ifdef HAVE_ACCL_VERBS
					if (user_param->verb_type == ACCL_INTF)
						ctx->exp_wr[index].exp_send_flags |= IBV_EXP_QP_BURST_SIGNALED;
					else {
					#endif
						if (user_param->use_exp == 1)
							ctx->exp_wr[index].exp_send_flags |= IBV_EXP_SEND_SIGNALED;
						else
					#endif
							ctx->wr[index].send_flags |= IBV_SEND_SIGNALED;
					#ifdef HAVE_ACCL_VERBS
					}
					#endif
				}
			}
		}

		if (user_param->use_event) {

			if (ctx_notify_events(ctx->channel)) {
				fprintf(stderr, "Failed to notify events to CQ");
				return_value = FAILURE;
				goto cleaning;
			}
		}

		if ((user_param->test_type == ITERATIONS && (totrcnt < tot_iters)) ||
			(user_param->test_type == DURATION && user_param->state != END_STATE)) {
			#ifdef HAVE_ACCL_VERBS
			if (user_param->verb_type == ACCL_INTF)
				ne = ctx->recv_cq_family->poll_cnt(ctx->recv_cq, CTX_POLL_BATCH);
			else
			#endif
				ne = ibv_poll_cq(ctx->recv_cq, CTX_POLL_BATCH, wc);

			if (ne > 0) {
				if (user_param->machine == SERVER && firstRx && user_param->test_type == DURATION) {
					firstRx = OFF;
					duration_param = user_param;
					user_param->iters = 0;
					duration_param->state = START_STATE;
					signal(SIGALRM, catch_alarm);
					alarm(user_param->margin);
				}

				for (i = 0; i < ne; i++) {
					wc_id = (user_param->verb_type == ACCL_INTF) ?
						0 : (int)wc[i].wr_id;

					if (user_param->verb_type != ACCL_INTF) {
						if (wc[i].status != IBV_WC_SUCCESS) {
							NOTIFY_COMP_ERROR_RECV(wc[i], totrcnt);
						}
					}

					rcnt_for_qp[wc_id]++;
					totrcnt++;
				}
			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return_value = FAILURE;
				goto cleaning;
			}
		}
		if ((totccnt < tot_iters) || (user_param->test_type == DURATION && user_param->state != END_STATE)) {
			#ifdef HAVE_ACCL_VERBS
			if (user_param->verb_type == ACCL_INTF)
				ne = ctx->send_cq_family->poll_cnt(ctx->send_cq, CTX_POLL_BATCH);
			else
			#endif
				ne = ibv_poll_cq(ctx->send_cq, CTX_POLL_BATCH, wc_tx);

			if (ne > 0) {
				for (i = 0; i < ne; i++) {
					wc_id = (user_param->verb_type == ACCL_INTF) ?
						0 : (int)wc[i].wr_id;

					if (user_param->verb_type != ACCL_INTF) {
						if (wc_tx[i].status != IBV_WC_SUCCESS)
							NOTIFY_COMP_ERROR_SEND(wc_tx[i], totscnt, totccnt);
					}

					totccnt += user_param->cq_mod;
					ctx->ccnt[wc_id] += user_param->cq_mod;

					if (user_param->noPeak == OFF) {

						if ((user_param->test_type == ITERATIONS && (totccnt >= tot_iters - 1)))
							user_param->tcompleted[tot_iters - 1] = get_cycles();
						else
							user_param->tcompleted[totccnt - 1] = get_cycles();
					}

					if (user_param->test_type == DURATION && user_param->state == SAMPLE_STATE)
						user_param->iters += user_param->cq_mod;
				}
			} else if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return_value = FAILURE;
				goto cleaning;
			}
			while (rwqe_sent - totccnt < user_param->rx_depth) {    /* Post more than buffer_size */
				if (user_param->test_type==DURATION ||
					rcnt_for_qp[0] + user_param->rx_depth <= user_param->iters) {
					#ifdef HAVE_ACCL_VERBS
					if (user_param->verb_type == ACCL_INTF) {
						if (ctx->qp_burst_family[0]->recv_burst(ctx->qp[0], ctx->rwr[0].sg_list, 1)) {
							fprintf(stderr, "Couldn't post recv burst (accelerated verbs).\n");
							return_value = FAILURE;
							goto cleaning;
						}
					} else {
					#endif
						if (ibv_post_recv(ctx->qp[0], &ctx->rwr[0], &bad_wr_recv)) {
							fprintf(stderr, "Couldn't post recv Qp=%d rcnt=%lu\n", 0, rcnt_for_qp[0]);
							return_value = 15;
							goto cleaning;
						}
					#ifdef HAVE_ACCL_VERBS
					}
					#endif

					if (SIZE(user_param->connection_type, user_param->size, !(int)user_param->machine) <= (ctx->cycle_buffer / 2)) {
						increase_loc_addr(ctx->rwr[0].sg_list,
								user_param->size,
								rwqe_sent,
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
