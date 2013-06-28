/*
   BlueZ - Bluetooth protocol stack for Linux
   Copyright (C) 2000-2001 Qualcomm Incorporated

   Written 2000,2001 by Maxim Krasnyansky <maxk@qualcomm.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 2 as
   published by the Free Software Foundation;

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
   OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
   IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) AND AUTHOR(S) BE LIABLE FOR ANY
   CLAIM, OR ANY SPECIAL INDIRECT OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES
   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
   ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
   OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

   ALL LIABILITY, INCLUDING LIABILITY FOR INFRINGEMENT OF ANY PATENTS,
   COPYRIGHTS, TRADEMARKS OR OTHER RIGHTS, RELATING TO USE OF THIS
   SOFTWARE IS DISCLAIMED.
*/

/* Bluetooth HCI sockets. */

#include <asm/unaligned.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>
#include <net/bluetooth/hci_mon.h>

static atomic_t monitor_promisc = ATOMIC_INIT(0);

/* ----- HCI socket interface ----- */

static inline int hci_test_bit(int nr, void *addr)
{
	return *((__u32 *) addr + (nr >> 5)) & ((__u32) 1 << (nr & 31));
}

static void skb_clone_fraglist(struct sk_buff *skb)
{
	struct sk_buff *list;
    
	skb_walk_frags(skb, list)
    skb_get(list);
}

static void __copy_skb_header(struct sk_buff *new, const struct sk_buff *old)
{
	new->tstamp		= old->tstamp;
	new->dev		= old->dev;
	new->transport_header	= old->transport_header;
	new->network_header	= old->network_header;
	new->mac_header		= old->mac_header;
	new->inner_transport_header = old->inner_transport_header;
	new->inner_network_header = old->inner_network_header;
	new->inner_mac_header = old->inner_mac_header;
	skb_dst_copy(new, old);
	new->rxhash		= old->rxhash;
	new->ooo_okay		= old->ooo_okay;
	new->l4_rxhash		= old->l4_rxhash;
	new->no_fcs		= old->no_fcs;
	new->encapsulation	= old->encapsulation;
#ifdef CONFIG_XFRM
	new->sp			= secpath_get(old->sp);
#endif
	memcpy(new->cb, old->cb, sizeof(old->cb));
	new->csum		= old->csum;
	new->local_df		= old->local_df;
	new->pkt_type		= old->pkt_type;
	new->ip_summed		= old->ip_summed;
	skb_copy_queue_mapping(new, old);
	new->priority		= old->priority;
#if IS_ENABLED(CONFIG_IP_VS)
	new->ipvs_property	= old->ipvs_property;
#endif
	new->pfmemalloc		= old->pfmemalloc;
	new->protocol		= old->protocol;
	new->mark		= old->mark;
	new->skb_iif		= old->skb_iif;
	__nf_copy(new, old);
#if IS_ENABLED(CONFIG_NETFILTER_XT_TARGET_TRACE)
	new->nf_trace		= old->nf_trace;
#endif
#ifdef CONFIG_NET_SCHED
	new->tc_index		= old->tc_index;
#ifdef CONFIG_NET_CLS_ACT
	new->tc_verd		= old->tc_verd;
#endif
#endif
	new->vlan_proto		= old->vlan_proto;
	new->vlan_tci		= old->vlan_tci;
    
	skb_copy_secmark(new, old);
}


static void copy_skb_header(struct sk_buff *new, const struct sk_buff *old)
{
#ifndef NET_SKBUFF_DATA_USES_OFFSET
	/*
	 *	Shift between the two data areas in bytes
	 */
	unsigned long offset = new->data - old->data;
#endif
    
	__copy_skb_header(new, old);
    
#ifndef NET_SKBUFF_DATA_USES_OFFSET
	skb_headers_offset_update(new, offset);
#endif
	skb_shinfo(new)->gso_size = skb_shinfo(old)->gso_size;
	skb_shinfo(new)->gso_segs = skb_shinfo(old)->gso_segs;
	skb_shinfo(new)->gso_type = skb_shinfo(old)->gso_type;
}

/**
 * __skb_frag_ref - take an addition reference on a paged fragment.
 * @frag: the paged fragment
 *
 * Takes an additional reference on the paged fragment @frag.
 */
static inline void __skb_frag_ref(skb_frag_t *frag)
{
	get_page(skb_frag_page(frag));
}

/**
 * skb_frag_ref - take an addition reference on a paged fragment of an skb.
 * @skb: the buffer
 * @f: the fragment offset.
 *
 * Takes an additional reference on the @f'th paged fragment of @skb.
 */
static inline void skb_frag_ref(struct sk_buff *skb, int f)
{
	__skb_frag_ref(&skb_shinfo(skb)->frags[f]);
}


/**
 *	skb_orphan_frags - orphan the frags contained in a buffer
 *	@skb: buffer to orphan frags from
 *	@gfp_mask: allocation mask for replacement pages
 *
 *	For each frag in the SKB which needs a destructor (i.e. has an
 *	owner) create a copy of that frag and release the original
 *	page by calling the destructor.
 */
static inline int skb_orphan_frags(struct sk_buff *skb, gfp_t gfp_mask)
{
	if (likely(!(skb_shinfo(skb)->tx_flags & SKBTX_DEV_ZEROCOPY)))
		return 0;
	return skb_copy_ubufs(skb, gfp_mask);
}

static inline int skb_alloc_rx_flag(const struct sk_buff *skb)
{
	if (skb_pfmemalloc(skb))
		return SKB_ALLOC_RX;
	return 0;
}

/**
 *	__pskb_copy	-	create copy of an sk_buff with private head.
 *	@skb: buffer to copy
 *	@headroom: headroom of new skb
 *	@gfp_mask: allocation priority
 *
 *	Make a copy of both an &sk_buff and part of its data, located
 *	in header. Fragmented data remain shared. This is used when
 *	the caller wishes to modify only header of &sk_buff and needs
 *	private copy of the header to alter. Returns %NULL on failure
 *	or the pointer to the buffer on success.
 *	The returned buffer has a reference count of 1.
 */

static struct sk_buff *__pskb_copy(struct sk_buff *skb, int headroom, gfp_t gfp_mask)
{
	unsigned int size = skb_headlen(skb) + headroom;
	struct sk_buff *n = __alloc_skb(size, gfp_mask,
                                    skb_alloc_rx_flag(skb), NUMA_NO_NODE);
    
	if (!n)
		goto out;
    
	/* Set the data pointer */
	skb_reserve(n, headroom);
	/* Set the tail pointer and length */
	skb_put(n, skb_headlen(skb));
	/* Copy the bytes */
	skb_copy_from_linear_data(skb, n->data, n->len);
    
	n->truesize += skb->data_len;
	n->data_len  = skb->data_len;
	n->len	     = skb->len;
    
	if (skb_shinfo(skb)->nr_frags) {
		int i;
        
		if (skb_orphan_frags(skb, gfp_mask)) {
			kfree_skb(n);
			n = NULL;
			goto out;
		}
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			skb_shinfo(n)->frags[i] = skb_shinfo(skb)->frags[i];
			skb_frag_ref(skb, i);
		}
		skb_shinfo(n)->nr_frags = i;
	}
    
	if (skb_has_frag_list(skb)) {
		skb_shinfo(n)->frag_list = skb_shinfo(skb)->frag_list;
		skb_clone_fraglist(n);
	}
    
	copy_skb_header(n, skb);
out:
	return n;
}


/* Security filter */
static struct hci_sec_filter hci_sec_filter = {
	/* Packet types */
	0x10,
	/* Events */
	{ 0x1000d9fe, 0x0000b00c },
	/* Commands */
	{
		{ 0x0 },
		/* OGF_LINK_CTL */
		{ 0xbe000006, 0x00000001, 0x00000000, 0x00 },
		/* OGF_LINK_POLICY */
		{ 0x00005200, 0x00000000, 0x00000000, 0x00 },
		/* OGF_HOST_CTL */
		{ 0xaab00200, 0x2b402aaa, 0x05220154, 0x00 },
		/* OGF_INFO_PARAM */
		{ 0x000002be, 0x00000000, 0x00000000, 0x00 },
		/* OGF_STATUS_PARAM */
		{ 0x000000ea, 0x00000000, 0x00000000, 0x00 }
	}
};

static struct bt_sock_list hci_sk_list = {
	.lock = __RW_LOCK_UNLOCKED(hci_sk_list.lock)
};

/* Send frame to RAW socket */
void hci_send_to_sock(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct sock *sk;
    struct hlist_node *node;
	struct sk_buff *skb_copy = NULL;

	BT_DBG("hdev %p len %d", hdev, skb->len);

	read_lock(&hci_sk_list.lock);

	sk_for_each(sk, node, &hci_sk_list.head) {
		struct hci_filter *flt;
		struct sk_buff *nskb;

		if (sk->sk_state != BT_BOUND || hci_pi(sk)->hdev != hdev)
			continue;

		/* Don't send frame to the socket it came from */
		if (skb->sk == sk)
			continue;

		if (hci_pi(sk)->channel != HCI_CHANNEL_RAW)
			continue;

		/* Apply filter */
		flt = &hci_pi(sk)->filter;

		if (!test_bit((bt_cb(skb)->pkt_type == HCI_VENDOR_PKT) ?
			      0 : (bt_cb(skb)->pkt_type & HCI_FLT_TYPE_BITS),
			      &flt->type_mask))
			continue;

		if (bt_cb(skb)->pkt_type == HCI_EVENT_PKT) {
			int evt = (*(__u8 *)skb->data & HCI_FLT_EVENT_BITS);

			if (!hci_test_bit(evt, &flt->event_mask))
				continue;

			if (flt->opcode &&
			    ((evt == HCI_EV_CMD_COMPLETE &&
			      flt->opcode !=
			      get_unaligned((__le16 *)(skb->data + 3))) ||
			     (evt == HCI_EV_CMD_STATUS &&
			      flt->opcode !=
			      get_unaligned((__le16 *)(skb->data + 4)))))
				continue;
		}

		if (!skb_copy) {
			/* Create a private copy with headroom */
			skb_copy = __pskb_copy(skb, 1, GFP_ATOMIC);
			if (!skb_copy)
				continue;

			/* Put type byte before the data */
			memcpy(skb_push(skb_copy, 1), &bt_cb(skb)->pkt_type, 1);
		}

		nskb = skb_clone(skb_copy, GFP_ATOMIC);
		if (!nskb)
			continue;

		if (sock_queue_rcv_skb(sk, nskb))
			kfree_skb(nskb);
	}

	read_unlock(&hci_sk_list.lock);

	kfree_skb(skb_copy);
}

/* Send frame to control socket */
void hci_send_to_control(struct sk_buff *skb, struct sock *skip_sk)
{
	struct sock *sk;
    struct hlist_node *node;

	BT_DBG("len %d", skb->len);

	read_lock(&hci_sk_list.lock);

	sk_for_each(sk, node, &hci_sk_list.head) {
		struct sk_buff *nskb;

		/* Skip the original socket */
		if (sk == skip_sk)
			continue;

		if (sk->sk_state != BT_BOUND)
			continue;

		if (hci_pi(sk)->channel != HCI_CHANNEL_CONTROL)
			continue;

		nskb = skb_clone(skb, GFP_ATOMIC);
		if (!nskb)
			continue;

		if (sock_queue_rcv_skb(sk, nskb))
			kfree_skb(nskb);
	}

	read_unlock(&hci_sk_list.lock);
}

/* Send frame to monitor socket */
void hci_send_to_monitor(struct hci_dev *hdev, struct sk_buff *skb)
{
	struct sock *sk;
    struct hlist_node *node;
	struct sk_buff *skb_copy = NULL;
	__le16 opcode;

	if (!atomic_read(&monitor_promisc))
		return;

	BT_DBG("hdev %p len %d", hdev, skb->len);

	switch (bt_cb(skb)->pkt_type) {
	case HCI_COMMAND_PKT:
		opcode = __constant_cpu_to_le16(HCI_MON_COMMAND_PKT);
		break;
	case HCI_EVENT_PKT:
		opcode = __constant_cpu_to_le16(HCI_MON_EVENT_PKT);
		break;
	case HCI_ACLDATA_PKT:
		if (bt_cb(skb)->incoming)
			opcode = __constant_cpu_to_le16(HCI_MON_ACL_RX_PKT);
		else
			opcode = __constant_cpu_to_le16(HCI_MON_ACL_TX_PKT);
		break;
	case HCI_SCODATA_PKT:
		if (bt_cb(skb)->incoming)
			opcode = __constant_cpu_to_le16(HCI_MON_SCO_RX_PKT);
		else
			opcode = __constant_cpu_to_le16(HCI_MON_SCO_TX_PKT);
		break;
	default:
		return;
	}

	read_lock(&hci_sk_list.lock);

	sk_for_each(sk, node, &hci_sk_list.head) {
		struct sk_buff *nskb;

		if (sk->sk_state != BT_BOUND)
			continue;

		if (hci_pi(sk)->channel != HCI_CHANNEL_MONITOR)
			continue;

		if (!skb_copy) {
			struct hci_mon_hdr *hdr;

			/* Create a private copy with headroom */
			skb_copy = __pskb_copy(skb, HCI_MON_HDR_SIZE,
					       GFP_ATOMIC);
			if (!skb_copy)
				continue;

			/* Put header before the data */
			hdr = (void *) skb_push(skb_copy, HCI_MON_HDR_SIZE);
			hdr->opcode = opcode;
			hdr->index = cpu_to_le16(hdev->id);
			hdr->len = cpu_to_le16(skb->len);
		}

		nskb = skb_clone(skb_copy, GFP_ATOMIC);
		if (!nskb)
			continue;

		if (sock_queue_rcv_skb(sk, nskb))
			kfree_skb(nskb);
	}

	read_unlock(&hci_sk_list.lock);

	kfree_skb(skb_copy);
}

static void send_monitor_event(struct sk_buff *skb)
{
	struct sock *sk;
    struct hlist_node *node;

	BT_DBG("len %d", skb->len);

	read_lock(&hci_sk_list.lock);

	sk_for_each(sk, node, &hci_sk_list.head) {
		struct sk_buff *nskb;

		if (sk->sk_state != BT_BOUND)
			continue;

		if (hci_pi(sk)->channel != HCI_CHANNEL_MONITOR)
			continue;

		nskb = skb_clone(skb, GFP_ATOMIC);
		if (!nskb)
			continue;

		if (sock_queue_rcv_skb(sk, nskb))
			kfree_skb(nskb);
	}

	read_unlock(&hci_sk_list.lock);
}

static struct sk_buff *create_monitor_event(struct hci_dev *hdev, int event)
{
	struct hci_mon_hdr *hdr;
	struct hci_mon_new_index *ni;
	struct sk_buff *skb;
	__le16 opcode;

	switch (event) {
	case HCI_DEV_REG:
		skb = bt_skb_alloc(HCI_MON_NEW_INDEX_SIZE, GFP_ATOMIC);
		if (!skb)
			return NULL;

		ni = (void *) skb_put(skb, HCI_MON_NEW_INDEX_SIZE);
		ni->type = hdev->dev_type;
		ni->bus = hdev->bus;
		bacpy(&ni->bdaddr, &hdev->bdaddr);
		memcpy(ni->name, hdev->name, 8);

		opcode = __constant_cpu_to_le16(HCI_MON_NEW_INDEX);
		break;

	case HCI_DEV_UNREG:
		skb = bt_skb_alloc(0, GFP_ATOMIC);
		if (!skb)
			return NULL;

		opcode = __constant_cpu_to_le16(HCI_MON_DEL_INDEX);
		break;

	default:
		return NULL;
	}

	__net_timestamp(skb);

	hdr = (void *) skb_push(skb, HCI_MON_HDR_SIZE);
	hdr->opcode = opcode;
	hdr->index = cpu_to_le16(hdev->id);
	hdr->len = cpu_to_le16(skb->len - HCI_MON_HDR_SIZE);

	return skb;
}

static void send_monitor_replay(struct sock *sk)
{
	struct hci_dev *hdev;

	read_lock(&hci_dev_list_lock);

	list_for_each_entry(hdev, &hci_dev_list, list) {
		struct sk_buff *skb;

		skb = create_monitor_event(hdev, HCI_DEV_REG);
		if (!skb)
			continue;

		if (sock_queue_rcv_skb(sk, skb))
			kfree_skb(skb);
	}

	read_unlock(&hci_dev_list_lock);
}

/* Generate internal stack event */
static void hci_si_event(struct hci_dev *hdev, int type, int dlen, void *data)
{
	struct hci_event_hdr *hdr;
	struct hci_ev_stack_internal *ev;
	struct sk_buff *skb;

	skb = bt_skb_alloc(HCI_EVENT_HDR_SIZE + sizeof(*ev) + dlen, GFP_ATOMIC);
	if (!skb)
		return;

	hdr = (void *) skb_put(skb, HCI_EVENT_HDR_SIZE);
	hdr->evt  = HCI_EV_STACK_INTERNAL;
	hdr->plen = sizeof(*ev) + dlen;

	ev  = (void *) skb_put(skb, sizeof(*ev) + dlen);
	ev->type = type;
	memcpy(ev->data, data, dlen);

	bt_cb(skb)->incoming = 1;
	__net_timestamp(skb);

	bt_cb(skb)->pkt_type = HCI_EVENT_PKT;
	skb->dev = (void *) hdev;
	hci_send_to_sock(hdev, skb);
	kfree_skb(skb);
}

void hci_sock_dev_event(struct hci_dev *hdev, int event)
{
	struct hci_ev_si_device ev;
    
	BT_DBG("hdev %s event %d", hdev->name, event);

	/* Send event to monitor */
	if (atomic_read(&monitor_promisc)) {
		struct sk_buff *skb;

		skb = create_monitor_event(hdev, event);
		if (skb) {
			send_monitor_event(skb);
			kfree_skb(skb);
		}
	}

	/* Send event to sockets */
	ev.event  = event;
	ev.dev_id = hdev->id;
	hci_si_event(NULL, HCI_EV_SI_DEVICE, sizeof(ev), &ev);

	if (event == HCI_DEV_UNREG) {
		struct sock *sk;
        struct hlist_node *node;

		/* Detach sockets from device */
		read_lock(&hci_sk_list.lock);
		sk_for_each(sk, node, &hci_sk_list.head) {
			bh_lock_sock_nested(sk);
			if (hci_pi(sk)->hdev == hdev) {
				hci_pi(sk)->hdev = NULL;
				sk->sk_err = EPIPE;
				sk->sk_state = BT_OPEN;
				sk->sk_state_change(sk);

				hci_dev_put(hdev);
			}
			bh_unlock_sock(sk);
		}
		read_unlock(&hci_sk_list.lock);
	}
}

static int hci_sock_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct hci_dev *hdev;

	BT_DBG("sock %p sk %p", sock, sk);

	if (!sk)
		return 0;

	hdev = hci_pi(sk)->hdev;

	if (hci_pi(sk)->channel == HCI_CHANNEL_MONITOR)
		atomic_dec(&monitor_promisc);

	bt_sock_unlink(&hci_sk_list, sk);

	if (hdev) {
		atomic_dec(&hdev->promisc);
		hci_dev_put(hdev);
	}

	sock_orphan(sk);

	skb_queue_purge(&sk->sk_receive_queue);
	skb_queue_purge(&sk->sk_write_queue);

	sock_put(sk);
	return 0;
}

static int hci_sock_blacklist_add(struct hci_dev *hdev, void __user *arg)
{
	bdaddr_t bdaddr;
	int err;

	if (copy_from_user(&bdaddr, arg, sizeof(bdaddr)))
		return -EFAULT;

	hci_dev_lock(hdev);

	err = hci_blacklist_add(hdev, &bdaddr, 0);

	hci_dev_unlock(hdev);

	return err;
}

static int hci_sock_blacklist_del(struct hci_dev *hdev, void __user *arg)
{
	bdaddr_t bdaddr;
	int err;

	if (copy_from_user(&bdaddr, arg, sizeof(bdaddr)))
		return -EFAULT;

	hci_dev_lock(hdev);

	err = hci_blacklist_del(hdev, &bdaddr, 0);

	hci_dev_unlock(hdev);

	return err;
}

/* Ioctls that require bound socket */
static int hci_sock_bound_ioctl(struct sock *sk, unsigned int cmd,
				unsigned long arg)
{
	struct hci_dev *hdev = hci_pi(sk)->hdev;

	if (!hdev)
		return -EBADFD;

	switch (cmd) {
	case HCISETRAW:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;

		if (test_bit(HCI_QUIRK_RAW_DEVICE, &hdev->quirks))
			return -EPERM;

		if (arg)
			set_bit(HCI_RAW, &hdev->flags);
		else
			clear_bit(HCI_RAW, &hdev->flags);

		return 0;

	case HCIGETCONNINFO:
		return hci_get_conn_info(hdev, (void __user *) arg);

	case HCIGETAUTHINFO:
		return hci_get_auth_info(hdev, (void __user *) arg);

	case HCIBLOCKADDR:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		return hci_sock_blacklist_add(hdev, (void __user *) arg);

	case HCIUNBLOCKADDR:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		return hci_sock_blacklist_del(hdev, (void __user *) arg);

	default:
		if (hdev->ioctl)
			return hdev->ioctl(hdev, cmd, arg);
		return -EINVAL;
	}
}

static int hci_sock_ioctl(struct socket *sock, unsigned int cmd,
			  unsigned long arg)
{
	struct sock *sk = sock->sk;
	void __user *argp = (void __user *) arg;
	int err;

	BT_DBG("cmd %x arg %lx", cmd, arg);

	switch (cmd) {
	case HCIGETDEVLIST:
		return hci_get_dev_list(argp);

	case HCIGETDEVINFO:
		return hci_get_dev_info(argp);

	case HCIGETCONNLIST:
		return hci_get_conn_list(argp);

	case HCIDEVUP:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		return hci_dev_open(arg);

	case HCIDEVDOWN:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		return hci_dev_close(arg);

	case HCIDEVRESET:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		return hci_dev_reset(arg);

	case HCIDEVRESTAT:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		return hci_dev_reset_stat(arg);

	case HCISETSCAN:
	case HCISETAUTH:
	case HCISETENCRYPT:
	case HCISETPTYPE:
	case HCISETLINKPOL:
	case HCISETLINKMODE:
	case HCISETACLMTU:
	case HCISETSCOMTU:
		if (!capable(CAP_NET_ADMIN))
			return -EPERM;
		return hci_dev_cmd(cmd, argp);

	case HCIINQUIRY:
		return hci_inquiry(argp);

	default:
		lock_sock(sk);
		err = hci_sock_bound_ioctl(sk, cmd, arg);
		release_sock(sk);
		return err;
	}
}

static int hci_sock_bind(struct socket *sock, struct sockaddr *addr,
			 int addr_len)
{
	struct sockaddr_hci haddr;
	struct sock *sk = sock->sk;
	struct hci_dev *hdev = NULL;
	int len, err = 0;

	BT_DBG("sock %p sk %p", sock, sk);

	if (!addr)
		return -EINVAL;

	memset(&haddr, 0, sizeof(haddr));
	len = min_t(unsigned int, sizeof(haddr), addr_len);
	memcpy(&haddr, addr, len);

	if (haddr.hci_family != AF_BLUETOOTH)
		return -EINVAL;

	lock_sock(sk);

	if (sk->sk_state == BT_BOUND) {
		err = -EALREADY;
		goto done;
	}

	switch (haddr.hci_channel) {
	case HCI_CHANNEL_RAW:
		if (hci_pi(sk)->hdev) {
			err = -EALREADY;
			goto done;
		}

		if (haddr.hci_dev != HCI_DEV_NONE) {
			hdev = hci_dev_get(haddr.hci_dev);
			if (!hdev) {
				err = -ENODEV;
				goto done;
			}

			atomic_inc(&hdev->promisc);
		}

		hci_pi(sk)->hdev = hdev;
		break;

	case HCI_CHANNEL_CONTROL:
		if (haddr.hci_dev != HCI_DEV_NONE) {
			err = -EINVAL;
			goto done;
		}

		if (!capable(CAP_NET_ADMIN)) {
			err = -EPERM;
			goto done;
		}

		break;

	case HCI_CHANNEL_MONITOR:
		if (haddr.hci_dev != HCI_DEV_NONE) {
			err = -EINVAL;
			goto done;
		}

		if (!capable(CAP_NET_RAW)) {
			err = -EPERM;
			goto done;
		}

		send_monitor_replay(sk);

		atomic_inc(&monitor_promisc);
		break;

	default:
		err = -EINVAL;
		goto done;
	}


	hci_pi(sk)->channel = haddr.hci_channel;
	sk->sk_state = BT_BOUND;

done:
	release_sock(sk);
	return err;
}

static int hci_sock_getname(struct socket *sock, struct sockaddr *addr,
			    int *addr_len, int peer)
{
	struct sockaddr_hci *haddr = (struct sockaddr_hci *) addr;
	struct sock *sk = sock->sk;
	struct hci_dev *hdev = hci_pi(sk)->hdev;

	BT_DBG("sock %p sk %p", sock, sk);

	if (!hdev)
		return -EBADFD;

	lock_sock(sk);

	*addr_len = sizeof(*haddr);
	haddr->hci_family = AF_BLUETOOTH;
	haddr->hci_dev    = hdev->id;
	haddr->hci_channel= 0;

	release_sock(sk);
	return 0;
}

static void hci_sock_cmsg(struct sock *sk, struct msghdr *msg,
			  struct sk_buff *skb)
{
	__u32 mask = hci_pi(sk)->cmsg_mask;

	if (mask & HCI_CMSG_DIR) {
		int incoming = bt_cb(skb)->incoming;
		put_cmsg(msg, SOL_HCI, HCI_CMSG_DIR, sizeof(incoming),
			 &incoming);
	}

	if (mask & HCI_CMSG_TSTAMP) {
#ifdef CONFIG_COMPAT
		struct compat_timeval ctv;
#endif
		struct timeval tv;
		void *data;
		int len;

		skb_get_timestamp(skb, &tv);

		data = &tv;
		len = sizeof(tv);
#ifdef CONFIG_COMPAT
		if (!COMPAT_USE_64BIT_TIME &&
		    (msg->msg_flags & MSG_CMSG_COMPAT)) {
			ctv.tv_sec = tv.tv_sec;
			ctv.tv_usec = tv.tv_usec;
			data = &ctv;
			len = sizeof(ctv);
		}
#endif

		put_cmsg(msg, SOL_HCI, HCI_CMSG_TSTAMP, len, data);
	}
}

static int hci_sock_recvmsg(struct kiocb *iocb, struct socket *sock,
			    struct msghdr *msg, size_t len, int flags)
{
	int noblock = flags & MSG_DONTWAIT;
	struct sock *sk = sock->sk;
	struct sk_buff *skb;
	int copied, err;

	BT_DBG("sock %p, sk %p", sock, sk);

	if (flags & (MSG_OOB))
		return -EOPNOTSUPP;

	if (sk->sk_state == BT_CLOSED)
		return 0;

	skb = skb_recv_datagram(sk, flags, noblock, &err);
	if (!skb)
		return err;

	msg->msg_namelen = 0;

	copied = skb->len;
	if (len < copied) {
		msg->msg_flags |= MSG_TRUNC;
		copied = len;
	}

	skb_reset_transport_header(skb);
	err = skb_copy_datagram_iovec(skb, 0, msg->msg_iov, copied);

	switch (hci_pi(sk)->channel) {
	case HCI_CHANNEL_RAW:
		hci_sock_cmsg(sk, msg, skb);
		break;
	case HCI_CHANNEL_CONTROL:
	case HCI_CHANNEL_MONITOR:
		sock_recv_timestamp(msg, sk, skb);
		break;
	}

	skb_free_datagram(sk, skb);

	return err ? : copied;
}

static int hci_sock_sendmsg(struct kiocb *iocb, struct socket *sock,
			    struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	struct hci_dev *hdev;
	struct sk_buff *skb;
	int err;

	BT_DBG("sock %p sk %p", sock, sk);

	if (msg->msg_flags & MSG_OOB)
		return -EOPNOTSUPP;

	if (msg->msg_flags & ~(MSG_DONTWAIT|MSG_NOSIGNAL|MSG_ERRQUEUE))
		return -EINVAL;

	if (len < 4 || len > HCI_MAX_FRAME_SIZE)
		return -EINVAL;

	lock_sock(sk);

	switch (hci_pi(sk)->channel) {
	case HCI_CHANNEL_RAW:
		break;
	case HCI_CHANNEL_CONTROL:
		err = mgmt_control(sk, msg, len);
		goto done;
	case HCI_CHANNEL_MONITOR:
		err = -EOPNOTSUPP;
		goto done;
	default:
		err = -EINVAL;
		goto done;
	}

	hdev = hci_pi(sk)->hdev;
	if (!hdev) {
		err = -EBADFD;
		goto done;
	}

	if (!test_bit(HCI_UP, &hdev->flags)) {
		err = -ENETDOWN;
		goto done;
	}

	skb = bt_skb_send_alloc(sk, len, msg->msg_flags & MSG_DONTWAIT, &err);
	if (!skb)
		goto done;

	if (memcpy_fromiovec(skb_put(skb, len), msg->msg_iov, len)) {
		err = -EFAULT;
		goto drop;
	}

	bt_cb(skb)->pkt_type = *((unsigned char *) skb->data);
	skb_pull(skb, 1);
	skb->dev = (void *) hdev;

	if (bt_cb(skb)->pkt_type == HCI_COMMAND_PKT) {
		u16 opcode = get_unaligned_le16(skb->data);
		u16 ogf = hci_opcode_ogf(opcode);
		u16 ocf = hci_opcode_ocf(opcode);

		if (((ogf > HCI_SFLT_MAX_OGF) ||
		     !hci_test_bit(ocf & HCI_FLT_OCF_BITS,
				   &hci_sec_filter.ocf_mask[ogf])) &&
		    !capable(CAP_NET_RAW)) {
			err = -EPERM;
			goto drop;
		}

		if (test_bit(HCI_RAW, &hdev->flags) || (ogf == 0x3f)) {
			skb_queue_tail(&hdev->raw_q, skb);
			queue_work(hdev->workqueue, &hdev->tx_work);
		} else {
			/* Stand-alone HCI commands must be flaged as
			 * single-command requests.
			 */
			bt_cb(skb)->req.start = true;

			skb_queue_tail(&hdev->cmd_q, skb);
			queue_work(hdev->workqueue, &hdev->cmd_work);
		}
	} else {
		if (!capable(CAP_NET_RAW)) {
			err = -EPERM;
			goto drop;
		}

		skb_queue_tail(&hdev->raw_q, skb);
		queue_work(hdev->workqueue, &hdev->tx_work);
	}

	err = len;

done:
	release_sock(sk);
	return err;

drop:
	kfree_skb(skb);
	goto done;
}

static int hci_sock_setsockopt(struct socket *sock, int level, int optname,
			       char __user *optval, unsigned int len)
{
	struct hci_ufilter uf = { .opcode = 0 };
	struct sock *sk = sock->sk;
	int err = 0, opt = 0;

	BT_DBG("sk %p, opt %d", sk, optname);

	lock_sock(sk);

	if (hci_pi(sk)->channel != HCI_CHANNEL_RAW) {
		err = -EINVAL;
		goto done;
	}

	switch (optname) {
	case HCI_DATA_DIR:
		if (get_user(opt, (int __user *)optval)) {
			err = -EFAULT;
			break;
		}

		if (opt)
			hci_pi(sk)->cmsg_mask |= HCI_CMSG_DIR;
		else
			hci_pi(sk)->cmsg_mask &= ~HCI_CMSG_DIR;
		break;

	case HCI_TIME_STAMP:
		if (get_user(opt, (int __user *)optval)) {
			err = -EFAULT;
			break;
		}

		if (opt)
			hci_pi(sk)->cmsg_mask |= HCI_CMSG_TSTAMP;
		else
			hci_pi(sk)->cmsg_mask &= ~HCI_CMSG_TSTAMP;
		break;

	case HCI_FILTER:
		{
			struct hci_filter *f = &hci_pi(sk)->filter;

			uf.type_mask = f->type_mask;
			uf.opcode    = f->opcode;
			uf.event_mask[0] = *((u32 *) f->event_mask + 0);
			uf.event_mask[1] = *((u32 *) f->event_mask + 1);
		}

		len = min_t(unsigned int, len, sizeof(uf));
		if (copy_from_user(&uf, optval, len)) {
			err = -EFAULT;
			break;
		}

		if (!capable(CAP_NET_RAW)) {
			uf.type_mask &= hci_sec_filter.type_mask;
			uf.event_mask[0] &= *((u32 *) hci_sec_filter.event_mask + 0);
			uf.event_mask[1] &= *((u32 *) hci_sec_filter.event_mask + 1);
		}

		{
			struct hci_filter *f = &hci_pi(sk)->filter;

			f->type_mask = uf.type_mask;
			f->opcode    = uf.opcode;
			*((u32 *) f->event_mask + 0) = uf.event_mask[0];
			*((u32 *) f->event_mask + 1) = uf.event_mask[1];
		}
		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

done:
	release_sock(sk);
	return err;
}

static int hci_sock_getsockopt(struct socket *sock, int level, int optname,
			       char __user *optval, int __user *optlen)
{
	struct hci_ufilter uf;
	struct sock *sk = sock->sk;
	int len, opt, err = 0;

	BT_DBG("sk %p, opt %d", sk, optname);

	if (get_user(len, optlen))
		return -EFAULT;

	lock_sock(sk);

	if (hci_pi(sk)->channel != HCI_CHANNEL_RAW) {
		err = -EINVAL;
		goto done;
	}

	switch (optname) {
	case HCI_DATA_DIR:
		if (hci_pi(sk)->cmsg_mask & HCI_CMSG_DIR)
			opt = 1;
		else
			opt = 0;

		if (put_user(opt, optval))
			err = -EFAULT;
		break;

	case HCI_TIME_STAMP:
		if (hci_pi(sk)->cmsg_mask & HCI_CMSG_TSTAMP)
			opt = 1;
		else
			opt = 0;

		if (put_user(opt, optval))
			err = -EFAULT;
		break;

	case HCI_FILTER:
		{
			struct hci_filter *f = &hci_pi(sk)->filter;

			memset(&uf, 0, sizeof(uf));
			uf.type_mask = f->type_mask;
			uf.opcode    = f->opcode;
			uf.event_mask[0] = *((u32 *) f->event_mask + 0);
			uf.event_mask[1] = *((u32 *) f->event_mask + 1);
		}

		len = min_t(unsigned int, len, sizeof(uf));
		if (copy_to_user(optval, &uf, len))
			err = -EFAULT;
		break;

	default:
		err = -ENOPROTOOPT;
		break;
	}

done:
	release_sock(sk);
	return err;
}

static const struct proto_ops hci_sock_ops = {
	.family		= PF_BLUETOOTH,
	.owner		= THIS_MODULE,
	.release	= hci_sock_release,
	.bind		= hci_sock_bind,
	.getname	= hci_sock_getname,
	.sendmsg	= hci_sock_sendmsg,
	.recvmsg	= hci_sock_recvmsg,
	.ioctl		= hci_sock_ioctl,
	.poll		= datagram_poll,
	.listen		= sock_no_listen,
	.shutdown	= sock_no_shutdown,
	.setsockopt	= hci_sock_setsockopt,
	.getsockopt	= hci_sock_getsockopt,
	.connect	= sock_no_connect,
	.socketpair	= sock_no_socketpair,
	.accept		= sock_no_accept,
	.mmap		= sock_no_mmap
};

static struct proto hci_sk_proto = {
	.name		= "HCI",
	.owner		= THIS_MODULE,
	.obj_size	= sizeof(struct hci_pinfo)
};

static int hci_sock_create(struct net *net, struct socket *sock, int protocol,
			   int kern)
{
	struct sock *sk;

	BT_DBG("sock %p", sock);

	if (sock->type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;

	sock->ops = &hci_sock_ops;

	sk = sk_alloc(net, PF_BLUETOOTH, GFP_ATOMIC, &hci_sk_proto);
	if (!sk)
		return -ENOMEM;

	sock_init_data(sock, sk);

	sock_reset_flag(sk, SOCK_ZAPPED);

	sk->sk_protocol = protocol;

	sock->state = SS_UNCONNECTED;
	sk->sk_state = BT_OPEN;

	bt_sock_link(&hci_sk_list, sk);
	return 0;
}

static const struct net_proto_family hci_sock_family_ops = {
	.family	= PF_BLUETOOTH,
	.owner	= THIS_MODULE,
	.create	= hci_sock_create,
};

int __init hci_sock_init(void)
{
	int err;

	err = proto_register(&hci_sk_proto, 0);
	if (err < 0)
		return err;

	err = bt_sock_register(BTPROTO_HCI, &hci_sock_family_ops);
	if (err < 0) {
		BT_ERR("HCI socket registration failed");
		goto error;
	}

	err = bt_procfs_init(&init_net, "hci", &hci_sk_list, NULL);
	if (err < 0) {
		BT_ERR("Failed to create HCI proc file");
		bt_sock_unregister(BTPROTO_HCI);
		goto error;
	}

	BT_INFO("HCI socket layer initialized");

	return 0;

error:
	proto_unregister(&hci_sk_proto);
	return err;
}

void hci_sock_cleanup(void)
{
	bt_procfs_cleanup(&init_net, "hci");
	bt_sock_unregister(BTPROTO_HCI);
	proto_unregister(&hci_sk_proto);
}
