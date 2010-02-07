
/************************************************************************ 
 *   Files and Modules included for work.	    					    *
 ************************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <byteswap.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include "multicast_resources.h"


/************************************************************************ 
 *
 ************************************************************************/
 
struct mcast_package {
	struct mcast_parameters *ptr_to_params;
	struct mcast_group *ptr_to_mcast;
	pthread_barrier_t *our_barrier;
};

/************************************************************************ 
 * 
 *  Umad registration methods.
 *
 ************************************************************************/

/****************************************
 * Function: prepare_mcast_mad
 ****************************************/
static void prepare_mcast_mad(u_int8_t method,
							  struct mcast_group *mcast_manager,
							  struct mcast_parameters *params,
							  struct sa_mad_packet_t *samad_packet)	 {

	u_int8_t *ptr;
	uint64_t comp_mask;	

	memset(samad_packet,0,sizeof(*samad_packet));

	/* prepare the MAD header. according to Table 145 in IB spec 1.2.1 */
	ptr = samad_packet->mad_header_buf; 
	ptr[0]                     = 0x01;					/* BaseVersion */
	ptr[1]                     = MANAGMENT_CLASS_SUBN_ADM;			/* MgmtClass */
	ptr[2]                     = 0x02; 					/* ClassVersion */
	ptr[3]                     = INSERTF(ptr[3], 0, method, 0, 7); 		/* Method */
	(*(u_int64_t *)(ptr + 8))  = htonll((u_int64_t)DEF_TRANS_ID);		/* TransactionID */
	(*(u_int16_t *)(ptr + 16)) = htons(SUBN_ADM_ATTR_MC_MEMBER_RECORD);	/* AttributeID */

	ptr = samad_packet->SubnetAdminData;

	memcpy(&ptr[0],mcast_manager->mgid.raw, 16);		  
	memcpy(&ptr[16],params->port_gid.raw, 16);

	(*(u_int32_t *)(ptr + 32)) = htonl(DEF_QKEY);
	(*(u_int16_t *)(ptr + 40)) = htons(params->pkey);
	ptr[39]                    = DEF_TCLASS;
	ptr[44]                    = INSERTF(ptr[44], 4, DEF_SL, 0, 4);
	ptr[44]                    = INSERTF(ptr[44], 0, DEF_FLOW_LABLE, 16, 4);
	ptr[45]                    = INSERTF(ptr[45], 0, DEF_FLOW_LABLE, 8, 8);
	ptr[46]                    = INSERTF(ptr[46], 0, DEF_FLOW_LABLE, 0, 8);
	ptr[48]                    = INSERTF(ptr[48], 0, MCMEMBER_JOINSTATE_FULL_MEMBER, 0, 4);

	comp_mask = SUBN_ADM_COMPMASK_MGID | SUBN_ADM_COMPMASK_PORT_GID | SUBN_ADM_COMPMASK_Q_KEY | 
				SUBN_ADM_COMPMASK_P_KEY | SUBN_ADM_COMPMASK_TCLASS | SUBN_ADM_COMPMASK_SL |
				SUBN_ADM_COMPMASK_FLOW_LABEL | SUBN_ADM_COMPMASK_JOIN_STATE;

	samad_packet->ComponentMask = htonll(comp_mask);
}

/*****************************************
* Function: check_mad_status
*****************************************/
static int check_mad_status(struct sa_mad_packet_t *samad_packet) {

	u_int8_t *ptr;
	u_int32_t user_trans_id;
	u_int16_t mad_header_status;

	ptr = samad_packet->mad_header_buf;
	
	// the upper 32 bits of TransactionID were set by the kernel 
	user_trans_id = ntohl(*(u_int32_t *)(ptr + 12)); 

	// check the TransactionID to make sure this is the response 
	// for the join/leave multicast group request we posted
	if (user_trans_id != DEF_TRANS_ID) {
		fprintf(stderr, "received a mad with TransactionID 0x%x, when expecting 0x%x\n", 
			(unsigned int)user_trans_id, (unsigned int)DEF_TRANS_ID);;
		return 1;
	}

	mad_header_status = 0x0;
	mad_header_status = INSERTF(mad_header_status, 8, ptr[4], 0, 7); 
	mad_header_status = INSERTF(mad_header_status, 0, ptr[5], 0, 8);

	if (mad_header_status) {
		fprintf(stderr,"received UMAD with an error: 0x%x\n", mad_header_status);
		return 1;
	}

	return 0;
}


/*****************************************
* Function: get_mlid_from_mad
*****************************************/
static void get_mlid_from_mad(struct sa_mad_packet_t *samad_packet,
							  uint16_t *mlid) {

	u_int8_t *ptr;

	ptr = samad_packet->SubnetAdminData;
	*mlid = ntohs(*(u_int16_t *)(ptr + 36));
}

/****************************************
 * Function: join_multicast_group
 ****************************************/
static int join_multicast_group(subn_adm_method method,
								struct mcast_group *mcast_manager,
								struct mcast_parameters *params) {

	int portid = -1;
	int agentid = -1;
	void *umad_buff = NULL;
	void *mad = NULL;
	struct ibv_port_attr port_attr;
	int length = MAD_SIZE;
	int test_result = 0;

	/* use casting to loose the "const char0 *" */
	portid = umad_open_port((char*)ibv_get_device_name(params->ib_dev),params->ib_port);
	if (portid < 0) {
		fprintf(stderr,"failed to open UMAD port %d\n",params->ib_port);
		goto cleanup;
	}

	agentid = umad_register(portid,MANAGMENT_CLASS_SUBN_ADM, 2, 0, 0);
	if (agentid < 0) {
		fprintf(stderr,"failed to register UMAD agent for MADs\n");
		goto cleanup;
	}

	umad_buff = umad_alloc(1, umad_size() + MAD_SIZE);
	if (!umad_buff) {
		fprintf(stderr, "failed to allocate MAD buffer\n");
		goto cleanup;
	}

	mad = umad_get_mad(umad_buff);
	prepare_mcast_mad(method,mcast_manager,params,(struct sa_mad_packet_t *)mad);

	if (ibv_query_port(params->ctx,params->ib_port,&port_attr)) {
		fprintf(stderr,"ibv_query_port failed\n");
		return 1;
	}

	if (umad_set_addr(umad_buff,port_attr.sm_lid,1,port_attr.sm_sl,QP1_WELL_KNOWN_Q_KEY) < 0) {
		fprintf(stderr, "failed to set the destination address of the SMP\n");
		goto cleanup;
	}

	if (umad_send(portid,agentid,umad_buff,MAD_SIZE,100,5) < 0) {
		fprintf(stderr, "failed to send MAD\n");
		goto cleanup;
	}

	if (umad_recv(portid,umad_buff,&length,5000) < 0) {
		fprintf(stderr, "failed to receive MAD response\n");
		goto cleanup;
	}

	if (check_mad_status((struct sa_mad_packet_t*)mad)) {
		fprintf(stderr, "failed to get mlid from MAD\n");
		goto cleanup;
	}

	//  "Join multicast group" message was sent 
	if (method == SUBN_ADM_METHOD_SET) {
		get_mlid_from_mad((struct sa_mad_packet_t*)mad,&mcast_manager->mlid);
		mcast_manager->mcast_state |= MCAST_IS_JOINED;

	//  "Leave multicast group" message was sent 
	} else { 
		mcast_manager->mcast_state &= ~MCAST_IS_JOINED;
	}

cleanup:
	if (umad_buff)
		umad_free(umad_buff);

	if (portid >= 0) {
		if (agentid >= 0) {
			if (umad_unregister(portid, agentid)) {
				fprintf(stderr, "failed to deregister UMAD agent for MADs\n");
				test_result = 1;
			}
		}

		if (umad_close_port(portid)) {
			fprintf(stderr, "failed to close UMAD portid\n");
			test_result = 1;
		}
	}

	return test_result;
}


/************************************************************************ 
 * Internal structures and static functions.
 ************************************************************************/

static int set_multicast_gid(struct mcast_group *mcast_manager,struct mcast_parameters *params) {

	uint8_t mcg_gid[16] = MCG_GID;
	const char *pstr = params->user_mgid;
	char *term = NULL;
	char tmp[20];
	int i;

	if (params->is_user_mgid == TRUE) {
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr+1);
		tmp[term - pstr] = 0;
		mcg_gid[0] = (unsigned char)strtoll(tmp, NULL, 0);
		for (i = 1; i < 15; ++i) {
			pstr += term - pstr + 1;
			term = strpbrk(pstr, ":");
			memcpy(tmp, pstr, term - pstr+1);
			tmp[term - pstr] = 0;
			mcg_gid[i] = (unsigned char)strtoll(tmp, NULL, 0);
		}
		pstr += term - pstr + 1;
		strcpy(tmp, pstr);
		mcg_gid[15] = (unsigned char)strtoll(tmp, NULL, 0);
	}

	memcpy(mcast_manager[0].mgid.raw,mcg_gid,16);
	for (i=1; i < params->num_of_groups; i++) {
		mcg_gid[15]++;
		memcpy(mcast_manager[i].mgid.raw,mcg_gid,16);
	}
	return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
static int run_iter_client(struct mcast_group *mcast_manager,
						   struct mcast_parameters *params) {

	int ne,scnt,ccnt;
	struct ibv_wc wc;
	struct ibv_send_wr *bad_wr;

	scnt = ccnt = 0;
	while (scnt < params->iterations || ccnt < params->iterations) {
		while (scnt < params->iterations && (scnt -ccnt) < params->tx_depth) {
			mcast_manager->posted[scnt++] = get_cycles();
			if (ibv_post_send(mcast_manager->send_qp,mcast_manager->wr,&bad_wr)) {
				fprintf(stderr, "Couldn't post send: scnt=%d\n",scnt);
				return 1;
			}
		}
		if (ccnt < params->iterations) {
			while (1) {
				ne = ibv_poll_cq(mcast_manager->mcg_cq,1,&wc);
				if (ne <= 0) break;
				if (wc.status != IBV_WC_SUCCESS) {
					printf("Failed to poll CQ successfully \n");
					return 1;
				}
				mcast_manager->completed[ccnt++] = get_cycles();
			}
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		}
	}
	return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
static int run_iter_server(struct mcast_group *mcast_manager,
						   struct mcast_parameters *params) {

	int i,j,ne,num_of_iters;
	int rcnt = 0;
	unsigned long scnt = 0;
	struct ibv_wc wc;
	struct ibv_recv_wr *bad_wr;
	cycles_t t;

	// Initilizing values of the time stamp arryas.
	for (i=0; i < params->num_qps_on_group; i++) {
		mcast_manager->recv_mcg[i].package_counter = 0;
	}
	num_of_iters = params->iterations*params->num_qps_on_group;
	while (rcnt < num_of_iters && scnt < MAX_POLL_ITERATION_TIMEOUT) {
		ne = ibv_poll_cq(mcast_manager->mcg_cq,1,&wc);
		if (ne > 0) {
			rcnt ++;
			scnt = 0;
			t = get_cycles();
			for (j=0; j < params->num_qps_on_group; j++) {
				if (wc.qp_num == mcast_manager->recv_mcg[j].qp->qp_num) {
					mcast_manager->recv_mcg[j].completion_array[mcast_manager->recv_mcg[j].package_counter++] = t;
					if (ibv_post_recv(mcast_manager->recv_mcg[j].qp,params->rwr,&bad_wr)) {
						fprintf(stderr, "Couldn't post recv: rcnt=%d\n",rcnt);
						return 1;
					}
					break;
				}
			}
		} else if (ne == 0) {
			scnt++;
		} else {
			fprintf(stderr, "Poll Recieve CQ failed\n");
			return 1;
		}
	}
	return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
static void* run_iter(void *arguments) {

	struct mcast_package *mcast = arguments;

	pthread_barrier_wait(mcast->our_barrier);
	if (mcast->ptr_to_params->is_client) {
		if (run_iter_client(mcast->ptr_to_mcast,mcast->ptr_to_params)) {
			fprintf(stderr,"Unable to send data\n");
		}
	}

	else {
		if (run_iter_server(mcast->ptr_to_mcast,mcast->ptr_to_params)) {
			fprintf(stderr,"Unable to send receive data\n");
		}
	}
	pthread_exit(NULL);
}




/************************************************************************ 
 *
 *   Multicast Resources methods implementation.			
 *
 ************************************************************************/
struct mcast_group* mcast_create_resources(struct mcast_parameters *params) {

	int i,j;
	struct mcast_group *new_mcast;

	ALLOCATE(new_mcast,struct mcast_group,params->num_of_groups);

	for (i=0; i < params->num_of_groups; i++) {
		// Client Side.
		if (params->is_client) {
			ALLOCATE(new_mcast[i].posted,cycles_t,params->iterations); 
			ALLOCATE(new_mcast[i].completed,cycles_t,params->iterations);

			// Server Side.
		} else {
			ALLOCATE(new_mcast[i].recv_mcg,struct mcg_qp,params->num_qps_on_group);
			new_mcast[i].ah = NULL;
			new_mcast[i].wr = NULL;
			new_mcast[i].posted = new_mcast[i].completed = NULL;
			for (j=0; j < params->num_qps_on_group; j++) {
				ALLOCATE(new_mcast[i].recv_mcg[j].completion_array,cycles_t,params->iterations);
				new_mcast[i].recv_mcg[j].package_counter = 0;
			}
		}
	}   
	return new_mcast;
}

/************************************************************************ 
 *
 ************************************************************************/
int mcast_init_resources(struct mcast_group *mcast_manager,
						 struct mcast_parameters *params,
						 struct ibv_qp_attr *attr,int flags) {

	int i,j;

	if (set_multicast_gid(mcast_manager,params)) {
		fprintf(stderr,"Unable to set mcast gids\n");
		return 1;
	}

	// mlid will be assigned to the new LID after the join 
	if (umad_init() < 0) {
		fprintf(stderr, "failed to init the UMAD library\n");
		return 1;
	}

	for (i=0; i < params->num_of_groups; i++) {

		if (join_multicast_group(SUBN_ADM_METHOD_SET,&mcast_manager[i],params)) {
			fprintf(stderr, "Failed to join mcast group no %d\n",i);
			return 1;
		}

		// Client side.
		if ( params->is_client) {
			if (ibv_modify_qp(mcast_manager[i].send_qp,attr,flags)) {
				fprintf(stderr, "Failed to modify UD Mcast QP to INIT\n");
				return 1;
			}
		}
		// Server side.
		else {
			for (j=0; j < params->num_qps_on_group; j++) {
				if (ibv_modify_qp(mcast_manager[i].recv_mcg[j].qp,attr,flags)) {
					fprintf(stderr, "Failed to modify UD Mcast QP to INIT\n");
					return 1;
				}
				if (ibv_attach_mcast(mcast_manager[i].recv_mcg[j].qp,&mcast_manager[i].mgid,mcast_manager[i].mlid)) {
					fprintf(stderr, "Couldn't attach QP to Multi group No. %d\n",i+1);
					return 1;
				}
				mcast_manager[i].mcast_state |= MCAST_IS_ATTACHED;
			}
		}
	}
	return 0;
}


/************************************************************************ 
 *
 ************************************************************************/
int mcast_create_qps(struct mcast_group *mcast_manager,
					 struct mcast_parameters *params,
					 struct ibv_qp_init_attr *attr) {

	int i,j;

	for (i=0; i < params->num_of_groups; i++) {
		attr->send_cq = mcast_manager[i].mcg_cq;
		attr->recv_cq = mcast_manager[i].mcg_cq;

		// Client Side.
		if (params->is_client) {
			mcast_manager[i].send_qp = ibv_create_qp(params->pd,attr);
			if (!mcast_manager[i].send_qp) {
				fprintf(stderr, "Couldn't create QP\n");
				return 1;
			}
		}
		// Server side.
		else {
			for (j=0; j < params->num_qps_on_group; j++) {
				mcast_manager[i].recv_mcg[j].qp = ibv_create_qp(params->pd,attr);
				if (!mcast_manager[i].recv_mcg[j].qp) {
					fprintf(stderr, "Couldn't create QP\n");
					return 1;
				}
			}
		}
	}
	return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
int mcast_modify_qp_to_rtr(struct mcast_group *mcast_manager,
						   struct mcast_parameters *params,
						   struct ibv_qp_attr *attr) {
	int i,j;

	attr->dest_qp_num = QPNUM_MCAST;
	attr->ah_attr.grh.hop_limit  = 1;
	attr->ah_attr.sl             = 0;
	attr->ah_attr.is_global      = 1;
	attr->ah_attr.grh.sgid_index = 0;
	for (i=0; i < params->num_of_groups; i++) {
		// Client side.
		if (params->is_client) {
			
			attr->ah_attr.dlid = mcast_manager[i].mlid;
			memcpy(attr->ah_attr.grh.dgid.raw,mcast_manager[i].mgid.raw,16);
			if (ibv_modify_qp(mcast_manager[i].send_qp,attr,IBV_QP_STATE)) {
				fprintf(stderr, "Failed to modify UD QP to RTR\n");
				return 1;
			}
			mcast_manager[i].ah = ibv_create_ah(params->pd,&attr->ah_attr);
			if (!mcast_manager[i].ah) {
				fprintf(stderr, "Failed to create AH for UD\n");
				return 1;
			}
		}
		// Server side.
		else {
			for (j=0; j < params->num_qps_on_group; j++) {
				if (ibv_modify_qp(mcast_manager[i].recv_mcg[j].qp,attr,IBV_QP_STATE)) {
					printf("Failed to modify Multicast QP to RTR %d\n",j+1);
					return 1;
				}
			}
		}
	}
	return 0;
}




/************************************************************************ 
 *
 ************************************************************************/
int mcast_post_receive(struct mcast_group *mcast_manager,
					   struct mcast_parameters *params) {

	int i,j,k;
	struct ibv_recv_wr  *bad_wr_recv;

	// Server side.
	if (!params->is_client) {
		for (i=0; i < params->num_of_groups; i++) {
			for (j=0; j < params->num_qps_on_group; j++) {
				params->rwr->wr_id = j;
				for (k=0; k < params->rx_depth; k++) {
					if (ibv_post_recv(mcast_manager[i].recv_mcg[j].qp,params->rwr,&bad_wr_recv)) {
						fprintf(stderr, "Couldn't post recv: counter=%d\n", j);
						return 14;
					}
				}
			}
		}
	}
	return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
int mcast_post_send(struct mcast_group *mcast_manager,
					struct mcast_parameters *params,
					struct ibv_sge *sge_list) {

	int i;

	if (params->is_client) {
		for (i=0; i < params->num_of_groups; i++) {
			ALLOCATE(mcast_manager[i].wr,struct ibv_send_wr,1);
			mcast_manager[i].wr->wr_id  = PINGPONG_SEND_WRID + i;
			mcast_manager[i].wr->sg_list = sge_list;
			mcast_manager[i].wr->num_sge = 1;
			mcast_manager[i].wr->opcode  = IBV_WR_SEND;
			mcast_manager[i].wr->next    = NULL;
			mcast_manager[i].wr->wr.ud.ah = mcast_manager[i].ah;
			mcast_manager[i].wr->wr.ud.remote_qkey = 0x11111111;
			mcast_manager[i].wr->wr.ud.remote_qpn =  QPNUM_MCAST;
			if (params->size > params->inline_size) {
				mcast_manager[i].wr->send_flags = IBV_SEND_SIGNALED;
			}
		}
	}
	return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
int mcast_run_iter_uni(struct mcast_group *mcast_manager,
					   struct mcast_parameters *params) {

	int i;
	pthread_barrier_t barrier;
	pthread_t *thread_array = NULL;
	struct mcast_package *current_mcast = NULL;
	void* (*ptr_to_func)(void*) = run_iter;

	ALLOCATE(thread_array,pthread_t,params->num_of_groups);
	ALLOCATE(current_mcast,struct mcast_package,params->num_of_groups);

	if (pthread_barrier_init(&barrier,NULL,params->num_of_groups+1)) {
		fprintf(stderr,"Couldn't create pthread barrier\n");
		return 1;
	}
	for (i=0; i < params->num_of_groups; i++) {
		current_mcast[i].ptr_to_params = params;
		current_mcast[i].ptr_to_mcast  = &(mcast_manager[i]);
		current_mcast[i].our_barrier   = &barrier;
		if (pthread_create(&(thread_array[i]),NULL,ptr_to_func,&current_mcast[i])) {
			printf("Error trying to create the new thread\n");
			return 1;
		}
	}
	pthread_barrier_wait(&barrier);
	for (i=0; i < params->num_of_groups; i++) {
		if (pthread_join(thread_array[i],NULL)) {
			printf("Problem using Pthreads\n");
			return 1;
		}
	}
	if (pthread_barrier_destroy(&barrier)) {
		fprintf(stderr,"Couldn't destroy the phtread barrier\n");
		return 1;
	}
	free(thread_array);
	free(current_mcast);
	return 0;
}


/************************************************************************ 
 *
 ************************************************************************/
void mcast_print_server(struct mcast_group *mcast_manager,
						struct mcast_parameters *params,
						int no_cpu_freq_fail) {

	int i,j;
	unsigned int pkg_cntr,lower;
	unsigned long packets_sum = 0;
	double cycles_to_units;
	struct mcg_qp current; 
	cycles_t max_first, min_last;

	current = mcast_manager[0].recv_mcg[0];
	pkg_cntr = current.package_counter  ? current.package_counter - 1 : 0;
	cycles_to_units = get_cpu_mhz(no_cpu_freq_fail) * 1000000;

	max_first = current.completion_array[0];
	min_last  = current.completion_array[pkg_cntr];

	for (i=0; i < params->num_of_groups; i++) {
		for (j=0; j < params->num_qps_on_group; j++) {
			current = mcast_manager[i].recv_mcg[j];
			if (current.completion_array[0] > max_first) {
				max_first = current.completion_array[0];
			}
			pkg_cntr = current.package_counter ? current.package_counter - 1: 0;
			if (current.completion_array[pkg_cntr] < min_last) {
				min_last = current.completion_array[pkg_cntr];
			}
		}
	}
	for (i=0; i < params->num_of_groups; i++) {
		for (j=0; j < params->num_qps_on_group; j++) {
			current = mcast_manager[i].recv_mcg[j];
			pkg_cntr = current.package_counter ? current.package_counter - 1: 0;
			lower = 0;
			while (current.completion_array[lower] < max_first && lower < pkg_cntr) {
				lower++;
			}
			while (pkg_cntr > 0 && current.completion_array[pkg_cntr] > min_last && pkg_cntr + 1 > lower) {
				pkg_cntr--;
			}
			packets_sum += (pkg_cntr - lower + 1);
		}
	}
	printf(" %d %6d %14.2lf %21.2f\n",params->size,params->iterations,
		   (double)packets_sum/(params->iterations*params->num_of_groups*params->num_qps_on_group),
		   packets_sum*params->size*cycles_to_units/(min_last - max_first) / 0x100000);
}

/************************************************************************ 
 *
 ************************************************************************/
void mcast_print_client(struct mcast_group *mcast_manager,
						struct mcast_parameters *params,
						int no_cpu_freq_fail) {

	double cycles_to_units;
	cycles_t max_first_posted,min_last_completed;
	int packets_sum = 0;
	int bottom,last_packet,i;

	last_packet = params->iterations - 1;
	max_first_posted = mcast_manager[0].posted[0];
	min_last_completed = mcast_manager[0].completed[last_packet];

	for (i=1; i < params->num_of_groups; i++) {
		if (mcast_manager[i].posted[0] > max_first_posted) {
			max_first_posted = mcast_manager[i].posted[0];
		}
		if (mcast_manager[i].completed[last_packet] < min_last_completed) {
			min_last_completed = mcast_manager[i].completed[last_packet];
		}
	}
	for (i=0; i < params->num_of_groups; i++) {
		bottom = 0;
		last_packet = params->iterations - 1;
		while (mcast_manager[i].posted[bottom] < max_first_posted &&
			   bottom < last_packet) bottom++;
		while (mcast_manager[i].completed[last_packet] > min_last_completed &&
			   last_packet > bottom + 1) last_packet--;
		packets_sum += last_packet - bottom + 1;
	}
	cycles_to_units = get_cpu_mhz(no_cpu_freq_fail) * 1000000;
	printf(" %d %6d %14.2lf %21.2f\n",params->size,params->iterations,
		   (double)packets_sum/(params->iterations*params->num_of_groups),
		   packets_sum*params->size*cycles_to_units/(min_last_completed - max_first_posted)/ 0x100000);
}


/************************************************************************ 
 *
 ************************************************************************/
int mcast_destroy_resources(struct mcast_group *mcast_manager,
							struct mcast_parameters *params) {


	int i,j;
	int test_result = 0;

	for (i=0; i < params->num_of_groups; i++) {
		if (params->is_client) {
			if (ibv_destroy_ah(mcast_manager[i].ah)) {
				fprintf(stderr, "failed to destroy AH\n");
				test_result = 1;
			}
			free(mcast_manager[i].wr);
			if (ibv_destroy_qp(mcast_manager[i].send_qp)) {
				fprintf(stderr, "failed to destroy QP\n");
				test_result = 1;
			}
		} else {
			for (j=0; j < params->num_qps_on_group; j++) {
				if (ibv_detach_mcast(mcast_manager[i].recv_mcg[j].qp,&mcast_manager[i].mgid,mcast_manager[i].mlid)) {
					fprintf(stderr, "failed to deatttached QP\n");
					test_result = 1;
				}
				if (ibv_destroy_qp(mcast_manager[i].recv_mcg[j].qp)) {
					fprintf(stderr, "failed to destroy QP\n");
					test_result = 1;
				}
				free(mcast_manager[i].recv_mcg[j].completion_array);
				mcast_manager[i].mcast_state &= ~MCAST_IS_ATTACHED;
			}
			free(mcast_manager[i].recv_mcg);
		}

		// leave the multicast group 
		if (mcast_manager[i].mcast_state & MCAST_IS_JOINED) {
			if (join_multicast_group(SUBN_ADM_METHOD_DELETE,&mcast_manager[i],params)) {
				fprintf(stderr, "failed to leave the mcast group\n");
				test_result = 1;
			}
		}

		if (ibv_destroy_cq(mcast_manager[i].mcg_cq)) {
			fprintf(stderr, "failed to destroy CQ\n");
			test_result = 1;
		}
	}
	free(mcast_manager);
	return test_result;
}
