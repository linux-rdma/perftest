/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */

#ifndef RAW_ETHERNET_STEERING_H
#define RAW_ETHERNET_STEERING_H

#include <infiniband/verbs.h>

struct perftest_parameters;

/*
 * Receive steering of raw_ethernet_bw.
 *
 * A NIC delivers a frame once to every QP whose rule matches it, so installing
 * the same rule on several QPs turns one frame into several completions. The
 * plan installs every receive rule once on rule-index modulo active QP count.
 * QPs that own no rule are inactive and get no receive WQEs.
 */

struct raw_eth_steering_rule {
	struct ibv_flow_attr	*attr;
	struct ibv_flow		*flow;
};

struct raw_eth_steering_plan {
	struct raw_eth_steering_rule	*rules;
	int				num_rules;
	int				num_recv_qps;
	int				mcast_attached;
	union ibv_gid			mgid;
};

/* Build receive rules and ownership without calling verbs. */
int raw_eth_steering_plan_build(struct raw_eth_steering_plan *plan,
				struct perftest_parameters *user_param);

void raw_eth_steering_plan_free(struct raw_eth_steering_plan *plan);

/* Flow installation rolls back on failure. Multicast is installed later. */
int raw_eth_steering_install_flows(struct raw_eth_steering_plan *plan, struct ibv_qp **qp);
int raw_eth_steering_install_mcast(struct raw_eth_steering_plan *plan, struct ibv_qp **qp);

/* Remove all installed state, continuing past failures. */
int raw_eth_steering_uninstall(struct raw_eth_steering_plan *plan, struct ibv_qp **qp);

#endif /* RAW_ETHERNET_STEERING_H */
