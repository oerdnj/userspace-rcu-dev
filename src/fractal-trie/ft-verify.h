// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-verify.h
 *
 * Userspace RCU library - Fractal Trie: cds_ft_verify: visited-set + recursive structural invariant checks.
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-verify.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * Integrity verification.
 *
 * ft_verify_node_recursive: recursively verify structural invariants
 * starting at @node_flag (which may be internal or compressed).
 * Returns 0 on success, -1 on first detected error (with details
 * printed to @out).  Must be called with mutual exclusion wrt
 * updaters.
 *
 * Checks performed:
 * - nr_child matches the actual count of non-NULL child slots.
 * - nr_keys equals the sum of children's nr_keys plus the count
 *   of unique keys from external node chains attached to this node.
 * - Parent pointers of children point back to the correct parent.
 * - Compressed node invariants (len > 0, no external_nodes).
 *
 * @ft: the Fractal Trie (for group/flag access).
 * @out: file stream for diagnostic output (may be NULL to suppress).
 * @node_flag: tagged pointer to the node being verified.
 * @expected_parent: tagged pointer that the node's
 *                   metadata->parent_word should match
 *                   (ft_trie_parent(@ft) for the root).
 * @depth: current depth (used for diagnostics).
 * @out_nr_keys: output -- total nr_keys in the subtree rooted here
 *               (written on success for parent aggregation).
 */
/*
 * Visited-pointer set for cds_ft_verify subtree-uniqueness check.
 *
 * Linear-probing open-addressing hash table keyed by node allocation
 * address (low tag bits stripped via ft_node_ptr).  Only used while a
 * single verify walk is in progress; the entire table is freed at the
 * end of cds_ft_verify.  It catches two bug classes:
 *
 * - A subtree shared by accident between two parents (a
 *   rebase/recompact bug class).
 * - A parent-pointer cycle.  It is detected before the upward
 *   adjacency walk in ft_verify_node_compressed can loop forever.
 */
struct ft_visited_set {
	void **slots;		/* NULL = empty bucket. */
	size_t cap;		/* Power of two. */
	size_t mask;		/* cap - 1. */
	size_t count;
};

static
size_t ft_visited_hash(void *p)
{
	/*
	 * Drop the low alignment bits.  Arena items are at least 16-byte
	 * aligned, so the low 4 bits are zero.  Then mix with the 64-bit
	 * golden-ratio multiplier.
	 */
	uintptr_t v = (uintptr_t) p >> 4;
	return (size_t) (v * 11400714819323198485ULL);
}

static
int ft_visited_init(struct ft_visited_set *vs)
{
	vs->cap = 64;
	vs->mask = vs->cap - 1;
	vs->count = 0;
	vs->slots = calloc(vs->cap, sizeof(void *));
	return vs->slots ? 0 : -1;
}

static
void ft_visited_destroy(struct ft_visited_set *vs)
{
	free(vs->slots);
	vs->slots = NULL;
}

static
int ft_visited_grow(struct ft_visited_set *vs)
{
	size_t new_cap = vs->cap * 2;
	size_t new_mask = new_cap - 1;
	void **new_slots = calloc(new_cap, sizeof(void *));
	size_t i;

	if (!new_slots)
		return -1;
	for (i = 0; i < vs->cap; i++) {
		void *key = vs->slots[i];
		size_t j;

		if (!key)
			continue;
		j = ft_visited_hash(key) & new_mask;
		while (new_slots[j] != NULL)
			j = (j + 1) & new_mask;
		new_slots[j] = key;
	}
	free(vs->slots);
	vs->slots = new_slots;
	vs->cap = new_cap;
	vs->mask = new_mask;
	return 0;
}

/*
 * Returns 1 if @key was newly inserted, 0 if @key was already present
 * (duplicate visit), -1 on allocation failure.  NULL keys are not
 * tracked (they are filtered out by callers anyway).
 */
static
int ft_visited_add(struct ft_visited_set *vs, void *key)
{
	size_t i;

	if (key == NULL)
		return 1;
	/* Keep load factor below 0.5 for fast linear probing. */
	if ((vs->count + 1) * 2 > vs->cap) {
		if (ft_visited_grow(vs))
			return -1;
	}
	i = ft_visited_hash(key) & vs->mask;
	while (vs->slots[i] != NULL) {
		if (vs->slots[i] == key)
			return 0;
		i = (i + 1) & vs->mask;
	}
	vs->slots[i] = key;
	vs->count++;
	return 1;
}

/*
 * Flip-proxy-at-rest check.
 *
 * cds_ft_verify runs under writer exclusion:
 * FEATURE_FT_VERIFY_AT_MUTATION calls it after the writer's mutation
 * completes.  A urcu flip parks a type-7 proxy in a slot only
 * transiently: between its park and its settle, inside a single mutator.
 * So once the writer is between operations, NO reachable slot may hold a
 * flip proxy.  A proxy found here is a fused flip that left a slot
 * UNSETTLED.  This is the "frozen-stale" slot that dangles after its
 * batch is reclaimed.
 *
 * The check reports the exact slot.  It is deterministic and
 * single-threaded, and it does not depend on a trace window.  It is the
 * recommended way to localize a remove / splice fusion bug.
 */
static
int ft_verify_no_proxy_at_rest(FILE *out, const char *what,
		struct cds_ft_inode_flag *val,
		struct cds_ft_inode_flag *node_flag, unsigned int depth)
{
	if (caa_unlikely(ft_node_flip_proxy(val))) {
		if (out)
			fprintf(out, "ft_verify: depth %u: %s at node %p holds a flip proxy AT REST (%p) -- a fused flip left this slot unsettled\n",
				depth, what, (void *) node_flag, (void *) val);
		return -1;
	}
	return 0;
}

/* Forward declaration so the per-kind helpers below can recurse. */
static
int ft_verify_node_recursive(const struct cds_ft *ft, FILE *out,
		struct ft_visited_set *visited,
		uint8_t *path,
		struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag *expected_parent,
		unsigned int depth,
		unsigned long *out_nr_keys);

/*
 * Verify the doubly-linked external-node duplicate chain anchored at
 * @head, owned by @owner_flag (the flagged pointer to the
 * internal/compressed node, or its slot's parent).
 *
 *   - Head's prev must equal @owner_flag (the parent flagged-pointer
 *     convention used by ft_metadata_set_external_nodes and by the
 *     slot-attached external publish in cds_ft_insert).
 *   - Each non-head node's prev must point to its predecessor.
 *   - No node may appear twice (cycle / aliasing across chains).  We
 *     reuse @visited so a node accidentally referenced from a second
 *     chain elsewhere in the trie is also caught.
 *   - @path is currently always NULL.  Historical end-to-end
 *     path/key consistency relied on a group-known stored-key
 *     offset, which is no longer tracked at group level -- callers
 *     now provide @key_offset per-call via
 *     cds_ft_speculative_lookup_key.
 *
 * Returns 0 on success, -1 on first violation.  No-op when @head is
 * NULL.
 */
/*
 * Speculative leaf-key correctness.
 *
 * This applies when the trie reads leaf keys for result capture
 * (ft->speculative_key_offset_active).  Every external leaf's STORED key
 * must then equal its STRUCTURAL position: the ordinal path
 * @path[0..@depth) to the leaf.
 *
 * The stored key is an app-owned field.  The library only ever reads it.
 * It cannot rewrite it across a re-keying move (graft / graft_swap /
 * merge_at with src!=dst).  So a mis-stamped or un-restamped leaf would
 * make a speculative lookup return the wrong key.
 *
 * The verifier catches it here, and thus after every mutation under
 * FEATURE_FT_VERIFY_AT_MUTATION.  The silent corruption becomes a loud,
 * located failure.  This is the verify-time check for the staging-graft
 * workflow, where the app stamps each leaf with its destination key.
 *
 * EAGER tries (cds_ft_attr_set_speculative_keys false) never read the
 * leaf key, so they are exempt.
 */
static
int ft_verify_speculative_key(const struct cds_ft *ft, FILE *out,
		const struct cds_ft_node *node, const uint8_t *path,
		unsigned int depth)
{
	const struct cds_ft_group *group = ft->group;
	const uint8_t *stored;
	size_t klen;
	uint8_t ord[FT_MAX_KEY_LEN];

	if (!ft->speculative_key_offset_active || !path)
		return 0;
	stored = (const uint8_t *) node + group->speculative_key_offset;
	if (group->key_len != CDS_FT_LEN_VARIABLE)
		klen = group->key_len;
	else if (group->key_len_offset_set)
		klen = *(const size_t *) ((const char *) node +
				group->key_len_offset);
	else
		klen = depth;	/* no length field: validate bytes only */
	if (klen != depth) {
		if (out)
			fprintf(out, "ft_verify: depth %u: leaf %p speculative key length %zu != structural depth %u (stale leaf key after a re-keying move? re-stamp it, or create the trie with cds_ft_attr_set_speculative_keys(false))\n",
				depth, (const void *) node, klen, depth);
		return -1;
	}
	ft_key_to_ordinals(ord, stored, depth, &group->key_map);
	if (depth && memcmp(ord, path, depth) != 0) {
		if (out)
			fprintf(out, "ft_verify: depth %u: leaf %p speculative key does not match its position (stale leaf key after a re-keying move? re-stamp it, or create the trie with cds_ft_attr_set_speculative_keys(false))\n",
				depth, (const void *) node);
		return -1;
	}
	return 0;
}

/*
 * THE PREFIX-HEAD BIT, CHECKED AT REST (FT_PARENT_PREFIX_HEAD).
 *
 * A head's back-edge word answers the shape question the up-walks ask.
 * Am I @owner_flag's external_nodes (my key ENDS there)?  Or am I a child
 * in one of its body slots?
 *
 * The answer must travel with the head, because a remove destroys the
 * holder's copy of it in place.  For that reason it cannot be verified
 * from the reader side.  It can be enumerated from here:
 *
 *	bit(head's parent word) == (owner->external_nodes == head)
 *
 * This holds on every LIVE head, in both list modes.  An install site may
 * forget to set the bit, or a re-home may carry a stale one.  Either is a
 * structural error at the next cds_ft_verify.  It is not a rare wrong-node
 * lookup under a parked reader.
 *
 * A COMPRESSED owner is skipped:
 *
 * - It has no external_nodes.
 * - Bit 4 of its flag is part of the address.
 * - ft_head_parent_word drops the bit into it by construction.
 */
static
int ft_verify_head_prefix_bit(FILE *out, struct cds_ft_inode_flag *owner_flag,
		const struct cds_ft_node *head,
		struct cds_ft_inode_flag *parent_word, unsigned int depth)
{
	const struct cds_ft_metadata *ometa;
	bool bit, is_prefix;

	if (!owner_flag || ft_node_external(owner_flag) ||
	    ft_node_compressed(owner_flag))
		return 0;
	ometa = cds_ft_item_to_metadata(ft_node_ptr(owner_flag));
	bit = ft_parent_prefix_head(parent_word);
	is_prefix = (const struct cds_ft_node *) ometa->external_nodes == head;
	if (bit != is_prefix) {
		if (out)
			fprintf(out, "ft_verify: depth %u: head %p parent word %p says prefix-head=%d, owner %p external_nodes %p says %d\n",
				depth, (const void *) head, (void *) parent_word,
				(int) bit, (void *) owner_flag,
				(const void *) ometa->external_nodes,
				(int) is_prefix);
		return -1;
	}
	return 0;
}

static
int ft_verify_external_chain(const struct cds_ft *ft, FILE *out,
		struct ft_visited_set *visited,
		const uint8_t *path,
		struct cds_ft_inode_flag *owner_flag,
		struct cds_ft_node *head,
		unsigned int depth)
{
	struct cds_ft_node *node = head;
	struct cds_ft_node *prev = NULL;
	const struct cds_ft_group *group = ft->group;
	bool check_path = (path != NULL);

	/*
	 * Every external leaf reached at @depth represents a key of
	 * length @depth.  This holds for a NIL terminator at metadata
	 * depth and for a full key at slot depth: both produce the same
	 * external chain.  That length must respect the group's
	 * max_key_len bound.  When the group is configured with
	 * CDS_FT_MAX_LEN_UNLIMITED (max_key_len == SIZE_MAX), this is a
	 * no-op since @depth is at most FT_MAX_KEY_LEN.
	 */
	if (head && (size_t) depth > group->max_key_len) {
		if (out)
			fprintf(out, "ft_verify: depth %u: external chain head %p exceeds group max_key_len %zu\n",
				depth, head, group->max_key_len);
		return -1;
	}
	while (node) {
		void *expected_prev = (prev == NULL) ?
			(void *) owner_flag : (void *) prev;
		int added = ft_visited_add(visited, node);

		if (added < 0) {
			if (out)
				fprintf(out, "ft_verify: depth %u: visited-set allocation failed in external chain at %p\n",
					depth, node);
			return -1;
		}
		if (added == 0) {
			if (out)
				fprintf(out, "ft_verify: depth %u: external chain node %p reached twice (cycle or alias)\n",
					depth, node);
			return -1;
		}
		if (prev == NULL && ft->ordered_list) {
			/*
			 * Ordered-list head: prev is the head's cell (cell-tagged),
			 * whose ->parent is the owner and ->node is this head.
			 */
			struct ft_ord_cell *cell = ft_ord_cell_ptr(node->prev);

			/*
			 * ☞ THE BACK CHANNEL IS CHECKED FOR A PARKED PROXY
			 * BEFORE IT IS COMPARED.  That order is the whole
			 * point of these two lines.
			 *
			 * ft_verify_no_proxy_at_rest() was applied only to the
			 * FORWARD edges:
			 *
			 * - root,
			 * - child slot,
			 * - cn->child,
			 * - compressed skip_slot,
			 * - external_nodes.
			 *
			 * So the words that name a node's PARENT were the one
			 * reachable class it never looked at.  Those are
			 * exactly the words the ordered-cell SW conversion
			 * moved onto the engine.  A raw compare of an
			 * unresolved type-7 proxy against a resolved owner
			 * cannot match.  So the failure surfaced as the
			 * generic mismatch below:
			 *
			 *   head ... cell ... {parent 0x..3bf, node N}
			 *         != expected {owner 0x..c0b, node N}
			 *
			 * That line names neither the class nor the cure.
			 * Only its low three bits (7) distinguished "an
			 * unsettled slot" from "a mis-wired trie".
			 *
			 * Every live reader of this word resolves first
			 * (ft-iter.h, ft-lookup-helpers.h, ft-helpers.h: the
			 * RESOLVE-FIRST contract in ft-helpers.h).  The
			 * verifier deliberately does NOT.  cds_ft_verify runs
			 * with the writer between operations.  At rest, a
			 * proxy here IS the defect.  It is not a value to be
			 * resolved through.  So report it as itself.
			 */
			if (ft_verify_no_proxy_at_rest(out,
					"external head prev (cell link)",
					(struct cds_ft_inode_flag *) node->prev,
					owner_flag, depth))
				return -1;
			if (!ft_node_external((struct cds_ft_inode_flag *) node->prev) &&
			    ft_verify_no_proxy_at_rest(out,
					"external head cell->parent",
					cell->parent, owner_flag, depth))
				return -1;
			if (ft_node_external((struct cds_ft_inode_flag *) node->prev) ||
			    (void *) ft_parent_prefix_strip(cell->parent) !=
					(void *) owner_flag ||
			    cell->node != node) {
				if (out)
					fprintf(out, "ft_verify: depth %u: head %p cell %p {parent %p, node %p} != expected {owner %p, node %p}\n",
						depth, node, (void *) cell,
						(void *) (ft_node_external((struct cds_ft_inode_flag *) node->prev) ? NULL : cell->parent),
						(void *) (ft_node_external((struct cds_ft_inode_flag *) node->prev) ? NULL : cell->node),
						(void *) owner_flag, (void *) node);
				return -1;
			}
			if (ft_verify_head_prefix_bit(out, owner_flag, node,
					cell->parent, depth) < 0)
				return -1;
		} else if (prev == NULL) {
			/* List off: a head's prev is the owner (flagged parent) directly. */
			/* Same class as the cell branch above: name the proxy. */
			if (ft_verify_no_proxy_at_rest(out, "external head prev",
					(struct cds_ft_inode_flag *) node->prev,
					owner_flag, depth))
				return -1;
			if ((void *) ft_parent_prefix_strip(
					(struct cds_ft_inode_flag *) node->prev) !=
					(void *) owner_flag) {
				if (out)
					fprintf(out, "ft_verify: depth %u: head %p prev %p != owner %p\n",
						depth, node, node->prev, (void *) owner_flag);
				return -1;
			}
			if (ft_verify_head_prefix_bit(out, owner_flag, node,
					(struct cds_ft_inode_flag *) node->prev,
					depth) < 0)
				return -1;
		} else {
			/*
			 * Interior chain link.  Same reason as the two head
			 * branches.  An unresolved proxy here would be
			 * reported as "prev != predecessor".  That reads as a
			 * broken chain, not as an unsettled slot.
			 */
			if (ft_verify_no_proxy_at_rest(out,
					"external chain node prev",
					(struct cds_ft_inode_flag *) node->prev,
					(struct cds_ft_inode_flag *) node, depth))
				return -1;
			if (node->prev != expected_prev) {
				if (out)
					fprintf(out, "ft_verify: depth %u: external chain node %p prev %p != predecessor %p\n",
						depth, node, node->prev, expected_prev);
				return -1;
			}
		}
		(void) check_path;
		(void) group;
		/* Every leaf in the chain shares this position; validate each. */
		if (ft_verify_speculative_key(ft, out, node, path, depth))
			return -1;
		prev = node;
		node = ft_node_next(node);
	}
	return 0;
}

/*
 * If @slot_val is skip-encoded, verify that the encoded slen equals
 * cn->len of the underlying compressed node.  Return 0 when the
 * slot is not skip-encoded or the slen matches.  Return -1 on mismatch
 * (with diagnostic to @out).  No-op on architectures without
 * FEATURE_FT_SKIP_COMPRESSED (ft_node_skip_compressed is constant
 * false and the body is dead-coded).
 *
 * This catches three bug classes:
 *
 * - A double-wrap of an already-skip-encoded child.
 * - A stale skip pointer left behind by a recompact that did not
 *   refresh the encoded slen.
 * - The parent-depth-span class of bug: a hop up past a skip-encoded
 *   child must move the byte-depth by the target's whole run.
 */
static
int ft_verify_skip_encoding(const struct cds_ft *ft, FILE *out, struct cds_ft_inode_flag *slot_val,
		unsigned int depth)
{
	struct cds_ft_compressed_node *cn;
	unsigned int slen, cn_len;

	if (!ft_node_skip_compressed(slot_val))
		return 0;
	slen = ft_skip_len(slot_val);
	cn = ft_skip_to_compressed(ft, slot_val);
	cn_len = cn->len;
	if (slen != cn_len) {
		if (out)
			fprintf(out, "ft_verify: depth %u: skip-encoded slot %p slen %u != cn->len %u (cn %p)\n",
				depth, slot_val, slen, cn_len, cn);
		return -1;
	}
	return 0;
}

/*
 * Verify a compressed node's invariants:
 *
 * - cn->len >= 1,
 * - no external_nodes,
 * - nr_child <= 1,
 * - the parent pointer matches.
 *
 * Then recurse into its child.  Finally check the stored nr_keys
 * against the child's subtree count plus any local end-of-path key.
 */
static
int ft_verify_node_compressed(const struct cds_ft *ft, FILE *out,
		struct ft_visited_set *visited,
		uint8_t *path,
		struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag *expected_parent,
		unsigned int depth,
		unsigned long *out_nr_keys)
{
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(node_flag);
	struct cds_ft_metadata *cn_meta = cds_ft_item_to_metadata(
		(struct cds_ft_inode *) cn);
	struct cds_ft_node *external_nodes = cn_meta->external_nodes;
	unsigned long child_nr_keys = 0;
	unsigned long local_keys = 0;
	unsigned long stored_nr_keys;

	/* Compressed path must have length >= 1. */
	if (cn->len < 1) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p has len %u < 1\n",
				depth, node_flag, (unsigned int) cn->len);
		return -1;
	}
	/*
	 * key_bytes[] must fit in the allocated slot.  The arena
	 * allocation order encodes the slot size; subtract the fixed
	 * header (offsetof(..., key_bytes)) to get the capacity, then
	 * assert cn->len fits.  This catches a stale len byte after a
	 * size-class mismatch.  An example is a chain-compress that
	 * grew len without reallocating into a larger size class.
	 * Lookup would otherwise silently overrun key_bytes[].
	 */
	{
		size_t alloc_size = 1UL << cds_ft_item_order(cn);
		size_t header_size = offsetof(struct cds_ft_compressed_node,
				key_bytes);
		size_t key_bytes_capacity = alloc_size - header_size;

		if ((size_t) cn->len > key_bytes_capacity) {
			if (out)
				fprintf(out, "ft_verify: depth %u: compressed node %p len %u exceeds key_bytes capacity %zu (alloc size %zu)\n",
					depth, node_flag,
					(unsigned int) cn->len,
					key_bytes_capacity, alloc_size);
			return -1;
		}
	}
	/* Parent pointer check. */
	if (cn_meta->parent_word != expected_parent) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p parent mismatch: "
				"expected %p, got %p\n",
				depth, node_flag, expected_parent, cn_meta->parent_word);
		return -1;
	}
	/* nr_child must be 0 or 1. */
	if (ft_meta_nr_child(cn_meta) > 1) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p nr_child %u > 1\n",
				depth, node_flag, ft_meta_nr_child(cn_meta));
		return -1;
	}
	/*
	 * FT_STATE_LOCK is the REVERSIBLE per-node writer lock.  At rest
	 * (all-updater quiescence) every copier has done one of two
	 * things:
	 *
	 * - It committed: the {LOCK|s -> TOMBSTONE|s} transition consumed
	 *   the fence.
	 * - It unwound: the txn wrapper's registry cleared the fence.
	 *
	 * A set bit here is a leaked fence: a bail path skipped
	 * ft_meta_lock_release.  Every future peer publish into this node
	 * would then abort forever.
	 */
	if (cn_meta->state & FT_STATE_LOCK) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p node lock set at rest (leaked lock)\n",
				depth, node_flag);
		return -1;
	}
	/*
	 * A REACHABLE node is never retired.  The commit that unlinks a
	 * node sets FT_STATE_TOMBSTONE (freeze-on-free, doc §4.B).  So a
	 * tombstone found by a walk from the root means that some commit
	 * retired the node while a slot still names it.  The reclaim will
	 * free the node under that slot.  Every op that reaches it is
	 * refused (or, before the refusal, misled).
	 */
	if (cn_meta->state & FT_STATE_TOMBSTONE) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p is RETIRED (tombstone) yet reachable\n",
				depth, node_flag);
		return -1;
	}
	/*
	 * cn->child / nr_child bookkeeping must agree:
	 *   nr_child == 1 implies cn->child is non-NULL (the one child);
	 *   nr_child == 0 implies cn->child is NULL.
	 * A drift between the two is a publish/clear bug.  The
	 * subtree-key recursion would not catch it on its own.  The
	 * key-aggregation path simply skips a NULL cn->child.  It would
	 * accept a stored nr_child of 1 with cn->child = NULL, as long
	 * as nr_keys also dropped to 0 in lockstep.
	 */
	if ((ft_meta_nr_child(cn_meta) == 1) != (ft_node_ptr(cn->child) != NULL)) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p nr_child %u does not match cn->child %p presence\n",
				depth, node_flag,
				ft_meta_nr_child(cn_meta), cn->child);
		return -1;
	}
	/* Compressed nodes must not carry external_nodes. */
	if (external_nodes) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p has external_nodes %p (forbidden)\n",
				depth, node_flag, external_nodes);
		return -1;
	}
	/*
	 * Canonicalization: no two adjacent compressed nodes.  Check both
	 * directions of the adjacency at every visited compressed:
	 *
	 *  - ancestor-chain (works in both modes, including skip-compress):
	 *    walk metadata->parent_word upward.  Every consecutive
	 *    compressed ancestor is part of an adjacency run.  The chain
	 *    walk is necessary in skip-compress mode, because a single
	 *    skip pointer can bypass two (or more) compresseds at once.
	 *    The slot's metadata-parent chase from the deepest underlying
	 *    child only surfaces the innermost compressed-being-skipped.
	 *    So the walker visits that one but not its compressed
	 *    ancestor(s).  Walking the parent chain at the visited
	 *    compressed re-exposes every adjacency in the bypassed run.
	 *
	 *  - child-side (cheap and direct, primary in non-skip mode):
	 *    cn->child must not be a (raw or skip-encoded) compressed.  In
	 *    canonical post-fix tries cn->child is never skip-encoded; the
	 *    skip-compressed disjunct is defensive.
	 *
	 * Chain-compress is responsible for fusing adjacencies into a
	 * single compressed.  A violation here means the canonicalization
	 * walk missed a case.  Skip mode would then silently double-wrap
	 * the grandparent's skip pointer.
	 */
	{
		struct cds_ft_inode_flag *child_in_chain = node_flag;
		struct cds_ft_inode_flag *anc = ft_parent_node(cn_meta->parent_word);
		bool adj_violation = false;

		while (anc && ft_node_compressed(anc)) {
			struct cds_ft_metadata *anc_meta =
				cds_ft_item_to_metadata(ft_node_ptr(anc));

			if (out)
				fprintf(out, "ft_verify: depth %u: compressed %p adjacent to compressed parent %p (no two adjacent compresseds)\n",
					depth, child_in_chain, anc);
			adj_violation = true;
			child_in_chain = anc;
			anc = ft_parent_node(anc_meta->parent_word);
		}
		if (adj_violation)
			return -1;
	}
	if (ft_node_skip_compressed(cn->child) ||
			(cn->child && ft_node_compressed(cn->child))) {
		if (out)
			fprintf(out, "ft_verify: depth %u: compressed node %p has %s child %p (no two adjacent compresseds)\n",
				depth, node_flag,
				ft_node_skip_compressed(cn->child) ?
					"skip-encoded compressed" : "compressed",
				cn->child);
		return -1;
	}
	/*
	 * Parent-slot offset round-trip.  Each compressed node records a
	 * pointer-stride offset from its parent to the slot that holds the
	 * pointer to itself.  That pointer is skip-encoded under
	 * FEATURE_FT_SKIP_COMPRESSED, and a raw compressed flag otherwise.
	 * The offset has two users:
	 *
	 * - ft_publish_to_parent uses it to refresh the slot when
	 *   cn->child is replaced.
	 * - The parent-pointer backtrack (e.g. position-based remove)
	 *   uses it to recover the slot.
	 *
	 * Verify the offset still resolves to a slot whose contents encode
	 * this very compressed.  A recompact / graft / relocate can leave a
	 * stale offset.  This check reports it at the mutation that
	 * introduced it, not as a corrupted slot at lookup / remove time.
	 * Checked on every build (the offset is no longer skip-specific).
	 *
	 * NULL slot means the offset was never set (offset == 0 with a
	 * non-NULL parent).  No round-trip to verify in that case.
	 * ft_get_parent_slot wants a non-const ft for the root case
	 * (parent == NULL); cast away const since we only read *slot.
	 */
	{
		struct cds_ft_inode_flag **skip_slot =
			ft_get_parent_slot(cn_meta, (struct cds_ft *) ft);

		if (skip_slot) {
			struct cds_ft_inode_flag *slot_val = *skip_slot;
			struct cds_ft_compressed_node *target_cn = NULL;

			if (ft_verify_no_proxy_at_rest(out, "compressed skip_slot",
					slot_val, node_flag, depth))
				return -1;
			if (ft_node_skip_compressed(slot_val))
				target_cn = ft_skip_to_compressed(ft, slot_val);
			else if (slot_val &&
				 ft_node_compressed(slot_val))
				target_cn = ft_compressed_node_ptr(slot_val);
			if (target_cn != cn) {
				if (out)
					fprintf(out, "ft_verify: depth %u: compressed %p parent_slot_offset round-trip mismatch: slot %p holds %p (resolves to cn %p, expected %p)\n",
						depth, node_flag, skip_slot,
						slot_val, target_cn, cn);
				return -1;
			}
			/*
			 * A ROUND-TRIP IS NOT AN EQUALITY.  The check above
			 * resolves the slot through ft_skip_to_compressed,
			 * which follows the CHILD's parent_word.  A RETIRED
			 * former child still names @cn there.  So a dual left
			 * encoding a superseded child passes the check.
			 * Every later publish into @cn->child then plans its
			 * expected-old against the value the dual does NOT
			 * hold, and aborts forever.
			 *
			 * What the publisher actually maintains is the
			 * equality: the dual encodes the CURRENT cn->child
			 * (ft_publish_to_parent writes the two as one flip).
			 * Check that.  A parked cn->child is not comparable at
			 * rest and is reported by the no-proxy check below.
			 */
			if (ft_node_skip_compressed(slot_val) &&
					!ft_node_flip_proxy(cn->child) &&
					ft_skip_child_ptr(slot_val) != cn->child) {
				if (out)
					fprintf(out, "ft_verify: depth %u: compressed %p stale SKIP_X dual: slot %p encodes child %p but cn->child is %p\n",
						depth, node_flag, skip_slot,
						ft_skip_child_ptr(slot_val),
						cn->child);
				return -1;
			}
		}
	}
	/*
	 * Path tracking: write the compressed key bytes into the path
	 * buffer at positions [depth..depth+cn->len-1].  Subsequent
	 * recursion / external-chain compares read these bytes back
	 * against leaf-stored keys.
	 */
	if (path)
		memcpy(path + depth, cn->key_bytes, cn->len);
	/* Recurse into the child. */
	if (ft_verify_no_proxy_at_rest(out, "compressed cn->child", cn->child,
			node_flag, depth))
		return -1;
	if (cn->child) {
		if (ft_node_external(cn->child)) {
			/* External leaf chain at end of compressed path. */
			if (ft_verify_external_chain(ft, out, visited, path,
					node_flag,
					(struct cds_ft_node *) ft_node_ptr(cn->child),
					depth + cn->len))
				return -1;
			local_keys = 1;	/* One unique key. */
		} else {
			/* Internal/compressed child. */
			if (ft_verify_node_recursive(ft, out, visited, path,
					cn->child,
					node_flag, depth + cn->len,
					&child_nr_keys))
				return -1;
		}
	}
	/*
	 * Verify nr_keys -- only when the trie maintains order statistics.
	 * With rank stats off the field is never written, so it carries no
	 * meaning.  Report the structurally-computed count upward instead.
	 * A caller that does check then sees a consistent value.  No caller
	 * ever will: the gate is group-wide.
	 */
	if (ft->rank_stats) {
		stored_nr_keys = ft_nr_keys_get(cn_meta);
		if (stored_nr_keys != child_nr_keys + local_keys) {
			if (out)
				fprintf(out, "ft_verify: depth %u: compressed node %p nr_keys mismatch: "
					"stored %lu, computed %lu (children %lu + local %lu)\n",
					depth, node_flag, stored_nr_keys,
					child_nr_keys + local_keys,
					child_nr_keys, local_keys);
			return -1;
		}
		*out_nr_keys = stored_nr_keys;
	} else {
		*out_nr_keys = child_nr_keys + local_keys;
	}
	return 0;
}

static
int ft_verify_node_recursive(const struct cds_ft *ft, FILE *out,
		struct ft_visited_set *visited,
		uint8_t *path,
		struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag *expected_parent,
		unsigned int depth,
		unsigned long *out_nr_keys)
{
	/*
	 * Subtree-uniqueness / cycle check.  Every traversable node
	 * (compressed, internal) must be reached exactly once from
	 * the root.  A duplicate visit means one of two things:
	 *
	 * - two parents share the same child subtree (rebase/recompact
	 *   bug);
	 * - a parent-pointer cycle has been introduced.
	 *
	 * Bail out before recursing further, so that the upward parent
	 * walks in the adjacency check cannot loop forever.
	 */
	{
		void *node_addr = ft_node_ptr(node_flag);
		int added = ft_visited_add(visited, node_addr);
		struct cds_ft_metadata *m = cds_ft_item_to_metadata(node_addr);

		if (added < 0) {
			if (out)
				fprintf(out, "ft_verify: depth %u: visited-set allocation failed at node %p\n",
					depth, node_flag);
			return -1;
		}
		if (added == 0) {
			if (out)
				fprintf(out, "ft_verify: depth %u: node %p reached twice (shared subtree or parent-pointer cycle)\n",
					depth, node_flag);
			return -1;
		}
		/*
		 * alloc_index round-trip: cds_ft_metadata_to_item walks back
		 * from the metadata to the arena slot using m->alloc_index.
		 * It must land on this very node; a corrupted alloc_index
		 * would otherwise survive verify and only fault later inside
		 * the allocator on free or recompact.
		 */
		if (cds_ft_metadata_to_item(m) != node_addr) {
			if (out)
				fprintf(out, "ft_verify: depth %u: node %p alloc_index round-trip yields %p (mismatch)\n",
					depth, node_flag,
					cds_ft_metadata_to_item(m));
			return -1;
		}
	}
	if (ft_node_compressed(node_flag))
		return ft_verify_node_compressed(ft, out, visited, path,
			node_flag, expected_parent, depth, out_nr_keys);

	/* --- Internal node (popcount, pigeon) --- */
	{
		struct cds_ft_inode *node = ft_node_ptr(node_flag);
		struct cds_ft_metadata *metadata = cds_ft_item_to_metadata(node);
		struct cds_ft_node *external_nodes = metadata->external_nodes;
		unsigned long total_child_keys = 0;
		unsigned long local_keys = 0;
		unsigned int counted_children = 0;
		unsigned int key;

		/* Parent pointer check (the root's parent is its trie). */
		if (metadata->parent_word != expected_parent) {
			if (out)
				fprintf(out, "ft_verify: depth %u: internal node %p parent mismatch: "
					"expected %p, got %p\n",
					depth, node_flag, expected_parent,
					metadata->parent_word);
			return -1;
		}
		/*
		 * Slot-offset round-trip (non-root).  The recorded
		 * parent_slot_offset must resolve, in the parent body, to the
		 * slot that holds this node.  This catches a stale/unset
		 * offset at the mutation that introduced it.  An example is a
		 * placement that passed a NULL slot to ft_set_parent.  Without
		 * the check, it would show later as a corrupted
		 * parent-pointer backtrack.
		 */
		if (ft_parent_node(metadata->parent_word)) {
			struct cds_ft_inode_flag **slot =
				ft_get_parent_slot(metadata,
						(struct cds_ft *) ft);

			if (!slot ||
			    ft_node_ptr(*slot) != ft_node_ptr(node_flag)) {
				if (out)
					fprintf(out, "ft_verify: depth %u: internal node %p slot_offset round-trip mismatch: slot %p holds %p\n",
						depth, node_flag, (void *) slot,
						slot ? (void *) *slot : NULL);
				return -1;
			}
		}
#ifdef FT_IMMEDIATE_FREE
		/*
		 * Check parent target is not poisoned (freed).
		 *
		 * ☠ THIS BLOCK DID NOT COMPILE, so the whole testing mode did
		 * not.  It still named @metadata->parent, a field that became
		 * @parent_word.  Nothing else reads the field under this
		 * ifdef, so the rename passed every build the gate runs.  The
		 * POISON-ON-FREE DETECTOR was lost with it.  The arena's 0xfe
		 * fill exists to catch a use-after-free class.  That class
		 * had no way to be seen.
		 */
		if (ft_parent_node(metadata->parent_word)) {
			unsigned char *p = (unsigned char *) ft_node_ptr(
				ft_parent_node(metadata->parent_word));
			if (*p == 0xfe) {
				if (out)
					fprintf(out, "ft_verify: depth %u: internal node %p parent %p points to freed (poisoned) node\n",
						depth, node_flag,
						(void *) metadata->parent_word);
				return -1;
			}
		}
#endif
		/*
		 * Type / alloc_index sanity.  ft_node_type already asserts
		 * the tag bits decode within FT_TYPE_BITS.  It does not
		 * verify that:
		 *
		 * - the entry is a real internal class,
		 * - the arena order matches the type's expected order,
		 * - nr_child fits the type's capacity.
		 *
		 * A corrupted tag/bitfield write would otherwise survive
		 * verify.  It would only manifest later as a wrong-sized scan
		 * or a min_child assertion.
		 */
		{
			unsigned int type_index = ft_node_type(node_flag);
			const struct cds_ft_type *type = &ft_types[type_index];
			size_t actual_order = cds_ft_item_order(node);

			if (type->type_class != FT_POPCOUNT &&
			    type->type_class != FT_PIGEON) {
				if (out)
					fprintf(out, "ft_verify: depth %u: internal node %p has non-internal type_class %d (type_index %u)\n",
						depth, node_flag,
						(int) type->type_class,
						type_index);
				return -1;
			}
			if (actual_order != type->order) {
				if (out)
					fprintf(out, "ft_verify: depth %u: internal node %p alloc order %zu mismatches type %u expected order %u\n",
						depth, node_flag,
						actual_order, type_index,
						(unsigned int) type->order);
				return -1;
			}
			if (ft_meta_nr_child(metadata) > type->max_child) {
				if (out)
					fprintf(out, "ft_verify: depth %u: internal node %p nr_child %u exceeds type %u max_child %u\n",
						depth, node_flag,
						ft_meta_nr_child(metadata), type_index,
						(unsigned int) type->max_child);
				return -1;
			}
		}
		/* Leaked node lock: see the compressed-node check. */
		if (metadata->state & FT_STATE_LOCK) {
			if (out)
				fprintf(out, "ft_verify: depth %u: internal node %p node lock set at rest (leaked lock)\n",
					depth, node_flag);
			return -1;
		}
		/* Reachable yet retired: see the compressed-node check. */
		if (metadata->state & FT_STATE_TOMBSTONE) {
			if (out)
				fprintf(out, "ft_verify: depth %u: internal node %p is RETIRED (tombstone) yet reachable\n",
					depth, node_flag);
			return -1;
		}
		/* Count external nodes attached to this node's metadata. */
		if (ft_verify_no_proxy_at_rest(out, "external_nodes",
				(struct cds_ft_inode_flag *) external_nodes,
				node_flag, depth))
			return -1;
		if (external_nodes) {
			if (ft_verify_external_chain(ft, out, visited, path,
					node_flag, external_nodes, depth))
				return -1;
			local_keys = 1;	/* One unique key position. */
		}
		/* Walk all 256 child slots. */
		for (key = 0; key < 256; key++) {
			struct cds_ft_inode_flag **slot = NULL;
			struct cds_ft_inode_flag *child_raw =
				ft_node_get_nth_skip(node_flag, &slot, (uint8_t) key, FT_PF_NONE);
			struct cds_ft_inode_flag *child;

			if (!child_raw)
				continue;
			if (ft_verify_no_proxy_at_rest(out, "child slot",
					child_raw, node_flag, depth))
				return -1;
			/*
			 * Raw slot may be skip-encoded; verify the encoded
			 * slen matches the underlying compressed's cn->len
			 * before resolving for the recursion.
			 */
			if (ft_verify_skip_encoding(ft, out, child_raw, depth + 1))
				return -1;
#ifdef FEATURE_FT_SKIP_COMPRESSED
			/*
			 * Slot-centric skip-slot invariant: a slot holding
			 * skip(cn) must be the very slot cn records as its
			 * skip_slot (the inverse of the cn-centric round-trip
			 * check above).  rec != slot names a dangling/aliased
			 * skip slot directly: a reparent left cn's skip_slot
			 * pointing elsewhere, or a placement never recorded it.
			 */
			if (ft_node_skip_compressed(child_raw) && slot) {
				struct cds_ft_compressed_node *scn =
					ft_skip_to_compressed(ft, child_raw);
				struct cds_ft_metadata *scnm =
					cds_ft_item_to_metadata(
						(struct cds_ft_inode *) scn);
				struct cds_ft_inode_flag **rec =
					ft_get_parent_slot(scnm,
						(struct cds_ft *) ft);

				if (rec != slot) {
					if (out)
						fprintf(out, "ft_verify: depth %u: node %p key %u slot %p holds skip(cn %p) but cn->skip_slot names %p (dangling skip slot)\n",
							depth, node_flag, key,
							(void *) slot, (void *) scn,
							(void *) rec);
					return -1;
				}
			}
#endif
			child = ft_resolve_skip_compressed(ft, child_raw);
			counted_children++;
			/*
			 * Path tracking: this slot's byte at @depth is
			 * the one being consumed to reach @child.
			 */
			if (path)
				path[depth] = (uint8_t) key;
			if (ft_node_external(child)) {
				/* External leaf chain at this slot. */
				if (ft_verify_external_chain(ft, out, visited,
						path, node_flag,
						(struct cds_ft_node *) ft_node_ptr(child),
						depth + 1))
					return -1;
				total_child_keys += 1;
			} else {
				unsigned long sub_keys = 0;

				if (ft_verify_node_recursive(ft, out, visited,
						path, child,
						node_flag, depth + 1,
						&sub_keys))
					return -1;
				total_child_keys += sub_keys;
			}
		}
		/* Verify nr_child. */
		if (ft_meta_nr_child(metadata) != counted_children) {
			if (out)
				fprintf(out, "ft_verify: depth %u: internal node %p nr_child mismatch: "
					"stored %u, counted %u\n",
					depth, node_flag, ft_meta_nr_child(metadata),
					counted_children);
			return -1;
		}
		/*
		 * A reachable non-root internal must hold at least one key: a
		 * child slot, or an external_nodes chain of its own.  With
		 * neither it is a DEAD END: an interior node that carries
		 * nothing, still wired into its parent's slot.
		 *
		 * The nr_child cross-check above cannot see this.  An emptied
		 * node has stored 0 and counted 0, and agrees with itself.
		 * Every other structural check passes too: parent pointer,
		 * slot round-trip and skip encoding.  So before this check
		 * the state was verify-clean.
		 *
		 * ft-inequality.h's empty-subtree arm relies on the absence
		 * of such a node BY NAME.  It says: "quiescently, reachable
		 * internal nodes have nr_child >= 1 (the upward walk prunes
		 * single-child chains wholesale ... so a slot-emptied
		 * internal is never left in place)".  On that premise it
		 * treats an empty node as "empty at this step" and climbs.
		 *
		 * When the premise breaks, the ordered walk either re-enters
		 * the same branch forever or skips the remaining subtree.
		 * Exact-key lookup still finds every key, which is why
		 * nothing else reports it.  So the validator has to be the
		 * one that names it.
		 *
		 * The root is exempt: an empty trie is exactly this shape.
		 */
		if (ft_parent_node(expected_parent) != NULL &&
		    counted_children == 0 &&
		    !external_nodes) {
			if (out) {
				unsigned int ti = ft_node_type(node_flag);

				fprintf(out, "ft_verify: depth %u: internal node %p is reachable with no children and no external_nodes (dead interior node left wired in its parent slot) [meta %p, type %u class %d max_child %u order %zu, incoming_byte %u, state %#lx, nr_keys %lu]\n",
					depth, node_flag, (void *) metadata, ti,
					(int) ft_types[ti].type_class,
					(unsigned int) ft_types[ti].max_child,
					cds_ft_item_order(node),
					(unsigned int) metadata->incoming_byte,
					(unsigned long) metadata->state,
					ft_nr_keys_get(metadata));
			}
			return -1;
		}
		/*
		 * Pigeon bitmap consistency.  Pigeon nodes maintain a
		 * 256-bit occupancy bitmap.  It is allocated alongside the
		 * node via ft_alloc_item with type->bitmap=true.  The
		 * directional / bitmap-scan readers consult it.
		 *
		 * The bitmap is a HINT.  The pointer array is the sole truth:
		 *
		 * - A non-NULL node->data[i] MUST have bit i set.  Otherwise
		 *   the directional scan would skip a live child.
		 * - A set bit over a NULL slot is allowed.  It is a sticky
		 *   soft-delete hole.  The scan rescans past it
		 *   (ft_pigeon_node_get_direction) and a recompact rebuilds
		 *   it away.
		 *
		 * So cross-check the weaker slot_set => bit_set.  This reports
		 * only a live child missing its bit, a desync that would lose
		 * directional results.
		 */
		{
			unsigned int t = ft_node_type(node_flag);
			const struct cds_ft_type *t_type = &ft_types[t];

			if (ft_type_is_pigeon(t_type->type_class)) {
				struct cds_ft_bitmap *bm =
					cds_ft_item_to_bitmap(node, t_type->order);
				unsigned int b;

				for (b = 0; b < FT_ENTRY_PER_NODE; b++) {
					struct cds_ft_inode_flag *child =
						ft_pigeon_node_get_nth(NULL, node,
							NULL, (uint8_t) b,
							FT_PF_NONE);
					bool slot_set = ft_node_ptr(child) != NULL;
					bool bit_set = cds_test_bit(bm->bitmap, b);

					/* A live slot must have its bit set; a set
					 * bit over a NULL slot is a tolerated sticky
					 * soft-delete hole. */
					if (slot_set && !bit_set) {
						if (out)
							fprintf(out, "ft_verify: depth %u: pigeon node %p slot %u: data %s, bitmap bit %s\n",
								depth, node_flag, b,
								slot_set ? "set" : "NULL",
								bit_set ? "set" : "clear");
						return -1;
					}
				}
			}
		}
#ifdef FEATURE_FT_COMPRESS
		/*
		 * Canonicalization (skip-compressed mode, non-root).  A
		 * single-child internal node with no external_nodes attached
		 * should have been replaced by a 1-byte compressed node.  In
		 * skip mode the compressed publishes as a skip-encoded
		 * pointer (zero read-side cost).  This is strictly cheaper
		 * than the 1-child internal it replaces.
		 *
		 * The external_nodes carve-out is mandatory.  Compressed
		 * nodes cannot carry external_nodes.  So an internal that
		 * hosts a NIL-key end-of-path and a single non-NIL branch
		 * must remain internal.
		 *
		 * The root is exempt.  The traversal entry reads ft->root
		 * directly.  There is no parent slot to encode the slen into,
		 * so the skip-encoded pointer's zero read-side cost has
		 * nowhere to attach.  A 1-byte compressed at root costs the
		 * same CL as a 1-child internal, so the canonicalization
		 * policy is allowed to keep it internal.
		 *
		 * In non-skip mode, ft_build_ordinal_chain keeps a 1-byte
		 * compressed floor at len >= 2.  So a 1-child internal at
		 * the head of a length-1 chain is canonical and must not
		 * trip this check.  The runtime gate handles the distinction.
		 *
		 * Dual of the existing "no two adjacent compresseds" check.
		 */
		if (ft_parent_node(expected_parent) != NULL &&
		    ft_group_skip_compressed(ft->group) &&
		    counted_children == 1 && !external_nodes) {
			if (out)
				fprintf(out, "ft_verify: depth %u: internal node %p has 1 child and no external_nodes (should be a 1-byte compressed in skip mode)\n",
					depth, node_flag);
			return -1;
		}
#endif
		/* Verify nr_keys -- only when the trie maintains order statistics. */
		if (ft->rank_stats) {
			unsigned long stored_nr_keys = ft_nr_keys_get(metadata);

			if (stored_nr_keys != total_child_keys + local_keys) {
				if (out)
					fprintf(out, "ft_verify: depth %u: internal node %p nr_keys mismatch: "
						"stored %lu, computed %lu (children %lu + local %lu)\n",
						depth, node_flag, stored_nr_keys,
						total_child_keys + local_keys,
						total_child_keys, local_keys);
				return -1;
			}
			*out_nr_keys = stored_nr_keys;
		} else {
			*out_nr_keys = total_child_keys + local_keys;
		}
		return 0;
	}
}


/*
 * ft_verify_ord_cells: verify the ordinal-cell list against the trie.
 *
 * Walk the trie in key order via the relational descent, and the cell
 * list in lockstep.  Assert that:
 *
 * - the list visits exactly the trie's distinct-key heads, in the same
 *   order;
 * - each visited cell is the head's own cell (head->prev), and
 *   cell->node points back at that head;
 * - the back-edge invariant ord_next(c)->ord_prev == c holds;
 * - the minimum cell's ord_prev is the sentinel;
 * - the walk terminates at the sentinel;
 * - ft_ord_first / ft_ord_last equal the trie minimum / maximum (an
 *   empty list = the sentinel pointing at itself).
 *
 * The oracle stays independent of the cell list: cache_valid is cleared
 * before each step, which forces the full descent.  The caller must hold
 * writer exclusion.  Return 0 on success, -1 on the first violation.
 */
static
int ft_verify_ord_cells(const struct cds_ft *cft, FILE *out)
{
	struct cds_ft *ft = (struct cds_ft *) cft;
	struct cds_ft_iter *iter;
	struct cds_ft_node *trie_head;
	struct ft_ord_cell *cell, *max_cell = NULL;
	int ret = 0;

	if (cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK) {
		if (out)
			fprintf(out, "ft_verify: ord-cell iter allocation failed\n");
		return -1;
	}
	iter->key_len = 0;
	iter->prefix_len = 0;
	iter->cache_valid = false;	/* force descent oracle */
	cds_ft_lookup_inequality_impl(ft, iter, FT_LOOKUP_GE,
			FT_LOOKUP_LIMIT_FIRST, false, false, NULL);
	trie_head = cds_ft_iter_node(iter);
	/* Sentinel-or-first: walk starts at the first cell (sentinel if empty). */
	cell = ft_ord_cell_resolve_ord(&ft->ord_sentinel.node.next);
	if (trie_head &&
	    !ft_ord_is_end(ft, ft_ord_cell_resolve_ord(
			&ft_ord_cell_ptr(trie_head->prev)->lnode.prev))) {
		if (out)
			fprintf(out, "ft_verify: ord-cell min head %p cell ord_prev is not the sentinel\n",
				(void *) trie_head);
		ret = -1;
		goto out;
	}
	while ((trie_head = cds_ft_iter_node(iter)) != NULL) {
		struct ft_ord_cell *head_cell =
			ft_ord_cell_ptr(rcu_dereference(trie_head->prev));
		struct ft_ord_cell *next_cell;

		if (cell != head_cell) {
			if (out)
				fprintf(out, "ft_verify: ord-cell order mismatch: list cell %p vs trie head %p cell %p\n",
					(void *) cell, (void *) trie_head,
					(void *) head_cell);
			ret = -1;
			goto out;
		}
		if (cell->node != trie_head) {
			if (out)
				fprintf(out, "ft_verify: ord-cell %p node %p != trie head %p\n",
					(void *) cell, (void *) cell->node,
					(void *) trie_head);
			ret = -1;
			goto out;
		}
		max_cell = cell;
		/*
		 * At the list tail next_cell resolves to the sentinel.  Verify
		 * runs under writer exclusion, so the list is fully circular
		 * here: there is no transient NULL termination.  The back-edge
		 * check below is SKIPPED for that end (ft_ord_is_end
		 * short-circuits).  The tail boundary (sentinel.prev == the
		 * maximum cell) is validated separately by the ft_ord_last()
		 * check after the loop.
		 */
		next_cell = ft_ord_cell_resolve_ord(&cell->lnode.next);
		if (!ft_ord_is_end(ft, next_cell) &&
		    ft_ord_cell_resolve_ord(&next_cell->lnode.prev) != cell) {
			if (out)
				fprintf(out, "ft_verify: ord-cell back-edge broken at cell %p (ord_next %p whose ord_prev is %p)\n",
					(void *) cell, (void *) next_cell,
					(void *) ft_ord_cell_resolve_ord(&next_cell->lnode.prev));
			ret = -1;
			goto out;
		}
		cell = next_cell;
		iter->cache_valid = false;	/* force descent oracle */
		cds_ft_lookup_inequality_impl(ft, iter, FT_LOOKUP_GT,
				FT_LOOKUP_LIMIT_NONE, false, false, NULL);
	}
	if (!ft_ord_is_end(ft, cell)) {
		if (out)
			fprintf(out, "ft_verify: ord-cell list longer than trie (extra cell %p)\n",
				(void *) cell);
		ret = -1;
	}
	if (ret == 0 && ft_ord_last(ft) != max_cell) {
		if (out)
			fprintf(out, "ft_verify: ord-cell tail %p != trie maximum cell %p\n",
				(void *) ft_ord_last(ft), (void *) max_cell);
		ret = -1;
	}
out:
	cds_ft_iter_destroy(iter);
	return ret;
}

/*
 * The whole of cds_ft_verify except the visited-set lifetime, which the
 * caller owns.  Sharing ONE set across several tries turns the existing
 * "reached twice" diagnostics into a CROSS-TRIE disjointness check
 * (cds_ft_verify_disjoint).
 *
 * Returns 0 when the trie passes all checks, -1 on the first violation.
 */
static
int ft_verify_one(const struct cds_ft *ft, FILE *out,
		struct ft_visited_set *visited)
{
	struct cds_ft_inode_flag *root = ft->root;
	unsigned long root_nr_keys = 0;
	uint8_t path_buf[FT_MAX_KEY_LEN];
	/*
	 * Track the structural key down to each leaf ONLY when this trie
	 * reads leaf-stored keys (ft->speculative_key_offset_active).  The
	 * external-chain walk then compares each leaf's stored speculative
	 * key against its position (ft_verify_speculative_key).  An EAGER
	 * trie does not consult the leaf key.  Path tracking is then left
	 * off and every path write/compare short-circuits, exactly as
	 * before this check existed.
	 */
	uint8_t *path = ft->speculative_key_offset_active ? path_buf : NULL;
	int ret;

	if (ft_verify_no_proxy_at_rest(out, "root", root, NULL, 0))
		return -1;
	/*
	 * Root-is-internal invariant.  ft->root must ALWAYS tag a plain
	 * internal node, never compressed / skip-compressed / external.
	 * The read path's hot descent relies on it.
	 *
	 * Every mutator that re-roots the trie (graft / graft_swap /
	 * detach / merge) materializes the new root through the
	 * build-invisible internal-root builders:
	 * ft_make_root_internal_glue and ft_build_extracted_root_glue.
	 * So no compressed root is ever published.
	 *
	 * The invariant is asserted here for two reasons:
	 *
	 * - Any future mutator that violates it is caught at the next
	 *   mutation point.
	 * - The descent's defensive non-internal-root resolver can be
	 *   retired once this is proven 0 across the bulk-op suite.
	 */
	if (!ft_node_internal(root)) {
		if (out)
			fprintf(out, "ft_verify: root %p is NOT internal (compressed=%d skip=%d external=%d)\n",
				(void *) root, (int) ft_node_compressed(root),
				(int) ft_node_skip_compressed(root),
				(int) ft_node_external(root));
		return -1;
	}
	/*
	 * @path is non-NULL only for a speculative-key-active trie (above).
	 * The leaf walk then validates each stored key against its
	 * position.  Otherwise @path is NULL and all path-tracking writes /
	 * compares short-circuit.
	 */
	/*
	 * The root's expected parent is THIS trie.  That is what makes the
	 * depth-0 check discriminating.  With NULL, the expected value was
	 * a constant that every trie shared.  Two tries rooted at one node
	 * then agreed with their own expectation and both passed.
	 */
	ret = ft_verify_node_recursive(ft, out, visited, path, root,
			ft_trie_parent(ft), 0, &root_nr_keys);
	if (ret)
		return -1;
	if (ft->group->ordered_list_set && ft_verify_ord_cells(ft, out))
		return -1;
	return 0;
}

/*
 * cds_ft_verify - Verify integrity of the entire Fractal Trie.
 *
 * Walk every internal and compressed node recursively, starting from
 * the root.  Check that nr_child, nr_keys and parent pointers are
 * self-consistent.
 *
 * Must be called with mutual exclusion wrt ALL updaters (writer
 * quiescence).  This is stronger than "the caller is a writer", for
 * two reasons.
 *
 * First, the walk reads slots and state words raw, with no MCAS proxy
 * resolution.  ft_verify_no_proxy_at_rest treats ANY reachable
 * in-flight proxy as an integrity error, because at rest a committed
 * txn leaves none.
 *
 * Second, its checks are inherently AT-REST checks.  The
 * global/cross-word invariants are:
 *
 * - nr_keys sums,
 * - nr_child vs actual slot population,
 * - parent symmetry,
 * - ord-list<->trie agreement.
 *
 * They span words that different peer txns commit independently.  So
 * no per-slot resolution can make them meaningful mid-flight.
 * Resolution yields old-XOR-new per WORD, never a global snapshot.
 *
 * The multi-writer counterpart is a (future) CONCURRENT VERIFIER.  It
 * is a §9-disciplined walk: it resolves every slot, and an in-flight
 * proxy is normal, not an error.  It is restricted to invariants that
 * are stable under peer commits:
 *
 * - per-node body-local checks on RCU-live immutable bodies;
 * - word-sets that commit atomically in ONE txn and have a dedicated
 *   consistent resolver (the (parent, offset) pair, the state word).
 *
 * See the verify-at-mutation hook note below.
 *
 * The visited set is PER CALL, so this validates one trie against
 * itself only.  A node reachable from two tries of the same group is
 * self-consistent in each and passes here; cds_ft_verify_disjoint is
 * the check that spans them.
 *
 * @out: file stream for diagnostic output on failure (may be NULL
 *       to suppress output).
 *
 * Returns CDS_FT_STATUS_OK if the trie passes all checks, or
 * CDS_FT_STATUS_INTEGRITY_ERROR on integrity violation.
 */
enum cds_ft_status cds_ft_verify(const struct cds_ft *ft, FILE *out)
{
	struct ft_visited_set visited;
	int ret;

	if (ft_visited_init(&visited)) {
		if (out)
			fprintf(out, "ft_verify: visited-set allocation failed\n");
		return CDS_FT_STATUS_INTEGRITY_ERROR;
	}
	ret = ft_verify_one(ft, out, &visited);
	ft_visited_destroy(&visited);
	return ret ? CDS_FT_STATUS_INTEGRITY_ERROR : CDS_FT_STATUS_OK;
}

/*
 * cds_ft_verify_disjoint - Run cds_ft_verify over @nr_fts tries at once.
 * Also check the invariant no single-trie walk can express: the tries
 * are NODE-DISJOINT.
 *
 * The cross-trie ops MOVE nodes between tries of a group (graft,
 * graft_swap, merge_at with src != dst); none of them SHARES one.  A
 * node left reachable from two tries is internally consistent in both.
 * Each trie's own parent pointers, nr_child and nr_keys still agree.
 * So cds_ft_verify passes on every trie individually.  The damage only
 * surfaces when a mutation in one trie rewrites the shared node.  The
 * OTHER trie then goes dirty underneath its owner, far from the op that
 * aliased it.
 *
 * One visited set spans all @nr_fts walks.  So the existing "reached
 * twice" diagnostics (subtree and external-chain) fire on the second
 * trie to reach a shared node.  Order-dependent in reporting only:
 * whichever trie is walked later names the node.
 *
 * The at-rest / writer-quiescence contract is the same as for
 * cds_ft_verify.  The tries must also be quiescent TOGETHER: an
 * in-flight cross-trie op legitimately has a node in hand between the
 * two.
 *
 * @fts:    array of @nr_fts tries; no NULL entries.  Passing the same trie
 *          twice is a caller error and reports as a self-alias.
 * @out:    diagnostic stream, may be NULL.
 *
 * Returns CDS_FT_STATUS_OK when every trie passes and they are disjoint,
 * CDS_FT_STATUS_INTEGRITY_ERROR on the first violation, or
 * CDS_FT_STATUS_INVALID_ARGUMENT_ERROR for a NULL entry.
 */
enum cds_ft_status cds_ft_verify_disjoint(struct cds_ft *const *fts,
		size_t nr_fts, FILE *out)
{
	struct ft_visited_set visited;
	size_t i;
	int ret = 0;

	if (!fts && nr_fts)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	if (ft_visited_init(&visited)) {
		if (out)
			fprintf(out, "ft_verify: visited-set allocation failed\n");
		return CDS_FT_STATUS_INTEGRITY_ERROR;
	}
	for (i = 0; i < nr_fts && !ret; i++) {
		if (!fts[i]) {
			ft_visited_destroy(&visited);
			return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
		}
		ret = ft_verify_one(fts[i], out, &visited);
		if (ret && out) {
			size_t j;

			fprintf(out, "ft_verify_disjoint: trie %zu of %zu (%p) failed\n",
				i, nr_fts, (void *) fts[i]);
			/*
			 * A shared node is a relation BETWEEN tries, so the roots
			 * of all of them are the context: an alias at depth 0 is
			 * two tries rooted at the same node, which no per-trie
			 * walk can see (a root's parent is NULL in every trie).
			 */
			for (j = 0; j < nr_fts; j++)
				fprintf(out, "ft_verify_disjoint:   trie %zu (%p) root %p\n",
					j, (void *) fts[j],
					fts[j] ? (void *) fts[j]->root : NULL);
		}
	}
	ft_visited_destroy(&visited);
	return ret ? CDS_FT_STATUS_INTEGRITY_ERROR : CDS_FT_STATUS_OK;
}

#ifdef FEATURE_FT_VERIFY_AT_MUTATION
/*
 * Hook called from CDS_FT_SCOPED_WRITER's scope-exit, before the
 * writer claim is released.  Sampled by the per-trie
 * @verify_at_mutation_period: the cds_ft_verify walk runs once every
 * @period mutations.  The counter is incremented and reset on the
 * boundary so it never exceeds @period - 1, avoiding any overflow /
 * cadence-drift issue on long-running workloads.  Period 0 disables
 * the walk entirely: the hook returns before it touches the counter.
 * On mismatch, abort with diagnostic.
 *
 * MULTI-WRITER: "verify-at-mutation" is a MISNOMER under concurrent
 * writers.  The name describes the trigger cadence.  But the hook
 * fires under ONE writer's scope, so what runs here is inherently a
 * CONCURRENT verifier.  Peers may be mid-commit, and their parked
 * proxies / half-visible txns are normal states, not corruption.
 *
 * The contract therefore holds under MW only once this hook runs the
 * concurrent-verifier subset.  That subset resolves every slot per
 * doc §9.  It checks only invariants stable under peer commits (see
 * the cds_ft_verify header for the split).
 *
 * TODAY the hook still calls the full at-rest cds_ft_verify, whose
 * global checks need all-updater quiescence.  Until that subset is
 * split out, enable FEATURE_FT_VERIFY_AT_MUTATION only with
 * externally-serialized writers.  This is an implementation gap, not
 * a design restriction.
 */
void ft_writer_scope_verify(struct cds_ft *ft)
{
	unsigned long period = ft->verify_at_mutation_period;

	if (period == 0)
		return;
	ft->verify_at_mutation_counter++;
	if (ft->verify_at_mutation_counter < period)
		return;
	ft->verify_at_mutation_counter = 0;

	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "FT verify-at-mutation: invariant violation on ft=%p\n",
			(void *) ft);
		abort();
	}
}
#endif

