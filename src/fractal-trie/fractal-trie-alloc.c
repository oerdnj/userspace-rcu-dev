// SPDX-FileCopyrightText: 2025-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * RCU Fractal Trie allocator for internal nodes.
 *
 * This allocator maximizes cache and TLB locality for the read-side fast
 * paths (lookups and traversals).  It separates item data from metadata:
 *
 *   - They sit on different cache lines.
 *   - They sit in different pages.  Each range places the item array and
 *     the metadata array in separate pages (see the layout below).
 *
 * A lookup or ordered traversal touches only the densely-packed item
 * region.  It never touches the metadata or bitmap regions.  This has two
 * consequences:
 *
 *   - The metadata pages stay out of the DTLB working set of the reader.
 *   - Resident memory (RSS) overstates the cache-hot working set.  The
 *     Fractal Trie keeps a denser cache-hot set than other trie
 *     implementations, even though its RSS is higher.
 *
 * The allocator achieves this with a strided allocation.
 *
 * Layout of allocation arena ranges for read-side items of size
 * item_len (power of 2):
 *
 *   nr_items = cds_ft_page_size / item_len.
 *
 *   Offset                 Content
 *
 *   0:                     array of nr_items elements of item_len each
 *   cds_ft_page_size:             struct cds_ft_alloc_range
 *   cds_ft_page_size + sizeof(struct cds_ft_alloc_range):
 *                          array of nr_items struct cds_ft_metadata_alloc
 *   2 * cds_ft_page_size - nr_items * sizeof(struct cds_ft_bitmap):
 *                          reverse array of nr_items struct cds_ft_bitmap (only for bitmap-bearing arenas)
 *
 * An allocation arena contains a linked list of allocation ranges.
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <urcu/fractal-trie.h>
#include <urcu/list.h>
#include <urcu/uatomic.h>
#include "fractal-trie-internal.h"

#ifdef FT_DEBUG_REMOVE_RETRY_CAP
__thread uint64_t ft_dbg_arena_ns;
__thread unsigned int ft_dbg_arena_waits;

static void ft_dbg_arena_mutex_lock(pthread_mutex_t *m)
{
	uint64_t t0 = ft_dbg_gp_clock();

	pthread_mutex_lock(m);
	ft_dbg_arena_ns += ft_dbg_gp_clock() - t0;
	ft_dbg_arena_waits++;
}
# define pthread_mutex_lock(m) ft_dbg_arena_mutex_lock(m)
#endif
#include "fractal-trie-trace.h"
#include "urcu-utils.h"

/*
 * Superblock size for the bump-allocator backing range_create.
 *
 * Each arena owns a list of superblocks.  Ranges are carved out of the
 * head superblock with a bump pointer.  A new superblock is mmap'd when
 * the head fills up.  The size is large enough that VMA fragmentation is
 * negligible.  It is small enough to avoid wasting address space on tiny
 * tries.
 *
 * The mmap is MAP_NORESERVE (lazily faulted).  So a superblock costs its
 * full size in VIRTUAL address space but almost no physical memory.
 *
 * On 32-bit, a process has only ~3 GiB of address space.  A bulk op's
 * node reserve eagerly creates one arena per node order and kind, hence
 * one superblock each.  A dozen 64 MiB superblocks would exhaust the
 * address space.  So keep the 32-bit superblock small.  The mmap calls
 * are slightly more frequent.  That does not matter next to staying
 * inside the address space.
 */
#ifndef FT_SUPERBLOCK_SIZE
# if (CAA_BITS_PER_LONG < 64)
#  define FT_SUPERBLOCK_SIZE (2UL * 1024 * 1024)
# else
#  define FT_SUPERBLOCK_SIZE (64UL * 1024 * 1024)
# endif
#endif

struct cds_ft_alloc_arena;

struct cds_ft_alloc_superblock {
	void *base;
	size_t size;
	size_t used;
	struct cds_list_head node;
};

__attribute__((visibility("hidden")))
size_t cds_ft_page_size;

/*
 * struct cds_ft_metadata_alloc and struct cds_ft_alloc_range are defined
 * in fractal-trie-internal.h so that the hot-path helpers
 * (cds_ft_item_to_range, cds_ft_item_to_metadata_fast,
 * cds_ft_item_to_bitmap) can inline into fractal-trie.c.  The arena
 * struct stays opaque to readers and is defined here.
 */
struct cds_ft_alloc_arena {
	struct cds_ft_group *ft_group;
	struct cds_list_head ranges;			/* List head of struct cds_ft_alloc_range. */
	struct cds_list_head superblocks;		/* List head of struct cds_ft_alloc_superblock. */
	size_t item_len_order;
	size_t max_nr_items_per_range;
	/*
	 * Ranges that currently hold reusable freed slots, in MRU order.
	 * These are the ranges whose per-range free_list_head != NULL.
	 *
	 * cds_ft_do_free_item moves a range to the head when a slot is
	 * freed into it.  So the next allocation reuses the hottest
	 * just-freed slot.  This recovers the locality that the former
	 * single per-arena freelist provided.
	 *
	 * Freelists are kept per range so that a drained range can be
	 * reclaimed in O(1), without a walk of a shared list.
	 */
	struct cds_list_head partial_ranges;
	/*
	 * Reclaimed ranges available for reuse.
	 *
	 * When a range fills then fully drains, its node-body region is
	 * released with MADV_DONTNEED (see ft_arena_reclaim_range).  The
	 * range is then parked here.  range_create recycles it before it
	 * allocates a fresh one.  This avoids a new mmap, and avoids
	 * re-applying the NUMA/THP policy.
	 *
	 * Both layouts use this list.  A far range could instead be
	 * munmap'd to also return its VA.  Recycling avoids the
	 * munmap+mmap+re-policy churn on range turnover.
	 */
	struct cds_list_head free_ranges;
	pthread_mutex_t lock;
	char *name;
	bool bitmap;
	bool compressed;	/* Dedicated compressed-node arena (speculative groups). */
	bool cell;		/* Dedicated ordinal-cell arena (ordered_list groups). */
};

static
void *cds_ft_range_get_nth_item(struct cds_ft_alloc_range *range, size_t n)
{
	/*
	 * Items occupy the page unit just before the range header:
	 *
	 *   - default layout: page_size;
	 *   - FT_FAR_METADATA: the leading 2 MiB of the macro
	 *     (range == macro base + 2 MiB).
	 */
	return (((char *) range) - FT_RANGE_PAGE_UNIT) + (n << range->arena->item_len_order);
}

static
struct cds_ft_alloc_range *cds_ft_metadata_to_range(struct cds_ft_metadata *metadata)
{
	struct cds_ft_metadata_alloc *metadata_alloc =
		caa_container_of(metadata, struct cds_ft_metadata_alloc, metadata);
	size_t index = metadata->alloc_index;
	struct cds_ft_alloc_range *range;

	range = (struct cds_ft_alloc_range *)((char *)(metadata_alloc - index) - sizeof(struct cds_ft_alloc_range));
	assert(index < range->arena->max_nr_items_per_range);
	return range;
}

struct cds_ft_metadata *cds_ft_item_to_metadata(void *p)
{
	struct cds_ft_alloc_range *range;

	/*
	 * @p must be a NODE, not a parked transaction proxy.
	 *
	 * This is the choke point for "treat this word as an arena item".
	 * The very next line dereferences the range header derived from @p.
	 *
	 * The mistake this assert catches: a caller reads a transacted slot
	 * RAW and hands the parked proxy straight here.  The proxy is a
	 * descriptor-record POINTER that carries the whole in-band tag.
	 * Without the assert, that caller faults on foreign memory, and
	 * nothing in the backtrace names the raw read.
	 *
	 * An arena item is (1 << item_len_order)-aligned and can never carry
	 * the whole tag.  So the assert only ever fires on that mistake.
	 * Resolve the slot (ft_resolve_flip_proxy / urcu_txn_load) before
	 * naming a node.
	 */
	assert(((uintptr_t) p & FT_PARENT_TAG_MASK) != FT_PARENT_TAG_MASK);
	range = cds_ft_item_to_range(p);
	return cds_ft_item_to_metadata_fast(p, range->arena->item_len_order);
}

size_t cds_ft_item_order(void *p)
{
	struct cds_ft_alloc_range *range = cds_ft_item_to_range(p);
	return range->arena->item_len_order;
}

void *cds_ft_metadata_to_item(struct cds_ft_metadata *metadata)
{
	size_t index = metadata->alloc_index;
	struct cds_ft_alloc_range *range = cds_ft_metadata_to_range(metadata);

	return cds_ft_range_get_nth_item(range, index);
}

static
size_t cds_ft_arena_range_alloc_size(size_t item_len_order, bool bitmap)
{
	size_t pg = FT_RANGE_PAGE_UNIT;

	if (bitmap)
		return 2 * pg;
	else
		return pg + sizeof(struct cds_ft_alloc_range) +
			(pg >> item_len_order) * sizeof(struct cds_ft_metadata_alloc);
}

/*
 * mbind() with MPOL_INTERLEAVE round-robins page placement across
 * the calling thread's allowed NUMA nodes, applied at superblock
 * creation while no page is faulted yet.
 *
 * It is enabled by default.  The fractal trie targets workloads where
 * many concurrent readers traverse shared data.  There, spreading the
 * arena across NUMA nodes wins by a wide margin over first-touch, which
 * concentrates it on a single node.
 *
 * Set CDS_FT_NUMA_INTERLEAVE=0 to opt out and fall back to first-touch
 * placement.  This is useful for workloads with thread-local working
 * sets, and for benchmarking against the no-policy baseline.
 *
 * Transparent hugepages are a separate, orthogonal concern.
 * ft_apply_thp_policy() handles them per arena, from the cds_ft_optimize
 * attribute.  The 2 MiB-granular interleave below is about NUMA placement
 * only.  The page size does not affect it.
 */
#ifdef __linux__
/*
 * mempolicy mode numbers -- match the linux/mempolicy.h enum.  Re-declared
 * locally to avoid pulling in the libnuma headers.
 */
#define FT_MPOL_DEFAULT		0
#define FT_MPOL_PREFERRED	1
#define FT_MPOL_BIND		2
#define FT_MPOL_INTERLEAVE	3
#define FT_MPOL_LOCAL		4	/* Linux 3.8+ */
#define FT_MPOL_F_MEMS_ALLOWED	(1U << 2)
#define FT_MAX_NUMA_NODES	1024
#define FT_NODEMASK_LONGS	(FT_MAX_NUMA_NODES / (sizeof(unsigned long) * 8))
#define FT_NODEMASK_BITS_PER_LONG	(sizeof(unsigned long) * 8)
/*
 * FT_NODEMASK_LONGS divides, so FT_MAX_NUMA_NODES must be a whole number of
 * words.  Otherwise the nodemask[] array rounds DOWN.  The scan in
 * ft_apply_interleave is bounded by FT_MAX_NUMA_NODES and would read past
 * the array.
 *
 * A machine with more NUMA nodes than this ceiling makes get_mempolicy()
 * return EINVAL.  The caller already treats that as "skip the interleave".
 */
urcu_static_assert(FT_MAX_NUMA_NODES % (sizeof(unsigned long) * 8) == 0,
		"FT_MAX_NUMA_NODES must be a whole number of words",
		ft_nodemask_whole_words);
#define FT_HUGEPAGE_SIZE	(2UL * 1024 * 1024)	/* 2 MiB */

/*
 * MADV_HUGEPAGE / MADV_NOHUGEPAGE (asm-generic/mman-common.h values 14/15).
 *
 * glibc only exposes them under _GNU_SOURCE.  So define them locally, like
 * the FT_MPOL_* numbers above.  This guarantees that ft_apply_thp_policy()
 * can advise THP per arena, whatever feature-test macros this translation
 * unit was built with.
 *
 * Without these the madvise() silently compiles out.  The per-arena policy
 * is then NOT applied on a kernel with transparent_hugepage=always.
 */
#ifndef MADV_NOHUGEPAGE
#define MADV_NOHUGEPAGE		15
#endif
#ifndef MADV_HUGEPAGE
#define MADV_HUGEPAGE		14
#endif
#ifndef MADV_DONTNEED
#define MADV_DONTNEED		4
#endif

/*
 * Whether MADV_DONTNEED on private anonymous memory zero-fills on refault.
 *
 * Linux guarantees it.  madvise(2) says that the range reads as
 * zero-fill-on-demand after MADV_DONTNEED.  The allocator relies on it:
 *
 *   - A reclaimed range's node-body region is released with MADV_DONTNEED.
 *   - The bump-allocation paths later hand it back out WITHOUT an explicit
 *     memset.  They trust the refault to read zero.  A fresh node must
 *     start zeroed.
 *
 * Some platforms (e.g. FreeBSD) do NOT zero on refault after
 * MADV_DONTNEED.  They keep the previous life's bytes instead.  There the
 * recycled body must be cleared on reuse.  ft_clear_recycled_slot does
 * that when this is 0.
 *
 * NOTE: the non-Linux path is provided for correctness.  But FT also
 * depends on Linux-only NUMA syscalls.  So non-Linux support as a whole is
 * incomplete and UNTESTED.
 */
#ifdef __linux__
#define FT_MADV_DONTNEED_ZEROES	1
#else
#define FT_MADV_DONTNEED_ZEROES	0
#endif

/*
 * MAP_NORESERVE: the arena maps are multi-MiB but fault in lazily and stay
 * mostly untouched, so do not reserve swap/commit for them up front.  Under
 * strict overcommit (vm.overcommit_memory=2) reserving the whole range would
 * otherwise fail the mmap even though almost none of it ever becomes resident.
 * Defined to 0 where the platform lacks the flag (then it is a plain map).
 */
#ifndef MAP_NORESERVE
#define MAP_NORESERVE		0
#endif

/*
 * Return 1 if the env var CDS_FT_NUMA_INTERLEAVE=0 is set, 0 otherwise.
 *
 * The variable is a debug override.  It tells the library to skip ALL
 * mbind() calls regardless of group policy.  Placement is then left
 * entirely to whatever the kernel / process policy decides.
 */
static
int ft_interleave_env_forces_skip(void)
{
	static int cached = -1;
	const char *env;
	int v;

	v = uatomic_load(&cached, CMM_RELAXED);
	if (v != -1)
		return v;
	env = getenv("CDS_FT_NUMA_INTERLEAVE");
	v = (env && env[0] == '0') ? 1 : 0;
	uatomic_store(&cached, v, CMM_RELAXED);
	return v;
}

/*
 * Per-arena Transparent Huge Page (THP) policy for FT allocator mappings.
 *
 * Page size does NOT affect cache-line selection on this hardware:
 *
 *   - The L1 is virtually indexed from the page offset.
 *   - The L2/L3 (and the L3 slice hash) are physically indexed.
 *   - A 2 MiB and a 4 KiB mapping over the *same physical frames* give
 *     identical cache behaviour.  A same-frames split-the-PMD experiment
 *     verified this.
 *
 * So THP is purely a TLB tradeoff.  The caller selects it per arena with
 * @huge.  @huge comes from the cds_ft_optimize attribute of the group or
 * of the external arena.  The default is CDS_FT_OPTIMIZE_THROUGHPUT, which
 * means huge.
 *
 *   huge (THROUGHPUT) -> MADV_HUGEPAGE.  2 MiB pages cut DTLB misses ~1000x;
 *     on the internal node arena (structured descent, reused hot top) this is
 *     measured +7% T1 / +14% T192.  On the external (leaf) arena it is
 *     throughput-neutral but banks DTLB headroom for large tries.  Costs a
 *     ~2 MiB RSS floor per range (a hugepage faults whole on first touch).
 *
 *   !huge (RSS) -> MADV_NOHUGEPAGE.  4 KiB pages, no hugepage floor: a small /
 *     sparse trie faults only what it touches.  Right for many small tries.
 *
 * This is advice only, with no eager populate.  So worker first-touch still
 * places pages on the local NUMA node.  The 2 MiB-granular mbind interleave
 * (ft_apply_interleave) is orthogonal and unaffected.
 */
static
void ft_apply_thp_policy(void *base, size_t size, int huge)
{
	if (huge) {
#ifdef MADV_HUGEPAGE
		(void) madvise(base, size, MADV_HUGEPAGE);
#endif
	} else {
#ifdef MADV_NOHUGEPAGE
		(void) madvise(base, size, MADV_NOHUGEPAGE);
#endif
	}
}

/*
 * Query the calling thread's process-default NUMA policy mode (set by
 * numactl, set_mempolicy(), or libnuma).  Returns one of FT_MPOL_*.
 * Returns FT_MPOL_DEFAULT on query failure (treat as "no policy set").
 */
static
int ft_query_process_mempolicy_mode(void)
{
	int mode = FT_MPOL_DEFAULT;
	long r;

	r = syscall(__NR_get_mempolicy, &mode, NULL, 0UL, NULL, 0UL);
	if (r < 0)
		return FT_MPOL_DEFAULT;
	return mode;
}

/*
 * The process-wide position in the per-2 MiB-chunk node rotation.
 *
 * The rotation used to be per REGION.  It restarted at the first allowed
 * node every time, so every region's first chunk was hard-bound to that
 * one node.  Under a strict MPOL_BIND, a process whose arenas are mostly
 * small piled onto that node.  Arenas are mostly small with many groups,
 * or with node orders that never grow past their first chunk.
 *
 * Measured 2026-09-26 on a 24-node box, with 24 processes creating groups
 * in a loop:
 *
 *   - Node 0 filled up (30 GB) while ~700 GB stayed free on the other
 *     nodes.
 *   - The kernel's policy-constrained OOM killer took processes
 *     machine-wide (a VM included).
 *
 * Each region now reserves a contiguous slice of one shared counter.  So
 * the chunks keep rotating across regions, groups and threads.
 */
static unsigned long ft_interleave_next_chunk;

/*
 * Apply the configured NUMA placement policy to the @size bytes at
 * @base.  There are three policies.  The CDS_FT_NUMA_INTERLEAVE=0 env
 * var forces a skip of all three.
 *
 *   - CDS_FT_NUMA_INTERLEAVE: per-2 MiB-chunk mbind(MPOL_BIND) round-
 *     robin across the calling thread's allowed nodes.  Binding whole
 *     2 MiB spans keeps contiguous virtual ranges node-local, unlike
 *     the kernel's 4 KiB-fine interleave.  The hardware prefetcher and
 *     NUMA locality favour this.  This coarse granularity is the
 *     measured win.  (The page size is a separate choice, see
 *     ft_apply_thp_policy.)  The rotation is PROCESS-WIDE
 *     (ft_interleave_next_chunk): each region takes the next slice of
 *     it, so region after region continues across the nodes.
 *     The policy falls back to whole-region MPOL_INTERLEAVE at native
 *     page granularity in three cases:
 *       - @base isn't 2 MiB-aligned;
 *       - the region is smaller than 2 MiB;
 *       - only one node is allowed.
 *
 *   - CDS_FT_NUMA_LOCAL: single mbind(MPOL_LOCAL) over the whole
 *     region.  Pages allocated within @base land on the local node
 *     of whichever thread first faults each page.  Persists even if
 *     the process policy is set to something else (e.g., interleave-
 *     all under a numactl wrapper).
 *
 *   - CDS_FT_NUMA_DEFAULT: query the process / libnuma policy via
 *     get_mempolicy().  If the process policy is DEFAULT or INTERLEAVE,
 *     treat this group as INTERLEAVE (apply 2 MiB-chunk MPOL_BIND).
 *     For a process INTERLEAVE policy, this gives the kernel's intent a
 *     THP-friendly placement.  For any other process policy
 *     (PREFERRED/BIND/LOCAL), skip mbind and let the kernel honor the
 *     process policy directly.
 */
static
void ft_apply_interleave(void *base, size_t size,
		enum cds_ft_numa_policy policy)
{
	unsigned long nodemask[FT_NODEMASK_LONGS] = { 0 };
	int allowed_nodes[FT_MAX_NUMA_NODES];
	unsigned long any = 0;
	int nr_allowed = 0;
	size_t i, off;
	long r;

	if (ft_interleave_env_forces_skip())
		return;

	if (policy == CDS_FT_NUMA_DEFAULT) {
		/*
		 * Default policy: interleave the FT arenas across the allowed
		 * nodes, UNLESS the caller has expressed an explicit
		 * per-process NUMA preference.  Then honor it: never override
		 * a stated intent.
		 *
		 * Rationale: FT is a shared, read-mostly, random-access
		 * structure.  First-touch placement piles it onto the
		 * builder's node.  Then:
		 *
		 *   - a single memory controller bottlenecks every other
		 *     node's readers;
		 *   - auto-NUMA-balancing thrashes the unbound pages.
		 *
		 * Interleave (an explicit mbind) spreads the bandwidth and
		 * exempts the pages from balancing.
		 *
		 *   MPOL_DEFAULT   (no preference)        -> interleave (good default)
		 *   MPOL_INTERLEAVE(process asked for it) -> interleave, upgraded to
		 *                                            our THP-friendly 2 MiB
		 *                                            granularity
		 *   MPOL_BIND / PREFERRED / LOCAL         -> honor it, leave the region
		 *                                            to the process policy
		 *
		 * The interleave below uses the MEMS_ALLOWED node set, so a cpuset
		 * that restricts nodes is still respected.  Opt out with env
		 * CDS_FT_NUMA_INTERLEAVE=0 or an explicit CDS_FT_NUMA_LOCAL group
		 * policy.
		 */
		int mode = ft_query_process_mempolicy_mode();

		if (mode != FT_MPOL_DEFAULT && mode != FT_MPOL_INTERLEAVE)
			return;
		policy = CDS_FT_NUMA_INTERLEAVE;
	}

	if (policy == CDS_FT_NUMA_LOCAL) {
		/*
		 * MPOL_LOCAL (Linux 3.8+).  Single mbind over the whole
		 * region.  Pages fault local to the thread that touches
		 * them first; if THP is enabled, hugepages allocate
		 * locally too.  No nodemask needed.
		 */
		(void) syscall(__NR_mbind, base, size, FT_MPOL_LOCAL,
				NULL, 0UL, 0);
		return;
	}

	/* CDS_FT_NUMA_INTERLEAVE -- per-2 MiB-chunk MPOL_BIND round-robin. */
	r = syscall(__NR_get_mempolicy, NULL, nodemask, (unsigned long) FT_MAX_NUMA_NODES,
			NULL, FT_MPOL_F_MEMS_ALLOWED);
	if (r < 0)
		return;
	for (i = 0; i < FT_NODEMASK_LONGS; i++)
		any |= nodemask[i];
	if (!any)
		return;
	/* Enumerate allowed node IDs. */
	for (i = 0; i < FT_MAX_NUMA_NODES; i++) {
		size_t idx = i / FT_NODEMASK_BITS_PER_LONG;
		size_t bit = i % FT_NODEMASK_BITS_PER_LONG;
		if (nodemask[idx] & (1UL << bit))
			allowed_nodes[nr_allowed++] = (int) i;
	}
	/*
	 * Per-2MB-chunk MPOL_BIND fast path.  It needs:
	 *
	 *   - 2 MiB alignment;
	 *   - at least one hugepage of room;
	 *   - multiple allowed nodes, for round-robin to do anything useful.
	 */
	if (nr_allowed > 1 &&
	    ((uintptr_t) base & (FT_HUGEPAGE_SIZE - 1)) == 0 &&
	    size >= FT_HUGEPAGE_SIZE) {
		unsigned long nr_chunks = size / FT_HUGEPAGE_SIZE;
		unsigned long chunk = uatomic_add_return(
			&ft_interleave_next_chunk, nr_chunks) - nr_chunks;

		for (off = 0; off + FT_HUGEPAGE_SIZE <= size;
				off += FT_HUGEPAGE_SIZE, chunk++) {
			unsigned long single[FT_NODEMASK_LONGS] = { 0 };
			int node = allowed_nodes[chunk % (unsigned long) nr_allowed];
			single[node / FT_NODEMASK_BITS_PER_LONG] =
				1UL << (node % FT_NODEMASK_BITS_PER_LONG);
			(void) syscall(__NR_mbind,
				(char *) base + off, FT_HUGEPAGE_SIZE,
				FT_MPOL_BIND, single,
				(unsigned long) FT_MAX_NUMA_NODES, 0);
		}
		/*
		 * Trailing sub-2MB region: spread it across all allowed
		 * nodes at native-page granularity.
		 */
		if (off < size) {
			(void) syscall(__NR_mbind,
				(char *) base + off, size - off,
				FT_MPOL_INTERLEAVE, nodemask,
				(unsigned long) FT_MAX_NUMA_NODES, 0);
		}
		return;
	}
	/* Fallback: whole region INTERLEAVE at native page granularity. */
	(void) syscall(__NR_mbind, base, size, FT_MPOL_INTERLEAVE,
			nodemask, (unsigned long) FT_MAX_NUMA_NODES, 0);
}
#else
static inline void ft_apply_interleave(void *base __attribute__((unused)),
		size_t size __attribute__((unused)),
		enum cds_ft_numa_policy policy __attribute__((unused))) {}
static inline void ft_apply_thp_policy(void *base __attribute__((unused)),
		size_t size __attribute__((unused)),
		int huge __attribute__((unused))) {}
#endif

/*
 * Allocate a fresh superblock big enough to host at least one
 * range of size min_size.  Pages are mmap'd anonymous, so they are
 * lazily zero-initialized on first touch.
 *
 * NUMA placement: if @numa_policy is INTERLEAVE/DEFAULT (and not
 * overridden by CDS_FT_NUMA_INTERLEAVE=0), apply per-2 MiB-chunk
 * mbind(MPOL_BIND) round-robin across the calling thread's allowed
 * NUMA nodes.
 *
 * The 2 MiB granularity keeps each contiguous 2 MiB virtual span on a
 * single node, unlike the kernel's default 4 KiB-fine interleave.  The
 * hardware prefetcher and NUMA locality both favour this.  This coarse
 * interleave is the real measured win.  It is independent of page size.
 *
 * Transparent hugepages are advised ON (MADV_HUGEPAGE) or OFF
 * (MADV_NOHUGEPAGE) according to @huge.  See ft_apply_thp_policy() for
 * the per-arena rationale.
 */
static __attribute__((unused))
struct cds_ft_alloc_superblock *superblock_create(size_t min_size,
		enum cds_ft_numa_policy numa_policy, int huge)
{
	struct cds_ft_alloc_superblock *sb;
	size_t size;
	void *base;

	size = FT_SUPERBLOCK_SIZE > min_size ? FT_SUPERBLOCK_SIZE : min_size;
	/* Round up to page boundary. */
	size = (size + cds_ft_get_page_size() - 1) & ~(cds_ft_get_page_size() - 1);
	base = mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED)
		return NULL;
	ft_apply_interleave(base, size, numa_policy);
	ft_apply_thp_policy(base, size, huge);
	sb = malloc(sizeof(*sb));
	if (!sb) {
		munmap(base, size);
		return NULL;
	}
	sb->base = base;
	sb->size = size;
	sb->used = 0;
	return sb;
}

static
void superblock_destroy(struct cds_ft_alloc_superblock *sb)
{
	cds_list_del(&sb->node);
	munmap(sb->base, sb->size);
	free(sb);
}

/*
 * Recycle a previously reclaimed range (both layouts), or return NULL if none
 * is parked on the arena's free list.
 *
 * A recycled range's node-body region was MADV_DONTNEED'd.  It re-faults
 * zero on use.  Its header + metadata page stays mapped and retains the
 * range's NUMA / THP policy.  So reusing it avoids a fresh mmap +
 * re-policy.  Reset the per-range bookkeeping before returning.
 *
 * Caller must hold arena->lock.
 */
static
struct cds_ft_alloc_range *range_recycle(struct cds_ft_alloc_arena *arena)
{
	struct cds_ft_alloc_range *range;

	if (cds_list_empty(&arena->free_ranges))
		return NULL;
	range = cds_list_first_entry(&arena->free_ranges,
			struct cds_ft_alloc_range, node);
	cds_list_del(&range->node);
	range->next_unused = 0;
	range->nr_live = 0;
	range->free_list_head = NULL;
	range->recompact_private = false;
	CDS_INIT_LIST_HEAD(&range->partial_node);
	return range;
}

/*
 * Allocate a fresh range when no recycled range is available.  Two layouts:
 * far-metadata gets its own 2 MiB-aligned mmap; near-metadata carves from the
 * arena's superblock pool (creating a new superblock when the head is
 * exhausted).  Caller must hold arena->lock.
 */
#ifdef FT_FAR_METADATA
static
struct cds_ft_alloc_range *range_create_fresh(struct cds_ft_alloc_arena *arena)
{
	/*
	 * Far-metadata range: a 2 MiB-aligned mmap of its own (over-map +
	 * trim to 2 MiB), not carved from a superblock.
	 *
	 *   - The leading 2 MiB is one dense run of node bodies.
	 *   - The remainder (starting at base + 2 MiB) holds the range
	 *     header + metadata[] array.
	 *
	 * So range == base + 2 MiB.  item->range is (item & ~MASK) + 2 MiB.
	 * The metadata offset is the constant base + 2 MiB.
	 */
	size_t alloc_size = cds_ft_arena_range_alloc_size(arena->item_len_order, arena->bitmap);
	size_t mapped = (alloc_size + cds_ft_get_page_size() - 1) & ~(cds_ft_get_page_size() - 1);
	size_t raw_size = mapped + FT_FAR_MACRO_SIZE;
	struct cds_ft_alloc_range *range;
	void *raw, *base;
	uintptr_t pre, post;
	int huge = (arena->ft_group->optimize == CDS_FT_OPTIMIZE_THROUGHPUT);

	raw = mmap(NULL, raw_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (raw == MAP_FAILED)
		return NULL;
	base = (void *) (((uintptr_t) raw + FT_FAR_MACRO_MASK) & ~(uintptr_t) FT_FAR_MACRO_MASK);
	pre = (uintptr_t) base - (uintptr_t) raw;
	post = raw_size - pre - mapped;
	if (pre)
		(void) munmap(raw, pre);
	if (post)
		(void) munmap((char *) base + mapped, post);
	ft_apply_interleave(base, mapped, arena->ft_group->numa_policy);
	ft_apply_thp_policy(base, mapped, huge /* internal: group cds_ft_optimize */);
	range = (struct cds_ft_alloc_range *) ((char *) base + FT_FAR_MACRO_SIZE);
	range->arena = arena;
	range->next_unused = 0;
	range->nr_live = 0;
	range->free_list_head = NULL;
	range->recompact_private = false;
	CDS_INIT_LIST_HEAD(&range->partial_node);
	return range;
}
#else
static
struct cds_ft_alloc_range *range_create_fresh(struct cds_ft_alloc_arena *arena)
{
	size_t alloc_size = cds_ft_arena_range_alloc_size(arena->item_len_order, arena->bitmap);
	size_t alloc_size_aligned = (alloc_size + cds_ft_get_page_size() - 1) & ~(cds_ft_get_page_size() - 1);
	struct cds_ft_alloc_superblock *sb;
	struct cds_ft_alloc_range *range;
	void *ptr;
	int huge = (arena->ft_group->optimize == CDS_FT_OPTIMIZE_THROUGHPUT);

	if (cds_list_empty(&arena->superblocks))
		goto create_sb;
	sb = cds_list_first_entry(&arena->superblocks,
			struct cds_ft_alloc_superblock, node);
	if (sb->used + alloc_size_aligned > sb->size)
		goto create_sb;
	goto carve;
create_sb:
	sb = superblock_create(alloc_size_aligned,
			arena->ft_group->numa_policy, huge);
	if (!sb)
		return NULL;
	cds_list_add(&sb->node, &arena->superblocks);
carve:
	ptr = (char *) sb->base + sb->used;
	sb->used += alloc_size_aligned;
	/* mmap'd anonymous pages are zero-initialized; no memset needed. */
	range = (struct cds_ft_alloc_range *) ((char *) ptr + cds_ft_get_page_size());
	range->arena = arena;
	/* next_unused / nr_live / free_list_head are zero from the fresh mmap. */
	CDS_INIT_LIST_HEAD(&range->partial_node);
	return range;
}
#endif /* FT_FAR_METADATA */

/*
 * Obtain a range for the arena: hand back a recycled range if one is parked,
 * otherwise allocate a fresh one.  Caller must hold arena->lock.
 */
static
struct cds_ft_alloc_range *range_create(struct cds_ft_alloc_arena *arena)
{
	struct cds_ft_alloc_range *range;

	range = range_recycle(arena);
	if (range)
		return range;
	return range_create_fresh(arena);
}

/*
 * Remove a range from the arena's range list.  Default ranges are owned by
 * the superblock (reclaimed at superblock destroy); far-metadata ranges own
 * their 2 MiB macro mmap and unmap it here.  Used at arena teardown for both
 * the live (arena->ranges) and recycled (arena->free_ranges) range lists.
 */
static
void range_destroy(struct cds_ft_alloc_range *range)
{
	cds_list_del(&range->node);
#ifdef FT_FAR_METADATA
	{
		size_t alloc_size = cds_ft_arena_range_alloc_size(
				range->arena->item_len_order, range->arena->bitmap);
		size_t mapped = (alloc_size + cds_ft_get_page_size() - 1) & ~(cds_ft_get_page_size() - 1);

		/* base == range - 2 MiB (items occupy the leading 2 MiB). */
		(void) munmap((char *) range - FT_FAR_MACRO_SIZE, mapped);
	}
#endif
}

/*
 * Reclaim a drained range (nr_live == 0, fully bumped).  The caller has
 * already removed its links from arena->ranges and arena->partial_ranges
 * under arena->lock.  The range is unreachable.  So the page-returning
 * syscall runs here OUTSIDE arena->lock, to keep mutator latency bounded.
 *
 * Both layouts (far and near) are handled the same way.  MADV_DONTNEED
 * returns the RAM of the node-body region.  The range is parked on
 * arena->free_ranges for range_create to recycle.  The header+metadata
 * page stays mapped.  The comment in the body gives the reasons.
 */
static
void ft_arena_reclaim_range(struct cds_ft_alloc_arena *arena,
		struct cds_ft_alloc_range *range)
{
	/*
	 * Return the node-body region to the OS.  It is the leading
	 * FT_RANGE_PAGE_UNIT, just before the range header.  Keep the
	 * header + metadata mapped.  It carries the recycle linkage and
	 * preserves the range's NUMA / THP policy, so reuse is a
	 * syscall-free re-fault.  The range is parked on free_ranges for
	 * range_create to recycle.
	 *
	 * This is layout-agnostic by design.  A far range owns its mapping
	 * and could be munmap'd outright to also return the VA.  Recycling
	 * is preferred for two reasons:
	 *
	 *   - It avoids the munmap + mmap + re-mbind + re-THP churn that
	 *     range turnover (e.g. recompaction) would otherwise pay.
	 *   - MADV_DONTNEED only takes mmap_lock for read.  munmap takes it
	 *     for write, which stalls other threads' faults.
	 *
	 * The retained VA is a bounded, reused pool, not a leak.  Capping it
	 * and munmap'ing the surplus is a later refinement.
	 */
	(void) madvise((char *) range - FT_RANGE_PAGE_UNIT,
			FT_RANGE_PAGE_UNIT, MADV_DONTNEED);
	pthread_mutex_lock(&arena->lock);
	cds_list_add(&range->node, &arena->free_ranges);
	pthread_mutex_unlock(&arena->lock);
}

#ifdef DEBUG_COUNTERS
/*
 * DEBUG_COUNTERS-only leak introspection (no public header decl; tests
 * weak-reference it).  Report the internal + compressed node-arena occupancy
 * of @ft's group:
 *
 *   @live_ranges       ranges still backed by RAM (on arena->ranges);
 *   @reclaimed_ranges  ranges whose pages were released via MADV_DONTNEED
 *                      and that are parked on arena->free_ranges;
 *   @internal_items    sum of per-range nr_live for the internal arenas;
 *   @compressed_items  sum of per-range nr_live for the compressed arenas;
 *   @range_bytes       the per-range page unit.
 *
 * Resident node-arena bytes ~= live_ranges * range_bytes.
 *
 * Caller must ensure no concurrent arena mutation: the lists are walked
 * without the arena lock.
 */
void cds_ft_debug_arena_resident(const struct cds_ft *ft, size_t *live_ranges,
		size_t *reclaimed_ranges, size_t *internal_items,
		size_t *compressed_items, size_t *range_bytes);
void cds_ft_debug_arena_resident(const struct cds_ft *ft, size_t *live_ranges,
		size_t *reclaimed_ranges, size_t *internal_items,
		size_t *compressed_items, size_t *range_bytes)
{
	struct cds_ft_group *group = ft->group;
	size_t lr = 0, rr = 0, internal_li = 0, compressed_li = 0;
	int i;

	for (i = 0; i <= FT_ALLOC_ORDER_MAX; i++) {
		struct cds_ft_alloc_arena *arenas[2];
		int a;

		arenas[0] = group->arena_order[i];		/* internal */
		arenas[1] = group->compressed_arena_order[i];	/* compressed */
		if (arenas[1] == arenas[0])
			arenas[1] = NULL;	/* shared: avoid double count */
		for (a = 0; a < 2; a++) {
			struct cds_ft_alloc_arena *arena = arenas[a];
			struct cds_ft_alloc_range *r;

			if (arena == NULL)
				continue;
			cds_list_for_each_entry(r, &arena->ranges, node) {
				lr++;
				if (a == 0)
					internal_li += r->nr_live;
				else
					compressed_li += r->nr_live;
			}
			cds_list_for_each_entry(r, &arena->free_ranges, node)
				rr++;
		}
	}
	if (live_ranges)
		*live_ranges = lr;
	if (reclaimed_ranges)
		*reclaimed_ranges = rr;
	if (internal_items)
		*internal_items = internal_li;
	if (compressed_items)
		*compressed_items = compressed_li;
	if (range_bytes)
		*range_bytes = FT_RANGE_PAGE_UNIT;
}
#endif /* DEBUG_COUNTERS */

static
struct cds_ft_alloc_arena *cds_ft_arena_create(struct cds_ft_group *ft_group,
		const char *arena_name, size_t item_len_order, bool bitmap)
{
	struct cds_ft_alloc_arena *arena;
	size_t max_items_per_range;

	if (!uatomic_load(&cds_ft_page_size, CMM_RELAXED))
		uatomic_store(&cds_ft_page_size, urcu_get_page_len(),
				CMM_RELAXED);

	/* Reject page sizes larger than the compile-time maximum. */
	if (cds_ft_get_page_size() > (1UL << FT_MAX_PAGE_ORDER)) {
		errno = EINVAL;
		return NULL;
	}
#ifdef FT_PAGE_SIZE_FIXED
	/*
	 * Architectures that hardcode page_size in the inline helpers
	 * (cds_ft_get_page_size) must match the kernel's reported page
	 * size at runtime.  Reject otherwise -- a mismatch would corrupt
	 * item-to-metadata address derivation on the read-side fast path.
	 */
	if (cds_ft_get_page_size() != FT_PAGE_SIZE_FIXED) {
		errno = EINVAL;
		return NULL;
	}
#endif
	/* item_len must be no larger than cds_ft_page_size. */
	if ((1UL << item_len_order) > cds_ft_get_page_size()) {
		errno = EINVAL;
		return NULL;
	}
#ifdef FT_FAR_METADATA
	/* Items fill the leading 2 MiB; metadata (+ bitmap) live past base+2MiB. */
	max_items_per_range = FT_FAR_MACRO_SIZE >> item_len_order;
	/* Bitmap arenas: header + metadata + bitmap must fit the second 2 MiB. */
	if (bitmap && (sizeof(struct cds_ft_alloc_range) +
			max_items_per_range * (sizeof(struct cds_ft_metadata_alloc) +
				sizeof(struct cds_ft_bitmap)) > FT_FAR_MACRO_SIZE)) {
		errno = EINVAL;
		return NULL;
	}
#else
	max_items_per_range = cds_ft_get_page_size() >> item_len_order;
	/* Ensure that range header, metadata array and bitmaps fit in a page. */
	if (bitmap && (sizeof(struct cds_ft_alloc_range) +
			max_items_per_range * (sizeof(struct cds_ft_metadata_alloc) +
				sizeof(struct cds_ft_bitmap)) > cds_ft_get_page_size())) {
		errno = EINVAL;
		return NULL;
	}
#endif
	arena = calloc(1, sizeof(struct cds_ft_alloc_arena));
	if (!arena)
		goto error_alloc;
	arena->ft_group = ft_group;
	arena->item_len_order = item_len_order;
	arena->max_nr_items_per_range = max_items_per_range;
	arena->bitmap = bitmap;
	/*
	 * The dedicated compressed-node arena (speculative groups route
	 * compressed nodes here via cds_ft_alloc_compressed_item).  Lets the
	 * compactor keep relocated compressed nodes in their own private
	 * ranges, separate from internal nodes of the same order.
	 */
	arena->compressed = arena_name &&
		!strcmp(arena_name, "cds_ft_alloc_compressed");
	arena->cell = arena_name && !strcmp(arena_name, "cds_ft_alloc_cell");
	CDS_INIT_LIST_HEAD(&arena->ranges);
	CDS_INIT_LIST_HEAD(&arena->partial_ranges);
	CDS_INIT_LIST_HEAD(&arena->free_ranges);
	CDS_INIT_LIST_HEAD(&arena->superblocks);
	if (arena_name) {
		arena->name = strdup(arena_name);
		if (!arena->name)
			goto error_alloc;
	}
	pthread_mutex_init(&arena->lock, NULL);
	return arena;

error_alloc:
	free(arena);
	errno = ENOMEM;
	return NULL;
}

static
void cds_ft_arena_destroy(struct cds_ft_alloc_arena *arena)
{
	struct cds_ft_alloc_range *range, *range_tmp;
	struct cds_ft_alloc_superblock *sb, *sb_tmp;

	if (!arena)
		return;
	pthread_mutex_destroy(&arena->lock);
	/*
	 * No separate partial_ranges drain is needed.  partial_ranges is an
	 * auxiliary index, linked via range->partial_node.  Every range on
	 * it is simultaneously on arena->ranges, linked via range->node.
	 * So the ranges + free_ranges walks below already free every range.
	 *
	 * The partial_node links are left dangling on teardown.  This is
	 * safe ONLY because partial_ranges is never traversed once the
	 * ranges are freed.  Any future teardown step that walks
	 * partial_ranges must run BEFORE these loops.
	 */
	cds_list_for_each_entry_safe(range, range_tmp, &arena->ranges, node)
		range_destroy(range);
	/*
	 * Reclaimed ranges parked on free_ranges also need teardown:
	 *
	 *   - Far ranges still own their mapping and must be munmap'd.
	 *     range_destroy does so.
	 *   - Near ranges are freed with their superblock below.  So
	 *     range_destroy just unlinks them here.
	 */
	cds_list_for_each_entry_safe(range, range_tmp, &arena->free_ranges, node)
		range_destroy(range);
	cds_list_for_each_entry_safe(sb, sb_tmp, &arena->superblocks, node)
		superblock_destroy(sb);
	free(arena->name);
	free(arena);
}

/*
 * Per-thread recompaction allocation context (NULL except on a thread running
 * cds_ft_compact).  See struct ft_recompact_alloc_ctx and the routing below.
 */
static __thread struct ft_recompact_alloc_ctx *ft_recompact_alloc_tls;

void ft_recompact_alloc_init(struct ft_recompact_alloc_ctx *ctx)
{
	size_t i;

	for (i = 0; i <= FT_ALLOC_ORDER_MAX; i++) {
		ctx->cur[i] = NULL;
		ctx->cur_compressed[i] = NULL;
	}
	ctx->cur_cell = NULL;
	CDS_INIT_LIST_HEAD(&ctx->all);
}

void ft_recompact_alloc_set_active(struct ft_recompact_alloc_ctx *ctx)
{
	ft_recompact_alloc_tls = ctx;
}

void ft_recompact_alloc_merge(struct ft_recompact_alloc_ctx *ctx)
{
	struct cds_ft_alloc_range *range, *tmp;

	/*
	 * Splice the private ranges into their arenas' general range lists so
	 * the general allocator sees them.  They hold the densely-relocated
	 * nodes; any free slots (the last, partially-filled range per order)
	 * become reusable through the normal frontier/partial paths.  Clearing
	 * recompact_private makes them ordinary ranges again.
	 */
	cds_list_for_each_entry_safe(range, tmp, &ctx->all, node) {
		struct cds_ft_alloc_arena *arena = range->arena;
		bool reclaim = false;

		pthread_mutex_lock(&arena->lock);
		cds_list_del(&range->node);
		range->recompact_private = false;
		if (range->nr_live == 0 &&
		    range->next_unused == arena->max_nr_items_per_range) {
			/* Fully drained while private: reclaim it now. */
			reclaim = true;
		} else {
			cds_list_add(&range->node, &arena->ranges);
			/*
			 * Slots freed while the range was private were threaded
			 * on its own free list WITHOUT the partial_ranges
			 * listing (see cds_ft_do_free_item).  Restore the
			 * "listed iff free_list_head != NULL" invariant, so
			 * that the general allocator can reuse them.
			 */
			if (range->free_list_head)
				cds_list_add(&range->partial_node,
					&arena->partial_ranges);
		}
		pthread_mutex_unlock(&arena->lock);
		if (reclaim)
			ft_arena_reclaim_range(arena, range);
	}
}

bool cds_ft_metadata_in_recompact_private(struct cds_ft_metadata *metadata)
{
	return cds_ft_metadata_to_range(metadata)->recompact_private;
}

/*
 * Initialize a freshly bump-allocated slot's per-slot kept-mapped state.
 *
 * A fresh mmap range presents zeroed metadata + bitmap.  A recycled range
 * does not.  At reclaim, only its node-body region was returned via
 * MADV_DONTNEED.  Its metadata[] array and its reverse bitmap[] array live
 * in the kept-mapped region, which starts at the range header.  They retain
 * the previous life's contents: the 0xfe poison written on free, or stale
 * counts / occupancy bits.
 *
 * That region is deliberately kept mapped (not MADV_DONTNEED'd).  It holds
 * the range header, which carries the recycle linkage.  It also preserves
 * the range's NUMA / THP policy for a syscall-free reuse.  See
 * ft_arena_reclaim_range.
 *
 * Clearing them here makes a bump slot from a recycled range equivalent to
 * one from a fresh range.  The freelist path provides the same guarantee
 * for reused slots.
 *
 * On Linux the node body itself is not touched.  It re-faults zero, after
 * MADV_DONTNEED on a recycled range and from the fresh mmap otherwise.  On
 * platforms where MADV_DONTNEED does not zero on refault, it is cleared
 * explicitly below (see FT_MADV_DONTNEED_ZEROES).
 *
 * alloc_index must be assigned before the bitmap address is derived:
 * cds_ft_metadata_to_item reads it.
 */
static
void ft_clear_recycled_slot(struct cds_ft_alloc_arena *arena,
		struct cds_ft_metadata_alloc *item, size_t item_index)
{
	memset(&item->metadata, 0, sizeof(item->metadata));
	item->metadata.alloc_index = item_index;
	if (arena->bitmap) {
		void *p = cds_ft_metadata_to_item(&item->metadata);

		memset(cds_ft_item_to_bitmap(p, arena->item_len_order), 0,
				sizeof(struct cds_ft_bitmap));
	}
	if (!FT_MADV_DONTNEED_ZEROES) {
		/*
		 * Where MADV_DONTNEED does not zero on refault, a recycled
		 * range's node body still holds the previous life's bytes.
		 * Clear it, as the freelist-reuse path does.  Compiled out on
		 * Linux, where the body re-faults zero.
		 */
		void *p = cds_ft_metadata_to_item(&item->metadata);

		memset(p, 0, 1UL << arena->item_len_order);
	}
}

static
struct cds_ft_metadata *cds_ft_arena_alloc(struct cds_ft_alloc_arena *arena)
{
	struct cds_ft_metadata_alloc *item;
	struct cds_ft_alloc_range *range;
	size_t item_index;

	pthread_mutex_lock(&arena->lock);

	/*
	 * A recompaction context is active on this thread.  Bump-allocate
	 * into a per-order private fresh range.  The range is created
	 * lazily.  It is kept off the arena's general lists until
	 * ft_recompact_alloc_merge splices it in.  So the relocated nodes
	 * pack densely.  Unrelated concurrent allocations on other threads
	 * (ctx == NULL) are unaffected by the path below.
	 */
	{
		struct ft_recompact_alloc_ctx *ctx = ft_recompact_alloc_tls;

		if (caa_unlikely(ctx != NULL)) {
			size_t order = arena->item_len_order;
			/*
			 * Internal, compressed-node and cell arenas may share
			 * item-length orders, but they are distinct arenas.
			 * So keep a separate private current-range for each.
			 * Otherwise items of different kinds at the same
			 * order would share a range.
			 */
			struct cds_ft_alloc_range **curp;

			if (arena->cell)
				curp = &ctx->cur_cell;
			else
			if (arena->compressed)
				curp = &ctx->cur_compressed[order];
			else
				curp = &ctx->cur[order];

			assert(order <= FT_ALLOC_ORDER_MAX);
			range = *curp;
			if (!range || range->next_unused ==
					arena->max_nr_items_per_range) {
				range = range_create(arena);
				if (!range) {
					errno = ENOMEM;
					pthread_mutex_unlock(&arena->lock);
					return NULL;
				}
				range->recompact_private = true;
				*curp = range;
				cds_list_add(&range->node, &ctx->all);
			}
			item_index = range->next_unused++;
			range->nr_live++;
			item = &range->metadata[item_index];
			/*
			 * Clear the bump slot: a recycled range's kept-mapped
			 * metadata/bitmap may carry the previous life's contents.
			 * See ft_clear_recycled_slot.
			 */
			ft_clear_recycled_slot(arena, item, item_index);
			pthread_mutex_unlock(&arena->lock);
			return &item->metadata;
		}
	}

	/*
	 * Reuse a freed slot from the most-recently-used range that still
	 * has one.  cds_ft_do_free_item keeps partial_ranges MRU-ordered,
	 * so its head holds the hottest just-freed slot.  This preserves
	 * the locality that the former single per-arena freelist provided.
	 * A range sits on partial_ranges exactly while its
	 * free_list_head != NULL.
	 */
	if (!cds_list_empty(&arena->partial_ranges)) {
		struct cds_ft_metadata_alloc *fl;
		size_t saved_alloc_index;
		void *p;

		range = cds_list_first_entry(&arena->partial_ranges,
				struct cds_ft_alloc_range, partial_node);
		fl = range->free_list_head;
		saved_alloc_index = fl->metadata.alloc_index;
		range->free_list_head = fl->free_list_next;
		if (!range->free_list_head)
			cds_list_del(&range->partial_node);
		p = cds_ft_metadata_to_item(&fl->metadata);
		memset(p, 0, 1U << arena->item_len_order);
		memset(&fl->metadata, 0, sizeof(fl->metadata));
		fl->metadata.alloc_index = saved_alloc_index;
		if (arena->bitmap) {
			struct cds_ft_bitmap *bitmap = cds_ft_item_to_bitmap(p, arena->item_len_order);
			memset(bitmap, 0, sizeof(struct cds_ft_bitmap));
		}
		range->nr_live++;
		pthread_mutex_unlock(&arena->lock);
		return &fl->metadata;
	}
	/*
	 * No reusable slot: bump the frontier range.  The frontier is the
	 * most recent range, first in the list.  If there is no range, or
	 * the frontier has no room left, create a new range and prepend it
	 * to the list.
	 */
	if (cds_list_empty(&arena->ranges))
		goto create_range;
	range = cds_list_first_entry(&arena->ranges, struct cds_ft_alloc_range, node);
	if (range->next_unused + 1 > arena->max_nr_items_per_range)
		goto create_range;
	else
		goto room_left;
create_range:
	range = range_create(arena);
	if (!range) {
		errno = ENOMEM;
		pthread_mutex_unlock(&arena->lock);
		return NULL;
	}
	/* Add range to head of list. */
	cds_list_add(&range->node, &arena->ranges);
room_left:
	/* First range in list has room left. */
	item_index = range->next_unused++;
	range->nr_live++;
	item = &range->metadata[item_index];
	/* Clear stale/poisoned metadata + bitmap from a recycled range;
	 * see ft_clear_recycled_slot. */
	ft_clear_recycled_slot(arena, item, item_index);
	pthread_mutex_unlock(&arena->lock);
	return &item->metadata;
}

#ifdef FEATURE_FT_FAULT_INJECT
/*
 * Test-only allocation fault injection (single-threaded use; compiled out
 * unless FEATURE_FT_FAULT_INJECT).
 *
 * -1 disables.  Arm to N: the next N item allocations succeed and the
 * (N+1)-th returns NULL (simulated -ENOMEM).  Then it auto-disarms.
 *
 * Used to drive mutators through every OOM error path.  Both
 * cds_ft_alloc_item and cds_ft_alloc_compressed_item route here.
 */
long cds_ft_fault_alloc_countdown = -1;
/*
 * Separate fault counter for flip-transaction allocations
 * (ft_flip_txn_create_bounded).  It has the same arm-to-N semantics as the
 * arena counter above, but is independent of it.  Flip-txns are raw malloc,
 * not arena items.  So a test arms THIS to drive the grow-and-abort /
 * pre-reserve commit paths without perturbing the arena-fault tests:
 *
 *   - the ft_ord_cell_flip_try abort;
 *   - the ft_chain_compress_fused / ft_detach_node txn pre-reservation
 *     failure.
 */
long cds_ft_fault_flip_countdown = -1;

/*
 * Test-only per-node LOCK-acquisition fault injection (MW LOCK_FINE, §9.3).
 * Counts down over ft_lock_member() calls and fails the (n+1)-th with
 * -EAGAIN, exactly as a peer holding the node's FT_STATE_LOCK would.
 *
 * Why this knob has to exist: a LOCK_FINE trie still serializes every writer
 * behind the FT-wide lock until the op-domains finish converting (§11.1), so no
 * peer can ever be holding a per-node lock -- the acquire cannot fail, and every
 * bail path it feeds (drop the fresh copy, unlock the members already held,
 * re-descend) is DEAD CODE that a green soak says nothing about.  This makes the
 * acquire fail on demand so those paths are actually executed and the abort
 * boundary (byte-for-byte clean, no leaked lock, no leaked allocation) is
 * tested rather than assumed.
 */
long cds_ft_fault_lock_countdown = -1;

/*
 * Test-only FINAL-COMMIT abort injection for the coherent-rekey merge fold.
 * Counts down over the fold's one commit and forces the (n+1)-th to abort,
 * exactly as a peer that won a raced MW slot would.
 *
 * Why this knob has to exist is stated as a measurement, not as an argument.
 * The shared-destination merge oracle contends the fold's acquires HARD.
 * Over 71218 merges it took:
 *
 *   - the publish-parent fence miss 62342 times;
 *   - the overlap spine 89 times;
 *   - the commit_edges miss 1079 times;
 *   - THIS exit 0 times.
 *
 * Every acquire sits ahead of the commit.  So a peer that could make the
 * commit lose has already been turned away by one of them.  Real contention
 * therefore does not reach the fold's LONGEST unwind.  That unwind has:
 *
 *   - a fully built cluster;
 *   - both glues;
 *   - the src side's retires;
 *   - every fence still held.
 *
 * Only injection reaches it.
 *
 * It routes through the engine's own unpublished-discard path
 * (@acquire_miss):
 *
 *   - age the handle;
 *   - clear every registered LOCK;
 *   - report ABORT.
 *
 * So the unwind under test is the real one and not a synthesised status.
 */
long cds_ft_fault_commit_countdown = -1;

/*
 * Test-only COMMIT-abort injection for the REPLACE FAMILY:
 *
 *   - _cds_ft_insert_replace's chain + one-commit publishes;
 *   - cds_ft_replace's two HEAD arms.
 *
 * Counts down over those commits and forces the (n+1)-th to abort,
 * exactly as a peer that won a raced MW slot would.
 *
 * Why this knob has to exist is stated as a measurement, not as an argument.
 * All FIVE of those abort arms run ZERO times across the whole fault-audit
 * suite.  So do the two that the head path already had.  The suite is
 * ft_unit and ft_inv both, with every arm instrumented and counted.  Two
 * reasons compound:
 *
 *   - The replace family was contract-excluded under LOCK_FINE when this
 *     was measured, so no peer raced it.  It has been FINE-concurrent since
 *     aa20b4a9.
 *   - Its commits carry only §4.B GUARDS, never an acquire.  So
 *     cds_ft_fault_lock_countdown, which fails an ACQUIRE, cannot reach
 *     them either.
 *
 * The existing fault sweep drives cds_ft_insert only.  So the arms that
 * decide whether a lost replace is reported as success were, until this
 * knob, unexecutable.
 *
 * It routes through the engine's own unpublished-discard path
 * (@acquire_miss):
 *
 *   - age the handle;
 *   - clear every registered LOCK;
 *   - report ABORT.
 *
 * So what runs is the real unwind and not a synthesised status.
 */
long cds_ft_fault_replace_countdown = -1;

/*
 * Test-only refused-acquire injection for cds_ft_compact_step's relocations.
 *
 * WHY THIS EXISTS.  ft_compact_relocate_at bails on -ENOMEM and on -EAGAIN.
 * It reports both through one bool (*@oom).  So a contention refusal is
 * announced as CDS_FT_COMPACT_OOM, "free memory and resume".  That is the
 * wrong remedy for a peer.
 *
 * The -EAGAIN half cannot occur while compact requires caller
 * writer-exclusion.  Measured: 1,798 relocations reached the bail, 6 with
 * -ENOMEM and 0 with -EAGAIN.  So the arm that the fine-grained conversion
 * must have has no way to run.  This forces it.
 *
 * ☠ IT FIRES BEFORE ft_node_recompact's LOCK-SET ACQUIRE, not on its commit.
 * The commit is contractually INFALLIBLE: the eager child re-parent ahead
 * of it is a point of no return.  ft_compact_relocate_at ASSERTS its
 * success, so arming that commit crashes on the assert.  Refusing the
 * acquire is the real -EAGAIN class.  It returns with nothing acquired and
 * nothing published.
 */
long cds_ft_fault_compact_countdown = -1;

/*
 * Test-only seam hook, called by ft_writer_lock_gp_wait between releasing the
 * FT-wide lock and waiting for the grace period.  A test uses it to park a
 * bulk op at its seam, where point writers may run.
 */
void (*cds_ft_fault_seam_hook)(struct cds_ft *ft);

/*
 * Test-only refused-acquire injection for cds_ft_remove_all.
 *
 * WHY THIS EXISTS.  remove_all's contention arm reports
 * CDS_FT_STATUS_BUSY_ERROR.  Its allocation arm reports MEMORY_ERROR.  No
 * workload reaches the contention arm on its own.  The op requires caller
 * writer-exclusion, so no peer can refuse it a lock-set.  Measured over
 * both suites, its failure tail ran ZERO times in 4,807,509 calls.  So the
 * arm that the fine-grained conversion needs has no other way to run.
 *
 * The injection is scoped to the dynamic extent of remove_all's own detach
 * (ft_removeall_fault_scope_enter/exit), not to a mode.  The reason: 99.95%
 * of its commits go through ft_detach_node, which builds its txn
 * internally.  Measured commits:
 *
 *   - detach: 697,344 of 697,675;
 *   - holder-commit: 101;
 *   - NIL-commit: 7.
 */
long cds_ft_fault_removeall_countdown = -1;
#endif

#ifdef FEATURE_FT_PROBE_EMPTY_INSERT
/*
 * A/B counters for the born-empty insert candidate (ft-insert.h).  They
 * are reported at exit so that a soak run records BOTH the catch and the
 * bail.  A change that makes the defect vanish while its bail never fires
 * has not fixed anything.  Four already-refuted removal-side candidates
 * were settled by this rule.
 */
unsigned long cds_ft_probe_empty_publish_split;
unsigned long cds_ft_probe_empty_publish_attach;
unsigned long cds_ft_probe_reach_split;
unsigned long cds_ft_probe_reach_attach;

__attribute__((destructor))
static void cds_ft_probe_empty_report(void)
{
	fprintf(stderr, "EMPTYPUB split=%lu attach=%lu (reached split=%lu attach=%lu)\n",
		cds_ft_probe_empty_publish_split,
		cds_ft_probe_empty_publish_attach,
		cds_ft_probe_reach_split, cds_ft_probe_reach_attach);
}
#endif

#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
/*
 * B4 residual probe (ft-graft.h, cds_ft_graft_swap).
 *
 * The oracle inv_graft_swap_shared_dst_nolist dies with this message:
 * "depth 1: internal node N parent mismatch: got (nil)".
 * It means that the extract side re-rooted into @swap_ft a node that is
 * STILL wired into dst at the graft point.
 *
 * Rather than argue about which compare is missing, COUNT the class.  After
 * this op's OWN insert commit reported OK, re-descend to @key.  Ask whether
 * the occupant that this attempt planned to displace is still there.
 * @reoccupy > 0 is the corruption, caught one statement before it is
 * committed.
 */
unsigned long cds_ft_probe_gs_commit_ok;
unsigned long cds_ft_probe_gs_reoccupy;
unsigned long cds_ft_probe_gs_slot_moved;
unsigned long cds_ft_probe_gs_retry;
unsigned long cds_ft_probe_gs_exact;
unsigned long cds_ft_probe_gs_kshort;
unsigned long cds_ft_probe_gs_delegate;
unsigned long cds_ft_probe_gs_fused;
unsigned long cds_ft_probe_gs_ext_child;
/*
 * @torn: the descent's TWO loads of the graft-point slot disagreed.
 *
 *   - Load #1: ft_descent_step sets d->nf from
 *     ft_node_get_nth_reanchor_slot.
 *   - Load #2: ft_graft_swap_descend then does *raw_ret = *d->nfp.
 *
 * The expected-old comes from #2.  The extracted occupant (d->nf) comes
 * from #1.  So a peer that swaps the graft point between the two loads
 * makes the op RATIFY displacing one node while RE-ROOTING another.
 *
 * @alias: swap_ft's root already IS the dst occupant on entry (the downstream
 * double-ownership state).
 *
 * @canon_alias: what we are about to publish equals what we quoted as
 * expected-old.  This is the no-op replace.
 */
/*
 * @pubabort: the KEY_SHORTER legacy publish's DROPPED commit status.  The
 * drop is ft_glue_publish's `(void) ft_ord_cell_flip_into`, whose comment
 * claims "ABORT unreachable under its exclusion".  By then its
 * apply_deferred has already rewritten LIVE dst parent back-pointers.  So a
 * dropped abort leaves them naming a node this op never published.
 */
unsigned long cds_ft_probe_gs_pubabort;
unsigned long cds_ft_probe_gs_pubok;
unsigned long cds_ft_probe_gs_fuse_pcn;
unsigned long cds_ft_probe_gs_fuse_ccn;
unsigned long cds_ft_probe_gs_fuse_len;
unsigned long cds_ft_probe_gs_fuse_incoh;
unsigned long cds_ft_probe_gs_fuse_incoh_committed;
unsigned long cds_ft_probe_gs_torn;
unsigned long cds_ft_probe_gs_alias;
unsigned long cds_ft_probe_gs_canon_alias;

__attribute__((destructor))
static void cds_ft_probe_gs_report(void)
{
	fprintf(stderr, "GSPROBE commit_ok=%lu reoccupy=%lu slot_moved=%lu "
		"retry=%lu | pubok=%lu pubabort=%lu | torn=%lu alias=%lu canon_alias=%lu "
		"| shapes exact=%lu kshort=%lu delegate=%lu "
		"fused=%lu ext_child=%lu | fuse_pcn=%lu fuse_ccn=%lu fuse_len=%lu "
		"fuse_incoh=%lu incoh_committed=%lu\n",
		cds_ft_probe_gs_commit_ok, cds_ft_probe_gs_reoccupy,
		cds_ft_probe_gs_slot_moved, cds_ft_probe_gs_retry,
		cds_ft_probe_gs_pubok, cds_ft_probe_gs_pubabort,
		cds_ft_probe_gs_torn, cds_ft_probe_gs_alias,
		cds_ft_probe_gs_canon_alias,
		cds_ft_probe_gs_exact, cds_ft_probe_gs_kshort,
		cds_ft_probe_gs_delegate, cds_ft_probe_gs_fused,
		cds_ft_probe_gs_ext_child, cds_ft_probe_gs_fuse_pcn,
		cds_ft_probe_gs_fuse_ccn, cds_ft_probe_gs_fuse_len,
		cds_ft_probe_gs_fuse_incoh,
		cds_ft_probe_gs_fuse_incoh_committed);
}
#endif

#ifdef FEATURE_FT_PROBE_PROMOTE
/*
 * §4.B unguarded external-promote probe.
 *
 * ft_node_replace_ptr's promote arm stores into the LIVE holder when @pub
 * is NULL.  The §4.B guard for that holder sits inside the
 * `pub && pub->armed` branch.  So the pub-less variant mutates before
 * acquiring.
 *
 * @pub is NULL exactly when the ordered list is off.  So the unguarded
 * variant needs a LIST-OFF trie AND a promote.  Counted at the call site;
 * see FT_PROMOTE_PROBE_INC.
 */
unsigned long cds_ft_probe_promote_deferred;
unsigned long cds_ft_probe_promote_immediate;
unsigned long cds_ft_probe_promote_guarded;

__attribute__((destructor))
static void cds_ft_probe_promote_report(void)
{
	fprintf(stderr, "PROMOTEPROBE promotes: deferred=%lu immediate(UNGUARDED)=%lu | §4.B acquire ran=%lu\n",
		cds_ft_probe_promote_deferred, cds_ft_probe_promote_immediate, cds_ft_probe_promote_guarded);
}
#endif

#ifdef FEATURE_FT_FAULT_INJECT

/*
 * Test-only REKEY-coherence second-walk fault injection
 * (automatic under the move gate).  Counts down over coherence checks and
 * forces the (n+1)-th to report a MISS, exactly as a concurrent in-trie
 * rekey that restructured the reader's path would.  The coherence checks
 * are the point lookup's two descents and the relational two-pass.  Both
 * go through ft_rekey_fault_miss().
 *
 * Why this knob has to exist: with no concurrent rekey the two passes
 * always AGREE.  So the coherent lookup's mismatch/retry arm is DEAD CODE,
 * and a green single-threaded soak says nothing about it.  This forces one
 * miss on demand.  The re-descend loop is then actually executed.  It is
 * shown to still return the correct (coherent) result, rather than assumed
 * to.  The forced miss is self-clearing (resets to -1 when it fires), so
 * it cannot livelock the loop.
 */
long cds_ft_fault_rekey_countdown = -1;
#endif

/*
 * Per-thread active node-allocation reserve set (MW §11 drop-mechanics).
 *
 * The reserve is a bulk op's OOM-avoidance pool.  graft / merge /
 * graft_swap pre-fill it while they can still fail cleanly.  Then they
 * ACTIVATE it.  From then on, every cds_ft_alloc_item into the target trie
 * DRAWS from the pool instead of the fallible arena.  So the commit cannot
 * fail mid-way.
 *
 * It is NOT a per-trie field.  That would be safe only while an FT-wide
 * writer lock serialized bulk ops on a trie.  The FT-wide-lock drop removes
 * that serialization.  Two concurrent grafts into one live dst would then
 * both stamp the same field and collide.
 *
 * A reserve is a stack-local owned by ONE op on ONE thread.  So its
 * activation state is naturally THREAD-LOCAL.  Each writer's bulk op owns
 * its reserve, and a peer on the same dst has its own.  There is no shared
 * field and no collision.
 *
 * A single op can activate ONE reserve on TWO tries.  The case is the
 * merge same-trie rekey: dst + the detach product @tmp.  So this is a
 * small array of {trie, reserve} records, not one slot.
 *
 * The old per-trie single-activation invariant was
 * assert(!ft->active_reserve) in activate.  It becomes: a trie is covered
 * by at most one record at a time (asserted).
 *
 * FT_TLS_RESERVE_MAX bounds concurrent activations on one thread (2 today).
 * An overflow asserts, so that a future nesting shape surfaces loudly
 * instead of silently corrupting.
 */
#define FT_TLS_RESERVE_MAX	4
struct ft_tls_reserve_rec {
	const struct cds_ft *ft;
	struct cds_ft_alloc_reserve *r;
};
static __thread struct ft_tls_reserve_rec ft_tls_reserves[FT_TLS_RESERVE_MAX];
static __thread unsigned int ft_tls_reserve_nr;


/* The reserve this thread has activated on @ft, or NULL. */
static inline
struct cds_ft_alloc_reserve *ft_tls_reserve_for(const struct cds_ft *ft)
{
	unsigned int i;

	for (i = 0; i < ft_tls_reserve_nr; i++)
		if (ft_tls_reserves[i].ft == ft)
			return ft_tls_reserves[i].r;
	return NULL;
}

bool cds_ft_alloc_reserve_covers(const struct cds_ft *ft)
{
	return ft_tls_reserve_for(ft) != NULL;
}

static
struct cds_ft_metadata *cds_ft_alloc_item_from(struct cds_ft *ft,
		struct cds_ft_alloc_arena **arena_p,
		const char *arena_name,
		size_t item_len_order, bool bitmap,
		enum cds_ft_alloc_kind kind)
{
	struct cds_ft_alloc_arena *arena;

	/*
	 * Draw from this thread's active node-allocation reserve for @ft.
	 * The draw sits ABOVE the fault hook and the arena.  So a bulk op
	 * that pre-filled a reserve cannot fail here mid-commit.  The fault
	 * hook only bites during the fill, before the reserve is activated.
	 * Cells are never reserved.
	 *
	 * The reserve set is thread-local.  A peer writer on @ft (or
	 * another trie) has its own.  No synchronization is needed.  The nr
	 * guard keeps the common no-reserve path to a single TLS load.
	 */
	if (kind != CDS_FT_ALLOC_KIND_CELL && ft_tls_reserve_nr) {
		struct cds_ft_alloc_reserve *r = ft_tls_reserve_for(ft);

		if (r) {
			unsigned int *cnt = &r->count[kind][item_len_order];

			if (*cnt)
				return r->items[kind][item_len_order][--(*cnt)];
#ifndef FEATURE_FT_COMPRESS
			if (kind == CDS_FT_ALLOC_KIND_NODE && r->branch_count
					&& item_len_order == r->branch_order)
				return r->branch[--r->branch_count];
#endif
			/*
			 * The reserve is active but exhausted for this (kind,
			 * order).  The op's manifest under-counted what it
			 * allocates.
			 *
			 * Surface it in debug.  Otherwise the op would fall
			 * through to a fallible allocation here, and the
			 * reserve's no-fail guarantee would be lost.  The
			 * report names the missing bucket, so a manifest can
			 * be extended to a new shape.  Production falls
			 * through, and degrades to pre-reserve behaviour.
			 *
			 * The report names ONE ATTEMPT's manifest.  An
			 * aborted attempt gives its items back
			 * (ft_alloc_reserve_refund), so a retry loop cannot
			 * walk a bucket down round by round.
			 */
			fprintf(stderr,
				"ft alloc reserve underflow: kind=%d order=%zu\n",
				(int) kind, item_len_order);
			assert(0);
		}
	}

#ifdef FEATURE_FT_FAULT_INJECT
	if (cds_ft_fault_alloc_countdown >= 0) {
		if (cds_ft_fault_alloc_countdown == 0) {
			cds_ft_fault_alloc_countdown = -1;
			errno = ENOMEM;
			return NULL;
		}
		cds_ft_fault_alloc_countdown--;
	}
#endif
	if (!uatomic_load(&cds_ft_page_size, CMM_RELAXED))
		uatomic_store(&cds_ft_page_size, urcu_get_page_len(),
				CMM_RELAXED);
	if ((1UL << item_len_order) > cds_ft_get_page_size()) {
		errno = EINVAL;
		return NULL;
	}
	arena = uatomic_load(arena_p, CMM_ACQUIRE);
	if (caa_unlikely(!arena)) {
		pthread_mutex_lock(&ft->group->arena_lock);
		arena = *arena_p;
		if (!arena) {
			arena = cds_ft_arena_create(ft->group, arena_name, item_len_order, bitmap);
			if (!arena) {
				pthread_mutex_unlock(&ft->group->arena_lock);
				return NULL;
			}
			uatomic_store(arena_p, arena, CMM_RELEASE);
		}
		pthread_mutex_unlock(&ft->group->arena_lock);
	}
	return cds_ft_arena_alloc(arena);
}

struct cds_ft_metadata *cds_ft_alloc_item(struct cds_ft *ft, size_t item_len_order, bool bitmap)
{
	return cds_ft_alloc_item_from(ft,
		&ft->group->arena_order[item_len_order],
		"cds_ft_alloc", item_len_order, bitmap,
		CDS_FT_ALLOC_KIND_NODE);
}

/*
 * Allocate a compressed node.
 *
 * A skip-compressed group routes it to a separate arena set.  The
 * compressed-node pages then don't share allocator pages with the
 * internal nodes, which ARE on the cand-mode descent hot path.
 * Compressed nodes are cold for lookup in such a group: the descent
 * never reads compressed-node bodies, only the skip_len high bits.
 *
 * Non-speculative groups can still use this entry point but get the
 * same single-arena-set behavior as cds_ft_alloc_item (caller routes).
 */
__attribute__((visibility("hidden")))
struct cds_ft_metadata *cds_ft_alloc_compressed_item(struct cds_ft *ft,
		size_t item_len_order)
{
	struct cds_ft_alloc_arena **arena_p;

	if (ft->group->speculative)
		arena_p = &ft->group->compressed_arena_order[item_len_order];
	else
		arena_p = &ft->group->arena_order[item_len_order];
	return cds_ft_alloc_item_from(ft, arena_p,
		ft->group->speculative ? "cds_ft_alloc_compressed" : "cds_ft_alloc",
		item_len_order, false,
		ft->group->speculative ? CDS_FT_ALLOC_KIND_COMPRESSED :
			CDS_FT_ALLOC_KIND_NODE);
}

/*
 * Allocate an ordinal cell from the group's dedicated cell arena.
 *
 * The uniform 32 B cells then pack contiguously in their own item region.
 * This is the dense stride that cds_ft_compact relocates them into.  Cells
 * are never mixed with the internal-node or compressed-node arenas.
 */
__attribute__((visibility("hidden")))
struct cds_ft_metadata *cds_ft_alloc_cell_item(struct cds_ft *ft)
{
	return cds_ft_alloc_item_from(ft, &ft->group->cell_arena,
		"cds_ft_alloc_cell", FT_ORD_CELL_ALLOC_ORDER, false,
		CDS_FT_ALLOC_KIND_CELL);
}

/*
 * Synchronous free body shared by the call_rcu callback and the
 * exclusive-mode fast path.  Frees the extended density counters,
 * then either poisons the slot (FT_IMMEDIATE_FREE testing mode) or
 * returns the slot to the arena free list.
 */
static
void cds_ft_do_free_item(struct cds_ft_metadata *metadata)
{
	/*
	 * ☞ THE HOOK BELONGS AT THE CHOKEPOINT, NOT AT THE WRAPPERS.
	 *
	 * The hook used to sit on cds_ft_free_item / _unpublished /
	 * _deferred.  Between them, those miss TWO routes back to the arena:
	 *  - the arena drain's bulk reclaim;
	 *  - cds_ft_free_item_rcu, the call_rcu callback (the common one).
	 *
	 * The non-owner-clear table is keyed by ADDRESS.  A free that the
	 * table does not see leaves a stale "held by tid T" against the
	 * chunk.  The allocator then hands the chunk out again.  The next
	 * legitimate take of the NEW item reads as a steal.
	 *
	 * Every route ends here.  This is the only place that cannot be
	 * missed.
	 */
	FT_DT_NOTE_FREED(metadata);

#ifdef FT_ENABLE_TRACING
	/*
	 * THE push itself.  It is keyed on the ITEM pointer, to match
	 * item_alloc / item_free.  @via 0 means that no site set it.  For
	 * this function, that means the call_rcu callback.
	 */
	FT_TP(item_reclaim, cds_ft_metadata_to_item(metadata),
		(unsigned int) (ft_dbg_free_via ? ft_dbg_free_via
					: FT_DBG_VIA_RCU));
#endif
	struct cds_ft_metadata_alloc *metadata_alloc;

	metadata_alloc =
		caa_container_of(metadata, struct cds_ft_metadata_alloc, metadata);


#ifdef FT_IMMEDIATE_FREE
	/*
	 * Immediate-free testing mode: poison metadata and node data
	 * so any subsequent access crashes deterministically and the
	 * slot is never reused.  See cds_ft_free_item() docstring for
	 * the safety constraints.
	 */
	{
		struct cds_ft_alloc_range *range =
			cds_ft_metadata_to_range(metadata);
		struct cds_ft_alloc_arena *arena = range->arena;
		size_t item_len = 1UL << arena->item_len_order;
		void *item = cds_ft_metadata_to_item(metadata);

		/* Single-threaded testing mode: no arena->lock needed. */
		assert(range->nr_live > 0);
		range->nr_live--;
		memset(item, 0xfe, item_len);
		memset(metadata_alloc, 0xfe, sizeof(*metadata_alloc));
	}
#else
	{
		struct cds_ft_alloc_range *range =
			cds_ft_metadata_to_range(metadata);
		struct cds_ft_alloc_arena *arena = range->arena;
		bool reclaim = false;

		pthread_mutex_lock(&arena->lock);
		assert(range->nr_live > 0);
		range->nr_live--;
		if (caa_unlikely(range->recompact_private)) {
			/*
			 * The range belongs to the private context of an
			 * in-flight compaction.  Two things must not happen:
			 *
			 *  - Unlinking range->node here, as the reclaim arm
			 *    below does.  range->node sits on the context's
			 *    own list (ctx->all).  The compactor thread
			 *    mutates that list under the per-arena locks of
			 *    OTHER arenas.  The unlink would corrupt it.
			 *
			 *  - Pushing the range onto partial_ranges.
			 *    Unrelated allocations would then land INSIDE
			 *    the private range.
			 *    cds_ft_metadata_in_recompact_private would
			 *    misclassify them as already-relocated.
			 *
			 * So thread the slot on the range's own free list
			 * only.  ft_recompact_alloc_merge restores the
			 * partial/reclaim invariants when the range becomes
			 * ordinary.
			 */
			metadata_alloc->free_list_next = range->free_list_head;
			range->free_list_head = metadata_alloc;
			pthread_mutex_unlock(&arena->lock);
			return;
		}
		if (range->nr_live == 0 &&
				range->next_unused == arena->max_nr_items_per_range) {
			/*
			 * Range was filled and is now fully drained: reclaim it.
			 * It still sits on partial_ranges via its earlier-freed
			 * slots (free_list_head != NULL), unless it held a single
			 * item.  Unlink it from both lists under the lock so it is
			 * unreachable, then return its pages outside the lock.  The
			 * slot being freed is discarded with the range, not pushed.
			 */
			if (range->free_list_head)
				cds_list_del(&range->partial_node);
			cds_list_del(&range->node);
			reclaim = true;
		} else {
			bool was_empty = (range->free_list_head == NULL);

			metadata_alloc->free_list_next = range->free_list_head;
			range->free_list_head = metadata_alloc;
			/*
			 * Keep partial_ranges MRU-ordered so the next allocation
			 * reuses this hot just-freed slot.  partial_node is on the
			 * list iff free_list_head != NULL: add it if the range had
			 * no freed slots before, otherwise move it to the head.
			 */
			if (was_empty)
				cds_list_add(&range->partial_node, &arena->partial_ranges);
			else
				cds_list_move(&range->partial_node, &arena->partial_ranges);
		}
		pthread_mutex_unlock(&arena->lock);
		if (reclaim)
			ft_arena_reclaim_range(arena, range);
	}
#endif
}

/*
 * Node-allocation reserve.  See struct cds_ft_alloc_reserve and the API
 * docstrings in fractal-trie-internal.h.
 *
 * The fill uses the low-level alloc entries (cds_ft_alloc_item /
 * cds_ft_alloc_compressed_item).  So the items are NOT leak-counted here:
 * the count lives in alloc_cds_ft_node.  This keeps the debug node
 * accounting balanced:
 *  - a drawn-and-used item is counted once, in alloc_cds_ft_node;
 *  - a drained (unused) item is freed uncounted.
 */
int cds_ft_alloc_reserve_add(struct cds_ft *ft, struct cds_ft_alloc_reserve *r,
		enum cds_ft_alloc_kind kind, size_t item_len_order, bool bitmap,
		unsigned int n)
{
	unsigned int i;

	assert(kind == CDS_FT_ALLOC_KIND_NODE ||
		kind == CDS_FT_ALLOC_KIND_COMPRESSED);
	assert(item_len_order <= FT_ALLOC_ORDER_MAX);
	/* Fill before activate: these are real (fallible) allocations. */
	assert(!ft_tls_reserve_for(ft));
	assert(r->count[kind][item_len_order] + n <= CDS_FT_ALLOC_RESERVE_CAP);
	for (i = 0; i < n; i++) {
		struct cds_ft_metadata *m;

		if (kind == CDS_FT_ALLOC_KIND_COMPRESSED)
			m = cds_ft_alloc_compressed_item(ft, item_len_order);
		else
			m = cds_ft_alloc_item(ft, item_len_order, bitmap);
		if (!m)
			return -ENOMEM;
		r->items[kind][item_len_order][r->count[kind][item_len_order]++] = m;
	}
	return 0;
}

#ifndef FEATURE_FT_COMPRESS
int cds_ft_alloc_reserve_add_branch(struct cds_ft *ft,
		struct cds_ft_alloc_reserve *r, size_t item_len_order, bool bitmap,
		unsigned int n)
{
	unsigned int i;

	assert(item_len_order <= FT_ALLOC_ORDER_MAX);
	assert(!ft_tls_reserve_for(ft));
	assert(!r->branch_count || r->branch_order == item_len_order);
	assert(r->branch_count + n <= CDS_FT_ALLOC_RESERVE_BRANCH_CAP);
	r->branch_order = item_len_order;
	for (i = 0; i < n; i++) {
		struct cds_ft_metadata *m;

		m = cds_ft_alloc_item(ft, item_len_order, bitmap);
		if (!m)
			return -ENOMEM;
		r->branch[r->branch_count++] = m;
	}
	return 0;
}
#endif

void cds_ft_alloc_reserve_activate(struct cds_ft *ft,
		struct cds_ft_alloc_reserve *r)
{
	assert(!ft_tls_reserve_for(ft));	/* a trie: activated at most once */
	assert(ft_tls_reserve_nr < FT_TLS_RESERVE_MAX);
	ft_tls_reserves[ft_tls_reserve_nr].ft = ft;
	ft_tls_reserves[ft_tls_reserve_nr].r = r;
	ft_tls_reserve_nr++;
}

void cds_ft_alloc_reserve_deactivate(struct cds_ft *ft)
{
	unsigned int i;

	for (i = 0; i < ft_tls_reserve_nr; i++) {
		if (ft_tls_reserves[i].ft == ft) {
			/* Order-independent removal (dst/tmp deactivate in any order). */
			ft_tls_reserves[i] = ft_tls_reserves[--ft_tls_reserve_nr];
			ft_tls_reserves[ft_tls_reserve_nr].ft = NULL;
			ft_tls_reserves[ft_tls_reserve_nr].r = NULL;
			return;
		}
	}
	assert(0);	/* deactivate without a matching activate */
}

#ifdef FT_ENABLE_TRACING
/* The fast-stop flag itself; see FT_TRACE_FREEZE in fractal-trie-trace.h. */
int ft_trace_frozen;
#endif

#ifdef FT_ENABLE_TRACING
/*
 * The reclaim ROUTE, so cds_ft_do_free_item can say which path pushed an item
 * back on the freelist.  Normally maintained by FT_DEBUG_CLIMB_AUDIT; tracing
 * needs it independently, and the two must not define it twice.
 */
__thread unsigned long ft_dbg_free_via;
#endif


void cds_ft_alloc_reserve_drain(struct cds_ft *ft,
		struct cds_ft_alloc_reserve *r)
{
	unsigned int k, o, i;

	assert(!ft_tls_reserve_for(ft));	/* deactivate before drain */
	(void) ft;
#ifdef FT_ENABLE_TRACING
	ft_dbg_free_via = FT_DBG_VIA_DRAIN;
#endif
	for (k = 0; k < CDS_FT_ALLOC_RESERVE_NR_KIND; k++) {
		for (o = 0; o <= FT_ALLOC_ORDER_MAX; o++) {
			for (i = 0; i < r->count[k][o]; i++) {
				cds_ft_do_free_item(r->items[k][o][i]);
			}
			r->count[k][o] = 0;
		}
	}
#ifndef FEATURE_FT_COMPRESS
	for (i = 0; i < r->branch_count; i++)
		cds_ft_do_free_item(r->branch[i]);
	r->branch_count = 0;
#endif
}

#ifdef FT_DEBUG_TOMBSTONE_AUDIT
unsigned long ft_unpub_free_calls;
#endif


static
void cds_ft_free_item_rcu(struct rcu_head *rcu_head)
{
	struct cds_ft_metadata_alloc *metadata_alloc =
		caa_container_of(rcu_head, struct cds_ft_metadata_alloc, rcu_head);
	cds_ft_do_free_item(&metadata_alloc->metadata);
}

/*
 * Release a metadata slot back to its arena.
 *
 * Concurrent-mode tries defer the actual freelist push by call_rcu
 * so concurrent RCU readers cannot dereference a slot the writer
 * has just unlinked.
 *
 * Exclusive-mode tries forbid concurrent readers.
 * cds_ft_make_exclusive() drains pre-existing readers via
 * synchronize_rcu before flipping the flag.  So the slot can be
 * pushed back synchronously, and the next allocation can reuse it
 * immediately.  This avoids the call_rcu round-trip.
 *
 * FT_IMMEDIATE_FREE testing mode poisons every slot in either
 * mode (see cds_ft_do_free_item).
 */
void cds_ft_free_item(struct cds_ft *ft, struct cds_ft_metadata *metadata)
{
#ifdef FT_IMMEDIATE_FREE
	/*
	 * Immediate free for use-after-free detection by mutation
	 * code.  Poisons metadata and node data so any subsequent
	 * access crashes deterministically.  Do NOT return to the
	 * free list.
	 *
	 * IMPORTANT: this mode is only safe for single-threaded
	 * mutation testing WITHOUT concurrent RCU readers.
	 *
	 * The item data poison would corrupt concurrent exact
	 * lookups (which only traverse item data and never touch
	 * metadata).  The metadata poison would additionally
	 * corrupt the concurrent paths that read metadata fields
	 * (parent, external_nodes, nr_keys) on the read-side:
	 *  - inequality lookups;
	 *  - iteration;
	 *  - skip-compressed traversal.
	 *
	 * Use this mode exclusively for validating that mutation
	 * paths do not access freed memory.
	 */
	(void) ft;
	cds_ft_do_free_item(metadata);
#else
	if (ft->exclusive) {
#ifdef FT_ENABLE_TRACING
		ft_dbg_free_via = FT_DBG_VIA_EXCLUSIVE;
		cds_ft_do_free_item(metadata);
		ft_dbg_free_via = 0;
#else
		cds_ft_do_free_item(metadata);
#endif
	} else {
		struct cds_ft_metadata_alloc *metadata_alloc =
			caa_container_of(metadata, struct cds_ft_metadata_alloc, metadata);
		struct cds_ft_alloc_range *range =
			cds_ft_metadata_to_range(metadata);
		struct cds_ft_alloc_arena *arena = range->arena;
		const struct rcu_flavor_struct *flavor = arena->ft_group->flavor;

		flavor->update_call_rcu(&metadata_alloc->rcu_head, cds_ft_free_item_rcu);
	}
#endif
}

#ifndef FT_IMMEDIATE_FREE
/*
 * Give an UNPUBLISHED item back to this thread's active reserve for @ft.
 * An attempt that drew from the reserve and then aborted returns what it
 * drew this way.  Return false when the item does not belong in a reserve
 * bucket; the caller then frees it to the arena.
 *
 * THE RESERVE'S OTHER HALF.  ft_bulk_node_reserve_fill sizes a reserve to a
 * superset of what ONE attempt at a bulk op's commit allocates.  The draw
 * path above treats an empty bucket as an under-counted manifest.  But the
 * ops that draw from a reserve RETRY.  On every contention bail,
 * ft_graft_keylen:
 *  - re-descends from retry_attach;
 *  - re-allocates that attempt's nodes: the fresh source root, and a whole
 *    glue cluster for a diverge shape.
 *
 * It does so after freeing the previous attempt's nodes to the ARENA.
 *
 * Each round therefore shrank the reserve by one attempt's worth.  Enough
 * rounds exhausted it, however generous the fill was.  This was measured
 * as a kind=0 order=5 underflow (the fresh source root), once a contended
 * same-trie rekey reached its ninth round.
 *
 * A refund makes an aborted attempt reserve-NEUTRAL.  One attempt's
 * manifest then bounds a whole retry loop.  No constant can provide that
 * property.
 *
 * The refund is SAFE: the item was never published, so no reader can reach
 * it and this thread still owns it.
 *
 * It is also cheaper than the free/alloc round trip it replaces.  The item
 * goes straight back to the bucket it came from, with:
 *  - no arena lock;
 *  - no grace period;
 *  - no change to range->nr_live.
 *
 * The slot stays live because it stays ours.  It is accounted exactly once,
 * when it is finally freed (by a later use, or by
 * cds_ft_alloc_reserve_drain).
 *
 * The item is CLEARED the way the arena's freelist-reuse path clears a
 * recycled slot: body, metadata with alloc_index preserved, reverse bitmap.
 * The reason is that the draw path hands items out as they are:
 *  - a reserve filled by cds_ft_alloc_reserve_add holds untouched arena
 *    items;
 *  - alloc_cds_ft_node's callers rely on the allocator returning zeroed
 *    memory.
 *
 * A used item is not zero.  Refunding one without the clear would hand the
 * next draw a dirty node.
 *
 * The item's ARENA names its bucket exactly: there is one arena per (kind,
 * order).  That lookup is also what rejects everything a reserve never
 * holds:
 *  - ordinal cells;
 *  - any item not from @ft's group's node arenas.
 *
 * The lazy arena create above publishes those two group slots with a
 * release store, so they are read as atomics here.  A relaxed load is
 * enough because this only compares pointer identity.  The comparison can
 * only come out wrong when a slot is still read as NULL.  That case falls
 * through to the plain arena free, which is what the caller did before this
 * refund existed.
 */
static
bool ft_alloc_reserve_refund(struct cds_ft *ft, struct cds_ft_metadata *metadata)
{
	struct cds_ft_alloc_reserve *r;
	struct cds_ft_metadata_alloc *item;
	struct cds_ft_alloc_arena *arena;
	enum cds_ft_alloc_kind kind;
	size_t order, alloc_index;
	unsigned int *cnt;
	bool full;
	void *p;

	if (!ft_tls_reserve_nr)
		return false;
	r = ft_tls_reserve_for(ft);
	if (!r)
		return false;
	arena = cds_ft_metadata_to_range(metadata)->arena;
	order = arena->item_len_order;
	if (order > FT_ALLOC_ORDER_MAX)
		return false;
	if (arena == uatomic_load(&ft->group->arena_order[order], CMM_RELAXED))
		kind = CDS_FT_ALLOC_KIND_NODE;
	else if (arena == uatomic_load(&ft->group->compressed_arena_order[order],
			CMM_RELAXED))
		kind = CDS_FT_ALLOC_KIND_COMPRESSED;
	else
		return false;		/* a cell, or not this group's node arena */
	cnt = &r->count[kind][order];
	full = *cnt >= CDS_FT_ALLOC_RESERVE_CAP;
#ifndef FEATURE_FT_COMPRESS
	/* A long branch drew past the bucket: its excess goes back to @branch. */
	if (full && (kind != CDS_FT_ALLOC_KIND_NODE || order != r->branch_order
			|| r->branch_count >= CDS_FT_ALLOC_RESERVE_BRANCH_CAP))
		return false;
#else
	if (full)
		return false;		/* bucket full: the arena takes it back */
#endif
	item = caa_container_of(metadata, struct cds_ft_metadata_alloc, metadata);
	p = cds_ft_metadata_to_item(metadata);
	alloc_index = metadata->alloc_index;
	memset(p, 0, 1UL << order);
	ft_clear_recycled_slot(arena, item, alloc_index);
#ifndef FEATURE_FT_COMPRESS
	if (full) {
		r->branch[r->branch_count++] = metadata;
		return true;
	}
#endif
	r->items[kind][order][(*cnt)++] = metadata;
	return true;
}
#endif	/* !FT_IMMEDIATE_FREE */

/*
 * Immediate-free path for items that were never published -- no reader
 * can hold a reference, so call_rcu would only delay arena reuse.
 * Always routes through the synchronous body, regardless of exclusive
 * mode or FT_IMMEDIATE_FREE configuration.  See declaration in
 * fractal-trie-internal.h for the safety contract.
 */
void cds_ft_free_item_unpublished(struct cds_ft *ft __attribute__((unused)),
		struct cds_ft_metadata *metadata)
{
#ifdef FT_DEBUG_TOMBSTONE_AUDIT
	/*
	 * Count the calls.  A green is then a claim about the CODE, not
	 * about whether this path runs at all.  An assert that nothing
	 * reaches is not coverage.
	 */
	uatomic_inc(&ft_unpub_free_calls);
	/*
	 * THE OTHER HALF of free_cds_ft_node's freeze-on-free guard.  That
	 * guard asserts that everything reaching the DEFERRED path carries
	 * the one-way tombstone.  It states the converse in prose only:
	 * "abandoned fresh (never-reader-visible) nodes use
	 * free_cds_ft_node_unpublished and do not reach here."  That
	 * converse was never checked.
	 *
	 * The tombstone is set only by a retire, i.e. only on a node that
	 * WAS published.  So this path's contract excludes a tombstoned
	 * item: "items that were never published, no reader can hold a
	 * reference".
	 *
	 * Freeing a tombstoned item immediately goes wrong in three steps:
	 *  - its metadata returns to the range freelist, under readers and
	 *    writers that may still hold it;
	 *  - free_list_next then lands on parent_word;
	 *  - an ancestor climb later dereferences parent_word.
	 *
	 * This is asserted rather than tolerated because the CALLER chooses
	 * the immediate path.  Nothing else can catch a site that picks it
	 * for a node it has already published.
	 */
	assert(!ft_meta_tombstone(metadata));
#endif
#ifndef FT_IMMEDIATE_FREE
	/*
	 * An attempt that drew this item from a reserve gets it back.  A
	 * retry loop then cannot drain the reserve that is supposed to cover
	 * it.  Excluded under FT_IMMEDIATE_FREE, whose whole point is that a
	 * freed slot is poisoned and never reused.
	 */
	if (ft_alloc_reserve_refund(ft, metadata))
		return;
#endif
#ifdef FT_ENABLE_TRACING
	ft_dbg_free_via = FT_DBG_VIA_UNPUB;
	cds_ft_do_free_item(metadata);
	ft_dbg_free_via = 0;
#else
	cds_ft_do_free_item(metadata);
#endif
}

/*
 * Always-deferred free for the compactor: routes through call_rcu even on
 * an EXCLUSIVE trie.  The compactor must never free synchronously:
 *  - the compaction step keeps navigating relative to nodes it has just
 *    unpublished;
 *  - the exclusive-mode synchronous free threads the freelist link
 *    through the freed slot immediately.
 *
 * (FT_IMMEDIATE_FREE testing mode keeps its poison-now behavior, like
 * cds_ft_free_item.)
 */
/*
 * ☠ THE LEAK BALANCE IS THIS FUNCTION'S JOB, NOT ITS CALLERS'.
 *
 * The counted free paths are WRAPPERS: free_cds_ft_node and
 * free_compressed_node bump the balance around cds_ft_free_item.  This
 * entry has no wrapper.  So for a long time each caller was expected to
 * bump the balance by hand.  Three of the five did not:
 *  - the rekey's old S_top;
 *  - the old root;
 *  - one compaction site.
 *
 * This made cds_ft_group_destroy report a leak of EXACTLY ONE NODE PER
 * SERVED REKEY.  The node was allocated, genuinely reclaimed, and never
 * counted.  It cost a full flight-recorder hunt to find that the node in
 * the report was not lost at all.
 *
 * A balance nobody owns drifts, so this owns it.  The arena knows which kind of
 * item it hands out, so the kind needs no parameter and no caller can get it
 * wrong.  ☞ The two callers that used to count themselves no longer do.
 */
static
void ft_free_item_account(struct cds_ft_metadata *metadata)
{
	struct cds_ft_alloc_range *range;
	struct cds_ft_alloc_arena *arena;
	struct cds_ft_group *group;

	/*
	 * Test the predicate FIRST.  This sits on the deferred-free hot
	 * path.  The range/arena/group walk is three dependent loads, and a
	 * build without counters must not pay for them.
	 */
	if (!ft_debug_counters())
		return;
	range = cds_ft_metadata_to_range(metadata);
	arena = range->arena;
	group = arena->ft_group;
	if (arena->cell) {
		uatomic_inc(&group->nr_cells_freed);
		return;
	}
	uatomic_inc(&group->nr_nodes_freed);
	if (arena->compressed)
		uatomic_inc(&group->nr_compressed_freed);
	else
		uatomic_inc(&group->nr_internal_freed);
}

void cds_ft_free_item_deferred(struct cds_ft *ft __attribute__((unused)),
		struct cds_ft_metadata *metadata)
{
	ft_free_item_account(metadata);
#ifdef FT_IMMEDIATE_FREE
	cds_ft_do_free_item(metadata);
#else
	{
	struct cds_ft_metadata_alloc *metadata_alloc =
		caa_container_of(metadata, struct cds_ft_metadata_alloc, metadata);
	struct cds_ft_alloc_range *range =
		cds_ft_metadata_to_range(metadata);
	struct cds_ft_alloc_arena *arena = range->arena;
	const struct rcu_flavor_struct *flavor = arena->ft_group->flavor;

	flavor->update_call_rcu(&metadata_alloc->rcu_head, cds_ft_free_item_rcu);
	}
#endif
}

void cds_ft_free_all_arenas(struct cds_ft_group *ft_group)
{
	int i;

	for (i = 0; i <= FT_ALLOC_ORDER_MAX; i++) {
		if (ft_group->arena_order[i]) {
			cds_ft_arena_destroy(ft_group->arena_order[i]);
			ft_group->arena_order[i] = NULL;
		}
		if (ft_group->compressed_arena_order[i]) {
			cds_ft_arena_destroy(ft_group->compressed_arena_order[i]);
			ft_group->compressed_arena_order[i] = NULL;
		}
	}
	if (ft_group->cell_arena) {
		cds_ft_arena_destroy(ft_group->cell_arena);
		ft_group->cell_arena = NULL;
	}
}

/*
 * External-node arena (public API): power-of-two size-class buddy
 * allocator backed by a linked list of mmap'd ranges.
 *
 * Each range is FT_EXT_ARENA_RANGE_SIZE bytes, aligned to its own
 * size (via over-mmap-and-trim) so range_base = item & ~(SIZE-1).
 * Each range starts with a 1 MiB per-cell metadata bitmap (1 byte /
 * 16-byte cell).  A byte says whether its cell is the start of an
 * allocated or free block, and at what order.
 * cds_ft_external_arena_free recovers the order from any item pointer
 * with one mask + one load.
 *
 * Cross-class buddy splitting, in alloc(O):
 *  - check freelist[O] first;
 *  - on miss, scan freelist[O+1..MAX] for a larger free block and
 *    pop it;
 *  - split it down to order O.  Each split pushes one smaller-order
 *    half onto the corresponding freelist.
 *
 * Cross-class buddy merging, in free(ptr):
 *  - read the block's order O from the cell map;
 *  - check its buddy, at offset ^ (1 << O);
 *  - if the buddy is also free at the same order, remove it from
 *    its freelist, mark one cell as "interior" and merge into an
 *    order-(O+1) block;
 *  - recurse upward.
 *
 * The trailing FT_EXT_ARENA_GUARD_SIZE bytes of every range are
 * reserved as a never-allocated guard.  The library's 32-byte SIMD
 * leaf compare can then safely over-read past the last
 * bump-allocated slot in the range.
 */

#define FT_EXT_ARENA_MIN_ORDER		4	/* 16 B minimum slot */
#define FT_EXT_ARENA_MAX_ORDER		22	/* 4 MiB maximum slot */
#define FT_EXT_ARENA_NR_CLASSES					\
	(FT_EXT_ARENA_MAX_ORDER - FT_EXT_ARENA_MIN_ORDER + 1)
#define FT_EXT_ARENA_RANGE_ORDER	24	/* 16 MiB per range */
#define FT_EXT_ARENA_RANGE_SIZE		(1UL << FT_EXT_ARENA_RANGE_ORDER)
#define FT_EXT_ARENA_RANGE_MASK		(FT_EXT_ARENA_RANGE_SIZE - 1)
#define FT_EXT_ARENA_GUARD_SIZE		4096UL
#define FT_EXT_ARENA_MIN_SLOT_SIZE	(1UL << FT_EXT_ARENA_MIN_ORDER)
#define FT_EXT_ARENA_NUM_CELLS						\
	(FT_EXT_ARENA_RANGE_SIZE / FT_EXT_ARENA_MIN_SLOT_SIZE)
/*
 * Cell sentinel for "not a block start" (interior of a larger block, or
 * unallocated region).
 *
 * Real cell values encode:
 *  - bit 0 = free;
 *  - bits 1-7 = (order - MIN_ORDER + 1).
 *
 * The +1 reserves the all-zero value as the sentinel.  A freshly mmap'd
 * range then reads CELL_NONE everywhere WITHOUT an eager memset.  The
 * 1 MiB cells[] of a 16 MiB range faults lazily: only the cells the
 * allocator touches.  Real values are therefore >= 2; value 1 is an unused
 * hole (uint8_t has ample room).
 */
#define FT_EXT_ARENA_CELL_NONE		0x00

static inline
uint8_t ft_ext_arena_cell_pack(int order, int is_free)
{
	return (uint8_t) ((((order) - FT_EXT_ARENA_MIN_ORDER + 1) << 1) |
			((is_free) ? 1 : 0));
}

static inline
int ft_ext_arena_cell_order(uint8_t c)
{
	return (int) (((c >> 1) & 0x7F) - 1) + FT_EXT_ARENA_MIN_ORDER;
}

static inline
int ft_ext_arena_cell_is_free(uint8_t c)
{
	return c & 0x01;
}

/* Doubly-linked freelist node -- fits in 16 B (MIN_SLOT_SIZE). */
struct cds_ft_external_arena_freenode {
	struct cds_ft_external_arena_freenode *next;
	struct cds_ft_external_arena_freenode *prev;
};

struct cds_ft_external_arena_range {
	struct cds_list_head node;		/* per-arena list */
	size_t bump;				/* next un-bumped byte offset */
	uint8_t cells[FT_EXT_ARENA_NUM_CELLS];	/* per-cell block metadata */
	/*
	 * Slots start at the first MIN_SLOT-aligned offset past
	 * sizeof(struct cds_ft_external_arena_range).
	 */
};

struct cds_ft_external_arena {
	pthread_mutex_t lock;
	struct cds_list_head ranges;
	struct cds_ft_external_arena_range *active_range;
	struct cds_ft_external_arena_freenode *freelist[FT_EXT_ARENA_NR_CLASSES];
	int huge;	/* THP policy for ranges: from the create-time cds_ft_optimize. */
};

/* External-arena attributes (cds_ft_external_arena_attr_*). */
struct cds_ft_external_arena_attr {
	enum cds_ft_optimize optimize;
};

enum cds_ft_status cds_ft_external_arena_attr_create(
		struct cds_ft_external_arena_attr **attr)
{
	struct cds_ft_external_arena_attr *a = calloc(1, sizeof(*a));

	if (!a)
		return CDS_FT_STATUS_MEMORY_ERROR;
	a->optimize = CDS_FT_OPTIMIZE_THROUGHPUT;
	*attr = a;
	return CDS_FT_STATUS_OK;
}

void cds_ft_external_arena_attr_destroy(struct cds_ft_external_arena_attr *attr)
{
	free(attr);
}

enum cds_ft_status cds_ft_external_arena_attr_set_optimize(
		struct cds_ft_external_arena_attr *attr, enum cds_ft_optimize opt)
{
	switch (opt) {
	case CDS_FT_OPTIMIZE_THROUGHPUT:
	case CDS_FT_OPTIMIZE_RSS:
		attr->optimize = opt;
		return CDS_FT_STATUS_OK;
	}
	return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
}

static
int ft_ext_arena_order_for_size(size_t size)
{
	int o;

	if (size <= (1UL << FT_EXT_ARENA_MIN_ORDER))
		return FT_EXT_ARENA_MIN_ORDER;
	/* Smallest order with (1 << order) >= size. */
	o = 64 - __builtin_clzll((unsigned long long)(size - 1));
	return o;
}

static inline
size_t ft_ext_arena_offset_in_range(const void *ptr)
{
	return (uintptr_t) ptr & FT_EXT_ARENA_RANGE_MASK;
}

static inline
struct cds_ft_external_arena_range *ft_ext_arena_range_of(const void *ptr)
{
	return (struct cds_ft_external_arena_range *)
		((uintptr_t) ptr & ~(uintptr_t) FT_EXT_ARENA_RANGE_MASK);
}

static inline
size_t ft_ext_arena_cell_index(const void *ptr)
{
	return ft_ext_arena_offset_in_range(ptr) / FT_EXT_ARENA_MIN_SLOT_SIZE;
}

static
struct cds_ft_external_arena_range *
ft_ext_arena_range_create(int huge)
{
	void *raw, *aligned;
	uintptr_t pre, post;
	size_t raw_size = 2 * FT_EXT_ARENA_RANGE_SIZE;
	struct cds_ft_external_arena_range *r;

	raw = mmap(NULL, raw_size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (raw == MAP_FAILED)
		return NULL;
	aligned = (void *)(((uintptr_t) raw + FT_EXT_ARENA_RANGE_MASK) &
			~(uintptr_t) FT_EXT_ARENA_RANGE_MASK);
	pre = (uintptr_t) aligned - (uintptr_t) raw;
	post = raw_size - pre - FT_EXT_ARENA_RANGE_SIZE;
	if (pre)
		(void) munmap(raw, pre);
	if (post)
		(void) munmap((char *) aligned + FT_EXT_ARENA_RANGE_SIZE, post);
	/*
	 * External arena isn't tied to a specific group, so it uses
	 * CDS_FT_NUMA_DEFAULT.  ft_apply_interleave then looks at the
	 * process policy:
	 *  - MPOL_DEFAULT (no process policy): interleave the range.
	 *  - MPOL_INTERLEAVE (typical numactl --interleave wrapper):
	 *    promote it to 2 MiB-chunk MPOL_BIND for NUMA-local
	 *    contiguous spans.
	 *  - LOCAL/PREFERRED/BIND: no mbind.  The kernel honors the
	 *    process choice at fault time.
	 */
	ft_apply_interleave(aligned, FT_EXT_ARENA_RANGE_SIZE,
			CDS_FT_NUMA_DEFAULT);
	/* THP per the arena's create-time cds_ft_optimize; see ft_apply_thp_policy. */
	ft_apply_thp_policy(aligned, FT_EXT_ARENA_RANGE_SIZE, huge);

	r = (struct cds_ft_external_arena_range *) aligned;
	CDS_INIT_LIST_HEAD(&r->node);
	/*
	 * No cells[] memset.  FT_EXT_ARENA_CELL_NONE is 0, and the fresh
	 * mmap is already zero-filled.  So every cell (header region,
	 * unbumped tail) reads "not a block start".  The 1 MiB cells[] array
	 * faults lazily: only the cells the allocator touches.
	 *
	 * First slot offset rounds the struct size up to MIN_SLOT_SIZE so
	 * cell-index math is uniform.
	 */
	r->bump = (sizeof(*r) + FT_EXT_ARENA_MIN_SLOT_SIZE - 1)
			& ~(FT_EXT_ARENA_MIN_SLOT_SIZE - 1);
	return r;
}

static inline
void ft_ext_arena_freelist_push(struct cds_ft_external_arena *a,
		int order, struct cds_ft_external_arena_freenode *fn)
{
	int idx = order - FT_EXT_ARENA_MIN_ORDER;
	struct cds_ft_external_arena_freenode *head = a->freelist[idx];

	fn->next = head;
	fn->prev = NULL;
	if (head)
		head->prev = fn;
	a->freelist[idx] = fn;
}

static inline
void ft_ext_arena_freelist_remove(struct cds_ft_external_arena *a,
		int order, struct cds_ft_external_arena_freenode *fn)
{
	int idx = order - FT_EXT_ARENA_MIN_ORDER;

	if (fn->prev)
		fn->prev->next = fn->next;
	else
		a->freelist[idx] = fn->next;
	if (fn->next)
		fn->next->prev = fn->prev;
}

static inline
struct cds_ft_external_arena_freenode *
ft_ext_arena_freelist_pop(struct cds_ft_external_arena *a, int order)
{
	int idx = order - FT_EXT_ARENA_MIN_ORDER;
	struct cds_ft_external_arena_freenode *fn = a->freelist[idx];

	if (fn)
		ft_ext_arena_freelist_remove(a, order, fn);
	return fn;
}

static
void *ft_ext_arena_bump_alloc(struct cds_ft_external_arena *a, int order)
{
	struct cds_ft_external_arena_range *r;
	size_t slot_size = 1UL << order;
	size_t aligned_bump;
	void *p;

	if (!a->active_range) {
		r = ft_ext_arena_range_create(a->huge);
		if (!r)
			return NULL;
		cds_list_add(&r->node, &a->ranges);
		a->active_range = r;
	}
	r = a->active_range;
	aligned_bump = (r->bump + slot_size - 1) & ~(slot_size - 1);
	if (aligned_bump + slot_size + FT_EXT_ARENA_GUARD_SIZE
			> FT_EXT_ARENA_RANGE_SIZE) {
		/* Doesn't fit; start a fresh range. */
		r = ft_ext_arena_range_create(a->huge);
		if (!r)
			return NULL;
		cds_list_add(&r->node, &a->ranges);
		a->active_range = r;
		aligned_bump = (r->bump + slot_size - 1) & ~(slot_size - 1);
		if (aligned_bump + slot_size + FT_EXT_ARENA_GUARD_SIZE
				> FT_EXT_ARENA_RANGE_SIZE)
			return NULL;	/* even a fresh range too small */
	}
	p = (char *) r + aligned_bump;
	r->bump = aligned_bump + slot_size;
	return p;
}

struct cds_ft_external_arena *cds_ft_external_arena_create(
		const struct cds_ft_external_arena_attr *attr)
{
	enum cds_ft_optimize optimize = attr ? attr->optimize :
			CDS_FT_OPTIMIZE_THROUGHPUT;
	struct cds_ft_external_arena *a;
	int i;

	if (!uatomic_load(&cds_ft_page_size, CMM_RELAXED))
		uatomic_store(&cds_ft_page_size, urcu_get_page_len(),
				CMM_RELAXED);
	a = calloc(1, sizeof(*a));
	if (!a)
		return NULL;
	pthread_mutex_init(&a->lock, NULL);
	CDS_INIT_LIST_HEAD(&a->ranges);
	a->active_range = NULL;
	a->huge = (optimize == CDS_FT_OPTIMIZE_THROUGHPUT);
	for (i = 0; i < FT_EXT_ARENA_NR_CLASSES; i++)
		a->freelist[i] = NULL;
	return a;
}

void *cds_ft_external_arena_alloc(struct cds_ft_external_arena *a,
		size_t size)
{
	int target_order, cur;
	void *p = NULL;
	struct cds_ft_external_arena_range *r;

	if (!a || !size)
		return NULL;
	target_order = ft_ext_arena_order_for_size(size);
	if (target_order > FT_EXT_ARENA_MAX_ORDER)
		return NULL;
	if (target_order < FT_EXT_ARENA_MIN_ORDER)
		target_order = FT_EXT_ARENA_MIN_ORDER;

	pthread_mutex_lock(&a->lock);
	/* Scan freelists from target order upward. */
	for (cur = target_order; cur <= FT_EXT_ARENA_MAX_ORDER; cur++) {
		if (a->freelist[cur - FT_EXT_ARENA_MIN_ORDER]) {
			p = ft_ext_arena_freelist_pop(a, cur);
			break;
		}
	}
	if (!p) {
		/* No freelist; bump from active range (creating one if needed). */
		p = ft_ext_arena_bump_alloc(a, target_order);
		cur = target_order;
		if (!p)
			goto unlock;
	}
	/* Split @p (currently at order @cur) down to @target_order. */
	while (cur > target_order) {
		size_t lower_off = ft_ext_arena_offset_in_range(p);
		size_t upper_off = lower_off + (1UL << (cur - 1));
		struct cds_ft_external_arena_range *rr = ft_ext_arena_range_of(p);
		struct cds_ft_external_arena_freenode *upper =
			(struct cds_ft_external_arena_freenode *)
			((char *) rr + upper_off);

		cur--;
		rr->cells[upper_off / FT_EXT_ARENA_MIN_SLOT_SIZE] =
			ft_ext_arena_cell_pack(cur, 1);
		ft_ext_arena_freelist_push(a, cur, upper);
	}
	r = ft_ext_arena_range_of(p);
	r->cells[ft_ext_arena_cell_index(p)] = ft_ext_arena_cell_pack(target_order, 0);
unlock:
	pthread_mutex_unlock(&a->lock);
	if (p)
		memset(p, 0, 1UL << target_order);
	return p;
}

void cds_ft_external_arena_free(struct cds_ft_external_arena *a, void *ptr)
{
	struct cds_ft_external_arena_range *r;
	int order;
	uint8_t cell;

	if (!a || !ptr)
		return;
	r = ft_ext_arena_range_of(ptr);
	/*
	 * Read the block's cell metadata before taking a->lock.  cells[] is
	 * otherwise mutated only under the lock.
	 *
	 * The unlocked read is safe because the cell that starts an
	 * allocated block is immutable until that block is freed:
	 *  - buddy merge only combines free blocks (the merge guard below
	 *    requires the buddy be free);
	 *  - split / alloc only write the cell of the block they carve or
	 *    hand out, never an allocated block's start cell.
	 *
	 * The caller owns @ptr's still-allocated block.  Freeing an unowned
	 * or already-freed pointer is caller misuse, caught by the assert
	 * below.  So no other thread can be writing this cell concurrently,
	 * and the order decoded here is still valid once the lock is held.
	 */
	cell = r->cells[ft_ext_arena_cell_index(ptr)];
	order = ft_ext_arena_cell_order(cell);
	/*
	 * Guard against freeing a pointer this arena never handed out, or a
	 * double free:
	 *  - An unallocated cell is FT_EXT_ARENA_CELL_NONE (0).  It decodes
	 *    to order 3, below FT_EXT_ARENA_MIN_ORDER.  The freelist index
	 *    (order - FT_EXT_ARENA_MIN_ORDER) would be -1, and the push
	 *    below would scribble freelist[-1].
	 *  - An already-free cell has the is_free bit set.
	 *
	 * The assert catches either as a diagnosable abort instead of
	 * silently corrupting the heap.  It is an assert only: a build with
	 * NDEBUG has no such guard.
	 */
	assert(order >= FT_EXT_ARENA_MIN_ORDER &&
			order <= FT_EXT_ARENA_MAX_ORDER &&
			!ft_ext_arena_cell_is_free(cell));

	pthread_mutex_lock(&a->lock);
	/* Try to merge upward with the buddy at each order. */
	while (order < FT_EXT_ARENA_MAX_ORDER) {
		size_t off = ft_ext_arena_offset_in_range(ptr);
		size_t buddy_off = off ^ (1UL << order);
		uint8_t buddy_cell;
		struct cds_ft_external_arena_freenode *buddy;
		void *merged;
		void *cleared;

		buddy_cell = r->cells[buddy_off / FT_EXT_ARENA_MIN_SLOT_SIZE];
		if (buddy_cell != ft_ext_arena_cell_pack(order, 1))
			break;
		buddy = (struct cds_ft_external_arena_freenode *)
			((char *) r + buddy_off);
		ft_ext_arena_freelist_remove(a, order, buddy);
		merged = (off < buddy_off) ? ptr : (void *) buddy;
		cleared = (off < buddy_off) ? (void *) buddy : ptr;
		r->cells[ft_ext_arena_cell_index(cleared)] = FT_EXT_ARENA_CELL_NONE;
		ptr = merged;
		order++;
	}
	r->cells[ft_ext_arena_cell_index(ptr)] = ft_ext_arena_cell_pack(order, 1);
	ft_ext_arena_freelist_push(a, order,
		(struct cds_ft_external_arena_freenode *) ptr);
	pthread_mutex_unlock(&a->lock);
}

void cds_ft_external_arena_destroy(struct cds_ft_external_arena *a)
{
	struct cds_ft_external_arena_range *r, *tmp;

	if (!a)
		return;
	cds_list_for_each_entry_safe(r, tmp, &a->ranges, node) {
		cds_list_del(&r->node);
		(void) munmap(r, FT_EXT_ARENA_RANGE_SIZE);
	}
	pthread_mutex_destroy(&a->lock);
	free(a);
}


#ifdef FT_RED_REKEY_NOLOCK
unsigned long ft_red_rekey_nolock_taken;

__attribute__((destructor))
static void ft_red_rekey_nolock_report(void)
{
	fprintf(stderr, "# FT_RED_REKEY_NOLOCK scopes_skipped=%lu\n",
		uatomic_load(&ft_red_rekey_nolock_taken, CMM_RELAXED));
}
#endif

#ifdef FT_DEBUG_BULK_ELEV
unsigned long ft_bulk_elev_self;
unsigned long ft_bulk_elev_peer;
unsigned long ft_bulk_take_self;
unsigned long ft_bulk_take_peer;
unsigned long ft_bulk_gate_calls;
unsigned long ft_bulk_scope_under_gate;

__attribute__((destructor))
static void ft_bulk_elev_report(void)
{
	fprintf(stderr, "# FT_DEBUG_BULK_ELEV elev self=%lu peer=%lu  "
		"wide_lock_taken self=%lu peer=%lu\n",
		uatomic_load(&ft_bulk_elev_self, CMM_RELAXED),
		uatomic_load(&ft_bulk_elev_peer, CMM_RELAXED),
		uatomic_load(&ft_bulk_take_self, CMM_RELAXED),
		uatomic_load(&ft_bulk_take_peer, CMM_RELAXED));
	fprintf(stderr, "# FT_DEBUG_BULK_ELEV gate_calls=%lu "
		"outer_scopes_under_own_gate=%lu\n",
		uatomic_load(&ft_bulk_gate_calls, CMM_RELAXED),
		uatomic_load(&ft_bulk_scope_under_gate, CMM_RELAXED));
}
#endif
