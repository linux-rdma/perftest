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
#ifndef PERFTEST_PARAMETERS_H
#define PERFTEST_PARAMETERS_H

// Files included for work.
#include <infiniband/verbs.h>
#include <unistd.h>
#include <malloc.h>
#include "get_clock.h"

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

// Connection types available.
#define RC  (0)
#define UC  (1)
#define UD  (2)
#define RawEth  (3)
// #define XRC 3 (TBD)

// Genral control definitions
#define OFF	     (0)
#define ON 	     (1)
#define SUCCESS	     (0)
#define FAILURE	     (1)
#define VERSION_EXIT (10)
#define HELP_EXIT	 (11)
#define MTU_FIX	     (7)
#define MAX_SIZE     (8388608)
#define LINK_FAILURE (4)
#define MAX_OUT_READ_HERMON (16)
#define MAX_OUT_READ        (4)
#define UD_ADDITION         (40)
#define RAWETH_ADDITION    (18)
#define HW_CRC_ADDITION    (4)

// Default Values of perftest parameters
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
#define DEF_INLINE    (-1)
#define DEF_TOS       (-1)

// Optimal Values for Inline
#define DEF_INLINE_WRITE (220)
#define DEF_INLINE_SEND_RC_UC (236)
#define DEF_INLINE_SEND_UD (188)

// Max and Min allowed values for perftest parameters.
#define MIN_TOS		(0)
#define MAX_TOS		(256)
#define MIN_IB_PORT   (1)
#define MAX_IB_PORT   (3)  //was 2
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
#define MIN_CQ_MOD    (1)
#define MAX_CQ_MOD    (1024)
#define MAX_INLINE    (912)

// Raw etherent defines
#define RAWETH_MIN_MSG_SIZE	(64)
#define MIN_MTU_RAW_ETERNET	(64)
#define MAX_MTU_RAW_ETERNET	(9600)


#define RESULT_LINE "---------------------------------------------------------------------------------------\n"

// The format of the results
#define RESULT_FMT		" #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]   MsgRate[Mpps]\n"

#define RESULT_FMT_G	" #bytes     #iterations    BW peak[Gb/sec]    BW average[Gb/sec]   MsgRate[Mpps]\n"

#define RESULT_FMT_LAT " #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]\n"

#define RESULT_FMT_LAT_DUR " #bytes        #iterations       t_avg[usec]\n"

// Result print format
#define REPORT_FMT     " %-7lu    %d           %-7.2lf            %-7.2lf		   %-7.6lf\n"

// Result print format for latency tests.
#define REPORT_FMT_LAT " %-7lu %d          %-7.2f        %-7.2f      %-7.2f\n"

#define REPORT_FMT_LAT_DUR " %-7lu       %d            %-7.2f\n"

#define CHECK_VALUE(arg,type,minv,maxv,name) 						    					\
	{ arg = (type)strtol(optarg, NULL, 0); if ((arg < minv) || (arg > maxv))                \
	{ fprintf(stderr," %s should be between %d and %d\n",name,minv,maxv); return 1; }}

// Macro for allocating.
#define ALLOCATE(var,type,size)                                     \
    { if((var = (type*)malloc(sizeof(type)*(size))) == NULL)        \
        { fprintf(stderr," Cannot Allocate\n"); exit(1);}}

// This is our string builder
#define GET_STRING(orig,temp) 						            \
	{ ALLOCATE(orig,char,(strlen(temp) + 1)); strcpy(orig,temp); }

#define MTU_SIZE(mtu_ind) (((uint64_t)1 << (MTU_FIX + mtu_ind)))

// The Verb of the benchmark.
typedef enum { SEND , WRITE, READ, ATOMIC } VerbType;

// The type of the test
typedef enum { LAT , BW } TestType;

// The type of the machine ( server or client actually).
typedef enum { SERVER , CLIENT , UNCHOSEN} MachineType;

// The type of the machine ( server or client actually).
typedef enum { LOCAL , REMOTE } PrintDataSide;

// The atomic test type
typedef enum {CMP_AND_SWAP, FETCH_AND_ADD} AtomicType;

// Type of test method.
typedef enum { ITERATIONS , DURATION } TestMethod;

//for duration calculation
typedef enum { START_STATE, SAMPLE_STATE, STOP_SAMPLE_STATE, END_STATE} DurationStates;

//Report format (Gbit/s VS MB/s)
enum ctx_report_fmt { GBS, MBS };

// Test method
enum ctx_test_method {RUN_REGULAR, RUN_ALL, RUN_INFINITELY};

// The type of the device
enum ctx_device {
	DEVICE_ERROR 	= -1 ,
	UNKNOWN 		= 0,
	CONNECTX 		= 1,
	CONNECTX2 		= 2,
	CONNECTX3 		= 3,
	CONNECTIB 		= 4,
	LEGACY 			= 5,
	CHELSIO_T4 		= 6,
	CHELSIO_T5 		= 7,
	CONNECTX3_PRO	= 8
};

struct perftest_parameters {

	int				port;
	char			*ib_devname;
	char			*servername;
	uint8_t			ib_port;
	uint8_t			ib_port2;
	int				mtu;
	enum ibv_mtu	curr_mtu;
	uint64_t		size;
	int				iters;
	int				tx_depth;
	uint8_t			qp_timeout;
	uint8_t			sl;
	int				gid_index;
	int				gid_index2;
	uint8_t			source_mac[6];
	uint8_t			dest_mac[6];
	int				is_source_mac;
	int				is_dest_mac;
	uint32_t		server_ip;
	uint32_t		client_ip;
	int				is_server_ip;
	int				is_client_ip;
	int				server_port;
	int				client_port;
	int				is_server_port;
	int				is_client_port;
	int				cpu_freq_f;
	int				connection_type;
	int				num_of_qps;
	int				use_event;
	int 			inline_size;
	int				out_reads;
	int				rx_depth;
	int				duplex;
	int				noPeak;
	int				cq_mod;
	int 			spec;
	int 			dualport;
	int 			post_list;
	int				duration;
	int				tos;
	int				margin;
	int 			is_bw_limit_passed;
	int 			is_msgrate_limit_passed;
	int 			is_limit_bw;
	int 			is_limit_msgrate;
	float			limit_bw;
	float			limit_msgrate;
	uint32_t		rem_ud_qpn;
	uint32_t		rem_ud_qkey;
	uint8_t			link_type;
	uint8_t			link_type2;
	MachineType		machine;
	PrintDataSide	side;
	VerbType		verb;
	TestType		tst;
	AtomicType		atomicType;
	TestMethod		test_type;
	DurationStates	state;
	int				sockfd;
	const char		*version;
	cycles_t		*tposted;
	cycles_t		*tcompleted;
	int				use_mcg;
	int 			use_rdma_cm;
	int				work_rdma_cm;
	char			*user_mgid;
	int				buff_size;
	// New test params format pilot. will be used in all flags soon,.
	enum ctx_test_method 	test_method;
	enum ibv_transport_type transport_type;
	enum ctx_report_fmt		report_fmt;
	struct report_options  	*r_flag;
	int 			mac_fwd;
	//results limits
	float min_bw_limit;
	float min_msgRate_limit;
};

struct report_options {
	int unsorted;
	int histogram;
	int cycles;
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
const char *link_layer_str(uint8_t link_layer);

/* parser
 *
 * Description : Setting default test parameters and parsing the user choises
 *
 * Parameters :
 *
 *	 user_param  - the parameters element.
 *	 argv & argc - from the user prompt.
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
 *	 context    - Context of the deivce.
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
 * Description : Print the peak and average throughput of the BW test.
 *
 * Parameters :
 *
 *	 user_param  - the parameters parameters.
 *
 */
void print_report_bw (struct perftest_parameters *user_param);

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

#endif /* PERFTEST_RESOURCES_H */
