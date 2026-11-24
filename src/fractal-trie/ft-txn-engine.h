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
 * re-implements it and nothing else.  The two engines:
 *   - Here it is the MCAS engine (<urcu/rcu-txn.h>).  A proxy is
 *     (struct urcu_txn_record * | tag).  Its decision is the record's
 *     descriptor status word.
 *   - In the SW engine (<urcu/rcu-txn-sw.h>) a proxy is a latch's proxy.
 *     Its decision is the flip group's selector.
 * Both are 16-byte aligned, which FT's 0xF tag needs (FT_FLIP_PROXY_TAG).
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
	return urcu_txn_is_proxy((void *) (uintptr_t) v, tag);
}

/* The proxy @v holds; @v must be one (ft_txn_is_proxy). */
FT_TXN_INLINE
struct ft_txn_parked *ft_txn_parked_of(const void *v, uintptr_t tag)
{
	return (struct ft_txn_parked *) urcu_txn_untag((void *) (uintptr_t) v,
			tag);
}

FT_TXN_INLINE
const struct ft_txn_decision *ft_txn_parked_decision(
		const struct ft_txn_parked *p)
{
	return (const struct ft_txn_decision *)
		((const struct urcu_txn_record *) p)->desc;
}

/*
 * Has the commit behind @d taken effect?  Acquire: a true answer makes the
 * new values' targets visible.  False while the commit is still in flight,
 * and after it failed.
 */
FT_TXN_INLINE
bool ft_txn_decision_committed(const struct ft_txn_decision *d)
{
	return urcu_txn_desc_status((const struct urcu_txn_desc *) d) ==
		URCU_TXN_DESC_SUCCEEDED;
}

/* The value @p stands for under a decision the caller already read. */
FT_TXN_INLINE
void *ft_txn_parked_value(const struct ft_txn_parked *p, bool committed)
{
	const struct urcu_txn_record *r = (const struct urcu_txn_record *) p;

	return committed ? r->new_ptr : r->old_ptr;
}

/* The value @p stands for now. */
FT_TXN_INLINE
void *ft_txn_parked_resolve(const struct ft_txn_parked *p)
{
	return urcu_txn_resolve_record((struct urcu_txn_record *)
			(uintptr_t) p);
}

/* A value loaded from a slot parked under @tag, proxy resolved. */
FT_TXN_INLINE
void *ft_txn_resolve(const void *v, uintptr_t tag)
{
	return urcu_txn_resolve((void *) (uintptr_t) v, tag);
}

/* Acquire-load @slot, parked under @tag, and resolve it. */
FT_TXN_INLINE
void *ft_txn_read(void **slot, uintptr_t tag)
{
	return urcu_txn_read(slot, tag);
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
	struct urcu_txn e;		/* the engine's handle */
	unsigned int nload;		/* loads by the current attempt */
	unsigned int cost;		/* highest cost of an ended attempt */
};

/* Initialize @h, with no attempt and no cost yet. */
FT_TXN_INLINE
void ft_txn_init(struct ft_txn *h)
{
	urcu_txn_init(&h->e, NULL);
	h->nload = 0;
	h->cost = 0;
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
	return h && h->e.desc && h->e.desc != URCU_TXN_ENOMEM;
}

/* The txn's record on @slot, or NULL.  @h may be NULL. */
FT_TXN_INLINE
const struct ft_txn_rec *ft_txn_find(struct ft_txn *h, void **slot)
{
	if (!ft_txn_live(h))
		return NULL;
	return (const struct ft_txn_rec *) urcu_txn_find(h->e.desc, slot);
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
	return ((const struct urcu_txn_record *) r)->old_ptr;
}

/* A record's new value: what the slot holds once the txn commits. */
FT_TXN_INLINE
void *ft_txn_rec_new(const struct ft_txn_rec *r)
{
	return ((const struct urcu_txn_record *) r)->new_ptr;
}

/*
 * Was the edge recorded SW (a park under the holder's lock) rather than MW
 * (installed by a CAS against its expected old)?  Always, under the SW
 * engine.
 */
FT_TXN_INLINE
bool ft_txn_rec_sw(const struct ft_txn_rec *r)
{
	return ((const struct urcu_txn_record *) r)->kind == URCU_TXN_KIND_SW;
}

/* How many edges the txn behind @h has recorded. */
FT_TXN_INLINE
unsigned int ft_txn_nr_records(const struct ft_txn *h)
{
	return ft_txn_live(h) ? h->e.desc->nr : 0;
}

/*
 * Where the txn behind @h stands.  EMPTY: no record set (none yet, or out of
 * memory).  A record set is UNDECIDED until its commit decides it.
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
	unsigned long st;

	if (!ft_txn_live(h))
		return FT_TXN_EMPTY;
	st = urcu_txn_desc_status(h->e.desc);
	if (st == URCU_TXN_DESC_UNDECIDED)
		return FT_TXN_UNDECIDED;
	return st == URCU_TXN_DESC_SUCCEEDED ? FT_TXN_COMMITTED :
		FT_TXN_FAILED;
}

/*
 * Drop the txn's record set without committing it.  An out-of-memory mark
 * stays, so the failure still surfaces at the commit.
 */
FT_TXN_INLINE
void ft_txn_discard(struct ft_txn *h)
{
	if (ft_txn_live(h)) {
		urcu_txn_destroy(h->e.desc);
		h->e.desc = NULL;
	}
}

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
 * A slot this txn did not record may hold ANOTHER commit's proxy, between
 * that commit's install and its settle.  The MCAS engine resolves it (and
 * waits while the other commit is undecided).  The SW engine's load assumes
 * a single updater and asserts that no such proxy exists.  So
 * -DFT_DEBUG_FOREIGN_PARKED counts, per call site and tag, the loads that
 * found one.
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
	if (!urcu_txn_is_proxy(raw, tag))
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
	return committed ? urcu_txn_load_committed(&h->e, slot, tag) :
		urcu_txn_load(&h->e, slot, tag);
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
	return urcu_txn_load(&h->e, slot, tag);
}

FT_TXN_INLINE
void *ft_txn_load_committed(struct ft_txn *h, void **slot, uintptr_t tag)
{
	h->nload++;
	return urcu_txn_load_committed(&h->e, slot, tag);
}
#endif

#endif /* _FT_TXN_ENGINE_H */
