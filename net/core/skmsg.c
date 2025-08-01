// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2017 - 2018 Covalent IO, Inc. http://covalent.io */

#include <linux/skmsg.h>
#include <linux/skbuff.h>
#include <linux/scatterlist.h>

#include <net/sock.h>
#include <net/tcp.h>
#include <net/tls.h>
#include <trace/events/sock.h>

static bool sk_msg_try_coalesce_ok(struct sk_msg *msg, int elem_first_coalesce)
{
	if (msg->sg.end > msg->sg.start &&
	    elem_first_coalesce < msg->sg.end)
		return true;

	if (msg->sg.end < msg->sg.start &&
	    (elem_first_coalesce > msg->sg.start ||
	     elem_first_coalesce < msg->sg.end))
		return true;

	return false;
}

int sk_msg_alloc(struct sock *sk, struct sk_msg *msg, int len,
		 int elem_first_coalesce)
{
	struct page_frag *pfrag = sk_page_frag(sk);
	u32 osize = msg->sg.size;
	int ret = 0;

	len -= msg->sg.size;
	while (len > 0) {
		struct scatterlist *sge;
		u32 orig_offset;
		int use, i;

		if (!sk_page_frag_refill(sk, pfrag)) {
			ret = -ENOMEM;
			goto msg_trim;
		}

		orig_offset = pfrag->offset;
		use = min_t(int, len, pfrag->size - orig_offset);
		if (!sk_wmem_schedule(sk, use)) {
			ret = -ENOMEM;
			goto msg_trim;
		}

		i = msg->sg.end;
		sk_msg_iter_var_prev(i);
		sge = &msg->sg.data[i];

		if (sk_msg_try_coalesce_ok(msg, elem_first_coalesce) &&
		    sg_page(sge) == pfrag->page &&
		    sge->offset + sge->length == orig_offset) {
			sge->length += use;
		} else {
			if (sk_msg_full(msg)) {
				ret = -ENOSPC;
				break;
			}

			sge = &msg->sg.data[msg->sg.end];
			sg_unmark_end(sge);
			sg_set_page(sge, pfrag->page, use, orig_offset);
			get_page(pfrag->page);
			sk_msg_iter_next(msg, end);
		}

		sk_mem_charge(sk, use);
		msg->sg.size += use;
		pfrag->offset += use;
		len -= use;
	}

	return ret;

msg_trim:
	sk_msg_trim(sk, msg, osize);
	return ret;
}
EXPORT_SYMBOL_GPL(sk_msg_alloc);

int sk_msg_clone(struct sock *sk, struct sk_msg *dst, struct sk_msg *src,
		 u32 off, u32 len)
{
	int i = src->sg.start;
	struct scatterlist *sge = sk_msg_elem(src, i);
	struct scatterlist *sgd = NULL;
	u32 sge_len, sge_off;

	while (off) {
		if (sge->length > off)
			break;
		off -= sge->length;
		sk_msg_iter_var_next(i);
		if (i == src->sg.end && off)
			return -ENOSPC;
		sge = sk_msg_elem(src, i);
	}

	while (len) {
		sge_len = sge->length - off;
		if (sge_len > len)
			sge_len = len;

		if (dst->sg.end)
			sgd = sk_msg_elem(dst, dst->sg.end - 1);

		if (sgd &&
		    (sg_page(sge) == sg_page(sgd)) &&
		    (sg_virt(sge) + off == sg_virt(sgd) + sgd->length)) {
			sgd->length += sge_len;
			dst->sg.size += sge_len;
		} else if (!sk_msg_full(dst)) {
			sge_off = sge->offset + off;
			sk_msg_page_add(dst, sg_page(sge), sge_len, sge_off);
		} else {
			return -ENOSPC;
		}

		off = 0;
		len -= sge_len;
		sk_mem_charge(sk, sge_len);
		sk_msg_iter_var_next(i);
		if (i == src->sg.end && len)
			return -ENOSPC;
		sge = sk_msg_elem(src, i);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(sk_msg_clone);

void sk_msg_return_zero(struct sock *sk, struct sk_msg *msg, int bytes)
{
	int i = msg->sg.start;

	do {
		struct scatterlist *sge = sk_msg_elem(msg, i);

		if (bytes < sge->length) {
			sge->length -= bytes;
			sge->offset += bytes;
			sk_mem_uncharge(sk, bytes);
			break;
		}

		sk_mem_uncharge(sk, sge->length);
		bytes -= sge->length;
		sge->length = 0;
		sge->offset = 0;
		sk_msg_iter_var_next(i);
	} while (bytes && i != msg->sg.end);
	msg->sg.start = i;
}
EXPORT_SYMBOL_GPL(sk_msg_return_zero);

void sk_msg_return(struct sock *sk, struct sk_msg *msg, int bytes)
{
	int i = msg->sg.start;

	do {
		struct scatterlist *sge = &msg->sg.data[i];
		int uncharge = (bytes < sge->length) ? bytes : sge->length;

		sk_mem_uncharge(sk, uncharge);
		bytes -= uncharge;
		sk_msg_iter_var_next(i);
	} while (i != msg->sg.end);
}
EXPORT_SYMBOL_GPL(sk_msg_return);

static int sk_msg_free_elem(struct sock *sk, struct sk_msg *msg, u32 i,
			    bool charge)
{
	struct scatterlist *sge = sk_msg_elem(msg, i);
	u32 len = sge->length;

	/* When the skb owns the memory we free it from consume_skb path. */
	if (!msg->skb) {
		if (charge)
			sk_mem_uncharge(sk, len);
		put_page(sg_page(sge));
	}
	memset(sge, 0, sizeof(*sge));
	return len;
}

static int __sk_msg_free(struct sock *sk, struct sk_msg *msg, u32 i,
			 bool charge)
{
	struct scatterlist *sge = sk_msg_elem(msg, i);
	int freed = 0;

	while (msg->sg.size) {
		msg->sg.size -= sge->length;
		freed += sk_msg_free_elem(sk, msg, i, charge);
		sk_msg_iter_var_next(i);
		sk_msg_check_to_free(msg, i, msg->sg.size);
		sge = sk_msg_elem(msg, i);
	}
	consume_skb(msg->skb);
	sk_msg_init(msg);
	return freed;
}

int sk_msg_free_nocharge(struct sock *sk, struct sk_msg *msg)
{
	return __sk_msg_free(sk, msg, msg->sg.start, false);
}
EXPORT_SYMBOL_GPL(sk_msg_free_nocharge);

int sk_msg_free(struct sock *sk, struct sk_msg *msg)
{
	return __sk_msg_free(sk, msg, msg->sg.start, true);
}
EXPORT_SYMBOL_GPL(sk_msg_free);

static void __sk_msg_free_partial(struct sock *sk, struct sk_msg *msg,
				  u32 bytes, bool charge)
{
	struct scatterlist *sge;
	u32 i = msg->sg.start;

	while (bytes) {
		sge = sk_msg_elem(msg, i);
		if (!sge->length)
			break;
		if (bytes < sge->length) {
			if (charge)
				sk_mem_uncharge(sk, bytes);
			sge->length -= bytes;
			sge->offset += bytes;
			msg->sg.size -= bytes;
			break;
		}

		msg->sg.size -= sge->length;
		bytes -= sge->length;
		sk_msg_free_elem(sk, msg, i, charge);
		sk_msg_iter_var_next(i);
		sk_msg_check_to_free(msg, i, bytes);
	}
	msg->sg.start = i;
}

void sk_msg_free_partial(struct sock *sk, struct sk_msg *msg, u32 bytes)
{
	__sk_msg_free_partial(sk, msg, bytes, true);
}
EXPORT_SYMBOL_GPL(sk_msg_free_partial);

void sk_msg_free_partial_nocharge(struct sock *sk, struct sk_msg *msg,
				  u32 bytes)
{
	__sk_msg_free_partial(sk, msg, bytes, false);
}

void sk_msg_trim(struct sock *sk, struct sk_msg *msg, int len)
{
	int trim = msg->sg.size - len;
	u32 i = msg->sg.end;

	if (trim <= 0) {
		WARN_ON(trim < 0);
		return;
	}

	sk_msg_iter_var_prev(i);
	msg->sg.size = len;
	while (msg->sg.data[i].length &&
	       trim >= msg->sg.data[i].length) {
		trim -= msg->sg.data[i].length;
		sk_msg_free_elem(sk, msg, i, true);
		sk_msg_iter_var_prev(i);
		if (!trim)
			goto out;
	}

	msg->sg.data[i].length -= trim;
	sk_mem_uncharge(sk, trim);
	/* Adjust copybreak if it falls into the trimmed part of last buf */
	if (msg->sg.curr == i && msg->sg.copybreak > msg->sg.data[i].length)
		msg->sg.copybreak = msg->sg.data[i].length;
out:
	sk_msg_iter_var_next(i);
	msg->sg.end = i;

	/* If we trim data a full sg elem before curr pointer update
	 * copybreak and current so that any future copy operations
	 * start at new copy location.
	 * However trimmed data that has not yet been used in a copy op
	 * does not require an update.
	 */
	if (!msg->sg.size) {
		msg->sg.curr = msg->sg.start;
		msg->sg.copybreak = 0;
	} else if (sk_msg_iter_dist(msg->sg.start, msg->sg.curr) >=
		   sk_msg_iter_dist(msg->sg.start, msg->sg.end)) {
		sk_msg_iter_var_prev(i);
		msg->sg.curr = i;
		msg->sg.copybreak = msg->sg.data[i].length;
	}
}
EXPORT_SYMBOL_GPL(sk_msg_trim);

int sk_msg_zerocopy_from_iter(struct sock *sk, struct iov_iter *from,
			      struct sk_msg *msg, u32 bytes)
{
	int i, maxpages, ret = 0, num_elems = sk_msg_elem_used(msg);
	const int to_max_pages = MAX_MSG_FRAGS;
	struct page *pages[MAX_MSG_FRAGS];
	ssize_t orig, copied, use, offset;

	orig = msg->sg.size;
	while (bytes > 0) {
		i = 0;
		maxpages = to_max_pages - num_elems;
		if (maxpages == 0) {
			ret = -EFAULT;
			goto out;
		}

		copied = iov_iter_get_pages2(from, pages, bytes, maxpages,
					    &offset);
		if (copied <= 0) {
			ret = -EFAULT;
			goto out;
		}

		bytes -= copied;
		msg->sg.size += copied;

		while (copied) {
			use = min_t(int, copied, PAGE_SIZE - offset);
			sg_set_page(&msg->sg.data[msg->sg.end],
				    pages[i], use, offset);
			sg_unmark_end(&msg->sg.data[msg->sg.end]);
			sk_mem_charge(sk, use);

			offset = 0;
			copied -= use;
			sk_msg_iter_next(msg, end);
			num_elems++;
			i++;
		}
		/* When zerocopy is mixed with sk_msg_*copy* operations we
		 * may have a copybreak set in this case clear and prefer
		 * zerocopy remainder when possible.
		 */
		msg->sg.copybreak = 0;
		msg->sg.curr = msg->sg.end;
	}
out:
	/* Revert iov_iter updates, msg will need to use 'trim' later if it
	 * also needs to be cleared.
	 */
	if (ret)
		iov_iter_revert(from, msg->sg.size - orig);
	return ret;
}
EXPORT_SYMBOL_GPL(sk_msg_zerocopy_from_iter);

int sk_msg_memcopy_from_iter(struct sock *sk, struct iov_iter *from,
			     struct sk_msg *msg, u32 bytes)
{
	int ret = -ENOSPC, i = msg->sg.curr;
	u32 copy, buf_size, copied = 0;
	struct scatterlist *sge;
	void *to;

	do {
		sge = sk_msg_elem(msg, i);
		/* This is possible if a trim operation shrunk the buffer */
		if (msg->sg.copybreak >= sge->length) {
			msg->sg.copybreak = 0;
			sk_msg_iter_var_next(i);
			if (i == msg->sg.end)
				break;
			sge = sk_msg_elem(msg, i);
		}

		buf_size = sge->length - msg->sg.copybreak;
		copy = (buf_size > bytes) ? bytes : buf_size;
		to = sg_virt(sge) + msg->sg.copybreak;
		msg->sg.copybreak += copy;
		if (sk->sk_route_caps & NETIF_F_NOCACHE_COPY)
			ret = copy_from_iter_nocache(to, copy, from);
		else
			ret = copy_from_iter(to, copy, from);
		if (ret != copy) {
			ret = -EFAULT;
			goto out;
		}
		bytes -= copy;
		copied += copy;
		if (!bytes)
			break;
		msg->sg.copybreak = 0;
		sk_msg_iter_var_next(i);
	} while (i != msg->sg.end);
out:
	msg->sg.curr = i;
	return (ret < 0) ? ret : copied;
}
EXPORT_SYMBOL_GPL(sk_msg_memcopy_from_iter);

/* Receive sk_msg from psock->ingress_msg to @msg. */
int sk_msg_recvmsg(struct sock *sk, struct sk_psock *psock, struct msghdr *msg,
		   int len, int flags)
{
	struct iov_iter *iter = &msg->msg_iter;
	int peek = flags & MSG_PEEK;
	struct sk_msg *msg_rx;
	int i, copied = 0;

	msg_rx = sk_psock_peek_msg(psock);
	while (copied != len) {
		struct scatterlist *sge;

		if (unlikely(!msg_rx))
			break;

		i = msg_rx->sg.start;
		do {
			struct page *page;
			int copy;

			sge = sk_msg_elem(msg_rx, i);
			copy = sge->length;
			page = sg_page(sge);
			if (copied + copy > len)
				copy = len - copied;
			if (copy)
				copy = copy_page_to_iter(page, sge->offset, copy, iter);
			if (!copy) {
				copied = copied ? copied : -EFAULT;
				goto out;
			}

			copied += copy;
			if (likely(!peek)) {
				sge->offset += copy;
				sge->length -= copy;
				if (!msg_rx->skb) {
					sk_mem_uncharge(sk, copy);
					atomic_sub(copy, &sk->sk_rmem_alloc);
				}
				msg_rx->sg.size -= copy;

				if (!sge->length) {
					sk_msg_iter_var_next(i);
					if (!msg_rx->skb)
						put_page(page);
				}
			} else {
				/* Lets not optimize peek case if copy_page_to_iter
				 * didn't copy the entire length lets just break.
				 */
				if (copy != sge->length)
					goto out;
				sk_msg_iter_var_next(i);
			}

			if (copied == len)
				break;
		} while ((i != msg_rx->sg.end) && !sg_is_last(sge));

		if (unlikely(peek)) {
			msg_rx = sk_psock_next_msg(psock, msg_rx);
			if (!msg_rx)
				break;
			continue;
		}

		msg_rx->sg.start = i;
		if (!sge->length && (i == msg_rx->sg.end || sg_is_last(sge))) {
			msg_rx = sk_psock_dequeue_msg(psock);
			kfree_sk_msg(msg_rx);
		}
		msg_rx = sk_psock_peek_msg(psock);
	}
out:
	return copied;
}
EXPORT_SYMBOL_GPL(sk_msg_recvmsg);

bool sk_msg_is_readable(struct sock *sk)
{
	struct sk_psock *psock;
	bool empty = true;

	rcu_read_lock();
	psock = sk_psock(sk);
	if (likely(psock))
		empty = list_empty(&psock->ingress_msg);
	rcu_read_unlock();
	return !empty;
}
EXPORT_SYMBOL_GPL(sk_msg_is_readable);

static struct sk_msg *alloc_sk_msg(gfp_t gfp)
{
	struct sk_msg *msg;

	msg = kzalloc(sizeof(*msg), gfp | __GFP_NOWARN);
	if (unlikely(!msg))
		return NULL;
	sg_init_marker(msg->sg.data, NR_MSG_FRAG_IDS);
	return msg;
}

static struct sk_msg *sk_psock_create_ingress_msg(struct sock *sk,
						  struct sk_buff *skb)
{
	if (atomic_read(&sk->sk_rmem_alloc) > sk->sk_rcvbuf)
		return NULL;

	if (!sk_rmem_schedule(sk, skb, skb->truesize))
		return NULL;

	return alloc_sk_msg(GFP_KERNEL);
}

static int sk_psock_skb_ingress_enqueue(struct sk_buff *skb,
					u32 off, u32 len,
					struct sk_psock *psock,
					struct sock *sk,
					struct sk_msg *msg,
					bool take_ref)
{
	int num_sge, copied;

	/* skb_to_sgvec will fail when the total number of fragments in
	 * frag_list and frags exceeds MAX_MSG_FRAGS. For example, the
	 * caller may aggregate multiple skbs.
	 */
	num_sge = skb_to_sgvec(skb, msg->sg.data, off, len);
	if (num_sge < 0) {
		/* skb linearize may fail with ENOMEM, but lets simply try again
		 * later if this happens. Under memory pressure we don't want to
		 * drop the skb. We need to linearize the skb so that the mapping
		 * in skb_to_sgvec can not error.
		 * Note that skb_linearize requires the skb not to be shared.
		 */
		if (skb_linearize(skb))
			return -EAGAIN;

		num_sge = skb_to_sgvec(skb, msg->sg.data, off, len);
		if (unlikely(num_sge < 0))
			return num_sge;
	}

#if IS_ENABLED(CONFIG_BPF_STREAM_PARSER)
	psock->ingress_bytes += len;
#endif
	copied = len;
	msg->sg.start = 0;
	msg->sg.size = copied;
	msg->sg.end = num_sge;
	msg->skb = take_ref ? skb_get(skb) : skb;

	sk_psock_queue_msg(psock, msg);
	sk_psock_data_ready(sk, psock);
	return copied;
}

static int sk_psock_skb_ingress_self(struct sk_psock *psock, struct sk_buff *skb,
				     u32 off, u32 len, bool take_ref);

static int sk_psock_skb_ingress(struct sk_psock *psock, struct sk_buff *skb,
				u32 off, u32 len)
{
	struct sock *sk = psock->sk;
	struct sk_msg *msg;
	int err;

	/* If we are receiving on the same sock skb->sk is already assigned,
	 * skip memory accounting and owner transition seeing it already set
	 * correctly.
	 */
	if (unlikely(skb->sk == sk))
		return sk_psock_skb_ingress_self(psock, skb, off, len, true);
	msg = sk_psock_create_ingress_msg(sk, skb);
	if (!msg)
		return -EAGAIN;

	/* This will transition ownership of the data from the socket where
	 * the BPF program was run initiating the redirect to the socket
	 * we will eventually receive this data on. The data will be released
	 * from skb_consume found in __tcp_bpf_recvmsg() after its been copied
	 * into user buffers.
	 */
	skb_set_owner_r(skb, sk);
	err = sk_psock_skb_ingress_enqueue(skb, off, len, psock, sk, msg, true);
	if (err < 0)
		kfree(msg);
	return err;
}

/* Puts an skb on the ingress queue of the socket already assigned to the
 * skb. In this case we do not need to check memory limits or skb_set_owner_r
 * because the skb is already accounted for here.
 */
static int sk_psock_skb_ingress_self(struct sk_psock *psock, struct sk_buff *skb,
				     u32 off, u32 len, bool take_ref)
{
	struct sk_msg *msg = alloc_sk_msg(GFP_ATOMIC);
	struct sock *sk = psock->sk;
	int err;

	if (unlikely(!msg))
		return -EAGAIN;
	skb_set_owner_r(skb, sk);
	err = sk_psock_skb_ingress_enqueue(skb, off, len, psock, sk, msg, take_ref);
	if (err < 0)
		kfree(msg);
	return err;
}

static int sk_psock_handle_skb(struct sk_psock *psock, struct sk_buff *skb,
			       u32 off, u32 len, bool ingress)
{
	if (!ingress) {
		if (!sock_writeable(psock->sk))
			return -EAGAIN;
		return skb_send_sock(psock->sk, skb, off, len);
	}

	return sk_psock_skb_ingress(psock, skb, off, len);
}

static void sk_psock_skb_state(struct sk_psock *psock,
			       struct sk_psock_work_state *state,
			       int len, int off)
{
	spin_lock_bh(&psock->ingress_lock);
	if (sk_psock_test_state(psock, SK_PSOCK_TX_ENABLED)) {
		state->len = len;
		state->off = off;
	}
	spin_unlock_bh(&psock->ingress_lock);
}

static void sk_psock_backlog(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct sk_psock *psock = container_of(dwork, struct sk_psock, work);
	struct sk_psock_work_state *state = &psock->work_state;
	struct sk_buff *skb = NULL;
	u32 len = 0, off = 0;
	bool ingress;
	int ret;

	/* If sk is quickly removed from the map and then added back, the old
	 * psock should not be scheduled, because there are now two psocks
	 * pointing to the same sk.
	 */
	if (!sk_psock_test_state(psock, SK_PSOCK_TX_ENABLED))
		return;

	/* Increment the psock refcnt to synchronize with close(fd) path in
	 * sock_map_close(), ensuring we wait for backlog thread completion
	 * before sk_socket freed. If refcnt increment fails, it indicates
	 * sock_map_close() completed with sk_socket potentially already freed.
	 */
	if (!sk_psock_get(psock->sk))
		return;
	mutex_lock(&psock->work_mutex);
	while ((skb = skb_peek(&psock->ingress_skb))) {
		len = skb->len;
		off = 0;
		if (skb_bpf_strparser(skb)) {
			struct strp_msg *stm = strp_msg(skb);

			off = stm->offset;
			len = stm->full_len;
		}

		/* Resume processing from previous partial state */
		if (unlikely(state->len)) {
			len = state->len;
			off = state->off;
		}

		ingress = skb_bpf_ingress(skb);
		skb_bpf_redirect_clear(skb);
		do {
			ret = -EIO;
			if (!sock_flag(psock->sk, SOCK_DEAD))
				ret = sk_psock_handle_skb(psock, skb, off,
							  len, ingress);
			if (ret <= 0) {
				if (ret == -EAGAIN) {
					sk_psock_skb_state(psock, state, len, off);
					/* Restore redir info we cleared before */
					skb_bpf_set_redir(skb, psock->sk, ingress);
					/* Delay slightly to prioritize any
					 * other work that might be here.
					 */
					if (sk_psock_test_state(psock, SK_PSOCK_TX_ENABLED))
						schedule_delayed_work(&psock->work, 1);
					goto end;
				}
				/* Hard errors break pipe and stop xmit. */
				sk_psock_report_error(psock, ret ? -ret : EPIPE);
				sk_psock_clear_state(psock, SK_PSOCK_TX_ENABLED);
				goto end;
			}
			off += ret;
			len -= ret;
		} while (len);

		/* The entire skb sent, clear state */
		sk_psock_skb_state(psock, state, 0, 0);
		skb = skb_dequeue(&psock->ingress_skb);
		kfree_skb(skb);
	}
end:
	mutex_unlock(&psock->work_mutex);
	sk_psock_put(psock->sk, psock);
}

struct sk_psock *sk_psock_init(struct sock *sk, int node)
{
	struct sk_psock *psock;
	struct proto *prot;

	write_lock_bh(&sk->sk_callback_lock);

	if (sk_is_inet(sk) && inet_csk_has_ulp(sk)) {
		psock = ERR_PTR(-EINVAL);
		goto out;
	}

	if (sk->sk_user_data) {
		psock = ERR_PTR(-EBUSY);
		goto out;
	}

	psock = kzalloc_node(sizeof(*psock), GFP_ATOMIC | __GFP_NOWARN, node);
	if (!psock) {
		psock = ERR_PTR(-ENOMEM);
		goto out;
	}

	prot = READ_ONCE(sk->sk_prot);
	psock->sk = sk;
	psock->eval = __SK_NONE;
	psock->sk_proto = prot;
	psock->saved_unhash = prot->unhash;
	psock->saved_destroy = prot->destroy;
	psock->saved_close = prot->close;
	psock->saved_write_space = sk->sk_write_space;

	INIT_LIST_HEAD(&psock->link);
	spin_lock_init(&psock->link_lock);

	INIT_DELAYED_WORK(&psock->work, sk_psock_backlog);
	mutex_init(&psock->work_mutex);
	INIT_LIST_HEAD(&psock->ingress_msg);
	spin_lock_init(&psock->ingress_lock);
	skb_queue_head_init(&psock->ingress_skb);

	sk_psock_set_state(psock, SK_PSOCK_TX_ENABLED);
	refcount_set(&psock->refcnt, 1);

	__rcu_assign_sk_user_data_with_flags(sk, psock,
					     SK_USER_DATA_NOCOPY |
					     SK_USER_DATA_PSOCK);
	sock_hold(sk);

out:
	write_unlock_bh(&sk->sk_callback_lock);
	return psock;
}
EXPORT_SYMBOL_GPL(sk_psock_init);

struct sk_psock_link *sk_psock_link_pop(struct sk_psock *psock)
{
	struct sk_psock_link *link;

	spin_lock_bh(&psock->link_lock);
	link = list_first_entry_or_null(&psock->link, struct sk_psock_link,
					list);
	if (link)
		list_del(&link->list);
	spin_unlock_bh(&psock->link_lock);
	return link;
}

static void __sk_psock_purge_ingress_msg(struct sk_psock *psock)
{
	struct sk_msg *msg, *tmp;

	list_for_each_entry_safe(msg, tmp, &psock->ingress_msg, list) {
		list_del(&msg->list);
		if (!msg->skb)
			atomic_sub(msg->sg.size, &psock->sk->sk_rmem_alloc);
		sk_msg_free(psock->sk, msg);
		kfree(msg);
	}
}

static void __sk_psock_zap_ingress(struct sk_psock *psock)
{
	struct sk_buff *skb;

	while ((skb = skb_dequeue(&psock->ingress_skb)) != NULL) {
		skb_bpf_redirect_clear(skb);
		sock_drop(psock->sk, skb);
	}
	__sk_psock_purge_ingress_msg(psock);
}

static void sk_psock_link_destroy(struct sk_psock *psock)
{
	struct sk_psock_link *link, *tmp;

	list_for_each_entry_safe(link, tmp, &psock->link, list) {
		list_del(&link->list);
		sk_psock_free_link(link);
	}
}

void sk_psock_stop(struct sk_psock *psock)
{
	spin_lock_bh(&psock->ingress_lock);
	sk_psock_clear_state(psock, SK_PSOCK_TX_ENABLED);
	sk_psock_cork_free(psock);
	spin_unlock_bh(&psock->ingress_lock);
}

static void sk_psock_done_strp(struct sk_psock *psock);

static void sk_psock_destroy(struct work_struct *work)
{
	struct sk_psock *psock = container_of(to_rcu_work(work),
					      struct sk_psock, rwork);
	/* No sk_callback_lock since already detached. */

	sk_psock_done_strp(psock);

	cancel_delayed_work_sync(&psock->work);
	__sk_psock_zap_ingress(psock);
	mutex_destroy(&psock->work_mutex);

	psock_progs_drop(&psock->progs);

	sk_psock_link_destroy(psock);
	sk_psock_cork_free(psock);

	if (psock->sk_redir)
		sock_put(psock->sk_redir);
	if (psock->sk_pair)
		sock_put(psock->sk_pair);
	sock_put(psock->sk);
	kfree(psock);
}

void sk_psock_drop(struct sock *sk, struct sk_psock *psock)
{
	write_lock_bh(&sk->sk_callback_lock);
	sk_psock_restore_proto(sk, psock);
	rcu_assign_sk_user_data(sk, NULL);
	if (psock->progs.stream_parser)
		sk_psock_stop_strp(sk, psock);
	else if (psock->progs.stream_verdict || psock->progs.skb_verdict)
		sk_psock_stop_verdict(sk, psock);
	write_unlock_bh(&sk->sk_callback_lock);

	sk_psock_stop(psock);

	INIT_RCU_WORK(&psock->rwork, sk_psock_destroy);
	queue_rcu_work(system_wq, &psock->rwork);
}
EXPORT_SYMBOL_GPL(sk_psock_drop);

static int sk_psock_map_verd(int verdict, bool redir)
{
	switch (verdict) {
	case SK_PASS:
		return redir ? __SK_REDIRECT : __SK_PASS;
	case SK_DROP:
	default:
		break;
	}

	return __SK_DROP;
}

int sk_psock_msg_verdict(struct sock *sk, struct sk_psock *psock,
			 struct sk_msg *msg)
{
	struct bpf_prog *prog;
	int ret;

	rcu_read_lock();
	prog = READ_ONCE(psock->progs.msg_parser);
	if (unlikely(!prog)) {
		ret = __SK_PASS;
		goto out;
	}

	sk_msg_compute_data_pointers(msg);
	msg->sk = sk;
	ret = bpf_prog_run_pin_on_cpu(prog, msg);
	ret = sk_psock_map_verd(ret, msg->sk_redir);
	psock->apply_bytes = msg->apply_bytes;
	if (ret == __SK_REDIRECT) {
		if (psock->sk_redir) {
			sock_put(psock->sk_redir);
			psock->sk_redir = NULL;
		}
		if (!msg->sk_redir) {
			ret = __SK_DROP;
			goto out;
		}
		psock->redir_ingress = sk_msg_to_ingress(msg);
		psock->sk_redir = msg->sk_redir;
		sock_hold(psock->sk_redir);
	}
out:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(sk_psock_msg_verdict);

static int sk_psock_skb_redirect(struct sk_psock *from, struct sk_buff *skb)
{
	struct sk_psock *psock_other;
	struct sock *sk_other;

	sk_other = skb_bpf_redirect_fetch(skb);
	/* This error is a buggy BPF program, it returned a redirect
	 * return code, but then didn't set a redirect interface.
	 */
	if (unlikely(!sk_other)) {
		skb_bpf_redirect_clear(skb);
		sock_drop(from->sk, skb);
		return -EIO;
	}
	psock_other = sk_psock(sk_other);
	/* This error indicates the socket is being torn down or had another
	 * error that caused the pipe to break. We can't send a packet on
	 * a socket that is in this state so we drop the skb.
	 */
	if (!psock_other || sock_flag(sk_other, SOCK_DEAD)) {
		skb_bpf_redirect_clear(skb);
		sock_drop(from->sk, skb);
		return -EIO;
	}
	spin_lock_bh(&psock_other->ingress_lock);
	if (!sk_psock_test_state(psock_other, SK_PSOCK_TX_ENABLED)) {
		spin_unlock_bh(&psock_other->ingress_lock);
		skb_bpf_redirect_clear(skb);
		sock_drop(from->sk, skb);
		return -EIO;
	}

	skb_queue_tail(&psock_other->ingress_skb, skb);
	schedule_delayed_work(&psock_other->work, 0);
	spin_unlock_bh(&psock_other->ingress_lock);
	return 0;
}

static void sk_psock_tls_verdict_apply(struct sk_buff *skb,
				       struct sk_psock *from, int verdict)
{
	switch (verdict) {
	case __SK_REDIRECT:
		sk_psock_skb_redirect(from, skb);
		break;
	case __SK_PASS:
	case __SK_DROP:
	default:
		break;
	}
}

int sk_psock_tls_strp_read(struct sk_psock *psock, struct sk_buff *skb)
{
	struct bpf_prog *prog;
	int ret = __SK_PASS;

	rcu_read_lock();
	prog = READ_ONCE(psock->progs.stream_verdict);
	if (likely(prog)) {
		skb->sk = psock->sk;
		skb_dst_drop(skb);
		skb_bpf_redirect_clear(skb);
		ret = bpf_prog_run_pin_on_cpu(prog, skb);
		ret = sk_psock_map_verd(ret, skb_bpf_redirect_fetch(skb));
		skb->sk = NULL;
	}
	sk_psock_tls_verdict_apply(skb, psock, ret);
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(sk_psock_tls_strp_read);

static int sk_psock_verdict_apply(struct sk_psock *psock, struct sk_buff *skb,
				  int verdict)
{
	struct sock *sk_other;
	int err = 0;
	u32 len, off;

	switch (verdict) {
	case __SK_PASS:
		err = -EIO;
		sk_other = psock->sk;
		if (sock_flag(sk_other, SOCK_DEAD) ||
		    !sk_psock_test_state(psock, SK_PSOCK_TX_ENABLED))
			goto out_free;

		skb_bpf_set_ingress(skb);

		/* If the queue is empty then we can submit directly
		 * into the msg queue. If its not empty we have to
		 * queue work otherwise we may get OOO data. Otherwise,
		 * if sk_psock_skb_ingress errors will be handled by
		 * retrying later from workqueue.
		 */
		if (skb_queue_empty(&psock->ingress_skb)) {
			len = skb->len;
			off = 0;
			if (skb_bpf_strparser(skb)) {
				struct strp_msg *stm = strp_msg(skb);

				off = stm->offset;
				len = stm->full_len;
			}
			err = sk_psock_skb_ingress_self(psock, skb, off, len, false);
		}
		if (err < 0) {
			spin_lock_bh(&psock->ingress_lock);
			if (sk_psock_test_state(psock, SK_PSOCK_TX_ENABLED)) {
				skb_queue_tail(&psock->ingress_skb, skb);
				schedule_delayed_work(&psock->work, 0);
				err = 0;
			}
			spin_unlock_bh(&psock->ingress_lock);
			if (err < 0)
				goto out_free;
		}
		break;
	case __SK_REDIRECT:
		tcp_eat_skb(psock->sk, skb);
		err = sk_psock_skb_redirect(psock, skb);
		break;
	case __SK_DROP:
	default:
out_free:
		skb_bpf_redirect_clear(skb);
		tcp_eat_skb(psock->sk, skb);
		sock_drop(psock->sk, skb);
	}

	return err;
}

static void sk_psock_write_space(struct sock *sk)
{
	struct sk_psock *psock;
	void (*write_space)(struct sock *sk) = NULL;

	rcu_read_lock();
	psock = sk_psock(sk);
	if (likely(psock)) {
		if (sk_psock_test_state(psock, SK_PSOCK_TX_ENABLED))
			schedule_delayed_work(&psock->work, 0);
		write_space = psock->saved_write_space;
	}
	rcu_read_unlock();
	if (write_space)
		write_space(sk);
}

#if IS_ENABLED(CONFIG_BPF_STREAM_PARSER)
static void sk_psock_strp_read(struct strparser *strp, struct sk_buff *skb)
{
	struct sk_psock *psock;
	struct bpf_prog *prog;
	int ret = __SK_DROP;
	struct sock *sk;

	rcu_read_lock();
	sk = strp->sk;
	psock = sk_psock(sk);
	if (unlikely(!psock)) {
		sock_drop(sk, skb);
		goto out;
	}
	prog = READ_ONCE(psock->progs.stream_verdict);
	if (likely(prog)) {
		skb->sk = sk;
		skb_dst_drop(skb);
		skb_bpf_redirect_clear(skb);
		ret = bpf_prog_run_pin_on_cpu(prog, skb);
		skb_bpf_set_strparser(skb);
		ret = sk_psock_map_verd(ret, skb_bpf_redirect_fetch(skb));
		skb->sk = NULL;
	}
	sk_psock_verdict_apply(psock, skb, ret);
out:
	rcu_read_unlock();
}

static int sk_psock_strp_read_done(struct strparser *strp, int err)
{
	return err;
}

static int sk_psock_strp_parse(struct strparser *strp, struct sk_buff *skb)
{
	struct sk_psock *psock = container_of(strp, struct sk_psock, strp);
	struct bpf_prog *prog;
	int ret = skb->len;

	rcu_read_lock();
	prog = READ_ONCE(psock->progs.stream_parser);
	if (likely(prog)) {
		skb->sk = psock->sk;
		ret = bpf_prog_run_pin_on_cpu(prog, skb);
		skb->sk = NULL;
	}
	rcu_read_unlock();
	return ret;
}

/* Called with socket lock held. */
static void sk_psock_strp_data_ready(struct sock *sk)
{
	struct sk_psock *psock;

	trace_sk_data_ready(sk);

	rcu_read_lock();
	psock = sk_psock(sk);
	if (likely(psock)) {
		if (tls_sw_has_ctx_rx(sk)) {
			psock->saved_data_ready(sk);
		} else {
			read_lock_bh(&sk->sk_callback_lock);
			strp_data_ready(&psock->strp);
			read_unlock_bh(&sk->sk_callback_lock);
		}
	}
	rcu_read_unlock();
}

int sk_psock_init_strp(struct sock *sk, struct sk_psock *psock)
{
	int ret;

	static const struct strp_callbacks cb = {
		.rcv_msg	= sk_psock_strp_read,
		.read_sock_done	= sk_psock_strp_read_done,
		.parse_msg	= sk_psock_strp_parse,
	};

	ret = strp_init(&psock->strp, sk, &cb);
	if (!ret)
		sk_psock_set_state(psock, SK_PSOCK_RX_STRP_ENABLED);

	if (sk_is_tcp(sk)) {
		psock->strp.cb.read_sock = tcp_bpf_strp_read_sock;
		psock->copied_seq = tcp_sk(sk)->copied_seq;
	}
	return ret;
}

void sk_psock_start_strp(struct sock *sk, struct sk_psock *psock)
{
	if (psock->saved_data_ready)
		return;

	psock->saved_data_ready = sk->sk_data_ready;
	sk->sk_data_ready = sk_psock_strp_data_ready;
	sk->sk_write_space = sk_psock_write_space;
}

void sk_psock_stop_strp(struct sock *sk, struct sk_psock *psock)
{
	psock_set_prog(&psock->progs.stream_parser, NULL);

	if (!psock->saved_data_ready)
		return;

	sk->sk_data_ready = psock->saved_data_ready;
	psock->saved_data_ready = NULL;
	strp_stop(&psock->strp);
}

static void sk_psock_done_strp(struct sk_psock *psock)
{
	/* Parser has been stopped */
	if (sk_psock_test_state(psock, SK_PSOCK_RX_STRP_ENABLED))
		strp_done(&psock->strp);
}
#else
static void sk_psock_done_strp(struct sk_psock *psock)
{
}
#endif /* CONFIG_BPF_STREAM_PARSER */

static int sk_psock_verdict_recv(struct sock *sk, struct sk_buff *skb)
{
	struct sk_psock *psock;
	struct bpf_prog *prog;
	int ret = __SK_DROP;
	int len = skb->len;

	rcu_read_lock();
	psock = sk_psock(sk);
	if (unlikely(!psock)) {
		len = 0;
		tcp_eat_skb(sk, skb);
		sock_drop(sk, skb);
		goto out;
	}
	prog = READ_ONCE(psock->progs.stream_verdict);
	if (!prog)
		prog = READ_ONCE(psock->progs.skb_verdict);
	if (likely(prog)) {
		skb_dst_drop(skb);
		skb_bpf_redirect_clear(skb);
		ret = bpf_prog_run_pin_on_cpu(prog, skb);
		ret = sk_psock_map_verd(ret, skb_bpf_redirect_fetch(skb));
	}
	ret = sk_psock_verdict_apply(psock, skb, ret);
	if (ret < 0)
		len = ret;
out:
	rcu_read_unlock();
	return len;
}

static void sk_psock_verdict_data_ready(struct sock *sk)
{
	struct socket *sock = sk->sk_socket;
	const struct proto_ops *ops;
	int copied;

	trace_sk_data_ready(sk);

	if (unlikely(!sock))
		return;
	ops = READ_ONCE(sock->ops);
	if (!ops || !ops->read_skb)
		return;
	copied = ops->read_skb(sk, sk_psock_verdict_recv);
	if (copied >= 0) {
		struct sk_psock *psock;

		rcu_read_lock();
		psock = sk_psock(sk);
		if (psock)
			sk_psock_data_ready(sk, psock);
		rcu_read_unlock();
	}
}

void sk_psock_start_verdict(struct sock *sk, struct sk_psock *psock)
{
	if (psock->saved_data_ready)
		return;

	psock->saved_data_ready = sk->sk_data_ready;
	sk->sk_data_ready = sk_psock_verdict_data_ready;
	sk->sk_write_space = sk_psock_write_space;
}

void sk_psock_stop_verdict(struct sock *sk, struct sk_psock *psock)
{
	psock_set_prog(&psock->progs.stream_verdict, NULL);
	psock_set_prog(&psock->progs.skb_verdict, NULL);

	if (!psock->saved_data_ready)
		return;

	sk->sk_data_ready = psock->saved_data_ready;
	psock->saved_data_ready = NULL;
}
