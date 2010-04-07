
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <byteswap.h>
#include "perftest_resources.h"


/************************************************************************ 
 *
 ************************************************************************/

static const char *sideArray[] = {"local","remote"};

static const char *gidArray[]  = {"GID","MGID"};

/************************************************************************ 
 *
 ************************************************************************/
static int ctx_write_keys(const struct pingpong_dest *my_dest,
						  struct pingpong_params *params) {
    
    if (params->use_index < 0 && !params->use_mcg) {

		char msg[KEY_MSG_SIZE];
		sprintf(msg,KEY_PRINT_FMT,my_dest->lid, my_dest->qpn,
				my_dest->psn, my_dest->rkey, my_dest->vaddr);

		if (write(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return -1;
		}

    } else {

		char msg[KEY_MSG_SIZE_GID];
    
		sprintf(msg,KEY_PRINT_FMT_GID, my_dest->lid, my_dest->qpn,
				my_dest->psn, my_dest->rkey, my_dest->vaddr,
				my_dest->dgid.raw[0],my_dest->dgid.raw[1],
				my_dest->dgid.raw[2],my_dest->dgid.raw[3],
				my_dest->dgid.raw[4],my_dest->dgid.raw[5],
				my_dest->dgid.raw[6],my_dest->dgid.raw[7],
				my_dest->dgid.raw[8],my_dest->dgid.raw[9],
				my_dest->dgid.raw[10],my_dest->dgid.raw[11],
				my_dest->dgid.raw[12],my_dest->dgid.raw[13],
				my_dest->dgid.raw[14],my_dest->dgid.raw[15]);

		if (write(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("client write");
			fprintf(stderr, "Couldn't send local address\n");
			return -1;
		}	
	}
    return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
static int ctx_read_keys(struct pingpong_dest *rem_dest, 
                         struct pingpong_params *params)  {
    
	if (params->use_index < 0 && !params->use_mcg) {

        int parsed;
		char msg[KEY_MSG_SIZE];

		if (read(params->sockfd, msg, sizeof msg) != sizeof msg) {
			perror("pp_read_keys");
			fprintf(stderr, "Couldn't read remote address\n");
			return -1;
		}

		parsed = sscanf(msg, KEY_PRINT_FMT, &rem_dest->lid, &rem_dest->qpn,
                        &rem_dest->psn, &rem_dest->rkey, &rem_dest->vaddr);

		if (parsed != 5) {
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
		rem_dest->rkey = (unsigned)strtol(tmp, NULL, 16); // RKEY

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
			rem_dest->dgid.raw[i] = (unsigned char)strtoll(tmp, NULL, 16);
		}
		pstr += term - pstr + 1;
		strcpy(tmp, pstr);
		rem_dest->dgid.raw[15] = (unsigned char)strtoll(tmp, NULL, 16);
	}
	return 0;
}

/************************************************************************ 
 *
 ************************************************************************/
uint16_t ctx_get_local_lid(struct ibv_context *context,int port) {

    struct ibv_port_attr attr;

    if (ibv_query_port(context,port,&attr))
	return 0;

    return attr.lid;
}


/************************************************************************ 
 *
 ************************************************************************/
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

/************************************************************************ 
 *
 ************************************************************************/
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


/************************************************************************ 
 *
 ************************************************************************/
int ctx_hand_shake(struct pingpong_params  *params,
				   struct pingpong_dest *my_dest,
				   struct pingpong_dest *rem_dest) {

    // Client.
    if (params->type == CLIENT) {
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

/************************************************************************ 
 *
 ************************************************************************/
void ctx_print_pingpong_data(struct pingpong_dest *element,
							 struct pingpong_params *params) {
 
    printf(ADDR_FMT,sideArray[params->side],element->lid,element->qpn,
		   element->psn,element->rkey,element->vaddr);

	if (params->use_index > -1 || params->use_mcg) {

		printf(GID_FMT,gidArray[params->use_mcg],
				element->dgid.raw[0], element->dgid.raw[1],
				element->dgid.raw[2], element->dgid.raw[3], 
			    element->dgid.raw[4], element->dgid.raw[5], 
			    element->dgid.raw[6], element->dgid.raw[7],
			   	element->dgid.raw[8], element->dgid.raw[9],
			    element->dgid.raw[10],element->dgid.raw[11],
			    element->dgid.raw[12],element->dgid.raw[13],
				element->dgid.raw[14],element->dgid.raw[15]);
	}
}

/************************************************************************ 
 *
 ************************************************************************/
int ctx_close_connection(struct pingpong_params  *params,
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

/************************************************************************ 
 * End
 ************************************************************************/
