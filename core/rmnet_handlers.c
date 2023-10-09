/* Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved.
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
 * RMNET Data ingress/egress handler
 *
 */

#include <linux/netdevice.h>
#include <linux/netdev_features.h>
#include <linux/if_arp.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/inet.h>
#include <net/ip6_checksum.h>
#include <net/sock.h>
#include <net/xfrm.h>
#include <linux/tracepoint.h>
#include <linux/ipa.h>
#include "rmnet_private.h"
#include "rmnet_config.h"
#include "rmnet_vnd.h"
#include "rmnet_map.h"
#include "rmnet_handlers.h"
#include "rmnet_descriptor.h"
#include "rmnet_ll.h"
#include "rmnet_eth_main.h"
#include "rmnet_module.h"

#include "rmnet_qmi.h"
#include "qmi_rmnet.h"

#define CREATE_TRACE_POINTS
#include "rmnet_trace.h"

#define RMNET_IP_VERSION_4 0x40
#define RMNET_IP_VERSION_6 0x60

EXPORT_TRACEPOINT_SYMBOL(rmnet_shs_low);
EXPORT_TRACEPOINT_SYMBOL(rmnet_shs_high);
EXPORT_TRACEPOINT_SYMBOL(rmnet_shs_err);
EXPORT_TRACEPOINT_SYMBOL(rmnet_shs_wq_low);
EXPORT_TRACEPOINT_SYMBOL(rmnet_shs_wq_high);
EXPORT_TRACEPOINT_SYMBOL(rmnet_shs_wq_err);
EXPORT_TRACEPOINT_SYMBOL(rmnet_perf_low);
EXPORT_TRACEPOINT_SYMBOL(rmnet_perf_high);
EXPORT_TRACEPOINT_SYMBOL(rmnet_perf_err);
EXPORT_TRACEPOINT_SYMBOL(rmnet_low);
EXPORT_TRACEPOINT_SYMBOL(rmnet_high);
EXPORT_TRACEPOINT_SYMBOL(rmnet_err);
EXPORT_TRACEPOINT_SYMBOL(rmnet_freq_update);
EXPORT_TRACEPOINT_SYMBOL(rmnet_freq_reset);
EXPORT_TRACEPOINT_SYMBOL(rmnet_freq_boost);
EXPORT_TRACEPOINT_SYMBOL(print_icmp_rx);

/* Helper Functions */

void rmnet_set_skb_proto(struct sk_buff *skb)
{
	switch (rmnet_map_data_ptr(skb)[0] & 0xF0) {
	case RMNET_IP_VERSION_4:
		skb->protocol = htons(ETH_P_IP);
		break;
	case RMNET_IP_VERSION_6:
		skb->protocol = htons(ETH_P_IPV6);
		break;
	default:
		skb->protocol = htons(ETH_P_MAP);
		break;
	}
}
EXPORT_SYMBOL(rmnet_set_skb_proto);

bool (*rmnet_shs_slow_start_detect)(u32 hash_key) __rcu __read_mostly;
EXPORT_SYMBOL(rmnet_shs_slow_start_detect);

bool rmnet_slow_start_on(u32 hash_key)
{
	bool (*rmnet_shs_slow_start_on)(u32 hash_key);

	rmnet_shs_slow_start_on = rcu_dereference(rmnet_shs_slow_start_detect);
	if (rmnet_shs_slow_start_on)
		return rmnet_shs_slow_start_on(hash_key);

	return false;
}
EXPORT_SYMBOL(rmnet_slow_start_on);

/* Shs hook handler */
int (*rmnet_shs_skb_entry)(struct sk_buff *skb,
			   struct rmnet_shs_clnt_s *cfg) __rcu __read_mostly;
EXPORT_SYMBOL(rmnet_shs_skb_entry);

int (*rmnet_shs_switch)(struct sk_buff *skb,
			   struct rmnet_shs_clnt_s *cfg) __rcu __read_mostly;
EXPORT_SYMBOL(rmnet_shs_switch);


/* Shs hook handler for work queue*/
int (*rmnet_shs_skb_entry_wq)(struct sk_buff *skb,
			      struct rmnet_shs_clnt_s *cfg) __rcu __read_mostly;
EXPORT_SYMBOL(rmnet_shs_skb_entry_wq);

/* Generic handler */

void
rmnet_deliver_skb(struct sk_buff *skb, struct rmnet_port *port)
{
	int (*rmnet_shs_stamp)(struct sk_buff *skb,
			       struct rmnet_shs_clnt_s *cfg);

	trace_rmnet_low(RMNET_MODULE, RMNET_DLVR_SKB, 0xDEF, 0xDEF,
			0xDEF, 0xDEF, (void *)skb, NULL);
	skb_reset_network_header(skb);
	rmnet_vnd_rx_fixup(skb->dev, skb->len);

	skb->pkt_type = PACKET_HOST;
	skb_set_mac_header(skb, 0);

	/* Low latency packets use a different balancing scheme */
	if (skb->priority == 0xda1a)
		goto skip_shs;

	rcu_read_lock();
	rmnet_shs_stamp = rcu_dereference(rmnet_shs_skb_entry);
	if (rmnet_shs_stamp) {
		rmnet_shs_stamp(skb, &port->shs_cfg);
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

skip_shs:
	netif_receive_skb(skb);
}
EXPORT_SYMBOL(rmnet_deliver_skb);

/* Important to note, port cannot be used here if it has gone stale */
void
rmnet_deliver_skb_wq(struct sk_buff *skb, struct rmnet_port *port,
		     enum rmnet_packet_context ctx)
{
	int (*rmnet_shs_stamp)(struct sk_buff *skb,
			       struct rmnet_shs_clnt_s *cfg);
	struct rmnet_priv *priv = netdev_priv(skb->dev);

	trace_rmnet_low(RMNET_MODULE, RMNET_DLVR_SKB, 0xDEF, 0xDEF,
			0xDEF, 0xDEF, (void *)skb, NULL);
	skb_reset_transport_header(skb);
	skb_reset_network_header(skb);
	rmnet_vnd_rx_fixup(skb->dev, skb->len);

	skb->pkt_type = PACKET_HOST;
	skb_set_mac_header(skb, 0);

	/* packets coming from work queue context due to packet flush timer
	 * must go through the special workqueue path in SHS driver
	 */
	rcu_read_lock();
	rmnet_shs_stamp = (!ctx) ? rcu_dereference(rmnet_shs_skb_entry) :
				   rcu_dereference(rmnet_shs_skb_entry_wq);
	if (rmnet_shs_stamp) {
		rmnet_shs_stamp(skb, &port->shs_cfg);
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

	if (ctx == RMNET_NET_RX_CTX)
		netif_receive_skb(skb);
	else
		gro_cells_receive(&priv->gro_cells, skb);
}
EXPORT_SYMBOL(rmnet_deliver_skb_wq);

/* Deliver a list of skbs after undoing coalescing */
static void rmnet_deliver_skb_list(struct sk_buff_head *head,
				   struct rmnet_port *port)
{
	struct sk_buff *skb;

	while ((skb = __skb_dequeue(head))) {
		rmnet_set_skb_proto(skb);
		rmnet_deliver_skb(skb, port);
	}
}

static void rmnet_ip_route_rcv(struct sk_buff *skb, struct rmnet_port *port)
{
	struct rmnet_priv *priv = NULL;
	struct rmnet_endpoint *ep;
	struct ipv6hdr *ip6h;
	int ip_len;
	__sum16 pseudo;
	__be16 frag_off;
	u16 pkt_len;
	u8 proto;

	trace_rmnet_skb_ip_route_entry(skb);

	skb_reset_transport_header(skb);
	skb_reset_network_header(skb);

	skb->pkt_type = PACKET_HOST;
	skb_set_mac_header(skb, 0);

	switch (rmnet_map_data_ptr(skb)[0] & 0xF0) {
	case RMNET_IP_VERSION_4:
		skb->protocol = htons(ETH_P_IP);
		ep = rmnet_get_ip4_route_endpoint(port,
						 &(ip_hdr(skb)->daddr));
		if (!ep)
			goto drop_skb;
		break;
	case RMNET_IP_VERSION_6:
		skb->protocol = htons(ETH_P_IPV6);
		ip6h = (struct ipv6hdr *) rmnet_map_data_ptr(skb);
		ep = rmnet_get_ip6_route_endpoint(port, &ip6h->saddr,
						  &ip6h->daddr);
		if (!ep)
			goto drop_skb;

		if (ep->call_type == RMNET_IP_ROUTE_CALL_TYPE_IP)
			break;

		proto = ip6h->nexthdr;
		ip_len = ipv6_skip_exthdr(skb, sizeof(*ip6h), &proto,
					  &frag_off);
		if (ip_len < 0 || frag_off)
			break;

		pkt_len = skb->len - ip_len;
		pseudo = ~csum_ipv6_magic(&ip6h->saddr, &ip6h->daddr, pkt_len,
					  proto, 0);
		if (proto == IPPROTO_UDP) {
			struct udphdr *up = (struct udphdr *)
					    (rmnet_map_data_ptr(skb) + ip_len);

			up->check = pseudo;
			skb->csum_offset = offsetof(struct udphdr, check);
		} else if (proto == IPPROTO_TCP) {
			struct tcphdr *tp = (struct tcphdr *)
					    (rmnet_map_data_ptr(skb) + ip_len);

			tp->check = pseudo;
			skb->csum_offset = offsetof(struct tcphdr, check);
		} else {
			break;
		}

		skb->ip_summed = CHECKSUM_PARTIAL;
		skb->csum_start = skb->data + ip_len - skb->head;
		break;
	default:
		goto drop_skb;
	}

	skb->dev = ep->egress_dev;
	rmnet_vnd_rx_fixup(skb->dev, skb->len);

	trace_rmnet_skb_ip_route_exit(skb);

	priv = netdev_priv(skb->dev);
	priv->stats.ip_route_rx_pkts++;
	netif_receive_skb(skb);
	return;

drop_skb:
	kfree_skb(skb);
	return;
}

int rmnet_ingress_eth_handler(struct sk_buff *skb,
							  struct rmnet_endpoint *eth_ep)
{
	int ret;
	rx_handler_result_t rc = -1;

	/* Framework function returns 0 if hook doesn't exist */
	ret = rmnet_module_hook_eth_rx_handler(&rc, &skb, eth_ep);
	if (ret != 0 && rc != -1)
		return 1;
	return 0;
}

/* MAP handler */

static void
__rmnet_map_ingress_handler(struct sk_buff *skb,
			    struct rmnet_port *port)
{
	struct rmnet_map_header *qmap;
	struct rmnet_endpoint *ep;
	struct sk_buff_head list;
	u16 len, pad;
	u8 mux_id;

	/* We don't need the spinlock since only we touch this */
	__skb_queue_head_init(&list);

	qmap = (struct rmnet_map_header *)rmnet_map_data_ptr(skb);
	if (qmap->cd_bit) {
		qmi_rmnet_set_dl_msg_active(port);
		if (port->data_format & RMNET_INGRESS_FORMAT_DL_MARKER) {
			if (!rmnet_map_flow_command(skb, port, false))
				return;
		}

		if (port->data_format & RMNET_FLAGS_INGRESS_MAP_COMMANDS)
			return rmnet_map_command(skb, port);

		goto free_skb;
	}

	mux_id = qmap->mux_id;

	if (mux_id >= RMNET_MAX_LOGICAL_EP)
		goto free_skb;

	ep = rmnet_get_endpoint(port, mux_id);
	if (!ep) {
		struct rmnet_endpoint *eth_ep = NULL;

		hlist_for_each_entry_rcu(ep, &port->eth_port.muxed_ep[mux_id], hlnode) {
			if (ep->mux_id == mux_id) {
				eth_ep = ep;
				break;
			}
		}

		if (eth_ep) {
			int rc;

			rc = rmnet_ingress_eth_handler(skb, eth_ep);
			/* Module framework returns 0 is function hook doesn't exist */
			if (rc == 0)
				goto free_skb;

			/* ETH Handler handles the freeing of SKB when done */
			return;
		} else {
			goto free_skb;
		}
	}

	pad = qmap->pad_len;
	len = ntohs(qmap->pkt_len) - pad;
	skb->dev = ep->egress_dev;

	/* Handle QMAPv5 packet */
	if (qmap->next_hdr &&
	    (port->data_format & (RMNET_FLAGS_INGRESS_COALESCE |
				  RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5))) {
		if (rmnet_map_process_next_hdr_packet(skb, &list, len))
			goto free_skb;
	} else {
		/* We only have the main QMAP header to worry about */
		pskb_pull(skb, sizeof(*qmap));

		rmnet_set_skb_proto(skb);

		if (port->data_format & RMNET_FLAGS_INGRESS_MAP_CKSUMV4) {
			if (!rmnet_map_checksum_downlink_packet(skb, len + pad))
				skb->ip_summed = CHECKSUM_UNNECESSARY;
		}

		pskb_trim(skb, len);

		/* Push the single packet onto the list */
		__skb_queue_tail(&list, skb);
	}

	if (port->data_format & RMNET_INGRESS_FORMAT_PS)
		qmi_rmnet_work_maybe_restart(port);

	rmnet_deliver_skb_list(&list, port);
	return;

free_skb:
	kfree_skb(skb);
}

static void rmnet_map_ipsec_record_error_type(struct sk_buff *skb,
					      struct rmnet_priv *priv)
{
	switch(rmnet_map_get_error_type(skb)) {
	case RMNET_MAP_ERROR_TYPE_NOT_SPEC:
		priv->stats.et_not_spec++;
		break;
	case RMNET_MAP_ERROR_TYPE_IPSEC_ENCAP:
		priv->stats.et_ipsec_encap++;
		break;
	case RMNET_MAP_ERROR_TYPE_IPSEC_DECAP:
		priv->stats.et_ipsec_decap++;
		break;
	default:
		priv->stats.et_invalid++;
		break;
	}
}

static void rmnet_map_ipsec_record_error_code(struct sk_buff *skb,
					      struct rmnet_priv *priv)
{
	switch(rmnet_map_get_error_code(skb)) {
	case RMNET_MAP_ERROR_CODE_NO_ERR:
		priv->stats.ec_no_err++;
		break;
	case RMNET_MAP_ERROR_CODE_DUP_SEQ:
		priv->stats.ec_dup_seq++;
		break;
	case RMNET_MAP_ERROR_CODE_OUT_OF_WIN:
		priv->stats.ec_out_of_win++;
		break;
	case RMNET_MAP_ERROR_CODE_AUTH_ERR:
		priv->stats.ec_auth_err++;
		break;
	case RMNET_MAP_ERROR_CODE_INC_PAD:
		priv->stats.ec_inc_pad++;
		break;
	case RMNET_MAP_ERROR_CODE_INC_ESP:
		priv->stats.ec_inc_esp++;
		break;
	case RMNET_MAP_ERROR_CODE_ECN_ERR:
		priv->stats.ec_ecn_err++;
		break;
	case RMNET_MAP_ERROR_CODE_POST_DECAP_NAT:
		priv->stats.ec_post_decap_nat++;
		break;
	case RMNET_MAP_ERROR_CODE_POST_DECAP_INNER_PKT:
		priv->stats.ec_post_decap_inner_pkt++;
		break;
	case RMNET_MAP_ERROR_CODE_POST_DECAP_INNER_FLTR_PKT:
		priv->stats.ec_post_decap_inner_flter_pkt++;
		break;
	case RMNET_MAP_ERROR_CODE_DECAP_SA_DISABLE:
		priv->stats.ec_decap_sa_disable++;
		break;
	case RMNET_MAP_ERROR_CODE_SW_HANDLE:
		priv->stats.ec_sw_handle++;
		break;
	case RMNET_MAP_ERROR_CODE_IN_PKT_VALIDATION:
		priv->stats.ec_in_pkt_validation++;
		break;
	case RMNET_MAP_ERROR_CODE_INPUT_PKT_SA_MISMATCH:
		priv->stats.ec_input_pkt_sa_mismatch++;
		break;
	case RMNET_MAP_ERROR_CODE_FRAG:
		priv->stats.ec_frag++;
		break;
	case RMNET_MAP_ERROR_CODE_DISCARD_RULE:
		priv->stats.ec_discard_rule++;
		break;
	case RMNET_MAP_ERROR_CODE_ENCAP_SA_DISABLE:
		priv->stats.ec_encap_sa_disable++;
		break;
	case RMNET_MAP_ERROR_CODE_SEQ_NUM_OVERFLOW:
		priv->stats.ec_code_seq_num_overflow++;
		break;
	case RMNET_MAP_ERROR_CODE_NEW_HW_DECAP:
		priv->stats.ec_new_hw_decap++;
		break;
	case RMNET_MAP_ERROR_CODE_NEW_HW_ENCAP_EXCEED_MTU:
		priv->stats.ec_new_hw_encap_exceed_mtu++;
		break;
	default:
		priv->stats.ec_invalid++;
		break;
	}
}

static void rmnet_map_ipsec_ingress_handler(struct sk_buff *skb,
					    struct rmnet_port *port)
{
	struct rmnet_map_header *qmap;
	struct rmnet_endpoint *ep;
	struct rmnet_priv *priv;
	u16 len, pad;
	u8 mux_id;

	qmap = (struct rmnet_map_header *)rmnet_map_data_ptr(skb);
	if (qmap->cd_bit) {
		port->stats.dl_ipsec_invalid_cmd++;
		goto free_skb;
	}

	mux_id = qmap->mux_id;

	if (mux_id >= RMNET_MAX_LOGICAL_EP) {
		port->stats.dl_ipsec_invalid_mux++;
		goto free_skb;
	}

	ep = rmnet_get_endpoint(port, mux_id);
	if (!ep) {
		port->stats.dl_ipsec_invalid_endpoint++;
		goto free_skb;
	}

	pad = qmap->pad_len;
	len = ntohs(qmap->pkt_len) - pad;
	skb->dev = ep->egress_dev;
	priv = netdev_priv(skb->dev);

	if (skb_get_rx_queue(skb) == IPA_RMNET_RX_QUEUE_IPSEC) {
		if (qmap->next_hdr &&
		    (port->data_format & RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5)) {
			if (rmnet_map_get_next_hdr_type(skb) !=
			    RMNET_MAP_HEADER_TYPE_CSUM_OFFLOAD) {
				priv->stats.dl1_hdr_type_err++;
				goto free_skb;
			}

			if (unlikely(!(skb->dev->features & NETIF_F_RXCSUM))) {
				priv->stats.csum_sw++;
			} else if (rmnet_map_get_csum_valid(skb)) {
				priv->stats.csum_ok++;
				skb->ip_summed = CHECKSUM_UNNECESSARY;
			} else {
				priv->stats.csum_validation_failed++;
			}

			pskb_pull(skb, (sizeof(struct rmnet_map_header) +
					sizeof(struct rmnet_map_v5_csum_header)));
		} else {
			pskb_pull(skb, sizeof(struct rmnet_map_header));
		}

		priv->stats.dl1_ok++;
	} else {
		if (qmap->next_hdr &&
		    (rmnet_map_get_next_hdr_type(skb) != RMNET_MAP_HEADER_TYPE_ERROR)) {
			priv->stats.dl2_hdr_type_err++;
			goto free_skb;
		}

		rmnet_map_ipsec_record_error_type(skb, priv);
		rmnet_map_ipsec_record_error_code(skb, priv);

		pskb_pull(skb, (sizeof(struct rmnet_map_header) +
				sizeof(struct rmnet_map_v5_error_header)));

		priv->stats.dl2_ok++;
	}

	rmnet_set_skb_proto(skb);
	pskb_trim(skb, len);

	rmnet_deliver_skb(skb, port);
	return;

free_skb:
	kfree_skb(skb);
}

static void rmnet_ipsec_ingress_handler(struct sk_buff *skb,
					struct rmnet_port *port)
{
	while (skb) {
		struct sk_buff *skb_frag = skb_shinfo(skb)->frag_list;

		skb_shinfo(skb)->frag_list = NULL;
		port->stats.dl_ipsec++;
		rmnet_map_ipsec_ingress_handler(skb, port);

		skb = skb_frag;
	}
}

int (*rmnet_perf_deag_entry)(struct sk_buff *skb,
			     struct rmnet_port *port) __rcu __read_mostly;
EXPORT_SYMBOL(rmnet_perf_deag_entry);

static void
rmnet_map_ingress_handler(struct sk_buff *skb,
			  struct rmnet_port *port)
{
	struct sk_buff *skbn;
	int (*rmnet_perf_core_deaggregate)(struct sk_buff *skb,
					   struct rmnet_port *port);

	if (skb->dev->type == ARPHRD_ETHER) {
		if (pskb_expand_head(skb, ETH_HLEN, 0, GFP_ATOMIC)) {
			kfree_skb(skb);
			return;
		}

		skb_push(skb, ETH_HLEN);
	}

	if ((port->data_format & RMNET_INGRESS_FORMAT_IP_ROUTE) &&
	    (skb_get_rx_queue(skb) == port->ip_route_params.rx_queue)) {
		rmnet_ip_route_rcv(skb, port);
		return;
	}

	if ((skb->dev->features & NETIF_F_HW_ESP) &&
	    (skb->dev->hw_enc_features & NETIF_F_HW_ESP) &&
	    (skb_rx_queue_recorded(skb) &&
	     ((skb_get_rx_queue(skb) == IPA_RMNET_RX_QUEUE_IPSEC) ||
	      (skb_get_rx_queue(skb) == IPA_RMNET_RX_QUEUE_IPSEC_ERROR)))) {
		rmnet_ipsec_ingress_handler(skb, port);
		return;
	}

	if (port->data_format & (RMNET_FLAGS_INGRESS_COALESCE |
				 RMNET_PRIV_FLAGS_INGRESS_MAP_CKSUMV5)) {
		if (skb_is_nonlinear(skb)) {
			rmnet_frag_ingress_handler(skb, port);
			return;
		}
	}

	/* No aggregation. Pass the frame on as is */
	if (!(port->data_format & RMNET_FLAGS_INGRESS_DEAGGREGATION)) {
		__rmnet_map_ingress_handler(skb, port);
		return;
	}

	if (skb->priority == 0xda1a)
		goto no_perf;

	/* Pass off handling to rmnet_perf module, if present */
	rcu_read_lock();
	rmnet_perf_core_deaggregate = rcu_dereference(rmnet_perf_deag_entry);
	if (rmnet_perf_core_deaggregate) {
		rmnet_perf_core_deaggregate(skb, port);
		rcu_read_unlock();
		return;
	}
	rcu_read_unlock();

no_perf:
	/* Deaggregation and freeing of HW originating
	 * buffers is done within here
	 */
	while (skb) {
		struct sk_buff *skb_frag = skb_shinfo(skb)->frag_list;

		skb_shinfo(skb)->frag_list = NULL;
		while ((skbn = rmnet_map_deaggregate(skb, port)) != NULL) {
			__rmnet_map_ingress_handler(skbn, port);

			if (skbn == skb)
				goto next_skb;
		}

		consume_skb(skb);
next_skb:
		skb = skb_frag;
	}
}

static int rmnet_map_egress_handler(struct sk_buff *skb,
				    struct rmnet_port *port, u8 mux_id,
				    struct net_device *orig_dev,
				    bool low_latency, u8 ipsec)
{
	int required_headroom, additional_header_len, csum_type, tso = 0;
	struct rmnet_map_header *map_header;
	struct rmnet_aggregation_state *state;

	additional_header_len = 0;
	required_headroom = sizeof(struct rmnet_map_header);
	csum_type = 0;

	if (ipsec) {
		/* always use MAPv1 format */
	} else if (port->data_format & RMNET_FLAGS_EGRESS_MAP_CKSUMV4) {
		additional_header_len = sizeof(struct rmnet_map_ul_csum_header);
		csum_type = RMNET_FLAGS_EGRESS_MAP_CKSUMV4;
	} else if ((port->data_format & RMNET_PRIV_FLAGS_EGRESS_MAP_CKSUMV5) ||
		   (port->data_format & RMNET_EGRESS_FORMAT_PRIORITY)) {
		additional_header_len = sizeof(struct rmnet_map_v5_csum_header);
		csum_type = RMNET_PRIV_FLAGS_EGRESS_MAP_CKSUMV5;
	}

	required_headroom += additional_header_len;

	if (skb_headroom(skb) < required_headroom) {
		if (pskb_expand_head(skb, required_headroom, 0, GFP_ATOMIC))
			return -ENOMEM;
	}

	if (port->data_format & RMNET_INGRESS_FORMAT_PS)
		qmi_rmnet_work_maybe_restart(port);

	state = &port->agg_state[(low_latency) ? RMNET_LL_AGG_STATE :
				 RMNET_DEFAULT_AGG_STATE];

	if (csum_type &&
	    (skb_shinfo(skb)->gso_type & (SKB_GSO_UDP_L4 | SKB_GSO_TCPV4 | SKB_GSO_TCPV6)) &&
	     skb_shinfo(skb)->gso_size) {
		spin_lock_bh(&state->agg_lock);
		rmnet_map_send_agg_skb(state);

		if (rmnet_map_add_tso_header(skb, port, orig_dev))
			return -EINVAL;
		csum_type = 0;
		tso = 1;
	}

	if (csum_type)
		rmnet_map_checksum_uplink_packet(skb, port, orig_dev,
						 csum_type);

	map_header = rmnet_map_add_map_header(skb, additional_header_len, 0,
					      port);
	if (!map_header)
		return -ENOMEM;

	if (ipsec)
		map_header->next_hdr = 0;

	map_header->mux_id = mux_id;

	if (port->data_format & RMNET_EGRESS_FORMAT_AGGREGATION) {
		if (state->params.agg_count < 2 ||
		    rmnet_map_tx_agg_skip(skb, required_headroom) || tso || ipsec)
			goto done;

		rmnet_map_tx_aggregate(skb, port, low_latency);
		return -EINPROGRESS;
	}

done:
	skb->protocol = htons(ETH_P_MAP);
	return 0;
}

static void
rmnet_bridge_handler(struct sk_buff *skb, struct net_device *bridge_dev)
{
	if (bridge_dev) {
		skb->dev = bridge_dev;
		dev_queue_xmit(skb);
	}
}

/* Ingress / Egress Entry Points */

/* Processes packet as per ingress data format for receiving device. Logical
 * endpoint is determined from packet inspection. Packet is then sent to the
 * egress device listed in the logical endpoint configuration.
 */
rx_handler_result_t rmnet_rx_handler(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct rmnet_port *port;
	struct net_device *dev;
	struct rmnet_skb_cb *cb;
	int (*rmnet_core_shs_switch)(struct sk_buff *skb,
				     struct rmnet_shs_clnt_s *cfg);

	if (!skb)
		goto done;

	if (skb->pkt_type == PACKET_LOOPBACK)
		return RX_HANDLER_PASS;

	trace_rmnet_low(RMNET_MODULE, RMNET_RCV_FROM_PND, 0xDEF,
			0xDEF, 0xDEF, 0xDEF, NULL, NULL);
	dev = skb->dev;
	port = rmnet_get_port(dev);
	if (unlikely(!port)) {
		atomic_long_inc(&skb->dev->rx_nohandler);
		kfree_skb(skb);
		goto done;
	}

	switch (port->rmnet_mode) {
	case RMNET_EPMODE_VND:

		rcu_read_lock();
		rmnet_core_shs_switch = rcu_dereference(rmnet_shs_switch);
		cb = RMNET_SKB_CB(skb);
		if (rmnet_core_shs_switch && !cb->qmap_steer &&
		    skb->priority != 0xda1a) {
			cb->qmap_steer = 1;
			rmnet_core_shs_switch(skb, &port->phy_shs_cfg);
			rcu_read_unlock();
			return RX_HANDLER_CONSUMED;
		}
		rcu_read_unlock();

		rmnet_map_ingress_handler(skb, port);
		break;
	case RMNET_EPMODE_BRIDGE:
		rmnet_bridge_handler(skb, port->bridge_ep);
		break;
	}

done:
	return RX_HANDLER_CONSUMED;
}
EXPORT_SYMBOL(rmnet_rx_handler);

/* Modifies packet as per logical endpoint configuration and egress data format
 * for egress device configured in logical endpoint. Packet is then transmitted
 * on the egress device.
 */
void rmnet_egress_handler(struct sk_buff *skb, bool low_latency, u8 ipsec)
{
	struct net_device *orig_dev;
	struct rmnet_port *port;
	struct rmnet_priv *priv;
	u8 mux_id;
	int err;
	u32 skb_len;

	trace_rmnet_low(RMNET_MODULE, RMNET_TX_UL_PKT, 0xDEF, 0xDEF, 0xDEF,
			0xDEF, (void *)skb, NULL);
	sk_pacing_shift_update(skb->sk, 8);

	orig_dev = skb->dev;
	priv = netdev_priv(orig_dev);
	skb->dev = priv->real_dev;
	mux_id = priv->mux_id;

	port = rmnet_get_port(skb->dev);
	trace_rmnet_skb_egress_entry(skb);
	if (!port)
		goto drop;

	skb_len = skb->len;
	if (port->data_format & RMNET_EGRESS_FORMAT_IP_ROUTE &&
	    priv->route_mode == RMNET_ROUTE_MODE_IP) {
		skb_set_queue_mapping(skb, port->ip_route_params.tx_queue);
		priv->stats.ip_route_tx_pkts++;
		goto direct_xmit;
	}
	err = rmnet_map_egress_handler(skb, port, mux_id, orig_dev,
				       low_latency, ipsec);
	if (err == -ENOMEM || err == -EINVAL) {
		goto drop;
	} else if (err == -EINPROGRESS) {
		rmnet_vnd_tx_fixup(orig_dev, skb_len);
		return;
	}

direct_xmit:
	trace_rmnet_skb_egress_exit(skb);
	rmnet_vnd_tx_fixup(orig_dev, skb_len);

	if (low_latency && !ipsec) {
		if (rmnet_ll_send_skb(skb)) {
			/* Drop but no need to free. Above API handles that */
			this_cpu_inc(priv->pcpu_stats->stats.tx_drops);
		}
		return;
	}

	if (ipsec == XFRM_DEV_OFFLOAD_OUT) {
		skb_set_queue_mapping(skb, IPA_RMNET_TX_QUEUE_IPSEC_ENCAP);
		priv->stats.ul1_ok++;
	} else if (ipsec == XFRM_DEV_OFFLOAD_IN) {
		skb_set_queue_mapping(skb, IPA_RMNET_TX_QUEUE_IPSEC_DECAP);
		priv->stats.ul2_ok++;
	}

	dev_queue_xmit(skb);

	return;

drop:
	this_cpu_inc(priv->pcpu_stats->stats.tx_drops);
	kfree_skb(skb);
}
