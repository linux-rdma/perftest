/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raw_ethernet_steering.h"
#include "raw_ethernet_resources.h"

static int plan_add_port_rule(struct raw_eth_steering_plan *plan,
			      enum ibv_flow_attr_type attr_type, uint8_t ib_port)
{
	struct ibv_flow_attr *attr = calloc(1, sizeof(*attr));

	if (!attr) {
		fprintf(stderr, " Cannot allocate flow rule\n");
		return FAILURE;
	}

	attr->type = attr_type;
	attr->size = sizeof(*attr);
	attr->port = ib_port;
	plan->rules[plan->num_rules++].attr = attr;
	return SUCCESS;
}

int raw_eth_steering_plan_build(struct raw_eth_steering_plan *plan,
				struct perftest_parameters *user_param)
{
	int flow_index;

	memset(plan, 0, sizeof(*plan));

	if (user_param->raw_mcast) {
		plan->num_recv_qps = 1;
		memcpy(&plan->mgid.raw[10], user_param->dest_mac, sizeof(user_param->dest_mac));
		return SUCCESS;
	}

	/* Every flow, plus one possible default rule. */
	plan->rules = calloc((size_t)user_param->flows + 1, sizeof(*plan->rules));
	if (!plan->rules) {
		fprintf(stderr, " Cannot allocate the steering plan\n");
		return FAILURE;
	}

#ifdef HAVE_SNIFFER
	if (user_param->use_sniffer) {
		if (plan_add_port_rule(plan, IBV_FLOW_ATTR_SNIFFER, user_param->ib_port))
			goto failure;
	} else
#endif
	{
		/* More than one flow requires ports, so the rules of different flows differ. */
		for (flow_index = 0; flow_index < user_param->flows; flow_index++) {
			struct raw_eth_steering_rule *rule = &plan->rules[plan->num_rules];

			if (set_up_flow_rules(&rule->attr, NULL, user_param,
					      user_param->server_port + flow_index,
					      user_param->client_port + flow_index))
				goto failure;
			plan->num_rules++;
		}

		if (user_param->use_promiscuous &&
		    plan_add_port_rule(plan, IBV_FLOW_ATTR_ALL_DEFAULT, user_param->ib_port))
			goto failure;
	}

	plan->num_recv_qps = plan->num_rules < user_param->num_of_qps ?
			     plan->num_rules : user_param->num_of_qps;
	return SUCCESS;

failure:
	raw_eth_steering_plan_free(plan);
	return FAILURE;
}

void raw_eth_steering_plan_free(struct raw_eth_steering_plan *plan)
{
	int i;

	for (i = 0; i < plan->num_rules; i++)
		free(plan->rules[i].attr);
	free(plan->rules);
	memset(plan, 0, sizeof(*plan));
}

int raw_eth_steering_install_flows(struct raw_eth_steering_plan *plan, struct ibv_qp **qp)
{
	int i;

	for (i = 0; i < plan->num_rules; i++) {
		struct raw_eth_steering_rule *rule = &plan->rules[i];
		int owner_qp = i % plan->num_recv_qps;

		rule->flow = ibv_create_flow(qp[owner_qp], rule->attr);
		if (!rule->flow) {
			fprintf(stderr, " Couldn't attach flow rule %d to QP %d: %s\n",
				i, owner_qp, strerror(errno));
			raw_eth_steering_uninstall(plan, qp);
			return FAILURE;
		}
	}
	return SUCCESS;
}

int raw_eth_steering_install_mcast(struct raw_eth_steering_plan *plan, struct ibv_qp **qp)
{
	const uint8_t *gid = plan->mgid.raw;
	int ret;

	ret = ibv_attach_mcast(qp[0], &plan->mgid, 0);
	if (ret) {
		fprintf(stderr, " Couldn't attach QP 0 to the multicast group: %s\n",
			strerror(ret));
		return FAILURE;
	}
	plan->mcast_attached = 1;

	printf(PERF_RAW_MGID_FMT, "MGID",
	       gid[0], gid[1], gid[2], gid[3], gid[4], gid[5], gid[6], gid[7],
	       gid[8], gid[9], gid[10], gid[11], gid[12], gid[13], gid[14], gid[15]);
	return SUCCESS;
}

int raw_eth_steering_uninstall(struct raw_eth_steering_plan *plan, struct ibv_qp **qp)
{
	int return_value = SUCCESS;
	int i, ret;

	for (i = plan->num_rules - 1; i >= 0; i--) {
		struct raw_eth_steering_rule *rule = &plan->rules[i];
		int owner_qp = i % plan->num_recv_qps;

		if (rule->flow) {
			ret = ibv_destroy_flow(rule->flow);
			if (!ret)
				rule->flow = NULL;
			if (ret && return_value == SUCCESS) {
				fprintf(stderr, " Couldn't remove flow rule %d from QP %d: %s\n",
					i, owner_qp, strerror(ret));
				return_value = FAILURE;
			}
		}
	}

	if (plan->mcast_attached) {
		ret = ibv_detach_mcast(qp[0], &plan->mgid, 0);
		if (!ret)
			plan->mcast_attached = 0;
		if (ret && return_value == SUCCESS) {
			fprintf(stderr, " Couldn't detach QP 0 from the multicast group: %s\n",
				strerror(ret));
			return_value = FAILURE;
		}
	}
	return return_value;
}
