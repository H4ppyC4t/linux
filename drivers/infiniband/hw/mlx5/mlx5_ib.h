/* SPDX-License-Identifier: GPL-2.0 OR Linux-OpenIB */
/*
 * Copyright (c) 2013-2020, Mellanox Technologies inc. All rights reserved.
 * Copyright (c) 2020, Intel Corporation. All rights reserved.
 */

#ifndef MLX5_IB_H
#define MLX5_IB_H

#include <linux/kernel.h>
#include <linux/sched.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_smi.h>
#include <linux/mlx5/driver.h>
#include <linux/mlx5/cq.h>
#include <linux/mlx5/fs.h>
#include <linux/mlx5/qp.h>
#include <linux/types.h>
#include <linux/mlx5/transobj.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/mlx5-abi.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/mlx5_user_ioctl_cmds.h>
#include <rdma/mlx5_user_ioctl_verbs.h>

#include "srq.h"
#include "qp.h"
#include "macsec.h"

#define mlx5_ib_dbg(_dev, format, arg...)                                      \
	dev_dbg(&(_dev)->ib_dev.dev, "%s:%d:(pid %d): " format, __func__,      \
		__LINE__, current->pid, ##arg)

#define mlx5_ib_err(_dev, format, arg...)                                      \
	dev_err(&(_dev)->ib_dev.dev, "%s:%d:(pid %d): " format, __func__,      \
		__LINE__, current->pid, ##arg)

#define mlx5_ib_warn(_dev, format, arg...)                                     \
	dev_warn(&(_dev)->ib_dev.dev, "%s:%d:(pid %d): " format, __func__,     \
		 __LINE__, current->pid, ##arg)

#define mlx5_ib_log(lvl, _dev, format, arg...)                                 \
	dev_printk(lvl, &(_dev)->ib_dev.dev,  "%s:%d:(pid %d): " format,       \
		   __func__, __LINE__, current->pid, ##arg)

#define MLX5_IB_DEFAULT_UIDX 0xffffff
#define MLX5_USER_ASSIGNED_UIDX_MASK __mlx5_mask(qpc, user_index)

static __always_inline unsigned long
__mlx5_log_page_size_to_bitmap(unsigned int log_pgsz_bits,
			       unsigned int pgsz_shift)
{
	unsigned int largest_pg_shift =
		min_t(unsigned long, (1ULL << log_pgsz_bits) - 1 + pgsz_shift,
		      BITS_PER_LONG - 1);

	/*
	 * Despite a command allowing it, the device does not support lower than
	 * 4k page size.
	 */
	pgsz_shift = max_t(unsigned int, MLX5_ADAPTER_PAGE_SHIFT, pgsz_shift);
	return GENMASK(largest_pg_shift, pgsz_shift);
}

static __always_inline unsigned long
__mlx5_page_offset_to_bitmask(unsigned int page_offset_bits,
			      unsigned int offset_shift)
{
	unsigned int largest_offset_shift =
		min_t(unsigned long, page_offset_bits - 1 + offset_shift,
		      BITS_PER_LONG - 1);

	return GENMASK(largest_offset_shift, offset_shift);
}

/*
 * QP/CQ/WQ/etc type commands take a page offset that satisifies:
 *   page_offset_quantized * (page_size/scale) = page_offset
 * Which restricts allowed page sizes to ones that satisify the above.
 */
unsigned long __mlx5_umem_find_best_quantized_pgoff(
	struct ib_umem *umem, unsigned long pgsz_bitmap,
	unsigned int page_offset_bits, u64 pgoff_bitmask, unsigned int scale,
	unsigned int *page_offset_quantized);
#define mlx5_umem_find_best_quantized_pgoff(umem, typ, log_pgsz_fld,           \
					    pgsz_shift, page_offset_fld,       \
					    scale, page_offset_quantized)      \
	__mlx5_umem_find_best_quantized_pgoff(                                 \
		umem,                                                          \
		__mlx5_log_page_size_to_bitmap(                                \
			__mlx5_bit_sz(typ, log_pgsz_fld), pgsz_shift),         \
		__mlx5_bit_sz(typ, page_offset_fld),                           \
		GENMASK(31, order_base_2(scale)), scale,                       \
		page_offset_quantized)

#define mlx5_umem_find_best_cq_quantized_pgoff(umem, typ, log_pgsz_fld,        \
					       pgsz_shift, page_offset_fld,    \
					       scale, page_offset_quantized)   \
	__mlx5_umem_find_best_quantized_pgoff(                                 \
		umem,                                                          \
		__mlx5_log_page_size_to_bitmap(                                \
			__mlx5_bit_sz(typ, log_pgsz_fld), pgsz_shift),         \
		__mlx5_bit_sz(typ, page_offset_fld), 0, scale,                 \
		page_offset_quantized)

enum {
	MLX5_IB_MMAP_OFFSET_START = 9,
	MLX5_IB_MMAP_OFFSET_END = 255,
};

enum {
	MLX5_IB_MMAP_CMD_SHIFT	= 8,
	MLX5_IB_MMAP_CMD_MASK	= 0xff,
};

enum {
	MLX5_RES_SCAT_DATA32_CQE	= 0x1,
	MLX5_RES_SCAT_DATA64_CQE	= 0x2,
	MLX5_REQ_SCAT_DATA32_CQE	= 0x11,
	MLX5_REQ_SCAT_DATA64_CQE	= 0x22,
};

enum mlx5_ib_mad_ifc_flags {
	MLX5_MAD_IFC_IGNORE_MKEY	= 1,
	MLX5_MAD_IFC_IGNORE_BKEY	= 2,
	MLX5_MAD_IFC_NET_VIEW		= 4,
};

enum {
	MLX5_CROSS_CHANNEL_BFREG         = 0,
};

enum {
	MLX5_CQE_VERSION_V0,
	MLX5_CQE_VERSION_V1,
};

enum {
	MLX5_TM_MAX_RNDV_MSG_SIZE	= 64,
	MLX5_TM_MAX_SGE			= 1,
};

enum {
	MLX5_IB_INVALID_UAR_INDEX	= BIT(31),
	MLX5_IB_INVALID_BFREG		= BIT(31),
};

enum {
	MLX5_MAX_MEMIC_PAGES = 0x100,
	MLX5_MEMIC_ALLOC_SIZE_MASK = 0x3f,
};

enum {
	MLX5_MEMIC_BASE_ALIGN	= 6,
	MLX5_MEMIC_BASE_SIZE	= 1 << MLX5_MEMIC_BASE_ALIGN,
};

enum mlx5_ib_mmap_type {
	MLX5_IB_MMAP_TYPE_MEMIC = 1,
	MLX5_IB_MMAP_TYPE_VAR = 2,
	MLX5_IB_MMAP_TYPE_UAR_WC = 3,
	MLX5_IB_MMAP_TYPE_UAR_NC = 4,
	MLX5_IB_MMAP_TYPE_MEMIC_OP = 5,
};

struct mlx5_bfreg_info {
	u32 *sys_pages;
	int num_low_latency_bfregs;
	unsigned int *count;

	/*
	 * protect bfreg allocation data structs
	 */
	struct mutex lock;
	u32 ver;
	u8 lib_uar_4k : 1;
	u8 lib_uar_dyn : 1;
	u32 num_sys_pages;
	u32 num_static_sys_pages;
	u32 total_num_bfregs;
	u32 num_dyn_bfregs;
};

struct mlx5_ib_ucontext {
	struct ib_ucontext	ibucontext;
	struct list_head	db_page_list;

	/* protect doorbell record alloc/free
	 */
	struct mutex		db_page_mutex;
	struct mlx5_bfreg_info	bfregi;
	u8			cqe_version;
	/* Transport Domain number */
	u32			tdn;

	u64			lib_caps;
	u16			devx_uid;
	/* For RoCE LAG TX affinity */
	atomic_t		tx_port_affinity;
};

static inline struct mlx5_ib_ucontext *to_mucontext(struct ib_ucontext *ibucontext)
{
	return container_of(ibucontext, struct mlx5_ib_ucontext, ibucontext);
}

struct mlx5_ib_pd {
	struct ib_pd		ibpd;
	u32			pdn;
	u16			uid;
};

enum {
	MLX5_IB_FLOW_ACTION_MODIFY_HEADER,
	MLX5_IB_FLOW_ACTION_PACKET_REFORMAT,
	MLX5_IB_FLOW_ACTION_DECAP,
};

#define MLX5_IB_FLOW_MCAST_PRIO		(MLX5_BY_PASS_NUM_PRIOS - 1)
#define MLX5_IB_FLOW_LAST_PRIO		(MLX5_BY_PASS_NUM_REGULAR_PRIOS - 1)
#if (MLX5_IB_FLOW_LAST_PRIO <= 0)
#error "Invalid number of bypass priorities"
#endif
#define MLX5_IB_FLOW_LEFTOVERS_PRIO	(MLX5_IB_FLOW_MCAST_PRIO + 1)

#define MLX5_IB_NUM_FLOW_FT		(MLX5_IB_FLOW_LEFTOVERS_PRIO + 1)
#define MLX5_IB_NUM_SNIFFER_FTS		2
#define MLX5_IB_NUM_EGRESS_FTS		1
#define MLX5_IB_NUM_FDB_FTS		MLX5_BY_PASS_NUM_REGULAR_PRIOS

struct mlx5_ib_anchor {
	struct mlx5_flow_table *ft;
	struct mlx5_flow_group *fg_goto_table;
	struct mlx5_flow_group *fg_drop;
	struct mlx5_flow_handle *rule_goto_table;
	struct mlx5_flow_handle *rule_drop;
	unsigned int rule_goto_table_ref;
};

struct mlx5_ib_flow_prio {
	struct mlx5_flow_table		*flow_table;
	struct mlx5_ib_anchor		anchor;
	unsigned int			refcount;
};

struct mlx5_ib_flow_handler {
	struct list_head		list;
	struct ib_flow			ibflow;
	struct mlx5_ib_flow_prio	*prio;
	struct mlx5_flow_handle		*rule;
	struct ib_counters		*ibcounters;
	struct mlx5_ib_dev		*dev;
	struct mlx5_ib_flow_matcher	*flow_matcher;
};

struct mlx5_ib_flow_matcher {
	struct mlx5_ib_match_params matcher_mask;
	int			mask_len;
	enum mlx5_ib_flow_type	flow_type;
	enum mlx5_flow_namespace_type ns_type;
	u16			priority;
	struct mlx5_core_dev	*mdev;
	atomic_t		usecnt;
	u8			match_criteria_enable;
	u32			ib_port;
};

struct mlx5_ib_steering_anchor {
	struct mlx5_ib_flow_prio *ft_prio;
	struct mlx5_ib_dev *dev;
	atomic_t usecnt;
};

struct mlx5_ib_pp {
	u16 index;
	struct mlx5_core_dev *mdev;
};

enum mlx5_ib_optional_counter_type {
	MLX5_IB_OPCOUNTER_CC_RX_CE_PKTS,
	MLX5_IB_OPCOUNTER_CC_RX_CNP_PKTS,
	MLX5_IB_OPCOUNTER_CC_TX_CNP_PKTS,
	MLX5_IB_OPCOUNTER_RDMA_TX_PACKETS,
	MLX5_IB_OPCOUNTER_RDMA_TX_BYTES,
	MLX5_IB_OPCOUNTER_RDMA_RX_PACKETS,
	MLX5_IB_OPCOUNTER_RDMA_RX_BYTES,

	MLX5_IB_OPCOUNTER_CC_RX_CE_PKTS_PER_QP,
	MLX5_IB_OPCOUNTER_CC_RX_CNP_PKTS_PER_QP,
	MLX5_IB_OPCOUNTER_CC_TX_CNP_PKTS_PER_QP,
	MLX5_IB_OPCOUNTER_RDMA_TX_PACKETS_PER_QP,
	MLX5_IB_OPCOUNTER_RDMA_TX_BYTES_PER_QP,
	MLX5_IB_OPCOUNTER_RDMA_RX_PACKETS_PER_QP,
	MLX5_IB_OPCOUNTER_RDMA_RX_BYTES_PER_QP,

	MLX5_IB_OPCOUNTER_MAX,
};

struct mlx5_ib_flow_db {
	struct mlx5_ib_flow_prio	prios[MLX5_IB_NUM_FLOW_FT];
	struct mlx5_ib_flow_prio	egress_prios[MLX5_IB_NUM_FLOW_FT];
	struct mlx5_ib_flow_prio	sniffer[MLX5_IB_NUM_SNIFFER_FTS];
	struct mlx5_ib_flow_prio	egress[MLX5_IB_NUM_EGRESS_FTS];
	struct mlx5_ib_flow_prio	fdb[MLX5_IB_NUM_FDB_FTS];
	struct mlx5_ib_flow_prio	rdma_rx[MLX5_IB_NUM_FLOW_FT];
	struct mlx5_ib_flow_prio	rdma_tx[MLX5_IB_NUM_FLOW_FT];
	struct mlx5_ib_flow_prio	opfcs[MLX5_IB_OPCOUNTER_MAX];
	struct mlx5_flow_table		*lag_demux_ft;
	struct mlx5_ib_flow_prio        *rdma_transport_rx[MLX5_RDMA_TRANSPORT_BYPASS_PRIO];
	struct mlx5_ib_flow_prio        *rdma_transport_tx[MLX5_RDMA_TRANSPORT_BYPASS_PRIO];
	/* Protect flow steering bypass flow tables
	 * when add/del flow rules.
	 * only single add/removal of flow steering rule could be done
	 * simultaneously.
	 */
	struct mutex			lock;
};

/* Use macros here so that don't have to duplicate
 * enum ib_qp_type for low-level driver
 */

#define MLX5_IB_QPT_REG_UMR	IB_QPT_RESERVED1
/*
 * IB_QPT_GSI creates the software wrapper around GSI, and MLX5_IB_QPT_HW_GSI
 * creates the actual hardware QP.
 */
#define MLX5_IB_QPT_HW_GSI	IB_QPT_RESERVED2
#define MLX5_IB_QPT_DCI		IB_QPT_RESERVED3
#define MLX5_IB_QPT_DCT		IB_QPT_RESERVED4
#define MLX5_IB_WR_UMR		IB_WR_RESERVED1

#define MLX5_IB_UPD_XLT_ZAP	      BIT(0)
#define MLX5_IB_UPD_XLT_ENABLE	      BIT(1)
#define MLX5_IB_UPD_XLT_ATOMIC	      BIT(2)
#define MLX5_IB_UPD_XLT_ADDR	      BIT(3)
#define MLX5_IB_UPD_XLT_PD	      BIT(4)
#define MLX5_IB_UPD_XLT_ACCESS	      BIT(5)
#define MLX5_IB_UPD_XLT_INDIRECT      BIT(6)
#define MLX5_IB_UPD_XLT_DOWNGRADE     BIT(7)
#define MLX5_IB_UPD_XLT_KEEP_PGSZ     BIT(8)

/* Private QP creation flags to be passed in ib_qp_init_attr.create_flags.
 *
 * These flags are intended for internal use by the mlx5_ib driver, and they
 * rely on the range reserved for that use in the ib_qp_create_flags enum.
 */
#define MLX5_IB_QP_CREATE_SQPN_QP1	IB_QP_CREATE_RESERVED_START

struct wr_list {
	u16	opcode;
	u16	next;
};

enum mlx5_ib_rq_flags {
	MLX5_IB_RQ_CVLAN_STRIPPING	= 1 << 0,
	MLX5_IB_RQ_PCI_WRITE_END_PADDING	= 1 << 1,
};

struct mlx5_ib_wq {
	struct mlx5_frag_buf_ctrl fbc;
	u64		       *wrid;
	u32		       *wr_data;
	struct wr_list	       *w_list;
	unsigned	       *wqe_head;
	u16		        unsig_count;

	/* serialize post to the work queue
	 */
	spinlock_t		lock;
	int			wqe_cnt;
	int			max_post;
	int			max_gs;
	int			offset;
	int			wqe_shift;
	unsigned		head;
	unsigned		tail;
	u16			cur_post;
	u16			last_poll;
	void			*cur_edge;
};

enum mlx5_ib_wq_flags {
	MLX5_IB_WQ_FLAGS_DELAY_DROP = 0x1,
	MLX5_IB_WQ_FLAGS_STRIDING_RQ = 0x2,
};

#define MLX5_MIN_SINGLE_WQE_LOG_NUM_STRIDES 9
#define MLX5_MAX_SINGLE_WQE_LOG_NUM_STRIDES 16
#define MLX5_MIN_SINGLE_STRIDE_LOG_NUM_BYTES 6
#define MLX5_MAX_SINGLE_STRIDE_LOG_NUM_BYTES 13
#define MLX5_EXT_MIN_SINGLE_WQE_LOG_NUM_STRIDES 3

struct mlx5_ib_rwq {
	struct ib_wq		ibwq;
	struct mlx5_core_qp	core_qp;
	u32			rq_num_pas;
	u32			log_rq_stride;
	u32			log_rq_size;
	u32			rq_page_offset;
	u32			log_page_size;
	u32			log_num_strides;
	u32			two_byte_shift_en;
	u32			single_stride_log_num_of_bytes;
	struct ib_umem		*umem;
	size_t			buf_size;
	unsigned int		page_shift;
	struct mlx5_db		db;
	u32			user_index;
	u32			wqe_count;
	u32			wqe_shift;
	int			wq_sig;
	u32			create_flags; /* Use enum mlx5_ib_wq_flags */
};

struct mlx5_ib_rwq_ind_table {
	struct ib_rwq_ind_table ib_rwq_ind_tbl;
	u32			rqtn;
	u16			uid;
};

struct mlx5_ib_ubuffer {
	struct ib_umem	       *umem;
	int			buf_size;
	u64			buf_addr;
};

struct mlx5_ib_qp_base {
	struct mlx5_ib_qp	*container_mibqp;
	struct mlx5_core_qp	mqp;
	struct mlx5_ib_ubuffer	ubuffer;
};

struct mlx5_ib_qp_trans {
	struct mlx5_ib_qp_base	base;
	u16			xrcdn;
	u32			alt_port;
	u8			atomic_rd_en;
	u8			resp_depth;
};

struct mlx5_ib_rss_qp {
	u32	tirn;
};

struct mlx5_ib_rq {
	struct mlx5_ib_qp_base base;
	struct mlx5_ib_wq	*rq;
	struct mlx5_ib_ubuffer	ubuffer;
	struct mlx5_db		*doorbell;
	u32			tirn;
	u8			state;
	u32			flags;
};

struct mlx5_ib_sq {
	struct mlx5_ib_qp_base base;
	struct mlx5_ib_wq	*sq;
	struct mlx5_ib_ubuffer  ubuffer;
	struct mlx5_db		*doorbell;
	struct mlx5_flow_handle	*flow_rule;
	u32			tisn;
	u8			state;
};

struct mlx5_ib_raw_packet_qp {
	struct mlx5_ib_sq sq;
	struct mlx5_ib_rq rq;
};

struct mlx5_bf {
	int			buf_size;
	unsigned long		offset;
	struct mlx5_sq_bfreg   *bfreg;
};

struct mlx5_ib_dct {
	struct mlx5_core_dct    mdct;
	u32                     *in;
};

struct mlx5_ib_gsi_qp {
	struct ib_qp *rx_qp;
	u32 port_num;
	struct ib_qp_cap cap;
	struct ib_cq *cq;
	struct mlx5_ib_gsi_wr *outstanding_wrs;
	u32 outstanding_pi, outstanding_ci;
	int num_qps;
	/* Protects access to the tx_qps. Post send operations synchronize
	 * with tx_qp creation in setup_qp(). Also protects the
	 * outstanding_wrs array and indices.
	 */
	spinlock_t lock;
	struct ib_qp **tx_qps;
};

struct mlx5_ib_qp {
	struct ib_qp		ibqp;
	union {
		struct mlx5_ib_qp_trans trans_qp;
		struct mlx5_ib_raw_packet_qp raw_packet_qp;
		struct mlx5_ib_rss_qp rss_qp;
		struct mlx5_ib_dct dct;
		struct mlx5_ib_gsi_qp gsi;
	};
	struct mlx5_frag_buf	buf;

	struct mlx5_db		db;
	struct mlx5_ib_wq	rq;

	u8			sq_signal_bits;
	u8			next_fence;
	struct mlx5_ib_wq	sq;

	/* serialize qp state modifications
	 */
	struct mutex		mutex;
	/* cached variant of create_flags from struct ib_qp_init_attr */
	u32			flags;
	u32			port;
	u8			state;
	int			max_inline_data;
	struct mlx5_bf	        bf;
	u8			has_rq:1;
	u8			is_rss:1;
	u8			is_ooo_rq:1;

	/* only for user space QPs. For kernel
	 * we have it from the bf object
	 */
	int			bfregn;

	struct list_head	qps_list;
	struct list_head	cq_recv_list;
	struct list_head	cq_send_list;
	struct mlx5_rate_limit	rl;
	u32                     underlay_qpn;
	u32			flags_en;
	/*
	 * IB/core doesn't store low-level QP types, so
	 * store both MLX and IBTA types in the field below.
	 */
	enum ib_qp_type		type;
	/* A flag to indicate if there's a new counter is configured
	 * but not take effective
	 */
	u32                     counter_pending;
	u16			gsi_lag_port;
};

struct mlx5_ib_cq_buf {
	struct mlx5_frag_buf_ctrl fbc;
	struct mlx5_frag_buf    frag_buf;
	struct ib_umem		*umem;
	int			cqe_size;
	int			nent;
};

enum mlx5_ib_cq_pr_flags {
	MLX5_IB_CQ_PR_FLAGS_CQE_128_PAD	= 1 << 0,
	MLX5_IB_CQ_PR_FLAGS_REAL_TIME_TS = 1 << 1,
};

struct mlx5_ib_cq {
	struct ib_cq		ibcq;
	struct mlx5_core_cq	mcq;
	struct mlx5_ib_cq_buf	buf;
	struct mlx5_db		db;

	/* serialize access to the CQ
	 */
	spinlock_t		lock;

	/* protect resize cq
	 */
	struct mutex		resize_mutex;
	struct mlx5_ib_cq_buf  *resize_buf;
	struct ib_umem	       *resize_umem;
	int			cqe_size;
	struct list_head	list_send_qp;
	struct list_head	list_recv_qp;
	u32			create_flags;
	struct list_head	wc_list;
	enum ib_cq_notify_flags notify_flags;
	struct work_struct	notify_work;
	u16			private_flags; /* Use mlx5_ib_cq_pr_flags */
};

struct mlx5_ib_wc {
	struct ib_wc wc;
	struct list_head list;
};

struct mlx5_ib_srq {
	struct ib_srq		ibsrq;
	struct mlx5_core_srq	msrq;
	struct mlx5_frag_buf	buf;
	struct mlx5_db		db;
	struct mlx5_frag_buf_ctrl fbc;
	u64		       *wrid;
	/* protect SRQ hanlding
	 */
	spinlock_t		lock;
	int			head;
	int			tail;
	u16			wqe_ctr;
	struct ib_umem	       *umem;
	/* serialize arming a SRQ
	 */
	struct mutex		mutex;
	int			wq_sig;
};

struct mlx5_ib_xrcd {
	struct ib_xrcd		ibxrcd;
	u32			xrcdn;
};

enum mlx5_ib_mtt_access_flags {
	MLX5_IB_MTT_READ  = (1 << 0),
	MLX5_IB_MTT_WRITE = (1 << 1),
};

struct mlx5_user_mmap_entry {
	struct rdma_user_mmap_entry rdma_entry;
	u8 mmap_flag;
	u64 address;
	u32 page_idx;
};

enum mlx5_mkey_type {
	MLX5_MKEY_MR = 1,
	MLX5_MKEY_MW,
	MLX5_MKEY_INDIRECT_DEVX,
	MLX5_MKEY_NULL,
	MLX5_MKEY_IMPLICIT_CHILD,
};

/* Used for non-existent ph value */
#define MLX5_IB_NO_PH 0xff

struct mlx5r_cache_rb_key {
	u8 ats:1;
	u8 ph;
	u16 st_index;
	unsigned int access_mode;
	unsigned int access_flags;
	unsigned int ndescs;
};

struct mlx5_ib_mkey {
	u32 key;
	enum mlx5_mkey_type type;
	unsigned int ndescs;
	struct wait_queue_head wait;
	refcount_t usecount;
	/* Cacheable user Mkey must hold either a rb_key or a cache_ent. */
	struct mlx5r_cache_rb_key rb_key;
	struct mlx5_cache_ent *cache_ent;
	u8 cacheable : 1;
};

#define MLX5_IB_MTT_PRESENT (MLX5_IB_MTT_READ | MLX5_IB_MTT_WRITE)

#define MLX5_IB_DM_MEMIC_ALLOWED_ACCESS (IB_ACCESS_LOCAL_WRITE   |\
					 IB_ACCESS_REMOTE_WRITE  |\
					 IB_ACCESS_REMOTE_READ   |\
					 IB_ACCESS_REMOTE_ATOMIC |\
					 IB_ZERO_BASED)

#define MLX5_IB_DM_SW_ICM_ALLOWED_ACCESS (IB_ACCESS_LOCAL_WRITE   |\
					  IB_ACCESS_REMOTE_WRITE  |\
					  IB_ACCESS_REMOTE_READ   |\
					  IB_ZERO_BASED)

#define mlx5_update_odp_stats(mr, counter_name, value)		\
	atomic64_add(value, &((mr)->odp_stats.counter_name))

#define mlx5_update_odp_stats_with_handled(mr, counter_name, value)         \
	do {                                                                \
		mlx5_update_odp_stats(mr, counter_name, value);             \
		atomic64_add(1, &((mr)->odp_stats.counter_name##_handled)); \
	} while (0)

struct mlx5_ib_mr {
	struct ib_mr ibmr;
	struct mlx5_ib_mkey mmkey;

	struct ib_umem *umem;
	/* The mr is data direct related */
	u8 data_direct :1;

	union {
		/* Used only by kernel MRs (umem == NULL) */
		struct {
			void *descs;
			void *descs_alloc;
			dma_addr_t desc_map;
			int max_descs;
			int desc_size;
			int access_mode;

			/* For Kernel IB_MR_TYPE_INTEGRITY */
			struct mlx5_core_sig_ctx *sig;
			struct mlx5_ib_mr *pi_mr;
			struct mlx5_ib_mr *klm_mr;
			struct mlx5_ib_mr *mtt_mr;
			u64 data_iova;
			u64 pi_iova;
			int meta_ndescs;
			int meta_length;
			int data_length;
		};

		/* Used only by User MRs (umem != NULL) */
		struct {
			unsigned int page_shift;
			/* Current access_flags */
			int access_flags;

			/* For User ODP */
			struct mlx5_ib_mr *parent;
			struct xarray implicit_children;
			union {
				struct work_struct work;
			} odp_destroy;
			struct ib_odp_counters odp_stats;
			bool is_odp_implicit;
			/* The affilated data direct crossed mr */
			struct mlx5_ib_mr *dd_crossed_mr;
			struct list_head dd_node;
			u8 revoked :1;
			/* Indicates previous dmabuf page fault occurred */
			u8 dmabuf_faulted:1;
			struct mlx5_ib_mkey null_mmkey;
		};
	};
};

static inline bool is_odp_mr(struct mlx5_ib_mr *mr)
{
	return IS_ENABLED(CONFIG_INFINIBAND_ON_DEMAND_PAGING) && mr->umem &&
	       mr->umem->is_odp;
}

static inline bool is_dmabuf_mr(struct mlx5_ib_mr *mr)
{
	return IS_ENABLED(CONFIG_INFINIBAND_ON_DEMAND_PAGING) && mr->umem &&
	       mr->umem->is_dmabuf;
}

struct mlx5_ib_mw {
	struct ib_mw		ibmw;
	struct mlx5_ib_mkey	mmkey;
};

struct mlx5_ib_umr_context {
	struct ib_cqe		cqe;
	enum ib_wc_status	status;
	struct completion	done;
};

enum {
	MLX5_UMR_STATE_UNINIT,
	MLX5_UMR_STATE_ACTIVE,
	MLX5_UMR_STATE_RECOVER,
	MLX5_UMR_STATE_ERR,
};

struct umr_common {
	struct ib_pd	*pd;
	struct ib_cq	*cq;
	struct ib_qp	*qp;
	/* Protects from UMR QP overflow
	 */
	struct semaphore	sem;
	/* Protects from using UMR while the UMR is not active
	 */
	struct mutex lock;
	unsigned int state;
	/* Protects from repeat UMR QP creation */
	struct mutex init_lock;
};

#define NUM_MKEYS_PER_PAGE \
	((PAGE_SIZE - sizeof(struct list_head)) / sizeof(u32))

struct mlx5_mkeys_page {
	u32 mkeys[NUM_MKEYS_PER_PAGE];
	struct list_head list;
};
static_assert(sizeof(struct mlx5_mkeys_page) == PAGE_SIZE);

struct mlx5_mkeys_queue {
	struct list_head pages_list;
	u32 num_pages;
	unsigned long ci;
	spinlock_t lock; /* sync list ops */
};

struct mlx5_cache_ent {
	struct mlx5_mkeys_queue	mkeys_queue;
	u32			pending;

	char                    name[4];

	struct rb_node		node;
	struct mlx5r_cache_rb_key rb_key;

	u8 is_tmp:1;
	u8 disabled:1;
	u8 fill_to_high_water:1;
	u8 tmp_cleanup_scheduled:1;

	/*
	 * - limit is the low water mark for stored mkeys, 2* limit is the
	 *   upper water mark.
	 */
	u32 in_use;
	u32 limit;

	/* Statistics */
	u32                     miss;

	struct mlx5_ib_dev     *dev;
	struct delayed_work	dwork;
};

struct mlx5r_async_create_mkey {
	union {
		u32 in[MLX5_ST_SZ_BYTES(create_mkey_in)];
		u32 out[MLX5_ST_SZ_DW(create_mkey_out)];
	};
	struct mlx5_async_work cb_work;
	struct mlx5_cache_ent *ent;
	u32 mkey;
};

struct mlx5_mkey_cache {
	struct workqueue_struct *wq;
	struct rb_root		rb_root;
	struct mutex		rb_lock;
	struct dentry		*fs_root;
	unsigned long		last_add;
};

struct mlx5_ib_port_resources {
	struct mlx5_ib_gsi_qp *gsi;
	struct work_struct pkey_change_work;
};

struct mlx5_data_direct_resources {
	u32 pdn;
	u32 mkey;
};

struct mlx5_ib_resources {
	struct ib_cq	*c0;
	struct mutex cq_lock;
	u32 xrcdn0;
	u32 xrcdn1;
	struct ib_pd	*p0;
	struct ib_srq	*s0;
	struct ib_srq	*s1;
	struct mutex srq_lock;
	struct mlx5_ib_port_resources ports[2];
};

#define MAX_OPFC_RULES 2

struct mlx5_ib_op_fc {
	struct mlx5_fc *fc;
	struct mlx5_flow_handle *rule[MAX_OPFC_RULES];
};

struct mlx5_ib_counters {
	struct rdma_stat_desc *descs;
	size_t *offsets;
	u32 num_q_counters;
	u32 num_cong_counters;
	u32 num_ext_ppcnt_counters;
	u32 num_op_counters;
	u16 set_id;
	struct mlx5_ib_op_fc opfcs[MLX5_IB_OPCOUNTER_MAX];
};

int mlx5_ib_fs_add_op_fc(struct mlx5_ib_dev *dev, u32 port_num,
			 struct mlx5_ib_op_fc *opfc,
			 enum mlx5_ib_optional_counter_type type);

void mlx5_ib_fs_remove_op_fc(struct mlx5_ib_dev *dev,
			     struct mlx5_ib_op_fc *opfc,
			     enum mlx5_ib_optional_counter_type type);

int mlx5r_fs_bind_op_fc(struct ib_qp *qp,
			struct mlx5_fc *fc_arr[MLX5_IB_OPCOUNTER_MAX],
			struct xarray *qpn_opfc_xa, u32 port);

void mlx5r_fs_unbind_op_fc(struct ib_qp *qp, struct xarray *qpn_opfc_xa);

void mlx5r_fs_destroy_fcs(struct mlx5_ib_dev *dev,
			  struct mlx5_fc *fc_arr[MLX5_IB_OPCOUNTER_MAX]);

struct mlx5_ib_multiport_info;

struct mlx5_ib_multiport {
	struct mlx5_ib_multiport_info *mpi;
	/* To be held when accessing the multiport info */
	spinlock_t mpi_lock;
};

struct mlx5_roce {
	/* Protect mlx5_ib_get_netdev from invoking dev_hold() with a NULL
	 * netdev pointer
	 */
	struct notifier_block	nb;
	struct netdev_net_notifier nn;
	struct notifier_block	mdev_nb;
	struct net_device	*tracking_netdev;
	atomic_t		tx_port_affinity;
	enum ib_port_state last_port_state;
	struct mlx5_ib_dev	*dev;
	u32			native_port_num;
};

struct mlx5_ib_port {
	struct mlx5_ib_counters cnts;
	struct mlx5_ib_multiport mp;
	struct mlx5_ib_dbg_cc_params *dbg_cc_params;
	struct mlx5_roce roce;
	struct mlx5_eswitch_rep		*rep;
#ifdef CONFIG_MLX5_MACSEC
	struct mlx5_reserved_gids *reserved_gids;
#endif
};

struct mlx5_ib_dbg_param {
	int			offset;
	struct mlx5_ib_dev	*dev;
	struct dentry		*dentry;
	u32			port_num;
};

enum mlx5_ib_dbg_cc_types {
	MLX5_IB_DBG_CC_RP_CLAMP_TGT_RATE,
	MLX5_IB_DBG_CC_RP_CLAMP_TGT_RATE_ATI,
	MLX5_IB_DBG_CC_RP_TIME_RESET,
	MLX5_IB_DBG_CC_RP_BYTE_RESET,
	MLX5_IB_DBG_CC_RP_THRESHOLD,
	MLX5_IB_DBG_CC_RP_AI_RATE,
	MLX5_IB_DBG_CC_RP_MAX_RATE,
	MLX5_IB_DBG_CC_RP_HAI_RATE,
	MLX5_IB_DBG_CC_RP_MIN_DEC_FAC,
	MLX5_IB_DBG_CC_RP_MIN_RATE,
	MLX5_IB_DBG_CC_RP_RATE_TO_SET_ON_FIRST_CNP,
	MLX5_IB_DBG_CC_RP_DCE_TCP_G,
	MLX5_IB_DBG_CC_RP_DCE_TCP_RTT,
	MLX5_IB_DBG_CC_RP_RATE_REDUCE_MONITOR_PERIOD,
	MLX5_IB_DBG_CC_RP_INITIAL_ALPHA_VALUE,
	MLX5_IB_DBG_CC_RP_GD,
	MLX5_IB_DBG_CC_NP_MIN_TIME_BETWEEN_CNPS,
	MLX5_IB_DBG_CC_NP_CNP_DSCP,
	MLX5_IB_DBG_CC_NP_CNP_PRIO_MODE,
	MLX5_IB_DBG_CC_NP_CNP_PRIO,
	MLX5_IB_DBG_CC_GENERAL_RTT_RESP_DSCP_VALID,
	MLX5_IB_DBG_CC_GENERAL_RTT_RESP_DSCP,
	MLX5_IB_DBG_CC_MAX,
};

struct mlx5_ib_dbg_cc_params {
	struct dentry			*root;
	struct mlx5_ib_dbg_param	params[MLX5_IB_DBG_CC_MAX];
};

enum {
	MLX5_MAX_DELAY_DROP_TIMEOUT_MS = 100,
};

struct mlx5_ib_delay_drop {
	struct mlx5_ib_dev     *dev;
	struct work_struct	delay_drop_work;
	/* serialize setting of delay drop */
	struct mutex		lock;
	u32			timeout;
	bool			activate;
	atomic_t		events_cnt;
	atomic_t		rqs_cnt;
	struct dentry		*dir_debugfs;
};

enum mlx5_ib_stages {
	MLX5_IB_STAGE_INIT,
	MLX5_IB_STAGE_FS,
	MLX5_IB_STAGE_CAPS,
	MLX5_IB_STAGE_NON_DEFAULT_CB,
	MLX5_IB_STAGE_ROCE,
	MLX5_IB_STAGE_QP,
	MLX5_IB_STAGE_SRQ,
	MLX5_IB_STAGE_DEVICE_RESOURCES,
	MLX5_IB_STAGE_ODP,
	MLX5_IB_STAGE_COUNTERS,
	MLX5_IB_STAGE_CONG_DEBUGFS,
	MLX5_IB_STAGE_BFREG,
	MLX5_IB_STAGE_PRE_IB_REG_UMR,
	MLX5_IB_STAGE_WHITELIST_UID,
	MLX5_IB_STAGE_IB_REG,
	MLX5_IB_STAGE_DEVICE_NOTIFIER,
	MLX5_IB_STAGE_POST_IB_REG_UMR,
	MLX5_IB_STAGE_DELAY_DROP,
	MLX5_IB_STAGE_RESTRACK,
	MLX5_IB_STAGE_MAX,
};

struct mlx5_ib_stage {
	int (*init)(struct mlx5_ib_dev *dev);
	void (*cleanup)(struct mlx5_ib_dev *dev);
};

#define STAGE_CREATE(_stage, _init, _cleanup) \
	.stage[_stage] = {.init = _init, .cleanup = _cleanup}

struct mlx5_ib_profile {
	struct mlx5_ib_stage stage[MLX5_IB_STAGE_MAX];
};

struct mlx5_ib_multiport_info {
	struct list_head list;
	struct mlx5_ib_dev *ibdev;
	struct mlx5_core_dev *mdev;
	struct notifier_block mdev_events;
	struct completion unref_comp;
	u64 sys_image_guid;
	u32 mdev_refcnt;
	bool is_master;
	bool unaffiliate;
};

struct mlx5_ib_flow_action {
	struct ib_flow_action		ib_action;
	union {
		struct {
			u64			    ib_flags;
			struct mlx5_accel_esp_xfrm *ctx;
		} esp_aes_gcm;
		struct {
			struct mlx5_ib_dev *dev;
			u32 sub_type;
			union {
				struct mlx5_modify_hdr *modify_hdr;
				struct mlx5_pkt_reformat *pkt_reformat;
			};
		} flow_action_raw;
	};
};

struct mlx5_dm {
	struct mlx5_core_dev *dev;
	/* This lock is used to protect the access to the shared
	 * allocation map when concurrent requests by different
	 * processes are handled.
	 */
	spinlock_t lock;
	DECLARE_BITMAP(memic_alloc_pages, MLX5_MAX_MEMIC_PAGES);
};

struct mlx5_read_counters_attr {
	struct mlx5_fc *hw_cntrs_hndl;
	u64 *out;
	u32 flags;
};

enum mlx5_ib_counters_type {
	MLX5_IB_COUNTERS_FLOW,
};

struct mlx5_ib_mcounters {
	struct ib_counters ibcntrs;
	enum mlx5_ib_counters_type type;
	/* number of counters supported for this counters type */
	u32 counters_num;
	struct mlx5_fc *hw_cntrs_hndl;
	/* read function for this counters type */
	int (*read_counters)(struct ib_device *ibdev,
			     struct mlx5_read_counters_attr *read_attr);
	/* max index set as part of create_flow */
	u32 cntrs_max_index;
	/* number of counters data entries (<description,index> pair) */
	u32 ncounters;
	/* counters data array for descriptions and indexes */
	struct mlx5_ib_flow_counters_desc *counters_data;
	/* protects access to mcounters internal data */
	struct mutex mcntrs_mutex;
};

static inline struct mlx5_ib_mcounters *
to_mcounters(struct ib_counters *ibcntrs)
{
	return container_of(ibcntrs, struct mlx5_ib_mcounters, ibcntrs);
}

int parse_flow_flow_action(struct mlx5_ib_flow_action *maction,
			   bool is_egress,
			   struct mlx5_flow_act *action);
struct mlx5_ib_lb_state {
	/* protect the user_td */
	struct mutex		mutex;
	u32			user_td;
	int			qps;
	bool			enabled;
};

struct mlx5_ib_pf_eq {
	struct notifier_block irq_nb;
	struct mlx5_ib_dev *dev;
	struct mlx5_eq *core;
	struct work_struct work;
	spinlock_t lock; /* Pagefaults spinlock */
	struct workqueue_struct *wq;
	mempool_t *pool;
};

struct mlx5_devx_event_table {
	struct mlx5_nb devx_nb;
	/* serialize updating the event_xa */
	struct mutex event_xa_lock;
	struct xarray event_xa;
};

struct mlx5_var_table {
	/* serialize updating the bitmap */
	struct mutex bitmap_lock;
	unsigned long *bitmap;
	u64 hw_start_addr;
	u32 stride_size;
	u64 num_var_hw_entries;
};

struct mlx5_port_caps {
	bool has_smi;
	u8 ext_port_cap;
};


struct mlx5_special_mkeys {
	u32 dump_fill_mkey;
	__be32 null_mkey;
	__be32 terminate_scatter_list_mkey;
};

struct mlx5_macsec {
	struct mutex lock; /* Protects mlx5_macsec internal contexts */
	struct list_head macsec_devices_list;
	struct notifier_block blocking_events_nb;
};

struct mlx5_ib_dev {
	struct ib_device		ib_dev;
	struct mlx5_core_dev		*mdev;
	struct mlx5_data_direct_dev	*data_direct_dev;
	/* protect accessing data_direct_dev */
	struct mutex			data_direct_lock;
	struct notifier_block		mdev_events;
	struct notifier_block           lag_events;
	int				num_ports;
	/* serialize update of capability mask
	 */
	struct mutex			cap_mask_mutex;
	u8				ib_active:1;
	u8				is_rep:1;
	u8				lag_active:1;
	u8				fill_delay;
	struct umr_common		umrc;
	/* sync used page count stats
	 */
	struct mlx5_ib_resources	devr;

	atomic_t			mkey_var;
	struct mlx5_mkey_cache		cache;
	struct timer_list		delay_timer;
	/* Prevents soft lock on massive reg MRs */
	struct mutex			slow_path_mutex;
	struct ib_odp_caps	odp_caps;
	u64			odp_max_size;
	struct mutex		odp_eq_mutex;
	struct mlx5_ib_pf_eq	odp_pf_eq;

	struct xarray		odp_mkeys;

	struct mlx5_ib_flow_db	*flow_db;
	/* protect resources needed as part of reset flow */
	spinlock_t		reset_flow_resource_lock;
	struct list_head	qp_list;
	struct list_head data_direct_mr_list;
	/* Array with num_ports elements */
	struct mlx5_ib_port	*port;
	struct mlx5_sq_bfreg	bfreg;
	struct mlx5_sq_bfreg	fp_bfreg;
	struct mlx5_ib_delay_drop	delay_drop;
	const struct mlx5_ib_profile	*profile;

	struct mlx5_ib_lb_state		lb;
	u8			umr_fence;
	struct list_head	ib_dev_list;
	u64			sys_image_guid;
	struct mlx5_dm		dm;
	u16			devx_whitelist_uid;
	struct mlx5_srq_table   srq_table;
	struct mlx5_qp_table    qp_table;
	struct mlx5_async_ctx   async_ctx;
	struct mlx5_devx_event_table devx_event_table;
	struct mlx5_var_table var_table;

	struct xarray sig_mrs;
	struct mlx5_port_caps port_caps[MLX5_MAX_PORTS];
	u16 pkey_table_len;
	u8 lag_ports;
	struct mlx5_special_mkeys mkeys;
	struct mlx5_data_direct_resources ddr;

#ifdef CONFIG_MLX5_MACSEC
	struct mlx5_macsec macsec;
#endif

	u8 num_plane;
	struct mlx5_ib_dev *smi_dev;
	const char *sub_dev_name;
};

static inline struct mlx5_ib_cq *to_mibcq(struct mlx5_core_cq *mcq)
{
	return container_of(mcq, struct mlx5_ib_cq, mcq);
}

static inline struct mlx5_ib_xrcd *to_mxrcd(struct ib_xrcd *ibxrcd)
{
	return container_of(ibxrcd, struct mlx5_ib_xrcd, ibxrcd);
}

static inline struct mlx5_ib_dev *to_mdev(struct ib_device *ibdev)
{
	return container_of(ibdev, struct mlx5_ib_dev, ib_dev);
}

static inline struct mlx5_ib_dev *mr_to_mdev(struct mlx5_ib_mr *mr)
{
	return to_mdev(mr->ibmr.device);
}

static inline struct mlx5_ib_dev *mlx5_udata_to_mdev(struct ib_udata *udata)
{
	struct mlx5_ib_ucontext *context = rdma_udata_to_drv_context(
		udata, struct mlx5_ib_ucontext, ibucontext);

	return to_mdev(context->ibucontext.device);
}

static inline struct mlx5_ib_cq *to_mcq(struct ib_cq *ibcq)
{
	return container_of(ibcq, struct mlx5_ib_cq, ibcq);
}

static inline struct mlx5_ib_qp *to_mibqp(struct mlx5_core_qp *mqp)
{
	return container_of(mqp, struct mlx5_ib_qp_base, mqp)->container_mibqp;
}

static inline struct mlx5_ib_rwq *to_mibrwq(struct mlx5_core_qp *core_qp)
{
	return container_of(core_qp, struct mlx5_ib_rwq, core_qp);
}

static inline struct mlx5_ib_pd *to_mpd(struct ib_pd *ibpd)
{
	return container_of(ibpd, struct mlx5_ib_pd, ibpd);
}

static inline struct mlx5_ib_srq *to_msrq(struct ib_srq *ibsrq)
{
	return container_of(ibsrq, struct mlx5_ib_srq, ibsrq);
}

static inline struct mlx5_ib_qp *to_mqp(struct ib_qp *ibqp)
{
	return container_of(ibqp, struct mlx5_ib_qp, ibqp);
}

static inline struct mlx5_ib_rwq *to_mrwq(struct ib_wq *ibwq)
{
	return container_of(ibwq, struct mlx5_ib_rwq, ibwq);
}

static inline struct mlx5_ib_rwq_ind_table *to_mrwq_ind_table(struct ib_rwq_ind_table *ib_rwq_ind_tbl)
{
	return container_of(ib_rwq_ind_tbl, struct mlx5_ib_rwq_ind_table, ib_rwq_ind_tbl);
}

static inline struct mlx5_ib_srq *to_mibsrq(struct mlx5_core_srq *msrq)
{
	return container_of(msrq, struct mlx5_ib_srq, msrq);
}

static inline struct mlx5_ib_mr *to_mmr(struct ib_mr *ibmr)
{
	return container_of(ibmr, struct mlx5_ib_mr, ibmr);
}

static inline struct mlx5_ib_mw *to_mmw(struct ib_mw *ibmw)
{
	return container_of(ibmw, struct mlx5_ib_mw, ibmw);
}

static inline struct mlx5_ib_flow_action *
to_mflow_act(struct ib_flow_action *ibact)
{
	return container_of(ibact, struct mlx5_ib_flow_action, ib_action);
}

static inline struct mlx5_user_mmap_entry *
to_mmmap(struct rdma_user_mmap_entry *rdma_entry)
{
	return container_of(rdma_entry,
		struct mlx5_user_mmap_entry, rdma_entry);
}

int mlx5_ib_dev_res_cq_init(struct mlx5_ib_dev *dev);
int mlx5_ib_dev_res_srq_init(struct mlx5_ib_dev *dev);
int mlx5_ib_db_map_user(struct mlx5_ib_ucontext *context, unsigned long virt,
			struct mlx5_db *db);
void mlx5_ib_db_unmap_user(struct mlx5_ib_ucontext *context, struct mlx5_db *db);
void __mlx5_ib_cq_clean(struct mlx5_ib_cq *cq, u32 qpn, struct mlx5_ib_srq *srq);
void mlx5_ib_cq_clean(struct mlx5_ib_cq *cq, u32 qpn, struct mlx5_ib_srq *srq);
void mlx5_ib_free_srq_wqe(struct mlx5_ib_srq *srq, int wqe_index);
int mlx5_ib_create_ah(struct ib_ah *ah, struct rdma_ah_init_attr *init_attr,
		      struct ib_udata *udata);
int mlx5_ib_query_ah(struct ib_ah *ibah, struct rdma_ah_attr *ah_attr);
static inline int mlx5_ib_destroy_ah(struct ib_ah *ah, u32 flags)
{
	return 0;
}
int mlx5_ib_create_srq(struct ib_srq *srq, struct ib_srq_init_attr *init_attr,
		       struct ib_udata *udata);
int mlx5_ib_modify_srq(struct ib_srq *ibsrq, struct ib_srq_attr *attr,
		       enum ib_srq_attr_mask attr_mask, struct ib_udata *udata);
int mlx5_ib_query_srq(struct ib_srq *ibsrq, struct ib_srq_attr *srq_attr);
int mlx5_ib_destroy_srq(struct ib_srq *srq, struct ib_udata *udata);
int mlx5_ib_post_srq_recv(struct ib_srq *ibsrq, const struct ib_recv_wr *wr,
			  const struct ib_recv_wr **bad_wr);
int mlx5_ib_enable_lb(struct mlx5_ib_dev *dev, bool td, bool qp);
void mlx5_ib_disable_lb(struct mlx5_ib_dev *dev, bool td, bool qp);
int mlx5_ib_create_qp(struct ib_qp *qp, struct ib_qp_init_attr *init_attr,
		      struct ib_udata *udata);
int mlx5_ib_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		      int attr_mask, struct ib_udata *udata);
int mlx5_ib_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *qp_attr, int qp_attr_mask,
		     struct ib_qp_init_attr *qp_init_attr);
int mlx5_ib_destroy_qp(struct ib_qp *qp, struct ib_udata *udata);
void mlx5_ib_drain_sq(struct ib_qp *qp);
void mlx5_ib_drain_rq(struct ib_qp *qp);
int mlx5_ib_read_wqe_sq(struct mlx5_ib_qp *qp, int wqe_index, void *buffer,
			size_t buflen, size_t *bc);
int mlx5_ib_read_wqe_rq(struct mlx5_ib_qp *qp, int wqe_index, void *buffer,
			size_t buflen, size_t *bc);
int mlx5_ib_read_wqe_srq(struct mlx5_ib_srq *srq, int wqe_index, void *buffer,
			 size_t buflen, size_t *bc);
int mlx5_ib_create_cq(struct ib_cq *ibcq, const struct ib_cq_init_attr *attr,
		      struct uverbs_attr_bundle *attrs);
int mlx5_ib_destroy_cq(struct ib_cq *cq, struct ib_udata *udata);
int mlx5_ib_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc);
int mlx5_ib_pre_destroy_cq(struct ib_cq *cq);
void mlx5_ib_post_destroy_cq(struct ib_cq *cq);
int mlx5_ib_arm_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags flags);
int mlx5_ib_modify_cq(struct ib_cq *cq, u16 cq_count, u16 cq_period);
int mlx5_ib_resize_cq(struct ib_cq *ibcq, int entries, struct ib_udata *udata);
struct ib_mr *mlx5_ib_get_dma_mr(struct ib_pd *pd, int acc);
struct ib_mr *mlx5_ib_reg_user_mr(struct ib_pd *pd, u64 start, u64 length,
				  u64 virt_addr, int access_flags,
				  struct ib_dmah *dmah,
				  struct ib_udata *udata);
struct ib_mr *mlx5_ib_reg_user_mr_dmabuf(struct ib_pd *pd, u64 start,
					 u64 length, u64 virt_addr,
					 int fd, int access_flags,
					 struct ib_dmah *dmah,
					 struct uverbs_attr_bundle *attrs);
int mlx5_ib_advise_mr(struct ib_pd *pd,
		      enum ib_uverbs_advise_mr_advice advice,
		      u32 flags,
		      struct ib_sge *sg_list,
		      u32 num_sge,
		      struct uverbs_attr_bundle *attrs);
int mlx5_ib_alloc_mw(struct ib_mw *mw, struct ib_udata *udata);
int mlx5_ib_dealloc_mw(struct ib_mw *mw);
struct mlx5_ib_mr *mlx5_ib_alloc_implicit_mr(struct mlx5_ib_pd *pd,
					     int access_flags);
void mlx5_ib_free_odp_mr(struct mlx5_ib_mr *mr);
struct ib_mr *mlx5_ib_rereg_user_mr(struct ib_mr *ib_mr, int flags, u64 start,
				    u64 length, u64 virt_addr, int access_flags,
				    struct ib_pd *pd, struct ib_udata *udata);
int mlx5_ib_dereg_mr(struct ib_mr *ibmr, struct ib_udata *udata);
struct ib_mr *mlx5_ib_alloc_mr(struct ib_pd *pd, enum ib_mr_type mr_type,
			       u32 max_num_sg);
struct ib_mr *mlx5_ib_alloc_mr_integrity(struct ib_pd *pd,
					 u32 max_num_sg,
					 u32 max_num_meta_sg);
int mlx5_ib_map_mr_sg(struct ib_mr *ibmr, struct scatterlist *sg, int sg_nents,
		      unsigned int *sg_offset);
int mlx5_ib_map_mr_sg_pi(struct ib_mr *ibmr, struct scatterlist *data_sg,
			 int data_sg_nents, unsigned int *data_sg_offset,
			 struct scatterlist *meta_sg, int meta_sg_nents,
			 unsigned int *meta_sg_offset);
int mlx5_ib_process_mad(struct ib_device *ibdev, int mad_flags, u32 port_num,
			const struct ib_wc *in_wc, const struct ib_grh *in_grh,
			const struct ib_mad *in, struct ib_mad *out,
			size_t *out_mad_size, u16 *out_mad_pkey_index);
int mlx5_ib_alloc_xrcd(struct ib_xrcd *xrcd, struct ib_udata *udata);
int mlx5_ib_dealloc_xrcd(struct ib_xrcd *xrcd, struct ib_udata *udata);
int mlx5_query_ext_port_caps(struct mlx5_ib_dev *dev, unsigned int port);
int mlx5_query_mad_ifc_system_image_guid(struct ib_device *ibdev,
					 __be64 *sys_image_guid);
int mlx5_query_mad_ifc_max_pkeys(struct ib_device *ibdev,
				 u16 *max_pkeys);
int mlx5_query_mad_ifc_vendor_id(struct ib_device *ibdev,
				 u32 *vendor_id);
int mlx5_query_mad_ifc_node_desc(struct mlx5_ib_dev *dev, char *node_desc);
int mlx5_query_mad_ifc_node_guid(struct mlx5_ib_dev *dev, __be64 *node_guid);
int mlx5_query_mad_ifc_pkey(struct ib_device *ibdev, u32 port, u16 index,
			    u16 *pkey);
int mlx5_query_mad_ifc_gids(struct ib_device *ibdev, u32 port, int index,
			    union ib_gid *gid);
int mlx5_query_mad_ifc_port(struct ib_device *ibdev, u32 port,
			    struct ib_port_attr *props);
int mlx5_ib_query_port(struct ib_device *ibdev, u32 port,
		       struct ib_port_attr *props);
void mlx5_ib_populate_pas(struct ib_umem *umem, size_t page_size, __be64 *pas,
			  u64 access_flags);
int mlx5_ib_get_cqe_size(struct ib_cq *ibcq);
int mlx5_mkey_cache_init(struct mlx5_ib_dev *dev);
void mlx5_mkey_cache_cleanup(struct mlx5_ib_dev *dev);
struct mlx5_cache_ent *
mlx5r_cache_create_ent_locked(struct mlx5_ib_dev *dev,
			      struct mlx5r_cache_rb_key rb_key,
			      bool persistent_entry);

struct mlx5_ib_mr *mlx5_mr_cache_alloc(struct mlx5_ib_dev *dev,
				       int access_flags, int access_mode,
				       int ndescs);

int mlx5_ib_check_mr_status(struct ib_mr *ibmr, u32 check_mask,
			    struct ib_mr_status *mr_status);
struct ib_wq *mlx5_ib_create_wq(struct ib_pd *pd,
				struct ib_wq_init_attr *init_attr,
				struct ib_udata *udata);
int mlx5_ib_destroy_wq(struct ib_wq *wq, struct ib_udata *udata);
int mlx5_ib_modify_wq(struct ib_wq *wq, struct ib_wq_attr *wq_attr,
		      u32 wq_attr_mask, struct ib_udata *udata);
int mlx5_ib_create_rwq_ind_table(struct ib_rwq_ind_table *ib_rwq_ind_table,
				 struct ib_rwq_ind_table_init_attr *init_attr,
				 struct ib_udata *udata);
int mlx5_ib_destroy_rwq_ind_table(struct ib_rwq_ind_table *wq_ind_table);
struct ib_mr *mlx5_ib_reg_dm_mr(struct ib_pd *pd, struct ib_dm *dm,
				struct ib_dm_mr_attr *attr,
				struct uverbs_attr_bundle *attrs);
void mlx5_ib_data_direct_bind(struct mlx5_ib_dev *ibdev,
			      struct mlx5_data_direct_dev *dev);
void mlx5_ib_data_direct_unbind(struct mlx5_ib_dev *ibdev);
void mlx5_ib_revoke_data_direct_mrs(struct mlx5_ib_dev *dev);

#ifdef CONFIG_INFINIBAND_ON_DEMAND_PAGING
int mlx5_ib_odp_init_one(struct mlx5_ib_dev *ibdev);
int mlx5r_odp_create_eq(struct mlx5_ib_dev *dev, struct mlx5_ib_pf_eq *eq);
void mlx5_ib_odp_cleanup_one(struct mlx5_ib_dev *ibdev);
int __init mlx5_ib_odp_init(void);
void mlx5_ib_odp_cleanup(void);
int mlx5_odp_init_mkey_cache(struct mlx5_ib_dev *dev);
int mlx5_odp_populate_xlt(void *xlt, size_t idx, size_t nentries,
			  struct mlx5_ib_mr *mr, int flags);

int mlx5_ib_advise_mr_prefetch(struct ib_pd *pd,
			       enum ib_uverbs_advise_mr_advice advice,
			       u32 flags, struct ib_sge *sg_list, u32 num_sge);
int mlx5_ib_init_odp_mr(struct mlx5_ib_mr *mr);
int mlx5_ib_init_dmabuf_mr(struct mlx5_ib_mr *mr);
#else /* CONFIG_INFINIBAND_ON_DEMAND_PAGING */
static inline int mlx5_ib_odp_init_one(struct mlx5_ib_dev *ibdev) { return 0; }
static inline int mlx5r_odp_create_eq(struct mlx5_ib_dev *dev,
				      struct mlx5_ib_pf_eq *eq)
{
	return 0;
}
static inline void mlx5_ib_odp_cleanup_one(struct mlx5_ib_dev *ibdev) {}
static inline int mlx5_ib_odp_init(void) { return 0; }
static inline void mlx5_ib_odp_cleanup(void)				    {}
static inline int mlx5_odp_init_mkey_cache(struct mlx5_ib_dev *dev)
{
	return 0;
}
static inline int mlx5_odp_populate_xlt(void *xlt, size_t idx, size_t nentries,
					struct mlx5_ib_mr *mr, int flags)
{
	return -EOPNOTSUPP;
}

static inline int
mlx5_ib_advise_mr_prefetch(struct ib_pd *pd,
			   enum ib_uverbs_advise_mr_advice advice, u32 flags,
			   struct ib_sge *sg_list, u32 num_sge)
{
	return -EOPNOTSUPP;
}
static inline int mlx5_ib_init_odp_mr(struct mlx5_ib_mr *mr)
{
	return -EOPNOTSUPP;
}
static inline int mlx5_ib_init_dmabuf_mr(struct mlx5_ib_mr *mr)
{
	return -EOPNOTSUPP;
}
#endif /* CONFIG_INFINIBAND_ON_DEMAND_PAGING */

extern const struct mmu_interval_notifier_ops mlx5_mn_ops;

/* Needed for rep profile */
void __mlx5_ib_remove(struct mlx5_ib_dev *dev,
		      const struct mlx5_ib_profile *profile,
		      int stage);
int __mlx5_ib_add(struct mlx5_ib_dev *dev,
		  const struct mlx5_ib_profile *profile);

int mlx5_ib_get_vf_config(struct ib_device *device, int vf,
			  u32 port, struct ifla_vf_info *info);
int mlx5_ib_set_vf_link_state(struct ib_device *device, int vf,
			      u32 port, int state);
int mlx5_ib_get_vf_stats(struct ib_device *device, int vf,
			 u32 port, struct ifla_vf_stats *stats);
int mlx5_ib_get_vf_guid(struct ib_device *device, int vf, u32 port,
			struct ifla_vf_guid *node_guid,
			struct ifla_vf_guid *port_guid);
int mlx5_ib_set_vf_guid(struct ib_device *device, int vf, u32 port,
			u64 guid, int type);

__be16 mlx5_get_roce_udp_sport_min(const struct mlx5_ib_dev *dev,
				   const struct ib_gid_attr *attr);

void mlx5_ib_cleanup_cong_debugfs(struct mlx5_ib_dev *dev, u32 port_num);
void mlx5_ib_init_cong_debugfs(struct mlx5_ib_dev *dev, u32 port_num);

/* GSI QP helper functions */
int mlx5_ib_create_gsi(struct ib_pd *pd, struct mlx5_ib_qp *mqp,
		       struct ib_qp_init_attr *attr);
int mlx5_ib_destroy_gsi(struct mlx5_ib_qp *mqp);
int mlx5_ib_gsi_modify_qp(struct ib_qp *qp, struct ib_qp_attr *attr,
			  int attr_mask);
int mlx5_ib_gsi_query_qp(struct ib_qp *qp, struct ib_qp_attr *qp_attr,
			 int qp_attr_mask,
			 struct ib_qp_init_attr *qp_init_attr);
int mlx5_ib_gsi_post_send(struct ib_qp *qp, const struct ib_send_wr *wr,
			  const struct ib_send_wr **bad_wr);
int mlx5_ib_gsi_post_recv(struct ib_qp *qp, const struct ib_recv_wr *wr,
			  const struct ib_recv_wr **bad_wr);
void mlx5_ib_gsi_pkey_change(struct mlx5_ib_gsi_qp *gsi);

int mlx5_ib_generate_wc(struct ib_cq *ibcq, struct ib_wc *wc);

void mlx5_ib_free_bfreg(struct mlx5_ib_dev *dev, struct mlx5_bfreg_info *bfregi,
			int bfregn);
struct mlx5_ib_dev *mlx5_ib_get_ibdev_from_mpi(struct mlx5_ib_multiport_info *mpi);
struct mlx5_core_dev *mlx5_ib_get_native_port_mdev(struct mlx5_ib_dev *dev,
						   u32 ib_port_num,
						   u32 *native_port_num);
void mlx5_ib_put_native_port_mdev(struct mlx5_ib_dev *dev,
				  u32 port_num);

extern const struct uapi_definition mlx5_ib_devx_defs[];
extern const struct uapi_definition mlx5_ib_flow_defs[];
extern const struct uapi_definition mlx5_ib_qos_defs[];
extern const struct uapi_definition mlx5_ib_std_types_defs[];
extern const struct uapi_definition mlx5_ib_create_cq_defs[];

static inline int is_qp1(enum ib_qp_type qp_type)
{
	return qp_type == MLX5_IB_QPT_HW_GSI || qp_type == IB_QPT_GSI;
}

static inline u32 check_cq_create_flags(u32 flags)
{
	/*
	 * It returns non-zero value for unsupported CQ
	 * create flags, otherwise it returns zero.
	 */
	return (flags & ~(IB_UVERBS_CQ_FLAGS_IGNORE_OVERRUN |
			  IB_UVERBS_CQ_FLAGS_TIMESTAMP_COMPLETION));
}

static inline int verify_assign_uidx(u8 cqe_version, u32 cmd_uidx,
				     u32 *user_index)
{
	if (cqe_version) {
		if ((cmd_uidx == MLX5_IB_DEFAULT_UIDX) ||
		    (cmd_uidx & ~MLX5_USER_ASSIGNED_UIDX_MASK))
			return -EINVAL;
		*user_index = cmd_uidx;
	} else {
		*user_index = MLX5_IB_DEFAULT_UIDX;
	}

	return 0;
}

static inline int get_qp_user_index(struct mlx5_ib_ucontext *ucontext,
				    struct mlx5_ib_create_qp *ucmd,
				    int inlen,
				    u32 *user_index)
{
	u8 cqe_version = ucontext->cqe_version;

	if ((offsetofend(typeof(*ucmd), uidx) <= inlen) && !cqe_version &&
	    (ucmd->uidx == MLX5_IB_DEFAULT_UIDX))
		return 0;

	if ((offsetofend(typeof(*ucmd), uidx) <= inlen) != !!cqe_version)
		return -EINVAL;

	return verify_assign_uidx(cqe_version, ucmd->uidx, user_index);
}

static inline int get_srq_user_index(struct mlx5_ib_ucontext *ucontext,
				     struct mlx5_ib_create_srq *ucmd,
				     int inlen,
				     u32 *user_index)
{
	u8 cqe_version = ucontext->cqe_version;

	if ((offsetofend(typeof(*ucmd), uidx) <= inlen) && !cqe_version &&
	    (ucmd->uidx == MLX5_IB_DEFAULT_UIDX))
		return 0;

	if ((offsetofend(typeof(*ucmd), uidx) <= inlen) != !!cqe_version)
		return -EINVAL;

	return verify_assign_uidx(cqe_version, ucmd->uidx, user_index);
}

static inline int get_uars_per_sys_page(struct mlx5_ib_dev *dev, bool lib_support)
{
	return lib_support && MLX5_CAP_GEN(dev->mdev, uar_4k) ?
				MLX5_UARS_IN_PAGE : 1;
}

extern void *xlt_emergency_page;

int bfregn_to_uar_index(struct mlx5_ib_dev *dev,
			struct mlx5_bfreg_info *bfregi, u32 bfregn,
			bool dyn_bfreg);

static inline int mlx5r_store_odp_mkey(struct mlx5_ib_dev *dev,
				       struct mlx5_ib_mkey *mmkey)
{
	refcount_set(&mmkey->usecount, 1);

	return xa_err(xa_store(&dev->odp_mkeys, mlx5_base_mkey(mmkey->key),
			       mmkey, GFP_KERNEL));
}

/* deref an mkey that can participate in ODP flow */
static inline void mlx5r_deref_odp_mkey(struct mlx5_ib_mkey *mmkey)
{
	if (refcount_dec_and_test(&mmkey->usecount))
		wake_up(&mmkey->wait);
}

/* deref an mkey that can participate in ODP flow and wait for relese */
static inline void mlx5r_deref_wait_odp_mkey(struct mlx5_ib_mkey *mmkey)
{
	mlx5r_deref_odp_mkey(mmkey);
	wait_event(mmkey->wait, refcount_read(&mmkey->usecount) == 0);
}

static inline bool mlx5_ib_lag_should_assign_affinity(struct mlx5_ib_dev *dev)
{
	/*
	 * If the driver is in hash mode and the port_select_flow_table_bypass cap
	 * is supported, it means that the driver no longer needs to assign the port
	 * affinity by default. If a user wants to set the port affinity explicitly,
	 * the user has a dedicated API to do that, so there is no need to assign
	 * the port affinity by default.
	 */
	if (dev->lag_active &&
	    mlx5_lag_mode_is_hash(dev->mdev) &&
	    MLX5_CAP_PORT_SELECTION(dev->mdev, port_select_flow_table_bypass))
		return 0;

	if (mlx5_lag_is_lacp_owner(dev->mdev) && !dev->lag_active)
		return 0;

	return dev->lag_active ||
		(MLX5_CAP_GEN(dev->mdev, num_lag_ports) > 1 &&
		 MLX5_CAP_GEN(dev->mdev, lag_tx_port_affinity));
}

static inline bool rt_supported(int ts_cap)
{
	return ts_cap == MLX5_TIMESTAMP_FORMAT_CAP_REAL_TIME ||
	       ts_cap == MLX5_TIMESTAMP_FORMAT_CAP_FREE_RUNNING_AND_REAL_TIME;
}

/*
 * PCI Peer to Peer is a trainwreck. If no switch is present then things
 * sometimes work, depending on the pci_distance_p2p logic for excluding broken
 * root complexes. However if a switch is present in the path, then things get
 * really ugly depending on how the switch is setup. This table assumes that the
 * root complex is strict and is validating that all req/reps are matches
 * perfectly - so any scenario where it sees only half the transaction is a
 * failure.
 *
 * CR/RR/DT  ATS RO P2P
 * 00X       X   X  OK
 * 010       X   X  fails (request is routed to root but root never sees comp)
 * 011       0   X  fails (request is routed to root but root never sees comp)
 * 011       1   X  OK
 * 10X       X   1  OK
 * 101       X   0  fails (completion is routed to root but root didn't see req)
 * 110       X   0  SLOW
 * 111       0   0  SLOW
 * 111       1   0  fails (completion is routed to root but root didn't see req)
 * 111       1   1  OK
 *
 * Unfortunately we cannot reliably know if a switch is present or what the
 * CR/RR/DT ACS settings are, as in a VM that is all hidden. Assume that
 * CR/RR/DT is 111 if the ATS cap is enabled and follow the last three rows.
 *
 * For now assume if the umem is a dma_buf then it is P2P.
 */
static inline bool mlx5_umem_needs_ats(struct mlx5_ib_dev *dev,
				       struct ib_umem *umem, int access_flags)
{
	if (!MLX5_CAP_GEN(dev->mdev, ats) || !umem->is_dmabuf)
		return false;
	return access_flags & IB_ACCESS_RELAXED_ORDERING;
}

int set_roce_addr(struct mlx5_ib_dev *dev, u32 port_num,
		  unsigned int index, const union ib_gid *gid,
		  const struct ib_gid_attr *attr);

static inline u32 smi_to_native_portnum(struct mlx5_ib_dev *dev, u32 port)
{
	return (port - 1) / dev->num_ports + 1;
}

static inline unsigned int get_max_log_entity_size_cap(struct mlx5_ib_dev *dev,
						       int access_mode)
{
	int max_log_size = 0;

	if (access_mode == MLX5_MKC_ACCESS_MODE_MTT)
		max_log_size =
			MLX5_CAP_GEN_2(dev->mdev, max_mkey_log_entity_size_mtt);
	else if (access_mode == MLX5_MKC_ACCESS_MODE_KSM)
		max_log_size = MLX5_CAP_GEN_2(
			dev->mdev, max_mkey_log_entity_size_fixed_buffer);

	if (!max_log_size ||
	    (max_log_size > 31 &&
	     !MLX5_CAP_GEN_2(dev->mdev, umr_log_entity_size_5)))
		max_log_size = 31;

	return max_log_size;
}

static inline unsigned int get_min_log_entity_size_cap(struct mlx5_ib_dev *dev,
						       int access_mode)
{
	int min_log_size = 0;

	if (access_mode == MLX5_MKC_ACCESS_MODE_KSM &&
	    MLX5_CAP_GEN_2(dev->mdev,
			   min_mkey_log_entity_size_fixed_buffer_valid))
		min_log_size = MLX5_CAP_GEN_2(
			dev->mdev, min_mkey_log_entity_size_fixed_buffer);
	else
		min_log_size =
			MLX5_CAP_GEN_2(dev->mdev, log_min_mkey_entity_size);

	min_log_size = max(min_log_size, MLX5_ADAPTER_PAGE_SHIFT);
	return min_log_size;
}

/*
 * For mkc users, instead of a page_offset the command has a start_iova which
 * specifies both the page_offset and the on-the-wire IOVA
 */
static __always_inline unsigned long
mlx5_umem_mkc_find_best_pgsz(struct mlx5_ib_dev *dev, struct ib_umem *umem,
			     u64 iova, int access_mode)
{
	unsigned int max_log_entity_size_cap, min_log_entity_size_cap;
	unsigned long bitmap;

	max_log_entity_size_cap = get_max_log_entity_size_cap(dev, access_mode);
	min_log_entity_size_cap = get_min_log_entity_size_cap(dev, access_mode);

	bitmap = GENMASK_ULL(max_log_entity_size_cap, min_log_entity_size_cap);

	return ib_umem_find_best_pgsz(umem, bitmap, iova);
}

static inline unsigned long
mlx5_umem_dmabuf_find_best_pgsz(struct ib_umem_dmabuf *umem_dmabuf,
				int access_mode)
{
	return mlx5_umem_mkc_find_best_pgsz(to_mdev(umem_dmabuf->umem.ibdev),
					    &umem_dmabuf->umem,
					    umem_dmabuf->umem.iova,
					    access_mode);
}

#endif /* MLX5_IB_H */
