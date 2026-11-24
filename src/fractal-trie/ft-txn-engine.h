// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_TXN_ENGINE_H
#define _FT_TXN_ENGINE_H

/*
 * THE COMMIT ENGINE AS FT SEES IT: the parked-slot format.
 *
 * While a commit is between install and settle, each of its slots holds a
 * PROXY.  A proxy is a tagged pointer to a small engine object.  It
 * resolves to the slot's old or new value.  ONE decision, shared by every
 * proxy of that commit, selects which.  Readers resolve through that
 * decision.  A reader that needs two slots to agree compares the decisions
 * of the two proxies.
 *
 * FT reads parked slots only through this file, so that an engine swap
 * re-implements it and nothing else.  The engine is the single-writer one
 * (<urcu/rcu-txn-sw.h>): a proxy is a latch's struct urcu_txn_sw_proxy,
 * and its decision is the flip group's selector.  Latches are 16-byte
 * aligned, which FT's 0xF tag needs (FT_FLIP_PROXY_TAG).
 */

#define FT_TXN_INLINE	static inline __attribute__((always_inline))

/* A parked slot's proxy, with its tag stripped.  Opaque outside this file. */
struct ft_txn_parked;

/* The decision every proxy of one commit resolves through. */
struct ft_txn_decision;

/* Does @v, loaded from a slot parked under @tag, hold a proxy? */
FT_TXN_INLINE
int ft_txn_is_proxy(const void *v, uintptr_t tag)
{
	return urcu_txn_sw_is_proxy(v, tag);
}

/* The proxy @v holds; @v must be one (ft_txn_is_proxy). */
FT_TXN_INLINE
struct ft_txn_parked *ft_txn_parked_of(const void *v, uintptr_t tag)
{
	return (struct ft_txn_parked *) ((uintptr_t) v & ~tag);
}

FT_TXN_INLINE
const struct ft_txn_decision *ft_txn_parked_decision(
		const struct ft_txn_parked *p)
{
	return (const struct ft_txn_decision *)
		((const struct urcu_txn_sw_proxy *) p)->group;
}

/*
 * Has the commit behind @d taken effect?  Acquire: a true answer makes the
 * new values' targets visible.  False while the commit is still in flight,
 * and after it failed.
 */
FT_TXN_INLINE
bool ft_txn_decision_committed(const struct ft_txn_decision *d)
{
	return uatomic_load(&((const struct urcu_txn_sw_group *) d)->selector,
			CMM_ACQUIRE) != 0;
}

/* The value @p stands for under a decision the caller already read. */
FT_TXN_INLINE
void *ft_txn_parked_value(const struct ft_txn_parked *p, bool committed)
{
	return ((const struct urcu_txn_sw_proxy *) p)->ptr[committed];
}

/* The value @p stands for now. */
FT_TXN_INLINE
void *ft_txn_parked_resolve(const struct ft_txn_parked *p)
{
	return urcu_txn_sw_proxy_get((const struct urcu_txn_sw_proxy *) p);
}

/* A value loaded from a slot parked under @tag, proxy resolved. */
FT_TXN_INLINE
void *ft_txn_resolve(const void *v, uintptr_t tag)
{
	if (caa_likely(!ft_txn_is_proxy(v, tag)))
		return (void *) (uintptr_t) v;
	return ft_txn_parked_resolve(ft_txn_parked_of(v, tag));
}

/* Acquire-load @slot, parked under @tag, and resolve it. */
FT_TXN_INLINE
void *ft_txn_read(void **slot, uintptr_t tag)
{
	return ft_txn_resolve(uatomic_load(slot, CMM_ACQUIRE), tag);
}

/*
 * Bit 0 of a pointer word marks a parked proxy.  The ordered-cell list
 * links and the duplicate chain (FT_HLIST_TAG) use it.  It is the bit
 * <urcu/rcu-txn-sw-list.h> uses for its own links.
 */
#define FT_TXN_TAG	1UL
urcu_static_assert(FT_TXN_TAG == URCU_TXN_SW_LIST_PROXY_TAG,
	"FT_TXN_TAG is the SW list's proxy tag", ft_txn_tag_is_list_tag);

/*
 * The ordered-cell list uses the links of <urcu/rcu-txn-sw-list.h>, plus
 * a logical-deletion mark: bit 1 of a deleted node's next link.
 */
#define FT_LIST_MARK	2UL

static inline
void *ft_list_set_mark(struct urcu_txn_sw_list_node *n)
{
	return (void *) ((uintptr_t) n | FT_LIST_MARK);
}

static inline
int ft_list_is_marked(void *v)
{
	return (int) ((uintptr_t) v & FT_LIST_MARK);
}

static inline
struct urcu_txn_sw_list_node *ft_list_unmark(void *v)
{
	return (struct urcu_txn_sw_list_node *)
			((uintptr_t) v & ~(uintptr_t) FT_LIST_MARK);
}

/*
 * Resolve a link loaded from an ordered-cell list slot: resolve a parked
 * proxy through the engine, and strip the mark.
 */
static inline
struct urcu_txn_sw_list_node *ft_txn_list_resolve(void *raw)
{
	if (caa_unlikely((uintptr_t) raw & (FT_TXN_TAG | FT_LIST_MARK)))
		return ft_list_unmark(ft_txn_resolve(raw, FT_TXN_TAG));
	return (struct urcu_txn_sw_list_node *) raw;
}

/* The resolved next and previous links of @node, in a read-side section. */
static inline
struct urcu_txn_sw_list_node *ft_txn_list_next_rcu(
		struct urcu_txn_sw_list_node *node)
{
	return ft_txn_list_resolve((void *) rcu_dereference(node->next));
}

static inline
struct urcu_txn_sw_list_node *ft_txn_list_prev_rcu(
		struct urcu_txn_sw_list_node *node)
{
	return ft_txn_list_resolve((void *) rcu_dereference(node->prev));
}

/*
 * FT's txn handle.  It wraps the engine's handle, so that an engine swap
 * changes the member and not every function that passes a handle.
 *
 * It also counts what an attempt costs: the loads it made through the
 * handle, plus the edges it recorded.  @cost keeps the highest cost of
 * the attempts that ended so far.  An op reads it to size the number of
 * attempts it may lose before it takes the lane (ft_op_lane_at).
 */
struct ft_txn {
	struct urcu_txn_sw_txn e;	/* the engine's handle */
	unsigned int nload;		/* loads by the current attempt */
	unsigned int cost;		/* highest cost of an ended attempt */
};

/* Initialize @h, with no attempt and no cost yet. */
FT_TXN_INLINE
void ft_txn_init(struct ft_txn *h)
{
	urcu_txn_sw_init(&h->e);
	h->nload = 0;
	h->cost = 0;
}

/*
 * Start an attempt on @h: no record, no out-of-memory mark, and the
 * attempt's loads count afresh.  The previous attempt discarded or
 * committed its records.
 */
FT_TXN_INLINE
void ft_txn_attempt_begin(struct ft_txn *h)
{
	urcu_txn_sw_discard(&h->e);
	urcu_txn_sw_init(&h->e);
	h->nload = 0;
}

/*
 * THE TXN'S OWN RECORDS.  FT reads what a txn recorded and never edits it.
 * A struct ft_txn_rec is one recorded edge, read through the accessors
 * below.  Lookups take the HANDLE, not the engine's record array.  A record
 * or a reservation that grows the array moves it.  A pointer to the array
 * taken earlier then names the old block (6f774617).
 */
struct ft_txn_rec;

/* Does the txn behind @h hold a record set (not empty, not out of memory)? */
FT_TXN_INLINE
bool ft_txn_live(const struct ft_txn *h)
{
	return h && h->e.latches && h->e.state != URCU_TXN_SW_OOM;
}

/* The txn's record on @slot, or NULL.  @h may be NULL. */
FT_TXN_INLINE
const struct ft_txn_rec *ft_txn_find(struct ft_txn *h, void **slot)
{
	if (!ft_txn_live(h))
		return NULL;
	return (const struct ft_txn_rec *) urcu_txn_sw_find(&h->e, slot);
}

/* Has the txn behind @h recorded an edge on @slot? */
FT_TXN_INLINE
bool ft_txn_recorded(struct ft_txn *h, void **slot)
{
	return ft_txn_find(h, slot) != NULL;
}

/* A record's expected old value. */
FT_TXN_INLINE
void *ft_txn_rec_old(const struct ft_txn_rec *r)
{
	return ((const struct urcu_txn_sw_latch *) r)->proxy.ptr[0];
}

/* A record's new value: what the slot holds once the txn commits. */
FT_TXN_INLINE
void *ft_txn_rec_new(const struct ft_txn_rec *r)
{
	return ((const struct urcu_txn_sw_latch *) r)->proxy.ptr[1];
}

/*
 * Was the edge recorded SW (a park under the holder's lock) rather than MW
 * (installed by a CAS against its expected old)?  Always, under the SW
 * engine.
 */
FT_TXN_INLINE
bool ft_txn_rec_sw(const struct ft_txn_rec *r)
{
	(void) r;
	return true;
}

/* The slot a record writes, and that slot's proxy tag. */
FT_TXN_INLINE
void **ft_txn_rec_slot(const struct ft_txn_rec *r)
{
	return ((const struct urcu_txn_sw_latch *) r)->slot;
}

FT_TXN_INLINE
uintptr_t ft_txn_rec_tag(const struct ft_txn_rec *r)
{
	return ((const struct urcu_txn_sw_latch *) r)->tag;
}

/* How many edges the txn behind @h has recorded. */
FT_TXN_INLINE
unsigned int ft_txn_nr_records(const struct ft_txn *h)
{
	return ft_txn_live(h) ? h->e.nr : 0;
}

/* Record @i of the txn behind @h, for i < ft_txn_nr_records(@h). */
FT_TXN_INLINE
const struct ft_txn_rec *ft_txn_rec_at(const struct ft_txn *h, unsigned int i)
{
	return (const struct ft_txn_rec *) &h->e.latches[i];
}

/*
 * Where the txn behind @h stands.  EMPTY: no record set (none yet, or out of
 * memory).  A record set is UNDECIDED until its commit decides it.  The
 * commit consumes the record set, so a handle never reads COMMITTED or
 * FAILED.
 */
enum ft_txn_state {
	FT_TXN_EMPTY,
	FT_TXN_UNDECIDED,
	FT_TXN_COMMITTED,
	FT_TXN_FAILED,
};

FT_TXN_INLINE
enum ft_txn_state ft_txn_state(const struct ft_txn *h)
{
	return ft_txn_live(h) ? FT_TXN_UNDECIDED : FT_TXN_EMPTY;
}

/*
 * Drop the txn's record set without committing it.  An out-of-memory mark
 * stays, so the failure still surfaces at the commit.
 */
FT_TXN_INLINE
void ft_txn_discard(struct ft_txn *h)
{
	if (h)
		urcu_txn_sw_discard(&h->e);
}

/*
 * Pre-size the record set of @h for @cap edges.  Return false when out of
 * memory.  The failure is sticky, and the commit reports it.
 */
FT_TXN_INLINE
bool ft_txn_reserve(struct ft_txn *h, unsigned int cap)
{
	return urcu_txn_sw_reserve(&h->e, cap);
}


/*
 * The record and commit entry points call the debug instruments of
 * ft-txn-rec-dbg.h.  Only fractal-trie.c includes that header, and only it
 * records or commits.  The allocator's unit sees the rest of this file.
 */
#ifdef _FT_TXN_REC_DBG_H
/* Fill the debug instruments' view @v of record @r of @h. */
FT_TXN_INLINE
void ft_txn_rec_dbg_view(const struct ft_txn *h, const struct ft_txn_rec *r,
		struct ft_rec_dbg *v)
{
	v->slot = ft_txn_rec_slot(r);
	v->old_ptr = ft_txn_rec_old(r);
	v->new_ptr = ft_txn_rec_new(r);
	v->tag = ft_txn_rec_tag(r);
	v->txn = h;
}

/*
 * Record the edge {*slot: old -> new} under @tag.  A second record on the
 * same slot merges onto the first: it keeps the first old value and takes
 * the new one.  Return false when out of memory.  The failure is sticky,
 * and the commit reports it.
 */
FT_TXN_INLINE
bool ft_txn_record(struct ft_txn *h, void **slot, void *old_ptr,
		void *new_ptr, uintptr_t tag)
{
	bool merged = FT_REC_DBG && ft_txn_recorded(h, slot);

	if (!urcu_txn_sw_record_chain(&h->e, slot, old_ptr, new_ptr, tag))
		return false;
	if (FT_REC_DBG) {
		struct ft_rec_dbg v;

		ft_txn_rec_dbg_view(h, ft_txn_find(h, slot), &v);
		ft_rec_dbg_recorded(&v, merged);
	}
	return true;
}

/*
 * Commit the records of @h, deferring reclaim through @reclaim.  Return OK,
 * or MEMORY_ERROR when a record could not be made.  The commit consumes the
 * records, and @h is then empty for the next txn of the op.
 */
FT_TXN_INLINE
enum urcu_txn_status ft_txn_commit(struct ft_txn *h,
		void (*reclaim)(struct rcu_head *, void (*)(struct rcu_head *)))
{
	if (FT_REC_DBG) {
		unsigned int i, nr = ft_txn_nr_records(h);

		for (i = 0; i < nr; i++) {
			struct ft_rec_dbg v;

			ft_txn_rec_dbg_view(h, ft_txn_rec_at(h, i), &v);
			ft_rec_dbg_will_commit(&v);
		}
	}
	{
		enum urcu_txn_status st = urcu_txn_sw_commit_flavor(&h->e,
				reclaim);

		urcu_txn_sw_init(&h->e);
		return st;
	}
}
#endif	/* _FT_TXN_REC_DBG_H */

/*
 * The current attempt of @h ends: keep its cost if it is the highest so
 * far.  Call it when the attempt is lost, and before a commit of a live
 * record set.
 */
FT_TXN_INLINE
void ft_txn_learn_cost(struct ft_txn *h)
{
	unsigned int c = h->nload + ft_txn_nr_records(h);

	if (c > h->cost)
		h->cost = c;
}

/*
 * LOADS INSIDE A TXN.  ft_txn_load() returns @slot as the txn behind @h will
 * leave it.  That is its own pending value when it recorded @slot, else the
 * slot's committed value.  ft_txn_load_committed() skips the pending value.
 *
 * A txn loads only words the op holds, so no other commit can have a
 * proxy parked there.  The engine's load asserts that under DEBUG_RCU.
 * -DFT_DEBUG_FOREIGN_PARKED counts, per call site and tag, the loads that
 * found one anyway.  ft_txn_load_committed() reads through ft_txn_read(),
 * which resolves a proxy instead.
 */
#ifdef FT_DEBUG_FOREIGN_PARKED
# include <stdio.h>

struct ft_fp_site {
	uint64_t key;		/* 0: free; claimed once by cmpxchg */
	const char *file;
	int line;
	uintptr_t tag;
	unsigned long n;
};

# define FT_FP_SITES	128
static struct ft_fp_site ft_fp_sites[FT_FP_SITES];
static unsigned long ft_fp_loads, ft_fp_parked, ft_fp_dropped;
/* ft_txn_parent_slot_at with a txn: snapshot vs read-your-own-writes path */
static unsigned long ft_fp_ps_snapshot, ft_fp_ps_ryw;

static inline
void ft_fp_note(struct ft_txn *h, void **slot, uintptr_t tag,
		const char *file, int line)
{
	void *raw = uatomic_load(slot, CMM_RELAXED);
	uint64_t key;
	unsigned int i, at;

	uatomic_inc(&ft_fp_loads);
	if (!ft_txn_is_proxy(raw, tag))
		return;
	if (ft_txn_recorded(h, slot))
		return;			/* recorded: the load returns our own value */
	uatomic_inc(&ft_fp_parked);
	key = ((uint64_t) (uintptr_t) file << 12) ^ ((uint64_t) line << 44) ^
		(uint64_t) tag ^ 1;
	at = (unsigned int) ((key ^ (key >> 29)) % FT_FP_SITES);
	for (i = 0; i < FT_FP_SITES; i++) {
		struct ft_fp_site *s = &ft_fp_sites[(at + i) % FT_FP_SITES];
		uint64_t k = uatomic_load(&s->key, CMM_ACQUIRE);

		if (!k && uatomic_cmpxchg(&s->key, 0, key) == 0) {
			s->file = file;
			s->line = line;
			s->tag = tag;
			k = key;
		}
		if (k == key) {
			uatomic_inc(&s->n);
			return;
		}
	}
	uatomic_inc(&ft_fp_dropped);
}

static void ft_fp_report(void) __attribute__((destructor));
static void ft_fp_report(void)
{
	unsigned int i;

	if (!ft_fp_loads)
		return;
	fprintf(stderr, "FT FOREIGN PARKED: %lu txn loads, %lu found another "
		"commit's proxy on a slot they did not record (%lu unsited)\n",
		ft_fp_loads, ft_fp_parked, ft_fp_dropped);
	fprintf(stderr, "  parent-slot derivations in a txn: %lu snapshot, "
		"%lu read-your-own-writes\n", ft_fp_ps_snapshot, ft_fp_ps_ryw);
	for (i = 0; i < FT_FP_SITES; i++)
		if (ft_fp_sites[i].n)
			fprintf(stderr, "  %12lu  tag 0x%lx  %s:%d\n",
				ft_fp_sites[i].n,
				(unsigned long) ft_fp_sites[i].tag,
				ft_fp_sites[i].file, ft_fp_sites[i].line);
}

FT_TXN_INLINE
void *ft_txn_load_at(struct ft_txn *h, void **slot, uintptr_t tag,
		bool committed, const char *file, int line)
{
	ft_fp_note(h, slot, tag, file, line);
	h->nload++;
	return committed ? ft_txn_read(slot, tag) :
		urcu_txn_sw_load(&h->e, slot, tag);
}
# define ft_txn_load(h, slot, tag)					\
	ft_txn_load_at((h), (slot), (tag), false, __FILE__, __LINE__)
# define ft_txn_load_committed(h, slot, tag)				\
	ft_txn_load_at((h), (slot), (tag), true, __FILE__, __LINE__)
#else
FT_TXN_INLINE
void *ft_txn_load(struct ft_txn *h, void **slot, uintptr_t tag)
{
	h->nload++;
	return urcu_txn_sw_load(&h->e, slot, tag);
}

FT_TXN_INLINE
void *ft_txn_load_committed(struct ft_txn *h, void **slot, uintptr_t tag)
{
	h->nload++;
	return ft_txn_read(slot, tag);
}
#endif

#endif /* _FT_TXN_ENGINE_H */
