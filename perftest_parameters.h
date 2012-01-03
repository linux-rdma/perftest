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

#ifndef PERFTEST_PARSER_H
#define PERFTEST_PARSER_H

// Files included for work.
#include <infiniband/verbs.h>

// Connection types availible.
#define RC  (0)
#define UC  (1) 
#define UD  (2)
// #define XRC 3 (TBD)

// Genral control definitions
#define OFF	     (0)
#define ON 	     (1)
#define SUCCESS	     (0)
#define FAILURE	     (1)
#define VERSION_EXIT (10)
#define MTU_FIX	     (7)
#define MAX_SIZE     (8388608)
#define LINK_FAILURE (4)
#define MAX_OUT_READ_HERMON (16)
#define MAX_OUT_READ        (4)
#define UD_ADDITION         (40)

// Default Values of perftest parameters
#define DEF_PORT      (18515)
#define DEF_IB_PORT   (1)
#define DEF_SIZE_BW   (65536)
#define DEF_SIZE_LAT  (2)
#define DEF_ITERS     (1000)
#define DEF_ITERS_WB  (5000)
#define DEF_TX_BW     (300)
#define DEF_TX_LAT    (50)
#define DEF_QP_TIME   (14)
#define DEF_SL	      (0)
#define DEF_GID_INDEX (-1)
#define DEF_NUM_QPS   (1)
#define DEF_INLINE_BW (0)
#define DEF_INLINE_LT (400)
#define DEF_RX_RDMA   (1)
#define DEF_RX_SEND   (600)
#define DEF_CQ_MOD    (50)
#define DEF_SIZE_ATOMIC (8)

// Max and Min allowed values for perftest parameters.
#define MIN_IB_PORT   (1)
#define MAX_IB_PORT   (2) 
#define MIN_ITER      (5)
#define MAX_ITER      (100000000)
#define MIN_TX 	      (50)
#define MAX_TX	      (15000)
#define MIN_SL	      (0)
#define MAX_SL	      (15)
#define MIN_GID_IX    (0)
#define MAX_GID_IX    (64)
#define MIN_QP_NUM    (1)
#define MAX_QP_NUM    (8)
#define MIN_INLINE    (0)
#define MAX_INLINE    (400)
#define MIN_QP_MCAST  (1)
#define MAX_QP_MCAST  (56)
#define MIN_RX	      (1)
#define MAX_RX	      (15000)
#define MIN_CQ_MOD    (1)
#define MAX_CQ_MOD    (1000)


#define RESULT_LINE "------------------------------------------------------------------\n"

// The format of the results
#define RESULT_FMT     " #bytes     #iterations    BW peak[MB/sec]    BW average[MB/sec]\n"

#define RESULT_FMT_LAT " #bytes #iterations    t_min[usec]    t_max[usec]  t_typical[usec]\n"

// Result print format
#define REPORT_FMT     " %-7lu    %d           %-7.2f            %-7.2f\n"

// Result print format for latency tests.
#define REPORT_FMT_LAT " %-7lu %d          %-7.2f        %-7.2f      %-7.2f\n"

#define CHECK_VALUE(arg,minv,maxv,name) 						    					\
	{ arg = strtol(optarg, NULL, 0); if ((arg < minv) || (arg > maxv))                  \
	{ fprintf(stderr," %s should be between %d and %d\n",name,minv,maxv); return 1; }}

// Macro for allocating.
#define ALLOCATE(var,type,size)                                     \
    { if((var = (type*)malloc(sizeof(type)*(size))) == NULL)        \
        { fprintf(stderr," Cannot Allocate\n"); exit(1);}}

#define GET_STRING(orig,temp) 						                \
	{ ALLOCATE(orig,char,(strlen(temp) + 1)); strcpy(orig,temp); }

#define MTU_SIZE(mtu_ind) ((1 << (MTU_FIX + mtu_ind)))

// The Verb of the benchmark.
typedef enum { SEND , WRITE, READ, ATOMIC } VerbType;

// The type of the machine ( server or client actually).
typedef enum { LAT , BW } TestType;

// The type of the machine ( server or client actually).
typedef enum { SERVER , CLIENT } MachineType;

// The type of the machine ( server or client actually).
typedef enum { LOCAL , REMOTE } PrintDataSide;

// The atomic test type
typedef enum {CMP_AND_SWAP, FETCH_AND_ADD} AtomicType;

// The type of the device (Hermon B0/A0 or no)
typedef enum { ERROR = -1 , NOT_HERMON = 0 , HERMON = 1} Device;

struct perftest_parameters {

	int				port;
	char			*ib_devname;
	char			*servername;
	int				ib_port;
	int				mtu;
	enum ibv_mtu	curr_mtu;
	uint64_t		size;
	int				iters;
	int				tx_depth;
	int				qp_timeout;
	int				sl;
	int				gid_index;
	int				all;
	int				cpu_freq_f;
	int				connection_type;
	int				num_of_qps;
	int				use_event;
	int 			inline_size;
	int				out_reads;
	int				use_mcg;
	int 			use_rdma_cm;
    int				work_rdma_cm;
	char			*user_mgid;
	int				rx_depth;
	int				duplex;
	int				noPeak;
	int				cq_mod;
	int 			spec;
	uint8_t 		link_type;
    MachineType		machine;
    PrintDataSide	side;
	VerbType		verb;
	TestType		tst;
	AtomicType		atomicType;
	int				sockfd;
	// int				cq_size;
	float			version;
	struct report_options  *r_flag;
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

#endif /* PERFTEST_RESOURCES_H */
