// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-rekey.h
 *
 * Userspace RCU library - Fractal Trie: the REKEY (same-trie move) writer.
 *
 * A rekey moves a subtree from one key to another WITHIN ONE TRIE.  Its source
 * and its destination are the same live structure, which readers use
 * concurrently.  The cross-trie graft/merge writers in ft-graft.h and
 * ft-merge.h differ: their entry gates REQUIRE an exclusive source
 * (src_ft != dst_ft && !src_ft->exclusive -> BUSY_ERROR).  So they have no
 * concurrent peer on the src side at all.
 *
 * The two used to share ft_merge_at_inner, ft_merge_spine_copy and
 * ft_merge_graft_subpos_inplace.  That was 599 lines of worker code carrying
 * ZERO tests on @rekey or on src_ft == dst_ft.  So the stricter contract
 * served both callers, unconditionally and invisibly.  This unit holds the
 * rekey's own copies: ft_rekey_at_inner, ft_rekey_spine_copy and
 * ft_rekey_subpos_inplace.  They were split off so that each side can be
 * specialised against its single contract.
 *
 * Functions that only compute SHAPE carry no atomicity contract and stay
 * SHARED in ft-merge.h: ft_merge_build, ft_merge_count,
 * ft_merge_ord_interleave_collect and the small helpers.  The boundary is
 * exactly "does it COMMIT, DRAIN, or CREATE A TXN".  That is the only thing
 * the two callers disagree about.
 *
 * Implementation unit: #included once into the fractal-trie.c translation
 * unit, AFTER ft-merge.h and ft-mutation-node.h (it uses their shared
 * helpers).  Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-rekey.h is an implementation unit; #include it from fractal-trie.c only"
#endif


/* ------------- moved from ft-mutation-node.h ------------- */

/*
 * ☞ WHAT ACTUALLY GUARANTEES READER COHERENCE FOR A MOVE.  It is NOT this
 * function.  The coherent reader validates a lookup with TWO descents and a
 * fold of the node ADDRESSES visited (ft_lookup_two_descents, ft-lookup.h;
 * folds at ft-descent.h).  That is sound only if every move perturbs the fold.
 * The RECOMPACT INVARIANTS already guarantee the perturbation, on the
 * DESTINATION side:
 *  - An occupancy ADD relocates the attach parent (-ERANGE on all four layout
 *    arms of _ft_node_set_nth, pigeon included).
 *  - On the merge arm, it relocates ft_merge_build's union node.  This is
 *    CHECKED at that arm's publish, not argued from construction: a
 *    compressed S_top is admitted there now, and the shared-run collapse can
 *    hand back a live node.
 * The src side relocates too (a delete recompacts, -EFBIG), but it is not
 * load-bearing.
 * The single exception is the bulk reserves' ft_in_place_excl_ok =
 * FEATURE_FT_INSERT_IN_PLACE && ft->exclusive.  An exclusive trie has NO
 * CONCURRENT READERS by contract.  (The POINT ops' in-place tier,
 * ft_in_place_ok, is wider.  But no bulk reserve or detach vouches it: a
 * move's dst attach parent relocates on a shared trie regardless.)
 *
 * So the fresh address this function gives @stop is REDUNDANT FOR THE WITNESS.
 * It is still needed for everything else below:
 *  - re-parenting @stop's children;
 *  - marking the children whose state words the commit parks into;
 *  - retiring @stop;
 *  - manufacturing a tail for a CUT source (@cut), which has no node at that
 *    depth to copy.
 * Read the "@stop is retired WHOLE" contract and the SCOPE note with that
 * split in mind.  See the ☠☠ warning at the pigeon -ERANGE arm
 * (ft-mutation-node.h) before assuming the destination-side guarantee is
 * free.
 *
 * ★ IT WILL NOT STAY FREE.  In-place mutation without a COW is a PLANNED
 * direction.  When it is re-allowed, the rekey must be SPECIAL-CASED to COW
 * the destination attach parent ITSELF, not inherit the copy from the
 * mutation path.  A publish-time assert must then check that this op
 * superseded the attach node, so the obligation is checked and not merely
 * intended.  Until then, this function's copy is what a CUT source and the
 * child duties need.  The destination side is what the reader needs.
 *
 * ft_rekey_cow_stop: identity-preserving COW of a LIVE published node
 * @stop_flag (the "S_top" of a same-trie rekey).  It does three things:
 *  - it makes a FRESH-address copy, returned UNPUBLISHED in *@stop_prime_ret;
 *  - it re-parents @stop's direct children onto the copy;
 *  - it RETIRES @stop.
 * All of these are records in the caller's mixed SW/MW commit @txn.  The
 * interior below @stop is SHARED (same child addresses), so only @stop's
 * identity moves.  That is exactly what the coherent reader's two-descent
 * address-witness needs to detect a move.  It mirrors ft_node_recompact's
 * RELOCATE copy/reparent core, with two differences:
 *  (a) It returns the copy for the CALLER to place.  A rekey publishes it at
 *      the dst junction.  The isolation test publishes it at @stop's own
 *      parent slot (a same-position in-place clone).
 *  (b) It drives the SW mixed engine.  @txn has structural_sw set, so the
 *      re-parents' parent-pointer + pso edges and the retire park SW (locked,
 *      cannot fail).
 *
 * SW SAFETY (why the parks cannot be clobbered):
 *  - @stop's lock owns the parent-pointer edge: no peer re-homes a child of a
 *    LOCK-held parent.
 *  - The re-parent's §4.B guard edge and the retire both park the STATE word.
 *    ft_meta_nr_child_inc CASes that word IGNORING LOCK.  So each such node
 *    is LOCK-MARKED here: @stop for the retire, every metadata-bearing child
 *    for its guard.  A peer's count CAS now HONORS the mark and spins
 *    (FT_STATE_INPLACE_WAIT_MASK) until this commit settles the word.
 * The interior stays SHARED, so children of children need no marks.
 *
 * ☠ RELEASE ATTRIBUTION -- do not re-derive.  ft_reparent_record_meta's
 * {live_state -> live_state} STATE edge releases each child's mark.  Its
 * live_state has LOCK masked out, and the edge is recorded UNCONDITIONALLY.
 * The retire consumes @stop's mark.  The pso edge does NOT release the mark.
 * Since @118245b0 the pso is its own word, and its edge is recorded only when
 * the slot index CHANGES.  A release resting on it would leak a permanent
 * LOCK on every child that lands at the same index.  The pre-§8.3 story (pso
 * rode the state word, the pso edge did the masking) reads plausibly and is
 * wrong.
 *
 * CALLER CONTRACT:
 *  - ft->lock_fine, and @txn armed structurally by the caller at CREATION
 *    (ft_flip_txn_arm_structural).  This function adds the PER-OP arm at its
 *    own stop fence, which is additive and is what carries the audit.
 *  - @marks / @snaps: caller-owned scratch of >= FT_ENTRY_PER_NODE + 1 entries;
 *    *@nr_marks returns the count recorded (published incrementally so a bail is
 *    covered).  These marks are NOT txn-registered (count can exceed
 *    FT_FLIP_TXN_MAX_LOCKS), so the CALLER MUST, after committing OR aborting
 *    @txn, sweep ft_meta_lock_release_if_held over marks[0..*@nr_marks) -- a
 *    no-op for the LOCK a commit consumed via the retire / pso SW edges, a
 *    release for any still held on abort (the ft_detach_node orphan-chain
 *    pattern).
 *  - On success (0): *@stop_prime_ret is the fresh UNPUBLISHED copy.  Before
 *    the commit, the caller wires its parent back-edge and records the
 *    forward publish into @txn.  A real rekey also clears @stop's src slot
 *    there.  Once the commit succeeds, the caller frees the OLD @stop via
 *    call_rcu after the grace period.
 *  - On failure (<0: -EAGAIN re-descend / -ENOMEM): *@stop_prime_ret is NULL.
 *    The unpublished copy (if any) is already freed and nothing is published.
 *    The caller still sweeps @marks.
 *
 * SCOPE: @stop is an internal POPCOUNT / PIGEON node with no co-located external
 * list, or a PLAIN COMPRESSED one.  [TODO: an external-head S_top, and a
 * co-located external list, still need recompact's extra arms.]
 *
 * THE COMPRESSED ARM IS THE SAME SHAPE WITH ONE CHILD.  A compressed node
 * carries a key-byte run and a single child.  So the copy is len + key_bytes
 * + that child, and the mark/re-parent sweep runs once instead of over a
 * bitmap.
 *
 * @cut CUTS that run: the copy carries key_bytes[@cut .. len) instead of the
 * whole span.  A src key ENDING INSIDE the run needs this: the moved top is
 * the run's tail, manufactured here because no node exists at that depth.
 * The child is unchanged: a run has exactly one, and everything below the cut
 * is everything the run held.  So the mark and re-parent are the same single
 * edge, and @stop is retired WHOLE exactly as an uncut copy retires it.  @cut
 * is 0 for every other caller and must be < len.  The bitmap arms reject a
 * non-zero @cut, because they have no run to cut.  Three things are
 * particular to this arm:
 *  - The CHILD COUNT is metadata, so setting ->child does not maintain it the
 *    way the bitmap arms' set_nth calls do.  cds_ft_verify cross-checks it.
 *  - The caller gets the PLAIN node flag back.  The skip form cannot be
 *    inverted before the commit: it names the child, and the node is
 *    recovered through that child's back-pointer, which still names the
 *    original.  So a caller that frees the copy on a later bail would free
 *    the live original.
 *  - A SKIP-encoded @stop never arrives.  The encoding is a group property
 *    that EAGER clears, and a rekey requires EAGER.  Asserted, not handled.
 */
/*
 * Lock one child of the COW'd stop node -- the §7.2 fan-out.  Every child of
 * one node sits at the SAME byte-depth.  So under a coarse spacing there are
 * two cases:
 *  - they all share one anchor on the path above.  This op frequently holds
 *    it already, so the collapse is 256 -> 0;
 *  - each child is its own anchor, and no two can collide.
 * Either way the dedupe in the choke point decides it.  No set is needed
 * here.
 */
static inline
int ft_rekey_cow_lock_child(const struct cds_ft *ft,
		const struct ft_lock_ctx *ctx, struct cds_ft_inode_flag *child,
		struct cds_ft_metadata *cm, unsigned int child_depth,
		struct ft_held_anchor *held)
{
	return ft_acquire_member(ft, ctx, child, cm, child_depth, held);
}

/*
 * Does this op hold @cm's OWN word, or did coarsening land the acquire on an
 * ANCHOR above it?  @h is the mark ft_rekey_cow_lock_child just took for @cm.
 *
 * This predicate decides the re-parent's state-edge KIND.  It is the SAME one
 * ft_glue_acquire_reparent_marks computes for @held_lock.  See the law
 * written there (ft-mutation-helpers.h): the SW park is a plain store that
 * validates nothing.  It is legal only because the op holds the very word it
 * parks.  Suppose the op holds an ANCESTOR's word instead.  Then a peer's
 * ft_meta_nr_child_inc from an insert BELOW the child sees a CLEAN word and
 * does not honour FT_STATE_INPLACE_WAIT_MASK.  The park CLOBBERS the peer's
 * count.  So a coarsened member keeps the MW guard and lets the peer abort
 * the commit.
 *
 * ☞ Under per-node locking, anchor and node are ONE word.  So this is true
 * whenever a mark was taken, and the arms below are byte-identical to the
 * unconditional form they replace.
 */
static inline
bool ft_rekey_mark_holds_child_word(const struct ft_held_anchor *h,
		const struct cds_ft_metadata *cm)
{
	return h->lock == cm || h->node_held;
}

/* Defined below; ft_rekey_cow_stop hands it the marks it takes. */
static
void ft_rekey_marks_to_txn(struct ft_flip_txn *txn,
		struct ft_held_anchor *marks, unsigned int nr_marks);

/*
 * THE COPY MUST SHOW THIS COMMIT'S OWN WRITES (@ryw).
 *
 * ft_rekey_cow_stop copies a node's slots as the trie has them and re-parents
 * the children it finds.  That is the right reading when the op has not
 * touched the node yet.  That covers the src S_top, the debug root clone, and
 * the merge arm's COW of a parent OFF the source path.
 *
 * It is the wrong reading for a node the op's DETACH has already edited in
 * this same txn.  There, the records are in the descriptor and the words are
 * not.  A verbatim copy would carry the child the detach retired.  And the
 * detach's store would land in the body this COW retires.  Every source path
 * crosses the ROOT, so a root frame is always that case.  A frame the src
 * key passes THROUGH on its way to a deeper junction is the same case one
 * level down.
 *
 * So, with @ryw, the copy loop asks the DESCRIPTOR first.
 * ft_flip_txn_record_count_parent applies the same reading to the count
 * chain ("any lane that records its relocation into the shared txn BEFORE
 * this walk runs is covered").  The re-parent sweep then dispatches on what
 * the descriptor already says about each child:
 *
 *  1. a GLUE-BUILT top folded into the slot (the pending forward publish,
 *     announced but not yet recorded): unpublished, so a plain store wires it.
 *  2. a node the caller NAMES as an earlier step's unpublished product
 *     (@fresh: the detach's junction copy, its collapse's merged run):
 *     plain store.  Named, not inferred: "no record yet" is an ordering
 *     fact, "unpublished" is what makes a plain store legal.
 *  3. a child this txn already RE-HOMED (its parent word carries a record)
 *     or already HOLDS (an earlier step's fence).  Chain onto that step's
 *     record, with its kind and its PENDING value as the expected old.  (A
 *     load on the first attempt reads the live word, not the record, and
 *     the chain would poison.)  Take no mark of our own and owe no second
 *     terminal.
 *  4. anything else: the ordinary path.  Mark it, then SW records.  An
 *     external child whose back-channel this txn already recorded (a head
 *     the detach promoted into the frame's slot) chains the same way, by its
 *     record.
 *
 * A SKIP word is decoded through the descriptor too
 * (ft_count_walk_survivor_meta).  Its one-hop inversion runs through the
 * child's back-pointer.  Until this commit flips, that back-pointer still
 * names the node the detach replaced.
 *
 * @ryw false is byte-neutral: the descriptor is never consulted.
 */
/*
 * The nodes an EARLIER step of this op built and left unpublished under the
 * frame: the detach's fresh junction copy, the run its collapse merged.  The
 * caller names them POSITIVELY, so that "no record on its parent word" is
 * never mistaken for "unpublished".  A store into a body nobody reaches is a
 * plain store.  A store into a live body is a reader-visible mutation that
 * stands at the abort boundary.
 */
struct ft_rekey_cow_fresh {
	struct cds_ft_metadata *m[2];
	unsigned int n;
};

static inline
bool ft_rekey_cow_fresh_has(const struct ft_rekey_cow_fresh *fresh,
		const struct cds_ft_metadata *m)
{
	unsigned int i;

	for (i = 0; fresh && i < fresh->n; i++)
		if (fresh->m[i] == m)
			return true;
	return false;
}

/*
 * THE DESCRIPTOR, READ AT EACH USE.  Never cache it across a record or a
 * reservation.  Either can GROW it, and a grow MOVES it (urcu_txn_grow).  A
 * pointer taken before the grow names the old block, where a lookup no
 * longer finds this commit's records.
 *
 * ft_rekey_cow_stop used to cache it ahead of its own
 * ft_flip_txn_reserve_extra.  When that reserve grew the descriptor:
 *  - the copy read a slot the detach had just republished (the collapse's
 *    run in place of the junction it retires) as its LIVE old value;
 *  - it re-homed the retiring junction into the copy;
 *  - it committed a tombstoned node still reachable.
 * This showed in ft_unit test_rekey_same_path_atomic_or_refused, root_frame
 * geometry 6 with rank stats and the ordered list.  The bug only needs the
 * grow to land there, and the number of records ahead of the copy decides
 * that.  It was found when moving the lock releases out of the txn changed
 * that number.
 *
 * The copy now looks records up through the txn HANDLE (ft_txn_find), which
 * reads the array at each lookup.  Returns NULL when the copy must not see
 * this commit (@ryw off), or when the txn holds no record set (none yet, or
 * out of memory).
 */
static inline
struct urcu_txn *ft_rekey_cow_ryw(const struct ft_flip_txn *txn, bool ryw)
{
	if (!ryw || !ft_txn_live(txn->mtxn))
		return NULL;
	return txn->mtxn;
}

static inline
bool ft_rekey_cow_slot_value(struct ft_flip_txn *txn,
		struct urcu_txn *ryw,
		struct cds_ft_inode_flag **src_slot,
		struct cds_ft_inode_flag **iter_ret)
{
	void *resolved;

	if (ryw) {
		const struct ft_txn_rec *r = ft_txn_find(ryw,
			(void **) src_slot);

		if (r) {
			*iter_ret = (struct cds_ft_inode_flag *) ft_txn_rec_new(r);
			return true;
		}
		if (txn->pending_pub_slot == src_slot && txn->pending_pub_val) {
			*iter_ret = txn->pending_pub_val;
			return true;
		}
	}
	if (!ft_flip_txn_resolve_prio(txn, (void **) src_slot, &resolved))
		return false;
	*iter_ret = (struct cds_ft_inode_flag *) resolved;
	return true;
}

#ifdef FEATURE_FT_SKIP_COMPRESSED
/*
 * Return the node a SKIP word denotes, read through this commit's records.
 * The one-hop inversion (ft_skip_to_compressed) goes through the skip child's
 * back-pointer.  Until the commit flips, that is PHYSICALLY the pre-op
 * parent.  So for a run this txn built or re-homed, it names the node the run
 * replaced.  There are two kinds of child:
 *  - an INTERNAL child is ft_count_walk_survivor_meta's case;
 *  - an EXTERNAL child is decoded here through the head's own back-channel:
 *    cell->parent with the list on, head->prev with it off.  This shape is a
 *    run over a single head: what a keyless one-child internal collapses
 *    into when that child is a head.
 * Returns NULL when nothing coherent can be named: a member, not a head; a
 * parked peer.
 */
static
struct cds_ft_metadata *ft_rekey_cow_skip_meta_ryw(struct cds_ft *ft,
		struct ft_flip_txn *txn, struct urcu_txn *ryw,
		struct cds_ft_inode_flag *skip)
{
	struct cds_ft_inode_flag *child = ft_skip_child_ptr(skip);
	struct cds_ft_node *en;
	const struct ft_txn_rec *r;
	struct cds_ft_inode_flag *pw;
	void **field;

	if (!child)
		return NULL;
	if (!ft_node_external(child))
		return ft_count_walk_survivor_meta(ft, ryw, skip);
	en = (struct cds_ft_node *) child;
	if (ft->ordered_list) {
		void *prev = ft_dereference_prev_resolved(en);

		if (!prev || !((uintptr_t) prev & FT_ORD_CELL_TAG))
			return NULL;
		field = (void **) &ft_ord_cell_ptr(prev)->parent;
	} else
		field = (void **) &en->prev;
	/*
	 * Use the record's pending value first.  Else do a WAITING load: this
	 * word may enter the txn's write set below.  A raw read would launder
	 * a peer's parked proxy into the peer's uncommitted parent.
	 */
	r = ft_txn_find(ryw, field);
	pw = r ? (struct cds_ft_inode_flag *) ft_txn_rec_new(r) :
		(struct cds_ft_inode_flag *) ft_txn_load(txn->mtxn, field,
			FT_FLIP_PROXY_TAG);
	pw = ft_parent_prefix_strip(pw);
	if (!pw || ft_node_flip_proxy(pw) || ft_node_external(pw) ||
			ft_node_skip_compressed(pw))
		return NULL;
	if (ft_node_compressed(pw))
		return cds_ft_item_to_metadata((struct cds_ft_inode *)
			ft_compressed_node_ptr(pw));
	return cds_ft_item_to_metadata(ft_node_ptr(pw));
}
#endif

/*
 * Is @child a node the glue BUILT (unpublished, plain-storable)?  Returns its
 * metadata when so, else NULL.  ft_glue_is_fresh answers by identity of the
 * body.  For a SKIP word it recovers the body through the child's
 * back-pointer.  That is still the pre-op parent for a wrapper the merge
 * just built over a live head.  So a skip word is matched against the built
 * runs by what it ENCODES instead: the child and the length.
 */
static
struct cds_ft_metadata *ft_rekey_cow_glue_fresh_meta(struct cds_ft *ft,
		struct ft_glue *glue, struct cds_ft_inode_flag *child)
{
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(child)) {
		int i;

		for (i = 0; i < glue->nr_built; i++) {
			struct cds_ft_inode_flag *b = glue->built[i];
			struct cds_ft_compressed_node *cn;

			if (ft_node_skip_compressed(b) || !ft_node_compressed(b))
				continue;
			cn = ft_compressed_node_ptr(b);
			if (cn->child == ft_skip_child_ptr(child) &&
					cn->len == ft_skip_len(child))
				return cds_ft_item_to_metadata(
					(struct cds_ft_inode *) cn);
		}
		return NULL;
	}
#endif
	if (!ft_glue_is_fresh(ft, glue, child))
		return NULL;
	return ft_child_state_meta(ft, child);
}

/* The node's key count as this commit will leave it (see @ryw above). */
static inline
unsigned long ft_rekey_cow_nr_keys(const struct cds_ft_metadata *m,
		struct urcu_txn *ryw)
{
	if (ryw) {
		const struct ft_txn_rec *r = ft_txn_find(ryw,
			(void **) (uintptr_t) &m->nr_keys);

		if (r)
			return (unsigned long) ft_txn_rec_new(r) >> 1;
	}
	return ft_nr_keys_get(m);
}

/*
 * Re-parent ONE child of the COW'd node onto the copy.  This is the sweep
 * body of ft_rekey_cow_stop's bitmap arms.  The @ryw dispatch above runs in
 * front of the ordinary mark-and-record path.
 *
 * @ctx is the CALLER's held set: what EARLIER steps of this op hold.  It is
 * consulted before this frame marks anything on the child, so a fence taken
 * here is never mistaken for one taken before.  @cctx is this frame's held
 * set, carrying the marks taken so far.  @src_slot is the slot's address in
 * the ORIGINAL node, the word the descriptor is asked about.
 */
static
int ft_rekey_cow_reparent_child(struct cds_ft *ft,
		const struct ft_lock_ctx *ctx, struct ft_lock_ctx *cctx,
		struct ft_flip_txn *txn, struct urcu_txn *ryw,
		struct cds_ft_inode_flag *iter, struct cds_ft_inode_flag *new_flag,
		struct cds_ft_inode_flag **slot, struct cds_ft_inode_flag **src_slot,
		unsigned int child_depth, const struct ft_rekey_cow_fresh *fresh,
		struct ft_held_anchor *marks, unsigned int *nm,
		unsigned int *nr_marks)
{
	struct cds_ft_metadata *cm;
	bool ryw_meta = false;

	(void) src_slot;

	if (ryw) {
		struct ft_glue *glue = ctx ? (struct ft_glue *) ctx->held.glue :
			NULL;
		const struct ft_txn_rec *pr;
		uintptr_t hs;
		bool rat;

		/* 1. a glue-built top folded in: unpublished. */
		if (glue) {
			struct cds_ft_metadata *fm =
				ft_rekey_cow_glue_fresh_meta(ft, glue, iter);

			if (fm) {
				rcu_assign_pointer(fm->parent_word, new_flag);
				ft_set_parent_slot(fm, new_flag, slot);
				return 0;
			}
		}
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_node_skip_compressed(iter)) {
			/*
			 * Never use the one-hop decode here.  For a run this
			 * commit built or re-homed, it lands on the node the
			 * run REPLACED.  What cannot be named coherently is
			 * refused, not guessed.
			 */
			cm = ft_rekey_cow_skip_meta_ryw(ft, txn, ryw, iter);
			if (!cm)
				return -EDOM;	/* FT_REKEY_UNCOVERED, defined below */
			ryw_meta = true;
		} else
#endif
			cm = ft_child_state_meta(ft, iter);
		if (cm) {
			pr = ft_txn_find(ryw, (void **) &cm->parent_word);
			/* 2. an earlier step's unpublished product, NAMED. */
			if (!pr && ft_rekey_cow_fresh_has(fresh, cm)) {
				rcu_assign_pointer(cm->parent_word, new_flag);
				ft_set_parent_slot(cm, new_flag, slot);
				return 0;
			}
			/* 3. re-homed or fenced by an EARLIER step of this op. */
			if (pr || ft_lock_ctx_holds(ctx, cm, &hs, &rat)) {
				bool held_earlier = ft_lock_ctx_holds(ctx, cm,
					&hs, &rat);
				void *old_pw, *new_pw;
				bool sw;

				/*
				 * This model does not produce an SW record on a
				 * word this op does not hold: an SW park is
				 * legal only under the holder's fence.  Refuse,
				 * so the word does not get a second terminal.
				 */
				if (pr && ft_txn_rec_sw(pr) && !held_earlier)
					return -EDOM;	/* FT_REKEY_UNCOVERED, defined below */
				/*
				 * Chain the parent word onto the earlier
				 * record's PENDING value, with that record's
				 * kind.  These are
				 * ft_flip_txn_record_parent_word's two arms,
				 * with the expected old taken from the
				 * descriptor.  On the op's FIRST attempt
				 * urcu_txn_load reads the live word, and a
				 * chain from it poisons.  The slot offset is
				 * unchanged (the copy is verbatim) and so is
				 * the incoming byte.  Neither is recorded.
				 */
				old_pw = pr ? ft_txn_rec_new(pr) : ft_txn_load(txn->mtxn,
					(void **) &cm->parent_word,
					FT_FLIP_PROXY_TAG);
				new_pw = ft_parent_word(ft, new_flag);
				sw = pr ? ft_txn_rec_sw(pr) : held_earlier;
				if (sw)
					ft_flip_txn_record_reserved(txn, cm,
						(void **) &cm->parent_word, old_pw,
						new_pw);
				else
					ft_flip_txn_record_tag_mw(txn,
						(void **) &cm->parent_word, old_pw,
						new_pw, FT_FLIP_PROXY_TAG
						FT_TK_MWA(FT_TK_MWA_PARENT_WORD));
				/*
				 * The state word has three cases:
				 *  - a held word: its terminal belongs to the
				 *    step that took it;
				 *  - a word this txn already guards: it needs
				 *    no second guard;
				 *  - else: the §4.B validate that every re-home
				 *    of an unheld child records.
				 */
				/*
				 * Through the handle: the records just above
				 * can have grown (moved) the record array.
				 */
				if (!held_earlier &&
						!ft_txn_recorded(txn->mtxn,
							(void **) &cm->state) &&
						!ft_flip_txn_releases_after(txn, cm)) {
					uintptr_t live = (uintptr_t) ft_txn_load(
						txn->mtxn, (void **) &cm->state,
						FT_STATE_PROXY) &
						~(FT_STATE_TOMBSTONE | FT_STATE_LOCK);

					ft_flip_txn_record_state_mw(txn, cm,
						(void *) live, (void *) live);
				}
				return 0;
			}
		} else if (ft_node_external(iter)) {
			/*
			 * An EXTERNAL child whose back-channel this txn already
			 * recorded: the head the detach PROMOTED into the
			 * frame's slot.  It chains by the record's pending
			 * value, for the reason case 3 gives.  A head the txn
			 * never touched takes the ordinary arm below.
			 */
			struct cds_ft_node *en = (struct cds_ft_node *) iter;
			void **field = NULL;

			if (ft->ordered_list) {
				void *prev = ft_dereference_prev_resolved(en);

				if (prev && ((uintptr_t) prev & FT_ORD_CELL_TAG))
					field = (void **)
						&ft_ord_cell_ptr(prev)->parent;
			} else
				field = (void **) &en->prev;
			pr = field ? ft_txn_find(ryw, field) : NULL;
			if (pr) {
				ft_head_stamp_incoming_byte(ft, en, new_flag, slot);
				ft_flip_txn_record_head_back_edge_owned(txn,
					field, ft_txn_rec_new(pr),
					ft_head_parent_word_slot(new_flag, slot),
					ft_back_edge_owner(ft_txn_rec_new(pr))
					FT_BE_SITE(FT_BE_REPARENT_META, ctx));
				return 0;
			}
		}
	} else
		cm = ft_child_state_meta(ft, iter);
	/* 4. The ordinary path: mark, then SW-record under the mark. */
	if (cm) {
		cctx->held.nr_extra = *nm;
		if (ft_rekey_cow_lock_child(ft, cctx, iter, cm, child_depth,
				&marks[*nm]))
			return -EAGAIN;
		(*nm)++;
		*nr_marks = *nm;
		/* register BEFORE the records this mark licenses. */
		ft_rekey_marks_to_txn(txn, marks, *nm);
	}
	if (ryw_meta)
		ft_reparent_record_meta(ft, txn, cm, new_flag, slot,
			/*child_marked=*/ ft_rekey_mark_holds_child_word(
				&marks[*nm - 1], cm),
			/*hold_ctx=*/ NULL, /*check_child=*/ true);
	else
		ft_reparent_record(ft, txn, iter, new_flag, slot,
			/*child_marked=*/ cm != NULL &&
				ft_rekey_mark_holds_child_word(&marks[*nm - 1],
					cm),
			/*hold_ctx=*/ NULL, /*check_child=*/ true);
	return 0;
}

static
int ft_rekey_cow_stop(struct cds_ft *ft, const struct ft_lock_ctx *ctx,
		struct ft_flip_txn *txn,
		struct cds_ft_inode_flag *stop_flag, unsigned int stop_depth,
		unsigned int cut, bool ryw,
		const struct ft_rekey_cow_fresh *fresh,
		struct cds_ft_inode_flag **stop_prime_ret,
		struct ft_held_anchor *marks,
		unsigned int *nr_marks)
{
	bool compressed = ft_node_compressed(stop_flag);
	/*
	 * Use ft_compressed_node_ptr, NOT ft_skip_to_compressed: the two
	 * accessors are not interchangeable.  The skip one masks the length
	 * bits off and treats what is left as the run's CHILD.  It then
	 * recovers the node through that child's back-pointer.  Given a PLAIN
	 * compressed flag, it reads a child pointer out of a flag that is not
	 * one.  This was measured: a segfault in the first re-parent, on a
	 * garbage child.  A skip-encoded @stop_flag never arrives here; the
	 * caller's shape gate refuses it.
	 */
	struct cds_ft_compressed_node *stop_cn = compressed ?
		ft_compressed_node_ptr(stop_flag) : NULL;
	unsigned int ti = compressed ? 0 : ft_node_type(stop_flag);
	const struct cds_ft_type *type = &ft_types[ti];
	struct cds_ft_inode *stop_node = compressed ?
		(struct cds_ft_inode *) stop_cn : ft_node_ptr(stop_flag);
	struct cds_ft_metadata *stop_meta = cds_ft_item_to_metadata(stop_node);
	struct cds_ft_inode *new_node = NULL;
	struct cds_ft_metadata *new_meta;
	struct cds_ft_inode_flag *new_flag;
	bool new_init_done = false;
	struct ft_held_anchor stop_held;
	/*
	 * Every child of @stop starts at the SAME byte-depth: one hop past a
	 * bitmap node, past the whole run for a compressed one.  That single
	 * depth is what the §7.2 fan-out hoists on.
	 */
	unsigned int child_depth;
	unsigned int nm = 0, i;
	int ret;
	/*
	 * THIS function's held set.  @marks is where its acquires live -- @stop's
	 * fence and one per child -- and none of them reaches a txn registry until
	 * the caller's sweep, so a child acquire consulting the CALLER's context
	 * cannot see them.  Under a coarse spacing every child of @stop anchors on
	 * the path above, which is frequently @stop itself, and the op then refuses
	 * its own fence.  @nr_extra is refreshed at each acquire because the fan-out
	 * is still growing it.
	 */
	struct ft_lock_ctx cctx;
	/*
	 * When the copy must show this commit's own records (@ryw), they are
	 * read through ft_rekey_cow_ryw at each use.  See its header.
	 */

	*stop_prime_ret = NULL;
	*nr_marks = 0;
	/*
	 * @structural_sw is the claim that matters: this body parks SW.  The
	 * EXCLUSION behind it depends on the mode:
	 *  - under lock_fine, the per-node DLM locks;
	 *  - under coarse, the FT-wide mutex (CDS_FT_SCOPED_WRITER, taken by
	 *    ft_rekey_graft_simple_locked).
	 * So an assert on lock_fine here asserted one mode's mechanism, not
	 * the property.
	 */
	assert(txn->structural_sw);
	/*
	 * A cut names a byte offset INTO a run, so only the compressed arm can
	 * honour one.  It must leave at least one byte behind, because a
	 * zero-length run is not a node.
	 */
	assert(!cut || (compressed && cut < (unsigned int) stop_cn->len));
	assert(compressed || type->type_class == FT_POPCOUNT ||
		type->type_class == FT_PIGEON);
#ifdef FEATURE_FT_SKIP_COMPRESSED
	assert(!ft_node_skip_compressed(stop_flag));	/* caller's gate */
#endif
	/*
	 * A CO-LOCATED EXTERNAL CHAIN is in scope.  It is one forward pointer
	 * to copy plus one back edge to record: the same shape as a child.  An
	 * S_top that IS an external head stays out of scope.  See the caller.
	 */

	/* 1. Acquire @stop's retire lock -- the fence BEFORE any body read. */
	if (ft_acquire_member(ft, ctx, stop_flag, stop_meta, stop_depth,
			&stop_held))
		return -EAGAIN;
	marks[nm++] = stop_held;
	*nr_marks = nm;
	/* register BEFORE the records this mark licenses. */
	ft_rekey_marks_to_txn(txn, marks, nm);
	/*
	 * PHASE B, STEP B6: the per-op arm, at the first point this op holds
	 * anything.  The caller already armed the txn structurally at CREATION
	 * (ft_flip_txn_arm_structural).  That arm covers every branch.  This
	 * one is ADDITIVE and buys the audit value: only a per-op arm sets
	 * @dbg_arm_per_op, and at creation @nr_locks is 0 so it cannot.
	 *
	 * Registering first is not optional: the records below name these marks
	 * as their owner.  This is register-BEFORE-record, the class 55c0350c
	 * closed across 17 sites.  ft_rekey_marks_to_txn is idempotent
	 * (@txn_owned).
	 */
	ft_rekey_marks_to_txn(txn, marks, nm);
	ft_flip_txn_arm_per_op(ft, txn);
	/*
	 * A held set carries ONE out-of-registry array, so a caller keeping marks
	 * of its own would lose them here.  No caller does; assert it rather than
	 * let a future one lose its exclusion silently.
	 */
	assert(!ctx || !ctx->held.nr_extra);
	ft_lock_ctx_init(&cctx, ft_lock_ctx_descent(ctx), txn, ctx ? ctx->op : NULL);
	cctx.held.extra = marks;

	/* <=2 edges/child (parent + pso) + 1 retire; caller reserves its publish. */
	if (!ft_flip_txn_reserve_extra(txn, compressed ? 2 + 1 :
			2 * ft_meta_nr_child_load(stop_meta) + 1)) {
		ret = -ENOMEM;
		goto out;
	}

	if (compressed) {
		/*
		 * 2'. + 3'. + 4'.  The one-child form: copy the run and its child,
		 *     then mark and re-parent that child.  The source child is read
		 *     RESOLVED through the txn, as the bitmap arms read theirs.
		 *     The reason is the same: a peer's parked flip proxy must
		 *     never be embedded in the copy.
		 */
		struct cds_ft_compressed_node *new_cn;
		struct cds_ft_inode_flag *child;
		struct cds_ft_metadata *cm;
		void *resolved;

		unsigned int tail_len = (unsigned int) stop_cn->len - cut;

		new_cn = alloc_compressed_node(ft, tail_len, &new_meta);
		if (!new_cn) {
			ret = -ENOMEM;
			goto out;
		}
		new_node = (struct cds_ft_inode *) new_cn;
		new_cn->len = tail_len;
		memcpy(new_cn->key_bytes, stop_cn->key_bytes + cut, tail_len);
		if (!ft_flip_txn_resolve_prio(txn, (void **) &stop_cn->child,
				&resolved)) {
			ret = -EAGAIN;
			goto abandon;
		}
		child = (struct cds_ft_inode_flag *) resolved;
		new_cn->child = child;
		/*
		 * The child count is metadata, not a body field.  So setting
		 * ->child does not maintain it the way the bitmap arms' set_nth
		 * calls do.  cds_ft_verify cross-checks it against cn->child's
		 * presence
		 * ("compressed node nr_child 0 does not match cn->child presence").
		 * Carry it over as ft_compact's clone does; it is 1 for a compressed
		 * node.
		 */
		ft_meta_nr_child_set(new_meta, ft_meta_nr_child(stop_meta));
		ft_nr_keys_store(ft, new_meta, ft_nr_keys_get(stop_meta),
			CMM_RELAXED);
		new_flag = ft_compressed_node_flag(new_cn);
		child_depth = stop_depth + stop_cn->len;
		cm = ft_child_state_meta(ft, child);
		if (cm) {
			uintptr_t csnap;

			cctx.held.nr_extra = nm;
			if (ft_rekey_cow_lock_child(ft, &cctx, child, cm,
					child_depth, &marks[nm])) {
				ret = -EAGAIN;
				goto abandon;
			}
			csnap = marks[nm].node_snap;
			nm++;
			*nr_marks = nm;
			/* register BEFORE the records this mark licenses. */
			ft_rekey_marks_to_txn(txn, marks, nm);
		}
		ft_reparent_record(ft, txn, child, new_flag, &new_cn->child,
			/*child_marked=*/ cm != NULL &&
				ft_rekey_mark_holds_child_word(&marks[nm - 1],
					cm),
			/*hold_ctx=*/ NULL, /*check_child=*/ true);
		/*
		 * 5'. Retire @stop and hand back the NODE flag, not the skip-encoded
		 *     slot form.
		 *
		 * ★ THE SKIP FORM IS NOT INVERTIBLE HERE, which is why the caller gets
		 * the plain one.  A skip value names the CHILD plus the run
		 * length.  ft_skip_to_compressed recovers the node through that
		 * child's back-pointer.  Until this txn commits, that
		 * back-pointer still names the OLD node.  So a caller that had
		 * to free the copy on a later bail would resolve the skip flag
		 * to the LIVE ORIGINAL and free that.  The plain flag inverts
		 * with ft_compressed_node_ptr and cannot.
		 */
		if (!stop_held.shared)
			ft_flip_txn_record_anchor_release(txn, &stop_held,
				stop_meta);
		ft_flip_txn_record_retire_anchored(txn, ctx, &stop_held,
			stop_meta);
		*stop_prime_ret = new_flag;
		return 0;
	}

	/* 2. Fresh same-type, same-capacity copy (zeroed; body rebuilt below). */
	new_node = alloc_cds_ft_node(ft, type, &new_meta);
	if (!new_node) {
		ret = -ENOMEM;
		goto out;
	}
	new_flag = ft_node_flag(new_node, ti);
	child_depth = stop_depth + 1;	/* a bitmap node spans ONE key byte */
	ft_nr_keys_store(ft, new_meta, ft_rekey_cow_nr_keys(stop_meta,
		ft_rekey_cow_ryw(txn, ryw)),
		CMM_RELAXED);

	/*
	 * 3. Copy children @stop -> @stop', each source slot RESOLVED (frozen
	 *    under @stop's fence) so a peer's parked flip proxy is never embedded.
	 *    Verbatim positions -> identity-preserving; interior stays shared.
	 */
#define COW_IS_INIT(bv) ({						\
	bool __i = false;						\
	if (type->type_class == FT_POPCOUNT) {				\
		__i = !new_init_done;					\
		new_init_done = true;					\
	}								\
	__i;								\
})
	if (type->type_class == FT_POPCOUNT) {
		uint8_t nc = ft_popcount_node_get_nr_child(type, stop_node);

		for (i = 0; i < nc; i++) {
			struct cds_ft_inode_flag *iter, **src_slot;
			uint8_t v;

			ft_popcount_node_get_ith_pos(type, stop_node, i, &v, &iter);
			if (!iter)
				continue;
			ft_node_get_nth_skip(stop_flag, &src_slot, v, FT_PF_NONE);
			if (!ft_rekey_cow_slot_value(txn,
					ft_rekey_cow_ryw(txn, ryw), src_slot,
					&iter)) {
				ret = -EAGAIN;
				goto abandon;
			}
			if (!iter)
				continue;
			if (type->popcount_2l)
				ret = ft_popcount_2l_node_set_nth(type, new_node,
						new_meta, v, iter, COW_IS_INIT(v));
			else if (type->popcount_1l)
				ret = ft_popcount_1l_node_set_nth(type, new_node,
						new_meta, v, iter, COW_IS_INIT(v));
			else
				ret = _ft_node_set_nth(ft, type, new_node, new_flag,
						new_meta, v, iter, COW_IS_INIT(v),
						true, /* in_place: build-invisible */ true,
						NULL FT_CH_TXN_NONE);
			assert(!ret);
		}
	} else {	/* FT_PIGEON */
		for (i = 0; i < FT_ENTRY_PER_NODE; i++) {
			struct cds_ft_inode_flag *iter, **src_slot;

			iter = ft_pigeon_node_get_ith_pos(type, stop_node, i);
			if (!iter)
				continue;
			ft_node_get_nth_skip(stop_flag, &src_slot, (uint8_t) i,
					FT_PF_NONE);
			if (!ft_rekey_cow_slot_value(txn,
					ft_rekey_cow_ryw(txn, ryw), src_slot,
					&iter)) {
				ret = -EAGAIN;
				goto abandon;
			}
			if (!iter)
				continue;
			if (type->popcount_2l)
				ret = ft_popcount_2l_node_set_nth(type, new_node,
						new_meta, (uint8_t) i, iter,
						COW_IS_INIT((uint8_t) i));
			else if (type->popcount_1l)
				ret = ft_popcount_1l_node_set_nth(type, new_node,
						new_meta, (uint8_t) i, iter,
						COW_IS_INIT((uint8_t) i));
			else
				ret = _ft_node_set_nth(ft, type, new_node, new_flag,
						new_meta, i, iter,
						COW_IS_INIT((uint8_t) i), true,
						/* in_place: build-invisible */ true,
						NULL FT_CH_TXN_NONE);
			assert(!ret);
		}
	}
#undef COW_IS_INIT

	/*
	 * 3b. The CO-LOCATED EXTERNAL CHAIN, if any: the key that ends exactly at
	 *     @stop.  Its head is APP-OWNED and never copied.  Only the forward
	 *     pointer moves to the copy, and the head's back edge is recorded
	 *     like any child's.  ft_reparent_record dispatches on the child
	 *     kind, so it writes whichever back-channel this group uses:
	 *      - the cell's parent with the list on;
	 *      - the head's own prev with it off.
	 *     There is no mark, because an external head carries no state word
	 *     (ft_child_state_meta returns NULL for one).
	 */
	if (stop_meta->external_nodes) {
		new_meta->external_nodes = stop_meta->external_nodes;
		ft_reparent_record(ft, txn,
			(struct cds_ft_inode_flag *) ft_dereference_external(
				new_meta->external_nodes),
			new_flag,
			(struct cds_ft_inode_flag **) &new_meta->external_nodes,
			/*child_marked=*/ false, /*hold_ctx=*/ NULL,
			/*check_child=*/ true);
	}

	/*
	 * 4. MARK each metadata-bearing child + record its SW re-parent onto
	 *    @stop'.  Iterate the FRESH node's children (post-copy).  The mark must
	 *    precede ft_reparent_record so its pso edge's new_state (LOCK masked
	 *    out) releases the mark at the commit flip.
	 */
	if (type->type_class == FT_POPCOUNT) {
		uint8_t nc = ft_popcount_node_get_nr_child(type, new_node);

		for (i = 0; i < nc; i++) {
			struct cds_ft_inode_flag *iter, **slot = NULL, **src_slot;
			uint8_t v;

			ft_popcount_node_get_ith_pos(type, new_node, i, &v, &iter);
			if (!iter)
				continue;
			ft_node_get_nth_skip(new_flag, &slot, v, FT_PF_NONE);
			ft_node_get_nth_skip(stop_flag, &src_slot, v, FT_PF_NONE);
			ret = ft_rekey_cow_reparent_child(ft, ctx, &cctx, txn,
				ft_rekey_cow_ryw(txn, ryw),
				iter, new_flag, slot, src_slot, child_depth, fresh,
				marks, &nm, nr_marks);
			if (ret)
				goto abandon;
		}
	} else {	/* FT_PIGEON */
		for (i = 0; i < FT_ENTRY_PER_NODE; i++) {
			struct cds_ft_inode_flag *iter, **slot = NULL, **src_slot;

			iter = ft_pigeon_node_get_ith_pos(type, new_node, i);
			if (!iter)
				continue;
			ft_node_get_nth_skip(new_flag, &slot, (uint8_t) i, FT_PF_NONE);
			ft_node_get_nth_skip(stop_flag, &src_slot, (uint8_t) i,
					FT_PF_NONE);
			ret = ft_rekey_cow_reparent_child(ft, ctx, &cctx, txn,
				ft_rekey_cow_ryw(txn, ryw),
				iter, new_flag, slot, src_slot, child_depth, fresh,
				marks, &nm, nr_marks);
			if (ret)
				goto abandon;
		}
	}

	/*
	 * 5. Retire @stop (SW fenced tombstone {LOCK|snap -> TOMBSTONE|snap}).
	 *    A peer's ft_meta_nr_child_inc(@stop) now honors the mark and spins, so
	 *    the SW park is not clobbered.  Not registered -- the caller's sweep
	 *    owns clearing (marks[0]).
	 */
	if (!stop_held.shared)
		ft_flip_txn_record_anchor_release(txn, &stop_held, stop_meta);
	ft_flip_txn_record_retire_anchored(txn, ctx, &stop_held, stop_meta);

	*stop_prime_ret = new_flag;
	return 0;

abandon:
	/* Each kind to its own arena: a compressed copy came from the other one. */
	if (compressed)
		free_compressed_node_unpublished(ft,
			(struct cds_ft_compressed_node *) new_node);
	else
		free_cds_ft_node_unpublished(ft, new_node);
out:
	return ret;	/* marks[0..*nr_marks) swept by the caller */
}

/* ------------- moved from fractal-trie.c ------------- */

/*
 * Does the located ordered-list splice pair (@pred, @succ) really BRACKET the
 * dst key range in key order?  ft_ord_cell_find_splice_pos derives the pair
 * from a RELATIONAL (inequality) descent.  Relational reads are NOT coherent
 * under concurrent structural mutation.  A peer rekey in flight can make that
 * descent return a pair that is genuinely ADJACENT in the list but sits at
 * the WRONG key position.  Every edge of the splice then validates (the pair
 * IS adjacent), and the commit installs the run in the wrong place.  The
 * ordered list is then a well-formed doubly-linked list that is no longer
 * key-ordered.
 *
 * So re-derive each neighbour's key from the STRUCTURE and require
 * pred < the dst range < succ.  The derivation is the same up-walk the
 * rekey-coherent reader uses (ordinal space, right-aligned in @scratch).
 * @dst_key is a PREFIX, so the range is every key that extends it.
 * ft_rekey_prefix_range_cmp places a neighbour against that range in either
 * group flavour.
 *
 * A sentinel / absent neighbour has no key, but it is NOT exempt.  It ASSERTS
 * that the run belongs at the list end, so that assertion is what is checked:
 *  - an absent pred means succ must really be the list MINIMUM;
 *  - an absent succ means pred must really be the list MAXIMUM.
 * A first cut skipped that, which left the incoherent derivation a way
 * through.  The descent returns a spurious "no key below the dst range"
 * (find_splice_pos returns pred == NULL and then probes GT), paired with a
 * succ that legitimately sorts above the range.  That pair passes a key-only
 * check.  It then reaches the endpoint-adjacency guard as a bogus PERMANENT
 * -EINVAL.  That was the last residual failure of the shared-junction oracle
 * (~1 run in 70).
 */
/*
 * Where does the ordinal key @kb (@klen bytes) sit relative to the RANGE of
 * keys the prefix @dst_ord (@dst_len bytes) covers?  Returns -1 below it, 0
 * inside it, 1 above.
 *
 * The trie's key order compares bytes over the COMMON length.  On a tie it
 * puts the SHORTER key first (measured: 'aa' < 'ab' < 'abc' < 'abd' < 'b').
 * So the range is [@dst_ord itself .. every key extending it]: a strict
 * PREFIX of @dst_ord sorts BELOW the range, and @dst_ord itself is INSIDE.
 *
 * A fixed-length group is the special case where no key is shorter than
 * @dst_len.  That is why this used to be expressible as a plain compare
 * against @dst_ord padded with the ordinal extremes.  That padding tied the
 * check to fixed lengths.  A compare over the common length needs no padding
 * at all.
 */
static
int ft_rekey_prefix_range_cmp(const uint8_t *kb, size_t klen,
		const uint8_t *dst_ord, size_t dst_len)
{
	size_t n = klen < dst_len ? klen : dst_len;
	int cmp = memcmp(kb, dst_ord, n);

	if (cmp != 0)
		return cmp < 0 ? -1 : 1;
	return klen < dst_len ? -1 : 0;
}

/*
 * Free the UNPUBLISHED S_top copy ft_rekey_cow_stop made, whichever kind it
 * is.  The two kinds come from different arenas.  The compressed one is
 * handed back as a plain node flag precisely so that this inversion is safe
 * (see cow_stop).
 *
 * ☠ ONLY A COPY -- AND THE CHECK IS HERE, NOT AT THE SEVEN BAIL SITES.  The
 * BARE-EXTERNAL-HEAD leg skips ft_rekey_cow_stop entirely: there is nothing
 * to copy.  It leaves @s_top_prime naming the LIVE, APP-OWNED head.  So on
 * that shape this must free NOTHING.  cow_stop never produces an external
 * copy, so "external" is an exact discriminator.  The check at this ONE
 * choke point covers every bail, including a future one someone adds without
 * reading this.  A per-site guard would be a site list, and a site list
 * cannot cover a rule this shape.
 *
 * What it prevents: the dispatch below has no external arm, so the head
 * would take the plain-node arm.  The APPLICATION'S OWN memory would then go
 * back to a library arena, while the trie still points at it from the
 * destination.
 */
static
void ft_rekey_free_stop_prime(struct cds_ft *ft, struct cds_ft_inode_flag *nf)
{
	if (!nf)
		return;
	if (ft_node_external(nf))
		return;		/* the live head: not ours, see above */
	if (ft_node_compressed(nf))
		free_compressed_node_unpublished(ft, ft_compressed_node_ptr(nf));
	else
		free_cds_ft_node_unpublished(ft, ft_node_ptr(nf));
}

#ifdef FEATURE_FT_MERGE
/*
 * Build the ORDERED-CELL edge set for an IN-TRIE interleave.  The moved run's
 * keys thread individually between the destination region's keys.  Every
 * relink is RECORDED, so the whole reorder lands in the move's one commit.
 *
 * This is the case a run splice cannot express.  When the moved suffixes do
 * not disjointly precede or follow the region's, the moved cells do not stay
 * contiguous.  So there is no single pair of boundary edges to write: each
 * survivor lands between two region cells.  ft_merge_ord_interleave_collect
 * computes exactly that order.  From an in-trie caller it needs @record_all,
 * because its survivors are live in the list being rebuilt.  They do not
 * arrive from a consumed source list.
 *
 * ON TOP OF THE COLLECT, two things the cross-trie merge never needs:
 *  - the SRC GAP.  A cross-trie merge throws its source list away.  Here the
 *    run vacates a position in the same list, so its old neighbours must be
 *    stitched to each other (ft_ord_cell_run_detach_edges, 2 edges).
 *  - an ADJACENCY case.  The run and the region are each contiguous and
 *    disjoint in the CURRENT list: their key prefixes are disjoint, and they
 *    only interleave AFTER the move.  So the ONLY way an edge slot can
 *    repeat is the two runs ABUTTING.  Then:
 *     - the region's neighbour on the abutting side is a cell of the run
 *       itself, so the collect would seed a boundary on a cell that is
 *       moving;
 *     - the gap closure would write the very link the collect writes.  That
 *       is two records on one slot, which the engine forbids.
 *
 * ★ HOW ABUTTING IS HANDLED, and why it is not a special case in the edge
 * building at all.  When the two are adjacent, the merged block occupies the
 * UNION of the two ranges.  So it has ONE boundary pair: the run's outer
 * neighbour on the abutting side, the region's on the other.  The gap the
 * run vacates is INSIDE the block, not beside it.  So the abutting side's
 * boundary is handed to the collect as its link target (@tail_link /
 * @prev_placed + @head_linked).  The gap closure is not emitted at all.
 * One boundary pair, written once.  The duplicate cannot arise because the
 * second writer is gone, not because the two happen to disagree.
 *
 * A COLLISION drops the colliding src head from the merged order.  The edges
 * below route every surviving link around that cell, so a new reader cannot
 * reach it once this commit lands.  The caller RECLAIMS it on the far side
 * of that commit (*@collided_ret / *@ncollide_ret), never here.
 *
 * Returns 0 with *@edges_ret and *@collided_ret (caller frees both) and *@n_ret,
 * or -EINVAL (a shape above), -EAGAIN (a torn read) or -ENOMEM.  Records nothing
 * itself.
 */
static
int ft_rekey_ord_interleave(struct cds_ft *ft, struct cds_ft_inode_flag *D,
		size_t dst_len, size_t src_len,
		struct cds_ft_node *run_rfirst, struct cds_ft_node *run_rlast,
		unsigned long merged_keys,
		struct ft_ord_cell_edge **edges_ret, unsigned int *n_ret,
		struct ft_ord_cell ***collided_ret, unsigned long *ncollide_ret)
{
	size_t max_len = ft->group->max_key_len;
	struct ft_ord_cell *rfc, *rlc, *run_pred, *run_succ;
	struct ft_ord_cell *dfirst, *dlast, *reg_pred, *reg_succ;
	struct ft_merge_src_cap *caps = NULL;
	uint8_t *pool = NULL;
	size_t pool_cap = 0, pool_len = 0;
	struct ft_ord_cell_edge *edges = NULL;
	struct ft_ord_cell **collided = NULL;
	struct ft_ord_cell *head_link, *tail_link;
	bool abut_run_first, abut_region_first, head_linked;
	unsigned long nsrc = 0, ncollide = 0, cap_n;
	unsigned int n;
	struct ft_ord_cell *sc, *slast;
	int ret;

	*edges_ret = NULL;
	*n_ret = 0;
	*collided_ret = NULL;
	*ncollide_ret = 0;
	rfc = ft_ord_cell_ptr(rcu_dereference(run_rfirst->prev));
	rlc = ft_ord_cell_ptr(rcu_dereference(run_rlast->prev));
	dfirst = ft_ord_cell_ptr(rcu_dereference(
		ft_subtree_minmax_head(ft, D, false)->prev));
	dlast = ft_ord_cell_ptr(rcu_dereference(
		ft_subtree_minmax_head(ft, D, true)->prev));
	if (!rfc || !rlc || !dfirst || !dlast)
		return -EAGAIN;
	run_pred = ft_ord_cell_resolve_ord(&rfc->lnode.prev);
	run_succ = ft_ord_cell_resolve_ord(&rlc->lnode.next);
	reg_pred = ft_ord_cell_resolve_ord(&dfirst->lnode.prev);
	reg_succ = ft_ord_cell_resolve_ord(&dlast->lnode.next);
	/*
	 * WHICH SIDE ABUTS, if either (see the header).  Both disjuncts of a side
	 * are tested rather than one: they are equivalent on a coherent list, and
	 * disagreeing means the list was read torn.
	 */
	abut_run_first = (run_succ == dfirst || reg_pred == rlc);
	abut_region_first = (run_pred == dlast || reg_succ == rfc);
	if (abut_run_first && abut_region_first)
		return -EAGAIN;		/* torn: a run cannot abut on both sides */
	/*
	 * The merged block's OWN boundary pair.  On the abutting side the
	 * region's neighbour is a cell of the run, so the block's neighbour is
	 * the run's outer one.  @head_linked then denies the pre-existing
	 * predecessor link the collect would otherwise assume, because
	 * @head_link still points into the run.  The region's walk bound stays
	 * @reg_succ either way.
	 */
	head_link = abut_run_first ? run_pred : reg_pred;
	head_linked = !abut_run_first;
	tail_link = abut_region_first ? run_succ : reg_succ;

	/*
	 * Capture the run's key SUFFIXES while it is still attached and
	 * up-walkable, exactly as ft_merge_spine_copy does.  The merge order
	 * is rebuilt from the two live runs, never from the structure about to
	 * be published.  So nothing needs a proxy installed first.
	 */
	caps = (struct ft_merge_src_cap *) malloc((merged_keys + 8) *
			sizeof(*caps));
	if (!caps)
		return -ENOMEM;
	sc = rfc;
	slast = rlc;
	for (;;) {
		uint8_t sbuf[FT_MAX_KEY_LEN];
		size_t sfl = ft_rebuild_key_upwalk(ft, sc, sbuf, max_len);
		size_t suf_len;

		if (sfl < src_len || nsrc >= merged_keys + 8) {
			ret = -EAGAIN;		/* torn up-walk, or the run grew */
			goto out;
		}
		suf_len = sfl - src_len;
		if (pool_len + suf_len > pool_cap) {
			size_t ncap = pool_cap ? pool_cap * 2 : 256;
			uint8_t *np;

			while (ncap < pool_len + suf_len)
				ncap *= 2;
			np = (uint8_t *) realloc(pool, ncap);
			if (!np) {
				ret = -ENOMEM;
				goto out;
			}
			pool = np;
			pool_cap = ncap;
		}
		memcpy(pool + pool_len, sbuf + (max_len - sfl) + src_len,
			suf_len);
		caps[nsrc].cell = sc;
		caps[nsrc].suffix_off = pool_len;
		caps[nsrc].suffix_len = suf_len;
		pool_len += suf_len;
		nsrc++;
		if (sc == slast)
			break;
		sc = ft_ord_cell_resolve_ord(&sc->lnode.next);
		if (!sc) {
			ret = -EAGAIN;
			goto out;
		}
	}

	/*
	 * The bound is the sum of:
	 *  - <= 2 visible edges per survivor run + 2 boundary;
	 *  - (record_all) up to 2 per survivor for its own links;
	 *  - the 2 src-gap edges.
	 * ☠ The gap edges are NOT emitted on the abutting side, but the term
	 * stays unconditional.  A bound that tracked the branch would have to
	 * be right about it, and this one costs two array slots.  With NDEBUG
	 * the assert below is compiled out, so an overflow here is a wild
	 * write.
	 */
	cap_n = 2 * merged_keys + 2 + 2 * nsrc + FT_ORD_CELL_RUN_DETACH_MAX_EDGES;
	edges = (struct ft_ord_cell_edge *) calloc(cap_n, sizeof(*edges));
	if (!edges) {
		ret = -ENOMEM;
		goto out;
	}
	/* One slot per src head: a collision can drop at most all of them. */
	if (nsrc) {
		collided = (struct ft_ord_cell **) calloc((size_t) nsrc,
				sizeof(*collided));
		if (!collided) {
			ret = -ENOMEM;
			goto out;
		}
	}
	n = ft_merge_ord_interleave_collect(ft, dst_len, dfirst, reg_succ,
			head_link, tail_link, head_linked, caps, nsrc, pool,
			edges, /*record_all=*/ true, &ncollide, collided);
	/*
	 * Close the gap the run vacates -- ONLY when the run does not abut the
	 * region.  When it abuts, that gap is interior to the merged block and
	 * the boundary pair above already spans it.  These two edges would
	 * then put a second record on a slot the collect wrote.
	 */
	if (!abut_run_first && !abut_region_first)
		n = ft_ord_cell_run_detach_edges(ft, run_rfirst, run_rlast,
				&rfc, &rlc, edges, n);
	assert(n <= cap_n);
	*edges_ret = edges;
	*n_ret = n;
	edges = NULL;
	*collided_ret = collided;
	*ncollide_ret = ncollide;
	collided = NULL;
	ret = 0;
out:
	free(collided);
	free(edges);
	free(pool);
	free(caps);
	return ret;
}
#endif /* FEATURE_FT_MERGE: the only caller is merge-only */

/*
 * Order two key SUFFIXES the way the trie orders keys: bytes over the common
 * length, and on a tie the shorter one first.
 */
static
int ft_rekey_suffix_cmp(const uint8_t *a, size_t alen, const uint8_t *b,
		size_t blen)
{
	size_t n = alen < blen ? alen : blen;
	int cmp = memcmp(a, b, n);

	if (cmp != 0)
		return cmp < 0 ? -1 : 1;
	if (alen == blen)
		return 0;
	return alen < blen ? -1 : 1;
}

/*
 * An OCCUPIED destination: where does the moved run land relative to the
 * merge region already there?  Returns:
 *  -1  entirely BELOW it;
 *   1  entirely ABOVE it;
 *   0  INTERLEAVED with it.
 *
 * Both sides are compared by the suffix BELOW their own merge point.  The
 * merged order is decided on that suffix.  The moved keys become
 * @dst_key ++ suffix and the region's are @dst_key ++ their own, so the
 * shared prefix cancels.
 *
 * ★ WHY THE ANSWER MATTERS SO MUCH.  Entirely below or entirely above, the
 * moved cells stay ONE CONTIGUOUS list range.  The move is then the same run
 * splice an empty destination gets, at the region's front or back instead of
 * into a gap.  INTERLEAVED, it is not a run move at all: the cells have to
 * be threaded individually between the region's.  That is
 * ft_merge_ord_interleave_collect's job.  That helper PLAIN-STORES each
 * surviving cell's links, on the premise that the cell "is not ord-reachable
 * in @dst -- never was".  This is true for the cross-trie merge it was
 * written for and FALSE here.  An in-trie rekey's source cells are live in
 * the very list being rebuilt, so those stores would be reader-visible and
 * non-atomic.  Interleaving in-trie needs a mode that RECORDS every relink
 * instead, so it is refused rather than approximated.
 *
 * A COLLISION cannot occur in the two cases this admits: identical full keys
 * mean identical suffixes, which strict disjointness excludes.  That keeps
 * the duplicate-chain absorption out of the picture here.
 */
static
int ft_rekey_run_vs_region(struct cds_ft *ft,
		struct ft_ord_cell *rfc, struct ft_ord_cell *rlc, size_t src_len,
		struct ft_ord_cell *dfirst, struct ft_ord_cell *dlast,
		size_t dst_len)
{
	size_t max_len = ft->group->max_key_len;
	uint8_t rb[FT_MAX_KEY_LEN], db[FT_MAX_KEY_LEN];
	size_t rl, dl;

	/* run MAX vs region MIN: below iff strictly less. */
	rl = ft_rebuild_key_upwalk(ft, rlc, rb, max_len);
	dl = ft_rebuild_key_upwalk(ft, dfirst, db, max_len);
	if (!rl || !dl || rl < src_len || dl < dst_len)
		return 0;			/* unreadable: treat as interleaved */
	if (ft_rekey_suffix_cmp(rb + (max_len - rl) + src_len, rl - src_len,
			db + (max_len - dl) + dst_len, dl - dst_len) < 0)
		return -1;
	/* run MIN vs region MAX: above iff strictly greater. */
	rl = ft_rebuild_key_upwalk(ft, rfc, rb, max_len);
	dl = ft_rebuild_key_upwalk(ft, dlast, db, max_len);
	if (!rl || !dl || rl < src_len || dl < dst_len)
		return 0;
	if (ft_rekey_suffix_cmp(rb + (max_len - rl) + src_len, rl - src_len,
			db + (max_len - dl) + dst_len, dl - dst_len) > 0)
		return 1;
	return 0;
}

/*
 * TRI-STATE, and the third value is the whole point:
 *   1  the pair brackets the dst range;
 *   0  the derivation was TORN (transient: re-derive);
 *  -1  a neighbour is STRUCTURALLY inside the range.
 *
 * ☠ THE TWO FAILURES ARE NOT THE SAME FAILURE, and answering both with a
 * retry is an infinite loop.  A torn read is a peer restructuring this
 * neighbourhood, and it clears on its own.  A neighbour inside the range
 * means a key legitimately EXTENDS the dst prefix: the destination is
 * occupied.  No retry can change that.  The op re-descends, re-derives the
 * identical pair, and refuses again forever.
 *
 * That shape is reachable whenever the caller's @merge_dst probe answered NO
 * for a destination that is in fact occupied.  The probe does that by
 * construction: it stops at a COMPRESSED (or external / skip) node and does
 * not decode it.  So an occupied destination reached through a compressed
 * run reads as an empty one.  This check was written under the premise "the
 * dst prefix is empty here", and that premise does not hold.  So the caller
 * must answer the miss with a SHAPE refusal the dispatcher can fall back on.
 */
/*
 * DOES THE DETACH'S ELEVATION CLIMB REACH THE NODE THE GRAFT REPUBLISHES?
 *
 * A detach that empties its parent does not stop there.  It ELEVATES, and
 * keeps elevating while each ancestor's only remaining child is the one just
 * cleared.  @graft_c is the junction the graft ADD-recompacts (it gains the
 * dst slot) and republishes as a fresh copy.  If the climb reaches @graft_c,
 * it evaluates emptiness against the SUPERSEDED original, which still has
 * only its old child.  So it sees an empty node where the graft's copy holds
 * two children.  It then walks on past the junction and clears edges that
 * the graft's freshly published subtree hangs from.
 *
 * Returns the number of levels the climb would rise.  Sets @reached when the
 * walk arrives at @graft_c or runs out of trie (the root).  This is pure
 * plan-time arithmetic over the ancestors' arities.  The caller holds the
 * locks that keep them still.
 *
 * ☠ A COMPRESSED ANCESTOR IS THE ONE THE CLIMB ALWAYS PASSES THROUGH.  This
 * walk used to STOP at it, calling that "the conservative answer".  It is the
 * opposite of conservative, and the file already said so twenty lines from
 * here: "a run is a node like any other to the detach -- it just has exactly
 * one child, so dropping S_top empties it and the detach's upward walk
 * ELEVATES past it".  A run has exactly one child BY CONSTRUCTION and can
 * never carry external_nodes.  So it is the only ancestor kind that the drop
 * is GUARANTEED to empty.  The stop reported steps == 0 ("no climb") for a
 * climb that always happens.  Every staleness term below is gated on that
 * count.
 *
 * ☠☠ WHAT THAT COST, measured at @4c8ac7fe on a clean detached worktree
 * (single-threaded, --enable-rcu-debug armed and SILENT throughout):
 *
 *     insert "bcbab", "bcacbx", "bcacby", "bb"
 *     cds_ft_rekey_merge(ft, dst "bab", src "bcacb")   -> STATUS_OK
 *
 * The call returns SUCCESS with the moved keys reachable at BOTH names (babx,
 * baby, bcacbx, bcacby all found).  The real detach climbs the run and then
 * republishes into the graft's SUPERSEDED attach-node body.  So the src
 * junction keeps its old slot while the subtree is also installed under the
 * relocated copy.  cds_ft_verify says "external chain node reached twice".
 * The same commit RETIRES nodes the live trie still reaches through.  Once
 * the arena hands them out again, an UNTOUCHED key is silently gone
 * (measured: "bcbab", never named by the call, absent after 4000 unrelated
 * inserts).
 *
 * Clean under -DNO_FEATURE_FT_COMPRESS, red under
 * -DNO_FEATURE_FT_SKIP_COMPRESSED: the path-compression lane, not the
 * encoding.
 *
 * @compressed_link reports that the walk crossed a run.  It used to DISARM
 * the fold, for two reasons.  Both are answered now:
 *  - The orphan freeze was plain-interiors-only.  It retires a run too
 *    (ft_rekey_fold_freeze_orphans).
 *  - @steps stops equalling the BYTE depth the climb rises: a run of length
 *    L spans L depths in one step.  The walk steps by ft_node_span.
 *    `climb_steps <= d_src.pdepth` survives as the range guard it always
 *    was, because every node spans at least one byte.
 * It is now a REPORT, not a verdict.  The staleness terms only ask WHETHER
 * the climb arrives, and they always used the honest count.
 *
 * ☞ @walk_runs, AND WHY IT IS THE NOSPLIT ARM ONLY.  This whole walk predicts
 * one staleness: "the detach republishes into a body the graft SUPERSEDED".
 * A body is superseded exactly when the graft RELOCATED it.  That is the
 * NOSPLIT arm's reserve recompaction (ft_node_set_nth_rec ->
 * ft_node_recompact).  The GLUE arm does not relocate the attach node: it
 * builds a fresh cluster.  The gate below already carries its own term for
 * that shape (`d_src.pnf == graft_c && prep != NOSPLIT`).
 *
 * ☠ MEASURED, and it is why this is a switch and not always-on.  Walking
 * runs unconditionally REFUSED test_rekey_promote_into_split_cn's first arm
 * (`rekey we<-wabb` on {wa,wabbb,wabby,q}).  That is a GLUE-prep shape that
 * is SERVED CORRECTLY: verify clean, keys right.  Its climb vector is
 * IDENTICAL to the corrupting one in every field the gate reads (bp
 * compressed, steps 1, reached 0, rest_under 1, no fold armed).  It differs
 * ONLY in @prep.  That is the discriminator, and it is the mechanism's own.
 * ☐ NOT ESTABLISHED: that a run-crossing climb is safe under GLUE in general.
 * One shape is served correctly; nothing here proves the rest are.  If a GLUE
 * corruption of this family ever turns up, this switch is where it lives.
 */
static
int ft_rekey_climb_reaches_graft(struct cds_ft *ft,
		struct cds_ft_inode_flag *bp,
		struct cds_ft_inode_flag *graft_c,
		bool *reached,
		struct cds_ft_inode_flag **rest_out,
		struct cds_ft_inode_flag **top_out,
		bool *compressed_link,
		unsigned int *bytes_out,
		bool walk_runs)
{
	struct cds_ft_inode_flag *n = bp;
	int steps = 0;

	*reached = false;
	if (rest_out)
		*rest_out = bp;
	if (top_out)
		*top_out = NULL;
	if (compressed_link)
		*compressed_link = false;
	if (bytes_out)
		*bytes_out = 0;
	while (n && steps < FT_MAX_KEY_LEN + 1) {
		struct cds_ft_metadata *m;
		bool run;

		/*
		 * SKIP FIRST: a skip word's low tag bits are 0, so the external
		 * and internal predicates both read it wrong.  A parent_word
		 * names the node itself and is never skip-encoded.  So this is
		 * a belt-and-braces stop, not a modelled case.  It is also the
		 * pre-existing behaviour for anything that is not a plain
		 * interior.
		 */
		if (ft_node_skip_compressed(n))
			break;
		run = ft_node_compressed(n);
		if (run && !walk_runs)
			break;			/* @n is where the climb RESTS */
		if (!run && !ft_node_internal(n))
			break;			/* @n is where the climb RESTS */
		m = ft_flag_to_metadata(ft, n);
		/*
		 * Survives the drop -- it keeps a child or a co-located key.
		 * A RUN never does: exactly one child by construction, and a
		 * compressed node cannot carry external_nodes at all.
		 */
		if (!run && (ft_meta_nr_child(m) != 1 || m->external_nodes))
			break;			/* @n is where the climb RESTS */
		if (run && compressed_link)
			*compressed_link = true;
		/*
		 * @n is CLEARED by the drop.  The LAST node so cleared is the
		 * one whose slot in the node above must go.  The fold arms
		 * @pending_del_slot on that slot when the node above is
		 * @graft_c.  It is reported here, not re-derived, so the
		 * arming and the refusal read the same walk.
		 */
		if (top_out)
			*top_out = n;
		/*
		 * Use @m, not a re-derivation.  It is the same metadata, and
		 * it is the only form that is right for BOTH kinds now that a
		 * run can be @n.
		 */
		n = ft_parent_node(rcu_dereference(m->parent_word));
		steps++;
		/*
		 * ...AND HOW FAR THAT WAS IN BYTES.  This is NOT @steps once a
		 * run is on the chain.  A node's parent sits at
		 * @depth - span(PARENT) (ft_child_depth_of states the same
		 * arithmetic forwards), and a run spans its whole @len.  ONE
		 * number is produced here.  The arming's @fold_rest_depth reads
		 * it, and the freeze walk re-derives it step by step, so the
		 * two cannot drift.  It is identical to @steps on any all-plain
		 * chain.  That is every chain the trie actually produces: "no
		 * two adjacent compresseds" plus the chain-compress fuse mean a
		 * cleared chain is at most ONE run.
		 */
		if (bytes_out && n)
			*bytes_out += ft_node_span(ft, n);
		if (rest_out)
			*rest_out = n;
		if (!n || n == graft_c) {	/* the root, or the graft's node */
			*reached = true;
			break;
		}
	}
	return steps;
}

static
int ft_rekey_splice_pos_brackets(struct cds_ft *ft, const uint8_t *dst_ord,
		size_t dst_len, struct ft_ord_cell *pred,
		struct ft_ord_cell *succ)
{
	size_t max_len = ft->group->max_key_len;
	uint8_t scratch[FT_MAX_KEY_LEN];
	struct ft_ord_cell *sentinel = ft_ord_sentinel_cell(ft);
	bool pred_end = !pred || pred == sentinel;
	bool succ_end = !succ || succ == sentinel;
	size_t klen;

	if (pred_end && succ_end)
		return 0;			/* "empty list" -- the run is IN it */
	if (pred_end && ft_ord_first(ft) != succ)
		return 0;			/* head insert, but succ is not the min */
	if (succ_end && ft_ord_last(ft) != pred)
		return 0;			/* tail insert, but pred is not the max */
	if (pred && pred != sentinel) {
		klen = ft_rebuild_key_upwalk(ft, pred, scratch, max_len);
		if (!klen)
			return 0;		/* unreadable: torn */
		if (ft_rekey_prefix_range_cmp(scratch + (max_len - klen),
				klen, dst_ord, dst_len) >= 0)
			return -1;		/* a key extends the dst prefix */
	}
	if (succ && succ != sentinel) {
		klen = ft_rebuild_key_upwalk(ft, succ, scratch, max_len);
		if (!klen)
			return 0;		/* unreadable: torn */
		if (ft_rekey_prefix_range_cmp(scratch + (max_len - klen),
				klen, dst_ord, dst_len) <= 0)
			return -1;		/* a key extends the dst prefix */
	}
	return 1;
}

/*
 * THREE KINDS OF REFUSAL.  Each has its own return code.  They have different
 * futures, and only one of them is the caller's fault:
 *
 *   -EINVAL              ARGUMENT.  The caller got it wrong.  No state of the
 *                        trie would make the call legal (zero/over-long key,
 *                        unequal lengths, src and dst overlapping).  Terminal.
 *                        Surfaces as CDS_FT_STATUS_INVALID_ARGUMENT_ERROR.
 *
 *   FT_REKEY_UNCOVERED   SHAPE or MODE.  The arguments and the trie are fine,
 *                        but THIS writer does not cover the case (a coarse
 *                        trie, a skip-compressed source, a boundary parent at
 *                        its min_child floor, ...).  It is a gap in the
 *                        one-decide cut.  Widening the cut closes it.  So
 *                        this is the only code the dispatcher may answer
 *                        with a fallback.
 *
 *   -ENOTSUP             BUILD.  The feature is compiled out
 *                        (-DNO_FEATURE_FT_MERGE).  Terminal.  No runtime
 *                        state or wider cut can change it.
 *
 * All three used to be -EINVAL.  The dispatch could then not tell "the caller
 * passed nonsense" from "this writer cannot express this shape", so the
 * fallback ran for both.  An argument error was answered by staging a move.
 * That move then failed differently or, worse, succeeded.
 */
#define FT_REKEY_UNCOVERED	(-EDOM)
/*
 * The "no COW needed" return of ft_rekey_merge_cow_publish_parent.  The src
 * DETACH relocates the parent of the dst attach point (same-parent splice).
 * The caller publishes normally.  After the detach, it checks that the fold
 * happened.
 */
#define FT_REKEY_WITNESS_BY_DETACH	1
/*
 * The "not yet" return of the same function.  The frame is ON the source
 * path, so the COW must run AFTER the src detach.  It then reads the detach's
 * edits out of the descriptor (ft_rekey_merge_cow_after_decide).  The helper
 * recorded the frame.  The caller runs ft_rekey_merge_cow_after_detach once
 * ft_detach_node returns.
 */
#define FT_REKEY_COW_AFTER_DETACH	2

/*
 * WHAT THE CLIMB-ARMED FOLD WRITES INTO @graft_c'S ONE SLOT.
 *
 * Both modes remove the same second edit: a detach which republishes into a
 * body this flip retires.  They differ only in the VALUE the fresh copy holds
 * in that slot when it is created.  With one name per mode, the freeze, the
 * count and the detach skip read one variable.  They do not derive the shape
 * again.
 */
enum ft_rekey_fold_mode {
	FT_REKEY_FOLD_NONE = 0,
	FT_REKEY_FOLD_DROP,	/* the slot goes: the drop empties all of it */
	FT_REKEY_FOLD_PROMOTE,	/* the slot takes the resting node's key chain */
	FT_REKEY_FOLD_REPLACE,	/* the slot takes a DEL-recompacted copy of it */
	/*
	 * The slot takes ONE FRESH RUN.  The resting node had exactly two
	 * children and no keys of its own.  Dropping the moved arm would
	 * leave a ONE-CHILD internal, which skip mode does not allow
	 * (cds_ft_verify: "1 child and no external_nodes (should be a 1-byte
	 * compressed)").  A copy of the node minus the arm is the WRONG
	 * answer there: the survivor becomes a run and the resting node goes
	 * away entirely.  This is its own mode and not a REPLACE sub-case,
	 * because the abort ownership differs.  The value is a COMPRESSED
	 * fresh node, and a LIVE node is absorbed into it.
	 */
	FT_REKEY_FOLD_COLLAPSE,
};




/*
 * DID THE ONE STRUCTURAL EDIT ACTUALLY RIDE THE GRAFT'S COPY?
 *
 * Two things depend on the answer, and BOTH must read it the same way:
 *   - the graft's key-count delta.  A folded move is net ZERO: the subtree
 *     leaves and re-enters the same node.
 *   - whether the detach runs at all.
 *
 * The copy loop that consumes the fold sets these flags.  The arming never
 * does.  So a shape that was armed and NOT consumed answers false here.  It
 * is refused, and is not detached into a superseded body.
 *
 * ONE flag answers for all three modes, because all three ride the SAME fold:
 *   - the height-0 drop (@del_folds_into_graft, where BP IS the recompacted
 *     node);
 *   - the climb-armed drop;
 *   - the promote, which is that drop carrying a replacement value
 *     (@pending_del_replace).
 *
 * @mode and @slot are not read.  They are parameters so that the signature
 * still says what the question is ABOUT.  A future mode that needs its own
 * witness also has a place to say so.
 */
static inline
bool ft_rekey_move_folded(const struct ft_flip_txn *txn,
		enum ft_rekey_fold_mode mode, struct cds_ft_inode_flag **slot)
{
	(void) mode;
	(void) slot;
	return txn->pending_del_folded;
}

/*
 * ☑ THE VALUE-WRITING FOLDS RUN ON EVERY BUILD.  There used to be a predicate
 * here, ft_rekey_fold_value_ok.  It was false wherever FEATURE_FT_COMPRESS was
 * on.  It refused the two modes whose fold WRITES a value into @graft_c's
 * slot (REPLACE, PROMOTE).  The reason given was that such a slot may owe a
 * SKIP_X DUAL: a second, skip-encoded path to the same child.  Only a PUBLISH
 * records a dual, and this fold makes no publish.
 *
 * ☠ THE PREMISE WAS ALREADY FALSE, and the next predicate showed it.  A dual
 * arises only when the PUBLISH PARENT is compressed.  ft_rekey_fold_shape_ok
 * below refuses a compressed or skip-encoded @graft_c and @edited outright.
 * The fold's slot lives in @graft_c.  No dual is owed.  The header of
 * ft_rekey_collapse_shape states exactly this argument for the COLLAPSE mode.
 * The argument never depended on which mode was asking.  COLLAPSE has been
 * armed on compressing builds since @b60510f3.  It is armed 55 times per
 * corpus and commits cleanly.
 *
 * ☠ WHAT THE GUARD REALLY HELD BACK was a LIVELOCK.  It is fixed below.  The
 * REPLACE arm looked its drop slot up with @fold_drop_child, the RESOLVED
 * flag, but the slot holds the SKIP-ENCODED word.  ft_node_find_child
 * compares RAW WORDS, so it could never match.  "stale plan: re-descend"
 * could never clear, and the op spun for ever refilling its reserve.  This
 * was ATTRIBUTED, not assumed.  Armed with that argument reverted, the
 * 3000-shape corpus wedges at seed ~324 (rc=124 in a 4G cage).  With the
 * argument in place, the corpus completes.
 *
 * MEASURED at the arming, on 3000 shapes x rank on/off x list on/off.  The
 * builds were --enable-rcu-debug default / nocompress / noskip, and release:
 *   - default and release: 2597 -> 2632 served;
 *   - noskip: 2557 -> 2649;
 *   - nocompress: unchanged (it never had the guard);
 *   - ZERO failures on every leg.
 * The +35 on the default build is exactly the two modes' own counts: PROMOTE
 * armed and consumed 14, REPLACE 21.  So the modes are not only admitted,
 * they FIRE.
 * The concurrent run used the default build under the documented-safe COARSE
 * strategy, with four perturbers on 2 cpus, 12 x 5 s.  Both arms were 12/12
 * clean, and the armed arm completes MORE moves (214 vs 195).
 */

/*
 * THE SHAPE BOTH FOLD MODES REQUIRE, in one expression so they cannot drift.
 *
 * @edited is the node whose slot in @graft_c the fold will write.  It is the
 * topmost node the drop empties (DROP), or the node the walk rests on
 * (PROMOTE).  Both need the SAME three things:
 *
 *   - a NOSPLIT, non-merge, uncut move.  A merge sets the prep deliberately.
 *     A cut source makes "the drop empties BP" untrue.  Neither is this
 *     shape.
 *   - @graft_c and @edited PLAIN.  A compressed or skip-encoded link puts a
 *     SECOND path on the same child (the SKIP_X dual).  Refreshing that dual
 *     is a publish, and this cut has no owner for it.
 *   - NOT the in-place tier.  The in-place arm of ft_node_set_nth_rec never
 *     runs the copy loop that consumes the fold.  The drop would go
 *     unconsumed, and the detach would run after all, into the body the
 *     graft superseded.  This is refused at PLAN time, before the reserve
 *     sets a bit that no bail takes back.
 */
/*
 * THE TWO-CHILD KEYLESS RESTING NODE: the shape whose REPLACE is not a replace.
 *
 * Take a node with exactly two children and no keys of its own.  Dropping
 * the moved arm from it leaves a ONE-CHILD internal, which skip mode does not
 * allow (cds_ft_verify: "1 child and no external_nodes (should be a 1-byte
 * compressed)").  ft_detach_node already refuses to publish such a node: its
 * shape-D gate folds the chain-compress prune into the same commit.  The
 * old-direction COLLAPSE of ft-graft.h does the same thing on this very
 * shape, one lane over, and says so.  The fold's REPLACE arm had no such
 * gate.  Its `nr_child >= 2` test is:
 *   - right for three or more children (the drop leaves two);
 *   - right for two WITH keys (the drop leaves one child plus keys, which
 *     verify exempts);
 *   - wrong for exactly this shape.
 *
 * ☞ ONLY ON A SKIP GROUP.  Without a skip group, a one-child internal is a
 * legal resting shape and the plain REPLACE is correct.  This must then stay
 * false, so that the REPLACE arm reads exactly as it did.
 *
 * ☞ AND IT ASKED NO BUILD QUESTION.  A SKIP_X dual arises only when the
 * PUBLISH PARENT is compressed.  ft_rekey_fold_shape_ok refuses that for both
 * @graft_c and @edited.  The collapsed run's slot is in @graft_c, so no dual
 * is owed.  That argument was written here for this mode.  It later retired
 * the build guard for the OTHER two modes (see above).  It never depended on
 * which mode was asking.
 */
static inline
bool ft_rekey_collapse_shape(const struct cds_ft *ft,
		const struct cds_ft_metadata *rm)
{
	return ft_group_skip_compressed(ft->group) &&
		ft_meta_nr_child(rm) == 2 && !rm->external_nodes;
}

/*
 * DOES THE WORD A SLOT HOLDS NAME @node?
 *
 * ☞ SPEAK THE ENCODED WORD, REASON ABOUT THE NODE IT DENOTES.  The fold's
 * REPLACE arm already follows this rule for ft_node_find_child.  On a skip
 * group, a slot whose child is a path-compressed RUN holds the SKIP_X
 * spelling of that run: @child | (len << FT_SKIP_LEN_SHIFT).  That word names
 * the run's CHILD, so it is never equal to the run's own flag.  The climb's
 * @climb_top is a flag the climb read from a parent_word, which is never
 * skip-encoded.  An identity compare against @climb_top can therefore never
 * hold for a compressed link.  This kept every such shape out of the fold.
 *
 * ☠ AND IT IS DERIVED FORWARD, FROM THE RUN, NEVER BY RESOLVING THE WORD.
 * ft_resolve_skip_compressed would read the answer out of the SKIP CHILD'S
 * BACK POINTER (ft_skip_to_compressed).  The header of ft_skip_to_compressed
 * says callers "get whatever the back-pointer currently says".  For an
 * EXTERNAL child it reads node->prev.  This project has measured that word
 * STALE AT REST, unbounded, on an idle trie.  A back-pointer may nominate a
 * CANDIDATE.  It may never deliver a VERDICT about where a node lives.
 *
 * Worse, resolving would spend a guard that is not this helper's.  The
 * identity compare it replaces carried TWO facts at once:
 *   - the slot holds @node's word;
 *   - @slot really is @node's slot.
 * Only the first is re-checked downstream:
 *   - the build re-reads the same slot for the same word;
 *   - the copy loop matches by slot ADDRESS;
 *   - the freeze walk re-resolves @edited's PARENT, not which of its slots.
 * So a resolver that accepted the wrong slot would create @graft_c's fresh
 * copy without some OTHER child.  That subtree would be silently unlinked,
 * and nothing after this point could tell.
 *
 * This helper compares the run's own (child, len) against the word's DECODED
 * fields.  That keeps both facts: two live runs cannot share a child, because
 * a child has one parent.  The compare is exactly as strong as the plain-flag
 * identity compare beside it.  It touches no back-pointer.  A torn read of
 * either field answers false, which costs an -EAGAIN and never a fold.
 *
 * The spelling rule mirrors its producer, ft_publish_compressed.  A run has
 * no SKIP_X form when skip compression is off for the group, or when the run
 * is too long to encode.  Such a run has only the plain flag, and the
 * identity compare above is then the whole answer.
 *
 * NULL, an external word (tag 0) and a peer's parked flip proxy all answer
 * false: none of them decodes as a skip word naming this run.
 */
/* ☞ NO @ft: this reads the two words and nothing else.  There is no group
 * lookup and no back-pointer.  The signature states this property by not
 * taking @ft. */
static inline
bool ft_rekey_slot_names(struct cds_ft_inode_flag *raw,
		struct cds_ft_inode_flag *node)
{
	const struct cds_ft_compressed_node *cn;

	if (!raw || !node)
		return false;
	if (raw == node)
		return true;		/* the plain spelling */
	/* Only a RUN has a second spelling. */
	if (!ft_node_compressed(node) || !ft_node_skip_compressed(raw))
		return false;
	cn = ft_compressed_node_ptr(node);
	return (unsigned int) ft_skip_len(raw) == (unsigned int) cn->len &&
		ft_skip_child_ptr(raw) == rcu_dereference(cn->child);
}

/*
 * @edited_dropped: the fold DROPS @edited's slot and writes no value into it.
 * So @edited is not edited at all.  It is unlinked whole, with every node
 * below it on the cleared chain.  That is the DROP mode.  It is why a
 * COMPRESSED @edited is admissible there and nowhere else:
 *
 *   - The "second path" the plain-interiors rule protects is the SKIP_X dual.
 *     A dual is only REFRESHED when the compressed node SURVIVES and its
 *     child is republished.  Here the run and the slot that spells it both
 *     die inside @graft_c's one fresh copy.  Nothing is left to refresh.
 *     (The surviving-run case is the compressed publish parent.  It is
 *     served at @088ec182 by ANNOUNCING the reserve's dual, which is a
 *     different mechanism.)
 *   - A run has exactly one child BY CONSTRUCTION and can carry no
 *     external_nodes.  So it is the one ancestor kind that the drop is
 *     GUARANTEED to empty.  The climb's own header already says exactly
 *     this about walking runs.
 *
 * A SKIP-ENCODED @edited stays refused on both modes.  The climb reads flags
 * out of parent_words, which are never skip-encoded.  So this is a
 * fail-closed test and not a modelled case.
 */
/*
 * @cut_ok: the caller's arm can express a CUT SOURCE.
 *
 * A cut means the src key ends INSIDE a run.  @d_src.nf is the run.  S_top is
 * the tail that ft_rekey_cow_stop manufactures.  That COW retires the run
 * WHOLE.  BP is then not "emptied by the drop": the whole SLOT goes.  Every
 * key under a run passes through all of its bytes, so everything below that
 * slot moves.  This is the right product for the value-writing modes, where
 * BP SURVIVES as a copy created without the run's slot.  It is not DROP's
 * shape at all: DROP's premise is that the chain ABOVE BP is emptied.
 *
 * The tree has always served this one level down.  @del_folds_into_graft (BP
 * IS @graft_c) carries no @src_cut term.  Its build arm arms the very same
 * pair (@d_src.nfp, @d_src.nf_raw) as this one.
 */
static inline
bool ft_rekey_fold_shape_ok(struct cds_ft *ft,
		struct cds_ft_inode_flag *graft_c,
		struct cds_ft_inode_flag *edited,
		enum ft_graft_prep prep, bool merge_dst, unsigned int src_cut,
		bool edited_dropped, bool cut_ok)
{
	return !merge_dst && (!src_cut || cut_ok) &&
		prep == FT_GRAFT_PREP_NOSPLIT &&
		!ft_in_place_excl_ok(ft) &&
		ft_node_internal(graft_c) && !ft_node_compressed(graft_c) &&
		!ft_node_skip_compressed(graft_c) &&
		(ft_node_internal(edited) ||
			(edited_dropped && ft_node_compressed(edited))) &&
		(edited_dropped || !ft_node_compressed(edited)) &&
		!ft_node_skip_compressed(edited);
}

/*
 * THE CHAIN A CLIMB-ARMED FOLD ORPHANS: freeze it into the SAME commit.
 *
 * @bp is the src junction.  @top is the child of @graft_c whose slot the fold
 * DROPS (@pending_del_slot).  Between them lies the run of single-child,
 * key-less interiors the drop empties, @bp innermost.  The flip that
 * publishes @graft_c's copy is also what unlinks them.  So their
 * freeze-on-free tombstones must ride THAT commit.  Their memory is reclaimed
 * after it (@rc, drained by ft_rekey_detach_free_orphans).
 *
 * This re-walks ft_detach_node's elevation climb.  It does NOT call into it.
 * That climb evaluates emptiness against the bodies this graft has already
 * superseded, which is the whole defect the fold exists to avoid.  Everything
 * BELOW the walk is the detach's machinery, verbatim:
 *   - ft_detach_orphan_planlock takes each node's word and validates the
 *     single-child premise ON the marked snapshot;
 *   - ft_detach_freeze_orphans records the fenced {LOCK|s -> TOMBSTONE|s}
 *     terminal from that snapshot.
 * So a peer that GREW one of the nodes between the plan and the commit tears
 * the expected-old, and this commit aborts clean.
 *
 * @steps is the length the gate's own climb measured.  The walk must produce
 * exactly that length and end on @top.  A re-derivation that disagrees means
 * the neighbourhood moved under the plan.  @pending_del_slot was armed from
 * the plan, not from this walk.
 *
 * PLAIN INTERIORS AND RUNS.  A SKIP-ENCODED link is still refused.  A skip
 * word names the far end, so it is not the word the fold's expected-old
 * holds.  The climb never produces one (it reads flags out of parent_words).
 * A COMPRESSED link IS admitted.  The walk needs three things from a node it
 * retires, and all three are true of a run:
 *   - ft_flag_to_metadata reaches its metadata (the same form the climb
 *     itself uses);
 *   - its state word carries nr_child == 1 (ft_meta_nr_child_set at every
 *     creation site) and it can hold no external_nodes;
 *   - ft_detach_freeze_orphans and ft_rekey_detach_free_orphans ALREADY
 *     dispatch on ft_node_compressed for the tombstone and the free.
 * The detach's own elevation retires the trailing run the same way
 * (ft-remove.h ~2985).  A run differs from a plain interior by its SPAN: it
 * rises @len bytes in one step.  So the depth bookkeeping steps by
 * ft_node_span and not by one.
 *
 * ☠ THE TWO REFUSALS ARE NOT THE SAME REFUSAL.  Answering both with -EAGAIN
 * is an infinite loop.
 *   - A node that MOVED or is HELD is a peer.  A re-descent derives a
 *     different plan.
 *   - A node that is COMPRESSED or SKIP-ENCODED is a property of the
 *     STRUCTURE.  The identical plan comes back for ever, and the retry loop
 *     above has no cap on a release build.
 * So the structural refusal returns FT_REKEY_UNCOVERED.  The dispatcher
 * answers it terminally, exactly as the sibling shape gates do.
 *
 * Return 0 with @rc->orphans filled and every mark still HELD (the caller's
 * sweep owns them).  Otherwise return -EAGAIN or FT_REKEY_UNCOVERED, with
 * every mark this function took RELEASED and @rc clean.
 */
static
int ft_rekey_fold_freeze_orphans(struct cds_ft *ft,
		struct ft_lock_ctx *ctx, struct ft_flip_txn *txn,
		struct cds_ft_inode_flag *bp, unsigned int bp_depth,
		struct cds_ft_inode_flag *top,
		struct cds_ft_inode_flag *promoted,
		struct cds_ft_inode_flag *promote_val,
		struct cds_ft_inode_flag *edited,
		struct cds_ft_inode_flag *graft_c, int steps,
		struct ft_detach_recompact_out *rc,
		struct ft_held_anchor *held, int *nr_held)
{
	struct cds_ft_inode_flag *n = bp;
	unsigned int depth = bp_depth;
	int ret = -EAGAIN;
	int i;

	rc->nr_orphans = 0;
	/*
	 * A PROMOTE whose walk never moved (@steps == 0) empties no chain at
	 * all.  The node the slot names IS @bp, and the loop below has
	 * nothing to collect before it.  Skipping the loop is not an
	 * optimisation: entered with @steps == 0, it would refuse on its own
	 * bound.
	 */
	if (steps == 0)
		goto chain_done;
	for (;;) {
		struct cds_ft_metadata *m;

		if (!n || ft_node_flip_proxy(n))
			goto refuse;		/* a peer mid-commit: transient */
		if ((!ft_node_internal(n) && !ft_node_compressed(n)) ||
				ft_node_skip_compressed(n)) {
			ret = FT_REKEY_UNCOVERED;	/* structure, terminal */
			goto refuse;
		}
		if (rc->nr_orphans >= steps || depth == 0)
			goto refuse;
		/*
		 * ft_flag_to_metadata is the form that names the kind it is
		 * given.  The climb itself uses it for its own @m.  The old
		 * spelling was not wrong here.  For a PLAIN COMPRESSED flag,
		 * ft_node_ptr masks ~7UL (bit 0 clear), and
		 * cds_ft_item_to_metadata(ft_node_ptr(n)) recovers the same
		 * metadata.  But ft_flag_to_metadata stays right if a
		 * skip-encoded @n ever reaches here.  The tree's own
		 * kind-dispatcher for this job also reads the same way
		 * (ft_detach_freeze_orphans).
		 */
		m = ft_flag_to_metadata(ft, n);
		/*
		 * A run satisfies this by construction: exactly one child,
		 * never any external_nodes.  So the premise is asserted
		 * here, not assumed.  The check reads the same word a peer
		 * would have to commit into to break the premise.
		 */
		if (ft_meta_nr_child(m) != 1 || m->external_nodes)
			goto refuse;
		/*
		 * ☠ PUBLISH THE MARKS THIS WALK ALREADY HOLDS BEFORE TAKING
		 * THE NEXT ONE, and again before the freeze.
		 * @ctx->held.extra names this array, and both consumers READ
		 * it:
		 *   - the next acquire, so that the op does not take its OWN
		 *     earlier mark for a peer's;
		 *   - the tombstone's owner-witness, under per-node
		 *     granularity.  There the anchor IS the node's own word
		 *     and no registry slot is spent.  The witness is then
		 *     the only thing that can say the op holds the word it
		 *     is about to park SW.
		 * ft_detach_node's own orphan walk grows @nr_extra at
		 * exactly these two points.
		 */
		ctx->held.nr_extra = (unsigned int) *nr_held;
		if (ft->lock_fine && ft_detach_orphan_planlock(ft, ctx, n, depth,
				m, true /*require_single_child*/, held, nr_held))
			goto refuse;
		rc->orphans[rc->nr_orphans++] = n;
		if (n == top)
			break;
		/*
		 * Step up through the RESOLVING reader, not a raw
		 * parent_word load.  A peer re-home parks a type-7 flip
		 * proxy on that word.  It commits the word with the slot
		 * offset as ONE txn.  So a raw read can pair a settled
		 * offset with a parked parent.  This walk asks only for the
		 * parent.  It still asks the way the arming above asked for
		 * the (parent, slot) pair.
		 */
		n = NULL;
		(void) ft_resolve_parent_slot(m, ft, &n);
		/*
		 * ☠ A NODE'S PARENT SITS AT @depth - span(PARENT), not at
		 * @depth - 1.  A run rises its whole @len in one step
		 * (ft_child_depth_of states the same arithmetic forwards).
		 * This walk used to be restricted to a plain chain, where
		 * the two are identical.  This depth DATES the DLM member of
		 * the next acquire.  So a wrong depth is a wrong anchor, not
		 * a wrong node.  An underflow means the neighbourhood moved
		 * under the plan: the walk's own re-derivation disagrees
		 * with the gate's.  @steps is checked against exactly that
		 * below.
		 */
		{
			unsigned int span = n ? ft_node_span(ft, n) : 0;

			if (span > depth)
				goto refuse;
			depth -= span;
		}
	}
	if (rc->nr_orphans != steps)
		goto refuse;
chain_done:
	/*
	 * THE PROMOTED NODE dies too, and it dies LAST.  The drop leaves it
	 * with no child, and the fold has published its key chain in its
	 * place.  So nothing reaches it after this flip.  Its premise is
	 * exactly one child, which the drop takes.  The same plan-lock the
	 * chain uses re-validates that premise ON the marked snapshot.
	 */
	if (promoted) {
		struct cds_ft_metadata *pm;

		/*
		 * PLAIN ONLY here, and not by inheritance.  PROMOTE
		 * publishes the promoted node's EXTERNAL CHAIN HEAD in its
		 * place.  A run carries no external_nodes at all.  So a
		 * compressed @promoted is not a shape this mode has.
		 */
		if (!ft_node_internal(promoted) || ft_node_compressed(promoted) ||
				ft_node_skip_compressed(promoted) ||
				ft_node_flip_proxy(promoted)) {
			ret = FT_REKEY_UNCOVERED;
			goto refuse;
		}
		if (rc->nr_orphans >= FT_MAX_DEPTH || depth == 0)
			goto refuse;
		pm = cds_ft_item_to_metadata(ft_node_ptr(promoted));
		if (ft_meta_nr_child(pm) != 1 || !pm->external_nodes)
			goto refuse;
		/*
		 * ☠ AND IT MUST STILL BE THE SAME HEAD.  @promote_val was
		 * read at the gate, and the copy loop has already baked it
		 * into the fresh body.  If a peer spliced a NEW head onto
		 * this chain in between, that fresh body would publish a
		 * STALE one.  ft_detach_node's own walk makes exactly this
		 * check against its @topmost_external_nodes and bails
		 * -EAGAIN.  This arm is that walk re-derived, so it owes the
		 * same check.  The refusal is transient by construction: a
		 * re-descent reads the new head.
		 */
		if (promote_val && (struct cds_ft_inode_flag *)
				ft_dereference_external(pm->external_nodes) !=
				promote_val)
			goto refuse;
		ctx->held.nr_extra = (unsigned int) *nr_held;
		if (ft->lock_fine && ft_detach_orphan_planlock(ft, ctx, promoted,
				depth, pm, true /*require_single_child*/,
				held, nr_held))
			goto refuse;
		rc->orphans[rc->nr_orphans++] = promoted;
	}
	/*
	 * ...and the edited node must still hang off the node the fold writes its
	 * slot IN.  The arming read that pair; this is the re-read that keeps the
	 * walk and the slot naming ONE edge.
	 */
	{
		struct cds_ft_inode_flag *edited_parent = NULL;

		/*
		 * ft_flag_to_metadata: @edited is the DROPPED node on the DROP
		 * mode, and that one may be a run.
		 */
		if (!ft_resolve_parent_slot(ft_flag_to_metadata(ft, edited), ft,
				&edited_parent) || edited_parent != graft_c)
			goto refuse;
	}
	/*
	 * The up-front reservation already carries these.  It budgets
	 * ft_freeze_reserve(@d_src.depth + 1) for the orphan chain of an
	 * ELEVATING DETACH.  This chain is that same chain, cleared by the
	 * fold instead.  The detach it replaces does not run, so nothing
	 * else spends the budget.
	 */
	ctx->held.nr_extra = (unsigned int) *nr_held;
	ft_detach_freeze_orphans(ft, txn, ctx, rc->orphans, rc->nr_orphans,
			NULL /*trailing skip-target: plain chain*/,
			ft->lock_fine ? held : NULL, NULL,
			/* @held is the committing frame's own (fold_held[]) */
			false);
	return 0;
refuse:
	/* STRICT: a refusal commits nothing, so every mark left is ours. */
	for (i = *nr_held - 1; i >= 0; i--)
		if (!held[i].shared && !held[i].txn_owned)
			ft_meta_lock_release(held[i].lock);
	*nr_held = 0;
	ctx->held.nr_extra = 0;
	rc->nr_orphans = 0;
	return ret;
}

/*
 * RELEASE what the fold's chain plan-locks still hold.  Do it BEFORE anyone
 * frees the nodes those marks live on.
 *
 * ☠ THAT ORDER IS THE WHOLE POINT.  A mark's word is the ORPHAN'S OWN
 * metadata.  On the committed path this op frees the orphans itself
 * (ft_rekey_detach_free_orphans).  A sweep placed after that free reads, and
 * conditionally writes, memory this function has already handed back.
 *   - On the concurrent tier this is benign: the free is grace-period
 *     deferred inside the caller's read section.
 *   - On an EXCLUSIVE trie it is NOT benign.  cds_ft_free_item returns the
 *     item to the arena at once, and the freelist link is a union over the
 *     metadata.
 * ft_detach_node sweeps its own orphan marks at its `end:` label, before its
 * caller frees.  The fold owes the same order.
 *
 * Idempotent by construction: it clears the count.  The unconditional sweep
 * at @sweep then iterates nothing on a path that already came through here.
 * Every OTHER path published nothing, so its marks are still {LOCK|s}.  This
 * sweep is what returns the structure byte-for-byte to its pre-op state.  A
 * mark whose anchor SURVIVES the retire is not ours.  ft_flip_txn_lock_own
 * handed it to the txn (@txn_owned).  The txn is the only owner that can
 * tell a consumed mark from a peer's fresh one on a still-lockable word.
 */
static inline
void ft_rekey_fold_sweep_orphan_marks(struct ft_held_anchor *held, int *nr_held,
		bool committed)
{
	int i;

	/*
	 * @committed: this sweep has TWO callers, on opposite sides of the
	 * commit:
	 *   - the bail exit, whose marks are provably still ours;
	 *   - the post-commit reclaim, whose fences the commit's fenced
	 *     tombstones may already have taken.
	 * Converting the sweep to the strict release for BOTH made ft_unit
	 * abort in the rekey family (assertion `s & FT_STATE_LOCK').
	 * Clear-if-held used to no-op on the consumed word, and the strict
	 * release asserts on it.
	 *
	 * ☐ THE COMMITTED ARM IS NOT CONVERTED YET.  A proper conversion
	 * samples ft_lock_terminal_drops_lock for each mark BEFORE the
	 * commit, because the txn wrapper is gone afterwards.  That is a
	 * change to the caller, not to this helper.  Until then, do not
	 * clear a word the op cannot prove is its own: leave it and COUNT
	 * it.  A leak is bounded and local.  A theft corrupts a peer's
	 * exclusion.
	 */
	for (i = 0; i < *nr_held; i++)
		if (!held[i].shared && !held[i].txn_owned) {
			if (committed) {
				FT_ORPHAN_CONSUMED();
				FT_ORPHAN_PREMISE_CHECK(held[i].lock);
				continue;
			}
			ft_meta_lock_release(held[i].lock);
			/*
			 * SCRUB only RELEASED-LIVE: a TOMBSTONED word is a
			 * CONSUMED fence and must keep answering holds().
			 */
			if (!(CMM_LOAD_SHARED(held[i].lock->state) &
					FT_STATE_TOMBSTONE))
				held[i].shared = true;
		}
	*nr_held = 0;
}



/*
 * A FOLDED COLLAPSE'S TWO LIFETIMES (struct ft_chain_compress_reclaim).  The
 * collapse records into this op's txn and commits nothing.  So it hands its
 * nodes back and does not free them.  When it returns, the chain it retires
 * is still LIVE AND LINKED, and a reader is entitled to be walking it.  Each
 * node belongs to one side of the commit.  The split is the same one
 * ft_detach_recompact_out draws between @old_node and @new_flag.
 *
 * RETIRED CHAIN: freed when the commit SUCCEEDS, because that commit is what
 * unlinks the nodes.  The free helpers themselves defer the free past
 * readers.
 */
static inline
void ft_rekey_collapse_free_retired(struct cds_ft *ft,
		struct ft_chain_compress_reclaim *rc)
{
	if (rc->boundary)
		free_cds_ft_node(ft, rc->boundary);
	if (rc->parent_cn)
		free_compressed_node(ft, rc->parent_cn);
	if (rc->child_cn)
		free_compressed_node(ft, rc->child_cn);
	memset(rc, 0, sizeof(*rc));
}

/*
 * An ELEVATING detach's ORPHAN CHAIN: the ancestors its upward walk cleared,
 * plus the trailing skip-compressed target.  It has the same lifetime as the
 * retired chain above, for the same reason.  THIS commit is what unlinks the
 * nodes, and their freeze-on-free tombstones rode it.  So they are reclaimed
 * here and nowhere else.  An ABORT leaves them live and linked, so no abort
 * path frees them.
 */
static inline
void ft_rekey_detach_free_orphans(struct cds_ft *ft,
	struct ft_detach_recompact_out *rc)
{
	int i;

	if (rc->orphan_trailing)
		free_compressed_node(ft, ft_skip_to_compressed(ft,
			rc->orphan_trailing));
	for (i = 0; i < rc->nr_orphans; i++) {
		if (ft_node_compressed(rc->orphans[i]))
			free_compressed_node(ft,
				ft_compressed_node_ptr(rc->orphans[i]));
		else
			free_cds_ft_node(ft, ft_node_ptr(rc->orphans[i]));
	}
	rc->nr_orphans = 0;
	rc->orphan_trailing = NULL;
}

/*
 * MERGED NODE: recorded but never published.  So it is ours to reclaim when
 * the commit ABORTS or the op bails before it.  The retired chain is NOT freed
 * on these paths: nothing unlinked it, so it is still live.
 */
static inline
void ft_rekey_collapse_free_unpublished(struct cds_ft *ft,
		struct ft_chain_compress_reclaim *rc)
{
	if (rc->new_cn)
		free_compressed_node_unpublished(ft, rc->new_cn);
	memset(rc, 0, sizeof(*rc));
}

/*
 * Attempt age past which a refusal that COULD be a peer is treated as the shape
 * it also could be.  See the -1 arm of the bracket check below for why the two
 * are indistinguishable from one attempt.
 */
#define FT_REKEY_UNCOVERED_AFTER	4096

/*
 * HAND THE FOLD'S OUT-OF-REGISTRY MARKS TO @txn, at the TAKE.
 *
 * ft_rekey_cow_stop's fences live in a fn-scope array, not in a lock-SET.
 * cow_stop reaches 17 distinct anchors on the unit fixture.  A set is the unit
 * the MCAS install SORTS for deadlock-free acquisition, so the array is the
 * right home for them.  But "not a set" was read as "not in the registry
 * either".  The registry is a different question: it is what a RECORD-time
 * owner check can see (ft_flip_txn_owns reads locks[] and nothing else).
 *
 * Every later acquire of one of these words DEDUPES against the array through
 * @held.extra.  It correctly registers nothing of its own
 * (ft_glue_acquire_reparent_marks' @h.shared arm).  So without this function
 * the registry never learns the word by ANY route.  Yet the re-parent records
 * those marks exist to license name it as their owner.
 *
 * Registration TRANSFERS the clear (ft_unlock_held's header).  @txn_owned
 * follows it, and the post-abort sweep must skip those entries: one owner per
 * fence.  A SHARED mark transfers nothing: the acquire that FIRST took the
 * word owns both.  This function is idempotent, so a caller may re-run it as
 * the array grows.
 *
 * The release records stay where they are (the marks loop before the commit).
 * ft_flip_txn_record_anchor_release_held reads the word through the txn and
 * chains onto whatever is already recorded there.  So it is order-independent
 * by construction.  The registration was NOT order-independent: every record
 * between here and there needs it.
 */
static
void ft_rekey_marks_to_txn(struct ft_flip_txn *txn,
		struct ft_held_anchor *marks, unsigned int nr_marks)
{
	unsigned int i;

	for (i = 0; i < nr_marks; i++) {
		if (marks[i].shared || marks[i].txn_owned)
			continue;
		(void) ft_flip_txn_lock_own(txn, &marks[i]);
	}
}

#if defined(DEBUG_RCU) || defined(CONFIG_RCU_DEBUG)
/*
 * THE DST-FRESHNESS OBLIGATION'S REACH COUNTERS.  A SILENT DETECTOR IS NOT
 * COVERAGE (three zeros hide behind one).  The publish-time asserts below pin
 * the invariant the BARE-HEAD src leg rests on: "every move publishes a fresh
 * library node at the destination attach point".  An assert that is compiled
 * in but never REACHED proves nothing about it.  So count each arm's
 * consultations and report at exit.  A green run can then state "asked N
 * times, never wrong" instead of "never failed".  The arms are:
 *   [0] the MERGE arm's union node M;
 *   [1] the NOSPLIT graft's recompacted attach parent;
 *   [2] the same-parent splice served by the src DETACH's relocation
 *       (FT_REKEY_WITNESS_BY_DETACH), whose witness is a fold the caller
 *       re-checks after the detach.
 * The counters are guarded on the same pair urcu_assert_debug itself is.  So
 * they exist exactly where the asserts can fire.
 */
static unsigned long ft_rekey_dst_fresh_reach[4];

static void ft_rekey_dst_fresh_fini(void) __attribute__((destructor));
static void ft_rekey_dst_fresh_fini(void)
{
	if (!ft_rekey_dst_fresh_reach[0] && !ft_rekey_dst_fresh_reach[1] &&
			!ft_rekey_dst_fresh_reach[2] && !ft_rekey_dst_fresh_reach[3])
		return;
	fprintf(stderr, "FT REKEY DST-FRESHNESS REACH: merge_union=%lu "
		"graft_recompact=%lu same_parent_detach=%lu post_detach_cow=%lu\n",
		ft_rekey_dst_fresh_reach[0], ft_rekey_dst_fresh_reach[1],
		ft_rekey_dst_fresh_reach[2], ft_rekey_dst_fresh_reach[3]);
	fflush(stderr);
}

# define FT_REKEY_DST_FRESH_REACH(arm) uatomic_inc(&ft_rekey_dst_fresh_reach[arm])
#else
# define FT_REKEY_DST_FRESH_REACH(arm) do { } while (0)
#endif

/*
 * ft-lifecycle.h is included AFTER this unit, and the repair below needs the
 * scope-less form.  One declaration, one definition, one TU.
 */
static enum cds_ft_status ft_recompute_max_used_key_len(struct cds_ft *ft);

/*
 * WOULD THE LENGTH CHANGE PUSH A MOVED KEY PAST THE GROUP'S LIMIT?
 *
 * A moved key K becomes @dst_key || (K minus the @src_key prefix).  Its new
 * length is dst_len + (len(K) - src_len).  len(K) is not known here.  The
 * trie's max_used_key_len HINT bounds it, so this is a CONSERVATIVE refusal.
 * It can refuse a move that no real key would overflow.  It never admits one
 * that would.
 *
 * ★ ONE PLACE DEFINES THE ANSWER.  Two call sites ask this:
 *   - the shape gate in ft_rekey_graft_simple_attempt, which refuses on it;
 *   - the hint repair in ft_rekey_graft_simple_locked, which pays for an
 *     exact answer when this says yes.
 * A second copy of the arithmetic would let the two drift.  The repair must
 * fire on exactly the cases the gate refuses.  Otherwise it either never
 * runs or runs on every move.
 */
static
bool ft_rekey_len_change_overflows(const struct cds_ft *ft,
		size_t src_len, size_t dst_len)
{
	size_t src_max = uatomic_load(&ft->max_used_key_len, CMM_RELAXED);

	if (dst_len > ft->group->max_key_len)
		return true;		/* no headroom at all; never underflow below */
	return src_max > src_len &&
		src_max - src_len > ft->group->max_key_len - dst_len;
}

#ifdef FEATURE_FT_MERGE
/*
 * The frame ft_rekey_merge_cow_publish_parent defers to after the src detach
 * (see ft_rekey_merge_cow_after_decide).
 */
struct ft_rekey_cow_after {
	bool armed;
	struct cds_ft_inode_flag *cow_nf, *cow_pnf, **cow_slot;
	unsigned int cow_depth, cow_pdepth;
};

/*
 * Is (@cow_nf, republished into @cow_pnf's slot) a frame that cow_stop and
 * the publish can take TODAY?  It needs both:
 *   - a PLAIN INTERNAL node (POPCOUNT / PIGEON).  A COMPRESSED one is a run.
 *     Republishing a run means re-encoding the SKIP_X dual in ITS parent and
 *     handing the glue a skip-encoded top.  The back edge of such a top
 *     inverts to the node this commit retires.
 *   - a NON-COMPRESSED parent, so the publish into it is ONE store and not
 *     also a dual re-encode.  NULL is the root's, and its slot takes the
 *     always-MW root route.
 */
static inline
bool ft_rekey_cow_frame_ok(struct cds_ft_inode_flag *cow_nf,
		struct cds_ft_inode_flag *cow_pnf)
{
	unsigned int pti;

	if (ft_node_external(cow_nf) || ft_node_compressed(cow_nf))
		return false;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(cow_nf))
		return false;
	if (cow_pnf && ft_node_skip_compressed(cow_pnf))
		return false;
#endif
	if (cow_pnf && (ft_node_compressed(cow_pnf) ||
			ft_node_external(cow_pnf)))
		return false;
	pti = ft_node_type(cow_nf);
	return ft_types[pti].type_class == FT_POPCOUNT ||
		ft_types[pti].type_class == FT_PIGEON;
}

/*
 * THE FRAME THE SOURCE PATH CROSSES IS COPIED AFTER THE DETACH.
 *
 * The COW above runs during the merge BUILD, before the src detach.  That is
 * the right order for a frame OFF the source path.  The detach never writes
 * into such a frame.  If the detach relocates the frame's PARENT, the
 * announced publish (@pending_pub_slot) is folded into that copy.
 *
 * On the source path it is the wrong order.  The detach DEL-recompacts the
 * src junction J and publishes the fresh J' INTO J'S PARENT'S SLOT.  When
 * that parent is the frame, a copy taken earlier carries the retired J, and
 * the detach's store lands in the body this COW retires.  This was MEASURED
 * on the root frame with the gate simply lifted:
 *   - a head's back edge naming the dead root (`head prev != owner`);
 *   - a stale skip word walking a reader into a NULL parent
 *     (ft_skip_reanchor);
 *   - an SW/MW kind conflict under rank stats.
 * Even a junction two levels down reaches the frame, through the detach's
 * -count walk.  That walk lands on the live word, and a copy taken before it
 * loses it.
 *
 * And EVERY source path crosses the ROOT.  The root frame is a dst head
 * hanging off the root, or off a run that does.  It was 6 of the 9 shapes
 * the corpus still refused here.  It is therefore never a pre-detach COW.
 * The same arm serves the deeper pass-through (src junction strictly BELOW
 * the frame), which used to be refused by design.
 *
 * So this arm only DECIDES here and records the frame (@after).  The caller
 * runs the COW after ft_detach_node, through ft_rekey_cow_stop's @ryw mode.
 * In that mode the copy loop and the re-parent sweep read this commit's own
 * records back out of the descriptor.
 *
 * The pending publish is ANNOUNCED for the detach.  If the detach relocates
 * the frame after all, its fold is the witness and the caller detects it.
 * The publish is NOT deferred to the glue.  A queued back edge aimed at a
 * node this commit retires is exactly the stale-top defect the same-path
 * class met.  The copy's own sweep re-homes the top.
 *
 * WHAT THE TOP MAY BE.  One of:
 *   - the splice's own live head (verbatim, the slot already holds it);
 *   - a GLUE-BUILT fresh node the merge wrapped (a dst ending inside a run
 *     over the head: `{ccb} + rekey_merge(dst "cc", src "aa")`).  The copy
 *     loop folds it into the slot in place of the child it replaces.  The
 *     sweep wires it with a plain store.
 * @merged_plain is that node's PLAIN flag.  The slot form is skip-encoded.
 * A skip word cannot be identified against the glue's built set, and its
 * parent cannot be set through the one-hop decode.  The node's parent is
 * wired NOW, while the frame is still the live parent.  The detach may fold
 * the publish into a copy of its own.  That copy's sweep then records the
 * re-home from the word as it stands, and a later plain store would put the
 * dead parent back.
 *
 * The frame is refused when it would be the moved top ITSELF (@cow_depth at
 * or past the src key: a destination inside its own source).  A run frame
 * keeps the verbatim requirement.  Its publish is a compressed one, two
 * stores and a dual, which is the compressed-publish-parent class.
 */
static
int ft_rekey_merge_cow_after_decide(struct cds_ft *ft,
		const struct ft_descent *d_dst, struct ft_glue *glue,
		struct cds_ft_inode_flag *merged_pub,
		struct cds_ft_inode_flag *merged_plain,
		struct cds_ft_inode_flag *cow_nf,
		struct cds_ft_inode_flag *cow_pnf,
		struct cds_ft_inode_flag **cow_slot,
		unsigned int cow_depth, unsigned int cow_pdepth,
		size_t src_len, struct ft_rekey_cow_after *after)
{
	/*
	 * The whole argument below is "the detach's edits are RECORDS in the
	 * descriptor".  An in-place tier edits nodes where they stand, so this
	 * copy would read nothing of them.  The tier is refused, as the
	 * same-path arm refuses it: loudly, not silently torn.
	 */
	if (ft_in_place_excl_ok(ft))
		return FT_REKEY_UNCOVERED;
	if ((size_t) cow_depth >= src_len)
		return FT_REKEY_UNCOVERED;
	if (!ft_rekey_cow_frame_ok(cow_nf, cow_pnf))
		return FT_REKEY_UNCOVERED;
	if (merged_pub != d_dst->nf) {
		if (ft_node_skip_compressed(merged_plain) ||
				ft_node_external(merged_plain) ||
				!ft_glue_is_fresh(ft, glue, merged_plain))
			return FT_REKEY_UNCOVERED;
		ft_set_parent(ft, merged_plain, d_dst->pnf, d_dst->nfp);
	} else if (!ft_node_external(merged_pub))
		return FT_REKEY_UNCOVERED;
	after->armed = true;
	after->cow_nf = cow_nf;
	after->cow_pnf = cow_pnf;
	after->cow_slot = cow_slot;
	after->cow_depth = cow_depth;
	after->cow_pdepth = cow_pdepth;
	return FT_REKEY_COW_AFTER_DETACH;
}

/*
 * W5: the destination attach point did not move, so MOVE ITS PARENT.
 *
 * COW @d_dst->pnf to a fresh address.  @d_dst->pnf is the immediate internal
 * parent of the node the merge attached to.  Republish the copy one level up,
 * at (@d_dst->ppnf, @d_dst->pnfp).  The interior is untouched: the copy is
 * verbatim and every child keeps its address.  Only one thing changes: the
 * identity of the node a descent passes THROUGH on its way to the merged
 * chain.  That identity is the whole product.  It is what the two-descent
 * witness of the rekey-coherent reader observes.
 *
 * ☞ WHY THE WITNESS IS OWED HERE AT ALL.  The caller's exit is taken exactly
 * when the union produced nothing fresh (a duplicate-chain splice onto a live
 * app-owned head).  The dst path then has no fresh address anywhere.  Take a
 * later insert on that same chain.  A reader looking up the ORIGINAL SRC key
 * could find it, at a location the move was supposed to have vacated.  A
 * torn descent would match a clean one, and the two-descent scheme would have
 * nothing to disagree about.
 *
 * ☠ THE PRIMITIVE IS ft_rekey_cow_stop.  THE TWO OBVIOUS ALTERNATIVES BOTH
 * FAIL.  This was measured, not reasoned:
 *  - ft_node_replace_ptr DOES NOT RELOCATE HERE.  It rebuilds a node only
 *    when the node's SHAPE must change.  This shape replaces one slot with
 *    the value it already holds (@merged_pub == the old child), which leaves
 *    nothing to do.  So it returns 0 with the LIVE node back and serves zero.
 *  - FT_RECOMPACT_RELOCATE publishes and re-parents children EAGERLY.  Its
 *    own contract calls that a "point of no return".  So it cannot build a
 *    copy for a commit that has not happened yet.
 * cow_stop is the one that fits:
 *  - it gives a fresh address;
 *  - it RECORDS the children's back edges and does not store them;
 *  - it retires @stop;
 *  - the caller does the publish.
 *
 * SCOPE.  Every term is a shape cow_stop or the publish cannot take TODAY:
 *  - a PLAIN INTERNAL parent (POPCOUNT / PIGEON).  A COMPRESSED one is a
 *    run.  Republishing a run means re-encoding the SKIP_X dual in ITS
 *    parent and handing the glue a skip-encoded top.  The back edge of such
 *    a top inverts to the node this commit retires (☞ ft_glue_is_fresh's
 *    header).
 *  - a NON-ROOT parent: &ft->root is a slot with no node word to lock, so the
 *    forward publish's SW park would be unguarded.  The compressed-dual arm
 *    above makes the same refusal for the same reason.
 *  - a NON-COMPRESSED grandparent, so the publish into it is ONE store and not
 *    also a SKIP_X dual re-encode.
 *
 * @marks is this frame's.  The merge arm reaches here having taken none (the
 * src-side cow_stop runs only on the !merge_dst arms).  So cow_stop starts at
 * index 0, and the caller's sweep covers what it takes.
 */
static
int ft_rekey_merge_cow_publish_parent(struct cds_ft *ft,
		struct ft_descent *d_dst, struct ft_flip_txn *txn,
		struct ft_op *optxn, struct ft_glue *glue,
		struct cds_ft_inode_flag *merged_pub,
		const uint8_t *src_ord, size_t src_len, const uint8_t *dst_ord,
		struct cds_ft_inode_flag *src_pnf,
		struct cds_ft_inode_flag *merged_plain,
		struct ft_rekey_cow_after *after, bool *frame_above_pnf,
		struct ft_held_anchor *marks, unsigned int *nr_marks,
		struct cds_ft_inode_flag **pp_prime_ret,
		struct cds_ft_inode **pp_old_ret)
{
	struct ft_lock_ctx gctx, cctx;
	struct ft_held_anchor gph;
	struct cds_ft_metadata *gp_meta, *pp_meta;
	struct cds_ft_inode_flag *pp_prime = NULL;
	/*
	 * THE FRAME THAT MOVES.  It has three parts:
	 *   - the node this arm COWs (@cow_nf, at @cow_depth);
	 *   - the parent it is republished into (@cow_pnf, at @cow_pdepth);
	 *   - the slot there (@cow_slot).
	 * See the selection below.
	 */
	struct cds_ft_inode_flag *cow_nf, *cow_pnf, **cow_slot;
	unsigned int cow_depth, cow_pdepth;
	bool via_run, on_src_path;
	int ret;

	*frame_above_pnf = false;
	if (!d_dst->pnf || !d_dst->nfp || !d_dst->pnfp)
		return FT_REKEY_UNCOVERED;
	/*
	 * ☞ THE NODE THAT MOVES IS THE NEAREST PLAIN ANCESTOR OF THE ATTACH
	 * SLOT, not necessarily the slot's owner.  The reader's two-descent
	 * witness (struct ft_visit_witness) hashes the PLAIN nodes a descent
	 * enters.  A compressed run is skipped over by its SKIP_X word and
	 * never visited.  (In no-skip mode a fresh run alone would still
	 * leave every visited node where it was.)  So relocating the run
	 * gives a reader nothing to notice.
	 *
	 * A leaf with no sibling at its last byte hangs off exactly such a
	 * run.  This is the ordinary shape.  It was 4 of the 10 shapes the
	 * corpus still refused here.  For it the COW goes ONE LEVEL UP: the
	 * run's parent, republished into ITS parent's slot.  The run is then
	 * an ordinary child of the copied node.  Its re-parent (parent word
	 * plus skip anchor) is the same kind-dispatched record every other
	 * child takes in ft_rekey_cow_stop's sweep.  Everything below is
	 * written against this frame, never against @pnf directly.
	 */
	via_run = ft_node_compressed(d_dst->pnf);
#ifdef FEATURE_FT_SKIP_COMPRESSED
	via_run = via_run || ft_node_skip_compressed(d_dst->pnf);
#endif
	if (via_run) {
		cow_nf = d_dst->ppnf;
		cow_depth = d_dst->ppdepth;
		cow_pnf = d_dst->pppnf;
		cow_slot = d_dst->ppnfp;
		cow_pdepth = d_dst->pppdepth;
	} else {
		cow_nf = d_dst->pnf;
		cow_depth = d_dst->pdepth;
		cow_pnf = d_dst->ppnf;
		cow_slot = d_dst->pnfp;
		cow_pdepth = d_dst->ppdepth;
	}
	if (!cow_nf || !cow_slot)
		return FT_REKEY_UNCOVERED;
	/*
	 * ☞ AND PAST A COMPRESSED GRANDPARENT, for the same reason.  A frame
	 * whose own parent is a run cannot be republished by one store.  It
	 * takes the run's child slot plus the SKIP_X dual re-encoded in the
	 * run's parent.  That is recorded at commit time, after the detach
	 * may have relocated that parent.  But nothing requires THIS frame:
	 * the witness is any plain node on the dst path getting a fresh
	 * address.  So climb once more, to the run's parent.  The descent
	 * knows that parent only when it is the ROOT (@pppdepth 0, its slot
	 * &ft->root, no parent to fence).  A deeper one stays refused,
	 * because its parent slot is outside the descent frame.
	 * Corpus seed 1424 is `{acc,cbc,ac,b}` dst acc src b.  There the dst
	 * head's parent is under a run off the root, and the src junction is
	 * the root itself.
	 */
	if (!via_run && cow_pnf && (ft_node_compressed(cow_pnf)
#ifdef FEATURE_FT_SKIP_COMPRESSED
			|| ft_node_skip_compressed(cow_pnf)
#endif
			) && d_dst->pppnf && d_dst->pppdepth == 0 &&
			d_dst->pppnfp == &ft->root) {
		cow_nf = d_dst->pppnf;
		cow_depth = 0;
		cow_pnf = NULL;
		cow_slot = d_dst->pppnfp;
		cow_pdepth = 0;
	}
	*frame_above_pnf = cow_nf != d_dst->pnf;
	/*
	 * ☑ A ROOT FRAME (@cow_pnf NULL, the slot &ft->root) IS NOT REFUSED
	 * HERE.  It is on every source path, so it takes the post-detach arm
	 * below (ft_rekey_merge_cow_after_decide).  It never takes the COW
	 * this function runs.  Three failures used to be measured against a
	 * naive relaxation of this gate:
	 *   - a head's back edge naming the dead root;
	 *   - a stale skip word walking a reader into a NULL parent;
	 *   - an SW/MW kind conflict under rank stats.
	 * Their cause was the pre-detach copy carrying the child the detach
	 * then retired.  The post-detach arm exists to avoid that.
	 */
	on_src_path = (size_t) cow_depth <= src_len &&
		!memcmp(src_ord, dst_ord, (size_t) cow_depth);
	/*
	 * ☠ THE COPY IS VERBATIM, so the attach slot must ALREADY hold what the
	 * merge wants published there.  That is the splice shape and only the
	 * splice shape: @merged_pub == the value the slot has.  The other way
	 * of reaching this exit is a merge that DID build a node, but whose
	 * freshness this gate cannot see.  The copy would then carry the
	 * SUPERSEDED child and lose the built one.  So that case keeps the
	 * refusal.
	 *
	 * The POST-DETACH copy is the exception.  Its copy loop folds the
	 * announced publish into the slot.  So a GLUE-BUILT top is admitted
	 * there (a dst ending inside the head's key, wrapped by
	 * ft_merge_wrap_prefix).  This holds for a plain frame only.  A run
	 * frame keeps the verbatim rule, because its publish is the
	 * compressed-parent kind.
	 */
	if (merged_pub != d_dst->nf && !(on_src_path && !via_run &&
			!ft_node_skip_compressed(merged_plain) &&
			!ft_node_external(merged_plain) &&
			ft_glue_is_fresh(ft, glue, merged_plain)))
		return FT_REKEY_UNCOVERED;
	/*
	 * ☠ THE RELOCATED NODE MUST NOT BE ON THE SOURCE PATH.  Every node the
	 * src key descends through belongs to the DETACH.  The detach
	 * DEL-recompacts the src junction and climbs from there.  So a node
	 * this frame has COW'd and RETIRED would be relocated a second time,
	 * by another owner, inside the same commit.
	 *
	 * ☠ AND THE CORPUS CANNOT SEE THIS.  Its 3000 shapes are 13/14
	 * `same_parent = 0` at this exit, and the one exception is refused
	 * for another reason.  So the whole class was untested until
	 * Mathieu's own dedicated case ran: {ab, ac} + rekey_merge(dst "ac",
	 * src "ab").  This is the LAST-BYTE move, which he named as the shape
	 * the witness exists for.  It is a clean refusal before this change.
	 * After it, it SPINS and allocates as it goes.  Generated corpora are
	 * a sample, not a cover.
	 *
	 * The test is the KEYS, not the nodes.  A descent is deterministic.
	 * So the src path reaches @pnf exactly when both hold:
	 *   - the src key is long enough to get to @pnf's depth;
	 *   - it agrees with the dst key on every byte down to that depth.
	 * That subsumes the same-parent case (@d_src.pnf == @d_dst.pnf).  It
	 * also catches the deeper src that only PASSES THROUGH @pnf on its
	 * way down.
	 *
	 * A src ENDING above @pnf is left in.  Its junction is an ancestor,
	 * and the detach climbs AWAY from @pnf.  A recompaction of the
	 * publish grandparent is already handled: ft_glue_set_publish
	 * announces the pending publish so that copy carries it
	 * (@pending_pub_slot).
	 */
	if (on_src_path) {
		/*
		 * ☑ THE SAME-PARENT SPLICE IS SERVED BY THE DETACH, NOT BY A
		 * COW.  This is the case where the src junction IS @pnf
		 * (last-byte move: {ab, ac}, dst "ac", src "ab").  The detach
		 * removes the src byte from @pnf.  With in-place mutation
		 * off, it also relocates @pnf, in one of two ways:
		 *   - a DEL recompaction to a fresh copy;
		 *   - the skip-mode COLLAPSE into a fresh run, for a node
		 *     left with one child and no keys.
		 * Either is the address-witness the two-descent scheme
		 * needs.  Both FOLD the caller's pending forward publish into
		 * the fresh node (@pending_pub_slot).  So the caller just
		 * publishes normally and verifies @pending_pub_folded after
		 * the detach.  A COW of @pnf on top of that is the measured
		 * HANG (two owners relocating one node in one commit).
		 *
		 * ☠ BOTH EASY READINGS OF THIS ARM WERE WRONG ONCE.
		 * Publishing normally was tried, and verify went red in all
		 * four modes.  The witness was not missing.  The fold's
		 * substitution assumed a FRESH top and plain-stored the live
		 * head's back edge.  The glue's queued entry then re-aimed
		 * that edge at the dead boundary.  See @pending_top_live at
		 * ft_chain_compress_fused and the matching skip in
		 * ft_glue_apply_deferred.
		 *
		 * The gate is the in-place predicate, by design.  When the
		 * in-place tier returns, a detach that edits @pnf in place
		 * produces no witness, and the fold never fires.  The
		 * post-detach check then turns this into a loud refusal, not
		 * a silent torn descent.
		 *
		 * The DEEPER src that only passes THROUGH the frame (its
		 * junction is below) is the POST-DETACH arm.  The detach
		 * publishes its fresh junction INTO the frame's slot.  So
		 * the frame is copied afterwards, with that store in it.
		 * ☞ ft_rekey_merge_cow_after_decide.  So is the root, which
		 * every source path crosses.
		 */
		if (src_pnf != cow_nf || ft_in_place_excl_ok(ft))
			return ft_rekey_merge_cow_after_decide(ft, d_dst, glue,
					merged_pub, merged_plain, cow_nf, cow_pnf,
					cow_slot, cow_depth, cow_pdepth, src_len,
					after);
		/*
		 * ☞ WHAT THIS ARM ADMITS, STATED RATHER THAN INHERITED.  It
		 * returns ahead of the COW arm's gates below.  Those gates
		 * are the COW's (a compressed grandparent, the type class
		 * the copy loop handles).  The detach is type-generic and
		 * leaves the grandparent alone.  Two things ARE load-bearing
		 * here and are pinned:
		 *
		 *  - THE TOP IS AN APP-OWNED EXTERNAL HEAD.  That is what a
		 *    splice hands back.  It is also what makes the live-top
		 *    fold downstream sound.  A head carries no state word,
		 *    so:
		 *      - no lock mark rides the glue's queued entry that
		 *        ft_glue_apply_deferred now drops (asserted there);
		 *      - no compressed absorption can free it
		 *        (ft_chain_compress_fused gates its absorb on
		 *        identity).
		 *    A live METADATA-bearing top is not a shape any armer
		 *    produces.  Refuse it rather than find out.
		 *  - @pnf IS A PLAIN INTERNAL.  A same-parent junction has
		 *    at least two children, so it cannot be a run or a run's
		 *    skip word.  This is vacuous today.  It is stated so
		 *    that the widening is visible.
		 */
		/*
		 * ...and VERBATIM: the slot must already hold @merged_pub.
		 * The post-detach arm's fold of a glue-built top is its own:
		 * its copy loop and sweep name the wrapper by what it
		 * encodes.  The detach's recompaction resolves a folded skip
		 * word through the head's back-pointer.  That back-pointer
		 * still names the run the wrapper replaces.  This is the
		 * parked child_ident item.  It is seed 2069 of the corpus,
		 * dst "accc" src "aa", which showed:
		 *   - a dangling skip slot;
		 *   - a reader walking off the root;
		 *   - a kind conflict under rank stats.
		 */
		if (merged_pub != d_dst->nf) {
			struct cds_ft_metadata *fm;

			/*
			 * ...OR A GLUE-BUILT WRAPPER over the spliced head.
			 * The dst ends inside a run over the head (seed
			 * 2069, dst "accc" src "aa").  The wrapper is folded
			 * into the DEL recompaction of the frame.  It is
			 * re-homed there by its PLAIN flag
			 * (@pending_pub_plain).
			 *
			 * The frame must be DEL-RECOMPACTED, not collapsed.
			 * A collapse takes the folded word as a run's child,
			 * and a skip word is not a node.  So the frame keeps
			 * another child or a key after the src byte leaves.
			 *
			 * Three shapes stay refused:
			 *   - a run frame (the deep fold);
			 *   - anything the glue did not build;
			 *   - a frame ABOVE the attach slot's parent (the
			 *     climb past a run off the root).
			 * For the last one, the fold cannot reach a slot two
			 * levels down.  The subsumption that then replaces
			 * the fold skips the forward store.  That is right
			 * only when the slot already holds the top.
			 */
			if (*frame_above_pnf || ft_node_skip_compressed(merged_plain) ||
					ft_node_external(merged_plain) ||
					!ft_glue_is_fresh(ft, glue, merged_plain))
				return FT_REKEY_UNCOVERED;
			fm = ft_flag_to_metadata(ft, cow_nf);
			if (!fm || (ft_meta_nr_child_load(fm) <= 2 &&
					!fm->external_nodes))
				return FT_REKEY_UNCOVERED;
		} else if (!ft_node_external(merged_pub))
			return FT_REKEY_UNCOVERED;
		if (ft_node_external(cow_nf) || ft_node_compressed(cow_nf))
			return FT_REKEY_UNCOVERED;
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_node_skip_compressed(cow_nf))
			return FT_REKEY_UNCOVERED;
#endif
		/*
		 * Through a run, the src detach COLLAPSES the shared
		 * ancestor into the run.  The pending publish is armed at
		 * the run's child slot.  It is then the DEEP fold
		 * (ft_chain_compress_deep_pending).  The live-top identity
		 * test of that fold is the one @pending_top_live already
		 * carries for it.
		 */
		return FT_REKEY_WITNESS_BY_DETACH;
	}
	if (!ft_rekey_cow_frame_ok(cow_nf, cow_pnf))
		return FT_REKEY_UNCOVERED;
	pp_meta = ft_flag_to_metadata(ft, cow_nf);
	if (!pp_meta)
		return -EAGAIN;
	/*
	 * ☑ A CO-LOCATED EXTERNAL CHAIN ON THE PARENT IS IN SCOPE.  This is
	 * the first caller that ever reached cow_stop's arm for one.  So
	 * cow_stop's two statements about itself could finally be
	 * adjudicated:
	 *   - the SCOPE note lists "a co-located external list" under the
	 *     TODO that "still needs recompact's extra arms";
	 *   - the inline comment at the arm says it is in scope, "one
	 *     forward pointer to copy plus one back edge to record, the
	 *     same shape as a child".
	 * MEASURED: the INLINE one is right.  The key set, the chain and
	 * cds_ft_verify are clean on {acbb,cb,a,c,abc,cc} +
	 * rekey_merge(dst "cc", src "acbb"), in all four rank/list modes.
	 *
	 * ☠ AND I BRIEFLY BELIEVED THE OPPOSITE ON A CONFOUNDED TEST.
	 * Refusing a co-located parent here made a `head prev != owner` red
	 * disappear.  That looked like a mechanism, and it was not.  The red
	 * came from the @pending_pub_folded misuse below.  The OLD parent
	 * stayed published, so verify walked it and found the chain naming
	 * the copy.  Refusing the co-located shape only removed the one SEED
	 * that reached the misuse from this side.  A refusal that deletes
	 * the one seed in a class tests the seed, not the mechanism.
	 */
	/*
	 * The copy and its publish edge both need room.  Take it BEFORE the
	 * COW.  A full glue is then a clean refusal, not a copy with nowhere
	 * to go.
	 */
	if (glue->nr_built + 1 > glue->cap_built ||
			glue->nr_deferred + 1 > glue->cap_deferred) {
		if (ft_glue_reserve(glue, glue->nr_built + 1,
				glue->nr_deferred + 1, glue->cap_free,
				glue->cap_splices))
			return -ENOMEM;
	}
	/*
	 * ☠ THE PUBLISH PARENT MOVES UP WITH THE PUBLISH.  The forward store
	 * now lands in a slot of the GRANDPARENT.  This txn is structural_sw,
	 * so that store PARKS, and a park does not arbitrate.  The op must
	 * hold the word behind the slot it parks.  That word is now @ppnf's
	 * and no longer @pnf's.  ft_glue_take_publish_parent is re-made here
	 * and not amended: it is the one place that owns the fence into the
	 * txn and links the terminal silencer.
	 *
	 * @pnf's own fence stays exactly where it is.  The caller took it
	 * when @pnf WAS the publish parent.  The txn owns it
	 * (@publish_parent_txn_owned was set for it), and its release is
	 * already recorded.  cow_stop's retire chains onto that same word
	 * behind the same fence.
	 *
	 * ☠ CHAIN THE GLUE, or a COARSE spacing livelocks the move.  This is
	 * the same reading as the compressed-dual acquire above.  @pnf's
	 * mark reaches no txn registry until ft_glue_txn_commit_edges, so
	 * the glue is the only witness that this op holds it.  Without the
	 * glue, a spacing that collapses @pnf and @ppnf onto ONE word would
	 * read the op's OWN fence as contention.  The op would then
	 * re-derive the identical plan forever.
	 */
	gp_meta = ft_flag_to_metadata(ft, cow_pnf);
	if (!gp_meta)
		return -EAGAIN;
	ft_lock_ctx_init(&gctx, d_dst, txn, optxn);
	gctx.held.glue = glue;
	if (ft_acquire_member(ft, &gctx, cow_pnf, gp_meta, cow_pdepth, &gph))
		return -EAGAIN;
	ft_glue_take_publish_parent(glue, gph.lock, gph.lock_snap, gph.shared);

	ft_lock_ctx_init(&cctx, d_dst, txn, optxn);
	cctx.held.glue = glue;
	ret = ft_rekey_cow_stop(ft, &cctx, txn, cow_nf, cow_depth,
			/*cut=*/ 0, /*ryw=*/ false, NULL, &pp_prime, marks, nr_marks);
	if (ret)
		return ret;
	ft_rekey_marks_to_txn(txn, marks, *nr_marks);
	/*
	 * ☠ TRACK THE COPY, AND IT IS NOT MERELY BOOKKEEPING.  An untracked
	 * child takes ft_glue_defer_edge's DEFERRED arm.  The copy's own
	 * parent word is then RECORDED at apply time instead of stored.  That
	 * word already carries a record of a different KIND from the forward
	 * publish, and urcu_txn_record_chain refuses this
	 * (`r->kind == kind`).  It was measured: an abort on
	 * rekey_merge(dst "ba", src "aab").  Every other glue publish top is
	 * glue-built and therefore takes the FRESH arm, a plain store into
	 * an unpublished body.  Tracking puts this copy on the same path.
	 *
	 * Tracking also hands the copy's RECLAIM to the glue, which is where
	 * it belongs.  ft_glue_abort frees the built set, and every bail
	 * below it routes through ft_glue_abort.  So the copy needs no free
	 * of its own.
	 */
	ft_glue_track(glue, pp_prime);
	/*
	 * The COPY is what the grandparent's slot must hold.  @merged_pub is
	 * already in the copy's own slot: cow_stop copied the parent
	 * verbatim, and the merge changed nothing at that slot.  cow_stop
	 * also recorded the external head's back edge onto the copy, along
	 * with every other child's.  So the attach point needs no publish of
	 * its own.
	 */
	ft_glue_set_publish(ft, glue, cow_pnf, cow_slot, pp_prime);
	/*
	 * ☠ AND THE COUNT DELTA MUST BE BAKED INTO THE COPY, because the walk no
	 * longer reaches it.  @glue->count_delta is charged from the publish
	 * parent UP, and the publish parent is now the GRANDPARENT -- so the copy,
	 * which sits between it and the merged chain, is the one node on the path
	 * the walk skips.  @pending_pub_node is the hook the glue commit already
	 * has for exactly this ("bake into the named survivor, walk from the
	 * shared stable parent"): it stores the delta directly into the named node
	 * and then starts the ancestor walk at THAT node's own parent.
	 */
	/*
	 * ☠ AND THE COUNT DELTA MUST BE BAKED INTO THE COPY, because the walk no
	 * longer reaches it.  @glue->count_delta is charged from the PUBLISH
	 * PARENT up, and the publish parent is now the GRANDPARENT.  The copy
	 * sits between the grandparent and the merged chain.  So it is the
	 * one node on the path the walk skips.  The caller bakes the delta
	 * where @count_delta is computed.  That is a build-invisible plain
	 * store into a node this commit has not published.
	 *
	 * ☠ NOT @pending_pub_folded.  It looks like the obvious hook and is
	 * the WRONG one.  Its FIRST consumer makes ft_glue_txn_commit_edges
	 * SKIP the forward publish outright ("a recompaction already folded
	 * this publish into the node that replaced @publish_parent").
	 * @pending_pub_slot is the slot ft_glue_set_publish just armed, so
	 * the guard always matches.  MEASURED:
	 *   - the copy is built;
	 *   - the old node stays published;
	 *   - the keys come out right through it;
	 *   - cds_ft_verify reports a DANGLING SKIP SLOT, because the copy's
	 *     re-parents landed on children the old node still holds.
	 */
	*pp_prime_ret = pp_prime;
	/*
	 * The ORIGINAL, for the caller's post-commit free (cow_stop's contract).
	 * ft_node_ptr is right only because the frame above admits a plain
	 * POPCOUNT/PIGEON node and nothing else.
	 */
	*pp_old_ret = ft_node_ptr(cow_nf);
	return 0;
}

/*
 * THE POST-DETACH COW (see ft_rekey_merge_cow_after_decide): the frame's
 * fence, the copy through the descriptor, the publish one level up.  It runs
 * after ft_detach_node succeeded and before the glue commit.  Every bail
 * returns with the copy glue-tracked (ft_glue_abort frees it) and the marks
 * published in @marks for the caller's sweep.
 *
 * The ROOT frame has no grandparent to fence.  Its forward slot is &ft->root.
 * _ft_publish_to_parent records that slot ROOT-OWNED, and the engine installs
 * it MW, arbitrated by the root-slot CAS.  Every other writer of that slot
 * takes the same route.  The glue's publish parent is then NULL.  The glue's
 * commit already reads that as "a root splice" (no §4.B guard, no count walk
 * above).  So the copy's own bake below is the whole of the count.
 *
 * The announcement is CONSUMED here.  The copy already holds the top when it
 * is created, so @pending_pub_folded must not be left standing.  Otherwise
 * the glue commit would skip the forward publish of the COPY, which it reads
 * off the same slot.  ft_glue_set_publish re-announces the copy's own slot.
 */
static
int ft_rekey_merge_cow_after_detach(struct cds_ft *ft,
		struct ft_descent *d_dst, struct ft_flip_txn *txn,
		struct ft_op *optxn, struct ft_glue *glue,
		const struct ft_rekey_cow_after *after,
		const struct ft_detach_recompact_out *detach_rc,
		struct ft_held_anchor *marks, unsigned int *nr_marks,
		struct cds_ft_inode_flag **pp_prime_ret,
		struct cds_ft_inode **pp_old_ret)
{
	struct ft_lock_ctx gctx, cctx;
	struct ft_held_anchor gph;
	struct cds_ft_inode_flag *pp_prime = NULL;
	/*
	 * What the detach built and left unpublished under the frame: its DEL
	 * copy of the junction and the run its collapse merged.  Anything else
	 * the copy meets takes the recorded path.  That path is also correct
	 * for a fresh node, only heavier.
	 */
	struct ft_rekey_cow_fresh fresh = { .n = 0 };
	int ret;

	if (detach_rc->new_flag)
		fresh.m[fresh.n++] = cds_ft_item_to_metadata(
			ft_node_ptr(detach_rc->new_flag));
	if (detach_rc->collapse.new_cn)
		fresh.m[fresh.n++] = cds_ft_item_to_metadata(
			(struct cds_ft_inode *) detach_rc->collapse.new_cn);

	if (glue->nr_built + 1 > glue->cap_built ||
			glue->nr_deferred + 1 > glue->cap_deferred) {
		if (ft_glue_reserve(glue, glue->nr_built + 1,
				glue->nr_deferred + 1, glue->cap_free,
				glue->cap_splices))
			return -ENOMEM;
	}
	/*
	 * The ROOT frame takes no grandparent fence.  The glue's
	 * publish-parent fields then keep what the merge lane's take of
	 * @d_dst.pnf left (@publish_parent_txn_owned / _shared).  For a frame
	 * above a run they describe the RUN's fence.  They are inert: the
	 * commit's NULL-parent arm neither acquires nor guards.  They must
	 * NOT be reset, because they also say who CLEARS that fence.
	 */
	if (after->cow_pnf) {
		struct cds_ft_metadata *gp_meta =
			ft_flag_to_metadata(ft, after->cow_pnf);

		if (!gp_meta)
			return -EAGAIN;
		ft_lock_ctx_init(&gctx, d_dst, txn, optxn);
		gctx.held.glue = glue;
		if (ft_acquire_member(ft, &gctx, after->cow_pnf, gp_meta,
				after->cow_pdepth, &gph))
			return -EAGAIN;
		ft_glue_take_publish_parent(glue, gph.lock, gph.lock_snap,
			gph.shared);
	}
	ft_lock_ctx_init(&cctx, d_dst, txn, optxn);
	cctx.held.glue = glue;
	ret = ft_rekey_cow_stop(ft, &cctx, txn, after->cow_nf,
			after->cow_depth, /*cut=*/ 0, /*ryw=*/ true, &fresh,
			&pp_prime, marks, nr_marks);
	if (ret)
		return ret;
	ft_rekey_marks_to_txn(txn, marks, *nr_marks);
	ft_glue_track(glue, pp_prime);
	txn->pending_pub_folded = false;
	txn->pending_pub_node = NULL;
	ft_glue_set_publish(ft, glue, after->cow_pnf, after->cow_slot,
		pp_prime);
	/* The copy sits below the walk's base (or IS the root): bake here. */
	if (glue->count_delta) {
		struct cds_ft_metadata *ppm =
			cds_ft_item_to_metadata(ft_node_ptr(pp_prime));

		ft_nr_keys_store(ft, ppm, ft_nr_keys_get(ppm) +
			(unsigned long) glue->count_delta, CMM_RELAXED);
	}
	*pp_prime_ret = pp_prime;
	*pp_old_ret = ft_node_ptr(after->cow_nf);
	return 0;
}

#endif /* FEATURE_FT_MERGE */

static
int ft_rekey_graft_simple_attempt(struct cds_ft *ft,
		const uint8_t *src_key, size_t src_len,
		const uint8_t *dst_key, size_t dst_len,
		bool require_empty, bool len_fits, struct ft_op *optxn)
{
	uint8_t src_ord[FT_MAX_KEY_LEN], dst_ord[FT_MAX_KEY_LEN];
	struct cds_ft_inode_flag *s_top, *s_top_prime = NULL, *attached_nf = NULL;
	struct cds_ft_node *run_rfirst = NULL, *run_rlast = NULL;
	struct ft_ord_cell *run_dpred = NULL, *run_dsucc = NULL;
	struct cds_ft_metadata *s_top_meta, *bp_meta;
	struct ft_detach_recompact_out detach_rc = { 0 };
	/*
	 * The merge arm relied on the src detach to relocate the parent of
	 * the dst attach point. This is the same-parent splice,
	 * FT_REKEY_WITNESS_BY_DETACH. The detach must then have folded the
	 * pending publish. Otherwise no witness exists.
	 */
	bool witness_by_detach = false;
	/*
	 * ...and the frame that arm chose was the run's parent: the head
	 * hangs off a run. The pending publish then sits in the run. That
	 * is one level below the node the detach relocates. A DEL
	 * recompaction of that node cannot fold it. See the subsumption at
	 * detach_bail.
	 */
	bool witness_above = false;
	struct ft_flip_txn *txn;
	struct ft_glue glue;
	struct ft_graft_store_state gst_st = { 0 };	/* GLUE never runs prepare */
	struct ft_descent d_src, d_dst;
	/*
	 * The src descent is the anchor source for every acquire this attempt
	 * makes. @d_src is valid only once the walk below has run. So the
	 * context is bound, or bound again, where it is used.
	 */
	struct ft_lock_ctx lctx_src;
	struct cds_ft_alloc_reserve reserve;
	struct ft_remove_pub pub = { .armed = false };
	/*
	 * The two nodes that the graft's own step locks. They depend on the
	 * dst shape it took:
	 *  - NOSPLIT: {dst parent, its parent};
	 *  - GLUE: {split compressed node, publish parent}.
	 * @graft_c is the one it retires. @graft_p is the one it releases.
	 */
	struct cds_ft_inode_flag *graft_c = NULL, *graft_p = NULL, *cn_flag = NULL;
	struct cds_ft_metadata *pp_meta = NULL;	/* GLUE publish-parent fence WE own */
	uintptr_t pp_snap = 0;
	/* ...unless it DEDUPED onto a word an earlier step of this op took. */
	bool pp_shared = false;
	/*
	 * INCREMENT 3: the dst point is occupied. Step 2 then unions into it
	 * with ft_merge_build. It does not graft a COW'd S_top' into a spare
	 * slot. The merged cluster reuses @glue as its dst side, so every
	 * abort / free_old / fini path below applies unchanged. @src_glue is
	 * the extra src side. It carries the retire of S_top itself: the
	 * merge does what cow_stop would.
	 */
	struct ft_glue src_glue;
#ifdef FEATURE_FT_MERGE
	struct ft_merge_ctx mctx;
	struct ft_merge_counts mcnt = { 0, 0, 0, 0, 0 };
#endif
	struct cds_ft_inode_flag *merged_nf = NULL;
	struct cds_ft_inode_flag *merged_pub = NULL;	/* @merged_nf, wrapped */
	/* ...and its PLAIN form, before the slot's skip encoding (W5's post arm). */
	struct cds_ft_inode_flag *merged_plain = NULL;
	/*
	 * W5: the COW of the dst attach point's PARENT, when the merge itself
	 * produces nothing fresh there.  It is glue-tracked, so the
	 * ft_glue_abort of every bail frees it. It is kept here only to
	 * carry the count bake.
	 */
	struct cds_ft_inode_flag *dst_pp_prime = NULL;
	/*
	 * The old body of the dst publish parent that @dst_pp_prime replaces.
	 * cow_stop's record retires it at the flip. This commit unlinks it.
	 * Per cow_stop's own contract, the caller frees it afterwards.
	 */
	struct cds_ft_inode *dst_pp_old = NULL;
#ifdef FEATURE_FT_MERGE
	/*
	 * The W5 frame the src detach must run before (on the source path).
	 *
	 * ☠ It is guarded because its type is. struct ft_rekey_cow_after is
	 * defined inside this file's merge region. W5 is a merge-only frame:
	 * the COW of the dst attach point's parent when the merge produces
	 * nothing fresh there. An unconditional declaration made
	 * -DNO_FEATURE_FT_MERGE fail to compile with "has initializer but
	 * incomplete type". That is how that whole configuration went dark
	 * on this branch.
	 */
	struct ft_rekey_cow_after cow_after = { .armed = false };
#endif
	struct ft_held_anchor ks_held = { 0 };	/* KEY_SHORTER run's overlap fence */
	bool ks_fenced = false;
	struct cds_ft_inode_flag *probe_D = NULL;	/* occupied dst merge point */
	/*
	 * KEY_SHORTER destination: the number of bytes of the compressed run
	 * @probe_D that the dst key consumes. The merge point then sits
	 * @dst_off_d bytes inside the run. 0 is the exact dst: the merge
	 * point is a node. ft_merge_build takes it as @off_d and splits the
	 * run there. ft_merge_wrap_prefix re-creates the bytes above.
	 */
	unsigned int probe_off_d = 0, dst_off_d = 0;
	/*
	 * The cells of the src heads that the interleave dropped. This
	 * happens for an occupied dst whose merge collides on a suffix. The
	 * edges that route every surviving link around them ride this op's
	 * commit. So readers stop reaching them only when it lands. They are
	 * freed after the commit and left alone on every abort. The array
	 * belongs to this frame either way. It is released at @sweep.
	 */
	struct ft_ord_cell **collided_cells = NULL;
	unsigned long nr_collided = 0;
	unsigned long merged_keys = 0;
	bool merge_dst = false, src_glue_live = false;
	/*
	 * The dst position abuts the moved run, so the move leaves the run's ordered
	 * position alone -- see the splice-position derivation.
	 */
	bool run_keeps_pos = false;
	bool s_top_compressed = false;	/* the moved top is a compressed run */
	/*
	 * THE BARE-HEAD SRC: the whole content of @src_key is one key that
	 * ends exactly there. So S_top is the app-owned external head itself:
	 * no library node, no metadata, no children.  @s_top_meta is NULL for
	 * it. Every consumer below dispatches on this flag.  Only the merge
	 * (occupied-dst) arm consumes the shape today. See the admission gate
	 * for why that is sound and what the empty-dst graft arm still owes.
	 */
	bool s_top_external = false;
	/*
	 * The src key ends inside a run, @src_cut bytes into it.  Then S_top
	 * is not a node: it is the tail of the run. ft_rekey_cow_stop
	 * manufactures it by copying @key_bytes from this offset instead of
	 * from zero.  The junction is the run's own parent slot. The whole
	 * run is retired: a run has one child, so cutting it moves
	 * everything it held.  Zero for the ordinary shape, where S_top is a
	 * node the descent landed on.
	 */
	unsigned int src_cut = 0;
#ifdef FEATURE_FT_MERGE
	/* Occupied dst: thread the cells individually, do not splice a run. */
	bool run_interleaves = false;
#endif
	/*
	 * +2, not +1. cow_stop can fill S_top plus all FT_ENTRY_PER_NODE of
	 * its children. The GLUE shape adds the one displaced child of the
	 * split cluster.
	 */
	struct ft_held_anchor marks[FT_ENTRY_PER_NODE + 2];
	const uint8_t *ik;
	unsigned int nr_marks = 0, adepth = 0, i, ti;
	bool marks_consumed = false;
	bool src_parent_held;
	/*
	 * Two separate questions:
	 *  - Does the detach's slot drop ride the graft's own recompaction
	 *    of BP? This is the @pending_del_slot arming below.
	 *  - May the graft therefore publish into &ft->root?
	 * There are two names because the second adds a build condition that
	 * the first must not inherit.
	 */
	bool del_folds_into_graft, root_pub_ok;
	/* the detach's climb walks up INTO the node the graft republishes */
	bool climb_reaches_graft = false;
	struct cds_ft_inode_flag *climb_rest = NULL;
	/* the climb rests on a child of @graft_c: the republish lands in its OLD body */
	bool climb_rest_under_graft = false;
	/*
	 * The climb crossed a compressed run.  This does not change whether
	 * the climb arrives at @graft_c: the staleness terms below use the
	 * honest count either way. It also no longer disarms anything, since
	 * the fold learned to speak the encoding. The orphan freeze retires a
	 * run like any other cleared ancestor. Its depth arithmetic steps by
	 * ft_node_span. The flag is kept as the climb's own report of what it
	 * walked.
	 * ☞ ft_rekey_climb_reaches_graft.
	 */
	bool climb_compressed = false;
	int climb_steps;
	/*
	 * ...and the byte depth that climb rose. @climb_steps equals it only
	 * while every node it stepped into spans one byte.  ☞ the climb.
	 */
	unsigned int climb_bytes = 0;
	/*
	 * ...and the same climb, armed instead of refused. The drop rides the
	 * graft's copy of @graft_c one or more levels above BP. The chain it
	 * empties is frozen into this commit. A detach does not walk it: the
	 * detach would read the superseded bodies.  @climb_top is the child
	 * of @graft_c whose slot goes. @fold_top_slot is that slot. It is
	 * taken with its parent as one coherent pair.
	 */
	/*
	 * THE CLIMB THE GLUE'S OLD-DIRECTION ARMING USED. It applies when the
	 * drop empties BP and the split holds the node the walk rests on. The
	 * chain the drop orphans is frozen into this same commit, exactly as
	 * the chain of the NOSPLIT fold is.  Zero means the arming named BP
	 * itself and there is no chain.
	 */
	int climb_split_steps = 0;
	struct cds_ft_inode_flag *climb_split_top = NULL;
	struct cds_ft_inode_flag *climb_split_rest = NULL;
	bool bp_folds_into_graft_c = false;
	enum ft_rekey_fold_mode fold_mode = FT_REKEY_FOLD_NONE;
	struct cds_ft_inode_flag *climb_top = NULL;
	struct cds_ft_inode_flag **fold_top_slot = NULL;
	/*
	 * ...and the raw word that slot held when the arming read it.  It is
	 * identical to @climb_top / @climb_rest on every plain link. Where
	 * the link is compressed, it is a run's SKIP_X spelling.  The copy
	 * loop compares @pending_del_expected against
	 * rcu_dereference(*slot), which is a raw word. So the raw form has
	 * to travel. The freeze walk and the shape reasoning use the
	 * resolved flag.
	 */
	struct cds_ft_inode_flag *fold_top_expected = NULL;
	/* PROMOTE only: the external chain head that takes the slot. */
	struct cds_ft_inode_flag *fold_promote_val = NULL;
	unsigned int fold_bp_depth = 0;
	unsigned int fold_rest_depth = 0;	/* REPLACE: the resting node's depth */
	/*
	 * REPLACE: the child the RESTING node loses, and the slot holding it.
	 *
	 * ☠ IT IS NOT ALWAYS THE SOURCE SUBTREE.  Only a climb that never
	 * moved loses S_top. Once the climb takes a step, the resting node
	 * loses the topmost cleared node instead. The chain below that node
	 * dies with it. Deriving this from @d_src alone is right only for the
	 * height-0 case. It wedges the writer for every other case.
	 * Measured on one two-step seed: 745,069 attempts all bailed
	 * -EAGAIN before the graft. ft_node_find_child looked for a
	 * grandchild.
	 */
	struct cds_ft_inode_flag *fold_drop_child = NULL;
	/* COLLAPSE: the child that SURVIVES the drop, captured RAW, and its ordinal. */
	struct cds_ft_inode_flag *fold_surv = NULL;
	uint8_t fold_surv_byte = 0;
	struct cds_ft_inode_flag **fold_drop_slot = NULL;
	struct cds_ft_inode_flag *fold_drop_expected = NULL;
	struct cds_ft_inode *fold_rest_old = NULL;	/* its retired body */
	/*
	 * REPLACE's FRESH copy, and the flag that says the commit took it.
	 *
	 * ☠ IT IS BUILT BEFORE THE RESERVE IS ACTIVATED. So nothing else owns
	 * it. The reserve drain, @gst_st.dest and @detach_rc do not.  Every
	 * path that does not commit must hand it back. There are five such
	 * paths. So the free lives at @sweep, which all five reach. The one
	 * flag the commit sets guards it.  Measured before that guard
	 * existed: 50 forced commit-aborts leaked exactly 50 nodes. The leak
	 * was silent: the op recovered and the trie verified clean.
	 */
	struct cds_ft_inode_flag *fold_rest_new = NULL;
	bool fold_rest_published = false;
	/*
	 * The plan-locks of the chain. They stay out of the txn registry for
	 * the reason ft_detach_node states for its own: the chain reaches
	 * FT_MAX_DEPTH, which is past FT_FLIP_TXN_MAX_LOCKS. They are the
	 * `extra[]` arm of the held set. They are swept at @sweep, like the
	 * detach sweeps its own.
	 */
	struct ft_held_anchor fold_held[FT_MAX_DEPTH];
	int nr_fold_held = 0;
	struct ft_lock_ctx fold_octx;		/* the op's OTHER marks, chained */
	enum ft_graft_prep prep;
	enum cds_ft_status gst;
	enum urcu_txn_status gcst = URCU_TXN_STATUS_OK, st;
	unsigned long cnt;
	int ret;

	/*
	 * ARGUMENT refusals: the caller got it wrong. No state of the trie
	 * would make this call legal.  The answer is a terminal -EINVAL,
	 * surfaced as CDS_FT_STATUS_INVALID_ARGUMENT_ERROR.
	 */
	if (src_len == 0 || dst_len == 0 ||
			src_len > FT_MAX_KEY_LEN || dst_len > FT_MAX_KEY_LEN)
		return -EINVAL;
	/*
	 * CAPABILITY refusal: nothing is wrong with the arguments or the
	 * trie. This writer does not cover the mode.  The code is -ENOTSUP,
	 * distinct from -EINVAL, because the two have opposite futures. An
	 * argument error is permanent for every caller. An uncovered mode is
	 * a gap in this writer, and widening its cut closes the gap.
	 */
	/*
	 * COARSE IS IN THIS WRITER'S CUT.  It was excluded for two reasons.
	 * Both have since gone. So the gate that stood here is removed, not
	 * re-argued.
	 *
	 * TERMINATION was the stated reason: "with this gate lifted the retry
	 * loop NEVER TERMINATES on a coarse trie".  It does not reproduce.
	 * It was measured twice on inv_rekey_coarse_mixed_writers, with both
	 * halves of the gate lifted. First 400 runs (2026-08-16). Then 16,711
	 * moves with 0 retries. The refusal histogram inverts the story.
	 * Coarse refuses ~0.47% per move. Its dominant refusal is the final
	 * MCAS commit aborting, which is exactly what the retry lane
	 * absorbs.  FINE, the mode that ships, refuses ~390% per move and
	 * terminates fine.
	 *
	 * EXCLUSION was the real reason. It was fixed, not refuted. This
	 * writer parks its structural edges SW, and an SW park cannot fail.
	 * On a coarse trie it used to take no writer scope at all, so those
	 * parks arbitrated against nobody.  ft_rekey_graft_simple_locked now
	 * takes CDS_FT_SCOPED_WRITER around its whole retry loop. That is the
	 * protocol every other coarse writer speaks. It is inert under
	 * lock_fine, where the per-node DLM locks are.
	 *
	 * ★ THAT IS CHECKED, not asserted. -DURCU_TXN_DEBUG_SETTLE reports
	 * every word a peer wrote between our park and our settle.  The
	 * FT-wide scope was deliberately removed (-DFT_RED_REKEY_NOLOCK,
	 * 68,655 scopes skipped). The check then found 0 foreign writes in
	 * 1.37M settle records. The detector itself is proven able to see
	 * that class (tests/unit/test_rcu_txn_settle_premise.c).
	 */
	ft_key_to_ordinals(src_ord, src_key, src_len, &ft->group->key_map);
	ft_key_to_ordinals(dst_ord, dst_key, dst_len, &ft->group->key_map);
	/*
	 * DISJOINT keys: neither may be a prefix of the other. Equal keys
	 * are the degenerate case.  A prefix relationship puts one key inside
	 * the other's subtree, so the move is circular.
	 *
	 * The same rule keeps the ordered-list splice sound, now that the
	 * shape gate no longer forces src_len == dst_len by construction.
	 * The run this move re-splices is exactly the keys under @src_key.
	 * That is an ordinally contiguous range. A splice neighbour strictly
	 * interior to that run would have to be bracketed by two run keys.
	 * That puts @dst_key inside the run's own range, which makes
	 * @src_key a prefix of it.  Rejecting that here leaves only the two
	 * endpoint-adjacency shapes for the guard below to catch. So "no
	 * cell of the moved run is a splice neighbour" holds by construction.
	 * (A neighbour interior to a peer's concurrently-moving run is a
	 * different question, still open. See the splice validation below.)
	 */
	if (memcmp(src_ord, dst_ord, src_len < dst_len ? src_len : dst_len) == 0)
		return -EINVAL;

	/*
	 * UNEQUAL LENGTHS ARE IN SCOPE. The destination probe below is what
	 * let them in. While it stopped at a compressed node, every
	 * unequal-length shape was refused anyway, at a later gate. A longer
	 * @dst_key puts the destination behind exactly the path-compressed
	 * run the probe declined to decode.  This was measured then. It is
	 * why relaxing the length rule alone was reverted and not landed.
	 *
	 * Nothing stored needs rewriting. A node carries neither a depth nor
	 * a key length: cds_ft_metadata is parent_word / external_nodes /
	 * parent_slot_offset, and cds_ft_node is prev / next. So the new key
	 * lengths of a moved subtree depend only on where its top hangs.
	 *
	 * Two things do follow from the length change. Both are handled the
	 * way ft_rekey_at_inner already handles them for the staged path:
	 *
	 *  - OVERFLOW.  A moved key K becomes @dst_key || (K minus the
	 *    @src_key prefix). A longer destination can push it past the
	 *    group's max_key_len. It would then overflow the fixed-size key
	 *    buffers downstream (the iterator's, the up-walk's).  Bound
	 *    len(K) by the trie's own max_used_key_len and refuse the shape.
	 *    Do not answer the argument here: ft_rekey_at_inner owns that
	 *    contract and returns OVERFLOW_ERROR for it. FT_REKEY_UNCOVERED
	 *    is the one code that falls through to it (see the dispatcher).
	 *    One place defines the answer.
	 *
	 *  - max_used_key_len itself, raised by the caller on a committed move.
	 *
	 * ☠ THE FIXED-LENGTH FLAVOUR IS EXCLUDED HERE, EXPLICITLY.  It does
	 * reach this writer: test_rekey_fixed_len_atomic_or_refused drives
	 * it. The equal-length rule used to keep it safe. With the lengths
	 * free, moving a 2-byte prefix onto a 1-byte one shortens every key
	 * it carries. A group whose keys all have one length cannot express
	 * that. The refusal is a shape code, not an argument answer, for the
	 * same reason as the overflow one. ft_rekey_at_inner owns the "a
	 * rekey needs a variable-length group" contract and returns
	 * INVALID_ARGUMENT for it.
	 *
	 * ☠ THE JUNCTION GATE BELOW IS WHAT KEEPS THIS HONEST. With the
	 * lengths free, the src and dst junctions no longer sit on the same
	 * level. So the cross-depth aliases it rejects are now reachable,
	 * not vacuous. They were written as rejections, not asserts, for
	 * exactly this day.
	 */
	if (src_len != dst_len) {
		if (ft->group->key_len != CDS_FT_LEN_VARIABLE)
			return FT_REKEY_UNCOVERED;
		if (!len_fits &&
		    ft_rekey_len_change_overflows(ft, src_len, dst_len))
			return FT_REKEY_UNCOVERED;
	}

	/*
	 * Descend src to S_top through plain internal nodes. Capture nfp,
	 * pnfp and depth. ft_detach_node needs them to bootstrap the
	 * src-slot clear + BP nr_child--.
	 */
	ft_descent_init(&d_src, ft);
	ik = src_ord;
	while (d_src.depth < src_len) {
		if (!d_src.nf || ft_node_external(d_src.nf))
			return FT_REKEY_UNCOVERED;
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_node_skip_compressed(d_src.nf))
			return FT_REKEY_UNCOVERED;
#endif
		/*
		 * A compressed run on the way down to S_top is crossed whole,
		 * the way the dst probe crosses one. step() advances a single
		 * byte. Walking a run with it counts the bytes but never
		 * resolves into cn->child. It lands depth-correct on NULL.
		 *
		 * A run that overshoots @src_len is not out of scope either:
		 * the src key simply ends inside it.  There is no node at that
		 * depth to be S_top, and no slot in the run to clear. So the
		 * move cuts the run instead. The moved top is its tail
		 * (@src_cut bytes in). The junction is the run's own parent
		 * slot.  Nothing is stranded by that. A run holds exactly one
		 * child, so everything below the cut is everything the run
		 * held. The run itself is retired whole.  The bytes before the
		 * cut must still match the key. The bytes after it are what
		 * the tail carries to the destination.
		 */
		if (ft_node_compressed(d_src.nf)) {
			struct cds_ft_compressed_node *cn =
				ft_compressed_node_ptr(d_src.nf);
			unsigned int remaining =
				(unsigned int) (src_len - d_src.depth);

			if ((unsigned int) cn->len > remaining) {
				if (ft_match_compressed_key(ik, cn, remaining)
						!= remaining)
					return FT_REKEY_UNCOVERED;
				src_cut = remaining;
				break;		/* S_top is this run's tail */
			}
			if (ft_match_compressed_key(ik, cn,
					(unsigned int) cn->len)
					!= (unsigned int) cn->len)
				return FT_REKEY_UNCOVERED;
			ft_descent_traverse_compressed(ft, &d_src, cn, &ik);
			continue;
		}
		ft_descent_step(ft, &d_src, *(ik++));
	}
	s_top = d_src.nf;
	/*
	 * @src_cut lands the descent on the run, not past it. So the depth
	 * it stopped at is the run's own, and the key ends @src_cut bytes
	 * further in.  Both readings say the same thing: the src prefix is
	 * exhausted exactly here.
	 */
	if (!s_top || d_src.depth + src_cut != src_len ||
			ft_node_flip_proxy(s_top))
		return FT_REKEY_UNCOVERED;
	/*
	 * A COMPRESSED S_top is in scope. ft_rekey_cow_stop copies the run
	 * and its one child. The one re-parent it records also re-points the
	 * skip back-channel. So nothing about the encoding needs recomputing.
	 *
	 * The SKIP-ENCODED slot form is refused. That refusal guards a shape
	 * a rekey cannot reach. It does not narrow this cut.  The encoding
	 * is a group property. cds_ft_group_attr_set_lookup_optimization
	 * clears CDS_FT_FLAG_SKIP_COMPRESSED for EAGER. A rekey requires
	 * EAGER, because it re-parents a leaf and cannot rewrite an app-owned
	 * stored key. So every compressed run a rekey meets was published
	 * plain.  Keep the check. It is cheap. It stops the accessor mismatch
	 * below from becoming a wild read if that coupling ever changes.
	 */
	/*
	 * AN EXTERNAL S_top IS ADMITTED. This is the bare head: the whole
	 * content of @src_key is one key that ends exactly there. The stated
	 * reason of the old refusal is gone.  It argued that the two-descent
	 * witness of the rekey-coherent reader (ft_lookup_two_descents) was
	 * built on ft_rekey_cow_stop giving the moved top a fresh address. An
	 * app-owned head cannot get one. That premise was adjudicated false
	 * (2026-08-31). Dst-side freshness carries the witness: every move
	 * publishes a fresh library node at the destination attach point:
	 *  - on the graft arm, the recompacted attach parent;
	 *  - on the merge arm, ft_merge_build's union node M.
	 * The _ft_node_set_nth -ERANGE and replace_ptr -EFBIG arms force it
	 * for every in-place mutation (ft-mutation-node.h). So no
	 * torn-but-unperturbed descent is constructible for a head. A head
	 * has no interior, hence no second commit-sensitive read below the
	 * junction.  ABA on the head itself is closed by the public reuse
	 * contract (fractal-trie.h: re-initialize after a grace period
	 * before re-insert).
	 *
	 * KIND DISPATCH ORDER is load-bearing here. A skip pointer onto an
	 * external leaf has low tag bits 0, so ft_node_external() matches it
	 * on the raw value (the probe's note below).  The descent resolves
	 * skip words, so a skip-encoded S_top should not reach this line.
	 * Still, the internal arm keeps its cheap belt-and-braces refusal,
	 * and the external arm must not weaken it. Test SKIP first, and
	 * refuse it.
	 *
	 * ☠ A SYNTHESISED CARRIER NODE WAS CONSIDERED AND REJECTED. Wrapping
	 * the head in a manufactured one-child internal violates the arity
	 * invariant: a one-child no-external internal is path-compressed
	 * away. It also buys a freshness that nothing consumes.
	 */
	if (ft_node_compressed(s_top)) {
		s_top_compressed = true;
	} else if (
#ifdef FEATURE_FT_SKIP_COMPRESSED
			!ft_node_skip_compressed(s_top) &&
#endif
			ft_node_external(s_top)) {
		s_top_external = true;
	} else {
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_node_skip_compressed(s_top))
			return FT_REKEY_UNCOVERED;
#endif
		ti = ft_node_type(s_top);
		if (ft_types[ti].type_class != FT_POPCOUNT &&
				ft_types[ti].type_class != FT_PIGEON)
			return FT_REKEY_UNCOVERED;
	}
	/*
	 * A key ending exactly at @src_key is @s_top's co-located external
	 * chain. It moves with the subtree: ft_rekey_cow_stop carries the
	 * forward pointer and records the head's back edge.  The head itself
	 * is app-owned and is never copied.  For the BARE-HEAD shape there is
	 * no metadata at all. cow_stop's duties collapse to:
	 *  - child re-parent, child marks: vacuous, a head has no children;
	 *  - the head's own back edge: owed. The merge arm's deferred-edge
	 *    machinery records it through ft_reparent_record's external arm;
	 *  - retire: must not happen, the node is app-owned.
	 * So @s_top_meta is NULL and every consumer below dispatches on
	 * @s_top_external.
	 */
	s_top_meta = s_top_external ? NULL : (s_top_compressed ?
		cds_ft_item_to_metadata((struct cds_ft_inode *)
			ft_compressed_node_ptr(s_top)) :
		cds_ft_item_to_metadata(ft_node_ptr(s_top)));

	/*
	 * BP (= S_top's parent).  A COMPRESSED BP is in scope. To the detach,
	 * a run is a node like any other. It just has exactly one child, so
	 * dropping S_top empties it and the detach's upward walk elevates
	 * past it.  The fold carries that shape: the orphan chain rides
	 * @detach_rc and is reclaimed after the one commit. So the only
	 * thing the run changes here is where its metadata lives.
	 *
	 * A SKIP-COMPRESSED BP stays out. Its metadata is the skip target's.
	 * The detach's trailing-skip arm is a second retire that the fold
	 * has no owner for.
	 */
	if (!d_src.pnf || ft_node_flip_proxy(d_src.pnf) ||
			ft_node_external(d_src.pnf))
		return FT_REKEY_UNCOVERED;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(d_src.pnf))
		return FT_REKEY_UNCOVERED;
#endif
	bp_meta = ft_node_compressed(d_src.pnf) ?
		cds_ft_item_to_metadata((struct cds_ft_inode *)
			ft_compressed_node_ptr(d_src.pnf)) :
		cds_ft_item_to_metadata(ft_node_ptr(d_src.pnf));
	/*
	 * RACE-WINDOW INJECTION for the descent->detach ABA hypothesis
	 * (-DFT_DELAY_INJECT, FT_DELAY_MODE=writer, FT_DELAY_US=N).
	 *
	 * @d_src.pnfp is an address into a node body. The descent above
	 * captures it. It is not dereferenced until ft_detach_node's climb
	 * loads *detach_parent_flag_ptr far below.  The node that owns that
	 * slot may be retired, freed and reallocated in between. Then the
	 * address still points at valid memory, but that memory belongs to a
	 * different node. The climb reads a value that was never in the slot
	 * it thinks it read.
	 *
	 * Here is the right place to widen the window. The descent has
	 * captured the slot and the shape gates have run. The op holds no
	 * lock and no reserve yet, so a delay lets peers actually retire and
	 * recycle it.  A later delay, once this op holds the graft's nodes,
	 * would block the very peers whose recycling the hypothesis needs.
	 *
	 * This is paired with the ABA generation witness in ft_detach_node.
	 * For the hypothesis to stand, delay OFF must read 0 and delay ON
	 * must read non-zero.
	 */
	/*
	 * The delay is PROBABILISTIC (FT_DELAY_ONE_IN=N). That is not a
	 * refinement: a flat delay here buys nothing.  Measured: 50us per
	 * attempt widened the window ~18x. It also dropped throughput from
	 * 8616 moves to 464 in the same wall time. So (window x
	 * opportunities) came out flat.  Delaying only 1 attempt in N keeps
	 * the other N-1 at full speed. So the number of wide windows per
	 * unit time actually rises.
	 */
	{
		static long one_in = -1;

		if (caa_unlikely(one_in < 0)) {
			const char *e = getenv("FT_DELAY_ONE_IN");

			one_in = e ? atol(e) : 1;
		}
		if (one_in <= 1) {
			ft_delay_writer();
		} else {
			static __thread unsigned long tick;

			if (++tick % (unsigned long) one_in == 0)
				ft_delay_writer();
		}
	}
	/*
	 * PROXY-SAFE count read. The raw ft_meta_nr_child() would decode a
	 * peer's parked FT_STATE_PROXY as a garbage child count. It would
	 * then reject a perfectly good shape as permanently invalid
	 * (measured: 10-12 spurious -EINVAL per stress run).
	 * ft_meta_nr_child_load resolves the proxy to the committed value.
	 */
	/*
	 * A BINARY BP IS COVERED.  An internal node has at least two
	 * children: a one-child node is path-compressed away. So two is the
	 * minimum arity. Refusing it would refuse the ordinary case, not a
	 * corner case. Dropping S_top from a binary BP leaves one child, so
	 * the move owes a chain-compress collapse. ft_chain_compress_fused
	 * records that collapse into this op's txn. It does not commit one of
	 * its own: two commits cannot be one decide. It hands its chain back
	 * for reclaim on the right side of the commit. The reservation below
	 * is sized for its edges.  So two children ride the same single
	 * decide as three.
	 *
	 * A ONE-CHILD BP is covered too. It is the compressed-run shape. A
	 * run holds exactly one child, so dropping S_top empties it and the
	 * detach's upward walk elevates. The walk stops only at a boundary,
	 * and nr_child > 1 is what makes one.  The orphan chain that walk
	 * clears rides @detach_rc back to this frame. It is freed after the
	 * one commit, which is the unlink it must follow.
	 *
	 * ZERO is not a shape. BP is S_top's parent, so it has at least the
	 * one child this move removes.  A count that reads below one is a
	 * torn or superseded read. The answer is to descend again.
	 */
	if (ft_meta_nr_child_load(bp_meta) < 1)
		return -EAGAIN;

	/*
	 * INCREMENT 3: an occupied dst is a merge, not an error. The moved
	 * subtree unions into whatever already sits at @dst_key. It is not
	 * grafted into a spare slot.  Probe read-only and conservatively, and
	 * record the answer for step 2. A descent that cannot reach the dst
	 * depth plainly proves nothing, so those shapes are left to the
	 * later gate.
	 *
	 * ★ WHY THE MERGE PATH DOES NOT COW S_top. It is not an oversight.
	 * ft_rekey_cow_stop exists to give the top of the moved subtree a
	 * fresh address. The coherent reader's two-descent witness then sees
	 * the move as a changed visited-node set.  A merge does that ALMOST
	 * always. ft_merge_build has exactly two exits that return a live
	 * node: the shared-run collapse needs both sides compressed, and the
	 * leaf-splice needs both external. Everywhere else it falls to its
	 * tail and returns a freshly allocated M. It retires S_top outright
	 * on the way.  An interposed COW would allocate a copy for the merge
	 * to consume and free.
	 *
	 * ☞ "ALMOST" IS WHY THE PUBLISH ASKS ft_glue_is_fresh AND DOES NOT
	 * TRUST THIS.  This used to read "cannot take either exit". It
	 * rested on a plan-time refusal of a compressed S_top. That refusal
	 * is gone. Measured: it refused 215 shapes over the corpus where 2
	 * take a live exit. A decidable test now stands in its place, at the
	 * one point that can answer it. cow_stop's other job is to mark the
	 * children whose state words the commit parks into. That job is not
	 * redundant. The glue's own acquire does it.
	 *
	 * SCOPE OF THIS FIRST CUT (each a permanent -EINVAL, checked at the
	 * gate below, not silently degraded):
	 *  - with the list ON, a moved run whose suffixes disjointly precede
	 *    or follow the destination region's.  Then the moved cells stay
	 *    one contiguous range and splice at a region boundary. That is
	 *    the same run move an empty destination gets.  An interleaved
	 *    range is refused. Threading the cells individually is
	 *    ft_merge_ord_interleave_collect's job. That helper plain-stores
	 *    the links of each surviving cell, because they are not
	 *    ord-reachable in a cross-trie merge. That is false here, where
	 *    the source cells live in the very list being rebuilt
	 *    (ft_rekey_run_vs_region).
	 *  - a PLAIN INTERNAL node at the dst POINT.  A compressed / skip /
	 *    external node AT @dst_len brings the KEY_SHORTER wrap and Edge-D
	 *    shapes, which are ft_merge_spine_copy's job.  A compressed run
	 *    strictly ABOVE the point is in scope -- see the probe below.
	 */
	{
		struct ft_descent d_probe;
		const uint8_t *pk = dst_ord;

		/*
		 * Decode a compressed run that the destination path crosses.
		 * Do not stop at it.  A run the dst key consumes whole is a
		 * matter of routing only. The node at @dst_len below it is
		 * whatever it is. That node chooses the arm, not the way the
		 * descent reached it.  Stopping short reported an occupied
		 * "az" (reached through the path-compressed run under 'a') as
		 * an empty destination. That sent an occupied-destination
		 * move to the empty-dst splice arm. Its bracket check then
		 * found the dst's own keys inside its range and refused,
		 * forever
		 * (project_ft_rekey_occupied_dst_behind_compressed_livelock).
		 *
		 * Two readings still stop the walk, and both leave @merge_dst false
		 * exactly as before:
		 *  - the run DIVERGES from @dst_ord: no key carries the dst prefix,
		 *    so the destination really is empty and the splice arm is right;
		 *  - the run OVERSHOOTS @dst_len (the dst key ends INSIDE it): the
		 *    KEY_SHORTER dst, still outside this cut.  It is
		 *    deliberately NOT hardened into an immediate shape
		 *    refusal. A peer split can make this reading transient.
		 *    So it keeps the BOUNDED path
		 *    (feedback_structural_single_threaded_can_be_transient_under_peers).
		 */
		ft_descent_init(&d_probe, ft);
		while (d_probe.depth < dst_len && d_probe.nf) {
			/*
			 * SKIP, then COMPRESSED, then EXTERNAL. This is the
			 * kind dispatch order the rest of the unit uses. It
			 * is not cosmetic. A skip pointer onto an external
			 * leaf has low tag bits 0, so ft_node_external()
			 * matches it on the raw value
			 * (feedback_skip_before_external_tag_order).
			 * The old AND-chain was order-blind because every arm
			 * meant the same thing. A dispatch is not.
			 */
#ifdef FEATURE_FT_SKIP_COMPRESSED
			if (ft_node_skip_compressed(d_probe.nf))
				break;
#endif
			if (ft_node_external(d_probe.nf))
				break;
			if (ft_node_compressed(d_probe.nf)) {
				struct cds_ft_compressed_node *cn =
					ft_compressed_node_ptr(d_probe.nf);
				unsigned int remaining =
					(unsigned int) (dst_len - d_probe.depth);

				if ((unsigned int) cn->len > remaining) {
					/*
					 * KEY_SHORTER dst: the key ends inside
					 * this run. The merge point is
					 * @remaining bytes into it.  Match only
					 * those bytes: comparing cn->len of
					 * them reads past the dst key. Stop AT
					 * the run rather than breaking out. The
					 * run is the destination, and
					 * ft_merge_build splits it at @off_d.
					 *
					 * ☠ The match is not optional.  The old
					 * arm short-circuited on the length
					 * alone and never compared a byte. So a
					 * run that diverges from the dst key
					 * reaches here too. Adopting it would
					 * union under a prefix the caller never
					 * named. It would also make an
					 * empty-dst graft report -EEXIST.  A
					 * diverging run means the destination
					 * really is empty: break.
					 */
					if (ft_match_compressed_key(pk, cn,
							remaining) != remaining)
						break;
					probe_off_d = remaining;
					break;
				}
				if (ft_match_compressed_key(pk, cn,
						(unsigned int) cn->len)
						!= (unsigned int) cn->len)
					break;
				ft_descent_traverse_compressed(ft, &d_probe, cn, &pk);
				continue;
			}
			ft_descent_step(ft, &d_probe, *(pk++));
		}
		merge_dst = (d_probe.depth + probe_off_d == dst_len &&
				d_probe.nf != NULL);
		probe_D = merge_dst ? d_probe.nf : NULL;
		/*
		 * @require_empty is the graft caller's semantics:
		 * cds_ft_rekey_graft refuses an occupied destination and does
		 * not union into it. -EEXIST is how that travels back. It is
		 * a distinct code because the dispatcher must tell two
		 * answers apart:
		 *  - "the destination holds content": terminal,
		 *    POPULATED_ERROR;
		 *  - "this shape is outside the one-decide cut": -EINVAL, try
		 *    the staged writer.
		 * The op is read-only so far, so this is a clean no-op bail.
		 */
		if (merge_dst && require_empty)
			return -EEXIST;
		/*
		 * ☑ A COMPRESSED S_top IS NOW IN SCOPE ON THE MERGE ARM TOO.
		 * The refusal that used to stand here named a real obligation
		 * with the wrong scope.  The merge arm skips
		 * ft_rekey_cow_stop. The refusal argued that this is fine,
		 * because ft_merge_build gives the moved top a fresh address
		 * by construction. In its words:
		 * "entered with an internal, non-compressed S_top,
		 * ft_merge_build cannot take either of the two exits that return a
		 * live node (the shared-run collapse needs BOTH SIDES COMPRESSED...)"
		 * It also argued that a compressed S_top unlocks that exit.
		 * That is true. But it is a plan-time proxy for a
		 * publish-time fact, and the proxy is two orders coarser than
		 * the fact.
		 *
		 * ☑ MEASURED over 3000 generated shapes x rank on/off x both
		 * list modes, on --enable-rcu-debug and on a release build.
		 * The gate fired 215 times (rank off).  The shape was then
		 * left to build, and a probe recorded what ft_merge_build
		 * actually handed back. That is a probe that records, not a
		 * guard that refuses. 86 of those reach the publish. Exactly
		 * two produce a top that ft_glue_is_fresh does not match.
		 * Both are the shape the premise describes and nothing else:
		 * both sides a compressed run. The first is {abaca,aab,ccb,bc}
		 * + rekey_merge(dst "cc", src "aa"). The second is
		 * {abcccc,cacaa,accca,caaaca,aaa} + dst "accc" src "aa".
		 * The other 213 were refused for a reason that does not apply
		 * to them.
		 *
		 * ☑ SO THE OBLIGATION MOVES TO WHERE IT IS DECIDABLE: the
		 * publish below, which asks ft_glue_is_fresh directly.  That
		 * is strictly stronger than the plan-time proxy in both
		 * directions. It admits every shape whose top really is
		 * fresh. It refuses a live top on a release build. There the
		 * obligation used to rest on a urcu_assert_debug, which a
		 * release build compiles out.
		 *
		 * ☞ WHAT IS STILL OWED, for those two: a COW of the merged
		 * top when ft_merge_build hands back a node that
		 * ft_glue_is_fresh does not match. It is the same
		 * copy-plus-reparent-plus-retire that ft_rekey_cow_stop
		 * already does for the graft arm's S_top.  The live node
		 * comes from ft_merge_materialize_suffix's `suffix_len == 0`
		 * exit, which returns `cn->child`. That is right for a
		 * subtree. It is wrong only when that helper is what produces
		 * the merge's top.
		 */
		/*
		 * ☑ A CO-LOCATED EXTERNAL CHAIN IS SERVED ON THE MERGE ARM TOO.
		 * It used to be refused here. The whole reason for the
		 * refusal was "nothing here has tested it". ft_merge_build
		 * would have to union that key into the destination's own
		 * chain. No oracle drove the shape.  That is a caution,
		 * not a defect. A caution is answered by testing it.
		 *
		 * ☑ TESTED NOW.  The gate was ablated over 3000 generated
		 * shapes x rank on/off x both list modes under
		 * --enable-rcu-debug. It fired 536 times in that corpus, so
		 * the shapes are reached. The failure count was identical to
		 * leaving it in place: 0 with rank off, 6 with rank on. The 6
		 * are the rank lane's own nr_keys residue and are unrelated.
		 * No key loss, no engine assert.
		 * test_rekey_merge_colocated_chain pins it single-threaded,
		 * on all four rank/list combinations:
		 *
		 *     insert {"ab","abm","abn","cdx","cdy"}
		 *     rekey_merge(dst "cd", src "ab")
		 *     -> OK with {cd,cdm,cdn,cdx,cdy}, count 5, verify clean
		 *
		 * ft_rekey_cow_stop carries the chain on the graft arm. On
		 * this arm, ft_merge_build lands it as the union node's own
		 * external_nodes. It already does the same for a BARE head.
		 * @s_top_meta is NULL for that one: its single key is the
		 * head, so there is no second co-located key to union.
		 */
		/*
		 * ☑ THE BARE HEAD IS NOW BOTH ARMS.  It used to be merge-arm
		 * only. An occupied dst consumes it with machinery that
		 * already speaks external-src. ft_merge_build takes S_ext:
		 *  - the head contributes no slots and lands as M's
		 *    external_nodes;
		 *  - its live back edge rides the commit as a deferred edge
		 *    -> ft_reparent_record's external arm;
		 *  - it records no retire for the head, which is app-owned;
		 *  - it returns a fresh union M.
		 * The EMPTY-dst graft arm had none of that. It runs
		 * ft_rekey_cow_stop and ft_store_at_graft_point. The payload
		 * back edge of the latter was a plain store, on the premise
		 * that the payload is a fresh invisible copy. For a live
		 * head, that store is reader-visible and survives an abort.
		 *
		 * The leg that closes it is exactly the two things the old
		 * comment named, and nothing else:
		 *  - RECORD THE BACK EDGE. @glue.payload_live routes
		 *    ft_store_at_graft_point's NOSPLIT arm through
		 *    ft_glue_defer_edge_origin. That reaches the same
		 *    external arm the merge side uses.
		 *  - SKIP THE RETIRE. There is nothing to COW, so
		 *    ft_rekey_cow_stop is not called at all. Its own header
		 *    keeps the shape out of scope. cow_stop is the only
		 *    thing that would have recorded a retire for S_top.
		 *
		 * ☠ SKIPPING cow_stop MEANS @s_top_prime IS THE LIVE HEAD, not
		 * a copy this frame owns. So ft_rekey_free_stop_prime refuses
		 * an external node at its own choke point (every bail path
		 * calls it). The post-commit reclaim of the old S_top skips
		 * this shape too. Without either, the application's own node
		 * goes to the node arena.
		 *
		 * The dst-side freshness that the coherent reader's witness
		 * rests on is unaffected. The reserve recompacts (relocates)
		 * the graft's attach parent, which is a fresh library node.
		 * That is the same adjudication (2026-08-31) the external
		 * S_top admission above already rests on.
		 *
		 * ☑ ADMITTED, AND WHAT IT WAITED FOR.  This gate stood for one
		 * commit longer than the leg did. Admitting the head made a
		 * new shape reachable. That shape hit a defect that was not
		 * the head's. ft_chain_compress_fused recorded a §4.B MW
		 * validate on a word the same op had already acquired SW.
		 * The twin shape verified that the defect was pre-existing.
		 * The twin is an
		 * internal src, which every build serves:
		 *
		 *     insert "cb","accba","aaaab","bcbx","bcby","bbab","acabc","bbcba"
		 *     cds_ft_rekey_merge(ft, dst "bbccc", src "bcb")
		 *
		 * It aborted `urcu_txn_record_chain: r->kind == kind` under
		 * --enable-rcu-debug.  The fix hands that site the op's held
		 * set. The head is admitted here in the commit after it.
		 *
		 * MEASURED at the admission, under --enable-rcu-debug: 3000
		 * rkfuzz shape seeds are clean. The graft's dst-freshness
		 * counter rises 581 -> 725. So 144 moves per corpus that were
		 * refused are now served.
		 */
		/*
		 * ☑ ON A RANK-STATS TRIE TOO, since @8608ed06.  This arm
		 * refused a bare external head there for two successive
		 * reasons. Both are now retired:
		 *  - first, a key loss through the chain-compress collapse's
		 *    one-slot-two-kinds (@148b0f71);
		 *  - then, the rank lane's nr_keys accounting. OK was
		 *    returned and every key was at the right name. But
		 *    cds_ft_verify was RED with `nr_keys mismatch`, sometimes
		 *    with cds_ft_count_keys itself wrong.
		 *
		 * The accounting was never this shape's. Most of the class
		 * fired on ordinary internal-src rekeys the gate never
		 * touched. It fell in four steps:
		 *  - the +count walk's ordering (@375d65ab, 178 -> 6);
		 *  - the REPLACE fold's survivor (@7000b65e, 106 -> 0 on
		 *    nocompress);
		 *  - the split cluster's count plus the walk that charges it
		 *    (@8608ed06, 6 -> 0).
		 *
		 * ☞ MEASURED WITH THE GATE ABLATED: 3000 generated shapes x
		 * both list modes x rank on. The builds were
		 * --enable-rcu-debug default / nocompress / noskip, and
		 * release. Every leg had 0 failures. About 600 more calls per
		 * corpus commit (default 1999 -> 2597, nocompress 2176 ->
		 * 2938, noskip 1980 -> 2557). Rank on now serves exactly what
		 * rank off does.  So the gate is gone.
		 * ☞ [[project_ft_rank_stats_nr_keys_is_the_lane_defect]]
		 */
#ifndef FEATURE_FT_MERGE
		/*
		 * An occupied destination is a merge: INCREMENT 3 unions S_top
		 * into it with ft_merge_build. So -DNO_FEATURE_FT_MERGE
		 * compiles that fold out along with the rest of the
		 * subsystem.  Report it the way the merge API itself does. Do
		 * not build a fold whose machinery is not there.  The
		 * EMPTY-dst rekey below is a graft and stays available.
		 */
		if (merge_dst)
			return -ENOTSUP;
#endif
	}

	/*
	 * List on: capture the endpoints of the moved subtree's contiguous
	 * ordered-cell run. They are the structural min/max external heads
	 * under S_top. Read them from the still-pristine list. The one commit
	 * can then unsplice the run from the src ordered position and
	 * re-splice it at the dst position (six MW boundary edges). It does
	 * so atomically with the structural move. A coherent reader never
	 * sees a moved key gone from the structure but still in the list, or
	 * the reverse.  cow_stop shares S_top's leaves (only S_top's own node
	 * relocates), so these heads stay valid across it.
	 *
	 * Locate the dst splice neighbours NOW, on the pristine list.
	 * find_splice_pos needs the dst attach point empty. It still is:
	 * nothing is published until the final commit.
	 *
	 * REJECT an adjacency shape up front. This happens before any txn,
	 * lock acquire or record, so the bail is a clean no-op -EINVAL.
	 * find_splice_pos runs while the run is STILL at src. So a dst gap
	 * that abuts the run resolves the run's own ENDPOINT cell as a
	 * splice neighbour: dsucc == run first, or dpred == run last. That
	 * would record a duplicate-slot MW edge. It would also plain-store
	 * a self-cyclic run link.  This hook only supports a dst position
	 * clear of the run's current ordered neighbourhood. A general rekey
	 * would locate the splice against the run-removed list instead.
	 *
	 * The "no run cell is a splice neighbour" guarantee is complete
	 * only JOINTLY:
	 *  - this guard rejects the two ENDPOINT-adjacency shapes;
	 *  - the DISJOINT-key rule at the top of this function excludes
	 *    the INTERIOR case. That is a dst gap whose neighbour is a run
	 *    cell strictly between rfc and rlc. Such a neighbour would have
	 *    to be bracketed by two run keys. That puts @dst_key inside the
	 *    run's own contiguous ordinal range. So @src_key would be a
	 *    prefix of it.
	 * The rule is stated here rather than left implicit in a shape
	 * gate, because no gate forces src_len == dst_len.
	 *
	 * VALIDATE the located pair FIRST. Bail -EAGAIN (transient,
	 * re-derive) when it does not bracket the dst key range.
	 * find_splice_pos derives the pair from a RELATIONAL descent. That
	 * descent is NOT coherent under concurrent structural mutation. So
	 * a peer rekey in flight can return a pair that is adjacent in the
	 * list but sits at the WRONG key position.
	 *
	 * ADJACENT + BRACKETING is the full correctness condition for a
	 * splice. Only adjacency was checked. The splice's own boundary
	 * edge (pred->next expect succ) validates adjacency at commit.
	 * ft_rekey_splice_pos_brackets validates the other half here.
	 *
	 * The bracket check must precede the endpoint-adjacency check. Then
	 * a racy pair involving a run endpoint surfaces as the transient
	 * -EAGAIN it is. It does not surface as the permanent -EINVAL of the
	 * genuine (correctly-derived, abutting) shape.  A racy pred == rlc
	 * drags succ = resolve(rlc->next) along. That succ sorts BELOW the
	 * dst range and fails the bracket.
	 *
	 * WHAT IS STILL OPEN (general rekey, not this hook's tested shapes).
	 * A pair that brackets and is adjacent when validated can still be
	 * invalidated afterwards, if a neighbour is interior to a peer's
	 * moving run. The peer's move leaves an interior cell's own links
	 * untouched. So no edge of this commit detects it, and the run lands
	 * inside the peer's run.  Every neighbour reachable in the tested
	 * layouts is either a never-moving key or a peer run's endpoint. The
	 * outer link of an endpoint is one of these edges, hence detected.
	 * Closing it in general needs the per-FT move seqcount / relational
	 * coherence (doc: in-trie-move-seqcount.md). That would also let a
	 * rekey validate the derivation itself.
	 */
	if (ft->ordered_list) {
		struct ft_ord_cell *rfc, *rlc;

		run_rfirst = ft_subtree_minmax_head(ft, s_top, false);
		run_rlast = ft_subtree_minmax_head(ft, s_top, true);
		rfc = ft_ord_cell_ptr(rcu_dereference(run_rfirst->prev));
		rlc = ft_ord_cell_ptr(rcu_dereference(run_rlast->prev));
		if (merge_dst) {
			/*
			 * OCCUPIED destination: the splice position is not a
			 * gap to search for. It is a boundary of the merge
			 * region already there. So derive it from that
			 * region's own endpoint cells, not relationally.
			 * That is strictly better than find_splice_pos: a
			 * structural read of the region needs no two-pass
			 * agreement and no bracket check to be trusted. It is
			 * available only here, where the region exists.
			 *
			 * Which boundary depends on where the moved run sorts
			 * relative to the region. An INTERLEAVE is refused.
			 * ft_rekey_run_vs_region says why: it needs machinery
			 * this writer does not have.
			 *
			 * THE DISJOINTNESS SURVIVES TO THE COMMIT. The
			 * splice's own edges make it so.  The only peer
			 * insert that can break it is one that lands below
			 * the region's minimum (for a run spliced in front).
			 * Anything inside the region still sorts above the
			 * whole run, so the order holds. Such an insert
			 * splices between @run_dpred and @run_dsucc. That is
			 * the very pair ft_ord_cell_run_resplice_edges
			 * records as pred->next == succ and
			 * succ->prev == pred.  So it aborts this commit and
			 * does not mis-order the list.  The same holds,
			 * symmetrically, for a run spliced behind.
			 */
			struct ft_ord_cell *dfirst, *dlast;

			dfirst = ft_ord_cell_ptr(rcu_dereference(
				ft_subtree_minmax_head(ft, probe_D, false)->prev));
			dlast = ft_ord_cell_ptr(rcu_dereference(
				ft_subtree_minmax_head(ft, probe_D, true)->prev));
			if (!dfirst || !dlast)
				return -EAGAIN;		/* region read torn */
			switch (ft_rekey_run_vs_region(ft, rfc, rlc, src_len,
					dfirst, dlast, dst_len)) {
			case -1:			/* run below: splice in front */
				run_dpred = ft_ord_cell_resolve_ord(
						&dfirst->lnode.prev);
				run_dsucc = dfirst;
				break;
			case 1:				/* run above: splice behind */
				run_dpred = dlast;
				run_dsucc = ft_ord_cell_resolve_ord(
						&dlast->lnode.next);
				break;
			default:
				/*
				 * INTERLEAVED: not a run move.  The cells thread
				 * individually between the region's, which
				 * ft_rekey_ord_interleave builds at the cell step
				 * below (and which refuses the shapes it cannot
				 * express).  No splice pair applies.
				 */
#ifdef FEATURE_FT_MERGE
				run_interleaves = true;
				break;
#else
				return -EINVAL;		/* no merge: no interleave */
#endif
			}
		} else {
			/*
			 * COHERENT derivation: two from-root traversals,
			 * compared by their visited-node witness
			 * (ft_ord_cell_find_splice_pos_coherent).
			 * This makes the splice position trustworthy. The
			 * single-pass relational answer is not, and no amount
			 * of checking the answer repairs that. Measured: the
			 * key-bracket check below, which is itself a
			 * relational-era read, was fooled too.
			 * Disagreement means a peer move is restructuring this
			 * neighbourhood: bail and re-derive.
			 */
			if (!ft_ord_cell_find_splice_pos_coherent(ft, dst_key,
					dst_len, &run_dpred, &run_dsucc))
				return -EAGAIN;	/* torn derivation: re-descend */
			/*
			 * Belt and braces, and cheap: the pair must also
			 * bracket the dst key range.  Adjacent + bracketing is
			 * the full correctness condition for a splice. The
			 * two-pass agreement establishes that the pair was not
			 * read torn. This check establishes that it is the
			 * right pair.
			 */
			switch (ft_rekey_splice_pos_brackets(ft, dst_ord,
					dst_len, run_dpred, run_dsucc)) {
			case 1:
				break;			/* bracketed: proceed */
			case -1:
				/*
				 * A key extends the dst prefix.  ☠ THAT IS NOT
				 * ALWAYS STRUCTURAL. This bound exists to
				 * correct that assumption. Single-threaded, it
				 * means the destination is occupied behind a
				 * node the @merge_dst probe would not decode.
				 * No retry can clear it. Under peers, another
				 * move mid-splice produces the very same
				 * reading transiently. Then retrying is
				 * exactly right.
				 * Measured: inv_rekey_graft_shared (16 writers
				 * over 2 shared junctions) refuses moves that
				 * used to succeed if this answers UNCOVERED at
				 * the first sight of it.
				 *
				 * So retry it like the contention it may be.
				 * Give up only once the attempt age says no
				 * peer is plausibly still responsible.  The
				 * bound converts the permanent case from an
				 * unbounded spin into a refusal. Its exact
				 * value only has to sit far above real
				 * contention and far below a livelock. The
				 * rekey oracles complete their moves with
				 * single-digit retries. The measured livelock
				 * ran 193k attempts.
				 */
				if (optxn->retry < FT_REKEY_UNCOVERED_AFTER)
					return -EAGAIN;
				/*
				 * Aged out, so the reading is structural: the
				 * destination really is occupied.  For a graft,
				 * that is the caller's answer. It is the same
				 * -EEXIST the decoded @merge_dst probe gives a
				 * few lines above. No wider cut changes it, so
				 * reporting UNCOVERED would send a settled
				 * refusal to a fallback.  Only a merge, which
				 * would union into that occupant, is the shape
				 * this writer leaves uncovered here.
				 */
				if (require_empty)
					return -EEXIST;
				return FT_REKEY_UNCOVERED;
			default:
				return -EAGAIN;		/* torn: re-derive */
			}
		}
		/*
		 * THE DST POSITION ABUTS THE RUN. That is not a shape to
		 * refuse. It means the moved keys sort into the same list slot
		 * the run already occupies. So the unsplice and the re-splice
		 * cancel, and the run stays put.  This is recorded as
		 * @run_keeps_pos and honoured at the splice below.
		 *
		 * Why it cancels, for the two ways it arises.  The located
		 * pair is adjacent in the list. So @run_dsucc == the run's
		 * first cell forces @run_dpred to be the run's own predecessor
		 * A. And @run_dpred == its last cell forces @run_dsucc to be
		 * its successor B.  Either way the dst key range lies strictly
		 * inside (A, B). The run is the only thing in (A, B). So
		 * the run's new keys sort between A and B exactly where its
		 * old ones did.  Nothing about the list has to change.
		 *
		 * Emitting the six edges anyway is what made this look
		 * unsupportable. The unsplice's A->next = B and the
		 * re-splice's A->next = rfc are two records on one slot. That
		 * breaks the engine's distinct-slot rule and would plain-store
		 * a self-cyclic run link.  This comment used to propose
		 * another repair: locate the pair against the run-removed
		 * list. That yields (A, B) and therefore those same cancelling
		 * edges. The cancellation is the answer, not a different
		 * derivation.
		 */
		if (run_dsucc == rfc || run_dpred == rlc)
			run_keeps_pos = true;
	}

	/*
	 * Subtree key count (count edges no-op if rank off).  A BARE HEAD
	 * carries exactly one key: a dup chain holds duplicates of that same
	 * key, and nr_keys counts distinct keys. ft_merge_build answers the
	 * same for it (*nr_keys_ret = 1 on the leaf arms). It is also what
	 * the detach's -@cnt walk must take back from the src ancestors.
	 */
	cnt = s_top_external ? 1 : ft_nr_keys_get(s_top_meta);

	/*
	 * Create it on the op's persistent handle, not a standalone one.
	 * Retry aging and the FIFO escalation turn live in @optxn and must
	 * survive this attempt. ft_flip_txn_create() inits a handle with no
	 * domain. With it, every attempt started fresh. No attempt ever
	 * qualified for a turn. Each one simply spun.
	 */
	txn = ft_flip_txn_create_on(ft, optxn);
	if (!txn)
		return -ENOMEM;

	/* 1. COW S_top -> S_top' (SW; records re-parents + retire, LOCKED). */
	/*
	 * The mode says nothing about &ft->root: a root records MW by
	 * construction (ft_flip_txn_record_root), whichever txn writes it.
	 *
	 * ☠ NO SHAPE HERE REACHES ONE TODAY, and saying so is the point.  A
	 * depth-1 source junction is the only shape that would make the
	 * detach recompact the root and republish it at that slot. The
	 * junction gate below refuses exactly that shape with
	 * FT_REKEY_UNCOVERED. Its stated reason is that &ft->root has "no
	 * node word to park the republish under".
	 *
	 * The record helper answers that reason. It answers it for every
	 * caller at once, not per txn. The gate can only decline the whole
	 * move. The record simply plants that one slot the way every other
	 * writer of it already does.  Lifting the gate is a separate
	 * question, because other refusals sit behind it. But arming is no
	 * longer part of it.
	 */
	/*
	 * PHASE B, STEP B6: the hand-arm is retired onto the one door.  It
	 * is armed at creation, because every branch of this op passes that
	 * point. The merge_dst fold never reaches ft_rekey_cow_stop. Arming
	 * only there left that fold all-MW, and it aborted its own commit
	 * forever. The nocompress leg caught that livelock as a hang.
	 */
	ft_flip_txn_arm_structural(ft, txn);
#ifdef FT_REKEY_CLAIM
	/*
	 * 9.1 AUDIT: this writer is hand-armed, so B0's owner assert never
	 * runs on it. @dbg_arm_per_op is set only by ft_flip_txn_arm_per_op
	 * and the claim. Point the claim at the one already-armed SW content
	 * site. The assert then names every slot it parks without owning.
	 * §9.1 calls those its missing fine-lock conversions.
	 *
	 * ☠ THE _ARMABLE FORM, NOT THE RAW ONE.  This driver runs under
	 * coarse and exclusive too (ft_rekey_graft_simple_locked takes
	 * CDS_FT_SCOPED_WRITER). There the SW parks are legal on the
	 * trie-wide exclusion. The per-node locks the assert looks for are
	 * deliberately never taken. So the raw claim reports the wide
	 * mutex's own soundness as a violation.
	 * ft_flip_txn_claim_per_op_armable exists to refuse that false
	 * positive. Skipping it cost a full misdiagnosis: ft_inv's coarse
	 * rekey arms aborted at ft_store_at_graft_point_commit's republish
	 * with ft->lock_fine == false. That reads exactly like a missing
	 * acquire and is not one.
	 */
	ft_flip_txn_claim_per_op_armable(ft, txn);
#endif
	if (!merge_dst && s_top_external) {
		/*
		 * THE BARE HEAD: there is nothing to COW.  ft_rekey_cow_stop's
		 * own header keeps this shape out of scope. Its three duties
		 * all collapse:
		 *  - a head has no children to re-parent and no body to fence;
		 *  - its retire must not happen (the node is app-owned);
		 *  - the fresh address the copy exists to give is not owed
		 *    here.
		 * The coherent reader's two-descent witness rests on dst-side
		 * freshness (adjudicated 2026-08-31, see the S_top admission
		 * above). And a head has no interior, hence no second
		 * commit-sensitive read below the junction.
		 *
		 * So the payload is the live head.  Two consequences follow.
		 * Both are carried explicitly, not left to a shape argument:
		 *
		 *  - no bail path may free it.  ft_rekey_free_stop_prime
		 *    refuses an external @nf at its own choke point for
		 *    exactly this reason. Otherwise the application's own node
		 *    goes back to the node arena. Its kind dispatch has no
		 *    external arm, so it would take the plain one on a node
		 *    that never came from the arena.
		 *  - @glue.payload_live is set below. So the graft's NOSPLIT
		 *    arm records the head's back edge into the fold's txn. It
		 *    does not plain-store it ahead of the publish.
		 *
		 * @marks stays empty. That is correct, not merely convenient.
		 * cow_stop's fences exist to protect the body it reads and the
		 * children it re-parents.  Nothing here reads a body.  The src
		 * junction's own expected-old on the slot that holds the head
		 * covers the head disappearing under us. The detach records
		 * it into this same commit.
		 */
		s_top_prime = s_top;
	} else if (!merge_dst) {
		ft_lock_ctx_init(&lctx_src, &d_src, txn, optxn);
		/*
		 * NOT bound to @optxn.  ft_flip_txn_create_*_on sets t->mtxn =
		 * op: an "_on" txn shares the handle and does not make its
		 * own. This driver's content txn is already
		 * ft_flip_txn_create_on(optxn).  Binding here would put the
		 * dedicated acquire txn on that same live handle and commit it
		 * mid-op (measured: SIGSEGV in test_rekey_graft_liston, every
		 * spacing).  The acquire's escalation aging and a content txn
		 * on one handle are mutually exclusive. Giving this path both
		 * needs a second handle, not a binding.
		 */
		ret = ft_rekey_cow_stop(ft, &lctx_src, txn, s_top, d_src.depth,
			src_cut, /*ryw=*/ false, NULL,
				&s_top_prime, marks, &nr_marks);
		if (ret) {
			ft_flip_txn_destroy(txn);	/* pre-commit bail: destroy caller-owned */
			goto sweep;
		}
		ft_rekey_marks_to_txn(txn, marks, nr_marks);
	}

	/*
	 * 2. + 3.  structural_sw stays true for the rest. The graft always
	 * relocates the dst attach node (a reserve recompaction). That
	 * acquires node locks over the dst parent + the republish
	 * grandparent. It records its re-parents / release / retire as SW
	 * under those locks. So the graft forward publish and recompact
	 * edges are correctly SW.  The detach's src-junction (BP) edges are
	 * unlocked, and no toggle is needed. Toggling OFF would wrongly
	 * demote the recompact's lock-expecting edges to MW -> expected-old
	 * mismatch -> abort.
	 *
	 * ★ The reason is NOT that "ft_ord_cell_record_into forces them MW
	 * regardless of structural_sw". This comment said so, and it is
	 * false for exactly these edges. ft_ord_cell_record_into dispatches
	 * on ft_edge_tag(). ft_edge_tag maps tag 0 -> FT_FLIP_PROXY_TAG.
	 * That takes the ft_flip_txn_record_tag branch, which honours
	 * structural_sw, not ft_flip_txn_record_tag_mw.  Only a non-zero
	 * (ordered-cell) tag is forced MW.  What actually makes the
	 * src-junction edges safe here is the exclusive-source gate, not the
	 * record path. So do not weaken that gate on the strength of the old
	 * sentence.
	 * The mixed commit installs the MW (detach) edges first, then parks
	 * the SW (graft + cow_stop) edges before the flip.
	 */

	/* 2. Graft-fold: record-only attach of S_top' at @dst_key. */
	ft_glue_init(&glue);
	glue.op = optxn;
	glue.txn = txn;
	glue.record_only = true;
	glue.src_key = src_key;		/* see @src_key: the split's old direction */
	glue.src_len = src_len;
	/*
	 * ☠ FUSE THE RETIRES INTO THAT ONE COMMIT, EVERY ARM.  @record_only
	 * says this glue does not commit. The caller's single commit is
	 * still ahead of every glue step. That includes the mark acquire in
	 * ft_glue_txn_commit_edges and the forward publish. That commit can
	 * abort.  With the flag clear, ft_glue_tombstone_free_list takes its
	 * standalone arm instead (ft_meta_tombstone_set_flip). It stamps the
	 * one-way LIVE->DEAD mark of each retired node immediately, outside
	 * @txn, where nothing rolls it back.
	 *
	 * That reads the §4.B freeze-on-free contract backwards.  A glue
	 * whose unlink is a flip-txn commit must record the mark as an edge
	 * of that commit. The lone-edge helper exists for retire sites whose
	 * unlink is not a commit yet. This is not one of them.  Fused, the
	 * mark lands with the unlink or not at all.
	 *
	 * MEASURED, and it is a livelock, not a cosmetic window. The mark is
	 * one-way, and ft_dlm_lock hard-refuses a tombstoned word with no
	 * bail. So a mark left by an attempt that then aborted makes that
	 * word unlockable forever.  Above per-node, the lock-set maps onto
	 * an anchor ancestor. So the retired node is the very word the next
	 * attempt's acquire needs. ft_unit
	 * test_rekey_compressed_bp_atomic_or_refused at
	 * CDS_FT_LOCK_SPACING_EXPONENTIAL wedged in 45 s, with no peer in
	 * the test at all. It counted 16.6M immediate marks against 8.3M
	 * acquires refusing on a word this thread had marked.  Per-node
	 * reaches the same store and is a contract violation there too. It
	 * just anchors on the node itself, so the retired node is never the
	 * one a retry re-locks.
	 *
	 * ☞ THE FLAG IS ONLY LEGAL BECAUSE EVERY ENTRY IS HELD.  A fused
	 * entry parks its tombstone SW. The STATE-WORD PROTOCOL permits that
	 * for step 2 only because step 1 took the word. An unheld entry here
	 * would be a blind store on a word this op does not own, able to
	 * strip a peer's LOCK bit. The record-time owner assert refuses it
	 * (-DFT_REKEY_CLAIM aborts at __ft_flip_txn_record_tag_ctx,
	 * single-threaded).  The fold's two unheld producers are both
	 * acquired now: the COLLAPSE arm's resting node, and the survivor
	 * run its build absorbs.  ☞ Recording them MW instead was tried and
	 * refuted. It satisfies the assert and then fails to converge at
	 * exponential. Measured: a descriptor leaked per attempt and RSS
	 * grew to 13.8 GB, with the commit returning OK throughout.  Do not
	 * re-introduce a kind dispatch here. Hold the word or do not retire
	 * it.
	 */
	glue.fuse_free_list = true;
	/*
	 * GLUE (compressed-divergence) shape: have the build fence the
	 * compressed node it splits before it reads its plan. The whole
	 * build then runs under that fence, and its retire rides our commit
	 * (ft_split_compressed_graft_build). The fence is @glue's until the
	 * commit registers it. ft_glue_abort is the single release point,
	 * and every bail below routes through it.
	 */
	glue.fence_split_cn = true;
	memset(&reserve, 0, sizeof(reserve));
	/*
	 * The node reserve exists for the graft arm. Its recompaction
	 * allocates under a no-fail contract and so activates the reserve.
	 * The merge arm never activates it. ft_merge_build allocates its
	 * cluster from the arena and is allowed to fail. That is the whole
	 * reason the fold can still bail there. So filling the reserve would
	 * allocate a batch per move only to drain it untouched.
	 */
	if (!merge_dst && ft_bulk_node_reserve_fill(ft, &reserve)) {
		ft_flip_txn_destroy(txn);
		ret = -ENOMEM;
		goto sweep;
	}
	if (merge_dst) {
#ifdef FEATURE_FT_MERGE
		/*
		 * INCREMENT 3, step 2': UNION S_top into the occupied dst.
		 *
		 * Descend the dst ourselves: ft_graft_build would report
		 * POPULATED and hand back nothing to publish into.  This
		 * descent mirrors the @merge_dst probe above byte for byte.
		 * That makes the two agree on a quiet tree. A wholly-consumed
		 * compressed run is decoded. Every reading the probe stops on
		 * (divergence, an overshooting run, a skip or external node)
		 * bails here too.  So a disagreement is a peer changing the
		 * shape between the probe and this descent. It keeps a
		 * terminal answer, not a retry. A probe and a descent that
		 * disagree on a quiet tree would be a defect this op cannot
		 * retry its way out of.
		 *
		 * ☠ AND THE MIRROR IS NOT TOTAL. The probe stops its WALK on
		 * the same readings. But it never inspects the node AT
		 * @dst_len. It only asks
		 * `merge_dst = (depth + off == dst_len && nf != NULL)`.
		 * So these all PASS the probe:
		 *  - an EXTERNAL D (the dst key is itself a bare head);
		 *  - a COMPRESSED D at offset 0;
		 *  - a skip-encoded D;
		 *  - a parked flip proxy;
		 *  - a ROOT D.
		 * The post-loop validation below refuses them. It does so on a
		 * QUIET tree, single-threaded, with no peer anywhere.  Those
		 * are UNIMPLEMENTED SHAPES. They are not caller mistakes and
		 * not disagreements.
		 *
		 * ★ HENCE FT_REKEY_UNCOVERED, NOT -EINVAL, AT EVERY EXIT IN THIS BLOCK.
		 * The three kinds of refusal are deliberately distinct (their
		 * header sits above FT_REKEY_UNCOVERED). -EINVAL is ARGUMENT:
		 * "the caller got it wrong and NO STATE of the trie would make
		 * the call legal". Not one of these exits is that.  Answering
		 * -EINVAL here surfaced CDS_FT_STATUS_INVALID_ARGUMENT_ERROR
		 * for a well-formed call. It blamed the caller for this
		 * writer's own cut.
		 *
		 * MEASURED before the change, single-threaded on a quiet trie.
		 *
		 *     insert {"az","bcm","bcn"}
		 *     rekey_merge(dst "az", src "bc")
		 *
		 * It answered "Invalid argument", because D at "az" is
		 * a bare external head. It correctly left the trie untouched
		 * and verified clean.  It now answers NOT_SUPPORTED.  The
		 * answer is terminal either way. The same-trie fallback
		 * refuses by design: a staged move hides a live key for a
		 * grace period. So this changes what the caller is told, not
		 * what the writer does.
		 *
		 * @d_dst then names four things: D, the publish parent, the
		 * publish slot and the grandparent. The publish parent may now
		 * be the compressed node itself, and the publish slot its
		 * ->child. That slot has
		 * metadata, a state word and a lock level of its own:
		 * ft_descent_traverse_compressed enters it as one. So every
		 * fence and acquire below reads it the same way it reads a
		 * plain parent.
		 */
		const uint8_t *dk = dst_ord;

		ft_descent_init(&d_dst, ft);
		while (d_dst.depth < dst_len) {
			/* Kind dispatch, SKIP first: see the probe's note. */
			if (!d_dst.nf
#ifdef FEATURE_FT_SKIP_COMPRESSED
					|| ft_node_skip_compressed(d_dst.nf)
#endif
					|| ft_node_external(d_dst.nf)) {
				ret = FT_REKEY_UNCOVERED;
				goto bail_build;
			}
			if (ft_node_compressed(d_dst.nf)) {
				struct cds_ft_compressed_node *cn =
					ft_compressed_node_ptr(d_dst.nf);
				unsigned int remaining =
					(unsigned int) (dst_len - d_dst.depth);

				if ((unsigned int) cn->len > remaining) {
					/*
					 * KEY_SHORTER dst, mirroring the probe: stop AT
					 * the run and carry the offset.  Only a run that
					 * MATCHES the remaining dst bytes is the
					 * destination; a diverging one is the empty-dst
					 * shape and belongs to the splice arm.
					 */
					if (ft_match_compressed_key(dk, cn,
							remaining) != remaining) {
						ret = FT_REKEY_UNCOVERED;
						goto bail_build;
					}
					dst_off_d = remaining;
					break;
				}
				if (ft_match_compressed_key(dk, cn,
						(unsigned int) cn->len)
						!= (unsigned int) cn->len) {
					ret = FT_REKEY_UNCOVERED;
					goto bail_build;
				}
				ft_descent_traverse_compressed(ft, &d_dst, cn, &dk);
				continue;
			}
			ft_descent_step(ft, &d_dst, *(dk++));
		}
		/*
		 * The merge point must be a plain internal node or a
		 * compressed run. The run is entered either:
		 *  - at its start (@dst_off_d == 0, the dst key ends exactly
		 *    where the run begins); or
		 *  - partway in (@dst_off_d > 0, the dst key ends inside it).
		 * A skip or external D remains the Edge-D / leaf-splice
		 * family, which belongs to ft_merge_spine_copy.
		 * d_dst.nf is non-NULL by the probe. It is re-checked because
		 * the probe ran outside this txn.
		 *
		 * ☑ THE START-OF-RUN ARM USED TO BE REFUSED HERE
		 * (`dst_off_d == 0 && ft_node_compressed(d_dst.nf)`). It
		 * refused a shape that nothing below this line objected to.
		 * Ablated over 3000 generated shapes, it aborted at once. But
		 * the abort was in
		 * `ft_node_type: Assertion !ft_node_compressed(node)`. That is
		 * in the type-class gate just below, whose own comment already
		 * says a compressed D "has no class to check".  That gate
		 * asked `dst_off_d == 0`. It used the offset as a proxy for
		 * "D is a plain internal". The proxy was true only because
		 * this line refused the other way of being at offset 0.  The
		 * two gates were holding each other up.
		 *
		 * With the type-class gate asking the node's kind instead,
		 * the shape is served. The same corpus ran on
		 * --enable-rcu-debug and release, in both list modes. There,
		 * 238 more calls commit. The failure count is unchanged (0 rank
		 * off, 6 rank on: the rank lane's own residue).
		 * ft_merge_build already dispatches on a compressed D at
		 * @off_d == 0. There is no KEY_SHORTER prefix to re-wrap, so
		 * @merged_pub replaces the run at its own slot. The
		 * dst-freshness gate at the publish checks the result the
		 * same way it does for every other merge point.
		 */
		if (d_dst.depth + dst_off_d != dst_len || !d_dst.nf ||
				ft_node_flip_proxy(d_dst.nf) ||
				(dst_off_d != 0 && !ft_node_compressed(d_dst.nf)) ||
#ifdef FEATURE_FT_SKIP_COMPRESSED
				ft_node_skip_compressed(d_dst.nf) ||
#endif
				!d_dst.pnf || !d_dst.nfp) {
			ret = FT_REKEY_UNCOVERED;
			goto bail_build;
		}
		/*
		 * The type-class gate asks whether the node scanner can read
		 * D's body. Only a plain internal merge point raises that
		 * question. ft_merge_build dispatches a compressed D on
		 * cn_d->key_bytes[off_d] and never scans a slot array. So a
		 * compressed D has no class to check, at either offset.
		 *
		 * ☞ ASK THE NODE'S KIND, NOT THE OFFSET.  This read
		 * `dst_off_d == 0`, which is equivalent today. The validation
		 * above refuses `dst_off_d != 0 && !ft_node_compressed(D)`,
		 * so past it a non-zero offset implies a compressed D. But it
		 * is equivalent only by way of a refusal. The offset form
		 * made ft_node_type() assert the moment the start-of-run arm
		 * was admitted.  The kind is the fact this gate is actually
		 * about.
		 */
		if (!ft_node_compressed(d_dst.nf) && !ft_node_external(d_dst.nf)) {
			unsigned int dti = ft_node_type(d_dst.nf);

			if (ft_types[dti].type_class != FT_POPCOUNT &&
					ft_types[dti].type_class != FT_PIGEON) {
				ret = FT_REKEY_UNCOVERED;
				goto bail_build;
			}
		}
		/*
		 * Fence the publish parent ourselves, exactly as the GLUE arm
		 * does and for the same reason. This txn is structural_sw, so
		 * the forward publish parks a plain store into that node's
		 * slot. The fold's rule is SW iff the op holds the slot's
		 * lock.  Otherwise ft_glue_txn_commit_edges would degrade an
		 * acquire miss to a §4.B guard.
		 */
		/*
		 * Hand the fold's glue its anchor source here, the single
		 * place that holds both (ft_merge_spine_copy does the same at
		 * its own gd.lock_d).  The fold's members are reached from the
		 * build, a deferred re-parent child above all. So nothing else
		 * can date them. A glue with no descent leaves every one of
		 * them undatable. Then the acquire bails and the caller
		 * re-descends. The next attempt is identical.
		 */
		glue.lock_d = &d_dst;
		/*
		 * And the SRC path for the members still on it.  The fold's
		 * deferred re-parents come from both sides: the src children
		 * being moved and the dst children being absorbed. Each must
		 * anchor where it is now. That is the only position a peer
		 * can descend to before this commit lands (§3).
		 */
		glue.lock_d_src = &d_src;
		pp_meta = ft_flag_to_metadata(ft, d_dst.pnf);
		{
			struct ft_lock_ctx dctx;
			struct ft_held_anchor pph;

			/* The DST descent dates this one: it IS its parent slot. */
			ft_lock_ctx_init(&dctx, &d_dst, txn, optxn);
			if (!pp_meta || ft_acquire_member(ft, &dctx, d_dst.pnf,
					pp_meta, d_dst.pdepth, &pph)) {
				pp_meta = NULL;
				ret = -EAGAIN;
				goto bail_build;
			}
			pp_meta = pph.lock;
			pp_snap = pph.lock_snap;
			/*
			 * A SHARED acquire deduped onto a word this op already
			 * holds. The fence is in force, so the forward publish
			 * may still park its SW store. But the first acquire
			 * owns both the release and the registry entry.
			 * @publish_parent_shared tells the commit so.  The
			 * split arm below carries the same flag. Refusing it
			 * here instead would refuse the op's own mark forever.
			 */
			pp_shared = pph.shared;
		}
		ft_glue_take_publish_parent(&glue, pp_meta, pp_snap, pp_shared);
#ifdef FEATURE_FT_SKIP_COMPRESSED
		/*
		 * A COMPRESSED publish parent is the shape that a destination
		 * behind a path-compressed run presents. It makes the forward
		 * publish two stores, not one. _ft_publish_to_parent also
		 * re-encodes the SKIP_X pointer that lets a candidate reader
		 * bypass the compressed node. That pointer sits in a slot of
		 * the compressed node's own parent.  This txn is
		 * structural_sw, so both stores park. A park does not
		 * arbitrate: the op must hold the word behind each.
		 * @publish_parent_holder covers the first. This covers the
		 * second. It is the same {GP} third lock-set member that
		 * ft_node_recompact takes whenever its own parent is
		 * compressed (§9.3).
		 */
		if (ft_node_compressed(d_dst.pnf)) {
			struct cds_ft_compressed_node *pcn =
				ft_compressed_node_ptr(d_dst.pnf);
			struct cds_ft_metadata *pcn_meta =
				cds_ft_item_to_metadata(
					(struct cds_ft_inode *) pcn);
			struct cds_ft_inode_flag **dual =
				ft_get_parent_slot(pcn_meta, ft);

			/*
			 * ☠ THE PUBLISH AND THE LOCK MUST NAME ONE SLOT.  The
			 * publish derives the dual from @pcn's own
			 * back-pointer. The lock below is taken on the parent
			 * the descent reached.  A disagreement means a peer
			 * re-homed @pcn since the descent. Then the lock would
			 * fence a node the publish never writes, and the store
			 * it does make would be an unguarded park.  Answer the
			 * peer, not the shape: this is precisely a condition a
			 * re-descent clears.
			 *
			 * (@pcn's own lock is taken above. Holding it makes
			 * the reading stable from here to the commit. A peer
			 * re-home of @pcn parks @pcn's state word, and it
			 * cannot do that behind this fence.)
			 */
			if (dual != d_dst.pnfp) {
				ret = -EAGAIN;
				goto bail_build;
			}
			if (dual && ft_node_skip_compressed(*dual)) {
				struct ft_lock_ctx gctx;
				struct ft_held_anchor gph;
				struct cds_ft_metadata *gp_meta;

				/*
				 * The dual lives in &ft->root: a slot with no
				 * node word to park under.  Same refusal the
				 * junction gate below makes for a root-level
				 * junction, and for the same reason.
				 */
				if (!d_dst.ppnf) {
					ret = FT_REKEY_UNCOVERED;
					goto bail_build;
				}
				gp_meta = ft_flag_to_metadata(ft, d_dst.ppnf);
				ft_lock_ctx_init(&gctx, &d_dst, txn, optxn);
				/*
				 * ☠ CHAIN THE GLUE, or a COARSE spacing
				 * livelocks the move.  @publish_parent's mark
				 * is set a few lines up. It reaches no txn
				 * registry until ft_glue_txn_commit_edges
				 * runs: the one decide has not happened yet.
				 * So the glue is the only witness that this op
				 * holds it.  Some spacings collapse the
				 * compressed parent and its own parent onto
				 * one word. There, an acquire that cannot see
				 * that witness reads the op's own fence as
				 * contention and bails -EAGAIN. Every retry
				 * re-derives the identical plan.  This is
				 * single-threaded, so no peer can ever clear
				 * it.
				 */
				gctx.held.glue = &glue;
				if (!gp_meta || ft_acquire_member(ft, &gctx,
						d_dst.ppnf, gp_meta,
						d_dst.ppdepth, &gph)) {
					ret = -EAGAIN;
					goto bail_build;
				}
				glue.publish_gp_holder = gph.lock;
				glue.publish_gp_snap = gph.lock_snap;
				/*
				 * A FRESH hand-off answers again -- see
				 * ft_glue_take_publish_parent.
				 */
				glue.publish_gp_scrubbed = false;
				/*
				 * Coarsening routinely collapses GP onto the
				 * publish parent's word: they are adjacent
				 * levels. The fence is in force either way,
				 * but only the first acquire owes the release.
				 */
				glue.publish_gp_shared = gph.shared;
				/*
				 * HAND THE FENCE TO THE TXN HERE, not at
				 * ft_glue_txn_commit_edges. The two lines are
				 * the same two that the held arm of
				 * ft_flip_txn_hold_or_lock_parent runs. This
				 * is the moment they become true.  Between
				 * here and commit_edges, the fold's detach
				 * recompacts under this very word and
				 * republishes into its body
				 * (ft_chain_compress_fused -> the forward
				 * slot). A record-time owner check has only
				 * the txn's locks[]. A fence that arrives
				 * afterwards reads as unowned at every record
				 * in between. -DFT_REKEY_CLAIM aborts on that.
				 *
				 * The detach's own acquire of that word
				 * dedupes (ft_chain_compress_fused registers
				 * only !held.shared) and so registers nothing.
				 * That is right, since this acquire owes the
				 * release.  After the transfer, that dedupe
				 * answers from the registry, which
				 * ft_held_set_snap reads first. So the coarse
				 * spacing that the glue-chaining above
				 * protects is unaffected.
				 *
				 * The release is recorded, not deferred. The
				 * early placement is the one its own contract
				 * asks for:
				 *  - ft_flip_txn_record_anchor_release_held is
				 *    placed by coverage, not by order;
				 *  - a §4.B guard planted before a release
				 *    would poison the txn;
				 *  - a retire this op decides later chains
				 *    onto the release's clean pending value.
				 */
				if (!gph.shared) {
					/*
					 * OWN it, not merely register it.
					 * The fields stay set. Without a
					 * linked silencer, this fence keeps
					 * answering holds() past the txn's
					 * terminal. That is where the release
					 * below is consumed and the word
					 * comes back LIVE (finding B).
					 */
					(void) ft_flip_txn_lock_own_silenced(
						txn, gph.lock, gph.lock_snap,
						&glue.publish_gp_scrubbed);
					FT_GLUE_OWN_COUNT(1);
					ft_flip_txn_record_anchor_release_held(
						txn, gph.lock);
					glue.publish_gp_txn_owned = true;
				}
			}
		}
#endif /* FEATURE_FT_SKIP_COMPRESSED */

		/* Size both glues from the read-only pre-pass, with headroom. */
		ft_glue_init(&src_glue);
		src_glue.op = optxn;
		/*
		 * The merge's SRC glue anchors from the src descent, as the dst
		 * glue does from @d_dst. Its overlap fences land on nodes still
		 * sitting under @s_top, and only @d_src dates those.
		 */
		src_glue.lock_d = &d_src;
		src_glue_live = true;
		/*
		 * ONE op, ONE held set.  The two glues commit together, so a
		 * mark either takes is a mark the other's dedupe must see.
		 * Under coarsening they do meet. A src child anchors on the
		 * src node the merge build fenced as an overlap. So the
		 * re-parent mark refuses against this op's own fence, and
		 * every retry rebuilds it.
		 */
		glue.peer = &src_glue;
		src_glue.peer = &glue;
		ft_merge_count(ft, s_top, 0, d_dst.nf, dst_off_d, &mcnt);
		if (ft_glue_reserve(&glue, mcnt.nb + 8, mcnt.nd + 8,
					mcnt.nf_dst + 8, mcnt.ns + 8) ||
		    ft_glue_reserve(&src_glue, 0, 0, mcnt.nf_src + 8, 0)) {
			ret = -ENOMEM;
			goto bail_build;
		}
		/*
		 * FUSE THE SRC FREE LIST TOO.  The dst side already carries the
		 * flag from the init above (every arm does, and why is stated
		 * there). This side must carry it for the reason the whole
		 * fold exists. Its free list carries S_top, whose retire has
		 * to flip with the publish. It must not be a standalone
		 * lone-edge store the commit could not roll back.  The
		 * reserve above sized both. The dst side must carry the flag
		 * regardless: a fenced overlap retire records its
		 * {LOCK|s -> TOMBSTONE|s} terminal into g->txn and asserts on
		 * the flag.
		 */
		src_glue.txn = txn;
		src_glue.fuse_free_list = true;
		mctx.dst_ft = ft;
		mctx.gd = &glue;
		mctx.gs = &src_glue;
		/*
		 * Fence the dst overlap spine.  The STAGED rekey reaches
		 * ft_merge_spine_copy already past its point of no return, and
		 * so must skip this. Here, the one-decide fold's single commit
		 * is still ahead. So a missed fence is a clean re-descend, and
		 * the fence is affordable.
		 */
		mctx.fence_overlap = ft->lock_fine;
		/*
		 * ★ And the SRC spine too, which the cross-trie merge does not
		 * need. cds_ft_merge_at owns its source exclusively. This
		 * source is the live in-trie subtree. The detach is only
		 * recorded into the same txn, so S_top and everything under it
		 * stays reachable to peers for the whole build window.
		 * Unfenced, a peer inserting below S_top during the copy is
		 * retired along with the node it was inserted into. That is a
		 * silent key loss. ft_merge_lock_overlap's header states the
		 * mechanism for the dst side; it is the same mechanism.
		 */
		mctx.fence_src = ft->lock_fine;
		mctx.overlap_contended = false;
		/*
		 * Where relative depth 0 IS on each side, so both spines' fences
		 * date their nodes ABSOLUTELY (struct ft_merge_ctx).  The fold
		 * enters both sides at offset 0, so each base is its descent's
		 * own cursor depth.
		 */
		/*
		 * @dst_base_depth dates the dst spine's fences absolutely. A
		 * KEY_SHORTER merge point sits @dst_off_d bytes below the
		 * cursor, not at it. So it has to add them, or every fence
		 * below is dated one run short.
		 */
		mctx.dst_base_depth = d_dst.depth + dst_off_d;
		mctx.src_base_depth = d_src.depth;
		/*
		 * ☠ FENCE THE KEY_SHORTER RUN OURSELVES.  ft_merge_build fences
		 * a compressed D only at @off_d == 0. Its stated argument:
		 * "a run re-entered at off_d > 0 by the shared-run RECURSION
		 * was already fenced by the frame that entered it at 0".
		 * That premise does not hold for a top-level entry at
		 * off_d > 0. No frame ever entered this run at 0, so nothing
		 * fenced it. Its retire would ride the plain free list.
		 *
		 * ft_merge_spine_copy uses the plain list for the same wrap.
		 * It is sound there because that txn is not structural_sw, so
		 * the tombstone is recorded MW and validates.  This txn is
		 * structural_sw unconditionally. An unfenced free-list entry
		 * is tombstoned with an SW park: a plain store on a state
		 * word this op does not hold. That is precisely the
		 * step-2-without-step-1 defect the STATE-WORD PROTOCOL names.
		 * It strips a peer's in-force LOCK bit.
		 * Measured: without this, -DFT_REKEY_CLAIM aborts
		 * single-threaded at ft_flip_txn_record_tag_ctx's owner
		 * assert. With it, the run is clean.
		 *
		 * Take it before the build reads @D: the acquire and the
		 * retire that ratifies it must bracket the same world. Date
		 * the run at its own start depth, not the merge point's.
		 */
		if (dst_off_d && mctx.fence_overlap) {
			struct ft_lock_ctx dctx;

			ft_glue_lock_ctx(&glue, &dctx);
			if (ft_merge_lock_overlap(ft, &dctx, d_dst.nf,
					(unsigned int) d_dst.depth,
					ft_compressed_node_ptr(d_dst.nf),
					&ks_held)) {
				ret = -EAGAIN;
				goto bail_build;
			}
			ft_glue_defer_free_fenced(&glue,
				ft_compressed_node_ptr(d_dst.nf), true, &ks_held);
			ks_fenced = true;
		}
		/*
		 * ☠ @src_cut, NOT 0, IS THE SOURCE-SIDE OFFSET.  @off_s is
		 * where the src key ends inside S_top. For a run the src key
		 * ends inside (@src_cut > 0), the two are different depths. 0
		 * is the run's own start. The bytes in [0, @src_cut) belong to
		 * the src key's prefix, not to the subtree being moved.
		 * Passing 0 moved the whole run. So every moved key arrived at
		 * `dst || whole-run` instead of `dst || run-tail`. The same
		 * commit clears the old name, so the key was reachable at
		 * neither name the caller knows.
		 *
		 * The graft arm never had to say this. ft_rekey_cow_stop
		 * manufactures the tail (its own @cut parameter, fed @src_cut
		 * at the call below). So by the time the graft publishes,
		 * offset 0 is the cut.  The merge arm skips cow_stop, so
		 * nothing had cut the run. This argument is the only place
		 * the cut can be expressed.
		 *
		 * MEASURED single-threaded on a quiet trie, in all four
		 * rank/list modes and both builds.
		 *
		 *     insert {acbaab,bba,ab,cacab}
		 *     rekey_merge(dst "a", src "cac")
		 *     -> "cacab" landed at "aacab"
		 *
		 * That is dst + the whole run "acab", where the contract says
		 * "aab".  cds_ft_count_keys and cds_ft_verify were both clean
		 * over it. Only a lookup at the two names the caller can name
		 * could see it.
		 *
		 * ☞ INERT UNTIL THE GATE ABOVE LIFTED.  @src_cut is non-zero
		 * only when the descent stopped on a run, which is exactly
		 * what sets @s_top_compressed. So for every shape this arm
		 * served before, it is 0 and this reads byte for byte as it
		 * did.
		 */
		merged_nf = ft_merge_build(&mctx, s_top, src_cut, d_dst.nf, dst_off_d, 0,
				d_dst.pdepth, &merged_keys);
		if (merged_nf == FT_MERGE_OOM) {
			merged_nf = NULL;
			ret = mctx.overlap_contended ? -EAGAIN : -ENOMEM;
			goto bail_build;
		}
		/*
		 * KEY_SHORTER dst: the merge point is inside the run. So
		 * @merged_nf is wrapped under the run's leading
		 * [0, @dst_off_d) bytes. The result replaces the run at its
		 * own slot.  ft_merge_build
		 * entered that run and did not copy it, so it recorded no free
		 * for it. The retire is the caller's, exactly as
		 * ft_merge_spine_copy's ks lane does it.
		 */
		merged_pub = merged_plain = merged_nf;
		if (dst_off_d) {
			struct cds_ft_compressed_node *wrap_cn =
				ft_compressed_node_ptr(d_dst.nf);
			uint8_t kbuf[FT_MAX_KEY_LEN];
			unsigned int wrap_depth = (unsigned int) d_dst.depth;

			if (!ks_fenced)
				ft_glue_defer_free(&glue, wrap_cn, true);
			memcpy(&kbuf[wrap_depth], wrap_cn->key_bytes, dst_off_d);
			merged_pub = ft_merge_wrap_prefix(&mctx, kbuf,
					wrap_depth, wrap_depth + dst_off_d,
					merged_nf, merged_keys);
			if (merged_pub == FT_MERGE_OOM) {
				merged_pub = NULL;
				ret = -ENOMEM;
				goto bail_build;
			}
			/*
			 * SLOT-CANONICAL, as every other lane publishes it.
			 * Both wrap arms hand back the plain compressed flag.
			 * The glue commit stores @top raw:
			 * _ft_publish_to_parent re-encodes only the
			 * compressed-parent dual, never the child.  Without
			 * this, the slot that held skip(run) ends up holding
			 * a plain flag at a skip-encodable length. Readers
			 * tolerate that form but nothing else produces it,
			 * and the skip optimisation is silently lost on that
			 * edge.  This mirrors ft_graft_swap's KEY_SHORTER
			 * publish and ft_merge_spine_copy's M_slot.
			 */
			merged_plain = merged_pub;
			if (ft_node_compressed(merged_pub))
				merged_pub = ft_publish_compressed(ft,
					ft_compressed_node_ptr(merged_pub),
					merged_pub);
		}
		/*
		 * ☠ THE DST-FRESHNESS OBLIGATION. IT IS A GATE, NOT AN ASSERT.
		 * The two-descent witness of the rekey-coherent reader
		 * (ft_lookup_two_descents) rests on every move publishing a
		 * fresh library node at the destination attach point. On this
		 * arm, that node is the union node M that ft_merge_build
		 * returns. This is what licenses the BARE-HEAD src admitted
		 * above: an app-owned head gets no fresh address of its own.
		 *
		 * ☑ IT USED TO BE A urcu_assert_debug HERE, GUARDED BY A
		 * PLAN-TIME PROXY UPSTREAM (`merge_dst && s_top_compressed`,
		 * and the merge point taking no external D).  Both halves
		 * were wrong for the job. The proxy refused 215 shapes over
		 * the 3000-shape corpus, where only two ever produce a live
		 * top. The assert was the only thing standing behind it, and
		 * it compiles out of a release build. So on the build that
		 * ships, the obligation was unchecked.  Asking
		 * ft_glue_is_fresh here is decidable, exact and
		 * unconditional. It costs one walk of @glue.built on the arm
		 * that is already committing.
		 *
		 * ☠ THE OBLIGATION IS TEMPORARY BY STATED INTENT. That is why
		 * it must not rest on construction. The pigeon -ERANGE arm
		 * carries a planned sticky-bitmap future
		 * (ft-mutation-node.h). The standing direction is to re-allow
		 * in-place mutations without COW, with rekey special-cased to
		 * COW the parent. If that special-case is ever missed, the
		 * breakage is silent. A torn descent that matches a clean one
		 * returns a wrong answer. No gate leg reddens.  A refusal
		 * is loud.
		 *
		 * REFUSING HERE IS A CLEAN NO-OP. This is upstream of
		 * ft_glue_set_publish, so nothing is recorded and nothing is
		 * stored. The -ENOMEM arm above already bails from the same
		 * point. @bail_build reclaims every fresh copy this attempt
		 * built.
		 *
		 * ft_glue_is_fresh matches by underlying identity, so the
		 * KEY_SHORTER wrap and the skip re-encode both still answer true.
		 */
		FT_REKEY_DST_FRESH_REACH(0);
		if (merged_pub == d_dst.nf ||
				!ft_glue_is_fresh(ft, &glue, merged_pub)) {
			/*
			 * ☑ THE UNION PRODUCED NOTHING FRESH -- SO MOVE THE PARENT.
			 *
			 * MEASURED over 3000 shapes: every arrival here is a
			 * DUPLICATE-CHAIN SPLICE.  There is one key on each
			 * side and no internal node under either. So
			 * ft_merge_build appends the src leaf to the dst
			 * head's dup chain and hands the head back. The attach
			 * slot keeps the value it had, and nothing structural
			 * is created on either side.  A COW of the merged top
			 * is impossible, not merely unhelpful. The top is the
			 * application's own external head, live at the
			 * destination. ft_glue_is_fresh answers false for any
			 * external outright ("externals are never
			 * glue-tracked").
			 *
			 * What can move is the head's immediate internal
			 * parent. Moving it is enough: the reader's second
			 * descent reaches the chain through that node. So
			 * relocating it is the address-witness the
			 * two-descent scheme needs.
			 *
			 * ☞ IT IS DONE UNCONDITIONALLY ON THIS EXIT, not on a
			 * runtime "did the path change" test.  Such a test is
			 * not computable here: the check runs during the merge
			 * build, before the detach has planned anything. The
			 * shape it would have to catch is precisely the one
			 * where the rekey does not change the internal path to
			 * the head. There the rekey moves only the last byte
			 * of the key, so the last-level internal node is the
			 * same node.  Today, every internal mutation a bulk op
			 * makes relocates on a shared trie (the bulk reserves
			 * and detaches vouch only ft_in_place_excl_ok). So a
			 * gate written now would read "changed" for the wrong
			 * reason. It would flip silently if that tier were
			 * ever widened. This exit already stands on one
			 * predicate: the attach point got no fresh address.
			 * That is the decidable half. It fails safe. An
			 * in-place ft_merge_build handing back a mutated D
			 * answers "not fresh" more often, never less.
			 */
			/*
			 * The src's nearest plain ancestor, for the
			 * same-parent test. A src leaf with no sibling at its
			 * last byte hangs off a run of its own. The node the
			 * detach relocates is that run's parent, not the run.
			 */
			struct cds_ft_inode_flag *src_plain = d_src.pnf;

			if (src_plain && (ft_node_compressed(src_plain)
#ifdef FEATURE_FT_SKIP_COMPRESSED
					|| ft_node_skip_compressed(src_plain)
#endif
					))
				src_plain = d_src.ppnf;
			ret = ft_rekey_merge_cow_publish_parent(ft, &d_dst, txn,
					optxn, &glue, merged_pub, src_ord,
					src_len, dst_ord, src_plain, merged_plain,
					&cow_after, &witness_above, marks, &nr_marks,
					&dst_pp_prime, &dst_pp_old);
			if (ret == FT_REKEY_COW_AFTER_DETACH) {
				/*
				 * ON THE SOURCE PATH: announce the publish for
				 * the detach's fold and copy the frame AFTER it
				 * (the block ahead of detach_bail).  Nothing is
				 * deferred to the glue. A queued back edge
				 * aimed at the node this commit retires is the
				 * stale top edge the same-path class met. The
				 * copy's own sweep re-homes the top.
				 */
				FT_REKEY_DST_FRESH_REACH(3);
				txn->pending_pub_slot = d_dst.nfp;
				txn->pending_pub_val = merged_pub;
				txn->pending_pub_plain = NULL;
				ret = 0;
			} else if (ret == FT_REKEY_WITNESS_BY_DETACH) {
				/*
				 * SAME-PARENT SPLICE: the src detach relocates
				 * @d_dst.pnf itself and folds this publish into
				 * the fresh node.  Publish normally; the fold is
				 * verified right after the detach (see
				 * @witness_by_detach at detach_bail).
				 */
				FT_REKEY_DST_FRESH_REACH(2);
				if (merged_pub != d_dst.nf) {
					/*
					 * A GLUE-BUILT top: set the glue's
					 * publish fields and the announcement
					 * by hand. Its parent is a plain store
					 * now: it is unpublished, and the frame
					 * is still its live parent. There is no
					 * deferred edge, which would be aimed
					 * at the node the detach retires.  The
					 * recompaction's sweep re-homes it
					 * through @pending_pub_plain.
					 */
					glue.publish_parent = d_dst.pnf;
					glue.publish_slot = d_dst.nfp;
					glue.top = merged_pub;
					txn->pending_pub_slot = d_dst.nfp;
					txn->pending_pub_val = merged_pub;
					txn->pending_pub_plain = merged_plain;
					ft_set_parent(ft, merged_plain, d_dst.pnf,
						d_dst.nfp);
				} else
					ft_glue_set_publish(ft, &glue, d_dst.pnf,
						d_dst.nfp, merged_pub);
				witness_by_detach = true;
				ret = 0;
			} else if (ret) {
				goto bail_build;
			}
			/*
			 * ☠ THE COPY'S ORIGINAL HAS NO OTHER OWNER.  cow_stop's
			 * contract: "the caller ... frees the OLD @stop via call_rcu
			 * after the grace period once the commit succeeds".  The src
			 * lane does that for its S_top (cds_ft_free_item_deferred
			 * (s_top_meta) below). This lane never did for the node
			 * it copied. MEASURED with the drain oracle: every
			 * move served by this arm leaked exactly one internal
			 * node (4 of 4 corpus seeds). The non-drain balance's
			 * floor had hidden it.  The helper names the node as
			 * @dst_pp_old: the frame it chose, which is the run's
			 * parent when the head hangs off a run. It is freed in
			 * the post-commit reclaim block.
			 */
		} else {
		/*
		 * The merged top replaces D in the publish parent's slot.  It
		 * is recorded, not stored. ft_glue_txn_commit_edges runs at
		 * step 3c below, after the detach, like the GLUE arm.
		 */
		ft_glue_set_publish(ft, &glue, d_dst.pnf, d_dst.nfp, merged_pub);
		}
		glue.attached_nf = merged_pub;
		/*
		 * ☠ THE DST DELTA IS WHAT THE UNION ADDED, NOT WHAT THE SRC HELD.
		 *
		 * A collided key is present on both sides. ft_merge_build
		 * splices the src leaf onto the dst head's duplicate chain
		 * (see the holder acquire just below). So the merged node
		 * holds one key where the two sides held two.  Charging @cnt
		 * to the publish parent's ancestors therefore over-counts by
		 * exactly the number of collisions, everywhere that walk
		 * lands. @merged_keys is the union's own answer for the node
		 * that replaces D. D's aggregate is what that slot held
		 * before. So the net this walk owes is their difference. The
		 * cross-trie lane already records the identical form
		 * (@merged_keys - @cnt_dst, ft_rekey_merge_spine's step 3).
		 *
		 * The KEY_SHORTER arm is covered unchanged. It replaces the
		 * whole run at that same slot with @merged_pub.
		 * ft_merge_wrap_prefix carries @merged_keys onto the wrapper.
		 * So both sides of the subtraction still describe exactly one
		 * slot.
		 *
		 * MEASURED with rank stats on, in either list mode. The move
		 * was "w" {wm,wn} onto an occupied "zq" {zqm,zqn}. Both keys
		 * collide. It left the depth-1 ancestor reading
		 * `stored 5, computed 3`.  The structure and the key set were
		 * already right. Only the aggregate was wrong, so nothing but
		 * cds_ft_verify and the rank/select queries could see it.  It
		 * could not be reached at all until the count walk stopped
		 * poisoning the descriptor
		 * (ft_flip_txn_record_count_parent's read-your-own-writes).
		 *
		 * ☑ THIS FIXES THE VALUE. THE WALK'S BASE was its own defect
		 * and is fixed too, in two halves.
		 *
		 * First half: a SIBLING move. There src and dst are under the
		 * same parent, so the publish parent is the src junction BP
		 * that the detach DEL-recompacts. Such a move charged this
		 * delta into the superseded copy. MEASURED then:
		 *
		 *     over {q,wam,wan,wbx,wby,wcz}
		 *     rekey("wa" -> "wb")
		 *     -> that node at `stored 3, computed 5`
		 *
		 * @85c8c8e6 gave the glue lane the equivalent of what the
		 * store lane already had:
		 *  - bake into the named survivor (@pending_pub_node);
		 *  - walk from the shared stable parent
		 *    (ft_glue_txn_commit_edges' folded-publish arm).
		 *
		 * Second half: the relocated node may be an ancestor of the
		 * base, the root above all. No caller-side re-base can name
		 * it. So the walk itself now follows the survivor through the
		 * descriptor (ft_flip_txn_record_count_parent's chain
		 * resolution).
		 */
		{
			unsigned long d_keys;

			/*
			 * ☠ THREE KINDS, NOT TWO.  An EXTERNAL D has no node
			 * metadata at all. ft_node_ptr() on it yields the
			 * application's leaf, which never came from the arena.
			 * cds_ft_item_to_metadata SEGVs on it.
			 * ft_node_key_count is the kind-complete answer:
			 * "One for an external leaf; the maintained nr_keys
			 * when the trie keeps order statistics, else a
			 * structural recount". The merge itself uses it via
			 * ft_merge_child_count.
			 */
			if (ft_node_external(d_dst.nf))
				d_keys = ft_node_key_count(ft, d_dst.nf);
			else {
				struct cds_ft_metadata *d_meta =
					ft_node_compressed(d_dst.nf) ?
					cds_ft_item_to_metadata((struct cds_ft_inode *)
						ft_compressed_node_ptr(d_dst.nf)) :
					cds_ft_item_to_metadata(ft_node_ptr(d_dst.nf));

				d_keys = ft_nr_keys_get(d_meta);
			}
			glue.count_delta = (long) merged_keys - (long) d_keys;
			/*
			 * W5: the relocated publish parent is BELOW the node
			 * the walk starts from.  So it takes its share here,
			 * with a plain store into an unpublished body.
			 */
			if (dst_pp_prime && glue.count_delta) {
				struct cds_ft_metadata *ppm =
					cds_ft_item_to_metadata(
						ft_node_ptr(dst_pp_prime));

				ft_nr_keys_store(ft, ppm,
					ft_nr_keys_get(ppm) +
						(unsigned long)
						glue.count_delta,
					CMM_RELAXED);
			}
		}
		/*
		 * COLLIDED KEYS.  When a full key is present on BOTH sides,
		 * ft_merge_build splices the src leaf onto the dst head's
		 * DUPLICATE CHAIN.  That append walks a LIVE chain, so it
		 * runs under the chain holder's node lock.  @cc91bd8b added
		 * this exclusion when it closed the last unlocked chain
		 * mutation.
		 *
		 * ft_merge_spine_copy takes the lock before its own point of
		 * no return.  It SKIPS it for a pre-reserved caller, whose
		 * placement cannot fail.  This fold is the case its comment
		 * anticipated: a single commit is still ahead of us.  So
		 * failing here is free and the lock is ours to take.
		 *
		 * This is a no-op on a collision-free merge, which is every
		 * disjoint union.
		 */
		if (ft_glue_acquire_splice_holders(ft, &glue)) {
			ret = -EAGAIN;
			goto bail_build;
		}
		attached_nf = merged_pub;
		adepth = (unsigned int) dst_len;
		prep = FT_GRAFT_PREP_NOSPLIT;	/* not a graft; keeps the arms below off */
#endif /* FEATURE_FT_MERGE */
	} else
	{
		/*
		 * The op's outstanding marks are ft_rekey_cow_stop's @stop
		 * fence and one per COW'd child.  They reach no registry until
		 * the sweep below.  So the build's own acquires can only see
		 * them through this frame.  The store-prepare arm passes the
		 * same frame.
		 *
		 * This is an IN-TRIE move: src and dst share a root.  Under a
		 * coarse spacing those marks and the split-CN fence are ONE
		 * word.  Without the frame the build refuses the op's own
		 * fence.  The caller then re-descends onto the identical
		 * shape forever.
		 */
		struct ft_lock_ctx bctx;

		ft_lock_ctx_init(&bctx, &d_src, txn, optxn);
		bctx.held.extra = marks;
		bctx.held.nr_extra = nr_marks;
		/*
		 * THE PAYLOAD IS THE LIVE HEAD, not a COW copy.  So the
		 * NOSPLIT arm of ft_store_at_graft_point must RECORD its back
		 * edge.  It must not plain-store it ahead of the publish.
		 * ☞ @payload_live.
		 * The BRANCH arm needs nothing: there the payload is the branch's
		 * leaf and ft_build_branch already defers a live leaf in glue mode.
		 *
		 * ☞ THE LEG IS FOUR PIECES AND THIS IS ONE:
		 *   - the cow_stop skip;
		 *   - the app-owned reclaim skip;
		 *   - ft_rekey_free_stop_prime's external guard;
		 *   - this assignment.
		 * They only work together.
		 */
		glue.payload_live = s_top_external;
		/*
		 * ARM THE OLD-DIRECTION DROP.  The case: the dst key diverges
		 * inside the very run S_top hangs off.  That run's one child
		 * IS S_top, the subtree @s_top_prime is the copy of.  So the
		 * split has no old half to keep and the move owes no detach.
		 * Naming the node is the whole condition: a run has exactly
		 * one child, so BP == the split node already says which edge
		 * goes.
		 *
		 * Set unconditionally.  The build only ever splits a
		 * compressed node, so a plain BP can never match and needs no
		 * test here.
		 */
		glue.drop_old_dir_of = src_cut ? d_src.nf : d_src.pnf;
		/*
		 * ARM THE OLD-DIRECTION REPLACE.  It is the drop's sibling,
		 * for a run whose child KEEPS siblings.  Like the drop:
		 *   - it is armed unconditionally;
		 *   - it is consumed only if the build actually splits that
		 *     very child's parent.
		 * Unlike the drop it names a slot as well as a node.  The old
		 * half survives and must be born without exactly one arm.
		 * ft_split_compressed_graft_build does the recompaction
		 * itself, at the one point where it knows it is splitting.
		 * See @old_dir_replace.
		 *
		 * ☑ TWO SHAPES, AND THE SPLIT PICKS BETWEEN THEM.
		 *   - Three or more children, or keys of its own: BP survives
		 *     the drop as a DEL-recompacted copy.
		 *   - Exactly two children and no keys: BP does not survive
		 *     at all.  The copy would be a one-child internal, which
		 *     is not a legal shape in skip mode.  So the split
		 *     COLLAPSES the old direction into one fused run and
		 *     retires BP whole.
		 * Both are armed by the same @nr_child >= 2.  The split reads
		 * BP itself and chooses.  Only it knows the suffix length the
		 * fused run has to fit in.
		 *
		 * The remaining terms keep the fresh-body seam the whole
		 * design rests on:
		 *   - a CUT source has no BP to recompact;
		 *   - the in-place tier can reserve a slot WITHOUT
		 *     relocating.  That would edit BP where it stands, under
		 *     the node this flip retires.
		 */
		if (d_src.pnf && d_src.nfp && !ft_in_place_excl_ok(ft) &&
				ft_node_internal(d_src.pnf) &&
				!ft_node_compressed(d_src.pnf) &&
				!ft_node_skip_compressed(d_src.pnf)) {
			struct cds_ft_metadata *bpm =
				ft_flag_to_metadata(ft, d_src.pnf);
			unsigned int nc = ft_meta_nr_child(bpm);

			/*
			 * ☑ ...AND THE ONE-CHILD-BUT-KEYED SHAPE, which is the
			 * old direction's PROMOTE.  The drop empties BP of
			 * CHILDREN and leaves its co-located key chain.  So
			 * the old half is that HEAD and BP goes away.
			 * ft_detach_node gives the same answer for a node
			 * the drop leaves childless but keyed.  The rekey
			 * fold's own FT_REKEY_FOLD_PROMOTE writes the same
			 * answer into @graft_c's slot.
			 * The split reads BP and picks between the three arms;
			 * this only says the shape is expressible.
			 */
			if (nc == 1 && bpm->external_nodes) {
				glue.old_dir_replace.of = d_src.pnf;
				glue.old_dir_replace.drop_slot = d_src.nfp;
				glue.old_dir_replace.drop_expected = d_src.nf_raw;
				glue.old_dir_replace.drop_child = d_src.nf_raw;
				glue.old_dir_replace.depth = d_src.pdepth;
			} else if (nc >= 2) {
				glue.old_dir_replace.of = d_src.pnf;
				glue.old_dir_replace.drop_slot = d_src.nfp;
				glue.old_dir_replace.drop_expected = d_src.nf_raw;
				/*
				 * ☠ THE RAW SLOT WORD, NOT THE RESOLVED FLAG.
				 * ft_node_find_child matches what the slot HOLDS
				 * (`iter == child_nf`).  On a compressed
				 * build, a skip-encoded slot holds a word
				 * that the resolve turns into something
				 * else.  So passing @d_src.nf finds nothing.
				 * The split then answers -EAGAIN on a shape
				 * no re-descent can change.
				 * The plain builds hid it: there the two are the
				 * same word.
				 */
				glue.old_dir_replace.drop_child = d_src.nf_raw;
				glue.old_dir_replace.depth = d_src.pdepth;
			}
		}
		/*
		 * ☑ ...AND THE SAME THREE ARMS ONE LEVEL UP, when the drop EMPTIES
		 * BP and the split's run holds the node the climb RESTS on.
		 *
		 * Everything above names BP (@d_src.pnf) as the child whose
		 * copy the split wires under its fresh suffix.  That is the
		 * whole shape only while BP SURVIVES the drop.
		 *
		 * When BP is one-child and keyless, the drop empties it and
		 * the detach's walk ELEVATES.  The node the split actually
		 * holds is then the one the walk comes to REST on: @cn_child
		 * is R, never BP.  So none of the three arms matches.  The
		 * collapse is left to publish at the split node's own home,
		 * which is the word the glue's forward publish repoints.
		 * Two plans, one word.  ft-remove.h's fused-collapse -EDOM
		 * is that collision.  The narrowed landing-parent guard
		 * beside it is the same family with a second exit.
		 *
		 * ☞ THE PRODUCT IS ALREADY BUILT.  The fresh cluster holds
		 * `[cn suffix] ++ [surviving ordinal] ++ [absorbed run]`.
		 * That is exactly what ft_split_compressed_graft_build's
		 * old-direction arms make for the UN-CLIMBED twin.  The two
		 * differ only in which node is named.  So this arms the same
		 * field on R and adds no builder.  The split picks COLLAPSE /
		 * REPLACE / PROMOTE by reading R, exactly as it does for BP.
		 *
		 * ☞ BP MAY BE A RUN HERE, and usually is.  The shape is "the
		 * drop EMPTIES BP".  A run is the one ancestor kind emptied
		 * BY CONSTRUCTION: exactly one child, never any
		 * external_nodes.  The arming above requires BP PLAIN because
		 * it writes a value into BP's own copy.  This one never
		 * touches BP: BP is orphaned with the rest of the cleared
		 * chain.  So it asks only that BP be a node the climb can
		 * walk.
		 *
		 * ☠ A SECOND, READ-ONLY CLIMB, and it feeds THIS ARMING ONLY.
		 * The gate's own climb runs after ft_graft_build, because it
		 * needs @graft_c, which the prep produces.
		 * ft_rekey_climb_reaches_graft's header records that walking
		 * runs on the GLUE lane refused a shape that is SERVED
		 * CORRECTLY.  The discriminator was @prep itself.
		 * Nothing here may reach the staleness terms.
		 */
		if (!src_cut && !ft_in_place_excl_ok(ft) && d_src.pnf && d_src.nfp &&
				(ft_node_internal(d_src.pnf) ||
					ft_node_compressed(d_src.pnf)) &&
				!ft_node_skip_compressed(d_src.pnf) &&
				!glue.old_dir_replace.of) {
			struct cds_ft_metadata *bpm2 =
				ft_flag_to_metadata(ft, d_src.pnf);

			if (ft_meta_nr_child(bpm2) == 1 && !bpm2->external_nodes) {
				bool reach2 = false;
				struct cds_ft_inode_flag *rest2 = NULL, *top2 = NULL;
				bool comp2 = false;
				unsigned int bytes2 = 0;
				int steps2 = ft_rekey_climb_reaches_graft(ft,
					d_src.pnf, NULL, &reach2, &rest2, &top2,
					&comp2, &bytes2, /*walk_runs=*/ true);

				if (steps2 >= 1 && rest2 && top2 && !reach2 &&
						ft_node_internal(rest2) &&
						!ft_node_compressed(rest2) &&
						!ft_node_skip_compressed(rest2) &&
						bytes2 < d_src.pdepth) {
					struct cds_ft_inode_flag *tp2 = NULL;
					struct cds_ft_inode_flag **dslot2 =
						ft_resolve_parent_slot(
							ft_flag_to_metadata(ft, top2),
							ft, &tp2);
					struct cds_ft_inode_flag *raw2 = dslot2 ?
						rcu_dereference(*dslot2) : NULL;

					/*
					 * ONE coherent (parent, slot) pair,
					 * and the RAW word it holds.  A run
					 * is spelled SKIP_X there.  The
					 * split's ft_node_find_child compares
					 * WORDS (☞ ft_rekey_slot_names).
					 */
					if (dslot2 && tp2 == rest2 &&
							ft_rekey_slot_names(raw2, top2)) {
						glue.old_dir_replace.of = rest2;
						glue.old_dir_replace.drop_slot = dslot2;
						glue.old_dir_replace.drop_expected = raw2;
						glue.old_dir_replace.drop_child = raw2;
						glue.old_dir_replace.depth =
							d_src.pdepth - bytes2;
						climb_split_steps = steps2;
						climb_split_top = top2;
						climb_split_rest = rest2;
					}
				}
			}
		}
		prep = ft_graft_build(ft, dst_ord, dst_len, s_top_prime, cnt,
			&d_dst, &glue, &bctx.held);
	}
	/*
	 * Non-buildable outcomes, mapped to the driver's contract.
	 *   - OOM or a lost split-fence race: the build has published
	 *     NOTHING.  On the RETRY arm it has not even marked the
	 *     compressed node.  Each is the clean transient the caller
	 *     re-descends on.
	 *   - An occupied graft point: the permanent shape error.  The
	 *     up-front dst probe already rejects it for the shapes it can
	 *     prove.  This is the same answer for the rest.
	 */
	if (prep == FT_GRAFT_PREP_OOM) {
		ret = -ENOMEM;
		goto bail_build;
	}
	if (prep == FT_GRAFT_PREP_RETRY) {
		ret = -EAGAIN;
		goto bail_build;
	}
	if (prep == FT_GRAFT_PREP_POPULATED) {
		/*
		 * Content at or below the graft point that the up-front
		 * probe could not prove: a compressed or skip-encoded
		 * occupant it declines to walk.
		 *   - For a GRAFT, that is the destination-occupied answer
		 *     its caller documents.
		 *   - For a MERGE, it is a dst shape this cut leaves to
		 *     ft_merge_spine_copy.  That is the definition of
		 *     UNCOVERED.
		 * The MERGE case used to answer with the ARGUMENT code, by
		 * the same slip as the dst descent above.  The sentence "a
		 * dst shape this cut leaves to ft_merge_spine_copy" was
		 * already there; only the code disagreed.
		 */
		ret = require_empty ? -EEXIST : FT_REKEY_UNCOVERED;
		goto bail_build;
	}
	/*
	 * A reanchoring descent step landed a live node SHALLOWER than the
	 * dispatched child.  So the captured publish chain names the wrong
	 * level.  The graft and insert take the same bail.  Nothing is
	 * reader-visible yet.
	 */
	if (caa_unlikely(d_dst.skip_conflict)) {
		ret = -EAGAIN;
		goto bail_build;
	}
	/*
	 * SHAPE GATE.  Two dst shapes are supported.  They differ only in
	 * WHICH TWO NODES the graft locks:
	 *
	 *  - NOSPLIT: an absent, append-in-place dst point.  The reserve
	 *    recompaction retires and RELOCATES the dst parent d_dst.pnf.
	 *    It holds its parent d_dst.ppnf and releases it at the flip.
	 *    An optional third member is the SKIP_X great-grandparent.
	 *    The same reserve recompaction takes it whenever d_dst.ppnf is
	 *    COMPRESSED (ft_store_at_graft_point_prepare's hint names it).
	 *    The dual it records there is announced as @pending_dual_slot
	 *    at the commit.  So a src detach that recompacts the
	 *    great-grandparent folds it.  ☞ The gate below no longer
	 *    refuses a compressed publish parent.
	 *  - GLUE: the dst key diverges INSIDE a compressed node, so the
	 *    build assembled the whole split cluster invisibly.  It holds
	 *    two nodes:
	 *      - the split node @cn, fenced by the build and retired by
	 *        our commit;
	 *      - @glue.publish_parent, fenced just below.  It is the live
	 *        node whose slot the forward publish replaces.
	 *    A COMPRESSED publish_parent cannot arise here.  @cn is itself
	 *    compressed and the trie admits no two adjacent compressed
	 *    nodes, so the split node's parent is plain.  The MERGE arm's
	 *    publish parent CAN be compressed.  That arm holds the
	 *    grandparent (@publish_gp_holder, taken with the publish
	 *    parent above).  It announces the dual through
	 *    ft_glue_txn_commit_edges.
	 *
	 * Against that pair, the src junction BP (= d_src.pnf) and its parent:
	 *   - BP's parent IS the graft-held node: REUSE the held lock
	 *     (@src_parent_held).  This is the shape the hook started with.
	 *   - BP's parent is any other node: the detach ACQUIRES it itself,
	 *     guarded (@parent_guard).  This is deadlock-free: both
	 *     acquires are try-locks that abort rather than block.
	 *   - BP IS the graft's PUBLISH PARENT: supported.  The detach
	 *     DEL-recompacts that node.  That would strand the forward
	 *     publish in the superseded copy.  So the op's pending publish
	 *     rides the recompaction instead (ft_flip_txn
	 *     @pending_pub_slot).  The replaced child and the shrunk
	 *     parent go live in ONE flip.
	 *   - BP, or BP's parent, IS the node the graft RETIRES: rejected
	 *     here, PERMANENTLY.  The detach would edit the copy the flip
	 *     retires, and no fallback writer expresses that shape.
	 *     (Cross-depth aliases are rejected rather than asserted so
	 *     that a relaxation of the length rule cannot silently reach
	 *     them.)
	 *
	 * For both, the junctions must also be BELOW the root: d_src.ppnf
	 * and the graft's own publish target are non-NULL.  A root-level
	 * junction republishes into &ft->root, a slot with no node word to
	 * lock.  So its SW park would be an unguarded plain store.
	 */
	if (merge_dst) {
		/*
		 * The merge's two nodes play the same roles as the graft's
		 * pair:
		 *   - @D is the one it RETIRES, through the fenced overlap
		 *     terminal;
		 *   - the publish parent is the one it RELEASES at the flip.
		 * Both are already held: D by the overlap fence, the parent
		 * by the mark above.  So the gate below only has to keep the
		 * detach's junctions clear of them.
		 */
		graft_c = d_dst.nf;
		graft_p = d_dst.pnf;
	} else if (prep == FT_GRAFT_PREP_GLUE) {
		cn_flag = d_dst.nf;		/* the compressed node the build split */
		graft_c = cn_flag;
		graft_p = glue.publish_parent;
	} else if (d_dst.nf && d_dst.depth != dst_len &&
			ft_node_external(d_dst.nf)) {
		/*
		 * THE DISPLACED EXTERNAL, and it is a PAIR ONE LEVEL DOWN from
		 * the arm below.  The slot at @d_dst.depth holds an EXTERNAL
		 * LEAF: a key ending on the destination path.  The attach
		 * DISPLACES it into the fresh branch's metadata
		 * (ft_store_at_graft_point_prepare's displaced arm).  That arm
		 * publishes the branch INTO @d_dst.pnf's existing slot.  The
		 * slot is already occupied.  So nothing is resized, nothing
		 * is recompacted, and @d_dst.ppnf is never touched.
		 *
		 * ☠ WHICH IS WHY THE PAIR MUST SHIFT.  @graft_p is not
		 * decoration.  @src_parent_held below is DERIVED from it
		 * (`d_src.ppnf == graft_p`).  It tells the src detach: "your
		 * recompaction's parent is already LOCK-held by an earlier
		 * step of this op, so do not acquire it".  Name @d_dst.ppnf
		 * here and that claim is FALSE for this arm: no step of the
		 * op ever takes it.  The detach's recompaction then
		 * republishes into a node it does not hold.  That is an SW
		 * park on an unowned word.
		 *
		 * MEASURED with @d_dst.ppnf (the arm below's pair) and this
		 * shape served.  The case is {aaca, aaacb, bc, baabaa, b,
		 * bbbb} + rekey_merge(dst "bcccaa", src "bbb").
		 * --enable-rcu-debug ABORTS at __ft_flip_txn_record_tag_ctx's
		 * owner assert.  The detach republishes the recompacted
		 * depth-1 node into the ROOT node's slot, while the op holds
		 * only {that node, the src top}.  A release build is SILENT:
		 * the park is a plain store into a live word, harmless
		 * single-threaded.  That is how a release-only measurement
		 * once read this shape as served.
		 *
		 * @graft_c is the EXTERNAL, and it is not a retire.  The leaf
		 * stays LIVE, re-parented under the fresh branch by a
		 * recorded edge.  It is named because the gates below ask
		 * whether the detach's junctions ARE the node the graft
		 * supersedes at that slot.  An external answers no to all of
		 * them: @d_src.pnf and @d_src.ppnf are internal nodes, and
		 * nothing hangs under a leaf.  This arm CAN meet one junction
		 * alias: BP == the graft's publish parent.  It is the
		 * supported one.  The pending publish rides the detach's
		 * recompaction (@pending_pub_slot, announced by hand in
		 * ft-graft.h's displaced arm).
		 *
		 * ☞ MEASURED, 4 modes x 3000 generated shapes, on the
		 * --enable-rcu-debug default and nocompress builds and on
		 * release:
		 *   - rank off: 210 more calls commit (2387 -> 2597);
		 *   - rank on: 155 more calls commit (1844 -> 1999).
		 * Every failure count is UNCHANGED against the same corpus
		 * with the shape refused: 0 at rank off, 6 / 106 at rank on.
		 * The latter is the rank lane's standing residue.
		 * ☞ [[project_ft_rekey_displaced_external_refusal]]
		 */
		graft_c = d_dst.nf;
		graft_p = d_dst.pnf;
	} else {
		graft_c = d_dst.pnf;
		graft_p = d_dst.ppnf;
		/*
		 * A SHORT landing is IN SCOPE.  The descent stopped above
		 * @dst_len on an empty slot.
		 * ft_store_at_graft_point_prepare's other arm already builds
		 * the intermediate path down to the key and reserves the slot
		 * for it.  It uses the same recompaction, the same parent
		 * hint and the same fold guard as the exact arm.  The two
		 * differ only in WHAT goes in the slot: the payload itself,
		 * or a fresh branch carrying it.  Neither changes which nodes
		 * this move locks, so @graft_c / @graft_p keep their meaning.
		 *
		 * @d_dst.nf REACHING HERE now means one of two things.  The
		 * common one, an EXTERNAL leaf above @dst_len, is served by
		 * the arm above rather than refused.  What is left:
		 *   AT @dst_len the point is OCCUPIED: the caller asked to move a
		 *   subtree onto a position that already holds one.  An argument
		 *   error, terminal, and the one reading here that no state of the
		 *   trie makes legal.  It fires ZERO times over 4 modes x 3000
		 *   generated shapes -- kept for the contract, not for a witness.
		 *   ABOVE @dst_len and NOT external: a descent that stopped short on
		 *   an internal node.  Also unwitnessed in that corpus, so it owes
		 *   UNCOVERED rather than an argument error -- nothing here has
		 *   established it is the caller's mistake.
		 */
		if (d_dst.nf) {
			ret = d_dst.depth == dst_len ?
				-EINVAL : FT_REKEY_UNCOVERED;
			goto bail_build;
		}
	}
	src_parent_held = d_src.ppnf == graft_p;
	/*
	 * BP IS THE NODE THE NOSPLIT GRAFT RECOMPACTS.  So the move is a slot
	 * RENAME inside ONE node.  BP is superseded exactly once: its copy is
	 * born holding the grafted child and no longer holding S_top.  The
	 * detach below is skipped entirely.  This is the arming condition of
	 * @pending_del_slot further down.  It is hoisted HERE because the
	 * root-junction gate needs the same answer.  With one expression,
	 * the two cannot drift.
	 *
	 * @merge_dst is load-bearing and is NOT redundant with @prep.  The
	 * merge arm sets prep = FT_GRAFT_PREP_NOSPLIT deliberately ("not a
	 * graft; keeps the arms below off").  So testing @prep alone would
	 * arm the fold on a merge.
	 */
	del_folds_into_graft = !merge_dst &&
		prep == FT_GRAFT_PREP_NOSPLIT &&
		d_src.pnf == graft_c;
	/*
	 * ...and may that folded shape publish into &ft->root?  Only where ARMED
	 * also means FOLDED.
	 *
	 * ☠ ft_node_set_nth_rec HAS AN IN-PLACE ARM THAT NEVER RUNS THE COPY
	 * LOOP.  That loop is what consumes the drop and sets
	 * @pending_del_folded.  Take an EXCLUSIVE trie built with
	 * FEATURE_FT_INSERT_IN_PLACE.  There the graft can reserve BP's slot
	 * without relocating BP.  The drop is then never consumed, and the
	 * detach runs after all.
	 *
	 * Refusing that at PLAN TIME is the point.  The detach notices too
	 * late.  By then the in-place reserve has already changed the LIVE
	 * node.  It has set BP's bitmap bit and appended a NULL pointer.  No
	 * bail path takes those back.  So a late refusal would leave a
	 * permanent reserved hole, instead of a trie byte-for-byte as it
	 * was.  The term costs nothing on a shared trie, where the bulk
	 * reserve's ft_in_place_excl_ok is false.
	 *
	 * It gates ONLY the root admission, never @del_folds_into_graft
	 * itself.  Disarming the fold would send the detach at a copy the
	 * prepare retires.  That is the very bug the arming exists to
	 * prevent.
	 */
	/*
	 * THE CLIMB, HOISTED ABOVE THE ROOT GATE because the arming below feeds
	 * @root_pub_ok.  110's shape is this fold AT THE ROOT.  Deriving it
	 * after the gate would refuse the very case the arming exists to serve.
	 * Pure plan-time reads; nothing here mutates.
	 *
	 * ☞ ONLY WHEN THE DETACH ACTUALLY RUNS.  @del_folds_into_graft means BP
	 * IS the node the NOSPLIT graft recompacts.  So the drop is a slot
	 * rename inside one node and the detach is SKIPPED.  There is no
	 * climb to model.  Modelling one refuses the shapes that
	 * test_rekey_cut_run_dst_split_keeps_keys pins as SERVED.
	 */
	climb_steps = del_folds_into_graft ? 0 :
		ft_rekey_climb_reaches_graft(ft, d_src.pnf, graft_c,
			&climb_reaches_graft, &climb_rest, &climb_top,
			&climb_compressed, &climb_bytes,
			/*walk_runs=*/ prep == FT_GRAFT_PREP_NOSPLIT);
	/*
	 * ARM THE FOLD FROM THE CLIMB, not from a fixed depth.
	 *
	 * @del_folds_into_graft is the HEIGHT-0 instance of this.  BP itself
	 * is the node the graft recompacts.  So the slot the detach would
	 * clear is already one of the slots the copy loop enumerates.  Every
	 * step the climb takes moves that slot one level UP.  The copy loop
	 * consumes @pending_del_slot BY ADDRESS, so it does not care which
	 * node the slot lives in.  When the climb ARRIVES AT @graft_c, the
	 * slot to drop is the one @graft_c holds @climb_top in.  The whole
	 * move is again ONE superseded node: @graft_c born without the src
	 * direction and with the destination.  The chain the drop empties is
	 * frozen and reclaimed by ft_rekey_fold_freeze_orphans below, exactly
	 * as an elevating detach's is.
	 *
	 * WHAT THIS DOES NOT SERVE, and why each is refused rather than
	 * approximated:
	 *   - BP KEEPS A CHILD OR A KEY (the climb takes no step, or rests below
	 *     @graft_c).  BP then SURVIVES the drop and owes a recompaction
	 *     of its own.  Its republish lands in @graft_c: a second edit
	 *     against a node this flip retires.  The climb's own arithmetic
	 *     is what says so.
	 *   - THE IN-PLACE TIER.  ft_node_set_nth_rec's in-place arm never runs
	 *     the copy loop.  So the drop would never be consumed, and the
	 *     detach would run after all, into the superseded body.  Refused
	 *     at PLAN time, before the reserve sets a bit no bail takes
	 *     back.  @root_pub_ok already carries the same reasoning.
	 *   - A CUT SOURCE.  There the src slot holds the RUN and S_top is a
	 *     COW'd tail.  So "BP is emptied by the drop" is not the shape
	 *     at all.
	 *   - A SKIP-ENCODED @climb_top.  Never produced: the climb reads flags out
	 *     of parent_words, which name the node itself.
	 *
	 * ☑ A COMPRESSED LINK ON THE CHAIN IS NO LONGER REFUSED.  The reason
	 * it was refused does not survive being written down.  It said that
	 * a run "puts a SECOND path (the SKIP_X dual) on the same child, and
	 * refreshing that dual is a second publish this cut has no owner
	 * for".  Nothing here REFRESHES a dual.  The slot that spells the run
	 * is DROPPED from a fresh @graft_c.  The run, its child word and that
	 * spelling all die with the retired chain.  A dual is owed only where
	 * the compressed node SURVIVES and its child is republished.  That is
	 * the compressed publish parent.  It is a different mechanism, served
	 * at @088ec182 by ANNOUNCING the reserve's dual.
	 * The climb has walked runs since @4c8ac7fe.  Its own header says a
	 * run "is the only ancestor kind that is GUARANTEED to be emptied by
	 * the drop".  Only the arming would not use the answer.  MEASURED:
	 * 127 of the 3000 generated shapes, 83 on this arm and 44 on the one
	 * below.
	 *
	 * The (parent, slot) pair comes from ONE coherent resolve.  So a peer
	 * re-home mid-read cannot pair a fresh offset with a stale parent.
	 * The slot is required to still NAME @climb_top, by the node it
	 * DENOTES, so a run's SKIP_X spelling passes.  The RAW word is what
	 * the drop carries as its expected-old (ft_rekey_slot_names).
	 *
	 * ☞ FOUR OF THESE TERMS CANNOT FIRE TODAY, and they are kept on purpose.
	 *   - @climb_top non-NULL is implied by `climb_steps >= 1`.
	 *   - Its node-kind tests are implied by the climb's own loop
	 *     condition: it walks internal flags only, and a compressed flag
	 *     has FT_INTERNAL_MASK clear.
	 *   - `climb_steps <= @d_src.pdepth` is implied by a climb that rests
	 *     on a live node.
	 * Two of them guard code below: the metadata deref here, and the
	 * walk's depth arithmetic.  So they are NULL/range guards rather than
	 * shape tests.  The other two say the same thing the freeze walk says
	 * one step later.  Saying it HERE is strictly better: at the gate
	 * nothing is built yet, while the walk's refusal has to unwind a
	 * graft.  None of them is a place to hang new meaning: the walk is
	 * where the chain's shape is decided.
	 */
	if (!del_folds_into_graft && climb_top && graft_c &&
			climb_reaches_graft && climb_rest == graft_c &&
			climb_steps >= 1 &&
			ft_rekey_fold_shape_ok(ft, graft_c, climb_top, prep,
				merge_dst, src_cut,
				true /*@climb_top is DROPPED, not edited*/,
				false /*a cut is not this arm's shape*/) &&
			climb_bytes <= d_src.pdepth) {
		struct cds_ft_inode_flag *top_parent = NULL;
		struct cds_ft_inode_flag **slot = ft_resolve_parent_slot(
			ft_flag_to_metadata(ft, climb_top), ft, &top_parent);
		struct cds_ft_inode_flag *raw = slot ?
			rcu_dereference(*slot) : NULL;

		if (slot && top_parent == graft_c &&
				ft_rekey_slot_names(raw, climb_top)) {
			bp_folds_into_graft_c = true;
			fold_mode = FT_REKEY_FOLD_DROP;
			fold_top_slot = slot;
			fold_top_expected = raw;
			fold_bp_depth = d_src.pdepth;
		}
	}
	/*
	 * ...AND THE SECOND MODE, on the SAME slot.  The walk comes to REST
	 * on a node the drop does NOT empty.  That node is a child of
	 * @graft_c.
	 *
	 * The detach would then edit the RESTING node and republish it into
	 * @graft_c.  That is the same second edit against a superseded body
	 * that the DROP mode above exists to remove.  It arrives one level
	 * shallower.  The fold absorbs it identically.  @graft_c's copy is
	 * born holding, in that one slot, whatever the detach would have
	 * published there:
	 *   - DROP writes NOTHING in the slot (@pending_del_slot);
	 *   - PROMOTE writes the resting node's EXTERNAL CHAIN HEAD
	 *     (@pending_pub_slot).  That is exactly what ft_detach_node does
	 *     for a node the drop leaves childless but KEYED.
	 *
	 * Only the childless-but-keyed shape here.  A resting node that keeps a
	 * CHILD needs a DEL-recompacted copy of itself as the published value.
	 * Building one is a separate piece of work.  So that shape stays
	 * refused, by the same @climb_rest_under_graft term as before.
	 *
	 * @climb_rest is BP itself when the climb never moved (@climb_steps ==
	 * 0).  It is the topmost cleared node's parent when it did.  One
	 * expression handles both, because the walk reports where it STOPPED,
	 * not how far it got.
	 */
	/*
	 * ☑ AND A COMPRESSED LINK ON THE CLEARED CHAIN IS ADMITTED HERE TOO.
	 * The DROP arm's reasoning above applies.  The run is unlinked from
	 * the RESTING node's fresh copy and dies with it.
	 * @climb_rest itself stays PLAIN.
	 * The fold WRITES a value into its slot.  A value-writing fold into a
	 * compressed node is the dual-refresh this cut has no owner for.  44
	 * of the 3000 generated shapes.
	 */
	if (!del_folds_into_graft && !bp_folds_into_graft_c && graft_c &&
			climb_rest && !climb_reaches_graft &&
			ft_rekey_fold_shape_ok(ft, graft_c, climb_rest, prep,
				merge_dst, src_cut,
				false /*@climb_rest is EDITED*/,
				climb_steps == 0 /*BP itself: see @cut_ok*/) &&
			climb_bytes < d_src.pdepth) {
		struct cds_ft_metadata *rm = cds_ft_item_to_metadata(
			ft_node_ptr(climb_rest));
		struct cds_ft_inode_flag *rest_parent = NULL;
		struct cds_ft_inode_flag **slot;

		slot = ft_resolve_parent_slot(rm, ft, &rest_parent);
		if (slot && rest_parent == graft_c &&
				rcu_dereference(*slot) == climb_rest &&
				ft_meta_nr_child(rm) >= 2) {
			/*
			 * REPLACE: the resting node SURVIVES the drop, so the
			 * value the slot takes is a DEL-recompacted copy of it.
			 * Built below, before the graft prepare, because the copy
			 * loop needs it in hand.
			 */
			struct cds_ft_inode_flag **dslot = d_src.nfp;
			struct cds_ft_inode_flag *dchild = d_src.nf;
			struct cds_ft_inode_flag *dexp = d_src.nf_raw;

			if (climb_steps >= 1) {
				struct cds_ft_inode_flag *tp = NULL;
				struct cds_ft_inode_flag *raw;

				dchild = climb_top;
				dslot = climb_top ? ft_resolve_parent_slot(
					ft_flag_to_metadata(ft, climb_top),
					ft, &tp) : NULL;
				raw = dslot ? rcu_dereference(*dslot) : NULL;
				/*
				 * ☠ THE RAW WORD, exactly as the @climb_steps == 0
				 * leg one line up takes @d_src.nf_raw, and
				 * for the same reason.  ft_node_find_child
				 * and ft_node_other_child below compare
				 * WORDS.  A compressed @climb_top is spelled
				 * SKIP_X in this slot.  Handed the resolved
				 * flag, the two can never match.  Then
				 * "stale plan: re-descend" never clears and
				 * the op spins for ever.  This is the
				 * measured livelock at
				 * ft_rekey_fold_shape_ok's header.
				 */
				if (!ft_rekey_slot_names(raw, climb_top) ||
						tp != climb_rest)
					dslot = NULL;	/* stale: leave unarmed */
				else
					dexp = raw;
			}
			/*
			 * COLLAPSE instead of REPLACE for the two-child keyless
			 * shape.  The survivor is captured RAW: a
			 * skip-encoded form must survive the round trip.
			 * ft_node_other_child's own header states the same
			 * reading, and @fold_drop_expected already carries it.
			 *
			 * An over-long fused run leaves the arm UNARMED rather
			 * than refusing here.  A refusal inside the build
			 * could only answer -EAGAIN on a DETERMINISTIC shape,
			 * so the op would re-plan onto it for ever.  Unarmed,
			 * the caller's own shape gate answers it terminally.
			 */
			if (dslot && ft_rekey_collapse_shape(ft, rm)) {
				struct cds_ft_compressed_node *scn = NULL;
				struct cds_ft_inode_flag *surv = NULL;
				uint8_t sb = 0;

				if (!ft_node_other_child(climb_rest, dexp,
						&sb, &surv)) {
					dslot = NULL;	/* stale: leave unarmed */
				} else {
					if (ft_node_skip_compressed(surv))
						scn = ft_skip_to_compressed(ft, surv);
					else if (ft_node_compressed(surv))
						scn = ft_compressed_node_ptr(surv);
					if (1U + (scn ? (unsigned int) scn->len : 0U) >
							FT_SKIP_LEN_MAX) {
						dslot = NULL;	/* unarmed */
					} else {
						fold_surv = surv;
						fold_surv_byte = sb;
					}
				}
			}
			if (dslot) {
				bp_folds_into_graft_c = true;
				fold_mode = fold_surv ? FT_REKEY_FOLD_COLLAPSE :
					FT_REKEY_FOLD_REPLACE;
				fold_top_slot = slot;
				fold_bp_depth = d_src.pdepth;
				fold_rest_depth = d_src.pdepth - climb_bytes;
				fold_drop_child = dchild;
				fold_drop_slot = dslot;
				fold_drop_expected = dexp;
			}
		} else if (slot && rest_parent == graft_c &&
				rcu_dereference(*slot) == climb_rest &&
				ft_meta_nr_child(rm) == 1 && rm->external_nodes) {
			struct cds_ft_node *head =
				ft_dereference_external(rm->external_nodes);

			/*
			 * The head IS the slot value: an external carries tag
			 * 0, so ft_node_external() reads the cast pointer.
			 * Its own back edge rides the copy loop's re-parent
			 * sweep.  That is the same arm that re-homes every
			 * other child of the fresh node.
			 */
			if (head) {
				bp_folds_into_graft_c = true;
				fold_mode = FT_REKEY_FOLD_PROMOTE;
				fold_top_slot = slot;
				fold_bp_depth = d_src.pdepth;
				fold_promote_val =
					(struct cds_ft_inode_flag *) head;
			}
		}
	}
	/*
	 * ☑ AND THE UNFOLDED ROOT PUBLISH, since the third disjunct.  The gate
	 * used to admit @graft_p == NULL only for a move whose src edit FOLDS
	 * into the graft's own copy.  The reasoning was that "a root-level
	 * junction republishes into &ft->root, a slot with no node word to lock,
	 * so its SW park would be an unguarded plain store".
	 *
	 * ☠ THAT CONFLATES TWO WRITES.
	 *   - The write to &ft->root is the GRAFT's forward publish.  The
	 *     engine records it as a ROOT edge.  It is arbitrated by the
	 *     root-slot CAS and never SW-parked.  ft_pub_rec_add flags it.
	 *     FT_ROOT_ASSERT_NOT_ROOT aborts a debug build that routes a
	 *     root slot through the tag path instead.
	 *   - The write that needs a node word is the SRC DETACH's
	 *     republish.  That one lands in BP's own parent.  @d_src.ppnf
	 *     names it, and it is a real node whenever it is non-NULL.
	 * So the folded move is not the only safe shape.  An UNFOLDED move
	 * whose src junction hangs off a real node is equally safe.  It is
	 * the same @d_src.ppnf the detach arm below hands the detach as its
	 * parent hint.
	 *
	 * ☞ MEASURED, 3000 shapes x rank on/off x list on/off, on
	 * --enable-rcu-debug default / nocompress / noskip and on release:
	 *   - default and release: 2632 -> 2669;
	 *   - nocompress: 2938 -> 2988;
	 *   - noskip: 2649 -> 2695.
	 * ZERO failures on every leg.  On nocompress no compression debt
	 * hides behind this gate.  There the +50 is EXACTLY the set the
	 * root-junction refusal was holding.  With it ablated, all 50 landed
	 * on the detach arm's own @graft_p check one step later, and nothing
	 * else changed.
	 *
	 * CONCURRENTLY, under the documented-safe COARSE strategy: one mover
	 * alternates this very shape ("abc" <-> the root slot "q") against
	 * four perturbers on 2 cpus.  12/12 clean on the default build and
	 * 6/6 on nocompress and noskip.  300 and 150 moves completed.  (6/6
	 * on FINE too, which is out of contract and reported only as a note.)
	 */
	root_pub_ok = !ft_in_place_excl_ok(ft) &&
		(del_folds_into_graft || bp_folds_into_graft_c || d_src.ppnf);
	/*
	 * A ROOT-LEVEL JUNCTION is a SHAPE this cut declines, and it owes
	 * FT_REKEY_UNCOVERED rather than -EINVAL.  The two are not
	 * interchangeable:
	 *   - -EINVAL is TERMINAL ("no state of the trie would make this
	 *     call legal");
	 *   - UNCOVERED falls through to the worker that owns the entry
	 *     contract.  That is what turns a declined shape into the
	 *     NOT_SUPPORTED the public entry documents.
	 * The caller's arguments here are perfectly valid.  What is missing
	 * is a node word to park the republish under (&ft->root has none).
	 *
	 * ☠ AND IT IS NOW AN ORDINARY REQUEST.  While the writer required
	 * equal key lengths, a one-byte source could only pair with a
	 * one-byte destination.  So this arm was a corner.  With the lengths
	 * free, "move the subtree at a depth-1 key" is a normal call.
	 * Answering it INVALID ARGUMENT would be a wrong answer rather than
	 * a narrow one.
	 *
	 * ☑ NARROWED to the shape where the move is a slot RENAME inside the root
	 * itself.  The republish does land in &ft->root, and needs no word
	 * there.  _ft_publish_to_parent_meta marks a @parent_slot ==
	 * &ft->root edge ROOT-owned.  The engine records it MW, arbitrated
	 * by the root-slot CAS: concurrent root relocations collide on
	 * old == current_root.  So nothing SW-parks there.  And because the
	 * drop FOLDS, three steps are skipped: the detach, the
	 * chain-compress collapse and the count walk.  That leaves no SECOND
	 * edit that would need a word of its own.
	 *
	 * MEASURED under --enable-rcu-debug.  The folded root shape completes
	 * and cds_ft_verify is clean.  This holds across five geometries (cut
	 * run / plain BP, dst landing exact / short, root left one-child) and
	 * in both list and rank-stats modes.  The SAME src shape with the
	 * fold UNARMED and the dst still at the root ABORTS the engine's
	 * SW/MW kind check.  That same unarmed shape one level BELOW the root
	 * is green.  This attributes the abort to the root junction rather
	 * than to the collapse both of them run.
	 *
	 * ☐ ONE GUARD IS ABSENT HERE, KNOWINGLY.  With @graft_p NULL the
	 * prepare's parent_guard records nothing: there is no parent metadata
	 * to key it on.  The commit takes its expected-old from a FRESH read
	 * of the root slot.  That is the plan-then-ratify shape that
	 * ft_pub_rec_add's own header warns about.  TWO things exclude a peer
	 * from that window, and BOTH are load-bearing:
	 *   - the root-slot CAS itself, which a concurrent root relocation loses;
	 *   - BULK SERIALIZATION.  ☠ Do NOT write this off as "a root node
	 *     is only ever retired, never re-homed".  That is FALSE:
	 *     cds_ft_detach_tree re-homes the LIVE root with a plain
	 *     parent_word rewrite and no retire (ft-detach.h).  What keeps
	 *     it out is serialization.  detach_tree runs under
	 *     CDS_FT_SCOPED_BULK_GATE and this move under ft_move_gate_enter
	 *     (FT_BULK_COHERENT).  An active bulk window makes every writer
	 *     scope re-take the FT-wide mutex.
	 * Nothing here DETECTS a future producer that re-homes a root outside that
	 * serialization; it would have to be caught by review.
	 */
	if (!graft_c || (!graft_p && !root_pub_ok)) {
		ret = FT_REKEY_UNCOVERED;
		goto bail_build;
	}
	/*
	 * ☑ THE CUT-RUN / SPLIT-RUN SHAPES ARE NOW SERVED ON EVERY MODE, and the
	 * gate that used to refuse them here is gone.  The measurement is
	 * kept, because it names the mechanism and the fix is elsewhere:
	 *
	 *   insert "zwa", "zwabq", "zwabcd";  rekey_merge(dst "zwe", src "zwabc")
	 *
	 * never returned on a COARSE trie.  The sequence was:
	 *   - Cutting the run makes ft_rekey_cow_stop SW-park the displaced
	 *     child's state word.
	 *   - The commit re-parents that child through
	 *     ft_glue_apply_deferred.  Its record kind is picked by the
	 *     deferred edge's @held_lock.
	 *   - That flag was left FALSE on every mode but FINE non-exclusive,
	 *     because ft_glue_acquire_reparent_marks returned 0 before
	 *     setting it.
	 *   - Yet ft_dlm_acquire_set_at has no mode gate.  So the op really
	 *     did hold the word it was about to record MW.
	 *   - The engine saw an SW record and an MW record on ONE slot
	 *     (`expected_old=0x4 seen=0x80004`, the op losing against its
	 *     own LOCK bit).  The retry loop re-ran the identical sequence
	 *     forever.
	 *
	 * ☞ The cure is to DETECT the hold on every mode and only TAKE a fresh
	 * mark where a peer exists -- see ft_glue_acquire_reparent_marks.  The
	 * refusal was never a property of the trie.  The SAME shape under
	 * FINE committed cleanly all along.  That dated it as a missing arm
	 * rather than an unexpressible move.
	 */
	/*
	 * ☠ THESE TERMS ARE NOT ARGUMENT ERRORS, AND THEY USED TO ANSWER LIKE
	 * ONE.  They say the src junction IS the node the graft retires, which
	 * no fallback writer expresses.  So the answer must be TERMINAL (no
	 * fallback), and -EINVAL was reached for that property.  But -EINVAL
	 * is this file's ARGUMENT code.  It is documented above as "the
	 * caller got it wrong and NO STATE OF THE TRIE would make the call
	 * legal".  That is demonstrably false here.  With the same arguments,
	 * `rekey_merge(dst="zwe", src="zhe")` on {zhelloa,zhellob,zq,za}
	 * refuses on -DNO_FEATURE_FT_COMPRESS and SUCCEEDS on the default
	 * build.  Compression collapses the one-child chain, so the src
	 * junction sits one level below the boundary.  Without it the detach
	 * must elevate, and term 4 (d_src.ppnf == graft_c) fires.  The
	 * condition is a property of the SHAPE, not of the arguments.
	 *
	 * -ENOTSUP is the code that carries both halves:
	 *   - it is TERMINAL in the dispatcher exactly as -EINVAL was (no
	 *     fallback is attempted);
	 *   - it is surfaced as CDS_FT_STATUS_NOT_SUPPORTED, which is already
	 *     what the sibling shape at the root-junction gate answers.
	 * What is served is UNCHANGED; only the name of the refusal is.
	 *
	 * ☞ The old comment claimed "two DLM tests pin that code".  Checked,
	 * twice and independently:
	 *   - test_rekey_merge_colocated_chain_refused and
	 *     test_rekey_colocated_external pin the MERGE arm's co-located
	 *     -EINVAL;
	 *   - the only other INVALID_ARGUMENT rekey assertions are the
	 *     genuine argument ones (overlapping / prefix keys, and the
	 *     speculative-trie attribute gate on an EMPTY trie).
	 * Nothing pins these four terms.
	 *
	 * BP == the graft's publish PARENT is NOT among them:
	 * the recompaction fold carries that shape (see @pending_pub_slot).
	 *
	 * NEITHER IS BP == the graft CHILD.
	 *   - Under a NOSPLIT prep the graft ADD-recompacts BP itself.  The
	 *     detach's slot drop rides that same copy via @pending_del_slot.
	 *     So BP is superseded ONCE and its child count is never
	 *     transiently short.
	 *   - Under a GLUE prep BP IS the run the split retires.  There the
	 *     build DROPS ITS OLD DIRECTION (@drop_old_dir_of).  So there is
	 *     no second edit against that node left to order.
	 * The term therefore asks whether the build actually took that path,
	 * not which prep it was.  A GLUE split that KEPT both arms still owes
	 * a detach into a node this flip retires.  That is the shape no
	 * writer expresses.
	 */
	/*
	 * AN ELEVATING DETACH IS EXPRESSIBLE.  A one-child BP is emptied by
	 * the slot drop.  So the detach's climb does not stop on BP: it
	 * ELEVATES and recompacts BP's PARENT.  The hint below still names
	 * BP's own junction.  That is right only while the boundary IS BP.
	 * Past a climb, ft_detach_node RE-DERIVES it from the walk that
	 * moved, which is the only frame that can.  The orphan chain that
	 * climb clears rides @detach_rc and is freed on the far side of the
	 * one commit.
	 */
	/*
	 * ☠ THE ARRIVAL AT @graft_c HAS NO HEIGHT BOUND, and `d_src.ppnf ==
	 * graft_c` sees only the climb that gets there in ONE step.  Two steps up,
	 * the same arrival used to be admitted.  On a build where a one-child
	 * chain survives as plain interiors (-DNO_FEATURE_FT_COMPRESS) it
	 * DESTROYED THE TRIE:
	 *
	 *     cds_ft_insert(ft, "zwabcd", 6, n);
	 *     cds_ft_rekey_merge(ft, "zwe", 3, "zwabc", 5);   -> STATUS_OK
	 *
	 * returned SUCCESS and left the ROOT NODE EMPTY.  Traced (LTTng, whole
	 * run):
	 *   - the graft ADD-recompacts the junction and republishes it
	 *     holding BOTH the old child and the new dst slot;
	 *   - the detach then recompacts the ROOT and clears its only edge.
	 * The cause: the climb evaluated every ancestor against the
	 * SUPERSEDED originals.  It never saw the copy that holds the moved
	 * subtree.  The node the graft superseded is retired TWICE in the
	 * same commit.
	 *
	 * ☑ THAT WHOLE CLASS IS NOW SERVED, at every height, by
	 * @bp_folds_into_graft_c above.  The drop rides the graft's own copy
	 * of @graft_c and there is no second edit left to order.  What stays
	 * refused here is what the arming REFUSED to arm:
	 *   - an arrival whose BP survives the drop;
	 *   - a cut source;
	 *   - a compressed link;
	 *   - the in-place tier.
	 * For those the detach's republish would still land in a superseded
	 * body.  One expression decides both, so the two cannot drift.
	 */
	/*
	 * ☠ AND THE SAME STALENESS ONE LEVEL SHALLOWER.  The climb can come
	 * to REST on a node whose PARENT is @graft_c.  The detach then
	 * publishes its recompacted copy into @graft_c's OLD body, the one
	 * the graft has already superseded.  The live trie keeps the stale
	 * child and the ordered cell is updated to the fresh one.  ft_verify
	 * reports "cell {parent X} != expected {owner Y}".  Worse, the stale
	 * node the live trie still reaches through is RETIRED by the same
	 * commit.
	 *
	 * Only when the climb actually MOVED (@climb_steps >= 1).  At zero
	 * steps BP itself is the node being recompacted.  Its parent being
	 * @graft_c is then term 4's own shape, which is already answered
	 * above.
	 */
	if (!del_folds_into_graft && climb_steps >= 1 && climb_rest &&
			ft_node_internal(climb_rest))
		climb_rest_under_graft = ft_parent_node(rcu_dereference(
			cds_ft_item_to_metadata(ft_node_ptr(climb_rest))
				->parent_word)) == graft_c;
	/*
	 * ☑ A COMPRESSED PUBLISH PARENT IS NO LONGER A TERM HERE.  It refused 98
	 * of 3000 corpus shapes on the default build, and only 17 of them were
	 * ever wrong.  In each of the 17, the src detach DEL-recompacted the
	 * GRANDPARENT as a plain copy in the same commit.  The grandparent is
	 * the node holding the SKIP_X dual.  It was BP itself, or the node a
	 * one-step elevation lands on.  The NOSPLIT reserve recompaction
	 * already held that grandparent and recorded the dual.  What it never
	 * did was ANNOUNCE it.  So the detach's copy of the grandparent was
	 * born with the committed skip word, and cds_ft_verify reported
	 * "stale SKIP_X dual".  The announcement now lives in
	 * ft_store_at_graft_point_commit's relocated arm
	 * (@pending_dual_slot).  It mirrors the one ft_glue_txn_commit_edges
	 * makes for the GLUE and MERGE publishes.  The other 81 were refused
	 * for nothing.  In those, the grandparent was untouched.  Or the
	 * chain-compress collapse had fused it into a run built around the
	 * pending forward value.
	 */
	if (ft_node_skip_compressed(graft_p) ||
			(d_src.pnf == graft_c &&
				prep != FT_GRAFT_PREP_NOSPLIT &&
				!glue.old_dir_dropped) ||
			(!bp_folds_into_graft_c && !glue.old_dir_replace.done &&
				(d_src.ppnf == graft_c ||
				 (climb_reaches_graft && climb_steps >= 2) ||
				 climb_rest_under_graft))) {
		ret = -ENOTSUP;		/* shape, terminal -- see above */
		goto bail_build;
	}
	/*
	 * Reserve:
	 *   - the graft slot edge and the detach struct/state edges;
	 *   - with the list on, the six ordered-cell run boundary edges (src
	 *     unsplice + dst splice);
	 *   - for the GLUE shape, the split cluster's own bound: its
	 *     deferred back-edges, forward publish, split retire and
	 *     free-list tombstones.  ft_graft_keylen reserves its standalone
	 *     txn to the same floor;
	 *   - the edges a folded chain-compress collapse adds when the detach
	 *     takes BP down to one child.
	 * The count walk is depth-bounded and only exists when the trie keeps
	 * rank stats.
	 */
	if (!ft_flip_txn_reserve_extra(txn, FT_REMOVE_COMMIT_REC_MAX_EDGES + 4 +
			/*
			 * A COMPRESSED publish parent on the NOSPLIT arm costs
			 * the same TWO records the merge arm counts below, now
			 * that the shape gate admits it:
			 *   - the SKIP_X dual the reserve recompaction
			 *     re-encodes in the grandparent;
			 *   - the {LOCK|s -> s} release of that grandparent.
			 * Counted unconditionally, for the reason stated
			 * there.  A -DURCU_TXN_DEBUG_RESERVE build ran the
			 * 3000-shape corpus silent WITHOUT this term, so
			 * today's headroom absorbs it.  The term is the plan
			 * saying what it holds, rather than the slack doing
			 * so.
			 */
			2 +
			(ft->ordered_list ? FT_ORD_CELL_RUN_DETACH_MAX_EDGES +
				FT_ORD_CELL_RUN_RESPLICE_MAX_EDGES : 0) +
			/*
			 * Merged cluster.  The terms are:
			 *   - up to THREE records per deferred re-parent
			 *     (parent, state guard, offset), now that the fold
			 *     routes them through ft_reparent_record;
			 *   - one tombstone per retired overlap node on either
			 *     side;
			 *   - the forward publish;
			 *   - the depth-bounded count walk.
			 * Sized from the read-only pre-pass, with the same +8
			 * headroom the glue arrays get.
			 */
#ifdef FEATURE_FT_MERGE
			(merge_dst ? 3 * (unsigned int) (mcnt.nd + 8) +
				(unsigned int) (mcnt.nf_dst + mcnt.nf_src + 16) + 8 +
				2 * (unsigned int) (mcnt.ns + 8) +	/* dup-chain splices: forward link + the head's prev */
				/*
				 * A COMPRESSED publish parent -- the destination
				 * behind a path-compressed run -- costs TWO more
				 * records than a plain one:
				 *   - the SKIP_X dual the forward publish
				 *     re-encodes;
				 *   - the {LOCK|s -> s} release of the
				 *     grandparent that owns it.
				 * Counted unconditionally rather than off the
				 * shape.  The folded collapse's term states
				 * the reasoning: a reservation is where OOM
				 * gets ANSWERED.  By the time the shortfall
				 * shows up, the op is past the point where
				 * there is an answer.
				 */
				2 +
				(ft->rank_stats ? (unsigned int) dst_len + 1 : 0) : 0) +
#endif
			(prep == FT_GRAFT_PREP_GLUE ?
				FT_GLUE_FLOOR_DEFERRED + 7 + 1 + FT_GLUE_FLOOR_FREE +
				(ft->rank_stats ? (unsigned int) dst_len + 1 : 0) : 0) +
			/*
			 * ...and THE GLUE'S FREE-LIST TOMBSTONES on every OTHER
			 * arm, because @glue.fuse_free_list is set for all of
			 * them (see the init, and the livelock it closes).
			 * cap_free is the bound.  No other arm reaching here
			 * calls ft_glue_reserve.  The merge arm does, and its
			 * own term above counts mcnt.nf_dst + nf_src.  So
			 * cap_free is exactly FT_GLUE_FLOOR_FREE, which is
			 * also what ft_glue_defer_free asserts against.
			 *
			 * DOUBLED, because a FENCED entry costs two records
			 * where the spacing splits its terminal:
			 *   - the tombstone on the retired node;
			 *   - the {LOCK|s -> s} release of the anchor that
			 *     carried it.
			 * Under per-node the two words are one, so it costs
			 * one record.
			 *
			 * Counted unconditionally, on the GLUE term's own
			 * reasoning.  It overlaps that term by one floor.  A
			 * reservation is where OOM gets ANSWERED.  Once the
			 * publish has landed, the op is past the point where
			 * there is an answer.  A shortfall then surfaces as a
			 * sticky -ENOMEM at the commit instead.
			 */
			2 * FT_GLUE_FLOOR_FREE +
#ifdef FEATURE_FT_SKIP_COMPRESSED
			/*
			 * A FOLDED COLLAPSE.  Dropping S_top can leave BP with
			 * a single child.  ft_chain_compress_fused then
			 * records the whole chain-compress into THIS txn,
			 * instead of committing a second one:
			 *   - the merged forward publish;
			 *   - the surviving child's (parent, offset) back-edge
			 *     pair;
			 *   - the §4.B parent guard;
			 *   - the collapsed chain's freeze tombstones.
			 * This is its own standalone bound MINUS the
			 * FT_REMOVE_COMMIT_REC_MAX_EDGES already counted
			 * above.  The two shapes SHARE that budget, because
			 * exactly one of them runs (@boundary_fused).
			 *
			 * ☠ RESERVED UNCONDITIONALLY, never off the shape gate's nr_child
			 * read.  That read is a plan-time snapshot of a word
			 * peers commit into.  So BP can fall to the collapsing
			 * arity between the gate and the detach's own count.
			 * The collapse would then fire under a reservation
			 * sized for its absence.  A reservation is where OOM
			 * gets answered.  Once the detach has cleared BP's
			 * slot, the op is past the point where there is an
			 * answer.  The shortfall then surfaces as a sticky
			 * -ENOMEM at the commit instead.
			 *
			 */
			(ft_group_skip_compressed(ft->group) ?
				3 + 1 /* §4.B parent guard */
				+ 1 /* back-edge (parent, offset) pair */
				+ (ft->rank_stats ? (unsigned int) d_src.depth + 1 : 0) : 0) +
#endif
			/*
			 * AN ELEVATING DETACH's orphan chain.  A one-child BP
			 * is EMPTIED by the slot drop, and a compressed run is
			 * exactly that.  So the detach's upward walk clears it
			 * and every one-child ancestor above it.  Each cleared
			 * node freezes into THIS txn.  The climb stops at the
			 * first boundary and can climb no higher than the src
			 * depth.  That is what bounds the term.
			 *
			 * Not gated on BP's arity.  The collapse term above
			 * states the reason.  That read is a plan-time
			 * snapshot of a word peers commit into.  A reservation
			 * is where OOM gets ANSWERED.  Not gated
			 * on skip-compression either.  The walk elevates
			 * through plain nodes and through RUNS alike.  A
			 * compressed orphan costs the same one tombstone
			 * (ft_detach_freeze_one is kind-agnostic).  So the
			 * bound is the chain's LENGTH either way.
			 */
			ft_freeze_reserve(ft, (unsigned int) d_src.depth + 1) +
			0)) {
		ret = -ENOMEM;
		goto bail_build;
	}
	if (merge_dst) {
		/*
		 * Nothing more to prepare:
		 *   - the merged cluster is built;
		 *   - its publish is set;
		 *   - its publish parent was fenced with the descent (both
		 *     above).
		 * The single reserve is already drained by ft_merge_build's own
		 * allocations, so there is no graft reserve to activate here.
		 */
		cds_ft_alloc_reserve_drain(ft, &reserve);
	} else if (prep == FT_GRAFT_PREP_GLUE) {
		/*
		 * FENCE the publish parent OURSELVES, and fail the move on a miss.
		 * Otherwise ft_glue_txn_commit_edges would route an unheld
		 * parent through ft_flip_txn_lock_or_guard_parent.  That
		 * helper DEGRADES an acquire miss to a §4.B guard.  This is
		 * correct for the cross-trie graft and wrong here.  This txn
		 * is structural_sw, so the forward publish PARKS a plain
		 * store into that node's slot.  The fold's rule is SW iff the
		 * op holds the slot's lock.  A miss (retired / proxied /
		 * peer-locked) is the clean transient: nothing is published,
		 * and ft_meta_lock_acquire did not set the fence.  Mirrors
		 * ft_graft_keylen's own pre-swap fence.
		 */
		{
			struct ft_held_anchor pph;
			struct ft_lock_ctx ppctx;

			/*
			 * ☠ DATE THE PUBLISH PARENT FROM THE DESCENT WHOSE PATH IT
			 * IS ON, WHICH IS THE DST ONE.
			 *
			 * @glue.publish_parent is the node the built cluster
			 * publishes INTO.  So it sits on the DESTINATION path
			 * by construction.  It was measured here as exactly
			 * @d_dst.pnf at @d_dst.pdepth.  This acquire used a
			 * ctx built from @d_src, and the src descent's window
			 * does not describe it.  So ft_lock_ctx_depth_of
			 * returned FALSE and the bail below fired.  Nothing
			 * about that is transient.  The next attempt
			 * re-descends, re-derives the same publish parent, and
			 * asks the same wrong window again.
			 *
			 * MEASURED, and it is a HANG, not a slowdown.  ft_unit
			 * test_rekey_collapse_one_slot_two_kinds at
			 * CDS_FT_LOCK_SPACING_EXPONENTIAL spun 50,001 attempts
			 * on one move.  That is the cap of the
			 * -DFT_DEBUG_REKEY_RETRY_CAP detector.  The case was
			 * the BARE-HEAD spelling, rekey dst "bbccc" src "bcb",
			 * single-threaded with no peer in the test.
			 *
			 * ☞ INVISIBLE AT PER-NODE BY CONSTRUCTION, which is why it
			 * survived.  There ft_lock_ctx_depth_of returns a
			 * DUMMY depth and always succeeds: a member anchors on
			 * itself, so no descent is required.  Only the
			 * exponential schedule reads the depth at all.
			 *
			 * ☞ THE GLUE ALREADY CARRIES THE RIGHT DESCENT.  ft-graft.h
			 * hands it over at both build entry points for
			 * precisely this acquire.  It says: "the glue's own
			 * acquires -- its publish parent above all -- fire
			 * from commit helpers that never see @d, and an
			 * acquire with no depth under a coarse spacing MISSES
			 * ... That is a livelock, not a failure".  The merge
			 * arm repeats it at its own glue.lock_d.  So take the
			 * ctx FROM THE GLUE rather than re-deriving it from
			 * the wrong side.
			 *
			 * NO FALLBACK to the src descent on a miss.  Anchoring one
			 * node from two different paths is what §1's agreement rule
			 * forbids: two ops would compute different words for it and
			 * exclude nothing.  One descent, the node's own.
			 */
			ft_glue_lock_ctx(&glue, &ppctx);
			/*
			 * The REST of the op's held set, exactly as the
			 * store-prepare and detach arms name it:
			 *   - ft_rekey_cow_stop's marks.
			 *     ft_rekey_marks_to_txn hands them to the registry
			 *     right after the stop, and again after the dst
			 *     take;
			 *   - the split-CN fence, which the glue holds
			 *     (ft_glue_lock_ctx carries that one).
			 * Under a coarse spacing this publish parent anchors
			 * onto one of them: the trie root, for an in-trie
			 * move.  A frame naming neither refuses the op's own
			 * fence.
			 */
			ppctx.held.extra = marks;
			ppctx.held.nr_extra = nr_marks;
			pp_meta = ft_flag_to_metadata(ft, glue.publish_parent);
			unsigned int ppd;

			/*
			 * The publish parent came from the glue, not from a
			 * descent step.  So the window is what dates it.  A
			 * node this descent never passed voids the attempt.
			 */
			if (!ft_lock_ctx_depth_of(ft, &ppctx,
						glue.publish_parent, &ppd) ||
					ft_acquire_member(ft, &ppctx,
						glue.publish_parent, pp_meta,
						ppd, &pph)) {
				pp_meta = NULL;
				ret = -EAGAIN;
				goto bail_build;
			}
			/*
			 * A SHARED hit is the dedupe WORKING.  The fence is in
			 * force from an earlier acquire of this same op, so
			 * the publish may park its SW store.  The fold's rule
			 * is SW iff the op holds the slot's word, and it does.
			 * What it must NOT do is settle the word twice.  That
			 * is what @publish_parent_shared tells the commit.
			 */
			pp_meta = pph.lock;
			pp_snap = pph.lock_snap;
			pp_shared = pph.shared;
		}
		ft_glue_take_publish_parent(&glue, pp_meta, pp_snap, pp_shared);
		/*
		 * The one LIVE node the split cluster re-parents is @cn's
		 * displaced child.  It moves onto the fresh suffix, or
		 * straight under the fresh branch when there is no suffix.
		 * Its (parent, offset) pair parks SW.  The offset lives in
		 * the state word that ft_meta_nr_child_inc CASes from an
		 * insert BELOW it, which @cn's fence does not exclude.  So
		 * MARK it, exactly as ft_rekey_cow_stop marks the children
		 * whose state words it parks into.
		 *
		 * The mark is released by the re-parent's {live_state ->
		 * live_state} STATE edge, which is recorded unconditionally.
		 * It is NOT released by the pso edge.  Since @118245b0 the
		 * pso edge is its own word and is recorded only when the
		 * slot index CHANGES.  Resting the release on it leaks a
		 * permanent LOCK on every child that lands at the same index
		 * (see ft_rekey_cow_stop's release-attribution note).  Every
		 * bail path releases it through the @marks sweep.  An
		 * external child has no state word and no metadata, so there
		 * is nothing to mark and nothing to clobber.
		 */
		{
			struct cds_ft_compressed_node *cn =
				ft_compressed_node_ptr(cn_flag);
			struct cds_ft_metadata *cm =
				ft_child_state_meta(ft, cn->child);

			if (cm) {
				/*
				 * The DST descent dates this one, and @cn IS its
				 * cursor.  So the displaced child is the
				 * cursor's immediate child.  That is the
				 * below-cursor case the window cannot name
				 * (§7.1).  Dating it from the SRC descent
				 * asked the wrong path entirely.  That was
				 * inert only because per-node reads no depth
				 * at all.
				 */
				struct ft_lock_ctx dctx;
				unsigned int cd;

				ft_lock_ctx_init(&dctx, &d_dst, txn, optxn);
					/*
				 * The REST of the op's held set:
				 *   - the cow_stop marks, which reach no
				 *     registry until the sweep;
				 *   - the split-CN fence, which the glue
				 *     holds.
				 * Under a coarse spacing this child anchors
				 * onto one of them.  A frame naming neither
				 * refuses a word the op took two steps
				 * earlier.  @nr_extra is read at the acquire
				 * because this same array is still growing
				 * (the mark lands at @nr_marks).
				 */
				dctx.held.extra = marks;
				dctx.held.nr_extra = nr_marks;
				dctx.held.glue = &glue;
				if (!ft_lock_ctx_depth_of(ft, &dctx, cn->child,
						&cd)) {
					struct cds_ft_inode_flag *lp = NULL;

					(void) ft_resolve_parent_slot(cm, ft,
						&lp);
					if (!ft_lock_ctx_depth_of_cursor_child(
							ft, &dctx, lp, &cd)) {
						ret = -EAGAIN;
						goto bail_build;
					}
				}
				if (ft_acquire_member(ft, &dctx, cn->child, cm,
						cd, &marks[nr_marks])) {
					ret = -EAGAIN;
					goto bail_build;
				}
				nr_marks++;
				/* Same handover, for the mark that lands late. */
				ft_rekey_marks_to_txn(txn, marks, nr_marks);
				/*
				 * Tell the glue we already hold this one.  The split
				 * build DEFERS this same child, and
				 * ft_glue_acquire_reparent_marks marks every
				 * deferred entry.  Without this it would fail
				 * against our own fence and bail -EAGAIN on
				 * every attempt.  Read-only to the glue:
				 * @marks below stays its sole release.
				 *
				 * ☠ ONLY when the mark holds the CHILD'S OWN word.
				 * @caller_holder claims exactly that.
				 * ft_glue_acquire_reparent_marks' @held_lock
				 * reads it through ft_lock_ctx_holds, and
				 * @held_lock picks the re-parent's record
				 * KIND.  Under a coarse spacing this acquire
				 * lands on an ANCESTOR anchor and the child's
				 * own word stays unheld.  The law at
				 * ft_rekey_mark_holds_child_word then applies
				 * unchanged: a coarsened member keeps the MW
				 * guard.
				 *
				 * The blanket handoff overclaimed it.  On one
				 * fold, two records of this same child then
				 * disagreed:
				 *   - ft_rekey_cow_stop's re-parent recorded
				 *     MW (its own predicate is honest);
				 *   - the glue's deferred re-parent recorded
				 *     SW.
				 * ONE slot, TWO kinds: the contradiction the
				 * engine's record police refuses.  Measured
				 * on ft_unit 114 at root-only: the child's
				 * parent_word chain hits `r->kind == kind`,
				 * and release fail-safes it into an MW
				 * promotion nothing audited.
				 *
				 * The glue does not need the handoff for
				 * exclusion there.  The ANCHOR is registered
				 * on the shared fold txn
				 * (ft_rekey_marks_to_txn just above).  So its
				 * own acquire dedupes through the registry
				 * instead of self-refusing.  Per-node is
				 * byte-identical: the mark is on the child
				 * and the predicate is true.
				 */
				if (ft_rekey_mark_holds_child_word(
						&marks[nr_marks - 1], cm))
					glue.caller_holder = cm;
			}
		}
		/*
		 * The NOSPLIT path passes the moved subtree's key count
		 * through ft_store_at_graft_point_commit.  The GLUE publish
		 * takes it here.  ft_glue_txn_commit_edges records the +count
		 * walk from the stable publish parent into the same commit.
		 */
		/*
		 * NET ZERO when the old direction was dropped.  The subtree
		 * LEAVES the run and RE-ENTERS under the same publish
		 * parent's slot, so no ancestor's key count moves.  The
		 * fresh path already carries the moved count from build, and
		 * there is no detach to walk it back.
		 */
		glue.count_delta = (glue.old_dir_dropped ||
			glue.old_dir_replace.done) ? 0 : (long) cnt;
		cds_ft_alloc_reserve_drain(ft, &reserve);	/* GLUE builds its own cluster */
	} else {
		/*
		 * Drive prepare + commit SEPARATELY (not the combined
		 * ft_store_at_graft_point wrapper).  The reserve
		 * recompaction's relocated old dst-parent copy
		 * (@gst_st.old_recompacted_node) is then visible here.  The
		 * graft ALWAYS relocates the attach node for its atomic
		 * publish.  Under record_only its old copy stays LIVE until
		 * the caller's commit, so the caller frees it post-commit.
		 */
		/*
		 * ARM THE DETACH'S SLOT DROP ON THE GRAFT'S OWN COPY when BP is
		 * the node this graft recompacts.  Both halves of the decide then
		 * edit BP exactly once, in one copy: born holding the grafted
		 * child and no longer holding S_top.  Without it the detach
		 * below addresses the copy this prepare retires, and sizes
		 * its shape from it.  That copy's committed child count is
		 * short by the child being added here.
		 *
		 * The expected-old is the LIVE flag the src descent resolved.
		 * ft_rekey_cow_stop retires S_top's own state word and builds
		 * S_top'.  But it leaves BP's slot holding S_top until the
		 * commit.
		 */
		if (del_folds_into_graft
#ifdef FT_RED_NO_DEL_FOLD
				&& 0	/* red control: see fractal-trie-internal.h */
#endif
		   ) {
			/*
			 * ☠ THE EXPECTED-OLD IS THE RAW WORD, NOT @s_top.
			 *
			 * The fold's compare is `*pending_del_slot ==
			 * pending_del_expected`, so it must be given what the
			 * SLOT holds.  @s_top is that word RESOLVED.  The two
			 * differ whenever the slot is skip-encoded.  That is
			 * the common case, since a default group carries
			 * CDS_FT_FLAG_SKIP_COMPRESSED.  Pairing them made the
			 * compare unsatisfiable from the moment of capture.
			 * ft_node_recompact's -EAGAIN then became a
			 * SELF-refusal no re-descent could clear: the op spun
			 * forever and the public call never returned.
			 *
			 * @nf_raw comes from the same load as @s_top, so the
			 * pair is coherent.  RE-READING the slot here instead
			 * would defeat the guard the compare exists for.  A
			 * peer that republished between the descent and this
			 * point would be silently adopted.  The fold would
			 * then drop a subtree the plan never examined.  So
			 * the re-read below is a VALIDATION, not the captured
			 * value: it fails closed.
			 *
			 * A parked flip proxy still refuses, as it did when the
			 * expected-old was resolved.  Otherwise raw-vs-raw
			 * could MATCH a proxy and fold against a transient
			 * word.
			 */
			if (ft_node_flip_proxy(d_src.nf_raw) ||
					rcu_dereference(*d_src.nfp) !=
						d_src.nf_raw) {
				/*
				 * -EAGAIN, NOT a shape refusal: both halves are
				 * TRANSIENT.  A parked proxy settles.  A slot
				 * that moved since the descent is a peer, not
				 * a property of the trie.  BP's word is never
				 * held before this point, so the window is
				 * open under MW.  Answering UNCOVERED
				 * terminally would send a delayed move to a
				 * worker that refuses every same-trie rekey.
				 * That reports NOT_SUPPORTED for a call a
				 * re-descent completes (the measured lesson
				 * at the bracket check's -1 arm above).
				 * A re-descent recaptures @nf_raw with its slot,
				 * so this cannot self-livelock.
				 */
				ret = -EAGAIN;
				goto bail_build;
			}
			txn->pending_del_slot = d_src.nfp;
			txn->pending_del_expected = d_src.nf_raw;
		} else if (bp_folds_into_graft_c &&
				fold_mode == FT_REKEY_FOLD_COLLAPSE) {
			/*
			 * COLLAPSE: the resting node had exactly two children
			 * and no keys.  So the drop does not leave a NODE at
			 * all.  It leaves ONE RUN:
			 *
			 *   [surviving ordinal] (++ [absorbed run]) -> subtree
			 *
			 * ft_build_branch lays that canonically.
			 * ft_try_compress_chain ABSORBS a compressed survivor
			 * into the same run.  The "no two adjacent
			 * compresseds" invariant requires that: stacking a
			 * 1-byte node on a compressed survivor would violate
			 * it.  On the way, the build:
			 *   - DEFERS the final child's back edge.  That child
			 *     is reader-visible, so the edge must flip with
			 *     the graft;
			 *   - DEFERS the absorbed node's free;
			 *   - hands back the PLAIN flag, because a fresh run's
			 *     skip form cannot resolve while that back edge is
			 *     deferred.
			 *
			 * ☞ MIRRORS ft-graft.h's old-direction COLLAPSE, which
			 * is this same shape one lane over.
			 */
			uint8_t buf[FT_MAX_KEY_LEN];
			unsigned long surv_keys;
			struct cds_ft_inode_flag *coll;
			struct cds_ft_inode_flag *surv2 = NULL;
			uint8_t sb2 = 0;
			struct ft_lock_ctx cctx;
			struct ft_held_anchor cr_held;
			struct cds_ft_compressed_node *surv_cn = NULL;

			if (rcu_dereference(*fold_top_slot) != climb_rest) {
				ret = -EAGAIN;
				goto bail_build;
			}
			/*
			 * ☠ FENCE BOTH LIVE NODES THIS COLLAPSE RETIRES, and do it
			 * HERE.  This is ahead of the body re-validation below.
			 * That re-validation is the first read of either one.
			 *
			 * The collapse retires TWO live, reader-reachable nodes:
			 *   - the resting node, whose two children it reads to
			 *     lay the run;
			 *   - the survivor, when it is a run itself.
			 *     ft_try_compress_chain ABSORBS its bytes into
			 *     that run.
			 * Both were retired UNFENCED until now, which was two
			 * defects in one:
			 *
			 *  1. THE UNLOCKED RETIRE (the ☐ RESIDUAL this comment used
			 *     to record as inherited).  The build reads a body
			 *     and holds nothing across that window.  So a peer
			 *     can add a third child inside it, and that child
			 *     is retired along with the node.  That is silent
			 *     key loss, the gap ft_glue_defer_free_fenced
			 *     exists to close.
			 *
			 *  2. A ONE-WAY MARK WRITTEN BY AN ATTEMPT THAT CAN STILL
			 *     ABORT.  An unfenced entry cannot ride the txn.
			 *     A fused free list parks the tombstone SW.  That
			 *     is step 2 of the STATE-WORD PROTOCOL without
			 *     step 1, and it can strip a peer's LOCK bit (the
			 *     record-time owner assert says so).  So the sweep
			 *     took its STANDALONE arm instead.  It stamped the
			 *     mark IMMEDIATELY, outside @txn, while this op's
			 *     single commit was still ahead of it.  The mark
			 *     is ONE-WAY, so an attempt that then aborted left
			 *     the word TOMBSTONE forever.  ft_dlm_lock
			 *     hard-refuses a tombstoned word with no bail.
			 *     Above per-node spacing the lock-set maps onto an
			 *     ANCHOR ANCESTOR, and the retired node IS the
			 *     word the next attempt needs.  There the op could
			 *     never converge.  Measured at EXPONENTIAL and
			 *     single-threaded, over 45 s: 16.6M immediate
			 *     marks against 8.3M acquires refusing on a word
			 *     THIS THREAD had marked.  It went on forever.
			 *
			 * Fencing answers both.  The acquire brackets the same
			 * world the retire ratifies.  The fenced entry records
			 * its {LOCK|s -> TOMBSTONE|s} terminal INTO @txn, so
			 * the mark lands with the unlink or not at all.
			 *
			 * A MISS IS THE CLEAN TRANSIENT.  Nothing is built and
			 * nothing is published.  ft_glue_abort at @bail_build
			 * releases whatever this block already filed
			 * (ft_glue_clear_fenced).  Return -EAGAIN, never a
			 * shape refusal.  The merge arm's own overlap fence
			 * does the same.
			 *
			 * @fold_rest_depth is the ABSOLUTE byte-depth the arming
			 * already computed for this node (d_src.pdepth -
			 * climb_bytes, under that arm's climb_bytes < pdepth
			 * guard).  It has to be absolute and it has to be the
			 * node's LIVE path.  A relative depth anchors the node
			 * on a different word than every other op computes for
			 * it.  The acquire then refuses its own fence forever
			 * (the measured exponential storm at ft_merge_build's
			 * @d_prov).
			 */
			ft_lock_ctx_init(&cctx, &d_src, txn, optxn);
			/*
			 * Name the rest of the op's held set, as every other
			 * acquire in this function does.  That is
			 * ft_rekey_cow_stop's marks plus the glue's own named
			 * fences.  Under a coarse spacing these two nodes
			 * anchor onto one of them.  A frame naming neither
			 * refuses the op's OWN fence.
			 */
			cctx.held.extra = marks;
			cctx.held.nr_extra = nr_marks;
			cctx.held.glue = &glue;
			if (ft_acquire_member(ft, &cctx, climb_rest,
					cds_ft_item_to_metadata(
						ft_node_ptr(climb_rest)),
					fold_rest_depth, &cr_held)) {
				ret = -EAGAIN;
				goto bail_build;
			}
			/*
			 * FILED NOW, not after the build.  From here the fence
			 * is the glue's.  So every bail below releases it
			 * through the one choke point.  That covers the
			 * re-validation, the -ENOMEM and the prepare.  None of
			 * them owes its own unlock.
			 */
			ft_glue_defer_free_fenced(&glue,
				ft_node_ptr(climb_rest), false, &cr_held);
			/*
			 * ...AND THE SURVIVOR, but only when it is the shape the
			 * build absorbs.  ft_try_compress_chain fuses a
			 * (skip-)compressed child's run into the one it lays
			 * and retires it.  It leaves a PLAIN or EXTERNAL
			 * survivor alone (no free-list entry, nothing to
			 * fence).  Its depth is one span below the resting
			 * node.
			 *
			 * The anchor goes to @glue.  The absorb is mid-build,
			 * past every clean bail, so it cannot take this itself
			 * (see @absorb_node).  It is ALWAYS consumed, because
			 * this arm is armed only under the same `1 + surv_len
			 * <= FT_SKIP_LEN_MAX` bound the absorb tests.
			 */
#ifdef FEATURE_FT_SKIP_COMPRESSED
			if (ft_node_skip_compressed(fold_surv))
				surv_cn = ft_skip_to_compressed(ft, fold_surv);
			else
#endif
			if (ft_node_compressed(fold_surv))
				surv_cn = ft_compressed_node_ptr(fold_surv);
			if (surv_cn) {
				if (ft_acquire_member(ft, &cctx, fold_surv,
						cds_ft_item_to_metadata(
							(struct cds_ft_inode *)
								surv_cn),
						fold_rest_depth +
							ft_node_span(ft,
								climb_rest),
						&glue.absorb_held)) {
					ret = -EAGAIN;
					goto bail_build;
				}
				glue.absorb_node = surv_cn;
			}
			/*
			 * RE-VALIDATE the body read at PLAN time.
			 *
			 * ☠ THE SURVIVOR IDENTITY IS NOT ENOUGH ON ITS OWN, and
			 * an earlier version of this comment claimed it was.
			 * ft_node_other_child returns the FIRST child that is not
			 * the excluded one.  So two changes leave `surv2 ==
			 * fold_surv && sb2 == fold_surv_byte` perfectly true:
			 *   - a THIRD child added at a higher ordinal;
			 *   - a co-located KEY added to the resting node.
			 * Yet the collapse premise (exactly two children, no
			 * keys) has gone.  So the SHAPE is re-asserted here
			 * too.  Only now does this block actually establish
			 * that "the body read at plan time is the body still
			 * there".
			 */
			if (!ft_node_other_child(climb_rest, fold_drop_expected,
					&sb2, &surv2) || surv2 != fold_surv ||
					sb2 != fold_surv_byte ||
					!ft_rekey_collapse_shape(ft,
						cds_ft_item_to_metadata(
							ft_node_ptr(climb_rest)))) {
				ret = -EAGAIN;	/* stale plan: re-descend */
				goto bail_build;
			}
			buf[0] = fold_surv_byte;
			/*
			 * The survivor's key count: the resting node held the
			 * moved arm and this one, and @cnt leaves with the move.
			 * The graft's own +count walk then stays net zero
			 * (ft_rekey_move_folded).
			 */
			surv_keys = ft_nr_keys_get(cds_ft_item_to_metadata(
				ft_node_ptr(climb_rest))) - cnt;
			coll = ft_build_branch(ft, buf, 0, 1, fold_surv,
				surv_keys, false, &glue);
			if (!coll) {
				/*
				 * ☞ CONTENTION IS NOT A MEMORY ERROR.  The build
				 * FENCES a live node it absorbs (@absorb_miss).
				 * A missed acquire must retry, not propagate
				 * as MEMORY_ERROR.  That is the engine's own
				 * rule that the two failure spaces stay
				 * apart.  The fence this arm pre-took above
				 * makes the miss unlikely here, but the build
				 * has a second absorb path of its own.
				 */
				ret = glue.absorb_miss ? -EAGAIN : -ENOMEM;
				goto bail_build;
			}
			/*
			 * ☠ THE HAND-OFF FENCE MUST NOT OUTLIVE THE BUILD THAT
			 * WAS SUPPOSED TO CONSUME IT.  The absorb clears
			 * @absorb_node as it files the fenced entry.  So a
			 * value still here means the build took a path that
			 * did not absorb.  Then nothing downstream ever
			 * releases the mark.  ft_glue_clear_fenced is the net,
			 * but the rekey's SUCCESS path does not run it.
			 * ft_dlm_lock refuses a LOCKED word, so the node would
			 * be unmutable for the life of the trie.
			 *
			 * UNREACHABLE BY THE ARMING.  This arm is armed only
			 * under the same `1 + surv_len <= FT_SKIP_LEN_MAX`
			 * bound the absorb tests, so a (skip-)compressed
			 * survivor is always absorbed.  That is exactly why it
			 * is written as a RELEASE AND RETRY rather than an
			 * assert.  The invariant lives in a different
			 * function's bound.  If someone widens one side of it,
			 * this degrades to a retry instead of wedging a node.
			 * @coll is glue-tracked, so ft_glue_abort reclaims it
			 * on the way out.
			 */
			if (caa_unlikely(glue.absorb_node)) {
				/* STRICT: a release-and-retry, nothing committed. */
				if (!glue.absorb_held.shared &&
						glue.absorb_held.lock)
					ft_meta_lock_release(
						glue.absorb_held.lock);
				glue.absorb_node = NULL;
				ret = -EAGAIN;
				goto bail_build;
			}
			/*
			 * ☠ NOT @fold_rest_new / @fold_rest_old.  Those are
			 * freed by this function's own bail and commit paths,
			 * with free_cds_ft_node_unpublished.  That is the
			 * wrong freer for a COMPRESSED node.  And @coll is
			 * GLUE-TRACKED (ft_build_branch took @glue), so
			 * ft_glue_abort already reclaims it.  Naming it in
			 * both places frees it twice: measured,
			 * cds_ft_do_free_item's `range->nr_live > 0`.
			 *
			 * ☞ THE RESTING NODE'S RETIRE IS ALREADY FILED, fenced,
			 * at the acquire above.  That is also why this arm may
			 * set @fuse_free_list at all.  The objection that used
			 * to stand here: a fenced entry asserts that flag, and
			 * the flag "promotes EVERY entry to a txn record --
			 * including the run ft_try_compress_chain just
			 * absorbed".  That would then fail the record-time
			 * owner check.  True, and now answered at the source:
			 * that run is fenced too, by the same block.  So there
			 * is no unheld entry left for the flag to promote.
			 */
			/*
			 * ☠ THE SUBSTITUTION IS ON @graft_c'S SLOT, not on the
			 * resting node's.  The fold's whole shape is "one slot
			 * in @graft_c changes value inside the copy the graft is
			 * already making".  The slot that HELD the resting node
			 * now holds the collapsed run.  Arming @fold_drop_slot
			 * instead names a slot INSIDE the resting node, which
			 * the copy loop never visits.  So the drop goes
			 * unconsumed and the detach guard below refuses the
			 * move.  Measured: 55 shapes landing on `!graft_p ||
			 * bp_folds_into_graft_c`.  REPLACE and PROMOTE arm the
			 * same pair; only the value differs.
			 */
			txn->pending_del_slot = fold_top_slot;
			txn->pending_del_expected = climb_rest;
			txn->pending_del_replace = coll;
			/*
			 * ...and SAY it is fresh.  The sweep must not infer that
			 * from "there is a substitution": PROMOTE substitutes a
			 * live app-owned head and owes a RECORDED re-parent.
			 */
			txn->pending_del_replace_fresh = true;
			/*
			 * ...and the CANONICAL word for the slot the copy loop
			 * will wire.
			 *
			 * ☞ WHY THE SWEEP NEEDS THE PLAIN FLAG, correctly
			 * stated.  It is NOT because the skip spelling depends
			 * on the parent or slot offset.  ft_publish_compressed
			 * is ft_skip_compressed_flag(cn->child, cn->len).  It
			 * is computed here and independent of both.  It is
			 * because RESOLVING a skip word goes through its
			 * child's BACK EDGE.  This run's child edge is DEFERRED
			 * until the flip.  A sweep handed the skip form would
			 * resolve it to whatever that child still points at.
			 * So the sweep gets the plain flag and the SLOT takes
			 * the skip form.
			 */
			if (ft_node_compressed(coll))
				txn->pending_del_replace_pub =
					ft_publish_compressed(ft,
						ft_compressed_node_ptr(coll),
						coll);
		} else if (bp_folds_into_graft_c &&
				fold_mode == FT_REKEY_FOLD_REPLACE) {
			/*
			 * REPLACE: build the resting node's DEL-recompacted copy
			 * HERE, before the graft's copy loop needs it.
			 *
			 * ☞ ft_node_replace_ptr's DEL arm writes the fresh flag
			 * into the @parent_node_flag_ptr the caller hands it and
			 * PUBLISHES NOTHING.  Its header says so ("a LOCAL
			 * out-param re-published by the caller").  That is
			 * exactly the seam this fold needs.  The detach's
			 * version of this same edit publishes into @graft_c,
			 * the body the graft is about to supersede.  That
			 * publish is the defect.  Here the caller that
			 * re-publishes it is the copy loop, into the fresh
			 * body, atomically.
			 *
			 * The old copy is retired by the recompaction's own
			 * record and reclaimed on the far side of the commit.
			 */
			struct cds_ft_inode_flag *fresh = climb_rest;
			struct ft_remove_pub rpub;
			struct ft_lock_ctx rctx;
			uint8_t rn = 0;

			if (rcu_dereference(*fold_top_slot) != climb_rest) {
				ret = -EAGAIN;
				goto bail_build;
			}
			/*
			 * ☠ THE ENCODED WORD, NOT THE RESOLVED FLAG.
			 * ft_node_find_child compares RAW WORDS (ft-lookup-node.h
			 * says so), and this slot may hold the SKIP-ENCODED form
			 * of the same child.  @fold_drop_child is the resolved
			 * flag; the arming already kept the word the slot
			 * actually holds in @fold_drop_expected.  Handed the
			 * resolved one, the two can never be equal on a skip
			 * group.  Then "stale plan: re-descend" NEVER clears,
			 * and the op spins for ever re-filling its node
			 * reserve.  That hang kept both value-writing modes
			 * refused on a compressing build.  ☞ the note at
			 * ft_rekey_fold_shape_ok for the attribution run.
			 *
			 * ★ Same law as the glue's skip identity:
			 *   - SPEAK THE SLOT'S ENCODED WORD when comparing or
			 *     storing;
			 *   - speak the node it DENOTES when reasoning about
			 *     identity.
			 */
			if (!ft_node_find_child(ft, climb_rest, fold_drop_expected,
					&rn, NULL)) {
				ret = -EAGAIN;	/* stale plan: re-descend */
				goto bail_build;
			}
			memset(&rpub, 0, sizeof(rpub));
			ft_lock_ctx_init(&rctx, &d_src, txn, optxn);
			rctx.held.extra = marks;
			rctx.held.nr_extra = nr_marks;
			ret = ft_node_replace_ptr(ft, fold_drop_slot,
					fold_drop_expected,
					&fresh, &fold_rest_old,
					ft_flag_to_metadata(ft, climb_rest),
					rn, NULL /*delete*/,
					/* bulk: the exclusive-only in-place tier */
					ft_in_place_delete_excl_ok(ft),
					false /*is_root: a child of @graft_c*/,
					fold_rest_depth, &rpub, txn,
					NULL /*held_hint: no publish to guard*/,
					&rctx);
			if (ret || fresh == climb_rest) {
				/*
				 * A recompaction that did not relocate leaves
				 * nothing to fold and would send the detach at
				 * the superseded body after all.
				 */
				if (!ret)
					ret = FT_REKEY_UNCOVERED;
				goto bail_build;
			}
			/*
			 * ☠ AND BAKE THE POST-REMOVAL COUNT INTO IT.  The
			 * COLLAPSE arm above does the same when it computes
			 * @surv_keys.  So does the detach's own recompaction
			 * arm (ft-remove.h, "the fresh copy carried the OLD
			 * node's count out of ft_node_recompact").  @fresh IS
			 * the resting node minus the moved arm.  It is a
			 * SURVIVOR: it keeps every other key.  So the net-zero
			 * the fold claims holds at @graft_c and NOT one level
			 * down.  This is a build-invisible plain store on a
			 * copy nothing reaches until the flip.  It is a no-op
			 * when order statistics are off (ft_nr_keys_store
			 * checks @rank_stats).
			 *
			 * Without it the detach that would have baked -@cnt is
			 * skipped by ft_rekey_move_folded and NOTHING charges
			 * @fresh.  MEASURED with rank stats on, over 1200
			 * generated shapes on -DNO_FEATURE_FT_COMPRESS.  The
			 * fold shape `folded && mode == REPLACE` is an EXACT
			 * discriminator for the residue.
			 *   - 40 of 40 such moves answer OK with every key at
			 *     the right name.  But cds_ft_verify is RED
			 *     (`nr_keys mismatch: stored N, computed N-cnt`).
			 *   - 0 of the other 1160 moves fail.
			 * Two-key repro:
			 * {"bca","baccc"} + rekey_merge(dst "a", src "bc").
			 */
			ft_nr_keys_store(ft, ft_flag_to_metadata(ft, fresh),
				ft_nr_keys_get(ft_flag_to_metadata(ft, fresh)) -
					cnt, CMM_RELAXED);
			txn->pending_del_slot = fold_top_slot;
			txn->pending_del_expected = climb_rest;
			txn->pending_del_replace = fresh;
			fold_rest_new = fresh;
		} else if (bp_folds_into_graft_c &&
				fold_mode == FT_REKEY_FOLD_PROMOTE) {
			/*
			 * PROMOTE.  The fresh copy is born holding the resting
			 * node's key chain in that slot.  The copy loop's
			 * @pending_pub_slot arm carries it.  The GLUE and MERGE
			 * builders announce the same field.  Here it is used by
			 * the one writer that never did.
			 *
			 * ☠ THAT ARM VALIDATES NO EXPECTED-OLD.  It substitutes
			 * whatever the slot resolved to.  So the pairing is
			 * checked HERE, and RATIFIED at the freeze below.  The
			 * freeze re-reads the resting node's parent under its
			 * own lock.
			 * Return -EAGAIN, never a shape refusal.  The slot is
			 * unheld until the freeze.  So a peer is a transient
			 * that a re-descent clears.
			 */
			if (rcu_dereference(*fold_top_slot) != climb_rest) {
				ret = -EAGAIN;
				goto bail_build;
			}
			txn->pending_del_slot = fold_top_slot;
			txn->pending_del_expected = climb_rest;
			txn->pending_del_replace = fold_promote_val;
		} else if (bp_folds_into_graft_c) {
			/*
			 * THE SAME ARMING ONE OR MORE LEVELS UP.  The slot is
			 * @graft_c's.  It holds the topmost node the drop
			 * empties.  The expected-old is THE WORD THAT SLOT HELD
			 * AT THE ARMING:
			 *   - @climb_top itself on a plain link;
			 *   - its SKIP_X spelling where the link is a run.
			 * ☞ the law at ft_rekey_slot_names.  The copy loop
			 * compares this by identity against
			 * rcu_dereference(*slot), so it must be the encoded
			 * word.  The freeze walk below retires the NODE
			 * @climb_top names.
			 *
			 * RE-VALIDATED here, not re-derived.  A peer may have
			 * republished @graft_c's slot since the gate.  That
			 * slot holds a subtree "copy all but this slot" would
			 * drop whole.  Adopting its word would fold against a
			 * plan that never examined it.
			 * Return -EAGAIN, never a shape refusal.  BP's
			 * neighbourhood is unheld until the freeze below.  So
			 * the window is open under MW, and a re-descent
			 * recaptures it.
			 */
			if (!fold_top_expected || rcu_dereference(*fold_top_slot) !=
					fold_top_expected) {
				ret = -EAGAIN;
				goto bail_build;
			}
			txn->pending_del_slot = fold_top_slot;
			txn->pending_del_expected = fold_top_expected;
		}
		cds_ft_alloc_reserve_activate(ft, &reserve);
		/*
		 * Hand over THE SRC DESCENT, for the deferred re-parents
		 * still on the src path.  The fold arm makes the same
		 * hand-off at its own glue.lock_d_src, for the same stated
		 * reason.  Nothing else can date a member reached from the
		 * BUILD.  A glue with no descent for it leaves it undatable
		 * forever.
		 *
		 * ft_store_at_graft_point_prepare sets @glue's lock_d from
		 * the DST descent it is handed (ft-graft.h).  A rekey
		 * GRAFT's destination is EMPTY BY DEFINITION: d_dst.nf is
		 * NULL.  So a @dst_origin=false member resolved through it
		 * has no cursor node to take a span from.
		 * ft_lock_ctx_depth_of_cursor_child then refuses.  What
		 * follows:
		 *   - ft_glue_acquire_reparent_marks returns -EAGAIN;
		 *   - the commit answers URCU_TXN_STATUS_ABORT
		 *     (ft-graft.h);
		 *   - this function returns -EIO;
		 *   - the caller's retry loop re-descends into an
		 *     IDENTICAL attempt.
		 * This is a livelock with no contention in it.  It is
		 * certain single-threaded.
		 *
		 * Only the EXPONENTIAL schedule reads the depth at all.
		 * ft_lock_ctx_depth_of_at answers per-node and root-only
		 * from the spacing alone.  That is why EXPONENTIAL was the
		 * only arm that hung.
		 */
		glue.lock_d_src = &d_src;
		{
			/*
			 * The op's outstanding marks are ft_rekey_cow_stop's
			 * @stop fence and one per COW'd child.  They reach no
			 * registry until the sweep below.  So the store's own
			 * recompactions can only see them through this frame.
			 *
			 * ☞ This frame prevents the STORE's own-fence refusal.
			 * That is NOT what the failure branch below catches on
			 * the shape measured here.  There the failure arrives
			 * with gst == CDS_FT_STATUS_OK and
			 * gcst == URCU_TXN_STATUS_ABORT.  It comes from the
			 * re-parent acquire above, not as a BUSY_ERROR from
			 * this prepare.
			 */
			struct ft_lock_ctx octx;

			ft_lock_ctx_init(&octx, &d_src, txn, optxn);
			octx.held.extra = marks;
			octx.held.nr_extra = nr_marks;
			/*
			 * AND THE GLUE's named fences.  The climb-rest /
			 * survivor fences above are filed into @glue before
			 * this prepare runs.  At root-only the store's
			 * recompaction maps onto the SAME word.  So does
			 * exponential, when they share an anchor.  A frame
			 * naming only @marks refused the op's own fence on
			 * every attempt.  That was a single-threaded livelock
			 * at the -EIO retry below.
			 */
#ifndef FT_DEBUG_GRAFT_OCTX_NO_GLUE
			octx.held.glue = &glue;
#endif
			gst = ft_store_at_graft_point_prepare(ft, dst_ord,
				dst_len, &d_dst, s_top_prime, cnt, &glue,
				&octx.held, &gst_st);
		}
		if (gst == CDS_FT_STATUS_OK)
			/*
			 * NET count.  The drop may be folded into this same
			 * copy.  Then the subtree LEAVES and RE-ENTERS one
			 * node.  BP' holds it under the grafted byte instead of
			 * the src byte.  So the key count of BP and of every
			 * ancestor is unchanged.  The detach below, which would
			 * have walked -@cnt, does not run.
			 */
			gcst = ft_store_at_graft_point_commit(ft, &attached_nf, &adepth,
					NULL /*run*/, &gst_st,
					ft_rekey_move_folded(txn, fold_mode,
						fold_top_slot) ? 0 : (long) cnt);
		cds_ft_alloc_reserve_deactivate(ft);
		cds_ft_alloc_reserve_drain(ft, &reserve);
		if (gst != CDS_FT_STATUS_OK || gcst != URCU_TXN_STATUS_OK) {
			/*
			 * Not expected single-threaded with the reserve
			 * pre-filled.  A record-only commit leaves the shared
			 * txn intact (the terminal commit is gated off).  So
			 * the caller owns cleanup:
			 *   - free S_top';
			 *   - abort the glue build;
			 *   - destroy the txn.
			 * (A prepare failure freed its own invisible build and
			 * left glue clean.)
			 */
			ft_glue_abort(ft, &glue);
	if (src_glue_live) {		/* merged cluster's src side */
		ft_glue_abort(ft, &src_glue);
		src_glue_live = false;
	}
			/*
			 * ☠ S_top' IS FREED AFTER THE GLUE'S FENCES, NEVER
			 * BEFORE THEM.  The build's ft_try_compress_chain can
			 * ABSORB the copy into the run it lays.  It then files
			 * it on @glue's free list FENCED, with a lock taken on
			 * S_top' itself.  So ft_glue_abort's
			 * ft_glue_clear_fenced releases that word.
			 * Freeing first handed the chunk back to the arena.
			 * The release then cleared a LOCK bit in freed memory.
			 * The rcu-debug balance caught it as FT LOCK UNDERFLOW
			 * (take, arena reset, plain release).  A recycled chunk
			 * would have lost a PEER's lock.
			 * Any bail after the absorb reaches it.  A peer
			 * refusing the store's recompaction is enough.  The
			 * later bails state the same rule for @gst_st.dest,
			 * below.
			 */
			/* NULL on the merge path: no COW */
			ft_rekey_free_stop_prime(ft, s_top_prime);
			ft_flip_txn_destroy(txn);
			/*
			 * The graft ALWAYS relocates the attach node.  That
			 * copy comes from ft_node_recompact and is NOT
			 * glue-tracked.  So the ft_glue_abort above does not
			 * reach it.
			 * Every other bail in this function frees it.  This
			 * one did not have to at first.  A record-only NOSPLIT
			 * commit had NO failure path until the re-parent
			 * acquire (ft-graft.h) gave it one.  An in-place
			 * recompact leaves @dest == the LIVE node.
			 * @old_recompacted_node is exactly the flag for that.
			 *
			 * ☠ AND free it AFTER THE REGISTRY SWEEP, like every
			 * other bail.  Its born-locked relocation fence is
			 * registered on @txn.  ft_flip_txn_destroy releases it
			 * through the node's own metadata.  Freeing first
			 * released into a freed chunk.
			 */
			if (gst_st.old_recompacted_node)
				free_cds_ft_node_unpublished(ft,
					ft_node_ptr(gst_st.dest));
			ret = -EIO;
			goto sweep;
		}
		/*
		 * ☠ THE DST-FRESHNESS OBLIGATION, the graft arm's half.
		 * The merge arm states the full reasoning at its own
		 * publish.  The property owed: the destination attach
		 * point ends up holding a node NO READER HAS SEEN.  Then
		 * the coherent reader's two-descent witness cannot match a
		 * torn descent against a clean one.
		 *
		 * ☠ AND THE OBLIGATION HAS TWO WITNESSES, not one.  This
		 * asserted only the first.  Its comment said the first was
		 * the shape: "the fresh node at the destination attach
		 * point is the RECOMPACTED ATTACH PARENT ...
		 * @old_recompacted_node is the witness that it did".  That
		 * describes one arm: a node ALREADY STANDS at the attach
		 * point, and the reserve recompaction relocates it.  It is
		 * not the only arm.
		 * The graft point may hold an EXTERNAL LEAF, a key ending
		 * on the dst path.  Then there is no attach parent to
		 * recompact.  The graft BUILDS A BRANCH and displaces the
		 * leaf into it.  @gst_st.dest is then NULL by
		 * construction, and so is @old_recompacted_node.  The node
		 * published at the attach point is that FRESH BRANCH.  It
		 * is carried in @gst_st.attached ("payload (at-node) or
		 * branch") and flagged by @gst_st.displaced_shape.
		 *
		 * ☑ MEASURED, and it is not a corner.  Over 3000 generated
		 * shapes, 219 reach this line with @old_recompacted_node
		 * NULL.  All 219 are `displaced_shape` with
		 * `ft_glue_is_fresh(attached)` TRUE.  This holds on the
		 * default build and on -DNO_FEATURE_FT_COMPRESS alike.
		 * Every one of them met the obligation.  The ASSERT was
		 * asking for a witness that arm does not produce.
		 *
		 * ☞ A CLAIM WITHOUT AN ARM.  Nothing here changes
		 * behaviour.  This shape is still refused upstream (the
		 * displaced-external gate).  So the line is unreachable
		 * today.  It is corrected NOW because it is the FIRST thing
		 * in the way of serving that shape.  A wrong witness fires
		 * as "the obligation is broken" on a move that honours it.
		 * Whoever lifts the gate would read it that way.
		 *
		 * The bulk in-place tier stays the documented exception.
		 * ft_in_place_excl_ok requires @ft->exclusive,
		 * "single-writer, NO CONCURRENT READERS".  So there is no
		 * reader to owe a fresh visited-node set to.  The POINT-op
		 * tier (ft_in_place_ok) is wider and never reaches this
		 * arm: the graft reserve vouches only the exclusive tier.
		 * A bulk caller may one day start vouching it.  Then this
		 * line turns the silent witness loss into a loud one.
		 */
		FT_REKEY_DST_FRESH_REACH(1);
		urcu_assert_debug(gst_st.old_recompacted_node != NULL ||
			(gst_st.displaced_shape &&
				ft_glue_is_fresh(ft, &glue, gst_st.attached)) ||
			ft_in_place_insert_excl_ok(ft));
	}

	/*
	 * 3. Detach-fold: remove S_top from BP (clear its slot + nr_child--).
	 * In the default (concurrent-safe) build EVERY popcount delete
	 * recompacts BP.  That recompaction republishes into BP's parent.
	 * There are two shapes.  The gate above decides which, and
	 * @src_parent_held carries it:
	 *   - BP's parent IS the spine ancestor the graft's dst-parent recompaction
	 *     already holds the lock (both junctions are its children).  REUSE the held
	 *     lock; re-acquiring it would abort -EAGAIN.
	 *   - BP's parent is a node this op holds nothing on.  The recompaction
	 *     acquires and releases it itself, in its own up-front lock-set commit.
	 * Either way the identity is passed EXPLICITLY, and its slot with
	 * it.  The recompaction does not resolve BP's current parent
	 * itself.  That resolve is racy.  A peer may have re-homed BP since
	 * this descent.  "BP's parent" would then be a node this op does
	 * NOT hold.  The result would be an SW park into a slot that no
	 * longer holds BP.
	 * @parent_guard puts the BP.parent == @parent read-set guard on the
	 * acquire commit in BOTH shapes (the held arm guards
	 * unconditionally).  So a re-home ABORTS it (-EAGAIN, trie
	 * pristine), and the caller re-descends.
	 * BP's parent is never compressed: the src descent rejects
	 * compressed nodes at every level it walks.  So there is no SKIP_X
	 * dual and no @gp member.
	 */
	/*
	 * ★ THE CONTENT TXN IS PART OF THIS OP'S HELD SET, and naming it is
	 * MANDATORY for a fold.  An acquire whose RELEASE has been RECORDED
	 * but not yet COMMITTED still lives in @txn's locks[] registry.
	 * Under the fold that is every acquire this op has made: the one
	 * decide has not run yet.  ft_flip_txn_record_release_lock drops the
	 * hold-trace entry as soon as it records the {LOCK|s -> s} edge
	 * ("the commit owns this release now").  The WORD keeps its LOCK bit
	 * until that commit lands.  So the registry is the ONLY remaining
	 * witness that this op holds the word.
	 *
	 * ☠ WITHOUT IT THE MOVE LIVELOCKS.  The graft side's
	 * ft_node_recompact acquires the src junction and records its
	 * release into @txn.  The collapse ft_chain_compress_fused runs
	 * inside the same decide.  It meets that LOCK bit still set.
	 * ft_dlm_acquire_set's dedupe cannot see a hold in a registry this
	 * frame does not name.  So ft_dlm_lock reads the op's OWN mark as
	 * contention.  The -EAGAIN retry then re-derives the identical plan
	 * forever.  It is single-threaded, so no peer can ever clear it.
	 *
	 * The op's marks so far are ft_rekey_cow_stop's @stop fence and one
	 * per COW'd child.  They reach no txn registry until the sweep
	 * below.  So the detach's own acquires see THOSE through @extra on
	 * this frame instead.  Under a coarse spacing S_top's fence lands on
	 * BP.  BP is exactly the node this detach recompacts.
	 */
	/*
	 * SKIP THE DETACH ENTIRELY when its slot drop already rode the
	 * graft's recompaction of BP (@pending_del_slot).  BP == graft_c
	 * means the src slot and the grafted slot are BOTH IN BP.  So the
	 * move is a slot rename inside one node, and that one copy is the
	 * whole structural edit:
	 *   - BP is superseded ONCE;
	 *   - its child set is right at birth;
	 *   - there is no second recompaction left to size a shape from.
	 *
	 * Armed-but-not-folded falls through here and detaches normally.
	 * The flag is set by the copy loop that actually consumed the drop,
	 * never by the arming.
	 */
	/*
	 * ...and skip it for the same reason when the GLUE split DROPPED
	 * the old direction.  The slot that would be cleared lives in a run
	 * this flip retires WHOLE.  The fresh path published in its place
	 * never held the src edge at all.
	 */
	/*
	 * BOTH src-side arms below report through @ret.  The third case
	 * owes nothing: a height-0 fold, or a GLUE split that dropped the
	 * old direction.  It must not be judged by a stale @ret.
	 */
	ret = 0;
	if (!ft_rekey_move_folded(txn, fold_mode, fold_top_slot) &&
			!glue.old_dir_dropped && !glue.old_dir_replace.done) {
		ft_lock_ctx_init(&lctx_src, &d_src, txn, optxn);
		lctx_src.held.extra = marks;
		lctx_src.held.nr_extra = nr_marks;
		/*
		 * And the GLUE, which holds the rest: the publish-parent
		 * fence above all.  Under a coarse spacing that fence and
		 * BP's recompaction are ONE word.  Without this, the detach
		 * refuses a fence this op took three steps earlier.
		 */
		lctx_src.held.glue = &glue;
		/*
		 * ☠ A ROOT-LEVEL BP-PARENT HAS NO WORD TO PARK THE REPUBLISH UNDER.
		 *
		 * The shape gate admits @graft_p == NULL only for the
		 * FOLDED move.  So reaching here with it NULL means the drop
		 * was ARMED and NOT consumed.  The detach would then be
		 * handed @parent == NULL with @parent_held TRUE.  It is true
		 * only because @src_parent_held compares two NULLs.  The
		 * recompaction would then park a republish into &ft->root
		 * as an unguarded plain store.
		 *
		 * FT_REKEY_UNCOVERED, NEVER -EAGAIN.  The retry loop re-runs
		 * -EAGAIN unconditionally.  Whatever made the copy loop skip
		 * the drop is a property of the build and the shape, not a
		 * transient.  So -EAGAIN here would spin forever, with no
		 * peer able to clear it.
		 *
		 * The refusal is clean.  The store commit above ran
		 * record_only, so nothing is published.  The bail below
		 * already reclaims every fresh copy this attempt built.
		 *
		 * Believed UNREACHABLE.  The in-place tier is the one that
		 * can reserve BP without relocating it.  The gate refuses
		 * the root shape on exactly that tier.  It is still kept as
		 * a guard, not an assert, because that reasoning is
		 * BUILD-CONDITIONAL.  A release build must not answer it
		 * with a wild store.
		 */
		/*
		 * ...and the SAME refusal for a climb-armed drop the copy
		 * loop did not consume.  @pending_del_slot named a slot in
		 * @graft_c.  A detach reaching here would instead:
		 *   - clear BP's own slot;
		 *   - leave @climb_top's chain wired under a node this
		 *     flip retires;
		 *   - republish into a body the graft has superseded.
		 * That is the defect the arming exists to avoid, arrived at
		 * from the other side.  Believed unreachable for the same
		 * build-conditional reason: the in-place tier is the arm
		 * that can reserve without relocating, and the arming
		 * refuses it.  Kept as a guard, not an assert, for the same
		 * reason.
		 */
		/*
		 * ☞ @root_pub_ok, not a bare NULL test.  Its third disjunct
		 * is exactly this arm's question: BP's parent is a real
		 * node.  Then the republish below has a word to park under.
		 * Only the graft's forward publish touches &ft->root.
		 * Reaching here with @graft_p NULL and @root_pub_ok false
		 * still means the drop was ARMED and NOT consumed.  That is
		 * the case the comment above describes.
		 */
		if ((!graft_p && !root_pub_ok) || bp_folds_into_graft_c) {
			ret = FT_REKEY_UNCOVERED;
			goto detach_bail;
		}
		ret = ft_detach_node(ft, &lctx_src, d_src.nfp, d_src.pnfp, d_src.depth,
				false /*free_detached_subtree: S_top is retired by cow_stop*/,
				NULL /*fuse_cell: list off*/, &pub, NULL /*run*/,
				NULL /*retire_glue*/, NULL /*freeze_leaf*/, 0,
				-(long) cnt, txn /*shared_txn*/, true /*record_only*/,
				&(const struct ft_parent_hint){	/* BP's parent: held or acquired */
					.parent = d_src.ppnf, .slot = d_src.pnfp,
					.gp = NULL, .gp_slot = NULL,
					.parent_held = src_parent_held,
					.parent_guard = true },
				&detach_rc /*old + fresh BP copies, reclaimed post-commit*/,
				/* bulk: exclusive-only tier */
				ft_in_place_delete_excl_ok(ft));
	} else if (bp_folds_into_graft_c) {
		/*
		 * THE FOLD'S OWN HALF OF THE DETACH.  The slot drop already
		 * rode the graft's copy of @graft_c.  The detach would still
		 * have done two things:
		 *   - freeze the chain its climb emptied;
		 *   - hand it back for reclaim on the far side of the
		 *     commit.
		 * That work is owed here, and to THIS txn, so the
		 * tombstones flip with the unlink.
		 *
		 * The held set names what the detach's own frame names, for
		 * the same reasons:
		 *   - @marks: ft_rekey_cow_stop's fence and one per COW'd
		 *     child.  None of them reach a registry until the
		 *     sweep.
		 *   - the GLUE: the publish-parent fence above all.  Under
		 *     a coarse spacing that fence and a chain node's anchor
		 *     are ONE word.
		 *   - the content txn: every acquire whose RELEASE is
		 *     recorded but not yet committed lives only in its
		 *     registry.
		 */
		ft_lock_ctx_init(&fold_octx, &d_src, txn, optxn);
		fold_octx.held.extra = marks;
		fold_octx.held.nr_extra = nr_marks;
		fold_octx.held.glue = &glue;
		/*
		 * ☠ A FRAME CARRIES ONE OUT-OF-REGISTRY ARRAY.  So the
		 * chain's own marks need a frame of their OWN, @fold_held
		 * here.  It is CHAINED to the one above; it does not
		 * replace it.
		 * Without the chain, every acquire in the walk is blind to
		 * the marks this op arrived with (ft_rekey_cow_stop's
		 * fence, the glue's publish parent).  It then refuses its
		 * own fence deterministically.  Without the inner frame,
		 * the tombstone has no witness that the op holds the word
		 * it parks SW.
		 */
		ft_lock_ctx_init(&lctx_src, &d_src, txn, optxn);
		lctx_src.held.extra = fold_held;
		lctx_src.held.nr_extra = 0;
		lctx_src.held.outer = &fold_octx.held;
		ret = ft_rekey_fold_freeze_orphans(ft, &lctx_src, txn,
				d_src.pnf, fold_bp_depth, climb_top,
				fold_mode == FT_REKEY_FOLD_PROMOTE ?
					climb_rest : NULL,
				fold_promote_val,
				fold_mode == FT_REKEY_FOLD_DROP ?
					climb_top : climb_rest,
				graft_c, climb_steps, &detach_rc, fold_held,
				&nr_fold_held);
	} else if (glue.old_dir_replace.done && climb_split_steps >= 1) {
		/*
		 * THE SPLIT'S OLD DIRECTION OWES THE SAME HALF.  Arming
		 * @old_dir_replace on the climb's RESTING node makes the
		 * split build the old half without the moved arm.  It also
		 * skips the detach.  The detach is what would otherwise
		 * have frozen the chain the drop emptied.  Nothing else
		 * reclaims it.  THIS commit unlinks BP and every one-child
		 * keyless node between it and @climb_top.  Nothing names
		 * them afterwards.
		 *
		 * ☠ MEASURED before this arm existed.  The 7 shapes served
		 * on the default build leaked EXACTLY ONE NODE each (the
		 * orphaned run BP).  Every one showed `alloc=N freed=N-1`.
		 * The 3000-shape corpus was GREEN THROUGHOUT.  A leak is
		 * invisible to a correctness oracle.  Only the
		 * -DDEBUG_COUNTERS build's node balance sees it.
		 *
		 * The bound is @climb_split_rest (R), not @graft_c.  R is
		 * the node the split holds, and the chain stops below it.
		 * Everything else is the fold's arm verbatim, for the
		 * reasons stated there.
		 */
		ft_lock_ctx_init(&fold_octx, &d_src, txn, optxn);
		fold_octx.held.extra = marks;
		fold_octx.held.nr_extra = nr_marks;
		fold_octx.held.glue = &glue;
		ft_lock_ctx_init(&lctx_src, &d_src, txn, optxn);
		lctx_src.held.extra = fold_held;
		lctx_src.held.nr_extra = 0;
		lctx_src.held.outer = &fold_octx.held;
		ret = ft_rekey_fold_freeze_orphans(ft, &lctx_src, txn,
				d_src.pnf, d_src.pdepth, climb_split_top,
				NULL /*promoted: the split owns R*/,
				NULL /*promote_val*/,
				climb_split_top /*edited: the dropped chain top*/,
				climb_split_rest, climb_split_steps,
				&detach_rc, fold_held, &nr_fold_held);
	}
	/*
	 * ☠ THE WITNESS MUST HAVE BEEN PRODUCED.  The same-parent splice
	 * published normally on a promise: the detach relocates the dst attach
	 * point's parent and folds the publish into the fresh node.  If no
	 * producer reported the fold, the parent was edited where it stood (or
	 * left alone).  Then:
	 *   - the glue would store the head back into a slot of the node it
	 *     already sits in;
	 *   - keys and structure would verify clean;
	 *   - the two-descent scheme would be silently unwitnessed.
	 * Refuse.  Nothing is committed under record_only.  detach_bail unwinds
	 * the rest.
	 */
#ifdef FT_RED_SAME_PATH_NO_FOLD
	/*
	 * RED CONTROL, never shipped.  Pretend no producer reported the fold.
	 * Then the refusal below and its unwind are EXERCISED.  No reachable
	 * shape takes it today: every same-parent junction relocates and folds.
	 * An unwind that is only argued is not measured.
	 * Expected on the same-path shapes:
	 *   - NOT_SUPPORTED;
	 *   - every key where it was;
	 *   - count exact;
	 *   - cds_ft_verify clean;
	 *   - node and cell balance exact under the drain oracle.
	 */
	if (!ret && witness_by_detach)
		txn->pending_pub_folded = false;
#endif
	/*
	 * THE FRAME ON THE SOURCE PATH IS COPIED NOW, with the detach's edits
	 * in it (ft_rekey_merge_cow_after_decide).  There are two outcomes, and
	 * both are a witness:
	 *   - The detach did NOT relocate the frame.  This is the ordinary
	 *     case: its store into the frame is a slot edit.  The COW runs
	 *     here, through the descriptor.
	 *   - The detach DID relocate it.  The src subtree emptied its
	 *     junction and the climb reached the frame (a no-skip shape).  Its
	 *     fold of the announced publish is the witness the same-path arm
	 *     relies on.  Point the glue's publish at that slot.  The commit
	 *     then skips the store and re-bases the count.  Defer nothing: the
	 *     recompaction's own sweep re-homed the top.
	 *
	 * A bail after a successful detach owns the detach's products, exactly
	 * as the refusal below this block does.
	 *
	 * ☠ THE WHOLE BLOCK IS MERGE-ONLY.  It is guarded for the same
	 * reason its @cow_after is.  W5 is the COW of the dst attach point's
	 * PARENT for a MERGE that produced nothing fresh there.  With no
	 * merge compiled in, nothing ever arms the frame, and
	 * `cow_after.armed` is unreachably false.  Left unguarded, the block
	 * referenced a merge-only TYPE from unconditional code.  Then
	 * -DNO_FEATURE_FT_MERGE did not COMPILE.  That is how the whole
	 * configuration went dark on this branch (since @2b7d8eec).  The
	 * guard is on the block, not its interior.  This keeps the nomerge
	 * build free of a dead arm that would need its own refusal and its
	 * own cleanup.
	 */
#ifdef FEATURE_FT_MERGE
	if (!ret && cow_after.armed) {
		if (txn->pending_pub_folded) {
			/*
			 * The fold names its survivor (@pending_pub_node).
			 * It is the frame's DEL copy.  When the frame
			 * collapsed into the dst run, it is the merged run
			 * instead.  The count re-base decodes that one
			 * compressed-aware.  The plain frame is retired
			 * either way.  That is the witness the same-path
			 * class already rests on.
			 */
			glue.publish_parent = d_dst.pnf;
			glue.publish_slot = d_dst.nfp;
			glue.top = merged_pub;
		} else if (!pub.armed && !detach_rc.new_flag &&
				!detach_rc.collapse.new_cn &&
				!detach_rc.collapse.boundary &&
				!detach_rc.nr_orphans &&
				!detach_rc.orphan_trailing) {
			/*
			 * A detach may report NOTHING: no publish, no copy,
			 * no collapse, no orphan.  The copy below cannot
			 * reason about it, because it re-parents on what the
			 * descriptor says the detach did.  Refuse cleanly:
			 * the trie is byte-identical, and the unwind is the
			 * post-detach refusal's.  Believed unreachable now:
			 * every detach outcome reports one of the four.  Kept
			 * as a guard, not an assert.
			 *
			 * ☑ THE PROMOTE ARM WAS REFUSED HERE TOO.  That arm
			 * is a keyed junction left childless.  Its head is
			 * lifted into the frame's slot (the @pub record) or
			 * into a run's child slot (nothing reported).  The
			 * record-only detach handed NEITHER the retired
			 * junction NOR the emptied run above the moved head
			 * to @detach_rc.  One node leaked per served move.
			 * That was the mechanism behind the drain oracle's
			 * residual population.
			 * The detach now budgets the promoted junction into
			 * its elevated chain (ft-remove.h, @nr_elevated).  It
			 * hands that chain out under the fold on BOTH parent
			 * shapes.  So a promote reports its orphans and takes
			 * the copy below, like every other outcome:
			 *   - the promoted head chains by its back-channel
			 *     record (ft_rekey_cow_reparent_child's
			 *     external arm);
			 *   - ft_rekey_detach_free_orphans reclaims the
			 *     junction and run post-commit.
			 */
			if (detach_rc.new_flag)
				free_cds_ft_node_unpublished(ft,
					ft_node_ptr(detach_rc.new_flag));
			ft_rekey_collapse_free_unpublished(ft, &detach_rc.collapse);
			ret = FT_REKEY_UNCOVERED;
		} else {
			ret = ft_rekey_merge_cow_after_detach(ft, &d_dst, txn,
					optxn, &glue, &cow_after, &detach_rc, marks,
					&nr_marks, &dst_pp_prime, &dst_pp_old);
			if (ret) {
				if (detach_rc.new_flag)
					free_cds_ft_node_unpublished(ft,
						ft_node_ptr(detach_rc.new_flag));
				ft_rekey_collapse_free_unpublished(ft,
					&detach_rc.collapse);
			}
		}
	}
#endif /* FEATURE_FT_MERGE: W5's frame is a merge-only shape */
	/*
	 * ☠ AND RECLAIM WHAT THE DETACH BUILT FOR THE COMMIT.  detach_bail
	 * was written for failures BEFORE or INSIDE the detach.  A refusal
	 * AFTER a successful detach leaves two things recorded in the txn:
	 *   - the fresh BP copy (@detach_rc.new_flag);
	 *   - the folded collapse's merged run (@detach_rc.collapse).
	 * Nobody owns them once the txn is destroyed.  MEASURED with the
	 * red control above: one node leaked per shape.  That lasted until
	 * this mirrored the glue-commit-refused path below, which reclaims
	 * exactly these two.
	 */
	/*
	 * THROUGH A RUN, A RECOMPACTION OF THE SHARED ANCESTOR SUBSUMES THE
	 * PUBLISH.  The pending publish is armed at the RUN's child slot,
	 * one level below the node the detach relocates.  When that node
	 * collapses into the run, the DEEP fold reports it.  When it keeps
	 * other children, it is DEL-recompacted instead.  The result is a
	 * fresh copy (@detach_rc.new_flag) that carries the run's word
	 * verbatim.  The run itself is untouched.  So nothing folds.  There
	 * is nothing to store either: the slot already holds the head.
	 * Say so the way a fold says it.  The glue commit then skips a
	 * forward store into the run.  It also skips the SKIP_X dual that
	 * store would re-encode into the retired copy.  It re-bases its
	 * count onto the survivor.  The witness is the copy.
	 */
	/*
	 * ☠ TWO CONDITIONS ARE LOAD-BEARING, and both are checked, not
	 * assumed.
	 *
	 * The count delta must be ZERO.  @pending_pub_node names the fresh
	 * copy of the run's PARENT.  The node owning @pending_pub_slot is
	 * the run, and nobody credits it.  A splice into an occupied chain
	 * is zero (the chain counts once).  Anything else is refused, not
	 * credited one level up.
	 *
	 * And the fresh copy must exist because the detach RECOMPACTED the
	 * shared ancestor.  The collapse path reports its own fold.  A
	 * collapse that cannot be spelled refuses terminally
	 * (ft_chain_compress_fused).  It does not fall back to a one-child
	 * internal this frame would then have blessed.
	 */
	if (!ret && witness_by_detach && witness_above &&
			!txn->pending_pub_folded && !txn->pending_pub_plain &&
			!glue.count_delta) {
		struct cds_ft_inode_flag *survivor = detach_rc.new_flag;

		/*
		 * The relocated ancestor may also have COLLAPSED into a run.
		 * It is then left with the dst side only and no key.  The
		 * merged run carries the path below it verbatim.  That run is
		 * the survivor the count re-base names.  The re-base decodes
		 * it compressed-aware.
		 */
		if (!survivor && detach_rc.collapse.new_cn)
			survivor = ft_compressed_node_flag(
				detach_rc.collapse.new_cn);
		if (survivor) {
			txn->pending_pub_folded = true;
			txn->pending_pub_node = survivor;
		}
	}
	if (!ret && witness_by_detach && !txn->pending_pub_folded) {
		if (detach_rc.new_flag)
			free_cds_ft_node_unpublished(ft,
				ft_node_ptr(detach_rc.new_flag));
		ft_rekey_collapse_free_unpublished(ft, &detach_rc.collapse);
		ret = FT_REKEY_UNCOVERED;
	}
detach_bail:
	if (ret) {
		/*
		 * Pre-commit bail.  Reclaim EVERY unpublished fresh copy built
		 * so far:
		 *  - S_top';
		 *  - the graft's relocated dst-parent copy, @gst_st.dest.  Its
		 *    old counterpart is @gst_st.old_recompacted_node.
		 * The graft always relocates the attach node.  Under
		 * record_only nothing it built is published until the caller's
		 * commit.  So this arm owns the fresh copy, exactly as the
		 * commit-abort arm below does.  @glue's own build is covered
		 * by ft_glue_abort.  nr_built is 0 for the NOSPLIT shape.
		 *
		 * REACHABLE.  A peer holding BP makes the detach's up-front
		 * lock-set acquire abort -EAGAIN right here.  In the
		 * non-shared-parent shape, a peer holding BP's own parent
		 * does the same.  So does a peer that re-homed BP since this
		 * descent (the @parent_guard read-set validation).
		 * The single-threaded route is a same-junction move: BP is
		 * the graft's own attach node, hence already LOCK-held.  That
		 * route is excluded.  The shape gate rejects it up front,
		 * before any of this is built.
		 *
		 * The CLIMB FOLD reaches it too, from its chain plan-lock.  A
		 * peer holding one of the emptied nodes is the same clean
		 * -EAGAIN.  So is a peer that GREW one since the gate's climb.
		 * That helper has already released every mark it took.
		 */
		pp_meta = NULL;		/* ft_glue_abort below is the single owner */
		ft_glue_abort(ft, &glue);
		if (src_glue_live) {	/* merged cluster's src side */
			ft_glue_abort(ft, &src_glue);
			src_glue_live = false;
		}
		/* NULL on the merge path: no COW.  AFTER the glue's fences:
		 * see the store bail's ☠ S_top' note. */
		ft_rekey_free_stop_prime(ft, s_top_prime);
		ft_flip_txn_destroy(txn);
		/*
		 * ☠ FREE AFTER THE REGISTRY SWEEP, NEVER BEFORE IT.
		 * @gst_st.dest carries a fence this attempt registered on @txn
		 * (the born-locked relocation acquire).  ft_flip_txn_destroy's
		 * sweep RELEASES it through the node's own metadata.  Freeing
		 * first hands that word to the allocator.  The allocator can
		 * refund the item into the active reserve and re-zero its
		 * metadata on the next draw.  The release then lands on a
		 * cleared word (assert) or on a LIVE peer's word (corruption).
		 */
		if (gst_st.old_recompacted_node)
			free_cds_ft_node_unpublished(ft, ft_node_ptr(gst_st.dest));
		goto sweep;
	}

	/*
	 * 3a'. THE STORE LANE'S +count WALK, run HERE and not where it was
	 * derived.  ft_flip_txn_record_count_parent resolves each ancestor
	 * against the relocations the descriptor ALREADY carries.  So it has
	 * to run AFTER the detach above, the step that can relocate the ROOT.
	 * The GLUE and MERGE lanes get this for free: their walk is inside
	 * ft_glue_txn_commit_edges at step 3c.  The NOSPLIT store lane walked
	 * inline at step 3a.  It charged the delta into a root copy this
	 * commit retires.  ☞ @deferred_count_base.
	 *
	 * It is placed before 3b rather than beside the glue commit.  It
	 * belongs to the DETACH's ordering, not to the publish's.  Everything
	 * from here on only adds edges.  So any later position would be
	 * equally correct, and this one is the earliest that is.
	 */
	if (glue.deferred_count_base && glue.deferred_count_delta)
		ft_flip_txn_record_count_parent(ft, txn,
			glue.deferred_count_base, glue.deferred_count_delta);

	/*
	 * 3b. Cell-fold (list on): record the six ordered-cell run boundary
	 * edges into the SHARED txn.  The run then unsplices from src and
	 * re-splices at dst ATOMICALLY with the structural move.
	 *
	 * The driver manages this (run == NULL to the structural folds above)
	 * rather than threading it through them.  The dst-splice PLAIN-STORES
	 * the run's outer links (rfc->prev, rlc->next) at record time.  The
	 * src unsplice READS those links to find the src neighbours.  So the
	 * src unsplice must record FIRST.  The structural folds' order can't
	 * provide that: the graft acquires its lock set before the detach
	 * reuses or acquires BP's parent.  So the cells are recorded here, in
	 * the required order, on the still-pristine live list.  No structural
	 * fold above published anything.
	 *
	 * All SIX edges ride the ONE commit (plan Q3):
	 *  - the src unsplice's two neighbour back-edges;
	 *  - the dst splice's two neighbour back-edges;
	 *  - the run's own two OUTER links.
	 * The outer links go through the same-trie
	 * ft_ord_cell_run_resplice_edges instead of the cross-trie
	 * ft_ord_cell_run_splice_edges.  The cross-trie form would PLAIN-STORE
	 * them.  A plain store is wrong for a LIVE run.  It publishes the dst
	 * neighbours before the boundary flip and survives an abort.  That
	 * permanently breaks the back-edges.  Cell edges are always MW.  So a
	 * peer's conflicting splice aborts this commit clean and the caller
	 * re-descends.
	 */
	if (ft->ordered_list) {
		/*
		 * RULE C (ft_dlm_acquire_set_at): the cell takes below are the
		 * op's class-1 take, so the glue's class-0 takes come first.
		 * Those are its reparent marks and its publish parent, which
		 * the ft_glue_txn_commit_edges fold further down takes.  A
		 * refusal skips the cells and leaves the fold to report it,
		 * down its own bail.
		 */
		if ((prep == FT_GRAFT_PREP_GLUE || merge_dst) &&
				ft_glue_lock_anchors(ft, &glue) !=
					URCU_TXN_STATUS_OK)
			goto cells_done;
		/* Zero-initialised: see the note on ms_edges in ft-merge.h. */
		struct ft_ord_cell_edge cedges[FT_ORD_CELL_RUN_DETACH_MAX_EDGES +
			FT_ORD_CELL_RUN_RESPLICE_MAX_EDGES] = { 0 };
		struct ft_ord_cell *rfc, *rlc, *src_pred, *src_succ;
		unsigned int cn;

		/*
		 * DISTINCT-SLOT precondition.  It is the engine's
		 * (rcu-txn-mcas.h): a txn's records must target
		 * pairwise-distinct slots.  Two of the six edges coincide iff
		 * a dst neighbour IS a src neighbour:
		 *  - dst_pred == src_pred puts two records on
		 *    &src_pred->lnode.next;
		 *  - dst_succ == src_succ puts two on &src_succ->lnode.prev.
		 * The endpoint-adjacency guard does NOT exclude that.  The dst
		 * pair was derived earlier than these src reads.  A peer move
		 * can have shifted the neighbourhood in between.  MEASURED at
		 * 1.5-2% of commits before the move counter pinned the window
		 * shut.  ft_flip_txn_create's expect_conflict makes the engine
		 * RECONCILE rather than corrupt (it poisons -> clean abort).
		 * But relying on that is relying on a fallback.  Bail
		 * explicitly instead, so the distinctness the engine requires
		 * holds BY CONSTRUCTION.  Clean -EAGAIN: the txn has recorded
		 * structural edges but published nothing.  So the bail below
		 * unwinds exactly like the other pre-commit bails.
		 */
		rfc = ft_ord_cell_ptr(rcu_dereference(run_rfirst->prev));
		rlc = ft_ord_cell_ptr(rcu_dereference(run_rlast->prev));
		src_pred = ft_ord_cell_resolve_ord(&rfc->lnode.prev);
		src_succ = ft_ord_cell_resolve_ord(&rlc->lnode.next);
		if (run_keeps_pos) {
			/*
			 * The run keeps its ordered position (the dst abuts
			 * it), so there is nothing to splice.  But "nothing
			 * changed" still has to be ASSERTED at the decide.
			 * Between the derivation above and this commit, a
			 * peer can splice a key into the run's own boundary.
			 * The new keys would then sort on the wrong side of
			 * it.  The list is left well-formed and no longer
			 * key-ordered, with no edge of this commit noticing.
			 * On the moving path the six edges are what notices.
			 * Here two VALIDATE edges (old == new) on the run's
			 * OUTER links do it.  So such a peer aborts this
			 * commit clean and the caller re-derives.
			 *
			 * The edges are on the run's own links rather than
			 * on A's and B's.  A peer may REMOVE a neighbour.  An
			 * edge on a dying cell's slot is worse than one on a
			 * cell this move already owns.  The two slots are
			 * distinct by construction: ->prev and ->next are
			 * different fields even for a one-cell run.
			 *
			 * ☞ ONLY WHERE A PEER CAN EXIST.  Trie-wide exclusion
			 * means one of:
			 *  - a coarse or exclusive trie;
			 *  - a bulk window holding the fine trie's FT-wide
			 *    writer lock.
			 * Under it no writer can splice into the run's
			 * boundary, so the validates have nothing to catch.
			 * And a validate is a CAS that stays MW even there
			 * (a park cannot fail).  MEASURED before
			 * (FT_DEBUG_MW_KEPT, ft_inv MW per run): these two
			 * were the whole bulk-window residue, CELL ~24k.
			 * -DFT_DEBUG_REKEY_KEEP_POS_VALIDATE records them
			 * anyway.
			 */
#ifndef FT_DEBUG_REKEY_KEEP_POS_VALIDATE
			if (ft_flip_txn_excludes_all(txn, ft))
				goto cells_done;
#endif
			cedges[0].tag = URCU_TXN_TAG;
			cedges[0].slot = (struct ft_ord_cell **) &rfc->lnode.prev;
			cedges[0].old_target = src_pred;
			cedges[0].new_target = src_pred;
			cedges[1].tag = URCU_TXN_TAG;
			cedges[1].slot = (struct ft_ord_cell **) &rlc->lnode.next;
			cedges[1].old_target = src_succ;
			cedges[1].new_target = src_succ;
			ft_ord_cell_record_into_ft(ft, txn, cedges, 2);
			goto cells_done;
		}
#ifdef FEATURE_FT_MERGE
		if (run_interleaves) {
			/*
			 * INTERLEAVE: every relink is recorded, so the cells
			 * reorder with the structural publish in the one
			 * commit.  The edge set is unbounded in the run
			 * length.  Hence it is heap-allocated and freed here.
			 * A refusal is a clean pre-commit bail like the
			 * distinct-slot one below.
			 */
			struct ft_ord_cell_edge *iedges = NULL;
			unsigned int in = 0;
			int iret = ft_rekey_ord_interleave(ft, probe_D, dst_len,
					src_len, run_rfirst, run_rlast,
					merged_keys, &iedges, &in,
					&collided_cells, &nr_collided);

			if (!iret && !ft_flip_txn_reserve_extra(txn, in)) {
				free(iedges);
				iedges = NULL;
				iret = -ENOMEM;
			}
			if (iret) {
				pp_meta = NULL;	/* ft_glue_abort: single owner */
				if (detach_rc.new_flag)
					free_cds_ft_node_unpublished(ft,
						ft_node_ptr(detach_rc.new_flag));
					ft_rekey_collapse_free_unpublished(ft,
					&detach_rc.collapse);
				ft_glue_abort(ft, &glue);
				if (src_glue_live) {
					ft_glue_abort(ft, &src_glue);
					src_glue_live = false;
				}
				/* NULL on the merge path: no COW.  AFTER the glue's fences:
				 * see the store bail's ☠ S_top' note. */
				ft_rekey_free_stop_prime(ft, s_top_prime);
				ft_flip_txn_destroy(txn);
				/*
				 * ☠ FREE AFTER THE REGISTRY SWEEP, NEVER BEFORE IT.
				 * @gst_st.dest carries a fence this attempt
				 * registered on @txn (the born-locked
				 * relocation acquire).  ft_flip_txn_destroy's
				 * sweep RELEASES it through the node's own
				 * metadata.  Freeing first hands that word to
				 * the allocator.  The allocator can refund the
				 * item into the active reserve and re-zero its
				 * metadata on the next draw.  The release then
				 * lands on a cleared word (assert) or on a
				 * LIVE peer's word (corruption).
				 */
				if (gst_st.old_recompacted_node)
					free_cds_ft_node_unpublished(ft,
						ft_node_ptr(gst_st.dest));
				ret = iret;
				goto sweep;
			}
			/*
			 * ☞ The interleave's edge set is built dynamically
			 * and names an arbitrary number of cells.  So the
			 * generic take is the only form that fits: lock what
			 * the edges name.  There is no re-derive here.  The
			 * builder consumed the plan it was given, so there
			 * is nothing to derive again.  A peer that moved a
			 * cell in between is caught by the edges' own
			 * expected-old at the commit.
			 */
			if (ft_cell_lockset_take_edges(ft, NULL, txn, iedges,
					in)) {
				free(iedges);
				goto cells_bail;
			}
#ifndef FT_DEBUG_REKEY_KEEP_POS_VALIDATE
			/*
			 * The interleave's VALIDATE edges (old == new) guard
			 * against a peer splicing in between.  Under
			 * trie-wide exclusion there is no such peer, and a
			 * validate stays a CAS even there.  This is the
			 * keep-position arm's reason and the same knob.  Its
			 * writes are kept, in order.
			 */
			if (ft_flip_txn_excludes_all(txn, ft)) {
				unsigned int ii, io = 0;

				for (ii = 0; ii < in; ii++)
					if (iedges[ii].old_target !=
							iedges[ii].new_target)
						iedges[io++] = iedges[ii];
				in = io;
			}
#endif
			ft_ord_cell_record_into_ft(ft, txn, iedges, in);
			free(iedges);
			goto cells_done;
		}
#endif /* FEATURE_FT_MERGE: an occupied dst is a merge */
		if (src_pred == ft_ord_or_sentinel(ft, run_dpred) ||
				src_succ == ft_ord_or_sentinel(ft, run_dsucc)) {
			pp_meta = NULL;		/* ft_glue_abort: single owner */
			if (detach_rc.new_flag)
				free_cds_ft_node_unpublished(ft,
					ft_node_ptr(detach_rc.new_flag));
			ft_rekey_collapse_free_unpublished(ft,
				&detach_rc.collapse);
			ft_glue_abort(ft, &glue);
			if (src_glue_live) {	/* merged cluster's src side */
				ft_glue_abort(ft, &src_glue);
				src_glue_live = false;
			}
			/* NULL on the merge path: no COW.  AFTER the glue's fences:
			 * see the store bail's ☠ S_top' note. */
			ft_rekey_free_stop_prime(ft, s_top_prime);
			ft_flip_txn_destroy(txn);
			/*
			 * ☠ FREE AFTER THE REGISTRY SWEEP, NEVER BEFORE IT.
			 * @gst_st.dest carries a fence this attempt registered
			 * on @txn (the born-locked relocation acquire).
			 * ft_flip_txn_destroy's sweep RELEASES it through the
			 * node's own metadata.  Freeing first hands that word
			 * to the allocator.  The allocator can refund the
			 * item into the active reserve and re-zero its
			 * metadata on the next draw.  The release then lands
			 * on a cleared word (assert) or on a LIVE peer's word
			 * (corruption).
			 */
			if (gst_st.old_recompacted_node)
				free_cds_ft_node_unpublished(ft,
					ft_node_ptr(gst_st.dest));
			ret = -EAGAIN;
			goto sweep;
		}

		/* src unsplice FIRST: reads the run's pristine outer links -> src neighbours. */
		cn = ft_ord_cell_run_detach_edges(ft, run_rfirst, run_rlast,
				&rfc, &rlc, cedges, 0);
		/*
		 * dst splice into the gap located up front on the pristine
		 * list.  It records the run's outer links rather than storing
		 * them, so the whole move is atomic and abort-clean.  All six
		 * slots are distinct per the check above.
		 */
		{
			/*
			 * ☞ THE SIX CELL WORDS THIS MOVE WRITES:
			 *  - the SOURCE pair (the detach above);
			 *  - the run's two ends;
			 *  - the DESTINATION pair (the resplice below).
			 * They are the same six the comment calls distinct.
			 * One take, because two class-1 takes in sequence are
			 * not one ordered set.
			 */
			struct ft_cell_plan plan = {
				.cell = rfc, .last = rlc,
				.pred = ft_ord_cell_resolve_ord(
					&rfc->lnode.prev),
				.succ = ft_ord_cell_resolve_ord(
					&rlc->lnode.next),
				.pred2 = ft_ord_or_sentinel(ft, run_dpred),
				.succ2 = ft_ord_or_sentinel(ft, run_dsucc),
			};

			if (ft_cell_lockset_take(ft, NULL, txn, &plan))
				goto cells_bail;
		}
		cn = ft_ord_cell_run_resplice_edges(ft, rfc, rlc, run_dpred,
				run_dsucc, cedges, cn);
		ft_ord_cell_record_into_ft(ft, txn, cedges, cn);
cells_done:
		;
	}

	/*
	 * 3c. GLUE shape only: record the split cluster's publish into the
	 * SAME txn.  The publish is:
	 *  - its hidden back-pointers (plain stores on unpublished nodes);
	 *  - the displaced child's live re-parent;
	 *  - the forward edge into the fenced publish parent;
	 *  - @cn's retire;
	 *  - the count walk.
	 * It runs LAST, after the detach and cell folds.  It is the step that
	 * does live bookkeeping, and it is specified to be called where abort
	 * is impossible.  Every arm that can still bail is above it.  The
	 * NOSPLIT shape is the other way round.  Its prepare/commit is where
	 * its recompaction ACQUIRES the lock set the detach then reuses, so
	 * it has to run first.  Under record_only this records and returns OK
	 * without committing.  The caller's one commit below publishes it.
	 * From here the txn registry, not this function, owns the publish
	 * parent's fence.
	 */
	if (prep == FT_GRAFT_PREP_GLUE || merge_dst) {
		/*
		 * The status is CHECKED, not discarded.  The fold's re-parent
		 * mark acquire runs inside this call, before it records
		 * anything.  It CAN miss on a contended child.  Discarding
		 * that would carry on to the commit below with children this
		 * op does not hold, and SW-park their state words unexcluded.
		 * That is precisely the silent-success shape (ignored commit
		 * status, op reports OK) that cost a whole source subtree
		 * once already.
		 *
		 * The miss is clean.  It happens before the first record and
		 * before ft_glue_tombstone_free_list.  So nothing of this
		 * cluster is in @txn and NOTHING is published.  The txn still
		 * carries only the detach and cell records, which this bail
		 * discards with it.  @pp_meta ownership has NOT
		 * transferred (that happens by recording), so we still owe its release
		 * and must not NULL it here.
		 */
		if (ft_glue_txn_commit_edges(ft, &glue, NULL, 0) !=
				URCU_TXN_STATUS_OK) {
			/*
			 * ☠ DO NOT release @pp_meta here.  An earlier version
			 * did.  It reasoned that a failure could only come
			 * from the mark acquire at the top of
			 * ft_glue_txn_commit_edges.  That acquire is before
			 * any record, so before ownership passes to the txn.
			 * That is an ARGUMENT about which failure happens,
			 * and under a SHARED destination it is false.
			 * commit_edges also fails AFTER routing the publish
			 * parent through the txn.  That routing NULLs
			 * g->publish_parent_holder and takes the fence with
			 * it.  Clearing it then asserts on an unheld word.
			 *
			 * ft_glue_abort below is the choke point.  It already
			 * does the right thing either way: clear_IF_HELD,
			 * guarded on the holder field the transfer NULLs.
			 * Just disown it so bail_build's own clear cannot
			 * double up.
			 */
			pp_meta = NULL;
			if (detach_rc.new_flag)
				free_cds_ft_node_unpublished(ft,
					ft_node_ptr(detach_rc.new_flag));
			ft_rekey_collapse_free_unpublished(ft,
				&detach_rc.collapse);
			ft_glue_abort(ft, &glue);
			if (src_glue_live) {	/* merged cluster's src side */
				ft_glue_abort(ft, &src_glue);
				src_glue_live = false;
			}
			/* NULL on the merge path: no COW.  AFTER the glue's fences:
			 * see the store bail's ☠ S_top' note. */
			ft_rekey_free_stop_prime(ft, s_top_prime);
			ft_flip_txn_destroy(txn);
			ret = -EAGAIN;
			goto sweep;
		}
		pp_meta = NULL;		/* ownership transferred to @txn */
		/*
		 * The merged cluster's SRC side has no publish of its own.
		 * It has only retires, S_top's among them.  So it never goes
		 * through ft_glue_txn_commit_edges.  Record its freezes into
		 * the shared txn directly, here.  This is on the committing
		 * path and after the last bail above.  It is exactly where
		 * the dst side's own tombstone step just ran.
		 */
		if (src_glue_live)
			ft_glue_tombstone_free_list(&src_glue);
		/*
		 * The dup-chain appends.  They are recorded, not stored, so
		 * the src duplicates become reachable ATOMICALLY with the
		 * merged structure.  A collided key never momentarily shows
		 * only its dst side.  They come after the cluster's edges,
		 * exactly as ft_merge_spine_copy orders it.
		 */
#ifdef FEATURE_FT_MERGE
		if (merge_dst) {
#ifdef FT_DEBUG_SPLICE_SEAM
			uatomic_inc(&ft_ss_site_rekey_atomic_calls);
			if (glue.nr_splices)
				uatomic_inc(&ft_ss_site_rekey_atomic);
#endif
			ft_glue_record_splices(ft, &glue, txn);
		}
#endif
	}

	/*
	 * ☠ THE MOVED HEAD'S KEY CHANGED.  So its cell's edge byte is
	 * re-stamped HERE from the DESTINATION KEY, for BOTH arms and EVERY
	 * landing.
	 *
	 * @incoming_byte's invariant is "the last byte of this head's KEY".
	 * INSERT establishes it that way unconditionally (ft-insert.h,
	 * `= key[key_len - 1]`).  It does so for a PREFIX head just as much
	 * as for a slot head, even though the up-walk reads it only for the
	 * latter.  Every KEY-INVARIANT re-home downstream RELIES on that:
	 *  - ft_park_live_parent_edge (a split);
	 *  - the remove's DETACH_UNCHAIN and DETACH_CN_PARENT promote;
	 *  - ft_record_child_back_edge.
	 * Each moves a head between a prefix position and a body slot,
	 * carries the byte along, and re-derives nothing.  That is correct,
	 * because the key did not change.
	 *
	 * ft_head_stamp_incoming_byte cannot serve this one.  It derives the
	 * byte from (@parent_nf, @slot).  That answers the question only for
	 * a BODY SLOT.  For a prefix landing the head's key is path(parent),
	 * and its last byte is NOT derivable there.  The parent's own
	 * @incoming_byte is documented UNUSED under a compressed grandparent
	 * (ft_set_parent).  So the generic placement primitive would have to
	 * climb.  This site has the answer outright.  That is exactly why
	 * the two CROSS-TRIE sites stamp `okey_dst[dst_key_len - 1]`
	 * directly (ft-merge.h, ft_rekey_subpos_inplace) instead of going
	 * through a placement helper.
	 *
	 * ☞ AND IT IS A PLAIN STORE ON PURPOSE.  A rekey gives its moved top
	 * a FRESH ADDRESS (ft_rekey_cow_stop).  That makes every back-edge
	 * write on it a plain store into something invisible.  It also
	 * removes any need to put this byte in the txn.  The BARE EXTERNAL
	 * HEAD is the one top that gets no copy, because it is app-owned.
	 * So here alone the store is reader-visible ahead of the publish.
	 * @payload_live already routes the head's PARENT word through the
	 * txn for that reason.  There is no such route for a byte.  The cure
	 * is not a transacted byte.  It is the same rule one level down: the
	 * head's ORDERED CELL is library-allocated and CAN be copied.
	 * ☞ ft_head_stamp_incoming_byte's header.
	 *
	 * ☠ MEASURED, all four builds.  The SLOT-only stamp is what left it:
	 *     insert "q","am","an","z1";  rekey_merge(dst "z", src "q")  -> OK
	 *     remove "z1"                       -> count 3, cds_ft_verify CLEAN
	 *     the walk then emits "q", which is NOT IN THE TRIE
	 * The head lands as the PREFIX head of z1's holder and keeps 'q'.
	 * The remove's promote carries it into a body slot where the walk
	 * reads it.  This is the same shape as the compressed-parent case
	 * one commit earlier.  The byte is unread AT THE LANDING and read
	 * after the next ordinary mutation.  So "does this landing read it"
	 * is never the right question.
	 */
	if (s_top_external && ft->ordered_list) {
		void *head_prev = ft_dereference_prev_resolved(
			(struct cds_ft_node *) s_top);

		/*
		 * ☠ ONLY A DUPLICATE-CHAIN HEAD OWNS A CELL.  The order links
		 * live in a cell "hung off each duplicate-chain head's
		 * cds_ft_node.prev" (fractal-trie-internal.h,
		 * @ordered_list_set).  A head that COLLIDED with a key already
		 * at the destination is spliced onto that key's chain and
		 * DEMOTED.  Its prev then names its chain predecessor, not a
		 * cell.  It needs no byte either: the walk materialises the
		 * whole chain from the CHAIN HEAD's cell.
		 *
		 * FT_ORD_CELL_TAG decides it and is unambiguous by
		 * construction.  The cell arena is
		 * 1<<FT_ORD_CELL_ALLOC_ORDER-aligned, so a cell keeps bits 0-2
		 * clear.  A leaf pointer has bit 0 clear.
		 *
		 * MEASURED without this:
		 *     {"ab" a bare head, "b" already a key}
		 *     rekey_merge(dst "b", src "ab")
		 * SEGVs in cds_ft_item_to_metadata.  prev was the predecessor
		 * LEAF (tag 0), read as a cell.  A 3000-shape corpus walk
		 * oracle found it, not the unit tests.
		 */
		/*
		 * ☠ AND NOT FOR A HEAD THIS COMMIT DEMOTES.  The tag test used
		 * to decide that by itself.  The splice STORED the demotion
		 * (prev = its chain predecessor, a leaf) at prepare time.  Now
		 * the demotion is a RECORD, so prev still resolves to the cell
		 * here and the tag test passes.  Stamping that cell would put
		 * the DST key's byte on a head that stays a head at its SOURCE
		 * key if this commit aborts.  The walk then emits a key not in
		 * the trie.  The splice list is the record of who is demoted.
		 */
		bool demoted = false;
		int si;

		for (si = 0; si < glue.nr_splices; si++)
			if (glue.splices[si].src_head ==
					(struct cds_ft_node *) s_top)
				demoted = true;
		if (!demoted && head_prev &&
				((uintptr_t) head_prev & FT_ORD_CELL_TAG))
			cds_ft_item_to_metadata(ft_ord_cell_ptr(head_prev))
				->incoming_byte = dst_ord[dst_len - 1];
	}
	/* 4. ONE commit of the whole stitch (consumes txn). */
#ifdef FEATURE_FT_FAULT_INJECT
	/*
	 * Test-only: abort this commit exactly as a peer winning a raced MW slot
	 * would.  Without it the fold's abort branch below is DEAD, and not
	 * for want of contention.  The shared-destination merge oracle drove
	 * 62342 publish-parent fence misses and 1079 commit_edges misses over
	 * 71218 merges.  It still took this exit 0 times, because every
	 * acquire is AHEAD of the commit and turns the peer away first.
	 * @acquire_miss is the engine's own discard-unpublished route.  So
	 * what runs below is the real unwind, not a synthesised status.
	 */
	if (merge_dst && cds_ft_fault_commit_countdown >= 0) {
		if (cds_ft_fault_commit_countdown == 0) {
			cds_ft_fault_commit_countdown = -1;
			txn->acquire_miss = true;
			FT_MISS_NOTE(txn, __func__, __LINE__);
		} else {
			cds_ft_fault_commit_countdown--;
		}
	}
#endif
	/*
	 * The marks' anchor releases.
	 *
	 * `marks_consumed` below claims the commit consumed every mark.  Its
	 * two justifications are a child's {live_state -> live_state}
	 * re-parent edge and @stop's retire.  Both are claims about each
	 * NODE's OWN word.  Under a coarse spacing the word the acquire TOOK
	 * is the node's ANCHOR, and no node terminal touches it.  Without
	 * this the LOCK survives the commit and leaks.  The next op to anchor
	 * there then refuses it forever, and a later release of it asserts.
	 *
	 * It is placed here because this is past the LAST acquire: @nr_marks
	 * is still growing up to the displaced-child mark above.  It is NOT
	 * placed here for ordering.  ft_flip_txn_record_anchor_release_held
	 * reads the word through the txn.  So it chains onto whatever the op
	 * has already recorded there, and is order-independent by
	 * construction.  Measured: recording it right after
	 * ft_rekey_cow_stop is equally green.  It is self-guarding too.  An
	 * UNcoarsened mark, whose node terminal IS its release, records
	 * nothing.  The default granularity stays byte-identical.
	 */
	for (i = 0; i < nr_marks; i++) {
		if (marks[i].shared)
			continue;	/* an earlier acquire owns its release */
		if (!ft_flip_txn_reserve_extra(txn, 1)) {
			ret = -ENOMEM;
			goto bail_build;
		}
		ft_flip_txn_record_anchor_release_held(txn, marks[i].lock);
	}
	/*
	 * THE CLAIM AT THE CHOKE POINT.  Every branch of this writer reaches this
	 * commit, and every branch of it parks SW.  An un-armed one aborts
	 * here forever with no peer involved.  The arm cannot live at this
	 * point, but the claim can.  The arm was once moved onto a branch one
	 * caller never visits.  The missing assert is what turned that
	 * deterministic livelock into a silent hang.
	 */
	assert(txn->structural_sw);
	st = ft_flip_txn_commit(ft, txn);
	if (st == URCU_TXN_STATUS_OK) {
		/*
		 * The commit CONSUMED every mark in @marks.  Each child's
		 * release is ft_reparent_record_meta's
		 * {live_state -> live_state} state edge.  live_state has LOCK
		 * masked, and the edge is recorded unconditionally.  @stop's
		 * release is its retire.  So the sweep below must NOT run: see
		 * its own comment.
		 */
		marks_consumed = true;
		/* The copy loop published it into @graft_c's fresh body. */
		fold_rest_published = true;
		/*
		 * SCRUB the frames that listed those marks.  The commit
		 * consumed their fences and freed the txn's descriptor.  An
		 * entry left answering holds() is finding A's RECORD-LESS
		 * stale frame.  The glue keeps acquiring below, and a stale
		 * "held" there is the measured exponential-MW livelock.
		 */
		glue.caller_holder = NULL;
		glue.publish_gp_holder = NULL;
		{
			int sc_i;

			for (sc_i = 0; sc_i < nr_marks; sc_i++)
				if (marks[sc_i].txn_owned &&
					!(CMM_LOAD_SHARED(
						marks[sc_i].lock->state) &
						FT_STATE_TOMBSTONE))
					marks[sc_i].shared = true;
		}
		ft_glue_free_old(ft, &glue);		/* graft old copies */
		/*
		 * The merged cluster's SRC side.  Its free list holds S_top
		 * itself and any src overlap node the build copied, all
		 * retired by this commit.  That is what makes the
		 * cds_ft_free_item_deferred below a MERGE-path double free.
		 * The merge already owns S_top's reclaim, so only the cow_stop
		 * path still owes it.
		 */
		if (src_glue_live)
			ft_glue_free_old(ft, &src_glue);
		/*
		 * Chain holders are NOT in the txn registry, so the commit
		 * does not consume them.  Release them here, the committed
		 * path's own point.  Every non-committed path reaches
		 * ft_glue_abort, which owns the same release.  That is the
		 * choke point, not the exits that happen to be visible.
		 */
		ft_glue_release_splice_holders(&glue);
		/*
		 * The reserve recompaction's relocated old dst-parent copy.
		 * Its retire committed with the flip (deferred past readers
		 * via the recompact's fenced tombstone).  So reclaim it now.
		 * This mirrors ft_store_at_graft_point_commit's own
		 * post-commit free (ft-graft.h).
		 */
		if (gst_st.old_recompacted_node)
			free_cds_ft_node(ft, gst_st.old_recompacted_node);
		if (detach_rc.old_node)
			free_cds_ft_node(ft, detach_rc.old_node);	/* old BP copy */
		/*
		 * The dst publish parent's OLD body (@dst_pp_old).  The COW
		 * the merge arm made of it superseded it.  cow_stop's record
		 * retired it and this commit unlinked it.  Nobody else owns
		 * it:
		 *  - not the glue's free list.  The copy is glue-BUILT and the
		 *    original never joins that list.  ft_glue_defer_free's
		 *    callers name src-side and D-side nodes only;
		 *  - not the detach.
		 * For the frame OFF the source path, no detach set is built
		 * from it at all.  For the frame ON it (the post-detach arm),
		 * the detach edits a SLOT of the frame.  It retires what hangs
		 * below that slot: its junction, a collapsed chain, runs.  It
		 * never retires the frame itself.  Had it relocated the frame,
		 * the pending publish would have folded into its copy.  This
		 * arm would then have built no copy of its own.  So
		 * @dst_pp_old is NULL exactly then.
		 */
		if (dst_pp_old)
			free_cds_ft_node(ft, dst_pp_old);
		/* The folded collapse's retired chain: this commit unlinked it. */
		ft_rekey_collapse_free_retired(ft, &detach_rc.collapse);
		/*
		 * The interleave's dropped heads' cells.  This commit is what
		 * routed every surviving link around them.  So it is what
		 * makes the grace-period defer inside ft_ord_cell_free cover
		 * the readers that still hold one.
		 */
		{
			unsigned long ci;

			for (ci = 0; ci < nr_collided; ci++)
				ft_ord_cell_free(ft, collided_cells[ci]);
		}
		/*
		 * Likewise an ELEVATING detach's orphan chain -- and the CLIMB
		 * FOLD's, which this op collected itself.  ☠ Its plan-locks live
		 * ON those nodes, so they are released FIRST: see the helper.
		 */
		ft_rekey_fold_sweep_orphan_marks(fold_held, &nr_fold_held,
			/*committed=*/ true);
		ft_rekey_detach_free_orphans(ft, &detach_rc);
		/*
		 * REPLACE's retired body.  It is not an orphan: the resting
		 * node SURVIVES as its fresh copy.  This is the copy the
		 * fold's recompaction superseded.  That recompaction's own
		 * record retired it and this commit unlinked it.
		 */
		if (fold_rest_old)
			free_cds_ft_node(ft, fold_rest_old);
		/*
		 * ...and the OLD-DIRECTION REPLACE's, for the same reason.  BP
		 * survives as the copy the split wired under the fresh suffix.
		 * This is the body that copy superseded.  The recompaction's
		 * own record inside the build retired it, and this commit
		 * unlinked it.  So it is reclaimed here rather than through
		 * the glue's free list.  That list owns what the SPLIT
		 * retired, not what a recompaction did.
		 */
		if (glue.old_dir_replace.old_body)
			free_cds_ft_node(ft, glue.old_dir_replace.old_body);
		/*
		 * Old S_top after the grace period -- but ONLY when nothing else
		 * already owns its reclaim.  TWO other owners exist:
		 *
		 *  - the MERGE retires S_top through @src_glue's free list, so
		 *    ft_glue_free_old above already owns it;
		 *  - a CUT source whose old direction was DROPPED.  There
		 *    S_top IS the compressed node the split retires.
		 *    @src_cut > 0 makes d_src.nf the run itself, which is what
		 *    arms the drop.  So the build put it on the GLUE free
		 *    list, which is again ft_glue_free_old's.
		 *
		 * Freeing it here as well call_rcu's ONE rcu_head TWICE.  The
		 * second enqueue self-links the wfcq node.  After that the
		 * call_rcu worker spins at 100% CPU and every later rcu
		 * barrier hangs.  This is INVISIBLE to the suite, to ASAN
		 * (arena-allocated) and to the DEBUG_COUNTERS balance
		 * (cds_ft_free_item_deferred bumps no counter).  The damage
		 * lands in the callback queue long after the op returns.
		 * Measured: one cut rekey, 3 s idle, then urcu_qsbr_barrier()
		 * never returns.
		 */
		/*
		 *  - and the BARE EXTERNAL HEAD is APP-OWNED.  It is not a
		 *    library node and it was never copied.  The very same
		 *    object is now live at the DESTINATION.  Reclaiming it
		 *    here would hand the application's node back to the arena
		 *    while the trie points at it.  @s_top_meta is NULL for it
		 *    in any case.  That is how this first showed itself: a
		 *    SIGSEGV in cds_ft_free_item_deferred on a NULL metadata,
		 *    one line in.  This is the second half of the "skip the
		 *    retire" duty the old bare-head refusal named.  Skipping
		 *    ft_rekey_cow_stop drops the RECORDED retire, and this
		 *    drops the post-commit RECLAIM.
		 */
		/*
		 * ☠ ...AND THE MERGE ONLY OWNS S_top WHEN IT STARTS AT THE RUN'S
		 * HEAD.  The @merge_dst term above rests on "the MERGE retires
		 * S_top through @src_glue's free list".  ft_merge_build defers
		 * it on exactly one condition, `S_comp && off_s == 0`, on both
		 * its fenced and unfenced arms.  A CUT SOURCE enters the merge
		 * with @off_s = @src_cut, which is non-zero BY DEFINITION.  So
		 * the src glue never takes the node, and the term hands it to
		 * an owner that declined it.  The third candidate,
		 * @glue.old_dir_dropped, is 0 on this path.  Nobody frees it.
		 *
		 * MEASURED with the drain oracle over the 3000-shape corpus on a
		 * -DDEBUG_COUNTERS build.  98 of 98 `merge_dst && src_cut`
		 * seeds leak exactly one compressed node.  0 of the 275 cut
		 * seeds that are NOT merges do: those take this free.  It is
		 * 100 per cent penetrant, and invisible to every other oracle.
		 * The keys are right and cds_ft_verify is clean on all of
		 * them.
		 *
		 * A cut is the one case where the merge's premise and this term
		 * disagree, so it is the one case added back.  No double free
		 * is possible, for the same reason.  The src glue's free list
		 * never received the node (@off_s > 0), so this is its only
		 * owner.  And this site is on the COMMITTED branch alone,
		 * which is what the rcu_head warning above is about.
		 */
		if ((!merge_dst || src_cut) && !s_top_external &&
				!(src_cut && glue.old_dir_dropped))
			cds_ft_free_item_deferred(ft, s_top_meta);
		ret = 0;
	} else {
		/*
		 * Abort (a peer won a raced MW slot): NOTHING published.  So
		 * reclaim every UNPUBLISHED fresh copy and abort the glue
		 * build.  The copies are:
		 *  - S_top';
		 *  - the graft's relocated dst-parent copy (@gst_st.dest);
		 *  - the detach's relocated BP copy (@detach_rc.new_flag).
		 * The retired old copies stay LIVE (their tombstones rolled
		 * back), so they are NOT freed here.  Unreachable under the
		 * single-writer contract, kept leak-free for future concurrent
		 * use.
		 */
		if (gst_st.old_recompacted_node)
			free_cds_ft_node_unpublished(ft, ft_node_ptr(gst_st.dest));
		if (detach_rc.new_flag)
			free_cds_ft_node_unpublished(ft, ft_node_ptr(detach_rc.new_flag));
		ft_rekey_collapse_free_unpublished(ft, &detach_rc.collapse);
		ft_glue_abort(ft, &glue);
			if (src_glue_live) {	/* merged cluster's src side */
				ft_glue_abort(ft, &src_glue);
				src_glue_live = false;
			}
		/* NULL on the merge path: no COW.  AFTER the glue's fences:
		 * see the store bail's ☠ S_top' note. */
		ft_rekey_free_stop_prime(ft, s_top_prime);
		ret = -EAGAIN;
	}
	ft_glue_fini(&glue);
	if (src_glue_live)
		ft_glue_fini(&src_glue);
	goto sweep;

cells_bail:
	/*
	 * ☠ A CELL TAKE REFUSED IN THE ORDERED-LIST STAGE OWES THE STAGE'S FULL
	 * UNWIND, not a bare `goto sweep`.  Its neighbours there are the
	 * adjacency refusal and the interleave collect's failure.  Each runs
	 * this same block inline.  The two takes added beside them (cell-list
	 * stages 3d, 4b) did not.  One skipped the txn destroy, and both
	 * skipped the glue aborts.  So a refusal left @txn open with the
	 * locks it registered, and the glue's fences, LOCKed for good.  This
	 * is the ft_inv 136 wedge's family: see ft_remove_take_refused.  Same
	 * order as the inline copies: the txn's registry sweep BEFORE
	 * @gst_st.dest is freed.
	 */
	pp_meta = NULL;		/* ft_glue_abort: single owner */
	if (detach_rc.new_flag)
		free_cds_ft_node_unpublished(ft, ft_node_ptr(detach_rc.new_flag));
	ft_rekey_collapse_free_unpublished(ft, &detach_rc.collapse);
	ft_glue_abort(ft, &glue);
	if (src_glue_live) {	/* merged cluster's src side */
		ft_glue_abort(ft, &src_glue);
		src_glue_live = false;
	}
	/* NULL on the merge path: no COW.  AFTER the glue's fences:
	 * see the store bail's ☠ S_top' note. */
	ft_rekey_free_stop_prime(ft, s_top_prime);
	ft_flip_txn_destroy(txn);
	if (gst_st.old_recompacted_node)
		free_cds_ft_node_unpublished(ft, ft_node_ptr(gst_st.dest));
	ret = -EAGAIN;
	goto sweep;

bail_build:
	/*
	 * Common unwind for every bail between the graft build and the point
	 * where the graft's own step has committed its records.  The build
	 * published nothing, so:
	 *  - drop the (possibly filled) node reserve;
	 *  - release the publish parent's fence if this function still owns
	 *    it.  Once ft_glue_txn_commit_edges has run, the txn registry
	 *    owns it, and clearing here would race a peer's re-mark;
	 *  - free the unpublished S_top' copy;
	 *  - let ft_glue_abort free the invisible cluster and release the
	 *    split node's fence (its single clear point).
	 * @marks is swept below, as on every path.
	 */
	cds_ft_alloc_reserve_drain(ft, &reserve);
	/*
	 * The publish-parent fence has EXACTLY ONE owner on this path:
	 * ft_glue_abort below.  @glue.publish_parent_holder is set in the
	 * same breath as @pp_meta.  ft_glue_abort clears it IF HELD.  The txn
	 * NULLs that field when a record transfers ownership.  A second clear
	 * here was right only while every bail above was known to still hold
	 * the fence.  Under a SHARED destination that stopped being true, and
	 * it asserted on an unheld word.  Disown and let the choke point do
	 * it.
	 */
	pp_meta = NULL;
	/*
	 * REACHED AFTER THE DETACH TOO: the mark-release reserve loop bails
	 * here.  So this label owes the same unpublished copies the
	 * post-detach @sweep bails free:
	 *  - the detach's relocated BP copy;
	 *  - a folded collapse's merged node.
	 * Both are zeroed on every path that reaches here BEFORE the detach.
	 * That makes one unconditional unwind correct for both halves.  The
	 * post-detach paths that free them themselves @goto sweep and skip
	 * this label, so neither can be freed twice.
	 */
	if (detach_rc.new_flag)
		free_cds_ft_node_unpublished(ft, ft_node_ptr(detach_rc.new_flag));
	ft_rekey_collapse_free_unpublished(ft, &detach_rc.collapse);
	ft_glue_abort(ft, &glue);
			if (src_glue_live) {	/* merged cluster's src side */
				ft_glue_abort(ft, &src_glue);
				src_glue_live = false;
			}
	/* NULL on the merge path: no COW.  AFTER the glue's fences:
	 * see the store bail's ☠ S_top' note. */
	ft_rekey_free_stop_prime(ft, s_top_prime);
	ft_flip_txn_destroy(txn);
	/*
	 * @gst_st.dest was LEAKED here: this bail is reachable with a relocated
	 * dest already built (the marks-reserve ENOMEM above).  @gst_st is
	 * zero-initialised, so the guard is safe on paths that never grafted.
	 * After the sweep, for the reason spelled out at the bails above.
	 */
	if (gst_st.old_recompacted_node)
		free_cds_ft_node_unpublished(ft, ft_node_ptr(gst_st.dest));

sweep:
	/*
	 * REPLACE's fresh copy, on every path the commit did NOT take.  It is
	 * placed here because all five bails reach this label and none of
	 * them owns it.  It is built before the reserve is activated, so the
	 * reserve drain does not cover it either.
	 */
	if (fold_rest_new && !fold_rest_published)
		free_cds_ft_node_unpublished(ft, ft_node_ptr(fold_rest_new));
	/*
	 * The collided-cell ARRAY belongs to this frame whatever the outcome.
	 * The CELLS in it are freed only on the committed path above.  An
	 * abort routed nothing around them, so they are still live and
	 * linked.  The free is here because EVERY path reaches this label.
	 * The commit and abort arms jump straight to it, so an unwind-only
	 * free would leak on success.
	 */
	free(collided_cells);
	collided_cells = NULL;
	/*
	 * Release every mark the commit did NOT consume.
	 *
	 * ONLY on paths that did not reach a successful commit.  This used to
	 * run unconditionally, on the reading that a consumed mark makes
	 * clear_if_held "a no-op".  That is true only with no peers.  After a
	 * successful commit each of these nodes is LIVE and CLEAN.  So a peer
	 * that re-marked one in the commit->sweep window has its fence
	 * CLEARED here.  That is fence theft, the same shape as the
	 * publish-parent holder's.  It is the precise thing
	 * ft_glue_release_reparent_marks' own contract ("call ONLY on paths
	 * that did NOT reach a successful commit") exists to avoid.  The fold
	 * is not concurrent yet, which is why this was invisible.  It has to
	 * be right before it is.
	 */
	/*
	 * ...and NEVER a mark the txn owns (@txn_owned).  Its clear rides
	 * ft_flip_txn_lock_release_all, whose release is the STRICT one.  A
	 * second clear here would leave that assert reading an already-clean
	 * word.
	 */
	/*
	 * The CLIMB FOLD's chain marks, for every path that did NOT reach the
	 * committed one.  That path swept them itself, before it freed the
	 * nodes they live on.  It left the count at zero, so this iterates
	 * nothing.  This sweep is UNCONDITIONAL where the @marks sweep below
	 * is not.  The reason is the one ft_detach_node's own orphan sweep
	 * states.  What is left here marks the orphan's OWN word, whose
	 * terminal is its fenced tombstone.  So a release cannot steal a
	 * peer's fence off it.
	 */
	ft_rekey_fold_sweep_orphan_marks(fold_held, &nr_fold_held,
		/*committed=*/ false);
	if (!marks_consumed)
		for (i = 0; i < nr_marks; i++)
			if (!marks[i].shared && !marks[i].txn_owned) {
				/* STRICT: @marks_consumed excludes the commit. */
				ft_meta_lock_release(marks[i].lock);
				/*
				 * SCRUB: the frame chain answers holds() from
				 * this array (ft_held_set_snap skips shared).
				 * A released entry left listed is the
				 * record-less stale frame of finding A.
				 */
				if (!(CMM_LOAD_SHARED(marks[i].lock->state) &
						FT_STATE_TOMBSTONE))
					marks[i].shared = true;
			}
	return ret;
}

/*
 * Retry wrapper: the escalation lane the fold never had.
 *
 * Every bail in the attempt above is abort-clean: the trie is byte-for-byte
 * as before.  So retrying is just calling again.  But calling again is not
 * enough on its own.  Progress under contention comes from AGING a PERSISTENT
 * handle.  ft_op_conflict() advances @optxn->retry.  Once it reaches the
 * fallback budget, the writer takes its FIFO turn on the trie's escalation
 * lane and commits without competition.  The caller's external "just call
 * again" loop produced a fresh handle per attempt.  A fresh handle resets
 * that age to zero every time, so the writer never qualifies and spins
 * instead.
 *
 * SCOPE: this arbitrates COMMITS.  It deliberately does NOT wrap the per-node
 * LOCK acquires.  An escalated acquirer would hold its FIFO turn while
 * spinning for a holder that is itself funnelled behind that turn.  That is
 * the circular wait documented at the FT-wide writer lock.  An acquire miss
 * stays a clean bail that re-descends.
 *
 * -EINVAL (shape) and -ENOMEM are terminal.  The transient contention codes
 * the attempt documents (-EAGAIN, -EIO) are what this loop absorbs.
 */
#ifdef FT_DEBUG_REKEY_RETRY_CAP
# include <stdio.h>
# include <stdlib.h>
# ifndef FT_REKEY_RETRY_CAP
#  define FT_REKEY_RETRY_CAP	50000
# endif
static __thread unsigned int ft_rekey_attempts;
#endif

static
int ft_rekey_graft_simple_locked(struct cds_ft *ft,
		const uint8_t *src_key, size_t src_len,
		const uint8_t *dst_key, size_t dst_len, bool require_empty)
{
	const struct rcu_flavor_struct *flavor = ft->group->flavor;
	struct ft_op_retry op_retry;
	/*
	 * The FIRST tick reports the code the PREVIOUS attempt returned.  On
	 * attempt one there is none.  So seed it rather than read an
	 * indeterminate local into a trace field.
	 */
	struct ft_op optxn;
	bool len_fits;
	int ret = 0;

	/*
	 * JOIN THE PEER EXCLUSION PROTOCOL.  This writer parks its structural
	 * edges SW (ft_flip_txn_set_structural_sw), and an SW park CANNOT FAIL:
	 * it does not arbitrate.  That is legal only where the op excludes every
	 * peer writer over those slots.  On a COARSE trie the protocol is the
	 * FT-WIDE MUTEX.  Every other writer takes it through exactly this
	 * scope.  This op used to take it nowhere, so its unfailable parks
	 * arbitrated against nobody.
	 *
	 * ☠ AND ON A FINE TRIE THIS SCOPE IS NOT INERT ANY MORE.  It used to say
	 * so ("ft_writer_lock_scope_enter returns early on @lock_fine"), and that
	 * was true before G5.25.  It is now false HERE IN PARTICULAR:
	 *  - the early return is conditioned on `!ft_bulk_active(ft)`;
	 *  - ft_bulk_active is a plain load of the trie-level @bulk_state
	 *    with NO self-exclusion;
	 *  - this op is itself a bulk holder by the time it arrives.  A
	 *    rekey enters the gate as FT_BULK_COHERENT (@bulk_gate_nr counts
	 *    "ALL bulk holders, rekey included");
	 *  - the PLACEMENT rule below puts the gate strictly before this
	 *    scope.
	 * So @bulk_state is non-zero, the early return does not fire, and this
	 * scope TAKES the FT-wide lock.
	 *
	 * That is the design, not an accident.  Bulk ops, rekey among them,
	 * mutually exclude on the FT-wide lock.  The bulk-in-progress state
	 * flips the point ops onto that same lock for the window.  The
	 * per-node DLM try-locks are what this op uses BENEATH that, not
	 * instead of it.
	 *
	 * PLACEMENT.  After the move gate, never before it.
	 * ft_move_gate_enter waits a GRACE PERIOD, and a peer parked on this
	 * mutex is an ONLINE, non-quiescent QSBR reader.  Holding the lock
	 * across that gate would be a writer waiting on the very readers it
	 * is blocking.  Both entries (_cds_ft_debug_rekey_graft_simple and
	 * ft_rekey_one_decide's dispatch) enter the gate before calling here,
	 * so this placement satisfies it for both.  The scope is around the
	 * WHOLE retry loop rather than each attempt.  That is the shape
	 * cds_ft_remove already uses, and it is safe for the same reason.
	 * @optxn parks QUIESCENT (below), so an escalated wait inside this
	 * scope does not stop the grace periods its peers need.
	 *
	 * No ft_writer_lock_gp_wait exists anywhere under the attempt: the
	 * two in this unit are on the STAGED path.  So nothing here drops and
	 * retakes the lock mid-scope.
	 */
	CDS_FT_SCOPED_WRITER(ft);

	/*
	 * ☠☠ REPAIR THE HINT BEFORE THE SHAPE GATE READS IT -- OR THE TRIE
	 * BRICKS.
	 *
	 * The raise at the bottom of this function is a CONSERVATIVE bound.
	 * Its own previous value is its input, so it ACCUMULATES.  With
	 * dst_len > src_len it stores cur + (dst_len - src_len) on EVERY
	 * committed move, referencing no key that exists.  MEASURED: a loop
	 * that rekeys the same two 3-byte keys drove max_used_key_len
	 * 0 -> 256 in 253 calls.  After that, ft_rekey_len_change_overflows
	 * refused every further unequal-length move FOREVER (ok=253
	 * overflow=147).  The longest key in the trie was 3 bytes the whole
	 * time.
	 *
	 * The bound cannot be made both cheap and tight.  len(K) for the
	 * moved subtree is not derivable in O(1) from anything the trie
	 * stores.  Any O(1) bound that is SOUND has to feed on the hint it
	 * just wrote.  So pay for the exact answer, and pay ONLY where the
	 * loose one would refuse.  That is one walk per saturation (~1 in
	 * 253 moves in the measurement above) instead of one per move.
	 * ft_recompute_max_used_key_len is the trie's own repair.
	 * cds_ft_recompute_stats documents it for exactly this ("when the
	 * caller needs an accurate max_used_key_len for a subsequent graft
	 * validation").  This call site simply stops making the caller ask.
	 *
	 * ☠ IT LOWERS, so it needs exclusion -- AND IT HAS IT ON BOTH LOCK
	 * MODES.  A reading of CDS_FT_SCOPED_WRITER alone does not suggest
	 * that.  On a COARSE trie that scope is the FT-wide mutex.  On a FINE
	 * trie it looks inert.  But this op entered ft_move_gate_enter first.
	 * So ft_bulk_active is TRUE, and ft_writer_lock_scope_enter RE-TAKES
	 * the FT-wide lock.  See fractal-trie-internal.h, G5.25: "while a BULK
	 * op is live, a FINE trie re-takes the FT-wide lock, so bulk and point
	 * writers arbitrate on one word again".  Verified under gdb on a
	 * default FINE trie at this call:
	 *     ft_wlock_held == ft, ft_wlock_depth == 1,
	 *     lock_fine true, bulk_state 1.
	 * A point writer either takes that lock too, or sampled the gate
	 * clear and is covered by its grace period.  So the walk sees every
	 * key.
	 *
	 * ☠☠ AND IT MUST NOT RUN ON EVERY CALL.  The predicate is ALSO true
	 * when the trie genuinely holds a long key, and a REFUSED move raises
	 * nothing.  So an unguarded repair re-walks the whole trie under that
	 * FT-wide lock for a refusal that was correct the first time.
	 * MEASURED at 300k keys plus one 255-byte key:
	 *  - 0.005 ms/call unrepaired, 232.6 ms/call repaired;
	 *  - four concurrent point inserters fell from 16,841/s to 562/s
	 *    waiting behind it.
	 * @max_used_key_len_walked is the discriminator.  A hint still at the
	 * value the last walk produced cannot have been inflated since.  So
	 * the refusal is TRUE and there is nothing to repair.
	 *
	 * Placement: OUTSIDE the retry loop, and OUTSIDE the txn and the
	 * per-attempt read pin.  The hint does not change under us, so
	 * re-walking per attempt would be pure cost.  The walk needs a read
	 * section of its own, because it dereferences.  It takes no grace
	 * period, so it cannot deadlock against the move gate the caller
	 * already passed.
	 */
	if (src_len != dst_len &&
			ft->group->key_len == CDS_FT_LEN_VARIABLE &&
			uatomic_load(&ft->max_used_key_len, CMM_RELAXED) !=
				uatomic_load(&ft->max_used_key_len_walked,
					CMM_RELAXED) &&
			ft_rekey_len_change_overflows(ft, src_len, dst_len)) {
		flavor->read_lock();
		(void) ft_recompute_max_used_key_len(ft);
		flavor->read_unlock();
	}
	/*
	 * WHERE THE HINT STILL REFUSES, ASK THE MOVED KEYS.  Past the repair
	 * the hint is exact for the TRIE.  A genuinely long key elsewhere
	 * still refuses a short subtree's move that fits: a false
	 * OVERFLOW_ERROR.  This is decided once, outside the retry loop.  The
	 * writer scope above keeps every other writer out of the trie for
	 * the whole call.  So the moved keys cannot change between attempts.
	 */
	len_fits = src_len != dst_len &&
		ft->group->key_len == CDS_FT_LEN_VARIABLE &&
		ft_rekey_len_change_overflows(ft, src_len, dst_len) &&
		!ft_moved_keys_overflow(ft, src_key, src_len, dst_len,
			ft->group->max_key_len);

#ifdef FT_DEBUG_REKEY_RETRY_CAP
	ft_rekey_attempts = 0;		/* per MOVE, not per thread lifetime */
#endif
	ft_txn_op_init(ft, &optxn);
	/*
	 * PARK THE ESCALATION QUIESCENT.  This op may do that; insert and
	 * remove may not.  urcu_txn_begin escalates, and therefore blocks,
	 * before it opens the txn's own read section.  So the only thing that
	 * could be pinned across that park is an RCU section the CALLER
	 * holds.  This entry has none:
	 *  - cds_ft_rekey_graft already forbids being called from a read
	 *    section (its move gate waits for a grace period);
	 *  - the per-attempt pin below is taken AFTER begin.
	 * A writer parked online is what stops every grace period in the
	 * process.  So quiescing here is what keeps a contended move from
	 * wedging the peers that wait on one.
	 */
	ft_op_set_park_quiescent(&optxn, 1);
	ft_op_retry_init(&op_retry, FT_OP_REKEY, src_key, src_len);
	for (;;) {
		/*
		 * The GENERIC cap, beside the rekey's own older one.  This is
		 * the arm that TRACES.  It fires the violation event and
		 * records the flight-recorder snapshot, so the window that led
		 * to the spin survives the spin.
		 */
		ft_op_retry_tick(ft, &op_retry, ret);
#ifdef FT_DEBUG_REKEY_RETRY_CAP
		/*
		 * A LIVELOCK DETECTOR.  It is cheap because of what -EAGAIN
		 * MEANS.  The code promises that retrying can help, i.e. that
		 * some PEER is responsible for the refusal.  So an attempt
		 * count that runs away is a self-refusal: a condition no
		 * re-descent can change.  The op will spin on it forever.
		 * Three of these have been found in this writer by hand, each
		 * presenting as a silent hang.  This turns the whole class
		 * into a loud failure without needing to know which site is
		 * at fault.
		 *
		 * The cap is deliberately far above any real contention.  A
		 * saturated multi-writer arm re-attempts single digits, so
		 * five figures cannot be a peer.
		 */
		if (caa_unlikely(++ft_rekey_attempts > FT_REKEY_RETRY_CAP)) {
			fprintf(stderr,
				"FT REKEY LIVELOCK: %u attempts on one move -- "
				"an -EAGAIN no re-descent can clear.  Single-"
				"threaded this is certain; under peers it is "
				"still far past any real contention.  Find the "
				"site by breaking on each -EAGAIN return in "
				"ft_rekey_graft_simple_attempt (full header path "
				"+ `set breakpoint pending on`); a refusal that "
				"is a property of the STRUCTURE owes "
				"FT_REKEY_UNCOVERED, not a retry.\n",
				ft_rekey_attempts);
			abort();
		}
#endif
		ft_op_begin(&optxn);
		/*
		 * PER ATTEMPT, not around the loop.  The pin exists to keep
		 * the nodes ONE attempt captures alive from descent through
		 * commit.  Each attempt re-descends.  So per-attempt is both
		 * sufficient and what leaves begin's park unpinned.
		 */
		flavor->read_lock();
		ret = ft_rekey_graft_simple_attempt(ft, src_key, src_len,
			dst_key, dst_len, require_empty, len_fits, &optxn);
		flavor->read_unlock();
		if (ret != -EAGAIN && ret != -EIO)
			break;
		/* Age the conflict, as cds_ft_replace does; the turn is forfeited. */
		ft_txn_attempt_bail(&optxn, true);
	}
	ft_op_end(&optxn);
	/*
	 * A COMMITTED move can LENGTHEN every key it carried (@dst_len >
	 * @src_len).  So raise the trie's high-water hint the same way
	 * ft_rekey_at_inner does for the staged path and cds_ft_merge_at for
	 * the cross-trie one.  The raise is bounded by the pre-move maximum.
	 * That is conservative in the only direction that matters: the hint
	 * may over-state, never under-state.  It is read AFTER the commit and
	 * stored unconditionally-if-greater.  So a peer that raised it
	 * further in between is not lowered.
	 *
	 * ★ WHY IT IS SAFE TO DO THIS AFTER THE ONE DECIDE rather than inside
	 * it.  No lookup / inequality / iteration path consumes the field.  So
	 * a reader between the flip and this store sees the moved keys and a
	 * stale hint, not an inconsistency it can act on.  The keys' own
	 * lengths are a property of where the subtree hangs, and that moved
	 * atomically.
	 *
	 * ☠ IT IS NOT AN UNCONSUMED HINT, though this comment said so.  THIS
	 * WRITER'S OWN unequal-length gate reads it four thousand lines up
	 * (ft_rekey_len_change_overflows).  So do the graft / merge overflow
	 * validations.  The store's input is its own previous output, so it
	 * ACCUMULATES.  See the repair at the top of this function, which
	 * keeps that accumulation from refusing every later move.
	 */
	if (ret == 0 && dst_len > src_len) {
		size_t cur = uatomic_load(&ft->max_used_key_len, CMM_RELAXED);
		size_t nm = cur > src_len ? dst_len + (cur - src_len) : dst_len;

		if (nm > cur)
			uatomic_store(&ft->max_used_key_len, nm, CMM_RELAXED);
	}
	return ret;
}

/*
 * The MOVE entry point: bracket the move in the mode gate, then run it under
 * our OWN read lock.
 *
 * Order matters and is the gate's whole purpose.  ft_move_gate_enter
 * publishes "expect a move" to readers and waits a grace period.  So every
 * reader still in a critical section has finished before the body below
 * mutates anything.  Readers that start after it see the gate and switch to
 * the coherent path.  A burst of concurrent moves pays ~one grace period in
 * total (they piggyback the first).
 *
 * CALLER CONTRACT (new, and inherent to the gate): a move BLOCKS on a grace
 * period.  So it must NOT be called from inside an RCU read-side critical
 * section: the GP would wait for the caller's own section.  This entry takes
 * the read lock the body needs itself, AFTER the gate.  The gate is entered
 * before the shape gates run, so a rejected move also pays the GP.  The
 * public entry (ft_rekey_one_decide's caller) cheap-checks the shape first
 * instead.
 *
 * An occupied destination MERGES here (@require_empty false).  That is what
 * the oracles driving this entry expect.  The graft-semantics refusal belongs
 * to the public entry that documents it.
 */
int _cds_ft_debug_rekey_graft_simple(struct cds_ft *ft,
		const uint8_t *src_key, size_t src_len,
		const uint8_t *dst_key, size_t dst_len)
{
	int ret;

	ft_move_gate_enter(ft);
	ret = ft_rekey_graft_simple_locked(ft, src_key, src_len, dst_key, dst_len,
			false);	/* pins per attempt: see the locked wrapper */
	ft_move_gate_exit(ft);
	return ret;
}

/*
 * The ATOMIC rekey, for the public entry points.  Move @src_key's subtree to
 * @dst_key as ONE decide, or report that this shape is not one it covers.
 *
 * Everything the caller needs to know is in the return code:
 *  - 0 committed the move atomically.  A reader sees the subtree at the
 *    source XOR the destination, with no instant where it is at neither.
 *    The staged writer (a committed detach, then a merge back) cannot
 *    provide that property;
 *  - FT_REKEY_UNCOVERED means the shape is outside this writer's cut and the
 *    caller should fall back;
 *  - -EINVAL is an ARGUMENT error and is terminal.  These two codes were one
 *    code once, and this sentence still said so long after they were split.
 *    See the three-kinds-of-refusal header above FT_REKEY_UNCOVERED;
 *  - -EEXIST is a GRAFT caller's occupied destination;
 *  - -ENOMEM and -ENOTSUP are terminal.
 * The transient contention codes never surface: the retry wrapper absorbs
 * them.
 *
 * It takes the READ LOCK the body needs but NOT the move gate.  So one gate
 * bracket in the caller covers this attempt and any fallback.  Two brackets
 * would pay two grace periods for one move.  The caller must therefore
 * already hold the gate and, by the gate's own contract, not be inside a read
 * section.
 *
 * It has hidden visibility rather than static.  The public entries live in
 * ft-merge.h, which is included ABOVE the composition this wraps (it needs
 * detach, graft and merge all in scope).  The declaration in
 * fractal-trie-internal.h is what bridges that.
 */
__attribute__((visibility("hidden")))
int ft_rekey_one_decide(struct cds_ft *ft,
		const uint8_t *src_key, size_t src_len,
		const uint8_t *dst_key, size_t dst_len, bool require_empty)
{
	/*
	 * The caller contract this entry documents, now CHECKED.  The move
	 * gate its caller holds waits a grace period.  The retry loop below
	 * parks the escalation QUIESCENT.  A caller that is inside a read
	 * section breaks both.  Exact only for flavors whose read_ongoing()
	 * counts sections; see the macro.
	 */
	CDS_FT_ASSERT_RCU_NOT_READ_LOCKED(ft);
	/*
	 * JOIN THE WRITER-EXCLUSION PROTOCOL THE REST OF THE TRIE USES.
	 *
	 * This writer parks its structural edges SW (see
	 * ft_flip_txn_set_structural_sw below).  An SW park CANNOT FAIL,
	 * because it does not arbitrate.  So it is legal only where the op
	 * excludes every peer writer over those slots.
	 *
	 * FINE mode: the per-node DLM LOCK try-locks this op takes ARE that
	 * protocol.  CDS_FT_SCOPED_WRITER skips the FT-wide mutex there (the
	 * §11 drop).  So this costs nothing and changes nothing.
	 *
	 * COARSE mode: every OTHER writer excludes through the FT-WIDE MUTEX
	 * this macro takes.  These writers are insert, remove, graft and merge.
	 * Without joining that mutex, this op shared NO exclusion protocol
	 * with its peers:
	 *  - its per-node locks arbitrate against nobody, because in coarse
	 *    mode nobody else takes them;
	 *  - its unfailable SW parks would be lost updates against a
	 *    concurrent insert or remove.
	 * That is what the assert(ft->lock_fine && ...) in ft_rekey_cow_stop
	 * was protecting.  It did not protect the mechanism.  It protected the
	 * fact that only FINE mode had a protocol this writer already spoke.
	 *
	 * ORDER matches the other writers: FT-wide mutex first, escalation
	 * domain second (urcu_txn_begin, inside the retry loop below).  So the
	 * two-lock cycle those ops could otherwise form does not arise.  The
	 * mutex is taken AFTER the caller's ft_move_gate_enter.  So the gate's
	 * grace period is not waited for while holding the mutex.
	 */
	/*
	 * NO read lock here: ft_rekey_graft_simple_locked pins PER ATTEMPT,
	 * which is what lets its escalation park quiesce.  This used to wrap
	 * the loop instead.  That pinned the park, and a contended move could
	 * then wedge every peer waiting on a grace period.
	 */
	return ft_rekey_graft_simple_locked(ft, src_key, src_len, dst_key, dst_len,
			require_empty);
}

/* ------------- moved from ft-merge.h ------------- */

/*
 * MERGE-ONLY.  The spine copy UNIONS the source subtree into an OCCUPIED
 * destination.  That is a merge by definition.  It is built out of the merge
 * subsystem's own machinery (ft_merge_ctx, ft_merge_build,
 * ft_merge_ord_interleave_collect).  With -DNO_FEATURE_FT_MERGE only the
 * rekey GRAFT (the empty-dst shape) remains available.  So this whole arm
 * compiles out and its caller answers CDS_FT_STATUS_NOT_SUPPORTED, exactly as
 * ft_rekey_graft_simple_attempt already does for the same shape.
 */
#ifdef FEATURE_FT_MERGE
static
enum cds_ft_status ft_rekey_spine_copy(struct cds_ft *dst_ft,
		struct cds_ft *src_ft, struct ft_descent *d_src,
		const uint8_t *src_key, size_t src_key_len, unsigned long cnt_src,
		unsigned int off_src, struct ft_descent *d_dst,
		unsigned long cnt_dst, unsigned int off_dst,
		size_t dst_key_len,
		struct ft_flip_txn **pre_txn, bool *contended)
{
	struct ft_glue gd, gs;
	/* Set from the commit below; read by ft_glue_clear_fenced. */
	bool fenced_committed = false;
	struct ft_merge_ctx ctx = { .dst_ft = dst_ft, .gd = &gd, .gs = &gs };
	struct ft_merge_counts cnt = { 0, 0, 0, 0, 0 };

#ifdef FT_DEBUG_SPLICE_SEAM
	uatomic_inc(&ft_ss_spine_entry);
#endif
	bool root_src = (src_key_len == 0);
	bool ks_dst = (off_dst > 0);
	bool ed;
	struct cds_ft_inode_flag *S = d_src->nf;
	struct cds_ft_inode_flag *D = d_dst->nf;
	struct cds_ft_inode_flag *M, *M_slot = NULL, *pub, *D_old;
	struct cds_ft_inode_flag *pub_parent = d_dst->pnf, **pub_slot = d_dst->nfp;
	struct cds_ft_inode *fresh_root = NULL;
	struct cds_ft_metadata *fresh_meta;
	struct ft_flip_txn *txn;
	unsigned long merged_keys = 0;
	bool ms_ord = dst_ft->group->ordered_list_set;
	struct ft_ord_cell *ms_cursor = NULL, *ms_prev = NULL, *ms_succ = NULL;
	struct cds_ft_node *ms_s_first = NULL, *ms_s_last = NULL;
	struct ft_ord_cell_edge *ms_edges = NULL;
	struct ft_merge_src_cap *ms_src_caps = NULL;	/* src survivor suffixes, pre-captured */
	uint8_t *ms_src_pool = NULL;			/* packed suffix byte pool */
	unsigned long ms_nsrc = 0;			/* src run cells captured */
	unsigned int ms_cap = 0;	/* interleave edge cap, set once merged_keys is known */
	unsigned int ms_n = 0;		/* interleave edges collected (staged pre-commit) */
	/*
	 * The caller pre-reserved this op's flip-txn, i.e. it is already PAST its
	 * own point of no return and pre-built everything so this placement cannot
	 * fail.  Gates the dup-chain acquire below -- the only step here that can
	 * decline.
	 */
	bool unfailable = (pre_txn && *pre_txn);

	*contended = false;

	/*
	 * Every dst merge-point shape is handled:
	 *  - the flip proxies the publish slot @pub_slot;
	 *  - the read-side descent resolves the type-7 proxy at every child
	 *    fetch (ft_resolve_flip_proxy, before the skip handler);
	 *  - ft_glue_set_publish wires the published node's parent to
	 *    @pub_parent.
	 * So a descent and an up-walk see a coherent old-XOR-merged view
	 * across the flip.
	 *
	 * Edge D: the merge point's PARENT is a COMPRESSED node cn_p reached
	 * via a grandparent skip slot.  cn_p's own parent cannot be
	 * compressed ("no two adjacent compressed" invariant).  So the
	 * grandparent slot d_dst->pnfp is a plain internal slot that merely
	 * *holds* skip(cn_p).  Handle it one level up:
	 *  - the merge is EXACT at d_dst->nf (off_dst == 0);
	 *  - M is wrapped under a fresh copy of the WHOLE cn_p;
	 *  - that copy is published into d_dst->pnfp in place of skip(cn_p).
	 * This is identical machinery to a KEY_SHORTER dst wrap, just with
	 * cn_p as the wrapped node and the grandparent as the publish point.
	 * ks_dst and ed are mutually exclusive: a KEY_SHORTER merge point
	 * sits inside cn_d, whose parent is internal.
	 */
	ed = (d_dst->pnf &&
	      ft_node_compressed(ft_resolve_skip_compressed(dst_ft, d_dst->pnf)));

	/* Size both glues from a read-only pre-pass (with headroom). */
	ft_merge_count(dst_ft, S, off_src, D, off_dst, &cnt);
	ft_glue_init(&gd);
	ft_glue_init(&gs);
	/*
	 * The dst glue's own acquires (@pub_parent, above all) fire from
	 * commit helpers that never see a descent.  So hand them the
	 * destination one here, the single place holding both.  The graft
	 * does the same at its ft_glue_set_publish.  @pub_parent is @d_dst's
	 * own parent or grandparent, so the window dates it.  Without this
	 * the acquire has no depth under a coarse spacing.  Its miss then
	 * aborts a commit the unfailable arm cannot retry, which is a
	 * LIVELOCK, not a failure.
	 */
	gd.lock_d = d_dst;
	/*
	 * And the SOURCE glue's, for the same reason.  A src overlap fence is
	 * dated on the path the node is on NOW, which only @d_src describes.
	 * It is inert for a cross-trie merge: @fence_src stays false, because
	 * the source is exclusive.  It is set so the two glues answer from
	 * the same rule.
	 */
	gs.lock_d = d_src;
	/*
	 * Where relative depth 0 IS, on each side (see @dst_base_depth).
	 */
	ctx.dst_base_depth = (unsigned int) d_dst->depth + off_dst;
	ctx.src_base_depth = (unsigned int) d_src->depth + off_src;
	if (ft_glue_reserve(&gd, cnt.nb + 8, cnt.nd + 8,
				cnt.nf_dst + 8, cnt.ns + 8) ||
	    ft_glue_reserve(&gs, 0, 0, cnt.nf_src + 8, 0)) {
		ft_glue_fini(&gd);
		ft_glue_fini(&gs);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}

	/* Root src: pre-allocate the fresh empty root for the commit swap. */
	if (root_src) {
		fresh_root = alloc_cds_ft_node(src_ft, &ft_types[0], &fresh_meta);
		if (!fresh_root) {
			ft_glue_fini(&gd);
			ft_glue_fini(&gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		fresh_meta->parent_word = ft_trie_parent(src_ft);
		ft_nr_keys_store(src_ft, fresh_meta, 0, CMM_RELAXED);
	}

	/* Build the merged cluster invisibly (the only build-phase fallible step). */
	/*
	 * DLM overlap-spine plan-lock (§9.4 M-2).  Fence each dst overlap
	 * node before the build reads it, and retire it through the fenced
	 * terminal.  Then a peer growing a node this merge is copying cannot
	 * be silently retired with it.  Skipped for the unfailable caller,
	 * which has no bail left to take.
	 */
	ctx.fence_overlap = dst_ft->lock_fine && !unfailable;
	ctx.fence_src = false;		/* cross-trie: the source is exclusive */
	M = ft_merge_build(&ctx, S, off_src, D, off_dst, 0, d_dst->pdepth,
			&merged_keys);
	if (M == FT_MERGE_OOM) {
		if (fresh_root)
			free_cds_ft_node_unpublished(src_ft, fresh_root);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		/*
		 * A missed overlap fence shares FT_MERGE_OOM's unwind but is
		 * CONTENTION, not memory.  Both tries are pristine: the build
		 * published nothing and ft_glue_abort released every fence it
		 * took.  So report it on the retry channel and let the caller
		 * re-descend.
		 */
		if (ctx.overlap_contended)
			*contended = true;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	/*
	 * The ordered-list interleave is folded into the structural flip (one
	 * commit for structure + ordered list).  So its <= 2*merged_keys+2
	 * cell edges share the structural flip batch.  Each survivor run
	 * costs at most two visible edges.  Bound it now that @merged_keys is
	 * known.
	 */
	if (ms_ord)
		ms_cap = 2u * (unsigned int) merged_keys + 2u;

	/*
	 * Compute @pub (the value to publish) and @pub_parent / @pub_slot
	 * (where).  For the compressed shapes, also reclaim the replaced node.
	 *
	 * KEY_SHORTER dst (off_dst > 0): the merge point sits off_dst bytes
	 * inside the compressed node cn_d = D, whose prefix bytes lie above
	 * it.  Wrap M under cn_d->key_bytes[0..off_dst) and replace cn_d at
	 * its own slot (d_dst->nfp).
	 *
	 * Edge D (ed): the merge point's parent is the compressed node cn_p.
	 * Wrap M under the WHOLE cn_p and replace cn_p at the grandparent
	 * slot d_dst->pnfp (pub_parent = d_dst->ppnf).
	 *
	 * Either way reclaim the replaced compressed node.  ft_merge_build
	 * entered it (or, for Edge D, did not touch it) without recording the
	 * free.  ft_merge_wrap_prefix canonicalizes a single-child M and
	 * preserves the dst_origin of a compressed M's child.
	 *
	 * EXACT dst with an internal/absent parent (off_dst == 0, !ed): M
	 * itself replaces the subtree.  Canonicalize a non-root single-child
	 * internal merge top.  When S and D contribute exactly one shared
	 * byte (a churned "ab"-prefix shape), ft_merge_build returns M as a
	 * 1-child internal with no external_nodes.  That is forbidden at a
	 * non-root position under skip mode (chain-compress invariant;
	 * cds_ft_verify catches it at the merge depth).  Collapse it to a
	 * 1-byte compressed via the glue.  Only the TOP M can hit this.  Its
	 * lone child is always a FRESH recursion result.  So the deferred
	 * child edge carries dst_origin=false correctly and disturbs no flip
	 * edge.  A root dst merge point (d_dst->pnf == NULL) is exempt: a
	 * 1-child internal is canonical at the root.
	 */
	if (ks_dst || ed) {
		struct cds_ft_compressed_node *wrap_cn;
		uint8_t kbuf[FT_MAX_KEY_LEN];
		unsigned int wrap_depth, wrap_len;

		if (ed) {
			wrap_cn = ft_compressed_node_ptr(
				ft_resolve_skip_compressed(dst_ft, d_dst->pnf));
			wrap_len = wrap_cn->len;
			wrap_depth = (unsigned int) d_dst->depth - wrap_len;
			pub_parent = d_dst->ppnf;
			pub_slot = d_dst->pnfp;
		} else {	/* ks_dst */
			wrap_cn = ft_compressed_node_ptr(D);
			wrap_len = off_dst;
			wrap_depth = (unsigned int) d_dst->depth;
		}
		ft_glue_defer_free(&gd, wrap_cn, true);
		memcpy(&kbuf[wrap_depth], wrap_cn->key_bytes, wrap_len);
		pub = ft_merge_wrap_prefix(&ctx, kbuf, wrap_depth,
				wrap_depth + wrap_len, M, merged_keys);
		if (pub == FT_MERGE_OOM) {
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	} else if (pub_parent) {
		pub = ft_compress_single_child_if_needed(dst_ft, M, &gd);
		if (pub == (struct cds_ft_inode_flag *) (long) -ENOMEM) {
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	} else {
		pub = M;
	}

	/*
	 * Take or create the flip-txn.  Structure, ordered list AND duplicate
	 * chains commit in ONE flip, so they share this txn.  It needs:
	 *  - one latch per dst-origin re-parent edge;
	 *  - one for the merge-point forward slot;
	 *  - @ms_cap for the ordered-list interleave's boundary edges;
	 *  - one per collided duplicate-chain splice.  This is the src run
	 *    tail-append, folded in below so the concatenation flips with
	 *    the structure.
	 * Reserve it up front to that
	 * bound.  Then every post-drain record is allocation-free.  Those
	 * records are the structural edges, the INSTALLED-state cell edges
	 * and the splice edges, which append into the reserved head chunk.
	 * The rekey hands in a pre-reserved txn (cannot fail).  Otherwise
	 * create one here, where failure aborts the still-invisible build
	 * (both tries pristine).
	 */
	{
		unsigned int nr_dst = 0;
		int j;

		for (j = 0; j < gd.nr_deferred; j++)
			if (gd.deferred[j].dst_origin)
				nr_dst++;
		txn = ft_flip_txn_take(pre_txn);
		if (!txn) {
			txn = ft_flip_txn_create(dst_ft);
			if (txn && !ft_flip_txn_reserve(txn,
					nr_dst + 1 + ms_cap + gd.cap_free
						+ 2 * gd.nr_splices /* forward link + the head's prev */
						+ 1 /* §4.B parent guard */
						/* + count walk: the (merged_keys - cnt_dst) nr_keys ancestor
						 * edges (BULK fold), bounded by the merge-point depth */
						+ (dst_ft->rank_stats ? (int) dst_key_len + 1 : 0)
				/* the split-retire terminal's second word, if any */
				+ ft_glue_split_cn_reserve(dst_ft))) {
				ft_flip_txn_destroy(txn);
				txn = NULL;
			} else if (txn) {
				/*
				 * The txn was created with gd.cap_free
				 * free-list headroom reserved.  So fuse
				 * each dst-overlap retire's freeze into
				 * @txn (atomic detach, see
				 * doc/design/mcas-multiwriter-readiness.md
				 * §4.B).  Then the whole retired dst
				 * spine freezes dead atomically with the
				 * forward publish that unlinks it.  The
				 * rekey take() path keeps the standalone
				 * flip.  The rekey pre-sizes its pre_txn
				 * with no room to grow here.
				 */
				gd.fuse_free_list = true;
			}
		}
	}
	if (!txn) {
		if (fresh_root)
			free_cds_ft_node_unpublished(src_ft, fresh_root);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	ft_glue_set_publish(dst_ft, &gd, pub_parent, pub_slot, pub);
	/*
	 * The dst forward publish commits through @txn (step 4 below).  Point
	 * gd->txn at it.  Then ft_glue_apply_deferred's tombstone_free_list
	 * records the fused dst retires into the same txn.  This alias is a
	 * no-op when fuse_free_list stayed false (the rekey take() path).
	 */
	gd.txn = txn;

	/*
	 * Compute the slot-canonical form of @pub for the @pub_slot stores.
	 * Both the flip proxy's new target and the settle use it.  A compressed
	 * @pub is published SKIP-ENCODED, exactly as a direct slot write would
	 * store it.  So a reader resolving the proxy gets a value with the same
	 * encoding as a normal slot read, and runs the same skip handling.
	 * set_publish already wired @pub's parent + skip_slot via the plain
	 * flag.  An internal @pub needs no re-encode.  That is EXACT only: a
	 * compressed-wrap @pub is always compressed.
	 *
	 * @D_old is the flip proxy's old target, the value @pub_slot holds now.
	 * Read it from the live slot, for two reasons:
	 *  - for the compressed shapes, the descent resolved d->nf / d->pnf to
	 *    the PLAIN flag, while the slot holds the SKIP form;
	 *  - *pub_slot is untouched until the flip.  The build is invisible,
	 *    and apply_deferred wires back-pointers, not this forward slot.
	 */
	if (ft_node_compressed(pub))
		M_slot = ft_publish_compressed(dst_ft,
				ft_compressed_node_ptr(pub), pub);
	else
		M_slot = pub;
	/*
	 * Use a WAITING load, not a raw one.  @pub_slot is this flip's forward
	 * edge, so it enters @txn's own write set.  "Untouched until the flip"
	 * is true of THIS op's stores.  It says nothing about a PEER parking a
	 * flip proxy there.  That proxy is a descriptor-record POINTER.  As an
	 * expected-old, it trips urcu_txn_add's !urcu_txn_is_proxy self-check.
	 */
	D_old = ft_txn_load(txn->mtxn, (void **) pub_slot,
		FT_FLIP_PROXY_TAG);

	/*
	 * Ordered list: capture two cells while D is still intact:
	 *  - the dst merge subtree's min head, the cursor for the post-commit
	 *    interleave walk;
	 *  - the region predecessor.
	 * D is still intact because the build only copied its spine.  The flip
	 * below moves its leaves into M.  The surviving src cells are spliced
	 * in after the commit.
	 */
	if (ms_ord) {
		ms_cursor = ft_ord_cell_ptr(rcu_dereference(
			ft_subtree_minmax_head(dst_ft, D, false)->prev));
		ms_prev = ft_ord_cell_resolve_ord(&ms_cursor->lnode.prev);
		/*
		 * The dst region run is [@ms_cursor .. D's max head].
		 * @ms_succ is the cell following it (NULL at the list
		 * tail).  It is the stop boundary for the interleave walk.
		 * It is also the trailing survivor's successor.
		 */
		ms_succ = ft_ord_cell_resolve_ord(&ft_ord_cell_ptr(rcu_dereference(
			ft_subtree_minmax_head(dst_ft, D, true)->prev))->lnode.next);
		/*
		 * Capture src's merged-subtree (S) run endpoints now, while S is
		 * still intact, but defer the actual run-unlink until AFTER the
		 * last fallible step (ft_merge_unlink_src_subtree).  The run
		 * unlink is an externally observable ordered-list
		 * mutation.  Done here, it would leave src's list
		 * inconsistent if the src subtree unlink below OOMs and we
		 * roll the whole merge back.  The unlink still happens
		 * before the drain.  So sync drains src ord-readers of the
		 * run too.
		 */
		ms_s_first = ft_subtree_minmax_head(dst_ft, S, false);
		ms_s_last = ft_subtree_minmax_head(dst_ft, S, true);
		/*
		 * Pre-allocate the interleave's edge scratch NOW, while the build
		 * is still abortable.  The interleave is collected
		 * pre-commit.  Its proxies are recorded into the
		 * already-reserved structural @txn.  So the commit tail has
		 * no allocation left, and cannot degrade to a non-atomic
		 * per-edge fallback.  Each survivor run costs at most two
		 * visible edges.  So @ms_cap (2*merged_keys+2) bounds
		 * @ms_edges.
		 *
		 * ☠ CALLOC, not malloc.  This is the twin of
		 * ft_merge_spine_copy's ms_edges, which 5f21b822 fixed and
		 * this one kept.  The collect fills edges FIELD BY FIELD
		 * and never sets @owner.  The record loop below passes
		 * @owner straight to the recorder's owns().
		 */
		ms_edges = calloc((size_t) ms_cap, sizeof(*ms_edges));
		if (!ms_edges) {
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		/*
		 * Capture the src run's key SUFFIXES (below the src merge point)
		 * NOW, while S is still attached and up-walkable.  The
		 * post-commit interleave merges them against the LIVE dst
		 * suffixes.  That rebuilds the merged order WITHOUT walking
		 * the merged structure that is about to be published.  So
		 * no proxy needs to be installed before the collect.
		 *
		 * Capturing them here, not at the collect, is mandatory.
		 * ft_merge_unlink_src_subtree below leaves S-root's parent
		 * stale.  So a src up-walk after the unlink could run into
		 * freed or relocated structure.
		 *
		 * @cnt_src bounds the run length: the src subtree's key
		 * count is >= its distinct heads.  The variable-length
		 * suffix bytes pack into a pool that realloc can grow.  The
		 * pool is addressed by offset, because a byte pointer would
		 * dangle across the realloc.  This is a fallible pre-pass
		 * before the last fallible step.  So any OOM aborts the
		 * still-invisible build, and both tries stay pristine.
		 */
		ms_src_caps = malloc((size_t) (cnt_src + 8) * sizeof(*ms_src_caps));
		if (ms_src_caps) {
			struct ft_ord_cell *sc = ft_ord_cell_ptr(
				rcu_dereference(ms_s_first->prev));
			struct ft_ord_cell *slast = ft_ord_cell_ptr(
				rcu_dereference(ms_s_last->prev));
			size_t s_max_len = dst_ft->group->max_key_len;
			size_t pool_cap = 0, pool_len = 0;
			uint8_t sbuf[FT_MAX_KEY_LEN];
			bool oom = false;

			for (;;) {
				size_t sfl, suf_len;

				/* Premises, not asserts: see the helper. */
				FT_MERGE_SRC_CAPTURE_PREMISE(
					!ft_ord_is_end(src_ft, sc),
					"walked off the list before the last head",
					sc);
				FT_MERGE_SRC_CAPTURE_PREMISE(
					ms_nsrc < cnt_src + 8,
					"more heads than @cnt_src + 8", sc);
				sfl = ft_rebuild_key_upwalk(dst_ft, sc, sbuf,
						s_max_len);
				FT_MERGE_SRC_CAPTURE_PREMISE(
					sfl >= src_key_len,
					"a head key shorter than the merge point",
					sc);
				suf_len = sfl - src_key_len;
				if (pool_len + suf_len > pool_cap) {
					size_t ncap = pool_cap ? pool_cap * 2 : 256;
					uint8_t *np;

					while (ncap < pool_len + suf_len)
						ncap *= 2;
					np = realloc(ms_src_pool, ncap);
					if (!np) {
						oom = true;
						break;
					}
					ms_src_pool = np;
					pool_cap = ncap;
				}
				memcpy(ms_src_pool + pool_len,
					sbuf + (s_max_len - sfl) + src_key_len,
					suf_len);
				ms_src_caps[ms_nsrc].cell = sc;
				ms_src_caps[ms_nsrc].suffix_off = pool_len;
				ms_src_caps[ms_nsrc].suffix_len = suf_len;
				pool_len += suf_len;
				ms_nsrc++;
				if (sc == slast)
					break;
				sc = ft_ord_cell_resolve_ord(&sc->lnode.next);
			}
			if (oom) {
				free(ms_src_pool);
				ms_src_pool = NULL;
				free(ms_src_caps);
				ms_src_caps = NULL;
			}
		}
		if (!ms_src_caps) {
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}

	/*
	 * 1. Unlink the merge source from src.  Root src: swap in the pre-
	 *    allocated empty root.  Non-root src: detach its branch in place --
	 *    the LAST fallible step.  On its OOM both tries are pristine (the
	 *    detach self-undoes, the cluster is still unpublished), so abort the
	 *    build; this preserves the no-rollback property.  Then drain src
	 *    readers of the moved content.
	 *
	 *    ===== Everything from the drain onward is failure-free. =====
	 */
	struct ft_detach_run sdrun = { .into = NULL };

	if (ms_ord && !root_src) {
		/*
		 * Non-root src: fuse the src run-unlink into the structural
		 * unlink flip of ft_merge_unlink_src_subtree.  It is
		 * EXCISE-ONLY (into == NULL): the run cells disperse to dst
		 * via the post-commit interleave.  The subpos src side does
		 * exactly the same.  This closes the src-side disappear
		 * window.
		 */
		sdrun.rfirst = ms_s_first;
		sdrun.rlast = ms_s_last;
	}
	/*
	 * Pre-reserve the src-side ordered-list commit txn before the src
	 * unlink, which can still fail.  One of two commits fires:
	 *  - the root-src root+endpoint swap;
	 *  - the rare unfused non-root run-unlink.
	 * Whichever fires is the src side's commit.  It cannot be aborted once
	 * the structural unlink is public.  So it commits through this
	 * pre-reserved txn (ft_ord_cell_flip_into).  The txn is sized to the
	 * larger bound (4-edge run-unlink >= 3-edge root swap).  OOM here
	 * aborts the still-invisible build (both tries pristine).  The
	 * lone-edge list-off paths (ft_root_edge_flip) need no txn.  So reserve
	 * only when ms_ord.
	 *
	 * ☠ WHY THE SRC SIDE IS TRANSACTED AT ALL.  cds_ft_merge_at and
	 * cds_ft_graft both REJECT a non-exclusive src (BUSY_ERROR).  An
	 * exclusive trie has no concurrent reader or writer to be atomic
	 * against.
	 *
	 * But that rejection is `src_ft != dst_ft`.  SAME-TRIE is excluded from
	 * it on purpose.  cds_ft_rekey_{graft,merge} reach this worker through
	 * ft_rekey_at_inner(ft, ..., ft, ...) with @rekey set.  So on that path
	 * @src_ft IS @dst_ft: the LIVE, SHARED, concurrently-read trie.  Its
	 * "src side" is therefore exactly as contended as its dst side.  Every
	 * edge here needs the atomicity that a cross-trie merge's src does not.
	 *
	 * So consider "the src is always exclusive now, drop the src-side txn".
	 * It is a sound reading of the entry gates.  It is a WRONG conclusion
	 * about this body.  The exclusivity that would justify it belongs to
	 * the CROSS-TRIE callers only.  This code is shared with the one caller
	 * that has none.
	 */
	struct ft_flip_txn *src_side_txn = NULL;

	/*
	 * Size the src-side commit txn per shape:
	 *  - A root src fuses the gs free-list freeze into its root swap / lone
	 *    root flip (atomic detach, doc §4.B).  That adds gs.cap_free
	 *    tombstone edges.  gs.cap_free is cnt.nf_src + 8, the exact upper
	 *    bound ft_glue_defer_free asserts against.
	 *  - A non-root src commits its run-unlink through a plain
	 *    FT_ORD_CELL_RUN_UNLINK txn.  Its gs freeze stays standalone.  It
	 *    is applied below, AFTER the unlink of ft_merge_unlink_src_subtree,
	 *    which can still fail.  So a rolled-back merge never freezes and
	 *    then strands.  Full non-root fusion, into ft_detach_node's flip,
	 *    is a §4.B residual.
	 *  - A list-off root with an EMPTY gs keeps its bare lone-edge store
	 *    (no txn, no grace period).
	 * ft_merge_build above fully populated gs.  So nr_free/cap_free are
	 * final here.  (gd is stamped in ft_glue_apply_deferred for the dst
	 * forward publish.)
	 */
	if (ms_ord) {
		src_side_txn = ft_flip_txn_create_bounded(src_ft, root_src ?
			FT_ROOT_LIST_SWAP_MAX_EDGES + (unsigned int) gs.cap_free :
			FT_ORD_CELL_RUN_UNLINK_MAX_EDGES);
		if (!src_side_txn) {
			free(ms_src_pool);
			free(ms_src_caps);
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	} else if (root_src && gs.nr_free) {
		/* List-off root with retires: 1 root edge + gs.cap_free tombstones. */
		src_side_txn = ft_flip_txn_create_bounded(src_ft,
			1u + (unsigned int) gs.cap_free);
		if (!src_side_txn) {
			free(ms_src_pool);
			free(ms_src_caps);
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}

	/*
	 * Dup-chain lock-set (MW LOCK_FINE).  Take the node lock of every
	 * distinct holder whose chain gets a collided src run appended at step
	 * 3c.  Then the tail walk + append run under the same per-node lock
	 * that insert, remove, promote and replace take on that chain.
	 *
	 * HERE is the last point where a miss is free.  Every fallible
	 * allocation above has succeeded.  The src unlink below is the point of
	 * no return.  So an -EAGAIN unwinds a build that is still entirely
	 * invisible.  The locks are held across the unlink, the drain and the
	 * single commit that installs the appends.  They are released just
	 * after that commit.
	 *
	 * UNDOING THE ATTEMPT IS TRIVIAL, and that makes the bail cheap.
	 * cds_ft_merge_at consumes an EXCLUSIVE source.  So nothing is
	 * published and nothing has moved.  Discarding the fresh cluster leaves
	 * both tries byte-for-byte as they were.  The caller just re-descends.
	 * That is why contention needs no place in enum cds_ft_status (see
	 * @contended).
	 *
	 * SKIPPED when the CALLER pre-reserved @pre_txn.  That marks a caller
	 * which has ALREADY passed its own point of no return.  It pre-built
	 * everything so this placement cannot fail.  Today that caller is the
	 * staged rekey (detach -> GP -> merge back).  Its content is already
	 * out of the trie and has nowhere to go if we bail.  A failable acquire
	 * under an unfailable placement is a contradiction.  Retrying there
	 * would re-draw a node reserve.  A discarded attempt returns that
	 * reserve to the ARENA, not to the reserve.
	 *
	 * Same-trie rekey gets its chain exclusion elsewhere, not from a
	 * retrofit here.  It comes from the up-front DLM lock set of the
	 * in-place one-decide writer.  That writer is a single commit with
	 * reader two-pass coherence.  It has no detach, and so no unfailable
	 * tail.  So this leaves the staged rekey's appends where they are
	 * today.
	 *
	 * This is a no-op on a non-lock_fine trie or a collision-free merge.
	 * Every merge of disjoint key sets is collision-free.  So the
	 * batch-staging workload pays nothing.
	 */
	if (!unfailable && ft_glue_acquire_splice_holders(dst_ft, &gd)) {
		free(ms_src_pool);
		free(ms_src_caps);
		free(ms_edges);
		ft_flip_txn_destroy(txn);
		if (src_side_txn)
			ft_flip_txn_destroy(src_side_txn);
		if (fresh_root)
			free_cds_ft_node_unpublished(src_ft, fresh_root);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		*contended = true;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}

	/*
	 * PRE-ACQUIRE THE PUBLISH TARGET here, for the same reason.  This is
	 * the last point at which failing to get it is free.
	 *
	 * The forward publish used to acquire @pub_parent at step 3, AFTER the
	 * src unlink.  It did so through ft_flip_txn_lock_or_guard_parent.
	 * That function cannot report a miss as a failure.  It sets
	 * @acquire_miss, and the commit then ABORTS.  By then the src subtree
	 * is unlinked and the merged cluster is unpublished.  So the keys exist
	 * in NEITHER trie.  The ignored commit status then let this function
	 * return CDS_FT_STATUS_OK.  That was silent data loss with a success
	 * code, measured across most merge shapes.
	 *
	 * @pub_parent is above the overlap spine, so it is never in the fenced
	 * set.  It CAN be a dup-chain holder we just locked (an external dst
	 * merge point).  So take that fence over rather than re-mark our own
	 * word.  Re-marking it is the self-deadlock this file has now hit
	 * twice.  Either way the holder rides @gd to the publish.  The publish
	 * records the {LOCK|s -> s} release directly, so it never consults
	 * lock_or_guard.  With no acquire left to miss, that abort cause is
	 * gone, not merely less likely.
	 *
	 * A miss here bails exactly like the splice acquire above: both tries
	 * pristine, contention reported, caller re-descends.
	 */
	if (!unfailable && dst_ft->lock_fine && pub_parent) {
		struct cds_ft_metadata *pm = ft_flag_to_metadata(dst_ft, pub_parent);
		uintptr_t psnap = 0;

		struct ft_lock_ctx pctx;
		struct ft_held_anchor ph;
		unsigned int pdep;
		bool took = ft_glue_splice_holder_take(&gd, pm, &psnap);

		ft_glue_lock_ctx(&gd, &pctx);
		if (!took && (!ft_lock_ctx_depth_of(dst_ft, &pctx, pub_parent,
					&pdep) ||
				ft_acquire_member(dst_ft, &pctx, pub_parent, pm,
					pdep, &ph))) {
			free(ms_src_pool);
			free(ms_src_caps);
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (src_side_txn)
				ft_flip_txn_destroy(src_side_txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			*contended = true;
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		/*
		 * The holder is whichever word protects @pub_parent: the splice
		 * fence this op already took over, or the anchor just acquired.
		 */
		/*
		 * A SHARED acquire owes no release.  The claim above says that
		 * @pub_parent is above the overlap spine, and so never in the
		 * fenced set.  That holds for the NODE, not for its ANCHOR:
		 *  - coarsening can put that anchor on an overlap node this op
		 *    has already fenced;
		 *  - root-only puts EVERY member on one word.
		 * Claiming it here would record a second terminal on a word
		 * whose fenced retire already owns one.  The engine poisons
		 * that.
		 */
		if (!took && ph.shared) {
			gd.publish_parent_holder = NULL;
			gd.publish_parent_snap = 0;
		} else {
			gd.publish_parent_holder = took ? pm : ph.lock;
			gd.publish_parent_snap = took ? psnap : ph.lock_snap;
		}
	}

	if (root_src) {
		/*
		 * A root src moves the WHOLE source.  So its run is the
		 * whole src ordered list.  Fuse the src->root swap with the
		 * head/tail clear into ONE flip (ft_root_list_swap_publish).
		 * Then a src reader never sees src structurally empty while
		 * its ordered-list front is still populated.  The run cells
		 * keep their internal links.
		 * Only the head/tail endpoints flip.  So the post-commit
		 * interleave still re-homes them to dst.
		 */
		if (ms_ord) {
			/*
			 * Empty src's sentinel (relink_dest NULL).  The
			 * post-commit interleave re-homes the run cells into
			 * dst.  It sets each survivor's links individually.
			 * Collided heads are freed.
			 *
			 * Fuse the gs free-list freeze into this root-swap
			 * txn (atomic detach, doc §4.B):
			 *  1. record each retired src-overlap node's
			 *     tombstone into src_side_txn;
			 *  2. the swap records the root + sentinel
			 *     endpoint edges into the same txn;
			 *  3. the swap commits them all in ONE flip.
			 * An empty gs records nothing.
			 */
			gs.txn = src_side_txn;
			gs.fuse_free_list = true;
			ft_glue_tombstone_free_list(&gs);
			ft_root_list_swap_publish(src_ft, src_side_txn,
				&src_ft->root,
				src_ft->root, ft_node_flag(fresh_root, 0),
				ft_ord_first(src_ft), NULL,
				ft_ord_last(src_ft), NULL, NULL, false);
			src_side_txn = NULL;	/* consumed */
		} else if (gs.nr_free) {
			/*
			 * List-off root with retires: fuse the gs free-list
			 * freeze into the root flip (atomic detach, doc
			 * §4.B).  Record the lone root edge + each retired
			 * node's tombstone into src_side_txn.  Commit them in
			 * ONE flip, a group flip like the list-on path.  Do
			 * not use a bare store followed by standalone
			 * freezes.  The root edge normalizes to the same
			 * FT_FLIP_PROXY_TAG the swap uses.
			 */
			/* Stage 5: the root's own lock, so this record can park SW. */
			(void) ft_root_lock_take(src_ft, NULL, src_side_txn);
			ft_flip_txn_record_root(src_side_txn,
				(void **) &src_ft->root,
				(void *) src_ft->root,
				(void *) ft_node_flag(fresh_root, 0));
			gs.txn = src_side_txn;
			gs.fuse_free_list = true;
			ft_glue_tombstone_free_list(&gs);
			ft_flip_txn_commit(src_ft, src_side_txn);
			src_side_txn = NULL;	/* consumed */
		} else {
			/*
			 * No ordered list, no retires: src->root is the only
			 * reader-visible slot.  Express the lone root edge as
			 * a single-edge flip descriptor.  That is one release
			 * store, like a bare rcu_assign_pointer.  Then the
			 * swap is MCAS-expressible like the list-on path.
			 */
			ft_root_edge_flip(src_ft, &src_ft->root,
				src_ft->root, ft_node_flag(fresh_root, 0));
		}
		FT_TP(root_publish, (const void *) src_ft, (const void *) src_ft->root);
	} else if (ft_merge_unlink_src_subtree(src_ft, src_key, src_key_len,
				cnt_src, ms_ord ? &sdrun : NULL, &gs) < 0) {
		/*
		 * OOM in the last fallible step.  @src_ft is left pristine:
		 * the run was not yet applied.  It commits in
		 * ft_detach_node's flip, past the fallible alloc.  So abort
		 * the still-invisible build.
		 */
		free(ms_src_pool);
		free(ms_src_caps);
		free(ms_edges);
		ft_flip_txn_destroy(txn);
		if (src_side_txn)
			ft_flip_txn_destroy(src_side_txn);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	/*
	 * Non-root src: gs (the src overlap-spine free-list) is now frozen
	 * dead ATOMICALLY with the unlink.  ft_merge_unlink_src_subtree
	 * threaded gs into ft_detach_node as its @retire_glue.  So each retired
	 * node's tombstone rode the SAME commit_txn flip that unlinked S (doc
	 * §4.B atomic detach).  On the OOM abort above, ft_detach_node left gs
	 * unmarked (src stays pristine, gs still live).  A root src froze gs
	 * fused into its root swap / flip above.
	 */
	/*
	 * The last fallible step has now committed.  So remove src's merged
	 * subtree (S) run from src's ordered list.  Its cells disperse to dst
	 * (survivors) or are freed (collisions).  This is deferred to here so
	 * that an OOM in the src unlink above leaves src's list untouched on
	 * rollback.  It is done before the drain, so sync drains src
	 * ord-readers of the run too.  It is skipped in two cases:
	 *  - a root src (fused into the root swap above);
	 *  - a non-root src whose unlink already fused the run (sdrun.armed).
	 */
	if (ms_ord && !root_src && !sdrun.armed) {
		ft_ord_cell_run_unlink(src_ft, src_side_txn, ms_s_first,
			ms_s_last);
		src_side_txn = NULL;	/* consumed */
	}
	/* Reserved but unused: a non-root run whose unlink already fused it. */
	if (src_side_txn)
		ft_flip_txn_destroy(src_side_txn);
	if (!src_ft->exclusive) {
#ifdef FT_DEBUG_SPLICE_SEAM
		uatomic_inc(&ft_ss_seam_spine);
#endif
		ft_writer_lock_gp_wait(src_ft);
	}

	/*
	 * 2. Re-parent the SRC-origin referenced subtrees directly.  The src
	 *    drain above made them unreachable to readers.  The cluster is
	 *    not yet forward-published.  So this is invisible.  dst-origin
	 *    edges are NOT applied here: they go through the flip.
	 */
	ft_glue_apply_deferred(dst_ft, &gd);

	/*
	 * 3. Record the structural edges into @txn (still PREPARE):
	 *    - the parent re-parent of every dst-origin child;
	 *    - the merge-point forward slot.
	 *    ft_glue_record_back_edge mirrors ft_set_parent's child-kind
	 *    dispatch.  It reads the field's current value as the old target.
	 *    It also records the parent slot offset with it.  The commit's
	 *    settle below writes each field to its new parent.  That subsumes
	 *    the old step-7 ft_set_parent.
	 *    Nothing is parked yet.  The commit's auto-install stages every
	 *    proxy (selector 0, resolving to old).  So up-walks and the root
	 *    descent still see the pre-merge dst until the flip.
	 */
	{
		int j;

		for (j = 0; j < gd.nr_deferred; j++) {
			if (!gd.deferred[j].dst_origin)
				continue;
			ft_glue_record_back_edge(dst_ft, txn,
				gd.deferred[j].child, gd.deferred[j].parent,
				gd.deferred[j].slot);
		}
		/* Forward publish: old dst subtree -> merged cluster. */
		/*
		 * VALIDATE (§4.B) / LOCK_FINE (step 6, §9.4 M-2): acquire the LIVE dst
		 * parent above the overlap spine as a RELEASE lock.  The
		 * forward publish is a same-slot REPLACE (nr_child invariant,
		 * §9.4 finding 1).  So pub_parent is a value-swap survivor
		 * and is not recompacted.  A guard fallback on an acquire
		 * miss is therefore correct.  A non-lock_fine trie falls to
		 * the §4.B guard.
		 *
		 * UNLESS WE ALREADY HOLD IT.  When the dst merge point is an
		 * EXTERNAL node, the collided head IS that node.  Its chain
		 * holder is its immediate parent, @pub_parent.  Re-marking a
		 * word this op already fenced MISSES.  A miss now sets
		 * acquire_miss and ABORTS a commit with no bail path left
		 * (the src is already unlinked).  Hand the held fence to the
		 * txn instead.  hold_or_lock records the {LOCK|s -> s}
		 * release, the guard's strictly stronger twin.  The txn owns
		 * the unlock from here, so the take clears our entry.
		 */
		/*
		 * The holder was acquired BEFORE the src unlink (see the
		 * pre-acquire above).  It was acquired either by marking
		 * @pub_parent here, or by taking over the dup-chain fence
		 * when the two coincide.  Recording its release directly
		 * keeps this publish off lock_or_guard.  A lock_or_guard
		 * miss would abort a commit that has no way left to fail
		 * safely.  A NULL holder means non-lock_fine or the
		 * unfailable caller.  That case routes to the ordinary
		 * acquire-or-guard exactly as before.
		 */
		{
			struct ft_lock_ctx mctx;

			ft_glue_lock_ctx(&gd, &mctx);
			ft_flip_txn_hold_or_lock_parent(dst_ft, txn, &mctx,
				pub_parent, FT_DEPTH_FROM_DESCENT,
				gd.publish_parent_holder,
				gd.publish_parent_snap);
			/*
			 * No @pub_parent: the slot is &dst_ft->root, and the
			 * root's own lock is P's -- taken where P's is.
			 */
			if (pub_slot == &dst_ft->root)
				ft_flip_txn_lock_root_unfailable(txn, &mctx,
					(void **) pub_slot, D_old);
		}
		/*
		 * OWNERSHIP TRANSFER (mirrors the graft): @txn's registry now
		 * owns this fence.  A commit consumes it through the recorded
		 * release.  An abort clears it.  So drop the glue's claim.
		 * No bail is left between here and the commit.  But a stale
		 * holder left behind would make the NEXT bail added here a
		 * double clear.
		 */
		gd.publish_parent_holder = NULL;
		gd.publish_parent_snap = 0;
		/*
		 * @pub_slot is d_dst->nfp.  At depth 0 it is &dst_ft->root.
		 * The record of the root is always MW and ignores the owner.
		 * Below depth 0 it is a child slot inside @pub_parent.
		 * @pub_parent is the node the hold_or_lock above put in this
		 * txn's registry.
		 */
		ft_flip_txn_record_publish(txn, dst_ft,
			pub_parent ? ft_flag_to_metadata(dst_ft, pub_parent) :
				NULL,
			pub_slot, D_old, M_slot);
	}

	/*
	 * 3b. Ordered list: collect the interleave and record its cell edges
	 *    into the SAME txn, STILL IN PREPARE.  No install happens before
	 *    the collect.
	 *    A two-pointer merge of the two LIVE cell runs rebuilds the merged
	 *    order.  The runs are:
	 *    - the dst region, up-walked over the intact D;
	 *    - the suffixes of the src run, captured pre-unlink.
	 *    So the collect does NOT need the merged structure that is about
	 *    to be published.  The last append-after-install is gone.  Every
	 *    edge is recorded before install.
	 *    Structure and ordered list thus commit in ONE flip: the commit
	 *    auto-installs every recorded proxy, then flips.  A reader never
	 *    sees the merged structural minimum ahead of the ordered-list
	 *    front.
	 *    The collect pre-sets the surviving cells' own links invisibly
	 *    (they are not yet ord-reachable).  It returns only the visible
	 *    boundary edges.
	 *    It runs before the splice fold (step 3c), but collisions are
	 *    invariant.  A collided src head is a floating duplicate: it is
	 *    only in the splice record and is never a distinct reachable
	 *    head.  So the merge enumerates the same heads either way.
	 *    Step 3c reads each demoted head's cell from its src-run prev.
	 *    That prev is still intact: this collect leaves it untouched.
	 */
	if (ms_ord) {
		ms_n = ft_merge_ord_interleave_collect(dst_ft, dst_key_len,
			ms_cursor, ms_succ, ms_prev, /*tail_link=*/ ms_succ,
			/*head_linked=*/ true, ms_src_caps, ms_nsrc,
			ms_src_pool, ms_edges, /*record_all=*/ false,
			/*ncollide=*/ NULL, /*collided=*/ NULL);
		/*
		 * ☑ ROUTED THROUGH THE TAG-DISPATCHING RECORDER, as its
		 * ft_merge_spine_copy twin already is.  This was a RAW
		 * record_tag loop.  Every edge took the structural_sw
		 * dispatch.  So a CELL edge could park SW under an armed txn
		 * with no cell lock held.  The only thing refusing that park
		 * was a NULL per-edge @owner.  With @ms_edges malloc'd, the
		 * @owner was not NULL.
		 * The loop also kept every edge here out of the cell guard's
		 * sight.  That includes the dst sentinel's begin/end words,
		 * which a new list minimum / maximum writes.  A point op
		 * writes them SW under their lock.  Here they were unlocked.
		 *
		 * So take what the edges name, then record by tag:
		 * - a cell edge the op holds parks under its lock;
		 * - the rest stay MW;
		 * - the guard names an unheld one.
		 */
		ft_merge_take_interleave_locks(dst_ft, &gd, txn, ms_edges, ms_n);
		ft_ord_cell_record_into_ft(dst_ft, txn, ms_edges, ms_n);
	}

	/*
	 * 3c. Duplicate-chain concatenation: fold each collided src run's
	 *    tail-append into the SAME txn, still in PREPARE.
	 *    It is recorded AFTER the interleave collect (3b).  So each
	 *    demoted src head's cell is captured from its intact src-run prev
	 *    before the append overwrites it.
	 *    It is recorded BEFORE the commit.  So the src duplicates become
	 *    reachable ATOMICALLY with the merged structure.  A collided key
	 *    never momentarily shows only its dst duplicates.  The old
	 *    post-commit apply had that window.
	 *    Each splice is one forward edge (dst tail -> src run).  So only
	 *    the tail carries a proxy; src_head->next rides along.
	 *    The rekey take() path reaches this too.  cds_ft_rekey_merge
	 *    unions into an OCCUPIED destination.  A full key present on both
	 *    sides collides there as in any other merge.  That is why that
	 *    path's pre-reservation budgets one splice edge per moved key.
	 *    (Only cds_ft_rekey_graft demands an empty destination, and it
	 *    never reaches the spine copy.)
	 *
	 *    Every append here runs under the chain holder's node lock.
	 *    ft_glue_acquire_splice_holders takes it before the point of no
	 *    return.  It is released after the commit below.  So the walk to
	 *    the tail cannot race a peer's append/unchain/promote on the same
	 *    chain.
	 */
#ifdef FT_DEBUG_SPLICE_SEAM
	{
		uatomic_inc(&ft_ss_site_rekey_staged_calls);
		if (gd.nr_splices)
			uatomic_inc(&ft_ss_site_rekey_staged);
	}
#endif
	ft_glue_record_splices(dst_ft, &gd, txn);

	/*
	 * Order-statistics fold (BULK): record the dst net key-count delta
	 * (merged_keys - cnt_dst) into the SAME txn.  The walk goes from
	 * @pub_parent up.  So the aggregate flips ATOMICALLY with the merged
	 * spine's forward publish (step 4).  It is exact under concurrent
	 * writers.  @pub is a fresh node that already carries merged_keys.
	 * So the walk begins one level up, at the stable @pub_parent.
	 * This is a no-op when rank stats are off or the count is unchanged.
	 */
	if (pub_parent && merged_keys != cnt_dst)
		ft_flip_txn_record_count_parent(dst_ft, txn, pub_parent,
			(long) merged_keys - (long) cnt_dst);

	/*
	 * 4. Commit.  One selector flip atomically switches from old to
	 *    merged:
	 *    - every dst-origin parent;
	 *    - the forward slot;
	 *    - every interleave cell edge.
	 *    The commit then settles each slot to its direct merged target.
	 *    (This reduces to a single release store of the forward slot
	 *    when the list is off and there is no dst-origin re-parent and no
	 *    collided splice.  Then there is no proxy and no grace period.)
	 *    The forward slot flips with the back-pointers.  So a reader
	 *    (descend then up-walk) only progresses old->merged.  The
	 *    ordered-list front advances in the same instant the merged
	 *    minimum becomes reachable.
	 */
	{
		enum urcu_txn_status mst = ft_flip_txn_commit(dst_ft, txn);

		/*
		 * Carried out of this block for ft_glue_clear_fenced below.
		 * The fate of the fenced retires is the OP's own knowledge.
		 * That sweep must not re-derive it from the state words.
		 */
		fenced_committed = (mst == URCU_TXN_STATUS_OK);

		/*
		 * The flip did not happen.  So no fenced retire took effect
		 * and this op owns none of those frees.  Renounce them before
		 * the step-7 reclaim.  The dominant reason a fenced terminal
		 * aborts is a PEER retiring the node under our fence (the
		 * retire primitives do not honour LOCK).  That peer owns the
		 * reclaim.  Freeing here would be a double free on top of an
		 * already-lost merge.
		 *
		 * The merge itself still cannot recover: the src is unlinked
		 * by now.  This is the pre-existing
		 * abort-after-point-of-no-return exposure, shared with the
		 * unlocked pub_parent publish.  But the merge must not
		 * corrupt the arena on the way out.
		 */
		if (mst != URCU_TXN_STATUS_OK)
			ft_glue_fenced_renounce_free(&gd);
	}

	/*
	 * 5. Drop the dup-chain holder locks.  The appends are installed, so
	 *    peers may mutate those chains again.  Do it BEFORE the step-7
	 *    reclaim below.  Most holders are dst overlap-spine nodes this
	 *    merge retires.  The fence has to come off while the node is
	 *    still there to clear.
	 *    (The fence survives the commit either way.  The free-list retire
	 *    records a plain {LOCK|s -> LOCK|s|TOMBSTONE} upgrade.  An
	 *    aborted commit leaves {LOCK|s}.  The one holder that IS the
	 *    publish target was handed to @txn above.  Its flip already
	 *    released it.)
	 */
	ft_glue_release_splice_holders(&gd);
	/*
	 *    Same for the overlap-spine plan-locks.  A COMMITTED fenced
	 *    retire consumed each fence into TOMBSTONE (clear_if_held
	 *    no-ops).  An ABORTED commit left {LOCK|s}.  That must come off,
	 *    or every later peer publish into that node fails forever.
	 *    One unconditional sweep covers both.  So no per-outcome
	 *    bookkeeping is kept.  Do it before the step-7 reclaim, while
	 *    the nodes are still addressable.
	 */
	ft_glue_clear_fenced(&gd, fenced_committed);

	/*
	 * 6. The dst net key-count delta (merged_keys - cnt_dst) is FOLDED
	 *    into the step-4 commit above.  It is recorded from @pub_parent
	 *    into @txn before the flip.  So it goes live ATOMICALLY with the
	 *    merged spine.  There is no post-commit propagate walk.
	 */

	/*
	 * 7. Reclaim the old overlap spines (src-side to src, dst-side to dst).
	 *    The flip-txn was already committed-and-reclaimed at step 4.
	 *    The commit settled every proxied slot to its direct merged
	 *    target: the dst-origin parents, the forward slot and the
	 *    interleave cell edges.  So the proxies are unreferenced.
	 *    The txn's parked block is deferred through the FT flavor.  It is
	 *    freed in place when the commit owed no grace period.
	 *    There is no dst synchronize_rcu: the flip subsumed the dst drain.
	 */
	ft_glue_free_old(src_ft, &gs);
	ft_glue_free_old(dst_ft, &gd);

	/*
	 * 8. Free the collided (demoted) src heads' cells.  The interleave was
	 * folded into the flip above.  It already rewired the surviving cells'
	 * stale src-run links away from these cells.  So new readers cannot
	 * reach them.  The grace-period defer inside the free covers readers
	 * that already hold such a link or are parked on a demoted head.
	 */
	if (ms_ord) {
		free(ms_edges);
		free(ms_src_caps);
		free(ms_src_pool);
	}
	ft_glue_free_collided_cells(dst_ft, &gd);

	ft_glue_fini(&gd);
	ft_glue_fini(&gs);
	return CDS_FT_STATUS_OK;
}
#endif /* FEATURE_FT_MERGE: an occupied dst is a merge */

static
enum cds_ft_status ft_rekey_subpos_inplace(struct cds_ft *dst_ft,
		struct cds_ft *src_ft,
		const uint8_t *okey_dst, const uint8_t *dst_key, size_t dst_key_len,
		const uint8_t *okey_src, size_t src_key_len,
		struct cds_ft_inode_flag *payload, unsigned long cnt_src,
		bool *handled)
{
	struct ft_glue glue;
	struct cds_ft_alloc_reserve reserve;
	struct ft_descent d;
	enum ft_graft_prep prep;
	struct cds_ft_inode_flag *attached_nf;
	bool ms_ord = dst_ft->group->ordered_list_set;
	struct ft_ord_cell *pred = NULL, *succ = NULL;
	struct ft_ord_cell *run_first = NULL, *run_last = NULL;
	struct cds_ft_node *s_first = NULL, *s_last = NULL;
	struct ft_graft_run mrun;
	struct ft_graft_run *run_arg = NULL;
	struct ft_detach_run srun = { .into = NULL, .armed = false };
	struct ft_detach_run *srunp = NULL;
	size_t src_max, nm, dm;
	/*
	 * MW LOCK_FINE drop: the "failure-free" commit below is only unfailable
	 * under the FT-wide lock.  Under the drop it can conflict with a peer:
	 * - its MCAS commit (GLUE);
	 * - its recompact prepare (NOSPLIT).
	 * It can then fail AFTER @ft_merge_unlink_src_subtree already excised
	 * the source subtree.  That unlink PRESERVES @payload: this op owns it
	 * and src is pristine-empty.  So the whole dst-side attach
	 * (re-descend -> build -> commit) is a retryable one-shot.
	 * @already_unlinked guards the src-side excise and the ordered-list
	 * run capture.  So a retry re-runs only the dst attach.
	 * @glue.fence_split_cn fences the compressed divergence node, as
	 * cds_ft_graft does.  So a concurrent grow of it is arbitrated:
	 * fence miss -> FT_GRAFT_PREP_RETRY -> re-descend.
	 * This mirrors cds_ft_graft's post-swap store retry.
	 */
	bool already_unlinked = false;
	struct ft_flip_txn *run_unlink_txn = NULL;
	struct ft_flip_txn *run_splice_txn = NULL;

	*handled = false;

	/*
	 * Classify the dst graft point with a build-invisible prep.
	 * - A GLUE diverge builds the whole split cluster into @glue.  The
	 *   cluster references @payload by deferred edge.  It is published
	 *   failure-free after the drain.
	 * - A NOSPLIT point is grafted post-drain by ft_store_at_graft_point.
	 *   The store draws from a pre-filled reserve, so it likewise cannot
	 *   fail on an arena allocation.
	 * Either way the source unlink is the single last fallible step.  So
	 * an OOM leaves both tries pristine: no rollback, no leak.
	 */
	unsigned long rm_depth __attribute__((unused)) = 0;

retry_merge:
	RSPIN_ENTER_X(0, rm_depth, 0, dst_ft->lock_fine && src_ft->exclusive);
	RSPIN_SITE_ENTER(1, rm_depth, dst_ft->lock_fine && src_ft->exclusive);
	ft_glue_init(&glue);
	/*
	 * Fence the compressed divergence node, like cds_ft_graft.  A
	 * concurrent grow of it is then arbitrated: on a miss the build
	 * re-descends (FT_GRAFT_PREP_RETRY).  ft_glue_init reset the flag, so
	 * set it on each attempt.
	 */
	glue.fence_split_cn = true;
	memset(&reserve, 0, sizeof(reserve));
	/*
	 * Every attach shape commits through @glue.txn, exactly like
	 * cds_ft_graft: the GLUE diverge and the NOSPLIT in-place store.
	 * Both draw out of this txn:
	 * - the build (below) tags its displaced old child dst_origin;
	 * - the NOSPLIT store reserves its slot proxy and run-splice edges.
	 * Create the txn BEFORE the build.  It is destroyed on a POPULATED
	 * point (nothing to commit).
	 */
	glue.txn = ft_flip_txn_create(dst_ft);
	if (!glue.txn || !ft_flip_txn_reserve(glue.txn,
			/* +1: fused recompact-relocate tombstone (§4.B);
			 * + FLOOR_FREE: fused free-list tombstones;
			 * + count walk: the +cnt_src nr_keys ancestor edges (BULK fold) */
			FT_GLUE_FLOOR_DEFERRED + 7 + 1 /* +1 §4.B parent guard */ + FT_GLUE_FLOOR_FREE
				+ (dst_ft->rank_stats ? (int) dst_key_len + 1 : 0))) {
		if (glue.txn)
			ft_flip_txn_destroy(glue.txn);
		ft_glue_fini(&glue);
		if (already_unlinked)
			goto retry_merge;	/* src consumed: OOM is transient */
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	glue.fuse_free_list = true;	/* reserved free-list headroom above (§4.B) */
	prep = ft_graft_build(dst_ft, okey_dst, dst_key_len, payload, cnt_src,
			&d, &glue, /*outer*/ NULL);
	*handled = true;
	if (prep == FT_GRAFT_PREP_RETRY) {
		/*
		 * Compressed-divergence @cn fence miss: a peer owns @cn.
		 * Nothing was built, so re-descend cleanly.  Before the
		 * unlink both tries are pristine.  After the unlink src is
		 * empty and @payload is owned.
		 */
		ft_glue_abort(dst_ft, &glue);
		ft_flip_txn_destroy(glue.txn);
		goto retry_merge;
	}
	if (prep == FT_GRAFT_PREP_OOM) {
		ft_glue_abort(dst_ft, &glue);
		ft_flip_txn_destroy(glue.txn);
		if (already_unlinked)
			goto retry_merge;	/* src consumed: OOM is transient */
		return CDS_FT_STATUS_MEMORY_ERROR;	/* both tries pristine */
	}
	if (prep == FT_GRAFT_PREP_POPULATED) {
		/*
		 * Defensive: cnt_dst == 0 should never yield an occupied
		 * point.  It is impossible post-unlink: the payload is already
		 * excised and owned, and no clean rc remains.  So assert
		 * rather than orphan it.
		 */
		assert(!already_unlinked);
		ft_flip_txn_destroy(glue.txn);
		ft_glue_fini(&glue);
		return CDS_FT_STATUS_POPULATED_ERROR;
	}
	if (prep == FT_GRAFT_PREP_NOSPLIT) {
		/*
		 * Pre-fill the store's node reserve while both tries are still
		 * pristine.  The store's slot proxy and run-splice edges draw
		 * from the already-reserved @glue.txn.  So no separate flip
		 * batch is needed.  Securing the reserve here, before the
		 * source unlink, makes the post-drain store wholly
		 * failure-free.  The unlink is then the true last fallible
		 * step and nothing strands.
		 *
		 * Use the GENEROUS superset fill, not a hand-rolled exact
		 * manifest.  ft_graft_keylen's NOSPLIT attach does the same
		 * for the same ft_store_at_graft_point.  Predicting the
		 * store's node set is fragile.  For example, attaching a high
		 * byte forces a RANGE recompact of the attach node even
		 * within its child capacity.  An exact count heuristic misses
		 * that grow.  It underflowed the reserve and aborted.  A bulk
		 * op already pays a grace period, so the throwaway
		 * pops/pushes are negligible.
		 */
		if (ft_bulk_node_reserve_fill(dst_ft, &reserve)) {
			cds_ft_alloc_reserve_drain(dst_ft, &reserve);
			ft_glue_abort(dst_ft, &glue);
			ft_flip_txn_destroy(glue.txn);
			if (already_unlinked)
				goto retry_merge;	/* src consumed: transient */
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}
	/* GLUE: cluster built invisibly.  NOSPLIT: reserve secured (+ glue.txn). */

	/*
	 * Graft-parity (cds_ft_graft hoists the same test pre-swap): an
	 * exact-depth OCCUPIED NOSPLIT point is a PERMANENT POPULATED
	 * condition.  Catch it HERE, on the FIRST pass, BEFORE the
	 * irreversible source unlink.  A misusing (non-disjoint) caller then
	 * gets a clean POPULATED_ERROR with BOTH tries pristine.  Otherwise it
	 * would reach the post-unlink store, assert, and strand @payload.
	 * This helper is dispatched only for the supported absent/diverged dst
	 * point (d.nf == NULL at key_len).  For that shape this is dead code.
	 * A retry (post-unlink) cannot newly occupy the point under the
	 * disjoint exclusive-src contract.  So the store's own POPULATED
	 * discrimination (below) stays a dead assert there too.
	 */
	if (!already_unlinked && prep == FT_GRAFT_PREP_NOSPLIT
			&& d.depth == dst_key_len && d.nf) {
		cds_ft_alloc_reserve_drain(dst_ft, &reserve);
		ft_glue_abort(dst_ft, &glue);
		ft_flip_txn_destroy(glue.txn);
		return CDS_FT_STATUS_POPULATED_ERROR;
	}

	/*
	 * Ordered list: locate the dst splice neighbours.  They are RE-FOUND
	 * on each attempt, because dst may have grown across a retry.  On the
	 * FIRST attempt only, capture the source subtree's run endpoints and
	 * cells while the subtree is still intact in src.
	 * @run_first / @run_last / @s_first / @s_last persist across retries.
	 * The unlink below excises the run ONCE.  The run is then owned (out
	 * of both lists).  So a retry re-splices the same owned run.
	 */
	if (ms_ord) {
		ft_ord_cell_find_splice_pos(dst_ft, dst_key, dst_key_len,
			&pred, &succ, NULL);
		if (!already_unlinked) {
			s_first = ft_subtree_minmax_head(src_ft, payload, false);
			s_last = ft_subtree_minmax_head(src_ft, payload, true);
			run_first = ft_ord_cell_ptr(rcu_dereference(s_first->prev));
			run_last = ft_ord_cell_ptr(rcu_dereference(s_last->prev));
			/* Src side: EXCISE-ONLY run so the structural unlink and the
			 * run's removal from src's ordered list commit in ONE flip
			 * (the disappear-side cross-view fix). */
			srun.rfirst = s_first;
			srun.rlast = s_last;
			srunp = &srun;
		}
		mrun.run_first = run_first;
		mrun.run_last = run_last;
		mrun.pred = pred;
		mrun.succ = succ;
		mrun.armed = false;
		run_arg = &mrun;
	}

	/*
	 * Source-side excise (ONE-SHOT, @already_unlinked):
	 * - reserve the run txns;
	 * - unlink the source subtree in place (preserving @payload);
	 * - remove the run from src's list;
	 * - drain src readers;
	 * - stamp an external payload's edge byte.
	 * A retry after a post-unlink commit abort skips all of this: src is
	 * already empty and @payload is owned.  It re-runs only the dst
	 * attach.
	 */
	if (!already_unlinked) {
		if (ms_ord) {
			/*
			 * Pre-reserve the standalone run-unlink + run-splice
			 * txns.  The rare unfused shapes touch src's / dst's
			 * list AFTER the structural flip is public, where they
			 * cannot abort.  OOM here aborts the still-invisible
			 * build (both tries pristine).
			 */
			run_unlink_txn = ft_flip_txn_create_bounded(src_ft,
				FT_ORD_CELL_RUN_UNLINK_MAX_EDGES);
			run_splice_txn = ft_flip_txn_create_bounded(dst_ft,
				FT_ORD_CELL_RUN_SPLICE_MAX_EDGES);
			if (!run_unlink_txn || !run_splice_txn) {
				if (run_unlink_txn)
					ft_flip_txn_destroy(run_unlink_txn);
				if (run_splice_txn)
					ft_flip_txn_destroy(run_splice_txn);
				run_unlink_txn = run_splice_txn = NULL;
				cds_ft_alloc_reserve_drain(dst_ft, &reserve);
				ft_glue_abort(dst_ft, &glue);
				ft_flip_txn_destroy(glue.txn);
				return CDS_FT_STATUS_MEMORY_ERROR;
			}
		}

		/*
		 * Last fallible SRC step: unlink the source subtree in place,
		 * preserving @payload.  On OOM the unlink self-undoes (src
		 * pristine).  The still-invisible cluster / reserve is
		 * released.  No rollback is needed.
		 */
		if (ft_merge_unlink_src_subtree(src_ft, okey_src, src_key_len,
				cnt_src, srunp, /*retire_glue=*/ NULL) < 0) {
			if (run_unlink_txn)
				ft_flip_txn_destroy(run_unlink_txn);
			if (run_splice_txn)
				ft_flip_txn_destroy(run_splice_txn);
			run_unlink_txn = run_splice_txn = NULL;
			cds_ft_alloc_reserve_drain(dst_ft, &reserve);
			ft_glue_abort(dst_ft, &glue);
			ft_flip_txn_destroy(glue.txn);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		already_unlinked = true;	/* POINT OF NO RETURN: @payload owned */

		/*
		 * Remove the source run from src's ordered list.  The cells
		 * keep their internal links for the dst splice.  Do it BEFORE
		 * the drain, so the sync drains src ord-readers of the run
		 * too.  The structural unlink FUSED this into its flip
		 * (@srun.armed).  The standalone remains a fallback.
		 */
		if (ms_ord && !srun.armed) {
			ft_ord_cell_run_unlink(src_ft, run_unlink_txn, s_first,
				s_last);
			run_unlink_txn = NULL;	/* consumed */
		}
		if (run_unlink_txn) {
			ft_flip_txn_destroy(run_unlink_txn);
			run_unlink_txn = NULL;
		}

		if (!src_ft->exclusive) {
#ifdef FT_DEBUG_SPLICE_SEAM
			uatomic_inc(&ft_ss_seam_subpos);
#endif
			ft_writer_lock_gp_wait(src_ft);
		}

		/*
		 * An EXTERNAL payload's edge byte changes (src_key's last byte
		 * -> dst_key's).  Stamp it in the head's CELL metadata now.
		 * The cell is in NEITHER list and structurally invisible, so
		 * no reader rebuilds a key from it.  Stamping while still in
		 * src's list would let a src reader rematerialize an
		 * out-of-namespace key.  Internal/compressed payloads keep
		 * every leaf's edge byte (the subtree moves wholesale).
		 */
		if (ms_ord && ft_node_external(payload))
			cds_ft_item_to_metadata(run_first)->incoming_byte =
				okey_dst[dst_key_len - 1];
	}

	if (prep == FT_GRAFT_PREP_GLUE) {
		/*
		 * Failure-free commit of the build-invisible diverge cluster.
		 * The displaced old child is LIVE: it stays reachable through
		 * the compressed node being split until the forward publish.
		 * So via @glue.txn it flips atomically with the forward
		 * publish and the ordered-list run-splice.  The whole attach
		 * is observed old XOR new.  The payload's hidden
		 * back-pointers are wired immediately inside the commit.
		 * Then reclaim the replaced compressed node.
		 */
		/*
		 * Order-statistics fold (BULK): the diverge cluster raises
		 * @glue.publish_parent's subtree by +cnt_src; record that walk
		 * into the same commit (a no-op when rank stats off).
		 */
		enum urcu_txn_status cst;

		glue.count_delta = (long) cnt_src;
		cst = ft_glue_txn_commit(dst_ft, &glue, run_arg);
		if (cst != URCU_TXN_STATUS_OK) {
			/*
			 * MW LOCK_FINE drop: the GLUE commit's MCAS footprint
			 * can conflict with a peer and ABORT, even though it
			 * allocates nothing.  The footprint is the forward
			 * publish, the fused run-splice and the fenced
			 * retires.  The flip rolled back: dst is byte-for-byte
			 * unchanged and the run-splice is not applied.
			 * ft_glue_txn_commit consumed glue.txn.  Free the
			 * failed attempt's invisible cluster and re-attach the
			 * owned @payload via the retry.  @run_splice_txn /
			 * @run_first / @run_last persist (src already
			 * excised).
			 */
			ft_glue_abort(dst_ft, &glue);
			goto retry_merge;
		}
		attached_nf = glue.attached_nf;
		ft_glue_free_old(dst_ft, &glue);
		ft_glue_fini(&glue);
	} else {
		/*
		 * NOSPLIT: graft the in-place payload at @d.  Node allocations
		 * draw from the reserve.  The slot proxy and run-splice edges
		 * draw from the pre-reserved @glue.txn.  Under the drop the
		 * store's recompact of the (contended) spine can still FAIL
		 * its prepare, after the source is already excised.
		 * ft_store_at_graft_point then returns -EAGAIN /
		 * MEMORY_ERROR: the FT-wide-lock "cannot fail" does not
		 * hold.  Re-attach the owned @payload via the retry.
		 */
		unsigned int adepth = 0;
		enum cds_ft_status st;

		cds_ft_alloc_reserve_activate(dst_ft, &reserve);
		st = ft_store_at_graft_point(dst_ft, okey_dst, dst_key_len, &d,
				payload, cnt_src, &attached_nf, &adepth, &glue,
				run_arg,
				(long) cnt_src);
		cds_ft_alloc_reserve_deactivate(dst_ft);
		cds_ft_alloc_reserve_drain(dst_ft, &reserve);
		if (st != CDS_FT_STATUS_OK) {
			/*
			 * ft_store_at_graft_point already freed its invisible
			 * build + txn (and, on a commit abort, the relocated
			 * copy) and cleaned up @glue.  Discriminate the
			 * failure class:
			 *
			 * - POPULATED is a PERMANENT occupied-point condition,
			 *   NOT a transient conflict.  Retrying it would
			 *   LIVELOCK and strand the owned @payload.
			 *   Post-unlink it is impossible for the supported
			 *   disjoint exclusive-src contract: no peer creates
			 *   the moved key.  So assert, as the pre-unlink
			 *   PREP_POPULATED arm does.
			 *   A general (non-disjoint) caller would still orphan
			 *   @payload here.  That is a peer racing the moved
			 *   key into the target during the
			 *   owned-but-unattached window.  The window exists
			 *   because this retry-based recovery excises the
			 *   source BEFORE the dst attach.
			 *   cds_ft_graft no longer has it.  Its exclusive
			 *   src-swap-fused arm lands the src retire ATOMICALLY
			 *   with the attach, only on commit success.  So the
			 *   same race MCAS-aborts, retries, and re-detects
			 *   POPULATED pre-commit with the source never
			 *   swapped.
			 *   Closing this gap for the sub-position move means
			 *   fusing ft_merge_unlink_src_subtree into the attach
			 *   flip.  It is a deferred parity follow-up (not
			 *   needed for any disjoint use).
			 * - Everything else is transient: re-descend and
			 *   re-attach the owned @payload.  BUSY_ERROR is
			 *   contention: a peer filled the reserve's byte, a
			 *   recompact could not lock, or the store's MCAS
			 *   commit ABORTed.  MEMORY_ERROR is a real allocation
			 *   failure.  Both retry.  They are kept distinct so
			 *   that a conflict never has to be read as an OOM.
			 */
			if (st == CDS_FT_STATUS_POPULATED_ERROR) {
				assert(!already_unlinked);
				return CDS_FT_STATUS_POPULATED_ERROR;
			}
			goto retry_merge;
		}
		ft_glue_fini(&glue);
	}

	/*
	 * Order-statistics: the +cnt_src ancestor walk is now FOLDED onto the
	 * attach commit above.  It uses:
	 * - glue.count_delta for the GLUE diverge and displaced-external
	 *   shapes;
	 * - ft_store_at_graft_point's count_delta for the in-place /
	 *   recompact-relocate slot shapes.
	 * So it flips ATOMICALLY with the structural publish.  It is exact
	 * under concurrent writers.
	 */

	/*
	 * Ordered list: every attach shape (GLUE, displaced-external, in-place
	 * slot) FUSED the run-splice into its structural flip (@armed).  The
	 * external edge-byte was stamped before that splice, above.
	 * The standalone two-commit splice remains as a defensive fallback for
	 * any not-yet-fused shape (none today).  Without it an unfused shape
	 * would strand the moved run out of the ordered list.  It commits
	 * through the pre-reserved @run_splice_txn, which cannot abort
	 * post-drain.  The fused common case frees that txn unused.
	 */
	if (ms_ord && !mrun.armed) {
		ft_ord_cell_run_splice(dst_ft, run_splice_txn, run_first,
			run_last, pred, succ);
		run_splice_txn = NULL;	/* consumed */
	}
	if (run_splice_txn)
		ft_flip_txn_destroy(run_splice_txn);	/* fused: unused */

	/* Raise dst's max_used_key_len for the moved keys (dst_key || suffix). */
	src_max = uatomic_load(&src_ft->max_used_key_len, CMM_RELAXED);
	nm = src_max > src_key_len ? dst_key_len + (src_max - src_key_len) :
		dst_key_len;
	dm = uatomic_load(&dst_ft->max_used_key_len, CMM_RELAXED);
	if (nm > dm)
		uatomic_store(&dst_ft->max_used_key_len, nm, CMM_RELAXED);

	return CDS_FT_STATUS_OK;
}

/*
 * @pre_txn carries a flip-txn the caller reserved before its own last fallible
 * step, so the spine-copy / graft commit below draws an unfailable txn instead
 * of allocating one.  NULL on the public paths (cds_ft_merge_at and the outer
 * cds_ft_rekey_* call), set only by the same-trie rekey's post-detach re-merge,
 * which pre-reserves it (sized from the O(1) subtree key counts:
 * the structural re-parent + folded ordered-list interleave, or the graft
 * cluster floor) so its post-detach merge cannot fail -- no reader-observable
 * rollback.  The consume site NULLs the slot it takes, so the rekey frees the
 * txn only when a given merge shape left it unused.
 */
static enum cds_ft_status ft_rekey_at_inner(struct cds_ft *dst_ft,
		const uint8_t *dst_key, size_t dst_key_len,
		struct cds_ft *src_ft,
		const uint8_t *src_key, size_t src_key_len,
		struct ft_flip_txn **pre_txn, enum ft_rekey_mode rekey)
{
#ifdef FT_DEBUG_SPLICE_SEAM
	uatomic_inc(&ft_ss_at_inner);
#endif
	struct cds_ft *subtree = NULL;
	enum cds_ft_status status;
	struct ft_descent d_src, d_dst;
	unsigned int off_src, off_dst;
	unsigned long cnt_src, cnt_dst;
	enum ft_graft_swap_case ks, kd;
	uint8_t okey_dst_buf[FT_MAX_KEY_LEN], okey_src_buf[FT_MAX_KEY_LEN];
	const uint8_t *okey_dst = dst_key, *okey_src = src_key;

	FT_TP(merge_enter, (const void *) dst_ft, (const void *) src_ft);

	if (!dst_ft || !src_ft) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (dst_ft->group != src_ft->group) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (dst_key_len > dst_ft->group->max_key_len ||
			src_key_len > dst_ft->group->max_key_len) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if ((dst_key_len > 0 && !dst_key) ||
			(src_key_len > 0 && !src_key)) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * Fixed-length groups: each source key K has length fixed_len.
	 * The stripped keys of the moved subtree have length
	 * (fixed_len - src_key_len).  The resulting destination key
	 * is dst_key || stripped, of length
	 * (dst_key_len + fixed_len - src_key_len).  fixed_len is the
	 * only key length the destination group accepts.  For the
	 * result to equal it, src_key_len and dst_key_len must be equal.
	 */
	if (dst_ft->group->key_len != CDS_FT_LEN_VARIABLE
			&& dst_key_len != src_key_len) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * merge_at is CROSS-TRIE only.  A same-trie move (src_ft == dst_ft) is a
	 * REKEY -- expressed by cds_ft_rekey_graft / cds_ft_rekey_merge, which reach
	 * this worker with @rekey set.  A same-trie cds_ft_merge_at (@rekey ==
	 * FT_REKEY_NONE) is rejected; use the dedicated rekey entry points instead.
	 */
	if (rekey == FT_REKEY_NONE && src_ft == dst_ft) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * A rekey on a SPECULATIVE (leaf-stored-key) trie is refused.  The
	 * move re-parents each leaf under @dst_key WITHOUT rewriting its
	 * app-owned stored key: the library never writes that field.  So
	 * every moved leaf would carry a key that no longer matches its
	 * structural position.  A speculative lookup would then return the
	 * wrong key.  ft_verify_speculative_key catches this only under
	 * FEATURE_FT_VERIFY_AT_MUTATION.  Only EAGER tries may rekey: they
	 * reconstruct the key from structure and never read the stored field.
	 */
	if (rekey != FT_REKEY_NONE && dst_ft->speculative_key_offset_active) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * The STAGED rekey needs a VARIABLE-length group, and the refusal belongs
	 * HERE, before anything is read or reserved.  The move is staged as a
	 * detach of @src_key's subtree into a transient trie followed by a merge of
	 * that trie back in at @dst_key, and a detached subtree carries keys
	 * STRIPPED of the prefix -- shorter than a fixed-length group's one key
	 * length, which is exactly why cds_ft_detach and cds_ft_graft take a
	 * non-root key on variable-length groups only.  The staged rekey is
	 * composed of those two operations, so it inherits the restriction.
	 *
	 * A fixed-length group is served by the ATOMIC writer instead
	 * (ft_rekey_one_decide, dispatched before this worker), which stages
	 * through no transient trie and so has no such restriction -- it is the
	 * only rekey a fixed-length group gets, and reaching here means its cut did
	 * not cover the shape.
	 *
	 * Enforcing it at the entry is what makes the refusal SAFE.  The placement
	 * is a merge of that transient at @dst_key, so it meets the fixed-length
	 * equal-prefix-length guard above with src_key_len == 0 != dst_key_len and
	 * refuses -- but only AFTER the detach has committed, leaving the caller an
	 * INVALID_ARGUMENT_ERROR (an "argument rejected, nothing happened" status)
	 * for a trie that has just lost every moved key to the destroyed transient.
	 */
	if (rekey != FT_REKEY_NONE &&
			dst_ft->group->key_len != CDS_FT_LEN_VARIABLE) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * MW LOCK_FINE (step 6, §9.5): a CROSS-trie merge consumes @src_ft,
	 * so @src_ft must be EXCLUSIVE.  An exclusive source skips its
	 * FT-wide lock.  Only dst's lock is left: one lock, no cross-trie
	 * deadlock.  A live (lock-mode, non-exclusive) cross-trie source is
	 * REJECTED with BUSY before any lock is taken.  The caller makes it
	 * exclusive first (cds_ft_make_exclusive).  A SAME-trie rekey
	 * (src == dst) is excluded: it takes one lock reentrantly.
	 * Inert outside lock-mode.
	 */
	if (src_ft != dst_ft && !src_ft->exclusive) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_BUSY_ERROR);
		return CDS_FT_STATUS_BUSY_ERROR;
	}

	/*
	 * merge_at is a mutator; the application provides mutual exclusion
	 * between mutators.  Take the reentrant writer-validation scope (like
	 * cds_ft_graft_swap), NOT a blanket flavor read lock.
	 *
	 * The reason is the REKEY entries, not merge_at.  cds_ft_merge_at
	 * consumes an EXCLUSIVE source.  So every ft_writer_lock_gp_wait on
	 * its path (both here and both in ft_graft_keylen) is
	 * !src_ft->exclusive-gated and never runs.  That path syncs nowhere,
	 * and the spine copy pins it under a read lock for exactly that
	 * reason.
	 * cds_ft_rekey_{graft,merge} share this body with src_ft == dst_ft, a
	 * LIVE trie.  There those waits DO run.  A read lock held across one
	 * would be a writer waiting on its own grace period.
	 * One body, two source contracts.  So the pin is taken per path, not
	 * here.
	 */
	ft_crosstrie_lock_mode_guard(dst_ft, src_ft);
	CDS_FT_SCOPED_WRITER(dst_ft);
	CDS_FT_SCOPED_WRITER(src_ft);

	/*
	 * Combined-length overflow validation (mirrors cds_ft_graft).  A
	 * moved key K becomes dst_key || (K - src_key prefix).  Its length is
	 * dst_key_len + len(K) - src_key_len.  Without this check a
	 * variable-length merge with dst_key_len > src_key_len could create
	 * keys longer than the group's max_key_len.  Such keys overflow the
	 * fixed-size key buffers downstream: the spine's compressed-wrap
	 * kbuf and the iterator buffers.  ft_moved_keys_overflow bounds
	 * len(K) exactly.
	 *
	 * ☞ UNDER THE LOCKS.  A same-trie rekey's point writers take this
	 * trie's FT-wide writer lock while the move gate is up.  The gate's
	 * own bulk lock does not exclude them.  So a moved-keys walk taken
	 * before the scopes above could miss a longer key inserted under
	 * @src_key after it.  It would then admit a move that overflows.
	 * Across tries the source is EXCLUSIVE, and a live one was refused
	 * BUSY just above.
	 */
	if (ft_moved_keys_overflow(src_ft, src_key, src_key_len,
			dst_key_len, dst_ft->group->max_key_len)) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_OVERFLOW_ERROR);
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	}

	/*
	 * Convert both keys to ORDINAL form ONCE.  Every internal consumer
	 * takes the ordinal form:
	 * - the merge-point descents;
	 * - the spine build;
	 * - the source-subtree unlink;
	 * - the ordered-list interleave.
	 * The detach+graft fallback below instead receives the ORIGINAL
	 * application keys: those entry points remap internally.
	 * Previously the descents consumed the application bytes raw while
	 * the unlink remapped.  On a non-identity key map the build then
	 * copied one subtree and the unlink targeted another.
	 */
	{
		const struct cds_ft_key_map *km = &dst_ft->group->key_map;

		if (caa_unlikely(!km->identity)) {
			ft_key_to_ordinals(okey_dst_buf, dst_key, dst_key_len,
				km);
			ft_key_to_ordinals(okey_src_buf, src_key, src_key_len,
				km);
			okey_dst = okey_dst_buf;
			okey_src = okey_src_buf;
		}
	}

	/*
	 * Same-trie "rekey" (src_ft == dst_ft): moving @src_key's subtree to
	 * @dst_key within one trie is allowed.  But the two keys must be
	 * DISJOINT: neither is a prefix of the other.  A prefix relationship
	 * means one key lies inside the other's subtree.  The move would
	 * then be circular, and @dst_key would not be absent.  Equal keys
	 * are a degenerate self-move.  The check is byte-position-wise.  It
	 * is identical in application and ordinal space, because the key map
	 * is a per-position bijection.  Cross-trie subtrees are disjoint by
	 * construction, so the guard is same-trie only.
	 */
	if (rekey != FT_REKEY_NONE) {
		size_t m = src_key_len < dst_key_len ? src_key_len : dst_key_len;

		if (m == 0 || memcmp(okey_src, okey_dst, m) == 0) {
			FT_TP(merge_exit,
				(int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
			return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
		}
	}

	/*
	 * Locate both merge points read-only (a writer descends its own
	 * stable state).  No content under @src_key means the merge is a
	 * no-op.  cnt_src == 0 covers an empty @src_ft root.  There
	 * src_key_len == 0, and the descent still reports EXACT at the
	 * always-present root.
	 */
	ks = ft_merge_descend(src_ft, okey_src, src_key_len, &d_src,
			&off_src, &cnt_src);
	MRG_SKIPCONF_PROBE(0, d_src);
	if (ks == FT_GRAFT_SWAP_DELEGATE || cnt_src == 0) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}

	/*
	 * Same-trie "rekey": detach @src_key's subtree into a transient trie,
	 * then merge that whole trie back in at @dst_key.  The detach fully
	 * commits -- recompacting the shared common-prefix ancestor and draining
	 * -- BEFORE the placement reads it, so the move's two sides no longer
	 * alias the same node.  The in-place reorder CANNOT be used here: its
	 * source unlink would recompact that shared ancestor out from under the
	 * build-invisible destination placement (a stale-slot publish).  The
	 * cross-trie merge handles an occupied @dst_key by MERGING and an absent
	 * one by grafting; it is made UNFAILABLE (its nodes + flip batches are
	 * pre-reserved before the detach below), so it always commits the move --
	 * there is no failure path that would restore the content to @src_key, i.e.
	 * no reader-observable rollback.
	 *
	 * A KEY_SHORTER source (off_src > 0, @src_key ends inside the compressed
	 * node @d_src.nf) is reduced to the EXACT node boundary first, exactly as
	 * the cross-trie path does: detach @cn_s->child via the full-compressed
	 * key (src prefix ++ all of cn_s), and merge at @dst_key extended by the
	 * residual cn_s bytes -- ft_detach_keylen overshoots a compressed node, so
	 * it must be handed a boundary key, not an interior one.
	 */
	/*
	 * ☠ THE STAGED REKEY WRITER IS GONE.  It detached the subtree into a
	 * TEMPORARY TRIE and merged it back.  So the moved keys were
	 * invisible to readers for a grace period per move.  The suite itself
	 * measured 1,163,089 absences over 18 moves.  An FT-wide writer lock
	 * does not redeem it.  RCU readers run CONCURRENTLY with the writer
	 * whatever the writer-exclusion mode.  So the absence window is a
	 * guarantee failure in every configuration, not a property of the
	 * fine-grained modes.
	 *
	 * A same-trie rekey is therefore served by the ATOMIC writer or not at
	 * all.  ft_rekey_one_decide commits the src clear, the dst publish and
	 * the re-parents as ONE flip.  ft_rekey_dispatch routes every shape
	 * its cut covers there.  Reaching this worker with @rekey set means
	 * the cut did NOT cover the shape.  Answer that, terminally.  Do not
	 * stage a move through a window no probe can talk away.
	 *
	 * (@rekey == FT_REKEY_NONE, the CROSS-TRIE cds_ft_merge_at, continues
	 * below.  It publishes build-invisibly into a separate destination
	 * trie and never hides a live key.)
	 */
	if (rekey != FT_REKEY_NONE)
		return CDS_FT_STATUS_NOT_SUPPORTED;

	bool md_rlock = false;
	bool md_contended;
	unsigned long md_spin = 0;	/* this op's merge_spine_retry depth (probe) */
	/*
	 * The op's PERSISTENT engine handle.  It spans the whole
	 * merge_spine_retry loop, the way ft_txn_op_init does for insert,
	 * remove and replace (doc §11).  Without it this loop ages nothing.
	 * Every attempt started at retry 0.  So the domain never escalated
	 * the writer into the per-trie FIFO fair-mutex lane, and the loop had
	 * no termination argument at all.  Measured under an exponential
	 * spacing: 585250 declined lock sets over 235141 merges, with one op
	 * re-descending 348 times.
	 *
	 * It is bound and bracketed ONLY on the arm that takes the read pin
	 * below.  That is not an optimization.  urcu_txn_begin() enters the
	 * RCU read side.  The SAME-TRIE rekey shares this body with a LIVE
	 * src, whose ft_writer_lock_gp_wait calls do run.  A read section
	 * held across one is a writer waiting on its own grace period.
	 * The condition `dst_ft->lock_fine && src_ft->exclusive` is exactly
	 * the source contract that syncs nowhere.  Every one of those 585250
	 * declines was measured there (livesrc=0).
	 */
	struct ft_op optxn;

	MRG_SPIN_PROBE(0);
	ft_txn_op_init(dst_ft, &optxn);

	/*
	 * Re-entered when a spine-copy attempt could not take its dup-chain
	 * lock set (@md_contended).  The attempt moved nothing.  An EXCLUSIVE
	 * source means the merge is build-invisible until its one commit.  So
	 * a declined attempt is trivially undone, and both tries are
	 * byte-for-byte as they were.  Re-descend, then rebuild.  The acquire
	 * declined because a peer is reshaping the dst spine we planned
	 * against.  This is the same plan->commit retry shape as
	 * cds_ft_graft's retry_attach and cds_ft_graft_swap's retry_swap.
	 */
merge_spine_retry:
	/*
	 * §11 cross-trie RCU-pinning: the spine-copy path below descends the
	 * live dst here.  Inside ft_rekey_spine_copy it node locks a
	 * descent-captured dst node (@d_dst->pnf / ->ppnf).  A node lock
	 * false-succeeds on a reclaimed+recycled node (the arena re-zeroes
	 * metadata).  So pin the captured nodes with the flavor read side
	 * across descent -> lock, as cds_ft_graft does.  The splice lock set
	 * acquires dup-chain holders.  They are captured during that same
	 * window, so this pin covers them too.
	 * With an EXCLUSIVE src the spine-copy's only grace period is its src
	 * drain.  That drain is !src_ft->exclusive-gated and skipped.  So the
	 * whole ft_rekey_spine_copy runs GP-free under the read lock.
	 * The pin is released:
	 * - before the spine-copy return;
	 * - on the fall-through to the detach/graft paths;
	 * - before each retry above (a retry re-descends, so it must re-pin
	 *   what it re-reads).
	 */
	if (dst_ft->lock_fine && src_ft->exclusive) {
		/*
		 * Open the attempt on the persistent handle FIRST.  A retry
		 * that has aged escalates here.  Escalation blocks on the
		 * domain's fair mutex.  That must not happen while holding
		 * the pin below.
		 * The explicit read_lock stays: it pins the descent-captured
		 * dst nodes.  It must not become conditional on the handle
		 * having a flavour bound (ft_txn_op_init binds NULL on an
		 * exclusive dst).  RCU read sections nest.  @md_rlock now
		 * marks both, and every site that releases it closes the txn
		 * too.
		 */
		ft_op_begin(&optxn);
		dst_ft->group->flavor->read_lock();
		md_rlock = true;
	}
	kd = ft_merge_descend(dst_ft, okey_dst, dst_key_len, &d_dst,
			&off_dst, &cnt_dst);
	MRG_SKIPCONF_PROBE(2, d_dst);
	/*
	 * Reanchor level-move: a peer chain-merge absorbed this slot's level
	 * into a longer compressed node while the descent walked it.  So
	 * @d_dst names a position that has moved.  Every count and offset
	 * derived from it describes the old shape.  Insert and graft already
	 * bail on this; merge read it and continued.
	 *
	 * Re-descend down the SAME unwind the spine copy's contention path
	 * uses.  Nothing is built here: the label is above the read-lock pin,
	 * and the detach branch returned long before.  So this is the state
	 * that path already returns to.  Release the pin first: a retry
	 * re-reads, so it must re-pin.
	 */
	if (caa_unlikely(d_dst.skip_conflict)) {
		if (md_rlock) {
			dst_ft->group->flavor->read_unlock();
			/* Nothing moved: age the conflict, close the attempt. */
			ft_txn_attempt_bail(&optxn, true);
			md_rlock = false;
		}
		md_spin++;
		MRG_SPIN_PROBE(2);
		MRG_SPIN_MAX(md_spin);
		goto merge_spine_retry;
	}

	/*
	 * Atomic build-invisible spine-copy of @src_ft's subtree at @src_key
	 * into @dst_ft's non-empty subtree at @dst_key.  Each side is one of:
	 * - EXACT (@off == 0): the subtree at @d->nf;
	 * - KEY_SHORTER (@off > 0): the key ends inside a compressed node.
	 *   The commit's unlink reclaims the src node.  The dst node is
	 *   wrapped under its prefix.
	 * ft_rekey_spine_copy handles every such shape: internal, external,
	 * compressed and compressed-parent dst merge points.  Only a dst that
	 * is empty under @dst_key falls through to the detach-based graft
	 * below.
	 */
	if ((ks == FT_GRAFT_SWAP_EXACT || ks == FT_GRAFT_SWAP_KEY_SHORTER)
			&& cnt_dst > 0
			&& (kd == FT_GRAFT_SWAP_EXACT
				|| kd == FT_GRAFT_SWAP_KEY_SHORTER)) {
#ifndef FEATURE_FT_MERGE
		/*
		 * ONLY THE REKEY GRAFT IS AVAILABLE with merge compiled out.
		 * @cnt_dst > 0 is an OCCUPIED destination, i.e. a merge.  It
		 * must NOT fall through to the empty-dst graft below.  That
		 * graft would attach into a subtree that already holds keys.
		 * Refuse it terminally, the way ft_rekey_graft_simple_attempt
		 * answers the same shape (-ENOTSUP ->
		 * CDS_FT_STATUS_NOT_SUPPORTED).
		 */
		if (md_rlock) {
			dst_ft->group->flavor->read_unlock();
			ft_op_end(&optxn);
			md_rlock = false;
		}
		return CDS_FT_STATUS_NOT_SUPPORTED;
#else
		md_contended = false;
#ifdef FT_DEBUG_SPLICE_SEAM
		uatomic_inc(&ft_ss_spine_gate);
#endif
		status = ft_rekey_spine_copy(dst_ft, src_ft, &d_src,
				okey_src, src_key_len, cnt_src, off_src,
				&d_dst, cnt_dst, off_dst, dst_key_len,
				pre_txn, &md_contended);
		if (md_contended) {
			/* Contention, nothing moved: re-pin, re-descend, rebuild. */
			if (md_rlock) {
				dst_ft->group->flavor->read_unlock();
				/*
				 * AGE IT.  This is the arm that spun 348 deep
				 * with nothing to make it terminate.
				 * ft_op_conflict carries the retry count on
				 * the persistent handle.  So this writer
				 * escalates into the FIFO lane and the
				 * contention drains.  end() then FORFEITS the
				 * turn: this is a pre-commit bail.  A bail
				 * that keeps its turn while the peer it waits
				 * on queues behind that same turn is the
				 * insert livelock.
				 */
				ft_txn_attempt_bail(&optxn, true);
				md_rlock = false;
			}
			md_spin++;
			MRG_SPIN_PROBE(1);
			if (src_ft->exclusive)
				MRG_SPIN_PROBE(4);
			MRG_SPIN_MAX(md_spin);
			goto merge_spine_retry;
		}
		if (status == CDS_FT_STATUS_OK) {
			/*
			 * Raise dst's max_used_key_len for the moved keys
			 * (dst_key_len + the longest stripped suffix), as the
			 * graft paths do.  Later graft overflow validations
			 * feed off it.
			 */
			size_t src_max = uatomic_load(&src_ft->max_used_key_len,
					CMM_RELAXED);
			size_t nm = src_max > src_key_len ?
				dst_key_len + (src_max - src_key_len) :
				dst_key_len;
			size_t dm = uatomic_load(&dst_ft->max_used_key_len,
					CMM_RELAXED);

			if (nm > dm)
				uatomic_store(&dst_ft->max_used_key_len, nm,
					CMM_RELAXED);
		}
		if (md_rlock) {
			dst_ft->group->flavor->read_unlock();
			ft_op_end(&optxn);
			md_rlock = false;
		}
		FT_TP(merge_exit, (int) status);
		return status;
#endif /* FEATURE_FT_MERGE */
	}
	/*
	 * Fall-through: the spine-copy shape did not apply.  This is a
	 * whole-source move, or an empty dst under @dst_key (-> the detach
	 * graft below).  Release the spine-copy descent pin here.  The
	 * src_key_len==0 graft re-descends under its own bracket above.  The
	 * detach-graft path re-resolves its own (parent, slot).
	 */
	if (md_rlock) {
		dst_ft->group->flavor->read_unlock();
		ft_op_end(&optxn);
		md_rlock = false;
	}

	/*
	 * Whole-source move (src_key_len == 0): the entire @src_ft is the
	 * payload.  So graft it directly: no detach, no fallible rollback.
	 * ft_graft is itself leak-free:
	 * - a diverge split is built invisibly;
	 * - on OOM a NOSPLIT attach restores the saved old source root
	 *   without allocating.
	 * So an allocation failure leaves both tries pristine.  This covers
	 * an empty dst root and a diverged dst_key alike.  It is also the
	 * leak-free path for "rekey within a trie".  That is: detach a
	 * sub-trie, then merge_at it at a new, currently-absent key in the
	 * same group.
	 * A dst_key occupied by an empty-internal leftover yields
	 * POPULATED_ERROR here, exactly as the detach-then-graft path did.
	 */
	if (src_key_len == 0) {
		/*
		 * A live (non-exclusive) cross-trie source was already
		 * rejected with BUSY at merge_at's entry.  So this graft sees
		 * an exclusive source, or a same-trie rekey.  ft_graft_keylen
		 * runs its fused body directly.
		 */
		if (dst_ft->lock_fine && src_ft->exclusive) {
			const struct rcu_flavor_struct *flavor =
				dst_ft->group->flavor;

			/*
			 * §11 cross-trie RCU-pinning: this is the SAME live-dst
			 * descent + node lock as cds_ft_graft.  It is reached
			 * through cds_ft_merge / cds_ft_merge_at.  So it needs
			 * the same read-side bracket.  The bracket pins
			 * descent-captured dst spine nodes against a peer
			 * relocate+free+recycle (see the bracket in
			 * cds_ft_graft).  With an EXCLUSIVE src the fused body
			 * takes no grace period (ft_writer_lock_gp_wait is
			 * !exclusive-gated).  So the whole-op read lock cannot
			 * self-deadlock.
			 *
			 * That is the SAME condition, and the same reason, as
			 * the spine-copy pin above.  Both branches read_lock()
			 * under exactly
			 * `dst_ft->lock_fine && src_ft->exclusive`.  The
			 * SOURCE CONTRACT decides it, not the branch.  A
			 * cross-trie src is exclusive, or the op was already
			 * refused with BUSY.  Every grace period on that path
			 * is !exclusive-gated.  Only the same-trie rekey
			 * (src == dst, a LIVE src) actually runs those waits.
			 * This condition excludes exactly that case from the
			 * read section.
			 */
			flavor->read_lock();
			status = ft_graft_keylen(dst_ft, dst_key, dst_key_len,
					src_ft, pre_txn);
			flavor->read_unlock();
		} else
			status = ft_graft_keylen(dst_ft, dst_key, dst_key_len,
					src_ft, pre_txn);
		FT_TP(merge_exit, (int) status);
		return status;
	}

	/*
	 * Empty dst ROOT (dst_key_len == 0, so the whole @dst_ft is empty at
	 * the merge point): move @src_ft@src_key in WITHOUT a fallible graft.
	 * So no rollback can strand the moved externals.  The steps:
	 * - pre-allocate the source's replacement root (fallible while
	 *   @src_ft is still pristine);
	 * - detach the source subtree (clean on its own failure);
	 * - SWAP it into @dst_ft's root.
	 * The swap is a failure-free root-to-root move.  It changes no
	 * parent pointer and has no "jump out" window: both ends are roots
	 * with parent NULL.  The detach already drained @src_ft's readers.
	 * This is the same reasoning as ft_graft's root path.  Nothing
	 * fallible follows the detach.  So this path has no rollback and
	 * cannot leak (contrast the diverged path below).
	 */
	if (dst_key_len == 0 && cnt_dst == 0) {
		struct cds_ft_inode *fresh_root;
		struct cds_ft_metadata *fresh_meta;
		struct cds_ft_metadata *dst_rmeta;
		struct cds_ft_inode_flag *dst_root_fenced;
		/*
		 * DEDUPED, not taken: an outer frame owns this fence and
		 * owns its release.  Clearing it here drops a fence the op
		 * still writes under.  One owner per fence.
		 */
		bool dst_root_shared = false;
		uintptr_t dst_root_snap;
		struct cds_ft_inode *old_dst_root;
		struct ft_flip_txn *appear_txn = NULL;
		size_t sm;
		int fence_ret;

		/*
		 * @cnt_dst was sampled far above.  An ENTIRE ft_detach_keylen
		 * of the source runs before the swap below.  This is the
		 * widest empty-dst window in the FT.  Re-decide emptiness
		 * UNDER the old root's node lock.  A contract-legal peer
		 * attach can then no longer land inside that window and be
		 * freed with the root it landed on.
		 * On a peer-populated dst, fall through to the DIVERGED path
		 * below, which merges into a populated destination.  An
		 * up-front cnt_dst != 0 would have done exactly that.  On a
		 * held root, report BUSY.
		 */
		fence_ret = ft_root_attach_fence_empty(dst_ft, &dst_root_fenced,
			&dst_rmeta, &dst_root_snap, &dst_root_shared,
			&optxn);
		if (fence_ret == -EEXIST)
			goto diverged;
		if (fence_ret) {
			status = CDS_FT_STATUS_BUSY_ERROR;
			goto out;
		}

		fresh_root = alloc_cds_ft_node(src_ft, &ft_types[0], &fresh_meta);
		if (!fresh_root) {
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			status = CDS_FT_STATUS_MEMORY_ERROR;
			goto out;
		}
		fresh_meta->parent_word = ft_trie_parent(src_ft);
		ft_nr_keys_store(src_ft, fresh_meta, 0, CMM_RELAXED);

		/*
		 * Pre-reserve the dst-appear root-swap txn BEFORE the detach.
		 * The swap is failure-free (post-detach), so it commits
		 * through this pre-reserved txn (ft_ord_cell_flip_into).  OOM
		 * here aborts while @src_ft is still pristine (no detach
		 * yet).  The dst old-root freeze-on-free tombstone rides the
		 * SAME flip (atomic detach, §4.B).  With the list on that is
		 * +1 edge.  With the list off it is a 2-edge txn (root edge
		 * + tombstone) instead of the former lone ft_root_edge_flip
		 * store.
		 */
		if (dst_ft->group->ordered_list_set)
			appear_txn = ft_flip_txn_create_bounded(dst_ft,
				FT_ROOT_LIST_SWAP_MAX_EDGES + 1);
		else
			appear_txn = ft_flip_txn_create_bounded(dst_ft, 2);
		if (!appear_txn) {
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			free_cds_ft_node_unpublished(src_ft, fresh_root);
			status = CDS_FT_STATUS_MEMORY_ERROR;
			goto out;
		}

		status = ft_detach_keylen(src_ft, src_key, src_key_len, &subtree);
		if (status < 0) {
			/* NOT_FOUND impossible: @src_ft had content. */
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			if (appear_txn)
				ft_flip_txn_destroy(appear_txn);
			free_cds_ft_node_unpublished(src_ft, fresh_root);
			goto out;
		}

		/*
		 * Failure-free swap.  @subtree->root carries the moved content
		 * with parent already NULL (set by the detach), so it drops
		 * straight into @dst_ft's empty root slot.  @subtree keeps the
		 * pre-allocated empty root so its destroy below frees nothing of
		 * the moved content.  @dst_ft was empty, so it adopts @subtree's
		 * whole ordered cell list wholesale (head/tail endpoints only).
		 */
		/*
		 * The FENCED root, not a fresh read.  Under the fence the two
		 * are equal by construction.  Using the fenced value keeps
		 * the retire, the swap's expected-old and the free naming ONE
		 * node.
		 */
		old_dst_root = ft_node_ptr(dst_root_fenced);
		/*
		 * Fuse the structural root swap with the ordered-list
		 * head/tail transfer into ONE flip
		 * (ft_root_list_swap_publish).  A reader then never sees the
		 * moved keys reachable in @dst_ft's structure while its
		 * ordered list is still empty.  That is the dst-appear
		 * cross-view window.  It is closed the same way as on the
		 * graft empty-dst path.
		 * Only @dst_ft has concurrent readers here.  @subtree is the
		 * fresh EXCLUSIVE trie the detach above just produced.  So
		 * its root reset + head/tail clear are plain stores.  There
		 * is no src-side disappear window, unlike ft_graft's
		 * cross-trie empty-dst.
		 *
		 * Freeze-on-free (doc §4.B): this swap retires the dst old
		 * root.  The tombstone of that root is recorded INTO the swap
		 * txn.  So the mark and the unlink flip atomically (atomic
		 * detach).  The path is failure-free past the detach above,
		 * so this runs only on the committing path.
		 */
		/*
		 * @subtree->root becomes @dst_ft's root.  So re-name its
		 * owner before either branch publishes it.  A cross-trie move
		 * leaves the back-edge naming the trie it came FROM.  That is
		 * exactly the aliasing cds_ft_verify reports at depth 0.
		 * @subtree is the fresh EXCLUSIVE detach product with no
		 * readers, so the store is safe ahead of the flip.
		 */
		cds_ft_item_to_metadata(ft_node_ptr(subtree->root))->parent_word =
			ft_trie_parent(dst_ft);
		if (dst_ft->group->ordered_list_set) {
			/*
			 * dst FILLS by adopting @subtree's whole list.
			 * @subtree is the fresh EXCLUSIVE detach product (no
			 * readers).  So its incoming run relinks to dst's
			 * sentinel in the SAME flip (relink_dest = subtree,
			 * relink_incoming = true).  @subtree's sentinel resets
			 * to empty with a plain store.
			 */
			ft_flip_txn_lock_register(appear_txn, dst_rmeta,
				dst_root_snap);
			ft_flip_txn_record_tombstone_locked(appear_txn,
				dst_rmeta, dst_root_snap);
			ft_root_list_swap_publish(dst_ft, appear_txn, &dst_ft->root,
				dst_root_fenced, subtree->root,
				NULL, ft_ord_first(subtree),
				NULL, ft_ord_last(subtree), subtree, true);
			urcu_txn_list_init(&subtree->ord_sentinel);
		} else {
			/*
			 * No ordered list: dst's root is the only reader-visible
			 * structural slot.  Commit the appear root edge and the
			 * old-root tombstone as ONE 2-edge flip through the
			 * pre-reserved txn.  A lone root edge would reduce to a
			 * release store.  The fused tombstone makes it
			 * multi-edge.  Readers resolve the transient root
			 * proxy exactly as on the list-on path.
			 * (@subtree is the fresh EXCLUSIVE trie, so its root reset
			 * below stays a plain store.)
			 */
			/* Stage 5: the root's own lock, so this record can park SW. */
			(void) ft_root_lock_take(dst_ft, NULL, appear_txn);
			ft_flip_txn_record_root(appear_txn,
				(void **) &dst_ft->root,
				(void *) dst_root_fenced, (void *) subtree->root);
			ft_flip_txn_lock_register(appear_txn, dst_rmeta,
				dst_root_snap);
			ft_flip_txn_record_tombstone_locked(appear_txn,
				dst_rmeta, dst_root_snap);
			ft_flip_txn_commit(dst_ft, appear_txn);
		}
		FT_TP(root_publish, (const void *) dst_ft,
			(const void *) dst_ft->root);
		subtree->root = ft_node_flag(fresh_root, 0);
		free_cds_ft_node(dst_ft, old_dst_root);

		/* dst_key_len == 0: dst keys equal the moved keys, same lengths. */
		sm = uatomic_load(&subtree->max_used_key_len, CMM_RELAXED);
		if (sm > uatomic_load(&dst_ft->max_used_key_len, CMM_RELAXED))
			uatomic_store(&dst_ft->max_used_key_len, sm, CMM_RELAXED);

		cds_ft_destroy(subtree);
		FT_TP(merge_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}

diverged:
	/*
	 * Sub-position source into a dst absent at @dst_key: graft the source
	 * subtree IN PLACE (no detach, no re-root).  The build-invisible
	 * cluster then references @d_src.nf directly, and the source unlink
	 * is the last fallible step.  This is the leak-free reorder.
	 * It handles every source-root shape: plain multi-child internal, an
	 * internal carrying external_nodes, external, compressed, 1-child.
	 * It handles them at both a GLUE diverge and a NOSPLIT (at-node /
	 * build-branch) dst point.
	 *
	 * A KEY_SHORTER source reduces to the EXACT shape.  There the src key
	 * ends @off_src bytes inside the compressed node @d_src.nf.  The moved
	 * subtree is @cn_s->child.  Its keys gain the residual bytes
	 * cn_s->key_bytes[off_src .. len) as a prefix.  So graft @cn_s->child
	 * at @dst_key extended by that residual.
	 * ft_merge_unlink_src_subtree overshoots @cn_s (it preserves
	 * @cn_s->child), so the source side is identical.  That unlink also
	 * reclaims @cn_s itself.
	 */
	if (ks == FT_GRAFT_SWAP_EXACT || ks == FT_GRAFT_SWAP_KEY_SHORTER) {
		bool handled;
		struct cds_ft_inode_flag *payload;
		const uint8_t *gokey = okey_dst, *gappkey = dst_key;
		size_t glen = dst_key_len;
		uint8_t ext_okey[FT_MAX_KEY_LEN], ext_app[FT_MAX_KEY_LEN];

		if (ks == FT_GRAFT_SWAP_EXACT) {
			payload = d_src.nf;	/* off_src == 0 by construction */
		} else {
			struct cds_ft_compressed_node *cn_s =
				ft_compressed_node_ptr(d_src.nf);
			unsigned int rlen = cn_s->len - off_src, j;
			const struct cds_ft_key_map *km =
				&dst_ft->group->key_map;

			payload = cn_s->child;
			memcpy(ext_okey, okey_dst, dst_key_len);
			memcpy(&ext_okey[dst_key_len], &cn_s->key_bytes[off_src],
				rlen);
			memcpy(ext_app, dst_key, dst_key_len);
			if (km->identity) {
				memcpy(&ext_app[dst_key_len],
					&cn_s->key_bytes[off_src], rlen);
			} else {
				for (j = 0; j < rlen; j++)
					ext_app[dst_key_len + j] =
						km->ordinal_to_key[
						cn_s->key_bytes[off_src + j]];
			}
			gokey = ext_okey;
			gappkey = ext_app;
			glen = dst_key_len + rlen;
		}

		status = ft_rekey_subpos_inplace(dst_ft, src_ft,
			gokey, gappkey, glen, okey_src, src_key_len,
			payload, cnt_src, &handled);
		if (handled) {
			FT_TP(merge_exit, (int) status);
			return status;
		}
	}

	/*
	 * Unreachable.  ft_merge_descend reports EXACT, KEY_SHORTER or
	 * DELEGATE.  DELEGATE returned OK at the top (cnt_src == 0).  Every
	 * EXACT / KEY_SHORTER source is handled above:
	 * - a non-empty dst by the build-invisible spine copy;
	 * - an empty dst root or whole-source move by their failure-free root
	 *   swaps;
	 * - a diverged / absent dst point by ft_rekey_subpos_inplace (which
	 *   always reports handled).
	 * No shape falls through.  So the old detach-then-graft fallback and
	 * its rollback are gone.  That fallback was the last path that
	 * carried the double-OOM leak.
	 */
	assert(0 && "cds_ft_merge_at: unhandled merge shape");
	status = CDS_FT_STATUS_OK;
out:
	FT_TP(merge_exit, (int) status);
	return status;
}

static
enum cds_ft_status ft_rekey_dispatch(struct cds_ft *ft,
		const uint8_t *dst_key, size_t dst_key_len,
		const uint8_t *src_key, size_t src_key_len,
		enum ft_rekey_mode rekey)
{
	bool require_empty = (rekey == FT_REKEY_GRAFT);
	enum cds_ft_status status;

	if (!ft)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	/*
	 * ☞ THE OPT-IN.  It is checked FIRST: before the gate and before
	 * any argument validation.  A trie that did not declare the
	 * capability must not pay a grace period, or take a lock, to be told
	 * so.  cds_ft_group_attr_set_rekey, default OFF.
	 *
	 * ☠ AND A REFUSAL HERE IS A CLAIM ABOUT THE TRIE: the caller gets
	 * NOT_SUPPORTED and the trie is UNCHANGED, never a silent no-op.  The
	 * hazard this creates is on the TEST side.  A suite whose trie creator
	 * forgets to opt in keeps calling rekey and gets -7 every time.  It
	 * then reads as green with zero rekey coverage.  This tree has been
	 * bitten by that wrong-green shape before.  So arm
	 * FT_DEBUG_REKEY_OPTIN_STRICT in at least one gate configuration.  It
	 * turns a refusal into an abort that names the trie.  That is the
	 * only form of the check a green run cannot absorb.
	 */
	if (!ft->group->rekey_set) {
#ifdef FT_DEBUG_REKEY_OPTIN_STRICT
		fprintf(stderr, "[Fatal] Fractal Trie: cds_ft_rekey_* on cds_ft=%p "
			"whose group did NOT opt in (cds_ft_group_attr_set_rekey). "
			"Under -DFT_DEBUG_REKEY_OPTIN_STRICT this aborts rather "
			"than returning NOT_SUPPORTED, so a test that lost its "
			"rekey coverage cannot report green.\n", (void *) ft);
		fflush(stderr);
		abort();
#else
		return CDS_FT_STATUS_NOT_SUPPORTED;
#endif
	}
	/*
	 * The atomic writer's shape-independent preconditions.  They are
	 * checked BEFORE the gate.  A trie that can never use the writer then
	 * does not pay a grace period to be told so.  Everything else the
	 * writer decides for itself, from the structure.
	 *
	 * ★ THIS LIST ALSO CARRIES EVERY ARGUMENT CHECK THE ATOMIC WRITER DOES NOT
	 * REPEAT.  ft_rekey_at_inner owns the entry contract:
	 * - NULL keys;
	 * - a length past the group maximum;
	 * - a SPECULATIVE trie (a move re-parents a leaf but cannot rewrite
	 *   its app-owned stored key, so every moved key would lie).
	 * It returns INVALID_ARGUMENT_ERROR for each.  Excluding them here,
	 * rather than re-checking them, keeps that contract in ONE place.
	 * A rejected argument falls through to the worker that defines the
	 * answer.  Only a request that is VALID and merely outside the atomic
	 * cut is a fallback.
	 */
	/*
	 * ★ NO EQUAL-LENGTH TERM.  The one decide used to require it.  The
	 * two lengths are now independent.  A longer destination can produce
	 * a combined-length OVERFLOW.  That is answered where the rest of the
	 * entry contract lives: the writer reports the shape as UNCOVERED and
	 * the fallback below returns OVERFLOW_ERROR.
	 * Both lengths are still bounded here, because a key longer than the
	 * group maximum is an ARGUMENT error.  This gate's job is only to
	 * keep the writer off requests it has no business deciding.
	 */
	bool one_decide =
		!ft->speculative_key_offset_active &&
		src_key && dst_key &&
		src_key_len != 0 && dst_key_len != 0 &&
		src_key_len <= ft->group->max_key_len &&
		dst_key_len <= ft->group->max_key_len;

	ft_move_gate_enter(ft);
#ifndef FT_DEBUG_REKEY_IN_PLACE
	ft_tls_in_place_off++;		/* see ft_tls_in_place_off */
#endif
#ifdef FT_RED_REKEY_NOLOCK
	ft_red_rekey_nolock = 1;	/* red control; see fractal-trie-internal.h */
#endif
	if (one_decide) {
		switch (ft_rekey_one_decide(ft, src_key, src_key_len, dst_key,
				dst_key_len, require_empty)) {
		case 0:
			status = CDS_FT_STATUS_OK;
			goto out;
		case -EEXIST:
			status = CDS_FT_STATUS_POPULATED_ERROR;
			goto out;
		case -ENOMEM:
			status = CDS_FT_STATUS_MEMORY_ERROR;
			goto out;
		case -ENOTSUP:
			/* Feature compiled out: no cut widens past that. */
			status = CDS_FT_STATUS_NOT_SUPPORTED;
			goto out;
		case -EINVAL:
			/*
			 * ARGUMENT error, now TERMINAL.  It used to fall into
			 * the fallback below together with the uncovered
			 * shapes.  So a caller passing a zero-length or
			 * overlapping key had its mistake answered by STAGING
			 * a move instead of by a refusal.
			 */
			status = CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
			goto out;
		default:
			/*
			 * FT_REKEY_UNCOVERED only: the one code that may fall
			 * back, because it is the only one a wider cut closes.
			 */
			break;
		}
	}
	/*
	 * The staged fallback, now on the REKEY TWIN.  Here src_ft == dst_ft.
	 * So this path and the cross-trie ft_merge_at_inner no longer share a
	 * body.  The two disagree about the src-side atomicity of that body.
	 */
	status = ft_rekey_at_inner(ft, dst_key, dst_key_len, ft,
			src_key, src_key_len, NULL, rekey);
out:
#ifdef FT_RED_REKEY_NOLOCK
	ft_red_rekey_nolock = 0;
#endif
#ifndef FT_DEBUG_REKEY_IN_PLACE
	ft_tls_in_place_off--;
#endif
	ft_move_gate_exit(ft);
	return status;
}

/*
 * COMPILED IN ON EVERY BUILD, INCLUDING -DNO_FEATURE_FT_MERGE.  The empty-dst
 * move is a GRAFT, not a union.  So none of the merge subsystem's machinery is
 * on its path: ft_merge_ctx, ft_merge_build, ft_merge_ord_interleave_collect.
 * That is a fact about the CALLER, not a hope about the callee.
 * @require_empty is true for FT_REKEY_GRAFT.  ft_rekey_one_decide answers an
 * occupied destination with -EEXIST (POPULATED_ERROR) BEFORE any merge_dst arm
 * runs.  So every #ifdef FEATURE_FT_MERGE region below that point is
 * unreachable from this entry on EVERY build, not merely on this one.  Those
 * regions are the ft_merge_build union, the interleave splice and the
 * reservation term.
 *
 * Gating the entry itself contradicted two things: the comment on
 * ft_rekey_spine_copy ("only the rekey GRAFT -- the empty-dst shape -- remains
 * available") and the public contract.  It also cost the whole same-trie move
 * on a merge-less build for nothing.
 * ☞ This does NOT make the call infallible: ft_rekey_at_inner still refuses
 * any rekey shape the atomic writer's cut misses, on every build.
 */
enum cds_ft_status cds_ft_rekey_graft(struct cds_ft *ft,
		const uint8_t *dst_key, size_t dst_key_len,
		const uint8_t *src_key, size_t src_key_len)
{
	FT_BW_OP(FT_BW_REKEY_GRAFT);
	return ft_rekey_dispatch(ft, dst_key, dst_key_len, src_key, src_key_len,
			FT_REKEY_GRAFT);
}

/*
 * MERGE-ONLY, unlike its graft sibling above.  The whole purpose of this entry
 * is the OCCUPIED destination.  That is a union built out of the merge
 * subsystem.  With -DNO_FEATURE_FT_MERGE it answers NOT_SUPPORTED, as
 * cds_ft_merge and cds_ft_merge_at do.
 */
enum cds_ft_status cds_ft_rekey_merge(struct cds_ft *ft,
		const uint8_t *dst_key, size_t dst_key_len,
		const uint8_t *src_key, size_t src_key_len)
{
#ifdef FEATURE_FT_MERGE
	FT_BW_OP(FT_BW_REKEY_MERGE);
	return ft_rekey_dispatch(ft, dst_key, dst_key_len, src_key, src_key_len,
			FT_REKEY_MERGE);
#else
	(void) ft; (void) dst_key; (void) dst_key_len;
	(void) src_key; (void) src_key_len;
	return CDS_FT_STATUS_NOT_SUPPORTED;
#endif
}
