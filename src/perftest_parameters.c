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

static const char *connStr[] = {"RC","UC","UD","RawEth"};
static const char *testsStr[] = {"Send","RDMA_Write","RDMA_Read","Atomic"};
static const char *portStates[] = {"Nop","Down","Init","Armed","","Active Defer"};
static const char *qp_state[] = {"OFF","ON"};
static const char *exchange_state[] = {"Ethernet","rdma_cm"};
static const char *atomicTypesStr[] = {"CMP_AND_SWAP","FETCH_AND_ADD"};

#ifdef _WIN32
// The link layer of the current port.
typedef enum { 
	IBV_LINK_LAYER_UNSPECIFIED = 0 , 
	IBV_LINK_LAYER_INFINIBAND = 1 , 
	IBV_LINK_LAYER_ETHERNET = 2 
} LinkType;
#endif

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
 * Description : Convert from presentation format of an Internet number in buffer
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
 *
 ******************************************************************************/
static void usage(const char *argv0,VerbType verb,TestType tst)	{

	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");

	printf("  -h, --help ");
	printf(" Show this help screen.\n");

	printf("  -p, --port=<port> ");
	printf(" Listen on/connect to port <port> (default %d)\n",DEF_PORT);

	printf("  -d, --ib-dev=<dev> ");
	printf(" Use IB device <dev> (default first device found)\n");

	printf("  -R, --rdma_cm ");
	printf(" Connect QPs with rdma_cm and run test on those QPs\n");

	printf("  -z, --com_rdma_cm ");
	printf(" Communicate with rdma_cm module to exchange data - use regular QPs\n");

	printf("  -i, --ib-port=<port> ");
	printf(" Use port <port> of IB device (default %d)\n",DEF_IB_PORT);

	printf("  -c, --connection=<RC/UC/UD> ");
	printf(" Connection type RC/UC/UD (default RC)\n");

	printf("  -m, --mtu=<mtu> ");
	printf(" Mtu size : 256 - 4096 (default port mtu)\n");

	if (verb != ATOMIC) {

		printf("  -s, --size=<size> ");
		printf(" Size of message to exchange (default %d)\n",tst == LAT ? DEF_SIZE_LAT : DEF_SIZE_BW);

		printf("  -a, --all ");
		printf(" Run sizes from 2 till 2^23\n");
	}

	printf("  -n, --iters=<iters> ");
	printf(" Number of exchanges (at least 2, default %d)\n",DEF_ITERS);

	if (tst == BW) {

		printf("  -t, --tx-depth=<dep> ");
		printf(" Size of tx queue (default %d)\n",tst == LAT ? DEF_TX_LAT : DEF_TX_BW);
	}

	printf("  -u, --qp-timeout=<timeout> ");
	printf(" QP timeout, timeout value is 4 usec * 2 ^(timeout), default %d\n",DEF_QP_TIME);

	printf("  -S, --sl=<sl> ");
	printf(" SL (default %d)\n",DEF_SL);

	printf("  -x, --gid-index=<index> ");
	printf(" Test uses GID with GID index (Default : IB - no gid . ETH - 0)\n");

	printf("  -F, --CPU-freq ");
	printf(" Do not fail even if cpufreq_ondemand module is loaded\n");

	printf("  -V, --version ");
	printf(" Display version number\n");

	if (verb == SEND) {
		printf("  -r, --rx-depth=<dep> ");
		printf(" Rx queue size (default %d)\n",DEF_RX_SEND);
	}

	if (verb != READ || verb != ATOMIC) {
		printf("  -I, --inline_size=<size> ");
		printf(" Max size of message to be sent in inline\n");
	}

	if (tst == BW) {

		printf("  -b, --bidirectional ");
		printf(" Measure bidirectional bandwidth (default unidirectional)\n");
		
		printf("  -Q, --cq-mod ");
		printf(" Generate Cqe only after <--cq-mod> completion\n");

		printf("  -O, --dualport ");
		printf(" Run test in dual-port mode.\n");

		printf("  -D, --duration ");
		printf(" Run test for a customized period of seconds.\n");
	
		printf("  -f, --margin ");
		printf(" measure results within margins. (default=2sec)\n");

		printf("  -l, --post_list=<list size>");
		printf(" Post list of WQEs of <list size> size (instead of single post)\n");
 
		printf("  --run_infinitely ");
		printf(" Run test forever, print results every 5 seconds\n");
	}

	if (verb != WRITE) {
		printf("  -e, --events ");
		printf(" Sleep on CQ events (default poll)\n");
	}

	if (tst == BW) {
		printf("  -N, --no peak-bw ");
		printf(" Cancel peak-bw calculation (default with peak)\n");
	}

	if (verb == SEND) {
		printf("  -g, --mcg=<num_of_qps> ");
		printf(" Send messages to multicast group with <num_of_qps> qps attached to it.\n");

		printf("  -M, --MGID=<multicast_gid> ");
		printf(" In multicast, uses <multicast_gid> as the group MGID.\n");
	}

	if (verb == READ || verb == ATOMIC) {
		printf("  -o, --outs=<num> ");
		printf(" num of outstanding read/atom(default max of device)\n");
	}

	if (verb == ATOMIC) {
		printf("  -A, --atomic_type=<type> ");
		printf(" type of atomic operation from {CMP_AND_SWAP,FETCH_AND_ADD} (default FETCH_AND_ADD)\n");
	}

	if (tst == LAT) {
		printf("  -C, --report-cycles ");
		printf(" report times in cpu cycle units (default microseconds)\n");

		printf("  -H, --report-histogram ");
		printf(" Print out all results (default print summary only)\n");

		printf("  -U, --report-unsorted ");
		printf(" (implies -H) print out unsorted results (default sorted)\n");
	}


	if (tst == BW) 
		printf("  -q, --qp=<num of qp's>  Num of qp's(default %d)\n",DEF_NUM_QPS);

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

		printf("  -J, --server_ip ");
		printf(" server ip address by this format X.X.X.X (using to send packets with IP header)\n");

		printf("  -j, --client_ip ");
		printf(" client ip address by this format X.X.X.X (using to send packets with IP header)\n");

		printf("  -K, --server_port ");
		printf(" server port number (using to send packets with UPD header)\n");

		printf("  -k, --client_port ");
		printf(" client port number (using to send packets with UDP header)\n");

		printf("  -Z, --server ");
		printf(" choose server side for the current machine (--server/--client must be selected )\n");

		printf("  -P, --client ");
		printf(" choose client side for the current machine (--server/--client must be selected)\n");
}
/******************************************************************************
 *
 ******************************************************************************/
static void init_perftest_params(struct perftest_parameters *user_param) {

	user_param->port       = DEF_PORT;
	user_param->ib_port    = DEF_IB_PORT;
	user_param->ib_port2        = DEF_IB_PORT2;
	user_param->size       = user_param->tst == BW ? DEF_SIZE_BW : DEF_SIZE_LAT;
	user_param->tx_depth   = user_param->tst == BW ? DEF_TX_BW : DEF_TX_LAT;
	user_param->qp_timeout = DEF_QP_TIME;
	user_param->test_method = RUN_REGULAR;
	user_param->cpu_freq_f = OFF;
	user_param->connection_type = (user_param->connection_type == RawEth) ? RawEth : RC;
	user_param->use_event  = OFF;
	user_param->num_of_qps = DEF_NUM_QPS;
	user_param->gid_index  = DEF_GID_INDEX;
	user_param->gid_index2  = DEF_GID_INDEX;
	user_param->inline_size = DEF_INLINE;
	user_param->use_mcg     = OFF;
	user_param->use_rdma_cm = OFF;
	user_param->work_rdma_cm = OFF;
	user_param->rx_depth    = user_param->verb == SEND ? DEF_RX_SEND : DEF_RX_RDMA;
	user_param->duplex		= OFF;
	user_param->noPeak		= OFF;
	user_param->cq_mod		= DEF_CQ_MOD;
	user_param->iters = (user_param->tst == BW && user_param->verb == WRITE) ? DEF_ITERS_WB : DEF_ITERS;
	user_param->dualport	= OFF;
	user_param->post_list	= 1;
	user_param->duration	= DEF_DURATION;
	user_param->margin		= DEF_MARGIN;
	user_param->test_type	= ITERATIONS;
	user_param->state		= START_STATE;

	if (user_param->tst == LAT) {
		user_param->r_flag->unsorted  = OFF;
		user_param->r_flag->histogram = OFF;
		user_param->r_flag->cycles    = OFF;
	}

	if (user_param->verb == ATOMIC) {
		user_param->atomicType = FETCH_AND_ADD;
		user_param->size = DEF_SIZE_ATOMIC;
	}

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

	} else { 
		fprintf(stderr," Invalid Connection type . please choose from {RC,UC,UD}\n"); 
		exit(1); 
	}
}
/******************************************************************************
  *
  ******************************************************************************/
static int set_eth_mtu(struct perftest_parameters *user_param) {

	if (user_param->mtu == 0) {
		user_param->mtu = 1500;
	}
	switch (user_param->mtu) {
				case 1500  :	user_param->curr_mtu = 1500;	 break;
				case 9600  : 	user_param->curr_mtu = 9600;	 break;
				default   :
					fprintf(stderr," Invalid MTU - %d \n",user_param->mtu);
					fprintf(stderr," Please choose mtu form {1500, 9600}\n");
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

	if (user_param->dualport==ON) {

		user_param->num_of_qps *= 2;
		if (!user_param->tst==BW) {

			printf(" Dual-port mode only supports BW tests.\n");
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

		if (user_param->tst == LAT) {
			printf(RESULT_LINE);
			fprintf(stderr, "Duration mode currently doesn't support latency tests.\n");
			exit(1); 
		}

		if (user_param->test_method == RUN_ALL) { 
			printf(RESULT_LINE);
			fprintf(stderr, "Duration mode currently doesn't support running on all sizes.\n");
			exit(1);
		}
	}

	if (user_param->use_mcg &&  user_param->gid_index == -1) {
			user_param->gid_index = 0;
			if (user_param->dualport==ON)
				user_param->gid_index2 = 0;
	}

	if (user_param->work_rdma_cm) {

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
	} 

	if (user_param->use_mcg) {

		if (user_param->connection_type != UD) 
			user_param->connection_type = UD;

		if (user_param->duplex && user_param->tst == BW) {
			user_param->num_of_qps++;

		} else if (user_param->tst == BW && user_param->machine == CLIENT) 
			user_param->num_of_qps = 1;
	}

	if (user_param->connection_type == RawEth) { 

		if (user_param->num_of_qps > 1) { 
			printf(RESULT_LINE);
			fprintf(stdout," Raw Ethernet test supports only 1 QP for now\n");
			exit(1);
		}

		if(user_param->machine == UNCHOSEN) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must choose test side --client or --server\n");
			exit(1);
		}

		if(user_param->machine == CLIENT && user_param->is_dest_mac == OFF) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n you must enter dest mac by this format -E XX:XX:XX:XX:XX:XX\n");
			exit(1);
		}

		if((user_param->is_server_ip == ON && user_param->is_client_ip == OFF) || (user_param->is_server_ip == OFF && user_param->is_client_ip == ON)) {
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n if you would like to send IP header,\n you must enter server&client ip addresses --server_ip X.X.X.X --client_ip X.X.X.X\n");
			exit(1);
		}

		if((user_param->is_server_port == ON && user_param->is_client_port == OFF) || (user_param->is_server_port == OFF && user_param->is_client_port == ON)) { 
			printf(RESULT_LINE);
			fprintf(stderr," Invalid Command line.\n if you would like to send UDP header,\n you must enter server&client port --server_port X --client_port X\n");
			exit(1);
		}
	}

	if (user_param->verb == SEND && user_param->machine == SERVER && !user_param->duplex)
		user_param->noPeak = ON;

	// Run infinitely dependencies
	if (user_param->test_method == RUN_INFINITELY) { 
		user_param->noPeak = ON;
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

	return;
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
        default:
		    return "Unknown";
    }
}

/****************************************************************************** 
 *
 ******************************************************************************/
static Device ib_dev_name(struct ibv_context *context) { 

	Device dev_fname = UNKNOWN;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		dev_fname = DEVICE_ERROR;
	}

	else { 

		switch (attr.vendor_part_id) { 
			case 4113  : dev_fname = CONNECTIB; break;
			case 4099  : dev_fname = CONNECTX3; break;
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
			default	   : dev_fname = UNKNOWN;
		}
	}

	return dev_fname;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static enum ibv_mtu set_mtu(struct ibv_context *context,uint8_t ib_port,int user_mtu) {

	struct ibv_port_attr port_attr;
	enum ibv_mtu curr_mtu;

	ibv_query_port(context,ib_port,&port_attr);

	// User did not ask for specific mtu.
	if (user_mtu == 0) 
		curr_mtu = port_attr.active_mtu;

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

#ifndef _WIN32
	curr_link = port_attr.link_layer; 
#else
	curr_link = IBV_LINK_LAYER_INFINIBAND;
#endif

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


	Device current_dev = ib_dev_name(context);

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

				case WRITE: user_param->inline_size = DEF_INLINE_WRITE; break;
				case SEND : user_param->inline_size = (user_param->connection_type == UD)? DEF_INLINE_SEND_UD :DEF_INLINE_SEND_RC_UC ; break;
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
int raw_ethernet_parser(struct perftest_parameters *user_param,char *argv[], int argc)
{
	int c;

	if(user_param->connection_type == RawEth)
	{
		user_param->machine = UNCHOSEN;
	}

	while (1) {
#ifndef _WIN32
		static const struct option long_options[] = {
			{ .name = "source_mac",     .has_arg = 1, .val = 'B' },
			{ .name = "dest_mac",       .has_arg = 1, .val = 'E' },
			{ .name = "server_ip",      .has_arg = 1, .val = 'J' },
			{ .name = "client_ip",      .has_arg = 1, .val = 'j' },
			{ .name = "server_port",    .has_arg = 1, .val = 'K' },
			{ .name = "client_port",    .has_arg = 1, .val = 'k' },
			{ .name = "server",         .has_arg = 0, .val = 'Z' },
			{ .name = "client",         .has_arg = 0, .val = 'P' },
            { 0 }
        };
#else

        static const struct option long_options[] = {
			{ "margin"			,1, NULL, 'f' },
			{ "source_mac"      ,1, NULL, 'B' },
			{ "dest_mac"        ,1, NULL, 'E' },
			{ "server_ip"       ,1, NULL, 'J' },
			{ "client_ip"       ,1, NULL, 'j' },
			{ "server_port"     ,1, NULL, 'K' },
			{ "client_port"     ,1, NULL, 'k' },
			{ "server"          ,0, NULL, 'Z' },
			{ "client"          ,0, NULL, 'P' },
			{ 0 }
		};
#endif

        c = getopt_long(argc,argv,"B:E:J:j:K:k:ZP",long_options,NULL);

        if (c == -1)
			break;
	switch (c) {
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
	case 'P': user_param->machine = CLIENT; break;
	case 'Z': user_param->machine = SERVER; break;
	default:
		return FAILURE;
	}
	}
	return SUCCESS;
}
/****************************************************************************** 
 *
 ******************************************************************************/
int parser(struct perftest_parameters *user_param,char *argv[], int argc) {

	int c;
	static int run_inf_flag = 0;

	init_perftest_params(user_param);
	if(user_param->connection_type == RawEth)
	{
		user_param->machine = UNCHOSEN;
	}

	while (1) {
#ifndef _WIN32
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
			{ .name = "rdma_cm",   		.has_arg = 0, .val = 'R' },
			{ .name = "help",           .has_arg = 0, .val = 'h' },
			{ .name = "MGID",           .has_arg = 1, .val = 'M' },
			{ .name = "rx-depth",       .has_arg = 1, .val = 'r' },
			{ .name = "bidirectional",  .has_arg = 0, .val = 'b' },
			{ .name = "cq-mod",  		.has_arg = 1, .val = 'Q' },
			{ .name = "noPeak",         .has_arg = 0, .val = 'N' },
			{ .name = "version",        .has_arg = 0, .val = 'V' },
			{ .name = "report-cycles",  .has_arg = 0, .val = 'C' },
			{ .name = "report-histogrm",.has_arg = 0, .val = 'H' },
			{ .name = "report-unsorted",.has_arg = 0, .val = 'U' },
			{ .name = "atomic_type",	.has_arg = 1, .val = 'A' },
			{ .name = "dualport",       .has_arg = 0, .val = 'O' },
			{ .name = "post_list",      .has_arg = 1, .val = 'l' },
			{ .name = "duration",       .has_arg = 1, .val = 'D' },
			{ .name = "margin",         .has_arg = 1, .val = 'f' },
			{ .name = "source_mac",     .has_arg = 1, .val = 'B' },
			{ .name = "dest_mac",       .has_arg = 1, .val = 'E' },
			{ .name = "server_ip",      .has_arg = 1, .val = 'J' },
			{ .name = "client_ip",      .has_arg = 1, .val = 'j' },
			{ .name = "server_port",    .has_arg = 1, .val = 'K' },
			{ .name = "client_port",    .has_arg = 1, .val = 'k' },
			{ .name = "server",         .has_arg = 0, .val = 'Z' },
			{ .name = "client",         .has_arg = 0, .val = 'P' },
			{ .name = "run_infinitely", .has_arg = 0, .flag = &run_inf_flag, .val = 1 },
            { 0 }
        };
#else

        static const struct option long_options[] = {
			{ "port",			1, NULL, 'p' },
			{ "ib-dev",			1, NULL, 'd' },
			{ "ib-port",		1, NULL, 'i' },
			{ "mtu",			1, NULL, 'm' },
			{ "size",			1, NULL, 's' },
			{ "iters",			1, NULL, 'n' },
			{ "tx-depth",		1, NULL, 't' },
			{ "qp-timeout", 	1, NULL, 'u' },
			{ "sl", 			1, NULL, 'S' },
			{ "gid-index",		1, NULL, 'x' },
			{ "all",			0, NULL, 'a' },
			{ "CPU-freq",		0, NULL, 'F' },
			{ "connection",		1, NULL, 'c' },
			{ "qp", 			1, NULL, 'q' },
			{ "events", 		0, NULL, 'e' },
			{ "inline_size",	1, NULL, 'I' },
			{ "outs",			1, NULL, 'o' },
			{ "mcg",			0, NULL, 'g' },
			{ "comm_rdma_cm",	0, NULL, 'z' },
			{ "rdma_cm",		0, NULL, 'R' },
			{ "help",			0, NULL, 'h' },
			{ "MGID",			1, NULL, 'M' },
			{ "rx-depth",		1, NULL, 'r' },
			{ "bidirectional",	0, NULL, 'b' },
			{ "cq-mod", 		1, NULL, 'Q' },
			{ "noPeak", 		0, NULL, 'N' },
			{ "version",		0, NULL, 'V' },
            { "report-cycles",	0, NULL, 'C' },
			{ "report-histogrm"	,0, NULL, 'H' },
            { "report-unsorted"	,0, NULL, 'U' },
			{ "atomic_type"		,1, NULL, 'A' },
			{ "dualport"		,0, NULL, 'O' },
			{ "post_list"		,1, NULL, 'l' },
			{ "duration"		,1, NULL, 'D' },
			{ "margin"			,1, NULL, 'f' },
			{ "source_mac"      ,1, NULL, 'B' },
			{ "dest_mac"        ,1, NULL, 'E' },
			{ "server_ip"       ,1, NULL, 'J' },
			{ "client_ip"       ,1, NULL, 'j' },
			{ "server_port"     ,1, NULL, 'K' },
			{ "client_port"     ,1, NULL, 'k' },
			{ "server"          ,0, NULL, 'Z' },
			{ "client"          ,0, NULL, 'P' },
            { "dualport"        ,0, &run_inf_flag, 1 },
			{ 0 }
		};
#endif
        c = getopt_long(argc,argv,"p:d:i:m:s:n:t:u:S:x:c:q:I:o:M:r:Q:A:l:D:f:B:E:J:j:K:k:aFegzRhbNVCHUOZP",long_options,NULL);

        if (c == -1)
			break;

        switch (c) {

			case 'p': user_param->port = strtol(optarg, NULL, 0); break;
#ifndef _WIN32
			case 'd': GET_STRING(user_param->ib_devname,strdupa(optarg)); break;
#else
			case 'd': GET_STRING(user_param->ib_devname,_strdup(optarg)); break;
#endif
			case 'i': CHECK_VALUE(user_param->ib_port,uint8_t,MIN_IB_PORT,MAX_IB_PORT,"IB Port"); break;
            case 'm': user_param->mtu  = strtol(optarg, NULL, 0); break;
			case 'n': CHECK_VALUE(user_param->iters,int,MIN_ITER,MAX_ITER,"Iteration num"); break;
			case 't': CHECK_VALUE(user_param->tx_depth,int,MIN_TX,MAX_TX,"Tx depth"); break;
			case 'u': user_param->qp_timeout = (uint8_t)strtol(optarg, NULL, 0); break;
			case 'S': user_param->sl = (uint8_t)strtol(optarg, NULL, 0);
				if (user_param->sl > MAX_SL) { 
					fprintf(stderr," Only %d Service levels\n",MAX_SL);
					return 1;
				} break;
			case 'x': CHECK_VALUE(user_param->gid_index,uint8_t,MIN_GID_IX,MAX_GID_IX,"Gid index"); break;
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
#ifndef _WIN32
			case 'M': GET_STRING(user_param->user_mgid,strdupa(optarg)); 
#else
			case 'M': GET_STRING(user_param->user_mgid,_strdup(optarg));
#endif
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
 				if (user_param->margin <= 0) {
					fprintf(stderr," margin must be greater than 0.\n");
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
			case 's': CHECK_VALUE(user_param->size,uint64_t,1,(UINT_MAX / 2),"Message size"); break;
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
			case 'P': user_param->machine = CLIENT; break;
			case 'Z': user_param->machine = SERVER; break;
			case 0: break; // required for long options to work.
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

	if (run_inf_flag) { 
		user_param->test_method = RUN_INFINITELY;
	}

	if(user_param->connection_type != RawEth) {
		
		if (optind == argc - 1) {
#ifndef _WIN32
			GET_STRING(user_param->servername,strdupa(argv[optind]));
#else
			GET_STRING(user_param->servername,_strdup(argv[optind]));
#endif
		} else if (optind < argc) {
				fprintf(stderr," Invalid Command line. Please check command rerun \n");
				return 1;
		}

		user_param->machine = user_param->servername ? CLIENT : SERVER;
	}
	force_dependecies(user_param);
    return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int check_link_and_mtu(struct ibv_context *context,struct perftest_parameters *user_param) {

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

		if (set_eth_mtu(user_param) != 0)
			fprintf(stderr, " Couldn't set Eth MTU\n");
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

	return SUCCESS;
}

/****************************************************************************** 
 *
 ******************************************************************************/
void ctx_print_test_info(struct perftest_parameters *user_param) {

	int temp = 0;

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

	} else {
		printf("Latency ");
	}

	if (user_param->use_mcg) 
		printf("Multicast ");

	printf("Test\n");

	if (user_param->use_event) {
		printf(" Test with events.\n");
		
	}

	if (user_param->use_mcg) 
		printf(" MultiCast runs on UD!\n");

	printf(" Dual-port       : %s\t\tDevice : %s\n", user_param->dualport ? "ON" : "OFF",user_param->ib_devname);
	printf(" Number of qps   : %d\n",user_param->num_of_qps);
	printf(" Connection type : %s\n",connStr[user_param->connection_type]);
	
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

	printf(" Mtu             : %luB\n",user_param->connection_type == RawEth ? user_param->curr_mtu : MTU_SIZE(user_param->curr_mtu));
	printf(" Link type       : %s\n" ,link_layer_str(user_param->link_type));

	if (user_param->gid_index != DEF_GID_INDEX)
		printf(" Gid index       : %d\n" ,user_param->gid_index);
	if ((user_param->dualport==ON) && (user_param->gid_index2 != DEF_GID_INDEX))
		printf(" Gid index2      : %d\n" ,user_param->gid_index2);

	if (user_param->verb != READ && user_param->verb != ATOMIC) 
		printf(" Max inline data : %dB\n",user_param->inline_size);

	else 
		printf(" Outstand reads  : %d\n",user_param->out_reads);

	printf(" rdma_cm QPs	 : %s\n",qp_state[user_param->work_rdma_cm]);

	if (user_param->use_rdma_cm) 
		temp = 1;

	printf(" Data ex. method : %s\n",exchange_state[temp]);

	if (user_param->work_rdma_cm && user_param->machine == SERVER) {
		printf(RESULT_LINE);
		printf(" Waiting for client rdma_cm QP to connect\n");
		printf(" Please run the same command with the IB/RoCE interface IP\n");
	}

	printf(RESULT_LINE);

}

/****************************************************************************** 
 *
 ******************************************************************************/
void print_report_bw (struct perftest_parameters *user_param) {

	double cycles_to_units,aux_up,aux_down;
	int location_arr, i, j, opt_posted = 0, opt_completed = 0;
	cycles_t t,opt_delta, peak_up, peak_down,tsize;

	opt_delta = user_param->tcompleted[opt_posted] - user_param->tposted[opt_completed];

	if (user_param->noPeak == OFF) {
		/* Find the peak bandwidth unless asked not to in command line*/
		for (i = 0; i < user_param->iters * user_param->num_of_qps; ++i) {
			for (j = i; j < user_param->iters * user_param->num_of_qps; ++j) {
				t = (user_param->tcompleted[j] - user_param->tposted[i]) / (j - i + 1);
				if (t < opt_delta) {
					opt_delta  = t;
					opt_posted = i;
					opt_completed = j;
				}
			}
		}
	}

#ifndef _WIN32
	cycles_to_units = get_cpu_mhz(user_param->cpu_freq_f) * 1000000;
#else
	cycles_to_units = get_cpu_mhz();
#endif

	tsize = user_param->duplex ? 2 : 1;
	tsize = tsize * user_param->size;
	aux_up = (double)tsize*user_param->iters;
	aux_up *= (user_param->test_type == DURATION) ? 1 : user_param->num_of_qps;
	location_arr = (user_param->noPeak) ? 0 : user_param->iters*user_param->num_of_qps - 1;
	aux_down = (double)(user_param->tcompleted[location_arr] - user_param->tposted[0]);
	peak_up = !(user_param->noPeak)*(cycles_t)tsize*(cycles_t)cycles_to_units;
	peak_down = (cycles_t)opt_delta * 0x100000;
	printf(REPORT_FMT,
		(unsigned long)user_param->size, 
		user_param->iters,
		(double)peak_up/peak_down,
	    (aux_up*cycles_to_units)/(aux_down*0x100000),
	    ((aux_up*cycles_to_units)/aux_down)/((unsigned long)user_param->size)/1000000);
}
/****************************************************************************** 
 * End
 ******************************************************************************/
