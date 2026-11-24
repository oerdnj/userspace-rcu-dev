// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _URCU_RCU_TXN_SW_H
#define _URCU_RCU_TXN_SW_H

/*
 * Single-updater transaction: atomically switch a *set* of pointers from an
 * "old" value to a "new" value with a single store, as observed by concurrent
 * RCU readers.
 *
 * Motivation
 * ----------
 * A structural RCU mutation often needs to re-point many slots (e.g. the
 * back-pointers of a set of live nodes being re-parented) such that a
 * reader never observes a partially-updated set.  Re-pointing the slots
 * one by one exposes intermediate states: a reader walking several of
 * them sees a mix of old and new targets.  The usual remedy is a
 * grace-period drain around the update, which is costly.
 *
 * The latch flip removes the intermediate states by one level of
 * indirection.  Each slot in the set is made to hold a tagged pointer to
 * a small "proxy" latch instead of the target directly.  A proxy holds
 * {old_ptr, new_ptr} and a pointer to a shared "flip group" carrying a
 * boolean selector word.  Resolving a proxy returns old_ptr while the
 * selector is 0 and new_ptr once it is 1.  Because every proxy in a group
 * reads the *same* selector, a single store flipping that selector
 * switches the whole set atomically: at any instant every proxy resolves
 * consistently to the old or the new target.
 *
 * The embedding data structure is responsible for:
 *   - tagging a proxy pointer so its readers recognise it (e.g. a spare
 *     low/high bit, or a reserved type code in an already-tagged pointer)
 *     and routing proxy resolution through urcu_txn_sw_proxy_get();
 *   - allocating proxies and the group (often a single backing block with
 *     one rcu_head), with whatever alignment its tagging scheme needs;
 *   - reclaiming them with call_rcu() after they are unpublished (every
 *     slot rewritten from the tagged proxy to the resolved target).
 *
 * Lifecycle (writer)
 * ------------------
 *   1. Build the new structure invisibly.
 *   2. urcu_txn_sw_group_init(group); for each slot, init a proxy with its
 *      {old, new} target and point the slot at the tagged proxy.  The
 *      selector is 0, so this is transparent to readers (they still
 *      resolve to old).
 *   3. urcu_txn_sw_group_commit(group): one release store, 0 -> 1.  Every proxy
 *      now resolves to new, atomically.
 *   4. "Settle": rewrite each slot from the tagged proxy back to the
 *      direct new target (idempotent for readers, since the proxy already
 *      resolves to new), then call_rcu() the proxies and group.
 *
 * Ordering / monotonicity
 * ------------------------
 * The selector is written exactly once (0 -> 1) and never back, so a
 * reader resolving several proxies over time observes a monotone
 * old...old,new...new sequence -- never new-then-old.  Embedders can rely
 * on this together with a fixed read order to avoid a per-traversal
 * snapshot (e.g. publishing the back edges of a re-parent through the
 * latch *before* the forward edge, so a reader -- which descends before it
 * walks back up -- can only ever progress old->new, never regress).
 *
 * SCOPE of "never new-then-old".  Monotonicity of the SELECTOR is unconditional
 * -- it is one word, stored once.  What a reader's SEQUENCE of resolutions
 * inherits from it depends on how the reader got from one slot to the next.  A
 * DEPENDENCY-CHAINED traversal (each hop's address derived from the value the
 * previous hop loaded) carries the order for free on every architecture urcu
 * supports, and so does any build using the default C11 dereference.  A reader
 * that hops WITHOUT that chain -- re-reading a pointer it cached earlier, or
 * walking prev-then-next between two independently-reached slots -- has no such
 * edge: under -DURCU_DEREFERENCE_USE_VOLATILE on weakly-ordered hardware its
 * two loads may be reordered, and it can observe the new selector's effect on
 * the later slot and the old on the earlier one.  Such a reader owes itself an
 * explicit acquire (or a dependency).  The fixed-read-order idiom above is
 * exactly a dependency-chained descent, which is why it is safe as stated.
 *
 * Two layers
 * ----------
 * This header provides two layers:
 *
 *   1. The low-level flip group / proxy primitive above (urcu_txn_sw_group,
 *      urcu_txn_sw_proxy, urcu_txn_sw_proxy_get, urcu_txn_sw_commit).  The
 *      embedder allocates and reclaims the proxies, tags them, and routes
 *      resolution -- as in the four-step lifecycle described above.
 *
 *   2. A growable multi-edge transaction, urcu_txn_sw_txn, built on that
 *      primitive (defined lower in this file).  It owns proxy allocation,
 *      installation, settle and reclaim, so an embedder records edges and
 *      commits a whole set atomically instead of hand-managing proxies.  Its
 *      lifecycle is a state machine:
 *
 *        init -> PREPARE --record--> ... --commit--> committed
 *
 *      record() appends an edge {slot, old, new} into the (realloc-grown)
 *      record array but installs nothing; the record set is FROZEN once
 *      commit() parks the proxies, matching the concurrent engine's contract
 *      (no edge may be added once a proxy is parked), so an embedder written
 *      against this transaction can migrate to <urcu/rcu-txn-mcas.h> mechanically.
 *      commit() parks every recorded proxy, flips the group and settles to new,
 *      then OWNS reclaim: it defers the txn through call_rcu() when it parked
 *      proxies, or frees it at once on the single-edge / empty / OOM paths.
 *      Each recorded edge carries its own tag (the bits OR'd into that slot's
 *      parked proxy value), so one transaction can mix slots with different
 *      tags; the embedder only checks commit()'s status.  See the
 *      urcu_txn_sw_txn block.
 *
 * RCU flavor
 * ----------
 * The transaction layer reclaims its parked proxies with call_rcu(), so this
 * header must be included AFTER an RCU flavor header (e.g. <urcu-qsbr.h>) that
 * maps call_rcu() to that flavor.  (The low-level flip group / proxy primitive
 * above has no such dependency -- an embedder using only it drives its own
 * reclaim.)
 */

#include <stdbool.h>
#include <stddef.h>			/* offsetof */
#include <stdlib.h>
#include <string.h>
#include <urcu/assert.h>
#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu/call-rcu.h>		/* struct rcu_head + call_rcu (commit reclaim) */
#include <urcu/rcu-txn-bloom.h>	/* shared RYW lookup filter (also used by rcu-txn.h) */
#include <urcu/rcu-txn-status.h>	/* enum urcu_txn_status */
#include <urcu/rcu-txn-slab.h>		/* shared per-CPU size-classed descriptor slab */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Shared selector for a flip group.  selector == 0 -> proxies resolve to
 * old_ptr; selector == 1 -> new_ptr.  Written once (0 -> 1) with release
 * semantics by urcu_txn_sw_group_commit(); read with acquire by
 * urcu_txn_sw_proxy_get().
 */
struct urcu_txn_sw_group {
	unsigned long selector;
};

/*
 * ptr[0] is the old target, ptr[1] the new one: the selector (0 -> old,
 * 1 -> new) indexes this array directly in urcu_txn_sw_proxy_get().
 */
struct urcu_txn_sw_proxy {
	void *ptr[2];
	struct urcu_txn_sw_group *group;
};

static inline
void urcu_txn_sw_group_init(struct urcu_txn_sw_group *group)
{
	group->selector = 0;
}

static inline
void urcu_txn_sw_proxy_init(struct urcu_txn_sw_proxy *proxy,
		struct urcu_txn_sw_group *group, void *old_ptr, void *new_ptr)
{
	proxy->ptr[0] = old_ptr;
	proxy->ptr[1] = new_ptr;
	proxy->group = group;
}

/*
 * Resolve a proxy to its current target.
 *
 * @proxy must have been obtained by dereferencing (rcu_dereference) the slot
 * that holds the tagged proxy pointer, so the dependency chain makes the
 * proxy's immutable fields (old_ptr, new_ptr, group) visible.  The selector is
 * the only mutable field: load it with acquire so the new target's contents --
 * published before urcu_txn_sw_group_commit()'s release store -- are visible
 * whenever selector == 1 is observed.
 *
 * The selector indexes proxy->ptr[] directly, so resolution is a pure data
 * dependency rather than a conditional branch.
 */
static inline
void *urcu_txn_sw_proxy_get(const struct urcu_txn_sw_proxy *proxy)
{
	return proxy->ptr[uatomic_load(&proxy->group->selector, CMM_ACQUIRE)];
}

/*
 * Commit the flip: switch every proxy in @group from old to new with a
 * single release store.  Must be called after the new targets are fully
 * built (the release pairs with urcu_txn_sw_proxy_get()'s acquire).
 */
static inline
void urcu_txn_sw_group_commit(struct urcu_txn_sw_group *group)
{
	uatomic_store(&group->selector, 1, CMM_RELEASE);
}

/*
 * Multi-edge flip transaction (urcu_txn_sw_txn)
 * ===========================================
 *
 * A growable transaction over a set of slots, built on the flip group above.
 * The handle is a small on-stack object the embedder declares and inits with
 * urcu_txn_sw_init(); nothing is allocated until edges are recorded, so the
 * init step has no failure mode.
 *
 *   init --> PREPARE --record--> PREPARE --commit--> committed (handle done)
 *              |                            |
 *              | record()/reserve() OOM     | commit owns reclaim:
 *              v (sticky URCU_TXN_SW_OOM)  | call_rcu group block (parked)
 *        commit reports MEMORY_ERROR         | or immediate record-array free
 *        and frees the record array          v (no proxy: empty/single)
 *                                          freed
 *
 * record() appends a latch descriptor {slot, old, new} into the record array
 * but installs NOTHING -- no proxy address is live yet, so the array grows by
 * realloc.  The record set is FROZEN once proxies are installed (which commit()
 * does internally), so record() must precede commit().  This is the same
 * frozen-set contract as the concurrent engine (<urcu/rcu-txn-mcas.h>), so
 * a single-writer embedder can later migrate to concurrent writers without
 * restructuring its mutations.  Two more contracts shared with that engine:
 * records must target PAIRWISE-DISTINCT slots -- unlike the concurrent
 * front-end, record() performs no same-slot reconcile, so a duplicate would
 * park two proxies on one slot and settle both in record order, silently
 * last-wins (a debug build asserts against it at install) -- and nothing
 * observes buffered writes (there are no transactional loads at all).
 *
 * commit() publishes the whole set atomically: it parks every recorded latch's
 * tagged proxy (readers still resolve to old; selector == 0), flips the group
 * with one release store (every proxy resolves to new at once), then settles
 * each slot to its direct new value.  commit() OWNS reclaim:
 *   - nr >= 2 (proxies parked): a reader may hold a proxy, whose old/new
 *     targets and selector live in the record array and the group block; both
 *     are deferred through one call_rcu(urcu_txn_sw_free_rcu) and reclaimed
 *     together after a grace period.
 *   - nr <= 1 / sticky OOM (no proxy ever published): the record array is freed
 *     at once and no group block is ever allocated.
 * It returns enum urcu_txn_status: OK on commit, or MEMORY_ERROR if a
 * reserve()/record() (or the lazy group block) allocation failed -- never ABORT
 * (a single updater has no contention).  An embedder checks only commit()'s
 * status: the stack handle needs no destroy, and there is no create()==NULL
 * checkpoint.  Fresh nodes are the embedder's; the txn never tracks them.
 *
 * OOM is sticky: a reserve()/record() that cannot allocate latches the handle
 * into URCU_TXN_SW_OOM and a later commit() reports MEMORY_ERROR (and frees
 * the record array), so an embedder may ignore the bool returns of
 * reserve()/record() and test only commit() -- matching the concurrent
 * front-end's contract.
 *
 * Each recorded edge carries its own tag (urcu_txn_sw_record's @tag): the bits
 * OR'd into that slot's parked proxy value so its readers recognise the proxy
 * (e.g. a reserved type code).  The latch array is allocated 16-byte aligned
 * (posix_memalign), so each latch -- hence each tagged proxy -- has its low 4
 * bits free for the embedder's tag.
 */

enum urcu_txn_sw_state {
	URCU_TXN_SW_PREPARE = 0,
	URCU_TXN_SW_INSTALLED,	/* internal: set once proxies are parked */
	URCU_TXN_SW_OOM,		/* sticky: commit -> MEMORY_ERROR */
};

struct urcu_txn_sw_latch {
	struct urcu_txn_sw_proxy proxy;	/* ptr[0]=old, ptr[1]=new, group */
	void **slot;			/* install / settle target */
	uintptr_t tag;			/*
					 * Embedder tag bits OR'd into THIS
					 * slot's parked proxy value at install
					 * (see urcu_txn_sw_record).  Per-record
					 * so heterogeneous slots -- e.g. a
					 * 0xF-tagged structural edge and a
					 * bit-0 list edge -- can share one
					 * transaction.
					 */
} __attribute__((aligned(16)));

/*
 * Transaction block (HEAP path): the single allocation that carries a committed
 * transaction across its grace period, mirroring the concurrent engine's struct
 * urcu_mcas (one block = header + inline records).  It holds the flip group
 * every parked proxy reads (&block->group, a plain pointer -- never tagged --
 * so it needs no alignment of its own), the rcu_head that defers reclaim, and
 * the record array INLINE (the proxies themselves live there).  Allocated on
 * the first record()/reserve() from the shared per-CPU slab and grown in
 * PREPARE (no proxy is live yet, so it may move); ONE free -- record array and
 * all -- reclaims it.  The 32-byte header keeps latches[] 16-byte aligned so
 * each tagged proxy has its low 4 bits free.  @cap is the physical capacity;
 * @slab (stamped at alloc) tells free the block's origin -- self-describing, so
 * a block allocated before the slab constructor ran or while it was disabled is
 * freed on the right path even if the slab enables in between.  The INLINE path
 * (urcu_txn_sw_init_inline, nr <= 1) never allocates a block.
 */
struct urcu_txn_sw_block {
	struct urcu_txn_sw_group group;		/* selector; parked proxies read &block->group */
	struct rcu_head rcu_head;		/* deferred-free handle */
	unsigned int cap;			/* physical capacity */
	unsigned int slab;			/*
						 * Block origin: slab (1) /
						 * exact malloc (0); the free
						 * discriminator, doubling as
						 * the pad that keeps latches[]
						 * 16-byte aligned.
						 */
	struct urcu_txn_sw_latch latches[];	/* INLINE record array (frozen at install) */
};

urcu_static_assert(!(offsetof(struct urcu_txn_sw_block, latches) % 16),
		"urcu_txn_sw_block.latches must be 16-byte aligned for proxy tagging",
		urcu_txn_sw_block_latches_aligned);

/*
 * Transaction handle: a small on-stack object with no allocation of its own.
 * The record array @latches grows in PREPARE -- safe because no proxy is
 * installed yet (no slot points into it) -- and is frozen once commit() parks
 * proxies.  @block is the lazily-allocated persistent group block (NULL until
 * proxies are parked); commit() hands @latches to it so a held proxy and its
 * selector are reclaimed together after a grace period.  The tag room that
 * matters is on the tagged proxies stored in slots; those live in @latches,
 * allocated 16-byte aligned (posix_memalign) so each latch -- hence each tagged
 * proxy -- keeps its low 4 bits free, letting a low-4-bit pointer-tagging
 * embedder (e.g. the fractal trie) route its slots through this engine.
 */
#ifdef URCU_TXN_SW_EXCL_VALIDATE
#include <pthread.h>
#endif

struct urcu_txn_sw_txn {
	enum urcu_txn_sw_state state;
	struct urcu_txn_sw_latch *latches;	/* record array (realloc-grown, or caller-owned if @latches_inline) */
	struct urcu_txn_sw_block *block;	/* heap path: single allocation (latches inline); NULL until first record */
	unsigned int nr;
	unsigned int cap;
	bool latches_inline;			/* @latches is caller storage: never realloc'd, never freed */
	bool disjoint;				/* write set declared slot-disjoint: skip the RYW find */
	bool bloom_live;			/* @ryw_bloom is armed (see urcu_txn_sw__find_ryw) */
	uint64_t ryw_bloom[URCU_TXN_BLOOM_WORDS];  /* RYW certain-miss filter */
#ifdef URCU_TXN_SW_EXCL_VALIDATE
	pthread_t excl_owner;			/*
						 * pthread_self() of the thread
						 * that init'd this handle; the
						 * handle must be driven end to
						 * end by it.  See the validator
						 * below.
						 */
#endif
};

#define URCU_TXN_SW_CAP	8	/* initial record-array capacity */

/*
 * URCU_TXN_SW_EXCL_VALIDATE: runtime validation of the SINGLE-UPDATER contract.
 *
 * This engine requires writer mutual exclusion -- it has no install-time CAS,
 * no conflict detection and no abort, so two writers racing on one slot simply
 * corrupt it, and nothing in a default build says so.  Enable with
 * -DURCU_TXN_SW_EXCL_VALIDATE to have a violation abort the process with a
 * report identifying the violated handle or slot.  Off by default (zero
 * overhead: no checks, and the handle does not even carry the owner field).
 *
 * There is no per-structure object to claim an owner on, as <urcu/rcu-txn.h>'s
 * concurrent front-end has in its escalation domain: the sw mutators take a
 * node (urcu_txn_sw_list_del_rcu(elem)), never a head, and an hlist head is one
 * bare pointer by design.  So the validator claims the two things that DO
 * exist:
 *
 *   - the HANDLE.  Its owner is the thread that init'd it, and reserve / record
 *     / install / commit must all run on that thread.  Catches a transaction
 *     handed between threads mid-flight.
 *
 *   - the SLOT, which is what two racing writers actually share, and so is
 *     where the real violation is visible.  In a correct single-updater
 *     program a slot being recorded or parked CANNOT already hold a parked
 *     proxy: this transaction parks nothing before install, its record set is
 *     frozen after, and any earlier transaction of the same (sole) writer
 *     settled its slots back to plain values before returning.  A proxy
 *     sitting there therefore means another writer is mid-transaction on that
 *     very slot right now.  Symmetrically, at settle each slot must still hold
 *     OUR proxy; anything else means a concurrent writer overwrote it.
 *
 * Like the fractal trie's FEATURE_FT_EXCL_VALIDATE, this catches the
 * deterministic case where the bad interleaving actually happens in this run.
 * Two writers that overlap but never observe each other's parked proxy -- one
 * settles before the other records -- still corrupt, and are still invisible
 * here.  A clean run is evidence, not proof.
 */
/*
 * Return true if @v, loaded from a slot recorded under @tag, is a parked
 * proxy rather than a live value: it carries all of @tag's bits.  The tag
 * contract (see urcu_txn_sw_record) is that no live value of that slot
 * does.  Strip the tag to reach the struct urcu_txn_sw_proxy.
 */
static inline
bool urcu_txn_sw_is_proxy(const void *v, uintptr_t tag)
{
	return tag != 0 && ((uintptr_t) v & tag) == tag;
}

#if defined(URCU_TXN_SW_EXCL_VALIDATE) || defined(URCU_TXN_SW_DEBUG_DISJOINT)
# include <stdio.h>			/* the validators' reports */
#endif

#ifdef URCU_TXN_SW_EXCL_VALIDATE

#define urcu_txn_sw__excl_abort(...)					\
	do {								\
		fprintf(stderr, "urcu-txn-sw single-updater violation: "	\
			__VA_ARGS__);					\
		fflush(stderr);						\
		abort();						\
	} while (0)


static inline
void urcu_txn_sw__excl_claim(struct urcu_txn_sw_txn *t)
{
	t->excl_owner = pthread_self();
}

static inline
void urcu_txn_sw__excl_owner(const struct urcu_txn_sw_txn *t, const char *what)
{
	pthread_t self = pthread_self();

	if (!pthread_equal(t->excl_owner, self))
		urcu_txn_sw__excl_abort("txn=%p: %s on a thread other than the one that initialized the handle -- one transaction is driven end to end by one thread\n",
			(const void *) t, what);
}

/* @slot must not already be parked by somebody else. */
static inline
void urcu_txn_sw__excl_slot_free(void **slot, uintptr_t tag, const char *what)
{
	void *v = uatomic_load(slot, CMM_RELAXED);

	if (urcu_txn_sw_is_proxy(v, tag))
		urcu_txn_sw__excl_abort("slot=%p already holds a parked proxy (%p) at %s: another writer is mid-transaction on it\n",
			(void *) slot, v, what);
}

/* At settle, @l's slot must still hold the proxy WE parked in it. */
static inline
void urcu_txn_sw__excl_slot_ours(struct urcu_txn_sw_latch *l)
{
	void *want = (void *) ((uintptr_t) &l->proxy | l->tag);
	void *v = uatomic_load(l->slot, CMM_RELAXED);

	if (v != want)
		urcu_txn_sw__excl_abort("slot=%p holds %p at settle, expected our parked proxy %p: a concurrent writer overwrote it\n",
			(void *) l->slot, v, want);
}

/*
 * The single-edge commit stores blind (no proxy is ever parked), so its only
 * witness is the value: @l's slot must still hold the recorded old.
 */
static inline
void urcu_txn_sw__excl_slot_unchanged(struct urcu_txn_sw_latch *l)
{
	void *v = uatomic_load(l->slot, CMM_RELAXED);

	if (v != l->proxy.ptr[0])
		urcu_txn_sw__excl_abort("slot=%p holds %p at the single-edge commit, but %p was recorded as its old: a concurrent writer changed it\n",
			(void *) l->slot, v, l->proxy.ptr[0]);
}

#else	/* !URCU_TXN_SW_EXCL_VALIDATE */

# define urcu_txn_sw__excl_claim(t)			do { } while (0)
# define urcu_txn_sw__excl_owner(t, what)		do { } while (0)
# define urcu_txn_sw__excl_slot_free(slot, tag, what)	do { } while (0)
# define urcu_txn_sw__excl_slot_ours(l)			do { } while (0)
# define urcu_txn_sw__excl_slot_unchanged(l)		do { } while (0)

#endif	/* URCU_TXN_SW_EXCL_VALIDATE */

/*
 * Initialize an on-stack transaction handle.  No allocation, so this cannot
 * fail; the first record()/reserve() is the first OOM checkpoint, and it is
 * sticky (commit reports MEMORY_ERROR).  Each edge carries its own tag (passed
 * to urcu_txn_sw_record), so the handle holds no per-txn tag hook.
 */
static inline
void urcu_txn_sw_init(struct urcu_txn_sw_txn *t)
{
	t->state = URCU_TXN_SW_PREPARE;
	t->latches = NULL;
	t->block = NULL;
	t->nr = 0;
	t->cap = 0;
	t->latches_inline = false;
	t->disjoint = false;
	t->bloom_live = false;		/* armed lazily; ryw_bloom needs no zeroing until then */
	urcu_txn_sw__excl_claim(t);
}

/*
 * Initialize a transaction whose record array is CALLER-PROVIDED storage @buf,
 * holding @cap latches (e.g. a 16-byte-aligned on-stack array -- struct
 * urcu_txn_sw_latch is aligned(16), so an array of it is, keeping each tagged
 * proxy's low 4 bits free).  No allocation, so it cannot fail; record() never
 * realloc-grows the inline array (overflow past @cap is an embedder sizing
 * bug), and commit() never frees it.  Use ONLY where the transaction need not
 * outlive the call: a lone-edge (or empty) commit parks no proxy and owes no
 * grace period, so nothing references the txn -- or its inline storage -- once
 * commit returns.  A txn that would park proxies (nr >= 2) MUST own heap
 * storage (urcu_txn_sw_init + record/reserve), since the parked record array
 * has to survive until a grace period; the engine asserts an inline buffer
 * never reaches that path.  This is the public analog of the bounded on-stack
 * flip that the fractal trie uses for its single-pointer publishes.
 */
static inline
void urcu_txn_sw_init_inline(struct urcu_txn_sw_txn *t,
		struct urcu_txn_sw_latch *buf, unsigned int cap)
{
	t->state = URCU_TXN_SW_PREPARE;
	t->latches = buf;
	t->block = NULL;
	t->nr = 0;
	t->cap = cap;
	t->latches_inline = true;
	t->disjoint = false;
	t->bloom_live = false;		/* armed lazily; ryw_bloom needs no zeroing until then */
	urcu_txn_sw__excl_claim(t);
}

#define urcu_txn_sw_blocksize(cap)	\
	(sizeof(struct urcu_txn_sw_block) + (size_t) (cap) * sizeof(struct urcu_txn_sw_latch))

/*
 * The heap-path transaction block is served from the shared per-CPU
 * size-classed slab in <urcu/rcu-txn-slab.h> (same machinery as the concurrent
 * engine).  Latch-count classes {4,8,16,32,64,128} map to byte sizes; a request
 * over the top class is an exact, uncached posix_memalign.  Each block is
 * stamped with its origin (urcu_txn_sw_block.slab), which free consults.
 * URCU_TXN_NO_CACHE disables the slab.
 */
static const unsigned int urcu_txn_sw_slab_rc[] = { 4u, 8u, 16u, 32u, 64u, 128u };
#define URCU_TXN_SW_SLAB_NCLASS	\
	((int) (sizeof(urcu_txn_sw_slab_rc) / sizeof(urcu_txn_sw_slab_rc[0])))

/*
 * The slab INSTANCE lives once, in liburcu-common (src/urcu-txn.c), which also
 * initializes it from a library constructor -- so this header requires linking
 * liburcu-common.  See the matching note in <urcu/rcu-txn-mcas.h> for why a
 * header-static definition would be wrong (per-TU arena/superblock
 * multiplication, non-shared freelists).
 */
extern struct urcu_slab urcu_txn_sw_slab;

/* Smallest latch-count class that fits @req latches, or -1 if over the top. */
static inline
int urcu_txn_sw_slab_class_of(unsigned int req)
{
	int i;

	for (i = 0; i < URCU_TXN_SW_SLAB_NCLASS; i++)
		if (req <= urcu_txn_sw_slab_rc[i])
			return i;
	return -1;
}

/*
 * Allocate a transaction block for >= @cap latches, 16-byte aligned (so the
 * inline latches[] -- hence every tagged proxy -- keeps its low 4 bits free).
 * From the slab when the request fits a class (its physical cap becomes the
 * class size), else an exact posix_memalign.  The flip group is initialized
 * here.  Returns NULL on OOM.
 */
static inline
struct urcu_txn_sw_block *urcu_txn_sw__block_alloc(unsigned int cap)
{
	struct urcu_txn_sw_block *blk;
	int cl;

	if (urcu_slab_enabled(&urcu_txn_sw_slab) &&
			(cl = urcu_txn_sw_slab_class_of(cap)) >= 0) {
		blk = (struct urcu_txn_sw_block *)
				urcu_slab_alloc(&urcu_txn_sw_slab, cl);
		if (caa_unlikely(!blk))
			return NULL;
		cap = urcu_txn_sw_slab_rc[cl];		/* physical class cap */
		blk->slab = 1;
	} else {
		void *p;

		if (posix_memalign(&p, 16, urcu_txn_sw_blocksize(cap)))
			return NULL;
		blk = (struct urcu_txn_sw_block *) p;
		blk->slab = 0;
	}
	urcu_txn_sw_group_init(&blk->group);
	blk->cap = cap;
	return blk;
}

/*
 * Free a block on the path that allocated it (the blk->slab stamp): to its
 * slab ORIGIN arena, or to malloc for an exact block.  The stamp -- not the
 * slab's current enabled state -- decides, so a block allocated before the
 * slab constructor ran is never misrouted after the slab enables.
 */
static inline
void urcu_txn_sw__block_free(struct urcu_txn_sw_block *blk)
{
	if (!blk)
		return;
	if (blk->slab)
		urcu_slab_free(blk);
	else
		free(blk);
}

/*
 * Pre-size the PREPARE record array to hold at least @cap latches.  Optional:
 * record() already realloc-grows the array on demand and fails cleanly on OOM,
 * which is the general (unbounded) model.  But an embedder whose edge count is
 * bounded by construction can reserve that bound once, up front, where failure
 * is clean -- then every subsequent record() appends without reallocating and
 * so cannot fail.  This trades the design's "fail-and-destroy replaces the
 * count pass" for a single up-front alloc, which is the right call for a
 * bounded txn whose records are interleaved through a build that does not
 * otherwise thread an OOM return.  Call after init, before any install; a
 * handle already buffering (an earlier reserve or record) grows to fit @cap,
 * mirroring the concurrent front-end's urcu_txn_reserve().  Returns false on
 * OOM; the failure is sticky (URCU_TXN_SW_OOM), so the caller may ignore this
 * return and let commit() report MEMORY_ERROR.  The one non-OOM, NON-sticky
 * false: asking a caller-storage (init_inline) handle for more than its fixed
 * buffer -- that is an embedder sizing bug, not a memory failure, and caller
 * storage can neither grow nor be adopted by the engine.
 */
static inline
bool urcu_txn_sw_reserve(struct urcu_txn_sw_txn *t, unsigned int cap)
{
	struct urcu_txn_sw_block *blk;

	if (caa_unlikely(t->state == URCU_TXN_SW_OOM))
		return false;			/* sticky: an earlier alloc failed */
	/*
	 * PREPARE only, exactly like record().  install() is a documented
	 * public entry, so a white-box caller can reach reserve() with proxies
	 * already parked -- and the grow path below would then memcpy the
	 * latches to a fresh block and FREE the old one while live slots still
	 * point into it: a reader dereferences a freed proxy.  The record set
	 * is frozen once installed.
	 */
	urcu_posix_assert(t->state == URCU_TXN_SW_PREPARE);
	urcu_txn_sw__excl_owner(t, "reserve()");
	if (t->latches) {
		if (cap <= t->cap)
			return true;		/* already sized (heap block or inline buf) */
		if (t->latches_inline) {
			/* Fixed caller storage: a sizing bug, not an OOM. */
			urcu_assert_debug(!t->latches_inline);
			return false;
		}
		/*
		 * Already buffering: grow to fit @cap.  No proxy is live in
		 * PREPARE, so the array may move (same rationale as record()'s
		 * grow); copy the buffered records into a fresh aligned block.
		 */
		blk = urcu_txn_sw__block_alloc(cap);
		if (!blk) {
			t->state = URCU_TXN_SW_OOM;	/* sticky: commit reports it */
			return false;
		}
		memcpy(blk->latches, t->latches,
				(size_t) t->nr * sizeof(*blk->latches));
		urcu_txn_sw__block_free(t->block);
		t->block = blk;
		t->latches = blk->latches;
		t->cap = blk->cap;
		return true;
	}
	if (cap < URCU_TXN_SW_CAP)
		cap = URCU_TXN_SW_CAP;
	blk = urcu_txn_sw__block_alloc(cap);
	if (!blk) {
		t->state = URCU_TXN_SW_OOM;	/* sticky: commit reports it */
		return false;
	}
	t->block = blk;
	t->latches = blk->latches;		/* records live inline in the block */
	t->cap = blk->cap;			/* physical class capacity */
	return true;
}

/*
 * True when the NEXT record() on @t appends into capacity the handle already
 * owns, and so cannot fail: no realloc, no group-block alloc, no path to
 * URCU_TXN_SW_OOM.  A reserve() that covered the bracket's edge bound is the
 * deliberate way to make this hold, but it is not the only one -- record()
 * grows on demand to URCU_TXN_SW_CAP, so an unreserved handle satisfies this
 * too until that first capacity is used up.  Capacity is what matters, not
 * which call supplied it.
 *
 * Sized for embedders that mutate state OUTSIDE the transaction as they record
 * (e.g. <urcu/rcu-txn-sw-hlist.h>'s writer-only pprev, a plain store that no
 * rollback can undo).  Such an embedder is only safe while a later record in
 * the same bracket cannot fail behind it, and this is that question.  A handle
 * already in URCU_TXN_SW_OOM answers false: it has no capacity to promise.
 */
static inline
bool urcu_txn_sw_append_is_infallible(const struct urcu_txn_sw_txn *t)
{
	return t->state == URCU_TXN_SW_PREPARE && t->nr < t->cap;
}

/* Free the record array of a handle that never parked proxies (no grace
 * period).  Caller-owned (inline) storage is never freed by the engine. */
static inline
void urcu_txn_sw__free_records(struct urcu_txn_sw_txn *t)
{
	if (!t->latches_inline)
		urcu_txn_sw__block_free(t->block);	/* one free; inline storage is caller-owned */
	t->block = NULL;
	t->latches = NULL;
}

/*
 * Drop every record of @t without committing it.  @t is then empty, and
 * records again from scratch.  An out-of-memory state stays, so the
 * commit still reports MEMORY_ERROR.
 *
 * Call it before install() or after the commit, never in between: parked
 * proxies point into the record array.  The engine frees the array it
 * owns.  Caller storage (urcu_txn_sw_init_inline()) is kept for the next
 * record until a commit, which consumes it.
 */
static inline
void urcu_txn_sw_discard(struct urcu_txn_sw_txn *t)
{
	urcu_posix_assert(t->state != URCU_TXN_SW_INSTALLED || !t->latches);
	urcu_txn_sw__excl_owner(t, "discard()");
	if (!t->latches_inline)
		urcu_txn_sw__free_records(t);
	if (!t->latches)
		t->cap = 0;
	t->nr = 0;
	t->bloom_live = false;
	if (t->state == URCU_TXN_SW_INSTALLED)
		t->state = URCU_TXN_SW_PREPARE;
}

/*
 * call_rcu callback: free a committed transaction's persistent group block and
 * the record array it carries, once the grace period has retired any proxy a
 * reader may still have held.
 */
static inline
void urcu_txn_sw_free_rcu(struct rcu_head *head)
{
	struct urcu_txn_sw_block *blk = caa_container_of(head,
			struct urcu_txn_sw_block, rcu_head);

	urcu_txn_sw__block_free(blk);		/* record array is inline -- one free */
}

/* Record a latch's {old, new, slot, tag}; its proxy->group is bound at
 * install. */
static inline
void urcu_txn_sw_latch_set(struct urcu_txn_sw_latch *l,
		void **slot, void *old_ptr, void *new_ptr, uintptr_t tag)
{
	l->proxy.ptr[0] = old_ptr;
	l->proxy.ptr[1] = new_ptr;
	l->slot = slot;
	l->tag = tag;
}

/* Park latch @l's tagged proxy into its slot (readers resolve to old).  The
 * parked value is &l->proxy OR'd with the slot's per-record tag; the latch
 * array is 16-byte aligned, so &l->proxy keeps its low 4 bits free for it. */
static inline
void urcu_txn_sw_latch_install(struct urcu_txn_sw_txn *t, struct urcu_txn_sw_latch *l)
{
	l->proxy.group = &t->block->group;	/* bind to the now-allocated group */
	uatomic_store(l->slot,
			(void *) ((uintptr_t) &l->proxy | l->tag), CMM_RELEASE);
}

/* This transaction's record for @slot, or NULL.  Linear, so O(nr) per call and
 * O(nr^2) to build a write set -- urcu_txn_sw__find_ryw() below is what keeps
 * that off the hot path.  The returned pointer is invalidated by the next
 * record() (a grow may move the array), so use it before recording again. */
static inline
struct urcu_txn_sw_latch *urcu_txn_sw__find(const struct urcu_txn_sw_txn *t,
		void **slot)
{
	unsigned int i;

	for (i = 0; i < t->nr; i++)
		if (t->latches[i].slot == slot)
			return &t->latches[i];
	return NULL;
}

/*
 * URCU_TXN_SW_BLOOM_MIN: the record count at which the shared RYW filter
 * (<urcu/rcu-txn-bloom.h>) starts to pay for itself, and below which this engine
 * does not arm it at all.
 *
 * The concurrent engine maintains its filter unconditionally because its
 * transactions are preceded by a traversal that reads far more slots than it
 * writes -- the filter's cost is noise against that, and the miss it answers is
 * the dominant case.  This engine has no such traversal: urcu_txn_sw_load() is
 * only ever called on a slot about to be recorded (readers use the _rcu
 * accessors, which never consult a write set), so loads track records ~1:1 and
 * the DOMINANT transaction is tiny -- a bitmap set_rcu is one edge, a list op
 * two.  There, find is zero-to-one pointer compares, cheaper than k hashes,
 * and zeroing the filter would be pure loss.
 *
 * So arm lazily, and on first USE rather than on the record that crosses the
 * threshold: below URCU_TXN_SW_BLOOM_MIN records pay nothing (not even the
 * reset -- the array stays untouched).  The filter is built and armed by the
 * first urcu_txn_sw__find_ryw() lookup that faces a write set at least that
 * long, then maintained on every record thereafter.  Arming on the crossing
 * record instead would make a transaction of exactly URCU_TXN_SW_BLOOM_MIN
 * edges build and commit the filter without ever testing it (~8% measured on an
 * 8-edge commit).  This buys the O(1) miss exactly where the O(nr^2) bites, and
 * costs one predictable compare where it does not.
 */
#ifndef URCU_TXN_SW_BLOOM_MIN
# define URCU_TXN_SW_BLOOM_MIN	8
#endif

/*
 * Build the filter from the records so far and arm it.  Called on the first
 * lookup that would scan a write set long enough to be worth filtering -- see
 * URCU_TXN_SW_BLOOM_MIN.
 */
static inline
void urcu_txn_sw__bloom_arm(struct urcu_txn_sw_txn *t)
{
	unsigned int i;

	memset(t->ryw_bloom, 0, sizeof(t->ryw_bloom));
	for (i = 0; i < t->nr; i++)
		urcu_txn__ryw_bloom_set(t->ryw_bloom, t->latches[i].slot);
	t->bloom_live = true;
}

/*
 * urcu_txn_sw__find() with the certain-miss filter in front: a clear bit means
 * the slot is DEFINITELY not recorded, so the scan is skipped; all k bits set
 * falls through to the authoritative find, which resolves the false positive.
 * The filter can only ever save the scan, never change the answer.
 */
static inline
struct urcu_txn_sw_latch *urcu_txn_sw__find_ryw(struct urcu_txn_sw_txn *t,
		void **slot)
{
#ifndef URCU_TXN_SW_RYW_NO_BLOOM
	if (caa_unlikely(!t->bloom_live)) {
		/*
		 * Arm on first USE, not on the record that crosses the
		 * threshold: a transaction of exactly URCU_TXN_SW_BLOOM_MIN
		 * edges would otherwise pay the build and commit without ever
		 * testing -- measured ~8% on an 8-edge commit, pure loss.
		 */
		if (t->nr < URCU_TXN_SW_BLOOM_MIN)
			return urcu_txn_sw__find(t, slot);
		urcu_txn_sw__bloom_arm(t);
	}
	if (!urcu_txn__ryw_bloom_test(t->ryw_bloom, slot))
		return NULL;			/* definitely absent */
#endif
	return urcu_txn_sw__find(t, slot);
}

/*
 * Return the record @t holds for @slot, or NULL.  Its proxy holds the
 * old value in ptr[0] and the new value in ptr[1].  The next record() or
 * reserve() on @t may move the record array, so use the result before
 * recording again.  A committed or discarded @t has no records.
 */
static inline
const struct urcu_txn_sw_latch *urcu_txn_sw_find(struct urcu_txn_sw_txn *t,
		void **slot)
{
	if (!t->latches)
		return NULL;
	return urcu_txn_sw__find_ryw(t, slot);
}

/*
 * Record one edge {*slot: old -> new} tagged with @tag (the bits OR'd into the
 * parked proxy value installed in *slot, so that slot's readers recognise the
 * proxy and route resolution -- e.g. the fractal trie's 0xF type code or a
 * list's bit-0).  PREPARE only -- the record set is frozen once proxies are
 * installed, so this must run before commit() (record() after a commit/install
 * is a usage error).  Records must target pairwise-distinct slots: record()
 * appends blindly -- no same-slot reconcile -- so recording one slot twice
 * parks two proxies on it and settles both in record order, silently last-wins
 * (install asserts against it in a debug build).  Returns false on OOM (the
 * only failure); the failure is sticky (URCU_TXN_SW_OOM), so the caller may
 * ignore this return and let commit() report MEMORY_ERROR.  The record array
 * realloc-grows on demand and nothing is installed here (no proxy address is
 * live until commit parks them).
 */
static inline
bool urcu_txn_sw_record(struct urcu_txn_sw_txn *t, void **slot,
		void *old_ptr, void *new_ptr, uintptr_t tag)
{
	struct urcu_txn_sw_latch *l;

	if (caa_unlikely(t->state == URCU_TXN_SW_OOM))
		return false;			/* sticky: an earlier alloc failed */
	urcu_posix_assert(t->state == URCU_TXN_SW_PREPARE);
	urcu_txn_sw__excl_owner(t, "record()");
	urcu_txn_sw__excl_slot_free(slot, tag, "record()");
	if (t->nr == t->cap) {
		unsigned int newcap = t->cap ? t->cap * 2 : URCU_TXN_SW_CAP;

		/*
		 * Caller-owned (inline) storage is sized to the embedder's edge
		 * bound and must never grow -- realloc-ing it would move caller
		 * (e.g. on-stack) memory.  Overflow here is an embedder sizing
		 * bug.
		 */
		urcu_posix_assert(!t->latches_inline);
		/*
		 * No proxy address is live yet -> the array may move.  No
		 * aligned realloc exists, so allocate a fresh 16-byte-aligned
		 * block, copy the buffered records, and free the old one.
		 */
		struct urcu_txn_sw_block *nb = urcu_txn_sw__block_alloc(newcap);

		if (!nb) {
			t->state = URCU_TXN_SW_OOM;	/* sticky: commit reports it */
			return false;
		}
		memcpy(nb->latches, t->latches,
				(size_t) t->nr * sizeof(*nb->latches));
		urcu_txn_sw__block_free(t->block);	/* NULL on the first grow */
		t->block = nb;
		t->latches = nb->latches;
		t->cap = nb->cap;
		/*
		 * The storage is now the ENGINE's, so drop the inline flag.  It
		 * can still be set here: the assert above compiles out under
		 * NDEBUG, and an oversized caller-storage handle then reaches
		 * this grow and MIGRATES to engine-owned heap (its records
		 * copied; the caller's buffer left untouched and, as promised,
		 * never freed).  Leaving the flag set would make
		 * __free_records() skip the free of the block we just adopted
		 * -- a leak -- and would trip commit()'s inline assert on a
		 * handle that is no longer inline.
		 */
		t->latches_inline = false;
	}
	l = &t->latches[t->nr++];
	urcu_txn_sw_latch_set(l, slot, old_ptr, new_ptr, tag);
	if (t->bloom_live)		/* armed: keep it current (disjoint never arms) */
		urcu_txn__ryw_bloom_set(t->ryw_bloom, slot);
	return true;
}


/*
 * Read-your-own-writes pair (urcu_txn_sw_load / urcu_txn_sw_record_chain)
 * =====================================================================
 *
 * urcu_txn_sw_record() appends BLINDLY and requires pairwise-distinct slots
 * (install() debug-asserts it; an NDEBUG build silently last-wins).  That is
 * the right default -- it keeps the common fixed-arity embedder (a list op
 * records two known-distinct edges) free of an O(nr) scan per store, and it
 * catches a duplicate as the embedder bug it usually is.
 *
 * But an embedder whose write site is COMPUTED rather than known -- a bitmap,
 * where 63 logical bits share one transacted word, so flips of distinct bit
 * indexes routinely land on the same slot -- needs the opposite: a same-slot
 * store must FUSE with the pending one, not duplicate it.  This pair provides
 * exactly that, mirroring <urcu/rcu-txn.h>'s default read-your-own-writes
 * (urcu_txn_load / urcu_txn_store) so an embedder written against it migrates
 * to the concurrent engine mechanically:
 *
 *     old = urcu_txn_sw_load(t, slot, TAG);
 *     urcu_txn_sw_record_chain(t, slot, old, f(old), TAG);
 *
 * The transaction keeps AT MOST ONE record per slot, exactly as the concurrent
 * engine does.  What this pair does NOT import is a read set: a load records
 * nothing and is not validated (there is nothing to validate against under a
 * single updater), so it is only ever this transaction's own pending state.
 *
 * SCOPE.  This fuses same-SLOT stores.  It does not make every composition
 * sound: an embedder whose new value is a NEIGHBOUR's pointer (a list) can
 * still build its write set from stale reads on pairwise-distinct slots unless
 * its traversal ALSO reads through urcu_txn_sw_load().  See the trap in
 * urcu_txn_sw_list_add_after_prepare().
 */

/*
 * Declare this handle's write set slot-DISJOINT: the RYW pair then skips its
 * find and behaves exactly as a raw read plus urcu_txn_sw_record() -- the fast
 * path, since read-your-own-writes is vacuous when no slot is re-touched.  The
 * single-updater analogue of urcu_txn_declare_disjoint() (<urcu/rcu-txn.h>),
 * and the same contract: distinctness is a property of the SLOT ADDRESS, not of
 * a key.  A bitmap packs 63 logical bits into one physical word, so distinct
 * bit indexes are NOT distinct slots: two per-bit flips may only share a
 * disjoint handle if the caller knows their bits fall in distinct WORDS.
 *
 * Worth declaring where the find is not free: it costs O(nr) per recorded edge,
 * so a transaction of n edges pays O(n^2).  Measured over a bitmap range (one
 * edge per spanned word), declaring disjoint is worth ~nothing at 2-8 edges,
 * ~1.6x at 32, and ~18x at 800 -- so the answer tracks the edge count, not the
 * structure.  A list op's two edges are one pointer compare: its _rcu wrappers
 * declare disjoint for the contract, not the cycles.
 *
 * A LONE urcu_txn_sw_bitmap_set_range_prepare() is the case that pays: it walks
 * word by word and touches each exactly once, so its handle may be declared
 * disjoint even though a bitmap's per-bit forms generally may not.  Do not
 * declare it if anything else in the same transaction can touch a word the
 * range spans.
 *
 * Contract: if a store DOES hit a recorded slot, the blind append parks two
 * proxies on it and settle stores both new values in record order -- the
 * EARLIER edit silently lost in a commit that reports OK (install()'s duplicate
 * scan traps it under DEBUG_RCU; a plain NDEBUG build corrupts quietly).  Build
 * with -DURCU_TXN_SW_DEBUG_DISJOINT to trap at the offending record instead --
 * but note it traps only the RECORD side: a load on a disjoint handle skips the
 * find entirely and silently returns the slot's COMMITTED value, which no debug
 * build sees.  Call after init and before the first record(); do not flip
 * mid-transaction.
 */
static inline
void urcu_txn_sw_declare_disjoint(struct urcu_txn_sw_txn *t)
{
	urcu_posix_assert(t->state == URCU_TXN_SW_PREPARE);
	urcu_posix_assert(!t->nr);		/* before the first record */
	t->disjoint = true;
}

/*
 * Load @slot as this transaction will leave it: the pending new_ptr of its
 * record for @slot if it has one, else the slot's current value.  @tag is the
 * slot's proxy tag, checked (debug) against the single-updater invariant that
 * an unrecorded slot always holds a settled literal -- this updater owns every
 * store to it, and its own prior transactions settled each slot back to a
 * literal before commit() returned, so no proxy can be parked here.
 *
 * On a handle that declared its write set disjoint this skips the find and
 * returns the committed value: nothing is pending for a slot that, by that
 * declaration, no earlier edge touched.
 */
static inline
void *urcu_txn_sw_load(struct urcu_txn_sw_txn *t, void **slot,
		uintptr_t tag)
{
	struct urcu_txn_sw_latch *l;
	void *v;

	urcu_txn_sw__excl_owner(t, "load()");
	if (!t->disjoint && (l = urcu_txn_sw__find_ryw(t, slot)) != NULL)
		return l->proxy.ptr[1];		/* our own pending write */
	v = uatomic_load(slot, CMM_RELAXED);
	urcu_assert_debug(((uintptr_t) v & tag) != tag);
	return v;
}

/*
 * Record edge {*slot: @old_ptr -> @new_ptr} tagged @tag, CHAINING onto this
 * transaction's existing record for @slot if it has one -- keeping that
 * record's committed old and advancing only its new_ptr -- instead of
 * appending a duplicate.  The fusing counterpart of urcu_txn_sw_record(); see
 * the pair's contract above.  Returns false on OOM (sticky; commit reports
 * MEMORY_ERROR), as record() does.
 *
 * @old_ptr must be what urcu_txn_sw_load() returned for @slot: on the chain
 * path it is the record's PENDING value, not the committed one, and a
 * disagreeing old means the caller computed @new_ptr from a stale read (debug
 * assert -- the concurrent engine poisons the attempt for the same reason).
 */
static inline
bool urcu_txn_sw_record_chain(struct urcu_txn_sw_txn *t, void **slot,
		void *old_ptr, void *new_ptr, uintptr_t tag)
{
	struct urcu_txn_sw_latch *l;

	if (caa_unlikely(t->state == URCU_TXN_SW_OOM))
		return false;			/* sticky: an earlier alloc failed */
	urcu_posix_assert(t->state == URCU_TXN_SW_PREPARE);
	urcu_txn_sw__excl_owner(t, "record_chain()");
	if (t->disjoint) {			/* declared: no slot repeats, skip the find */
#ifdef URCU_TXN_SW_DEBUG_DISJOINT
		if (urcu_txn_sw__find(t, slot) != NULL) {
			fprintf(stderr, "urcu-txn-sw: disjoint-contract violation: "
				"slot %p is already recorded in this transaction, but the "
				"handle declared its write set disjoint via "
				"urcu_txn_sw_declare_disjoint().  The blind append would "
				"park a second proxy on it and settle both in record order, "
				"silently losing the earlier edit.  Use the default (do not "
				"declare disjoint) for a mutator whose composed edges can "
				"alias a slot.\n", (void *) slot);
			abort();
		}
#endif
		return urcu_txn_sw_record(t, slot, old_ptr, new_ptr, tag);
	}
	l = urcu_txn_sw__find_ryw(t, slot);
	if (!l)
		return urcu_txn_sw_record(t, slot, old_ptr, new_ptr, tag);
	urcu_assert_debug(l->tag == tag);
	urcu_assert_debug(l->proxy.ptr[1] == old_ptr);	/* caller read its own writes */
	l->proxy.ptr[1] = new_ptr;		/* committed old preserved */
	return true;
}

/*
 * PREPARE -> INSTALLED, parking every recorded latch's proxy into its slot
 * (readers still resolve to old; selector == 0).  Allocates the group block
 * lazily; on OOM it leaves the handle sticky (commit reports MEMORY_ERROR) and
 * parks nothing.  commit() drives this for nr >= 2; a white-box caller that
 * needs work BETWEEN install and the flip may call it directly.
 */
static inline
void urcu_txn_sw_install(struct urcu_txn_sw_txn *t)
{
	unsigned int i;

	urcu_txn_sw__excl_owner(t, "install()");
	/*
	 * Contract (see urcu_txn_sw_init_inline): a CALLER-storage handle must
	 * never park proxies -- its records do not outlive the call, and it
	 * owns no block to hang the proxies' group off.  Enforce it HERE.  The
	 * header claims "the engine asserts an inline buffer never reaches that
	 * path", but neither existing assert covers it: record()'s fires only
	 * on a grow (nr == cap), and commit()'s runs AFTER install has already
	 * stored.  Without this guard an inline handle with 2..cap records
	 * walks into the branch below -- !t->block is ALSO the normal state of
	 * every inline handle -- which repoints t->latches at a fresh block,
	 * DISCARDING the caller's records, and the park loop then
	 * release-stores tagged proxies through that block's UNINITIALIZED
	 * l->slot pointers: wild stores to garbage addresses.
	 */
	urcu_posix_assert(!t->latches_inline);
	if (caa_unlikely(!t->block)) {		/* white-box install with no record */
		/*
		 * The only way to be block-less on a heap handle: nothing was
		 * ever recorded or reserved.  Anything else would strand the
		 * records buffered in the array we are about to replace.
		 */
		urcu_posix_assert(!t->nr);
		t->block = urcu_txn_sw__block_alloc(URCU_TXN_SW_CAP);
		if (caa_unlikely(!t->block)) {
			t->state = URCU_TXN_SW_OOM;	/* sticky; nothing parked */
			return;
		}
		t->latches = t->block->latches;
		t->cap = t->block->cap;
	}
	/*
	 * Engine precondition: records target pairwise-distinct slots.
	 * record() has no same-slot reconcile, so a duplicate parks two proxies
	 * on one slot and settles both in record order -- silently last-wins.
	 * The record array is unsorted (record order is the embedder's), so
	 * pair-scan mirroring the concurrent engine's adjacent check after its
	 * sort.  Debug-only: no cost under NDEBUG, and sw transactions are
	 * small.
	 */
	for (i = 1; i < t->nr; i++) {
		unsigned int j;

		for (j = 0; j < i; j++)
			urcu_assert_debug(t->latches[i].slot != t->latches[j].slot);
	}
	t->state = URCU_TXN_SW_INSTALLED;
	for (i = 0; i < t->nr; i++) {
		urcu_txn_sw__excl_slot_free(t->latches[i].slot,
				t->latches[i].tag, "install (park)");
		urcu_txn_sw_latch_install(t, &t->latches[i]);
	}
}

/*
 * Commit: flip the group (every proxy resolves to new atomically), then settle
 * each slot to its direct new value.  commit() OWNS reclaim and consumes the
 * handle's allocations -- the caller must re-init the handle to reuse it.
 * Returns enum urcu_txn_status: MEMORY_ERROR if a reserve()/record()/group-
 * block alloc had failed (sticky), otherwise OK.  A single updater has no
 * contention, so ABORT is never returned.
 *
 * @call_rcu_fn is the reclaim deferral: when proxies are parked (nr >= 2), the
 * group block (carrying the record array) is handed to call_rcu_fn(block,
 * urcu_txn_sw_free_rcu) so it is freed after a grace period retires any proxy a
 * reader may still hold.  It has the flavor call_rcu signature, so an embedder
 * passes its RCU flavor's call_rcu directly -- letting a FLAVOR-AGNOSTIC
 * embedder (one that selects its flavor at runtime, e.g. through a
 * rcu_flavor_struct vtable: flavor->update_call_rcu) drive reclaim without this
 * header binding a compile-time flavor.  An embedder that KNOWS no reader can
 * hold a proxy (a single-threaded / exclusive build) may pass a synchronous
 * shim -- void f(head, func){ func(head); } -- to free in place with no grace
 * period.  The convenience wrapper urcu_txn_sw_commit() passes the
 * compile-time-selected call_rcu (hence its include-after-flavor requirement).
 *
 * Reclaim:
 *   - nr >= 2 (proxies parked): the group block (carrying the record array) is
 *     deferred through call_rcu_fn(.., urcu_txn_sw_free_rcu).
 *   - nr <= 1 / sticky OOM (no proxy ever published): the record array is freed
 *     at once (caller-owned inline storage is left untouched); no group block
 *     was allocated, and @call_rcu_fn is never invoked.
 *
 * Two PREPARE shortcuts let an embedder record then commit WITHOUT an explicit
 * urcu_txn_sw_install():
 *
 *   - Single edge (nr == 1): one recorded edge has no cross-edge atomicity to
 *     provide -- a lone release store to its slot IS already an atomic commit
 *     -- so no proxy is installed and no group block is allocated.  A reader of
 *     that slot observes the old or the new target directly, never a proxy, so
 *     none can be held: no grace period is owed and the record array is freed
 *     at once.  This makes the common one-pointer publish as cheap as a bare
 *     rcu_assign_pointer, with no proxy alloc / install / settle / GP reclaim.
 *
 *   - Multi-edge (nr >= 2): auto-install -- allocate the group block, park
 *     every proxy, then flip.  (A white-box caller that needs work BETWEEN
 *     install and the flip can call urcu_txn_sw_install() itself; commit then
 *     reuses the block it allocated and takes the call_rcu_fn reclaim path.)
 */
static inline
enum urcu_txn_status urcu_txn_sw_commit_flavor(struct urcu_txn_sw_txn *t,
		void (*call_rcu_fn)(struct rcu_head *,
			void (*)(struct rcu_head *)))
{
	struct urcu_txn_sw_block *blk;
	unsigned int i;

	urcu_txn_sw__excl_owner(t, "commit()");
	if (caa_unlikely(t->state == URCU_TXN_SW_OOM)) {
		urcu_txn_sw__free_records(t);
		return URCU_TXN_STATUS_MEMORY_ERROR;
	}
	if (t->state == URCU_TXN_SW_PREPARE) {
		if (t->nr <= 1) {
			if (t->nr == 1) {
				struct urcu_txn_sw_latch *l = &t->latches[0];

				urcu_txn_sw__excl_slot_unchanged(l);
				uatomic_store(l->slot, l->proxy.ptr[1],
						CMM_RELEASE);
			}
			/* nr == 0: empty txn, nothing published. */
			urcu_txn_sw__free_records(t);	/* no proxy: free now */
			return URCU_TXN_STATUS_OK;
		}
		urcu_txn_sw_install(t);	/* lazily allocs block, parks proxies */
		if (caa_unlikely(t->state == URCU_TXN_SW_OOM)) {
			urcu_txn_sw__free_records(t);
			return URCU_TXN_STATUS_MEMORY_ERROR;
		}
	}

	/*
	 * INSTALLED: proxies parked in the block, which carries the record
	 * array inline.  The GP free reclaims it, so it must be heap-owned: an
	 * inline (caller-storage) txn never parks proxies (record() asserts it
	 * cannot grow past its lone-edge bound), so nr >= 2 here implies a heap
	 * block.
	 */
	urcu_posix_assert(!t->latches_inline);
	blk = t->block;
	urcu_txn_sw_group_commit(&blk->group);
	for (i = 0; i < t->nr; i++) {
		struct urcu_txn_sw_latch *l = &blk->latches[i];

		urcu_txn_sw__excl_slot_ours(l);
		uatomic_store(l->slot, l->proxy.ptr[1], CMM_RELEASE);
	}
	call_rcu_fn(&blk->rcu_head, urcu_txn_sw_free_rcu);	/* a reader may hold a proxy */
	t->block = NULL;		/* handle consumed */
	t->latches = NULL;
	return URCU_TXN_STATUS_OK;
}

/*
 * Commit deferring reclaim through the compile-time-selected RCU flavor's
 * call_rcu (so this header must be included after an RCU flavor header).  A
 * thin wrapper over urcu_txn_sw_commit_flavor(); see it for the full contract.
 */
static inline
enum urcu_txn_status urcu_txn_sw_commit(struct urcu_txn_sw_txn *t)
{
	return urcu_txn_sw_commit_flavor(t, call_rcu);
}

#ifdef __cplusplus
}
#endif

#endif /* _URCU_RCU_TXN_SW_H */
