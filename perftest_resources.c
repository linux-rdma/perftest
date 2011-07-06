
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <limits.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <math.h>
// #include <byteswap.h>
#include "perftest_resources.h"


/****************************************************************************** 
 *
 ******************************************************************************/

static const char *sideArray[]  = {"local", "remote"};

static const char *gidArray[]   =  {"GID", "MGID"};

static const char *portStates[] = {"Nop","Down","Init","Armed","","Active Defer"};

static const char *testsStr[]   = {"Send","RDMA_Write","RDMA_Read"};

static const char *connStr[]    = {"RC","UC","UD"};


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
Device is_dev_hermon(struct ibv_context *context) { 

	Device is_hermon = NOT_HERMON;
	struct ibv_device_attr attr;

	if (ibv_query_device(context,&attr)) {
		is_hermon = FAILURE;
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
static int ctx_write_keys(const struct pingpong_dest *my_dest,
						  struct perftest_parameters *params) {

    if (params->gid_index == -1 && !params->use_mcg) {

		char msg[KEY_MSG_SIZE];
		sprintf(msg,KEY_PRINT_FMT,my_dest->lid,my_dest->out_reads,
				my_dest->qpn,my_dest->psn, my_dest->rkey, my_dest->vaddr);
		if (write(params->sockfd,msg,sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return -1;
		}

    } else {

		char msg[KEY_MSG_SIZE_GID];
    
		sprintf(msg,KEY_PRINT_FMT_GID, my_dest->lid,my_dest->out_reads,
				my_dest->qpn,my_dest->psn, my_dest->rkey, my_dest->vaddr,
				my_dest->gid.raw[0],my_dest->gid.raw[1],
				my_dest->gid.raw[2],my_dest->gid.raw[3],
				my_dest->gid.raw[4],my_dest->gid.raw[5],
				my_dest->gid.raw[6],my_dest->gid.raw[7],
				my_dest->gid.raw[8],my_dest->gid.raw[9],
				my_dest->gid.raw[10],my_dest->gid.raw[11],
				my_dest->gid.raw[12],my_dest->gid.raw[13],
				my_dest->gid.raw[14],my_dest->gid.raw[15]);
		if (write(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return -1;
		}	
	}
    return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
static int ctx_read_keys(struct pingpong_dest *rem_dest, 
                         struct perftest_parameters *params)  {
    
	if (params->gid_index == -1 && !params->use_mcg) {

        int parsed;
		char msg[KEY_MSG_SIZE];

		if (read(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return -1;
		}

		parsed = sscanf(msg,KEY_PRINT_FMT,&rem_dest->lid,
						&rem_dest->out_reads,&rem_dest->qpn,
						&rem_dest->psn, &rem_dest->rkey,&rem_dest->vaddr);

		if (parsed != 6) {
			fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg, msg);
			return -1;
		}
        
	} else {

		char msg[KEY_MSG_SIZE_GID];
		char *pstr = msg, *term;
		char tmp[20];
		int i;

		if (read(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return -1;
		}
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->lid = (int)strtol(tmp, NULL, 16); // LID

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->out_reads = (int)strtol(tmp, NULL, 16); // OUT_READS

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->qpn = (int)strtol(tmp, NULL, 16); // QPN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->psn = (int)strtol(tmp, NULL, 16); // PSN

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->rkey = (unsigned)strtoul(tmp, NULL, 16); // RKEY

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->vaddr = strtoull(tmp, NULL, 16); // VA

		for (i = 0; i < 15; ++i) {
			pstr += term - pstr + 1;
			term = strpbrk(pstr, ":");
			memcpy(tmp, pstr, term - pstr);
			tmp[term - pstr] = 0;
			rem_dest->gid.raw[i] = (unsigned char)strtoll(tmp, NULL, 16);
		}
		pstr += term - pstr + 1;
		strcpy(tmp, pstr);
		rem_dest->gid.raw[15] = (unsigned char)strtoll(tmp, NULL, 16);
	}
	return 0;
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

	printf("  -p, --port=<port> ");
	printf(" Listen on/connect to port <port> (default %d)\n",DEF_PORT);

	printf("  -d, --ib-dev=<dev> ");
	printf(" Use IB device <dev> (default first device found)\n");

	printf("  -i, --ib-port=<port> ");
	printf(" Use port <port> of IB device (default %d)\n",DEF_IB_PORT);

	printf("  -c, --connection=<RC/UC/UD> ");
	printf(" Connection type RC/UC/UD (default RC)\n");

	printf("  -m, --mtu=<mtu> ");
	printf(" Mtu size : 256 - 4096 (default port mtu)\n");

	printf("  -s, --size=<size> ");
	printf(" Size of message to exchange (default %d)\n",tst == LAT ? DEF_SIZE_LAT : DEF_SIZE_BW);

	printf("  -a, --all ");
	printf(" Run sizes from 2 till 2^23\n");

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

	if (verb != READ) {
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

	if (verb == READ) {
		printf("  -o, --outs=<num> ");
		printf(" num of outstanding read/atom(default max of device)\n");
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
}

/******************************************************************************
 *
 ******************************************************************************/
static void change_conn_type(int *cptr,VerbType verb,const char *optarg) {

	if (strcmp(connStr[0],optarg)==0) 
		*cptr = RC;

	else if (strcmp(connStr[1],optarg)==0) { 
		*cptr = UC;
		if (verb == READ) { 
			  fprintf(stderr," UC connection not possible in READ verb\n"); 
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
            { 0 }
        };

        c = getopt_long(argc,argv,"p:d:i:m:o:c:s:g:n:t:I:r:u:q:S:x:M:Q:lVaehbNFCHU",long_options,NULL);

        if (c == -1)
			break;

        switch (c) {

			case 'p': user_param->port = strtol(optarg, NULL, 0);                			  break;
			case 'd': GET_STRING(user_param->ib_devname,strdupa(optarg)); 			  	  	  break;
			case 'i': CHECK_VALUE(user_param->ib_port,MIN_IB_PORT,MAX_IB_PORT,"IB Port"); 	  break;
            case 'm': user_param->mtu  = strtol(optarg, NULL, 0); 							  break;
			case 's': CHECK_VALUE(user_param->size,1,(UINT_MAX / 2),"Message size"); 		  break;
			case 'n': CHECK_VALUE(user_param->iters,MIN_ITER,MAX_ITER,"Iteration num"); 	  break;
			case 't': CHECK_VALUE(user_param->tx_depth,MIN_TX,MAX_TX,"Tx depth"); 			  break;
			case 'u': user_param->qp_timeout = strtol(optarg, NULL, 0); 					  break;
			case 'S': CHECK_VALUE(user_param->sl,MIN_SL,MAX_SL,"SL num");					  break;
			case 'x': CHECK_VALUE(user_param->gid_index,MIN_GID_IX,MAX_GID_IX,"Gid index");   break;
			case 'Q': CHECK_VALUE(user_param->cq_mod,MIN_CQ_MOD,MAX_CQ_MOD,"CQ moderation");  break;
			case 'a': user_param->all 		 = ON;											  break;
			case 'F': user_param->cpu_freq_f = ON; 											  break;
			case 'c': change_conn_type(&user_param->connection_type,user_param->verb,optarg); break;
			case 'V': printf("Version: %.2f\n",user_param->version); return 1;
			case 'h': usage(argv[0],user_param->verb,user_param->tst); return 1;

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
				if (user_param->verb != READ) {
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

	// Assign server / clients roles.
	user_param->machine = user_param->servername ? CLIENT : SERVER;

    return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
void ctx_print_test_info(struct perftest_parameters *user_param) {

	printf(RESULT_LINE);
	printf("                    ");
	printf("%s ",testsStr[user_param->verb]);

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

	if (user_param->use_mcg) {
		printf("Multicast ");

		if (user_param->duplex) {
			user_param->num_of_qps++;

		} else if (user_param->machine == CLIENT) {
			user_param->num_of_qps = 1;
		}
	}

	printf("Test\n");

	if (user_param->use_event) {
		printf(" Test with events.\n");
		
	}
	if (user_param->use_mcg && user_param->connection_type != UD) {
		printf(" MultiCast only runs on UD ! changing to UD\n");
		user_param->connection_type = UD;

	}

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
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_device* ctx_find_dev(const char *ib_devname) {

	int num_of_device;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev = NULL;

	dev_list = ibv_get_device_list(&num_of_device);

	if (num_of_device <= 0) {
		fprintf(stderr," Did not detect devices \n");
		fprintf(stderr," If device exists, check if driver is up\n");
		return NULL;
	}

	if (!ib_devname) {
		ib_dev = dev_list[0];
		if (!ib_dev)
			fprintf(stderr, "No IB devices found\n");
	} else {
		for (; (ib_dev = *dev_list); ++dev_list)
			if (!strcmp(ibv_get_device_name(ib_dev), ib_devname))
				break;
		if (!ib_dev)
			fprintf(stderr, "IB device %s not found\n", ib_devname);
	}
	return ib_dev;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_set_mtu(struct ibv_context *context,struct perftest_parameters *params) {

	struct ibv_port_attr port_attr;

	if (ibv_query_port(context,params->ib_port,&port_attr)) {
		fprintf(stderr,"Unable to query port\n");
		return -1;
	}

	// User did not ask for specific mtu.
	if (params->mtu == 0) {
		params->curr_mtu = port_attr.active_mtu;

	} else {

		switch (params->mtu) {

			case 256  :	params->curr_mtu = IBV_MTU_256;	 break;
			case 512  : params->curr_mtu = IBV_MTU_512;	 break;
			case 1024 :	params->curr_mtu = IBV_MTU_1024; break;
			case 2048 :	params->curr_mtu = IBV_MTU_2048; break;
			case 4096 :	params->curr_mtu = IBV_MTU_4096; break;
			default   :	
				fprintf(stderr," Invalid MTU - %d \n",params->mtu);
				fprintf(stderr," Please choose mtu form {256,512,1024,2048,4096}\n");
				return -1;
		}
		
		if (params->curr_mtu > port_attr.active_mtu) {
			fprintf(stdout,"Requested mtu is higher than active mtu \n");
			fprintf(stdout,"Changing to active mtu \n");
			params->curr_mtu = port_attr.active_mtu;
		}
	}

	printf(" Mtu             : %d\n",MTU_SIZE(params->curr_mtu)); 
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_set_link_layer(struct ibv_context *context,
					   struct perftest_parameters *params) {

	struct ibv_port_attr port_attr;

	if (ibv_query_port(context,params->ib_port,&port_attr)) {
		fprintf(stderr,"Unable to query port attributes\n");
		return -1;
	}

	if (port_attr.state != IBV_PORT_ACTIVE) {
		fprintf(stderr," Port number %d state is %s\n"
					  ,params->ib_port
					  ,portStates[port_attr.state]);
		return -1;
	}

	params->link_type = port_attr.link_layer; 

	if (!strcmp(link_layer_str(params->link_type),"Unknown")) {
		fprintf(stderr," Unable to determine link layer \n");
		return -1;
	}
	else {
		printf(" Link type       : %s\n",link_layer_str(params->link_type));
	}
	
	if (!strcmp(link_layer_str(params->link_type),"Ethernet") &&  params->gid_index == -1) {
			params->gid_index = 0;
	}

	if (params->use_mcg &&  params->gid_index == -1) {
			params->gid_index = 0;
	}

	if (params->gid_index > -1 && (params->machine == CLIENT || params->duplex)) {
		fprintf(stdout," Gid index       : %d\n",params->gid_index);
	}

	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_cq* ctx_cq_create(struct ibv_context *context, 
							 struct ibv_comp_channel *channel,
							 struct perftest_parameters *param) {

	int cq_depth;
	struct ibv_cq *curr_cq = NULL;

	if (param->verb == WRITE || param->verb == READ)
		cq_depth = param->tx_depth*param->num_of_qps;

	else if (param->duplex) 		
		 cq_depth = param->tx_depth + param->rx_depth*(param->num_of_qps); 
	else if (param->machine == CLIENT) 
		cq_depth = param->tx_depth;

	else 
		cq_depth = param->rx_depth*param->num_of_qps;

	curr_cq = ibv_create_cq(context,cq_depth,NULL,channel,0);

	return curr_cq;
}

/****************************************************************************** 
 *
 ******************************************************************************/
struct ibv_qp* ctx_qp_create(struct ibv_pd *pd,
							 struct ibv_cq *send_cq,
							 struct ibv_cq *recv_cq,
							 struct perftest_parameters *param) {
	
	struct ibv_qp_init_attr attr;
	struct ibv_qp* qp = NULL;
	
	memset(&attr, 0, sizeof(struct ibv_qp_init_attr));
	attr.send_cq = send_cq;
	attr.recv_cq = recv_cq; 
	attr.cap.max_send_wr  = param->tx_depth;
	attr.cap.max_recv_wr  = param->rx_depth;
	attr.cap.max_send_sge = MAX_SEND_SGE;
	attr.cap.max_recv_sge = MAX_RECV_SGE;
	attr.cap.max_inline_data = param->inline_size;
		
	switch (param->connection_type) {
		case RC : attr.qp_type = IBV_QPT_RC; break;
		case UC : attr.qp_type = IBV_QPT_UC; break;
		case UD : attr.qp_type = IBV_QPT_UD; break;
		default:  fprintf(stderr, "Unknown connection type \n");
				  return NULL;
	}

	qp = ibv_create_qp(pd,&attr);
	if (!qp)  {
		fprintf(stderr, "Couldn't create QP\n");
		return NULL;
	}
	return qp;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_modify_qp_to_init(struct ibv_qp *qp,struct perftest_parameters *param)  {

    struct ibv_qp_attr attr;
    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT;
    
    memset(&attr, 0, sizeof(struct ibv_qp_attr));
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = param->ib_port;
    
    if (param->connection_type == UD) {
		// assert(param->verb == SEND);
		attr.qkey = DEF_QKEY;
		flags |= IBV_QP_QKEY;

	} else {
		switch(param->verb) {
			case READ  : attr.qp_access_flags = IBV_ACCESS_REMOTE_READ;  break;
			case WRITE : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE; break;
			case SEND  : attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE	|
											    IBV_ACCESS_LOCAL_WRITE;
		}
		flags |= IBV_QP_ACCESS_FLAGS;
    }
    
	if (ibv_modify_qp(qp,&attr,flags))  {
		fprintf(stderr, "Failed to modify QP to INIT\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
uint16_t ctx_get_local_lid(struct ibv_context *context,int port) {

    struct ibv_port_attr attr;

    if (ibv_query_port(context,port,&attr))
	return 0;

    return attr.lid;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_set_out_reads(struct ibv_context *context,int num_user_reads) {


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

	printf(" Outstand reads  : %d\n",num_user_reads);

	return num_user_reads;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_client_connect(const char *servername,int port) {
    
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int n;
	int sockfd = -1;

	if (asprintf(&service, "%d", port) < 0)
		return -1;

	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		return n;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return sockfd;
	}
	return sockfd;
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_server_connect(int port)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	int sockfd = -1, connfd;
	int n;

	if (asprintf(&service, "%d", port) < 0)
		return -1;

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		return n;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return sockfd;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);
	if (connfd < 0) {
		perror("server accept");
		fprintf(stderr, "accept() failed\n");
		close(sockfd);
		return connfd;
	}

	close(sockfd);
	return connfd;
}


/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_hand_shake(struct perftest_parameters  *params,
				   struct pingpong_dest *my_dest,
				   struct pingpong_dest *rem_dest) {

    // Client.
    if (params->machine == CLIENT) {
		if (ctx_write_keys(my_dest,params)) {
			fprintf(stderr,"Unable to write on the socket\n");
			return -1;
		}
		if (ctx_read_keys(rem_dest,params)) {
			fprintf(stderr,"Unable to Read from the socket\n");
			return -1;
		}
    }
    // Server.
    else {
		if (ctx_read_keys(rem_dest,params)) {
			fprintf(stderr,"Unable to Read from the socket\n");
			return -1;
		}
		if (ctx_write_keys(my_dest,params)) {
			fprintf(stderr,"Unable to write on the socket\n");
			return -1;
		}
    }
    return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
void ctx_print_pingpong_data(struct pingpong_dest *element,
							 struct perftest_parameters *params) {

	int is_there_mgid,local_mgid,remote_mgid;

	// First of all we print the basic format.
    printf(BASIC_ADDR_FMT,sideArray[params->side],element->lid,element->qpn,element->psn);

	switch (params->verb) {

		case READ  : printf(READ_FMT,element->out_reads);
		case WRITE : printf(RDMA_FMT,element->rkey,element->vaddr);
		default    : putchar('\n');
	}

	local_mgid    = (params->side == LOCAL)  && (params->machine == SERVER);
	remote_mgid   = (params->side == REMOTE) && (params->machine == CLIENT);
	is_there_mgid =  params->duplex || remote_mgid || local_mgid;

	if (params->gid_index > -1 || (params->use_mcg && is_there_mgid)) {

		printf(GID_FMT,gidArray[params->use_mcg && is_there_mgid],
				element->gid.raw[0], element->gid.raw[1],
				element->gid.raw[2], element->gid.raw[3], 
			    element->gid.raw[4], element->gid.raw[5], 
			    element->gid.raw[6], element->gid.raw[7],
			   	element->gid.raw[8], element->gid.raw[9],
			    element->gid.raw[10],element->gid.raw[11],
			    element->gid.raw[12],element->gid.raw[13],
				element->gid.raw[14],element->gid.raw[15]);
	}
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline int ctx_notify_events(struct ibv_cq *cq,struct ibv_comp_channel *channel) {

	struct ibv_cq 		*ev_cq;
	void          		*ev_ctx;

	if (ibv_get_cq_event(channel,&ev_cq,&ev_ctx)) {
		fprintf(stderr, "Failed to get cq_event\n");
		return 1;
	}

	if (ev_cq != cq) {
		fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
		return 1;
	}

	ibv_ack_cq_events(cq,1);

	if (ibv_req_notify_cq(cq, 0)) {
		fprintf(stderr, "Couldn't request CQ notification\n");
		return 1;
	}
	return 0;
}

/****************************************************************************** 
 *
 ******************************************************************************/
inline void	increase_rem_addr(struct ibv_send_wr *wr,int size,
							  int scnt,uint64_t prim_addr) {

	wr->wr.rdma.remote_addr += INC(size);		    

	if( ((scnt+1) % (CYCLE_BUFFER/ INC(size))) == 0)
		wr->wr.rdma.remote_addr = prim_addr;
}
		     		
/****************************************************************************** 
 *
 ******************************************************************************/
inline void increase_loc_addr(struct ibv_sge *sg,int size,int rcnt,
							  uint64_t prim_addr,int server_is_ud) {


	if (server_is_ud) 
		sg->addr -= (CACHE_LINE_SIZE - UD_ADDITION);

	sg->addr  += INC(size);

    if( ((rcnt+1) % (CYCLE_BUFFER/ INC(size))) == 0 )
		sg->addr = prim_addr;

    if (server_is_ud) 
		sg->addr += (CACHE_LINE_SIZE - UD_ADDITION);
}

/****************************************************************************** 
 *
 ******************************************************************************/
int ctx_close_connection(struct perftest_parameters  *params,
				         struct pingpong_dest *my_dest,
				         struct pingpong_dest *rem_dest) {


	// Signal client is finished.
    if (ctx_hand_shake(params,my_dest,rem_dest)) {
        return -1;
        
    }

	// Close the Socket file descriptor.
	if (write(params->sockfd,"done",sizeof "done") != sizeof "done") {
		perror(" Client write");
		fprintf(stderr,"Couldn't write to socket\n");
		return -1;
	}
	close(params->sockfd);
	return 0;
}
/****************************************************************************** 
 * End
 ******************************************************************************/
