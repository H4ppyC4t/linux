// SPDX-License-Identifier: GPL-2.0-or-later
/* L2TPv3 IP encapsulation support for IPv6
 *
 * Copyright (c) 2012 Katalix Systems Ltd
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/icmp.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/random.h>
#include <linux/socket.h>
#include <linux/l2tp.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <net/sock.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/udp.h>
#include <net/inet_common.h>
#include <net/tcp_states.h>
#include <net/protocol.h>
#include <net/xfrm.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>

#include <net/transp_v6.h>
#include <net/addrconf.h>
#include <net/ip6_route.h>

#include "l2tp_core.h"

/* per-net private data for this module */
static unsigned int l2tp_ip6_net_id;
struct l2tp_ip6_net {
	rwlock_t l2tp_ip6_lock;
	struct hlist_head l2tp_ip6_table;
	struct hlist_head l2tp_ip6_bind_table;
};

struct l2tp_ip6_sock {
	/* inet_sock has to be the first member of l2tp_ip6_sock */
	struct inet_sock	inet;

	u32			conn_id;
	u32			peer_conn_id;

	struct ipv6_pinfo	inet6;
};

static struct l2tp_ip6_sock *l2tp_ip6_sk(const struct sock *sk)
{
	return (struct l2tp_ip6_sock *)sk;
}

static struct l2tp_ip6_net *l2tp_ip6_pernet(const struct net *net)
{
	return net_generic(net, l2tp_ip6_net_id);
}

static struct sock *__l2tp_ip6_bind_lookup(const struct net *net,
					   const struct in6_addr *laddr,
					   const struct in6_addr *raddr,
					   int dif, u32 tunnel_id)
{
	struct l2tp_ip6_net *pn = l2tp_ip6_pernet(net);
	struct sock *sk;

	sk_for_each_bound(sk, &pn->l2tp_ip6_bind_table) {
		const struct in6_addr *sk_laddr = inet6_rcv_saddr(sk);
		const struct in6_addr *sk_raddr = &sk->sk_v6_daddr;
		const struct l2tp_ip6_sock *l2tp = l2tp_ip6_sk(sk);
		int bound_dev_if;

		if (!net_eq(sock_net(sk), net))
			continue;

		bound_dev_if = READ_ONCE(sk->sk_bound_dev_if);
		if (bound_dev_if && dif && bound_dev_if != dif)
			continue;

		if (sk_laddr && !ipv6_addr_any(sk_laddr) &&
		    !ipv6_addr_any(laddr) && !ipv6_addr_equal(sk_laddr, laddr))
			continue;

		if (!ipv6_addr_any(sk_raddr) && raddr &&
		    !ipv6_addr_any(raddr) && !ipv6_addr_equal(sk_raddr, raddr))
			continue;

		if (l2tp->conn_id != tunnel_id)
			continue;

		goto found;
	}

	sk = NULL;
found:
	return sk;
}

/* When processing receive frames, there are two cases to
 * consider. Data frames consist of a non-zero session-id and an
 * optional cookie. Control frames consist of a regular L2TP header
 * preceded by 32-bits of zeros.
 *
 * L2TPv3 Session Header Over IP
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                           Session ID                          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               Cookie (optional, maximum 64 bits)...
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *                                                                 |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * L2TPv3 Control Message Header Over IP
 *
 *  0                   1                   2                   3
 *  0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                      (32 bits of zeros)                       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |T|L|x|x|S|x|x|x|x|x|x|x|  Ver  |             Length            |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                     Control Connection ID                     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |               Ns              |               Nr              |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * All control frames are passed to userspace.
 */
static int l2tp_ip6_recv(struct sk_buff *skb)
{
	struct net *net = dev_net(skb->dev);
	struct l2tp_ip6_net *pn;
	struct sock *sk;
	u32 session_id;
	u32 tunnel_id;
	unsigned char *ptr, *optr;
	struct l2tp_session *session;
	struct l2tp_tunnel *tunnel = NULL;
	struct ipv6hdr *iph;

	pn = l2tp_ip6_pernet(net);

	if (!pskb_may_pull(skb, 4))
		goto discard;

	/* Point to L2TP header */
	optr = skb->data;
	ptr = skb->data;
	session_id = ntohl(*((__be32 *)ptr));
	ptr += 4;

	/* RFC3931: L2TP/IP packets have the first 4 bytes containing
	 * the session_id. If it is 0, the packet is a L2TP control
	 * frame and the session_id value can be discarded.
	 */
	if (session_id == 0) {
		__skb_pull(skb, 4);
		goto pass_up;
	}

	/* Ok, this is a data packet. Lookup the session. */
	session = l2tp_v3_session_get(net, NULL, session_id);
	if (!session)
		goto discard;

	tunnel = session->tunnel;
	if (!tunnel)
		goto discard_sess;

	if (l2tp_v3_ensure_opt_in_linear(session, skb, &ptr, &optr))
		goto discard_sess;

	l2tp_recv_common(session, skb, ptr, optr, 0, skb->len);
	l2tp_session_put(session);

	return 0;

pass_up:
	/* Get the tunnel_id from the L2TP header */
	if (!pskb_may_pull(skb, 12))
		goto discard;

	if ((skb->data[0] & 0xc0) != 0xc0)
		goto discard;

	tunnel_id = ntohl(*(__be32 *)&skb->data[4]);
	iph = ipv6_hdr(skb);

	read_lock_bh(&pn->l2tp_ip6_lock);
	sk = __l2tp_ip6_bind_lookup(net, &iph->daddr, &iph->saddr,
				    inet6_iif(skb), tunnel_id);
	if (!sk) {
		read_unlock_bh(&pn->l2tp_ip6_lock);
		goto discard;
	}
	sock_hold(sk);
	read_unlock_bh(&pn->l2tp_ip6_lock);

	if (!xfrm6_policy_check(sk, XFRM_POLICY_IN, skb))
		goto discard_put;

	nf_reset_ct(skb);

	return sk_receive_skb(sk, skb, 1);

discard_sess:
	l2tp_session_put(session);
	goto discard;

discard_put:
	sock_put(sk);

discard:
	kfree_skb(skb);
	return 0;
}

static int l2tp_ip6_hash(struct sock *sk)
{
	struct l2tp_ip6_net *pn = l2tp_ip6_pernet(sock_net(sk));

	if (sk_unhashed(sk)) {
		write_lock_bh(&pn->l2tp_ip6_lock);
		sk_add_node(sk, &pn->l2tp_ip6_table);
		write_unlock_bh(&pn->l2tp_ip6_lock);
	}
	return 0;
}

static void l2tp_ip6_unhash(struct sock *sk)
{
	struct l2tp_ip6_net *pn = l2tp_ip6_pernet(sock_net(sk));

	if (sk_unhashed(sk))
		return;
	write_lock_bh(&pn->l2tp_ip6_lock);
	sk_del_node_init(sk);
	write_unlock_bh(&pn->l2tp_ip6_lock);
}

static int l2tp_ip6_open(struct sock *sk)
{
	/* Prevent autobind. We don't have ports. */
	inet_sk(sk)->inet_num = IPPROTO_L2TP;

	l2tp_ip6_hash(sk);
	return 0;
}

static void l2tp_ip6_close(struct sock *sk, long timeout)
{
	struct l2tp_ip6_net *pn = l2tp_ip6_pernet(sock_net(sk));

	write_lock_bh(&pn->l2tp_ip6_lock);
	hlist_del_init(&sk->sk_bind_node);
	sk_del_node_init(sk);
	write_unlock_bh(&pn->l2tp_ip6_lock);

	sk_common_release(sk);
}

static void l2tp_ip6_destroy_sock(struct sock *sk)
{
	struct l2tp_tunnel *tunnel;

	lock_sock(sk);
	ip6_flush_pending_frames(sk);
	release_sock(sk);

	tunnel = l2tp_sk_to_tunnel(sk);
	if (tunnel) {
		l2tp_tunnel_delete(tunnel);
		l2tp_tunnel_put(tunnel);
	}
}

static int l2tp_ip6_bind(struct sock *sk, struct sockaddr *uaddr, int addr_len)
{
	struct inet_sock *inet = inet_sk(sk);
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct sockaddr_l2tpip6 *addr = (struct sockaddr_l2tpip6 *)uaddr;
	struct net *net = sock_net(sk);
	struct l2tp_ip6_net *pn;
	__be32 v4addr = 0;
	int bound_dev_if;
	int addr_type;
	int err;

	pn = l2tp_ip6_pernet(net);

	if (addr->l2tp_family != AF_INET6)
		return -EINVAL;
	if (addr_len < sizeof(*addr))
		return -EINVAL;

	addr_type = ipv6_addr_type(&addr->l2tp_addr);

	/* l2tp_ip6 sockets are IPv6 only */
	if (addr_type == IPV6_ADDR_MAPPED)
		return -EADDRNOTAVAIL;

	/* L2TP is point-point, not multicast */
	if (addr_type & IPV6_ADDR_MULTICAST)
		return -EADDRNOTAVAIL;

	lock_sock(sk);

	err = -EINVAL;
	if (!sock_flag(sk, SOCK_ZAPPED))
		goto out_unlock;

	if (sk->sk_state != TCP_CLOSE)
		goto out_unlock;

	bound_dev_if = sk->sk_bound_dev_if;

	/* Check if the address belongs to the host. */
	rcu_read_lock();
	if (addr_type != IPV6_ADDR_ANY) {
		struct net_device *dev = NULL;

		if (addr_type & IPV6_ADDR_LINKLOCAL) {
			if (addr->l2tp_scope_id)
				bound_dev_if = addr->l2tp_scope_id;

			/* Binding to link-local address requires an
			 * interface.
			 */
			if (!bound_dev_if)
				goto out_unlock_rcu;

			err = -ENODEV;
			dev = dev_get_by_index_rcu(sock_net(sk), bound_dev_if);
			if (!dev)
				goto out_unlock_rcu;
		}

		/* ipv4 addr of the socket is invalid.  Only the
		 * unspecified and mapped address have a v4 equivalent.
		 */
		v4addr = LOOPBACK4_IPV6;
		err = -EADDRNOTAVAIL;
		if (!ipv6_chk_addr(sock_net(sk), &addr->l2tp_addr, dev, 0))
			goto out_unlock_rcu;
	}
	rcu_read_unlock();

	write_lock_bh(&pn->l2tp_ip6_lock);
	if (__l2tp_ip6_bind_lookup(net, &addr->l2tp_addr, NULL, bound_dev_if,
				   addr->l2tp_conn_id)) {
		write_unlock_bh(&pn->l2tp_ip6_lock);
		err = -EADDRINUSE;
		goto out_unlock;
	}

	inet->inet_saddr = v4addr;
	inet->inet_rcv_saddr = v4addr;
	sk->sk_bound_dev_if = bound_dev_if;
	sk->sk_v6_rcv_saddr = addr->l2tp_addr;
	np->saddr = addr->l2tp_addr;

	l2tp_ip6_sk(sk)->conn_id = addr->l2tp_conn_id;

	sk_add_bind_node(sk, &pn->l2tp_ip6_bind_table);
	sk_del_node_init(sk);
	write_unlock_bh(&pn->l2tp_ip6_lock);

	sock_reset_flag(sk, SOCK_ZAPPED);
	release_sock(sk);
	return 0;

out_unlock_rcu:
	rcu_read_unlock();
out_unlock:
	release_sock(sk);

	return err;
}

static int l2tp_ip6_connect(struct sock *sk, struct sockaddr *uaddr,
			    int addr_len)
{
	struct sockaddr_l2tpip6 *lsa = (struct sockaddr_l2tpip6 *)uaddr;
	struct sockaddr_in6	*usin = (struct sockaddr_in6 *)uaddr;
	struct in6_addr	*daddr;
	int	addr_type;
	int rc;
	struct l2tp_ip6_net *pn;

	if (addr_len < sizeof(*lsa))
		return -EINVAL;

	if (usin->sin6_family != AF_INET6)
		return -EINVAL;

	addr_type = ipv6_addr_type(&usin->sin6_addr);
	if (addr_type & IPV6_ADDR_MULTICAST)
		return -EINVAL;

	if (addr_type & IPV6_ADDR_MAPPED) {
		daddr = &usin->sin6_addr;
		if (ipv4_is_multicast(daddr->s6_addr32[3]))
			return -EINVAL;
	}

	lock_sock(sk);

	 /* Must bind first - autobinding does not work */
	if (sock_flag(sk, SOCK_ZAPPED)) {
		rc = -EINVAL;
		goto out_sk;
	}

	rc = __ip6_datagram_connect(sk, uaddr, addr_len);
	if (rc < 0)
		goto out_sk;

	l2tp_ip6_sk(sk)->peer_conn_id = lsa->l2tp_conn_id;

	pn = l2tp_ip6_pernet(sock_net(sk));
	write_lock_bh(&pn->l2tp_ip6_lock);
	hlist_del_init(&sk->sk_bind_node);
	sk_add_bind_node(sk, &pn->l2tp_ip6_bind_table);
	write_unlock_bh(&pn->l2tp_ip6_lock);

out_sk:
	release_sock(sk);

	return rc;
}

static int l2tp_ip6_disconnect(struct sock *sk, int flags)
{
	if (sock_flag(sk, SOCK_ZAPPED))
		return 0;

	return __udp_disconnect(sk, flags);
}

static int l2tp_ip6_getname(struct socket *sock, struct sockaddr *uaddr,
			    int peer)
{
	struct sockaddr_l2tpip6 *lsa = (struct sockaddr_l2tpip6 *)uaddr;
	struct sock *sk = sock->sk;
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct l2tp_ip6_sock *lsk = l2tp_ip6_sk(sk);

	lsa->l2tp_family = AF_INET6;
	lsa->l2tp_flowinfo = 0;
	lsa->l2tp_scope_id = 0;
	lsa->l2tp_unused = 0;
	if (peer) {
		if (!lsk->peer_conn_id)
			return -ENOTCONN;
		lsa->l2tp_conn_id = lsk->peer_conn_id;
		lsa->l2tp_addr = sk->sk_v6_daddr;
		if (inet6_test_bit(SNDFLOW, sk))
			lsa->l2tp_flowinfo = np->flow_label;
	} else {
		if (ipv6_addr_any(&sk->sk_v6_rcv_saddr))
			lsa->l2tp_addr = np->saddr;
		else
			lsa->l2tp_addr = sk->sk_v6_rcv_saddr;

		lsa->l2tp_conn_id = lsk->conn_id;
	}
	if (ipv6_addr_type(&lsa->l2tp_addr) & IPV6_ADDR_LINKLOCAL)
		lsa->l2tp_scope_id = READ_ONCE(sk->sk_bound_dev_if);
	return sizeof(*lsa);
}

static int l2tp_ip6_backlog_recv(struct sock *sk, struct sk_buff *skb)
{
	int rc;

	/* Charge it to the socket, dropping if the queue is full. */
	rc = sock_queue_rcv_skb(sk, skb);
	if (rc < 0)
		goto drop;

	return 0;

drop:
	IP_INC_STATS(sock_net(sk), IPSTATS_MIB_INDISCARDS);
	kfree_skb(skb);
	return -1;
}

static int l2tp_ip6_push_pending_frames(struct sock *sk)
{
	struct sk_buff *skb;
	__be32 *transhdr = NULL;
	int err = 0;

	skb = skb_peek(&sk->sk_write_queue);
	if (!skb)
		goto out;

	transhdr = (__be32 *)skb_transport_header(skb);
	*transhdr = 0;

	err = ip6_push_pending_frames(sk);

out:
	return err;
}

/* Userspace will call sendmsg() on the tunnel socket to send L2TP
 * control frames.
 */
static int l2tp_ip6_sendmsg(struct sock *sk, struct msghdr *msg, size_t len)
{
	struct ipv6_txoptions opt_space;
	DECLARE_SOCKADDR(struct sockaddr_l2tpip6 *, lsa, msg->msg_name);
	struct in6_addr *daddr, *final_p, final;
	struct ipv6_pinfo *np = inet6_sk(sk);
	struct ipv6_txoptions *opt_to_free = NULL;
	struct ipv6_txoptions *opt = NULL;
	struct ip6_flowlabel *flowlabel = NULL;
	struct dst_entry *dst = NULL;
	struct flowi6 fl6;
	struct ipcm6_cookie ipc6;
	int addr_len = msg->msg_namelen;
	int transhdrlen = 4; /* zero session-id */
	int ulen;
	int err;

	/* Rough check on arithmetic overflow,
	 * better check is made in ip6_append_data().
	 */
	if (len > INT_MAX - transhdrlen)
		return -EMSGSIZE;

	/* Mirror BSD error message compatibility */
	if (msg->msg_flags & MSG_OOB)
		return -EOPNOTSUPP;

	/* Get and verify the address */
	memset(&fl6, 0, sizeof(fl6));

	fl6.flowi6_mark = READ_ONCE(sk->sk_mark);
	fl6.flowi6_uid = sk_uid(sk);

	ipcm6_init_sk(&ipc6, sk);

	if (lsa) {
		if (addr_len < SIN6_LEN_RFC2133)
			return -EINVAL;

		if (lsa->l2tp_family && lsa->l2tp_family != AF_INET6)
			return -EAFNOSUPPORT;

		daddr = &lsa->l2tp_addr;
		if (inet6_test_bit(SNDFLOW, sk)) {
			fl6.flowlabel = lsa->l2tp_flowinfo & IPV6_FLOWINFO_MASK;
			if (fl6.flowlabel & IPV6_FLOWLABEL_MASK) {
				flowlabel = fl6_sock_lookup(sk, fl6.flowlabel);
				if (IS_ERR(flowlabel))
					return -EINVAL;
			}
		}

		/* Otherwise it will be difficult to maintain
		 * sk->sk_dst_cache.
		 */
		if (sk->sk_state == TCP_ESTABLISHED &&
		    ipv6_addr_equal(daddr, &sk->sk_v6_daddr))
			daddr = &sk->sk_v6_daddr;

		if (addr_len >= sizeof(struct sockaddr_in6) &&
		    lsa->l2tp_scope_id &&
		    ipv6_addr_type(daddr) & IPV6_ADDR_LINKLOCAL)
			fl6.flowi6_oif = lsa->l2tp_scope_id;
	} else {
		if (sk->sk_state != TCP_ESTABLISHED)
			return -EDESTADDRREQ;

		daddr = &sk->sk_v6_daddr;
		fl6.flowlabel = np->flow_label;
	}

	if (fl6.flowi6_oif == 0)
		fl6.flowi6_oif = READ_ONCE(sk->sk_bound_dev_if);

	if (msg->msg_controllen) {
		opt = &opt_space;
		memset(opt, 0, sizeof(struct ipv6_txoptions));
		opt->tot_len = sizeof(struct ipv6_txoptions);
		ipc6.opt = opt;

		err = ip6_datagram_send_ctl(sock_net(sk), sk, msg, &fl6, &ipc6);
		if (err < 0) {
			fl6_sock_release(flowlabel);
			return err;
		}
		if ((fl6.flowlabel & IPV6_FLOWLABEL_MASK) && !flowlabel) {
			flowlabel = fl6_sock_lookup(sk, fl6.flowlabel);
			if (IS_ERR(flowlabel))
				return -EINVAL;
		}
		if (!(opt->opt_nflen | opt->opt_flen))
			opt = NULL;
	}

	if (!opt) {
		opt = txopt_get(np);
		opt_to_free = opt;
	}
	if (flowlabel)
		opt = fl6_merge_options(&opt_space, flowlabel, opt);
	opt = ipv6_fixup_options(&opt_space, opt);
	ipc6.opt = opt;

	fl6.flowi6_proto = sk->sk_protocol;
	if (!ipv6_addr_any(daddr))
		fl6.daddr = *daddr;
	else
		fl6.daddr.s6_addr[15] = 0x1; /* :: means loopback (BSD'ism) */
	if (ipv6_addr_any(&fl6.saddr) && !ipv6_addr_any(&np->saddr))
		fl6.saddr = np->saddr;

	final_p = fl6_update_dst(&fl6, opt, &final);

	if (!fl6.flowi6_oif && ipv6_addr_is_multicast(&fl6.daddr))
		fl6.flowi6_oif = READ_ONCE(np->mcast_oif);
	else if (!fl6.flowi6_oif)
		fl6.flowi6_oif = READ_ONCE(np->ucast_oif);

	security_sk_classify_flow(sk, flowi6_to_flowi_common(&fl6));

	fl6.flowlabel = ip6_make_flowinfo(ipc6.tclass, fl6.flowlabel);

	dst = ip6_dst_lookup_flow(sock_net(sk), sk, &fl6, final_p);
	if (IS_ERR(dst)) {
		err = PTR_ERR(dst);
		goto out;
	}

	if (ipc6.hlimit < 0)
		ipc6.hlimit = ip6_sk_dst_hoplimit(np, &fl6, dst);

	if (msg->msg_flags & MSG_CONFIRM)
		goto do_confirm;

back_from_confirm:
	lock_sock(sk);
	ulen = len + (skb_queue_empty(&sk->sk_write_queue) ? transhdrlen : 0);
	err = ip6_append_data(sk, ip_generic_getfrag, msg,
			      ulen, transhdrlen, &ipc6,
			      &fl6, dst_rt6_info(dst),
			      msg->msg_flags);
	if (err)
		ip6_flush_pending_frames(sk);
	else if (!(msg->msg_flags & MSG_MORE))
		err = l2tp_ip6_push_pending_frames(sk);
	release_sock(sk);
done:
	dst_release(dst);
out:
	fl6_sock_release(flowlabel);
	txopt_put(opt_to_free);

	return err < 0 ? err : len;

do_confirm:
	if (msg->msg_flags & MSG_PROBE)
		dst_confirm_neigh(dst, &fl6.daddr);
	if (!(msg->msg_flags & MSG_PROBE) || len)
		goto back_from_confirm;
	err = 0;
	goto done;
}

static int l2tp_ip6_recvmsg(struct sock *sk, struct msghdr *msg, size_t len,
			    int flags, int *addr_len)
{
	struct ipv6_pinfo *np = inet6_sk(sk);
	DECLARE_SOCKADDR(struct sockaddr_l2tpip6 *, lsa, msg->msg_name);
	size_t copied = 0;
	int err = -EOPNOTSUPP;
	struct sk_buff *skb;

	if (flags & MSG_OOB)
		goto out;

	if (flags & MSG_ERRQUEUE)
		return ipv6_recv_error(sk, msg, len, addr_len);

	skb = skb_recv_datagram(sk, flags, &err);
	if (!skb)
		goto out;

	copied = skb->len;
	if (len < copied) {
		msg->msg_flags |= MSG_TRUNC;
		copied = len;
	}

	err = skb_copy_datagram_msg(skb, 0, msg, copied);
	if (err)
		goto done;

	sock_recv_timestamp(msg, sk, skb);

	/* Copy the address. */
	if (lsa) {
		lsa->l2tp_family = AF_INET6;
		lsa->l2tp_unused = 0;
		lsa->l2tp_addr = ipv6_hdr(skb)->saddr;
		lsa->l2tp_flowinfo = 0;
		lsa->l2tp_scope_id = 0;
		lsa->l2tp_conn_id = 0;
		if (ipv6_addr_type(&lsa->l2tp_addr) & IPV6_ADDR_LINKLOCAL)
			lsa->l2tp_scope_id = inet6_iif(skb);
		*addr_len = sizeof(*lsa);
	}

	if (np->rxopt.all)
		ip6_datagram_recv_ctl(sk, msg, skb);

	if (flags & MSG_TRUNC)
		copied = skb->len;
done:
	skb_free_datagram(sk, skb);
out:
	return err ? err : copied;
}

static struct proto l2tp_ip6_prot = {
	.name		   = "L2TP/IPv6",
	.owner		   = THIS_MODULE,
	.init		   = l2tp_ip6_open,
	.close		   = l2tp_ip6_close,
	.bind		   = l2tp_ip6_bind,
	.connect	   = l2tp_ip6_connect,
	.disconnect	   = l2tp_ip6_disconnect,
	.ioctl		   = l2tp_ioctl,
	.destroy	   = l2tp_ip6_destroy_sock,
	.setsockopt	   = ipv6_setsockopt,
	.getsockopt	   = ipv6_getsockopt,
	.sendmsg	   = l2tp_ip6_sendmsg,
	.recvmsg	   = l2tp_ip6_recvmsg,
	.backlog_rcv	   = l2tp_ip6_backlog_recv,
	.hash		   = l2tp_ip6_hash,
	.unhash		   = l2tp_ip6_unhash,
	.obj_size	   = sizeof(struct l2tp_ip6_sock),
	.ipv6_pinfo_offset = offsetof(struct l2tp_ip6_sock, inet6),
};

static const struct proto_ops l2tp_ip6_ops = {
	.family		   = PF_INET6,
	.owner		   = THIS_MODULE,
	.release	   = inet6_release,
	.bind		   = inet6_bind,
	.connect	   = inet_dgram_connect,
	.socketpair	   = sock_no_socketpair,
	.accept		   = sock_no_accept,
	.getname	   = l2tp_ip6_getname,
	.poll		   = datagram_poll,
	.ioctl		   = inet6_ioctl,
	.gettstamp	   = sock_gettstamp,
	.listen		   = sock_no_listen,
	.shutdown	   = inet_shutdown,
	.setsockopt	   = sock_common_setsockopt,
	.getsockopt	   = sock_common_getsockopt,
	.sendmsg	   = inet_sendmsg,
	.recvmsg	   = sock_common_recvmsg,
	.mmap		   = sock_no_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl	   = inet6_compat_ioctl,
#endif
};

static struct inet_protosw l2tp_ip6_protosw = {
	.type		= SOCK_DGRAM,
	.protocol	= IPPROTO_L2TP,
	.prot		= &l2tp_ip6_prot,
	.ops		= &l2tp_ip6_ops,
};

static struct inet6_protocol l2tp_ip6_protocol __read_mostly = {
	.handler	= l2tp_ip6_recv,
};

static __net_init int l2tp_ip6_init_net(struct net *net)
{
	struct l2tp_ip6_net *pn = net_generic(net, l2tp_ip6_net_id);

	rwlock_init(&pn->l2tp_ip6_lock);
	INIT_HLIST_HEAD(&pn->l2tp_ip6_table);
	INIT_HLIST_HEAD(&pn->l2tp_ip6_bind_table);
	return 0;
}

static __net_exit void l2tp_ip6_exit_net(struct net *net)
{
	struct l2tp_ip6_net *pn = l2tp_ip6_pernet(net);

	write_lock_bh(&pn->l2tp_ip6_lock);
	WARN_ON_ONCE(hlist_count_nodes(&pn->l2tp_ip6_table) != 0);
	WARN_ON_ONCE(hlist_count_nodes(&pn->l2tp_ip6_bind_table) != 0);
	write_unlock_bh(&pn->l2tp_ip6_lock);
}

static struct pernet_operations l2tp_ip6_net_ops = {
	.init = l2tp_ip6_init_net,
	.exit = l2tp_ip6_exit_net,
	.id   = &l2tp_ip6_net_id,
	.size = sizeof(struct l2tp_ip6_net),
};

static int __init l2tp_ip6_init(void)
{
	int err;

	pr_info("L2TP IP encapsulation support for IPv6 (L2TPv3)\n");

	err = register_pernet_device(&l2tp_ip6_net_ops);
	if (err)
		goto out;

	err = proto_register(&l2tp_ip6_prot, 1);
	if (err != 0)
		goto out1;

	err = inet6_add_protocol(&l2tp_ip6_protocol, IPPROTO_L2TP);
	if (err)
		goto out2;

	inet6_register_protosw(&l2tp_ip6_protosw);
	return 0;

out2:
	proto_unregister(&l2tp_ip6_prot);
out1:
	unregister_pernet_device(&l2tp_ip6_net_ops);
out:
	return err;
}

static void __exit l2tp_ip6_exit(void)
{
	inet6_unregister_protosw(&l2tp_ip6_protosw);
	inet6_del_protocol(&l2tp_ip6_protocol, IPPROTO_L2TP);
	proto_unregister(&l2tp_ip6_prot);
	unregister_pernet_device(&l2tp_ip6_net_ops);
}

module_init(l2tp_ip6_init);
module_exit(l2tp_ip6_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Elston <celston@katalix.com>");
MODULE_DESCRIPTION("L2TP IP encapsulation for IPv6");
MODULE_VERSION("1.0");

/* Use the values of SOCK_DGRAM (2) as type and IPPROTO_L2TP (115) as protocol,
 * because __stringify doesn't like enums
 */
MODULE_ALIAS_NET_PF_PROTO_TYPE(PF_INET6, 115, 2);
MODULE_ALIAS_NET_PF_PROTO(PF_INET6, 115);
