// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-iter.h
 *
 * Userspace RCU library - Fractal Trie: the public iterator (create/destroy/get_key/set_key/bind/reset/copy) and batched cell next/prev iteration.
 *
 * Implementation unit: #included once into the fractal-trie.c translation
 * unit (preserves cross-module inlining).  Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-iter.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * Sentinel stored in iter->key_len: the length is DEFERRED.  The ordinal-cell
 * land stores it for a VARIABLE-length identity group.
 * ft_iter_resolve_key_len() resolves it on demand.  The length comes from the
 * leaf (key_len_offset) when present, else from the parent up-walk.  A keyless
 * cell walk therefore reads neither the key nor the length.
 *
 * SIZE_MAX is never a valid key length (max_key_len bounds it).  A consumer
 * that forgets to resolve hits an obvious overflow, not a silently-stale value.
 */
#define FT_ITER_KEY_LEN_LAZY	((size_t) -1)

/*
 * Lazy-ref accessor model (scoped to the ordinal-cell ordered list).
 *
 * This applies to a cell group with a leaf-key offset and an identity key
 * map.  There, the result key of an ordered iteration is held as a LIVE
 * REFERENCE into the matched leaf (iter->node + speculative_key_offset).  It
 * is not copied into iter_key(iter) on every cell-walk step.
 *
 * That removes the per-step key copy, and it saves a real leaf load.  The
 * cell walk never touches the leaf otherwise: its node and ord_next co-reside
 * in the 32B cell.  The descent path differs: its going-up anchor would load
 * the leaf regardless.  That is why the by-reference key was a wash there.
 *
 * iter_key(iter) is therefore NOT the current key for such a position.  Every
 * reader of the current-position key MUST go through ft_iter_read_key().
 * Missing one silently corrupts (e.g. cds_ft_remove_all locating a wrong key).
 */
static inline_lookup
bool ft_iter_key_referenced(const struct cds_ft_iter *iter)
{
	const struct cds_ft_group *group = iter->ft->group;

	return group->ordered_list_set && iter->ft->speculative_key_offset_active &&
		group->key_map.identity && iter->cache_valid && iter->node;
}

/*
 * Resolve the current head's cell for a cell-walk step:
 *  - When the cached cursor still refers to iter->node, use it.  This does
 *    not touch the leaf.
 *  - Else re-enter the walk via the head's prev.  This costs one leaf load,
 *    paid once per walk entry.
 * Return NULL when there is no cell to re-enter from.
 *
 * ☠ A CHAIN MEMBER'S prev IS A NODE, NOT A CELL.  @iter->node was a head when
 * the reader found it.  But a concurrent same-parent rekey merge splices a
 * moved head into an existing chain.  This DEMOTES the head in place: its
 * prev becomes the new predecessor node.
 *
 * Cast as a cell, that node's bytes gave the up-walk a NULL "parent" and a
 * heap address as the cell.  cds_ft_item_to_metadata faulted on it.
 * MEASURED: inv_rekey_merge_same_parent_coherent_readers, SIGSEGV in a reader
 * on every run.
 *
 * A member carries its head's key, so the head's cell is the member's
 * position.  Hop to it.  The hop is bounded exactly as ft_ext_head_word
 * bounds the walk.
 */
static inline_lookup
struct ft_ord_cell *ft_ord_cell_cursor(const struct cds_ft_iter *iter)
{
	struct cds_ft_node *cur = iter->node;
	void *prev;
	unsigned long hops = 0;

	if (iter->ord_cell_node == iter->node)
		return iter->ord_cell;
	prev = ft_dereference_prev_resolved(cur);
#ifndef FT_DEBUG_NO_CURSOR_MEMBER_HOP
	while (prev && ft_node_external((struct cds_ft_inode_flag *) prev)) {
		if (++hops > FT_EXT_CHAIN_HOPS_MAX)
			return NULL;
		cur = (struct cds_ft_node *) prev;
		prev = ft_dereference_prev_resolved(cur);
	}
#else
	(void) hops;
#endif
	return prev ? ft_ord_cell_ptr(prev) : NULL;
}

/*
 * The climb's recent history, recorded only by the violation-dump build below.
 * Declared unconditionally so the walk can carry a NULL pointer to it in an
 * ordinary build without a second set of declarations.
 */
#define FT_UPWALK_HIST	8
struct ft_upwalk_hist {
	struct cds_ft_inode_flag *nf;
	uintptr_t state;
	void *rcu_func;		/* cds_ft_free_item_rcu => queued/freed */
	struct cds_ft_inode_flag *parent_raw;	/* meta->parent_word, unlaundered */
	struct cds_ft_inode_flag **slot;	/* the (parent, PSO) pair's slot */
	struct cds_ft_inode_flag *slot_val;	/* what that slot actually holds */
};

#ifdef FT_DEBUG_PARENT_VIOLATION
#include <stdio.h>
/*
 * One flag arms BOTH parent-word dumps: this one and ft_get_parent_rcu's in
 * ft-helpers.h.  They check the same invariant at the two loads that carry
 * it.  A report from either is only readable beside the other.
 *
 * Self-diagnosing form of the parent-word assert below.  The bare assert says
 * only THAT a parent read is illegal.  At -O1 every local naming WHICH node
 * produced it is optimized out of the core.  The interesting half is then
 * unrecoverable: the node whose @parent_word yielded the bad value.  That
 * node is the one suspected of having been freed while still referenced.
 * Dump it at the point of detection instead, where the walk is still live.
 *
 * @from is that node (NULL when the bad value came from @cell->parent itself).
 *
 * The two words at @bad are printed raw.  A metadata slot returned to its
 * range freelist has @free_list_next over @parent_word.  A stale parent read
 * therefore lands on a struct cds_ft_metadata_alloc, whose rcu_head is
 * {next, func}.  A @bad[1] that symbolizes to cds_ft_free_item_rcu is the
 * freed-item signature.  Symbolize offline: that callback is static to
 * fractal-trie-alloc.c.
 *
 * The dump names the node whose parent_word was clobbered.  But the DANGLING
 * LINK is one level BELOW that, on the last node still alive.  That link is
 * what has to be explained.  @hist therefore carries each level.  The
 * live -> freed boundary is then visible in the dump, not inferred from it.
 *
 * Naming the boundary is still not naming the STALE EDGE.  A live node whose
 * parent_word reaches a freed node is equally consistent with two causes.
 *  (a) A retire missed this child's back-pointer.
 *  (b) The back-pointer was never stale at all.  The node ABOVE it was
 *      itself freed and reallocated under the link that reached it.
 * The FORWARD edge tells the two apart.  This node's own (parent, PSO) pair
 * derives a slot.  That slot must hold this very node.  The DEEPEST level
 * whose slot does NOT hold it is where the structure first disagrees with
 * itself.  The level below that names the op to look at.
 */
__attribute__((noinline, cold, unused))
static void ft_upwalk_parent_violation(const struct cds_ft *ft,
		struct ft_ord_cell *cell, struct cds_ft_inode_flag *from,
		struct cds_ft_inode_flag *bad, unsigned int level,
		const struct ft_upwalk_hist *hist)
{
	fprintf(stderr, "FT_UPWALK_VIOLATION: illegal parent %p at level %u\n",
		(void *) bad, level);
	fprintf(stderr, "  ft=%p cell=%p cell->node=%p cell->parent=%p\n",
		(const void *) ft, (void *) cell,
		cell ? (void *) cell->node : NULL,
		cell ? (void *) cell->parent : NULL);
	if (cell) {
		struct cds_ft_metadata *cm = cds_ft_item_to_metadata(cell);

		fprintf(stderr, "  cell meta=%p parent_word=%p state=0x%lx\n",
			(void *) cm, (void *) cm->parent_word,
			(unsigned long) cm->state);
	}
	if (from) {
		struct cds_ft_metadata *fm = ft_flag_to_metadata(ft, from);

		fprintf(stderr, "  from=%p meta=%p parent_word=%p external_nodes=%p state=0x%lx\n",
			(void *) from, (void *) fm, (void *) fm->parent_word,
			(void *) fm->external_nodes, (unsigned long) fm->state);
		fprintf(stderr, "  from rcu_head words: %p %p\n",
			((void **) fm)[-2], ((void **) fm)[-1]);
	}
	fprintf(stderr, "  bad rcu_head words: %p %p\n",
		((void **) bad)[0], ((void **) bad)[1]);
	if (hist) {
		unsigned int i, n = level < FT_UPWALK_HIST ?
			level : FT_UPWALK_HIST;

		/*
		 * DIFFERS is flagged on the RAW words, deliberately.
		 * Resolving a skip form here would dereference a slot that
		 * is already suspect.  A fault there could lose the whole
		 * dump.  A slot holding the SKIP form of the same node
		 * differs raw-wise.  So decode any flag offline before
		 * calling the slot stale.  Both words are printed for
		 * exactly that.
		 */
		fprintf(stderr, "  climb (deepest first), TOMB = state bit 1,\n"
			"  DIFFERS = the slot my own (parent, PSO) pair derives does not hold me (raw):\n");
		for (i = 0; i < n; i++)
			fprintf(stderr, "    L%u nf=%p state=0x%lx%s rcu_func=%p praw=%p slot=%p *slot=%p%s\n",
				i, (void *) hist[i].nf,
				(unsigned long) hist[i].state,
				(hist[i].state & FT_STATE_TOMBSTONE) ?
					" TOMB" : " live",
				hist[i].rcu_func,
				(void *) hist[i].parent_raw,
				(void *) hist[i].slot,
				(void *) hist[i].slot_val,
				(hist[i].slot && hist[i].slot_val != hist[i].nf) ?
					"  <== DIFFERS" : "");
	}
	fflush(stderr);
	abort();
}
# define ft_upwalk_check_parent(ft, cell, from, bad, level, hist)	\
	do {								\
		if (caa_unlikely((bad) && ft_node_external(bad)))	\
			ft_upwalk_parent_violation((ft), (cell), (from),	\
					(bad), (level), (hist));	\
	} while (0)
#else
# define ft_upwalk_check_parent(ft, cell, from, bad, level, hist)	\
	do {								\
		(void) (cell); (void) (from); (void) (level);		\
		(void) (hist);						\
		assert(!(bad) || !ft_node_external(bad));		\
	} while (0)
#endif

/*
 * Rebuild the ORDINAL key for a cell head @cell by walking UP the parent
 * chain.  Each level's branch byte is recovered structurally:
 *   - external head: its last byte = the head's cell-metadata incoming_byte
 *     (unless its parent is a compressed node, whose key_bytes already span
 *     through the head's position).
 *   - internal node: metadata->incoming_byte (skip the root, which has none).
 *   - compressed node: its key_bytes[] span PLUS metadata->incoming_byte (the
 *     slot byte under which it hangs in its parent -- separate from key_bytes).
 * Fill @out from the END (the caller sizes it to @max_len bytes) and return
 * the key length.  The key occupies @out[@max_len - length .. @max_len).
 * Return 0 when @cell is NULL or when the key does not fit in @max_len bytes.
 *
 * This is the structural key source for the ordered-list walk.  It needs NO
 * speculative_key_offset (in-leaf key) and NO parent-bitmap inversion.  It
 * makes the ordered list usable on an EAGER / no-leaf-key trie.  Valid only
 * while the RCU lock that produced @cell is held continuously.
 */
static inline_lookup
size_t ft_rebuild_key_upwalk(const struct cds_ft *ft, struct ft_ord_cell *cell,
		uint8_t *out, size_t max_len)
{
	struct cds_ft_inode_flag *nf;
	struct cds_ft_inode_flag *from = NULL;	/* whose parent_word gave @nf */
	unsigned int level = 0;
	bool is_prefix;
#ifdef FT_DEBUG_PARENT_VIOLATION
	struct ft_upwalk_hist hist[FT_UPWALK_HIST];
	struct ft_upwalk_hist *histp = hist;
#else
	struct ft_upwalk_hist *histp = NULL;
#endif
	size_t pos = max_len;	/* fill DEEPEST-byte-first leftward from the end */

	(void) ft;
	if (!cell)
		return 0;

	/*
	 * Resolve a cds_ft_merge / graft_swap flip proxy on every parent load.
	 * A concurrent bulk op re-parents nodes via a type-7 proxy.  The proxy
	 * is installed BEFORE the op's drain.  Without the resolve, an up-walk
	 * running under the reader's RCU lock would dereference the proxy as a
	 * node.  The resolve gives the view-appropriate (old-or-merged) parent,
	 * consistent across the walk.
	 */
	/*
	 * Launder the head's parent through ft_parent_node() exactly as the climb
	 * below does.  A parent word at a ROOT POSITION names the owning TRIE,
	 * not a node.  It must never be walked as a node.  ft_parent_node()
	 * turns that stamp into NULL, and the root-position arm below already
	 * handles NULL.  Every other parent reader in the tree goes through it:
	 * ft-verify, ft-compact, ft-insert and this function's own climb.  This
	 * load was the only one that did not.
	 *
	 * The laundering is an identity TODAY.  The walk deliberately does not
	 * rely on that.  It is an identity for two reasons:
	 *  - A head always hangs under a node.  Even a lone nil key is held by
	 *    a childless-internal wrapper AT the root.  So @cell->parent is an
	 *    internal or compressed flag, whose low nibble is never 0.
	 *  - No writer stamps a cell.  Every ft_trie_parent() store targets a
	 *    metadata @parent_word.
	 * Measured: ZERO root-position cells in 215 M up-walks across both
	 * spacings and ft_unit.
	 *
	 * The walk is written this way so that it stays correct if cells ever
	 * adopt the owner stamp that the metadata parent words already carry.
	 * It would otherwise silently dereference a struct cds_ft as a node.
	 */
	nf = ft_parent_node_resolved(rcu_dereference(cell->parent));
	/*
	 * A PREFIX head's parent word says so on the word itself, with
	 * FT_PARENT_PREFIX_HEAD.  The flag was set when the head was installed
	 * at the holder's external_nodes.  Every re-home since has carried it.
	 * It is the head-shape answer.  It stays right on a DEAD head, whose
	 * holder no longer names it: the node the word names is unchanged.
	 */
	is_prefix = ft_parent_prefix_head(nf);
	nf = ft_parent_prefix_strip(nf);
	/*
	 * A parent link names an INTERNAL or COMPRESSED node, never an external
	 * one.  ft_get_parent_rcu asserts the same invariant on its own load.
	 * It is asserted here too: this walk is the other consumer of a parent
	 * word, and it had no check at all.
	 *
	 * It is not a formality.  A metadata slot returned to its range freelist
	 * has @free_list_next written over @parent_word.  They are the same
	 * bytes; see the layout note on struct cds_ft_metadata.  A freelist
	 * link is >= 8-byte aligned, so ft_node_external() ACCEPTS it.
	 *
	 * Without this check, the walk carries that link into
	 * ft_flag_to_metadata below.  It dies two frames later inside
	 * cds_ft_item_to_metadata, on an address in the metadata region.
	 * Nothing left on the stack names the stale reference.  Trap it at the
	 * LOAD instead, where @cell and the walk are still in the frame.
	 */
	ft_upwalk_check_parent(ft, cell, NULL, nf, 0, histp);

	/*
	 * Fill the buffer FROM THE END.  Write the deepest (leaf-edge) byte at
	 * out[max_len-1] and grow leftward.  The key ends up in correct order
	 * in out[pos .. max_len), with NO reversal.
	 *
	 * The length is DERIVED from the walk: pos drops by the number of bytes
	 * the walk contributes.  No key_len is needed, so variable-length keys
	 * with no in-leaf length work.
	 *
	 * A compressed span lands as one contiguous forward memcpy: its
	 * key_bytes are already in key order.  On underflow past out[0], fail
	 * (return 0).  The key STARTS at out[max_len - returned]; the caller
	 * keeps that offset.
	 *
	 * Head's last byte:
	 *  - The head hangs off an internal node: it sits in a slot.  The slot
	 *    byte is the head's cell-metadata incoming_byte.
	 *  - The head's parent is a compressed node: the head is that node's
	 *    child and carries no separate edge byte.  The compressed key_bytes
	 *    run through the head's position.
	 */
	if (!nf) {
		/*
		 * Head AT A ROOT POSITION: @cell->parent was NULL, or a trie
		 * stamp that the load above resolved to NULL.  Its byte stands
		 * alone.  Unobserved in practice (see the load's measurement).
		 * The arm is kept, not removed: a stamped cell would land here.
		 */
		struct cds_ft_metadata *hmeta = cds_ft_item_to_metadata(cell);

		if (pos == 0)
			return 0;
		out[--pos] = (uint8_t) hmeta->incoming_byte;
	} else if (!ft_node_compressed(ft_resolve_skip_compressed(ft, nf))) {
		/*
		 * Parent is an internal (slot-array) node.  Write the head's edge
		 * byte ONLY when the head hangs off a SLOT.  A PREFIX key sits at the
		 * parent's external_nodes: the key ENDS at the parent.  Its
		 * last byte IS the parent's own incoming edge, which is written
		 * when the parent is processed below.  It must not be counted
		 * twice here.  Fixed-length tries have no prefix keys, so this
		 * is always a slot head there.
		 *
		 * ★ ONE WORD ANSWERS IT, and it is the word already loaded above.
		 * The head's back edge carries its own shape
		 * (FT_PARENT_PREFIX_HEAD).  The parent it names and the shape
		 * it has come from a SINGLE load, so they cannot disagree.
		 * This replaces two inferences.  Both were two-word reads, and
		 * both were measurably wrong:
		 *
		 *  - "not the parent's external_nodes" is NOT "hangs off a slot".  A
		 *    parked reader legitimately holds the cell of a head whose key a
		 *    concurrent remove has already taken out.  The remove that
		 *    makes the key of a PREFIX head disappear is an IN-PLACE
		 *    clear of the holder's external_nodes.  The holder stays.
		 *    Its external_nodes reads NULL, or a re-inserted head of
		 *    the same key.  The inference then turns the dead prefix
		 *    head into a slot head and appends its own last byte a
		 *    second time.
		 *    MEASURED with a key->node oracle over {Bdd,Bds,Bs} while
		 *    "Bd" is churned.  About 1.5 M keys per 8 s run spelled
		 *    "Bdd" on the "Bd" node.  100% came from a node whose
		 *    removal had started.  The parent's slot for that byte held
		 *    a DIFFERENT node.  This is a legal key on the wrong node,
		 *    which a universe check cannot see.
		 *  - "is the head in the parent's slot for its byte" tears the same
		 *    way.  A glue commit wraps a slot head "m" under a fresh
		 *    internal node as its prefix head
		 *    (inv_merge_rerooted_nosplit_branch).  That leaves a LIVE
		 *    head "in neither place", 39/39.
		 *
		 * ☠ AND A RE-READ OF THIS WORD IS NOT A REPAIR, it is a second
		 * straddle.  A re-read used to back the holder query.  It took
		 * the SHAPE from a later load, while the climb below kept the
		 * PARENT from the earlier one.  Now the shape lives on the
		 * word.  A re-home committed between the two loads then hands
		 * back "prefix head" for a parent that is no longer the one
		 * being walked.  The key comes out a byte short
		 * (inv_merge_rerooted_glue_no_escape, len-0 keys).  Read it
		 * ONCE.
		 */
		if (!is_prefix) {
			struct cds_ft_metadata *hmeta =
				cds_ft_item_to_metadata(cell);

			if (pos == 0)
				return 0;
			out[--pos] = (uint8_t) hmeta->incoming_byte;
		}
	}
	/* else parent compressed: the head byte is covered by its key_bytes. */

	while (nf) {
		struct cds_ft_inode_flag *rnf;
		struct cds_ft_metadata *meta;

		/* Same invariant, re-checked per level: see the load above. */
		ft_upwalk_check_parent(ft, cell, from, nf, level, histp);
		rnf = ft_resolve_skip_compressed(ft, nf);
		meta = ft_flag_to_metadata(ft, nf);
#ifdef FT_DEBUG_PARENT_VIOLATION
		if (level < FT_UPWALK_HIST) {
			struct cds_ft_inode_flag *praw = meta->parent_word;
			struct cds_ft_inode_flag *pnode = ft_parent_node(praw);

			hist[level].nf = nf;
			hist[level].state = meta->state;
			/* rcu_head sits immediately BEFORE the metadata. */
			hist[level].rcu_func = ((void **) meta)[-1];
			hist[level].parent_raw = praw;
			hist[level].slot = NULL;
			hist[level].slot_val = NULL;
			/*
			 * The forward edge, for the back-edge comparison in the
			 * dump.  Resolved only while the parent word still names
			 * a node OR a root position (whose slot is &ft->root).
			 * A freelist link is 8-mod-16.  It clears neither
			 * ft_parent_is_trie's alignment nor ft_node_external's
			 * tag.  The slot address would be computed off junk.
			 */
			if (!pnode || !ft_node_external(pnode)) {
				hist[level].slot = ft_get_parent_slot(meta,
						(struct cds_ft *) ft);
				if (hist[level].slot)
					hist[level].slot_val =
						*hist[level].slot;
			}
		}
#endif

		if (ft_node_compressed(rnf)) {
			const struct cds_ft_compressed_node *cn =
				(const struct cds_ft_compressed_node *)
				ft_node_ptr(rnf);
			unsigned int len = cn->len;

			/*
			 * Compressed span in key order (key_bytes[0..len)).
			 * It sits immediately to the LEFT of what we've written
			 * so far.  A single forward memcpy places it correctly.
			 */
			if (pos < len)
				return 0;
			pos -= len;
			memcpy(&out[pos], cn->key_bytes, len);
		}
		/*
		 * This node's incoming edge byte (the slot byte in its parent).
		 * Read meta->parent_word ONCE (resolving a concurrent bulk op's
		 * flip proxy).  Use it for both the compressed-parent test and
		 * the advance.
		 *
		 * The byte is contributed only when the PARENT is an internal
		 * (slot-array) node.  A compressed parent's last key_byte
		 * already IS this edge, and it is counted when that parent is
		 * processed.  So skip it here.  This mirrors the head's skip
		 * when its parent is compressed.  The root has no parent and
		 * contributes none.
		 */
		{
			struct cds_ft_inode_flag *parent =
				ft_parent_node_resolved(
					rcu_dereference(meta->parent_word));

			if (parent && !ft_node_compressed(
					ft_resolve_skip_compressed(ft, parent))) {
				if (pos == 0)
					return 0;
				out[--pos] = (uint8_t) meta->incoming_byte;
			}
			from = nf;	/* whose parent_word produced the next @nf */
			nf = parent;
			level++;
		}
	}

	/* Key now occupies out[pos .. max_len) in key order; length = max_len - pos. */
	return max_len - pos;
}

/*
 * One-shot structural materialization for a VARIABLE-length EAGER ordered-list
 * iterator (no in-leaf key, no key_len_offset).  The parent up-walk derives
 * BOTH the key bytes (into iter_key) AND the length in a SINGLE walk.
 *
 * Cache the length in iter->key_len, which clears the LAZY sentinel.  A later
 * read_key / resolve_key_len in the same step then reuses it and does not
 * walk again.  Return the length (0 on a NIL key / overflow / missing cell).
 */
static inline_lookup
size_t ft_iter_upwalk_into_buf(struct cds_ft_iter *iter)
{
	size_t max_len = iter->ft->group->max_key_len;
	struct ft_ord_cell *cell = ft_ord_cell_cursor(iter);
	size_t n = 0;

	if (cell)
		n = ft_rebuild_key_upwalk(iter->ft, cell, iter_key(iter), max_len);
	iter->key_len = n;
	iter->key_off = max_len - n;	/* key lives at iter_key[key_off ..) */
	iter->path_len = n + 1;
	return n;
}

static inline_lookup
const uint8_t *ft_iter_read_key(const struct cds_ft_iter *iter)
{
	const struct cds_ft_group *group = iter->ft->group;

	if (ft_iter_key_referenced(iter))
		return (const uint8_t *) iter->node + group->speculative_key_offset;
	/*
	 * EAGER ordered-list walk (no in-leaf key): rematerialize the current
	 * key STRUCTURALLY via the parent up-walk into iter_key.
	 *
	 * This is reached only when a key consumer asks for the key.  A
	 * keyless/count walk never calls this, so the O(depth) walk is paid
	 * strictly on demand.
	 *
	 * The walk recovers ORDINAL bytes from the trie structure.  That is
	 * what iter_key holds for ANY key map (consumers remap via
	 * ft_ordinals_to_key), so there is no identity requirement.
	 *
	 *  - FIXED-length: walk into the buffer.  The length is
	 *    group->key_len.
	 *  - VARIABLE-length: derive the length from the SAME walk.
	 *    ft_iter_upwalk_into_buf caches it.  The LAZY sentinel coordinates
	 *    this with ft_iter_resolve_key_len, so one walk serves both.
	 */
	if (group->ordered_list_set && !iter->ft->speculative_key_offset_active &&
			iter->cache_valid && iter->node) {
		if (group->key_len == CDS_FT_LEN_VARIABLE) {
			/*
			 * The up-walk fills the key at the buffer TAIL and records
			 * iter->key_off.  See above for the LAZY sentinel.
			 */
			if (iter->key_len == FT_ITER_KEY_LEN_LAZY)
				ft_iter_upwalk_into_buf(
					(struct cds_ft_iter *) iter);
			return iter_key(iter) + iter->key_off;
		} else {
			struct ft_ord_cell *cell = ft_ord_cell_cursor(iter);
			size_t max_len = group->max_key_len;
			size_t n;

			if (cell && (n = ft_rebuild_key_upwalk(iter->ft, cell,
					iter_key(iter), max_len)) != 0) {
				((struct cds_ft_iter *) iter)->key_off =
					max_len - n;
				return iter_key(iter) + (max_len - n);
			}
		}
	}
	return iter_key(iter) + iter->key_off;
}

/*
 * Resolve (and cache) the iterator's current-position key length.
 *
 * For a deferred-length cell position, read the length once and cache it
 * into iter->key_len (and path_len).  The length comes from the leaf, or from
 * the parent up-walk (see below).  Otherwise return iter->key_len unchanged.
 * That is a no-op for fixed-length, non-identity, descent and non-cell
 * positions.
 *
 * EVERY reader of the current-position length must call this before reading
 * iter->key_len:
 *  - cds_ft_iter_get_key;
 *  - cds_ft_remove*;
 *  - the skip rebuilds;
 *  - bind;
 *  - the max-key-len scan.
 *
 * The LAZY sentinel is only ever set with cache_valid && node, so the leaf
 * read is safe.
 */
static inline_lookup
size_t ft_iter_resolve_key_len(struct cds_ft_iter *iter)
{
	if (caa_unlikely(iter->key_len == FT_ITER_KEY_LEN_LAZY)) {
		/*
		 * VARIABLE-length EAGER ordered-list (no in-leaf KEY at
		 * speculative_key_offset).  The key source is the iter buffer.
		 * Clearing the LAZY sentinel doubles as "buffer filled" for
		 * ft_iter_read_key.  So the length MUST come from the parent
		 * up-walk: the same walk fills iter_key (+ key_off) and derives
		 * the length.  This holds even when an in-leaf LENGTH
		 * (key_len_offset) is configured.
		 *
		 * Only a leaf-referenced key (in-leaf key present) may take the
		 * leaf-length shortcut: its key reads never touch the buffer.
		 */
		if (!iter->ft->group->key_len_offset_set ||
				!iter->ft->speculative_key_offset_active) {
			ft_iter_upwalk_into_buf(iter);
			return iter->key_len;
		}
		iter->key_len = *(const size_t *) ((const char *) iter->node +
			iter->ft->group->key_len_offset);
		iter->path_len = iter->key_len + 1;
	}
	return iter->key_len;
}

/*
 * Copy a live leaf-referenced key into iter_key so it survives the position
 * being detached (UNCACHED auto-invalidate, bind, any cache_valid clear).
 * This is a no-op self-copy when the key is already a value there.  Bytes
 * are ordinal (ft_iter_key_referenced requires an identity map).
 *
 * Resolve the length first.  A deferred-length position then materializes
 * both the key and the length before its node is dropped.
 */
static inline_lookup
void ft_iter_materialize_key(struct cds_ft_iter *iter)
{
	size_t klen = ft_iter_resolve_key_len(iter);
	const uint8_t *cur = ft_iter_read_key(iter);

	/*
	 * Pin the current key at the FRONT of iter_key (key_off = 0).
	 *
	 * The hot cell-walk read keeps the up-walk key at the buffer TAIL (no
	 * move).  But materialize is the bind / UNCACHED path: the next
	 * operation re-descends from the saved key.  The relational descent
	 * reads its search key from iter_key and writes its result into the
	 * SAME buffer.  That is only safe when the key starts at offset 0
	 * (result == search before divergence).
	 *
	 * Use memmove because @cur may overlap iter_key.  @cur is the up-walk
	 * tail or a leaf reference.
	 */
	if (cur != iter_key(iter)) {
		memmove(iter_key(iter), cur, klen);
		iter->key_off = 0;
	}
}


/*
 * Land an ordinal-cell walk result on @iter: materialize the head's key from
 * its leaf and cache the cell as the walk cursor.  @cell == NULL reports
 * NOT_FOUND (end of list).  Shared by the cell fast path and the O(1)
 * lookup_first / lookup_last endpoints.  Returns iter->status.
 */
static inline_lookup
enum cds_ft_status ft_ord_cell_iter_land(struct cds_ft *ft,
		struct cds_ft_iter *iter, struct ft_ord_cell *cell)
{
	struct cds_ft_node *node;
	size_t rlen;

	if (!cell) {
		iter->node = NULL;
		iter->ord_cell = NULL;
		iter->ord_cell_node = NULL;
		iter->cache_valid = false;
		iter_debug_path_update(iter);
		iter->status = CDS_FT_STATUS_NOT_FOUND;
		return iter->status;
	}
	node = cell->node;
	iter->node = node;
	iter->ord_cell = cell;
	iter->ord_cell_node = node;
	iter->cache_valid = true;
	/*
	 * Materialize as little as possible.  The cell walk's point is that a
	 * keyless / count traversal touches NO leaf.
	 *
	 *  - Identity map, or EAGER group: no key copy.
	 *    With an identity map and an in-leaf key, the key is read in place
	 *    from the leaf (see ft_iter_key_referenced).  In an EAGER group (no
	 *    in-leaf key), the up-walk in ft_iter_read_key recovers the ordinal
	 *    bytes structurally, identity or not.
	 *    A VARIABLE-length group also DEFERS the length.  iter->key_len is
	 *    the LAZY sentinel.  ft_iter_resolve_key_len() resolves it on
	 *    demand, only in the key-consuming ops (get_key / remove / skip /
	 *    bind).  So a keyless variable-length walk reads neither the key
	 *    nor the length from the leaf.
	 *    A FIXED-length group takes its length from group->key_len (no
	 *    leaf touch either).
	 *  - Non-identity map WITH an in-leaf key: the leaf bytes are in
	 *    application order.  They must be remapped to ordinal order now,
	 *    and that needs the length now.  Read the length (from the leaf,
	 *    for variable) and copy the key into iter_key.
	 *    ft_iter_key_referenced is then false, so consumers use the
	 *    buffer.
	 */
	if (caa_likely(ft->group->key_map.identity ||
			!ft->speculative_key_offset_active)) {
		if (ft->group->key_len != CDS_FT_LEN_VARIABLE) {
			iter->key_len = ft->group->key_len;
			iter->path_len = ft->group->key_len + 1;
		} else {
			iter->key_len = FT_ITER_KEY_LEN_LAZY;
			iter->path_len = 0;	/* materialized with the length */
		}
	} else {
		rlen = (ft->group->key_len != CDS_FT_LEN_VARIABLE) ?
			ft->group->key_len :
			*(const size_t *) ((const char *) node +
				ft->group->key_len_offset);
		ft_speculative_keycopy_unconditional(ft, node, iter_key(iter),
			(ssize_t) rlen);
		iter->key_len = rlen;
		iter->path_len = rlen + 1;
	}
	iter_debug_path_update(iter);
	iter->status = CDS_FT_STATUS_OK;
	return iter->status;
}

/*
 * Return true when the ordinal-cell O(1) endpoints / fast path can serve
 * @iter.  All of these must hold:
 *  - the list is enabled;
 *  - the result key and its length are recoverable, from the leaf or from
 *    the parent up-walk;
 *  - the traversal is unscoped.
 */
static inline_lookup
bool ft_ord_cell_fastpath_ok(const struct cds_ft *ft,
		const struct cds_ft_iter *iter)
{
	/*
	 * EAGER structural up-walk.  An ordered-list group with NO in-leaf key
	 * (no speculative_key_offset) gets BOTH the result key AND its length
	 * from the parent up-walk (ft_iter_read_key / ft_iter_resolve_key_len).
	 * This holds for fixed OR variable length.  It needs no in-leaf key and
	 * no key_len_offset.  The walk recovers ORDINAL bytes, so it serves any
	 * key map (a non-identity map is applied on result-key copy-out).
	 */
	bool up_walk = ft->group->ordered_list_set &&
		!ft->speculative_key_offset_active;

	return ft->group->ordered_list_set &&
		(up_walk ||
		 /*
		  * In-leaf key (ft_ord_cell_iter_land reads/remaps it from the
		  * leaf): needs the length too -- fixed, or variable with an
		  * in-leaf length (key_len_offset).
		  */
		 (ft->group->key_len != CDS_FT_LEN_VARIABLE ||
			ft->group->key_len_offset_set)) &&
		iter->prefix_len == 0;
}

enum cds_ft_status cds_ft_iter_create(struct cds_ft *ft, struct cds_ft_iter **result_iter)
{
	size_t max_key_len = ft->group->max_key_len;
	/*
	 * Tail pad of FT_KEY_READABLE_PAD bytes after the key buffer so
	 * the descent can SIMD-load 32 bytes past key[key_len-1] without
	 * faulting.  The iter-based lookup paths pass FT_KEY_READABLE_PAD
	 * as @key_readable_pad (bytes safely loadable past key end).
	 */
	size_t key_size  = (max_key_len + FT_KEY_READABLE_PAD) * sizeof(uint8_t);
	struct cds_ft_iter *iter = calloc(1, sizeof(*iter) + key_size);

	CDS_FT_SCOPED_READER(ft);
	if (!iter) {
		*result_iter = NULL;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	iter->ft = ft;
	iter->cache_mode = CDS_FT_ITER_CACHED;
	*result_iter = iter;
	FT_TP(iter_create, (const void *) ft, (const void *) *result_iter);
	return CDS_FT_STATUS_OK;
}

void cds_ft_iter_destroy(struct cds_ft_iter *iter)
{
	FT_TP(iter_destroy, (const void *) iter->ft, (const void *) iter);
	free(iter);
}

enum cds_ft_status cds_ft_iter_status(const struct cds_ft_iter *iter)
{
	return iter->status;
}

enum cds_ft_status cds_ft_iter_get_key(struct cds_ft_iter *iter,
		uint8_t *result_key, size_t result_key_max_len, size_t *result_key_len)
{
	size_t klen = ft_iter_resolve_key_len(iter);

	*result_key_len = klen;
	if (klen > result_key_max_len)
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	/*
	 * Lazy-ref: when the current key is a live reference into iter->node,
	 * read it from the leaf; else from the iter_key value.
	 * ft_iter_resolve_key_len above materialized a deferred
	 * (variable-length) length from the same leaf.  The contract is the
	 * same as for reusing the cached position: the RCU lock is held
	 * continuously.  Cross-CS callers call cds_ft_iter_bind_key() first.
	 */
	ft_ordinals_to_key(result_key, ft_iter_read_key(iter), klen,
			&iter->ft->group->key_map);
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_node_get_key(const struct cds_ft *ft,
		const struct cds_ft_node *node, uint8_t *result_key,
		size_t result_key_max_len, size_t *result_key_len)
{
	const struct cds_ft_group *group = ft->group;
	const uint8_t *ordinals;
	size_t klen;
	uint8_t scratch[FT_MAX_KEY_LEN];

	if (!node)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	if (ft->speculative_key_offset_active) {
		/*
		 * In-leaf key: the head stores its ordinal key bytes (and, for a
		 * variable-length group, its length) directly.  Read them in place.
		 */
		ordinals = (const uint8_t *) node + group->speculative_key_offset;
		klen = (group->key_len != CDS_FT_LEN_VARIABLE) ? group->key_len :
			*(const size_t *) ((const char *) node +
				group->key_len_offset);
	} else {
		/*
		 * No in-leaf key: rebuild the ordinal key by the structural parent
		 * up-walk from the head's cell.  cds_ft_iter_get_key uses the
		 * same EAGER source.
		 *
		 * This requires the cell to exist, i.e. the ordered list
		 * enabled.
		 * A list-off trie allocates no cells: head->prev is the flagged
		 * parent directly, which is NOT an up-walk source.
		 *
		 * The walk fills @scratch from the tail and returns the length.
		 * The key starts at @scratch[max-len].
		 */
		if (ft->ordered_list) {
			struct ft_ord_cell *cell = ft_ord_cell_ptr(
				ft_dereference_prev_resolved((struct cds_ft_node *) node));
			size_t max_len = group->max_key_len;

			klen = ft_rebuild_key_upwalk(ft, cell, scratch, max_len);
			ordinals = scratch + (max_len - klen);
		} else {
			/* List off: no cell, no in-leaf key -> not materializable. */
			return CDS_FT_STATUS_NOT_FOUND;
		}
	}
	*result_key_len = klen;
	if (klen > result_key_max_len)
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	ft_ordinals_to_key(result_key, ordinals, klen, &group->key_map);
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_iter_get_prefix(struct cds_ft_iter *iter,
		uint8_t *result_key, size_t result_key_max_len, size_t *result_key_len)
{
	*result_key_len = iter->prefix_len;
	if (iter->prefix_len > result_key_max_len)
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	ft_ordinals_to_key(result_key, iter_key(iter), iter->prefix_len,
			&iter->ft->group->key_map);
	return CDS_FT_STATUS_OK;
}

/*
 * Internal: set the iterator's search key from ALREADY-ORDINAL bytes (no key
 * map application; the public cds_ft_iter_set_key remaps and validates).  Used
 * by the bulk-op machinery, which converts the caller's key once at the public
 * entry and threads the ordinal form internally.
 */
static
void ft_iter_set_key_ordinals(struct cds_ft_iter *iter, const uint8_t *ordinals,
		size_t key_len)
{
	/*
	 * Setting a new key invalidates the cached position: the next
	 * operation re-descends from the root by the new key.
	 */
	memcpy(iter_key(iter), ordinals, key_len);
	iter->cache_valid = false;
	iter_debug_path_clear(iter);
	iter->path_len = 0;
	iter->key_len = key_len;
	iter->key_off = 0;	/* search key sits at the front of iter_key */
}

enum cds_ft_status cds_ft_iter_set_key(struct cds_ft_iter *iter, const uint8_t *key, size_t key_len)
{
	const struct cds_ft_key_map *km = &iter->ft->group->key_map;
	uint8_t ordinal_buf[FT_MAX_KEY_LEN];
	const uint8_t *key_ordinals;

	key_len = ft_key_len(iter->ft, key_len);
	FT_TP(iter_set_key_enter, (const void *) iter->ft, (const void *) iter,
		key, key_len == CDS_FT_LEN_ERROR ? 0 : key_len,
		(int) iter->key_len, (int) iter->path_len);
	if (key_len > iter->ft->group->max_key_len)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	if (caa_likely(km->identity)) {
		key_ordinals = key;
	} else {
		ft_key_to_ordinals(ordinal_buf, key, key_len, km);
		key_ordinals = ordinal_buf;
	}
	ft_iter_set_key_ordinals(iter, key_ordinals, key_len);
	FT_TP(iter_set_key_exit, (const void *) iter->ft, (const void *) iter,
		(int) iter->path_len);
	return CDS_FT_STATUS_OK;
}

/*
 * The prefix is a subset of the current key. Set the key before setting
 * the prefix length.
 */
enum cds_ft_status cds_ft_iter_set_prefix_len(struct cds_ft_iter *iter, size_t prefix_len)
{
	if (prefix_len > ft_iter_resolve_key_len(iter)) {
		FT_TP(iter_set_prefix_len, (const void *) iter->ft,
			(const void *) iter, (int) prefix_len);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	iter->prefix_len = prefix_len;
	FT_TP(iter_set_prefix_len, (const void *) iter->ft,
		(const void *) iter, (int) prefix_len);
	return CDS_FT_STATUS_OK;
}

void cds_ft_iter_reset(struct cds_ft_iter *iter)
{
	FT_TP(iter_reset, (const void *) iter->ft, (const void *) iter);
	iter->cache_valid = false;
	iter_debug_path_clear(iter);
	iter->status = CDS_FT_STATUS_OK;
	iter->path_len = 0;
	iter->key_len = 0;
	iter->prefix_len = 0;
	iter->node = NULL;
#ifdef DEBUG_CLEAR_ITER
	{
		const struct cds_ft_group *ft_group = iter->ft->group;

		/* Reset to 0 for debugging. */
		memset(iter_key(iter), 0, ft_group->max_key_len * sizeof(uint8_t));
	}
#endif
}

void cds_ft_iter_bind_key(struct cds_ft_iter *iter)
{
	FT_TP(iter_bind_key, (const void *) iter->ft, (const void *) iter);
	/*
	 * Materialize a live leaf-referenced key into the iterator's own buffer
	 * BEFORE detaching.  A resume in another critical section then
	 * re-descends from the stable buffer, not from the leaf.  The leaf may
	 * be reclaimed after the unlock.  For groups whose key is already a
	 * value, this is a no-op self-copy and a plain invalidate.
	 */
	ft_iter_materialize_key(iter);
	iter->cache_valid = false;
	iter_debug_path_clear(iter);
	iter->path_len = 0;
	iter->node = NULL;
}

/*
 * Keys a library walk visits per read-side section.  A walk over a whole
 * trie must not hold one section throughout: no grace period of the flavor
 * could complete until it ends.
 */
#define FT_WALK_KEYS_PER_SECTION	1024

/*
 * Let a grace period complete in the middle of a walk on @iter.  Call it
 * from inside the walk's read-side section, with @iter on a key.  The next
 * cds_ft_next() re-descends from that key and returns its successor.
 *
 * This only helps when the walk's section is the outermost one.  Inside a
 * caller's section the unlock drops one nesting level and nothing more.
 * Under QSBR the unlock is a no-op: only the caller can report a quiescent
 * state.
 *
 * The caller must keep writers out of the walked keys: a walk that drops
 * its section is not a snapshot.
 */
static
void ft_walk_yield(struct cds_ft_iter *iter)
{
	const struct rcu_flavor_struct *flavor = iter->ft->group->flavor;

	cds_ft_iter_bind_key(iter);
	flavor->read_unlock();
	flavor->read_lock();
}

void cds_ft_iter_copy(struct cds_ft_iter *dst, const struct cds_ft_iter *src)
{
	dst->status = src->status;
	dst->cache_mode = src->cache_mode;
	dst->cache_valid = src->cache_valid;
	dst->path_len = src->path_len;
	dst->key_len = src->key_len;
	dst->key_off = src->key_off;
	dst->prefix_len = src->prefix_len;
	dst->node = src->node;
	dst->ord_cell = src->ord_cell;
	dst->ord_cell_node = src->ord_cell_node;
#ifdef URCU_FRACTAL_TRIE_DEBUG_PATH
	dst->gp_state = src->gp_state;
	dst->gp_state_valid = src->gp_state_valid;
#endif
	/*
	 * Copy the key buffer only when the source key is a VALUE there.  It is
	 * NOT one in two cases:
	 *  - The key is a live leaf reference (identity cell position with an
	 *    in-leaf key).  iter_key(src) is stale.  @dst shares src->node and
	 *    cache_valid, so it reads the key from the same leaf.
	 *  - The length is the deferred LAZY sentinel (EAGER identity cell
	 *    position, no in-leaf key).  Nothing was materialized yet.  @dst
	 *    shares the position and re-derives the key and the length from
	 *    the up-walk on demand.  Copying would be a SIZE_MAX memcpy.
	 * Copy at key_off.  A key materialized by the up-walk lives at the
	 * buffer TAIL.  It is mirrored to the same offset in @dst (key_off is
	 * copied above).
	 */
	if (!ft_iter_key_referenced(src) &&
			src->key_len != FT_ITER_KEY_LEN_LAZY)
		memcpy(iter_key(dst) + src->key_off,
			iter_key(src) + src->key_off, src->key_len);
}

struct cds_ft_node *cds_ft_iter_node(const struct cds_ft_iter *iter)
{
	return iter->node;
}

/*
 * Iterator-free ordered batched gather, shared by cds_ft_cell_next_batch /
 * cds_ft_cell_prev_batch.
 *
 * Walk @ft's ordered cell list from @cursor.  A NULL @cursor means the list
 * minimum for forward, the maximum for reverse.  Emit up to @cap opaque CELL
 * handles into @buf.  Write the resume cell to @next_cursor: the one after
 * the last emitted, NULL at the end.
 *
 * The walk predicts the physical next cell with cmm_ptr_eq.  Post-compaction
 * the cells are contiguous in key order at @stride.  A cell's
 * ord_next/ord_prev then usually resolves to cur +/- stride.  cmm_ptr_eq
 * validates the arithmetic guess and lets the compiler address the NEXT load
 * off it.  This breaks the dependent-load chain, so the gather pipelines.
 * The prediction pays only on a compacted arena and is a no-op otherwise.
 *
 * @forward is a literal at both call sites, so always_inline folds the
 * direction out.
 *
 * Cell-native: the cursor, the emitted handles and the resume cursor are all
 * CELLS.  The walk never touches an external head node, neither to emit nor
 * to resume.  There is no node<->cell round-trip, hence no resume cache is
 * needed.  The caller recovers the node from a cell via cds_ft_cell_node().
 * That is an inlined load of the hot cell line, not a cold node->prev.  The
 * caller materializes keys lazily via cds_ft_cell_get_key().  A keyless (no
 * in-leaf key) ordered scan thus touches ZERO external-head cachelines in
 * the library.
 *
 * ORDERED-LIST ONLY: the step IS the cell ord_next / ord_prev walk.  A
 * list-off trie has no cell list, so it yields CDS_FT_STATUS_NOT_SUPPORTED
 * (*count = 0, *next_cursor = NULL).  Use the iterator there (cds_ft_next /
 * cds_ft_prev, structural descent).
 *
 * RCU CONTRACT: @cursor and every emitted cell are valid only while the read
 * lock that produced @cursor is held continuously (see cds_ft_cell_get_key).
 */
static inline_lookup
enum cds_ft_status ft_cell_batch_dir(struct cds_ft *ft,
		const struct cds_ft_cell *cursor, const struct cds_ft_cell **buf,
		size_t cap, size_t *count,
		const struct cds_ft_cell **next_cursor, const bool forward)
{
	size_t n = 0;
	const uintptr_t stride = (uintptr_t) 1 << FT_ORD_CELL_ALLOC_ORDER;
	struct ft_ord_cell *cur;

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);
	*next_cursor = NULL;
	*count = 0;
	if (caa_unlikely(!ft->ordered_list))
		return CDS_FT_STATUS_NOT_SUPPORTED;
	if (caa_unlikely(cap == 0))
		return CDS_FT_STATUS_OK;
	if (cursor)
		cur = (struct ft_ord_cell *) cursor;	/* resume AT the cell */
	else
		cur = ft_ord_cell_resolve_ord(forward ?
			&ft->ord_sentinel.node.next :
			&ft->ord_sentinel.node.prev);
	/*
	 * Sentinel topology: a link off the end resolves to one of two values.
	 *  - The trie's own sentinel pseudo-cell.
	 *  - NULL, transiently, while a cross-trie run move has NULL-terminated
	 *    a moved run before its finalize.
	 * Both are ft_ord_is_end.  So terminate on that, not on "== sentinel"
	 * or "!= NULL".
	 *
	 * @loaded may thus be NULL here.  The MLP guess/cmm_ptr_eq stays
	 * correct because @guess is never NULL: cmm_ptr_eq(NULL, guess) is
	 * false.  A NULL @loaded falls through and terminates the loop.  It
	 * does not mis-fold.
	 */
	while (n < cap && !ft_ord_is_end(ft, cur)) {
		struct ft_ord_cell *loaded, *guess;

		buf[n++] = (const struct cds_ft_cell *) cur;
		loaded = forward ?
			ft_ord_cell_resolve_ord(&cur->lnode.next) :
			ft_ord_cell_resolve_ord(&cur->lnode.prev);
		guess = (struct ft_ord_cell *) (forward ?
			(uintptr_t) cur + stride :
			(uintptr_t) cur - stride);
		cur = cmm_ptr_eq(loaded, guess) ? guess : loaded;
	}
	/* End of list (cur is the sentinel) => no resume cursor. */
	*next_cursor = ft_ord_is_end(ft, cur) ?
		NULL : (const struct cds_ft_cell *) cur;
	*count = n;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_cell_next_batch(struct cds_ft *ft,
		const struct cds_ft_cell *cursor, const struct cds_ft_cell **buf,
		size_t cap, size_t *count, const struct cds_ft_cell **next_cursor)
{
	return ft_cell_batch_dir(ft, cursor, buf, cap, count, next_cursor, true);
}

enum cds_ft_status cds_ft_cell_prev_batch(struct cds_ft *ft,
		const struct cds_ft_cell *cursor, const struct cds_ft_cell **buf,
		size_t cap, size_t *count, const struct cds_ft_cell **next_cursor)
{
	return ft_cell_batch_dir(ft, cursor, buf, cap, count, next_cursor, false);
}

/* Invariant offset of the head-node pointer within the opaque cell. */
size_t cds_ft_cell_node_offset(void)
{
	return offsetof(struct ft_ord_cell, node);
}

/*
 * Materialize a key from an opaque cell handle (the lazy companion to the cell
 * batch).  The sources are the same as for cds_ft_node_get_key: the in-leaf
 * key when configured, else the parent up-walk.  But the CELL drives them, so
 * the up-walk path never touches the external head node.
 */
enum cds_ft_status cds_ft_cell_get_key(const struct cds_ft *ft,
		const struct cds_ft_cell *cell_opaque, uint8_t *result_key,
		size_t result_key_max_len, size_t *result_key_len)
{
	const struct cds_ft_group *group = ft->group;
	struct ft_ord_cell *cell = (struct ft_ord_cell *) cell_opaque;
	const uint8_t *ordinals;
	size_t klen;
	uint8_t scratch[FT_MAX_KEY_LEN];

	if (!cell)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	if (ft->speculative_key_offset_active) {
		const struct cds_ft_node *node = rcu_dereference(cell->node);

		ordinals = (const uint8_t *) node + group->speculative_key_offset;
		klen = (group->key_len != CDS_FT_LEN_VARIABLE) ? group->key_len :
			*(const size_t *) ((const char *) node +
				group->key_len_offset);
	} else {
		size_t max_len = group->max_key_len;

		klen = ft_rebuild_key_upwalk(ft, cell, scratch, max_len);
		ordinals = scratch + (max_len - klen);
	}
	*result_key_len = klen;
	if (klen > result_key_max_len)
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	ft_ordinals_to_key(result_key, ordinals, klen, &group->key_map);
	return CDS_FT_STATUS_OK;
}

bool cds_ft_group_ordered_list(const struct cds_ft_group *group)
{
	return group->ordered_list_set;
}

bool cds_ft_group_rank_stats(const struct cds_ft_group *group)
{
	return group->rank_stats_set;
}

enum cds_ft_status cds_ft_iter_set_cache_mode(struct cds_ft_iter *iter,
		enum cds_ft_iter_cache_mode mode)
{
	switch (mode) {
	case CDS_FT_ITER_CACHED:
		break;
	case CDS_FT_ITER_UNCACHED:
		/*
		 * Switching to uncached mode: any previously cached
		 * path may become stale if the caller drops the RCU
		 * read-side lock, so invalidate it now.  Materialize a
		 * reference-keycopy key first (same as the per-op uncached
		 * auto-invalidate), so the next re-descent reads the saved
		 * key rather than a stale leaf.
		 */
		if (iter->cache_mode == CDS_FT_ITER_CACHED) {
			ft_iter_materialize_key(iter);
			iter->cache_valid = false;
			iter_debug_path_clear(iter);
			iter->path_len = 0;
		}
		break;
	default:
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	iter->cache_mode = mode;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_iter_cache_mode cds_ft_iter_get_cache_mode(
		const struct cds_ft_iter *iter)
{
	return iter->cache_mode;
}
