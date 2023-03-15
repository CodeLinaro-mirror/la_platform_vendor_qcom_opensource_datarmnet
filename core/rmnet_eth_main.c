/* Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * RMNET_ETH main handler
 *
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/ipa.h>
#include <linux/if_ether.h>
#include "rmnet_eth_main.h"
#include "rmnet_private.h"
#include "rmnet_config.h"
#include "rmnet_handlers.h"
#include "rmnet_module.h"

struct rtnl_link_ops rmnet_eth_link_ops;

static DEFINE_SPINLOCK(dev_count_lock);

static uint32_t rmnet_eth_drop_reason_counts[RMNET_ETH_DROP_REASON_MAX];

static struct rmnet_eth_port*
rmnet_eth_get_port_rtnl(const struct net_device *real_dev)
{
	struct rmnet_port *port;

	port = rtnl_dereference(real_dev->rx_handler_data);

	if (port)
		return &port->eth_port;

	return NULL;
}

void rmnet_veth_rx_fixup(struct net_device *dev, u32 skb_len)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);
	struct rmnet_eth_pcpu_stats *pcpu_ptr;

	pcpu_ptr = this_cpu_ptr(priv->pcpu_stats);

	u64_stats_update_begin(&pcpu_ptr->syncp);
	pcpu_ptr->stats.rx_frames++;
	pcpu_ptr->stats.rx_bytes += skb_len;
	u64_stats_update_end(&pcpu_ptr->syncp);
}

void rmnet_veth_tx_fixup(struct net_device *dev, u32 skb_len)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);
	struct rmnet_eth_pcpu_stats *pcpu_ptr;

	pcpu_ptr = this_cpu_ptr(priv->pcpu_stats);

	u64_stats_update_begin(&pcpu_ptr->syncp);
	pcpu_ptr->stats.tx_frames++;
	pcpu_ptr->stats.tx_bytes += skb_len;
	u64_stats_update_end(&pcpu_ptr->syncp);
}

static void
rmnet_eth_drop_rx_skb(struct sk_buff *skb, enum rmnet_eth_drop_reasons reason)
{
	struct rmnet_eth_priv *priv;

	if (skb) {
		priv = netdev_priv(skb->dev);
		kfree_skb(skb);
		this_cpu_inc(priv->pcpu_stats->stats.rx_drops);
		rmnet_eth_drop_reason_counts[reason]++;
	}
}

static void
rmnet_eth_drop_tx_skb(struct sk_buff *skb, struct rmnet_eth_priv *priv,
		      enum rmnet_eth_drop_reasons reason)
{
	if (skb) {
		kfree_skb(skb);
		if (priv)
			this_cpu_inc(priv->pcpu_stats->stats.tx_drops);
		rmnet_eth_drop_reason_counts[reason]++;
	}
}

struct rmnet_map_header *rmnet_eth_map_add_map_header(struct sk_buff *skb,
						      int hdrlen, int pad,
						      struct rmnet_port *port)
{
	struct rmnet_map_header *map_header;
	u32 padding, map_datalen;
	u8 *padbytes;

	map_datalen = skb->len - hdrlen;
	map_header = (struct rmnet_map_header *)
			skb_push(skb, sizeof(struct rmnet_map_header));
	memset(map_header, 0, sizeof(struct rmnet_map_header));

	/* Set next_hdr bit for csum offload packets */
	if (port->data_format & RMNET_FLAGS_EGRESS_MAP_CKSUMV5)
		map_header->next_hdr = 1;

	if (pad == RMNET_MAP_NO_PAD_BYTES) {
		map_header->pkt_len = htons(map_datalen);
		return map_header;
	}

	padding = ALIGN(map_datalen, 4) - map_datalen;

	if (padding == 0)
		goto done;

	if (skb_tailroom(skb) < padding)
		return NULL;

	padbytes = (u8 *)skb_put(skb, padding);
	memset(padbytes, 0, padding);

done:
	map_header->pkt_len = htons(map_datalen + padding);
	map_header->pad_len = padding & 0x3F;

	return map_header;
}

void rmnet_eth_map_v5_checksum_uplink_packet(struct sk_buff *skb,
					     struct rmnet_port *port,
					     struct net_device *orig_dev)
{
	struct rmnet_map_v5_csum_header *ul_header;

	ul_header = (struct rmnet_map_v5_csum_header *)
		    skb_push(skb, sizeof(*ul_header));
	/* memset takes care of CV bit being 0 */
	memset(ul_header, 0, sizeof(*ul_header));
	ul_header->header_type = RMNET_MAP_HEADER_TYPE_CSUM_OFFLOAD;
}

static int rmnet_eth_map_egress_handler(struct sk_buff *skb,
                                        struct rmnet_port *port, u8 mux_id,
				    	struct net_device *orig_dev)
{
	int required_headroom, additional_header_len;
	struct rmnet_map_header *map_header;

	additional_header_len = 0;
	required_headroom = sizeof(struct rmnet_map_header);

	if ((port->data_format & RMNET_FLAGS_EGRESS_MAP_CKSUMV5))
		additional_header_len = sizeof(struct rmnet_map_v5_csum_header);

	required_headroom += additional_header_len;

	if (skb_headroom(skb) < required_headroom) {
		if (pskb_expand_head(skb, required_headroom, 0, GFP_ATOMIC))
			return -ENOMEM;
	}

	rmnet_eth_map_v5_checksum_uplink_packet(skb, port, orig_dev);

	map_header = rmnet_eth_map_add_map_header(skb, additional_header_len, 0,
					      	  port);
	if (!map_header)
		return -ENOMEM;

	map_header->mux_id = mux_id;
	skb->protocol = htons(ETH_P_MAP);
	return 0;
}

void rmnet_eth_egress_handler(struct sk_buff *skb)
{
	struct net_device *orig_dev;
	struct rmnet_eth_port *e_port;
	struct rmnet_port *port;
	struct rmnet_eth_priv *priv;
	u8 mux_id;
	int err;
	u32 skb_len;
	enum rmnet_eth_drop_reasons reason;

	orig_dev = skb->dev;
	priv = netdev_priv(orig_dev);
	skb->dev = priv->real_dev;
	mux_id = priv->mux_id;

	e_port = rmnet_eth_get_port_rtnl(skb->dev);
	if (!e_port) {
		reason = RMNET_ETH_TX_INV_EPORT;
		goto drop;
	}

	port = container_of(e_port, struct rmnet_port, eth_port);
	if (!port) {
		reason = RMNET_ETH_TX_INV_PHY_PORT;
		goto drop;
	}

	skb_len = skb->len;
	err = rmnet_eth_map_egress_handler(skb, port, mux_id, orig_dev);
	if (err == -ENOMEM) {
		reason = RMNET_ETH_TX_MAP_FAILURE;
		goto drop;
	}

	skb->queue_mapping = IPA_RMNET_TX_QUEUE_ETH_PDU;

	rmnet_veth_tx_fixup(orig_dev, skb_len);
	dev_queue_xmit(skb);

	return;
drop:
	rmnet_eth_drop_tx_skb(skb, priv, reason);
}

static netdev_tx_t rmnet_veth_start_xmit(struct sk_buff *skb,
                                         struct net_device *dev)
{
	struct rmnet_eth_priv *priv;

	priv = netdev_priv(dev);
	if (priv->real_dev) {
		rmnet_eth_egress_handler(skb);
	} else {
		rmnet_eth_drop_tx_skb(skb, priv, RMNET_ETH_RX_INV_REAL_DEV);
	}
	return NETDEV_TX_OK;
}

static int rmnet_veth_change_mtu(struct net_device *rmnet_eth_dev, int new_mtu)
{
	if (new_mtu < 0 || new_mtu > RMNET_MAX_PACKET_SIZE)
		return -EINVAL;

	rmnet_eth_dev->mtu = new_mtu;
	return 0;
}

static int rmnet_veth_get_iflink(const struct net_device *dev)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);

	return priv->real_dev->ifindex;
}

static int rmnet_veth_init(struct net_device *dev)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);

	priv->pcpu_stats = alloc_percpu(struct rmnet_eth_pcpu_stats);
	if (!priv->pcpu_stats)
		return -ENOMEM;

	return 0;
}

static void rmnet_veth_uninit(struct net_device *dev)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);

	free_percpu(priv->pcpu_stats);
}

static void rmnet_veth_get_stats64(struct net_device *dev,
                                   struct rtnl_link_stats64 *s)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);
	struct rmnet_veth_stats total_stats;
	struct rmnet_eth_pcpu_stats *pcpu_ptr;
	unsigned int cpu, start;

	memset(&total_stats, 0, sizeof(struct rmnet_veth_stats));

	for_each_possible_cpu(cpu) {
		pcpu_ptr = per_cpu_ptr(priv->pcpu_stats, cpu);

		do {
			start = u64_stats_fetch_begin_irq(&pcpu_ptr->syncp);
			total_stats.rx_frames += pcpu_ptr->stats.rx_frames;
			total_stats.rx_bytes += pcpu_ptr->stats.rx_bytes;
			total_stats.tx_frames += pcpu_ptr->stats.tx_frames;
			total_stats.tx_bytes += pcpu_ptr->stats.tx_bytes;
		} while (u64_stats_fetch_retry_irq(&pcpu_ptr->syncp, start));

		total_stats.tx_drops += pcpu_ptr->stats.tx_drops;
		total_stats.rx_drops += pcpu_ptr->stats.rx_drops;
	}

	s->rx_packets = total_stats.rx_frames;
	s->rx_bytes = total_stats.rx_bytes;
	s->rx_dropped = total_stats.rx_drops;
	s->tx_packets = total_stats.tx_frames;
	s->tx_bytes = total_stats.tx_bytes;
	s->tx_dropped = total_stats.tx_drops;
}

static const struct net_device_ops rmnet_veth_ops = {
	.ndo_start_xmit = rmnet_veth_start_xmit,
	.ndo_change_mtu = rmnet_veth_change_mtu,
	.ndo_get_iflink = rmnet_veth_get_iflink,
	.ndo_init       = rmnet_veth_init,
	.ndo_uninit     = rmnet_veth_uninit,
	.ndo_get_stats64 = rmnet_veth_get_stats64,
};

struct rmnet_endpoint *rmnet_eth_get_endpoint(struct rmnet_eth_port *port,
					      u8 mux_id)
{
	struct rmnet_endpoint *ep;

	hlist_for_each_entry_rcu(ep, &port->muxed_ep[mux_id], hlnode) {
		if (ep->mux_id == mux_id)
			return ep;
	}

	return NULL;
}

static const struct rmnet_module_hook_register_info
rmnet_eth_hook = {
	.hooknum = RMNET_MODULE_HOOK_ETH_RX_HANDLER,
	.func = rmnet_eth_rx_handler,
};

void rmnet_eth_set_hooks(void)
{
	rmnet_module_hook_register(&rmnet_eth_hook, 1);

}

void rmnet_eth_unset_hooks(void)
{
	rmnet_module_hook_unregister_no_sync(&rmnet_eth_hook, 1);
}

static void
rmnet_eth_deliver_skb(struct sk_buff *skb)
{
	skb_reset_network_header(skb);
	rmnet_veth_rx_fixup(skb->dev, skb->len);
	netif_receive_skb(skb);
}

static void
rmnet_eth_map_ingress_handler(struct sk_buff *skb, struct rmnet_eth_port *port)
{

	u16 len, pad;
	u8 mux_id;
	struct rmnet_endpoint *ep;
	struct rmnet_map_header *qmap;
	struct rmnet_port *main_port;
	enum rmnet_eth_drop_reasons reason;

	qmap = (struct rmnet_map_header *)rmnet_map_data_ptr(skb);
	mux_id = qmap->mux_id;
	if (mux_id >= RMNET_MAX_LOGICAL_EP) {
		reason = RMNET_ETH_RX_BAD_ETH;
		goto drop;
	}

	ep = rmnet_eth_get_endpoint(port, mux_id);
	if (!ep) {
		reason = RMNET_ETH_RX_BAD_EP;
		goto drop;
	}

	pad = qmap->pad_len;
	len = ntohs(qmap->pkt_len) - pad;
	skb->dev = ep->egress_dev;
	main_port = container_of(port, struct rmnet_port, eth_port);

	if (qmap->next_hdr &&
	    (main_port->data_format & (RMNET_FLAGS_INGRESS_COALESCE |
				  RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5))) {

		switch (rmnet_map_get_next_hdr_type(skb)) {
		case RMNET_MAP_HEADER_TYPE_CSUM_OFFLOAD:
			if (rmnet_map_get_csum_valid(skb)) {
				skb->ip_summed = CHECKSUM_NONE;
			}

			/* Pull unnecessary headers and move the rest to the linear
			* section of the skb.
			*/
			pskb_pull(skb,
				(sizeof(struct rmnet_map_header) +
				sizeof(struct rmnet_map_v5_csum_header)));

			/* Remove padding only for csum offload packets.
			* Coalesced packets should never have padding.
			*/
			pskb_trim(skb, len);
			skb->protocol = eth_type_trans(skb, skb->dev);

			rmnet_eth_deliver_skb(skb);
			break;
		default:
			reason = RMNET_ETH_RX_INV_HDR;
			goto drop;
		}
	}
	return;

drop:
	rmnet_eth_drop_rx_skb(skb, reason);
}

rx_handler_result_t rmnet_eth_rx_handler(struct sk_buff **pskb,
					 struct rmnet_endpoint *eth_ep)
{
	struct sk_buff *skb = *pskb;
	struct rmnet_eth_port *port;
	struct net_device *dev;

	if (!skb)
		goto done;

	if (skb->pkt_type == PACKET_LOOPBACK)
		return RX_HANDLER_PASS;

	dev = skb->dev;
	port = rmnet_eth_get_port_rtnl(dev);
	if (unlikely(!port)) {
		atomic_long_inc(&skb->dev->rx_nohandler);
		rmnet_eth_drop_rx_skb(skb, RMNET_ETH_RX_INV_PORT);
		goto done;
	}

	skb->dev = eth_ep->egress_dev;
	switch (port->rmnet_mode) {
	case RMNET_EPMODE_VND:
		rmnet_eth_map_ingress_handler(skb, port);
		break;
	default:
		rmnet_eth_drop_rx_skb(skb, RMNET_ETH_RX_INV_MODE);
	}

done:
	return RX_HANDLER_CONSUMED;
}

static const struct nla_policy rmnet_eth_policy[__IFLA_RMNET_EXT_MAX] = {
	[IFLA_RMNET_MUX_ID] = {
		.type = NLA_U16
	},
	[IFLA_RMNET_FLAGS] = {
		.len = sizeof(struct ifla_rmnet_flags)
	},
};

u8 rmnet_veth_get_mux(struct net_device *rmnet_dev)
{
	struct rmnet_eth_priv *priv;

	priv = netdev_priv(rmnet_dev);
	return priv->mux_id;
}

int rmnet_veth_dellink(u8 id, struct rmnet_eth_port *port,
                       struct rmnet_endpoint *ep)
{
	if (id >= RMNET_MAX_LOGICAL_EP || !ep->egress_dev)
		return -EINVAL;

	ep->egress_dev = NULL;
	port->nr_rmnet_eth_devs--;
	return 0;
}

int rmnet_veth_newlink(u8 id, struct net_device *rmnet_eth_dev,
                       struct rmnet_eth_port *port,
                       struct net_device *real_dev,
                       struct rmnet_endpoint *ep)
{
	struct rmnet_eth_priv *priv = netdev_priv(rmnet_eth_dev);
	int rc;

	if (ep->egress_dev)
		return -EINVAL;

	if (rmnet_eth_get_endpoint(port, id))
		return -EBUSY;

	priv->real_dev = real_dev;

	rc = register_netdevice(rmnet_eth_dev);
	if (!rc) {
		ep->egress_dev = rmnet_eth_dev;
		ep->mux_id = id;
		port->nr_rmnet_eth_devs++;

		rmnet_eth_dev->rtnl_link_ops = &rmnet_eth_link_ops;

		priv->mux_id = id;
		netdev_dbg(rmnet_eth_dev, "rmnet dev created\n");
	}

	return rc;
}

static int rmnet_eth_register_real_device(struct net_device *real_dev)
{
	ASSERT_RTNL();

	rmnet_is_real_dev_registered(real_dev);

	return 0;
}

void rmnet_eth_setup(struct net_device *rmnet_eth_dev)
{
	ether_setup(rmnet_eth_dev);
	random_ether_addr(rmnet_eth_dev->dev_addr);
	rmnet_eth_dev->netdev_ops = &rmnet_veth_ops;
	rmnet_eth_dev->needed_headroom = RMNET_ETH_NEEDED_HEADROOM;
}

static int rmnet_eth_rtnl_validate(struct nlattr *tb[], struct nlattr *data[],
                                   struct netlink_ext_ack *extack)
{
	u16 mux_id;

	if (!data)
		return -EINVAL;

	if (data[IFLA_RMNET_MUX_ID]) {
		mux_id = nla_get_u16(data[IFLA_RMNET_MUX_ID]);
		if (mux_id > (RMNET_MAX_LOGICAL_EP - 1))
			return -ERANGE;
	}

	return 0;
}

static int rmnet_eth_unregister_real_device(struct net_device *real_dev,
                                            struct rmnet_eth_port *eth_port)
{
	struct rmnet_port *port;
	port = rtnl_dereference(real_dev->rx_handler_data);

	/* release reference on real_dev */
	if((port->nr_rmnet_devs == 0) && (eth_port->nr_rmnet_eth_devs == 0)){
		rmnet_clean_pending_real_dev(real_dev, port);
	}

	netdev_dbg(real_dev, "Removed from rmnet\n");
	return 0;
}

static int rmnet_eth_newlink(struct net *src_net, struct net_device *dev,
                             struct nlattr *tb[], struct nlattr *data[],
			     struct netlink_ext_ack *extack)
{
	struct net_device *real_dev;
	int mode = RMNET_EPMODE_VND;
	struct rmnet_endpoint *ep;
	struct rmnet_eth_port *port;
	int err = 0;
	u16 mux_id;

	real_dev = __dev_get_by_index(src_net, nla_get_u32(tb[IFLA_LINK]));
	if (!real_dev || !dev)
		return -ENODEV;

	if (!data[IFLA_RMNET_MUX_ID])
		return -EINVAL;

	ep = kzalloc(sizeof(*ep), GFP_ATOMIC);
	if (!ep)
		return -ENOMEM;

	mux_id = nla_get_u16(data[IFLA_RMNET_MUX_ID]);

	err = rmnet_eth_register_real_device(real_dev);
	if (err)
		goto err0;

	port = rmnet_eth_get_port_rtnl(real_dev);
	if (!port)
		goto err0;

	err = rmnet_veth_newlink(mux_id, dev, port, real_dev, ep);
	if (err)
		goto err1;

	port->rmnet_mode = mode;

	hlist_add_head_rcu(&ep->hlnode, &port->muxed_ep[mux_id]);

	return 0;

err1:
	rmnet_eth_unregister_real_device(real_dev, port);
err0:
	kfree(ep);
	return err;
}

static void rmnet_eth_dellink(struct net_device *dev, struct list_head *head)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);
	struct net_device *real_dev;
	struct rmnet_endpoint *ep;
	struct rmnet_eth_port *port;
	u8 mux_id;

	real_dev = priv->real_dev;
	if (!real_dev || !rmnet_is_real_dev_registered(real_dev))
		return;

	port = rmnet_eth_get_port_rtnl(real_dev);
	if (!port) {
		pr_err("%s() Invalid port for dev: %s\n\n", __func__, real_dev->name);
		return;
	}

	mux_id = rmnet_veth_get_mux(dev);
	ep = rmnet_eth_get_endpoint(port, mux_id);
	if (ep) {
		hlist_del_init_rcu(&ep->hlnode);
		synchronize_rcu();
		rmnet_veth_dellink(mux_id, port, ep);
		kfree(ep);
	}
	unregister_netdevice(dev);
	rmnet_eth_unregister_real_device(real_dev, port);
}

static size_t rmnet_eth_get_size(const struct net_device *dev)
{
	return
		/* IFLA_RMNET_MUX_ID */
		nla_total_size(2) +
		/* IFLA_RMNET_FLAGS */
		nla_total_size(sizeof(struct ifla_rmnet_flags));
}

static int rmnet_eth_changelink(struct net_device *dev, struct nlattr *tb[],
                                struct nlattr *data[],
                                struct netlink_ext_ack *extack)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);
	struct net_device *real_dev;
	struct rmnet_endpoint *ep;
	struct rmnet_eth_port *port;
	u16 mux_id;
	int rc = 0;

	real_dev = __dev_get_by_index(dev_net(dev),
				      nla_get_u32(tb[IFLA_LINK]));

	if (!real_dev || !dev || !rmnet_is_real_dev_registered(real_dev))
		return -ENODEV;

	port = rmnet_eth_get_port_rtnl(real_dev);
	if (!port) {
		pr_err("%s() Invalid port for dev: %s\n", __func__, real_dev->name);
		return -EINVAL;
	}

	if (data[IFLA_RMNET_MUX_ID]) {
		mux_id = nla_get_u16(data[IFLA_RMNET_MUX_ID]);
		ep = rmnet_eth_get_endpoint(port, priv->mux_id);
		if (!ep)
			return -ENODEV;

		hlist_del_init_rcu(&ep->hlnode);
		hlist_add_head_rcu(&ep->hlnode, &port->muxed_ep[mux_id]);

		ep->mux_id = mux_id;
		priv->mux_id = mux_id;
	}

	if (data[IFLA_RMNET_FLAGS]) {
		struct ifla_rmnet_flags *flags;
		struct rmnet_port *rport;

		flags = nla_data(data[IFLA_RMNET_FLAGS]);
		rport = container_of(port, struct rmnet_port, eth_port);
		rport->data_format = flags->flags & flags->mask;
	}

	return rc;
}

static int rmnet_eth_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct rmnet_eth_priv *priv = netdev_priv(dev);
	struct net_device *real_dev;
	struct ifla_rmnet_flags f;
	struct rmnet_eth_port *port = NULL;

	real_dev = priv->real_dev;

	if (nla_put_u16(skb, IFLA_RMNET_MUX_ID, priv->mux_id))
		goto nla_put_failure;

	if (rmnet_is_real_dev_registered(real_dev)) {
		struct rmnet_port *rport = NULL;

		port = rmnet_eth_get_port_rtnl(real_dev);
		if (!port) {
			pr_err("%s() Invalid port for real dev: %s\n", __func__, real_dev->name);
			goto nla_put_failure;
		}
		rport = container_of(port, struct rmnet_port, eth_port);
		if(rport)
			f.flags = rport->data_format;
	} else {
		f.flags = 0;
	}

	f.mask  = ~0;

	if (nla_put(skb, IFLA_RMNET_FLAGS, sizeof(f), &f))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

struct rtnl_link_ops rmnet_eth_link_ops __read_mostly = {
	.kind		= "rmnet_eth",
	.maxtype	= __IFLA_RMNET_EXT_MAX - 1,
	.priv_size	= sizeof(struct rmnet_eth_priv),
	.setup		= rmnet_eth_setup,
	.validate	= rmnet_eth_rtnl_validate,
	.newlink	= rmnet_eth_newlink,
	.dellink	= rmnet_eth_dellink,
	.get_size	= rmnet_eth_get_size,
	.changelink     = rmnet_eth_changelink,
	.policy		= rmnet_eth_policy,
	.fill_info	= rmnet_eth_fill_info,
};

static void rmnet_eth_force_unassociate_device(struct net_device *dev)
{
	struct net_device *real_dev = dev;
	struct hlist_node *tmp_ep;
	struct rmnet_endpoint *ep;
	struct rmnet_eth_port *port;
	unsigned long bkt_ep;
	LIST_HEAD(list);
	HLIST_HEAD(cleanup_list);

	if (!rmnet_is_real_dev_registered(real_dev))
		return;

	ASSERT_RTNL();

	port = rmnet_eth_get_port_rtnl(dev);
        if (!port)
                return;

	if (!port) {
		pr_err("%s() Invalid port for real dev: %s\n", __func__, real_dev->name);
		return;
	}

	hlist_for_each_entry_rcu(ep, &port->muxed_ep[0], hlnode)
		hlist_del_init_rcu(&ep->hlnode);

	hash_for_each_safe(port->muxed_ep, bkt_ep, tmp_ep, ep, hlnode) {
		unregister_netdevice_queue(ep->egress_dev, &list);
		rmnet_veth_dellink(ep->mux_id, port, ep);

		hlist_del_init_rcu(&ep->hlnode);
		hlist_add_head(&ep->hlnode, &cleanup_list);
	}

	synchronize_rcu();

	hlist_for_each_entry_safe(ep, tmp_ep, &cleanup_list, hlnode) {
		hlist_del(&ep->hlnode);
		kfree(ep);
	}

	/* Unregistering devices in context before freeing port.
	 * If this API becomes non-context their order should switch.
	 */
	unregister_netdevice_many(&list);

	rmnet_eth_unregister_real_device(real_dev, port);
}

static int rmnet_eth_notify_cb(struct notifier_block *nb,
                               unsigned long event, void *data)
{
	struct net_device *dev = netdev_notifier_info_to_dev(data);
	static int num_reg_devs = 0;
	int is_eth_dev = 0;

	if (!dev)
		return NOTIFY_DONE;

	is_eth_dev = !strncmp(dev->name, RMNET_ETH_PREFIX,
			      strlen(RMNET_ETH_PREFIX));

	switch (event) {
	case NETDEV_REGISTER:
		if (is_eth_dev) {
			spin_lock(&dev_count_lock);
			rmnet_eth_set_hooks();
			num_reg_devs++;
			spin_unlock(&dev_count_lock);
		}

		break;
	case NETDEV_UNREGISTER:
		if (is_eth_dev) {
			spin_lock(&dev_count_lock);
			netdev_dbg(dev, "Kernel unregister rmnet_eth device\n");
			if (num_reg_devs == 1) {
				rmnet_eth_unset_hooks();
				num_reg_devs--;
			} else if (num_reg_devs > 1)
				num_reg_devs--;

			spin_unlock(&dev_count_lock);
		} else if (!strncmp(dev->name, RMNET_ETH_PHY_PREFIX,
				    strlen(RMNET_ETH_PHY_PREFIX)))
			rmnet_eth_force_unassociate_device(dev);
		break;
	case NETDEV_DOWN:
		break;
	default:
		break;
	}

	return NOTIFY_DONE;
}

static struct notifier_block rmnet_eth_dev_notifier __read_mostly = {
	.notifier_call = rmnet_eth_notify_cb,
};

static int __init rmnet_eth_init(void)
{
	int rc = 0;

	rc = register_netdevice_notifier(&rmnet_eth_dev_notifier);
	if (rc != 0)
		return rc;

	rc = rtnl_link_register(&rmnet_eth_link_ops);
	if (rc != 0) {
		unregister_netdevice_notifier(&rmnet_eth_dev_notifier);
		return rc;
	}

	return rc;
}

static void __exit rmnet_eth_exit(void)
{
	unregister_netdevice_notifier(&rmnet_eth_dev_notifier);
	rtnl_link_unregister(&rmnet_eth_link_ops);
}

module_init(rmnet_eth_init)
module_exit(rmnet_eth_exit)

MODULE_DESCRIPTION("RmNet ETH Driver");
MODULE_LICENSE("GPL v2");
