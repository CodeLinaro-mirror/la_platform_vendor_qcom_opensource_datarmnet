/* Copyright (c) 2013, 2016-2017, 2019-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * RMNET Data ingress/egress handler
 *
 */

#ifndef _RMNET_HANDLERS_H_
#define _RMNET_HANDLERS_H_

#include "rmnet_config.h"

enum rmnet_packet_context {
	RMNET_NET_RX_CTX,
	RMNET_WQ_CTX,
};

void rmnet_egress_handler(struct sk_buff *skb, bool low_latency, u8 ipsec);
void rmnet_deliver_skb(struct sk_buff *skb, struct rmnet_port *port);
void rmnet_deliver_skb_wq(struct sk_buff *skb, struct rmnet_port *port,
			  enum rmnet_packet_context ctx);
void rmnet_set_skb_proto(struct sk_buff *skb);
bool rmnet_slow_start_on(u32 hash_key);
rx_handler_result_t _rmnet_map_ingress_handler(struct sk_buff *skb,
					       struct rmnet_port *port);
rx_handler_result_t rmnet_rx_handler(struct sk_buff **pskb);
rx_handler_result_t rmnet_rx_priv_handler(struct sk_buff **pskb);
int rmnet_ingress_eth_handler(struct sk_buff *skb,
                              struct rmnet_endpoint *eth_ep);

#endif /* _RMNET_HANDLERS_H_ */
