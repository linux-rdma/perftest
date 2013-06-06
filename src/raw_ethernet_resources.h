#ifndef RAW_ETHERNET_RESOURCES_H
#define RAW_ETHERNET_RESOURCES_H


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

 struct raw_ethernet_info {
 	uint8_t mac[6];
 	uint32_t ip;
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
void gen_eth_header(struct ETH_header* eth_header,uint8_t* src_mac,uint8_t* dst_mac, uint16_t eth_type);

void print_spec(struct ibv_flow_attr* flow_rules,struct perftest_parameters* user_parm);

void print_ethernet_header(struct ETH_header* p_ethernet_header);

void print_ip_header(IP_V4_header* ip_header);

void print_udp_header(UDP_header* udp_header);

void print_pkt(void* pkt,struct perftest_parameters *user_param);

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
 *		sizePkt - size of the requested packet
 */
void build_pkt_on_buffer(struct ETH_header* eth_header,
						 struct raw_ethernet_info *my_dest_info,
						 struct raw_ethernet_info *rem_dest_info,
						 struct perftest_parameters *user_param,
						 uint16_t eth_type,
						 uint16_t ip_next_protocol,
						 int print_flag,
						 int sizePkt);
						 
/*  create_raw_eth_pkt
 * 	Description: build raw Ethernet packet by user arguments
 *				 on bw test, build one packet and duplicate it on the buffer
 *				 on lat test, build only one packet on the buffer (for the ping pong method)
 *
 *	Parameters:
 *				user_param - user_parameters struct for this test
 *				ctx - Test Context.
 *				my_dest_info - ethernet information of me
 *				rem_dest_info - ethernet information of the remote
*/
void create_raw_eth_pkt( struct perftest_parameters *user_param,
						 struct pingpong_context 	*ctx ,
						 struct raw_ethernet_info	*my_dest_info,
						 struct raw_ethernet_info	*rem_dest_info);
						 
/*calc_flow_rules_size
* Description: calculate the size of the flow(size of headers - ib, ethernet and ip/udp if available)
* Parameters:
*				is_ip_header - if ip header is exist, count the header's size
*				is_udp_header - if udp header is exist, count the header's size
*
*/
int calc_flow_rules_size(int is_ip_header,int is_udp_header);

/* send_set_up_connection
 * Description: init raw_ethernet_info and ibv_flow_spec to user args
 *
 *	Parameters:
 *				flow_rules - Pointer to output, is set to header buffer and specification information
 *				ctx - Test Context.
 *				user_parm - user_parameters struct for this test
 *				my_dest_info - ethernet information of me
 *				rem_dest_info - ethernet information of the remote
 *
*/

/*static*/ int send_set_up_connection(
	struct ibv_flow_attr **flow_rules,
	struct pingpong_context *ctx,
	struct perftest_parameters *user_parm,
	struct raw_ethernet_info* my_dest_info,
	struct raw_ethernet_info* rem_dest_info);
	

#endif //RAW_ETHERNET_RESOURCES_H