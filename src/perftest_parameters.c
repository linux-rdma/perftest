#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include <arpa/inet.h>
#include "perftest_parameters.h"

#define MAC_LEN (17)
#define MAC_ARR_LEN (6)
#define HEX_BASE (16)

static const char *connStr[] = {"RC","UC","UD","RawEth","XRC","DC"};
static const char *testsStr[] = {"Send","RDMA_Write","RDMA_Read","Atomic"};
static const char *portStates[] = {"Nop","Down","Init","Armed","","Active Defer"};
static const char *qp_state[] = {"OFF","ON"};
static const char *exchange_state[] = {"Ethernet","rdma_cm"};
static const char *atomicTypesStr[] = {"CMP_AND_SWAP","FETCH_AND_ADD"};

/******************************************************************************
 * parse_mac_from_str.
 *
 * Description : parse string by format of"XX:XX:XX:XX:XX:XX" to uint8_t array in size 6 for MAC adderes
 *
 *  Parameters :
 *		mac - char*.
 *		*addr - pointer to output array
 *
 * Return Value : SUCCESS, FAILURE.
******************************************************************************/
static int parse_mac_from_str(char *mac, u_int8_t *addr)
{
	char tmpMac[MAC_LEN+1];
	char *tmpField;
	int fieldNum = 0;

	if (strlen(mac) != MAC_LEN)
	{
		fprintf(stderr, "invalid MAC length\n");
		return FAILURE;
	}
	if (addr == NULL)
	{
		fprintf(stderr, "invalid  output addr array\n");
		return FAILURE;
	}

	strcpy(tmpMac, mac);
	tmpField = strtok(tmpMac, ":");
	while (tmpField != NULL && fieldNum < MAC_ARR_LEN)
	{
	  char *chk;
	  int tmpVal;
	  tmpVal = strtoul(tmpField, &chk, HEX_BASE);
	  if (tmpVal > 0xff)
	  {
		fprintf(stderr, "field %d value %X out of range\n", fieldNum, tmpVal);
		return FAILURE;
	  }
	  if (*chk != 0)
	  {
		fprintf(stderr, "Non-digit character %c (%0x) detected in field %d\n", *chk, *chk, fieldNum);
		return FAILURE;
	  }
	  addr[fieldNum++] = (u_int8_t) tmpVal;
	  tmpField = strtok(NULL, ":");
	}
	if (tmpField != NULL || fieldNum != MAC_ARR_LEN)
	{
		fprintf(stderr, "MAC address longer than six fields\n");
		return FAILURE;
	}
	return SUCCESS;
}

/******************************************************************************
  parse_ip_from_str.
 *
 * Description : Convert from presentation format of an Internet number in nuffer
   starting at CP to the binary network format and store result for
   interface type AF in buffer starting at BUF.
 *
 *  Parameters :
 *		*ip - char* ip string.
 *		*addr - pointer to output array
 *
 * Return Value : SUCCESS, FAILURE.
 *
 ******************************************************************************/
int parse_ip_from_str(char *ip, u_int32_t *addr)
{
	return inet_pton(AF_INET, ip, addr);
}

/******************************************************************************
  check_valid_udp_port.
 ******************************************************************************/
int check_if_valid_udp_port(int udp_port)
{
	return ON;
}
/******************************************************************************
  get cache line size from system
 ******************************************************************************/
static int get_cache_line_size()
{
	int size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
	if (size == 0)
	{
		char* file_name = "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size";
		FILE *fp;
		char line[10];
		fp = fopen(file_name, "r");      //open file , read only
		if (fp == NULL)
		{
			return DEF_CACHE_LINE_SIZE;
		}
		fgets(line,10,fp);
		size = atoi(line);
		fclose(fp);
	}

	if (size <= 0)
		size = DEF_CACHE_LINE_SIZE;

	return size;
}
/******************************************************************************
 *
 ******************************************************************************/
static void usage(const char *argv0,VerbType verb,TestType tst)	{

	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");

	if (verb != ATOMIC) {

		printf("  -a, --all ");
		printf(" Run sizes from 2 till 2^23\n");
	}

	if (verb == ATOMIC) {
		printf("  -A, --atomic_type=<type> ");
		printf(" type of atomic operation from {CMP_AND_SWAP,FETCH_AND_ADD} (default FETCH_AND_ADD)\n");
	}

	if (tst == BW) {
		printf("  -b, --bidirectional ");
		printf(" Measure bidirectional bandwidth (default unidirectional)\n");
	}

	if (verb == SEND) {
		printf("  -c, --connection=<RC/XRC/UC/UD/DC> ");
		printf(" Connection type RC/XRC/UC/UD/DC (default RC)\n");
	} else 	if (verb == WRITE) {
		printf("  -c, --connection=<RC/XRC/UC/DC> ");
		printf(" Connection type RC/XRC/UC/DC (default RC)\n");
	} else if (verb == READ || verb == ATOMIC) {
		printf("  -c, --connection=<RC/XRC/DC> ");
		printf(" Connection type RC/XRC/DC (default RC)\n");
	}

	if (tst == LAT) {
		printf("  -C, --report-cycles ");
		printf(" report times in cpu cycle units (default microseconds)\n");
	}

	printf("  -d, --ib-dev=<dev> ");
	printf(" Use IB device <dev> (default first device found)\n");

	printf("  -D, --duration ");
	printf(" Run test for a customized period of seconds.\n");

	if (verb != WRITE) {
		printf("  -e, --events ");
		printf(" Sleep on CQ events (default poll)\n");
	}

	printf("  -f, --margin ");
	printf(" measure results within margins. (default=2sec)\n");

	printf("  -F, --CPU-freq ");
	printf(" Do not fail even if cpufreq_ondemand module is loaded\n");

	if (verb == SEND) {
		printf("  -g, --mcg ");
		printf(" Send messages to multicast group with 1 QP attached to it.\n");
	}

	printf("  -h, --help ");
	printf(" Show this help screen.\n");

	if (tst == LAT) {
		printf("  -H, --report-histogram ");
		printf(" Print out all results (default print summary only)\n");
	}

	printf("  -i, --ib-port=<port> ");
	printf(" Use port <port> of IB device (default %d)\n",DEF_IB_PORT);

	if (verb != READ && verb != ATOMIC) {
		printf("  -I, --inline_size=<size> ");
		printf(" Max size of message to be sent in inline\n");
	}

	if (tst == BW) {
		printf("  -l, --post_list=<list size>");
		printf(" Post list of WQEs of <list size> size (instead of single post)\n");
	}

	printf("  -m, --mtu=<mtu> ");
	printf(" Mtu size : 256 - 4096 (default port mtu)\n");

	if (verb == SEND) {
		printf("  -M, --MGID=<multicast_gid> ");
		printf(" In multicast, uses <multicast_gid> as the group MGID.\n");
	}

	printf("  -n, --iters=<iters> ");
	printf(" Number of exchanges (at least %d, default %d)\n", MIN_ITER, ((verb == WRITE) && (tst == BW)) ? DEF_ITERS_WB : DEF_ITERS);

	if (tst == BW) {
		printf("  -N, --no peak-bw ");
		printf(" Cancel peak-bw calculation (default with peak)\n");
	}

	if (verb == READ || verb == ATOMIC) {
		printf("  -o, --outs=<num> ");
		printf(" num of outstanding read/atom(default max of device)\n");
	}

	if (tst == BW) {
		printf("  -O, --dualport ");
		printf(" Run test in dual-port mode.\n");
	}

	printf("  -p, --port=<port> ");
	printf(" Listen on/connect to port <port> (default %d)\n",DEF_PORT);

	if (tst == BW) {
		printf("  -q, --qp=<num of qp's>  Num of qp's(default %d)\n",DEF_NUM_QPS);
	}

	if (tst == BW) {
		printf("  -Q, --cq-mod ");
		printf(" Generate Cqe only after <--cq-mod> completion\n");
	}

	if (verb == SEND) {
		printf("  -r, --rx-depth=<dep> ");
		printf(" Rx queue size (default %d).",DEF_RX_SEND);
		printf(" If using srq, rx-depth controls max-wr size of the srq\n");
	}

	printf("  -R, --rdma_cm ");
	printf(" Connect QPs with rdma_cm and run test on those QPs\n");

	if (verb != ATOMIC) {
		printf("  -s, --size=<size> ");
		printf(" Size of message to exchange (default %d)\n",tst == LAT ? DEF_SIZE_LAT : DEF_SIZE_BW);
	}

	printf("  -S, --sl=<sl> ");
	printf(" SL (default %d)\n",DEF_SL);

	if (tst == BW) {
		printf("  -t, --tx-depth=<dep> ");
		printf(" Size of tx queue (default %d)\n",tst == LAT ? DEF_TX_LAT : DEF_TX_BW);
	}

	printf("  -T, --tos=<tos value> ");
	printf(" Set <tos_value> to RDMA-CM QPs. availible only with -R flag. values 0-256 (default off)\n");

	printf("  -u, --qp-timeout=<timeout> ");
	printf(" QP timeout, timeout value is 4 usec * 2 ^(timeout), default %d\n",DEF_QP_TIME);

	if (tst == LAT) {
		printf("  -U, --report-unsorted ");
		printf(" (implies -H) print out unsorted results (default sorted)\n");
	}

	printf("  -V, --version ");
	printf(" Display version number\n");

	if (tst == BW) {
		printf("  -w, --limit_bw ");
		printf(" Set verifier limit for bandwidth\n");
	}
	printf("  -x, --gid-index=<index> ");
	printf(" Test uses GID with GID index (Default : IB - no gid . ETH - 0)\n");

	if (tst == BW) {
		printf("  -y, --limit_msgrate ");
		printf(" Set verifier limit for Msg Rate\n");
	}

	printf("  -z, --com_rdma_cm ");
	printf(" Communicate with rdma_cm module to exchange data - use regular QPs\n");

	//Long flags
	putchar('\n');

	printf("      --cpu_util ");
	printf(" Show CPU Utilization in report, valid only in Duration mode \n");

	printf("      --dont_xchg_versions ");
	printf(" Do not exchange versions and MTU with other side \n");

	if (verb != WRITE) {
		printf("      --inline_recv=<size> ");
		printf(" Max size of message to be sent in inline receive\n");
	}

	printf("      --ipv6 ");
	printf(" Use IPv6 GID. Default is IPv4\n");

	if (tst == LAT) {
		printf("      --latency_gap=<delay_time> ");
                printf(" delay time between each post send\n");
	}
	printf("      --output=<units>");
	printf(" Set verbosity output level: bandwidth , message_rate, latency_typical \n");

	printf("      --pkey_index=<pkey index> PKey index to use for QP\n");

	if ( tst == BW ) {
		printf("      --report-both ");
		printf(" Report RX & TX results separately on Bidirectinal BW tests\n");

		printf("      --report_gbits ");
		printf(" Report Max/Average BW of test in Gbit/sec (instead of MB/sec)\n");

		printf("      --report-per-port ");
		printf(" Report BW data on both ports when running Dualport and Duration mode\n");

		printf("      --reversed ");
		printf(" Reverse traffic direction - Server send to client\n");

		printf("      --run_infinitely ");
		printf(" Run test forever, print results every <duration> seconds\n");
	}

        printf("      --retry_count=<value> ");
        printf(" Set retry count value in rdma_cm mode\n");

	#ifdef HAVE_CUDA
	printf("      --use_cuda ");
	printf(" Use CUDA lib for GPU-Direct testing.\n");
	#endif

	#ifdef HAVE_VERBS_EXP
	printf("      --use_exp ");
	printf(" Use Experimental verbs in data path. Default is OFF.\n");
	#endif

	#ifdef HAVE_ODP
	printf("      --odp ");
	printf(" Use On Demand Paging instead of Memory Registration.\n");
	#endif

	if (tst == BW) {
		printf("\n Rate Limiter:\n");
		printf("      --burst_size=<size>");
		printf(" Set the amount of messages to send in a burst when using rate limiter\n");

		printf("      --rate_limit=<rate[pps]>");
		printf(" Set the maximum rate of sent packages\n");

		printf("      --rate_units=<units>");
		printf(" [Mgp] Set the units for rate limit to MBps (M), Gbps (g) or pps (p)\n");
	}

	putchar('\n');
}
/******************************************************************************
  usage
 ******************************************************************************/
void usage_raw_ethernet(){
		printf("  Raw Ethernet options :\n");
		printf("  -B, --source_mac ");
		printf(" source MAC address by this format XX:XX:XX:XX:XX:XX (default take the MAC address form GID)\n");

		printf("  -E, --dest_mac ");
		printf(" destination MAC address by this format XX:XX:XX:XX:XX:XX **MUST** be entered \n");

		printf("  -J, --dest_ip ");
		printf(" destination ip address by this format X.X.X.X (using to send packets with IP header)\n");

		printf("  -j, --source_ip ");
		printf(" source ip address by this format X.X.X.X (using to send packets with IP header)\n");

		printf("  -K, --dest_port ");
		printf(" destination port number (using to send packets with UDP header as default, or you can use --tcp flag to send TCP Header)\n");

		printf("  -k, --source_port ");
		printf(" source port number (using to send packets with UDP header as default, or you can use --tcp flag to send TCP Header)\n");

		printf("  -Z, --server ");
		printf(" choose server side for the current machine (--server/--client must be selected )\n");

		printf("  -P, --client ");
		printf(" choose client side for the current machine (--server/--client must be selected)\n");

		printf("  -v, --mac_fwd ");
		printf(" run mac forwarding test \n");

		printf("      --promiscuous");
		printf(" run promiscuous mode.\n");

		printf("      --tcp ");
		printf(" send TCP Packets. must include IP and Ports information.\n");

		printf("\n");

}
/******************************************************************************
 *
 ******************************************************************************/
static void init_perftest_params(struct perftest_parameters *user_param) {

	user_param->port       	= DEF_PORT;
	user_param->ib_port    	= DEF_IB_PORT;
	user_param->ib_port2	= DEF_IB_PORT2;
	user_param->size       	= (user_param->tst == BW ) ? DEF_SIZE_BW : DEF_SIZE_LAT;
	user_param->tx_depth   	= (user_param->tst == BW ) ? DEF_TX_BW : DEF_TX_LAT;
	user_param->qp_timeout 	= DEF_QP_TIME;
	user_param->test_method = RUN_REGULAR;
	user_param->cpu_freq_f 	= OFF;
	user_param->connection_type = (user_param->connection_type == RawEth) ? RawEth : RC;
	user_param->use_event  	= OFF;
	user_param->num_of_qps 	= DEF_NUM_QPS;
	user_param->gid_index  	= DEF_GID_INDEX;
	user_param->gid_index2  = DEF_GID_INDEX;
	user_param->use_gid_user = 0;
	user_param->inline_size = DEF_INLINE;
	user_param->use_mcg     = OFF;
	user_param->use_rdma_cm = OFF;
	user_param->work_rdma_cm = OFF;
	user_param->rx_depth    = user_param->verb == SEND ? DEF_RX_SEND : DEF_RX_RDMA;
	user_param->duplex	= OFF;
	user_param->noPeak	= OFF;
	user_param->cq_mod	= DEF_CQ_MOD;
	user_param->iters = (user_param->tst == BW && user_param->verb == WRITE) ? DEF_ITERS_WB : DEF_ITERS;
	user_param->dualport	= OFF;
	user_param->post_list	= 1;
	user_param->use_srq = OFF;
	user_param->use_xrc = OFF;
	user_param->use_rss = OFF;
	user_param->srq_exists = OFF;
	user_param->duration	= DEF_DURATION;
	user_param->margin	= DEF_INIT_MARGIN;
	user_param->test_type	= ITERATIONS;
	user_param->state	= START_STATE;
	user_param->tos		= DEF_TOS;
	user_param->mac_fwd	= OFF;
	user_param->report_fmt = MBS;
	user_param->report_both = OFF;
	user_param->is_reversed = OFF;
	user_param->is_limit_bw = OFF;
	user_param->limit_bw = 0;
	user_param->is_limit_msgrate = OFF;
	user_param->limit_msgrate = 0;
	user_param->pkey_index    = 0;
	user_param->raw_qos 	  = 0;
	user_param->inline_recv_size = 0;
	user_param->tcp = 0;
	user_param->is_rate_limiting = 0;
	user_param->burst_size = 0;
	user_param->rate_limit = 0;
	user_param->rate_units = MEGA_BYTE_PS;
	user_param->output = -1;
	user_param->use_cuda = 0;
	user_param->iters_per_port[0] = 0;
	user_param->iters_per_port[1] = 0;

	if (user_param->tst == LAT) {
		user_param->r_flag->unsorted  = OFF;
		user_param->r_flag->histogram = OFF;
		user_param->r_flag->cycles    = OFF;
	}

	if (user_param->verb == ATOMIC) {
		user_param->atomicType = FETCH_AND_ADD;
		user_param->size = DEF_SIZE_ATOMIC;
	}

	user_param->cpu_util = 0;
	user_param->cpu_util_data.enable = 0;
	user_param->retry_count = DEF_RETRY_COUNT;
	user_param->dont_xchg_versions = 0;
	user_param->use_exp = 0;
	user_param->ipv6 = 0;
	user_param->report_per_port = 0;
	user_param->use_odp = 0;
	user_param->use_promiscuous = 0;
	user_param->check_alive_exited = 0;
	user_param->raw_mcast = 0;
	user_param->masked_atomics = 0;

	user_param->cache_line_size = get_cache_line_size();

	user_param->cycle_buffer = sysconf(_SC_PAGESIZE);
	if (user_param->cycle_buffer <= 0)
	{
		user_param->cycle_buffer = DEF_PAGE_SIZE;
	}

}

 /******************************************************************************
  *
  ******************************************************************************/
static int ctx_chk_pkey_index(struct ibv_context *context,int pkey_idx) 
{
	int idx = 0;
	struct ibv_device_attr attr;

	if (!ibv_query_device(context,&attr)) {
		if (pkey_idx > attr.max_pkeys - 1) {
			printf(RESULT_LINE);
			fprintf(stderr," Specified PKey Index, %i, greater than allowed max, %i\n",pkey_idx,attr.max_pkeys - 1);
			fprintf(stderr," Changing to 0\n");
			idx = 0;
		} else
			idx = pkey_idx;
	} else {
		fprintf(stderr," Unable to validata PKey Index, changing to 0\n");
		idx = 0;
	}

	return idx;

}


/******************************************************************************
 *
 ******************************************************************************/
static void change_conn_type(int *cptr,VerbType verb,const char *optarg) {

	if (strcmp(connStr[0],optarg)==0)
		*cptr = RC;

	else if (strcmp(connStr[1],optarg)==0) {
		*cptr = UC;
		if (verb == READ || verb == ATOMIC) {
			  fprintf(stderr," UC connection not possible in READ/ATOMIC verbs\n");
			  exit(1);
		}

	} else if (strcmp(connStr[2],optarg)==0)  {
		*cptr = UD;
		if (verb != SEND) {
			fprintf(stderr," UD connection only possible in SEND verb\n");
			exit(1);
		}
	} else if(strcmp(connStr[3],optarg)==0) {
		*cptr = RawEth;

	} else if(strcmp(connStr[4],optarg)==0) {
#ifdef HAVE_XRCD
		*cptr = XRC;
#else
		fprintf(stderr," XRC not detected in libibverbs\n");
		exit(1);
#endif
	} else if (strcmp(connStr[5],optarg)==0) {
#ifdef HAVE_DC
		*cptr = DC;
#else
		fprintf(stderr," DC not detected in libibverbs\n");
		exit(1);
#endif
	} else {
		fprintf(stderr," Invalid Connection type . please choose from {RC,UC,UD}\n");
		exit(1);
	}
}
/******************************************************************************
  *
  ******************************************************************************/
int set_eth_mtu(struct perftest_parameters *user_param) {

	if (user_param->mtu == 0) {
		user_param->mtu = 1518;//1500
	}

	if(user_param->mtu >= MIN_MTU_RAW_ETERNET && user_param->mtu <= MAX_MTU_RAW_ETERNET) {
		user_param->curr_mtu = user_param->mtu;

	} else {

		fprintf(stderr," Invalid MTU - %d \n",user_param->mtu);
		fprintf(stderr," Please choose mtu form {64, 9600}\n");
		return -1;
	}

	return 0;
}
/******************************************************************************
 *
 ******************************************************************************/
static void force_dependecies(struct perftest_parameters *user_param) {

	// Additional configuration and assignments.
	if (user_param->test_type == ITERATIONS) {

		if (user_param->tx_depth > user_param->iters) {
			user_param->tx_depth = user_param->iters;
		}

		if (user_param->verb == SEND && user_param->rx_depth > user_param->iters) {
			user_param->rx_depth = user_param->iters;
		}

		if (user_param->connection_type == UD || user_param->connection_type == UC)
		{
			if (user_param->rx_depth == DEF_RX_SEND)
			{
				user_param->rx_depth = (user_param->iters < UC_MAX_RX) ? user_param->iters : UC_MAX_RX;
			}
		} 
	}

	if (user_param->cq_mod > user_param->tx_depth) {
		user_param->cq_mod = user_param->tx_depth;
	}

	if (user_param->verb == READ || user_param->verb == ATOMIC)
		user_param->inline_size = 0;

	if (user_param->test_method == RUN_ALL)
		user_param->size = MAX_SIZE;

	if (user_param->verb == ATOMIC && user_param->size != DEF_SIZE_ATOMIC) {
		user_param->size = DEF_SIZE_ATOMIC;
	}

	if (user_param->use_srq && user_param->verb != SEND) {
		printf(RESULT_LINE);
		printf(" Using SRQ only avavilible in SEND tests.\n");
		exit (1);
	}

	if (user_param->dualport==ON) {

		user_param->num_of_qps *= 2;
		if (!user_param->tst==BW) {
			printf(" Dual-port mode only supports BW tests.\n");
			exit (1);
		}

		if (user_param->use_mcg){
			printf(" Dual-port mode not supported in multicast feature\n");
			exit (1);
		}
	}

	if (user_param->post_list > 1) {
		user_param->cq_mod = user_param->post_list;
		printf(RESULT_LINE);
		printf("Post List requested - CQ moderation will be the size of the post list\n");

		if (user_param->num_of_qps > 1) {
			user_param->tx_depth = 128;
			printf(RESULT_LINE);
			printf(" Reducing TX depth to 128 to diaphragm time between post sends of each time\n");
		}
	}

	if (user_param->test_type==DURATION) {

		// When working with Duration, iters=0 helps us to satisfy loop cond. in run_iter_bw.
		// We also use it for "global" counter of packets.
		user_param->iters = 0;
		user_param->noPeak = ON;

		if (user_param->use_event) {
			printf(RESULT_LINE);
		    fprintf(stderr,"Duration mode doesn't work with events.\n");
			exit(1);
		}

		if (user_param->test_method == RUN_ALL) {
			printf(RESULT_LINE);
			fprintf(stderr, "Duration mode currently doesn't support running on all sizes.\n");
			exit(1);
		}
		if (user_param->cpu_util) {
			user_param->cpu_util_data.enable = 1;
		}
	}

	if ( (user_param->test_type != DURATION) && user_param->cpu_util ) {
		printf(RESULT_LINE);
		fprintf(stderr, " CPU Utilization works only with Duration mode.\n");
	}

	if (user_param->use_mcg &&  user_param->gid_index == -1) {
			user_param->gid_index = 0;
	}

	if (user_param->work_rdma_cm) {

		if (user_param->connection_type == UC) {
			printf(RESULT_LINE);
			printf(" UC is not supported in librdmacm\n");
			exit(1);
		}

		if (user_param->use_mcg) {
			printf(RESULT_LINE);
			printf(" Perftest still doesn't support Multicast with rdma_cm\n");
			exit(1);
		}

		if (user_param->dualport) {
			printf(RESULT_LINE);
			printf(" Perftest still doesn't support Dual Port with rdma_cm\n");
			exit(1);
		}

		if (user_param->num_of_qps > 1) {
			printf(RESULT_LINE);
			fprintf(stdout," Perftest only supports 1 rmda_cm QP for now\n");
			exit(1);
		}
		user_param->use_rdma_cm = ON;

	} else if (user_param->tos != DEF_TOS && user_param->connection_type != RawEth) {
		fprintf(stdout," TOS only valid for rdma_cm based QP and RawEth QP \n");
		exit(1);
	}

	if (user_param->use_mcg) {

		if (user_param->connection_type != UD)
			user_param->connection_type = UD;

		if (user_param->duplex) {
			fprintf(stdout,"Bidirectional mode not supported in multicast\n");
			exit (1);
		}

		if (user_param->num_of_qps > 1) {
			fprintf(stdout,"Only 1 QP supported in multicast\n");
			exit(1);
		}
	}

	if(user_param->verb == ATOMIC && user_param->use_odp) {
		printf(RESULT_LINE);
		fprintf(stderr," ODP does not support ATOMICS for now\n");
		exit(1);
	}
	if (user_param->connection_type == RawEth) {

		if (user_param->num_of_qps > 1 && !user_param->use_rss) {
			printf(RESULT_LINE);
			fprintf(stdout," Raw Ethernet test supports only 1 QP for now\n");
			exit(1);
		}

		if(user_param->machine == UNCHOSEN) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must choose test side --client or --server\n");
			exit(1);
		}

		if(user_param->machine == CLIENT && user_param->is_dest_mac == OFF && !user_param->raw_mcast) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must enter dest mac by this format -E XX:XX:XX:XX:XX:XX\n");
			exit(1);
		}

		// if((user_param->is_server_ip == ON && user_param->is_client_ip == OFF) || (user_param->is_server_ip == OFF && user_param->is_client_ip == ON)) {
			// printf(RESULT_LINE);
			// fprintf(stderr," Invalid Command line.\n if you would like to send IP header,\n you must enter server&client ip addresses --server_ip X.X.X.X --client_ip X.X.X.X\n");
			// exit(1);
		// }

		if((user_param->is_server_port == ON && user_param->is_client_port == OFF) || (user_param->is_server_port == OFF && user_param->is_client_port == ON)) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n if you would like to send UDP header,\n you must enter server&client port --server_port X --client_port X\n");
			exit(1);
		}

		// Mac forwarding dependencies
		if (user_param->duplex == OFF && user_param->mac_fwd == ON) {
			printf("mac_fwd should run in duplex mode only. changing to duplex mode.\n");
			user_param->duplex = ON;
		}
        if (user_param->mac_fwd == ON && user_param->cq_mod >= user_param->rx_depth) {
            fprintf(stderr," CQ moderation can't be grater than rx depth.\n");
            user_param->cq_mod = user_param->rx_depth < user_param->tx_depth ? user_param->rx_depth : user_param->tx_depth;
            fprintf(stderr," Changing CQ moderation to min( rx depth , tx depth) = %d.\n",user_param->cq_mod);
        }

		if (user_param->raw_mcast && user_param->duplex)
		{
			printf(" Multicast feature works on unidirectional traffic only\n");
			exit(1);
		}

	}

	if (user_param->verb == SEND && user_param->tst == BW && user_param->machine == SERVER && !user_param->duplex )
		user_param->noPeak = ON;

	// Run infinitely dependencies
	if (user_param->test_method == RUN_INFINITELY) {
		user_param->noPeak = ON;
		user_param->test_type = DURATION;
		if (user_param->use_event) {
			printf(RESULT_LINE);
			fprintf(stderr," run_infinitely does not support events feature yet.\n");
			exit(1);
		}

		if (user_param->tst == LAT) {
			printf(RESULT_LINE);
			fprintf(stderr," run_infinitely exists only in BW tests for now.\n");
			exit(1);

		}

		if (user_param->duplex && user_param->verb == SEND) {
			printf(RESULT_LINE);
			fprintf(stderr," run_infinitely not supported in SEND Bidirectional BW test\n");
			exit(1);
		}
	}

	//raw ethernet send latency
	//client and server must enter the destination mac
	if (user_param->connection_type == RawEth && user_param->tst == LAT && user_param->verb == SEND) {

		if (user_param-> is_dest_mac == OFF) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must enter dest mac by this format -E XX:XX:XX:XX:XX:XX\n");
			exit(1);
		}

	}

	if (user_param->connection_type == DC && !user_param->use_srq)
		user_param->use_srq = 1;

	// XRC Part
	if (user_param->connection_type == XRC) {

		if (user_param->work_rdma_cm == ON) {
			printf(RESULT_LINE);
			fprintf(stderr," XRC does not support RDMA_CM\n");
			exit(1);
		}

		user_param->use_xrc = ON;
		user_param->use_srq = ON;
	}

	if(user_param->connection_type == DC) {
		if (user_param->work_rdma_cm == ON) {
			printf(RESULT_LINE);
			fprintf(stderr," DC does not support RDMA_CM\n");
			exit(1);
		}
	}

	#ifndef HAVE_RSS_EXP
	if (user_param->use_rss) {
		printf(RESULT_LINE);
		fprintf(stderr," RSS feature is not available in libibverbs\n");
		exit(1);
	}
	#endif

	if ((user_param->use_srq && (user_param->tst == LAT || user_param->machine == SERVER || user_param->duplex == ON)) || user_param->use_xrc)
		user_param->srq_exists = 1;

	if (user_param->burst_size > 0) {
		if (user_param->is_rate_limiting == 0) {
			printf(RESULT_LINE);
			fprintf(stderr," Can't enable burst mode when rate limiter is off\n");
			exit(1);
		}
	}

	if (user_param->is_rate_limiting == 1) {
		if (user_param->tst != BW || user_param->verb == ATOMIC || (user_param->verb == SEND && user_param->duplex)) {
			printf(RESULT_LINE);
			fprintf(stderr," Rate limiter cann't be executed on non-BW, ATOMIC or bidirectional SEND tests\n");
			exit(1);
		}

		if (user_param->burst_size <= 0) {
			printf(RESULT_LINE);
			fprintf(stderr," Setting burst size to tx depth = %d\n",user_param->tx_depth);
			user_param->burst_size = user_param->tx_depth;
		}
	}

	if (user_param->output != -1) {
		if (user_param->tst == BW && !(user_param->output == OUTPUT_BW || user_param->output == OUTPUT_MR)) {
			printf(RESULT_LINE);
			fprintf(stderr," Output verbosity level for BW can be: bandwidth, message_rate\n");
			exit(1);
		}

		if (user_param->tst == LAT && !(user_param->output == OUTPUT_LAT)) {
			printf(RESULT_LINE);
			fprintf(stderr," Output verbosity level for LAT can be: latency\n");
			exit(1);
		}
	}

	if ( (user_param->latency_gap > 0) && user_param->tst != LAT ) {
		printf(RESULT_LINE);
		fprintf(stderr," Latency gap feature is only for latency tests\n");
		exit(1);
	}

	if ( user_param->test_type == DURATION && user_param->margin == DEF_INIT_MARGIN) {
		user_param->margin = user_param->duration / 4;
	}

	#if defined(HAVE_VERBS_EXP) && defined(HAVE_DC)
	if (user_param->connection_type == DC) {
		user_param->use_exp = 1;
	}
	#endif

	#ifdef HAVE_CUDA
	if (user_param->use_cuda) {
		if (user_param->tst != BW) {
			printf(RESULT_LINE);
			fprintf(stderr," Perftest supports CUDA only in BW tests\n");
			exit(1);
		}
	}
	#endif

	if ( (user_param->connection_type == UD) && (user_param->inline_size > MAX_INLINE_UD) ) {
		printf(RESULT_LINE);
		fprintf(stderr,"Setting inline size to %d (Max inline size in UD)\n",MAX_INLINE_UD);
		user_param->inline_size = MAX_INLINE_UD;
	}

	if (user_param->report_per_port && (user_param->test_type != DURATION || !user_param->dualport))
	{
		printf(RESULT_LINE);
		fprintf(stderr,"report per port feature work only with Duration and Dualport\n");
		exit(1);
	}

	return;
}

/******************************************************************************
 *
 ******************************************************************************/
const char *transport_str(enum ibv_transport_type type) {

	switch (type) {
		case IBV_TRANSPORT_IB:
			return "IB";
			break;
		case IBV_TRANSPORT_IWARP:
			return "IW";
			break;
		default:
			return "Unknown";
	}
}

/******************************************************************************
 *
 ******************************************************************************/
const char *link_layer_str(uint8_t link_layer) {

	switch (link_layer) {

        case IBV_LINK_LAYER_UNSPECIFIED:
        case IBV_LINK_LAYER_INFINIBAND:
			return "IB";
        case IBV_LINK_LAYER_ETHERNET:
			return "Ethernet";
		#ifdef HAVE_SCIF
		case IBV_LINK_LAYER_SCIF:
			return "SCIF";
		#endif
        default:
		    return "Unknown";
    }
}

/******************************************************************************
 *
 ******************************************************************************/
enum ctx_device ib_dev_name(struct ibv_context *context) {

	enum ctx_device dev_fname = UNKNOWN;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		dev_fname = DEVICE_ERROR;
	}

	else if (attr.vendor_id == 5157) {

		switch (attr.vendor_part_id >> 12) {
			case 10 :
			case 4  : dev_fname = CHELSIO_T4; break;
			case 11 :
			case 5  : dev_fname = CHELSIO_T5; break;
			default : dev_fname = UNKNOWN; break;
		}

	// Assuming it's Mellanox HCA or unknown.
	// If you want Inline support in other vendor devices, please send patch to idos@dev.mellanox.co.il
	} else {

		switch (attr.vendor_part_id) {
			case 4113  : dev_fname = CONNECTIB; break;
			case 4099  : dev_fname = CONNECTX3; break;
			case 4100  : dev_fname = CONNECTX3; break;
			case 4103  : dev_fname = CONNECTX3_PRO; break;
			case 4104  : dev_fname = CONNECTX3_PRO; break;
			case 26418 : dev_fname = CONNECTX2; break;
			case 26428 : dev_fname = CONNECTX2; break;
			case 26438 : dev_fname = CONNECTX2; break;
			case 26448 : dev_fname = CONNECTX2; break;
			case 26458 : dev_fname = CONNECTX2; break;
			case 26468 : dev_fname = CONNECTX2; break;
			case 26478 : dev_fname = CONNECTX2; break;
			case 25408 : dev_fname = CONNECTX;  break;
			case 25418 : dev_fname = CONNECTX;  break;
			case 25428 : dev_fname = CONNECTX;  break;
			case 25448 : dev_fname = CONNECTX;  break;
			case 1824  : dev_fname = SKYHAWK;  break;
			default	   : dev_fname = UNKNOWN;
		}
	}

	return dev_fname;
}

/******************************************************************************
 *
 ******************************************************************************/
enum ibv_mtu set_mtu(struct ibv_context *context,uint8_t ib_port,int user_mtu) {

	struct ibv_port_attr port_attr;
	enum ibv_mtu curr_mtu;

	ibv_query_port(context,ib_port,&port_attr);

	// User did not ask for specific mtu.
	if (user_mtu == 0) {
		enum ctx_device current_dev = ib_dev_name(context);
		curr_mtu = port_attr.active_mtu;
		//CX3_PRO and CX3 have a HW bug in 4K MTU, so we're forcing it to be 2K MTU
		if (curr_mtu == IBV_MTU_4096 && (current_dev == CONNECTX3_PRO || current_dev == CONNECTX3))
			curr_mtu = IBV_MTU_2048;
	}

	else {

		switch (user_mtu) {

			case 256  :	curr_mtu = IBV_MTU_256;	 break;
			case 512  : curr_mtu = IBV_MTU_512;	 break;
			case 1024 :	curr_mtu = IBV_MTU_1024; break;
			case 2048 :	curr_mtu = IBV_MTU_2048; break;
			case 4096 :	curr_mtu = IBV_MTU_4096; break;
			default   :
				fprintf(stderr," Invalid MTU - %d \n",user_mtu);
				fprintf(stderr," Please choose mtu from {256,512,1024,2048,4096}\n");
				fprintf(stderr," Will run with the port active mtu - %d\n",port_attr.active_mtu);
				curr_mtu = port_attr.active_mtu;
		}

		if (curr_mtu > port_attr.active_mtu) {
			fprintf(stdout,"Requested mtu is higher than active mtu \n");
			fprintf(stdout,"Changing to active mtu - %d\n",port_attr.active_mtu);
			curr_mtu = port_attr.active_mtu;
		}
	}
	return curr_mtu;
}

/******************************************************************************
 *
 ******************************************************************************/
static uint8_t set_link_layer(struct ibv_context *context,uint8_t ib_port) {

	struct ibv_port_attr port_attr;
	uint8_t curr_link;

	if (ibv_query_port(context,ib_port,&port_attr)) {
		fprintf(stderr," Unable to query port attributes\n");
		return LINK_FAILURE;
	}

	if (port_attr.state != IBV_PORT_ACTIVE) {
		fprintf(stderr," Port number %d state is %s\n"
					  ,ib_port
					  ,portStates[port_attr.state]);
		return LINK_FAILURE;
	}

	curr_link = port_attr.link_layer;
	if (!strcmp(link_layer_str(curr_link),"Unknown")) {
			fprintf(stderr," Unable to determine link layer \n");
			return LINK_FAILURE;
	}
	//return curr_link;
	//printf("link_layer: %d\n",port_attr.link_layer);
	return port_attr.link_layer;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ctx_set_out_reads(struct ibv_context *context,int num_user_reads) {


	int max_reads = 0;
	struct ibv_device_attr attr;

	if (!ibv_query_device(context,&attr)) {
		max_reads = attr.max_qp_rd_atom;
	}

	if (num_user_reads > max_reads) {
		printf(RESULT_LINE);
		fprintf(stderr," Number of outstanding reads is above max = %d\n",max_reads);
		fprintf(stderr," Changing to that max value\n");
		num_user_reads = max_reads;
	}
	else if (num_user_reads <= 0) {
		num_user_reads = max_reads;
	}

	return num_user_reads;
}

/******************************************************************************
 *
 ******************************************************************************/
static void ctx_set_max_inline(struct ibv_context *context,struct perftest_parameters *user_param) {


	enum ctx_device current_dev = ib_dev_name(context);

	if (current_dev == UNKNOWN || current_dev == DEVICE_ERROR) {

		if (user_param->inline_size != DEF_INLINE) {
			printf(RESULT_LINE);
			fprintf(stderr,"Device not recognized to implement inline feature. Disabling it\n");
		}
		user_param->inline_size = 0;
		return;
	}

	if (user_param->inline_size == DEF_INLINE) {

		if (user_param->tst ==LAT) {

			switch(user_param->verb) {

				case WRITE: user_param->inline_size = (user_param->connection_type == DC)? DEF_INLINE_DC : DEF_INLINE_WRITE; break;
				case SEND : user_param->inline_size = (user_param->connection_type == DC)? DEF_INLINE_DC : (user_param->connection_type == UD)? DEF_INLINE_SEND_UD :
					((user_param->connection_type == XRC) ? DEF_INLINE_SEND_XRC : DEF_INLINE_SEND_RC_UC) ; break;
				default   : user_param->inline_size = 0;
			}

		} else {
			user_param->inline_size = 0;
		}
	}

	return;
}
/******************************************************************************
 *
 ******************************************************************************/
int parser(struct perftest_parameters *user_param,char *argv[], int argc) {

	int c,size_len;
	int size_factor = 1;
	static int run_inf_flag = 0;
	static int report_fmt_flag = 0;
	static int srq_flag = 0;
	static int report_both_flag = 0;
	static int is_reversed_flag = 0;
	static int pkey_flag = 0;
	static int inline_recv_flag = 0;
	static int tcp_flag = 0;
	static int burst_size_flag = 0;
	static int rate_limit_flag = 0;
	static int rate_units_flag = 0;
	static int verbosity_output_flag = 0;
	static int cpu_util_flag = 0;
	static int latency_gap_flag = 0;
	static int retry_count_flag = 0;
	static int dont_xchg_versions_flag = 0;
	static int use_exp_flag = 0;
	static int use_cuda_flag = 0;
	static int ipv6_flag = 0;
	static int report_per_port_flag = 0;
	static int odp_flag = 0;
	static int use_promiscuous_flag = 0;
	static int raw_mcast_flag = 0;

	init_perftest_params(user_param);

	if(user_param->connection_type == RawEth)
		user_param->machine = UNCHOSEN;

	while (1) {
		static const struct option long_options[] = {
			{ .name = "port",           .has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",         .has_arg = 1, .val = 'd' },
			{ .name = "ib-port",        .has_arg = 1, .val = 'i' },
			{ .name = "mtu",            .has_arg = 1, .val = 'm' },
			{ .name = "size",           .has_arg = 1, .val = 's' },
			{ .name = "iters",          .has_arg = 1, .val = 'n' },
			{ .name = "tx-depth",       .has_arg = 1, .val = 't' },
			{ .name = "qp-timeout",     .has_arg = 1, .val = 'u' },
			{ .name = "sl",             .has_arg = 1, .val = 'S' },
			{ .name = "gid-index",      .has_arg = 1, .val = 'x' },
			{ .name = "all",            .has_arg = 0, .val = 'a' },
			{ .name = "CPU-freq",       .has_arg = 0, .val = 'F' },
			{ .name = "connection",     .has_arg = 1, .val = 'c' },
			{ .name = "qp",             .has_arg = 1, .val = 'q' },
			{ .name = "events",         .has_arg = 0, .val = 'e' },
			{ .name = "inline_size",    .has_arg = 1, .val = 'I' },
			{ .name = "outs",           .has_arg = 1, .val = 'o' },
			{ .name = "mcg",            .has_arg = 0, .val = 'g' },
			{ .name = "comm_rdma_cm",   .has_arg = 0, .val = 'z' },
			{ .name = "rdma_cm",   	    .has_arg = 0, .val = 'R' },
			{ .name = "tos",            .has_arg = 1, .val = 'T' },
			{ .name = "help",           .has_arg = 0, .val = 'h' },
			{ .name = "MGID",           .has_arg = 1, .val = 'M' },
			{ .name = "rx-depth",       .has_arg = 1, .val = 'r' },
			{ .name = "bidirectional",  .has_arg = 0, .val = 'b' },
			{ .name = "cq-mod",  	    .has_arg = 1, .val = 'Q' },
			{ .name = "noPeak",         .has_arg = 0, .val = 'N' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "report-cycles",  .has_arg = 0, .val = 'C' },
			{ .name = "report-histogrm",.has_arg = 0, .val = 'H' },
			{ .name = "report-unsorted",.has_arg = 0, .val = 'U' },
			{ .name = "atomic_type",    .has_arg = 1, .val = 'A' },
			{ .name = "dualport",       .has_arg = 0, .val = 'O' },
			{ .name = "post_list",      .has_arg = 1, .val = 'l' },
			{ .name = "duration",       .has_arg = 1, .val = 'D' },
			{ .name = "margin",         .has_arg = 1, .val = 'f' },
			{ .name = "source_mac",     .has_arg = 1, .val = 'B' },
			{ .name = "dest_mac",       .has_arg = 1, .val = 'E' },
			{ .name = "dest_ip",        .has_arg = 1, .val = 'J' },
			{ .name = "source_ip",      .has_arg = 1, .val = 'j' },
			{ .name = "dest_port", 		.has_arg = 1, .val = 'K' },
			{ .name = "source_port",    .has_arg = 1, .val = 'k' },
			{ .name = "limit_bw",       .has_arg = 1, .val = 'w' },
			{ .name = "limit_msgrate",  .has_arg = 1, .val = 'y' },
			{ .name = "server",         .has_arg = 0, .val = 'Z' },
			{ .name = "client",         .has_arg = 0, .val = 'P' },
			{ .name = "mac_fwd",        .has_arg = 0, .val = 'v' },
			{ .name = "use_rss",        .has_arg = 0, .val = 'G' },
			{ .name = "run_infinitely", .has_arg = 0, .flag = &run_inf_flag, .val = 1 },
			{ .name = "report_gbits",   .has_arg = 0, .flag = &report_fmt_flag, .val = 1},
			{ .name = "use-srq",        .has_arg = 0, .flag = &srq_flag, .val = 1},
			{ .name = "report-both",    .has_arg = 0, .flag = &report_both_flag, .val = 1},
			{ .name = "reversed",       .has_arg = 0, .flag = &is_reversed_flag, .val = 1},
			{ .name = "pkey_index",     .has_arg = 1, .flag = &pkey_flag, .val = 1},
			{ .name = "inline_recv",    .has_arg = 1, .flag = &inline_recv_flag, .val = 1},
			{ .name = "tcp",		.has_arg = 0, .flag = &tcp_flag, .val = 1},
			{ .name = "burst_size",		.has_arg = 1, .flag = &burst_size_flag, .val = 1},
			{ .name = "rate_limit",		.has_arg = 1, .flag = &rate_limit_flag, .val = 1},
			{ .name = "rate_units",		.has_arg = 1, .flag = &rate_units_flag, .val = 1},
			{ .name = "output",		.has_arg = 1, .flag = &verbosity_output_flag, .val = 1},
			{ .name = "cpu_util",           .has_arg = 0, .flag = &cpu_util_flag, .val = 1},
			{ .name = "latency_gap",        .has_arg = 1, .flag = &latency_gap_flag, .val = 1},
			{ .name = "retry_count",        .has_arg = 1, .flag = &retry_count_flag, .val = 1},
			{ .name = "dont_xchg_versions",        .has_arg = 0, .flag = &dont_xchg_versions_flag, .val = 1},
			#ifdef HAVE_VERBS_EXP
			{ .name = "use_exp",           .has_arg = 0, .flag = &use_exp_flag, .val = 1},
			#endif
			{ .name = "use_cuda",		.has_arg = 0, .flag = &use_cuda_flag, .val = 1},
			{ .name = "ipv6",		.has_arg = 0, .flag = &ipv6_flag, .val = 1},
			{ .name = "report-per-port",		.has_arg = 0, .flag = &report_per_port_flag, .val = 1},
			{ .name = "odp",		.has_arg = 0, .flag = &odp_flag, .val = 1},
			{ .name = "promiscuous",		.has_arg = 0, .flag = &use_promiscuous_flag, .val = 1},
			{ .name = "raw_mcast",		.has_arg = 0, .flag = &raw_mcast_flag, .val = 1},
            { 0 }
        };
        c = getopt_long(argc,argv,"w:y:p:d:i:m:s:n:t:u:S:x:c:q:I:o:M:r:Q:A:l:D:f:B:T:E:J:j:K:k:aFegzRvhbNVCHUOZP",long_options,NULL);

        if (c == -1)
			break;

        switch (c) {

			case 'p': user_param->port = strtol(optarg, NULL, 0); break;
			case 'd': GET_STRING(user_param->ib_devname,strdupa(optarg)); break;
			case 'i': CHECK_VALUE(user_param->ib_port,uint8_t,MIN_IB_PORT,MAX_IB_PORT,"IB Port"); break;
			case 'm': user_param->mtu  = strtol(optarg, NULL, 0); break;
			case 'n': CHECK_VALUE(user_param->iters,int,MIN_ITER,MAX_ITER,"Iteration num"); break;
			case 't': CHECK_VALUE(user_param->tx_depth,int,MIN_TX,MAX_TX,"Tx depth"); break;
			case 'T': CHECK_VALUE(user_param->tos,int,MIN_TOS,MAX_TOS,"TOS"); break;
			case 'u': user_param->qp_timeout = (uint8_t)strtol(optarg, NULL, 0); break;
			case 'S': user_param->sl = (uint8_t)strtol(optarg, NULL, 0);
				if (user_param->sl > MAX_SL) {
					fprintf(stderr," Only %d Service levels\n",MAX_SL);
					return 1;
				}
				if (user_param->connection_type == RawEth)
					user_param->raw_qos = 1;
				break;
			case 'x': CHECK_VALUE(user_param->gid_index,uint8_t,MIN_GID_IX,MAX_GID_IX,"Gid index"); 
				  user_param->use_gid_user = 1; break;
			case 'c': change_conn_type(&user_param->connection_type,user_param->verb,optarg); break;
			case 'q':
				if (user_param->tst != BW) {
					fprintf(stderr," Multiple QPs only available on bw tests\n");
					return 1;
				}
				CHECK_VALUE(user_param->num_of_qps,int,MIN_QP_NUM,MAX_QP_NUM,"num of Qps");
				break;
			case 'I': CHECK_VALUE(user_param->inline_size,int,0,MAX_INLINE,"Max inline");
				if (user_param->verb == READ || user_param->verb ==ATOMIC) {
					fprintf(stderr," Inline feature not available on READ/Atomic verbs\n");
					return 1;
				} break;
			case 'o': user_param->out_reads = strtol(optarg, NULL, 0);
				if (user_param->verb != READ && user_param->verb != ATOMIC) {
					fprintf(stderr," Setting Outstanding reads only available on READ verb\n");
					return 1;
				} break;
			case 'M': GET_STRING(user_param->user_mgid,strdupa(optarg)); break;
			case 'r': CHECK_VALUE(user_param->rx_depth,int,MIN_RX,MAX_RX," Rx depth");
				if (user_param->verb != SEND && user_param->rx_depth > DEF_RX_RDMA) {
					fprintf(stderr," On RDMA verbs rx depth can be only 1\n");
					return 1;
				} break;
			case 'Q': CHECK_VALUE(user_param->cq_mod,int,MIN_CQ_MOD,MAX_CQ_MOD,"CQ moderation"); break;
			case 'A':
				if (user_param->verb != ATOMIC) {
					fprintf(stderr," You are not running the atomic_lat/bw test!\n");
					fprintf(stderr," To change the atomic action type, you must run one of the atomic tests\n");
					return 1;
				}

				if (strcmp(atomicTypesStr[0],optarg)==0)
					user_param->atomicType = CMP_AND_SWAP;

				else if (strcmp(atomicTypesStr[1],optarg)==0)
					user_param->atomicType = FETCH_AND_ADD;

				else {
					fprintf(stderr," Invalid Atomic type! please choose from {CMP_AND_SWAP,FETCH_AND_ADD}\n");
					exit(1);
				}
				break;
			case 'l': user_param->post_list = strtol(optarg, NULL, 0); break;
			case 'D': user_param->duration = strtol(optarg, NULL, 0);
				if (user_param->duration <= 0) {
						fprintf(stderr," Duration period must be greater than 0\n");
						return 1;
				}
				user_param->test_type = DURATION;
				break;
			case 'f': user_param->margin = strtol(optarg, NULL, 0);
 				if (user_param->margin < 0) {
					fprintf(stderr," margin must be positive.\n");
					return 1;
				} break;
			case 'O':
				user_param->ib_port  = DEF_IB_PORT;
				user_param->ib_port2 = DEF_IB_PORT2;
				user_param->dualport = ON;
				break;
			case 'a': user_param->test_method = RUN_ALL; break;
			case 'F': user_param->cpu_freq_f = ON; break;
			case 'V': printf("Version: %s\n",user_param->version); return VERSION_EXIT;
			case 'h': usage(argv[0],user_param->verb,user_param->tst);
					  if(user_param->connection_type == RawEth)
					  {
						 usage_raw_ethernet();
					  }
					  return HELP_EXIT;
			case 'z': user_param->use_rdma_cm = ON; break;
			case 'R': user_param->work_rdma_cm = ON; break;
			case 's': size_len = (int)strlen(optarg);
				  if (optarg[size_len-1] == 'K') {
				  	optarg[size_len-1] = '\0';
					size_factor = 1024;
				  }
				  if (optarg[size_len-1] == 'M') {
				  	optarg[size_len-1] = '\0';
					size_factor = 1024*1024;
				  }
				  user_param->size = (uint64_t)strtol(optarg, NULL, 0) * size_factor;
				  if (user_param->size < 1 || user_param->size > (UINT_MAX / 2)) {
				  	fprintf(stderr," Message Size should be between %d and %d\n",1,UINT_MAX/2);
					return 1;
				  }	
				  break;
			case 'e': user_param->use_event = ON;
				if (user_param->verb == WRITE) {
					fprintf(stderr," Events feature not available on WRITE verb\n");
					return 1;
				} break;
			case 'b': user_param->duplex = ON;
				if (user_param->tst == LAT) {
					fprintf(stderr," Bidirectional is only available in BW test\n");
					return 1;
				} break;
			case 'N': user_param->noPeak = ON;
				if (user_param->tst == LAT) {
					fprintf(stderr," NoPeak only valid for BW tests\n");
					return 1;
				} break;
			case 'C':
				if (user_param->tst != LAT) {
					fprintf(stderr," Availible only on Latency tests\n");
					return 1;
				}
				user_param->r_flag->cycles = ON;
				break;
			case 'g': user_param->use_mcg = ON;
				if (user_param->verb != SEND) {
					fprintf(stderr," MultiCast feature only available on SEND verb\n");
					return 1;
				} break;
			case 'H':
				if (user_param->tst != LAT) {
					fprintf(stderr," Availible only on Latency tests\n");
					return 1;
				}
				user_param->r_flag->histogram = ON;
				break;
				case 'U':
				if (user_param->tst != LAT) {
					fprintf(stderr," Availible only on Latency tests\n");
					return 1;
				}
				user_param->r_flag->unsorted = ON;
				break;
			case 'B':
				user_param->is_source_mac = ON;
				if(parse_mac_from_str(optarg, user_param->source_mac))
					return FAILURE;
				break;
			case 'E':
				user_param->is_dest_mac = ON;
				if(parse_mac_from_str(optarg, user_param->dest_mac))
					return FAILURE;
				break;
			case 'J':
				user_param->is_server_ip = ON;
				if(1 != parse_ip_from_str(optarg, &(user_param->server_ip)))
				{
					fprintf(stderr," Invalid server IP address\n");
					return FAILURE;
				}
				break;
			case 'j':
				user_param->is_client_ip = ON;
				if(1 != parse_ip_from_str(optarg, &(user_param->client_ip)))
				{
					fprintf(stderr," Invalid client IP address\n");
					return FAILURE;
				}
				break;
			case 'K':
				user_param->is_server_port = ON;
				user_param->server_port = strtol(optarg, NULL, 0);
				if(OFF == check_if_valid_udp_port(user_param->server_port))
				{
					fprintf(stderr," Invalid server UDP port\n");
					return FAILURE;
				}
				break;
			case 'k':
				user_param->is_client_port = ON;
				user_param->client_port = strtol(optarg, NULL, 0);
				if(OFF == check_if_valid_udp_port(user_param->client_port))
				{
					fprintf(stderr," Invalid client UDP port\n");
					return FAILURE;
				}
				break;
			case 'w':
				user_param->is_limit_bw = ON;
				user_param->limit_bw = strtof(optarg,NULL);
				if (user_param->limit_bw < 0) {

					fprintf(stderr, " Invalid Minimum BW Limit\n");
					return FAILURE;
				}
				break;
			case 'y':
				user_param->is_limit_msgrate = ON;
				user_param->limit_msgrate = strtof(optarg,NULL);
				if (user_param->limit_msgrate < 0) {

					fprintf(stderr, " Invalid Minimum msgRate Limit\n");
					return FAILURE;
				}
				break;
			case 'P': user_param->machine = CLIENT; break;
			case 'Z': user_param->machine = SERVER; break;
			case 'v': user_param->mac_fwd = ON; break;
			case 'G': user_param->use_rss = ON; break;
			case 0: // required for long options to work.
				if (pkey_flag) {
					user_param->pkey_index = strtol(optarg,NULL,0);
					pkey_flag = 0;
				}
				if (inline_recv_flag) {
					user_param->inline_recv_size = strtol(optarg,NULL,0);
					inline_recv_flag = 0;
				}
				if (rate_limit_flag) {
					user_param->is_rate_limiting = 1;
					user_param->rate_limit = strtol(optarg,NULL,0);
					if (user_param->rate_limit < 0) {
						fprintf(stderr, " Rate limit must be non-negative\n");
						return FAILURE;
					}
					rate_limit_flag = 0;
				}
				if (burst_size_flag) {
					user_param->burst_size = strtol(optarg,NULL,0);
					if (user_param->burst_size < 0) {
						fprintf(stderr, " Burst size must be non-negative\n");
						return FAILURE;
					}
					burst_size_flag = 0;
				}
				if (rate_units_flag) {
					if (strcmp("M",optarg) == 0) {
						user_param->rate_units = MEGA_BYTE_PS;
					} else if (strcmp("g",optarg) == 0) {
						user_param->rate_units = GIGA_BIT_PS;
					} else if (strcmp("p",optarg) == 0) {
						user_param->rate_units = PACKET_PS;
					} else {
						fprintf(stderr, " Invalid rate limit units. Please use M,g or p\n");
						return FAILURE;
					}
					rate_units_flag = 0;
				}
				if (verbosity_output_flag) {
					if (strcmp("bandwidth",optarg) == 0) {
						user_param->output = OUTPUT_BW;
					} else if (strcmp("message_rate",optarg) == 0) {
						user_param->output = OUTPUT_MR;
					} else if (strcmp("latency",optarg) == 0) {
						user_param->output = OUTPUT_LAT;
					} else {
						fprintf(stderr, " Invalid verbosity level output flag. Please use bandwidth, latency, message_rate\n");
						return FAILURE;
					}
					verbosity_output_flag = 0;
				}
				if (latency_gap_flag) {
					user_param->latency_gap = strtol(optarg,NULL,0);
					if (user_param->latency_gap < 0) {
                                                fprintf(stderr, " Latency gap time must be non-negative\n");
                                                return FAILURE;
                                        }
                                        latency_gap_flag = 0;
                                }
				if (retry_count_flag) {
					user_param->retry_count = strtol(optarg,NULL,0);
					if (user_param->retry_count < 0) {
						fprintf(stderr, " Retry Count value must be positive\n");
						return FAILURE;
					}
					retry_count_flag = 0;
				}
				break;

			default:
				fprintf(stderr," Invalid Command or flag.\n");
				fprintf(stderr," Please check command line and run again.\n\n");
				usage(argv[0],user_param->verb,user_param->tst);
				if(user_param->connection_type == RawEth) {
					usage_raw_ethernet();
				}
				return 1;
		 }
	}

	if (tcp_flag) {
		user_param->tcp = 1;
	}
	if (run_inf_flag) {
		user_param->test_method = RUN_INFINITELY;
	}

	if (srq_flag) {
		user_param->use_srq = 1;
	}

	if (report_fmt_flag) {
		user_param->report_fmt = GBS;
	}

	if (dont_xchg_versions_flag) {
		user_param->dont_xchg_versions = 1;
	}

	if (use_exp_flag) {
		user_param->use_exp = 1;
	}

	if (use_cuda_flag) {
		user_param->use_cuda = 1;
	}
	if (report_both_flag) {
		user_param->report_both = 1;
	}
	
	if (is_reversed_flag) {
		user_param->is_reversed = 1;
	}

	if (cpu_util_flag) {
                user_param->cpu_util = 1;
	}

	if (report_per_port_flag) {
		user_param->report_per_port = 1;
	}

	if (ipv6_flag) {
                user_param->ipv6 = 1;
	}

	if(odp_flag) {
		user_param->use_odp = 1;
	}

	if (use_promiscuous_flag) {
		user_param->use_promiscuous = 1;
	}

	if (raw_mcast_flag) {
		user_param->raw_mcast = 1;
	}
	if (optind == argc - 1) {
		GET_STRING(user_param->servername,strdupa(argv[optind]));

	} else if (optind < argc) {
		fprintf(stderr," Invalid Command line. Please check command rerun \n");
		return 1;
	}

	if(user_param->connection_type != RawEth)
		user_param->machine = user_param->servername ? CLIENT : SERVER;

	//fan-in addition
	if (user_param->is_reversed) {
		if (user_param->machine == SERVER)
			user_param->machine = CLIENT;
		else
			user_param->machine = SERVER;
	}

	force_dependecies(user_param);
    return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int check_link_and_mtu(struct ibv_context *context,struct perftest_parameters *user_param) {

	user_param->transport_type = context->device->transport_type;
	user_param->link_type = set_link_layer(context,user_param->ib_port);

	if (user_param->link_type == LINK_FAILURE) {
		fprintf(stderr, " Couldn't set the link layer\n");
		return FAILURE;
	}

	if (user_param->link_type == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index == -1) {
			user_param->gid_index = 0;
	}

	if (user_param->connection_type == RawEth) {

		if (user_param->link_type != IBV_LINK_LAYER_ETHERNET) {
			fprintf(stderr, " Raw Etherent test can only run on Ethernet link! exiting ...\n");
			return FAILURE;
		}

		if (set_eth_mtu(user_param) != 0 ) {
			fprintf(stderr, " Couldn't set Eth MTU\n");
			return FAILURE;
		}
	} else {
		user_param->curr_mtu = set_mtu(context,user_param->ib_port,user_param->mtu);
	}

	// in case of dual-port mode
	if (user_param->dualport==ON) {

		user_param->link_type2 = set_link_layer(context,user_param->ib_port2);
		if (user_param->link_type2 == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index2 == -1) {
			user_param->gid_index2 = 1;
		}
		if (user_param->link_type2 == LINK_FAILURE) {
			fprintf(stderr, " Couldn't set the link layer\n");
			return FAILURE;
		}
	}

	// Compute Max inline size with pre found statistics values
	ctx_set_max_inline(context,user_param);

	if (user_param->verb == READ || user_param->verb == ATOMIC)
		user_param->out_reads = ctx_set_out_reads(context,user_param->out_reads);
	else
		user_param->out_reads = 1;

	if (user_param->connection_type == UD && user_param->size > MTU_SIZE(user_param->curr_mtu)) {

		if (user_param->test_method == RUN_ALL) {
			fprintf(stderr," Max msg size in UD is MTU %lu\n",MTU_SIZE(user_param->curr_mtu));
			fprintf(stderr," Changing to this MTU\n");
		}
		user_param->size = MTU_SIZE(user_param->curr_mtu);
	}
	//checking msg size in raw ethernet
	if (user_param->connection_type == RawEth){
		if (user_param->size > user_param->curr_mtu) {
			fprintf(stderr," Max msg size in RawEth is MTU %d\n",user_param->curr_mtu);
			fprintf(stderr," Changing msg size to this MTU\n");
			user_param->size = user_param->curr_mtu;
		} else if (user_param->size < RAWETH_MIN_MSG_SIZE) {
			printf(" Min msg size for RawEth is 64B - changing msg size to 64 \n");
			user_param->size = RAWETH_MIN_MSG_SIZE;
		}
	}
	if (!user_param->ib_devname)
		GET_STRING(user_param->ib_devname,ibv_get_device_name(context->device))

	if (user_param->pkey_index > 0)
		user_param->pkey_index = ctx_chk_pkey_index(context, user_param->pkey_index);

	return SUCCESS;
}


/******************************************************************************
 *
 ******************************************************************************/
int check_link(struct ibv_context *context,struct perftest_parameters *user_param) {

	user_param->transport_type = context->device->transport_type;
	user_param->link_type = set_link_layer(context,user_param->ib_port);

	if (user_param->link_type == LINK_FAILURE) {
		fprintf(stderr, " Couldn't set the link layer\n");
		return FAILURE;
	}

	if (user_param->link_type == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index == -1) {
			user_param->gid_index = 0;
	}

	if (user_param->connection_type == RawEth) {

		if (user_param->link_type != IBV_LINK_LAYER_ETHERNET) {
			fprintf(stderr, " Raw Etherent test can only run on Ethernet link! exiting ...\n");
			return FAILURE;
		}
	}

	// in case of dual-port mode
	if (user_param->dualport==ON) {

		user_param->link_type2 = set_link_layer(context,user_param->ib_port2);
		if (user_param->link_type2 == IBV_LINK_LAYER_ETHERNET &&  user_param->gid_index2 == -1) {
			user_param->gid_index2 = 1;
		}
		if (user_param->link_type2 == LINK_FAILURE) {
			fprintf(stderr, " Couldn't set the link layer\n");
			return FAILURE;
		}
	}

	// Compute Max inline size with pre found statistics values
	ctx_set_max_inline(context,user_param);

	if (user_param->verb == READ || user_param->verb == ATOMIC)
		user_param->out_reads = ctx_set_out_reads(context,user_param->out_reads);
	else
		user_param->out_reads = 1;


	if (!user_param->ib_devname)
		GET_STRING(user_param->ib_devname,ibv_get_device_name(context->device))

	if (user_param->pkey_index > 0)
		user_param->pkey_index = ctx_chk_pkey_index(context, user_param->pkey_index);

	return SUCCESS;
}


/******************************************************************************
 *
 ******************************************************************************/
void ctx_print_test_info(struct perftest_parameters *user_param) {

	int temp = 0;

	if (user_param->output != FULL_VERBOSITY)
		return;

	printf(RESULT_LINE);
	printf("                    ");
	printf("%s ",testsStr[user_param->verb]);

	if (user_param->verb == ATOMIC) {
		printf("%s ",atomicTypesStr[user_param->atomicType]);
	}

	if (user_param->tst == BW) {

		if (user_param->duplex) {
			printf("Bidirectional ");
		}

		if (user_param->post_list > 1) {
			printf("Post List ");
		}

		printf("BW ");

	} else if (user_param->tst == LAT) {
		printf("Latency ");
	}

	if (user_param->mac_fwd) {
		printf("forwarding ");
	}

	if (user_param->use_mcg)
		printf("Multicast ");

	printf("Test\n");

	if (user_param->use_event) {
		printf(" Test with events.\n");

	}

	if (user_param->use_mcg)
		printf(" MultiCast runs on UD!\n");

	printf(" Dual-port       : %s\t\tDevice         : %s\n", user_param->dualport ? "ON" : "OFF",user_param->ib_devname);
	printf(" Number of qps   : %d\t\tTransport type : %s\n", user_param->num_of_qps, transport_str(user_param->transport_type));
	printf(" Connection type : %s\t\tUsing SRQ      : %s\n", connStr[user_param->connection_type], user_param->use_srq ? "ON"  : "OFF");

	if (user_param->machine == CLIENT || user_param->duplex) {
		printf(" TX depth        : %d\n",user_param->tx_depth);
	}

	if (user_param->post_list > 1)
		printf(" Post List       : %d\n",user_param->post_list);

	if (user_param->verb == SEND && (user_param->machine == SERVER || user_param->duplex)) {
		printf(" RX depth        : %d\n",user_param->rx_depth);
	}

	if (user_param->tst == BW) {
		printf(" CQ Moderation   : %d\n",user_param->cq_mod);
	}

	printf(" Mtu             : %lu[B]\n",user_param->connection_type == RawEth ? user_param->curr_mtu : MTU_SIZE(user_param->curr_mtu));
	printf(" Link type       : %s\n" ,link_layer_str(user_param->link_type));

    //we use the receive buffer only for mac forwarding.
    if (user_param->mac_fwd == ON)
    	printf(" Buffer size     : %d[B]\n" ,user_param->buff_size/2);

	if (user_param->gid_index != DEF_GID_INDEX)
		printf(" Gid index       : %d\n" ,user_param->gid_index);
	if ((user_param->dualport==ON) && (user_param->gid_index2 != DEF_GID_INDEX))
		printf(" Gid index2      : %d\n" ,user_param->gid_index2);

	if (user_param->verb != READ && user_param->verb != ATOMIC)
		printf(" Max inline data : %d[B]\n",user_param->inline_size);

	else
		printf(" Outstand reads  : %d\n",user_param->out_reads);

	printf(" rdma_cm QPs	 : %s\n",qp_state[user_param->work_rdma_cm]);

	if (user_param->use_rdma_cm)
		temp = 1;

	printf(" Data ex. method : %s",exchange_state[temp]);

	if (user_param->work_rdma_cm) {

		if (user_param->tos != DEF_TOS) {
                        printf(" \tTOS    : %d",user_param->tos);
                }

		if (user_param->machine == SERVER) {
			putchar('\n');
			printf(RESULT_LINE);
			printf(" Waiting for client rdma_cm QP to connect\n");
			printf(" Please run the same command with the IB/RoCE interface IP");
		}
	}
	putchar('\n');

	printf(RESULT_LINE);

}

static float calc_cpu_util (struct perftest_parameters *user_param)
{
	long long ustat_diff, idle_diff;
	ustat_diff = user_param->cpu_util_data.ustat[1] - user_param->cpu_util_data.ustat[0];
	idle_diff = user_param->cpu_util_data.idle[1] - user_param->cpu_util_data.idle[0];
	if ((ustat_diff + idle_diff) != 0)
		return ((float)ustat_diff / (ustat_diff + idle_diff)) * 100;
	else
		return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
void print_report_bw (struct perftest_parameters *user_param, struct bw_report_data *my_bw_rep) {

	double cycles_to_units,sum_of_test_cycles;
	int location_arr;
	int opt_completed = 0;
	int opt_posted = 0;
	int i,j;
	int run_inf_bi_factor;
	int num_of_qps = user_param->num_of_qps;
	long format_factor;
	long num_of_calculated_iters = user_param->iters;
	int free_my_bw_rep = 0;

	cycles_t t,opt_delta, peak_up, peak_down,tsize;

	opt_delta = user_param->tcompleted[opt_posted] - user_param->tposted[opt_completed];

	if((user_param->connection_type == DC ||user_param->use_xrc) && user_param->duplex)
		num_of_qps /= 2;

	if (user_param->noPeak == OFF) {
		/* Find the peak bandwidth unless asked not to in command line*/
		for (i = 0; i < user_param->iters * num_of_qps; ++i) {
			for (j = i; j < user_param->iters * num_of_qps; ++j) {
				t = (user_param->tcompleted[j] - user_param->tposted[i]) / (j - i + 1);
				if (t < opt_delta) {
					opt_delta  = t;
					opt_posted = i;
					opt_completed = j;
				}
			}
		}
	}

	cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f) * 1000000;
	if ((cycles_to_units == 0 && !user_param->cpu_freq_f)) {
		fprintf(stderr,"Can't produce a report\n");
		exit(1);
	}

	run_inf_bi_factor = (user_param->duplex && user_param->test_method == RUN_INFINITELY) ? (user_param->verb == SEND ? 1 : 2) : 1 ;
	tsize = run_inf_bi_factor * user_param->size;
	num_of_calculated_iters *= (user_param->test_type == DURATION) ? 1 : num_of_qps;
	location_arr = (user_param->noPeak) ? 0 : user_param->iters*num_of_qps - 1;
	//support in GBS format
	format_factor = (user_param->report_fmt == MBS) ? 0x100000 : 125000000;

	sum_of_test_cycles = ((double)(user_param->tcompleted[location_arr] - user_param->tposted[0]));

	double bw_avg = ((double)tsize*num_of_calculated_iters * cycles_to_units) / (sum_of_test_cycles * format_factor);
	double msgRate_avg = ((double)num_of_calculated_iters * cycles_to_units * run_inf_bi_factor) / (sum_of_test_cycles * 1000000);

	double bw_avg_p1 = ((double)tsize*user_param->iters_per_port[0] * cycles_to_units) / (sum_of_test_cycles * format_factor);
	double msgRate_avg_p1 = ((double)user_param->iters_per_port[0] * cycles_to_units * run_inf_bi_factor) / (sum_of_test_cycles * 1000000);

	double bw_avg_p2 = ((double)tsize*user_param->iters_per_port[1] * cycles_to_units) / (sum_of_test_cycles * format_factor);
	double msgRate_avg_p2 = ((double)user_param->iters_per_port[1] * cycles_to_units * run_inf_bi_factor) / (sum_of_test_cycles * 1000000);

	peak_up = !(user_param->noPeak)*(cycles_t)tsize*(cycles_t)cycles_to_units;
	peak_down = (cycles_t)opt_delta * format_factor;

	if (my_bw_rep == NULL) {
		free_my_bw_rep = 1;
		ALLOCATE(my_bw_rep , struct bw_report_data , 1);
		memset(my_bw_rep, 0, sizeof(struct bw_report_data));
	}

	my_bw_rep->size = (unsigned long)user_param->size;
	my_bw_rep->iters = user_param->iters;
	my_bw_rep->bw_peak = (double)peak_up/peak_down;
	my_bw_rep->bw_avg = bw_avg;
	my_bw_rep->msgRate_avg = msgRate_avg;
	my_bw_rep->bw_avg_p1 = bw_avg_p1;
	my_bw_rep->msgRate_avg_p1 = msgRate_avg_p1;
	my_bw_rep->bw_avg_p2 = bw_avg_p2;
	my_bw_rep->msgRate_avg_p2 = msgRate_avg_p2;
	my_bw_rep->sl = user_param->sl;

	if (!user_param->duplex || (user_param->verb == SEND && user_param->test_type == DURATION) 
							|| user_param->test_method == RUN_INFINITELY || user_param->connection_type == RawEth)
		print_full_bw_report(user_param, my_bw_rep, NULL);

	if (free_my_bw_rep == 1) {
		free(my_bw_rep);
	}
}

/******************************************************************************
 *
 ******************************************************************************/

void print_full_bw_report (struct perftest_parameters *user_param, struct bw_report_data *my_bw_rep, struct bw_report_data *rem_bw_rep)
{

 	double bw_peak     = my_bw_rep->bw_peak;
 	double bw_avg      = my_bw_rep->bw_avg;
	double bw_avg_p1      = my_bw_rep->bw_avg_p1;
	double bw_avg_p2      = my_bw_rep->bw_avg_p2;
 	double msgRate_avg = my_bw_rep->msgRate_avg;
	double msgRate_avg_p1 = my_bw_rep->msgRate_avg_p1;
	double msgRate_avg_p2 = my_bw_rep->msgRate_avg_p2;
	int inc_accuracy = ((bw_avg < 0.1) && (user_param->report_fmt == GBS));

	if (rem_bw_rep != NULL) {
 		bw_peak     += rem_bw_rep->bw_peak;
 		bw_avg      += rem_bw_rep->bw_avg;
		bw_avg_p1      += rem_bw_rep->bw_avg_p1;
		bw_avg_p2      += rem_bw_rep->bw_avg_p2;
		msgRate_avg += rem_bw_rep->msgRate_avg;
		msgRate_avg_p1 += rem_bw_rep->msgRate_avg_p1;
		msgRate_avg_p2 += rem_bw_rep->msgRate_avg_p2;
	}

	if ( (user_param->duplex && rem_bw_rep != NULL) ||  (!user_param->duplex && rem_bw_rep == NULL)) {//bibw test, and not report-both printing
		// Verify Limits
		if ( ((user_param->is_limit_bw == ON )&& (user_param->limit_bw > bw_avg)) )
			user_param->is_bw_limit_passed |= 0;
		else
			user_param->is_bw_limit_passed |= 1;

		if ( (user_param->is_limit_msgrate) && (user_param->limit_msgrate > msgRate_avg) )
			user_param->is_msgrate_limit_passed |= 0;
		else
			user_param->is_msgrate_limit_passed |= 1;
	}

	if (user_param->output == OUTPUT_BW)
		printf("%lf\n",bw_avg);
	else if (user_param->output == OUTPUT_MR)
		printf("%lf\n",msgRate_avg);
	else if (user_param->raw_qos)
		printf( REPORT_FMT_QOS, my_bw_rep->size, my_bw_rep->sl, my_bw_rep->iters, bw_peak, bw_avg, msgRate_avg);
	else if (user_param->report_per_port)
		printf(REPORT_FMT_PER_PORT, my_bw_rep->size, my_bw_rep->iters, bw_peak, bw_avg, msgRate_avg, bw_avg_p1, msgRate_avg_p1, bw_avg_p2, msgRate_avg_p2);
	else
		printf( inc_accuracy ? REPORT_FMT_EXT : REPORT_FMT, my_bw_rep->size, my_bw_rep->iters, bw_peak, bw_avg, msgRate_avg);
	if (user_param->output == FULL_VERBOSITY)
		printf( user_param->cpu_util_data.enable ? REPORT_EXT_CPU_UTIL : REPORT_EXT , calc_cpu_util(user_param));
}
/******************************************************************************
 *
 ******************************************************************************/
static inline cycles_t get_median(int n, cycles_t delta[])
{
    if ((n - 1) % 2)
        return(delta[n / 2] + delta[n / 2 - 1]) / 2;
    else
        return delta[n / 2];
}

/******************************************************************************
 *
 ******************************************************************************/
static int cycles_compare(const void *aptr, const void *bptr)
{
    const cycles_t *a = aptr;
    const cycles_t *b = bptr;
    if (*a < *b) return -1;
    if (*a > *b) return 1;
    return 0;

}

/******************************************************************************
 *
 ******************************************************************************/
void print_report_lat (struct perftest_parameters *user_param)
{

    int i;
	int rtt_factor;
	double cycles_to_units;
    cycles_t median;
	cycles_t *delta = NULL;
    const char* units;
	double latency;

	rtt_factor = (user_param->verb == READ || user_param->verb == ATOMIC) ? 1 : 2;
	ALLOCATE(delta,cycles_t,user_param->iters - 1);

    for (i = 0; i < user_param->iters - 1; ++i)
        delta[i] = user_param->tposted[i + 1] - user_param->tposted[i];

    if (user_param->r_flag->cycles) {
        cycles_to_units = 1;
        units = "cycles";

    } else {
        cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
		if ((cycles_to_units == 0 && !user_param->cpu_freq_f)) {
			fprintf(stderr,"Can't produce a report\n");
			exit(1);
		}

        units = "usec";
    }

    if (user_param->r_flag->unsorted) {
        printf("#, %s\n", units);
        for (i = 0; i < user_param->iters - 1; ++i)
		    printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / rtt_factor);
    }

    qsort(delta, user_param->iters - 1, sizeof *delta, cycles_compare);

    if (user_param->r_flag->histogram) {
        printf("#, %s\n", units);
        for (i = 0; i < user_param->iters - 1; ++i)
            printf("%d, %g\n", i + 1, delta[i] / cycles_to_units / rtt_factor);
    }

    median = get_median(user_param->iters - 1, delta);

	latency = median / cycles_to_units / rtt_factor;

	if (user_param->output == OUTPUT_LAT) {
		printf("%lf\n",latency);
	}
	else {
		printf(REPORT_FMT_LAT,
				(unsigned long)user_param->size,
				user_param->iters,
				delta[0] / cycles_to_units / rtt_factor,
				delta[user_param->iters - 2] / cycles_to_units / rtt_factor,
				latency);
		printf( user_param->cpu_util_data.enable ? REPORT_EXT_CPU_UTIL : REPORT_EXT , calc_cpu_util(user_param));
	}
    free(delta);
}

/******************************************************************************
 *
 ******************************************************************************/
void print_report_lat_duration (struct perftest_parameters *user_param)
{
	int rtt_factor;
	double cycles_to_units;
	cycles_t test_sample_time;
	double latency;

	rtt_factor = (user_param->verb == READ || user_param->verb == ATOMIC) ? 1 : 2;
	cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f);
	if ((cycles_to_units == 0 && !user_param->cpu_freq_f)) {
		fprintf(stderr,"Can't produce a report\n");
		exit(1);
	}

	test_sample_time = (user_param->tcompleted[0] - user_param->tposted[0]);
	latency = (((test_sample_time / cycles_to_units) / rtt_factor) / user_param->iters);

	if (user_param->output == OUTPUT_LAT) {
		printf("%lf\n",latency);
	}
	else {
		printf(REPORT_FMT_LAT_DUR,
			user_param->size,
			user_param->iters,
			latency);
		printf( user_param->cpu_util_data.enable ? REPORT_EXT_CPU_UTIL : REPORT_EXT , calc_cpu_util(user_param));
	}
}
/******************************************************************************
 * End
 ******************************************************************************/
