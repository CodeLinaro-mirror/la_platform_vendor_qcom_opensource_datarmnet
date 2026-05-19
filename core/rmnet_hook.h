/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#if !defined(__RMNET_HOOKS__) || defined(__RMNET_HOOK_MULTIREAD__)
#define __RMNET_HOOKS__

#include <linux/skbuff.h>
#include <linux/tcp.h>
#include "rmnet_descriptor.h"

RMNET_MODULE_HOOK(offload_ingress,
	RMNET_MODULE_HOOK_NUM(OFFLOAD_INGRESS),
	RMNET_MODULE_HOOK_PROTOCOL(struct list_head *desc_list,
				   struct rmnet_port *port),
	RMNET_MODULE_HOOK_ARGS(desc_list, port),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(offload_chain_end,
	RMNET_MODULE_HOOK_NUM(OFFLOAD_CHAIN_END),
	RMNET_MODULE_HOOK_PROTOCOL(void),
	RMNET_MODULE_HOOK_ARGS(),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(shs_skb_entry,
	RMNET_MODULE_HOOK_NUM(SHS_SKB_ENTRY),
	RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff *skb,
				   struct rmnet_shs_clnt_s *cfg),
	RMNET_MODULE_HOOK_ARGS(skb, cfg),
	RMNET_MODULE_HOOK_RETURN_TYPE(int)
);

RMNET_MODULE_HOOK(shs_switch,
	RMNET_MODULE_HOOK_NUM(SHS_SWITCH),
	RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff *skb,
				   struct rmnet_shs_clnt_s *cfg),
	RMNET_MODULE_HOOK_ARGS(skb, cfg),
	RMNET_MODULE_HOOK_RETURN_TYPE(int)
);

RMNET_MODULE_HOOK(perf_tether_ingress,
	RMNET_MODULE_HOOK_NUM(PERF_TETHER_INGRESS),
	RMNET_MODULE_HOOK_PROTOCOL(struct tcphdr *tp,
				   struct sk_buff *skb),
	RMNET_MODULE_HOOK_ARGS(tp, skb),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(perf_tether_egress,
	RMNET_MODULE_HOOK_NUM(PERF_TETHER_EGRESS),
	RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff *skb),
	RMNET_MODULE_HOOK_ARGS(skb),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(perf_tether_cmd,
	RMNET_MODULE_HOOK_NUM(PERF_TETHER_CMD),
	RMNET_MODULE_HOOK_PROTOCOL(u8 message, u64 val),
	RMNET_MODULE_HOOK_ARGS(message, val),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(perf_ingress,
	RMNET_MODULE_HOOK_NUM(PERF_INGRESS),
	RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff *skb),
	RMNET_MODULE_HOOK_ARGS(skb),
	RMNET_MODULE_HOOK_RETURN_TYPE(int)
);

RMNET_MODULE_HOOK(perf_egress,
	RMNET_MODULE_HOOK_NUM(PERF_EGRESS),
	RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff *skb),
	RMNET_MODULE_HOOK_ARGS(skb),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(aps_pre_queue,
	RMNET_MODULE_HOOK_NUM(APS_PRE_QUEUE),
	RMNET_MODULE_HOOK_PROTOCOL(struct net_device *dev, struct sk_buff *skb),
	RMNET_MODULE_HOOK_ARGS(dev, skb),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(aps_post_queue,
	RMNET_MODULE_HOOK_NUM(APS_POST_QUEUE),
	RMNET_MODULE_HOOK_PROTOCOL(struct net_device *dev, struct sk_buff *skb),
	RMNET_MODULE_HOOK_ARGS(dev, skb),
	RMNET_MODULE_HOOK_RETURN_TYPE(int)
);

RMNET_MODULE_HOOK(wlan_flow_match,
	RMNET_MODULE_HOOK_NUM(WLAN_FLOW_MATCH),
	RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff *skb),
	RMNET_MODULE_HOOK_ARGS(skb),
	RMNET_MODULE_HOOK_RETURN_TYPE(void)
);

RMNET_MODULE_HOOK(eth_rx_handler,
	RMNET_MODULE_HOOK_NUM(ETH_RX_HANDLER),
	RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff **pskb,
				   struct rmnet_endpoint *eth_ep),
	RMNET_MODULE_HOOK_ARGS(pskb, eth_ep),
	RMNET_MODULE_HOOK_RETURN_TYPE(rx_handler_result_t)
);

RMNET_MODULE_HOOK(wlan_ingress_rx_handler,
		RMNET_MODULE_HOOK_NUM(WLAN_INGRESS_RX_HANDLER),
		RMNET_MODULE_HOOK_PROTOCOL(struct sk_buff **pskb),
		RMNET_MODULE_HOOK_ARGS(pskb),
		RMNET_MODULE_HOOK_RETURN_TYPE(rx_handler_result_t)
);


#endif
