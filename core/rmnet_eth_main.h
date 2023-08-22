/* Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * RMNET_ETH main handler
 *
 */

#include <linux/netdevice.h>
#ifndef _RMNET_CONFIG_H_
#include "rmnet_config.h"
#endif/*_RMNET_CONFIG_H_*/

#ifndef _RMNET_ETH_MAIN_H_
#define _RMNET_ETH_MAIN_H_

#define RMNET_ETH_FAILURE         (-1)
#define RMNET_ETH_SUCCESS         (0)
#define RMNET_ETH_NEEDED_HEADROOM (16)
#define RMNET_ETH_PREFIX          "rmnet_eth"
#define RMNET_ETH_PHY_PREFIX      "rmnet_ipa"

struct rmnet_endpoint;

enum rmnet_eth_drop_reasons {
        RMNET_ETH_RX_BAD_ETH,
        RMNET_ETH_RX_BAD_EP,
        RMNET_ETH_RX_INV_HDR,
        RMNET_ETH_RX_INV_PORT,
        RMNET_ETH_RX_INV_MODE,
        RMNET_ETH_TX_INV_EPORT,
        RMNET_ETH_TX_INV_PHY_PORT,
        RMNET_ETH_TX_MAP_FAILURE,
        RMNET_ETH_RX_INV_REAL_DEV,
        RMNET_ETH_DROP_REASON_MAX,
};

struct rmnet_veth_stats {
        u64 rx_frames;
        u64 rx_bytes;
        u64 rx_drops;
        u64 tx_frames;
        u64 tx_bytes;
        u64 tx_drops;
};

struct rmnet_eth_pcpu_stats {
        struct rmnet_veth_stats stats;
        struct u64_stats_sync syncp;
};

struct rmnet_eth_priv {
	u8 mux_id;
	struct net_device *real_dev;
        struct rmnet_eth_pcpu_stats __percpu *pcpu_stats;
};

struct rmnet_eth_port {
	u8 nr_rmnet_eth_devs;
	u8 rmnet_mode;
	struct hlist_head muxed_ep[RMNET_MAX_LOGICAL_EP];
	struct net_device *bridge_ep;
	struct rmnet_eth_priv priv;
};

rx_handler_result_t rmnet_eth_rx_handler(struct sk_buff **pskb,
                                         struct rmnet_endpoint *eth_ep);


#endif /* _RMNET_ETH_MAIN_H_ */
