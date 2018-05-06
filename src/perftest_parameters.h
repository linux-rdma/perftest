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
 *  This API defines structs, formats and enums for all perftest benchmarks.
 *  It includes parameters parser, and a report generator.
 *
 * Methods :
 *
 *  link_layer_str  - Return a String representation of the link type.
 *  parser - Setting default test parameters and parsing the user choices.
 *  check_link - Configures test MTU,inline and link layer of the test.
 *  check_link_and_mtu     - Configures test MTU,inline and link layer of the test.
 *  print_report_bw - Calculate the peak and average throughput of the BW test.
 *  print_full_bw_report    - Print the peak and average throughput of the BW test.
 *  print_report_lat - Print the min/max/median latency samples taken from a latency test.
 *  print_report_lat_duration     - Prints only the avergae latency for samples taken from
 *									a latency test with Duration..
 *  set_mtu - set MTU from the port or user.
 *  set_eth_mtu    - set MTU for Raw Ethernet tests.
 */
#ifndef PERFTEST_PARAMETERS_H
#define PERFTEST_PARAMETERS_H

#include <infiniband/verbs.h>
#include <unistd.h>
#if !defined(__FreeBSD__)
#include <malloc.h>
#endif
#include "get_clock.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_VERBS_EXP
#include <infiniband/verbs_exp.h>
#endif

#ifdef HAVE_CUDA
#include CUDA_PATH
#endif

/* Connection types available. */
#define RC  (0)
#define UC  (1)
#define UD  (2)
#define RawEth  (3)
#define XRC (4)
#define DC  (5)

/* Genral control definitions */
#define OFF	     (0)
#define ON 	     (1)
#define SUCCESS	     (0)
#define FAILURE	     (1)
#define VERSION_EXIT (10)
#define HELP_EXIT	 (11)
#define MTU_FIX	     (7)
#define MAX_SIZE     (8388608)
#define LINK_FAILURE (-1)
#define LINK_UNSPEC (-2)
#define MAX_OUT_READ_HERMON (16)
#define MAX_OUT_READ        (4)
#define UD_ADDITION         (40)
#define RAWETH_ADDITION    (18)
#define HW_CRC_ADDITION    (4)

/* Default Values of perftest parameters */
#define DEF_PORT      (18515)
#define DEF_IB_PORT   (1)
#define DEF_IB_PORT2  (2)
#define DEF_SIZE_BW   (65536)
#define DEF_SIZE_LAT  (2)
#define DEF_ITERS     (1000)
#define DEF_ITERS_WB  (5000)
#define DEF_TX_BW     (128)
#define DEF_TX_LAT    (1)
#define DEF_QP_TIME   (14)
#define DEF_SL	      (0)
#define DEF_GID_INDEX (-1)
#define DEF_NUM_QPS   (1)
#define DEF_RX_RDMA   (1)
#define DEF_RX_SEND   (512)
#define DEF_CQ_MOD    (100)
#define DEF_SIZE_ATOMIC (8)
#define DEF_QKEY      0x11111111
#define DEF_DURATION  (5)
#define	DEF_MARGIN    (2)
#define DEF_INIT_MARGIN (-1)
#define DEF_INLINE    (-1)
#define DEF_TOS       (-1)
#define DEF_RETRY_COUNT (7)
#define DEF_CACHE_LINE_SIZE (64)
#define DEF_PAGE_SIZE (4096)
#define DEF_FLOWS (1)
#define RATE_VALUES_COUNT (18)

/* Optimal Values for Inline */
#define DEF_INLINE_WRITE (220)
#define DEF_INLINE_SEND_RC_UC (236)
#define DEF_INLINE_SEND_XRC (236)
#define DEF_INLINE_SEND_UD (188)
#define DEF_INLINE_DC (150)

/* Max and Min allowed values for perftest parameters. */
#define MIN_TOS		(0)
#define MAX_TOS		(255)
#define MIN_IB_PORT   (1)
#define MAX_IB_PORT   (3)
#define MIN_ITER      (5)
#define MAX_ITER      (100000000)
#define MIN_TX 	      (1)
#define MAX_TX	      (15000)
#define MIN_SL	      (0)
#define MAX_SL	      (15)
#define MIN_GID_IX    (0)
#define MAX_GID_IX    (64)
#define MIN_QP_NUM    (1)
#define MAX_QP_NUM    (16384)
#define MIN_QP_MCAST  (1)
#define MAX_QP_MCAST  (56)
#define MIN_RX	      (1)
#define MAX_RX	      (16384)
#define UC_MAX_RX     (16000)
#define MIN_CQ_MOD    (1)
#define MAX_CQ_MOD    (1024)
#define MAX_INLINE    (912)
#define MAX_INLINE_UD (884)
#define MIN_EQ_NUM    (0)
#define MAX_EQ_NUM    (2048)

/* Raw etherent defines */
#define RAWETH_MIN_MSG_SIZE	(64)
#define MIN_MTU_RAW_ETERNET	(64)
#define MAX_MTU_RAW_ETERNET	(9600)
#define MIN_FS_PORT		(5000)
#define MAX_FS_PORT		(65536)
#define VLAN_PCP_VARIOUS        (8)

#define RESULT_LINE "---------------------------------------------------------------------------------------\n"

#define RESULT_LINE_PER_PORT "-------------------------------------------------------------------------------------------------------------------------------------------------------------------\n"
#define CYCLES	"cycles"
#define USEC	"usec"
/* The format of the results */
#define RESULT_FMT		" #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]"

#define RESULT_FMT_PER_PORT	" #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]   BW Port1[MB/sec]   MsgRate Port1[Mpps]   BW Port2[MB/sec]   MsgRate Port2[Mpps]"

#define RESULT_FMT_G	" #bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]"

#define RESULT_FMT_G_PER_PORT	" #bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]   BW Port1[Gb/sec]   MsgRate Port1[Mpps]   BW Port2[Gb/sec]   MsgRate Port2[Mpps]"

#define RESULT_FMT_QOS  " #bytes    #sl      #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]"

#define RESULT_FMT_G_QOS  " #bytes    #sl      #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]"

#define RESULT_FMT_LAT " #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]    t_avg[usec]    t_stdev[usec]   99""%"" percentile[usec]   99.9""%"" percentile[usec] "

#define RESULT_FMT_LAT_DUR " #bytes        #iterations       t_avg[usec]    tps average"

#define RESULT_EXT "\n"

#define RESULT_EXT_CPU_UTIL "    CPU_Util[%%]\n"

#define RESULT_FMT_FS_RATE " #flows	fs_min_time[usec]	fs_max_time[usec]	fs_typical_time[usec]	fs_avg_time[usec]	fps[flow per sec]"

#define RESULT_FMT_FS_RATE_DUR " #flows		fs_avg_time[usec]    	fps[flow per sec]"

/* Result print format */
#define REPORT_FMT     " %-7lu    %-10lu       %-7.2lf            %-7.2lf		   %-7.6lf"

#define REPORT_FMT_EXT     " %-7lu    %lu           %-7.6lf            %-7.6lf            %-7.6lf"

#define REPORT_FMT_PER_PORT     " %-7lu    %-10lu     %-7.2lf            %-7.2lf		   %-7.6lf        %-7.2lf            %-7.6lf              %-7.2lf            %-7.6lf"

#define REPORT_EXT	"\n"

#define REPORT_EXT_CPU_UTIL	"	    %-3.2f\n"

#define REPORT_FMT_QOS " %-7lu    %d           %lu           %-7.2lf            %-7.2lf                  %-7.6lf\n"

/* Result print format for latency tests. */
#define REPORT_FMT_LAT " %-7lu %d          %-7.2f        %-7.2f      %-7.2f  	       %-7.2f     	%-7.2f		%-7.2f 		%-7.2f"
#define REPORT_FMT_LAT_DUR " %-7lu       %d            %-7.2f        %-7.2f"

#define REPORT_FMT_FS_RATE " %d          %-7.2f        		%-7.2f      	%-7.2f  	       		%-7.2f     	%-7.2f"

#define REPORT_FMT_FS_RATE_DUR " %d               %-7.2f		%-7.2f"

#define CHECK_VALUE(arg,type,minv,maxv,name) 						    					\
{ arg = (type)strtol(optarg, NULL, 0); if ((arg < minv) || (arg > maxv))                \
	{ fprintf(stderr," %s should be between %d and %d\n",name,minv,maxv); return 1; }}

/* Macro for allocating. */
#define ALLOCATE(var,type,size)                                     \
{ if((var = (type*)malloc(sizeof(type)*(size))) == NULL)        \
	{ fprintf(stderr," Cannot Allocate\n"); exit(1);}}

/* This is our string builder */
#define GET_STRING(orig,temp) 						            \
{ ALLOCATE(orig,char,(strlen(temp) + 1)); strcpy(orig,temp); }

#define MTU_SIZE(mtu_ind) (((uint64_t)1 << (MTU_FIX + mtu_ind)))

#define MAX_VERSION 16	/* Reserve 15 bytes for version numbers */

#define GET_ARRAY_SIZE(arr) (sizeof((arr)) / sizeof((arr[0])))

/* The Verb of the benchmark. */
typedef enum { SEND , WRITE, READ, ATOMIC } VerbType;

/* The type of the test */
typedef enum { LAT , BW , LAT_BY_BW, FS_RATE } TestType;

/* The type of the machine ( server or client actually). */
typedef enum { SERVER , CLIENT , UNCHOSEN} MachineType;

/* The type of the machine ( server or client actually). */
typedef enum { LOCAL , REMOTE } PrintDataSide;

/* The atomic test type */
typedef enum {CMP_AND_SWAP, FETCH_AND_ADD} AtomicType;

/* Type of test method. */
typedef enum { ITERATIONS , DURATION } TestMethod;

/* for duration calculation */
typedef enum { START_STATE, SAMPLE_STATE, STOP_SAMPLE_STATE, END_STATE} DurationStates;

/* Report format (Gbit/s VS MB/s) */
enum ctx_report_fmt { GBS, MBS };

/* Test method */
enum ctx_test_method {RUN_REGULAR, RUN_ALL, RUN_INFINITELY};

/* The type of the device */
enum ctx_device {
	DEVICE_ERROR		= -1,
	UNKNOWN			= 0,
	CONNECTX 		= 1,
	CONNECTX2 		= 2,
	CONNECTX3 		= 3,
	CONNECTIB 		= 4,
	LEGACY 			= 5,
	CHELSIO_T4 		= 6,
	CHELSIO_T5 		= 7,
	CONNECTX3_PRO		= 8,
	SKYHAWK			= 9,
	CONNECTX4		= 10,
	CONNECTX4LX		= 11,
	QLOGIC_E4		= 12,
	QLOGIC_AH		= 13,
	CHELSIO_T6		= 14,
	CONNECTX5		= 15,
	CONNECTX5EX		= 16,
	CONNECTX6		= 17,
	BLUEFIELD		= 18
};

/* Units for rate limiter */
enum rate_limiter_units {MEGA_BYTE_PS, GIGA_BIT_PS, PACKET_PS};

/*Types rate limit*/
enum rate_limiter_types {HW_RATE_LIMIT, SW_RATE_LIMIT, PP_RATE_LIMIT, DISABLE_RATE_LIMIT};

/* Verbosity Levels for test report */
enum verbosity_level {FULL_VERBOSITY=-1, OUTPUT_BW=0, OUTPUT_MR, OUTPUT_LAT };

/*Accelerated verbs */
enum verbs_intf {
	NORMAL_INTF,
	ACCL_INTF,
};

struct cpu_util_data {
	int enable;
	long long ustat[2];
	long long idle[2];
};

struct check_alive_data {
	int current_totrcnt;
	int last_totrcnt;
	int g_total_iters;
	int to_exit;
	int is_events;
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
 *
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
}__attribute__((packed));*/

struct perftest_parameters {

	int				port;
	char				*ib_devname;
	char				*servername;
	uint8_t				ib_port;
	uint8_t				ib_port2;
	int				mtu;
	enum ibv_mtu			curr_mtu;
	uint64_t			size;
	uint64_t			dct_key;
	int				iters;
	uint64_t			iters_per_port[2];
	uint64_t			*port_by_qp;
	int				tx_depth;
	uint8_t				qp_timeout;
	uint8_t				sl;
	int				gid_index;
	int				gid_index2;
	int				use_gid_user;
	uint8_t				source_mac[6];
	uint8_t				dest_mac[6];
	int				is_source_mac;
	int				is_dest_mac;
	uint8_t				server_ip6[16];
	uint8_t				client_ip6[16];
	uint8_t				local_ip6[16];
	uint8_t				remote_ip6[16];
	uint8_t				local_mac[6];
	uint8_t				remote_mac[6];
	uint32_t			client_ip;
	uint32_t			server_ip;
	int				is_server_ip;
	int				is_client_ip;
	uint32_t			local_ip;
	uint32_t			remote_ip;
	int				server_port;
	int				client_port;
	int				tcp;
	int				is_server_port;
	int				is_client_port;
	int				local_port;
	int				remote_port;
	int 				is_old_raw_eth_param;
	int 				is_new_raw_eth_param;
	uint16_t			ethertype;
	int				is_ethertype;
	int				cpu_freq_f;
	int				connection_type;
	int				num_of_qps;
	int				use_event;
	int				eq_num;
	int				use_eq_num;
	int 				inline_size;
	int				inline_recv_size;
	int				out_reads;
	int				rx_depth;
	int				duplex;
	int				noPeak;
	int				cq_mod;
	int 				spec;
	int 				dualport;
	int 				post_list;
	int				duration;
	int 				use_srq;
	int				use_xrc;
	int				use_rss;
	int				srq_exists;
	int				tos;
	int				margin;
	int 				is_bw_limit_passed;
	int 				is_msgrate_limit_passed;
	int 				is_limit_bw;
	int 				is_limit_msgrate;
	float				limit_bw;
	float				limit_msgrate;
	uint32_t			rem_ud_qpn;
	uint32_t			rem_ud_qkey;
	int8_t				link_type;
	int8_t				link_type2;
	MachineType			machine;
	PrintDataSide			side;
	VerbType			verb;
	TestType			tst;
	AtomicType			atomicType;
	TestMethod			test_type;
	DurationStates			state;
	int				sockfd;
	char				version[MAX_VERSION];
	char				rem_version[MAX_VERSION];
	cycles_t			*tposted;
	cycles_t			*tcompleted;
	int				use_mcg;
	int 				use_rdma_cm;
	int				is_reversed;
	int				work_rdma_cm;
	char				*user_mgid;
	int				buff_size;
	int             		pkey_index;
	int				raw_qos;
	int				use_cuda;
	char				*mmap_file;
	unsigned long			mmap_offset;
	/* New test params format pilot. will be used in all flags soon,. */
	enum ctx_test_method 		test_method;
	enum ibv_transport_type 	transport_type;
	enum ctx_report_fmt		report_fmt;
	struct report_options  		*r_flag	;
	int 				mac_fwd;
	int report_both; /* in bidirectional tests, report tx and rx separately */
	/* results limits */
	float 				min_bw_limit;
	float 				min_msgRate_limit;
	/* Rate Limiter */
	char				*rate_limit_str;
	double 				rate_limit;
	int				valid_hw_rate_limit_index;
	int 				burst_size;
	int				typical_pkt_size;
	enum 				rate_limiter_units rate_units;
	enum 				rate_limiter_types rate_limit_type;
	int				is_rate_limit_type;
	enum verbosity_level 		output;
	int 				cpu_util;
	struct cpu_util_data 		cpu_util_data;
	int 				latency_gap;
	int 				flow_label;
	int 				retry_count;
	int 				dont_xchg_versions;
	int 				use_exp;
	int 				ipv6;
	int 				raw_ipv6;
	int 				report_per_port;
	int 				use_odp;
	int				use_hugepages;
	int				use_promiscuous;
	int				use_sniffer;
	int				check_alive_exited;
	int				raw_mcast;
	int				masked_atomics;
	int				cycle_buffer;
	int				cache_line_size;
	enum verbs_intf			verb_type;
	int				is_exp_cq;
	int				is_exp_qp;
	int				use_res_domain;
	int				mr_per_qp;
	uint16_t			dlid;
	uint8_t				traffic_class;
	uint32_t			wait_destroy;
	int				disable_fcs;
	int				flows;
	int				flows_burst;
	uint32_t			reply_every;
	int				perform_warm_up;
	int				use_ooo;
	int                             vlan_en;
	uint32_t			vlan_pcp;
	void 				(*print_eth_func)(void*);

};

struct report_options {
	int unsorted;
	int histogram;
	int cycles;
};

struct bw_report_data {
	unsigned long size;
	uint64_t iters;
	double bw_peak;
	double bw_avg;
	double bw_avg_p1;
	double bw_avg_p2;
	double msgRate_avg;
	double msgRate_avg_p1;
	double msgRate_avg_p2;
	int sl;
};

struct rate_gbps_string {
	enum ibv_rate rate_gbps_enum;
	char* rate_gbps_str;
};
/*
 *Enums taken from verbs.h
 */
static const struct rate_gbps_string RATE_VALUES[RATE_VALUES_COUNT] = {
	{IBV_RATE_2_5_GBPS, "2.5"},
	{IBV_RATE_5_GBPS, "5"},
	{IBV_RATE_10_GBPS, "10"},
	{IBV_RATE_14_GBPS, "14"},
	{IBV_RATE_20_GBPS, "20"},
	{IBV_RATE_25_GBPS, "25"},
	{IBV_RATE_30_GBPS, "30"},
	{IBV_RATE_40_GBPS, "40"},
	{IBV_RATE_56_GBPS, "56"},
	{IBV_RATE_60_GBPS, "60"},
	{IBV_RATE_80_GBPS, "80"},
	{IBV_RATE_100_GBPS, "100"},
	{IBV_RATE_112_GBPS, "112"},
	{IBV_RATE_120_GBPS, "120"},
	{IBV_RATE_168_GBPS, "168"},
	{IBV_RATE_200_GBPS, "200"},
	{IBV_RATE_300_GBPS, "300"},
	{IBV_RATE_MAX, "MAX"}
};




/* link_layer_str
 *
 * Description : Return a String representation of the link type.
 *
 * link_layer   : (According to verbs.h) :
 *		IBV_LINK_LAYER_UNSPECIFIED.
 *      IBV_LINK_LAYER_INFINIBAND.
 *      IBV_LINK_LAYER_ETHERNET.
 *
 * Return Value :"IB", "Etherent" or "Unknown".
 */
const char *link_layer_str(int8_t link_layer);

/* str_link_layer
 *
 * Description : Try to parse a string into a verbs link layer type.
 *
 * link_layer   : (According to verbs.h) :
 *      "IB"       -> IBV_LINK_LAYER_INFINIBAND.
 *      "Ethernet" -> IBV_LINK_LAYER_ETHERNET.
 *      otherwise  -> LINK_FAILURE.
 *
 * Return Value : IBV_LINK_LAYER or LINK_FAILURE
 */
const int str_link_layer(const char *str);

/* parser
 *
 * Description : Setting default test parameters and parsing the user choises
 *
 * Parameters :
 *
 *      user_param  - the parameters element.
 *      argv & argc - from the user prompt.
 *
 * Return Value : 0 upon success. -1 if it fails.
 */
int parser(struct perftest_parameters *user_param,char *argv[], int argc);

/* check_link_and_mtu
 *
 * Description : Configures test MTU,inline and link layer of the test.
 *
 * Parameters :
 *
 *      context    - Context of the device.
 *      user_param - Perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int check_link(struct ibv_context *context,struct perftest_parameters *user_param);

/* check_link_and_mtu
 *
 * Description : Configures test MTU,inline and link layer of the test.
 *
 * Parameters :
 *

 *	 context    - Context of the device.
 *	 user_param - Perftest parameters.
 *
 * Return Value : SUCCESS, FAILURE.
 */
int check_link_and_mtu(struct ibv_context *context,struct perftest_parameters *user_param);

/* ctx_print_test_info
 *
 * Description : Prints all the parameters selected for this run.
 *
 * Parameters :
 *
 *	 user_param  - the parameters parameters.
 *
 */
void ctx_print_test_info(struct perftest_parameters *user_param);

/* print_report_bw
 *
 * Description : Calculate the peak and average throughput of the BW test.
 *				 The function will print when not in duplex mode.
 *
 * Parameters :
 *
 *	 user_param  - the parameters parameters.
 *   my_bw_rep   - get my bw test report.
 *
 */
void print_report_bw (struct perftest_parameters *user_param, struct bw_report_data *my_bw_rep);

/* print_full_bw_report
 *
 * Description : Print the peak and average throughput of the BW test.
 *				 If rem_bw_rep is not NULL, the function will sum the server and client results.
 *
 * Parameters :
 *
 *	 user_param  - the parameters parameters.
 *   my_bw_rep   - my bw test report.
 *   rem_bw_rep   - remote's bw test report.
 *
 */
void print_full_bw_report (struct perftest_parameters *user_param, struct bw_report_data *my_bw_rep, struct bw_report_data *rem_bw_rep);

/* print_report_lat
 *
 * Description : Print the min/max/median latency samples taken from a latency test.
 * 				 It also support a unsorted/histogram report of all samples.
 *
 * Parameters :
 *
 *   user_param  - the parameters parameters.
 *
 */
void print_report_lat (struct perftest_parameters *user_param);

/* print_report_lat_duration
 *
 * Description : Prints only the avergae latency for samples taken from a latency test
 *				 With Duration.
 *
 * Parameters :
 *
 *   user_param  - the parameters parameters.
 *
 */
void print_report_lat_duration (struct perftest_parameters *user_param);

/* print_report_fs_rate
 *
 * Description : Prints the Flow steering rate and avarage latency to create flow
 *
 * Parameters :
 *
 *   user_param  - the parameters parameters.
 *
 */
void print_report_fs_rate (struct perftest_parameters *user_param);

/* set_mtu
 *
 * Description : set MTU from the port or user
 *
 * Parameters :
 *
 *   context  - Context of the device.
 *   ib_port  - ib port number that's in use.
 *   user_mtu  - MTU that the user supplied.
 *
 * Return Value : MTU size
 */
enum ibv_mtu set_mtu(struct ibv_context *context,uint8_t ib_port,int user_mtu);

/* set_eth_mtu
 *
 * Description : set MTU for Raw Ethernet tests
 *
 * Parameters :
 *
 *   user_param  - the parameters parameters.
 *
 * Return Value : MTU size
 */
int set_eth_mtu(struct perftest_parameters *user_param);

/******************************************************************************
 *
 ******************************************************************************/
enum ctx_device ib_dev_name(struct ibv_context *context);

#endif /* PERFTEST_RESOURCES_H */
