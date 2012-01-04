#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <limits.h>
#include "perftest_parameters.h"


static const char *connStr[] = {"RC","UC","UD"};
static const char *testsStr[] = {"Send","RDMA_Write","RDMA_Read","Atomic"};
static const char *portStates[] = {"Nop","Down","Init","Armed","","Active Defer"};
static const char *qp_state[] = {"OFF","ON"};
static const char *exchange_state[] = {"Ethernet","rdma_cm"};
static const char *atomicTypesStr[] = {"CMP_AND_SWAP","FETCH_AND_ADD"};

/****************************************************************************** 
 *
 ******************************************************************************/
static void usage(const char *argv0,VerbType verb,TestType tst)	{

	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n", argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");

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

	printf("  -t, --tx-depth=<dep> ");
	printf(" Size of tx queue (default %d)\n",tst == LAT ? DEF_TX_LAT : DEF_TX_BW);

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

	if (verb == SEND && tst == BW) {
		printf("  -r, --rx-depth=<dep> ");
		printf(" Rx queue size (default %d)\n",DEF_RX_SEND);
	}

	if (verb != READ || verb != ATOMIC) {
		printf("  -I, --inline_size=<size> ");
		printf(" Max size of message to be sent in inline (default %d)\n",tst == LAT ? DEF_INLINE_LT : DEF_INLINE_BW);
	}

	if (tst == BW) {
		printf("  -b, --bidirectional ");
		printf(" Measure bidirectional bandwidth (default unidirectional)\n");
		
		printf("  -Q, --cq-mod ");
		printf(" Generate Cqe only after <--cq-mod> completion\n");
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

	if (verb == WRITE && tst == BW) 
		printf("  -q, --qp=<num of qp's>  Num of qp's(default %d)\n",DEF_NUM_QPS);

	putchar('\n');
}

/******************************************************************************
 *
 ******************************************************************************/
static void init_perftest_params(struct perftest_parameters *user_param) {

	user_param->port       = DEF_PORT;
	user_param->ib_port    = DEF_IB_PORT;
	user_param->size       = user_param->tst == BW ? DEF_SIZE_BW : DEF_SIZE_LAT;
	user_param->tx_depth   = user_param->tst == BW ? DEF_TX_BW : DEF_TX_LAT;
	user_param->qp_timeout = DEF_QP_TIME;
	user_param->all		   = OFF;
	user_param->cpu_freq_f = OFF;
	user_param->connection_type = RC;
	user_param->use_event  = OFF;
	user_param->num_of_qps = DEF_NUM_QPS;
	user_param->gid_index  = DEF_GID_INDEX;
	user_param->inline_size = user_param->tst == BW ? DEF_INLINE_BW : DEF_INLINE_LT;
	user_param->use_mcg     = OFF;
	user_param->use_rdma_cm = OFF;
	user_param->work_rdma_cm = OFF;
	user_param->rx_depth    = user_param->verb == SEND ? DEF_RX_SEND : DEF_RX_RDMA;
	user_param->duplex		= OFF;
	user_param->noPeak		= OFF;
	user_param->cq_mod		= DEF_CQ_MOD;
	user_param->iters = (user_param->tst == BW && user_param->verb == WRITE) ? DEF_ITERS_WB : DEF_ITERS;

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

	} else { 
		fprintf(stderr," Invalid Connection type . please choose from {RC,UC,UD}\n"); 
		exit(1); 
	}
}

/******************************************************************************
 *
 ******************************************************************************/
static void force_dependecies(struct perftest_parameters *user_param) {


	// Additional configuration and assignments.
	if (user_param->tx_depth > user_param->iters) {
		user_param->tx_depth = user_param->iters;
	}

	if (user_param->cq_mod > user_param->tx_depth) {
		user_param->cq_mod = user_param->tx_depth;
	}

	if (user_param->verb == SEND && user_param->tst == BW &&
		user_param->rx_depth > user_param->iters) {
		user_param->rx_depth = user_param->iters;
	}

	if (user_param->use_mcg &&  user_param->gid_index == -1) {
			user_param->gid_index = 0;
	}

	if (user_param->verb == READ || user_param->verb == ATOMIC) 
		user_param->inline_size = 0;
	
	//if (user_param->verb == WRITE || user_param->verb == READ || user_param->verb == ATOMIC)
	//	user_param->cq_size = user_param->tx_depth*user_param->num_of_qps;

	//else if (user_param->duplex) 		
	//	 user_param->cq_size = user_param->tx_depth + user_param->rx_depth*(user_param->num_of_qps);

	//else if (user_param->machine == CLIENT) 
	//	user_param->cq_size = user_param->tx_depth;

	//else 
	//	user_param->cq_size = user_param->rx_depth*user_param->num_of_qps;

	if (user_param->work_rdma_cm && user_param->use_mcg) {

		printf(RESULT_LINE);
		printf(" Perftest still doesn't support Multicast with rdam_cm\n");
		user_param->use_mcg = OFF;
		user_param->num_of_qps = 1;
	} 

	if (user_param->use_mcg) {

		if (user_param->connection_type != UD) 
			user_param->connection_type = UD;

		if (user_param->duplex && user_param->tst == BW) {
			user_param->num_of_qps++;

		} else if (user_param->tst == BW && user_param->machine == CLIENT) 
			user_param->num_of_qps = 1;
	}

	if (user_param->all == ON) 	
		user_param->size = MAX_SIZE;

	if (user_param->work_rdma_cm == ON) {
		user_param->use_rdma_cm = ON;
		if (user_param->num_of_qps > 1) {
			user_param->num_of_qps = 1;
			printf(RESULT_LINE);
			fprintf(stdout," Perftest only supports 1 rmda_cm QP for now\n");
		}
	}

	if (user_param->verb == ATOMIC && user_param->size != DEF_SIZE_ATOMIC) {
		user_param->size = DEF_SIZE_ATOMIC;
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
static Device is_dev_hermon(struct ibv_context *context) { 

	Device is_hermon = NOT_HERMON;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		is_hermon = ERROR;
	}
	// Checks the devide type for setting the max outstanding reads.
	else if (attr.vendor_part_id == 25408  || attr.vendor_part_id == 25418  ||
			 attr.vendor_part_id == 25448  || attr.vendor_part_id == 26418  || 
			 attr.vendor_part_id == 26428  || attr.vendor_part_id == 26438  ||
			 attr.vendor_part_id == 26448  || attr.vendor_part_id == 26458  ||
			 attr.vendor_part_id == 26468  || attr.vendor_part_id == 26478  ||
			 attr.vendor_part_id == 4099 ) {
				is_hermon = HERMON;		
	}
	return is_hermon;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static enum ibv_mtu set_mtu(struct ibv_context *context,int ib_port,int user_mtu) {

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
				fprintf(stderr," Please choose mtu form {256,512,1024,2048,4096}\n");
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
static int8_t set_link_layer(struct ibv_context *context,int ib_port) {

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

	return curr_link;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ctx_set_out_reads(struct ibv_context *context,int num_user_reads) {


	int max_reads;

	max_reads = (is_dev_hermon(context) == HERMON) ? MAX_OUT_READ_HERMON : MAX_OUT_READ;

	if (num_user_reads > max_reads) {
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
int parser(struct perftest_parameters *user_param,char *argv[], int argc) {

	int c;

	init_perftest_params(user_param);

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
			{ .name = "mcg",            .has_arg = 1, .val = 'g' },
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
            { 0 }
        };

        c = getopt_long(argc,argv,"p:d:i:m:o:c:s:g:n:t:I:r:u:q:S:x:M:Q:A:lVaezRhbNFCHU",long_options,NULL);

        if (c == -1)
			break;

        switch (c) {

			case 'p': user_param->port = strtol(optarg, NULL, 0);                			  break;
			case 'd': GET_STRING(user_param->ib_devname,strdupa(optarg)); 			  	  	  break;
			case 'i': CHECK_VALUE(user_param->ib_port,MIN_IB_PORT,MAX_IB_PORT,"IB Port"); 	  break;
            case 'm': user_param->mtu  = strtol(optarg, NULL, 0); 							  break;
			case 'n': CHECK_VALUE(user_param->iters,MIN_ITER,MAX_ITER,"Iteration num"); 	  break;
			case 't': CHECK_VALUE(user_param->tx_depth,MIN_TX,MAX_TX,"Tx depth"); 			  break;
			case 'u': user_param->qp_timeout = strtol(optarg, NULL, 0); 					  break;
			case 'S': CHECK_VALUE(user_param->sl,MIN_SL,MAX_SL,"SL num");					  break;
			case 'x': CHECK_VALUE(user_param->gid_index,MIN_GID_IX,MAX_GID_IX,"Gid index");   break;
			case 'Q': CHECK_VALUE(user_param->cq_mod,MIN_CQ_MOD,MAX_CQ_MOD,"CQ moderation");  break;
			case 'a': user_param->all = ON;											  		  break;
			case 'F': user_param->cpu_freq_f = ON; 											  break;
			case 'c': change_conn_type(&user_param->connection_type,user_param->verb,optarg); break;
			case 'V': printf("Version: %.2f\n",user_param->version); return VERSION_EXIT;
			case 'h': usage(argv[0],user_param->verb,user_param->tst); return 1;
			case 'z': user_param->use_rdma_cm = ON; break;
			case 'R': user_param->work_rdma_cm = ON; break;
			case 's': CHECK_VALUE(user_param->size,1,(UINT_MAX / 2),"Message size"); break;
			case 'q': CHECK_VALUE(user_param->num_of_qps,MIN_QP_NUM,MAX_QP_NUM,"num of Qps"); 
				if (user_param->verb != WRITE || user_param->tst != BW) {
					fprintf(stderr," Multiple QPs only availible on ib_write_bw and ib_write_bw_postlist\n");
                    return 1;
				} break;
				
			case 'e': user_param->use_event = ON;
				if (user_param->verb == WRITE) {
					fprintf(stderr," Events feature not availible on WRITE verb\n");
                    return 1;
                } break;

            case 'I': CHECK_VALUE(user_param->inline_size,MIN_INLINE,MAX_INLINE,"Inline size");
				if (user_param->verb == READ) {
					fprintf(stderr," Inline feature not availible on READ verb\n");
                    return 1;
				} break;

            case 'o': user_param->out_reads = strtol(optarg, NULL, 0);
				if (user_param->verb != READ && user_param->verb != ATOMIC) {
					fprintf(stderr," Setting Outstanding reads only availible on READ verb\n");
                    return 1;
                } break;
            
            case 'g': user_param->use_mcg = ON;
				CHECK_VALUE(user_param->num_of_qps,MIN_QP_MCAST,MAX_QP_MCAST," Num of Mcast QP");
				if (user_param->verb != SEND) {
					fprintf(stderr," MultiCast feature only availible on SEND verb\n");
					return 1;
                } break;

			case 'M': GET_STRING(user_param->user_mgid,strdupa(optarg)); 
			    if (user_param->verb != SEND) {
                    fprintf(stderr," MultiCast feature only availible on SEND verb\n");
				    return 1;
                } break;

			case 'r': CHECK_VALUE(user_param->rx_depth,MIN_RX,MAX_RX," Rx depth");
				if (user_param->verb != SEND && user_param->rx_depth > DEF_RX_RDMA) {
					fprintf(stderr," On RDMA verbs rx depth can be only 1\n");
					return 1;
                } break;

			case 'b': user_param->duplex = ON;
				if (user_param->tst == LAT) {
					fprintf(stderr," Bidirectional is only availible in BW test\n");
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
				user_param->r_flag->cycles = ON; break; 

			case 'A':
				if (user_param->verb != ATOMIC) {
					fprintf(stderr," You are not running the atomic_lat/bw test!\n");
					fprintf(stderr," To change the atomic action type, you must run one of the atomic tests\n");
					return 1;
                }

				if (strcmp(atomicTypesStr[0],optarg)==0) 
					user_param->atomicType = CMP_AND_SWAP;

				else if (strcmp(atomicTypesStr[0],optarg)==1) 
					user_param->atomicType = FETCH_AND_ADD;

				else {
					fprintf(stderr," Invalid Atomic type! please choose from {CMP_AND_SWAP,FETCH_AND_ADD}\n"); 
					exit(1);
				}
				break;

			case 'H': 
				if (user_param->tst != LAT) {
					fprintf(stderr," Availible only on Latency tests\n");
					return 1;
                }
				user_param->r_flag->histogram = ON;	break;
	
            case 'U': 
				if (user_param->tst != LAT) {
					fprintf(stderr," Availible only on Latency tests\n");
					return 1;
                } 
				user_param->r_flag->unsorted = ON; break;

			default: 
				fprintf(stderr," Invalid Command or flag.\n"); 
				fprintf(stderr," Please check command line and run again.\n\n");
				usage(argv[0],user_param->verb,user_param->tst);
				return 1;
        }
    }

    if (optind == argc - 1) {
		GET_STRING(user_param->servername,strdupa(argv[optind])); 

	} else if (optind < argc) {
            fprintf(stderr," Invalid Command line. Please check command rerun \n"); 
            return 1;
    }

	user_param->machine = user_param->servername ? CLIENT : SERVER;
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

	user_param->curr_mtu = set_mtu(context,user_param->ib_port,user_param->mtu);

	if (is_dev_hermon(context) != HERMON && user_param->inline_size != 0)
		user_param->inline_size = 0;

	if (user_param->verb == READ || user_param->verb == ATOMIC)
		user_param->out_reads = ctx_set_out_reads(context,user_param->out_reads);


	if (user_param->connection_type == UD && 
		user_param->size > MTU_SIZE(user_param->curr_mtu)) {

		if (user_param->all == OFF) {
			fprintf(stderr," Max msg size in UD is MTU %d\n",MTU_SIZE(user_param->curr_mtu));
			fprintf(stderr," Changing to this MTU\n");
		}
		user_param->size = MTU_SIZE(user_param->curr_mtu);
	}

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

		if (user_param->spec) {
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

	printf(" Number of qps   : %d\n",user_param->num_of_qps);
	printf(" Connection type : %s\n",connStr[user_param->connection_type]);
	
	if (user_param->machine == CLIENT || user_param->duplex) {
		printf(" TX depth        : %d\n",user_param->tx_depth);

	} 

	if (user_param->verb == SEND && (user_param->machine == SERVER || user_param->duplex)) { 
		printf(" RX depth        : %d\n",user_param->rx_depth);
	}

	if (user_param->tst == BW || user_param->verb == SEND) {
		printf(" CQ Moderation   : %d\n",user_param->cq_mod);
	} 

	printf(" Mtu             : %dB\n",MTU_SIZE(user_param->curr_mtu));
	printf(" Link type       : %s\n" ,link_layer_str(user_param->link_type));

	if (user_param->gid_index != DEF_GID_INDEX)
		printf(" Gid index       : %d\n" ,user_param->gid_index);

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
 * End
 ******************************************************************************/
