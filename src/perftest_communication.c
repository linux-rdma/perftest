#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <getopt.h>
#if !defined(__FreeBSD__)
#include <malloc.h>
#include <byteswap.h>
#endif
#include <errno.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include "perftest_communication.h"
#include "host_memory.h"
#ifdef HAVE_SRD
#include <infiniband/efadv.h>
#endif
#if defined(__FreeBSD__)
#include <ctype.h>
#endif

#if defined(__FreeBSD__)
#define s6_addr32 __u6_addr.__u6_addr32
/* states: S_N: normal, S_I: comparing integral part, S_F: comparing
           fractional parts, S_Z: idem but with leading Zeroes only */
#define  S_N    0x0
#define  S_I    0x4
#define  S_F    0x8
#define  S_Z    0xC

/* result_type: CMP: return diff; LEN: compare using len_diff/diff */
#define  CMP    2
#define  LEN    3


/* Compare S1 and S2 as strings holding indices/version numbers,
   returning less than, equal to or greater than zero if S1 is less than,
   equal to or greater than S2 (for more info, see the Glibc texinfo doc).  */
// cppcheck-suppress unusedFunction
int strverscmp (const char *s1, const char *s2)
{
  const unsigned char *p1 = (const unsigned char *) s1;
  const unsigned char *p2 = (const unsigned char *) s2;
  unsigned char c1, c2;
  int state;
  int diff;

  /* Symbol(s)    0       [1-9]   others  (padding)
     Transition   (10) 0  (01) d  (00) x  (11) -   */
  static const unsigned int next_state[] =
    {
      /* state    x    d    0    - */
      /* S_N */  S_N, S_I, S_Z, S_N,
      /* S_I */  S_N, S_I, S_I, S_I,
      /* S_F */  S_N, S_F, S_F, S_F,
      /* S_Z */  S_N, S_F, S_Z, S_Z
    };

  static const int result_type[] =
    {
      /* state   x/x  x/d  x/0  x/-  d/x  d/d  d/0  d/-
                 0/x  0/d  0/0  0/-  -/x  -/d  -/0  -/- */

      /* S_N */  CMP, CMP, CMP, CMP, CMP, LEN, CMP, CMP,
                 CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
      /* S_I */  CMP, -1,  -1,  CMP, +1,  LEN, LEN, CMP,
                 +1,  LEN, LEN, CMP, CMP, CMP, CMP, CMP,
      /* S_F */  CMP, CMP, CMP, CMP, CMP, LEN, CMP, CMP,
                 CMP, CMP, CMP, CMP, CMP, CMP, CMP, CMP,
      /* S_Z */  CMP, +1,  +1,  CMP, -1,  CMP, CMP, CMP,
                 -1,  CMP, CMP, CMP
    };

  if (p1 == p2)
    return 0;

  c1 = *p1++;
  c2 = *p2++;
  /* Hint: '0' is a digit too.  */
  state = S_N | ((c1 == '0') + (isdigit (c1) != 0));

  while ((diff = c1 - c2) == 0 && c1 != '\0')
    {
      state = next_state[state];
      c1 = *p1++;
      c2 = *p2++;
      state |= (c1 == '0') + (isdigit (c1) != 0);
    }

  state = result_type[state << 2 | (((c2 == '0') + (isdigit (c2) != 0)))];

  switch (state)
    {
    case CMP:
      return diff;

    case LEN:
      while (isdigit (*p1++))
	if (!isdigit (*p2++))
	  return 1;

      return isdigit (*p2) ? -1 : diff;

    default:
      return state;
    }
}
#endif
/******************************************************************************
 *
 ******************************************************************************/

static const char *sideArray[]  = {"local", "remote"};
static const char *gidArray[]   = {"GID"  , "MGID"};

static inline int valid_mtu_size(int mtu_size)
{
	return !(mtu_size < IBV_MTU_256 || mtu_size > IBV_MTU_4096);
}

static inline int ipv6_addr_v4mapped(const struct in6_addr *a)
{
	return ((a->s6_addr32[0] | a->s6_addr32[1]) |
			(a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL ||
			/* IPv4 encoded multicast addresses */
			(a->s6_addr32[0] == htonl(0xff0e0000) &&
			((a->s6_addr32[1] |
			(a->s6_addr32[2] ^ htonl(0x0000ffff))) == 0UL));
}


/******************************************************************************
 *
 ******************************************************************************/
// cppcheck-suppress unusedFunction
double bswap_double(double x)
{
	union {
		double ddata;
		uint64_t u64data;
	} d1, d2;

	d1.ddata = x;
	d2.u64data = bswap_64(d1.u64data);
	return d2.ddata;
}

/******************************************************************************
 *
 ******************************************************************************/


static int post_one_recv_wqe(struct pingpong_context *ctx)
{
	struct ibv_recv_wr wr;
	struct ibv_recv_wr *bad_wr;
	struct ibv_sge list;

	list.addr   = (uintptr_t)ctx->buf[0];
	list.length = sizeof(struct pingpong_dest);
	list.lkey   = ctx->mr[0]->lkey;

	wr.next = NULL;
	wr.wr_id = SYNC_SPEC_ID;
	wr.sg_list = &list;
	wr.num_sge = 1;

	if (ibv_post_recv(ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_recv failed for RDMA_CM QP\n");
		return FAILURE;
	}

	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
static int post_recv_to_get_ah(struct pingpong_context *ctx)
{
	struct ibv_recv_wr wr;
	struct ibv_recv_wr *bad_wr;
	struct ibv_sge list;

	list.addr   = (uintptr_t)ctx->buf[0];
	list.length = UD_ADDITION + sizeof(uint32_t);
	list.lkey   = ctx->mr[0]->lkey;

	wr.next = NULL;
	wr.wr_id = 0;
	wr.sg_list = &list;
	wr.num_sge = 1;

	if (ibv_post_recv(ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_recv failed for RDMA_CM QP\n");
		return FAILURE;
	}

	return SUCCESS;

}

/******************************************************************************
 *
 ******************************************************************************/
static int send_qp_num_for_ah(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr;
	struct ibv_sge list;
	struct ibv_wc wc;
	int ne;

	memcpy(ctx->buf[0], &ctx->qp[0]->qp_num, sizeof(uint32_t));

	list.addr   = (uintptr_t)ctx->buf[0];
	list.length = sizeof(uint32_t);
	list.lkey   = ctx->mr[0]->lkey;

	wr.wr_id      = 0;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND_WITH_IMM;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.next       = NULL;
	wr.imm_data   = htonl(ctx->qp[0]->qp_num);

	wr.wr.ud.ah = ctx->ah[0];
	wr.wr.ud.remote_qpn  = user_param->rem_ud_qpn;
	wr.wr.ud.remote_qkey = user_param->rem_ud_qkey;


	if (ibv_post_send(ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_send failed\n");
		return 1;
	}

	do {
		ne = ibv_poll_cq(ctx->send_cq, 1,&wc);
	} while (ne == 0);

	//coverity[uninit_use]
	if (wc.status || wc.opcode != IBV_WC_SEND || wc.wr_id != 0) {
		fprintf(stderr, " Couldn't post send my QP number %d\n",(int)wc.status);
		return 1;
	}

	return 0;

}

/******************************************************************************
 *
 ******************************************************************************/
static int create_ah_from_wc_recv(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	struct ibv_qp_attr attr;
	struct ibv_qp_init_attr init_attr;
	struct ibv_wc wc;
	int ne;

	do {
		ne = ibv_poll_cq(ctx->recv_cq,1,&wc);
	} while (ne == 0);

	//coverity[uninit_use]
	if (wc.status || !(wc.opcode & IBV_WC_RECV) || wc.wr_id != 0) {
		fprintf(stderr, "Bad wc status when trying to create AH -- %d -- %d \n",(int)wc.status,(int)wc.wr_id);
		return 1;
	}

	ctx->ah[0] = ibv_create_ah_from_wc(ctx->pd, &wc, (struct ibv_grh*)ctx->buf[0], ctx->cm_id->port_num);
	user_param->ah_allocated = 1;
	user_param->rem_ud_qpn = ntohl(wc.imm_data);
	ibv_query_qp(ctx->qp[0],&attr, IBV_QP_QKEY,&init_attr);
	//coverity[uninit_use]
	user_param->rem_ud_qkey = attr.qkey;

	return 0;
}


/******************************************************************************
 *
 ******************************************************************************/
static int ethernet_write_keys(struct pingpong_dest *my_dest,
		struct perftest_comm *comm)
{
	if (my_dest->gid_index == -1) {

		char msg[KEY_MSG_SIZE];

		sprintf(msg,KEY_PRINT_FMT,my_dest->lid,my_dest->out_reads,
				my_dest->qpn,my_dest->psn, my_dest->rkey, my_dest->vaddr, my_dest->srqn);

		if (write(comm->rdma_params->sockfd,msg,sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return 1;
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
				my_dest->gid.raw[14],my_dest->gid.raw[15],
				my_dest->srqn);

		if (write(comm->rdma_params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return 1;
		}

	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ethernet_read_keys(struct pingpong_dest *rem_dest,
		struct perftest_comm *comm)
{
	if (rem_dest->gid_index == -1) {

		int parsed;
		char msg[KEY_MSG_SIZE];

		if (read(comm->rdma_params->sockfd, msg, sizeof msg) != sizeof msg) {
			fprintf(stderr, "ethernet_read_keys: Couldn't read remote address\n");
			return 1;
		}

		parsed = sscanf(msg,KEY_PRINT_FMT,(unsigned int*)&rem_dest->lid,
				(unsigned int*)&rem_dest->out_reads,(unsigned int*)&rem_dest->qpn,
				(unsigned int*)&rem_dest->psn, &rem_dest->rkey,&rem_dest->vaddr,&rem_dest->srqn);

		if (parsed != 7) {
			//coverity[string_null]
			fprintf(stderr, "Couldn't parse line <%.*s>\n",(int)sizeof msg, msg);
			return 1;
		}

	} else {

		char msg[KEY_MSG_SIZE_GID];
		char *pstr = msg, *term;
		char tmp[120];
		int i;

		if (read(comm->rdma_params->sockfd, msg, sizeof msg) != sizeof msg) {
			fprintf(stderr, "ethernet_read_keys: Couldn't read remote address\n");
			return 1;
		}

		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->lid = (int)strtol(tmp, NULL, 16); /*LID*/

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->out_reads = (int)strtol(tmp, NULL, 16); /*OUT_READS*/

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->qpn = (int)strtol(tmp, NULL, 16); /*QPN*/

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->psn = (int)strtol(tmp, NULL, 16); /*PSN*/

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->rkey = (unsigned)strtoul(tmp, NULL, 16); /*RKEY*/

		pstr += term - pstr + 1;
		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;

		rem_dest->vaddr = strtoull(tmp, NULL, 16); /*VA*/

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


		pstr += term - pstr + 4;

		term = strpbrk(pstr, ":");
		memcpy(tmp, pstr, term - pstr);
		tmp[term - pstr] = 0;
		rem_dest->srqn = (unsigned)strtoul(tmp, NULL, 16); /*SRQN*/

	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int rdma_write_keys(struct pingpong_dest *my_dest,
		struct perftest_comm *comm)
{
	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr;
	struct ibv_sge list;
	struct ibv_wc wc;
	int ne;

	#ifdef HAVE_ENDIAN
	int i;
	struct pingpong_dest m_my_dest;

	m_my_dest.lid 		= htobe32(my_dest->lid);
	m_my_dest.out_reads 	= htobe32(my_dest->out_reads);
	m_my_dest.qpn 		= htobe32(my_dest->qpn);
	m_my_dest.psn 		= htobe32(my_dest->psn);
	m_my_dest.rkey 		= htobe32(my_dest->rkey);
	m_my_dest.srqn		= htobe32(my_dest->srqn);
	m_my_dest.gid_index	= htobe32(my_dest->gid_index);
	m_my_dest.vaddr		= htobe64(my_dest->vaddr);

	for(i=0; i<16; i++) {
		m_my_dest.gid.raw[i] = my_dest->gid.raw[i];
	}

	memcpy(comm->rdma_ctx->buf[0], &m_my_dest, sizeof(struct pingpong_dest));
	#else
	memcpy(comm->rdma_ctx->buf[0], &my_dest, sizeof(struct pingpong_dest));
	#endif
	list.addr   = (uintptr_t)comm->rdma_ctx->buf[0];
	list.length = sizeof(struct pingpong_dest);
	list.lkey   = comm->rdma_ctx->mr[0]->lkey;


	wr.wr_id      = SYNC_SPEC_ID;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.next       = NULL;

	if (ibv_post_send(comm->rdma_ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_send failed\n");
		return 1;
	}

	do {
		ne = ibv_poll_cq(comm->rdma_ctx->send_cq, 1,&wc);
	} while (ne == 0);

	//coverity[uninit_use]
	if (wc.status || wc.opcode != IBV_WC_SEND || wc.wr_id != SYNC_SPEC_ID) {
		fprintf(stderr, " Bad wc status %d\n",(int)wc.status);
		return 1;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int rdma_read_keys(struct pingpong_dest *rem_dest,
		struct perftest_comm *comm)
{
	#ifdef HAVE_ENDIAN
	struct pingpong_dest a_rem_dest;
	#endif
	struct ibv_wc wc;
	int ne;

	do {
		ne = ibv_poll_cq(comm->rdma_ctx->recv_cq,1,&wc);
	} while (ne == 0);

	if (wc.status || !(wc.opcode & IBV_WC_RECV) || wc.wr_id != SYNC_SPEC_ID) {
		//coverity[uninit_use_in_call]
		fprintf(stderr, "Bad wc status -- %d -- %d \n",(int)wc.status,(int)wc.wr_id);
		return 1;
	}

	#ifdef HAVE_ENDIAN
	memcpy(&a_rem_dest,comm->rdma_ctx->buf[0],sizeof(struct pingpong_dest));
	rem_dest->lid   = ntohl(a_rem_dest.lid);
	rem_dest->out_reads     = ntohl(a_rem_dest.out_reads);
	rem_dest->qpn   = ntohl(a_rem_dest.qpn);
	rem_dest->psn   = ntohl(a_rem_dest.psn);
	rem_dest->rkey  = ntohl(a_rem_dest.rkey);

	rem_dest->vaddr         = be64toh(a_rem_dest.vaddr);
	memcpy(rem_dest->gid.raw, &(a_rem_dest.gid), 16*sizeof(uint8_t));
	#else
	memcpy(&rem_dest,comm->rdma_ctx->buf[0],sizeof(struct pingpong_dest));
	#endif

	if (post_one_recv_wqe(comm->rdma_ctx)) {
		fprintf(stderr, "Couldn't post send \n");
		return 1;
	}

	return 0;
}

#ifdef HAVE_GID_TYPE
#ifndef HAVE_GID_TYPE_DECLARED
enum ibv_gid_type
{
	IBV_GID_TYPE_IB_ROCE_V1,
	IBV_GID_TYPE_ROCE_V2,
};

int ibv_query_gid_type(struct ibv_context *context, uint8_t port_num,
	unsigned int index, enum ibv_gid_type *type);

#endif

enum who_is_better {LEFT_IS_BETTER, EQUAL, RIGHT_IS_BETTER};

struct roce_version_sorted_enum {
	enum ibv_gid_type type;
	int rate;
};

/* This struct defines which RoCE version is more important for default usage */
#ifndef HAVE_GID_TYPE_DECLARED
struct roce_version_sorted_enum roce_versions_sorted[] = {
	{IBV_GID_TYPE_IB_ROCE_V1, 1},
	{IBV_GID_TYPE_ROCE_V2, 2},
};
#else
struct roce_version_sorted_enum roce_versions_sorted[] = {
	{IBV_GID_TYPE_ROCE_V1, 1},
	{IBV_GID_TYPE_ROCE_V2, 2},
};
#endif

int find_roce_version_rate(enum ibv_gid_type roce_ver) {
	int i;
	int arr_len = GET_ARRAY_SIZE(roce_versions_sorted);

	for (i = 0; i < arr_len; i++)
	{
		if (roce_versions_sorted[i].type == roce_ver)
			return roce_versions_sorted[i].rate;
	}
	return -1;
}

/* RoCE V2 > V1
 * other RoCE versions will be ignored until added to roce_versions_sorted array */
static int check_better_roce_version(enum ibv_gid_type roce_ver, enum ibv_gid_type roce_ver_rival)
{
	int roce_ver_rate = find_roce_version_rate(roce_ver);
	int roce_ver_rate_rival = find_roce_version_rate(roce_ver_rival);

	if (roce_ver_rate < roce_ver_rate_rival)
		return RIGHT_IS_BETTER;
	else if (roce_ver_rate > roce_ver_rate_rival)
		return LEFT_IS_BETTER;
	else
		return EQUAL;
}
#endif

static int get_best_gid_index (struct pingpong_context *ctx,
		  struct perftest_parameters *user_param,
		  struct ibv_port_attr *attr, int port)
{
	int gid_index = 0, i;
	union ibv_gid temp_gid, temp_gid_rival;
	int is_ipv4, is_ipv4_rival;

	for (i = 1; i < attr->gid_tbl_len; i++) {
		if (ibv_query_gid(ctx->context, port, gid_index, &temp_gid)) {
			return -1;
		}

		if (ibv_query_gid(ctx->context, port, i, &temp_gid_rival)) {
			return -1;
		}

		is_ipv4 = ipv6_addr_v4mapped((struct in6_addr *)temp_gid.raw);
		is_ipv4_rival = ipv6_addr_v4mapped((struct in6_addr *)temp_gid_rival.raw);

		if (is_ipv4_rival && !is_ipv4 && !user_param->ipv6)
			gid_index = i;
		else if (!is_ipv4_rival && is_ipv4 && user_param->ipv6)
			gid_index = i;
#ifdef HAVE_GID_TYPE
		else {
#ifdef HAVE_GID_TYPE_DECLARED
			struct ibv_gid_entry roce_version, roce_version_rival;

			if (ibv_query_gid_ex(ctx->context, port, gid_index, &roce_version, 0))
				continue;

			if (ibv_query_gid_ex(ctx->context, port, i, &roce_version_rival, 0))
				continue;

			//coverity[uninit_use_in_call]
			if (check_better_roce_version(roce_version.gid_type, roce_version_rival.gid_type) == RIGHT_IS_BETTER) {
				gid_index = i;
			}

#else
			enum ibv_gid_type roce_version, roce_version_rival;

			if (ibv_query_gid_type(ctx->context, port, gid_index, &roce_version))
				continue;

			if (ibv_query_gid_type(ctx->context, port, i, &roce_version_rival))
				continue;

			if (check_better_roce_version(roce_version, roce_version_rival) == RIGHT_IS_BETTER) {
				gid_index = i;
			}
#endif
		}
#endif
	}
	return gid_index;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ethernet_client_connect(struct perftest_comm *comm)
{
	struct addrinfo *res, *t;
	struct addrinfo hints;
	char *service;
	struct sockaddr_in source;

	int sockfd = -1;
	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (comm->rdma_params->has_source_ip) {
		memset(&source, 0, sizeof(source));
		source.sin_family = AF_INET;
		source.sin_addr.s_addr = inet_addr(comm->rdma_params->source_ip);
	}

	if (check_add_port(&service,comm->rdma_params->port,comm->rdma_params->servername,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic address and port\n");
		return 1;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (comm->rdma_params->has_source_ip) {
				if (bind(sockfd, (struct sockaddr *)&source, sizeof(source)) < 0)
				{
					fprintf(stderr, "Failed to bind socket\n");
					close(sockfd);
					return 1;
				}
			}
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n",comm->rdma_params->servername,comm->rdma_params->port);
		return 1;
	}

	comm->rdma_params->sockfd = sockfd;
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
static int ethernet_server_connect(struct perftest_comm *comm)
{
	struct addrinfo *res, *t;
	struct addrinfo hints;
	char *service;
	int n;
	int sockfd = -1, connfd;
	char *src_ip = comm->rdma_params->has_source_ip ? comm->rdma_params->source_ip : NULL;

	memset(&hints, 0, sizeof hints);
	hints.ai_flags    = AI_PASSIVE;
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (check_add_port(&service,comm->rdma_params->port,src_ip,&hints,&res))
	{
		fprintf(stderr, "Problem in resolving basic address and port\n");
		return 1;
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
		fprintf(stderr, "Couldn't listen to port %d\n", comm->rdma_params->port);
		return 1;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);

	if (connfd < 0) {
		perror("server accept");
		fprintf(stderr, "accept() failed\n");
		close(sockfd);
		return 1;
	}
	close(sockfd);
	comm->rdma_params->sockfd = connfd;
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int set_up_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct pingpong_dest *my_dest)
{
	int num_of_qps = user_param->num_of_qps;
	int num_of_qps_per_port = user_param->num_of_qps / 2;
	int i;
	union ibv_gid temp_gid;
	union ibv_gid temp_gid2;
	struct ibv_port_attr attr;

	srand48(getpid() * time(NULL));

	/*in xrc with bidirectional,
	there are send qps and recv qps. the actual number of send/recv qps
	is num_of_qps / 2.
	*/
	if ( (user_param->connection_type == DC || user_param->use_xrc) && (user_param->duplex || user_param->tst == LAT)) {
		num_of_qps /= 2;
		num_of_qps_per_port = num_of_qps / 2;
	}

	if (user_param->gid_index != -1) {
		if (ibv_query_port(ctx->context, user_param->ib_port, &attr))
			return 0;

		if (user_param->use_gid_user) {
			if (ibv_query_gid(ctx->context, user_param->ib_port, user_param->gid_index, &temp_gid))
				return -1;
		} else {
			//coverity[uninit_use_in_call]
			user_param->gid_index = get_best_gid_index(ctx, user_param, &attr, user_param->ib_port);
			if (user_param->gid_index < 0)
				return -1;
			if (ibv_query_gid(ctx->context, user_param->ib_port, user_param->gid_index, &temp_gid))
				return -1;
		}
	}

	if (user_param->dualport == ON) {
		if (user_param->gid_index2 != -1) {
			if (ibv_query_port(ctx->context, user_param->ib_port2, &attr))
				return 0;

			if (user_param->use_gid_user) {
				if (ibv_query_gid(ctx->context, user_param->ib_port2, user_param->gid_index, &temp_gid2))
					return -1;
			} else {
				user_param->gid_index2 = get_best_gid_index(ctx, user_param, &attr, user_param->ib_port2);
				if (user_param->gid_index2 < 0)
					return -1;
				if (ibv_query_gid(ctx->context, user_param->ib_port2, user_param->gid_index2, &temp_gid2))
						return -1;
			}
		}
	}

	for (i = 0; i < user_param->num_of_qps; i++) {

		if (user_param->dualport == ON) {
			/*first half of qps are for ib_port and second half are for ib_port2
			in xrc with bidirectional, the first half of qps are xrc_send qps and
			the second half are xrc_recv qps. the first half of the send/recv qps
			are for ib_port1 and the second half are for ib_port2
			*/
			if (i % num_of_qps < num_of_qps_per_port) {
				my_dest[i].lid   = ctx_get_local_lid(ctx->context,user_param->ib_port);
				my_dest[i].gid_index = user_param->gid_index;
			} else {
				my_dest[i].lid   = ctx_get_local_lid(ctx->context,user_param->ib_port2);
				my_dest[i].gid_index = user_param->gid_index2;
			}
		} else {
			/*single-port case*/
			my_dest[i].lid   = ctx_get_local_lid(ctx->context,user_param->ib_port);
			my_dest[i].gid_index = user_param->gid_index;
		}

		my_dest[i].qpn   = ctx->qp[i]->qp_num;
		my_dest[i].psn   = lrand48() & 0xffffff;
		my_dest[i].rkey  = ctx->mr[i]->rkey;

		/* Each qp gives his receive buffer address.*/
		my_dest[i].out_reads = user_param->out_reads;
		if (user_param->mr_per_qp)
			my_dest[i].vaddr = (uintptr_t)ctx->buf[i] + BUFF_SIZE(ctx->size,ctx->cycle_buffer);
		else
			my_dest[i].vaddr = (uintptr_t)ctx->buf[0] + (user_param->num_of_qps + i)*BUFF_SIZE(ctx->size,ctx->cycle_buffer);

		if (user_param->dualport==ON) {

			if (i % num_of_qps < num_of_qps_per_port)
				memcpy(my_dest[i].gid.raw,temp_gid.raw ,16);

			else {
				//coverity[uninit_use_in_call]
				memcpy(my_dest[i].gid.raw,temp_gid2.raw ,16);
			}
		} else {
			//coverity[uninit_use_in_call]
			memcpy(my_dest[i].gid.raw,temp_gid.raw ,16);
		}
	}

	#ifdef HAVE_XRCD
	if (user_param->use_xrc) {
		for (i=0; i < user_param->num_of_qps; i++) {
			if (ibv_get_srq_num(ctx->srq,&(my_dest[i].srqn))) {
				fprintf(stderr, "Couldn't get SRQ number\n");
				return 1;
			}
		}
	}
	#endif

	if(user_param->machine == SERVER || user_param->duplex || user_param->tst == LAT) {
		if (user_param->connection_type == DC) {
			for (i=0; i < user_param->num_of_qps; i++) {
				if (ibv_get_srq_num(ctx->srq, &(my_dest[i].srqn))) {
					fprintf(stderr, "Couldn't get SRQ number\n");
					return 1;
				}
			}
		}
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int rdma_client_connect(struct pingpong_context *ctx,struct perftest_parameters *user_param)
{
	char *service;
	int temp,num_of_retry= NUM_OF_RETRIES;
	struct sockaddr_in sin, source_sin;
	struct sockaddr *source_ptr = NULL;
	struct addrinfo *res;
	struct rdma_cm_event *event;
	struct rdma_conn_param conn_param;
	struct addrinfo hints;

	memset(&hints, 0, sizeof hints);
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (check_add_port(&service,user_param->port,user_param->servername,&hints,&res)) {
		fprintf(stderr, "Problem in resolving basic address and port\n");
		return FAILURE;
	}

	if (res->ai_family != PF_INET) {
		freeaddrinfo(res);
		return FAILURE;
	}
	memcpy(&sin, res->ai_addr, sizeof(sin));
	freeaddrinfo(res);
	sin.sin_port = htons((unsigned short)user_param->port);

	if (user_param->has_source_ip) {
		if (check_add_port(&service, 0x0, user_param->source_ip, &hints, &res))
		{
			fprintf(stderr, "Problem in resolving basic address and port\n");
			return FAILURE;
		}
		memset(&source_sin, 0x0, sizeof(source_sin));
		//coverity[deref_after_free]
		memcpy(&source_sin, res->ai_addr, sizeof(source_sin));
		source_ptr = (struct sockaddr *)&source_sin;
		freeaddrinfo(res);
	}
	while (1)
	{

		if (num_of_retry == 0) {
			fprintf(stderr, "Received %d times ADDR_ERROR\n",NUM_OF_RETRIES);
			return FAILURE;
		}

		if (rdma_resolve_addr(ctx->cm_id, source_ptr, (struct sockaddr *)&sin, 2000)) {
			fprintf(stderr, "rdma_resolve_addr failed\n");
			return FAILURE;
		}

		if (rdma_get_cm_event(ctx->cm_channel,&event)) {
			fprintf(stderr, "rdma_get_cm_events failed\n");
			return FAILURE;
		}

		//coverity[uninit_use]
		if (event->event == RDMA_CM_EVENT_ADDR_ERROR) {
			num_of_retry--;
			rdma_ack_cm_event(event);
			continue;
		}

		if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
			fprintf(stderr, "unexpected CM event %d\n",event->event);
			rdma_ack_cm_event(event);
			return FAILURE;
		}

		rdma_ack_cm_event(event);
		break;
	}

	if (user_param->tos != DEF_TOS) {

		if (rdma_set_option(ctx->cm_id,RDMA_OPTION_ID,RDMA_OPTION_ID_TOS,&user_param->tos,sizeof(uint8_t))) {
			fprintf(stderr, " Set TOS option failed: %d\n",event->event);
			return FAILURE;
		}
	}

	while (1) {

		if (num_of_retry <= 0) {
			fprintf(stderr, "Received %d times ADDR_ERROR - aborting\n",NUM_OF_RETRIES);
			return FAILURE;
		}

		if (rdma_resolve_route(ctx->cm_id,2000)) {
			fprintf(stderr, "rdma_resolve_route failed\n");
			return FAILURE;
		}

		if (rdma_get_cm_event(ctx->cm_channel,&event)) {
			fprintf(stderr, "rdma_get_cm_events failed\n");
			return FAILURE;
		}

		if (event->event == RDMA_CM_EVENT_ROUTE_ERROR) {
			num_of_retry--;
			rdma_ack_cm_event(event);
			continue;
		}

		if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
			fprintf(stderr, "unexpected CM event %d\n",event->event);
			rdma_ack_cm_event(event);
			return FAILURE;
		}

		rdma_ack_cm_event(event);
		break;
	}

	ctx->context = ctx->cm_id->verbs;
	temp = user_param->work_rdma_cm;
	user_param->work_rdma_cm = ON;

	if (ctx_init(ctx, user_param)) {
		fprintf(stderr," Unable to create the resources needed by comm struct\n");
		return FAILURE;
	}

	memset(&conn_param, 0, sizeof conn_param);
	if (user_param->verb == READ || user_param->verb == ATOMIC) {
		conn_param.responder_resources = user_param->out_reads;
		conn_param.initiator_depth = user_param->out_reads;
	}
	user_param->work_rdma_cm = temp;
	conn_param.retry_count = user_param->retry_count;
	conn_param.rnr_retry_count = 7;

	if (user_param->work_rdma_cm == OFF) {

		if (post_one_recv_wqe(ctx)) {
			fprintf(stderr, "Couldn't post send \n");
			return 1;
		}
	}

	if (rdma_connect(ctx->cm_id,&conn_param)) {
		fprintf(stderr, "Function rdma_connect failed\n");
		return FAILURE;
	}

	if (rdma_get_cm_event(ctx->cm_channel,&event)) {
		fprintf(stderr, "rdma_get_cm_events failed\n");
		return FAILURE;
	}

	if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
		fprintf(stderr, "Unexpected CM event bl blka %d\n", event->event);
		rdma_ack_cm_event(event);
                return FAILURE;
	}

	if (user_param->connection_type == UD) {

		user_param->rem_ud_qpn  = event->param.ud.qp_num;
		user_param->rem_ud_qkey = event->param.ud.qkey;

		ctx->ah[0] = ibv_create_ah(ctx->pd,&event->param.ud.ah_attr);
		if (!ctx->ah[0]) {
			printf(" Unable to create address handler for UD QP\n");
			return FAILURE;
		}
		user_param->ah_allocated = 1;

		if (user_param->tst == LAT || (user_param->tst == BW && user_param->duplex)) {

			if (send_qp_num_for_ah(ctx,user_param)) {
				printf(" Unable to send my QP number\n");
				return FAILURE;
			}
		}
	}

	rdma_ack_cm_event(event);
	return SUCCESS;
}

/******************************************************************************
 *
 ******************************************************************************/
// cppcheck-suppress unusedFunction
int retry_rdma_connect(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int i, max_retries = 10;
	int delay = 100000; /* 100 millisec */

	for (i = 0; i < max_retries; i++) {
		if (create_rdma_resources(ctx,user_param)) {
			fprintf(stderr," Unable to create rdma resources\n");
			return FAILURE;
		}
		if (rdma_client_connect(ctx,user_param) == SUCCESS)
			return SUCCESS;
		if (destroy_rdma_resources(ctx,user_param)) {
			fprintf(stderr,"Unable to destroy rdma resources\n");
			return FAILURE;
		}
		usleep(delay);
	}
	fprintf(stderr,"Unable to connect (retries = %d)\n", max_retries);
	return FAILURE;
}

/******************************************************************************
  + *
  + ******************************************************************************/
int rdma_server_connect(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int temp;
	struct addrinfo *res;
	struct rdma_cm_event *event;
	struct rdma_conn_param conn_param;
	struct addrinfo hints;
	char *service;
	struct sockaddr_in sin;
	char* src_ip = user_param->has_source_ip ? user_param->source_ip : NULL;

	memset(&hints, 0, sizeof hints);
	hints.ai_flags    = AI_PASSIVE;
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

        memset(&sin, 0x0, sizeof(sin));

	if (check_add_port(&service,user_param->port,src_ip,&hints,&res))
	{
		fprintf(stderr, "Problem in resolving basic address and port\n");
		return FAILURE;
	}

	if (res->ai_family != PF_INET) {
		freeaddrinfo(res);
		return FAILURE;
	}
	memcpy(&sin, res->ai_addr, sizeof(sin));
	sin.sin_port = htons((unsigned short)user_param->port);
	freeaddrinfo(res);

	if (rdma_bind_addr(ctx->cm_id_control,(struct sockaddr *)&sin)) {
		fprintf(stderr," rdma_bind_addr failed\n");
		return 1;
	}

	if (rdma_listen(ctx->cm_id_control, user_param->num_of_qps)) {
		fprintf(stderr, "rdma_listen failed\n");
		return 1;
	}

	if (rdma_get_cm_event(ctx->cm_channel,&event)) {
		fprintf(stderr, "rdma_get_cm_events failed\n");
		return 1;
	}

	//coverity[uninit_use]
	if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
		fprintf(stderr, "bad event waiting for connect request %d\n",event->event);
		return 1;
	}

	ctx->cm_id = (struct rdma_cm_id*)event->id;
	ctx->context = ctx->cm_id->verbs;

	if (user_param->work_rdma_cm == ON)
		alloc_ctx(ctx,user_param);

	temp = user_param->work_rdma_cm;
	user_param->work_rdma_cm = ON;

	if (ctx_init(ctx,user_param)) {
		fprintf(stderr," Unable to create the resources needed by comm struct\n");
		return FAILURE;
	}

	memset(&conn_param, 0, sizeof conn_param);
	if (user_param->verb == READ || user_param->verb == ATOMIC) {
		conn_param.responder_resources = user_param->out_reads;
		conn_param.initiator_depth = user_param->out_reads;
	}
	if (user_param->connection_type == UD)
		conn_param.qp_num = ctx->qp[0]->qp_num;

	conn_param.retry_count = user_param->retry_count;
	conn_param.rnr_retry_count = 7;
	user_param->work_rdma_cm = temp;

	if (user_param->work_rdma_cm == OFF) {

		if (post_one_recv_wqe(ctx)) {
			fprintf(stderr, "Couldn't post send \n");
			return 1;
		}

	} else if (user_param->connection_type == UD) {

		if (user_param->tst == LAT || (user_param->tst == BW && user_param->duplex)) {

			if (post_recv_to_get_ah(ctx)) {
				fprintf(stderr, "Couldn't post send \n");
				return 1;
			}
		}
	}

	if (rdma_accept(ctx->cm_id, &conn_param)) {
		fprintf(stderr, "Function rdma_accept failed\n");
		return 1;
	}

	if (user_param->work_rdma_cm && user_param->connection_type == UD) {

		if (user_param->tst == LAT || (user_param->tst == BW && user_param->duplex)) {
			if (create_ah_from_wc_recv(ctx,user_param)) {
				fprintf(stderr, "Unable to create AH from WC\n");
				return 1;
			}
		}
	}

	rdma_ack_cm_event(event);
	rdma_destroy_id(ctx->cm_id_control);
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int create_comm_struct(struct perftest_comm *comm,
		struct perftest_parameters *user_param)
{
	MAIN_ALLOC(comm->rdma_params, struct perftest_parameters, 1, return_error);
	memset(comm->rdma_params, 0, sizeof(struct perftest_parameters));

	comm->rdma_params->port		   	= user_param->port;
	comm->rdma_params->sockfd      		= -1;
	comm->rdma_params->gid_index   		= user_param->gid_index;
	comm->rdma_params->gid_index2 		= user_param->gid_index2;
	comm->rdma_params->use_rdma_cm 		= user_param->use_rdma_cm;
	comm->rdma_params->servername  		= user_param->servername;
	comm->rdma_params->machine 	   	= user_param->machine;
	comm->rdma_params->side		   	= LOCAL;
	comm->rdma_params->verb		   	= user_param->verb;
	comm->rdma_params->use_mcg	   	= user_param->use_mcg;
	comm->rdma_params->duplex	   	= user_param->duplex;
	comm->rdma_params->tos         		= DEF_TOS;
	comm->rdma_params->use_xrc	   	= user_param->use_xrc;
	comm->rdma_params->connection_type	= user_param->connection_type;
	comm->rdma_params->output      		= user_param->output;
	comm->rdma_params->report_per_port 	= user_param->report_per_port;
	comm->rdma_params->retry_count		= user_param->retry_count;
	comm->rdma_params->mr_per_qp		= user_param->mr_per_qp;
	comm->rdma_params->dlid			= user_param->dlid;
	comm->rdma_params->cycle_buffer         = user_param->cycle_buffer;
	comm->rdma_params->use_old_post_send	= user_param->use_old_post_send;
	comm->rdma_params->source_ip		= user_param->source_ip;
	comm->rdma_params->has_source_ip	= user_param->has_source_ip;
	comm->rdma_params->memory_type		= MEMORY_HOST;
	comm->rdma_params->memory_create	= host_memory_create;

	if (user_param->use_rdma_cm) {

		MAIN_ALLOC(comm->rdma_ctx, struct pingpong_context, 1, free_rdma_params);
		memset(comm->rdma_ctx, 0, sizeof(struct pingpong_context));

		comm->rdma_params->tx_depth = 1;
		comm->rdma_params->rx_depth = 1;
		comm->rdma_params->connection_type = RC;
		comm->rdma_params->num_of_qps = 1;
		comm->rdma_params->verb	= SEND;
		comm->rdma_params->size = sizeof(struct pingpong_dest);
		comm->rdma_ctx->context = NULL;

		comm->rdma_ctx->memory = comm->rdma_params->memory_create(comm->rdma_params);
		if (comm->rdma_ctx->memory == NULL)
			goto free_rdma_ctx;

		MAIN_ALLOC(comm->rdma_ctx->mr, struct ibv_mr*, user_param->num_of_qps, free_memory_ctx);
		MAIN_ALLOC(comm->rdma_ctx->buf, void* , user_param->num_of_qps, free_mr);
		MAIN_ALLOC(comm->rdma_ctx->qp,struct ibv_qp*,comm->rdma_params->num_of_qps, free_buf);
		#ifdef HAVE_IBV_WR_API
		MAIN_ALLOC(comm->rdma_ctx->qpx,struct ibv_qp_ex*,comm->rdma_params->num_of_qps, free_qp);
		#endif
		#ifdef HAVE_DCS
		MAIN_ALLOC(comm->rdma_ctx->dci_stream_id,uint32_t, comm->rdma_params->num_of_qps, free_qpx);
		#endif
		comm->rdma_ctx->buff_size = user_param->cycle_buffer;

		if (create_rdma_resources(comm->rdma_ctx,comm->rdma_params)) {
			fprintf(stderr," Unable to create the resources needed by comm struct\n");
			goto free_mem;
		}
	}

	if ((user_param->counter_ctx) && (counters_open(user_param->counter_ctx,
		user_param->ib_devname, user_param->ib_port))) {
		fprintf(stderr," Unable to access performance counters\n");
		if (user_param->use_rdma_cm)
			goto free_mem;
		else
			goto free_rdma_params;
	}

	return SUCCESS;

free_mem: __attribute__((unused))
	#ifdef HAVE_DCS
	free(comm->rdma_ctx->dci_stream_id);
	#endif
// cppcheck-suppress unusedLabelConfiguration
free_qpx: __attribute__((unused))
	#ifdef HAVE_IBV_WR_API
	free(comm->rdma_ctx->qpx);
	#endif
// cppcheck-suppress unusedLabelConfiguration
free_qp:
	free(comm->rdma_ctx->qp);
free_buf:
	free(comm->rdma_ctx->buf);
free_mr:
	free(comm->rdma_ctx->mr);
free_memory_ctx:
	comm->rdma_ctx->memory->destroy(comm->rdma_ctx->memory);
free_rdma_ctx:
	free(comm->rdma_ctx);
free_rdma_params:
	free(comm->rdma_params);
return_error:
	return FAILURE;
}
/******************************************************************************
 *
 ******************************************************************************/
void dealloc_comm_struct(struct perftest_comm *comm,
		struct perftest_parameters *user_param)
{

	if (user_param->use_rdma_cm) {
		free(comm->rdma_ctx->mr);
		free(comm->rdma_ctx->buf);
		free(comm->rdma_ctx->qp);
		#ifdef HAVE_IBV_WR_API
		free(comm->rdma_ctx->qpx);
		#endif
		#ifdef HAVE_DCS
		free(comm->rdma_ctx->dci_stream_id);
		#endif
		if (comm->rdma_ctx->memory != NULL)
		{
			comm->rdma_ctx->memory->destroy(comm->rdma_ctx->memory);
			comm->rdma_ctx->memory = NULL;
		}
		free(comm->rdma_ctx);
	}

	free(comm->rdma_params);
}

/******************************************************************************
 *
 ******************************************************************************/
int establish_connection(struct perftest_comm *comm)
{

	if (comm->rdma_params->use_rdma_cm) {
		if (comm->rdma_params->machine == CLIENT) {
			if (rdma_client_connect(comm->rdma_ctx,comm->rdma_params)) {
				fprintf(stderr," Unable to perform rdma_client function\n");
				return 1;
			}
		} else {
			if (rdma_server_connect(comm->rdma_ctx,comm->rdma_params)) {
				fprintf(stderr," Unable to perform rdma_server function\n");
				return 1;
			}
		}
	} else {
		int (*ptr)(struct perftest_comm*);
		ptr = comm->rdma_params->servername ? &ethernet_client_connect : &ethernet_server_connect;

		if ((*ptr)(comm)) {
			fprintf(stderr,"Unable to open file descriptor for socket connection");
			return 1;
		}
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_hand_shake(struct perftest_comm *comm,
		struct pingpong_dest *my_dest,
		struct pingpong_dest *rem_dest)
{
	int (*read_func_ptr) (struct pingpong_dest*,struct perftest_comm*);
	int (*write_func_ptr)(struct pingpong_dest*,struct perftest_comm*);

	if (comm->rdma_params->use_rdma_cm || comm->rdma_params->work_rdma_cm) {
		read_func_ptr  = &rdma_read_keys;
		write_func_ptr = &rdma_write_keys;

	} else {
		read_func_ptr  = &ethernet_read_keys;
		write_func_ptr = &ethernet_write_keys;

	}

	rem_dest->gid_index = my_dest->gid_index;
	if (comm->rdma_params->servername) {
		if ((*write_func_ptr)(my_dest,comm)) {
			fprintf(stderr," Unable to write to socket/rdma_cm\n");
			return 1;
		}
		if ((*read_func_ptr)(rem_dest,comm)) {
			fprintf(stderr," Unable to read from socket/rdma_cm\n");
			return 1;
		}

		/*Server side will wait for the client side to reach the write function.*/
	} else {

		if ((*read_func_ptr)(rem_dest,comm)) {
			fprintf(stderr," Unable to read to socket/rdma_cm\n");
			return 1;
		}
		if ((*write_func_ptr)(my_dest,comm)) {
			fprintf(stderr," Unable to write from socket/rdma_cm\n");
			return 1;
		}
	}

	return 0;
}





/******************************************************************************
 *
 ******************************************************************************/
int ctx_xchg_data_ethernet( struct perftest_comm *comm,
		void *my_data,
		void *rem_data,int size)
{
	if (comm->rdma_params->servername) {
		if (ethernet_write_data(comm, (char *) my_data, size)) {
			fprintf(stderr," Unable to write to socket/rdma_cm\n");
			return 1;
		}

		if (ethernet_read_data(comm, (char *) rem_data, size)) {
			fprintf(stderr," Unable to read from socket/rdma_cm\n");
			return 1;
		}

		/*Server side will wait for the client side to reach the write function.*/
	} else {

		if (ethernet_read_data(comm, (char *) rem_data, size)) {
			fprintf(stderr," Unable to read to socket/rdma_cm\n");
			return 1;
		}

		if (ethernet_write_data(comm, (char *) my_data, size)) {
			fprintf(stderr," Unable to write from socket/rdma_cm\n");
			return 1;
		}
	}
	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int ctx_xchg_data_rdma( struct perftest_comm *comm,
		void *my_data,
		void *rem_data,int size)
{
	if (comm->rdma_params->servername) {
		if (rdma_write_data(my_data,comm,size)) {
			fprintf(stderr," Unable to write to socket/rdma_cm\n");
			return 1;
		}

		if (rdma_read_data(rem_data,comm,size)) {
			fprintf(stderr," Unable to read from socket/rdma_cm\n");
			return 1;
		}

		/*Server side will wait for the client side to reach the write function.*/
	} else {

		if (rdma_read_data(rem_data,comm,size)) {
			fprintf(stderr," Unable to read to socket/rdma_cm\n");
			return 1;
		}

		if (rdma_write_data(my_data,comm,size)) {
			fprintf(stderr," Unable to write from socket/rdma_cm\n");
			return 1;
		}
	}
	return 0;
}


/******************************************************************************
 *
 ******************************************************************************/
int rdma_read_data(void *data,
		struct perftest_comm *comm, int size)
{
	struct ibv_wc wc;
	int ne;

	do {
		ne = ibv_poll_cq(comm->rdma_ctx->recv_cq,1,&wc);
	} while (ne == 0);

	if (wc.status || !(wc.opcode & IBV_WC_RECV) || wc.wr_id != SYNC_SPEC_ID) {
		//coverity[uninit_use_in_call]
		fprintf(stderr, "Bad wc status -- %d -- %d \n",(int)wc.status,(int)wc.wr_id);
		return 1;
	}

	memcpy(data,comm->rdma_ctx->buf[0], size);

	if (post_one_recv_wqe(comm->rdma_ctx)) {
		fprintf(stderr, "Couldn't post send \n");
		return 1;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int rdma_write_data(void *data,
		struct perftest_comm *comm, int size)
{
	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr;
	struct ibv_sge list;
	struct ibv_wc wc;
	int ne;
	memcpy(comm->rdma_ctx->buf[0],data,size);

	list.addr   = (uintptr_t)comm->rdma_ctx->buf[0];
	list.length = size;
	list.lkey   = comm->rdma_ctx->mr[0]->lkey;

	wr.wr_id      = SYNC_SPEC_ID;
	wr.sg_list    = &list;
	wr.num_sge    = 1;
	wr.opcode     = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.next       = NULL;

	if (ibv_post_send(comm->rdma_ctx->qp[0],&wr,&bad_wr)) {
		fprintf(stderr, "Function ibv_post_send failed\n");
		return 1;
	}

	do {
		ne = ibv_poll_cq(comm->rdma_ctx->send_cq, 1,&wc);
	} while (ne == 0);

	//coverity[uninit_use]
	if (wc.status || wc.opcode != IBV_WC_SEND || wc.wr_id != SYNC_SPEC_ID) {
		fprintf(stderr, " Bad wc status %d\n",(int)wc.status);
		return 1;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
int ethernet_write_data(struct perftest_comm *comm, char *msg, size_t size)
{
	if (write(comm->rdma_params->sockfd, msg, size) != size) {
		perror("client write");
		fprintf(stderr, "Couldn't send reports\n");
		return 1;
	}

	return 0;

}
/******************************************************************************
 *
 ******************************************************************************/
int ethernet_read_data(struct perftest_comm *comm, char *recv_msg, size_t size)
{
	if (read(comm->rdma_params->sockfd, recv_msg, size) != size) {
		fprintf(stderr, "ethernet_read_data: Couldn't read reports\n");
		return 1;
	}

	return 0;
}


/******************************************************************************
 *
 ******************************************************************************/
int ctx_xchg_data( struct perftest_comm *comm,
		void *my_data,
		void *rem_data,int size)
{
	if (comm->rdma_params->use_rdma_cm || comm->rdma_params->work_rdma_cm) {
		if (ctx_xchg_data_rdma(comm,my_data,rem_data,size))
			return 1;
	} else {
		if (ctx_xchg_data_ethernet(comm,my_data,rem_data,size))
			return 1;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
void xchg_bw_reports (struct perftest_comm *comm, struct bw_report_data *my_bw_rep,
		struct bw_report_data *rem_bw_rep, float remote_version)
{
	struct bw_report_data temp;
	int size;

	temp.size = hton_long(my_bw_rep->size);

	if ( remote_version >= 5.33 )
		temp.iters = hton_long(my_bw_rep->iters);
	else
		temp.iters = hton_int(my_bw_rep->iters);

	temp.bw_peak = hton_double(my_bw_rep->bw_peak);
	temp.bw_avg = hton_double(my_bw_rep->bw_avg);
	temp.bw_avg_p1 = hton_double(my_bw_rep->bw_avg_p1);
	temp.bw_avg_p2 = hton_double(my_bw_rep->bw_avg_p2);
	temp.msgRate_avg = hton_double(my_bw_rep->msgRate_avg);
	temp.msgRate_avg_p1 = hton_double(my_bw_rep->msgRate_avg_p1);
	temp.msgRate_avg_p2 = hton_double(my_bw_rep->msgRate_avg_p2);

	/*******************Exchange Reports*******************/
	if (ctx_xchg_data(comm, (void*) (&temp.size), (void*) (&rem_bw_rep->size), sizeof(unsigned long))) {
		fprintf(stderr," Failed to exchange data between server and clients\n");
		exit(1);
	}

	size = (remote_version >= 5.33) ? sizeof(uint64_t) : sizeof(int);

	if (ctx_xchg_data(comm, (void*) (&temp.iters), (void*) (&rem_bw_rep->iters), size)) {
		fprintf(stderr," Failed to exchange data between server and clients\n");
		exit(1);
	}
	if (ctx_xchg_data(comm, (void*) (&temp.bw_peak), (void*) (&rem_bw_rep->bw_peak), sizeof(double))) {
		fprintf(stderr," Failed to exchange data between server and clients\n");
		exit(1);
	}
	if (ctx_xchg_data(comm, (void*) (&temp.bw_avg), (void*) (&rem_bw_rep->bw_avg), sizeof(double))) {
		fprintf(stderr," Failed to exchange data between server and clients\n");
		exit(1);
	}
	if (ctx_xchg_data(comm, (void*) (&temp.msgRate_avg), (void*) (&rem_bw_rep->msgRate_avg), sizeof(double))) {
		fprintf(stderr," Failed to exchange data between server and clients\n");
		exit(1);
	}

	/*exchange data for report per port feature. should keep compatibility*/
	if (comm->rdma_params->report_per_port) {
		if (ctx_xchg_data(comm, (void*) (&temp.bw_avg_p1), (void*) (&rem_bw_rep->bw_avg_p1), sizeof(double))) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			exit(1);
		}
		if (ctx_xchg_data(comm, (void*) (&temp.msgRate_avg_p1), (void*) (&rem_bw_rep->msgRate_avg_p1), sizeof(double))) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			exit(1);
		}
		if (ctx_xchg_data(comm, (void*) (&temp.bw_avg_p2), (void*) (&rem_bw_rep->bw_avg_p2), sizeof(double))) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			exit(1);
		}
		if (ctx_xchg_data(comm, (void*) (&temp.msgRate_avg_p2), (void*) (&rem_bw_rep->msgRate_avg_p2), sizeof(double))) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			exit(1);
		}
	}
	// cppcheck-suppress selfAssignment
	rem_bw_rep->size = hton_long(rem_bw_rep->size);

	if ( remote_version >= 5.33 ) {
		// cppcheck-suppress selfAssignment
		rem_bw_rep->iters = hton_long(rem_bw_rep->iters);
	}
	else {
		rem_bw_rep->iters = hton_int(rem_bw_rep->iters);
	}
	// cppcheck-suppress selfAssignment
	rem_bw_rep->bw_peak = hton_double(rem_bw_rep->bw_peak);
	// cppcheck-suppress selfAssignment
	rem_bw_rep->bw_avg = hton_double(rem_bw_rep->bw_avg);
	// cppcheck-suppress selfAssignment
	rem_bw_rep->bw_avg_p1 = hton_double(rem_bw_rep->bw_avg_p1);
	// cppcheck-suppress selfAssignment
	rem_bw_rep->bw_avg_p2 = hton_double(rem_bw_rep->bw_avg_p2);
	// cppcheck-suppress selfAssignment
	rem_bw_rep->msgRate_avg = hton_double(rem_bw_rep->msgRate_avg);
	// cppcheck-suppress selfAssignment
	rem_bw_rep->msgRate_avg_p1 = hton_double(rem_bw_rep->msgRate_avg_p1);
	// cppcheck-suppress selfAssignment
	rem_bw_rep->msgRate_avg_p2 = hton_double(rem_bw_rep->msgRate_avg_p2);

}

/******************************************************************************
 *
 ******************************************************************************/
void ctx_print_pingpong_data(struct pingpong_dest *element,
		struct perftest_comm *comm)
{
	int is_there_mgid,local_mgid,remote_mgid;

	/* use dlid value from user (if user specified and only on the remote side) */
	uint16_t dlid = (comm->rdma_params->dlid && comm->rdma_params->side == REMOTE) ?
				comm->rdma_params->dlid : element->lid;

	if (comm->rdma_params->output != FULL_VERBOSITY)
		return;
	/*First of all we print the basic format.*/
	printf(BASIC_ADDR_FMT, sideArray[comm->rdma_params->side], dlid, element->qpn, element->psn);

	switch (comm->rdma_params->verb) {
		case READ  : printf(READ_FMT,element->out_reads);
		case WRITE : printf(RDMA_FMT,element->rkey,element->vaddr);
		default : ;
	}

	if (comm->rdma_params->use_xrc) {
		printf(XRC_FMT,element->srqn);
	} else if (comm->rdma_params->connection_type == DC){
		printf(DC_FMT,element->srqn);
	}

	putchar('\n');

	local_mgid    = (comm->rdma_params->side == LOCAL)  && (comm->rdma_params->machine == SERVER);
	remote_mgid   = (comm->rdma_params->side == REMOTE)  && (comm->rdma_params->machine == CLIENT);
	is_there_mgid =  comm->rdma_params->duplex || remote_mgid || local_mgid;

	if ((comm->rdma_params->gid_index > -1 || (comm->rdma_params->use_mcg && is_there_mgid)) && comm->rdma_params->connection_type != RawEth) {

		printf(PERF_GID_FMT,gidArray[comm->rdma_params->use_mcg && is_there_mgid],
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
int ctx_close_connection(struct perftest_comm *comm,
		struct pingpong_dest *my_dest,
		struct pingpong_dest *rem_dest)
{
	/*Signal client is finished.*/
	if (ctx_hand_shake(comm,my_dest,rem_dest)) {
		return 1;
	}

	if (!comm->rdma_params->use_rdma_cm && !comm->rdma_params->work_rdma_cm) {

		if (write(comm->rdma_params->sockfd,"done",sizeof "done") != sizeof "done") {
			perror(" Client write");
			fprintf(stderr,"Couldn't write to socket\n");
			return -1;
		}

		close(comm->rdma_params->sockfd);
		return 0;
	}

	return 0;
}

/******************************************************************************
 *
 ******************************************************************************/
void exchange_versions(struct perftest_comm *user_comm, struct perftest_parameters *user_param)
{
	if (!user_param->dont_xchg_versions) {
		if (ctx_xchg_data(user_comm,(void*)(&user_param->version),(void*)(&user_param->rem_version),sizeof(user_param->rem_version))) {
			fprintf(stderr," Failed to exchange data between server and clients\n");
			exit(1);
		}
	}
}

/******************************************************************************
 *
 ******************************************************************************/
void check_version_compatibility(struct perftest_parameters *user_param)
{
	if ((atof(user_param->rem_version) < 5.70))
	{
		fprintf(stderr, "Current implementation is not compatible with versions older than 5.70\n");
		exit(1);
	}
}

/******************************************************************************
 *
 ******************************************************************************/
void check_sys_data(struct perftest_comm *user_comm, struct perftest_parameters *user_param)
{
	int rem_cycle_buffer = 0;
	int rem_cache_line_size = 0;

	int m_cycle_buffer = hton_int(user_param->cycle_buffer);
	int m_cache_line_size = hton_int(user_param->cache_line_size);

	/*keep compatibility between older versions, without this feature.*/
	if ( !(atof(user_param->rem_version) >= 5.32) ) {
		return;
	}

	if (!user_param->dont_xchg_versions) {
		if (ctx_xchg_data(user_comm,(void*)(&m_cycle_buffer),(void*)(&rem_cycle_buffer), sizeof(user_param->cycle_buffer))) {
			fprintf(stderr," Failed to exchange Page Size data between server and client\n");
			exit(1);
		}
		if (ctx_xchg_data(user_comm,(void*)(&m_cache_line_size),(void*)(&rem_cache_line_size), sizeof(user_param->cache_line_size))) {
			fprintf(stderr," Failed to exchange Cache Line Size data between server and client\n");
			exit(1);
		}
	}

	rem_cycle_buffer = ntoh_int(rem_cycle_buffer);
	rem_cache_line_size = ntoh_int(rem_cache_line_size);

	/*take the max and update user_param*/
	user_param->cycle_buffer = (rem_cycle_buffer > user_param->cycle_buffer) ? rem_cycle_buffer : user_param->cycle_buffer;
	user_param->cache_line_size = (rem_cache_line_size > user_param->cache_line_size) ? rem_cache_line_size : user_param->cache_line_size;

	/*update user_comm as well*/
	if (user_param->use_rdma_cm) {
		user_comm->rdma_ctx->buff_size = user_param->cycle_buffer;
	}

}

/******************************************************************************
 *
 ******************************************************************************/
int check_mtu(struct ibv_context *context,struct perftest_parameters *user_param, struct perftest_comm *user_comm) {
	int curr_mtu, rem_mtu;
	char cur[sizeof(int)];
	char rem[sizeof(int)];
	int size_of_cur;
	float rem_vers = atof(user_param->rem_version);

	if (user_param->connection_type == RawEth) {
		if (set_eth_mtu(user_param) != 0 ) {
			fprintf(stderr, " Couldn't set Eth MTU\n");
			return FAILURE;
		}
	} else {
		curr_mtu = (int) (set_mtu(context,user_param->ib_port,user_param->mtu));
		if (!user_param->dont_xchg_versions) {
			/*add mtu set in remote node from version 5.1 and above*/
			if (rem_vers >= 5.1 ) {
				sprintf(cur,"%d",curr_mtu);

				/*fix a buffer overflow issue in ppc.*/
				size_of_cur = (rem_vers >= 5.31) ? sizeof(char[2]) : sizeof(int);

				if (ctx_xchg_data(user_comm,(void*)(cur),(void*)(rem),size_of_cur)) {
					fprintf(stderr," Failed to exchange data between server and clients\n");
					exit(1);
				}
				rem_mtu = (int) strtol(rem, (char **)NULL, 10);
				user_param->curr_mtu = (enum ibv_mtu)((valid_mtu_size(rem_mtu) && (curr_mtu > rem_mtu)) ? rem_mtu : curr_mtu);
			} else {
				user_param->curr_mtu = (enum ibv_mtu)(curr_mtu);
			}
		} else {
			user_param->curr_mtu = (enum ibv_mtu)(curr_mtu);
		}
	}

	if (user_param->connection_type == UD && user_param->size > MTU_SIZE(user_param->curr_mtu)) {
		if (user_param->test_method == RUN_ALL || !user_param->req_size) {
			fprintf(stderr," Max msg size in UD is MTU %lu\n",MTU_SIZE(user_param->curr_mtu));
			fprintf(stderr," Changing to this MTU\n");
			user_param->size = MTU_SIZE(user_param->curr_mtu);
		}
		else
		{
			fprintf(stderr," Max message size in UD cannot be greater than MTU \n");
			return FAILURE;
		}
	} else if (user_param->connection_type == RawEth) {
		/* checking msg size in raw ethernet */
		if (user_param->size > user_param->curr_mtu) {
			fprintf(stderr," Max msg size in RawEth is MTU %d\n",user_param->curr_mtu);
			fprintf(stderr," Changing msg size to this MTU\n");
			user_param->size = user_param->curr_mtu;
		} else if (user_param->size < RAWETH_MIN_MSG_SIZE) {
			printf(" Min msg size for RawEth is 64B - changing msg size to 64 \n");
			user_param->size = RAWETH_MIN_MSG_SIZE;
		}
	} else if (user_param->connection_type == SRD) {
		if (user_param->verb == SEND) {
			struct ibv_port_attr port_attr;

			if (ibv_query_port(context, user_param->ib_port, &port_attr)) {
				fprintf(stderr, " Error when trying to query port\n");
				exit(1);
			}

			//coverity[uninit_use]
			if (user_param->size > port_attr.max_msg_sz) {
				if (user_param->test_method == RUN_ALL || !user_param->req_size) {
					fprintf(stderr, " Max msg size is %u\n",
						port_attr.max_msg_sz);
					fprintf(stderr, " Changing to this size\n");
					user_param->size = port_attr.max_msg_sz;
				} else {
					fprintf(stderr," Max message size in SRD cannot be greater than %u \n",
						port_attr.max_msg_sz);
					return FAILURE;
				}
			}
		} else if (user_param->verb == READ || user_param->verb == WRITE) {
#ifdef HAVE_SRD
			struct efadv_device_attr efa_device_attr = {0};

			if (efadv_query_device(context, &efa_device_attr, sizeof(efa_device_attr))) {
				fprintf(stderr, " Error when trying to query EFA device\n");
				exit(1);
			}

			if (user_param->verb == READ) {
#ifdef HAVE_SRD_WITH_RDMA_READ
				if (!(efa_device_attr.device_caps & EFADV_DEVICE_ATTR_CAPS_RDMA_READ)) {
					fprintf(stderr, "Read verb is not supported with this EFA device\n");
					exit(1);
				}
#else
				fprintf(stderr, "SRD connection not possible in READ verb\n");
				exit(1);
#endif
			} else if (user_param->verb == WRITE) {
#ifdef HAVE_SRD_WITH_RDMA_WRITE
				if (!(efa_device_attr.device_caps & EFADV_DEVICE_ATTR_CAPS_RDMA_WRITE)) {
					fprintf(stderr, "Write verb is not supported with this EFA device\n");
					exit(1);
				}
#else
				fprintf(stderr, "SRD connection not possible in WRITE verb\n");
				exit(1);
#endif
			}

			if (user_param->size > efa_device_attr.max_rdma_size) {
				if (user_param->test_method == RUN_ALL || !user_param->req_size) {
					fprintf(stderr, " Max RDMA request size is %u\n",
						efa_device_attr.max_rdma_size);
					fprintf(stderr, " Changing to this size\n");
					user_param->size = efa_device_attr.max_rdma_size;
				} else {
					fprintf(stderr, " Max RDMA request size in SRD cannot be greater than %u\n",
						efa_device_attr.max_rdma_size);
					return FAILURE;
				}
			}
#else
			fprintf(stderr, "SRD connection was not found\n");
			exit(1);
#endif
		}
	}

	return SUCCESS;
}

/******************************************************************************
*
******************************************************************************/
int ctx_check_gid_compatibility(struct pingpong_dest *my_dest,
		struct pingpong_dest *rem_dest)
{
	int gid_type1, gid_type2;

	/*ipv4 - 1 , ipv6 - 0 */
	gid_type1 = ipv6_addr_v4mapped((struct in6_addr *)my_dest->gid.raw);
	gid_type2 = ipv6_addr_v4mapped((struct in6_addr *)rem_dest->gid.raw);

	if (gid_type1 != gid_type2)
		return 1;

	return 0;
}


/******************************************************************************
*
******************************************************************************/
int rdma_cm_get_rdma_address(struct perftest_parameters *user_param,
		struct rdma_addrinfo *hints, struct rdma_addrinfo **rai)
{
	int rc;
	char port[6] = "", error_message[ERROR_MSG_SIZE] = "";

	sprintf(port, "%d", user_param->port);
	hints->ai_family = AF_INET;
	// if we have servername specified, it is a client, we should use server name
	// if it is not specified, we should use explicit source_ip if possible
	if ((NULL != user_param->servername) || (!user_param->has_source_ip)) {
		rc = rdma_getaddrinfo(user_param->servername, port, hints, rai);
	}
	else {
		rc = rdma_getaddrinfo(user_param->source_ip, port, hints, rai);
	}

	if (rc) {
		sprintf(error_message,
			"Failed to get RDMA CM address info - Error: %s",
			gai_strerror(rc));
		goto error;
	}

	return rc;

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_request_ud_connection_parameters(struct pingpong_context *ctx,
		struct perftest_parameters *user_param,
		struct rdma_conn_param *conn_param)
{
	int rc = SUCCESS, connection_index, needed;
	char error_message[ERROR_MSG_SIZE] = "";
	struct ibv_recv_wr recv_wr, *recv_failure;
	struct ibv_sge sge;
	struct cma_node *cm_node;

	needed = ((user_param->connection_type == UD)
				&& ((user_param->tst == BW && user_param->duplex)
					|| (user_param->tst == LAT)));

	if (!needed) {
		return rc;
	}

	connection_index = ctx->cma_master.connection_index;
	cm_node = &ctx->cma_master.nodes[connection_index];

	recv_wr.next = NULL;
	recv_wr.sg_list = &sge;
	recv_wr.num_sge = 1;
	recv_wr.wr_id = 0;

	sge.length = UD_ADDITION + sizeof(uint32_t);
	sge.lkey = ctx->mr[connection_index]->lkey;
	sge.addr = (uintptr_t)ctx->buf[connection_index];

	rc = ibv_post_recv(cm_node->cma_id->qp, &recv_wr, &recv_failure);
	if (rc) {
		sprintf(error_message,
			"Failed to post receive for connection %d for RDMA CM UD "
			"request connection parameters.", connection_index);
		goto error;
	}

	conn_param->qp_num = cm_node->cma_id->qp->qp_num;
	return rc;

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_initialize_ud_connection_parameters(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int rc = SUCCESS, connection_index, needed, cqes;
	char error_message[ERROR_MSG_SIZE] = "";
	struct ibv_qp_attr attr;
	struct ibv_qp_init_attr init_attr;
	struct ibv_wc wc;
	struct cma_node *cm_node;
	void* grh_buffer;

	needed = ((user_param->connection_type == UD)
				&& ((user_param->tst == BW && user_param->duplex)
					|| (user_param->tst == LAT)));

	if (!needed) {
		return rc;
	}

	connection_index = ctx->cma_master.connection_index;
	do {
		cqes = ibv_poll_cq(ctx->recv_cq, 1, &wc);
	} while (cqes == 0);

	rc = wc.status || !(wc.opcode & IBV_WC_RECV) || wc.wr_id != 0;
	if (rc) {
		sprintf(error_message,
			"Failed to poll CQ.\nBad WQE status when trying to create "
			"AH for UD connection in RDMA CM:\n Status: %d ; ID: %d.",
			(int)wc.status, (int)wc.wr_id);
		goto error;
	}

	cm_node = &ctx->cma_master.nodes[connection_index];
	grh_buffer = ctx->buf[connection_index];

	if (user_param->memory_type != MEMORY_HOST){
		struct pingpong_dest *temp_buffer;
		ALLOCATE(temp_buffer, struct pingpong_dest, sizeof(struct pingpong_dest));
		ctx->memory->copy_buffer_to_host(temp_buffer, ctx->buf[connection_index], sizeof(struct pingpong_dest));
		grh_buffer = temp_buffer;
	}

	ctx->ah[connection_index] = ibv_create_ah_from_wc(ctx->pd, &wc,
		grh_buffer, cm_node->cma_id->port_num);

	if (user_param->memory_type != MEMORY_HOST){
		free(grh_buffer);
	}

	user_param->ah_allocated = 1;
	ibv_query_qp(cm_node->cma_id->qp, &attr, IBV_QP_QKEY, &init_attr);
	//coverity[uninit_use_in_call]
	cm_node->remote_qpn = ntohl(wc.imm_data);
	//coverity[uninit_use]
	cm_node->remote_qkey = attr.qkey;
	return rc;

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_send_ud_connection_parameters(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, int connection_index)
{
	int rc, cqes;
	char error_message[ERROR_MSG_SIZE] = "";
	struct ibv_send_wr send_wr, *bad_send_wr;
	struct ibv_sge sge;
	struct ibv_wc wc;
	struct cma_node *cm_node;

	cm_node = &ctx->cma_master.nodes[connection_index];

	send_wr.next = NULL;
	send_wr.sg_list = &sge;
	send_wr.num_sge = 1;
	send_wr.opcode = IBV_WR_SEND_WITH_IMM;
	send_wr.send_flags = IBV_SEND_SIGNALED;
	send_wr.wr_id = 0;
	send_wr.imm_data = htonl(cm_node->cma_id->qp->qp_num);

	send_wr.wr.ud.ah = ctx->ah[connection_index];
	send_wr.wr.ud.remote_qpn = cm_node->remote_qpn;
	send_wr.wr.ud.remote_qkey = cm_node->remote_qkey;

	sge.length = sizeof(uint32_t);
	sge.lkey = ctx->mr[connection_index]->lkey;
	sge.addr = (uintptr_t)ctx->buf[connection_index];

	rc = ibv_post_send(cm_node->cma_id->qp, &send_wr, &bad_send_wr);
	if (rc) {
		sprintf(error_message,
			"Failed to post send for connection %d for RDMA CM UD "
			"send connection parameters.", connection_index);
		goto error;
	}

	do {
		cqes = ibv_poll_cq(ctx->send_cq, 1, &wc);
	} while (cqes == 0);

	//coverity[uninit_use]
	rc = wc.status || wc.opcode != IBV_WC_SEND || wc.wr_id != 0;
	if (rc) {
		sprintf(error_message,
			"Failed to poll CQ.\nBad WQE status when trying to send "
			"UD connection parameters in RDMA CM:\n Status: %d ; ID: %d.",
			(int)wc.status, (int)wc.wr_id);
		goto error;
	}

	return rc;

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_establish_ud_connection(struct pingpong_context *ctx,
	struct perftest_parameters *user_param, struct rdma_cm_event *event)
{
	int rc = SUCCESS, connection_index, needed;
	char *error_message;
	struct cma_node *cm_node;

	needed = user_param->connection_type == UD;

	if (!needed) {
		return rc;
	}

	connection_index = ctx->cma_master.disconnects_left;
	cm_node = &ctx->cma_master.nodes[connection_index];
	cm_node->remote_qpn = event->param.ud.qp_num;
	cm_node->remote_qkey = event->param.ud.qkey;

	ctx->ah[connection_index] = ibv_create_ah(ctx->pd,
		&event->param.ud.ah_attr);
	if (!ctx->ah[connection_index]) {
		error_message = "Failed to create AH for RDMA CM connection.";
		goto error;
	}
	user_param->ah_allocated = 1;

	if ((user_param->tst == BW && user_param->duplex)
		|| (user_param->tst == LAT)) {
		rc = rdma_cm_send_ud_connection_parameters(ctx, user_param,
			connection_index);
		if (rc) {
			error_message = "Failed to send UD connection parameters "
				"to the remote node.";
			goto error;
		}
	}

	return rc;

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
void rdma_cm_connect_error(struct pingpong_context *ctx)
{
	ctx->cma_master.connects_left--;
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_address_handler(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_id *cma_id)
{
	int rc;
	char *error_message;

	if (user_param->tos != DEF_TOS) {
		rc = rdma_set_option(cma_id, RDMA_OPTION_ID,
			RDMA_OPTION_ID_TOS, &user_param->tos, sizeof(uint8_t));
		if (rc) {
			error_message = \
				"Failed to set ToS(Type of Service) option for RDMA "
				"CM connection.";
			goto error;
		}
	}

	rc = rdma_resolve_route(cma_id, 2000);
	if (rc) {
		error_message = "Failed to resolve RDMA CM route.";
		rdma_cm_connect_error(ctx);
		goto error;
	}

	return rc;

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_route_handler(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_id *cma_id)
{
	int rc, connection_index;
	char *error_message;
	struct rdma_conn_param conn_param;

	ctx->context = cma_id->verbs;
	connection_index = ctx->cma_master.connection_index;

	// Initialization of client contexts in case of first connection:
	if (connection_index == 0) {
		rc = ctx_init(ctx, user_param);
		if (rc) {
			error_message = "Failed to initialize RDMA contexts.";
			goto error;
		}
	}

	ctx->cm_id = cma_id;
	rc = create_qp_main(ctx, user_param, connection_index);
	if (rc) {
		error_message = "Failed to create QP.";
		goto error;
	}

	memset(&conn_param, 0, sizeof conn_param);

	if (user_param->verb == READ || user_param->verb == ATOMIC) {
		conn_param.responder_resources = user_param->out_reads;
		conn_param.initiator_depth = user_param->out_reads;
	}

	conn_param.retry_count = user_param->retry_count;
	conn_param.rnr_retry_count = user_param->retry_count;
	conn_param.private_data = ctx->cma_master.rai->ai_connect;
	conn_param.private_data_len = ctx->cma_master.rai->ai_connect_len;

	rc = rdma_connect(cma_id, &conn_param);
	if (rc) {
		error_message = "Failed to connect through RDMA CM.";
		goto error;
	}

	ctx->cma_master.nodes[connection_index].connected = 1;
	ctx->cma_master.connection_index++;
	return rc;

error:
	rdma_cm_connect_error(ctx);
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_connection_request_handler(struct pingpong_context *ctx,
				       struct perftest_parameters *user_param,
				       struct rdma_cm_event *event, struct rdma_cm_id *cma_id)
{
	int rc, connection_index;
	char *error_message = "";
	struct cma_node *cm_node;
	struct rdma_conn_param conn_param;
	struct ibv_qp_attr rtr_attr = {
		.min_rnr_timer = MIN_RNR_TIMER,
	};

	connection_index = ctx->cma_master.connection_index;

	if (connection_index == user_param->num_of_qps) {
		goto error_1;
	}

	cm_node = &ctx->cma_master.nodes[connection_index];
	cm_node->cma_id = cma_id;

	ctx->context = cma_id->verbs;
	// Initialization of server contexts in case of first connection:
	if (connection_index == 0) {
		rc = ctx_init(ctx, user_param);
		if (rc) {
			error_message = "Failed to initialize RDMA contexts.";
			goto error_2;
		}
	}

	ctx->cm_id = cm_node->cma_id;
	rc = create_qp_main(ctx, user_param, connection_index);
	if (rc) {
		error_message = "Failed to create QP.";
		goto error_2;
	}

	memset(&conn_param, 0, sizeof(conn_param));

	if (user_param->verb == READ || user_param->verb == ATOMIC) {
		/* Clamp responder depth based on initiator resources on the peer */
		conn_param.responder_resources =
			(user_param->out_reads > event->param.conn.initiator_depth)
			? event->param.conn.initiator_depth : user_param->out_reads;

		/* Clamp initiator depth based on responder resources on the peer */
		conn_param.initiator_depth =
			(user_param->out_reads > event->param.conn.responder_resources)
			? event->param.conn.responder_resources : user_param->out_reads;
	}

	conn_param.retry_count = user_param->retry_count;
	conn_param.rnr_retry_count = user_param->retry_count;

	rc = rdma_cm_request_ud_connection_parameters(ctx, user_param,
		&conn_param);
	if (rc) {
		error_message = \
			"Failed request UD connection parameters for RDMA CM.";
		goto error_2;
	}

	rc = rdma_accept(ctx->cm_id, &conn_param);
	if (rc) {
		error_message = "Failed to accept RDMA CM connection.";
		goto error_2;
	}

	if (user_param->connection_type == RC)
	{
		ibv_modify_qp(ctx->qp[connection_index], &rtr_attr, IBV_QP_MIN_RNR_TIMER);
	}

	rc = rdma_cm_initialize_ud_connection_parameters(ctx, user_param);
	if (rc) {
		error_message = \
			"Failed to initialize UD connection parameters for RDMA CM.";
		goto error_2;
	}

	ctx->cma_master.nodes[connection_index].connected = 1;
	ctx->cma_master.connection_index++;
	ctx->cma_master.connects_left--;
	ctx->cma_master.disconnects_left++;
	return rc;

error_2:
	cm_node->cma_id = NULL;
	rdma_cm_connect_error(ctx);

error_1:
	rdma_reject(ctx->cm_id, NULL, 0);
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_connection_established_handler(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_event *event)
{
	int rc = SUCCESS;
	char *error_message;

	rc = rdma_cm_establish_ud_connection(ctx, user_param, event);
	if (rc) {
		error_message = "Failed to establish UD connection for RDMA CM.";
		goto error;
	}

	if (user_param->machine == CLIENT) {
		ctx->cma_master.connects_left--;
		ctx->cma_master.disconnects_left++;
	}

	return rc;

error:
	rdma_cm_connect_error(ctx);
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_event_error_handler(struct pingpong_context *ctx,
		struct rdma_cm_event *event)
{
	char error_message[ERROR_MSG_SIZE] = "";

	sprintf(error_message, "RDMA CM event error:\nEvent: %s; error: %d.\n",
		rdma_event_str(event->event), event->status);
	rdma_cm_connect_error(ctx);
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
void rdma_cm_disconnect_handler(struct pingpong_context *ctx)
{
	ctx->cma_master.disconnects_left--;
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_events_dispatcher(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_cm_id *cma_id,
		struct rdma_cm_event *event)
{
	int rc = SUCCESS;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rc = rdma_cm_address_handler(ctx, user_param, cma_id);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		rc = rdma_cm_route_handler(ctx, user_param, cma_id);
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		rc = rdma_cm_connection_request_handler(ctx, user_param, event, cma_id);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		rc = rdma_cm_connection_established_handler(ctx, user_param, event);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		rc = rdma_cm_event_error_handler(ctx, event);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		rdma_cm_disconnect_handler(ctx);
		break;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		/* Cleanup will occur after test completes. */
		break;
	default:
		break;
	}

	return rc;
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_connect_events(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int rc = SUCCESS;
	char *error_message;
	struct rdma_cm_event *event;

	while (ctx->cma_master.connects_left) {
		rc = rdma_get_cm_event(ctx->cma_master.channel, &event);
		if (rc) {
			error_message = "Failed to get RDMA CM event.";
			goto error;
		}

		//coverity[uninit_use]
		rc = rdma_cm_events_dispatcher(ctx, user_param, event->id, event);
		if (rc) {
			error_message = "Failed to handle RDMA CM event.";
			goto ack;
		}

		rc = rdma_ack_cm_event(event);
		if (rc) {
			error_message = "Failed to ACK RDMA CM event after handling.";
			goto error;
		}
	}

	return rc;
ack:
	rdma_ack_cm_event(event);
error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_disconnect_nodes(struct pingpong_context *ctx,
		struct perftest_parameters *user_param)
{
	int rc = SUCCESS, i;
	char *error_message;
	struct rdma_cm_event *event;

	if (!ctx->cma_master.disconnects_left
		|| user_param->connection_type == UD)
		return rc;

	for (i = 0; i < user_param->num_of_qps; i++) {
		if (!ctx->cma_master.nodes[i].connected) {
			continue;
		}

		ctx->cma_master.nodes[i].connected = 0;
		rc = rdma_disconnect(ctx->cma_master.nodes[i].cma_id);
		if (rc) {
			error_message = "Failed to disconnect RDMA CM connection.";
			goto error;
		}
	}
	while (ctx->cma_master.disconnects_left) {
		rc = rdma_get_cm_event(ctx->cma_master.channel, &event);
		if (rc) {
			error_message = "Failed to get RDMA CM event.";
			goto error;
		}
		//coverity[uninit_use]
		rc = rdma_cm_events_dispatcher(ctx, user_param, event->id, event);
		if (rc) {
			error_message = "Failed to handle RDMA CM event.";
			goto ack;
		}
		rc = rdma_ack_cm_event(event);
		if (rc) {
			error_message = "Failed to ACK RDMA CM event after handling.";
			goto error;
		}
	}

	return rc;
ack:
	rdma_ack_cm_event(event);
error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_server_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_addrinfo *hints)
{
	int rc;
	char error_message[ERROR_MSG_SIZE] = "";
	struct rdma_cm_id *listen_id;

	rc = rdma_create_id(ctx->cma_master.channel, &listen_id, &ctx->cma_master,
		hints->ai_port_space);
	if (rc) {
		sprintf(error_message, "Failed to create RDMA CM server control ID.");
		goto error;
	}

	hints->ai_flags |= RAI_PASSIVE;
	rc = rdma_cm_get_rdma_address(user_param, hints, &ctx->cma_master.rai);
	if (rc) {
		sprintf(error_message,
			"Failed to get RDMA CM address - Error: %s.", gai_strerror(rc));
		goto destroy_id;
	}

	rc = rdma_bind_addr(listen_id, ctx->cma_master.rai->ai_src_addr);
	if (rc) {
		sprintf(error_message,
			"Failed to bind RDMA CM address on the server.");
		goto destroy_id;
	}

	rc = rdma_listen(listen_id, user_param->num_of_qps);
	if (rc) {
		sprintf(error_message,
			"Failed to listen on RDMA CM server listen ID.");
		goto destroy_id;
	}

	rc = rdma_cm_connect_events(ctx, user_param);
	if (rc) {
		goto destroy_id;
	}

	rc = rdma_destroy_id(listen_id);
	if (rc) {
		sprintf(error_message, "Failed to destroy RDMA CM server listen ID.");
		goto error;
	}

	return rc;

destroy_id:
	//coverity[uninit_use_in_call]
	rdma_destroy_id(listen_id);

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int _rdma_cm_client_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_addrinfo *hints)
{
	int i, rc;
	char error_message[ERROR_MSG_SIZE] = "";

	rc = rdma_cm_get_rdma_address(user_param, hints, &ctx->cma_master.rai);
	if (rc) {
		sprintf(error_message,
			"Failed to get RDMA CM address - Error: %s.", gai_strerror(rc));
		goto error;
	}

	for (i = 0; i < user_param->num_of_qps; i++) {
		rc = rdma_resolve_addr(ctx->cma_master.nodes[i].cma_id,
			ctx->cma_master.rai->ai_src_addr,
			ctx->cma_master.rai->ai_dst_addr, 2000);
		if (rc) {
			sprintf(error_message, "Failed to resolve RDMA CM address.");
			rdma_cm_connect_error(ctx);
			goto error;
		}
	}

	rc = rdma_cm_connect_events(ctx, user_param);
	if (rc) {
		sprintf(error_message, "Failed to connect RDMA CM events.");
		goto error;
	}

	return rc;

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int rdma_cm_client_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct rdma_addrinfo *hints)
{
	int i, rc, max_retries = 10, delay = 100000;
	char error_message[ERROR_MSG_SIZE] = "";

	for (i = 0; i < max_retries; i++) {
		rc = _rdma_cm_client_connection(ctx, user_param, hints);
		if (!rc) {
			return rc;
		}

		rc = rdma_cm_destroy_cma(ctx, user_param);
		if (rc) {
			sprintf(error_message, "Failed to destroy RDMA CM contexts.");
			goto error;
		}
		usleep(delay);
	}

	sprintf(error_message,
		"Failed to connect RDMA CM client, tried %d times.", max_retries);

error:
	return error_handler(error_message);
}

/******************************************************************************
*
******************************************************************************/
int create_rdma_cm_connection(struct pingpong_context *ctx,
		struct perftest_parameters *user_param, struct perftest_comm *comm,
		struct pingpong_dest *my_dest, struct pingpong_dest *rem_dest)
{
	int i;
	int rc;
	char *error_message;
	struct rdma_addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	ctx->cma_master.connects_left = user_param->num_of_qps;

	ctx->cma_master.channel = rdma_create_event_channel();
	if (!ctx->cma_master.channel) {
		error_message = "Failed to create RDMA CM event channel.";
		goto error;
	}

	rc = rdma_cm_allocate_nodes(ctx, user_param, &hints);
	if (rc) {
		error_message = "Failed to allocate RDMA CM nodes.";
		goto destroy_event_channel;
	}

	rc = ctx_hand_shake(comm, &my_dest[0], &rem_dest[0]);
	if (rc) {
		error_message = "Failed to sync between client and server "
			"before creating RDMA CM connection.";
		goto destroy_rdma_id;
	}

	if (user_param->machine == CLIENT) {
		rc = rdma_cm_client_connection(ctx, user_param, &hints);
	} else {
		rc = rdma_cm_server_connection(ctx, user_param, &hints);
	}

	if (rc) {
		error_message = "Failed to create RDMA CM connection.";
		free(hints.ai_src_addr);
		goto destroy_event_channel;
	}

	rc = ctx_hand_shake(comm, &my_dest[0], &rem_dest[0]);
	if (rc) {
		error_message = "Failed to sync between client and server "
			"after creating RDMA CM connection.";
		goto destroy_rdma_id;
	}

	free(hints.ai_src_addr);

	return rc;


destroy_rdma_id:
	if (user_param->machine == CLIENT) {
		for (i = 0; i < user_param->num_of_qps; i++)
			rdma_destroy_id(ctx->cma_master.nodes[i].cma_id);
	}
	free(ctx->cma_master.nodes);
	free(hints.ai_src_addr);

destroy_event_channel:
	rdma_destroy_event_channel(ctx->cma_master.channel);

error:
	return error_handler(error_message);
}


/******************************************************************************
 * End
 ******************************************************************************/
