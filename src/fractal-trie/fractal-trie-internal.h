// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef _URCU_FT_INTERNAL_H
#define _URCU_FT_INTERNAL_H

/*
 * This include is for FT_DT_ARMED alone.  fractal-trie-alloc.c includes only
 * this header.  The forget hook of the non-owner-clear detector lives in that
 * file.  Without a shared gate, an armed build would compile the forget out
 * of the allocator.  Every recycled address would then read as a stolen lock.
 */
#include "ft-dt-arm.h"

/*
 * src/fractal-trie-internal.h
 *
 * Userspace RCU library - Fractal Trie Internal Header
 */

/*
 * ============================ Build options ============================
 *
 * Compile-time toggles for the Fractal Trie.  This is the index; the
 * rationale and exact semantics of each option live at its definition
 * site further down (or in the noted file).  None of these change the
 * public API or the on-the-wire contract -- they trade implementation
 * features, memory layout, or diagnostics.
 *
 * Functional features (enabled by default; disable for a simpler trie):
 *
 *   FEATURE_FT_COMPRESS         Prefix (path) compression of single-child
 *                               chains.  Disable with -DNO_FEATURE_FT_COMPRESS.
 *   FEATURE_FT_SKIP_COMPRESSED  Pack the skip length + child pointer into
 *                               the parent slot's high bits so a descent
 *                               bypasses the compressed cache line.  Auto
 *                               on 64-bit arches that define FT_SKIP_LEN_BITS
 *                               and pass the runtime VA probe; requires
 *                               FEATURE_FT_COMPRESS.
 *                               Disable with -DNO_FEATURE_FT_SKIP_COMPRESSED.
 *   FEATURE_INLINE_LOOKUP       Force-inline the lookup hot path (no call
 *                               boundaries on descent).
 *                               Disable with -DNO_FEATURE_INLINE_LOOKUP.
 *   FEATURE_FT_INSERT_IN_PLACE  Point inserts append into the live node under
 *                               the held lock instead of recompacting it.
 *                               Disable with -DNO_FEATURE_FT_INSERT_IN_PLACE.
 *   FEATURE_FT_DELETE_IN_PLACE  Point deletes soft-delete from the live node
 *                               under the held lock instead of recompacting.
 *                               Disable with -DNO_FEATURE_FT_DELETE_IN_PLACE.
 *
 * (The library-owned ordered-cell index is always compiled in; it is
 * gated per group at runtime via cds_ft_group_attr_set_ordered_list,
 * not at build time.)
 *
 * Memory layout (architecture-defaulted; override to force):
 *
 *   FT_NEAR_METADATA            page_size-range item/metadata striding;
 *                               default on 64-bit.
 *                               Force selection with -DFT_NEAR_METADATA.
 *   FT_FAR_METADATA             2 MiB macro-range layout; default on 32-bit.
 *                               Force selection with -DFT_FAR_METADATA.
 *
 * Validation and debugging (disabled by default; testing only, non-trivial
 * overhead):
 *
 *   FEATURE_FT_VERIFY_AT_MUTATION  cds_ft_verify the whole trie at the exit
 *                                  of every public write.
 *                                  Enable with -DFEATURE_FT_VERIFY_AT_MUTATION.
 *   FEATURE_FT_EXCL_VALIDATE       Runtime check of the writer/reader
 *                                  access-discipline contract.
 *                                  Enable with -DFEATURE_FT_EXCL_VALIDATE.
 *   DEBUG_COUNTERS                 Group-scoped node / cell alloc balance
 *                                  accounting.
 *                                  Enable with -DDEBUG_COUNTERS.
 *   DEBUG / DEBUG_CLEAR_ITER       Verbose dbg_printf / poison recycled
 *                                  iterator state.
 *                                  Enable with -DDEBUG, -DDEBUG_CLEAR_ITER.
 *
 * Diagnostics and tracing:
 *
 *   CDS_FT_SUPPRESS_ISA_WARNING    Silence the x86 -mpopcnt / -mbmi build
 *                                  #warnings.
 *                                  Enable with -DCDS_FT_SUPPRESS_ISA_WARNING.
 *   FT_ENABLE_TRACING              Emit LTTng-UST tracepoints from the read /
 *                                  mutation paths (see fractal-trie.c).
 *                                  Enable with -DFT_ENABLE_TRACING.
 * =======================================================================
 */

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <unistd.h>
#include <urcu/list.h>
/*
 * FT parks a concurrent MCAS proxy under its own pointer tag: type 7, low
 * nibble 0xF (see FT_FLIP_PROXY_TAG in ft-helpers.h).  The proxy is a
 * struct urcu_txn_record pointer.  It is 16-byte aligned, so its low 4 bits
 * are free.
 *
 * FT does not use the engine's default tag, bit 0.  Here bit 0 alone marks an
 * internal node, so a bit-0 proxy would alias one.  No real node, NULL or
 * skip pointer can carry a 0xF low nibble.
 *
 * The tag is now carried per record:
 *   - FT passes FT_FLIP_PROXY_TAG to every urcu_txn_store()/load_validate()
 *     (see ft_flip_txn_commit).
 *   - The engine stores it in urcu_txn_record.proxy_tag.
 *   - The engine forms the parked proxy as (record | proxy_tag).
 *   - The read hot paths recognise it with ft_node_flip_proxy().  This is
 *     the same low-nibble test they already run.
 *
 * No per-TU macro override of the engine is needed.
 */
/*
 * FT is a flavor-agnostic library.  It brackets its RCU read-side sections
 * through the group's RCU flavor, which is bound at runtime.
 *
 * An op binds that flavor (ft_txn_op_init).  ft_op_begin() / ft_op_end()
 * then open the section in that flavor directly (doc §11: the FT owns the
 * bracket).  An exclusive trie binds no flavor: it has no reader to defend,
 * and its frees are synchronous.
 *
 * The engine handle binds no flavor either.  So urcu_txn_begin() /
 * urcu_txn_end() fall back to the URCU_TXN_RCU_READ_LOCK macros.  These
 * macros are overridden to no-ops here, so that:
 *   - the engine opens no section of its own;
 *   - no compile-time flavor's rcu_read_lock symbol is bound into the
 *     flavor-agnostic build.
 */
#define URCU_TXN_RCU_READ_LOCK()	do { } while (0)
#define URCU_TXN_RCU_READ_UNLOCK()	do { } while (0)
#include <urcu/fair-mutex.h>	/* MW coarse lock-mode FT-wide writer lock */
#include <urcu/rcu-txn.h>
#include <urcu/rcu-txn-list.h>
#include "ft-txn-engine.h"	/* FT's view of the parked-slot format */

/*
 * The escalation lane of a trie.  It is a FIFO fair mutex.  A starving op
 * queues on it, so a writer that keeps losing makes progress.
 *
 * An op that earned the lane (it aged past its budget) raises @active.
 * @active tells every other op to queue behind that op too.  This lasts for
 * the length of that episode.
 */
struct ft_lane {
	struct cds_fair_mutex lock;
	unsigned long active;
};

/*
 * The FT op handle.  There is one per operation.  It spans the retry loop of
 * the operation.
 *
 * @txn is the handle of the commit engine.  Each attempt opens and closes it
 * (ft_op_begin / ft_op_end).  A flip txn built on the op
 * (ft_flip_txn_create_on) records into it.
 *
 * Everything else here is FT policy:
 *   - the lane;
 *   - the retry age that earns the lane;
 *   - the turn the op holds.
 * It lives with FT and not with one engine.  The SW engine
 * (<urcu/rcu-txn-sw.h>) has no lane.
 */
struct ft_op {
	struct ft_txn txn;		/* its engine handle has no domain */
	struct ft_lane *lane;		/* NULL: never escalates (exclusive trie) */
	const struct rcu_flavor_struct *flavor;	/* for the quiescent park */
	unsigned long retry;		/* attempts lost so far */
	struct cds_fair_mutex_node waiter;	/* our node while awaiting the turn */
	int in_lane;			/* we hold the lane */
	int lane_published;		/* we raised lane->active and owe the clear */
	int park_quiescent;		/* park OFFLINE (ft_op_set_park_quiescent) */
	int retrying;			/* a COMMIT aborted: keep the turn */
};

/*
 * Depth of the escalation lane THIS THREAD holds (0 = none).  It exists for
 * one invariant: never wait for a grace period while holding the lane (see
 * ft_writer_lock_gp_wait).
 */
extern __thread unsigned int ft_op_lane_depth;

static inline
int ft_op_in_lane(void)
{
	return ft_op_lane_depth != 0;
}
#include <urcu/rculfhash.h>
#include <urcu/arch.h>
#include <urcu/assert.h>	/* urcu_assert_debug: the engine self-checks' arm */
#include <urcu/call-rcu.h>
#include <urcu/uatomic.h>
#include <urcu/fractal-trie.h>

#include <assert.h>

/*
 * FT-wide-lock DROP is the DEFAULT for a FINE-locking trie (§11 rollout,
 * 2026-07-19).  A FINE trie skips the FT-wide writer mutex and relies solely
 * on its per-node node lock-sets + MCAS arbitration for writer exclusion --
 * so disjoint writers run in parallel instead of serialising on one mutex
 * (doc/design/ft-wide-lock-drop-mechanics.md; certified by the §11.4 point-op
 * 1600/1600 gate and the cross-trie oracles).  COARSE tries are unaffected
 * (COARSE keeps the mutex).
 *
 * This is now UNCONDITIONAL.  The drop was the only shipping behaviour.  Its
 * opt-out (-DFEATURE_FT_MW_LOCK_FINE_KEEP) had no gate config, so the retained
 * mutex was untested by construction.  Both flags are gone.  The group's
 * writer strategy decides at RUNTIME: FINE always drops the FT-wide mutex,
 * and COARSE always keeps it.
 */
/*
 * Internal sentinel.  ft_key_len() returns it when it cannot resolve a
 * key-length argument (e.g. CDS_FT_LEN_DEFAULT passed to a variable-length
 * group).  It is numerically equal to the public CDS_FT_LEN_DEFAULT /
 * CDS_FT_LEN_VARIABLE sentinels.  It differs by its role: it is an internal
 * resolution *result*, not an argument supplied by the caller.
 */
#define CDS_FT_LEN_ERROR	SIZE_MAX

/*
 * If the internal bit is set in a pointer, the pointer refers to an internal
 * Fractal Trie node.  Otherwise it refers to a node outside of the Fractal
 * Trie.  This can be used for variable length keys to identify the end of
 * key.
 */
/*
 * Pointer tag encoding (bits 0-1):
 *
 *   (ptr & 0b011) == 0b00   ->  external node (leaf) or NULL
 *   (ptr & 0b001) == 0b001  ->  internal node (bit 0 set), bits 1-3 = type index
 *   (ptr & 0b011) == 0b010  ->  compressed path node
 *
 * Internal nodes always have bit 0 set; the type index encoding lives
 * in bits 1-3.  Compressed nodes use bit 1 with bit 0 clear (16-byte
 * alignment).  External nodes have bits 0-1 clear (8-byte aligned).
 */
#define FT_INTERNAL_BITS	1
#define FT_INTERNAL_MASK	(1U << 0)
#define FT_COMPRESSED_MASK	(1U << 1)
#define FT_TAG_MASK		(FT_COMPRESSED_MASK | FT_INTERNAL_MASK)	/* 0b011 -- for compressed ptr unmasking */

/*
 * This is followed by a number of bits reserved to represent the child
 * type.
 */
#define FT_TYPE_BITS	3
#define FT_TYPE_MAX_NR	(1UL << FT_TYPE_BITS)
#define FT_TYPE_MASK	((FT_TYPE_MAX_NR - 1) << FT_INTERNAL_BITS)
#define FT_PTR_MASK	(~(FT_TYPE_MASK | FT_INTERNAL_MASK))

/*
 * Internal group flag: skip-compressed pointer encoding.
 *
 * Bit set in struct cds_ft_group::flags when the SPECULATIVE lookup
 * optimization (default; cds_ft_group_attr_set_lookup_optimization)
 * is selected AND the arch supports skip-encoded pointers.  Not
 * part of the public API.
 */
#define CDS_FT_FLAG_SKIP_COMPRESSED	(1U << 0)

/*
 * Skip-compressed pointer encoding.
 *
 * When CDS_FT_FLAG_SKIP_COMPRESSED is set, "skip pointers" replace
 * compressed node pointers.  A skip pointer points directly to the
 * compressed node's child.  The read fast path (candidate lookup) thus
 * skips the compressed node.
 *
 * Encoding: the high bits of a pointer (starting at FT_SKIP_LEN_SHIFT)
 * store the compressed path length.  These bits must be zero in normal
 * userspace pointers.  A non-zero value identifies a skip pointer.
 * The number of available bits (FT_SKIP_LEN_BITS) and thus the
 * maximum encodable path length (FT_SKIP_LEN_MAX) are architecture-
 * dependent.  Compressed paths longer than FT_SKIP_LEN_MAX keep the
 * traditional compressed node pointer (no skip optimization).
 *
 * Per-architecture parameters:
 *   x86-64:    shift=57, bits=7, max path=127  (LA57: 57-bit VA)
 *   AArch64:   shift=56, bits=8, max path=255  (LVA: 52-bit VA)
 *   PPC64:     shift=56, bits=8, max path=255  (Radix: 52-bit VA)
 *   riscv64:   shift=56, bits=8, max path=255  (Sv57: 57-bit VA)
 *   MIPS64:    shift=56, bits=8, max path=255  (48-bit VA)
 *   LoongArch: shift=56, bits=8, max path=255  (48-bit VA)
 *
 * New architectures can be added by defining FT_SKIP_LEN_SHIFT and
 * FT_SKIP_LEN_BITS below, enabling FEATURE_FT_SKIP_COMPRESSED in
 * the architecture gate, and verifying that userspace pointers have
 * the selected bits clear.  Architectures with fewer available high
 * bits can still benefit from skip-compressed with a smaller
 * FT_SKIP_LEN_BITS; the fallback to traditional compressed pointers
 * handles longer paths transparently.
 *
 * The compressed node remains allocated.  Key bytes, inequality lookup
 * and exact lookup need it.  It is accessible via the child node's
 * metadata->parent pointer, or via cds_ft_node.prev for the head of an
 * external duplicate chain.
 *
 * Dual-pointer RCU publication:
 *
 * A skip pointer and cn->child are two views of the same child
 * pointer.  When cn->child is replaced (recompact, attach), both
 * must be updated.  The update order is:
 *
 *   1. rcu_assign_pointer(*skip_slot, new_skip_ptr)
 *   2. rcu_assign_pointer(cn->child, new_child)  [or *parent_slot]
 *
 * Skip pointer first ensures candidate readers (which follow the
 * skip pointer) immediately see the new child.  Exact and
 * inequality readers (which follow cn->child via the compressed
 * handler) see the old child until step 2.  The old child remains
 * alive until after a grace period.
 *
 * Between steps 1 and 2, the two reader paths see different but
 * individually consistent tree states (old vs. new subtree).  No
 * reader sees a freed node.  This is the standard RCU guarantee:
 * concurrent readers may observe pre-mutation or post-mutation
 * state, never a mix of both within a single traversal.
 *
 * This ordered two-step publish is for SINGLE-EDGE replacement.  A
 * multi-edge commit that includes this edge (cds_ft_merge_at) switches
 * the slot through the flip latch instead (FT_FLIP_PROXY, see
 * <urcu/rcu-txn-sw.h>):
 *
 *   - the slot transiently holds a flip proxy;
 *   - one commit flips the whole set old->new atomically;
 *   - the skip view is re-established when the flip settles.
 *
 * The read side reads the child's metadata->parent in
 * ft_skip_to_compressed.  The write side writes it in ft_set_parent.
 * The same holds for cds_ft_node.prev, for the head of an external
 * duplicate chain.  Both sides use rcu_dereference / rcu_assign_pointer
 * for proper ordering.
 */

/* Per-architecture skip-length encoding parameters (64-bit only). */
#if defined(URCU_ARCH_AMD64)
# define FT_SKIP_LEN_SHIFT	57	/* Bits 57-63 (7 bits). LA57: 57-bit VA. */
# define FT_SKIP_LEN_BITS	7
#elif defined(URCU_ARCH_AARCH64)
# define FT_SKIP_LEN_SHIFT	56	/* Bits 56-63 (8 bits). LVA: 52-bit VA. */
# define FT_SKIP_LEN_BITS	8
#elif defined(URCU_ARCH_PPC64)
# define FT_SKIP_LEN_SHIFT	56	/* Bits 56-63 (8 bits). Radix: 52-bit VA. */
# define FT_SKIP_LEN_BITS	8
#elif defined(URCU_ARCH_RISCV) && CAA_BITS_PER_LONG >= 64
# define FT_SKIP_LEN_SHIFT	56	/* Bits 56-63 (8 bits). Sv57: 57-bit VA. */
# define FT_SKIP_LEN_BITS	8
#elif defined(URCU_ARCH_MIPS) && CAA_BITS_PER_LONG >= 64
# define FT_SKIP_LEN_SHIFT	56	/* Bits 56-63 (8 bits). 48-bit VA. */
# define FT_SKIP_LEN_BITS	8
#elif defined(URCU_ARCH_LOONGARCH)
# define FT_SKIP_LEN_SHIFT	56	/* Bits 56-63 (8 bits). 48-bit VA. */
# define FT_SKIP_LEN_BITS	8
#endif

/*
 * The longest run ONE compressed node can spell: the limit of its 8-bit len.
 *
 * A run longer than FT_SKIP_LEN_MAX can still be spelled.
 * ft_publish_compressed publishes it through a PLAIN compressed flag.
 * cds_ft_insert builds that shape for a long unique suffix, and
 * cds_ft_verify accepts it.
 *
 * A site that FUSES runs caps the fused length at FT_CN_LEN_MAX, never at
 * FT_SKIP_LEN_MAX.  Declining at FT_SKIP_LEN_MAX leaves in place what the
 * fuse exists to remove: the one-child internal node, or the adjacent
 * compressed nodes.
 *
 * -DFT_DEBUG_OVERLONG_BULK_CAP restores the old cap (red control).
 */
#ifndef FT_DEBUG_OVERLONG_BULK_CAP
# define FT_CN_LEN_MAX		UINT8_MAX
#else
# define FT_CN_LEN_MAX		FT_SKIP_LEN_MAX
#endif

#ifdef FT_SKIP_LEN_BITS
# define FT_SKIP_LEN_MAX	((1U << FT_SKIP_LEN_BITS) - 1)
# define FT_SKIP_LEN_MASK	(((unsigned long) FT_SKIP_LEN_MAX) << FT_SKIP_LEN_SHIFT)
# define FT_ADDR_MASK		((1UL << FT_SKIP_LEN_SHIFT) - 1)
#else
/*
 * Fallback on architectures without skip-compressed support (notably
 * 32-bit).  FEATURE_FT_SKIP_COMPRESSED is also undefined in that
 * case.  Call sites guarded by ft_group_skip_compressed() therefore
 * short-circuit before they evaluate FT_SKIP_LEN_MAX.  The fallback
 * value keeps those expressions type-correct at compile time.  It does
 * not change runtime behavior.
 */
# define FT_SKIP_LEN_MAX	0U
#endif

#define FT_ENTRY_PER_NODE	256
#define FT_MAX_KEY_LEN	256			/* Maximum key length supported. */
#define FT_MAX_DEPTH	(FT_MAX_KEY_LEN + 1)	/* Maximum depth, including root. */

/*
 * Number of bytes of safe over-read past a caller's key_len that the
 * descent SIMD comparator may load.  Library-internal: used to size
 * the iter-allocated key buffer (struct cds_ft_iter) and as the
 * implicit horizon for library-internal key buffers.  Sized for a
 * 32-byte AVX2 load.
 */
#define FT_KEY_READABLE_PAD	32U

/*
 * Entry for NULL node is at index 6 (32-bit) or 7 (64-bit) of the
 * table. It is never encoded in flags.
 */
#if (CAA_BITS_PER_LONG < 64)
# define NODE_INDEX_NULL		6
#else
# define NODE_INDEX_NULL		7
#endif

/*
 * FT_INTERNAL_ORDER_MIN: alloc order of the smallest internal node type
 * (index 0).  Internal-node orders increase by 1 per index.  The
 * ft_types[] table is constructed so that this holds.  The prefetch fast
 * path can thus derive an item's order as
 * (type_index + FT_INTERNAL_ORDER_MIN) without loading ft_types[].
 *
 * Distinct from FT_ALLOC_ORDER_MIN, which is the allocator's minimum
 * order (16 B) used by compressed nodes with short key tails.
 */
#define FT_INTERNAL_ORDER_MIN		5

#define FT_ALLOC_ORDER_MAX		12
#define FT_ALLOC_ORDER_MIN		4	/* Minimum item size: 16 bytes. */

/*
 * Maximum page size order supported per architecture.  Used to size the
 * alloc_index bitfield so that the maximum number of items per range
 * (page_size >> FT_ALLOC_ORDER_MIN) fits without truncation.
 *
 * A runtime check in cds_ft_arena_create() rejects page sizes larger
 * than (1 << FT_MAX_PAGE_ORDER).
 *
 * "Page size" here is the kernel BASE page size (urcu_get_page_len),
 * never a huge page.  Transparent huge pages are orthogonal.  THP only
 * remaps the same frames at a coarser TLB granularity.  It changes
 * neither cds_ft_get_page_size() nor the items-per-range count.
 *
 * The 2 MiB FT_FAR_METADATA layout is a separate compile-time path.  It
 * sizes alloc_index independently and does not use FT_MAX_PAGE_ORDER.
 */
#if defined(URCU_ARCH_AMD64)
# define FT_MAX_PAGE_ORDER	12	/* x86-64: 4 KiB pages. */
#elif defined(URCU_ARCH_AARCH64)
# define FT_MAX_PAGE_ORDER	16	/* ARM64: up to 64 KiB pages. */
#elif defined(URCU_ARCH_PPC64)
# define FT_MAX_PAGE_ORDER	16	/* POWER: up to 64 KiB pages. */
#else
# define FT_MAX_PAGE_ORDER	16	/* Conservative default: 64 KiB. */
#endif

#define FT_ALLOC_INDEX_BITS	(FT_MAX_PAGE_ORDER - FT_ALLOC_ORDER_MIN)

/*
 * Far-metadata is the default internal-arena layout.  Compared with the near
 * (page_size-range) layout, it gives:
 *   - the same lookup throughput;
 *   - a tighter multi-thread tail (fewer outliers);
 *   - marginally lower RSS on large tries.
 * Define FT_NEAR_METADATA to fall back to the original page_size-range layout.
 */
#if !defined(FT_NEAR_METADATA) && !defined(FT_FAR_METADATA)
# if (CAA_BITS_PER_LONG < 64)
/*
 * 32-bit: FT_FAR carves the address space into 2 MiB macro ranges.  It
 * reserves virtual address space in 2 MiB units, even though each range
 * faults in lazily.  That needs too much virtual address space for a ~3 GiB
 * user address space.  Default to the page_size-range near layout there.
 */
#  define FT_NEAR_METADATA
# else
#  define FT_FAR_METADATA
# endif
#endif

#ifdef FT_FAR_METADATA
/*
 * FT_FAR_METADATA: the exact near range layout, but with the page unit scaled
 * from page_size (4 KiB) to a 2 MiB macro.  A range becomes:
 *
 *   [ ITEMS  N x 2^order  @base            ]   (N = 2 MiB >> order, one dense
 *   [ header + METADATA[] @base + 2 MiB    ]    2 MiB-aligned run of node bodies)
 *   [ BITMAP grows backward from base + 4 MiB (bitmap arenas only)         ]
 *
 * So the items are one contiguous 2 MiB run.  By contrast, the default
 * 4 KiB items page is diluted by an interleaved metadata page every 8 KiB.
 *
 * The metadata array starts at the CONSTANT offset base + 2 MiB.  There is
 * no per-order offset table.  The hot-path item->metadata / item->bitmap
 * helpers are the default formulas with page_size replaced by
 * FT_FAR_MACRO_SIZE.
 */
#define FT_FAR_MACRO_ORDER	21			/* 2 MiB macro page. */
#define FT_FAR_MACRO_SIZE	(1UL << FT_FAR_MACRO_ORDER)
#define FT_FAR_MACRO_MASK	(FT_FAR_MACRO_SIZE - 1)
#endif /* FT_FAR_METADATA */

#define FT_BITMAP_LEN			32

/*
 * On x86_64 / i386, warn loudly when popcount or BMI/BMI2 ISA flags
 * are missing from the compile.  Several hot-path inlines silently
 * fall back to software emulation when the compiler isn't told the
 * target supports the native instruction:
 *
 *   - __builtin_popcount* on the popcount-node byte-step.
 *   - bzhi-style masked popcount on the popcount_2l byte-step
 *     (scan_6 / scan_16 / scan_32_8 / scan_64_4).
 *   - Generic codegen on bit-manipulation helpers.
 *
 * The fallbacks cost roughly 10-20% on lookup throughput in our
 * microbenches.  Override with -DCDS_FT_SUPPRESS_ISA_WARNING when
 * intentionally building for a stripped-down target.
 */
#if !defined(CDS_FT_SUPPRESS_ISA_WARNING) && \
		(defined(__x86_64__) || defined(__i386__))
# if !defined(__POPCNT__)
#  warning "Fractal trie: building for x86 without -mpopcnt; __builtin_popcount* will use a software fallback. Expect ~10-20% lookup-throughput regression. Add -mpopcnt or -march=native, or define CDS_FT_SUPPRESS_ISA_WARNING to silence."
# endif
# if !defined(__BMI__)
#  warning "Fractal trie: building for x86 without -mbmi; bit-manipulation helpers will use generic codegen. Add -mbmi or -march=native, or define CDS_FT_SUPPRESS_ISA_WARNING to silence."
# endif
# if !defined(__BMI2__)
#  warning "Fractal trie: building for x86 without -mbmi2; bzhi-style masked popcount on the popcount_2l byte-step degrades to a 5-insn fallback (~10% slower on the dns workload). Add -mbmi2 or -march=native, or define CDS_FT_SUPPRESS_ISA_WARNING to silence."
# endif
#endif

/*
 * FEATURE_INLINE_LOOKUP: force-inline the lookup hot-path helpers.
 * inline_lookup expands to inline __attribute__((always_inline)), so the
 * descent dispatches through no call boundaries.  When it is disabled, the
 * compiler chooses.  This trades lookup latency for smaller code.
 *
 * Enabled by default.  Disable with -DNO_FEATURE_INLINE_LOOKUP.
 */
#ifndef NO_FEATURE_INLINE_LOOKUP
# define FEATURE_INLINE_LOOKUP
#endif

/*
 * FEATURE_INLINE_INEQUALITY_LOOKUP: also force-inline the inequality descent,
 * and specialize it per mode.  The inequality descent is the relational seek
 * and its up/down tree traversal.
 *
 * OFF by default (opt-in).  The ordered-cell list is the fast path for ordered
 * traversal: cds_ft_next / cds_ft_prev resolve a cell pointer with no descent.
 * For groups using the ordered list, the tree descent is only the amortized
 * seek / seed.  It is better left as one shared, non-specialized function,
 * which is far smaller code.
 *
 * Enable it for inequality-heavy workloads WITHOUT the ordered list.  There
 * the descent is the per-operation cost.
 *
 * It governs only the tier-2 descent (ft_ineq_descend).  The tier-1
 * ordered-cell fast path is always inlined under FEATURE_INLINE_LOOKUP.
 */

/*
 * FEATURE_FT_COMPRESS: enable prefix compression (path compaction).
 * When enabled, chains of single-child internal nodes are replaced
 * with compressed path nodes.  Disabling compiles out all compressed
 * node handling, producing a simpler trie with no path compaction.
 *
 * Enabled by default.  Disable with -DNO_FEATURE_FT_COMPRESS.
 */
#ifndef NO_FEATURE_FT_COMPRESS
# define FEATURE_FT_COMPRESS
#endif

/*
 * FEATURE_FT_KEY_MAP: support a non-identity key map.  A key map is a custom
 * byte ordering set via cds_ft_group_attr_set_key_map, e.g. a
 * case-insensitive or collation order.  Identity maps (plain byte-ordinal
 * keys) work either way.
 *
 * Enabled by default.  Disable with -DNO_FEATURE_FT_KEY_MAP for byte-key-only
 * builds.  This compiles out the per-key-API non-identity lookup
 * specializations (~25 KiB of .text).  cds_ft_group_attr_set_key_map then
 * returns CDS_FT_STATUS_NOT_SUPPORTED.
 */
#ifndef NO_FEATURE_FT_KEY_MAP
# define FEATURE_FT_KEY_MAP
#endif

/*
 * FEATURE_FT_MERGE: the cds_ft_merge / cds_ft_merge_at bulk operation
 * (atomic spine-copy merge of one trie into another at a key position).
 *
 * Enabled by default.  Disable with -DNO_FEATURE_FT_MERGE for builds
 * that never merge tries.  This compiles out the whole merge subsystem
 * (~20 KiB of .text):
 *   - the recursive spine-copy build/count;
 *   - the ordered-cell interleave;
 *   - the in-place src unlink;
 *   - the reserve pre-pass.
 * cds_ft_merge / cds_ft_merge_at then return CDS_FT_STATUS_NOT_SUPPORTED.
 * graft, graft_swap and detach are unaffected.  They remain part of the
 * core bulk API.
 */
#ifndef NO_FEATURE_FT_MERGE
# define FEATURE_FT_MERGE
#endif

/*
 * FEATURE_FT_INSERT_IN_PLACE: in-place occupancy-bitmap safe-append (the O(1)
 * insert tier).
 * FEATURE_FT_DELETE_IN_PLACE: in-place soft-delete (the delete tier).
 *
 * Both are on by default since 2026-09-26, and they are independent.  One
 * macro used to enable both.  It was split the same day, so that root-cause
 * work can switch either tier alone.  -DNO_FEATURE_FT_INSERT_IN_PLACE turns
 * the insert tier back into recompact-on-insert.
 * -DNO_FEATURE_FT_DELETE_IN_PLACE turns the delete tier back into
 * recompact-on-delete.
 *
 * The insert tier.  A POINT insert is applied IN PLACE when it lands at a
 * node's tail rank and the tier has spare capacity.  The child slot is
 * published.  The node's occupancy-bitmap bit is set with a relaxed store on
 * the LIVE node's body (ft_popcount_node_set_nth Cases 2A/2B,
 * ft_pigeon_node_set_nth).
 *
 * The delete tier.  A point delete that leaves the holder above min_child
 * does not rebuild the node.  It NULLs the slot and decrements nr_child
 * through the commit.  This applies on shared tries too, since 2026-09-16
 * (the three point-remove ft_detach_node calls).
 *
 * Delete-on / insert-off is a supported mix.  Refilling a soft-deleted hole
 * is itself an insert-tier store.  With the insert tier off, the refill
 * reports -ERANGE and the recompact drops the hole.  With the insert tier
 * on, a popcount node refills a hole in place only at its END.  That is the
 * one position an append would take.  A hole below a live subkey recompacts
 * too: in-place inserts happen at the end of the subkeys, never before.
 *
 * The following holds on EVERY trie type.  The insert store is safe against
 * concurrent WRITERS because the op HOLDS the node before it writes.  The
 * hold is one of these (ft_attach_node's acquire hoist; ft_in_place_ok's
 * header):
 *   - the node's DLM lock;
 *   - the node's anchor, at a coarser spacing;
 *   - the FT-wide lock, under the coarse strategy.
 * The store is safe against concurrent READERS because an in-place mutation
 * never removes a bitmap bit.  So no existing rank moves.
 *
 * The bulk ops (graft / merge / rekey reserves and detaches) and the
 * build-path wrapper stay on the exclusive-only tier (ft_in_place_excl_ok).
 * Each stays there until it is converted to lock-before-write.  On a shared
 * trie, the dst attach parent of a same-trie move must relocate anyway, for
 * the reader-coherence witness.
 *
 * Without the insert tier, every new-occupancy insert reports -ERANGE
 * instead.  A new-occupancy insert is any insert that is not an in-place
 * pointer replace at an already-occupied slot.  The setter wrapper
 * (ft_node_set_nth_rec) then routes it through ft_node_recompact(ADD_SAME).
 * A fresh node carrying the new entry AND its bitmap is built
 * build-invisibly, and the parent edge is flipped.  A non-tail insert
 * already does exactly this.
 *
 * That was the shape the MW-with-CAS model needed (the Invariant-2
 * disjoint-word hazard, see doc/design/mcas-multiwriter-readiness.md S4).
 * Every node change was a whole-node replacement via the parent flip-txn
 * edge, so one CAS could arbitrate every word.  The DLM locks now provide
 * that exclusion.  A build without the insert tier still pays the cost: the
 * O(1) in-place insert turns into an O(node) alloc-and-copy recompact.  See
 * doc/design/ft-reintroduce-in-place-mutations.md.
 *
 * Both popcount and pigeon nodes recompact uniformly here.
 *
 * FUTURE (noted, not done; kept simple for now).  A PIGEON slot is
 * direct-indexed, so its pointer store is already a clean flip edge.  Its
 * bitmap is only an occupancy HINT: point lookups read the slot, and
 * iteration rescans past a set bit whose slot is NULL.  The pigeon recompact
 * is therefore avoidable.  Make the hint bitmap STICKY instead:
 *   - set a bit with an atomic OR;
 *   - never clear a bit in place (a delete leaves the bit; only a recompact
 *     rebuilds a clean bitmap);
 *   - optionally trigger a cleanup recompact once stale bits get high.
 * This keeps pigeon's O(1) insert/delete.  See
 * doc/design/mcas-multiwriter-readiness.md S4.
 *
 * Default: both tiers on.  Lock-before-write makes them safe against
 * concurrent writers.  The sticky bitmap bit makes them safe against readers
 * (above).  The gate keeps the recompact paths covered with its no-in-place,
 * no-in-place-insert and no-in-place-delete configs.
 */
#if defined(NO_FEATURE_FT_INSERT_IN_PLACE) && defined(FEATURE_FT_INSERT_IN_PLACE)
# error "FEATURE_FT_INSERT_IN_PLACE and NO_FEATURE_FT_INSERT_IN_PLACE both defined"
#endif
#if defined(NO_FEATURE_FT_DELETE_IN_PLACE) && defined(FEATURE_FT_DELETE_IN_PLACE)
# error "FEATURE_FT_DELETE_IN_PLACE and NO_FEATURE_FT_DELETE_IN_PLACE both defined"
#endif
#if !defined(NO_FEATURE_FT_INSERT_IN_PLACE) && !defined(FEATURE_FT_INSERT_IN_PLACE)
# define FEATURE_FT_INSERT_IN_PLACE
#endif
#if !defined(NO_FEATURE_FT_DELETE_IN_PLACE) && !defined(FEATURE_FT_DELETE_IN_PLACE)
# define FEATURE_FT_DELETE_IN_PLACE
#endif

/*
 * Skip-compressed pointers encode the compressed path length in the
 * high bits of pointers.  These are FT_SKIP_LEN_BITS bits starting at
 * FT_SKIP_LEN_SHIFT, e.g. bits 57-63 on x86-64 and 56-63 on AArch64
 * (see the per-architecture encoding parameters above).  This requires
 * architectures where those bits are guaranteed zero for userspace
 * pointers.
 *
 * Enabled on architectures that define FT_SKIP_LEN_BITS.  A runtime
 * validation (mmap probe) at flag-set time rejects the feature
 * if the encoding bits fall within the kernel's VA range.
 *
 * Not supported on 32-bit architectures (no spare high pointer bits;
 * FT_SKIP_LEN_BITS is left undefined there) nor on s390x (full 64-bit
 * virtual addresses).
 *
 * Requires FEATURE_FT_COMPRESS (skip-compressed is meaningless
 * without compressed nodes).
 *
 * Override with -DNO_FEATURE_FT_SKIP_COMPRESSED to force-disable.
 */
#ifdef NO_FEATURE_FT_COMPRESS
# ifndef NO_FEATURE_FT_SKIP_COMPRESSED
#  define NO_FEATURE_FT_SKIP_COMPRESSED
# endif
#endif
#ifndef NO_FEATURE_FT_SKIP_COMPRESSED
# if defined(FT_SKIP_LEN_BITS)
#  define FEATURE_FT_SKIP_COMPRESSED
# endif
#endif

/*
 * Library-owned ordered sibling list.
 *
 * The list threads the duplicate-chain heads (one per distinct key) into a
 * key-ordered doubly-linked list of LIBRARY-OWNED "ordinal cells"
 * (struct ft_ord_cell).  cds_ft_next / cds_ft_prev then walk the cell list
 * in O(1) per step, instead of an O(depth) trie descent + backtrack.
 *
 * The order links live OUTSIDE the application leaf.  struct cds_ft_node is
 * unchanged (no ABI growth, no app offset).  The cell is reached via the
 * head's cds_ft_node.prev:
 *   - A head's prev now points to its cell.  The pointer is tagged
 *     FT_INTERNAL_MASK, so the head-vs-dup test
 *     ft_node_external(prev)==false is preserved.
 *   - The head's parent moves into ft_ord_cell.parent.
 * The trie's DOWNWARD child slots still point directly at the external
 * node and skip the cell.  The cell is interposed only on the UPWARD walk
 * (parent recovery) and the ordered traversal.
 *
 * The cells are library-owned, so they are RELOCATABLE.  cds_ft_compact
 * packs them in key order, so that ordered iteration becomes a dense scan.
 * (The in-leaf chain is latency-bound because each step depends on a random
 * leaf load.)  Cells are kept reader-coherent via the flip-latch.
 *
 * Always compiled in (mandatory; there is no non-cell build).  Both the cell
 * and its ordered LIST are runtime-gated per group via
 * cds_ft_group_attr_set_ordered_list.  The setting is recorded as the trie's
 * immutable ft->ordered_list flag, which is read on the hot path.
 *   - An enabled group allocates a cell per distinct-key head and maintains
 *     the key-ordered list.  The head's parent is reached through the cell.
 *   - An unset group allocates NO cells: head->prev IS the flagged parent
 *     directly.  It skips all list maintenance.  So it pays neither the
 *     per-head cell nor the indirection.
 */

/*
 * Library-owned ordinal cell: one per distinct-key duplicate-chain head.
 *   @ord_prev/@ord_next: key-ordered doubly-linked list of cells.
 *   @node:   the external head this cell indexes (cell -> leaf, for the key).
 *   @parent: the head's flagged parent internal node (relocated out of
 *            cds_ft_node.prev, which now points at this cell).
 * Reached from a head as ft_ord_cell_ptr(rcu_dereference(head->prev)).
 */
struct ft_ord_cell {
	/*
	 * ☞ THE ONE FAMILY WHOSE MW IS ARGUED, NOT ASSERTED (register note 6):
	 * @lnode's two links have NO owning node lock at per-node OR exponential
	 * spacing, because a splice rewrites the cells of the NEIGHBOURING KEYS,
	 * whose holders this op never acquires.  MW by design, at every spacing;
	 * only a root-only lock-set or the FT-wide bulk gate would cover them.
	 * The deletion MARK on a retired cell's next is load-bearing.  A peer
	 * splice that captured that dead cell as pred/succ fails on this mark.
	 * ☠ Feed expected-olds through the txn handle, not through
	 * ft_ord_cell_resolve_ord.  An optimistic read of a slot this txn also
	 * WRITES is a doomed install under contention.  It causes aborts, not
	 * corruption.
	 *
	 * @parent, by contrast, HAS a single owner: the holder P.  It is MW
	 * only by debt.  It is also still written raw on ft_set_parent's path.
	 * So both paths are legal only while each holds P or the bulk gate.
	 *
	 * @lnode holds the links of the key-ordered doubly-linked list.  It is
	 * embedded as the public concurrent bidir-list node
	 * (<urcu/rcu-txn-list.h>).  lnode.next is the old ord_next and
	 * lnode.prev is the old ord_prev.  ft_ord_cell_of() (container_of)
	 * recovers the cell from a link.  ft_ord_cell_lnode() returns the link
	 * node of a cell.
	 *
	 * With the public node embedded, the single-cell
	 * splice/unsplice/replace use the list's composable _prepare ops,
	 * folded into the FT structural flip-txn.  The cell edges carry the
	 * concurrent list's engine proxy tag (URCU_TXN_TAG, bit 0).  Readers
	 * resolve them with ft_txn_list_resolve (ft_ord_cell_resolve_ord).
	 */
	struct urcu_txn_list_node lnode;
	struct cds_ft_node *node;
	struct cds_ft_inode_flag *parent;
};

/*
 * PREFIX-HEAD bit on an external head's OWN parent word.
 *
 * A head's key is reconstructed structurally (ft_rebuild_key_upwalk,
 * ft_upwalk_edge_bytes) from the word that names its parent.  The ONE fact
 * that word did not carry is which of two shapes the head has:
 *   - a SLOT head: key = path(parent) + its own edge byte;
 *   - a PREFIX head sitting at the parent's external_nodes:
 *     key = path(parent), so the key ENDS at the parent.
 *
 * ☠ THE SHAPE ANSWER MUST TRAVEL WITH THE HEAD, NOT WITH THE HOLDER.
 * Inferring it from the holder's LIVE external_nodes reads a second word.  A
 * key-disappearing remove (or a replace) of a prefix head DESTROYS that
 * answer IN PLACE while a parked reader still holds the head.  The dead
 * prefix head then reads as a slot head, and its key gains a spurious
 * trailing byte.  The result is a LEGAL key of the trie, on the WRONG node.
 * Measured, default build, one writer:
 *   - a present key answered NOT_FOUND;
 *   - a key absent at every instant answered with a neighbour's node.
 *
 * So the head itself carries the bit, on the word it already loads:
 * cell->parent with the ordered list ON, node->prev with it OFF.  The
 * encoding is the SAME either way.  The reader therefore needs no
 * ordered_list dispatch and no second load.  The answer is
 * snapshot-consistent with the pointer by construction.
 *
 * ★ SET AT INSTALL, NOT AT REMOVAL.  The bit is written when the head is
 * INSTALLED at an internal holder's external_nodes
 * (ft_publish_external_nodes_prev and the insert/replace install sites).
 * That is the only moment the shape is decided.  After that:
 *   - a re-home CARRIES the bit;
 *   - a re-home into a body SLOT clears it;
 *   - a head whose holder later drops it is never written again.
 * So a dead prefix head keeps its last correct answer for as long as a
 * parked reader can reach it.  That is what makes the removal-time mark
 * unnecessary.
 *
 * ★ AND IT IS VERIFIABLE AT REST.  cds_ft_verify asserts, on every LIVE
 * head, bit == (holder->external_nodes == head).  An install site that
 * forgets the bit, or a re-home that carries a stale one, is an ENUMERABLE
 * invariant violation.  It is not a rare reader-visible wrong answer.
 *
 * The bit is bit 4.  It is above the four in-band tag bits
 * (FT_PARENT_TAG_MASK) and below the smallest internal node's alignment
 * (order 5, 32 bytes).  A prefix head only ever hangs at an INTERNAL node,
 * because a compressed node carries no external_nodes.  So the bit never
 * aliases a compressed node's 16-byte alignment.  A re-home under a
 * compressed parent drops it by construction.
 */
#define FT_PARENT_PREFIX_HEAD	\
	((uintptr_t) (FT_INTERNAL_MASK | FT_TYPE_MASK) + 1)

urcu_static_assert(!(FT_PARENT_PREFIX_HEAD &
			(FT_INTERNAL_MASK | FT_TYPE_MASK | FT_COMPRESSED_MASK)),
		"the prefix-head bit must not overlap the parent tag nibble",
		ft_parent_prefix_head_above_tag);
/*
 * ★ AND THE ALIGNMENT PREMISE IS MACHINE-CHECKED, not left to the comment
 * above.  The mark is sound only for these two reasons:
 *   - Every INTERNAL node is allocated at
 *     ft_types[].order >= FT_INTERNAL_ORDER_MIN (5 = 32 B).
 *   - The allocator hands out order-aligned items: item = range_base +
 *     (n << order), with range_base page- or 2 MiB-aligned.
 * So bit 4 is never part of an internal node's address.
 * FT_ALLOC_ORDER_MIN (4 = 16 B) is the COMPRESSED tier.  That is exactly why
 * the helpers below refuse to touch a compressed flag.  Raising the mark or
 * lowering FT_INTERNAL_ORDER_MIN breaks this and must fail the build.
 */
urcu_static_assert(FT_PARENT_PREFIX_HEAD <
			((uintptr_t) 1 << FT_INTERNAL_ORDER_MIN),
		"the prefix-head bit must fit below the smallest internal node's alignment",
		ft_parent_prefix_head_below_internal_align);

/*
 * All four helpers take a RESOLVED parent word (no flip proxy: a descriptor
 * pointer's bit 4 is address) and read the bit ONLY on an INTERNAL flag: a
 * compressed node is 16-byte aligned, so bit 4 of a compressed parent flag is
 * part of its address and must never be tested, set or cleared.
 */
static inline
bool ft_parent_prefix_head(const struct cds_ft_inode_flag *parent)
{
	return ((uintptr_t) parent & (FT_INTERNAL_MASK |
			FT_PARENT_PREFIX_HEAD)) ==
		(FT_INTERNAL_MASK | FT_PARENT_PREFIX_HEAD);
}

static inline
struct cds_ft_inode_flag *ft_parent_prefix_strip(struct cds_ft_inode_flag *parent)
{
	if (!((uintptr_t) parent & FT_INTERNAL_MASK))
		return parent;
	return (struct cds_ft_inode_flag *)
		((uintptr_t) parent & ~FT_PARENT_PREFIX_HEAD);
}

/* Recover the cell owning an embedded ordered-list link node, and vice versa. */
static inline
struct ft_ord_cell *ft_ord_cell_of(const struct urcu_txn_list_node *lnode)
{
	return caa_container_of(lnode, struct ft_ord_cell, lnode);
}

static inline
struct urcu_txn_list_node *ft_ord_cell_lnode(struct ft_ord_cell *cell)
{
	return &cell->lnode;
}

/*
 * Item-length order for the dedicated cell arena: the smallest power of two
 * that holds a struct ft_ord_cell (32 B => order 5).  A 1<<order-aligned cell
 * keeps bits 0-2 clear, so the FT_ORD_CELL_TAG (bit 0) and flip-proxy (type 7)
 * encodings stay unambiguous.  Bump it if the cell grows (the assert guards).
 */
#define FT_ORD_CELL_ALLOC_ORDER		5
urcu_static_assert(sizeof(struct ft_ord_cell) <= (1U << FT_ORD_CELL_ALLOC_ORDER),
		"struct ft_ord_cell must fit the cell arena item size",
		ord_cell_fits_alloc_order);
/*
 * lnode MUST be the first field (offset 0).  ft_ord_cell_of() is then a
 * 0-offset cast, so:
 *   - ft_ord_cell_of(NULL) == NULL;
 *   - ft_ord_cell_of(&ft->ord_sentinel.node) == &ft->ord_sentinel.node.
 * Both aliasings are load-bearing:
 *   - ft_ord_is_end() recognises a NULL link and the trie's own sentinel as
 *     "end" with a single pointer compare.
 *   - The cross-trie run moves NULL-terminate a moved run's outer links as a
 *     universal end.
 * A field placed before lnode would turn ft_ord_cell_of(NULL) into a small
 * non-NULL garbage pointer.  ft_ord_is_end() would mistake it for a real
 * cell.
 */
urcu_static_assert(offsetof(struct ft_ord_cell, lnode) == 0,
		"struct ft_ord_cell.lnode must be the first field (offset 0)",
		ord_cell_lnode_offset_zero);

/*
 * FEATURE_FT_EXCL_VALIDATE: runtime validation of the access-discipline
 * contract.  Writers claim a per-trie owner via atomic CAS at the
 * public API boundary.  A writer/writer overlap aborts the process
 * with a violation report.
 *
 * The writer/writer check above is mode-independent: the contract
 * serializes writers in both modes.  Only reader validation depends on
 * the trie mode:
 *
 *   Exclusive mode: readers are counted.  A writer entering with any
 *   reader present aborts.  So does a reader entering with a writer
 *   present.
 *
 *   Concurrent mode: a well-formed reader either holds the RCU
 *   read-side lock or guarantees external mutual exclusion against
 *   mutators.  At reader entry the validator queries the RCU flavor's
 *   read_ongoing().
 *     - If true, the reader is RCU-protected and may overlap with a
 *       single writer (no-op).
 *     - If false, the reader is asserting the mutex-claim path.  It
 *       is treated like an exclusive-mode reader: a concurrent writer
 *       aborts.
 *   The validator only catches the deterministic case, where the bad
 *   interleaving actually occurs in this run.  A reader that holds
 *   neither protection but does not overlap with any writer cannot
 *   be detected from a single observation.
 *
 * Off by default (zero overhead).  Enable with -DFEATURE_FT_EXCL_VALIDATE.
 */

/*
 * FEATURE_FT_VERIFY_AT_MUTATION: walk the entire trie at the exit of
 * every public write API (insert / remove / replace / graft / detach)
 * and run cds_ft_verify.  On any invariant mismatch, print a
 * diagnostic to stderr and abort the process.
 *
 * Catches structural / nr_keys / parent-pointer regressions
 * at the mutation that introduced them, instead of via downstream
 * symptoms.  The recursive walk is O(N) per mutation, so this is for
 * testing / debugging only.
 *
 * Off by default (zero overhead).  Enable with -DFEATURE_FT_VERIFY_AT_MUTATION.
 */
struct cds_ft;
#ifdef FEATURE_FT_VERIFY_AT_MUTATION
void ft_writer_scope_verify(struct cds_ft *ft);
#endif

#ifdef FEATURE_INLINE_LOOKUP
#define inline_lookup	inline __attribute__((always_inline))
#else
#define inline_lookup
#endif

/*
 * inline_ineq: like inline_lookup but gated on FEATURE_INLINE_INEQUALITY_LOOKUP,
 * for the tier-2 inequality descent (ft_ineq_descend).  It is off by default.
 * The descent is then plain static: one shared copy across all modes/limits.
 */
#ifdef FEATURE_INLINE_INEQUALITY_LOOKUP
#define inline_ineq	inline_lookup
#else
#define inline_ineq
#endif

/*
 * Spill/reload helpers for breaking the live range of a variable.
 *
 * FT_SPILL_TO_STACK(var) stores @var to its own stack slot through
 * a volatile lvalue.  This forces gcc to actually emit the store and
 * to stop assuming any register copy is in sync with memory.  With
 * no normal use of @var until the matching reload, gcc is free to
 * recycle the register that held @var.
 *
 * FT_RELOAD_FROM_STACK(var) is the matching reload -- a volatile
 * load from @var's stack slot into a fresh register.
 *
 * The two MUST be paired.  Every code path that reaches the
 * reload must have executed the matching spill first; otherwise
 * the reload returns an uninitialized stack-slot value.
 *
 * Implementation: an empty inline asm with `"=m"`/`"=r"`
 * constraints would declare the memory or register as touched
 * but emit no instructions.  gcc's data-flow then folds the
 * pair away and keeps @var live across the gap, which defeats
 * the purpose.  The volatile cast emits the actual store and load.
 *
 * Caveat: if the freed register would be useful inside the gap,
 * the gap must not derive its work-state from @var (e.g. via
 * macros that dereference @var).  Each such reference would
 * force a reload from the spill slot.  Capture work-state into
 * local pointers BEFORE the spill and refer to those locals
 * inside the gap.
 */
#define FT_SPILL_TO_STACK(var)						\
	do {								\
		*(volatile __typeof__(var) *) &(var) = (var);		\
	} while (0)

#define FT_RELOAD_FROM_STACK(var)					\
	do {								\
		(var) = *(volatile __typeof__(var) *) &(var);		\
	} while (0)

enum {
	FT_NO_BITMAP = false,
	FT_BITMAP = true,
};

/* Never declared. Opaque type used to store flagged node pointers. */
struct cds_ft_inode_flag;
struct cds_ft_inode;
struct cds_ft_alloc_arena;

/*
 * Recorder for ft_publish_to_parent's reader-visible edges.
 *
 * When a key-disappearing remove recompacts a node, two steps must happen
 * ATOMICALLY:
 *   - the rebuilt node is published into its parent slot;
 *   - the dead head cell is unspliced from the ordered list.
 * This is the remove dual of the insert splice window.
 *
 * With a non-NULL rec, ft_publish_to_parent still performs all of its
 * bookkeeping that readers cannot see (parent-slot offset, trace events).
 * It does not perform its 1-2 reader-visible stores.  It RECORDS them here
 * instead.  The caller (ft_detach_node) can then commit them in one flip
 * with the cell edges.  There are at most two edges: the forward parent
 * slot, plus a compressed parent's SKIP_X dual pointer.
 */
struct ft_lock_ctx;

struct ft_pub_rec {
	/*
	 * ☞ THE OP'S LOCK CONTEXT.  It is carried for the SAME reason @root
	 * and @owner are: the producer is the only frame that has it, and the
	 * replays are several and far away.
	 *
	 * Only FT_OWNER_ASSERT_OWNED_CTX consumes it.  That assert needs the
	 * WIDE witness.  ft_flip_txn_owns reads @locks[] alone.  So a hold
	 * filed in @extra, in the glue, or in an OUTER frame answers false,
	 * and the assert fires on a word the op really does own.
	 * ft_lock_ctx_holds consults all four.
	 *
	 * MEASURED over one ft_inv run: the SKIP_X dual's acquire exits SHARED
	 * 533642 times against REGISTERED 148705.  So 78% of the honest "I
	 * hold this" answers are invisible to the narrow witness.
	 *
	 * @ctx is NULL where the producer has no ctx.  The assert is then
	 * exactly as narrow as it was.
	 */
	const struct ft_lock_ctx *ctx;
	struct cds_ft_inode_flag **slot[3];
	struct cds_ft_inode_flag *old_val[3];
	struct cds_ft_inode_flag *new_val[3];
	/*
	 * Per-edge: this slot is a TRIE ROOT (&ft->root), so every replay of
	 * this rec must record it MW (ft_flip_txn_record_root).  A root lives
	 * in no node, so no lock-set can own it and no structural_sw op may
	 * park it.  The publish is the only place that KNOWS -- it branches on
	 * the NULL parent already (the root_publish tracepoint) -- and the
	 * replays are several and far away, so the answer travels with the
	 * edge rather than being re-derived at each of them.
	 */
	bool root[3];
	/*
	 * Per-edge: the node whose DLM lock OWNS this slot (§8), for the
	 * record-time owner check every replay of this rec runs
	 * (FT_OWNER_ASSERT_OWNED).  Carried per EDGE for the same reason
	 * @root is: the publish is the only place that knows, and the replays
	 * are several and far away.
	 *
	 * NULL is the SAFE default and it means "this producer does not name
	 * an owner" -- the record is then never eligible for a per-op SW park,
	 * which is the all-MW behaviour every unconverted path already has.
	 * ☠ It is NOT the mirror of @root: an unset @root would wrongly PARK a
	 * root, while an unset @owner only declines to convert.
	 */
	struct cds_ft_metadata *owner[3];
	/*
	 * Per-edge: does the OP HOLD @owner's DLM lock?  This is the third
	 * answer of the parentage triple.  @owner cannot replace it.
	 *
	 * ☠ A NULL @owner DOES NOT FAIL CLOSED.  The dispatching recorder
	 * (ft_flip_txn_record_tag) branches on @t->structural_sw ALONE.
	 * @owner feeds the debug assert and the counters, and nothing else.
	 * So an armed txn parks an unnamed slot SW just as readily as a named
	 * one.  In a build without --enable-rcu-debug it does so silently.
	 * The comment that used to stand on @owner said "the record is then
	 * never eligible for a per-op SW park".  That was a STALE MECHANISM.
	 *
	 * This is the word that decides.  When it is false, the replays
	 * record MW, which is stricter and always sound.  The FORWARD edge
	 * inherits the caller's own @slot_owner_nf declaration.  The SKIP_X
	 * DUAL cannot, and that is the whole reason this field exists.  The
	 * dual's owner is the GRANDPARENT.  It is DERIVED inside the publish
	 * helper from a back-pointer, so only the op can say whether it
	 * acquired it.
	 *
	 * ☠ AND TODAY EVERY PRODUCER SAYS FALSE: all twelve
	 * _ft_publish_to_parent callers plus ft_node_recompact's own dual
	 * site, the insert lane included.  The text that used to stand here
	 * was a STALE CLAIM: "ft_detach_node's republish does (the recompact
	 * takes {C,P,GP} exactly when P is compressed)".  That site passes
	 * false and says why in capitals.  The acquire is a PLAN-TIME fact,
	 * while the dual's owner is DERIVED FRESH at publish (ft-remove.h,
	 * the "☠ FALSE, AND `old_recompacted_node != NULL` WAS NOT SOUND"
	 * note).
	 *
	 * ☞ SO THIS FIELD IS ABOUT KIND ONLY, AND NEVER ABOUT EXCLUSION.  The
	 * dual's home is an MW slot globally.  Holding GP is what EXCLUDES a
	 * peer recompaction from copying that body out from under the record.
	 * That is ft_lock_skip_dual_gp's job (§9.3's third lock-set member),
	 * not this word's.  A reader of this field may conclude "false, so
	 * the record is MW, so it is safe".  That answers the kind question
	 * and leaves the exclusion one open.  That is exactly how the
	 * stale-dual livelock got in.
	 */
	bool owner_held[3];
	unsigned int n;
	/*
	 * The commit engine handle this rec's edges will be recorded into, when
	 * the caller has one.  It is not plumbing for the record: the caller
	 * records by itself.  It lets the SKIP_X dual slot be derived
	 * READ-YOUR-OWN-WRITES.
	 *
	 * The dual lives in the compressed parent's OWN parent, at the offset
	 * that parent's metadata records.  Both words are ordinary transacted
	 * slots.  An op that RE-PARENTS the compressed node in this very txn
	 * has pending edges on them.  A raw derivation then answers with the
	 * PRE-OP slot, a word inside the node the same commit retires.  The
	 * refreshed dual lands in the copy nobody will read, and the live
	 * trie keeps a dual naming a superseded child.
	 *
	 * NULL keeps the raw derivation.  A direct-store publish has no txn to
	 * consult, and no pending edges to miss.
	 */
	struct ft_txn *mtxn;
};

/*
 * Struct layout, ONE WORD PER LINE so that it holds on both ABIs.  LP64 byte
 * offsets are in brackets.  The struct is 48 B on LP64 and 24 B on ILP32:
 * the same six words at half the width, with zero internal padding.
 *   word 0 [ 0]: parent_word
 *   word 1 [ 8]: external_nodes
 *   word 2 [16]: parent_slot_offset -- its OWN word since the §8.3 split
 *   word 3 [24]: nr_keys
 *   word 4 [32]: state -- nr_child lives here (bits 2-10), not in a bitfield
 *   word 5 [40]: alloc_index + incoming_byte packed, plus tail padding
 *
 * In cds_ft_metadata_alloc (64 B on LP64, one cache line), rcu_head is a
 * SEPARATE field placed before the metadata union.  It overlaps no metadata
 * field, so every metadata field stays valid for the whole RCU grace period.
 *
 * The union's OTHER member has no such property, and the difference matters.
 * @free_list_next occupies exactly the bytes of @parent_word.  So returning
 * an item to its range freelist overwrites the parent link with a pointer to
 * another cds_ft_metadata_alloc.  That write lands only AFTER the grace
 * period:
 *   - cds_ft_free_item defers it through call_rcu;
 *   - the exclusive-mode synchronous push has no concurrent readers by
 *     construction.
 * So no reader that respects its pin can observe it.
 *
 * A reader that does NOT respect its pin holds a reference past its grace
 * period.  It reads a freelist link.  That link is >= 8-byte aligned, so
 * ft_node_external() and every other structural predicate accept it.  The
 * load does not fault.  The link MASQUERADES as a legal external parent and
 * faults later, in cds_ft_item_to_metadata, on an address in the metadata
 * region.
 *
 * So the overlap is what makes such a use-after-free LOUD.  Separating the
 * union would leave @parent_word holding the node's last real parent.  The
 * same stale reader would then walk a plausible wrong tree in silence.
 */
/*
 * Bit layout of the per-node MCAS state word (struct cds_ft_metadata.state):
 *   bit 0       proxy (in-band flip marker)
 *   bit 1       tombstone (LIVE->DEAD)
 *   bits 2-10   nr_child
 *   bits 11-18  FREE
 *   bit 19      LOCK (the reversible per-node writer lock)
 *   bits 20+    free
 * ☞ Bits 11-18 no longer hold parent_slot_offset.  It moved to its own word
 * (cds_ft_metadata::parent_slot_offset).  The FT_PSO_* block below records
 * that split and its reason.
 * See doc/design/mcas-multiwriter-readiness.md §4.2 and, for bit 19,
 * doc/design/mw-writer-lock-escalation-model.md §0/§2.
 */
#define FT_STATE_PROXY			((uintptr_t) 1 << 0)
#define FT_STATE_TOMBSTONE		((uintptr_t) 1 << 1)
#define FT_STATE_NR_CHILD_SHIFT		2
#define FT_STATE_NR_CHILD_BITS		9	/* live-child count, max 256 (bits 2-10) */
#define FT_STATE_NR_CHILD_VALMASK	(((uintptr_t) 1 << FT_STATE_NR_CHILD_BITS) - 1)
#define FT_STATE_NR_CHILD_MASK		(FT_STATE_NR_CHILD_VALMASK << FT_STATE_NR_CHILD_SHIFT)
#define FT_STATE_NR_CHILD_ONE		((uintptr_t) 1 << FT_STATE_NR_CHILD_SHIFT)
/*
 * Bits 11-18 are FREE.  They used to hold parent_slot_offset, which now lives
 * in its own word (cds_ft_metadata::parent_slot_offset, FT_PSO_* below).
 *
 * The old packing existed so that a re-home committed the parent edge and
 * the offset "as ONE atomic MCAS state edge".  But @parent was ALWAYS a
 * separate word, so that pairing never came from the packing.  It comes from
 * two other mechanisms:
 *   - Both edges ride one flip-txn: ft_reparent_record_meta records
 *     &meta->parent and the offset word into the same commit.
 *   - ft_get_parent_slot's reader-side coherence loop validates the pair
 *     explicitly (same-descriptor check + a parent re-read).
 *
 * Splitting therefore preserves the guarantee.  It also ends the CROSS-LOCK
 * RMW that sharing forced.  nr_child is NODE-owned.  parent_slot_offset is
 * PARENT-owned per the edge principle (doc §8.3).  One word cannot be owned
 * by two locks.
 *
 * Splitting also removes a SELF-DEADLOCK.  The offset setter had to spin on
 * FT_STATE_INPLACE_WAIT_MASK, which includes FT_STATE_LOCK unconditionally.
 * So an op holding that node's own lock waited on itself (see the reverted
 * b20c471e).  The offset word carries no LOCK bit, so its wait is over the
 * engine proxy alone.
 */
#define FT_PSO_SHIFT			1	/* bit 0 stays clear: engine proxy tag */
#define FT_PSO_BITS			8
#define FT_PSO_VALMASK			(((uintptr_t) 1 << FT_PSO_BITS) - 1)
#define FT_PSO_ENCODE(off)		(((uintptr_t) (off) & FT_PSO_VALMASK) \
						<< FT_PSO_SHIFT)
#define FT_PSO_DECODE(word)		((unsigned int) (((uintptr_t) (word) \
						>> FT_PSO_SHIFT) & FT_PSO_VALMASK))
/*
 * FT_STATE_LOCK (bit 19, above the free bits 11-18): the REVERSIBLE per-node
 * WRITER LOCK (MW lock-escalation model, §0).  It is acquired with a CAS and
 * held across the build/reparent plan window.  Contention returns -EAGAIN.
 * The lock is resolved at commit.
 *
 * ACQUIRE: a standalone CAS {clean -> |LOCK}, taken BEFORE the first read of
 * the node it protects.  Every peer publish into the node then aborts on its
 * §4.B guard.  (The guard's clean-LIVE expectation masks this bit like the
 * tombstone.)  So the protected body cannot go stale under the holder
 * without someone aborting.  The mark CAS fails when the bit is already set,
 * on a parked proxy, or on a real retire.  A failure means "could not
 * acquire": return -EAGAIN and re-descend.
 *
 * TERMINALS: exactly two on a successful commit.  Both are expressed as a
 * RECORDED MCAS edge on the state word.  The expected old of that edge is the
 * mark's CLEAN snapshot, never a fresh read (the CORE_682870 defect-1
 * contract).
 *
 *   RETIRE  {LOCK|s -> TOMBSTONE|s}  ft_flip_txn_record_tombstone_locked()
 *           The holder copied the node away; it dies at the commit.  One-way
 *           tombstone semantics (exactly-once retire token, freeze-on-free) are
 *           UNCHANGED.
 *   RELEASE {LOCK|s -> s}            ft_flip_txn_record_release_lock()
 *           The holder only needed exclusion; the node SURVIVES the commit.
 *           This is what a lock-set member that is edited but not retired needs
 *           (recompact's parent P, §9.3) -- a lock that unlocks.
 *
 * On ABORT / a pre-commit bail the bit is CAS-cleared instead (the registry
 * drain, ft_flip_txn_lock_release_all) and the node stays live.  So the
 * ABORT terminal and the RELEASE terminal agree on the resulting word.  They
 * differ only in who writes it: a bare CAS or the atomic commit.
 *
 * Unlike the tombstone, LOCK is reversible BY DESIGN and never implies
 * death.  It is never set at rest (ft-verify.h reports a leaked lock).
 */
/*
 * Bit 19 is pinned LITERALLY.  It is not derived from any other field's
 * width.  Otherwise, moving the writer lock (to bit 11, say) would compile
 * silently.  It would be an invisible change to the meaning of every state
 * word.  Bits 11-18 stay free (see the free-bits note above).
 */
#define FT_STATE_LOCK		((uintptr_t) 1 << 19)
#define FT_STATE_TAG_MASK		(FT_STATE_PROXY | FT_STATE_TOMBSTONE)

/*
 * FT_STATE_INPLACE_WAIT_MASK: the state-word bits on which the standalone
 * in-place CAS primitives must WAIT (spin + re-derive) instead of CASing
 * blindly.  A set bit means that a concurrent writer is mid-operation on the
 * word.  The primitives are ft_meta_nr_child_inc / _dec, and
 * ft_meta_nr_child_dec_flip via ft_meta_state_transition.
 * (ft_meta_parent_slot_offset_set no longer waits on this mask.  The offset
 * has its own word, which carries no LOCK bit.  See the free-bits note
 * above.)
 *
 * The mask has two bits:
 *   - FT_STATE_PROXY: an engine MCAS flip is parked here.  The word holds a
 *     record pointer, not a plain state.  CASing would write
 *     f(latch-pointer) back.  Always waited out, in every build.
 *   - FT_STATE_LOCK: a peer holds the per-node writer lock.  It may be
 *     SW-parking this word (a retire / lock release / re-home) in an
 *     in-flight mixed sw/mw commit.  An SW park is a plain store that never
 *     validates, so a racing CAS here would clobber it (<urcu/rcu-txn.h>:
 *     "if any other transaction may store_mw() the same slot, this park
 *     races that CAS").  The correct sequence is to wait for the holder's
 *     commit to settle the word first, then CAS onto the settled value:
 *       - After a RELEASE the word is the pre-lock snapshot.  A legitimate
 *         +/-1 lands correctly.
 *       - After a RETIRE the word is TOMBSTONE|snap.  The bump is harmless:
 *         the node is about to be freed.  The racing op's own §4.B guard
 *         then aborts and re-descends past the dead node.
 *
 * INVARIANT this relies on (exhaustively audited 2026-07-23): NO op calls
 * these standalone primitives on a node it ITSELF holds LOCK on.  A LOCK-held
 * node's count / offset change always rides the flip-txn as a RECORDED edge
 * (ft_flip_txn_record_tag_excluded / ft_flip_txn_record_release_lock /
 * ft_flip_txn_record_count_parent), never these primitives.  So the spin is
 * on a PEER's lock only and cannot self-deadlock.  A future guard->lock
 * conversion must not route a locked node's nr_child through these
 * standalone primitives instead of a recorded edge.  That would break this
 * invariant.
 *
 * LOCK IS IN THE MASK UNCONDITIONALLY.  Per-node lock-sets are the only
 * multi-writer implementation, so there is no build in which this bit is
 * merely a copy fence.  The consequence is load-bearing: an in-place
 * nr_child update SPINS while a peer holds the lock.  It HONORS the lock
 * instead of racing it.
 */
#define FT_STATE_INPLACE_WAIT_MASK	(FT_STATE_PROXY | FT_STATE_LOCK)

/*
 * ============================================================================
 * THE TRANSACTED-SLOT REGISTER: who owns each word, and what KIND it takes
 * ============================================================================
 *
 * Every word below is written through the txn engine.  So every one of them
 * answers three questions, and they are THREE, not two:
 *   - @owner names the node whose lock excludes other writers (§8.2 "a
 *     node's body is its own").
 *   - @owner_held says whether THIS op holds that lock.  This picks the
 *     record KIND.
 *   - "Does this op EXCLUDE the word" is neither of the above.  A missing
 *     lock or guard loses this one.
 * The stale-SKIP_X-dual livelock came from answering the first two and
 * skipping the third.
 *
 * KIND IS A PROPERTY OF THE SLOT, NOT OF THE RECORD.  A slot is SW XOR MW:
 *   - SW: parked under a held lock.  The park cannot fail.
 *   - MW: CAS-arbitrated.
 * One thread parking SW while another CASes MW is a lost or torn write that
 * the engine cannot detect.  MW is always safe.  SW is a PROMISE of
 * exclusion.
 *
 * ☞ AND THERE IS EXACTLY ONE LEGITIMATE MIX ON ONE WORD: the LOCK-BEARING
 * WORD PROTOCOL.  The @state rows below USE it.  The old blanket phrasing
 * ("the same kind in EVERY txn and EVERY thread") contradicted it.
 *
 *     An MW record TAKES the lock.  Every write to that word AFTER it, while
 *     the lock is held, may be SW: the bit updates, the other sub-fields, and
 *     the SW release that drops the lock.
 *
 * The MW take is what establishes the exclusion.  So everything sequenced
 * after it is already excluded and needs no CAS.
 *
 * ☞ AND STAYING MW AFTER THE TAKE IS CORRECT, JUST WASTEFUL.  The protocol
 * PERMITS the SW writes.  It does not require them.  A CAS under an
 * exclusion that already holds arbitrates against nobody and still lands the
 * right value.  So converting these to SW is an EFFICIENCY step, and a
 * producer left MW is never a correctness bug.  That is why every row here
 * may sit at MW indefinitely, and why MW is the safe default for a new one.
 * What IS a bug is the reverse pairing: an SW park on a word some OTHER
 * writer may CAS.  The park does not arbitrate, and the CAS's expected-old
 * was read before it.
 *
 * ☠ IT DOES NOT HAVE TO BE ONE TXN.  Txn 1 takes the lock, txn 2 changes
 * state under it, txn 3 unlocks: that is correct.  The lock is what holds
 * across them, not the txn boundary.  The ORDER is the whole content of the
 * rule: MW-then-SW is the protocol, and SW-then-MW is the bug.  The engine's
 * debug police now trap exactly that bug (urcu_txn_record_chain accepts
 * `r->kind == kind || r->kind == MW`).
 *
 * ⇒ SO THE RULE SPLITS BY WHETHER THE WORD CARRIES ITS OWN LOCK:
 *
 *   - A LOCK-BEARING word (@state): the protocol above.  Its rows legitimately
 *     read MW for the LOCK take and SW for the tombstone / release / nr_child++
 *     that follow it on the SAME word.  That is not a kind conflict.
 *   - Every OTHER word (an internal body child slot, the SKIP_X dual, the chain
 *     pointers, @external_nodes, the back edge): NOTHING written to the slot
 *     establishes exclusion over it.  The exclusion lives in ANOTHER word's
 *     lock.  So there is no MW take to sequence behind, and every writer must
 *     agree.  A conversion to SW is legal only once EVERY producer holds the
 *     owner, never one lane at a time.
 *
 * ☠ Do not read the first bullet as a licence for the second.  A body word
 * has no lock bit, so "the lock serialises an SW park against an MW CAS" is
 * FALSE of it: the peer's CAS is arbitrated against nothing.  See
 * urcu_txn_store_sw in <urcu/rcu-txn.h>: "a slot is SW xor MW, globally".
 * That is the rule for every word that cannot take a lock in its own bits.
 *
 * WHEN SW IS REACHABLE AT ALL -- three doors:
 *   1. ft_txn_content_sw_ok(): a COARSE or exclusive trie is armed SW
 *      trie-wide, at EVERY spacing.
 *   2. ft_flip_txn_arm_per_op(): under LOCK_FINE, at EVERY SPACING since
 *      2026-09-17.  The KIND IS THEN DECIDED PER RECORD.  A record parks SW
 *      only where the txn owns its owner (__ft_flip_txn_record_tag_ctx).
 *      It falls back to MW otherwise.
 *      ☞ This door used to refuse every spacing but per-node.  The reason
 *      was the PREDICATE: ft_flip_txn_owns compared the registry's words
 *      against the record's node, while a coarse spacing registers that
 *      node's ANCHOR.  The acquire's answer now travels with the
 *      registration:
 *        - ft_held_anchor's @member;
 *        - @covered, for a member whose word deduped onto another's;
 *        - the root-only anchor, which needs no descent.
 *      So the answer is exact where the acquire filed it and conservative
 *      beyond it.  The old spacing column's "MW by constr." rows are
 *      therefore SW-capable now.  What still decides them is ownership,
 *      record by record.
 *   3. ft_flip_txn_arm_structural(): the rekey writer and the root-COW driver
 *      arm SW under FINE at ANY spacing (self-labelled "Phase E's DEBT").
 *      This is the one door through which an SW park and an MW CAS can meet
 *      on one structural word.  What keeps them apart today is the FT-wide
 *      bulk gate, not the DLM.  Such a writer is marked @sw_body and is
 *      EXEMPT from door 2's per-record gate.  Its edges sit on words it
 *      fenced itself, so falling back to MW would abort its own commit
 *      forever.
 *
 * ☠ OWNERSHIP IS TAKEN, NEVER OBSERVED.  An MW {live->live} VALIDATE on a
 * word a peer holds is not arbitrated against that peer's SW parks.  The
 * validator is the loser: its proxy is overwritten and its settle-back CAS
 * fails silently.  A guard is a weaker representative of a lock only for a
 * word nobody copies under it.  It is NEVER a substitute for the take.  See
 * the state-word protocol in ft-mutation-helpers.h.
 *
 * ☠ AND THE ANCHOR MOVES THE WORD.  Under a coarse spacing ft_anchor_meta
 * maps a lock-set member to an ANCHOR ANCESTOR.  So the word a peer must
 * contend for is NOT the node's own state word.  A fix that validates a
 * node's own word is therefore untested above per-node by construction.  An
 * ACQUIRE gets §1's AGREEMENT property (every op resolves the same anchor).
 * A guard does not.
 *
 *   SLOT                          OWNER (§8.2)        per-node   exponential
 *   ----                          -----------         --------   -----------
 *   internal body child slot      the node it is in   SW armed   SW if owned
 *   cds_ft_compressed_node.child  that cn             SW armed   SW if owned
 *   cds_ft.root                   NONE (no node)      MW         MW   [DESIGN]
 *   SKIP_X dual (GP body word)    the GRANDparent     SW armed   SW if owned (1)
 *   cds_ft_metadata.external_nodes the node it is in  SW armed   SW if owned
 *   metadata.state  LOCK take     node / its anchor   MW         MW   [DESIGN]
 *   metadata.state  tombstone     ditto               SW armed   SW if owned
 *   metadata.state  lock release  ditto               SW armed   SW if owned
 *   metadata.state  nr_child++    ditto               SW armed   SW if owned
 *   metadata.state  nr_child--    ditto               MW  (2)    MW   [DESIGN]
 *   metadata.state  {live->live}  --                  MW         MW   [DESIGN]
 *   metadata.parent_word          the PARENT (3)      SW if parent held
 *   metadata.parent_slot_offset   the PARENT (3)      SW if parent held
 *   metadata.nr_keys              FT-wide lock (9)    MW         MW   [debt]
 *   metadata.incoming_byte        not transacted (7)
 *   metadata.alloc_index          ALLOCATOR-PRIVATE (7) -- never transacted
 *   cds_ft_node.next              the chain HOLDER    MW  (4)    MW   [debt]
 *   cds_ft_node.prev  (member)    the chain HOLDER    MW / SW(5) MW   [debt]
 *   cds_ft_node.prev  (head)      the holder P        SW if parent held (5)
 *   ft_ord_cell.lnode.next/prev   NONE (6)            MW         MW   [DESIGN]
 *   ft_ord_cell.parent            the holder P        MW + raw   MW   [debt]
 *   ft_ord_cell.node              BUILD-INVISIBLE (8) -- never transacted
 *
 * ☞ "[DESIGN]" means the word can never convert: it has no single owner (the
 * root slot lives in no node; an
 * ordered-list splice rewrites the cells of NEIGHBOURING keys, whose holders
 * the op never acquires) or it IS the arbitration point (the lock take).
 * Everything else marked [debt] has a named owner and is on the conversion
 * surface; "MW by constr." is neither -- it is door 2 above, and it changes
 * the moment that gate lifts.
 *
 * (1) The dual's owner is DERIVED at publish from cn's back-pointer, so only
 *     the op can vouch for it.  Every producer passes @owner_held false, the
 *     insert lane and the remove lane alike.  Both now ACQUIRE it
 *     (ft_insert_lock_skip_dual_gp, ft_lock_skip_dual_gp).
 *     ☑ AND THE TWO NAMED BLOCKERS ARE DISCHARGED.  This row said it "stays
 *     MW until ft_node_recompact's own dual site (FT_OWNER_UNPLUMBED) and the
 *     glue publishes can vouch too".
 *       - The glue publishes vouch through the FT-wide writer lock.
 *       - The relocation site vouches from its own acquire.  It always HELD
 *         the word.  It was asked through a registry that structurally could
 *         not answer.
 *     MEASURED at per-node AND exponential: every record that COMMITS is now
 *     excluded.  The only producers still answering no were ft_promote_head's
 *     two arms, and only on an acquire MISS.  They now BAIL at the miss
 *     instead of emitting a record they cannot vouch for.  So the kind is
 *     decided where the record is made, and not by whether some later commit
 *     discards it.  @root keeps MW by design.
 *
 *     ☑ SO THIS ROW IS OFF [debt], MEASURED TWO WAYS, per-node AND
 *     exponential:
 *       (a) -DFT_DEBUG_DUAL_SITE, at the dual's OWN recorder: named_unheld = 0
 *           AND unnamed = 0 for EVERY producer.
 *       (b) -DFT_DEBUG_TXN_KIND, the G4 surface: DUAL, DUAL_ROOT and
 *           DUAL_NAMED all read ZERO.  So no dual record lands in MW_ALWAYS
 *           at all.  (DUAL_NAMED is "a producer that could vouch if it
 *           acquired".)
 *     ☞ "SW armed" here means what it means in the rows above: SW once the txn
 *     armed per-op, MW_STRUCT when it did not.  The measurement says the dual
 *     is no longer MW BY CLASS.  It does not claim every record is SW.
 *
 *     ☠ `DUAL_UNNAMED` (~470k-620k per ft_inv leg) WAS READ AS "NOT THIS ROW",
 *     on the claim that every dual is recorded through ft_pub_rec_add_at
 *     (unnamed = 0 in the same run).  That claim was FALSE.
 *     _cds_ft_insert_replace's leaf replace builds its edges BY HAND (the
 *     forward edge, or the SKIP_X dual plus cn->child) and names no owner.
 *     So its dual never reached that recorder.  The per-site split (FT_SA_REC,
 *     keyed on the txn site) puts the whole bucket on that one op.
 *     -DFT_DEBUG_STRUCT_ANCHOR asked each edge's owner.  The forward edge and
 *     cn->child were held.  The dual's GRANDPARENT was not: 15,729 (per-node) /
 *     10,279 (exponential) records per ft_inv leg, every one committed.
 *     ☑ The op now takes GP (ft_lock_skip_dual_gp) in both leaf arms and derives
 *     the edges after that acquire: 0 uncovered at every spacing.  The records
 *     stay MW and still carry no owner.
 *     ☠ NOT a backtrace at the classification: backtrace_symbols_fd takes the
 *     loader lock.  From 207 threads inside the commit loop it WEDGED the
 *     leg: 0 of 152 tests in 1800 s.
 * (2) ☑ WAS FT-SLOT-1 (fixed), not a conversion question.  ft_state_edge
 *     leaves .tag 0, and ft_edge_tag defaults an untagged edge to
 *     FT_FLIP_PROXY_TAG (0xF).  So the remove's fused nr_child-- parks a
 *     0xF-tagged proxy on a word that every other producer and resolver tags
 *     FT_STATE_PROXY (0x1).  is_proxy with 0x1 accepts it, and untag yields
 *     desc|0xE, a misaligned record pointer.  Debug builds trap tag aliasing
 *     on the RECORD path only.
 * (3) ☑ FT-SLOT-3 SETTLED 2026-09-17 (Mathieu): the KIND rests on the PARENT
 *     whose slot names the child (§8.2's owner).  It no longer rests on
 *     holding the CHILD.  The two predicates no longer disagree, because
 *     there is one.
 *
 *     WHY THE PARENT.  A re-home changes WHICH SLOT names the child.  So
 *     every writer of this word rewrites that slot in the parent, and
 *     therefore holds the parent.  Only SOME also hold the child: the
 *     recompaction sweep re-homes every child of the node it copies and
 *     holds none of them.  The parent is what makes those writers exclude
 *     each other.  The child was a sufficient extra licence that left the
 *     sweep MW forever.
 *
 *     MEASURED (-DFT_DEBUG_SLOT3, per ft_inv leg), the parent is held for:
 *       - 1,246,594 of 1,246,659 records at per-node;
 *       - 1,390,410 of 1,390,475 records at exponential;
 *       - 1,272,852 of 1,272,917 records at root-only.
 *     EVERY record the child licensed was one the parent licensed too.
 *     After the switch the lane reads PARENT_WORD 65 and PSO 0 per leg at
 *     every spacing.  The 65 are compaction's own txns (nr_locks 0), whose
 *     exclusion the API still leaves to the caller.
 *     ☞ Since then compaction takes its DLM set too: a shared fine trie
 *     records no re-home without the parent.  Where the op holds no owner,
 *     the trie excludes every writer, and the word parks SW
 *     (ft_flip_txn_record_rehome_excluded).  Anything else aborts.
 * (4) ☠ See FT-SLOT-2 for this word's dropped mark check.  And MW is
 *     load-bearing here TODAY: ft_hlist_freeze_sole_prepare's derived
 *     NULL is the only thing that turns an UNHELD sole-entry derivation into
 *     an abort.  Re-derive under the holder lock before any SW park.
 * (5) ☑ FT-SLOT-4 (text corrected).  ft_promote_head already parks a head's
 *     prev SW (owner = the holder), while ft-txn-hlist.h records the same
 *     word class MW.  This is not a live race: every producer holds the
 *     holder, or is bulk-gated, or the head is build-invisible.  But
 *     "genuinely unlocked" is FALSE of this word, and the kind-stats
 *     HEAD_BACK entry now says so.
 *
 *     ☑ AND A SECOND PRODUCER HAS NOW CONVERTED: ft_glue_record_splices' store
 *     to @src_head->prev, via ft_hlist_store_sw.  Its MW was a BACKSTOP
 *     against cds_ft_compact_step, the one peer the bulk gate does not park.
 *     Compaction is LAST in the plan and excluded by its own contract, so
 *     the CAS was arbitrating against nobody.  Measured before flipping:
 *     5401 of 5401 stores with the FT-wide writer lock held inside a bulk
 *     body, 0 drain seams in-window (-DFT_DEBUG_SPLICE_SEAM).  ⇒ Keeping an
 *     MW CAS to survive an op the contract excludes is how a [debt] word
 *     stays MW forever.
 *
 *     ☠ The REST of the class is NOT converted, and must not be flipped site
 *     by site.  rcu-txn.h makes a slot SW xor MW GLOBALLY.  The point-op
 *     chain sites run against each other under per-node holder locks, with
 *     RAW producers beside them (ft_set_parent's external arm).  That is one
 *     whole-class step, not six small ones.
 * (6) The one family whose design-MW is ARGUED rather than asserted.
 * (7) ☑ THESE TWO SHARE ONE WORD, AND THAT IS THE CLASSIFICATION.  Neither is
 *     transacted, so neither took a KIND and neither was listed here.  But
 *     @incoming_byte IS written on a PLACED node: ft_set_parent_slot, from
 *     ft_set_parent AND ft_publish_to_parent, i.e. every publish path.  And
 *     it is a BITFIELD: each write is a read-modify-write of the whole tail
 *     word, @alloc_index included.  A register that lists only transacted
 *     words hides that.  That is why @alloc_index is named here and not left
 *     out.
 *
 *     The struct's own note rests the sharing on "no two writers ever race
 *     here".  The load-bearing half is narrower and checkable: @alloc_index
 *     is IMMUTABLE while the node is reachable.  The allocator writes it
 *     once (fractal-trie-alloc.c, at alloc) and never again.  It is saved
 *     and restored around the free-list memset, where the item is
 *     unreachable.  So a racing @incoming_byte RMW writes the SAME
 *     @alloc_index back and cannot lose it, whatever the byte does.
 *     ☠ A new field packed into this word would have to inherit that, and a
 *     mutable one could NOT.  It would need @incoming_byte's writers to
 *     exclude each other, which nothing here does.  A value that must be
 *     coherent with a structural flip belongs in @state.
 * (8) ☑ Written RAW, twice.  Both sites are legal by the last rule in this
 *     block (a node no reader can reach):
 *       - ft_ord_cell_alloc;
 *       - ft-compact.h's relocation.  It stores into a cell freshly returned
 *         by cds_ft_alloc_cell_item and published only by the later
 *         ft_ord_cell_swap.
 *     No site writes a LIVE cell's @node, so it never needed a kind.  It is
 *     listed because "absent from the register" and "cannot be written" are
 *     different claims, and only the second one is true here.
 * (9) Decided 2026-09-17.  This row was [DESIGN] on "the count climbs
 *     ancestors nobody locked".  That is true only of a FINE writer, and
 *     ft-lifecycle.h never lets a rank-stats trie be one: the group is
 *     coerced to CDS_FT_WRITER_LOCK_COARSE.  Every writer of the word
 *     therefore holds the FT-wide writer lock (door 1 above).  So the word
 *     may be SW under that condition, and under no other.  A rank-stats
 *     trie that ever runs FINE puts the word back to MW.
 *
 * ----------------------------------------------------------------------------
 * OPEN DEFECTS AND OPEN QUESTIONS IN THIS TABLE, tagged so they can be found:
 * grep -rn 'FT-SLOT-' src/fractal-trie/
 *
 *   FT-SLOT-1  ☑ FIXED.  A state-word TAG MISMATCH.  ft_state_edge left
 *              .tag 0, and ft_edge_tag defaults an untagged edge to
 *              FT_FLIP_PROXY_TAG (0xF).  So the remove's fused nr_child--
 *              parked a 0xF-tagged proxy on a word that every other
 *              producer and resolver tags FT_STATE_PROXY (0x1).  That
 *              includes its own caller's expected-old load two lines up.
 *              urcu_txn_is_proxy(v, 0x1) ACCEPTS 0xF.  urcu_txn_untag then
 *              yields desc|0xE, a misaligned record pointer that the
 *              resolve path dereferences.  The engine's debug net covers
 *              the RECORD path only.
 *              ☠ AND ITS REACH WAS NARROWER THAN IT LOOKED, which is the
 *              part worth keeping.  The delete arm returns -EFBIG unless
 *              ft_in_place_ok(), i.e. -DFEATURE_FT_INSERT_IN_PLACE on an
 *              EXCLUSIVE trie.  That is NOT the default, which recompacts
 *              on remove.  Measured: 0 reaches across ft_unit and ft_inv's
 *              four list/rank modes with the detector armed.  The assert
 *              fires at ft_unit test 24 of the in-place build.  So it was
 *              LATENT, never live: the one config that plants the bad tag
 *              has no concurrent resolver by that feature's own contract.
 *              In-place IS now extended to concurrent tries for the point
 *              ops (ft_in_place_ok).  That is what the assert beside the
 *              edge is armed for.
 *              ☞ ft_state_edge.  Its one caller (ft_remove_one_commit's
 *              not-held nr_child--) now records with FT_STATE_PROXY spelled
 *              explicitly and no longer builds the edge.
 *
 *   FT-SLOT-2  ☑ CLOSED IN BOTH PLACES.  The MARK CHECK is BACK in
 *              ft_hlist_insert_after_prepare, as a REFUSAL (-ENOENT), not
 *              an assert.  So the prepare once again enforces what
 *              ft-txn-hlist.h's header has always promised ("a concurrent
 *              insert_after(H) onto a sole-node chain sees the mark and
 *              aborts").  Unchecked it is a LOST INSERT.  MARK(NULL) reads
 *              back as the bare value 2.  That value passes `succ != NULL`
 *              and makes the second store record slot
 *              &((struct cds_ft_node *) 2)->prev.
 *              ft_hlist_del_prepare carries the SAME refusal.  There the
 *              shape reproduced 8/8 as a SEGV once _cds_ft_insert_replace
 *              began tombstoning the chains it displaces.
 *              ☞ THE CALLER-SIDE HALF STANDS TOO.  Both duplicate-append
 *              arms RE-VALIDATE under the holder lock and refuse a retired
 *              head (ft_node_is_removed), so no walk can END on one.  That
 *              makes two enforcement points, which is what the header
 *              describes.
 *              ☠ AND THE PREPARE-SIDE REFUSAL IS UNEXERCISED BY THE SUITE.
 *              A counter on both arms (-DFT_DEBUG_MARK_REFUSAL) reads 0
 *              across every point-op row measured 2026-09-14
 *              (insert_replace, same_key append / inserts / removes /
 *              replace, insert_unique, remove_all nolist/list/prefix).  It
 *              is a GUARD with no witness, not a path with coverage.  Do
 *              not read its presence as evidence the shape is reachable
 *              today.  Do not delete it on the strength of that zero
 *              either.
 *              ☞ ft_hlist_insert_after_prepare.  Note the sibling shapes
 *              the survey flagged there: del_prepare / replace_prepare can
 *              still carry a marked next into a LIVE pred->next.  That is
 *              excluded only by the lock and never checked.
 *
 *   FT-SLOT-3  ☑ SOUND, AND NOW THE REASON IS NAMED.  It was not named
 *              before, and that is why it read as a defect.
 *              @parent_word / @parent_slot_offset carry two ownership
 *              predicates (the model's PARENT, the code's @child_held).
 *              ft_glue_record_back_edge passes child_held TRUE
 *              unconditionally, including for a displaced PUBLISHED child
 *              (measured, from ft_merge_at_inner).  So on an armed txn it
 *              PARKS SW on a live node's parent word.
 *              That is legal, but NOT because the child is in the DLM
 *              lock-set: it is not.  Every caller is a BULK op.  Under
 *              lock_fine a POINT op RE-TAKES the FT-wide lock while a bulk
 *              op is live (FT_BULK_WIDE_LOCK + ft_bulk_active).  So bulk
 *              and point writers arbitrate on one word again: "the whole
 *              of G5.5's exclusion".
 *              ☞ THE CALLER SET, RE-ENUMERATED FROM THE TREE.  An earlier
 *              text said "merge, rekey, the glue commit".  That names a
 *              MECHANISM, not the ops that reach it, and so omitted GRAFT.
 *              There are two direct callers (ft-merge.h, ft-rekey.h), plus
 *              ft_glue_txn_commit_edges.  That function is reached from:
 *                - ft_glue_txn_commit (merge, graft x2, rekey);
 *                - ft_glue_txn_commit_replace (graft);
 *                - ft-rekey.h directly.
 *              So: MERGE, REKEY, GRAFT, all three bulk-gated.
 *              (cds_ft_detach reaches ft_glue_apply_deferred, a DIFFERENT
 *              function, and does not record a back edge.)  A future
 *              reader must re-check this list when the lock below is
 *              relaxed.  Verify it, do not trust it.
 *              The old comment said "by construction" without naming the
 *              construction, which is how this came to look unverified.
 *              ☠ SO IT IS A DEPENDENCY, NOT AN INVARIANT.  The recorded
 *              direction is to relax the FT-wide lock to only flipping the
 *              dual-descent state.  On that day this SW park loses its
 *              exclusion, and @child_held here must become a real answer
 *              about the lock-set.  Revisit it WITH that change, not after.
 *              ☞ ft_glue_record_back_edge.
 *
 *   FT-SLOT-4  ☑ CORRECTED (the texts; the code they described is unchanged
 *              except for one deletion).
 *              HEAD_BACK is documented "NEVER converts / permanent false
 *              arm".  Yet ft_promote_head parks that same word SW with
 *              owner = the holder, and ft_back_edge_owner names P as its
 *              owner.  Three statements, one slot.  Related drift:
 *                - ft_chain_next_flip is dead code whose header still
 *                  names callers.
 *                - The DUAL counter class now receives HELD duals plus
 *                  forward edges plus &ft->root.  So readings of that
 *                  column no longer mean what its definition says.
 * ----------------------------------------------------------------------------
 *
 * ☠ AND NO WORD ABOVE MAY BE WRITTEN RAW once it is reachable.  A plain store
 * does not resolve a peer's parked proxy.  It overwrites the descriptor
 * pointer and breaks atomicity for every txn naming the slot.  The lone-edge
 * fast paths (ft_ord_cell_flip_one and its callers) are raw by construction.
 * They are legal only for a bulk op under the FT-wide gate or a node no
 * reader can reach.  Each remaining raw site must say which of those two it
 * is.
 * ============================================================================
 */
struct cds_ft_metadata {
	/* 8-byte aligned fields. */
	/*
	 * ☞ Register: the MODEL owns this edge by the PARENT (§8.2, decision
	 * 09-03); the CODE keys its record kind on holding the CHILD
	 * (@child_held).  Two predicates for one word -- do not add a third, and
	 * a re-parent that already recorded it chains by the EXISTING record's
	 * kind.  MW here is a LOCK-SET REACH question (class PARENT_WORD),
	 * convertible by widening the set, never by arming.
	 * ☠ The glue passes @child_held true for every back edge, including a
	 * displaced PUBLISHED child; an SW park there is legal only while that
	 * child is in the op's lock-set.
	 */
	/*
	 * Tagged pointer to parent node.
	 *
	 * ☠ A ROOT DOES NOT STORE NULL HERE.  It stores the TRIE pointer
	 * (ft_parent_word / ft_trie_parent).  So the three states are
	 * DISTINGUISHABLE:
	 *   - a tagged node is a real parent;
	 *   - ft_parent_is_trie() is the ROOT;
	 *   - NULL is a TRANSIENT RE-HOME, between a detach / graft_swap
	 *     clearing the link and the new placement completing.
	 * An up-walk may therefore STOP at the trie and REFUSE on NULL.  It
	 * does not mistake one for the other.  That mistake is what used to
	 * make such a walk truncate silently.
	 *
	 * Such a node is not reader-reachable anyway: publication wires the
	 * parent before the node is reachable, and synchronize_rcu separates
	 * the phases.
	 *
	 * The mutation side writes this word via rcu_assign_pointer.  The read
	 * side (ft_skip_to_compressed, ft_get_parent_rcu) reads it via
	 * rcu_dereference.
	 */
	struct cds_ft_inode_flag *parent_word;
	/*
	 * ☞ Register: the PREFIX-HEAD word -- the forward edge a lookup follows
	 * to the key that ENDS at this node, and the head of its same-key chain.
	 * A structural slot of THIS node, so the excluding lock is this node's
	 * state word at per-node spacing and its ANCHOR above; SW on an armed
	 * txn, MW otherwise (MW by construction for a FINE point op above
	 * per-node).  The chain links BEHIND it are a different family entirely
	 * (cds_ft_node.next/prev, owned by the holder).
	 * ☠ With the ordered list AND rank stats both off, a lone edge on this
	 * word is committed by the raw lone-store path -- a plain store on a word
	 * other ops transact, which overwrites a parked proxy.  Every write here
	 * must resolve through the engine or hold an exclusion covering every
	 * other writer, list on or off.
	 */
	struct cds_ft_node *external_nodes;	/* List of external nodes at this trie location. */

	/*
	 * parent_slot_offset (0..255): the pointer-stride offset of this node's
	 * slot in its parent.  It is PARENT-owned per the edge principle.  It
	 * has its own word since the §8.3 split.  See the FT_PSO_* block above
	 * @state for why it left the state word.
	 *
	 * TRANSACTED: a re-home records it into the same flip-txn as
	 * &meta->parent (ft_reparent_record_meta).  So the slot can hold a
	 * parked FT_STATE_PROXY.  Every access must go through the
	 * ft_meta_parent_slot_offset* helpers, never a raw load or store.
	 *
	 * The value is stored SHIFTED (FT_PSO_SHIFT).  This keeps bit 0 clear
	 * for the engine's in-band proxy tag, as the engine requires of every
	 * live value.
	 */
	/*
	 * ☞ Register: same owner and same kind predicate as @parent_word -- the
	 * model owns it by the PARENT (§8.3 split it out of @state precisely
	 * because one word cannot be owned by two locks), the code keys its kind
	 * on holding the CHILD.  Recorded ONLY by a live re-home, into the same
	 * txn as @parent_word, so the (parent, offset) pair flips atomically; a
	 * reader must resolve the pair together, never field by field.
	 * ☠ Its raw setter waits out a parked FT_STATE_PROXY but deliberately NOT
	 * the LOCK bit; do not "fix" that -- waiting on the lock self-deadlocked.
	 */
	uintptr_t parent_slot_offset;

	/*
	 * Total unique keys in subtree.
	 * Stored with uatomic_store release, loaded with acquire.
	 */
	/*
	 * ☞ Register: the count is propagated up the ancestor chain to the root,
	 * through nodes a FINE op never acquires.  That is why a rank-stats
	 * group is COERCED to CDS_FT_WRITER_LOCK_COARSE (ft-lifecycle.h).  So
	 * every writer of this word holds the FT-wide writer lock.  The word
	 * may be SW under that condition and that condition only.  Its MW today
	 * is debt, not design.  It is stored as (count << 1).  Bit 0 is
	 * FT_NR_KEYS_PROXY_TAG.
	 * ☠ One txn may walk one ancestor TWICE (-1 then +1).  The expected old
	 * must come from the DESCRIPTOR first.  Otherwise the second walk
	 * poisons the commit, and the op livelocks with no contention at all.
	 */
	unsigned long nr_keys;

	/*
	 * Per-node MCAS state word (doc/design/mcas-multiwriter-readiness.md
	 * §4.2).  One uintptr_t so a single committed flip-latch edge can carry
	 * a node-state change:
	 *   bit 0   FT_STATE_PROXY     -- in-band flip marker (set only mid-flip;
	 *                                 lets the scalar ride the flip-latch).
	 *   bit 1   FT_STATE_TOMBSTONE -- one-way LIVE->DEAD deleted latch (§4.B).
	 *   bits 2-10  nr_child        -- live-child count (max 256, 9 bits).
	 *   bits 11-18 FREE -- they used to hold parent_slot_offset, which now
	 *                                 lives in its OWN word
	 *                                 (@parent_slot_offset above).  A word
	 *                                 cannot be owned by two locks.  The
	 *                                 offset is PARENT-owned, while this
	 *                                 word is node-owned.
	 *   bit 19     FT_STATE_LOCK  -- the reversible per-node writer lock.
	 * Access nr_child / parent_slot_offset ONLY via the ft_meta_nr_child* /
	 * ft_meta_parent_slot_offset* helpers.  They mask their own field and
	 * preserve the others.  Never read/write the word directly.
	 */
	/*
	 * ☞ THE SLOT REGISTER above is authoritative.  This word is the one
	 * with a PROTOCOL rather than a single kind.  ft-mutation-helpers.h's
	 * "THE STATE-WORD PROTOCOL" states it step by step:
	 *   - The LOCK TAKE is MW at every spacing.  It IS the arbitration
	 *     point: an SW take would hand two ops the same node.
	 *   - The tombstone, the lock release and a recorded nr_child delta
	 *     are SW on an armed txn.
	 * Under a coarse spacing the TAKE and RELEASE land on the state word of
	 * this node's ANCHOR ANCESTOR.  The tombstone and nr_child edges land
	 * here, unlocked.  What excludes them is the anchor, plus the agreement
	 * that every writer of this node resolves the same one.
	 *
	 * ☠ DO NOT SUBSTITUTE A GUARD FOR THE TAKE.  An MW {live->live} validate
	 * on this word is not arbitrated against a peer's SW park: the validator
	 * loses silently.  Ownership is taken, never observed.
	 * ☠ A state edge routed through an ft_ord_cell_edge MUST set
	 * .tag = FT_STATE_PROXY.  See note (2) in the register.
	 */
	uintptr_t state;

	/*
	 * Packed bitfield -- small fields in a single uint32_t.
	 *
	 * nr_child lives in @state above, not here.  parent_slot_offset does
	 * NOT live in @state: §8.3 gave it its OWN word (@parent_slot_offset),
	 * because a word cannot be owned by two locks.  The slot offset is
	 * PARENT-owned, while @state is node-owned.  Sharing them
	 * self-deadlocked the offset setter against its own node's LOCK bit.
	 * Access it via the ft_meta_parent_slot_offset* helpers.
	 *
	 * alloc_index:            near: FT_ALLOC_INDEX_BITS + 3 spare bits of
	 *                         headroom above the page_size >>
	 *                         FT_ALLOC_ORDER_MIN minimum; far: a separate
	 *                         uint32_t (see below).
	 */
#ifdef FT_FAR_METADATA
	/*
	 * A 2 MiB far macro-block holds far more than 256 items (e.g. ~18 700
	 * order-5 nodes).  That overflows the FT_ALLOC_INDEX_BITS (8-bit)
	 * packed field.  Store alloc_index in the struct's TAIL WORD, which it
	 * shares with @incoming_byte.  This costs no additional word and leaves
	 * every field above it untouched.  24 bits (16 M) is ample for a 2 MiB
	 * block.  The top 8 bits carry incoming_byte (below), so the tail
	 * needs no separate byte.
	 */
	/*
	 * ☞ Register: THIS TAIL WORD IS NOT TRANSACTED.  @alloc_index is written
	 * once by the allocator.  @incoming_byte is a pure function of (parent,
	 * slot) and is re-stored plainly at every child placement.  Across a
	 * recompact that is a same-value write, which is what makes an unlocked
	 * store legal.  Its only ordering is the release of the parent pointer
	 * that follows it.  So a reader's up-walk must acquire @parent_word
	 * BEFORE reading the byte.
	 * ☠ IT IS A BITFIELD: every write is a read-modify-write of the whole
	 * word.  These two fields can share it only because no two writers ever
	 * race here.  A new field packed in inherits that obligation.  A value
	 * that must be coherent with a structural flip belongs in @state.
	 */
	uint32_t alloc_index:24;
	uint32_t incoming_byte:8;
#else
	/*
	 * Near metadata: alloc_index is packed into this word, sized with
	 * 3 spare bits of headroom above the FT_ALLOC_INDEX_BITS minimum
	 * needed to index a page-sized range.
	 */
	uint32_t alloc_index:(FT_ALLOC_INDEX_BITS + 3);
	/*
	 * Branch byte on the parent->this edge: the key byte consumed entering
	 * this node.  It is maintained at every child placement.  An UPWARD key
	 * rebuild (ft_rebuild_key_upwalk) thus recovers each level's byte in
	 * O(1), with no in-leaf key copy and no parent-bitmap inversion.  This
	 * is the structural key source that lets the ordered-list walk
	 * materialize its result key WITHOUT a speculative_key_offset (e.g. an
	 * EAGER trie).
	 *   - internal node: the single incoming edge byte.
	 *   - compressed node: unused -- key_bytes[0] already IS that byte.
	 *   - external head: stored in the head's CELL metadata (the cell is the
	 *     head's metadata record), set to the key's last byte at insert.
	 * Near layout: shares the struct's tail word with @alloc_index, so it
	 * costs no additional word and leaves every field above it untouched.
	 */
	uint8_t incoming_byte;
#endif
#ifdef FEATURE_FT_HOLD_TRACE
	/*
	 * E.2 exclusion-oracle stamp: which thread currently claims this
	 * NODE's exclusion, and from where.  The claim is made at the acquire
	 * primitives (ft_hold_trace_note).  It is yielded when the covering
	 * lock's release records (ft_hold_trace_drop).
	 *
	 * The stamp is MEMBER-keyed, not anchor-keyed.  Two writers covering
	 * one node through DIFFERENT anchors both stamp HERE, and the second
	 * claim aborts with both owners named.  (This anchor disagreement is
	 * what the spacing refusal exists to prevent.)
	 *
	 * The allocator's metadata memset zeroes the stamp.  The free-poison
	 * lands only after the GP that follows the release, so a yield always
	 * precedes it.
	 */
	unsigned long dbg_owner_tid;
	const char *dbg_owner_fn;
	int dbg_owner_line;
	/*
	 * The ANCHOR the owner's site DERIVED for this member: the word it
	 * actually locked.  The member-keyed stamp exists to catch two ops
	 * covering one node through DIFFERENT anchor derivations.  Only the
	 * anchors distinguish that class from a genuine two-system collision.
	 */
	const struct cds_ft_metadata *dbg_owner_anchor;
	/*
	 * The last anchor ANY op derived for this node, kept across yields.
	 * At a fixed spacing the anchor is a function of the node and its
	 * DEPTH.  So two ops deriving different words for one node is a
	 * derivation disagreement.  That class leaves each op excluding on a
	 * word the other never takes.  Detecting it here needs no collision.
	 * So it measures the DISAGREEMENT RATE, instead of waiting for two ops
	 * to lose the race on the same node.
	 */
	const struct cds_ft_metadata *dbg_last_anchor;
#endif
};

/*
 * nr_child accessors -- nr_child lives in @state bits 2+, with the proxy
 * (bit 0) and tombstone (bit 1) tags below it.  Reads shift the tags out;
 * writes preserve them.  Inc/dec add/subtract one count unit, which leaves
 * the low tag bits untouched.
 */
static inline
unsigned int ft_state_nr_child(uintptr_t state)
{
	return (unsigned int) ((state >> FT_STATE_NR_CHILD_SHIFT)
			& FT_STATE_NR_CHILD_VALMASK);
}

static inline
unsigned int ft_meta_nr_child(const struct cds_ft_metadata *meta)
{
	return ft_state_nr_child(meta->state);
}

static inline
void ft_meta_nr_child_set(struct cds_ft_metadata *meta, unsigned int n)
{
	/* Replace only the nr_child field; preserve tags + parent_slot_offset. */
	meta->state = ((uintptr_t) n << FT_STATE_NR_CHILD_SHIFT)
		| (meta->state & ~FT_STATE_NR_CHILD_MASK);
}

/*
 * Add or subtract one from nr_child, as a LATCH-HONORING CAS transition
 * (Phase 4.3).
 *
 * The former plain read-modify-write raced with concurrent writers on SHARED
 * nodes.  The set_nth type arms run on a LIVE published node whenever an
 * in-place insert (the case-0 reserve) targets a spine node that two writers
 * share.  A plain += then fails in two ways:
 * - It LOSES increments.  nr_child drops below the popcount, which feeds
 *   garbage to every rank-bounded walk.
 * - Worse, it does ARITHMETIC ON A PARKED FT_STATE_PROXY pointer when a
 *   peer's state edge is mid-commit.  The result is a corrupted near-pointer
 *   with tag bits, and a resolver then follows it.
 *
 * The CAS loop re-derives the value from the current word.  It waits for a
 * parked proxy to clear; the owner's settle bounds that wait.  This mirrors
 * ft_meta_state_transition.  A fresh single-owner node pays one uncontended
 * CAS.
 */
static inline
void ft_meta_nr_child_inc(struct cds_ft_metadata *meta)
{
	for (;;) {
		uintptr_t s = CMM_LOAD_SHARED(meta->state);

		if (caa_unlikely(s & FT_STATE_INPLACE_WAIT_MASK)) {
			caa_cpu_relax();
			continue;
		}
		if (caa_likely(uatomic_cmpxchg(&meta->state, s,
				s + FT_STATE_NR_CHILD_ONE) == s))
			return;
	}
}

static inline
void ft_meta_nr_child_dec(struct cds_ft_metadata *meta)
{
	for (;;) {
		uintptr_t s = CMM_LOAD_SHARED(meta->state);

		if (caa_unlikely(s & FT_STATE_INPLACE_WAIT_MASK)) {
			caa_cpu_relax();
			continue;
		}
		if (caa_likely(uatomic_cmpxchg(&meta->state, s,
				s - FT_STATE_NR_CHILD_ONE) == s))
			return;
	}
}

/*
 * Return parent_slot_offset: the offset of this node's slot in its parent
 * body, in pointer strides.
 *
 * The offset is held in its OWN word (@parent_slot_offset), NOT in @state.
 * §8.3 split it out.  @state keeps nr_child and the lock/proxy bits.
 *
 * This is the raw reader.  It suits a node that the caller owns or that is
 * quiescent.  Other callers use a different reader:
 * - A reader that may race with a mid-commit proxy uses the resolving
 *   ft_meta_parent_slot_offset_load.
 * - A backtracker that recovers the (parent, offset) PAIR must use
 *   ft_resolve_parent_slot (same-mcas + stability re-read).
 *
 * Root nodes (parent == NULL) leave this 0 and never read it.
 */
static inline
unsigned int ft_meta_parent_slot_offset(const struct cds_ft_metadata *meta)
{
	return FT_PSO_DECODE(meta->parent_slot_offset);
}

/*
 * Set parent_slot_offset.
 *
 * The setter owns the whole word now: the §8.3 split moved the offset out of
 * @state.  So it no longer has to preserve nr_child or the state tag bits.
 *
 * It must still respect the ENGINE.  This slot is transacted, so a peer can
 * have an FT_STATE_PROXY parked here.  A blind store over a proxy pointer
 * would drop a live descriptor that a resolver is about to follow.  So wait
 * for the proxy to clear; do not skip it.  The offset must land on the
 * SETTLED word.  The owner's settle bounds the wait.
 *
 * ★ The wait is on FT_STATE_PROXY ALONE, deliberately.  It is NOT on
 * FT_STATE_INPLACE_WAIT_MASK.  That mask includes FT_STATE_LOCK under the
 * DLM lock-sets.  While the offset shared @state, an op that held this
 * node's own node lock spun on itself forever.  That self-deadlock is what
 * reverted b20c471e.  It was single-threaded, in ft_unit
 * test_lookup_nth_varlen: a prefix insert builds a glue node and re-parents
 * a node that the op has locked.  The offset word has no LOCK bit to wait
 * on.  That is the whole point of the split, so DO NOT reintroduce that mask
 * here.
 *
 * A LIVE, reader-reachable node must not come through here at all.  Its
 * re-home records the offset into the same flip-txn as &meta->parent
 * (ft_reparent_record_meta), so the pair flips atomically.  This setter is
 * for two kinds of node:
 * - fresh or invisible nodes;
 * - the bulk-op roots that still run under the application's mutual
 *   exclusion between mutators (detach root, graft branch re-root).
 *
 * Skip the store when the offset is unchanged.  The same-value republish of
 * the in-place reserve is the hot case.
 */
#ifdef FT_DEBUG_PAIR_STORE
/*
 * The glue's same-op torn-window ring (ft-helpers.h, ft_dt_glue_check).  A
 * bulk op's glue fills it.  It is cleared when the op's outermost FT-wide
 * writer scope releases.  So an entry never outlives the op that noted it.
 */
#define FT_DT_GLUE_RING	128
static __thread struct {
	const struct cds_ft_metadata *meta;
	struct cds_ft_inode_flag *new_parent;
} ft_dt_glue_ring[FT_DT_GLUE_RING];
static __thread unsigned int ft_dt_glue_ring_n;
#endif
#if defined(FT_DEBUG_DEL_TOMB) || defined(FT_DEBUG_PAIR_STORE)
/*
 * PROBE: score the rule that this setter's header states.  A LIVE,
 * reader-reachable child must not come through here.  The probe is defined
 * next to ft_slot_in_node in ft-helpers.h, where the predicates it needs
 * live.
 * -DFT_DEBUG_PAIR_STORE builds these two probes alone, without the rest of the
 * FT_DEBUG_DEL_TOMB family.
 */
static void ft_dt_pso_store_probe(const struct cds_ft_metadata *meta,
		unsigned int off);
/* The same question for @incoming_byte, which the up-walk reads beside the parent. */
static unsigned int ft_dt_ib_store_probe(const struct cds_ft_metadata *meta,
		unsigned int byte);
# define FT_DT_IB_STORE(meta, byte)	ft_dt_ib_store_probe((meta), (byte))
#else
# define FT_DT_IB_STORE(meta, byte)	(byte)
#endif

static inline
void ft_meta_parent_slot_offset_set(struct cds_ft_metadata *meta, unsigned int off)
{
	uintptr_t n = FT_PSO_ENCODE(off);

#if defined(FT_DEBUG_DEL_TOMB) || defined(FT_DEBUG_PAIR_STORE)
	ft_dt_pso_store_probe(meta, off);
#endif
	for (;;) {
		uintptr_t s = CMM_LOAD_SHARED(meta->parent_slot_offset);

		if (caa_unlikely(s & FT_STATE_PROXY)) {
			caa_cpu_relax();
			continue;
		}
		if (n == s)
			return;		/* same-value republish: nothing to do */
		if (caa_likely(uatomic_cmpxchg(&meta->parent_slot_offset, s, n) == s))
			return;
	}
}

/*
 * Read the one-way LIVE->DEAD tombstone (state bit 1, §4.B freeze-on-free).
 * True once the node has been marked dead at retire (before its unlink commit).
 */
static inline
bool ft_meta_tombstone(const struct cds_ft_metadata *meta)
{
	return (meta->state & FT_STATE_TOMBSTONE) != 0;
}

/*
 * Compressed path node.  Replaces a chain of single-child internal
 * nodes with a single node storing the key bytes inline.
 *
 * cn->child can point to any node type: internal, compressed,
 * or external.  An external child means a key terminates at the
 * end of the compressed path.  However, the compressed node's
 * metadata->external_nodes must NOT be used.  A variable-length key
 * entry belongs in one of two places:
 * - on an internal node, which can have metadata->external_nodes;
 * - as the child pointer of a compressed node.
 *
 * Tagged in the parent's child pointer with FT_COMPRESSED_MASK
 * (bit 1 set, bit 0 clear).
 *
 * Layout: [child pointer] [len] [key_bytes...]
 */
struct cds_ft_compressed_node {
	/*
	 * ☞ Register: @child is this node's OWN body word.  So the lock that
	 * excludes a writer is this cn's state word at per-node spacing, and
	 * its ANCHOR above that spacing.  The record is SW on an armed txn
	 * and MW otherwise.  It is MW by construction for a FINE point op
	 * above per-node.
	 *
	 * ☠ IT NEVER TRAVELS ALONE.  While this cn is reached through a SKIP_X
	 * word, the GRANDPARENT slot encodes THIS value.  A republish must
	 * refresh both in ONE commit.  The GRANDPARENT owns the dual, not
	 * this node.  So hold GP (ft_lock_skip_dual_gp /
	 * ft_insert_lock_skip_dual_gp) before recording it.
	 *
	 * The reason: a recompaction of GP copies its body under GP's lock.
	 * On the strength of that lock it drops the per-slot read-set.  So a
	 * dual recorded without holding GP lands in the body being retired,
	 * while the fresh copy keeps the stale word.  This field and the
	 * reachable dual then disagree forever.
	 */
	struct cds_ft_inode_flag *child;	/* Child at end of compressed path. */
	uint8_t len;				/* Number of key bytes in path (1-255). */
	uint8_t key_bytes[];			/* Compressed key path (flexible array). */
};

struct cds_ft_bitmap {
	/*
	 * Bitmap is attached to pigeon nodes only.  Pigeon has no key
	 * array and no internal bitmap header.  The bitmap-scan path
	 * needs this metadata bitmap to find populated slots without
	 * a 256-pointer linear scan.  Cost: 2 cache line loads + 1
	 * extra TLB hit, compared to 32 cache line loads worst case
	 * without the bitmap.  Popcount tiers carry their own bitmaps
	 * inline in the node body and do not need a metadata bitmap.
	 *
	 *   Pigeon bitmap memory use (32-bit): 32 B / 1024 B node =  3.1%
	 *   Pigeon bitmap memory use (64-bit): 32 B / 2048 B node =  1.6%
	 */
	unsigned long bitmap[FT_BITMAP_LEN / sizeof(unsigned long)];
} __attribute__((__aligned__(FT_BITMAP_LEN)));

/*
 * Per-group byte collation map.  It is a bijection between raw key bytes
 * and their ordinal (sort) position.  It lets the trie order keys by the
 * application's collation instead of by raw byte value.  @key_to_ordinal
 * and @ordinal_to_key are inverse 256-entry permutations.  @identity is
 * set when the map is the identity (byte == ordinal).  The hot paths
 * special-case it to skip the remap.
 */
struct cds_ft_key_map {
	bool identity;
	uint8_t key_to_ordinal[256];
	uint8_t ordinal_to_key[256];
};

/*
 * A group of Fractal Trie instances (struct cds_ft).  The tries of a group
 * share:
 * - the allocation arenas;
 * - a key collation map;
 * - the creation-time configuration (key length, lookup optimization,
 *   NUMA / page-size policy, ordered-list mode).
 *
 * The bulk operations (graft, graft_swap, detach, merge) move whole
 * sub-tries between tries of the SAME group.  That is why the arenas and
 * the leak accounting live here and not per-trie.  A group is created by
 * cds_ft_group_create and configured via cds_ft_group_attr_create.
 * Individual tries are added with cds_ft_create.
 */

/*
 * Node-allocation reserve (see cds_ft_group::active_reserve).  It holds
 * nodes that a bulk op pre-allocated.  The op's commit phase can then draw
 * them without an arena allocation that could fail.  The reserve is keyed by
 * [kind][item_len_order]:
 * - kind 0 is the regular internal-node arena (CDS_FT_ALLOC_KIND_NODE);
 * - kind 1 is the compressed-node arena (CDS_FT_ALLOC_KIND_COMPRESSED).
 *
 * Cells are never reserved: the bulk ops move existing cells and do not
 * allocate new ones.  The per-bucket capacity is small, because the exact
 * node need of each bulk op is O(1) per (kind, order).  Reserve fills assert
 * against it.
 *
 * There is ONE exception, without FEATURE_FT_COMPRESS.  ft_build_branch
 * cannot fold a branch into one compressed node.  So it builds a chain of
 * single-child nodes, one per key byte.  That is up to FT_MAX_KEY_LEN nodes
 * of the smallest node type, not O(1).  The excess of that bucket lives in
 * @branch (cds_ft_alloc_reserve_add_branch):
 * - the draw path takes from @branch once the ordinary bucket is empty;
 * - a refund spills into @branch once the ordinary bucket is full.
 */
#define CDS_FT_ALLOC_RESERVE_NR_KIND	2
#define CDS_FT_ALLOC_RESERVE_CAP	8
#ifndef FEATURE_FT_COMPRESS
#define CDS_FT_ALLOC_RESERVE_BRANCH_CAP	FT_MAX_KEY_LEN
#endif

enum cds_ft_alloc_kind {
	CDS_FT_ALLOC_KIND_NODE = 0,
	CDS_FT_ALLOC_KIND_COMPRESSED = 1,
	CDS_FT_ALLOC_KIND_CELL = 2,	/* never reserved */
};

struct cds_ft_alloc_reserve {
	struct cds_ft_metadata *items[CDS_FT_ALLOC_RESERVE_NR_KIND]
			[FT_ALLOC_ORDER_MAX + 1][CDS_FT_ALLOC_RESERVE_CAP];
	unsigned int count[CDS_FT_ALLOC_RESERVE_NR_KIND][FT_ALLOC_ORDER_MAX + 1];
#ifndef FEATURE_FT_COMPRESS
	/* Excess of the (NODE, @branch_order) bucket; 0 until add_branch. */
	struct cds_ft_metadata *branch[CDS_FT_ALLOC_RESERVE_BRANCH_CAP];
	unsigned int branch_count;
	size_t branch_order;
#endif
};

struct cds_ft_group {
	size_t max_tree_depth;
	size_t key_len;
	size_t max_key_len;		/* Maximum key length allowed. */
	unsigned int flags;		/* CDS_FT_FLAG_* creation-time flags. */
	const struct rcu_flavor_struct *flavor;
	/*
	 * Structural-writer concurrency strategy for the group's tries (MW
	 * lock-escalation model).  Copied to each trie at create; a locking
	 * strategy (COARSE or FINE) makes the trie take the FT-wide writer lock
	 * at every mutation.  Default CDS_FT_WRITER_LOCK_FINE (DLM), resolved at
	 * group create from @writer_strategy_set.
	 */
	enum cds_ft_writer_strategy writer_strategy;
	/* Allocation arenas. */
	struct cds_ft_alloc_arena *arena_order[FT_ALLOC_ORDER_MAX + 1];
	/*
	 * Separate arena set for compressed nodes in skip-compressed
	 * groups (speculative=true).  Cand-mode descent never reads
	 * compressed-node bodies: skip-encoded pointers carry skip_len
	 * in the high bits, and the descent jumps past the compressed
	 * key bytes.  In a shared arena, compressed nodes would
	 * pollute the same allocator pages as the internal nodes that
	 * ARE on the descent hot path.  Routing compressed allocations
	 * to a dedicated arena keeps them off the descent's I/D-cache
	 * and TLB working set.  NULL slots fall back to arena_order[]
	 * for groups where speculative is false.
	 */
	struct cds_ft_alloc_arena *compressed_arena_order[FT_ALLOC_ORDER_MAX + 1];
	/*
	 * Dedicated arena for ordinal cells (ordered_list_set groups).
	 * It is separate from arena_order[] so that the uniform 32 B cell
	 * bodies pack contiguously in their own item region.  That is the
	 * dense ord-walk stride that cds_ft_compact exploits.  In
	 * arena_order[] the cells would be diluted among the internal-node
	 * ranges.  The arena is created lazily on the first cell allocation.
	 * It has a single order, because the cell is a uniform 32 B.
	 * cds_ft_free_item recovers the range (and its rcu_head) from the
	 * cell pointer.  The range serves the reader-safe deferred reclaim
	 * and the nr_live draining.
	 */
	struct cds_ft_alloc_arena *cell_arena;
	/*
	 * Cell leak accounting (ft_debug_counters only).  Cells migrate
	 * between the group's tries via the bulk ops: a cell is allocated
	 * in one trie and freed in another.  So they are accounted at
	 * group granularity.  cds_ft_group_destroy reports a mismatch.
	 * A free is counted synchronously at the free request, not at the
	 * deferred reclaim.  So allocated == freed means that the free of
	 * every cell was issued.
	 */
	unsigned long nr_cells_allocated, nr_cells_freed;
	/*
	 * Node leak accounting (ft_debug_counters only).  Like the cells
	 * above, internal and compressed nodes migrate between the group's
	 * tries via the bulk ops.  A graft allocates a node accounted to
	 * the source trie and frees it accounted to the destination.  So
	 * nodes are accounted at group granularity.  cds_ft_group_destroy
	 * reports a mismatch once every trie has drained its deferred frees.
	 */
	unsigned long nr_nodes_allocated, nr_nodes_freed;
	unsigned long nr_internal_alloc, nr_internal_freed;
	unsigned long nr_compressed_alloc, nr_compressed_freed;
	/* up-walks that started on a duplicate-chain MEMBER (ft_ext_head_word) */
	unsigned long nr_ext_member_hops;
	pthread_mutex_t arena_lock;	/* Protects lazy arena creation. */
	struct cds_ft_key_map key_map;
	unsigned long nr_ft_instances;	/* Number of Fractal Trie instances in the group. */

	/*
	 * @speculative: SPECULATIVE lookup optimization (default).  When
	 *   true, the group's compressed-node allocations are routed to a
	 *   dedicated arena set for improved descent locality.
	 *   Orthogonal to CDS_FT_FLAG_SKIP_COMPRESSED (the skip-encoded
	 *   pointer optimization, which is also gated on
	 *   arch-availability).  See
	 *   cds_ft_group_attr_set_lookup_optimization.
	 */
	bool speculative;
	/*
	 * @speculative_key_offset: byte offset from the (struct cds_ft_node *)
	 *   stored in the trie to the caller's key bytes.  The speculative
	 *   inequality lookup uses it to copy a result key directly from the
	 *   leaf.  The leaf is the single source of the key, so the descent
	 *   need not load compressed-node cache lines to rebuild it.
	 *   @speculative_key_offset_set records whether it was configured.
	 *   Offset 0 is a valid value, so a separate flag is required.  Only
	 *   consulted when @speculative and CDS_FT_FLAG_SKIP_COMPRESSED are
	 *   set.  See cds_ft_group_attr_set_speculative_key_offset.
	 */
	size_t speculative_key_offset;
	bool speculative_key_offset_set;
	/*
	 * @key_len_offset: byte offset from the (struct cds_ft_node *) to a
	 *   size_t holding the leaf's key length.  @key_len_offset_set records
	 *   whether it was configured.  The offset lets the ordered cell list
	 *   materialize a VARIABLE-length result key from the leaf.  The
	 *   descent normally derives the length as it walks, but the cell
	 *   fast path skips the descent.  Unused for fixed-length groups,
	 *   where the length is group->key_len.  See
	 *   cds_ft_group_attr_set_key_len_offset.
	 */
	size_t key_len_offset;
	bool key_len_offset_set;
	/*
	 * @ordered_list_set: enable the library-owned ordered sibling list
	 *   (FEATURE_FT_ORD_CELL).  The order links live in a library-owned
	 *   relocatable "ordinal cell" (struct ft_ord_cell).  The cell hangs
	 *   off the cds_ft_node.prev of each duplicate-chain head.  The
	 *   application leaf is unchanged: no ord fields, no offset to
	 *   declare.  The result key is materialized in one of two ways:
	 *   - from the leaf, when speculative_key_offset is configured (plus
	 *     key_len_offset for a variable-length group);
	 *   - otherwise structurally, by the parent up-walk
	 *     (ft_rebuild_key_upwalk).  The walk recovers ordinal bytes, so
	 *     it serves any key map.
	 *   See cds_ft_group_attr_set_ordered_list.
	 */
	bool ordered_list_set;

	/*
	 * @rekey_set: the application opted this group's tries into the in-trie
	 * MOVE ops (cds_ft_rekey_graft / cds_ft_rekey_merge).  DEFAULT OFF --
	 * see cds_ft_group_attr_set_rekey.  A trie that cannot host a move
	 * needs no reader coherence at all.  So this also decides
	 * @rekey_coherence, and with it the lookup specializations that
	 * ft_install_lookup_ops selects.
	 */
	bool rekey_set;
	/*
	 * @rank_stats_set: maintain the per-node order-statistics key counts
	 *   (cds_ft_metadata.nr_keys).  They back the rank / select / count
	 *   queries (cds_ft_count_keys / _prefix, cds_ft_lookup_nth / _last,
	 *   cds_ft_iter_skip_forward / _reverse).  Default OFF.  When off,
	 *   the library performs no count propagation and the queries fall
	 *   back:
	 *   - count uses a full enumeration;
	 *   - select / skip use first/last + next/prev iteration.
	 *   Immutable after group creation.  See
	 *   cds_ft_group_attr_set_rank_stats.
	 */
	bool rank_stats_set;
	/*
	 * @numa_policy: NUMA placement policy for the group's internal
	 *   allocator superblocks.  See
	 *   cds_ft_group_attr_set_numa_policy.  Default: INTERLEAVE at
	 *   2 MiB chunk granularity.  Overridden by the env var
	 *   CDS_FT_NUMA_INTERLEAVE=0 to force LOCAL placement (for
	 *   benchmarking without recompiling).
	 */
	enum cds_ft_numa_policy numa_policy;
	/*
	 * @optimize: page-size policy for the internal + compressed node
	 *   arenas (CDS_FT_OPTIMIZE_THROUGHPUT -> 2 MiB, _RSS -> 4 KiB).
	 *   See cds_ft_group_attr_set_optimize.  Default: THROUGHPUT.
	 */
	enum cds_ft_optimize optimize;
};


/*
 * Per-API lookup function pointers, cached on struct cds_ft.  The
 * public entry points dispatch via one indirect tail-call to a
 * fully-specialized inner.  There is no per-call runtime branch on
 * group shape: the library picks the right inner once at cds_ft_create
 * and caches it here.  See ft_install_lookup_ops in fractal-trie.c.
 *
 * Each pointer's signature matches the corresponding cds_ft_*
 * public API.  The pointer is stable for the lifetime of the trie,
 * because the group flags (key_map.identity,
 * CDS_FT_FLAG_SKIP_COMPRESSED bit) are immutable after group creation.
 * So the whole dispatch cost on the hot path is a single load +
 * indirect jmp.  Runtime-gated dispatch showed a ~22 ns regression.
 */
struct cds_ft;
struct cds_ft_node;
struct cds_ft_iter;
typedef enum cds_ft_status (*cds_ft_lookup_key_fn)(
		struct cds_ft *ft, const uint8_t *key,
		size_t key_len, size_t key_readable_pad,
		struct cds_ft_node **result_node);
typedef enum cds_ft_status (*cds_ft_lookup_iter_fn)(
		struct cds_ft *ft, struct cds_ft_iter *iter);
/*
 * Signature shared by cds_ft_lookup_partial_key and
 * cds_ft_lookup_longest_match_key: returns the partial/longest match
 * length via @match_len and the matched node via @result_node.
 */
typedef enum cds_ft_status (*cds_ft_lookup_prefix_key_fn)(
		struct cds_ft *ft, const uint8_t *key,
		size_t key_len, size_t *match_len,
		struct cds_ft_node **result_node);

struct cds_ft_compact_state;

/*
 * A single Fractal Trie instance: one ordered map from byte keys to
 * application-owned nodes.  It is rooted at @root and belongs to a
 * @group, whose arenas and configuration it uses.  It caches:
 * - the specialized per-API lookup dispatch pointers (installed once at
 *   cds_ft_create);
 * - the access-discipline mode (@exclusive vs concurrent RCU readers);
 * - the ordered-list mirror flag, with the cached list endpoints;
 * - any in-progress compaction state.
 * Created by cds_ft_create.
 */
struct cds_ft {
	struct cds_ft_group *group;

	/*
	 * ☞ Register: THE ROOT SLOT IS THE ONE CAS-ONLY STRUCTURAL WORD.  It
	 * lives in no node, so no state word can make a plain park legal at ANY
	 * spacing -- root-only spacing locks the root NODE's state word, which is
	 * a different word.  Every record of it is MW whatever the txn's mode, so
	 * route it through ft_flip_txn_record_root or carry the per-edge .root /
	 * rec->root[] flag so the replays do.  MW by DESIGN: it never converts.
	 * ☠ The LONE-EDGE fast paths store this word RAW (ft_ord_cell_flip_one
	 * and its callers).  That is legal only for a bulk op under the FT-wide
	 * gate: a raw store does not resolve a peer's parked proxy, it overwrites
	 * the descriptor pointer.  A point op reaching a raw root store has left
	 * the engine's atomicity for everyone.
	 */
	struct cds_ft_inode_flag *root;		/* Root node (arena-allocated, always present, always internal). */

	/*
	 * Specialized lookup dispatch pointers -- installed at
	 * cds_ft_create based on group flags.  See the typedef
	 * comment above.  Placed near @root so a single cache-line
	 * fetch on lookup entry serves the descent.
	 */
	cds_ft_lookup_key_fn lookup_key_fn;
	cds_ft_lookup_key_fn lookup_candidate_key_fn;
	cds_ft_lookup_iter_fn lookup_iter_fn;
	cds_ft_lookup_prefix_key_fn lookup_partial_key_fn;
	cds_ft_lookup_iter_fn lookup_partial_iter_fn;
	cds_ft_lookup_prefix_key_fn lookup_longest_match_key_fn;
	cds_ft_lookup_iter_fn lookup_longest_match_iter_fn;
	/*
	 * Limit-none relational entries (cds_ft_lookup_le/ge/lt/gt, and
	 * cds_ft_next/prev which resolve to gt/lt).  ft_install_lookup_ops
	 * installs them once at create, to the specialization that fits
	 * use_keycopy.  So the public entry is a single indirect tail-call
	 * with no per-call config branch.  first/last stay on the inline
	 * dispatcher (cold path).
	 */
	cds_ft_lookup_iter_fn lookup_le_fn;
	cds_ft_lookup_iter_fn lookup_ge_fn;
	cds_ft_lookup_iter_fn lookup_lt_fn;
	cds_ft_lookup_iter_fn lookup_gt_fn;

	size_t max_used_key_len;		/* Maximum key length inserted (conservative). */
	/*
	 * The value that @max_used_key_len held immediately after the last
	 * EXACT walk (ft_recompute_max_used_key_len).  With it a repair can
	 * tell an INFLATED hint from a merely LARGE one.  Equal means "already
	 * walked at this value": nothing has raised the hint since.  A walk
	 * would then recompute the same number, so the refusal it feeds is
	 * TRUE, not an artifact.
	 *
	 * ☠ WITHOUT IT THE REPAIR IS AN O(n) STALL PER CALL.  The
	 * unequal-length gate of the same-trie rekey is also true whenever the
	 * trie genuinely HOLDS a long key.  A refused move raises nothing.  So
	 * every repeated call walked the whole trie under the FT-wide lock,
	 * for a refusal that was correct the first time.  MEASURED at 300k
	 * keys plus one 255-byte key:
	 * - 0.005 ms/call before the repair existed;
	 * - 232.6 ms/call with the unguarded repair;
	 * - concurrent point inserters fell from 16,841/s to 562/s behind
	 *   the lock.
	 *
	 * It is written only where that same walk writes @max_used_key_len,
	 * hint first.  A peer raise that lands between the two stores leaves
	 * them UNEQUAL.  That costs one extra walk and never skips a needed
	 * one.  Both are relaxed.  The pair is a heuristic about the hint, not
	 * a lock.
	 */
	size_t max_used_key_len_walked;

	/*
	 * The active node-allocation reserve is NOT stored here.  It is the
	 * bulk-op OOM-avoidance pool for merge_at's sub-position residual /
	 * graft.  It is THREAD-LOCAL (fractal-trie-alloc.c, ft_tls_reserves[]).
	 * A reserve is a stack-local owned by one bulk op on one thread.  Its
	 * activation state is per-thread, so concurrent bulk ops on the same
	 * live dst (post FT-wide-lock drop, §11) each own their reserve.  They
	 * do not collide on a shared field.  Query via
	 * cds_ft_alloc_reserve_covers().
	 */

	/*
	 * Access discipline. When true, access is serialized
	 * externally (single-threaded or mutex-protected) and no
	 * concurrent RCU readers exist.  Some mutation operations
	 * (graft, graft_swap) would otherwise require a grace period
	 * to re-parent a subtree.  They may then skip
	 * synchronize_rcu().  When false, concurrent RCU readers are
	 * permitted.
	 */
	bool exclusive;

	/*
	 * Mirror of group->ordered_list_set, cached on the trie.  The
	 * read-side head-parent resolver (ft_resolve_head_prev, on the descent
	 * / skip / backtrack hot paths) tests one local flag instead of
	 * chasing ft->group->ordered_list_set.  It is immutable after
	 * cds_ft_create, because the group's mode is fixed at attr time.  So a
	 * concurrent reader's branch is race-free.  When false the trie
	 * allocates NO ordinal cells: a head's prev IS its flagged parent
	 * directly, exactly as in a non-cell build.  The per-key cell RSS and
	 * its list maintenance are paid only when the list is enabled.
	 */
	bool ordered_list;

	/*
	 * Mirror of group->rank_stats_set, cached on the trie.  The
	 * order-statistics query hot paths and the per-mutation count
	 * propagation test one local flag instead of chasing ft->group.
	 * Immutable after cds_ft_create.  When false the trie maintains no
	 * per-node nr_keys, and the rank / select / count queries use the
	 * enumeration / iteration fallback.
	 */
	bool rank_stats;

	/*
	 * Effective per-trie speculative-leaf-key state.  It is true when
	 * the group has a speculative_key_offset AND this trie was NOT
	 * created with cds_ft_attr_set_speculative_keys(attr, false).
	 *
	 * When false, every lookup on this trie reconstructs the result key
	 * from the trie structure (the EAGER path).  It NEVER reads the
	 * leaf's stored key field.  So the trie may safely hold leaves whose
	 * stored key does not match their position:
	 * - a staging graft source, whose leaves are stamped with their
	 *   future destination key;
	 * - a detach result, whose leaves carry their pre-detach key at a
	 *   now-stripped position.
	 *
	 * Set at create before ft_install_lookup_ops and immutable after, so
	 * a concurrent reader's branch is race-free.  It mirrors
	 * group->speculative_key_offset_set, but per-trie, so that individual
	 * tries in a speculative group can opt out.
	 */
	bool speculative_key_offset_active;

	/*
	 * REKEY (in-trie move) coherence, opt-in per-trie (doc:
	 * rekey-second-walk).
	 *
	 * When true, every EXACT lookup on this trie runs a second walk: the
	 * parent-pointer up-walk that rematerializes the key from the leaf.
	 * The lookup compares those bytes against the ones it descended
	 * with.  A mismatch means that a concurrent rekey restructured the
	 * descent's path, so the reader re-descends from the root.
	 *
	 * This is what lets a same-trie merge_at move a subtree with a
	 * fallible reattach commit.  Readers self-verify instead of relying
	 * on a synchronize_rcu gap or a global seqcount.  It turns this
	 * trie's reads from wait-free into lock-free (bounded by writer move
	 * progress).
	 *
	 * It is set at create from the rekey opt-in and an EAGER trie (no
	 * in-leaf key), with the ordered list on or off.  It is immutable
	 * after ft_install_lookup_ops, because the
	 * coherent lookup specializations are keyed off it.  So a concurrent
	 * reader's branch (really the fn-ptr choice) is race-free.
	 */
	bool rekey_coherence;

	/*
	 * ALWAYS-COHERENT: readers on this trie take the two-pass path
	 * unconditionally.  They never consult @move_active (ft_move_active
	 * returns true outright).  Set for an EXTERNAL_SYNC trie that opted
	 * into rekey.
	 *
	 * ☠ IT IS NOT A PERFORMANCE KNOB -- IT IS WHAT MAKES REKEY LEGAL THERE.
	 * The move gate's discipline has three steps: publish @move_active,
	 * wait ONE FULL GRACE PERIOD, only then mutate.  The GP is there so
	 * that no reader is still running under the old rule.  Under
	 * CDS_FT_WRITER_EXTERNAL_SYNC that grace period is taken while the
	 * APPLICATION holds its own writer exclusion.  The library can
	 * neither drop that lock nor place itself outside it.  That is
	 * precisely the deadlock that ft_writer_lock_gp_wait exists to avoid
	 * for the library's OWN mutex ("writers parked on the lock are
	 * RCU-online and non-quiescent, so they are precisely what stops this
	 * grace period from ever completing").  Measured 2026-09-20: 2 gate
	 * grace periods per pair of rekeys, taken under the app's lock.
	 *
	 * With every reader ALREADY on the coherent path, no reader runs
	 * under the old rule.  So there is nothing to wait for, and the gate
	 * owns no grace period at all.  The move gate pays one GP to keep
	 * the steady-state reader on the fast path.  In this mode that trade
	 * is the wrong way round.
	 *
	 * The price: every exact lookup on such a trie always runs the
	 * second walk.  That is the cost of rekey under an exclusion that
	 * the library does not own.
	 */
	bool rekey_always_coherent;

	/*
	 * MOVE MODE GATE (doc/design: the per-trie move refcount).  An in-trie
	 * MOVE (rekey) cannot be made coherent for free on the read side.  It
	 * must also not make the STEADY-STATE reader pay.  So readers run in
	 * one of two modes, and this word is the mode.
	 *
	 *   @move_active == 0  =>  no move can be in flight: readers take the FAST
	 *                          path and perform NO coherence check at all.
	 *   @move_active != 0  =>  readers take the COHERENT path.
	 *
	 * A GRACE PERIOD makes the switch safe, not ordering tricks.  A mover
	 * sets @move_active, then waits a GP, and only THEN mutates the
	 * structure.  So a reader cannot observe move-induced incoherence
	 * while it believes it is in fast mode.  If it sampled 0, the mover's
	 * GP still waits for that reader's own critical section to end before
	 * the mover touches anything.  For the same reason, one sample per
	 * read section is enough.
	 *
	 * The cost is confined to move windows.  The steady state pays ONE
	 * STABLE READ of a word nobody writes.  That is the whole point of a
	 * refcount here, and not a per-step sequence counter:
	 * - no shared-write cache-line bounce on the read path;
	 * - no unbounded reader retry under a stream of moves.
	 *
	 * @move_gate_lock / @move_gate_cond / @move_gate_nr / @gate_gp_nr
	 * serialize the movers and let them PIGGYBACK.  Only the 0->1
	 * transition pays a grace period.  A mover that arrives while that GP
	 * is in flight waits for it instead of starting its own.  So a burst
	 * of moves costs ~one GP in total.  This is the batching shape of
	 * src/urcu-call-rcu-impl.h's splice + single synchronize_rcu.  Access
	 * only through ft_move_gate_enter / ft_move_gate_exit /
	 * ft_move_active.
	 */
	unsigned long move_active;
	pthread_mutex_t move_gate_lock;
	pthread_cond_t move_gate_cond;
	unsigned long move_gate_nr;	/* COHERENT holders (rekey) */
	/*
	 * THE WRITER-SIDE WORD, beside @move_active and deliberately NOT it.
	 * @move_active is READER-facing: nonzero puts every reader on the
	 * COHERENT path.  A detach / graft / merge has no reason to impose
	 * that cost.  G5.5's exclusion is a WRITER question: "is any bulk op
	 * live?".  So it gets its own trie-level word.  The word is read once
	 * per writer scope behind caa_likely, and it is free in steady state.
	 *
	 * ☠ ONE GATE, ONE GP, TWO WORDS, TWO REFCOUNTS.  The machinery below
	 * (@move_gate_lock / @move_gate_cond / @gate_gp_nr) is SHARED, so a
	 * rekey burst and a detach burst still amortize into one grace period.
	 * The refcount that decides when each WORD clears must not be shared.
	 * A single counter would do one of two things:
	 * - leave @move_active set by a detach, which is the exact cost this
	 *   split exists to avoid;
	 * - clear it while a rekey is still live, which is a correctness bug.
	 *
	 * @bulk_gate_nr is the count under @move_gate_lock.  @bulk_state is
	 * the SAME count, published so that peers read it without the lock,
	 * in ONE load (ft_bulk_active).  There are two words because the
	 * mutex serializes the gate's own arithmetic, while the predicate
	 * must be readable from a writer scope that holds nothing.
	 */
	unsigned long bulk_state;
	unsigned long bulk_gate_nr;	/* ALL bulk holders, rekey included */
	/*
	 * In-flight grace periods, NOT a boolean.  Each runs OUTSIDE the lock,
	 * so TWO owners can overlap: a detach publishing @bulk_active and a
	 * rekey then publishing @move_active each own one.  A boolean cannot
	 * name two owners.  The second clear would release waiters whose
	 * word has not yet had a full grace period.
	 */
	unsigned long gate_gp_nr;
#ifdef FT_DEBUG_BULK_WINDOW
	/*
	 * Bulk-window measurement state.  It is PER-TRIE, because the gate
	 * is.  Two tries can be inside ft_move_gate_enter at once: every
	 * cross-trie scenario does exactly that.  A process-wide @t_open
	 * would be clobbered by the other trie's window.  Written only under
	 * @move_gate_lock, so it inherits the gate's own serialization.
	 */
	uint64_t bw_t_open;		/* when this trie's window opened (0->1) */
	uint64_t bw_t_close;		/* when it last closed (1->0), for the gap */
	/*
	 * Counted PER TRIE and folded into the global histograms once per
	 * WINDOW, not once per ENTER.  Atomics on process-wide counters inside
	 * this lock cost 2x throughput here (measured, 8 movers).  The
	 * critical section is already contended.  A shared line bounced
	 * between sockets lengthens it enough to convoy every mover behind
	 * it.  These are plain increments of trie-local words under
	 * @move_gate_lock.
	 */
	unsigned long bw_enters;
	unsigned long bw_piggyback;
	unsigned long bw_peak;
#endif

	/*
	 * In-progress compaction state (cds_ft_compact_begin), or NULL.
	 * Set at begin, cleared at end.  It serves two purposes:
	 * - cds_ft_compact_begin rejects a second concurrent compaction on
	 *   the same trie;
	 * - cds_ft_destroy finalizes a compaction that the caller forgot to
	 *   end.  It merges the private ranges back so they are not leaked.
	 * Written under the caller's writer exclusion, like every other
	 * mutation.
	 */
	struct cds_ft_compact_state *active_compact;

	/*
	 * Per-trie writer-contention escalation lane (struct ft_lane).  A
	 * concurrent-mode op binds it (ft_txn_op_init).  A starved writer
	 * then escalates into the FIFO fair-mutex lane once it has lost its
	 * budget of attempts (ft_op_lane_at; doc/design/
	 * mcas-multiwriter-readiness.md §11).  It is per-trie because writer
	 * contention is per-trie: ops on different tries share no slots.
	 * Initialized at create; no destructor (futex/word state only).
	 * Unused (never escalates) on an exclusive trie -- ft_txn_op_init
	 * binds NULL there.
	 */
	struct ft_lane lane;

	/*
	 * MW COARSE lock-mode FT-wide writer lock (doc/design/
	 * mw-writer-lock-escalation-model.md §10.5 / §11.3 step 2).  There is
	 * ONE lock per trie.  Every mutator on a lock-mode trie takes it.
	 * Readers never take it (classic RCU single-writer).
	 *
	 * It is a PLAIN FIFO-fair mutex (<urcu/fair-mutex.h>), deliberately
	 * NOT a transacted word driven through the txn engine.  A lock
	 * acquire cannot ride the MCAS/escalation engine.  That engine's
	 * progress proof assumes that a transaction AT THE LANE HEAD WILL
	 * SUCCEED: nothing behind it can invalidate it.  But an acquire
	 * cannot succeed until an EXTERNAL event, the holder's release.  An
	 * escalated acquirer would hold its FIFO turn while spinning.
	 * domain->active then funnels every txn on the domain into the lane
	 * behind that waiter, INCLUDING THE HOLDER'S OWN commit and release.
	 * This has two effects:
	 * - a circular wait;
	 * - a fresh MCAS descriptor allocated per retry (unbounded slab
	 *   growth).
	 * Both were observed.  A mutex is a mutex.
	 *
	 * GRACE-PERIOD RULE (the invariant rcu-txn.h:1126 also relies on): a
	 * holder must NEVER wait on a grace period while holding this lock.
	 * The writers parked on it are RCU-online and non-quiescent, so they
	 * are exactly what prevents that grace period from completing
	 * (deadlock).  Ops that synchronize_rcu mid-flight drop and retake
	 * the lock across the wait via ft_writer_lock_gp_wait().  The GP
	 * always sits at a seam BETWEEN two distinct commits, so releasing
	 * there costs no atomicity.
	 */
	struct cds_fair_mutex writer_lock;
	/*
	 * ☑ BULK-vs-BULK, AND IT IS A SECOND LOCK ON PURPOSE.  @writer_lock
	 * cannot carry this.  ft_writer_lock_gp_wait DROPS it across every
	 * mid-body grace period.  There are seven live sites in detach /
	 * graft / rekey.  So two bulk ops meet at any drain seam.  It cannot
	 * simply be HELD across those GPs either.  Since G5.25 a point op
	 * takes @writer_lock too while a bulk op is live.  That wait stays
	 * ONLINE by contract (ft_writer_lock_take: the caller may hold
	 * reader-derived references across it).  So a holder waiting on a GP
	 * would be waiting on the very readers it is blocking.
	 *
	 * This lock restores that property BY CONSTRUCTION instead of
	 * assuming it.  ONLY bulk ops ever wait here.  A bulk entry forbids a
	 * read-section caller (fractal-trie.h: "Do NOT call these operations
	 * from within an RCU read-side critical section").  So its waiters
	 * park OFFLINE and never hold a grace period up.  The lock is
	 * therefore held CONTINUOUSLY across the drain seams, which is what
	 * closes them.
	 *
	 * ORDER: strictly OUTSIDE @writer_lock.  Taken after the bulk gate (so
	 * the gate keeps its refcounted one-GP-per-burst piggyback) and before
	 * any writer scope; never taken while holding @writer_lock.
	 */
	struct cds_fair_mutex bulk_lock;
	/*
	 * Hot-path gate: writer_strategy == CDS_FT_WRITER_LOCK_FINE.  The
	 * fine-grained (per-node lock-set) conversions read THIS:
	 * COARSE deliberately derives no lock-set (§10.5 -- one FT-wide lock, no
	 * per-node locks), so a per-node acquire there would be pure cost.
	 *
	 * ☠ STALE AS WRITTEN -- a FINE trie NO LONGER takes the FT-wide writer
	 * lock: ft_writer_lock_scope_enter drops it ALL-AT-ONCE for FINE, and has
	 * since the §11 drop landed.  What remains true is the reason it once
	 * did.  ☑ The ONE case that takes it back is a live BULK op (G5.25): the
	 * refcount decides, so point ops keep the lock-free fast path in steady
	 * state and re-serialize only inside a bulk window.
	 */
	bool lock_fine;
	/*
	 * CDS_FT_WRITER_EXTERNAL_SYNC: the APPLICATION provides writer
	 * exclusion.  It can be a single thread, its own mutex around every
	 * mutating call, or any other means.  The library therefore takes NO
	 * writer lock of its own.
	 *
	 * ☠ THIS IS NOT @exclusive, and the difference is the READERS.
	 * cds_ft_make_exclusive additionally promises NO CONCURRENT RCU READERS,
	 * which licenses skipping reader-visible publication discipline.  This
	 * mode keeps readers, so proxies, tags and grace periods all stay.  It
	 * is a COARSE trie in every respect except who provides the exclusion.
	 * @lock_fine is false here, so door 1 arms it SW trie-wide and every
	 * consumer of @exclusive is untouched.
	 *
	 * ☞ AND THE PROMOTION LANES ARE INERT, not special-cased.  Rekey and
	 * the bulk gate exist to promote FINE locking to a wider exclusion.
	 * This mode has no fine locking to promote, and neither has COARSE.
	 */
	bool external_sync;


	/*
	 * Per-trie circular sentinel of the ordinal-cell list.  The list is a
	 * circular doubly-linked list of cells (struct ft_ord_cell.lnode),
	 * anchored at this embedded sentinel node:
	 * - ord_sentinel.node.next is the first cell's lnode (the old
	 *   ord_cell_head);
	 * - ord_sentinel.node.prev is the last cell's lnode (the old
	 *   ord_cell_tail);
	 * - the first cell's lnode.prev and the last cell's lnode.next point
	 *   back at &ord_sentinel.node.
	 *
	 * In an empty (or disabled) list the sentinel points at itself
	 * (urcu_txn_list_init).  A resolved link that equals
	 * &ord_sentinel.node is "off the end" (ft_ord_is_end).  The head/tail
	 * endpoint flips are now just the sentinel node's next/prev edges.  A
	 * normal splice whose boundary neighbour is the sentinel produces
	 * them naturally.  Read the endpoints via ft_ord_first / ft_ord_last
	 * (NULL when empty).
	 */
	struct urcu_txn_list_head ord_sentinel;
	/*
	 * ☞ THE THREE WORDS NO NODE OWNS, GIVEN OWNERS.
	 *
	 * @ord_sentinel's two list ends and @root are lock-BEARING words with
	 * no lock-bearing ancestor: no node owns them.  That is why they were
	 * the permanent MW residue (the MW_ALWAYS ROOT class, and the cell
	 * census's `sentinel` bucket).  "No node owns it" describes the
	 * absence of an owner, not an impossibility.  So give them owners.
	 *
	 * Each is a full struct cds_ft_metadata and not a bare word, so that
	 * every existing primitive works unchanged:
	 * - ft_meta_lock_acquire/release CAS the same @state bit;
	 * - ft_flip_txn_lock_register_held files the same anchor;
	 * - ft_dlm_acquire_set sorts them by address alongside node anchors.
	 *
	 * ☠ ONE LOCK PER END, NOT ONE FOR THE LIST.  @ord_sentinel.node.next
	 * (the list BEGIN) and .prev (the END) are independent words.  A
	 * shared lock would serialise a head splice against a tail splice for
	 * nothing.
	 *
	 * These are lock ANCHORS only.  They are never reachable as trie
	 * nodes and never handed to a descent.  A lock-only ft_dlm_member
	 * names them.
	 */
	struct cds_ft_metadata ord_begin_lock;	/* &ord_sentinel.node.next */
	struct cds_ft_metadata ord_end_lock;	/* &ord_sentinel.node.prev */
	struct cds_ft_metadata root_lock;	/* &root */

#ifdef FEATURE_FT_EXCL_VALIDATE
	/*
	 * Access-discipline validator state.  @excl_owner holds the
	 * pthread_self() of the thread currently inside a writer API
	 * (claimed via atomic CAS), or 0 when no writer is active.
	 * @excl_writer_depth is a reentry depth counter, accessed only
	 * by the owning thread.  Both are meaningful ONLY where writers
	 * actually serialize -- see @excl_nr_writers.  @excl_nr_readers
	 * counts readers that are currently inside a reader API on an
	 * exclusive-mode trie; concurrent-mode readers do not touch it
	 * (RCU handles them).
	 *
	 * @excl_nr_writers is the FINE-trie replacement for @excl_owner.  A
	 * FINE trie skips the FT-wide writer mutex, so DISJOINT WRITERS RUN
	 * IN PARALLEL BY DESIGN and no single owner word can represent them.
	 * Count them instead.  The reader check can then still ask the only
	 * question that matters there: is a DIFFERENT thread writing?
	 */
	unsigned long excl_owner;
	unsigned long excl_writer_depth;
	unsigned long excl_nr_readers;
	unsigned long excl_nr_writers;
#endif

#ifdef FEATURE_FT_VERIFY_AT_MUTATION
	/*
	 * Verify-at-mutation sampling.  ft_writer_scope_verify runs
	 * the full O(N) cds_ft_verify walk once every
	 * @verify_at_mutation_period mutations.
	 * @verify_at_mutation_counter increments on every writer-scope
	 * exit and is reset to 0 each time the period is reached.  So
	 * it never exceeds @verify_at_mutation_period - 1 (no overflow
	 * concerns even on long-running workloads).  Period 1
	 * reproduces the historical "every mutation" cadence.  Larger
	 * periods are useful on large tries where O(N) per mutation
	 * is impractical.  Period 0 disables the walk entirely.  Both
	 * fields are write-side only (mutex-held during the writer
	 * scope), so plain accesses are safe.
	 */
	unsigned long verify_at_mutation_period;
	unsigned long verify_at_mutation_counter;
#endif
} __attribute__((aligned(16)));	/* >= FT_PARENT_TRIE_ALIGN: a trie pointer
				 * is stored in the TRANSACTED parent slot and
				 * must clear FT's whole in-band tag. */

/*
 * Root ownership: a root's metadata->parent names its OWNING TRIE.
 *
 * Every non-root node's parent identifies the node above it.  So a node
 * reachable from two tries contradicts one of them, and cds_ft_verify
 * catches it.  A root has no node above it.  Encoding that as NULL makes
 * the back-edge carry no identity at exactly the one position where the
 * owner is the TRIE and not a node.  Two tries rooted at one node would
 * then each agree with their own expected parent, and no per-trie walk
 * could tell.  Naming the trie closes that.  The check that the walk
 * already performs at every other depth then also holds at depth 0.
 *
 * ENCODING.  metadata->parent is a TRANSACTED slot: it carries FT's in-band
 * flip proxy.  The proxy tag is FT_INTERNAL_MASK | FT_TYPE_MASK: the low
 * FOUR bits, not one.  (The MCAS engine owns bit 0 by default and lets an
 * embedder widen it.  FT does, with type 7 == 0xF.)  So a value stored
 * here is unambiguous only when its whole low nibble is clear:
 *
 *   internal          bit 0 set
 *   flip proxy        low nibble == FT_FLIP_PROXY_TAG (0xF)
 *   compressed        bit 1 set
 *   skip-compressed   carries the OWNING node's own tag in bits 0-1, because a
 *                     skip pointer names the compressed node's child -- which
 *                     is the very node whose parent field this is, and only
 *                     internal / compressed nodes have a metadata (an external
 *                     cds_ft_node is {prev, next}).  So never 0000.
 *   owning trie       low nibble clear
 *
 * That is what FT_PARENT_TRIE_ALIGN and the assertions below buy.  A
 * struct cds_ft * only qualifies while it is aligned past the whole tag.
 * Plain malloc alignment (_Alignof(max_align_t)) is 16 on LP64 but 8 on
 * common 32-bit ABIs.  That is not enough, and it is not something to leave
 * to the allocator.
 *
 * The classification is one mask.  Its cost matches the NULL test that it
 * replaces on the parent-pointer backtrack path.  NULL keeps its meaning of
 * "not yet parented" for a node under build, which is never reachable.
 */

#ifdef FEATURE_FT_PROBE_REANCHOR
extern unsigned long ft_probe_mrg_desc[3], ft_probe_mrg_conf[3];
#define MRG_SKIPCONF_PROBE(i, d)					\
	do {								\
		__atomic_fetch_add(&ft_probe_mrg_desc[i], 1,		\
				__ATOMIC_RELAXED);			\
		if ((d).skip_conflict)					\
			__atomic_fetch_add(&ft_probe_mrg_conf[i], 1,	\
					__ATOMIC_RELAXED);		\
	} while (0)
extern unsigned long ft_probe_ranch[2], ft_probe_rewind[2];
#define MRG_REANCHOR_PROBE(i, rw)					\
	do {								\
		__atomic_fetch_add(&ft_probe_ranch[i], 1,		\
				__ATOMIC_RELAXED);			\
		if ((rw) != 0)						\
			__atomic_fetch_add(&ft_probe_rewind[i], 1,	\
					__ATOMIC_RELAXED);		\
	} while (0)
/*
 * merge_spine_retry's spin depth.  ft-merge.h brackets no engine
 * transaction.  So that loop ages nothing and has no termination argument,
 * unlike the retry of every other mutator.  Before adding one, measure
 * whether it ever actually spins.  The slots are:
 *   [0] merge ops
 *   [1] retries from a declined dup-chain lock set
 *   [2] retries from a reanchor level-move
 *   [3] the deepest single op's retries
 *   [4] splits [1] by SOURCE CONTRACT
 *
 * The source contract decides whether the fix is even expressible, because
 * one body serves two callers:
 * - cds_ft_merge_at consumes an EXCLUSIVE src.  So every
 *   ft_writer_lock_gp_wait on its path is !exclusive-gated, and the spine
 *   copy runs GP-free UNDER A READ LOCK.
 * - cds_ft_rekey_* share the body with src_ft == dst_ft, a LIVE trie.
 *   There those waits DO run.  A read section held across one would be a
 *   writer waiting on its own grace period.
 * ft_op_begin() takes the read side, so it can only bracket the first.
 */
extern unsigned long ft_probe_mspin[5];
#define MRG_SPIN_PROBE(i)						\
	__atomic_fetch_add(&ft_probe_mspin[i], 1, __ATOMIC_RELAXED)
#define MRG_SPIN_MAX(v)							\
	do {								\
		unsigned long v_ = (v);					\
		unsigned long o_ = __atomic_load_n(&ft_probe_mspin[3],	\
				__ATOMIC_RELAXED);			\
		while (v_ > o_ && !__atomic_compare_exchange_n(		\
				&ft_probe_mspin[3], &o_, v_, 0,		\
				__ATOMIC_RELAXED, __ATOMIC_RELAXED))	\
			;						\
	} while (0)
/*
 * The OTHER unbracketed retry loops.  They were probed as a CLASS after
 * merge_spine_retry turned out to spin 377 deep with no aging:
 * - ft_merge_graft_subpos_inplace's retry_merge;
 * - ft_graft_keylen's retry_attach;
 * - cds_ft_graft_swap's retry_swap.
 * The merge source itself calls the last two "the same plan->commit retry
 * shape".  They are in a file (ft-graft.h) that brackets no engine txn at
 * all.
 *
 * The count is taken at the LABEL and not at each of the 24 goto edges, so
 * it cannot drift from the control flow.  Pairs: {entries beyond the first,
 * deepest single op}.  [0,1] retry_merge  [2,3] retry_attach  [4,5] retry_swap.
 */
extern unsigned long ft_probe_rspin[6];
#define RSPIN_ENTER(i, v)						\
	do {								\
		unsigned long d_, o_;					\
									\
		(v)++;							\
		if ((v) < 2)						\
			break;						\
		d_ = (v) - 1;						\
		__atomic_fetch_add(&ft_probe_rspin[i], 1,		\
				__ATOMIC_RELAXED);			\
		o_ = __atomic_load_n(&ft_probe_rspin[(i) + 1],		\
				__ATOMIC_RELAXED);			\
		while (d_ > o_ && !__atomic_compare_exchange_n(		\
				&ft_probe_rspin[(i) + 1], &o_, d_, 0,	\
				__ATOMIC_RELAXED, __ATOMIC_RELAXED))	\
			;						\
	} while (0)
/*
 * Split each of those retry counts by the SOURCE CONTRACT.  The contract
 * decides whether a persistent-handle fix is EXPRESSIBLE at all, and not
 * merely desirable.  ft_probe_mspin[4] answered the same question for
 * merge_spine_retry (exclsrc=585250 livesrc=0).
 *
 * A bracket does two things, and ft_writer_lock_gp_wait() tolerates
 * neither:
 * - ft_op_begin() opens an RCU read section.  ft_writer_lock_gp_wait()
 *   self-deadlocks under it.
 * - ft_op_conflict() ages the op into the trie's FIFO lane.
 *   ft_writer_lock_gp_wait() asserts !ft_op_in_lane() under it.
 * So a bracket may only span a body that takes NO grace period.  Every
 * grace period on these three paths is gated on a source being LIVE.  The
 * conditions below exclude a live source:
 *
 *   [0] retry_merge   dst_ft->lock_fine && src_ft->exclusive
 *   [1] retry_attach  dst_ft->lock_fine && src_ft->exclusive
 *   [2] retry_swap    dst_ft->lock_fine && swap_ft->exclusive
 *
 * A loop whose retries all land OUTSIDE its condition cannot be fixed this
 * way.  That is a measurement, not an argument.
 *
 * ft_probe_rspin_n[] is this split's RED CONTROL, and it is not optional.
 * live == 0 would read identically in two cases:
 * - no retry ever happens off-contract;
 * - @cond is simply TRUE BY CONSTRUCTION at that label.
 * The second reading makes the whole measurement vacuous.  So count the ops
 * that ENTER each loop with @cond false (first entry, v == 1).  A non-zero n
 * proves that the condition varies at that site and that the counter can
 * tell the two apart.  n == 0 means the split says nothing at all there.
 */
extern unsigned long ft_probe_rspin_x[3], ft_probe_rspin_n[3];
/*
 * PROBE (2026-08-20): ft_probe_rspin_n[] counts only OFF-CONTRACT entries.
 * So a zero there cannot distinguish "the loop is never entered
 * off-contract" from "the loop is never entered AT ALL".  Both read as
 * offc=0.  The second is a statement about reachability, not about the
 * contract.  So count TOTAL entries per SITE, split by @cond.
 *   [site][0] = entries with cond FALSE, [site][1] = entries with cond TRUE
 *   site 0 = merge subpos, site 1 = unused (was the staged rekey's subpos),
 *   site 2 = graft_swap
 */
extern unsigned long ft_probe_rspin_e[3][2];
#define RSPIN_SITE_ENTER(site, v, cond)					\
	do {								\
		if ((v) == 1)						\
			__atomic_fetch_add(				\
				&ft_probe_rspin_e[site][(cond) ? 1 : 0],\
				1, __ATOMIC_RELAXED);			\
	} while (0)
#define RSPIN_ENTER_X(i, v, xi, cond)					\
	do {								\
		bool c_ = (cond);					\
									\
		RSPIN_ENTER(i, v);					\
		if ((v) >= 2 && c_)					\
			__atomic_fetch_add(&ft_probe_rspin_x[xi], 1,	\
					__ATOMIC_RELAXED);		\
		if ((v) == 1 && !c_)					\
			__atomic_fetch_add(&ft_probe_rspin_n[xi], 1,	\
					__ATOMIC_RELAXED);		\
	} while (0)
#else
#define RSPIN_ENTER(i, v)		do { } while (0)
#define RSPIN_ENTER_X(i, v, xi, cond)	do { } while (0)
#define RSPIN_SITE_ENTER(site, v, cond)	do { } while (0)
#define MRG_SKIPCONF_PROBE(i, d)	do { } while (0)
#define MRG_REANCHOR_PROBE(i, rw)	do { } while (0)
#define MRG_SPIN_PROBE(i)		do { } while (0)
#define MRG_SPIN_MAX(v)			do { } while (0)
#endif

#define FT_PARENT_TAG_MASK	((uintptr_t) (FT_INTERNAL_MASK | FT_TYPE_MASK))
#define FT_PARENT_TRIE_ALIGN	(FT_PARENT_TAG_MASK + 1)

urcu_static_assert(!(FT_PARENT_TRIE_ALIGN & FT_PARENT_TAG_MASK),
		"FT_PARENT_TRIE_ALIGN must be a power of two covering the whole parent tag",
		ft_parent_trie_align_pow2);
static inline
struct cds_ft_inode_flag *ft_trie_parent(const struct cds_ft *ft)
{
	return (struct cds_ft_inode_flag *) (uintptr_t) ft;
}

static inline
bool ft_parent_is_trie(const struct cds_ft_inode_flag *parent)
{
	return parent && !((uintptr_t) parent & FT_PARENT_TAG_MASK);
}

urcu_static_assert(!(__alignof__(struct cds_ft) % FT_PARENT_TRIE_ALIGN),
		"struct cds_ft must be aligned past FT's in-band parent tag: a trie pointer lives in the transacted metadata->parent slot",
		ft_trie_alignment_covers_parent_tag);

static inline
struct cds_ft *ft_parent_trie(struct cds_ft_inode_flag *parent)
{
	return (struct cds_ft *) (uintptr_t) parent;
}

/*
 * True for a node at the top of a trie -- the position whose slot is
 * &ft->root.  Replaces the "parent == NULL" root test.
 */
static inline
bool ft_parent_is_root_position(const struct cds_ft_inode_flag *parent)
{
	return !parent || ft_parent_is_trie(parent);
}

/*
 * Return the node ABOVE this position, or NULL at a root.
 *
 * Every consumer that asks "who is my parent NODE" goes through here: an
 * up-walk, a republish into the parent's slot, a parent-slot offset.  A
 * root's stored parent names its owning TRIE.  The trie is not a node and
 * must never be walked or dereferenced as one.  The function takes the
 * value and not the metadata, so each caller keeps its own load and memory
 * ordering.
 *
 * Two readers want the raw word instead:
 * - ft_resolve_parent_slot, which turns it into &ft->root;
 * - cds_ft_verify, which checks ownership.
 * A transacted store also reads it raw, because the expected-old must be
 * the word as it stands.
 */
static inline
struct cds_ft_inode_flag *ft_parent_node(struct cds_ft_inode_flag *parent)
{
	return ft_parent_is_trie(parent) ? NULL : parent;
}

/*
 * Return the word to STORE in a metadata->parent, given the parent NODE
 * (NULL at a root).  This is the inverse of ft_parent_node, and the two
 * form a pair for this reason.  A publish path computes its parent as a
 * node, where NULL means "into &ft->root".  It must not write that NULL
 * through.  The node it builds would become a root that belongs to nobody,
 * and the depth-0 ownership check would have nothing to compare.
 *
 * A copy that inherits an existing parent word verbatim does not need this.
 * The word it copies already names whatever it should.
 */
static inline
struct cds_ft_inode_flag *ft_parent_word(const struct cds_ft *ft,
		struct cds_ft_inode_flag *parent_node)
{
	return parent_node ? parent_node : ft_trie_parent(ft);
}

/*
 * Access-discipline validator.  See FEATURE_FT_EXCL_VALIDATE above.
 *
 * The helpers form two nested pairs: ft_excl_writer_enter / _exit
 * around every writer API body, and ft_excl_reader_enter /
 * ft_excl_reader_scope_exit around every reader API body.  The
 * CDS_FT_SCOPED_{READER,WRITER}(ft) macros wrap the pair in a GCC
 * cleanup-attribute variable so any return path in the body
 * automatically runs the matching _exit.
 *
 * Reader entry returns a per-call scope (struct ft_excl_reader_scope)
 * that records whether the reader claimed the trie's reader counter.
 * In concurrent mode a reader that holds the RCU read-side lock at
 * entry does NOT claim: it may safely overlap a writer.  A reader
 * that does not hold it is treated like an exclusive-mode reader
 * and claims.
 *
 * When FEATURE_FT_EXCL_VALIDATE is off the helpers are empty and
 * the compiler inlines them away.
 */

/*
 * MW COARSE lock-mode: per-thread FT-wide writer-lock state.
 *
 * @ft_wlock_waiter is this thread's FIFO queue node.  It lives in TLS
 * because cds_fair_mutex_lock() enqueues the node BY ADDRESS.  The address
 * must stay stable from lock to unlock, and these happen in two different
 * functions (the writer-scope enter and its cleanup).
 *
 * @ft_wlock_held names the trie whose lock this thread holds (NULL = none).
 * It doubles as the reentrancy test.  A thread holds AT MOST ONE FT-wide
 * lock.  Cross-trie graft / merge / graft_swap are the only ops that scope
 * two tries.  Two complements keep the limit true for them:
 * - ft_crosstrie_lock_mode_guard rejects a BOTH-LIVE lock-mode pair.
 * - Step 6's exclusive-source contract requires the CONSUMED source
 *   (graft/merge src, graft_swap swap) to be EXCLUSIVE.  A live source is
 *   rejected with BUSY at the op entry.  An exclusive trie's writer scope
 *   SKIPS the lock (ft_writer_lock_scope_enter).
 * So a cross-trie op holds only the live dst's lock.
 *
 * @ft_wlock_depth counts reentrant writer scopes on that one trie (a
 * same-thread nest such as graft_swap -> graft).  So the lock is taken once
 * at the outermost enter and released once at the outermost exit.
 */
static __thread struct cds_fair_mutex_node ft_wlock_waiter;
/*
 * The same, for @bulk_lock.  It is a SEPARATE node because one thread holds
 * both locks at once (bulk_lock outside, writer_lock inside).  A FIFO node
 * serves one queue at a time.  @ft_bulk_lock_held / @ft_bulk_lock_depth
 * make the take reentrant.  Bulk bodies NEST: a rekey's staged fallback
 * re-enters the gate.  That is why ft_bulk_self_depth is depth-counted.  A
 * non-reentrant take here would wedge the op on its own lock.
 */
static __thread struct cds_fair_mutex_node ft_bulk_lock_waiter;
extern __thread unsigned long ft_bulk_self_depth;
static __thread struct cds_ft *ft_bulk_lock_held;
static __thread unsigned long ft_bulk_lock_depth;

#ifdef FT_DEBUG_BULK_LOCK
/*
 * TEMPORARY: is the bulk-vs-bulk lock actually EXERCISED, and is it HELD across
 * the drain seams it exists to close?  A green run proves nothing if the lock
 * is never taken.  @seam_under is the number that matters.  It counts the
 * gp_waits reached while holding the lock.  Each one is a seam that a peer
 * bulk op can no longer enter.
 */
/* Weak: this header is an impl unit included by more than one TU. */
__attribute__((weak)) unsigned long ft_bl_takes;
__attribute__((weak)) unsigned long ft_bl_nested;
__attribute__((weak)) unsigned long ft_bl_seam_under;
__attribute__((weak)) unsigned long ft_bl_seam_all;
__attribute__((weak)) unsigned long ft_bl_seam_excl;
__attribute__((weak)) unsigned long ft_bl_seam_selfdepth;
__attribute__((weak)) unsigned long ft_bl_seam_selfdepth;
__attribute__((weak)) unsigned long ft_bl_reported;
static void ft_bl_report(void) __attribute__((destructor));
static void ft_bl_report(void)
{
	if (!ft_bl_takes && !ft_bl_seam_all)
		return;
	if (__atomic_fetch_add(&ft_bl_reported, 1, __ATOMIC_RELAXED))
		return;			/* one destructor per TU; print once */
	fprintf(stderr, "FT_BULK_LOCK  takes=%lu nested=%lu | gp_wait seams: "
		"total=%lu UNDER the bulk lock=%lu (of the rest: "
		"exclusive=%lu in-a-bulk-body=%lu)\n",
		ft_bl_takes, ft_bl_nested, ft_bl_seam_all, ft_bl_seam_under,
		ft_bl_seam_excl, ft_bl_seam_selfdepth);
}
# define FT_BL_TALLY(c)	__atomic_fetch_add(&(c), 1, __ATOMIC_RELAXED)
#else
# define FT_BL_TALLY(c)	do { } while (0)
#endif
static __thread struct cds_ft *ft_wlock_held;
static __thread unsigned long ft_wlock_depth;

/*
 * RED CONTROL for the rekey's single-recompaction fold.  This is a
 * deliberately BROKEN build, never shipped and never a default.  It stops
 * ft_rekey_graft_simple_attempt from arming @pending_del_slot.  A
 * same-junction move then ATTACHES to BP and DETACHES from it as two
 * recompactions again.  The second recompaction:
 * - reads the child count of the copy that the first retired;
 * - fuses a boundary that keeps two children;
 * - retires a child that the attach just re-parented.
 * test_merge_rekey_same_trie then aborts on the engine's SW-xor-MW slot rule
 * (urcu_txn_record_chain, r->kind == kind).  That is the failure the fold
 * exists to remove, so this knob keeps that claim falsifiable.
 */
#ifdef FT_RED_REKEY_NOLOCK
/*
 * RED CONTROL for inv_rekey_coarse_mixed_writers.  This is a deliberately
 * BROKEN build, never shipped and never a default.  Set across
 * ft_rekey_dispatch, it makes the rekey take no FT-wide writer lock.  The
 * atomic rekey was in exactly that state before the @lock_fine gate was
 * understood.  On a COARSE trie every OTHER writer excludes through that
 * mutex.  A rekey that skips it arbitrates with nobody, so its edge installs
 * are lost updates against a concurrent insert or remove.
 *
 * It is injected HERE and not by deleting a CDS_FT_SCOPED_WRITER.  The
 * control asks a question about the whole op: "does this writer join the
 * protocol at all".  The staged path also nests several scopes.
 */
static __thread int ft_red_rekey_nolock;
extern unsigned long ft_red_rekey_nolock_taken;	/* GLOBAL: outlives the writers */
#endif

#ifdef FT_DEBUG_BULK_ELEV
/*
 * ★ COUNT THE ARM (G5.25 bulk gate).  A test that mixes bulk and point
 * writers tests the bulk-vs-point pairing only if the point ops actually
 * LAND in a bulk window.  Otherwise its green says nothing about the gate.
 * These count every writer scope that took the FT-wide lock BECAUSE the gate
 * was up, split by who is asking:
 *
 *   @ft_bulk_elev_self  the bulk op's OWN scope, inside its own gate.  Nonzero
 *                       says the gate-holder elevates too -- i.e. the FT-wide
 *                       lock really is the word both sides meet on.  Zero says
 *                       the bulk op opted out and the gate only serializes
 *                       point ops against each other for its duration.
 *   @ft_bulk_elev_peer  a PEER op that found the gate up and elevated.  This is
 *                       the coverage number: it is how many point ops ran
 *                       inside a bulk window.
 *
 * Debug-only.  The thread depth is the only way to tell the two apart: the
 * gate word is refcounted, so it cannot say who raised it.
 */
extern unsigned long ft_bulk_elev_self;
extern unsigned long ft_bulk_elev_peer;
static __thread unsigned long ft_bulk_gate_depth;
/*
 * And the same split over the OUTERMOST scopes that actually TAKE the FT-wide
 * lock.  Reentrant nested scopes are excluded: they hold it already.  These
 * say who is on the lock, where the pair above says who was asked to elevate.
 */
extern unsigned long ft_bulk_take_self;
extern unsigned long ft_bulk_take_peer;
extern unsigned long ft_bulk_gate_calls;	/* ft_bulk_gate_enter calls */
extern unsigned long ft_bulk_scope_under_gate;	/* outer scopes with depth > 0 */
#endif

/*
 * The access validator's writer OWNER word follows the FT-WIDE LOCK, not
 * the writer scope.  ft_writer_lock_gp_wait is why.  That function drops the
 * lock mid-scope by design, so on a COARSE trie two writer SCOPES
 * legitimately overlap.  The validator kept the single-owner CAS there on
 * this premise: "a COARSE trie serializes under the FT-wide mutex, so its
 * owner word is accurate anyway".  That premise stops holding.  The check
 * fired the moment writers began making progress at all: "writer conflict
 * -- owner ..., entering thread ...".
 *
 * So hand the claim back with the lock and retake it with the lock.  This is
 * a no-op in two cases:
 * - the validator counts instead of owning (concurrent + FINE);
 * - there is no lock to drop.  An exclusive trie skips it, and has no
 *   concurrent writer to race.
 */
#ifdef FEATURE_FT_EXCL_VALIDATE
static inline
unsigned long ft_excl_owner_release(struct cds_ft *ft)
{
	unsigned long depth;

	if (ft->lock_fine && !ft->exclusive)
		return 0;			/* counted, not owned */
	depth = ft->excl_writer_depth;
	ft->excl_writer_depth = 0;
	uatomic_store(&ft->excl_owner, 0, CMM_RELEASE);
	return depth;
}

static inline
void ft_excl_owner_reclaim(struct cds_ft *ft, unsigned long depth)
{
	if (!depth)
		return;
	/* We hold the lock again, so we ARE the owner: store, do not contend. */
	uatomic_store(&ft->excl_owner, (unsigned long) pthread_self(),
		CMM_RELEASE);
	ft->excl_writer_depth = depth;
}
#else
static inline
unsigned long ft_excl_owner_release(struct cds_ft *ft __attribute__((unused)))
{
	return 0;
}

static inline
void ft_excl_owner_reclaim(struct cds_ft *ft __attribute__((unused)),
		unsigned long depth __attribute__((unused)))
{
}
#endif

/*
 * THE TWO WAITS ON THE FT-WIDE WRITER LOCK, AND WHY ONLY ONE OF THEM QUIESCES.
 *
 * Both waits below block on the same fair mutex.  They differ in what the
 * WAITING THREAD is still answering for.  So they differ in whether going
 * RCU-offline is permitted.
 *
 *   ft_writer_lock_take()  SCOPE ENTRY -- stays ONLINE.  The caller may be
 *                          inside its own RCU read-side critical section and
 *                          may hold reader-derived references across the
 *                          call.  fractal-trie.h's reference-lifetime
 *                          contract REQUIRES that section be continuous:
 *                          cds_ft_remove / cds_ft_replace dereference the
 *                          caller's @node (via @node->prev) with no
 *                          re-descent.  Under QSBR thread_offline() reports
 *                          a quiescent state.  So parking offline HERE
 *                          would end that section and expose @node.  It
 *                          would expose the caller's key with it, since
 *                          ft_iter_read_key() may return a pointer INTO
 *                          @node.  So this wait does not quiesce.
 *
 *   ft_writer_lock_park()  gp_wait RE-ACQUIRE -- goes OFFLINE.  This is
 *                          sound here and only here.  The thread has just
 *                          waited out a full grace period.  So everything
 *                          it holds is ALREADY exposed to reclamation.  The
 *                          writer lock / exclusivity must already keep it
 *                          valid, not this thread's online-ness.  Being
 *                          offline for the subsequent wait adds no exposure.
 *
 * WHAT THE ONLINE ENTRY WAIT COSTS, MEASURED (2026-08-28, -O2 -DNDEBUG, QSBR,
 * inv_concurrent_writers_coarse_lock: 16 writers, one coarse trie, 200 ms):
 * - the lock runs ~75% utilized (5811 holds, mean 26 us);
 * - a CONTENDED entry wait is p50 >= 524 us, p99 >= 1.05 ms, max 1.99 ms;
 * - 31% of acquisitions are uncontended at ~32 ns.
 * An online waiter is a non-quiescent QSBR reader for exactly that long, so
 * it delays every grace period in the process.
 * ACCEPTED DELIBERATELY (Mathieu, 2026-08-28): correctness of the caller's
 * reference contract outranks grace-period latency on a coarse trie.
 *
 * ☠ IT IS A DELAY, NOT A WEDGE.  That is a property of the GRACE-PERIOD
 * RULE above (@writer_lock).  A holder NEVER waits on a grace period while
 * holding, because ft_writer_lock_gp_wait drops the lock across every one.
 * So an online entry waiter never waits on a holder that itself waits on
 * that waiter.  It acquires within queue-depth x hold and then completes.
 * The group wedge that 781e0b9a measured (four concurrent
 * cds_ft_rekey_graft writers making ZERO moves) had THREE THREADS IN THE
 * gp_wait RE-ACQUIRE.  That site keeps its offline park, so that fix is
 * untouched.
 *
 * The distinction between the two is a no-op on the memb / mb / bp flavors,
 * where thread_offline() is an empty function.  Those flavors have always
 * waited online here, and their callers' read sections have always survived.
 */
#ifdef FT_DEBUG_WLOCK_HOLD
/*
 * =====================================================================
 * MEASUREMENT (opt-in: -DFT_DEBUG_WLOCK_HOLD).  NOT part of the library.
 * =====================================================================
 *
 * This PRICED THE ENTRY-WAIT DECISION.  It is the source of the numbers
 * quoted in the two-waits comment above.  The entry wait now stays ONLINE
 * (ft_writer_lock_take).  So a waiter is a non-quiescent QSBR reader for
 * its whole WAIT, and it delays every grace period in the process for that
 * long.  WAIT is therefore the number the decision needs: the grace-period
 * latency that the coarse path imposes.  HOLD is what generates it.  Re-run
 * this whenever the coarse hold distribution could have moved.  The
 * accepted cost is only as good as the distribution it was accepted
 * against.
 *
 * Both are recorded in log2(ns) buckets and dumped at process exit.  There
 * is exactly one acquire path (this function, reached from both sites) and
 * two release sites (scope_exit and gp_wait's drop).
 *
 * ☠ COARSE ONLY BY CONSTRUCTION: a FINE trie returns from
 * ft_writer_lock_scope_enter before the park, so a FINE run records ZERO.
 * That is a CONFIGURATION check, not a measurement.  Read the n= line
 * before reading anything else.
 */
#include <stdio.h>
#include <time.h>

#define FT_WLH_BUCKETS	40
static unsigned long ft_wlh_hold[FT_WLH_BUCKETS];
static unsigned long ft_wlh_wait[FT_WLH_BUCKETS];
static unsigned long ft_wlh_hold_n, ft_wlh_wait_n;
static unsigned long ft_wlh_hold_sum, ft_wlh_wait_sum;
static unsigned long ft_wlh_hold_max, ft_wlh_wait_max;
static __thread uint64_t ft_wlh_acq_ns;

static inline
uint64_t ft_wlh_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static inline
void ft_wlh_record(unsigned long *hist, unsigned long *n, unsigned long *sum,
		unsigned long *max, uint64_t ns)
{
	unsigned int b = 0;
	uint64_t v = ns;

	while (v >>= 1)
		b++;
	if (b >= FT_WLH_BUCKETS)
		b = FT_WLH_BUCKETS - 1;
	uatomic_inc(&hist[b]);
	uatomic_inc(n);
	uatomic_add(sum, (unsigned long) ns);
	if (ns > uatomic_load(max, CMM_RELAXED))
		uatomic_store(max, (unsigned long) ns, CMM_RELAXED);
}

static
void ft_wlh_dump_one(const char *tag, const unsigned long *hist,
		unsigned long n, unsigned long sum, unsigned long max)
{
	unsigned long cum = 0, p50 = 0, p90 = 0, p99 = 0;
	unsigned int i;

	fprintf(stderr, "FT WLOCK %s: n=%lu mean=%luns max=%luns\n",
		tag, n, n ? sum / n : 0UL, max);
	for (i = 0; i < FT_WLH_BUCKETS; i++) {
		if (!hist[i])
			continue;
		cum += hist[i];
		if (!p50 && cum * 100 >= n * 50)
			p50 = 1UL << i;
		if (!p90 && cum * 100 >= n * 90)
			p90 = 1UL << i;
		if (!p99 && cum * 100 >= n * 99)
			p99 = 1UL << i;
		fprintf(stderr, "  >=2^%-2u (%12lu ns) %10lu\n",
			i, 1UL << i, hist[i]);
	}
	/* log2 buckets: each percentile is a LOWER bound, exact to 2x. */
	fprintf(stderr, "FT WLOCK %s: p50>=%luns p90>=%luns p99>=%luns\n",
		tag, p50, p90, p99);
}

__attribute__((destructor))
static
void ft_wlh_dump(void)
{
	if (!ft_wlh_hold_n && !ft_wlh_wait_n) {
		fprintf(stderr, "FT WLOCK: NO SAMPLES -- the FT-wide lock was "
			"never taken (a FINE or exclusive trie).  This is a "
			"configuration miss, not a measurement.\n");
		return;
	}
	ft_wlh_dump_one("HOLD", ft_wlh_hold, ft_wlh_hold_n, ft_wlh_hold_sum,
			ft_wlh_hold_max);
	ft_wlh_dump_one("WAIT", ft_wlh_wait, ft_wlh_wait_n, ft_wlh_wait_sum,
			ft_wlh_wait_max);
}
#endif /* FT_DEBUG_WLOCK_HOLD */

#ifdef FT_DEBUG_WLOCK_HOLD
# define FT_WLH_WAIT_BEGIN	uint64_t ft_wlh_t0 = ft_wlh_now()
# define FT_WLH_WAIT_END						\
	do {								\
		uint64_t t1__ = ft_wlh_now();				\
									\
		ft_wlh_record(ft_wlh_wait, &ft_wlh_wait_n,		\
				&ft_wlh_wait_sum, &ft_wlh_wait_max,	\
				t1__ - ft_wlh_t0);			\
		ft_wlh_acq_ns = t1__;					\
	} while (0)
#else
# define FT_WLH_WAIT_BEGIN	do { } while (0)
# define FT_WLH_WAIT_END	do { } while (0)
#endif

/* SCOPE ENTRY: stays ONLINE -- see the two-waits comment above. */
static inline
void ft_writer_lock_take(struct cds_ft *ft)
{
	FT_WLH_WAIT_BEGIN;

	cds_fair_mutex_lock(&ft->writer_lock, &ft_wlock_waiter);
	FT_WLH_WAIT_END;
}

/* gp_wait RE-ACQUIRE: goes OFFLINE -- see the two-waits comment above. */
static inline
void ft_writer_lock_park(struct cds_ft *ft)
{
	const struct rcu_flavor_struct *flavor = ft->group->flavor;
	FT_WLH_WAIT_BEGIN;

	flavor->thread_offline();
	cds_fair_mutex_lock(&ft->writer_lock, &ft_wlock_waiter);
	flavor->thread_online();
	FT_WLH_WAIT_END;
}

/*
 * Take the FT-wide writer lock at the OUTERMOST writer scope.  A nested scope
 * on the same trie is a reentrant no-op.  Every trie is lock-mode now (COARSE
 * or FINE), so there is no optimistic early-out.  A FINE trie skips the
 * FT-wide lock further down, under the lock-set drop.
 */
/*
 * G5.25: while a bulk op is live, a FINE trie RE-TAKES the FT-wide writer lock
 * instead of widening every point op's lock-set to the root.  On by default:
 * it is the shipping bulk-vs-point exclusion, and the ONLY one.  The per-op
 * lock-set widening it replaced was removed once this landed.  Set to 0 only
 * to measure its cost.  A 0 build has NO bulk-vs-point exclusion at all.
 */
#ifndef FT_BULK_WIDE_LOCK
# define FT_BULK_WIDE_LOCK	1
#endif

/* Defined below with the rest of the bulk gate; needed by the scope enter. */
static inline bool ft_bulk_active(const struct cds_ft *ft);

/*
 * The writer scopes' own read sections this thread holds (see
 * ft_writer_lock_scope_enter), and the flavor they were taken with.
 * ft_writer_lock_gp_wait drops them across its grace period, exactly as it
 * drops the FT-wide lock.  Otherwise a writer scope that waits for a grace
 * period outside a bulk gate (cds_ft_make_exclusive) would wait for itself.
 */
static __thread unsigned long ft_wscope_rcu_held;
static __thread const struct rcu_flavor_struct *ft_wscope_rcu_flavor;
#ifdef URCU_FRACTAL_TRIE_DEBUG_LOCKING
/*
 * Some point ops require the CALLER to hold a read-side section, because a
 * cached iterator's node must stay live.  Those sections would blind
 * CDS_FT_ASSERT_RCU_READ_LOCKED there: the library's section satisfies
 * read_ongoing().  So record the caller's state at the outermost such
 * section.  Those sites ask it (CDS_FT_ASSERT_CALLER_RCU_READ_LOCKED).
 */
static __thread int ft_wscope_caller_reading;
#endif

/*
 * Returns true when the scope holds a read-side section of @ft's flavor,
 * which ft_writer_lock_scope_exit's caller must drop (see the FINE arm).
 */
static inline
bool ft_writer_lock_scope_enter(struct cds_ft *ft)
{
	if (ft_wlock_held == ft) {
		ft_wlock_depth++;		/* reentry on the trie we hold */
		return false;
	}
	if (ft->external_sync) {
		/*
		 * CDS_FT_WRITER_EXTERNAL_SYNC: the caller excludes every writer,
		 * so there is no FT-wide lock to take.  That is the whole point
		 * of the mode.  Readers are UNAFFECTED (see @external_sync):
		 * this skips the writer mutex, nothing else.
		 *
		 * This test sits beside the @exclusive early-out and after the
		 * reentrancy test, for the reason given there.  It is safe for
		 * the same reason too.  ft_writer_lock_scope_exit keys its
		 * release off @ft_wlock_held IDENTITY.  It never re-reads
		 * either flag.
		 */
		return false;
	}
	if (ft->exclusive) {
		/*
		 * An exclusive trie is private / single-writer by contract
		 * (step 6, §9.5).  It has no concurrent writers, so no FT-wide
		 * lock is needed.
		 *
		 * Skipping the lock lets a cross-trie op hold only the LIVE
		 * side's lock.  The exclusive consumed source rides through
		 * the fused op body.  That source is the caller's
		 * cds_ft_make_exclusive'd src, or a detach product.  So the op
		 * holds one lock at a time and has no cross-trie deadlock.
		 *
		 * This is checked AFTER the reentrancy test on purpose.  A trie
		 * whose lock we already hold can flip exclusive mid-scope
		 * (cds_ft_make_exclusive).  It must keep depth-counting its
		 * nested scopes.  ft_writer_lock_scope_exit keys its release
		 * off @ft_wlock_held identity and never re-reads @exclusive.
		 * So a mid-scope flip of the flag cannot unbalance the lock.
		 */
		return false;
	}
#ifdef FT_RED_REKEY_NOLOCK
	/*
	 * RED CONTROL, never a shipped configuration.  See @ft_red_rekey_nolock:
	 * with it set this thread's writer scopes take NO FT-wide lock.  That
	 * is precisely the defect inv_rekey_coarse_mixed_writers exists to
	 * detect.
	 *
	 * Placed FIRST so no lock state is recorded at all:
	 *   - the exit keys off @ft_wlock_held identity, so it no-ops by
	 *     itself;
	 *   - ft_writer_lock_gp_wait sees held == NULL and degrades to a plain
	 *     synchronize_rcu.
	 * The injection is therefore state-balanced.
	 */
	if (ft_red_rekey_nolock) {
		/*
		 * ★ COUNT THE ARM.  A red control that is never TAKEN looks
		 * the same as a green one.  The whole purpose of this one is
		 * to make a green mean something.  So report how many scopes
		 * it actually skipped, as FT_RED_PARENT_WORD_SW does.
		 * Measured 68,655 on inv_rekey_coarse_mixed_writers.
		 */
		uatomic_inc(&ft_red_rekey_nolock_taken);
		return false;
	}
#endif
#ifdef FT_DEBUG_BULK_ELEV
	if (ft_bulk_gate_depth)
		uatomic_inc(&ft_bulk_scope_under_gate);
	if (ft->lock_fine && FT_BULK_WIDE_LOCK
			&& caa_unlikely(ft_bulk_active(ft)))
		uatomic_inc(ft_bulk_gate_depth ? &ft_bulk_elev_self
			: &ft_bulk_elev_peer);
#endif
	/*
	 * ☠ THE GATE IS SAMPLED INSIDE A READ SECTION THAT SPANS THE OP.
	 *
	 * The bulk gate publishes @bulk_state and then waits ONE grace period.
	 * So it excludes exactly the point ops that were inside a read-side
	 * section when they read the gate as clear.  Two facts left a gap:
	 *   - the per-attempt section ft_op_begin takes opens only AFTER
	 *     this sample, and closes between attempts;
	 *   - a caller need not hold a section of its own (cds_ft_insert and
	 *     friends require none).
	 * Under QSBR an online thread is a reader throughout, so that gap was
	 * invisible.  Under memb / mb / bp it let a point op that sampled
	 * "clear" run LOCK-FREE through a bulk body.
	 *
	 * MEASURED with a memb build of inv_rekey_fine_mixed_writers, rekeys
	 * paced 2 ms apart.  A probe flagged a point attempt that starts
	 * unlocked while a bulk op holds the FT-wide lock past its gate:
	 *   - as is: up to 2 per run;
	 *   - with a 2 ms preemption injected after the sample: 17-166, in 6
	 *     of 6 runs;
	 *   - with the section taken first: 0 in 6 of 6 runs.
	 *
	 * So take the section BEFORE the sample:
	 *   - gate clear: keep the section until the scope exits.  Every
	 *     attempt's own section nests inside it.
	 *   - gate raised: drop the section and take the FT-wide lock, the
	 *     exclusion that case uses.
	 * A BULK op's own scope also takes the raised branch (its gate is up).
	 * So a bulk op, which waits for grace periods, never holds this
	 * section.  A point op holding it never waits on anything a GP-waiting
	 * bulk op holds:
	 *   - the gate's GP runs before the bulk op takes any lock;
	 *   - ft_writer_lock_gp_wait drops the FT-wide lock across every later
	 *     one.
	 */
	if (ft->lock_fine) {
#ifndef FT_DEBUG_NO_SCOPE_READ_SECTION
#ifdef URCU_FRACTAL_TRIE_DEBUG_LOCKING
		if (!ft_wscope_rcu_held)
			ft_wscope_caller_reading =
				ft->group->flavor->read_ongoing();
#endif
		ft->group->flavor->read_lock();
#endif
		if (!(FT_BULK_WIDE_LOCK && caa_unlikely(ft_bulk_active(ft)))) {
			/*
			 * ☑ G5.25 -- AND THE ONE CASE THAT TAKES IT BACK.  While a BULK
			 * op is live, a FINE trie RE-TAKES the FT-wide lock, so bulk and
			 * point writers arbitrate on one word again.  That is the whole
			 * of G5.5's exclusion.  It is obtained by NOT dropping
			 * a lock that already exists.  The alternative was to
			 * widen every point op's lock-set to the root.
			 * Compared with that:
			 *   - no ancestor ledger to consume, no up-walk to date members;
			 *   - NO RELEASE-OWNER PROBLEM AT ALL.  A scoped mutex
			 *     has no registry and no reservation to size.  It
			 *     has no "acquire before the txn exists" blocker
			 *     either (the four sites that defeated the
			 *     widening's coverage);
			 *   - and it QUEUES.  The widened DLM acquire spins
			 *     URCU_TXN_WAIT_PATIENCE and then ABORTS, which is the
			 *     retry-storm hazard.  cds_fair_mutex is FIFO.
			 * ★ Sound against the seam rule for free.
			 * ft_writer_lock_gp_wait DROPS this lock across every
			 * grace period.  @writer_lock says: "the GP always
			 * sits at a seam BETWEEN two distinct commits, so
			 * releasing there costs no atomicity".
			 * ★ And the sample races nothing.  A point op that
			 * read the gate as clear is inside the read section
			 * taken just above.  That is NOT the one
			 * ft_op_begin takes later (see above).  It is
			 * exactly what the gate's publish-then-one-GP waits
			 * for.
			 * ☠ The cost is honest and accepted (2026-08-29): while any bulk
			 * op is live, point ops serialize trie-wide.  At level 0 the
			 * widening serialized them on the ROOT's lock anyway.
			 * So this trades an equivalent exclusion for far less
			 * machinery.
			 */
			/*
			 * FT-WIDE-LOCK DROP (§11 drop-mechanics, MCAS-first): a FINE trie's
			 * op-domains are ALL converted to per-node lock-sets (LOCK
			 * try-locks) that arbitrate writers directly, so the FT-wide mutex
			 * is redundant -- SKIP it and let the per-node locks be the sole
			 * exclusion.  Dropped ALL-AT-ONCE for FINE (not op-domain by
			 * op-domain): the FT-wide lock sits at the per-OP writer scope, not
			 * a per-domain sub-scope, and one MCAS-abort-safe net covers every
			 * domain uniformly.  Residual §11.1 under-count sites (I-1
			 * in-place nr_child, I-4b skip-dual P, §8.3 word-sharing) stay
			 * MCAS-abort-safe here: expected-old catches the race as a spurious
			 * retry, not a lost update (that safety net is what the later sw
			 * cutover removes, where full lock-set completeness becomes
			 * mandatory).  ft_writer_lock_scope_exit no-ops (keys off
			 * @ft_wlock_held identity, never set) and ft_writer_lock_gp_wait
			 * degrades to a plain synchronize_rcu (held == NULL).
			 */
#ifndef FT_DEBUG_NO_SCOPE_READ_SECTION
			/* Nested sections of one thread share one flavor. */
			assert(!ft_wscope_rcu_held ||
				ft_wscope_rcu_flavor == ft->group->flavor);
			if (!ft_wscope_rcu_held++)
				ft_wscope_rcu_flavor = ft->group->flavor;
			return true;
#else
			return false;
#endif
		}
#ifndef FT_DEBUG_NO_SCOPE_READ_SECTION
		ft->group->flavor->read_unlock();
#endif
	}
	if (caa_unlikely(ft_wlock_held != NULL)) {
		/*
		 * Two FT-wide locks at once.  Only a cross-trie op could nest
		 * them, and those are rejected on a lock-mode trie.  So
		 * reaching here means that guard was bypassed.  Fail loudly
		 * rather than deadlock (thread 1 holds A wants B; thread 2
		 * holds B wants A).
		 */
		fprintf(stderr, "cds_ft: thread already holds the FT-wide writer "
			"lock of cds_ft=%p while entering cds_ft=%p\n",
			(void *) ft_wlock_held, (void *) ft);
		fflush(stderr);
		abort();
	}
#ifdef FT_DEBUG_BULK_ELEV
	uatomic_inc(ft_bulk_gate_depth ? &ft_bulk_take_self
		: &ft_bulk_take_peer);
#endif
	ft_writer_lock_take(ft);
	ft_wlock_held = ft;
	ft_wlock_depth = 1;
	return false;
}

/* Release at the OUTERMOST scope only; a nested exit just unwinds the depth. */
static inline
void ft_writer_lock_scope_exit(struct cds_ft *ft)
{
	/*
	 * Unwind only the lock THIS thread actually holds for @ft.  When the
	 * enter SKIPPED the lock (an exclusive trie) or we hold a DIFFERENT
	 * trie's lock, @ft_wlock_held != ft and there is nothing to release.
	 *
	 * The test keys off identity.  It does not re-read @ft->exclusive.  So
	 * an @exclusive flip between enter and exit can never unbalance us:
	 *   - cds_ft_make_exclusive (false->true) took the lock at enter, when
	 *     exclusive was false.  It releases it here (held == ft).
	 *   - cds_ft_make_shared (true->false) skipped the lock at enter, when
	 *     exclusive was true.  It skips it here (held != ft).
	 */
	if (ft_wlock_held != ft)
		return;
	if (--ft_wlock_depth != 0)
		return;			/* nested scope: keep the lock held */
	ft_wlock_held = NULL;
#ifdef FT_DEBUG_PAIR_STORE
	if (ft_dt_glue_ring_n) {
		memset(ft_dt_glue_ring, 0, sizeof(ft_dt_glue_ring));
		ft_dt_glue_ring_n = 0;
	}
#endif
#ifdef FT_DEBUG_WLOCK_HOLD
	ft_wlh_record(ft_wlh_hold, &ft_wlh_hold_n, &ft_wlh_hold_sum,
			&ft_wlh_hold_max, ft_wlh_now() - ft_wlh_acq_ns);
#endif
	(void) cds_fair_mutex_unlock(&ft->writer_lock, &ft_wlock_waiter);
}

/*
 * Wait for a grace period from inside a writer scope.  DROP the FT-wide lock
 * across the wait and retake it after.
 *
 * This is mandatory, not an optimization.  Writers parked on the lock are
 * RCU-online and non-quiescent, so they are precisely what stops this grace
 * period from ever completing.  A holder that waits on a GP while holding the
 * lock deadlocks against its own waiters.  rcu-txn.h:1126 leans on the same
 * invariant: "the lane owner never blocks on a GP while holding the mutex."
 *
 * Dropping here costs no atomicity.  Every mid-op grace period in this
 * codebase sits at a seam BETWEEN two distinct commits.  At that seam:
 *   - the structure is already published-consistent;
 *   - the subtree being drained is already unreachable.
 * So a peer that mutates while we wait sees a coherent trie.  On an optimistic
 * trie (or outside any writer scope) this is a plain synchronize_rcu.
 */
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
/* ★ See the remove retry exit: -EAGAIN (contention) vs -ENOENT (dead derivation). */
static __thread unsigned long ft_dbg_rm_eagain;
static __thread unsigned long ft_dbg_rm_enoent;
#endif

#ifdef FT_DEBUG_REMOVE_RETRY_CAP
# include <time.h>
static __thread uint64_t ft_dbg_gp_ns;
static __thread unsigned int ft_dbg_gp_calls;
/* Arena-lock wait accounting; defined in fractal-trie-alloc.c (its TU). */
extern __thread uint64_t ft_dbg_arena_ns;
extern __thread unsigned int ft_dbg_arena_waits;

static inline uint64_t ft_dbg_gp_clock(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}
#endif

#ifdef FT_DEBUG_BULK_WINDOW
/*
 * =====================================================================
 * MEASUREMENT (opt-in: -DFT_DEBUG_BULK_WINDOW).  NOT part of the library.
 * =====================================================================
 *
 * PRICES G5.5.  Under G5.5 a point op, while a bulk op holds the move gate,
 * ADDS every ancestor lock up to the ROOT -- so the trie serializes for as
 * long as the gate is held.  The number that decides whether that is
 * affordable is the UNION WINDOW: the interval during which move_gate_nr > 0.
 *
 * ☠ IT IS NOT per-op duration x rate.  The gate is REFCOUNTED, so concurrent
 * movers COALESCE: a burst can hold ONE window open end to end, and the 0->1
 * owner's grace period is paid ONCE for all of them.  Multiplying a per-op
 * duration by a rate therefore overcounts windows and undercounts their
 * length.  Only the transitions can answer it, so that is what is recorded:
 *
 *   WINDOW  0->1 .. 1->0, the interval point ops would pay for;
 *   GAP     1->0 .. next 0->1, the trie's free time between windows;
 *   GP      the 0->1 owner's own update_synchronize_rcu().
 *
 * The GP split is the ACTIONABLE half: if a window is nearly all GP, G5.5's
 * cost is grace-period latency and the lever is opening fewer windows, not
 * shortening bulk bodies.  Attribution is exact -- the GP is timed by the
 * owner around its own call, never inferred across threads.
 *
 * Both transitions already happen under @move_gate_lock and there is exactly
 * one 0->1 and one 1->0 per window, so the open/close pairing is TOTAL and
 * needs no lock of its own.  @bw_t_open lives in struct cds_ft, not here,
 * because the gate is per-trie.
 *
 * ☠☠ THE GATE ALONE MEASURES THE CHEAPEST MEMBER OF THE FAMILY, so it is NOT
 * reported alone.  Only rekey enters the gate today (ft-rekey.h:3667, :5991,
 * plus the ft_unit-only hold-open shim at fractal-trie.c:303, whose windows are
 * a test fixture and not a measurement).  G5.5 must extend it to the other six
 * public bulk entries -- and THOSE have the long windows: exactly two functions
 * in the FT reach update_synchronize_rcu (ft-merge.h:2587), and the second,
 * ft_writer_lock_gp_wait, is called from INSIDE bulk bodies -- graft 4 sites,
 * detach 3, and rekey's own staged fallback 2.  The rekey ONE-DECIDE path the
 * gate hook does see was built to avoid mid-op GPs entirely (ft-rekey.h:3543),
 * so measuring it and calling it the family would bias the answer toward the
 * shortest member, in the direction that flatters G5.5.
 *
 * So the gate hook is the CALIBRATION POINT, and the number G5.5 actually needs
 * is the PER-CLASS BODY: a wall-clock bracket around each of the 7 public bulk
 * entries, which is the window each would open once the gate is extended over
 * it.  BODY already contains that op's in-body GPs; BODY_GP reports how much of
 * it they are.  A run without a given class records ZERO for it, which is a
 * CONFIGURATION MISS, not "that op has no window" -- so the dump says which
 * classes ran, in words.  Read the n= line before reading anything else.
 *
 * ☠ AND IT PRICES ONE HALF ONLY.  This measures EXPOSURE (how often, how long
 * the trie is serialized).  It cannot price the LIVENESS risk -- point ops
 * widened to the root spin-abort-retrying on a queueless acquire -- because
 * that regime DOES NOT EXIST in this binary: point ops do not widen yet.  No
 * hook on today's gate can observe it, and duration does not bound it (a
 * starving lane is on record here: the remove retry lane does not drain under
 * lock-holder preemption, and acquire backoff was refuted).  Liveness is the
 * first gate on the G5.5 BUILD, not a measurement that precedes it.
 */
#include <stdio.h>
#include <time.h>

#define FT_BW_BUCKETS	40
static unsigned long ft_bw_win[FT_BW_BUCKETS];
static unsigned long ft_bw_gap[FT_BW_BUCKETS];
static unsigned long ft_bw_gp[FT_BW_BUCKETS];
static unsigned long ft_bw_win_n, ft_bw_gap_n, ft_bw_gp_n;
static unsigned long ft_bw_win_sum, ft_bw_gap_sum, ft_bw_gp_sum;
static unsigned long ft_bw_win_max, ft_bw_gap_max, ft_bw_gp_max;
static unsigned long ft_bw_opens;	/* 0->1 transitions */
static unsigned long ft_bw_enters;	/* every enter: /opens = PIGGYBACK */
static unsigned long ft_bw_peak;	/* max movers coalesced in one window */
static unsigned long ft_bw_piggyback;	/* movers that WAITED on the owner's GP */
static uint64_t ft_bw_first_open, ft_bw_last_close;

/*
 * PER-CLASS BODY.  One bracket per public bulk entry; the index order is the
 * order they are declared in include/urcu/fractal-trie.h.
 */
enum ft_bw_class {
	FT_BW_GRAFT, FT_BW_GRAFT_SWAP, FT_BW_DETACH,
	FT_BW_MERGE_AT, FT_BW_REKEY_GRAFT, FT_BW_REKEY_MERGE, FT_BW_NR_CLASS,
};
/*
 * cds_ft_merge has no class of its own: it is the same-prefix case and
 * delegates to cds_ft_merge_at (ft-merge.h:3822), so it is counted there.
 * Bracketing both would count one op twice.
 */
static const char * const ft_bw_class_name[FT_BW_NR_CLASS] = {
	"graft", "graft_swap", "detach", "merge_at",
	"rekey_graft", "rekey_merge",
};
static unsigned long ft_bw_body[FT_BW_NR_CLASS][FT_BW_BUCKETS];
static unsigned long ft_bw_body_n[FT_BW_NR_CLASS];
static unsigned long ft_bw_body_sum[FT_BW_NR_CLASS];
static unsigned long ft_bw_body_max[FT_BW_NR_CLASS];
static unsigned long ft_bw_body_gp_sum[FT_BW_NR_CLASS];
static unsigned long ft_bw_body_gp_n[FT_BW_NR_CLASS];	/* ops with >=1 in-body GP */

/*
 * IN-BODY GRACE PERIODS.  The calling thread accumulates them at BOTH sites
 * that reach update_synchronize_rcu: ft_writer_lock_gp_wait and the gate's own
 * 0->1 wait.  __thread is correct HERE, because an op's body GPs are run by
 * the op's own thread.  The window's open and close are routinely DIFFERENT
 * threads, which is why @bw_t_open is per-trie instead.
 */
static __thread uint64_t ft_bw_gp_acc;
static __thread unsigned long ft_bw_gp_acc_n;

static inline
uint64_t ft_bw_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

static inline
void ft_bw_record(unsigned long *hist, unsigned long *n, unsigned long *sum,
		unsigned long *max, uint64_t ns)
{
	unsigned int b = 0;
	uint64_t v = ns;

	while (v >>= 1)
		b++;
	if (b >= FT_BW_BUCKETS)
		b = FT_BW_BUCKETS - 1;
	uatomic_inc(&hist[b]);
	uatomic_inc(n);
	uatomic_add(sum, (unsigned long) ns);
	if (ns > uatomic_load(max, CMM_RELAXED))
		uatomic_store(max, (unsigned long) ns, CMM_RELAXED);
}

static __attribute__((unused))
void ft_bw_dump_one(const char *tag, const unsigned long *hist,
		unsigned long n, unsigned long sum, unsigned long max)
{
	unsigned long cum = 0, p50 = 0, p90 = 0, p99 = 0;
	unsigned int i;

	fprintf(stderr, "FT BULKWIN %s: n=%lu mean=%luns max=%luns\n",
		tag, n, n ? sum / n : 0UL, max);
	for (i = 0; i < FT_BW_BUCKETS; i++) {
		if (!hist[i])
			continue;
		cum += hist[i];
		if (!p50 && cum * 100 >= n * 50)
			p50 = 1UL << i;
		if (!p90 && cum * 100 >= n * 90)
			p90 = 1UL << i;
		if (!p99 && cum * 100 >= n * 99)
			p99 = 1UL << i;
		fprintf(stderr, "  >=2^%-2u (%12lu ns) %10lu\n",
			i, 1UL << i, hist[i]);
	}
	/* log2 buckets: each percentile is a LOWER bound, exact to 2x. */
	fprintf(stderr, "FT BULKWIN %s: p50>=%luns p90>=%luns p99>=%luns\n",
		tag, p50, p90, p99);
}

/*
 * The bracket.  Wall clock plus the GP time this thread accumulated inside the
 * body, so BODY and BODY_GP are reported against the same interval.
 */
struct ft_bw_op {
	uint64_t t0;
	uint64_t gp0;
	unsigned long gpn0;
	enum ft_bw_class cls;
};

static inline
void ft_bw_op_begin(struct ft_bw_op *op, enum ft_bw_class cls)
{
	op->cls = cls;
	op->gp0 = ft_bw_gp_acc;
	op->gpn0 = ft_bw_gp_acc_n;
	op->t0 = ft_bw_now();
}

static inline
void ft_bw_op_end(struct ft_bw_op *op)
{
	uint64_t d = ft_bw_now() - op->t0;

	ft_bw_record(ft_bw_body[op->cls], &ft_bw_body_n[op->cls],
			&ft_bw_body_sum[op->cls], &ft_bw_body_max[op->cls], d);
	uatomic_add(&ft_bw_body_gp_sum[op->cls],
			(unsigned long) (ft_bw_gp_acc - op->gp0));
	if (ft_bw_gp_acc_n != op->gpn0)
		uatomic_inc(&ft_bw_body_gp_n[op->cls]);
}

static inline
void ft_bw_op_cleanup(struct ft_bw_op *op)
{
	ft_bw_op_end(op);
}

/*
 * Scoped so that EVERY return path is covered.  These entries reject bad
 * arguments with early returns.  A bracket that only wrapped the success path
 * would silently omit them.  Rejections do no work, so they land in the low
 * buckets.  The workloads here pass valid arguments, so rejections are cold.
 */
#ifdef FT_BW_ISOLATE_GATE
/* Perturbation A/B only: gate hooks armed, per-class body bracket removed. */
# define FT_BW_OP(cls)		do { } while (0)
#else
#define FT_BW_OP(cls)							\
	struct ft_bw_op ft_bw_op__					\
		__attribute__((cleanup(ft_bw_op_cleanup)));		\
	ft_bw_op_begin(&ft_bw_op__, (cls))
#endif

#ifdef FT_BW_OWNS_DUMP
__attribute__((destructor))
static
void ft_bw_dump(void)
{
	uint64_t span;

	unsigned int c;
	unsigned long any_body = 0;

	for (c = 0; c < FT_BW_NR_CLASS; c++)
		any_body += ft_bw_body_n[c];
	/*
	 * THE PER-CLASS BODY IS THE G5.5 NUMBER: the window each op would open
	 * once the gate is extended over it.  Printed FIRST, and printed even
	 * when the gate itself was never entered, because a graft-only or
	 * detach-only run has no windows today and still has the windows G5.5
	 * would create.
	 */
	if (any_body) {
		fprintf(stderr, "FT BULKWIN: --- PER-CLASS BODY (the PREDICTED "
			"G5.5 window per bulk op) ---\n");
		for (c = 0; c < FT_BW_NR_CLASS; c++) {
			if (!ft_bw_body_n[c]) {
				fprintf(stderr, "FT BULKWIN BODY %-12s: NOT "
					"EXERCISED (configuration miss, not a "
					"zero window)\n", ft_bw_class_name[c]);
				continue;
			}
			ft_bw_dump_one(ft_bw_class_name[c], ft_bw_body[c],
					ft_bw_body_n[c], ft_bw_body_sum[c],
					ft_bw_body_max[c]);
			fprintf(stderr, "FT BULKWIN BODY %-12s: in-body GP "
				"%.2f%% of body time, %lu/%lu ops took >=1 GP\n",
				ft_bw_class_name[c],
				ft_bw_body_sum[c] ? 100.0 *
					(double) ft_bw_body_gp_sum[c] /
					(double) ft_bw_body_sum[c] : 0.0,
				ft_bw_body_gp_n[c], ft_bw_body_n[c]);
		}
	} else {
		fprintf(stderr, "FT BULKWIN: NO BULK OP RAN AT ALL -- "
			"CONFIGURATION MISS, not a measurement.\n");
	}
	if (!ft_bw_opens) {
		fprintf(stderr, "FT BULKWIN: NO WINDOWS -- the move gate was "
			"never entered.  Only rekey enters it today "
			"(ft-rekey.h:3667,:5991); a graft/detach/merge run "
			"legitimately shows zero here while still having the "
			"per-class bodies above.  CONFIGURATION MISS, not a "
			"measurement.\n");
		return;
	}
	ft_bw_dump_one("WINDOW", ft_bw_win, ft_bw_win_n, ft_bw_win_sum,
			ft_bw_win_max);
	ft_bw_dump_one("GP", ft_bw_gp, ft_bw_gp_n, ft_bw_gp_sum, ft_bw_gp_max);
	ft_bw_dump_one("GAP", ft_bw_gap, ft_bw_gap_n, ft_bw_gap_sum,
			ft_bw_gap_max);
	/*
	 * COALESCING is why a rate cannot replace this number: @enters movers
	 * were served by @opens windows and @opens grace periods.
	 */
	fprintf(stderr, "FT BULKWIN: enters=%lu opens=%lu movers_per_window=%.2f "
		"waited_on_owner_GP=%lu peak_concurrent=%lu\n",
		ft_bw_enters, ft_bw_opens,
		ft_bw_opens ? (double) ft_bw_enters / (double) ft_bw_opens : 0.0,
		ft_bw_piggyback, ft_bw_peak);
	if (ft_bw_win_n != ft_bw_opens)
		fprintf(stderr, "FT BULKWIN: %lu window(s) STILL OPEN at exit "
			"and therefore NOT counted -- duty cycle is a LOWER "
			"bound.\n", ft_bw_opens - ft_bw_win_n);
	/*
	 * DUTY CYCLE over the measured span (first open .. last close), which
	 * is the fraction of the time a point op would have been serialized
	 * trie-wide.  Over the SPAN, not over process lifetime: setup and
	 * teardown are not workload.
	 */
	span = ft_bw_last_close - ft_bw_first_open;
	fprintf(stderr, "FT BULKWIN: span=%luns open=%luns DUTY=%.2f%% "
		"(GP share of open time=%.2f%%)\n",
		(unsigned long) span, ft_bw_win_sum,
		span ? 100.0 * (double) ft_bw_win_sum / (double) span : 0.0,
		ft_bw_win_sum ? 100.0 * (double) ft_bw_gp_sum /
				(double) ft_bw_win_sum : 0.0);
}
#endif /* FT_BW_OWNS_DUMP */
#endif /* FT_DEBUG_BULK_WINDOW */

#ifndef FT_DEBUG_BULK_WINDOW
# define FT_BW_OP(cls)		do { } while (0)
#endif

#ifdef FEATURE_FT_HOLD_TRACE

/*
 * THE SEAM-RULE ARM: no NODE lock may be held across a grace period.  Defined
 * with the hold ledger in ft-mutation-helpers.h, which is included long after
 * this header; forward-declared here the way ft_hold_trace_leak_canary is.
 */
static void ft_seam_check(const char *site);
#endif

#ifdef FT_DEBUG_SPLICE_SEAM
/*
 * THE SPLICE-SEAM PROBE (build knob; -DFT_DEBUG_SPLICE_SEAM).
 *
 * ft_glue_record_splices writes @src_head->prev with the SRC head's chain
 * holder NOT acquired (its own comment says so).  The exclusion is the FT-wide
 * writer lock plus the bulk gate.  The MW expected-old is the backstop.
 * Converting cds_ft_node.prev to SW removes that backstop.
 *
 * So one question must be answered FIRST.  A DRAIN SEAM is
 * ft_writer_lock_gp_wait: it DROPS the FT-wide lock and lets a point op in.
 * Can a drain seam land inside the window where a splice is recorded but not
 * yet committed?
 *
 * ☞ The in-tree claim is STATIC: "No ft_writer_lock_gp_wait exists anywhere
 * under the attempt" (ft-rekey.h).  A static claim about a call graph is a
 * test you can run.  @ft_splice_window is the window; the seam counts it.
 *
 * Declared as an opaque pointer because struct ft_glue is defined in
 * ft-mutation-helpers.h, which this header precedes.
 */
static __thread void *ft_splice_window;
static unsigned long ft_ss_seam_all __attribute__((unused)),
	ft_ss_seam_in_window __attribute__((unused));
#endif

#ifdef FT_DEBUG_BULK_ACQ
/*
 * -DFT_DEBUG_BULK_ACQ keeps two per-thread counts:
 *   - the drain seams this thread has taken;
 *   - that count when the thread's OUTERMOST bulk gate opened.
 * With them, an acquire inside a bulk op can say whether a seam already
 * happened earlier in that op.
 */
static __thread unsigned long ft_ba_seam_n, ft_ba_seam_at_gate;
/* ARM YIELD: seams taken inside a bulk op, and with an acquire after them. */
static unsigned long ft_ba_seams_in_bulk __attribute__((unused));
# define FT_BA_GATE_NOTE()						\
	do {								\
		if (!ft_bulk_self_depth)				\
			ft_ba_seam_at_gate = ft_ba_seam_n;		\
	} while (0)
#else
# define FT_BA_GATE_NOTE()	do { } while (0)
#endif
/*
 * ☠ THE NON-OWNER-CLEAR TABLE IS KEYED BY ADDRESS.  A freed and recycled item
 * would leave a stale "held by tid T" entry.  The next legitimate take of the
 * new node would then be reported as a steal.  So forget the word when its
 * item goes back to the allocator.  Declared here because ft-helpers.h, where
 * the frees live, precedes the table.
 *
 * FT_DT_ARMED comes from ft-txn-rec-dbg.h, which is included at the top of
 * this file.  The allocator's TU includes only this header.  So it cannot see
 * a different gate than the table does (see the arm's own comment).
 */
#if FT_DT_ARMED
void ft_dt_note_freed(const struct cds_ft_metadata *m);
void ft_dt_note_forgotten(const struct cds_ft_metadata *m);
# define FT_DT_NOTE_FREED(m_)	ft_dt_note_freed(m_)
# define FT_DT_NOTE_FORGOTTEN(m_)	ft_dt_note_forgotten(m_)
#else
# define FT_DT_NOTE_FREED(m_)	do { } while (0)
# define FT_DT_NOTE_FORGOTTEN(m_)	do { } while (0)
#endif

/*
 * INCARNATION MARKS FOR THE LOCK RING.  The ring is keyed by ADDRESS, so a
 * freed and recycled item makes ONE history out of TWO logical nodes.  Without
 * a boundary marker, every replay across a reuse is ambiguous.  One such
 * replay was misread as a single node's story (2026-09-23).
 *
 * Declared here because ft-helpers.h, where the FT's node alloc/free live, is
 * included long before the ring itself.  Defined with the ring, non-static,
 * the way ft_seam_check is.
 */
#if defined(FT_DEBUG_LOCK_LEAK) || defined(FT_ENABLE_TRACING)
/*
 * ☞ ALSO IN TRACE-ONLY MODE.  Gated on the RING alone, these marks vanished
 * from exactly the build that needs them.  There, the captured history of a
 * word shows two threads taking it with no release between.  Did the bit
 * vanish because the ITEM WAS RECYCLED?  A reuse boundary answers precisely
 * that.
 */
void ft_ll_mark_reuse(const struct cds_ft_metadata *m);
void ft_ll_mark_freed(const struct cds_ft_metadata *m);
# define FT_LL_MARK_REUSE(m_)	ft_ll_mark_reuse(m_)
# define FT_LL_MARK_FREED(m_)	ft_ll_mark_freed(m_)
#else
# define FT_LL_MARK_REUSE(m_)	do { } while (0)
# define FT_LL_MARK_FREED(m_)	do { } while (0)
#endif

#ifdef FEATURE_FT_FAULT_INJECT
/* Test-only seam hook: see ft_writer_lock_gp_wait.  NULL when unused. */
extern void (*cds_ft_fault_seam_hook)(struct cds_ft *ft);
#endif

#ifdef FT_DEBUG_SEAM
/*
 * ☞ -DFT_DEBUG_SEAM: THE SEAM RULE, CHECKED WHERE IT IS RELIED ON.
 *
 * ft_writer_lock_gp_wait drops the writer lock of @h for a grace period.  @h
 * is the trie this thread actually HOLDS.  It is not always the argument:
 * graft / rekey pass @src_ft while holding the destination's.  G5.25 lets FINE
 * point writers of @h through while the lock is down.  The rule that allows
 * it: the seam sits BETWEEN two consistent commits.
 *
 * The part of that rule a point writer can trip over structurally is the
 * ordered list.  Every writer primitive assumes the list is circular through
 * @h's own sentinel.  A bulk op that leaves a NULL or foreign end on @h here
 * broke the rule.  The whole-trie graft_swap did
 * (inv_graft_swap_whole_seam_points).  So name the seam and stop.
 *
 * The check costs two resolving loads per end.  The list cannot move under
 * us: the lock we still hold keeps point writers of @h off, and the bulk gate
 * drained the rest.
 */
/*
 * ...AND THE ROOT THE POINT WRITERS THIS SEAM ADMITS WILL RE-DESCEND FROM.
 * Defined with the FT helpers (ft-mutation-helpers.h) and forward-declared
 * here the way ft_seam_check is: this header precedes ft_flag_to_metadata.
 */
/* NOT static: other units of the library reach this seam too. */
void ft_seam_check_root(struct cds_ft *h, const void *site);

static __attribute__((noinline, unused))
void ft_seam_check_list(struct cds_ft *h, const void *site)
{
	struct urcu_txn_list_node *s, *first, *last, *fp, *ln;

	if (!h || !h->ordered_list)
		return;
	s = &h->ord_sentinel.node;
	first = urcu_txn_list_unmark(ft_txn_list_next_rcu(s));
	last = urcu_txn_list_unmark(ft_txn_list_prev_rcu(s));
	if (first == s && last == s)
		return;				/* empty: self-looped */
	fp = first ? urcu_txn_list_unmark(ft_txn_list_prev_rcu(first)) : NULL;
	ln = last ? urcu_txn_list_unmark(ft_txn_list_next_rcu(last)) : NULL;
	if (caa_likely(first && last && fp == s && ln == s))
		return;
	fprintf(stderr, "FT SEAM RULE: trie %p dropped its writer lock for a grace "
		"period with its ordered list NOT circular (first %p ->prev %p, "
		"last %p ->next %p, sentinel %p) at %p -- a bulk op left a "
		"transient end visible to the point writers this seam admits\n",
		(void *) h, (void *) first, (void *) fp, (void *) last,
		(void *) ln, (void *) s, site);
	fflush(stderr);
	abort();
}
#endif

static inline
void ft_writer_lock_gp_wait(struct cds_ft *ft)
{
#ifdef FT_DEBUG_BULK_ACQ
	ft_ba_seam_n++;
	if (ft_bulk_self_depth)
		uatomic_inc(&ft_ba_seams_in_bulk);
#endif
#ifdef FT_DEBUG_SPLICE_SEAM
	uatomic_inc(&ft_ss_seam_all);
	if (ft_splice_window)
		uatomic_inc(&ft_ss_seam_in_window);
#endif
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
	uint64_t ft_dbg_gp_t0 = ft_dbg_gp_clock();
#endif
	struct cds_ft *held = ft_wlock_held;

#ifdef FT_DEBUG_SEAM
	ft_seam_check_list(held, __builtin_return_address(0));
	ft_seam_check_root(held, __builtin_return_address(0));
#endif
	FT_BL_TALLY(ft_bl_seam_all);
	if (ft_bulk_lock_held != NULL) {
		FT_BL_TALLY(ft_bl_seam_under);
	} else {
		if (ft->exclusive)
			FT_BL_TALLY(ft_bl_seam_excl);
		if (ft_bulk_self_depth)
			FT_BL_TALLY(ft_bl_seam_selfdepth);

	}
	unsigned long depth = ft_wlock_depth;
	unsigned long own = 0;

	if (held) {
		own = ft_excl_owner_release(held);
		ft_wlock_held = NULL;
		ft_wlock_depth = 0;
#ifdef FT_DEBUG_WLOCK_HOLD
		ft_wlh_record(ft_wlh_hold, &ft_wlh_hold_n, &ft_wlh_hold_sum,
				&ft_wlh_hold_max, ft_wlh_now() - ft_wlh_acq_ns);
#endif
		(void) cds_fair_mutex_unlock(&held->writer_lock, &ft_wlock_waiter);
	}
#ifdef FEATURE_FT_FAULT_INJECT
	/*
	 * Test-only: let a test park this op at its seam, with the FT-wide
	 * lock released and the grace period not yet started.
	 */
	if (cds_ft_fault_seam_hook)
		cds_ft_fault_seam_hook(ft);
#endif
	/*
	 * NEVER a grace period while holding a txn escalation fallback.  A peer
	 * parked on that lock is an ONLINE, non-quiescent QSBR reader.  So it
	 * is exactly what would stop this grace period, and it is waiting for
	 * the lock this thread holds.  The two then wait on each other forever.
	 * That is the freeze already on record for the escalation path.
	 *
	 * Measured 0 of these across every concurrent oracle, against ~11000
	 * escalations in the heaviest.  So this asserts a property the code
	 * HAS.  It is here to name the cause the one time it is broken, because
	 * the wedge's own stacks blame the innocent.
	 */
	assert(!ft_op_in_lane());
#ifdef FEATURE_FT_HOLD_TRACE
	ft_seam_check("ft_writer_lock_gp_wait");
#endif
	/*
	 * And NEVER a grace period inside this thread's own writer-scope read
	 * sections.  Drop them for the wait and re-take them after, as the lock
	 * above.  Only a scope that waits for a grace period WITHOUT a bulk gate
	 * holds one here: cds_ft_make_exclusive on a FINE trie.  A bulk op's
	 * own scope never takes it (see ft_writer_lock_scope_enter).
	 */
	unsigned long nsec = ft_wscope_rcu_held;
	unsigned long isec;

	/*
	 * A scope that keeps a section never takes the FT-wide lock, so the
	 * re-acquire below (OFFLINE, ft_writer_lock_park) never runs with one.
	 */
	assert(!(held && nsec));
#ifdef FT_DEBUG_GP_KEEPS_SCOPE_SECTIONS
	nsec = 0;	/* red control: wait inside them */
#endif
	for (isec = 0; isec < nsec; isec++)
		ft_wscope_rcu_flavor->read_unlock();
#ifdef FT_DEBUG_BULK_WINDOW
	{
		uint64_t t0__ = ft_bw_now();

		ft->group->flavor->update_synchronize_rcu();
		ft_bw_gp_acc += ft_bw_now() - t0__;
		ft_bw_gp_acc_n++;
	}
#else
	ft->group->flavor->update_synchronize_rcu();
#endif
	for (isec = 0; isec < nsec; isec++)
		ft_wscope_rcu_flavor->read_lock();
	if (held) {
		/*
		 * OFFLINE for the re-acquire, or the drop above buys nothing when
		 * there is more than one writer: see ft_writer_lock_park.
		 */
		ft_writer_lock_park(held);
		ft_wlock_held = held;
		ft_wlock_depth = depth;
		ft_excl_owner_reclaim(held, own);
	}
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
	ft_dbg_gp_ns += ft_dbg_gp_clock() - ft_dbg_gp_t0;
	ft_dbg_gp_calls++;
#endif
}

/*
 * VISITED-NODE WITNESS for the two-from-root coherence check.
 *
 * A traversal that runs while an in-trie move is in flight can be TORN.  It can
 * resolve one edge on the old side of the move's commit and another on the new
 * side.  It then lands somewhere that was never correct at any instant.
 *
 * The witness is how a second traversal detects that the first one was torn.
 * Each pass accumulates the ADDRESSES of the nodes it visits, and the two
 * passes are compared.  Two SEQUENTIAL passes cannot both straddle the same
 * commit, because the second starts after the first ended.  So agreement means
 * neither was torn.
 *
 * The witness compares IDENTITY, not value:
 *   - a move COWs its stitch points, so a moved subtree's junction and top
 *     get FRESH addresses;
 *   - RCU cannot recycle a node inside a read section.
 * This makes the witness immune to the away-and-back (oscillating rekey) case.
 * That case fools any comparison of keys or result nodes.
 *
 * The witness is an order-dependent 64-bit hash plus a visit count, not a
 * stored set:
 *   - constant space;
 *   - no allocation on the read path;
 *   - order-sensitive: two passes over an unchanged structure visit the same
 *     nodes in the same order.
 * A hash collision would accept a torn pass, at ~2^-64 per comparison.
 *
 * @torn marks a pass that could not finish.  A walk that climbs parent
 * pointers counts levels from the depth it descended to.  A move to a key
 * of a different length re-parents the moved nodes at a new depth.  So a
 * pass that straddles such a move can run out of ancestors before it
 * reaches its level.  It then stops and tears its witness, and a torn
 * witness never compares equal.  Such a pass always crossed the move's
 * fresh copy, so the retry it forces is the one the address compare would
 * have forced had the pass finished.
 */
struct ft_visit_witness {
	uint64_t h;
	unsigned long n;
	bool torn;
};

static inline
void ft_witness_init(struct ft_visit_witness *w)
{
	w->h = 0xcbf29ce484222325ULL;	/* FNV-1a offset basis */
	w->n = 0;
	w->torn = false;
}

static inline
void ft_witness_tear(struct ft_visit_witness *w)
{
	w->torn = true;
}

static inline
void ft_witness_visit(struct ft_visit_witness *w, const void *node)
{
	uint64_t x = (uint64_t) (uintptr_t) node;

	/*
	 * splitmix64 finalizer before folding: node addresses are arena-strided
	 * and share their low and high bits, so mixing first keeps the fold from
	 * cancelling structurally-similar addresses.
	 */
	x += 0x9e3779b97f4a7c15ULL;
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
	x ^= x >> 31;
	w->h = (w->h ^ x) * 0x100000001b3ULL;
	w->n++;
}

static inline
bool ft_witness_equal(const struct ft_visit_witness *a,
		const struct ft_visit_witness *b)
{
	return !a->torn && !b->torn && a->h == b->h && a->n == b->n;
}

/*
 * Is an in-trie MOVE in flight (or about to be)?  The reader-side mode read of
 * the move gate: 0 means fast path with no coherence check.  See
 * struct cds_ft::move_active for two points:
 *   - why one sample per read section is sound;
 *   - why this is a plain load of a quiet word rather than a sequence counter.
 */
static inline
bool ft_move_active(const struct cds_ft *ft)
{
	/*
	 * ☞ ALWAYS-COHERENT TRIES ANSWER YES WITHOUT LOOKING.  See
	 * @rekey_always_coherent.  On such a trie readers never leave the
	 * coherent path, so there is no mode to sample.  More to the point,
	 * there is no mode TRANSITION for the move gate to fence with a grace
	 * period.  That is the whole reason the flag exists.
	 *
	 * Immutable after cds_ft_create, so this is a predictable branch on a
	 * value the reader already has in cache beside @move_active.
	 */
	if (ft->rekey_always_coherent)
		return true;
	return CMM_LOAD_SHARED(ft->move_active) != 0;
}

/*
 * Is any BULK op live?  This is the writer-side tier-1 gate that G5.5 turns
 * on.  It is false in steady state and costs ONE trie-level load.  When it is
 * true, a FINE trie's point op RE-TAKES the FT-wide writer lock instead of
 * dropping it.  Its ONE consumer is ft_writer_lock_scope_enter (G5.25), which
 * is where the whole bulk-vs-point exclusion lives.
 */
static inline
bool ft_bulk_active(const struct cds_ft *ft)
{
	return CMM_LOAD_SHARED(ft->bulk_state) != 0;
}

/*
 * ☞ DOES @bulk_state HAVE A CONSUMER ON THIS TRIE?
 *
 * The word has exactly one: ft_writer_lock_scope_enter's FINE branch.  A FINE
 * trie skips the FT-wide writer mutex UNLESS a bulk op is live.  Every other
 * writer mode already excludes the point writer without ever loading the word:
 *
 *   COARSE         every writer scope takes the FT-wide mutex unconditionally,
 *                  so bulk and point writers already arbitrate on one word;
 *   EXTERNAL_SYNC  the caller excludes every writer by contract, and the scope
 *                  returns BEFORE the FINE branch;
 *   exclusive      single writer by contract, same early return.
 *
 * On those tries the word is published, FENCED WITH A GRACE PERIOD, and read by
 * nobody.  (MATHIEU, 2026-09-20: "in external sync mode and coarse trie, there
 * is no fine locking writers to contend with, so the bulk lock and fence is
 * useless.")  Every other reference to ft_bulk_active in the tree is inside
 * FT_ANC_LEDGER / FT_DEBUG_WIDEN_OWNER / FT_DEBUG_TXN_KIND instrumentation.
 *
 * ☠ AND THE FENCE IS THE EXPENSIVE HALF.  A FT_BULK_WRITER_ONLY op publishes
 * NO reader word.  Those ops are detach, graft, graft_swap and merge_at.  So
 * @bulk_state is the only thing that can make the 0->1 opener own a grace
 * period.  On a coarse or external-sync trie that is a full synchronize_rcu
 * per bulk window, paid to fence a word nothing reads.
 *
 * A rekey (FT_BULK_COHERENT) is unaffected.  It owns its GP for @move_active,
 * which is a READER word and is needed in every mode.  Readers stay concurrent
 * under EXTERNAL_SYNC, which excludes writers and nothing else.
 *
 * ☠ THE STORE ITSELF STAYS UNCONDITIONAL, and deliberately.  It is one relaxed
 * word.  A trie can leave @exclusive / @external_sync mid-window
 * (cds_ft_make_shared).  With the store kept, that trie degrades to "peers
 * take the FT-wide lock", which is the conservative side.  Only the fence is
 * dropped, exactly as the exit already drops it ("No grace period is needed
 * on the way out").
 */
static inline
bool ft_bulk_state_has_consumer(const struct cds_ft *ft)
{
#ifdef FT_RED_BULK_FENCE_ALWAYS
	/*
	 * CONTROL, never a shipped configuration: fence unconditionally, which
	 * is the pre-2026-09-20 behaviour.  Here so the saving can be measured
	 * as an A/B in one tree rather than against a remembered number.
	 */
	(void) ft;
	return true;
#else
	return FT_BULK_WIDE_LOCK && ft->lock_fine && !ft->exclusive
		&& !ft->external_sync;
#endif
}

#ifdef FT_DEBUG_BULK_GATE_GP
/*
 * ARM YIELD for the fence above: a "saved" count with no "owned" beside it
 * cannot tell a real saving from a gate that never opens.
 */
__attribute__((weak)) unsigned long ft_bg_gp_owned, ft_bg_gp_saved,
	ft_bg_gp_coherent, ft_bg_gp_coherent_saved, ft_bg_reported;
static void ft_bg_report(void) __attribute__((destructor));
static void ft_bg_report(void)
{
	if (!ft_bg_gp_owned && !ft_bg_gp_saved && !ft_bg_gp_coherent
			&& !ft_bg_gp_coherent_saved)
		return;
	if (__atomic_fetch_add(&ft_bg_reported, 1, __ATOMIC_RELAXED))
		return;
	fprintf(stderr, "FT_BULK_GATE_GP  writer-word opens: fenced=%lu "
		"SAVED(no consumer)=%lu | reader-word (move_active) GPs=%lu "
		"SAVED(always-coherent)=%lu\n",
		ft_bg_gp_owned, ft_bg_gp_saved, ft_bg_gp_coherent,
		ft_bg_gp_coherent_saved);
}
# define FT_BG_TALLY(c)	__atomic_fetch_add(&(c), 1, __ATOMIC_RELAXED)
#else
# define FT_BG_TALLY(c)	do { } while (0)
#endif

/*
 * Which words a bulk op publishes.  Only a REKEY needs reader coherence; every
 * other bulk op publishes the writer word alone.
 */
enum ft_bulk_kind {
	FT_BULK_WRITER_ONLY,	/* detach / graft / graft_swap / merge_at */
	FT_BULK_COHERENT,	/* rekey: also publishes @move_active */
};

/*
 * ☠ AM I MYSELF A BULK OP?  A bulk op must not widen its own point-op helpers
 * against the gate it is holding.  This is a depth count, not a flag, because
 * bulk bodies nest (a rekey's staged fallback re-enters).  The inner exit
 * would clear a flag while the outer op is still live.
 */
extern __thread unsigned long ft_bulk_self_depth;

#ifdef FT_DEBUG_WIDEN_OWNER
extern unsigned long ft_wo_gate_enters;
#endif

/*
 * Enter the move mode gate: publish "expect a move" to readers and make sure
 * EVERY live reader has observed it before the caller mutates anything.
 *
 * Who pays the grace period:
 *   - the 0->1 mover owns it;
 *   - movers that arrive while that GP is in flight PIGGYBACK it: they wait,
 *     and do not start another;
 *   - movers that arrive after it completed proceed immediately.  By then
 *     @move_active has been set for at least one full GP, so the "all live
 *     readers see it" invariant already holds.
 * A burst of moves therefore costs ~one GP, not one per move.
 *
 * BLOCKING, and therefore NOT callable from an RCU read-side critical section:
 * the GP would wait for the caller's own section.  A mover takes its read lock
 * (for the descents it then does) AFTER this returns.
 */
/*
 * THE RECLAIM ROUTE.  Shared by the climb audit and by FT_ENABLE_TRACING's
 * item_reclaim event, so it must not live inside either one's guard.
 */
#ifdef FT_ENABLE_TRACING
#define FT_DBG_VIA_RCU		1	/* call_rcu callback: GP paid */
#define FT_DBG_VIA_EXCLUSIVE	2	/* exclusive trie: no readers by contract */
#define FT_DBG_VIA_UNPUB	3	/* "never published", freed immediately */
#define FT_DBG_VIA_DRAIN	4	/* reserve drain: no GP, no defer */
extern __thread unsigned long ft_dbg_free_via;
#endif



static inline
void ft_bulk_gate_enter_gp(struct cds_ft *ft, enum ft_bulk_kind kind)
{
	bool own_gp = false;

#ifdef FT_DEBUG_BULK_ELEV
	ft_bulk_gate_depth++;		/* see @ft_bulk_elev_self */
	uatomic_inc(&ft_bulk_gate_calls);
#endif

#ifdef FT_DEBUG_WIDEN_OWNER
	/*
	 * ☠ THE CONTROL FOR THE OTHER ZERO, and it must live under THIS flag.
	 * Without it "no acquire ever saw a live bulk op" cannot be told from
	 * "no bulk op ever ran".  The bulk-window instrument has its own enter
	 * counter, but behind a DIFFERENT -D.  So reading it here would be a
	 * feature-flag-matrix miss reported as a measurement.
	 */
	uatomic_inc(&ft_wo_gate_enters);
#endif
	pthread_mutex_lock(&ft->move_gate_lock);
#ifdef FT_DEBUG_BULK_WINDOW
	/*
	 * The window measured is the WRITER word's (@bulk_gate_nr > 0), because
	 * that is the interval G5.5 makes point ops pay for.  Under
	 * @move_gate_lock, so it inherits the gate's serialization.
	 *
	 * A PIGGYBACK is an enter that actually WAITED on an owner's GP.  It is
	 * not merely any non-owner.  An arrival after the GP completed waits
	 * for nothing, and counting it would overstate "a burst pays one GP".
	 */
	ft->bw_enters++;
	if (ft->bulk_gate_nr == 0)
		ft->bw_t_open = ft_bw_now();
	else if (ft->gate_gp_nr)
		ft->bw_piggyback++;
	if (ft->bulk_gate_nr + 1 > ft->bw_peak)
		ft->bw_peak = ft->bulk_gate_nr + 1;
#endif
	/*
	 * Publish each word this op needs that is not already published, and
	 * OWN A GRACE PERIOD if it published anything.  Both words need the
	 * same discipline: publish, one full GP, only THEN mutate.  A peer that
	 * sampled the word as clear before the store is still running on the
	 * old rule:
	 *   - for @move_active, a reader on the fast path;
	 *   - for @bulk_active (G5.25), a point op that has NOT taken the
	 *     FT-wide writer lock.  It sampled the gate clear at its writer
	 *     scope and skipped the lock.  So it is not excluded against this
	 *     op yet.
	 */
	if (ft->bulk_gate_nr++ == 0) {
		/*
		 * Fence @bulk_state only where something LOADS it; see
		 * ft_bulk_state_has_consumer.  Note this decides the FENCE
		 * alone -- the store below is unconditional.
		 */
		if (ft_bulk_state_has_consumer(ft)) {
			own_gp = true;
			FT_BG_TALLY(ft_bg_gp_owned);
		} else
			FT_BG_TALLY(ft_bg_gp_saved);
	}
	if (kind == FT_BULK_COHERENT && ft->move_gate_nr++ == 0) {
		CMM_STORE_SHARED(ft->move_active, 1);
		/*
		 * A READER word, so every mode that has a reader to TRANSITION
		 * needs the fence.  An ALWAYS-COHERENT trie has none.  Its
		 * readers were on the two-pass path before this store and will
		 * be after it; ft_move_active does not even load the word.  The
		 * grace period would be waiting for a mode change that is not
		 * happening.
		 *
		 * ☠ AND ON SUCH A TRIE IT IS NOT MERELY WASTE.  @external_sync
		 * means the APPLICATION's writer exclusion is held across this
		 * call.  So this GP would wait while holding a lock the library
		 * cannot drop (see @rekey_always_coherent).
		 *
		 * The store stays, for the reason the @bulk_state store does.
		 * It is one relaxed word, and it keeps the two paths reading
		 * alike.
		 */
		if (!ft->rekey_always_coherent) {
			own_gp = true;
			FT_BG_TALLY(ft_bg_gp_coherent);
		} else
			FT_BG_TALLY(ft_bg_gp_coherent_saved);
	}
	CMM_STORE_SHARED(ft->bulk_state, ft->bulk_gate_nr);
	if (own_gp) {
#ifdef FT_DEBUG_ES_GP
		{
			/* The GATE's own grace period, same question. */
			static struct ft_es_gp_site _es_gate = {
				.file = __FILE__, .line = __LINE__ };

			ft_es_gp_note(&_es_gate, ft);
		}
#endif
		ft->gate_gp_nr++;
		pthread_mutex_unlock(&ft->move_gate_lock);
		/*
		 * The barrier the whole gate exists for.  Peers that were
		 * already inside a critical section when the store landed may
		 * still believe they are in the old mode.  Let them finish.
		 */
		assert(!ft_op_in_lane());	/* see gp_wait */
		/*
		 * A bulk op enters its gate before any writer scope, so it
		 * holds none of the scope read sections this GP would wait on.
		 */
		assert(!ft_wscope_rcu_held);
#ifdef FEATURE_FT_HOLD_TRACE
		ft_seam_check("ft_bulk_gate_enter");
#endif
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
		{
			uint64_t t0__ = ft_dbg_gp_clock();

			ft->group->flavor->update_synchronize_rcu();
			ft_dbg_gp_ns += ft_dbg_gp_clock() - t0__;
			ft_dbg_gp_calls++;
		}
#elif defined(FT_DEBUG_BULK_WINDOW)
		{
			uint64_t t0__ = ft_bw_now(), d__;

			ft->group->flavor->update_synchronize_rcu();
			d__ = ft_bw_now() - t0__;
			ft_bw_record(ft_bw_gp, &ft_bw_gp_n, &ft_bw_gp_sum,
					&ft_bw_gp_max, d__);
			ft_bw_gp_acc += d__;
			ft_bw_gp_acc_n++;
		}
#else
		ft->group->flavor->update_synchronize_rcu();
#endif
		pthread_mutex_lock(&ft->move_gate_lock);
		ft->gate_gp_nr--;
		pthread_cond_broadcast(&ft->move_gate_cond);
		pthread_mutex_unlock(&ft->move_gate_lock);
		FT_BA_GATE_NOTE();
		ft_bulk_self_depth++;
		return;
	}
	if (ft->gate_gp_nr) {
		/*
		 * PIGGYBACK, and go QUIESCENT while doing it.  Under a
		 * quiescent-state flavor (QSBR) a registered thread counts as
		 * being inside a read section until it reports otherwise.  So
		 * a waiter that simply blocked here would be a reader the
		 * owner's grace period waits for.  The owner would wait for us
		 * and we would wait for the owner.  That is a hard deadlock.
		 * It is what happens without these two calls (measured: the
		 * 16-writer oracle wedged).  Sound because a bulk op holds NO
		 * read section at this point: the gate is entered BEFORE the
		 * read lock, by contract.  A no-op on the memb / mb flavors.
		 *
		 * ☠ WAIT FOR ALL in-flight GPs, not for "the" one.  Our words
		 * are already published.  But a peer may still be inside the
		 * very GP that makes one of them safe to act on.  A waiter
		 * cannot cheaply tell which GP covers which word.  Waiting for
		 * every one is conservative and never wrong.  Waiting for the
		 * wrong one would let us mutate against a peer that has not yet
		 * observed our word.
		 */
		ft->group->flavor->thread_offline();
		do {
			pthread_cond_wait(&ft->move_gate_cond,
					&ft->move_gate_lock);
		} while (ft->gate_gp_nr);
		pthread_mutex_unlock(&ft->move_gate_lock);
		ft->group->flavor->thread_online();
		FT_BA_GATE_NOTE();
		ft_bulk_self_depth++;
		return;
	}
	pthread_mutex_unlock(&ft->move_gate_lock);
	FT_BA_GATE_NOTE();
		ft_bulk_self_depth++;
}

/*
 * Take @bulk_lock: the BULK-vs-BULK serializer, held continuously across the
 * drain seams.
 *
 * Parks OFFLINE.  That is legal here and NOT at the writer scope's entry.
 * Every waiter on this lock is another bulk op, and a bulk entry forbids a
 * read-section caller.  So quiescing exposes no reader-derived reference, and
 * a waiter holds up no grace period.
 *
 * Reentrant on the same trie (bulk bodies nest).  An EXCLUSIVE trie skips it
 * for the same reason it skips the writer lock: no concurrent writer exists.
 */

static inline
void ft_bulk_lock_enter(struct cds_ft *ft)
{
	const struct rcu_flavor_struct *flavor = ft->group->flavor;

	if (ft->exclusive || ft->external_sync)
		return;
	if (ft_bulk_lock_held == ft) {
		ft_bulk_lock_depth++;
		FT_BL_TALLY(ft_bl_nested);
		return;
	}
	if (caa_unlikely(ft_bulk_lock_held != NULL)) {
		/*
		 * Two bulk locks at once.  A bulk op gates ONE live trie (a
		 * cross-trie op's source is exclusive and skips this).  So a
		 * second live trie here would be a lock-order inversion waiting
		 * to deadlock.  Same disposition as the FT-wide lock's nest
		 * check above: refuse loudly.  An assert would be compiled out
		 * of a release build, which would then leak a lock.
		 */
		fprintf(stderr, "[Fatal] Fractal Trie: bulk lock already held "
				"on %p while entering %p\n",
			(void *) ft_bulk_lock_held, (void *) ft);
		abort();
	}
	flavor->thread_offline();
	cds_fair_mutex_lock(&ft->bulk_lock, &ft_bulk_lock_waiter);
	flavor->thread_online();
	ft_bulk_lock_held = ft;
	ft_bulk_lock_depth = 1;
	FT_BL_TALLY(ft_bl_takes);
}

static inline
void ft_bulk_lock_exit(struct cds_ft *ft)
{
	/*
	 * Keys off HOLDER IDENTITY, never off @ft->exclusive, exactly as
	 * ft_writer_lock_scope_exit does.  Re-reading @exclusive here would
	 * LEAK the lock if the flag flipped between enter and exit.  The op
	 * took the lock on a live trie and would then decline to release it.
	 */
	if (ft_bulk_lock_held != ft)
		return;
	if (--ft_bulk_lock_depth)
		return;
	ft_bulk_lock_held = NULL;
	(void) cds_fair_mutex_unlock(&ft->bulk_lock, &ft_bulk_lock_waiter);
}

/*
 * Enter the bulk window: the GATE first, then the BULK-vs-BULK lock.
 *
 * ORDER IS THE POINT.  The gate is refcounted and owns the grace period.
 * Entering it first keeps the piggyback: peers share the GP inside the gate
 * and only then queue on @bulk_lock.  That is what makes this true: "a burst
 * of concurrent moves pays about ONE grace period between them, not one
 * each".  Taking the lock first would serialize the GPs and make the burst
 * pay one EACH.
 */
static inline
void ft_bulk_gate_enter(struct cds_ft *ft, enum ft_bulk_kind kind)
{
	ft_bulk_gate_enter_gp(ft, kind);
	ft_bulk_lock_enter(ft);
}

/*
 * The rekey spelling, unchanged for its callers: a move needs BOTH words.
 */
static inline
void ft_move_gate_enter(struct cds_ft *ft)
{
	ft_bulk_gate_enter(ft, FT_BULK_COHERENT);
}

/*
 * Leave the move gate; the LAST mover out returns readers to the fast path.  No
 * grace period is needed on the way out.  A reader that still sees
 * @move_active set merely runs the coherent path once more.  That is never
 * wrong, only slower.
 */
/*
 * THE GATE'S EXIT PREMISE, MADE CHECKABLE.
 *
 * ft_bulk_gate_exit clears @move_active with NO grace period.  The reason it
 * is allowed to is stated below, and has two halves:
 *   - a reader that still sees the word set merely runs the conservative
 *     rule once more;
 *   - a reader that sees it CLEAR starts after the structure is final.
 * The second half is a claim about the MOVER: that it has published
 * everything before it clears.  Nothing checked it.
 *
 * A flip-txn that is still open is exactly "something not published yet".  So
 * count them per thread and assert none is left at the last bracket.
 *
 * The counter is also REPORTED (max seen).  A silent zero here could mean
 * "the premise holds".  It could also mean "this TU's copy of the counter was
 * never incremented".  This codebase has been bitten by that per-TU wrong
 * zero more than once.
 */
#if defined(DEBUG_RCU) || defined(CONFIG_RCU_DEBUG) || defined(FT_DEBUG_GATE_PENDING)
static __thread unsigned long ft_flip_txn_open_nr;
static unsigned long ft_flip_txn_open_max;
# define FT_TXN_OPEN_INC()						\
	do {								\
		if (++ft_flip_txn_open_nr >				\
				uatomic_read(&ft_flip_txn_open_max))	\
			uatomic_set(&ft_flip_txn_open_max,		\
				ft_flip_txn_open_nr);			\
	} while (0)
# define FT_TXN_OPEN_DEC()	(ft_flip_txn_open_nr--)
static __attribute__((destructor)) void ft_txn_open_report(void)
{
	if (uatomic_read(&ft_flip_txn_open_max))
		fprintf(stderr, "FT GATE PENDING: deepest open flip-txn nesting seen %lu (0 left at every gate exit)\n",
			uatomic_read(&ft_flip_txn_open_max));
}
#else
# define FT_TXN_OPEN_INC()	do { } while (0)
# define FT_TXN_OPEN_DEC()	do { } while (0)
#endif

static inline
void ft_bulk_gate_exit(struct cds_ft *ft, enum ft_bulk_kind kind)
{
	/* Reverse of the acquire order: @bulk_lock is INSIDE the gate. */
	ft_bulk_lock_exit(ft);
	ft_bulk_self_depth--;
#ifdef FT_DEBUG_BULK_ELEV
	ft_bulk_gate_depth--;		/* see @ft_bulk_elev_self */
#endif
	pthread_mutex_lock(&ft->move_gate_lock);
	/*
	 * Each word clears on ITS OWN refcount reaching zero.  No grace period
	 * on the way out.  A peer that still sees a word set merely runs the
	 * more conservative rule once more.  That is never wrong, only slower.
	 */
	if (kind == FT_BULK_COHERENT && --ft->move_gate_nr == 0) {
#if defined(DEBUG_RCU) || defined(CONFIG_RCU_DEBUG) || defined(FT_DEBUG_GATE_PENDING)
		/* Nothing of this mover's may still be unpublished here. */
		urcu_assert_debug(!ft_flip_txn_open_nr);
#endif
		CMM_STORE_SHARED(ft->move_active, 0);
	}
	if (--ft->bulk_gate_nr) {
		CMM_STORE_SHARED(ft->bulk_state, ft->bulk_gate_nr);
	} else {
		CMM_STORE_SHARED(ft->bulk_state, 0);
#ifdef FT_DEBUG_BULK_WINDOW
		{
			uint64_t now__ = ft_bw_now();

			ft_bw_record(ft_bw_win, &ft_bw_win_n, &ft_bw_win_sum,
					&ft_bw_win_max, now__ - ft->bw_t_open);
			uatomic_inc(&ft_bw_opens);
			if (ft->bw_t_close)
				ft_bw_record(ft_bw_gap, &ft_bw_gap_n,
						&ft_bw_gap_sum, &ft_bw_gap_max,
						ft->bw_t_open - ft->bw_t_close);
			uatomic_add(&ft_bw_enters, ft->bw_enters);
			uatomic_add(&ft_bw_piggyback, ft->bw_piggyback);
			if (ft->bw_peak > uatomic_load(&ft_bw_peak, CMM_RELAXED))
				uatomic_store(&ft_bw_peak, ft->bw_peak,
						CMM_RELAXED);
			ft->bw_enters = 0;
			ft->bw_piggyback = 0;
			ft->bw_peak = 0;
			if (!ft_bw_first_open)
				ft_bw_first_open = ft->bw_t_open;
			ft->bw_t_close = now__;
			ft_bw_last_close = now__;
		}
#endif
	}
	pthread_mutex_unlock(&ft->move_gate_lock);
}

static inline
void ft_move_gate_exit(struct cds_ft *ft)
{
	ft_bulk_gate_exit(ft, FT_BULK_COHERENT);
}

struct ft_bulk_gate_scope {
	struct cds_ft *ft;
	enum ft_bulk_kind kind;
};

static inline
void ft_bulk_gate_scope_end(struct ft_bulk_gate_scope *s)
{
	ft_bulk_gate_exit(s->ft, s->kind);
}

/*
 * The gate is scoped because the public bulk entries reject arguments with
 * EARLY RETURNS.  A bracket that covered only the success path would leak the
 * gate.  @bulk_active would then stay set for the life of the trie.  Every
 * point op would serialize on the FT-wide writer lock forever.
 *
 * PLACE IT AFTER the cheap argument checks and BEFORE any lock or read
 * section.  The gate BLOCKS on a grace period, so:
 * - a caller already inside an RCU read section would make the grace period
 *   wait on itself;
 * - a caller already holding a node lock would break the seam rule
 *   (internal.h: no node lock may be held across a GP).
 */
#define CDS_FT_SCOPED_BULK_GATE(ft_, kind_)				\
	struct ft_bulk_gate_scope ft_bulk_gate_scope__			\
		__attribute__((cleanup(ft_bulk_gate_scope_end))) =	\
		{ (ft_), (kind_) };					\
	ft_bulk_gate_enter((ft_), (kind_))

struct ft_excl_reader_scope {
	struct cds_ft *ft;
#ifdef FEATURE_FT_EXCL_VALIDATE
	bool claimed;	/* true iff we incremented ft->excl_nr_readers. */
#endif
};

#ifdef FEATURE_FT_EXCL_VALIDATE

/*
 * Report an access-discipline violation and abort.  This is a macro rather
 * than a vfprintf wrapper, for two reasons.  The caller's varargs forward
 * straight to fprintf, whose own attribute format-checks them.  And abort()
 * keeps it noreturn.
 */
#define ft_excl_abort(...)						\
	do {								\
		fprintf(stderr, "FT access-discipline violation: "	\
			__VA_ARGS__);					\
		fflush(stderr);						\
		abort();						\
	} while (0)

/*
 * This thread's writer nesting, for the FINE path below:
 * - @ft_excl_self_ft and @ft_excl_self_depth mirror ft_wlock_held and
 *   ft_wlock_depth.  They track one trie: the live dst of a cross-trie op.
 * - @ft_excl_self_any counts writer scopes on ANY trie.
 *
 * The reader check aborts only when ALL of this thread's scopes are accounted
 * for on the trie being read.  So an untracked second trie costs a missed
 * report, never a false one.
 */
static __thread struct cds_ft *ft_excl_self_ft;
static __thread unsigned long ft_excl_self_depth;
static __thread unsigned long ft_excl_self_any;

static inline
unsigned long ft_excl_self_writers(const struct cds_ft *ft)
{
	return ft_excl_self_ft == ft ? ft_excl_self_depth : 0;
}

static inline
bool ft_excl_writer_enter(struct cds_ft *ft)
{
	unsigned long self = (unsigned long) pthread_self();
	unsigned long prev, nr;
	bool rcu;

	/*
	 * MW lock-mode: take the FT-wide writer lock at the outermost scope
	 * before the discipline checks, so they run single-writer.  No-op on an
	 * optimistic trie.
	 */
	rcu = ft_writer_lock_scope_enter(ft);
	ft_excl_self_any++;
	if (ft_excl_self_ft == ft) {
		ft_excl_self_depth++;
	} else if (!ft_excl_self_ft) {
		ft_excl_self_ft = ft;
		ft_excl_self_depth = 1;
	}
	if (ft->lock_fine && !ft->exclusive) {
		/*
		 * CONCURRENT + FINE is the ONLY combination where
		 * writer/writer overlap conforms to the contract.  A FINE
		 * trie skips the FT-wide writer mutex.  Writers on disjoint
		 * subtrees proceed in parallel, and only structural
		 * collisions serialize.  That is the POINT of fine locking.
		 * The single-owner CAS aborted on that overlap.  So every
		 * concurrent oracle tripped this validator by construction,
		 * and it could only ever run single-threaded.  Count writers
		 * instead; the reader check below uses the count.
		 *
		 * The other combinations KEEP the owner CAS, and it is the
		 * whole point there:
		 * - An EXCLUSIVE trie is one whose caller promised to
		 *   serialize access.  Overlap is exactly the user error
		 *   this validator exists to catch.  An exclusive trie is
		 *   FINE by default, so gating on lock_fine alone would
		 *   have disabled the check where it matters most.
		 * - A COARSE trie serializes under the FT-wide mutex, so its
		 *   owner word is accurate anyway.
		 */
		uatomic_add(&ft->excl_nr_writers, 1);
	} else {
		prev = uatomic_cmpxchg(&ft->excl_owner, 0, self);
		if (prev != 0) {
			if (prev == self) {
				/* Reentry from the same thread (e.g. graft_swap
				 * delegating to graft). */
				ft->excl_writer_depth++;
				return rcu;
			}
			ft_excl_abort("cds_ft=%p: writer conflict -- owner 0x%lx, entering thread 0x%lx\n",
				(void *) ft, prev, self);
		}
		ft->excl_writer_depth = 1;
	}
	/*
	 * A non-zero reader count means a reader is asserting mutual
	 * exclusion against us.  In exclusive mode every reader claims.
	 * In concurrent mode, only the readers that did not hold the RCU
	 * read-side lock at entry claim.  Either way, overlap is a
	 * contract violation.
	 */
	nr = uatomic_load(&ft->excl_nr_readers, CMM_ACQUIRE);
	if (nr != 0)
		ft_excl_abort("cds_ft=%p: writer 0x%lx entering with %lu concurrent reader(s) (%s mode)\n",
			(void *) ft, self, nr,
			ft->exclusive ? "exclusive" : "concurrent without RCU read-side lock");
	return rcu;
}

static inline
void ft_excl_writer_exit(struct cds_ft *ft)
{
	if (ft->lock_fine && !ft->exclusive) {
		uatomic_sub(&ft->excl_nr_writers, 1);
	} else if (--ft->excl_writer_depth == 0) {
		uatomic_store(&ft->excl_owner, 0, CMM_RELEASE);
	}
	ft_excl_self_any--;
	if (ft_excl_self_ft == ft && --ft_excl_self_depth == 0)
		ft_excl_self_ft = NULL;
}

static inline
struct ft_excl_reader_scope ft_excl_reader_enter(struct cds_ft *ft)
{
	struct ft_excl_reader_scope scope = { .ft = ft, .claimed = false };
	unsigned long owner;

	if (!ft->exclusive) {
		/*
		 * Concurrent mode: a well-formed reader either holds
		 * the RCU read-side lock or guarantees external mutual
		 * exclusion against mutators.  RCU-protected readers
		 * may overlap a single writer, so they're a no-op here.
		 * Mutex-claim readers fall through and are validated
		 * just like exclusive-mode readers.
		 */
		if (ft->group->flavor->read_ongoing())
			return scope;
	}
	scope.claimed = true;
	uatomic_add(&ft->excl_nr_readers, 1);
	owner = uatomic_load(&ft->excl_owner, CMM_ACQUIRE);
	if (owner != 0 && owner != (unsigned long) pthread_self())
		ft_excl_abort("cds_ft=%p: reader 0x%lx entering with writer 0x%lx active (%s mode)\n",
			(void *) ft, (unsigned long) pthread_self(), owner,
			ft->exclusive ? "exclusive" : "concurrent without RCU read-side lock");
	if (ft->lock_fine && !ft->exclusive) {
		unsigned long mine = ft_excl_self_writers(ft);
		unsigned long nw = uatomic_load(&ft->excl_nr_writers, CMM_ACQUIRE);

		/*
		 * A FINE trie claims no owner, so ask the count.  Abort only
		 * when the excess provably belongs to ANOTHER thread.  For
		 * that, this thread's own scopes must all be accounted for
		 * on THIS trie (@ft_excl_self_any == @mine).  Otherwise an
		 * untracked second trie would make us report our own write
		 * as a peer's.
		 */
		if (nw > mine && ft_excl_self_any == mine)
			ft_excl_abort("cds_ft=%p: reader 0x%lx entering with %lu concurrent writer(s) (concurrent without RCU read-side lock)\n",
				(void *) ft, (unsigned long) pthread_self(),
				nw - mine);
	}
	return scope;
}

static inline
void ft_excl_reader_exit_scope(const struct ft_excl_reader_scope *scope)
{
	if (scope->claimed)
		uatomic_sub(&scope->ft->excl_nr_readers, 1);
}

#else /* !FEATURE_FT_EXCL_VALIDATE */

static inline bool ft_excl_writer_enter(struct cds_ft *ft)
{
	/* MW lock-mode FT-wide lock; no-op on an optimistic trie. */
	return ft_writer_lock_scope_enter(ft);
}
static inline void ft_excl_writer_exit(struct cds_ft *ft)  { (void) ft; }
static inline
struct ft_excl_reader_scope ft_excl_reader_enter(struct cds_ft *ft)
{
	struct ft_excl_reader_scope scope = { .ft = ft };

	return scope;
}
static inline
void ft_excl_reader_exit_scope(const struct ft_excl_reader_scope *scope)
{
	(void) scope;
}

#endif /* FEATURE_FT_EXCL_VALIDATE */

/*
 * One writer scope: the trie, and whether its enter took a read-side section of
 * the trie's flavor (ft_writer_lock_scope_enter's FINE arm).  Carried in the
 * scope object rather than re-derived at exit, so the exit drops exactly what
 * this enter took, whatever the gate reads by then.
 */
struct ft_writer_scope {
	struct cds_ft *ft;
	bool rcu;
};

static inline
struct ft_writer_scope ft_excl_writer_scope_enter(struct cds_ft *ft)
{
	struct ft_writer_scope scope = { .ft = ft };

	scope.rcu = ft_excl_writer_enter(ft);
	return scope;
}

static inline
void ft_excl_writer_scope_exit(struct ft_writer_scope *scope)
{
	struct cds_ft *ft = scope->ft;

#ifdef FEATURE_FT_VERIFY_AT_MUTATION
	/*
	 * Verify before releasing the writer claim so a concurrent
	 * writer cannot start mutating while we walk the trie.  Runs under
	 * the FT-wide lock (released last, below).
	 */
	ft_writer_scope_verify(ft);
#endif
	ft_excl_writer_exit(ft);
	/*
	 * MW lock-mode: release the FT-wide writer lock at the outermost
	 * scope, after the discipline checks / verify walked the trie under
	 * it.  No-op on an optimistic trie.
	 */
	ft_writer_lock_scope_exit(ft);
	/* Last out: the section the enter took first. */
	if (scope->rcu) {
		ft_wscope_rcu_held--;
		ft->group->flavor->read_unlock();
	}
}
static inline
void ft_excl_reader_scope_exit(struct ft_excl_reader_scope *scope)
{
	ft_excl_reader_exit_scope(scope);
}

/*
 * Two-level token-paste so __COUNTER__ is expanded before the
 * concatenation, producing a unique variable name for every use.
 * This allows multiple CDS_FT_SCOPED_* per function body (e.g. the
 * two-ft graft / graft_swap paths).
 */
#define CDS_FT_CAT2_(a, b) a ## b
#define CDS_FT_CAT_(a, b) CDS_FT_CAT2_(a, b)

#define CDS_FT_SCOPED_WRITER(ft)					\
	struct ft_writer_scope CDS_FT_CAT_(_ft_excl_scope_, __COUNTER__)	\
		__attribute__((unused,					\
			cleanup(ft_excl_writer_scope_exit))) =		\
		ft_excl_writer_scope_enter(ft)

#define CDS_FT_SCOPED_READER(ft)					\
	struct ft_excl_reader_scope CDS_FT_CAT_(_ft_excl_scope_, __COUNTER__) \
		__attribute__((unused,					\
			cleanup(ft_excl_reader_scope_exit))) =		\
		ft_excl_reader_enter(ft)

/*
 * MW lock-mode: hard guard at the entry of a CROSS-trie op.  Such an op is a
 * graft, merge or graft_swap between DISTINCT tries @a and @b.  The guard runs
 * before the two per-ft writer scopes are taken.
 *
 * It fires only when BOTH tries are LIVE (non-exclusive) lock-mode tries.  The
 * two writer scopes of that pair would take two FT-wide locks with no lock
 * order.  They could deadlock: thread 1 grafts a->b while thread 2 grafts b->a.
 *
 * Step 6 (§9.5) avoids that pair by CONTRACT:
 * - A cross-trie op requires its consumed source (graft/merge src, graft_swap
 *   swap) to be EXCLUSIVE.
 * - The public entry rejects a live source with CDS_FT_STATUS_BUSY_ERROR
 *   before it takes any lock.
 * - The writer scope of an exclusive source skips the lock
 *   (ft_writer_lock_scope_enter).  So one lock is held and there is no
 *   deadlock.
 *
 * So the source is always exclusive when an op reaches the fused body.  This
 * guard is a defense-in-depth assert.  A both-live pair that reaches it means
 * a BUSY gate was missed, which is a bug.  The guard aborts rather than
 * deadlock.
 *
 * A SAME-trie merge (@a == @b) takes one lock reentrantly and is allowed, so
 * the guard fires only on distinct tries.  An op that touches an EXCLUSIVE
 * side is already single-domain: only the lock of the live side is taken.
 */
static inline
void ft_crosstrie_lock_mode_guard(const struct cds_ft *a, const struct cds_ft *b)
{
	if (caa_unlikely(a != b && !a->exclusive && !b->exclusive)) {
		fprintf(stderr,
			"cds_ft: both-live cross-trie op on lock-mode tries reached "
			"the fused body (cds_ft=%p / %p); the source must be "
			"exclusive -- a live source should have been rejected with "
			"CDS_FT_STATUS_BUSY_ERROR\n", (const void *) a, (const void *) b);
		fflush(stderr);
		abort();
	}
}

/*
 * Allocator layout (see fractal-trie-alloc.c for the full picture).
 *
 * Each 2*page_size range holds the items array (page 0), then the
 * per-range header (struct cds_ft_alloc_range) followed by the
 * per-item metadata array (page 1).  Optional bitmap array grows
 * backward from (range base + 2*page_size) on bitmap-bearing arenas.
 *
 * Two cache lines are most often prefetched from a tagged child
 * pointer.  They hold the item's metadata and (for bitmap types) its
 * bitmap.  Pure pointer arithmetic derives both from the item's order.
 * The helpers below live in this header so that the prefetch-hint
 * path can compute their addresses inline, without a cross-TU call
 * into fractal-trie-alloc.c.
 *
 * struct cds_ft_alloc_arena remains opaque here.  Only these
 * out-of-line slow paths need its fields, and they stay in
 * fractal-trie-alloc.c:
 * - cds_ft_item_to_metadata;
 * - cds_ft_item_order;
 * - cds_ft_metadata_to_item.
 */
struct cds_ft_alloc_arena;

struct cds_ft_metadata_alloc {
	struct rcu_head rcu_head;
	union {
		struct cds_ft_metadata_alloc *free_list_next;
		struct cds_ft_metadata metadata;
	};
};

struct cds_ft_alloc_range {
	struct cds_list_head node;			/* Linked list of ranges. */
	struct cds_ft_alloc_arena *arena;		/* Backward reference to arena. */
	size_t next_unused;
	/*
	 * Number of currently-live (allocated, not free-listed) items in
	 * this range.  Incremented in cds_ft_arena_alloc (both the
	 * free-list reuse and bump paths) and decremented in
	 * cds_ft_do_free_item, all under arena->lock.  A range whose
	 * nr_live reaches 0 holds no live nodes and is a candidate for
	 * reclaiming its backing pages to the OS.
	 */
	size_t nr_live;
	/*
	 * Per-range LIFO freelist of returned slots, threaded through
	 * cds_ft_metadata_alloc::free_list_next.  The freelist is per
	 * range rather than per arena, so that a drained range is
	 * reclaimed in O(1).  Its freed slots leave the allocatable set
	 * together with the range.  No shared list is walked and no
	 * entry is left dangling.  The range is linked onto
	 * arena->partial_ranges via @partial_node exactly while
	 * @free_list_head != NULL.
	 */
	struct cds_ft_metadata_alloc *free_list_head;
	struct cds_list_head partial_node;
	/*
	 * Set while this range is a private destination of an in-progress
	 * cds_ft_compact (allocated through the recompaction context, not yet
	 * merged into the arena).  The compactor descends past nodes.  It
	 * reads this flag to tell, in O(1), whether a node has already been
	 * relocated in this pass.  So it relocates each node exactly once.
	 * Cleared when the range is merged back into the arena's general
	 * pool.
	 */
	bool recompact_private;

	struct cds_ft_metadata_alloc metadata[];
};

/*
 * Some architectures have a fixed kernel page size.  For them, declare
 * the size as a compile-time constant.  Then cds_ft_get_page_size()
 * folds away, and the mask/shift arithmetic in the inline helpers below
 * collapses into immediate operands.  Runtime validation in
 * cds_ft_arena_create() rejects a mismatched kernel page size.
 */
#if defined(URCU_ARCH_X86) || defined(URCU_ARCH_S390)
# define FT_PAGE_SIZE_FIXED	4096UL
#endif

__attribute__((visibility("hidden")))
extern size_t cds_ft_page_size;

static inline size_t cds_ft_get_page_size(void)
{
#ifdef FT_PAGE_SIZE_FIXED
	return FT_PAGE_SIZE_FIXED;
#else
	/*
	 * cds_ft_page_size is lazily initialized with an atomic check-and-set
	 * (see the allocator entry points).  It never changes afterward.  So
	 * a relaxed load suffices: it cannot tear and carries no dependent
	 * data.  Routing every read through this accessor keeps all accesses
	 * to the global atomic.
	 */
	return uatomic_load(&cds_ft_page_size, CMM_RELAXED);
#endif
}

/*
 * FT_FAR_METADATA scales the layout's page unit from page_size to a 2 MiB
 * macro:
 * - items fill [base, base+2MiB);
 * - the range header + metadata[] start at base+2MiB;
 * - bitmaps grow backward from base+4MiB.
 *
 * The helpers are the default formulas with cds_ft_get_page_size() replaced by
 * FT_FAR_MACRO_SIZE.
 */
#ifdef FT_FAR_METADATA
# define FT_RANGE_PAGE_UNIT	FT_FAR_MACRO_SIZE
#else
# define FT_RANGE_PAGE_UNIT	cds_ft_get_page_size()
#endif

static inline
struct cds_ft_alloc_range *cds_ft_item_to_range(void *p)
{
	size_t pg = FT_RANGE_PAGE_UNIT;
	void *base = (void *)((unsigned long) p & ~(pg - 1));

	return (struct cds_ft_alloc_range *) ((char *) base + pg);
}

static inline
struct cds_ft_metadata *cds_ft_item_to_metadata_fast(void *p, size_t item_len_order)
{
	struct cds_ft_alloc_range *range;

	/* A node, not a parked proxy -- see cds_ft_item_to_metadata. */
	assert(((uintptr_t) p & FT_PARENT_TAG_MASK) != FT_PARENT_TAG_MASK);
	range = cds_ft_item_to_range(p);
	size_t page_offset = (unsigned long) p & (FT_RANGE_PAGE_UNIT - 1);
	size_t index = page_offset >> item_len_order;

	return &range->metadata[index].metadata;
}

/*
 * bitmap array is indexed backwards from range base + (2 * page unit).
 */
static inline
struct cds_ft_bitmap *cds_ft_item_to_bitmap(void *p, size_t item_len_order)
{
	size_t pg = FT_RANGE_PAGE_UNIT;
	void *base = (void *)((unsigned long) p & ~(pg - 1));
	size_t index = ((unsigned long) p & (pg - 1)) >> item_len_order;

	return (struct cds_ft_bitmap *) ((char *) base + (2 * pg) -
			((index + 1) * sizeof(struct cds_ft_bitmap)));
}

__attribute__((visibility("hidden")))
void cds_ft_free_all_arenas(struct cds_ft_group *ft_group);

__attribute__((visibility("hidden")))
struct cds_ft_metadata *cds_ft_item_to_metadata(void *p);

__attribute__((visibility("hidden")))
size_t cds_ft_item_order(void *p);

__attribute__((visibility("hidden")))
void *cds_ft_metadata_to_item(struct cds_ft_metadata *metadata);

__attribute__((visibility("hidden")))
struct cds_ft_metadata *cds_ft_alloc_item(struct cds_ft *ft, size_t item_len_order, bool bitmap);

/*
 * Same as cds_ft_alloc_item but routes to the per-group compressed-
 * node arena set when the group is speculative (skip-compressed).
 * Compressed-node pages are off the descent hot path in that mode.
 * Keeping them out of the internal-node arenas reduces I-cache,
 * D-cache and TLB interference for cand-mode lookups.
 */
__attribute__((visibility("hidden")))
struct cds_ft_metadata *cds_ft_alloc_compressed_item(struct cds_ft *ft, size_t item_len_order);

/*
 * Allocate one ordinal cell from the group's dedicated cell arena
 * (FT_ORD_CELL_ALLOC_ORDER, no bitmap).  Returns a metadata whose item is the
 * 32 B cell (cds_ft_metadata_to_item); freed via cds_ft_free_item.
 */
__attribute__((visibility("hidden")))
struct cds_ft_metadata *cds_ft_alloc_cell_item(struct cds_ft *ft);

__attribute__((visibility("hidden")))
void cds_ft_free_item(struct cds_ft *ft, struct cds_ft_metadata *metadata);

/*
 * Node-allocation reserve API (see struct cds_ft_alloc_reserve).  It serves a
 * bulk op whose node need is deterministic.  The op:
 * 1. pre-fills a zero-initialized reserve with the exact items it will
 *    allocate;
 * 2. activates the reserve;
 * 3. runs its commit, which draws from the reserve (no arena allocation, no
 *    failure);
 * 4. deactivates the reserve;
 * 5. drains any unused items.
 *
 * Fill is the only fallible step.  When it fails, the caller drains and
 * aborts with nothing mutated.
 *
 * Add @n items of (@kind, @item_len_order) to @r.  @bitmap is the node type's
 * bitmap flag (order uniquely determines it; ignored for COMPRESSED).  Returns
 * 0, or -ENOMEM (caller drains @r and aborts).  Must run with the reserve NOT
 * yet active (these are real arena allocations).  Asserts the running total
 * fits CDS_FT_ALLOC_RESERVE_CAP.
 */
__attribute__((visibility("hidden")))
int cds_ft_alloc_reserve_add(struct cds_ft *ft, struct cds_ft_alloc_reserve *r,
		enum cds_ft_alloc_kind kind, size_t item_len_order, bool bitmap,
		unsigned int n);

#ifndef FEATURE_FT_COMPRESS
/*
 * Add @n single-child branch nodes (CDS_FT_ALLOC_KIND_NODE, @item_len_order) to
 * @r's branch excess (see struct cds_ft_alloc_reserve).  One order per reserve;
 * same contract as cds_ft_alloc_reserve_add, asserted against
 * CDS_FT_ALLOC_RESERVE_BRANCH_CAP.
 */
__attribute__((visibility("hidden")))
int cds_ft_alloc_reserve_add_branch(struct cds_ft *ft,
		struct cds_ft_alloc_reserve *r, size_t item_len_order, bool bitmap,
		unsigned int n);
#endif

/*
 * Make @r the active reserve for @ft in THIS THREAD's reserve set.  A bulk op
 * that allocates into several tries activates the SAME @r on each.  An example
 * is the merge same-trie rekey: dst + the transient detach product.  The
 * reserve is thread-local.  So a concurrent bulk op on the same live dst owns
 * its own reserve and does not collide.  That is what makes it safe with no
 * FT-wide mutex (§11).
 * A trie may be activated at most once at a time (asserted).
 */
__attribute__((visibility("hidden")))
void cds_ft_alloc_reserve_activate(struct cds_ft *ft,
		struct cds_ft_alloc_reserve *r);

/* Remove @ft from this thread's active reserve set. */
__attribute__((visibility("hidden")))
void cds_ft_alloc_reserve_deactivate(struct cds_ft *ft);

/* True iff this thread has an active reserve covering @ft. */
__attribute__((visibility("hidden")))
bool cds_ft_alloc_reserve_covers(const struct cds_ft *ft);

/* Free every item still held in @r (the op drew fewer than it reserved). */
__attribute__((visibility("hidden")))
void cds_ft_alloc_reserve_drain(struct cds_ft *ft, struct cds_ft_alloc_reserve *r);

/*
 * cds_ft_free_item_unpublished - immediate free for items that were
 * never published (no reader can possibly hold a reference).  Bypasses
 * call_rcu and returns the slot directly to the arena free list.
 *
 * Use only for nodes that never escaped the writer's stack (i.e.,
 * speculative candidate nodes built but rejected before publication).
 * Calling this on a published node corrupts concurrent readers.
 */
__attribute__((visibility("hidden")))
void cds_ft_free_item_unpublished(struct cds_ft *ft, struct cds_ft_metadata *metadata);

/*
 * Recompaction allocation context (cds_ft_compact).
 * ft_recompact_alloc_set_active makes it active on the recompacting thread.
 * While it is active, cds_ft_arena_alloc routes that thread's allocations
 * into the per-order private fresh ranges held here.  These ranges are kept
 * off the arena's general range lists, for two reasons:
 * - the relocated nodes pack densely;
 * - concurrent unrelated allocations neither land among them nor get forced
 *   to bump.
 *
 * ft_recompact_alloc_merge splices the private ranges into their arenas.
 * @cur is indexed by the arena's item_len_order.
 */
struct ft_recompact_alloc_ctx {
	/*
	 * Per-order current private range.  There is one array for the
	 * internal-node arenas (@cur) and one for the dedicated compressed-node
	 * arena (@cur_compressed).  The two share item-length orders but are
	 * distinct arenas.
	 */
	struct cds_ft_alloc_range *cur[FT_ALLOC_ORDER_MAX + 1];
	struct cds_ft_alloc_range *cur_compressed[FT_ALLOC_ORDER_MAX + 1];
	/*
	 * Current private range for the dedicated cell arena (single: cells are
	 * uniform FT_ORD_CELL_ALLOC_ORDER).  Keeps relocated cells off the
	 * internal/compressed cursors of the same order so they pack into their
	 * own dense range.
	 */
	struct cds_ft_alloc_range *cur_cell;
	struct cds_list_head all;
};

/* Initialize a context (no ranges yet, not active). */
__attribute__((visibility("hidden")))
void ft_recompact_alloc_init(struct ft_recompact_alloc_ctx *ctx);

/*
 * Route this thread's subsequent internal-node allocations into @ctx's private
 * ranges (pass NULL to stop routing).  Called around each compaction step so
 * the caller's other mutations between steps allocate normally.
 */
__attribute__((visibility("hidden")))
void ft_recompact_alloc_set_active(struct ft_recompact_alloc_ctx *ctx);

/*
 * Splice @ctx's private ranges into their arenas' general pools and clear
 * their recompact_private flag.  Called once when compaction completes.
 */
__attribute__((visibility("hidden")))
void ft_recompact_alloc_merge(struct ft_recompact_alloc_ctx *ctx);

/* True if @metadata's item lives in a range currently flagged recompact_private. */
__attribute__((visibility("hidden")))
bool cds_ft_metadata_in_recompact_private(struct cds_ft_metadata *metadata);

/* Always-deferred (call_rcu) free, even on exclusive tries: compactor use. */
__attribute__((visibility("hidden")))
void cds_ft_free_item_deferred(struct cds_ft *ft,
		struct cds_ft_metadata *metadata);

#ifdef __linux__
#include <syscall.h>
#endif

#ifdef DEBUG
#define dbg_printf(fmt, args...)				\
	fprintf(stderr, "[debug fractal_trie %s()@%s:%u] " fmt,	\
		__func__, __FILE__, __LINE__, ## args)
#else
#define dbg_printf(fmt, args...)				\
do {								\
	/* do nothing but check printf format */		\
	if (0)							\
		fprintf(stderr, "[debug fractal_trie %s()@%s:%u] " fmt, \
			__func__, __FILE__, __LINE__, ## args);	\
} while (0)
#endif

#ifdef DEBUG_COUNTERS
static inline
int ft_debug_counters(void)
{
	return 1;
}
#else
static inline
int ft_debug_counters(void)
{
	return 0;
}
#endif

/*
 * Delay injection for race condition testing.  When enabled via
 * FT_DELAY_INJECT, inserts a usleep at critical points to widen
 * race windows between concurrent readers and writers.
 *
 * Injection sites:
 *   FT_DELAY_WRITER  - writer side (between publish and propagation)
 *   FT_DELAY_READER  - reader side (during descent/backtracking)
 *   FT_DELAY_BOTH    - both sides
 *   FT_DELAY_RANDOM  - randomly per call (50% chance each site)
 */
enum ft_delay_mode {
	FT_DELAY_NONE    = 0,
	FT_DELAY_WRITER  = (1 << 0),
	FT_DELAY_READER  = (1 << 1),
	FT_DELAY_BOTH    = FT_DELAY_WRITER | FT_DELAY_READER,
	FT_DELAY_RANDOM  = (1 << 2),
	/*
	 * The ACQUIRE SEAM.  A writer loads a pointer or a child count BEFORE
	 * it takes the node's lock, and RE-READS it after.  Decisions are made
	 * from the first sample: which node to route to, how big the copy
	 * must be.  Each of them is only as good as the second sample.
	 * Widening exactly that gap turns the rarest of these races into a
	 * reliable reproducer.  See ft_delay_seam().  This mode is separate
	 * from FT_DELAY_WRITER because a blanket writer delay perturbs the
	 * whole op and reproduces nothing in particular.
	 */
	FT_DELAY_ACQUIRE = (1 << 3),
};

/*
 * WHICH seam delays.
 *
 * ☠ ARMING THEM ALL AT ONCE SUPPRESSES THE VERY RACES IT IS MEANT TO EXPOSE.
 * Every thread passes through many acquire sites.  A delay armed everywhere
 * slows all the racers by a similar total.  The SKEW is the only thing that
 * widens a window, and it averages out.
 *
 * MEASURED on inv_concurrent_remove_all_nolist with the NODE_INDEX_NULL
 * refusal ablated:
 *   no delay                      3 SIGSEGVs / 100 runs
 *   every site armed at 2us/50%   ZERO / 100 runs
 *
 * So arm ONE site (FT_DELAY_SITES=recompact).  Treat a quieter run under
 * injection as evidence the injection is wrong, never as evidence of a fix.
 */
/*
 * ☠ FT_DELAY_SITE_ACQUIRE DOES NOT OPEN A DERIVE-BEFORE-LOCK WINDOW.  This is
 * the "averages out" rule above, measured on the class it looks made for.
 * The case is a stale-plan lost insert (the sole-entry freeze, 1d74625e).
 *
 * MEASURED:
 * - The remover is PREEMPTED between its unheld read and its first take
 *   (taskset 2 cpus + 2 spinners): the loss reproduces at ~0.4% of rows.
 * - The fix is disabled (-DFT_RED_NO_CCF_PLAN_CHECK) and this site sleeps
 *   instead (FT_DELAY_SPIN=0 FT_DELAY_US=50 FT_DELAY_PCT=10, 122M firings):
 *   0 losses in 2,000 rows where ~8 were due.
 *
 * The site sleeps the PEER at its own takes too, so the peer rarely finishes
 * an append inside the gap.  Use CPU pinning with competing spinners for that
 * class.
 *
 * Two more traps:
 * - Without FT_DELAY_SPIN=0 a site SPINS ~200 pauses and FT_DELAY_US is
 *   ignored.
 * - A sleep inside a point op stalls grace periods.  So bound the rows per
 *   process, or the call_rcu backlog outgrows a memory cage.
 */
enum ft_delay_site {
	FT_DELAY_SITE_ACQUIRE   = (1 << 0),	/* the 3 shared lock helpers */
	FT_DELAY_SITE_RECOMPACT = (1 << 1),	/* post-lock child-count re-read */
	FT_DELAY_SITE_INSERT    = (1 << 2),	/* in-place reserve's hoisted lock */
	/*
	 * POST-LOCK: after an acquire, before the re-read it protects.  It
	 * cannot widen a peer's window, because the acquire excludes the
	 * peers.  It is kept only to study an op's OWN ordering, never to
	 * reproduce a lost race.
	 */
	FT_DELAY_SITE_POSTLOCK  = (1 << 3),
	/*
	 * THE STALE-EXPECTED-OLD SEAM, for the class where an acquire does NOT
	 * exclude the writer.  A STATE-word record samples the word when it
	 * is built.  Its settle writes that sample back.  The write is blind
	 * if the record is SW.  A peer may TAKE the lock between the two.
	 * The settle then erases the peer's LOCK bit, and the peer asserts
	 * at its own release (ft_meta_lock_release: s & FT_STATE_LOCK).
	 * A POSTLOCK delay cannot widen THIS gap.  Its comment assumes the
	 * acquire excludes everyone, which is exactly the assumption under
	 * test.
	 *
	 *   STATEREC  after a state-word record is filed, before its commit.
	 *   HELD      after a lock take succeeds, before the op's own records,
	 *             so a peer's already-built record settles inside the hold.
	 */
	FT_DELAY_SITE_STATEREC  = (1 << 4),
	FT_DELAY_SITE_HELD      = (1 << 5),
	/*
	 * THE ACQUIRE LANE'S PARK.  -DFT_RED_ACQ_LANE_OFFLINE restores the
	 * pre-2026-09-23 behaviour, which parked the point op OFFLINE.
	 * - Armed WITH it, this site widens the window in which the op is
	 *   QUIESCENT mid-operation.  A peer's call_rcu-deferred frees can
	 *   then reclaim the very @node its retry re-derives from.
	 * - Armed WITHOUT it, the same delay lengthens an ONLINE park and
	 *   nothing is exposed.
	 *
	 * That is exactly the A/B.  Use the usleep form (FT_DELAY_SPIN=0
	 * FT_DELAY_US=...) with a small FT_DELAY_PCT: a grace period has to
	 * fit inside the park.
	 */
	FT_DELAY_SITE_LANE      = (1 << 6),
	FT_DELAY_SITE_ALL       = 0x7f,
	/*
	 * Between the load and the update of the key-length hint
	 * (ft_raise_max_used_key_len).  Opt-in only (FT_DELAY_SITES=hint),
	 * not in _ALL, so the existing injection runs are unchanged.
	 */
	FT_DELAY_SITE_HINT      = (1 << 7),
};

#ifdef FT_DELAY_INJECT
extern enum ft_delay_mode ft_delay_mode;
extern unsigned int ft_delay_us;
extern unsigned int ft_delay_pct;
extern unsigned int ft_delay_sites;
extern unsigned int ft_delay_spin;

static inline
void ft_delay_writer(void)
{
	if ((ft_delay_mode & FT_DELAY_WRITER) ||
	    ((ft_delay_mode & FT_DELAY_RANDOM) && (rand() & 1)))
		usleep(ft_delay_us);
}

static inline
void ft_delay_reader(void)
{
	if ((ft_delay_mode & FT_DELAY_READER) ||
	    ((ft_delay_mode & FT_DELAY_RANDOM) && (rand() & 1)))
		usleep(ft_delay_us);
}

/*
 * Per-thread xorshift.  The delay must be decided WITHOUT serializing the
 * threads it is meant to skew.  glibc rand() takes a global lock.  Using it
 * here would order the very racers whose interleaving is under test.
 * FT_DELAY_RANDOM above has that flaw.  Seeded off the TLS slot's own address.
 */
static inline
unsigned int ft_delay_rand(void)
{
	static __thread unsigned int seed;

	if (caa_unlikely(!seed))
		seed = (unsigned int) (uintptr_t) &seed ^ 0x9e3779b9u;
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

/*
 * Call between a writer's PRE-LOCK sample and the POST-LOCK re-read that is
 * supposed to supersede it.
 *
 * ☠ DELAY ONLY SOME OF THE CALLERS.  Delaying every one slows all the racers
 * equally and reproduces nothing.  The peer that has to slip INTO the gap is
 * sitting in the same usleep.  What exposes the race is SKEW: one thread
 * pauses at the seam while another runs through at full speed.  So each call
 * tosses its own coin (FT_DELAY_PCT, default 50).
 */
static unsigned long ft_delay_fired[8];
static const char *const ft_delay_site_name[8] = {
	"acquire", "recompact", "insert", "postlock", "staterec", "held",
	"lane", "hint"
};
static __attribute__((destructor)) void ft_delay_fired_report(void)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		if (uatomic_read(&ft_delay_fired[i]))
			fprintf(stderr, "FT DELAY: site %s fired %lu times\n",
				ft_delay_site_name[i],
				uatomic_read(&ft_delay_fired[i]));
}

static inline
void ft_delay_seam(enum ft_delay_site site)
{
	if (caa_likely(!(ft_delay_mode & (FT_DELAY_ACQUIRE | FT_DELAY_RANDOM))))
		return;
	if (!(ft_delay_sites & (unsigned int) site))
		return;
	if (ft_delay_pct < 100 && (ft_delay_rand() % 100u) >= ft_delay_pct)
		return;
	/*
	 * ☠ COUNT THE FIRINGS.  A quiet run under injection is evidence about
	 * the code only if the injection HAPPENED.  Without this count, a
	 * site that is never reached and a race that does not exist read
	 * identically.  This file already warns that a quieter run under
	 * injection is evidence the injection is wrong.
	 */
	uatomic_inc(&ft_delay_fired[__builtin_ctz((unsigned int) site) & 7]);
	/*
	 * ☠ A SLEEP IS THE WRONG SHAPE FOR A NANOSECOND WINDOW.  usleep() has
	 * two defects here:
	 * - It cannot resolve below the scheduler's granularity (~50us
	 *   however small the argument).
	 * - It parks the thread.  The racer was supposed to be merely
	 *   NUDGED, and it is descheduled.  The interleaving under test is
	 *   destroyed rather than widened.
	 *
	 * MEASURED with the NODE_INDEX_NULL refusal ablated:
	 *   no delay                        1-3 SIGSEGVs per 100 runs
	 *   usleep at ANY single site, 5us  ZERO
	 *
	 * So the default is a SPIN of @ft_delay_spin pause instructions.  It
	 * stays on the CPU and costs tens of nanoseconds to microseconds.  It
	 * is the only form that can widen a window of this size.  FT_DELAY_US
	 * is honoured only when explicitly asked for (FT_DELAY_SPIN=0), for
	 * the rare race whose window really is a scheduling event.
	 */
	if (caa_likely(ft_delay_spin)) {
		unsigned int i;

		for (i = 0; i < ft_delay_spin; i++)
			caa_cpu_relax();
		return;
	}
	usleep(ft_delay_us);
}
#else
static inline void ft_delay_writer(void) { }
static inline void ft_delay_reader(void) { }
static inline void ft_delay_seam(enum ft_delay_site site) { (void) site; }
#endif

#ifdef URCU_FRACTAL_TRIE_DEBUG_LOCKING
# define CDS_FT_ASSERT_RCU_READ_LOCKED(ft)                                     \
	do {                                                                   \
		if (caa_unlikely(!(ft)->group->flavor->read_ongoing())) {      \
			fprintf(stderr, "[Fatal] Fractal Trie API violation: " \
					"RCU read-side lock not held at "      \
					"%s:%d\n", __FILE__, __LINE__);        \
			abort();                                               \
		}                                                              \
	} while (0)
/*
 * The same check for the point ops whose CONTRACT puts the section on the
 * CALLER (a cached iterator's node must stay live).  The writer scope takes
 * its own section, which would satisfy read_ongoing() on the caller's behalf.
 * So the scope records the caller's state when it takes that section, and
 * this check asks the recorded state.
 */
# define CDS_FT_ASSERT_CALLER_RCU_READ_LOCKED(ft)                              \
	do {                                                                   \
		if (caa_unlikely(!(ft_wscope_rcu_held ?                        \
				ft_wscope_caller_reading :                     \
				(ft)->group->flavor->read_ongoing()))) {       \
			fprintf(stderr, "[Fatal] Fractal Trie API violation: " \
					"RCU read-side lock not held by the "  \
					"caller at %s:%d\n", __FILE__,         \
					__LINE__);                             \
			abort();                                               \
		}                                                              \
	} while (0)
#else
# define CDS_FT_ASSERT_RCU_READ_LOCKED(ft) do { } while (0)
# define CDS_FT_ASSERT_CALLER_RCU_READ_LOCKED(ft) do { } while (0)
#endif

/*
 * The CONVERSE: entries that BLOCK on a grace period must NOT be called from
 * inside an RCU read-side critical section.  The grace period would wait on
 * the caller's own section.  ft_rekey_one_decide is one such entry:
 * - Its caller holds the move gate, which waits a grace period.
 * - Its retry loop lets the escalation park take the thread OFFLINE
 *   (urcu_txn_set_park_quiescent).  That silently quiesces a caller that
 *   believes it is pinned.
 *
 * ☠ ONLY MEANINGFUL FOR FLAVORS WHOSE read_ongoing() COUNTS SECTIONS (memb,
 * mb, bp).  Under QSBR, read_ongoing() reads `urcu_qsbr_reader.ctr`, which is
 * ONLINE-ness.  It is non-zero for any registered thread, whether or not the
 * thread is in a section.  So this check would fire on every correct call.
 * Do not enable URCU_FRACTAL_TRIE_DEBUG_LOCKING on a QSBR build.
 */
#ifdef URCU_FRACTAL_TRIE_DEBUG_LOCKING
# define CDS_FT_ASSERT_RCU_NOT_READ_LOCKED(ft)                                 \
	do {                                                                   \
		if (caa_unlikely((ft)->group->flavor->read_ongoing())) {       \
			fprintf(stderr, "[Fatal] Fractal Trie API violation: " \
					"called from inside an RCU read-side " \
					"critical section at %s:%d -- this "   \
					"entry waits for a grace period\n",    \
					__FILE__, __LINE__);                   \
			abort();                                               \
		}                                                              \
	} while (0)
#else
# define CDS_FT_ASSERT_RCU_NOT_READ_LOCKED(ft) do { } while (0)
#endif

/*
 * URCU_FRACTAL_TRIE_DEBUG_PATH:
 *
 * Define this at build time to enable debug checks that detect use of
 * an invalid cached iterator path.  When enabled, three complementary
 * helpers track grace-period state inside the iterator:
 *
 *  iter_debug_path_snapshot() -- unconditionally captures a fresh
 *      grace-period poll state via the RCU flavor's
 *      update_start_poll_synchronize_rcu.  Called once at the entry of
 *      every fresh-population operation (lookup, longest-match lookup,
 *      inequality lookup slow path and early exit).  Because it always
 *      overwrites the snapshot, an iterator that is reused across
 *      distinct RCU read-side critical sections gets a current baseline.
 *
 *  iter_debug_path_check() -- polls the existing snapshot via the
 *      flavor's update_poll_state_synchronize_rcu.  Called at
 *      continuation entry points that consume a previously populated
 *      cached path (inequality lookup fast path, replace, remove).  A
 *      full grace period may have elapsed since the snapshot.  Then
 *      the RCU read-side lock must have been dropped, and the cached
 *      path is invalid.  The helper reports this and calls abort().
 *
 *  iter_debug_path_update() -- invalidates the snapshot when the path
 *      becomes invalid (node not found / end of traversal).  It never
 *      captures a new snapshot.  The one taken at the operation's entry
 *      persists as long as the path remains valid.  This gives a
 *      tighter detection window.
 *
 * The check is probabilistic in one direction:
 * - A false return from poll does not prove the lock was held
 *   continuously.  The grace period may simply not have completed yet.
 * - A true return is a definitive contract violation.
 *
 * This makes the check useful as a debugging aid without introducing
 * false positives.
 *
 * This option adds fields to struct cds_ft_iter, which is opaque to
 * applications.  Only the library needs to be rebuilt; the application
 * ABI is not affected.
 *
 * Requires liburcu >= 0.14 for the poll_state_synchronize_rcu APIs
 * and a struct rcu_flavor_struct that provides
 * update_start_poll_synchronize_rcu and
 * update_poll_state_synchronize_rcu function pointers.
 */

/*
 * Tracepoint node-kind identifiers.
 *
 * These values are exposed as an LTTng-UST enumeration in src/ft_tp.h
 * (via the LTTNG_UST_TRACEPOINT_ENUM declaration which references
 * these C enum labels).  Keeping both in the same enum guarantees the
 * C side and the trace-metadata side cannot drift out of sync.
 *
 * Labels describe the underlying structure without requiring the
 * reader to know the build's pointer width:
 *   - P2L_<bytes>: popcount_2l (2-level root_bm + sub_bm[] popcount
 *     layout; sub_bm bit width varies by tier and target pointer
 *     width); total node size in bytes.
 *   - P1L_<bytes>: popcount_1l (32-byte 256-bit bitmap +
 *     ptr-table); total node size in bytes.
 *   - PIGEON_<bytes>: 256-entry direct table; total node size in
 *     bytes (1024 on 32-bit, 2048 on 64-bit).
 *
 * Skip-compression is an orthogonal property (a skip-compressed
 * pointer can point to any underlying node type), so it is not
 * encoded in this enum.  Tracepoints that emit node_kind expose a
 * companion "skip_compressed" boolean field alongside it.
 *
 * The superset listed here covers every variant realizable by any
 * 32-bit or 64-bit build configuration; a given build may not emit
 * all of them.
 */
enum ft_tp_node_kind {
	FT_TP_NODE_NULL			=  0,
	FT_TP_NODE_EXTERNAL		=  1,
	FT_TP_NODE_COMPRESSED		=  2,
	FT_TP_NODE_P2L_32		=  5,
	FT_TP_NODE_P2L_64		=  6,
	FT_TP_NODE_P2L_128		=  7,
	FT_TP_NODE_P1L_128		=  4,
	FT_TP_NODE_P1L_256		=  8,
	FT_TP_NODE_P1L_512		=  9,
	FT_TP_NODE_P1L_1024		= 10,
	FT_TP_NODE_PIGEON_1024		= 11,
	FT_TP_NODE_PIGEON_2048		= 12,
	FT_TP_NODE_UNKNOWN		= 13,
};

/*
 * Central node-type definitions and small helpers moved here from the
 * fractal-trie.c top matter so the master file is just the assembly unit.
 * (The ft_types[] data table itself stays a single-TU definition in
 * fractal-trie/ft-tables.h -- it cannot live in this multi-TU header.)
 */
#ifndef abs_int
#define abs_int(a)	((int) (a) > 0 ? (int) (a) : -((int) (a)))
#endif

struct cds_ft_group_attr {
	size_t key_len;		/* Fixed key length in bytes, or CDS_FT_LEN_VARIABLE. */
	size_t max_key_len;	/* Maximum key length allowed (bounds key buffers). */
	struct cds_ft_key_map key_map;	/* Per-position key<->ordinal byte remap; see cds_ft_group_attr_set_key_map. */
	unsigned int flags;	/* CDS_FT_FLAG_* creation-time flags. */
	bool speculative;	/* See cds_ft_lookup_optimization. */
	/*
	 * Byte offset from the (struct cds_ft_node *) stored in the trie to
	 * the start of the caller-stored key bytes, and whether it was
	 * configured.  Lets the speculative inequality path copy a result
	 * key from the leaf instead of rebuilding it from compressed-node
	 * bytes.  See cds_ft_group_attr_set_speculative_key_offset.
	 */
	size_t speculative_key_offset;
	bool speculative_key_offset_set;
	/*
	 * Offset from the (struct cds_ft_node *) to a size_t holding the leaf's
	 * key length in the caller's leaf, and whether configured.  Lets the
	 * ordered-list fast path materialize a variable-length result key from
	 * the leaf without the descent's per-level length computation.  See
	 * cds_ft_group_attr_set_key_len_offset.
	 */
	size_t key_len_offset;
	bool key_len_offset_set;
	/*
	 * Enable the library-owned ordered sibling list (FEATURE_FT_ORD_CELL):
	 * order links live in library-owned ordinal cells, not the app leaf.
	 * See cds_ft_group_attr_set_ordered_list.
	 */
	bool ordered_list_set;
	/*
	 * Opt this group's tries into the in-trie MOVE ops (rekey).  DEFAULT
	 * FALSE -- cds_ft_rekey_graft / cds_ft_rekey_merge answer
	 * CDS_FT_STATUS_NOT_SUPPORTED unless it is set.  See
	 * cds_ft_group_attr_set_rekey.
	 */
	bool rekey_set;
	/*
	 * Maintain the per-node order-statistics key counts (nr_keys) that back
	 * cds_ft_count_keys / _prefix, cds_ft_lookup_nth / _last, and
	 * cds_ft_iter_skip_forward / _reverse.  Default OFF.  When disabled,
	 * the library keeps no per-node count.  Those queries then fall back
	 * to enumeration (count) or to first/last + next/prev iteration
	 * (select / skip).  See cds_ft_group_attr_set_rank_stats.
	 */
	bool rank_stats_set;
	/*
	 * Distinguishes "the caller never chose a strategy" from "the caller
	 * explicitly chose a strategy", which calloc-zero alone cannot: unset
	 * resolves to the DLM default.
	 */
	bool writer_strategy_set;
	enum cds_ft_numa_policy numa_policy;	/* See cds_ft_group_attr_set_numa_policy. */
	enum cds_ft_optimize optimize;		/* See cds_ft_group_attr_set_optimize. */
	/*
	 * Structural-writer concurrency strategy (MW lock-escalation model).
	 * Meaningful only when @writer_strategy_set; otherwise the group takes
	 * the CDS_FT_WRITER_LOCK_FINE default.  See
	 * cds_ft_group_attr_set_writer_strategy.
	 */
	enum cds_ft_writer_strategy writer_strategy;
};

struct cds_ft_attr {
	bool exclusive;		/* Exclusive (single-writer, no concurrent readers) vs concurrent; see cds_ft_attr_set_exclusive. */
	bool speculative_keys_disabled;	/* This trie ignores the group's speculative_key_offset (EAGER lookups); see cds_ft_attr_set_speculative_keys.  calloc default false = inherit the group. */
};

enum cds_ft_type_class {
	FT_POPCOUNT = 0,	/* Popcount-bitmap: popcount_1l / popcount_2l */
	FT_PIGEON = 1,		/* Pigeon: direct indexed */
	/* Leaf nodes are implicit from their height in the trie */
	FT_NR_TYPES = 2,

	FT_NULL,	/* not an encoded type, but keeps code regular */
};

#define ft_type_is_popcount(tc)	((tc) == FT_POPCOUNT)
#define ft_type_is_pigeon(tc)	((tc) == FT_PIGEON)

struct cds_ft_type {
	enum cds_ft_type_class type_class;
	uint16_t min_child;		/* minimum number of children: 1 to 256 */
	uint16_t max_child;		/* maximum number of children: 1 to 256 */
	uint16_t order;			/* node size is (1 << order), in bytes */
	bool bitmap;			/* allocate bitmap */
	bool popcount_2l;		/* 2-level popcount-bitmap layout */
	bool popcount_1l;		/* 1-level byte popcount layout */
};

/*
 * The cds_ft_inode contains the compressed node data needed for
 * the read-side traversal. Because the actual layout depends on the
 * node's type_class (Popcount or Pigeon), the struct uses a single
 * pointer-aligned byte array. The allocator sizes this array dynamically
 * based on the type's order (1 << type->order).
 *
 * The allocator guarantees that each node is naturally aligned
 * to its exact size boundary. For example, a node with order 6 (64 bytes)
 * is guaranteed to be aligned on a 64-byte boundary in memory. This ensures
 * optimal cache-line alignment, prevents false sharing, and guarantees that
 * a node never straddles a memory page boundary. This ensures that accessing
 * a single node hits at most one TLB entry, minimizing read-side latency.
 *
 * Memory Layouts by Type Class:
 *
 * 1. FT_POPCOUNT (two variants, popcount_2l and popcount_1l):
 * - popcount_2l: a fixed-size header packs a root_bm and the per-chunk
 *   sub_bm.  The header is 8 B, 12 B or 16 B, depending on the
 *   scan_<rootbits>_<subbits> variant.  An array of child pointers
 *   indexed by bitmap rank follows it.
 * - popcount_1l: 32 B flat 256-bit bitmap header, followed by an
 *   array of child pointers indexed by bitmap rank.
 *
 * 2. FT_PIGEON:
 * - A direct, flat array of up to 256 (struct cds_ft_inode_flag *) pointers.
 * - No key array is stored inside the node -- the key is implicit
 *   from the pointer's array index.
 * - Because 'data' is explicitly pointer-aligned, it can be safely cast
 *   directly to (struct cds_ft_inode_flag **).
 *
 * Note: For all configurations, the true total number of children is
 * strictly maintained in the out-of-line metadata (struct cds_ft_metadata).
 *
 * Note on array sizing: the array has the maximum possible node size
 * (Pigeon: 256 pointers).  C99 does not allow a Flexible Array Member
 * as the first field of a structure.
 */

struct cds_ft_inode {
	uint8_t data[FT_ENTRY_PER_NODE * sizeof(struct cds_ft_inode_flag *)]
		__attribute__((__aligned__(sizeof(struct cds_ft_inode_flag *))));
};

#ifdef FT_DEBUG_TOMBSTONE_AUDIT
extern unsigned long ft_unpub_free_calls;
#endif

#endif /* _URCU_FT_INTERNAL_H */
