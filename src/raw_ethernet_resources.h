#ifndef RAW_ETHERNET_RESOURCES_H
#define RAW_ETHERNET_RESOURCES_H


#if defined(__FreeBSD__)
#include <sys/types.h>
#include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <netinet/ip.h>
#include <poll.h>
#include "perftest_parameters.h"
#include "perftest_resources.h"
#include "multicast_resources.h"
#include "perftest_communication.h"

#undef __LITTLE_ENDIAN
#if defined(__FreeBSD__)
#include <infiniband/byteorder.h>
#else
#include <asm/byteorder.h>
#endif

#define INFO "INFO"
#define TRACE "TRACE"

#ifdef DEBUG
#define DEBUG_LOG(type,fmt, args...) fprintf(stderr,"file:%s: %d ""["type"]"fmt"\n",__FILE__,__LINE__,args)
#else
#define DEBUG_LOG(type,fmt, args...)
#endif

#define PERF_MAC_FMT " %02X:%02X:%02X:%02X:%02X:%02X"

#define IP_ETHER_TYPE (0x800)
#define IP6_ETHER_TYPE (0x86DD)
#define PRINT_ON (1)
#define PRINT_OFF (0)
#define UDP_PROTOCOL (0x11)
#define TCP_PROTOCOL (0x06)
#define IP_HEADER_LEN (20)
#define DEFAULT_IPV6_NEXT_HDR (0x3b)

struct raw_ethernet_info {
	uint8_t mac[6];
	uint32_t ip;
	uint8_t ip6[16];
	int port;
};



/* gen_eth_header .
 * Description :create raw Ethernet header on buffer
 *
 * Parameters :
 *	 	eth_header - Pointer to output
 *	 	src_mac - source MAC address of the packet
 *	 	dst_mac - destination MAC address of the packet
 *	 	eth_type - IP/or size of ptk
 *
 */
struct ETH_header {
	uint8_t dst_mac[6];
	uint8_t src_mac[6];
	uint16_t eth_type;
}__attribute__((packed));

struct ETH_vlan_header {
        uint8_t dst_mac[6];
        uint8_t src_mac[6];
        uint32_t vlan_header;
        uint16_t eth_type;
}__attribute__((packed));

#define VLAN_TPID (0x8100)
#define VLAN_VID (0x001)
#define VLAN_CFI (0)

#if defined(__FreeBSD__)
#if BYTE_ORDER == BIG_ENDIAN
#define __BIG_ENDIAN_BITFIELD
#define htobe32_const(x) (x)
#elif BYTE_ORDER == LITTLE_ENDIAN
#define __LITTLE_ENDIAN_BITFIELD
#define htobe32_const(x) (((x) >> 24) | (((x) >> 8) & 0xff00) | \
    ((((x) & 0xffffff) << 8) & 0xff0000) | ((((x) & 0xff) << 24) & 0xff000000))
#else
#error "Must set BYTE_ORDER"
#endif
#endif

struct IP_V6_header {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u8			priority:4,
				version:4;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u8			version:4,
				priority:4;
#endif
	__u8			flow_lbl[3];

	__be16			payload_len;
	__u8			nexthdr;
	__u8			hop_limit;

	struct	in6_addr	saddr;
	struct	in6_addr	daddr;
}__attribute__((packed));

struct IP_V4_header{
	#if defined(__LITTLE_ENDIAN_BITFIELD)
	uint8_t		ihl:4;
	uint8_t		version:4;
	#elif defined(__BIG_ENDIAN_BITFIELD)
	uint8_t		version:4;
	uint8_t		ihl:4;
	#endif
	uint8_t		tos;
	uint16_t 	tot_len;
	uint16_t	id;
	uint16_t	frag_off;
	uint8_t		ttl;
	uint8_t		protocol;
	uint16_t	check;
	uint32_t	saddr;
	uint32_t	daddr;
}__attribute__((packed));

struct UDP_header {
	u_short	uh_sport;		/* source port */
	u_short	uh_dport;		/* destination port */
	u_short	uh_ulen;		/* udp length */
	u_short	uh_sum;			/* udp checksum */
}__attribute__((packed));

struct TCP_header {
	uint16_t        th_sport;               /* source port */
	uint16_t        th_dport;               /* destination port */
	uint32_t        th_seq;
	uint32_t        th_ack;
	uint8_t         th_rsv:4;
	uint8_t         th_doff:4;
	uint8_t         th_falgs;
	uint16_t        th_window;
	uint16_t        th_check;
	uint16_t        th_urgptr;
}__attribute__((packed));

void gen_eth_header(struct ETH_header* eth_header,uint8_t* src_mac,uint8_t* dst_mac, uint16_t eth_type);
#ifdef HAVE_RAW_ETH_EXP
void print_spec(struct ibv_exp_flow_attr* flow_rules,struct perftest_parameters* user_param);
#else
void print_spec(struct ibv_flow_attr* flow_rules,struct perftest_parameters* user_param);
#endif
//void print_ethernet_header(struct ETH_header* p_ethernet_header);
void print_ethernet_header(void* p_ethernet_header);
//void print_ethernet_vlan_header(struct ETH_vlan_header* p_ethernet_header);
void print_ethernet_vlan_header(void* p_ethernet_header);
void print_ip_header(struct IP_V4_header* ip_header);
void print_udp_header(struct UDP_header* udp_header);
void print_pkt(void* pkt,struct perftest_parameters *user_param);

int check_flow_steering_support(char *dev_name);

/* build_pkt_on_buffer
 * Description: build single Ethernet packet on ctx buffer
 *
 * Parameters:
 *		eth_header - Pointer to output
 *		my_dest_info - ethernet information of me
 *		rem_dest_info - ethernet information of the remote
 *		user_param - user_parameters struct for this test
 *		eth_type -
 *		ip_next_protocol -
 *		print_flag - if print_flag == TRUE : print the packet after it's done
 *		pkt_size - size of the requested packet
 *		flows_offset - current offset from the base flow
 */
void build_pkt_on_buffer(struct ETH_header* eth_header,
		struct raw_ethernet_info *my_dest_info,
		struct raw_ethernet_info *rem_dest_info,
		struct perftest_parameters *user_param,
		uint16_t eth_type,
		uint16_t ip_next_protocol,
		int print_flag,
		int pkt_size,
		int flows_offset);

/*  create_raw_eth_pkt
 * 	Description: build raw Ethernet packet by user arguments
 *				 on bw test, build one packet and duplicate it on the buffer
 *				 on lat test, build only one packet on the buffer (for the ping pong method)
 *
 *	Parameters:
 *				user_param - user_parameters struct for this test
 *				ctx - Test Context.
 *				buf - The QP's packet buffer.
 *				my_dest_info - ethernet information of me
 *				rem_dest_info - ethernet information of the remote
 */
void create_raw_eth_pkt( struct perftest_parameters *user_param,
		struct pingpong_context 	*ctx ,
		void		 		*eth_header,
		struct raw_ethernet_info	*my_dest_info,
		struct raw_ethernet_info	*rem_dest_info);

/*calc_flow_rules_size
 * Description: calculate the size of the flow(size of headers - ib, ethernet and ip/udp if available)
 * Parameters:
 *				is_ip_header - if ip header is exist, count the header's size
 *				is_udp_header - if udp header is exist, count the header's size
 *
 */
int calc_flow_rules_size(struct perftest_parameters *user_param, int is_ip_header,int is_udp_header);

/* send_set_up_connection
 * Description: init raw_ethernet_info and ibv_flow_spec to user args
 *
 *	Parameters:
 *				flow_rules - Pointer to output, is set to header buffer and specification information
 *				ctx - Test Context.
 *				user_param - user_parameters struct for this test
 *				my_dest_info - ethernet information of me
 *				rem_dest_info - ethernet information of the remote
 *
 */

int send_set_up_connection(
		#ifdef HAVE_RAW_ETH_EXP
		struct ibv_exp_flow_attr **flow_rules,
		#else
		struct ibv_flow_attr **flow_rules,
		#endif
		struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct raw_ethernet_info* my_dest_info,
		struct raw_ethernet_info* rem_dest_info);

/* gen_ip_header .

 * Description :create IP header on buffer
 *
 * Parameters :
 * 		ip_header_buff - Pointer to output
 * 		saddr - source IP address of the packet(network order)
 * 		daddr - destination IP address of the packet(network order)
 * 		pkt_size - size of the packet
 *		hop_limit - hop limit (ttl for ipv4)
 *		flows_offset - current offset from the base flow
 */
void gen_ip_header(void* ip_header_buff, uint32_t* saddr, uint32_t* daddr,
		   uint8_t protocol, int pkt_size, int hop_limit, int tos, int flows_offset);

/* gen_udp_header .

 * Description :create UDP header on buffer
 *
 * Parameters :
 * 		UDP_header_buffer - Pointer to output
 *		src_port - source UDP port of the packet
 *		dst_port -destination UDP port of the packet
 *		pkt_size - size of the packet
 */
void gen_udp_header(void* UDP_header_buffer, int src_port, int dst_port, int pkt_size);

/* gen_tcp_header .

 * Description :create TCP header on buffer
 *
 * Parameters :
 * 		TCP_header_buffer - Pointer to output
 *		src_port - source TCP port of the packet
 *		dst_port -destination TCP port of the packet
 */
void gen_tcp_header(void* TCP_header_buffer,int src_port ,int dst_port);

/* run_iter_fw
 *
 * Description :
 *
 *  In this method we receive packets and "turn them around"
 *  this is done by changing the dmac with the smac
 *
 * Parameters :
 *
 *  ctx     - Test Context.
 *  user_param  - user_parameters struct for this test.
 */
int run_iter_fw(struct pingpong_context *ctx,struct perftest_parameters *user_param);

/* switch_smac_dmac
 *
 * Description : In this method we receive buffer and change it's dmac and smac
 *
 * Parameters :
 *
 *  sg     - sg->addr is pointer to the buffer.
 */
static __inline void switch_smac_dmac(struct ibv_sge *sg)
{
	struct ETH_header* eth_header;
	eth_header = (struct ETH_header*)sg->addr;
	uint8_t tmp_mac[6] = {0} ;
	memcpy(tmp_mac , (uint8_t *)eth_header + sizeof(eth_header->src_mac) ,sizeof(eth_header->src_mac));
	memcpy((uint8_t *)eth_header->src_mac , (uint8_t *)eth_header->dst_mac ,sizeof(eth_header->src_mac));
	memcpy((uint8_t *)eth_header->dst_mac  , tmp_mac ,sizeof(tmp_mac));
}

/* set_up_flow_rules
 * Description: set the flow rules objects
 *
 *	Parameters:
 *				flow_rules 	- Pointer to output, is set to header buffer and specification information
 *				ctx 		- Test Context.
 *				user_param 	- user_parameters struct for this test
 *				local_port 	- the local port in the flow rule
 *				remote_port 	- the remote port in the flow rule
 */

int set_up_flow_rules(
		#ifdef HAVE_RAW_ETH_EXP
		struct ibv_exp_flow_attr **flow_rules,
		#else
		struct ibv_flow_attr **flow_rules,
		#endif
		struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		int local_port,
		int remote_port);
/* set_up_fs_rules
 * Description: set the flow rules objects  for FS rate test
 *
 *	Parameters:
 *				flow_rules 	- Pointer to output, is set to header buffer and specification information
 *				ctx 		- Test Context.
 *				user_param 	- user_parameters struct for this test
 *				allocated_flows	- number of flow ruled that are allocated and ready to be set
 *
 */

int set_up_fs_rules(
		#ifdef HAVE_RAW_ETH_EXP
		struct ibv_exp_flow_attr **flow_rules,
		#else
		struct ibv_flow_attr **flow_rules,
		#endif
		struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		uint64_t allocated_flows);

#endif /* RAW_ETHERNET_RESOURCES_H */
