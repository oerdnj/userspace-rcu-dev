// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-lookup-node.h
 *
 * Userspace RCU library - Fractal Trie: node-structure READ operations and
 * the bitmap layouts they are built on: popcount (1L / 2L) and pigeon.
 *
 * This file holds:
 *   - the layout primitives (pointer array / bitmap addressing / header
 *     sizing);
 *   - the read scanners (get_nth / get_direction / get_minmax /
 *     get_ith_pos) with their per-class implementations;
 *   - the non-inlined *_shared scanner copies.  The write path calls them
 *     through the fractal-trie.c redirect;
 *   - the per-class LOW-level set_nth.  They stay here, next to the bitmap
 *     layout they manipulate.
 *
 * The high-level child-slot mutators live in ft-mutation-node.h.  They are
 * the set_nth / replace_ptr dispatchers and ft_node_recompact.  They are
 * split out so that they can sit after ft-mutation-helpers.h instead of
 * forward-declaring ft_set_parent_raw.
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-lookup-node.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * Forward declarations for the 2-level popcount-bitmap node helpers.
 * Definitions follow further down (after the ft_dereference_acquire_*
 * macros they depend on).
 */
static inline_lookup
uint8_t ft_popcount_2l_node_get_nr_child(const struct cds_ft_type *type,
		struct cds_ft_inode *node);
static inline_lookup
void ft_popcount_2l_node_get_ith_pos(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		uint8_t i,
		uint8_t *v,
		struct cds_ft_inode_flag **iter);
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_node_get_direction(
		const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		int n, uint8_t *result_key,
		enum ft_direction dir);

/*
 * Forward declarations for the 1-level byte-popcount node helpers
 * (a single 256-bit occupancy bitmap, no second-level summary).
 */
static inline_lookup
uint8_t ft_popcount_1l_node_get_nr_child(const struct cds_ft_type *type,
		struct cds_ft_inode *node);
static inline_lookup
void ft_popcount_1l_node_get_ith_pos(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		uint8_t i,
		uint8_t *v,
		struct cds_ft_inode_flag **iter);
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_1l_node_get_direction(
		const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		int n, uint8_t *result_key,
		enum ft_direction dir);

/*
 * FT_POPCOUNT class nr_child: dispatch to the popcount_1l or
 * popcount_2l layout helper.  Both count populated slots via
 * popcount of the bitmap (independent of the soft-delete pointer
 * accounting in the metadata nr_child count).
 */
static inline_lookup
uint8_t ft_popcount_node_get_nr_child(const struct cds_ft_type *type,
		struct cds_ft_inode *node)
{
	assert(ft_type_is_popcount(type->type_class));
	if (type->popcount_2l)
		return ft_popcount_2l_node_get_nr_child(type, node);
	assert(type->popcount_1l);
	return ft_popcount_1l_node_get_nr_child(type, node);
}

/*
 * The order in which values and pointers are does does not matter: if
 * a value is missing, we return NULL. If a value is there, but its
 * associated pointers is still NULL, we return NULL too.
 */


/*
 * Runtime validation of ft_types[] popcount_1l class membership assumed
 * by the per-type dispatcher.  Called from cds_ft_group_create at init.
 * Not a static assert because ft_types[].max_child is a struct
 * member read, which gcc doesn't treat as an integer constant
 * expression.
 */
static inline __attribute__((unused))
void ft_specialized_scan_layout_assert(void)
{
#if CAA_BITS_PER_LONG >= 64
	assert(FT_ALIGN(ft_types[0].max_child, sizeof(void *)) == 8);
	assert(FT_ALIGN(ft_types[1].max_child, sizeof(void *)) == 8);
	assert(FT_ALIGN(ft_types[2].max_child, sizeof(void *)) == 16);
	assert(FT_ALIGN(ft_types[3].max_child, sizeof(void *)) == 32);
	assert(ft_types[4].popcount_1l);
	assert(ft_types[5].popcount_1l);
	assert(ft_type_is_pigeon(ft_types[6].type_class));
#else
	assert(ft_types[0].popcount_2l);
	assert(ft_types[1].popcount_2l);
	assert(ft_types[2].popcount_1l);
	assert(ft_types[3].popcount_1l);
	assert(ft_types[4].popcount_1l);
	assert(ft_type_is_pigeon(ft_types[5].type_class));
#endif
}

/* Forward declarations for the per-type popcount scanners. */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_16_16_max_3(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint);
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_16_16_max_5(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint);
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_32_8(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint);
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_64_4(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint);
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_1l_scan_28(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint);

/*
 * FT_POPCOUNT class get_direction: dispatch to popcount_1l or
 * popcount_2l layout helper.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_node_get_direction(
		const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		int n, uint8_t *result_key,
		enum ft_direction dir)
{
	assert(ft_type_is_popcount(type->type_class));
	if (type->popcount_2l)
		return ft_popcount_2l_node_get_direction(
				type, node, n, result_key, dir);
	assert(type->popcount_1l);
	return ft_popcount_1l_node_get_direction(
			type, node, n, result_key, dir);
}

/*
 * FT_POPCOUNT class get_ith_pos: dispatch to popcount_1l or
 * popcount_2l layout helper.  Returns the byte value v and
 * the (acquire-loaded) child pointer for the i-th populated slot in
 * popcount order.  Stub in baseline builds.
 */
static inline_lookup
void ft_popcount_node_get_ith_pos(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		uint8_t i,
		uint8_t *v,
		struct cds_ft_inode_flag **iter)
{
	assert(ft_type_is_popcount(type->type_class));
	assert(i < ft_popcount_node_get_nr_child(type, node));
	if (type->popcount_2l) {
		ft_popcount_2l_node_get_ith_pos(type, node, i, v, iter);
		return;
	}
	assert(type->popcount_1l);
	ft_popcount_1l_node_get_ith_pos(type, node, i, v, iter);
}

/*
 * Generic 2-level popcount-bitmap node header (4+4 byte-split).
 *
 * Used by the scan_16_16_max_<N> family (max_child=3 and max_child=5).
 * Other 2L variants use flat layouts and access node->data
 * directly without this struct (scan_32_8 / scan_64_4).
 *
 *   root_bm   : 16-bit; bit i set iff some child key has high
 *               nibble == i (high 4 bits of the byte)
 *   sub_bm[k] : 16-bit; presence of the low nibble for the k-th
 *               popcount-ordered hi position
 *
 * The pointer slot index for byte n = (hi<<4)|lo is the rank of (hi,lo)
 * among populated entries.  It equals the popcount of all bits set
 * below the (hi, lo) position across the concatenated sub_bm[] in
 * popcount order.
 *
 * For order-5 nodes (32 B, max_child = 3):
 *   header = 8 B (root_bm + 3 sub_bm)
 *   ptrs   = 24 B (3 child pointers)
 *
 * scan_16_16_max_3 reads the header as one u64 load and decomposes it
 * via fixed bit-shifts:
 *   low 16 bits   = root_bm
 *   upper 48 bits = sub_bm[0..2] in popcount order, with sub_bm[0] at
 *                   bits 16..31
 * The rank `popcount(subs & ((1ULL << bit_pos) - 1))` thus requires
 * sub_bm[0] in the low bits of the loaded u64.  In memory that maps
 * to first-to-last byte order on little-endian and last-to-first on
 * big-endian.  So the writer flips the in-memory offsets accordingly
 * (see ft_popcount_2l_root_bm_addr / ft_popcount_2l_sub_bm_addr).
 * max_child=5 uses per-u16 reads (scan_16_16_max_5).  It keeps the
 * canonical "root_bm at offset 0, sub_bm[k] at 2+2k" layout on both
 * arches.
 *
 * The lookup is branch-free past the two presence tests.  It uses the
 * portable __builtin_popcount{,ll} and no SIMD intrinsics.  So the
 * same code compiles for any architecture with a popcount intrinsic,
 * including software-emulated popcount on older targets.
 */
struct ft_popcount_2l_header {
	uint16_t root_bm;
	uint16_t sub_bm[];	/* length = type->max_child */
};

/*
 * Endian-aware addressing of the root_bm and sub_bm[k] u16 slots in
 * a 2-level popcount header.  See the layout comment above for why
 * max_child=3 reverses on big-endian.
 */
static inline_lookup
uint16_t *ft_popcount_2l_root_bm_addr(struct cds_ft_inode *node,
		unsigned int max_lc)
{
	unsigned int offset = 0;

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	if (max_lc == 3)
		offset = 2U * max_lc;	/* root_bm follows reversed sub_bm[] */
#endif
	(void) max_lc;
	return (uint16_t *) &node->data[offset];
}

static inline_lookup
uint16_t *ft_popcount_2l_sub_bm_addr(struct cds_ft_inode *node,
		unsigned int max_lc, unsigned int k)
{
	unsigned int offset = 2U + 2U * k;

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	if (max_lc == 3)
		offset = 2U * (max_lc - 1U - k);	/* sub_bm[0] highest */
#endif
	(void) max_lc;
	return (uint16_t *) &node->data[offset];
}

/*
 * Prefetch the sibling a next/prev walk will likely visit.  @slot may be one
 * past the pointer array, so the load can read the start of the next arena
 * item.  A writer may be initializing that item.  The value is only a hint,
 * but ThreadSanitizer reports the read as a race, so skip it there.
 */
static inline
void ft_prefetch_neighbor(struct cds_ft_inode_flag **slot)
{
#ifdef CMM_SANITIZE_THREAD
	(void) slot;
#else
	ft_maybe_prefetch_nta(uatomic_load(slot));
#endif
}

/*
 * Load a popcount bitmap word of a node that readers can reach.  An
 * in-place insert changes it concurrently, with relaxed stores, so load it
 * with a relaxed atomic load.  The bitmap orders nothing: an in-place insert
 * only appends at the highest rank, into a slot that is still NULL, and
 * publishes the child with a release store.  A reader which sees the new
 * bit before the child reads NULL, and finds the key absent.
 */
static inline
uint16_t ft_pc_bm16(const struct cds_ft_inode *node, unsigned int off)
{
	return uatomic_load((const uint16_t *) &node->data[off], CMM_RELAXED);
}

static inline
uint32_t ft_pc_bm32(const struct cds_ft_inode *node, unsigned int off)
{
	return uatomic_load((const uint32_t *) &node->data[off], CMM_RELAXED);
}

static inline
uint64_t ft_pc_bm64(const struct cds_ft_inode *node, unsigned int off)
{
	return uatomic_load((const uint64_t *) &node->data[off], CMM_RELAXED);
}

static inline
uint16_t ft_pc_bm(const uint16_t *bm)
{
	return uatomic_load(bm, CMM_RELAXED);
}

static inline_lookup
unsigned int ft_popcount_2l_header_bytes(unsigned int max_lc)
{
	if (max_lc == 6)
		return 12;	/* 5+3 split: 4B root + 8B packed_bms */
	if (max_lc == 12 || max_lc == 14 || max_lc == 16)
		return 16;	/* 6+2 FLAT: 8B root + 8B packed_bms */
	return (unsigned int) (sizeof(uint16_t) * (1U + max_lc));
}

static inline_lookup
struct cds_ft_inode_flag **ft_popcount_2l_pointers(
		struct cds_ft_inode *node, const struct cds_ft_type *type)
{
	unsigned int max_lc = type->max_child;
	unsigned int byte_offset;

	/*
	 * Flat scan_32_8 (12 B hdr + 4 B pad) and scan_64_4 (16 B hdr)
	 * scanners hardcode the ptr table at node+16.
	 *
	 * max_child=12 and 16 reuse the scan_64_4 layout on 32-bit.  Its
	 * 16-slot bitmap caps the safe distinct-hi count at 16.  The
	 * 8-slot cap of scan_32_8 is too tight for 12+ children in the
	 * worst case.
	 *
	 * max_child=16 lives in a 128 B order-7 node with 48 B trailing
	 * slack.  The bitmap cap is binding, not the node size.
	 */
	if (max_lc == 6 || max_lc == 12 || max_lc == 14 || max_lc == 16)
		byte_offset = 16;
	else
		byte_offset = FT_ALIGN(
			ft_popcount_2l_header_bytes(max_lc),
			sizeof(void *));
	return (struct cds_ft_inode_flag **)
		((uint8_t *) node + byte_offset);
}

/*
 * Lookup primitive: 2-level popcount, max_child = 3.
 *
 * One 64-bit load brings root_bm + sub_bm[0..2] into a single
 * register: low 16 bits = root_bm, upper 48 bits = sub_bm[0..2]
 * concatenated in popcount order.  Two presence tests (high then
 * low nibble) guard a single popcount-prefix-sum into the pointer
 * array.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_16_16_max_3(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	uint64_t bms = ft_pc_bm64(node, 0);
	uint16_t root = (uint16_t) bms;
	uint64_t subs = bms >> 16;
	unsigned int hi = (unsigned int) n >> 4;
	unsigned int lo = (unsigned int) n & 0xFU;
	unsigned int slot1, bit_pos, ptr_idx;
	struct cds_ft_inode_flag **pointers;

	/* 1. Root check (high nibble present?). */
	if (caa_unlikely(!((root >> hi) & 1U)))
		goto not_found;

	slot1 = (unsigned int) __builtin_popcount(root & ((1U << hi) - 1U));

	/*
	 * Absolute bit position of (hi, lo) in the 48-bit subs concat.
	 * (slot1 << 4) | lo is identical to slot1 * 16 + lo because
	 * lo < 16.  It folds the multiply+add into a single shift+OR
	 * dependency.
	 */
	bit_pos = (slot1 << 4) | lo;

	/* 2. Sub-bitmap check (low nibble present in this hi's slot?). */
	if (caa_unlikely(!((subs >> bit_pos) & 1ULL)))
		goto not_found;

	/* 3. Pointer index = rank of (hi, lo) among populated entries. */
	ptr_idx = (unsigned int) __builtin_popcountll(
			subs & ((1ULL << bit_pos) - 1ULL));
	pointers = (struct cds_ft_inode_flag **) ((uint8_t *) node + 8);
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = &pointers[ptr_idx];
	return ft_dereference_acquire_prefetch_hint(pointers[ptr_idx], pf_hint);

not_found:
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = NULL;
	return NULL;
}

/*
 * Lookup primitive: 2-level popcount, max_child = 5 (32-bit only).
 *
 * Same generic 2L layout as scan_16_16_max_3, with two more slots:
 *   [0..1]   root_bm        (u16)
 *   [2..11]  sub_bm[0..4]   (5 x u16)
 *   [12..31] 5 x 4-byte pointers
 *
 * 32-bit-native: 16-bit loads for root_bm and sub_bm[], 32-bit
 * popcount.  There are no 64-bit ops on the hot path.  On i386 they
 * would compile to register-pair handling.  The prefix sum runs a
 * short loop of 0..4 iterations over sub_bm[0..slot1-1].  The count
 * is bounded, so the compiler can predict / unroll the loop.
 *
 * Trade-off vs scan_16_16_max_3 (max_child=3): one variable-length
 * prefix loop in exchange for max_child 3 -> 5.  This recovers the
 * 12 B of trailing waste per 32 B node.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_16_16_max_5(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	uint16_t root = ft_pc_bm16(node, 0);
	const uint16_t *subs = (const uint16_t *) (node->data + 2);
	unsigned int hi = (unsigned int) n >> 4;
	unsigned int lo = (unsigned int) n & 0xFU;
	unsigned int slot1, ptr_idx, k;
	uint16_t sub;
	struct cds_ft_inode_flag **pointers;

	if (caa_unlikely(!((root >> hi) & 1U)))
		goto not_found;

	slot1 = (unsigned int) __builtin_popcount(
			root & (uint16_t) ((1U << hi) - 1U));
	sub = subs[slot1];

	if (caa_unlikely(!((sub >> lo) & 1U)))
		goto not_found;

	ptr_idx = (unsigned int) __builtin_popcount(
			sub & (uint16_t) ((1U << lo) - 1U));
	for (k = 0; k < slot1; k++)
		ptr_idx += (unsigned int) __builtin_popcount(subs[k]);

	pointers = (struct cds_ft_inode_flag **) ((uint8_t *) node + 12);
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = &pointers[ptr_idx];
	return ft_dereference_acquire_prefetch_hint(pointers[ptr_idx], pf_hint);

not_found:
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = NULL;
	return NULL;
}

/*
 * Lookup primitive: per-slot flat popcount with 5+3 byte-split,
 * max_child = 6.
 *
 * Layout in the 64-byte order-6 node.  It is a FLAT layout, NOT shared
 * with scan_16_16_max_3, which uses the 4+4 sub_bm[] layout.
 *   [0..3]   root_bm (32-bit; bit set per populated hi = n >> 3)
 *   [4..11]  packed_bms (8 bytes = 8 x 8-bit sub_bm; only first 6 used)
 *   [12..15] padding
 *   [16..63] 6 x cds_ft_inode_flag *
 *
 * The byte split is 5-bit hi + 3-bit lo.  It departs from the
 * quadbit-popcount (QP) 4+4 convention to fit all sub_bms in one u64.
 * packed_bms is a flat 64-bit view: bit at position (slot1*8 + lo) is
 * set iff (hi, lo) is a populated child.  The single packed bitmap
 * collapses prior + chunk-select + sub_bm popcount into one
 * popcount(bms & mask).
 *
 * Hot path:
 *   slot1   = popcount(root & ((1<<hi)-1))
 *   p       = (slot1 << 3) | lo
 *   ptr_idx = popcount(bms & ((1<<p)-1))
 *
 * There is no chunk-select cmov and no per-slot prior cache.  A single
 * packed bitmap suffices because max_child=6 fits in 48 bits, well
 * within a u64.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_32_8(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	uint32_t root = ft_pc_bm32(node, 0);
	uint64_t bms  = ft_pc_bm64(node, 4);
	unsigned int hi = (unsigned int) n >> 3;
	unsigned int lo = (unsigned int) n & 0x7U;
	unsigned int slot1, p, ptr_idx;
	struct cds_ft_inode_flag **pointers;

	if (caa_unlikely(!((root >> hi) & 1U)))
		goto not_found;

	slot1 = (unsigned int) __builtin_popcount(root & ((1U << hi) - 1U));
	p = (slot1 << 3) | lo;

	if (caa_unlikely(!((bms >> p) & 1ULL)))
		goto not_found;

	ptr_idx = (unsigned int) __builtin_popcountll(
			bms & ((1ULL << p) - 1ULL));

	pointers = (struct cds_ft_inode_flag **) ((uint8_t *) node + 16);
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = &pointers[ptr_idx];
	return ft_dereference_acquire_prefetch_hint(pointers[ptr_idx], pf_hint);

not_found:
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = NULL;
	return NULL;
}

/*
 * Lookup primitive: flat packed sub_bms with 6+2 byte-split,
 * max_child = 14.
 *
 * Layout in the 128-byte order-7 node.  It is 6+2 FLAT, not shared with
 * scan_16_16_max_3 / scan_32_8.
 *   [0..7]    root_bm (u64; bit set per populated hi = n >> 2)
 *   [8..15]   packed_bms (u64; 14 x 4-bit sub_bms, 56 bits used)
 *   [16..127] 14 x cds_ft_inode_flag *
 *
 * The 4-bit sub_bm width comes from lo = n & 3 (4 entries per chunk).
 * 14 chunks * 4 bits = 56 bits, fits a u64 with 8 bits to spare.
 * 14 is the largest count keeping the node within 128B (16 byte
 * header + 14 * 8 = 128 exactly).
 *
 * Both root and packed_bms load at constant offsets (no data-dep
 * load).  A single popcountll on (packed_bms & mask) yields ptr_idx.
 * There is no chunk-select cmov and no per-slot prior cache to refresh.
 *
 * Hot path:
 *   slot1   = popcount(root & ((1<<hi)-1))
 *   p       = (slot1 << 2) | lo
 *   ptr_idx = popcount(packed_bms & ((1<<p)-1))
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_scan_64_4(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	uint64_t root = ft_pc_bm64(node, 0);
	uint64_t bms  = ft_pc_bm64(node, 8);
	unsigned int hi = (unsigned int) n >> 2;
	unsigned int lo = (unsigned int) n & 0x3U;
	unsigned int slot1, p, ptr_idx;
	struct cds_ft_inode_flag **pointers;

	if (caa_unlikely(!((root >> hi) & 1ULL)))
		goto not_found;

	slot1 = (unsigned int) __builtin_popcountll(
			root & ((1ULL << hi) - 1ULL));
	p = (slot1 << 2) | lo;

	if (caa_unlikely(!((bms >> p) & 1ULL)))
		goto not_found;

	ptr_idx = (unsigned int) __builtin_popcountll(
			bms & ((1ULL << p) - 1ULL));

	pointers = (struct cds_ft_inode_flag **) ((uint8_t *) node + 16);
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = &pointers[ptr_idx];
	return ft_dereference_acquire_prefetch_hint(pointers[ptr_idx], pf_hint);

not_found:
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = NULL;
	return NULL;
}

/*
 * Total populated entries = sum of popcount(sub_bm[k]) for k where
 * sub_bm[k] is in use.  Since sub_bm[k] for k >= popcount(root_bm)
 * is unused (held at zero), summing all sub_bm values is safe.
 *
 * For max_child = 3, the three sub bitmaps fit in 6 bytes and
 * a single qword load lets us read them all alongside root_bm.
 */
static inline_lookup
uint8_t ft_popcount_2l_node_get_nr_child(const struct cds_ft_type *type,
		struct cds_ft_inode *node)
{
	unsigned int max_lc = type->max_child;

	if (max_lc == 6) {
		/* Flat 5+3 layout: nr_child = popcount of packed_bms. */
		uint64_t bms = ft_pc_bm64(node, 4);
		return (uint8_t) __builtin_popcountll(bms);
	}
	if (max_lc == 12 || max_lc == 14 || max_lc == 16) {
		/* Flat 6+2 layout: nr_child = popcount of packed_bms. */
		uint64_t bms = ft_pc_bm64(node, 8);
		return (uint8_t) __builtin_popcountll(bms);
	}
	{
		unsigned int total = 0, k;

		for (k = 0; k < max_lc; k++)
			total += (unsigned int) __builtin_popcount(
					ft_pc_bm(ft_popcount_2l_sub_bm_addr(
						node, max_lc, k)));
		return (uint8_t) total;
	}
}

/*
 * Locate the i-th populated entry in (hi,lo)-ascending order and
 * return both the encoded byte v = (hi<<4)|lo and the child pointer.
 * Walks sub_bm[] in popcount order, accumulating bit counts; within
 * the sub_bm that contains the i-th entry, finds the relative bit
 * position via masked popcount expansion.
 */
static inline_lookup
void ft_popcount_2l_node_get_ith_pos(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		uint8_t i,
		uint8_t *v,
		struct cds_ft_inode_flag **iter)
{
	unsigned int max_lc = type->max_child;
	struct cds_ft_inode_flag **pointers = ft_popcount_2l_pointers(node, type);

	if (max_lc == 6) {
		/* Flat 5+3 layout: find i-th set bit in packed_bms. */
		uint32_t root = ft_pc_bm32(node, 0);
		uint64_t bms  = ft_pc_bm64(node, 4);
		unsigned int slot1, lo, hi, p, j;
		uint64_t walk = bms;
		uint32_t r_walk;

		for (j = 0; j < i; j++)
			walk &= walk - 1ULL;
		p = (unsigned int) __builtin_ctzll(walk);
		slot1 = p >> 3;
		lo = p & 0x7U;

		r_walk = root;
		for (j = 0; j < slot1; j++)
			r_walk &= r_walk - 1U;
		hi = (unsigned int) __builtin_ctz(r_walk);

		*v = (uint8_t) ((hi << 3) | lo);
		*iter = ft_dereference_acquire(pointers[i]);
		return;
	}
	if (max_lc == 12 || max_lc == 14 || max_lc == 16) {
		/* Flat 6+2 layout: find i-th set bit in packed_bms. */
		uint64_t root = ft_pc_bm64(node, 0);
		uint64_t bms  = ft_pc_bm64(node, 8);
		unsigned int slot1, lo, hi, p, j;
		uint64_t walk = bms, r_walk;

		for (j = 0; j < i; j++)
			walk &= walk - 1ULL;
		p = (unsigned int) __builtin_ctzll(walk);
		slot1 = p >> 2;
		lo = p & 0x3U;

		r_walk = root;
		for (j = 0; j < slot1; j++)
			r_walk &= r_walk - 1ULL;
		hi = (unsigned int) __builtin_ctzll(r_walk);

		*v = (uint8_t) ((hi << 2) | lo);
		*iter = ft_dereference_acquire(pointers[i]);
		return;
	}
	{
		uint16_t root = ft_pc_bm(
			ft_popcount_2l_root_bm_addr(node, max_lc));
		unsigned int target = i, k, hi, lo;
		uint16_t sub = 0, root_walk;
		unsigned int sub_pop = 0;

		for (k = 0; k < max_lc; k++) {
			sub = ft_pc_bm(
				ft_popcount_2l_sub_bm_addr(node, max_lc, k));
			sub_pop = (unsigned int) __builtin_popcount(sub);
			if (target < sub_pop)
				break;
			target -= sub_pop;
		}
		assert(k < max_lc);
		/* hi = position of the k-th set bit in root_bm. */
		root_walk = root;
		hi = 0;
		for (unsigned int j = 0; j < k; j++) {
			unsigned int b = (unsigned int) __builtin_ctz(root_walk);
			root_walk &= root_walk - 1U;
			hi = b;
		}
		hi = (unsigned int) __builtin_ctz(root_walk);
		/* lo = position of the target-th set bit in sub. */
		{
			uint16_t s = sub;
			for (unsigned int j = 0; j < target; j++)
				s &= (uint16_t)(s - 1U);
			lo = (unsigned int) __builtin_ctz(s);
		}
		*v = (uint8_t) ((hi << 4) | lo);
		*iter = ft_dereference_acquire(pointers[i]);
	}
}

/*
 * Bit-scan core for the 2-level layouts.
 *
 * Their sub-bitmaps are concatenated in popcount order.  Bit
 * (slot1*G + lo) is set iff lo-value @lo is present under the slot1-th
 * populated hi-group, where:
 *   G     = 1 << @lo_bits
 *   slot1 = popcount(root & ((1<<hi)-1))
 *
 * The concat is passed split as @subs_lo (bits 0..63) and @subs_hi (bits
 * 64..127):
 *   - the three u64-packed layouts (scan_16_16_max_3, scan_32_8,
 *     scan_64_4) fit in @subs_lo alone (@subs_hi = 0);
 *   - scan_16_16_max_5 (32-bit; 5x16 = 80 bits) carries its top group in
 *     @subs_hi.
 *
 * Find the nearest set bit toward @dir from byte @n.  Return its byte
 * value (hi << lo_bits | lo), or -1 if there is none in that direction.
 * hi is recovered by selecting the slot1-th set bit of @root.
 *
 * The scan itself is the shared cds_find_{next,prev}_bit primitive, the
 * same one popcount_1l / pigeon use.  It runs over the concat materialized
 * as an unsigned long array, so it naturally spans the >64-bit max_5
 * concat.  It bounds the scan at @total = nr_groups*G.  So a transient
 * stray bit in an unused slot (>= total) during a concurrent insert shift
 * is ignored.
 *
 * @n is the EXCLUSIVE byte bound.  It may be one of the get_minmax
 * sentinels, which are NOT valid bytes:
 *   - -1 for FT_RIGHT/leftmost;
 *   - FT_ENTRY_PER_NODE for FT_LEFT/rightmost.
 * So decode hi/lo only for an in-range byte.  The resulting inclusive
 * start bit maps 1:1 onto the primitive's start_bit.  The primitive also
 * handles the out-of-range start that the sentinels produce.
 */
static inline_lookup
int ft_popcount_2l_dir_byte(uint64_t root, uint64_t subs_lo, uint64_t subs_hi,
		unsigned int lo_bits, unsigned int nr_groups,
		int n, enum ft_direction dir)
{
	unsigned int lo_mask = (1U << lo_bits) - 1U;
	unsigned int total = nr_groups << lo_bits;	/* <= 80 */
	unsigned int slot1p, lop, hip, j;
	uint64_t r;
	int start, p;
#if (CAA_BITS_PER_LONG >= 64)
	unsigned long bm[2] = { (unsigned long) subs_lo, (unsigned long) subs_hi };
#else
	unsigned long bm[4] = {
		(unsigned long) subs_lo, (unsigned long) (subs_lo >> 32),
		(unsigned long) subs_hi, (unsigned long) (subs_hi >> 32),
	};
#endif

	if (total == 0)
		return -1;
	if (dir == FT_RIGHT) {
		if (n < 0) {
			start = 0;				/* find first (smallest byte) */
		} else if (n >= 255) {
			return -1;				/* no byte > 255 */
		} else {
			unsigned int hi = (unsigned int) n >> lo_bits;
			unsigned int lo = (unsigned int) n & lo_mask;
			unsigned int slot1 = (unsigned int) __builtin_popcountll(
					root & (((uint64_t) 1 << hi) - 1U));

			start = ((root >> hi) & 1ULL) ?
				(int) ((slot1 << lo_bits) + lo + 1U) : (int) (slot1 << lo_bits);
		}
		p = cds_find_next_bit(bm, total, start);
	} else {
		if (n > 255) {
			start = (int) total - 1;		/* find last (largest byte) */
		} else if (n <= 0) {
			return -1;				/* no byte < 0 */
		} else {
			unsigned int hi = (unsigned int) n >> lo_bits;
			unsigned int lo = (unsigned int) n & lo_mask;
			unsigned int slot1 = (unsigned int) __builtin_popcountll(
					root & (((uint64_t) 1 << hi) - 1U));

			start = (((root >> hi) & 1ULL) ?
				(int) ((slot1 << lo_bits) + lo) : (int) (slot1 << lo_bits)) - 1;
		}
		p = cds_find_prev_bit(bm, total, start);
	}
	if (p < 0)
		return -1;
	/* p -> (slot1p, lop); recover hi by selecting the slot1p-th set bit. */
	slot1p = (unsigned int) p >> lo_bits;
	lop = (unsigned int) p & lo_mask;
	r = root;
	for (j = 0; j < slot1p; j++)
		r &= r - 1;		/* clear the lowest set bit */
	hip = (unsigned int) __builtin_ctzll(r);
	return (int) ((hip << lo_bits) | lop);
}

/*
 * Find the leftmost (FT_LEFT: largest v < n) or rightmost (FT_RIGHT:
 * smallest v > n) populated entry.
 *
 * Every 2-level layout bit-scans its concatenated sub-bitmap toward @dir
 * with ft_popcount_2l_dir_byte.  That helper delegates the scan to the
 * shared cds_find_{next,prev}_bit, the same primitive popcount_1l / pigeon
 * use.  The sub_bms are stored contiguously in popcount order, so one scan
 * crosses hi-groups.  A single select on root_bm then recovers the hi
 * nibble.
 *
 * The layout's scan primitive maps the byte to its pointer.  The bitmap is
 * only a hint: a soft-deleted entry has its bit set and a NULL pointer.
 * The scan continues past such an entry.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_2l_node_get_direction(
		const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		int n, uint8_t *result_key,
		enum ft_direction dir)
{
	unsigned int max_lc = type->max_child;
	uint64_t root, subs_lo, subs_hi = 0;
	unsigned int lo_bits, nr_groups;

	assert(dir == FT_LEFT || dir == FT_RIGHT);

	if (max_lc == 6) {			/* scan_32_8: 5+3 */
		root = ft_pc_bm32(node, 0);
		subs_lo = ft_pc_bm64(node, 4);
		lo_bits = 3;
	} else if (max_lc == 3) {		/* scan_16_16_max_3: 4+4 packed */
		uint64_t bms = ft_pc_bm64(node, 0);

		root = (uint16_t) bms;
		subs_lo = bms >> 16;
		lo_bits = 4;
#if (CAA_BITS_PER_LONG < 64)
	} else if (max_lc == 5) {		/* scan_16_16_max_5: 5 x u16, 80-bit concat */
		const uint16_t *s = (const uint16_t *) &node->data[2];

		root = ft_pc_bm16(node, 0);
		subs_lo = (uint64_t) s[0] | ((uint64_t) s[1] << 16)
			| ((uint64_t) s[2] << 32) | ((uint64_t) s[3] << 48);
		subs_hi = s[4];		/* top group -> bits 64..79 */
		lo_bits = 4;
#endif
	} else {				/* scan_64_4: 6+2 (max 12/14/16) */
		root = ft_pc_bm64(node, 0);
		subs_lo = ft_pc_bm64(node, 8);
		lo_bits = 2;
	}
	nr_groups = (unsigned int) __builtin_popcountll(root);
	cmm_smp_rmb();	/* read bitmaps before pointers */
	for (;;) {
		int byte = ft_popcount_2l_dir_byte(root, subs_lo, subs_hi,
				lo_bits, nr_groups, n, dir);
		struct cds_ft_inode_flag *ptr;
		struct cds_ft_inode_flag **slot = NULL;

		if (byte < 0)
			return NULL;
		ptr = (max_lc == 6) ?
			ft_popcount_2l_scan_32_8(node, &slot,
				(uint8_t) byte, FT_PF_NONE) :
		      (max_lc == 3) ?
			ft_popcount_2l_scan_16_16_max_3(node, &slot,
				(uint8_t) byte, FT_PF_NONE) :
#if (CAA_BITS_PER_LONG < 64)
		      (max_lc == 5) ?
			ft_popcount_2l_scan_16_16_max_5(node, &slot,
				(uint8_t) byte, FT_PF_NONE) :
#endif
			ft_popcount_2l_scan_64_4(node, &slot,
				(uint8_t) byte, FT_PF_NONE);
		if (ptr) {
			/*
			 * Spatial prefetch.  The child pointers are a dense
			 * byte-ordered array.  So the adjacent sibling in
			 * iteration order is slot[+1] (FT_RIGHT / next) or
			 * slot[-1] (FT_LEFT / prev).  It is a near-future
			 * cds_ft_next/prev target.
			 *
			 * No bounds check: the one-element over/under-read cannot
			 * fault, by allocator construction.
			 *   - slot[+1] past the last pointer of a full node:
			 *     the worst case is a node at the end of the 2 MiB
			 *     node-body run.  The load then lands in the
			 *     FT_FAR_METADATA metadata array, which the
			 *     allocator places immediately after that run.
			 *     Within a node it is just an unused allocated
			 *     slot.
			 *   - slot[-1] before pointers[0] lands in the node's
			 *     bitmap header, which precedes the pointer array.
			 * Either way the load hits allocated memory.  The word
			 * read is unrelated to a real child pointer.  But
			 * ft_maybe_prefetch (__builtin_prefetch) silently drops
			 * NULL / non-canonical addresses without faulting.
			 */
			ft_prefetch_neighbor(dir == FT_RIGHT ?
					&slot[1] : &slot[-1]);
			*result_key = (uint8_t) byte;
			return ptr;
		}
		n = byte;	/* soft-deleted: scan past this bit */
	}
}

/*
 * Insert (n, child_node_flag) into a freshly-allocated, unpublished
 * popcount_2l node.  Called only from the recompact path.  The new node
 * is not yet wired into the trie, so direct in-place mutation (shifts
 * of sub_bm[] and pointers[]) is race-free.
 *
 * is_init=true zeroes the bitmap header and writes pointers[0] for
 * the first entry.  Subsequent calls with is_init=false fold the new
 * entry into the existing layout, shifting sub_bm slots and pointer
 * slots as needed to keep popcount order.
 *
 * Returns 0 on success.  Asserts caller-precondition violations
 * (full node, duplicate key) since recompact paths are responsible
 * for choosing the right destination type.
 */
static
int ft_popcount_2l_node_set_nth(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		struct cds_ft_metadata *metadata,
		uint8_t n,
		struct cds_ft_inode_flag *child_node_flag,
		bool is_init)
{
	struct cds_ft_inode_flag **pointers = ft_popcount_2l_pointers(node, type);
	unsigned int max_lc = type->max_child;

	if (max_lc == 6) {
		/* Flat 5+3 layout. */
		unsigned int hi = (unsigned int) n >> 3;
		unsigned int lo = (unsigned int) n & 0x7U;
		uint32_t root;
		uint64_t bms;
		unsigned int slot1, p, ptr_idx, nr_child, k;

		if (is_init) {
			/* Alloc returned zeroed memory; just write new state. */
			*(uint32_t *) &node->data[0] = (uint32_t) (1U << hi);
			*(uint64_t *) &node->data[4] = (uint64_t) (1ULL << lo);
			pointers[0] = child_node_flag;
			ft_meta_nr_child_inc(metadata);
			return 0;
		}

		root = *(const uint32_t *) &node->data[0];
		bms  = *(const uint64_t *) &node->data[4];
		nr_child = (unsigned int) __builtin_popcountll(bms);
		assert(nr_child < max_lc);

		slot1 = (unsigned int) __builtin_popcount(root & ((1U << hi) - 1U));
		p = (slot1 << 3) | lo;

		if (!((root >> hi) & 1U)) {
			/* New hi: shift packed_bms bytes [slot1..) up by one byte. */
			uint64_t low_mask = (slot1 == 0) ? 0ULL
				: ((1ULL << (slot1 * 8)) - 1ULL);
			uint64_t low_part = bms & low_mask;
			uint64_t high_part = (bms & ~low_mask) << 8;
			uint64_t new_byte = ((uint64_t)(1U << lo)) << (slot1 * 8);
			bms = low_part | new_byte | high_part;
			root |= (1U << hi);
		} else {
			/* Existing hi: OR new lo bit into byte at slot1 position. */
			assert(!((bms >> p) & 1ULL));	/* duplicate would be a bug */
			bms |= ((uint64_t)(1U << lo)) << (slot1 * 8);
		}

		ptr_idx = (unsigned int) __builtin_popcountll(
				bms & ((1ULL << p) - 1ULL));
		/* Shift pointers[ptr_idx..nr_child) up. */
		for (k = nr_child; k > ptr_idx; k--)
			pointers[k] = pointers[k - 1];
		pointers[ptr_idx] = child_node_flag;

		*(uint32_t *) &node->data[0] = root;
		*(uint64_t *) &node->data[4] = bms;
		ft_meta_nr_child_inc(metadata);
		return 0;
	}

	if (max_lc == 12 || max_lc == 14 || max_lc == 16) {
		/* Flat 6+2 layout. */
		unsigned int hi = (unsigned int) n >> 2;
		unsigned int lo = (unsigned int) n & 0x3U;
		uint64_t root, bms;
		unsigned int slot1, p, ptr_idx, nr_child, k;

		if (is_init) {
			*(uint64_t *) &node->data[0] = 1ULL << hi;
			*(uint64_t *) &node->data[8] = (uint64_t) (1ULL << lo);
			pointers[0] = child_node_flag;
			ft_meta_nr_child_inc(metadata);
			return 0;
		}

		root = *(const uint64_t *) &node->data[0];
		bms  = *(const uint64_t *) &node->data[8];
		nr_child = (unsigned int) __builtin_popcountll(bms);
		assert(nr_child < max_lc);

		slot1 = (unsigned int) __builtin_popcountll(
				root & ((1ULL << hi) - 1ULL));
		p = (slot1 << 2) | lo;

		if (!((root >> hi) & 1ULL)) {
			/* New hi: shift packed_bms nibbles [slot1..) up by one nibble (4 bits). */
			uint64_t low_mask = (slot1 == 0) ? 0ULL
				: ((1ULL << (slot1 * 4)) - 1ULL);
			uint64_t low_part = bms & low_mask;
			uint64_t high_part = (bms & ~low_mask) << 4;
			uint64_t new_nibble = ((uint64_t)(1U << lo)) << (slot1 * 4);
			bms = low_part | new_nibble | high_part;
			root |= 1ULL << hi;
		} else {
			/* Existing hi: OR new lo bit into slot1's nibble. */
			assert(!((bms >> p) & 1ULL));	/* duplicate would be a bug */
			bms |= ((uint64_t)(1U << lo)) << (slot1 * 4);
		}

		ptr_idx = (unsigned int) __builtin_popcountll(
				bms & ((1ULL << p) - 1ULL));
		/* Shift pointers[ptr_idx..nr_child) up. */
		for (k = nr_child; k > ptr_idx; k--)
			pointers[k] = pointers[k - 1];
		pointers[ptr_idx] = child_node_flag;

		*(uint64_t *) &node->data[0] = root;
		*(uint64_t *) &node->data[8] = bms;
		ft_meta_nr_child_inc(metadata);
		return 0;
	}

	{
	unsigned int hi = (unsigned int) n >> 4;
	unsigned int lo = (unsigned int) n & 0xFU;
	uint16_t root, sub;
	unsigned int slot1, ptr_idx, nr_child, k;

	if (is_init) {
		memset(&node->data[0], 0, ft_popcount_2l_header_bytes(max_lc));
		*ft_popcount_2l_root_bm_addr(node, max_lc) = (uint16_t) (1U << hi);
		*ft_popcount_2l_sub_bm_addr(node, max_lc, 0) = (uint16_t) (1U << lo);
		pointers[0] = child_node_flag;
		ft_meta_nr_child_inc(metadata);
		return 0;
	}

	root = *ft_popcount_2l_root_bm_addr(node, max_lc);
	nr_child = ft_popcount_2l_node_get_nr_child(type, node);
	assert(nr_child < max_lc);
	slot1 = (unsigned int) __builtin_popcount(root & ((1U << hi) - 1U));

	if (!((root >> hi) & 1U)) {
		/*
		 * New hi: shift sub_bm[slot1..nr_used) one slot up to
		 * make room.  nr_used = popcount(root) -- the number
		 * of currently-occupied sub_bm slots.
		 */
		unsigned int nr_used = (unsigned int) __builtin_popcount(root);
		for (k = nr_used; k > slot1; k--)
			*ft_popcount_2l_sub_bm_addr(node, max_lc, k) =
				*ft_popcount_2l_sub_bm_addr(node, max_lc, k - 1);
		*ft_popcount_2l_sub_bm_addr(node, max_lc, slot1) = 0;
		*ft_popcount_2l_root_bm_addr(node, max_lc) =
			(uint16_t) (root | (1U << hi));
	}
	sub = *ft_popcount_2l_sub_bm_addr(node, max_lc, slot1);
	assert(!((sub >> lo) & 1U));	/* duplicate key would be a bug */

	/* Compute pointer insertion index across full layout. */
	{
		uint64_t subs_below = 0;
		for (k = 0; k < slot1; k++)
			subs_below += (uint64_t) __builtin_popcount(
				*ft_popcount_2l_sub_bm_addr(node, max_lc, k));
		subs_below += (uint64_t) __builtin_popcount(
				(unsigned int) sub & ((1U << lo) - 1U));
		ptr_idx = (unsigned int) subs_below;
	}

	/* Shift pointers[ptr_idx..nr_child) up. */
	for (k = nr_child; k > ptr_idx; k--)
		pointers[k] = pointers[k - 1];
	pointers[ptr_idx] = child_node_flag;

	*ft_popcount_2l_sub_bm_addr(node, max_lc, slot1) =
		(uint16_t) (sub | (1U << lo));
	ft_meta_nr_child_inc(metadata);
	return 0;
	}
}

/*
 * 1-level byte-popcount node header.
 *
 * One bit per possible byte value (0..255).  Used by larger
 * popcount nodes (orders 8/9/10).  There the 2-level popcount_2l
 * layout would not fit alongside the pointer array.
 *
 * Layout in the order-8 (256-byte) node:
 *   [0..31]   bm[4] (256-bit bitmap, 4 x uint64_t)
 *   [32..255] 28 x cds_ft_inode_flag *
 */
struct ft_popcount_1l_header {
	uint64_t bm[4];
};

static inline_lookup
struct cds_ft_inode_flag **ft_popcount_1l_pointers(
		struct cds_ft_inode *node)
{
	return (struct cds_ft_inode_flag **) ((uint8_t *) node + 32);
}

/*
 * Lookup primitive: 1-level byte popcount, 256-bit bitmap.
 *
 * Four 64-bit loads bring the entire bitmap.  word_idx selects which
 * 64-bit chunk holds bit n; ptr_idx is the popcount of all chunks
 * before plus the masked prefix of the current chunk.  All four
 * popcountll calls can issue in parallel; the dependency chain is
 * load -> popcount -> add chain.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_1l_scan_28(
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	/* Relaxed loads: an in-place insert sets a bit concurrently (ft_pc_bm64). */
	uint64_t w0 = ft_pc_bm64(node, 0);
	uint64_t w1 = ft_pc_bm64(node, 8);
	uint64_t w2 = ft_pc_bm64(node, 16);
	uint64_t w3 = ft_pc_bm64(node, 24);
	unsigned int word_idx = (unsigned int) n >> 6;
	unsigned int bit_idx = (unsigned int) n & 63U;
	uint64_t word, prefix_mask;
	unsigned int ptr_idx;
	struct cds_ft_inode_flag **pointers;

	switch (word_idx) {
	case 0:
		word = w0;
		ptr_idx = 0;
		break;
	case 1:
		word = w1;
		ptr_idx = (unsigned int) __builtin_popcountll(w0);
		break;
	case 2:
		word = w2;
		ptr_idx = (unsigned int) __builtin_popcountll(w0)
			+ (unsigned int) __builtin_popcountll(w1);
		break;
	default:	/* case 3 */
		word = w3;
		ptr_idx = (unsigned int) __builtin_popcountll(w0)
			+ (unsigned int) __builtin_popcountll(w1)
			+ (unsigned int) __builtin_popcountll(w2);
		break;
	}

	if (caa_unlikely(!((word >> bit_idx) & 1ULL)))
		goto not_found;

	prefix_mask = (1ULL << bit_idx) - 1ULL;
	ptr_idx += (unsigned int) __builtin_popcountll(word & prefix_mask);

	pointers = (struct cds_ft_inode_flag **) ((uint8_t *) node + 32);
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = &pointers[ptr_idx];
	return ft_dereference_acquire_prefetch_hint(pointers[ptr_idx], pf_hint);

not_found:
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = NULL;
	return NULL;
}

static inline_lookup
uint8_t ft_popcount_1l_node_get_nr_child(const struct cds_ft_type *type,
		struct cds_ft_inode *node)
{
	const struct ft_popcount_1l_header *hdr =
		(const struct ft_popcount_1l_header *) &node->data[0];
	(void) type;
	return (uint8_t) (
		__builtin_popcountll(uatomic_load(&hdr->bm[0], CMM_RELAXED)) +
		__builtin_popcountll(uatomic_load(&hdr->bm[1], CMM_RELAXED)) +
		__builtin_popcountll(uatomic_load(&hdr->bm[2], CMM_RELAXED)) +
		__builtin_popcountll(uatomic_load(&hdr->bm[3], CMM_RELAXED)));
}

/*
 * Find the i-th set bit in the 256-bit bitmap.  Return its byte
 * value (0..255) and the corresponding pointer.  Iterate over the
 * four 64-bit words.  Use a "skip the bottom popcount bits" pattern
 * (clear the lowest set bit i times) to land on the target.
 */
static inline_lookup
void ft_popcount_1l_node_get_ith_pos(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		uint8_t i,
		uint8_t *v,
		struct cds_ft_inode_flag **iter)
{
	struct ft_popcount_1l_header *hdr =
		(struct ft_popcount_1l_header *) &node->data[0];
	struct cds_ft_inode_flag **pointers =
		ft_popcount_1l_pointers(node);
	unsigned int word_idx = 0;
	unsigned int rem = i;
	uint64_t word;
	unsigned int pop;
	unsigned int bit;
	(void) type;

	for (word_idx = 0; word_idx < 4; word_idx++) {
		pop = (unsigned int) __builtin_popcountll(
			uatomic_load(&hdr->bm[word_idx], CMM_RELAXED));
		if (rem < pop)
			break;
		rem -= pop;
	}
	assert(word_idx < 4);
	word = uatomic_load(&hdr->bm[word_idx], CMM_RELAXED);
	while (rem--)
		word &= word - 1ULL;	/* clear lowest set bit */
	bit = (unsigned int) __builtin_ctzll(word);
	*v = (uint8_t) (word_idx * 64U + bit);
	*iter = ft_dereference_acquire(pointers[i]);
}

/*
 * Recover the branch byte indexing @slot within internal parent @node (the
 * inverse of byte -> child slot).
 *   - Pigeon: the body is a 256-entry array indexed by byte, so the slot
 *     offset IS the byte.
 *   - Popcount: children are packed by set-bit rank.  So the slot's index
 *     in the packed pointer array (slot - pointers_base) is its rank.
 *     get_ith_pos maps the rank to the byte.
 * @node is always an internal (pigeon/popcount) node.  ft_set_parent_slot
 * guards out compressed parents, which have no slot array.
 */
static
uint8_t ft_slot_to_byte(const struct cds_ft_type *type,
		struct cds_ft_inode *node, struct cds_ft_inode_flag **slot)
{
	if (ft_type_is_pigeon(type->type_class))
		return (uint8_t) (slot -
			(struct cds_ft_inode_flag **) node->data);
	{
		struct cds_ft_inode_flag **base = type->popcount_2l ?
			ft_popcount_2l_pointers(node, type) :
			ft_popcount_1l_pointers(node);
		uint8_t v;
		struct cds_ft_inode_flag *child;

		ft_popcount_node_get_ith_pos(type, node,
			(uint8_t) (slot - base), &v, &child);
		return v;
	}
}

/*
 * Find the leftmost (largest v < n) or rightmost (smallest v > n)
 * populated entry.
 *
 * Scan the inline 256-bit occupancy bitmap for the nearest set bit toward
 * @dir with cds_find_prev_bit / cds_find_next_bit, as
 * ft_pigeon_node_get_direction does.  Then map that byte value to its
 * compact pointer via ft_popcount_1l_scan_28.  The bitmap is only a hint:
 * a soft-deleted entry keeps its bit set but holds a NULL pointer.  On
 * NULL, continue the scan past that bit.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_popcount_1l_node_get_direction(
		const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		int n, uint8_t *result_key,
		enum ft_direction dir)
{
	assert(dir == FT_LEFT || dir == FT_RIGHT);

	{
		struct ft_popcount_1l_header *hdr =
			(struct ft_popcount_1l_header *) &node->data[0];
		unsigned long bm[sizeof(hdr->bm) / sizeof(unsigned long)];
		unsigned int k;
		int i;

		(void) type;	/* inline bitmap: the vtable type arg is unused here */
retry:
		/* A relaxed snapshot: an in-place insert may set a bit. */
		for (k = 0; k < sizeof(bm) / sizeof(bm[0]); k++)
			bm[k] = uatomic_load(&((unsigned long *) hdr->bm)[k],
					CMM_RELAXED);
		if (dir == FT_LEFT)
			i = cds_find_prev_bit(bm, FT_ENTRY_PER_NODE, n - 1);
		else
			i = cds_find_next_bit(bm, FT_ENTRY_PER_NODE, n + 1);
		if (i < 0)
			return NULL;
		{
			struct cds_ft_inode_flag **slot = NULL;
			struct cds_ft_inode_flag *ptr =
				ft_popcount_1l_scan_28(node, &slot,
					(uint8_t) i, FT_PF_NONE);

			if (!ptr) {
				/*
				 * Soft-deleted: the bit is still set but the
				 * pointer load (source of truth) is NULL.
				 * Continue the bitmap scan past this bit.
				 */
				n = i;
				goto retry;
			}
			/*
			 * Spatial prefetch (dense byte-ordered pointers): the
			 * adjacent sibling is slot[+1] (FT_RIGHT) / slot[-1]
			 * (FT_LEFT).  It is a near-future cds_ft_next/prev
			 * target.  The unchecked slot[+-1] over/under-read is
			 * fault-free.  The reason is the same allocator
			 * construction as documented in
			 * ft_popcount_2l_node_get_direction:
			 *   - +1: FT_FAR_METADATA after the 2 MiB run;
			 *   - -1: bitmap header before pointers.
			 */
			ft_prefetch_neighbor(dir == FT_RIGHT ?
					&slot[1] : &slot[-1]);
			*result_key = (uint8_t) i;
			return ptr;
		}
	}
}

/*
 * Insert (n, child_node_flag) into a freshly-allocated, unpublished
 * byte-popcount node.  Called only from the recompact path.
 *
 * is_init=true zeroes the bitmap and writes pointers[0].  Subsequent
 * non-init calls fold a new entry into the existing layout, shifting
 * pointers as needed to keep popcount order.
 */
static
int ft_popcount_1l_node_set_nth(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		struct cds_ft_metadata *metadata,
		uint8_t n,
		struct cds_ft_inode_flag *child_node_flag,
		bool is_init)
{
	struct ft_popcount_1l_header *hdr =
		(struct ft_popcount_1l_header *) &node->data[0];
	struct cds_ft_inode_flag **pointers =
		ft_popcount_1l_pointers(node);
	unsigned int word_idx = (unsigned int) n >> 6;
	unsigned int bit_idx = (unsigned int) n & 63U;
	unsigned int nr_child, ptr_idx, k;
	uint64_t word;

	if (is_init) {
		memset(hdr, 0, sizeof(*hdr));
		hdr->bm[word_idx] = 1ULL << bit_idx;
		pointers[0] = child_node_flag;
		ft_meta_nr_child_inc(metadata);
		return 0;
	}

	nr_child = ft_popcount_1l_node_get_nr_child(type, node);
	assert(nr_child < type->max_child);
	word = hdr->bm[word_idx];
	assert(!((word >> bit_idx) & 1ULL));	/* duplicate key would be a bug */

	ptr_idx = 0;
	for (k = 0; k < word_idx; k++)
		ptr_idx += (unsigned int) __builtin_popcountll(hdr->bm[k]);
	ptr_idx += (unsigned int) __builtin_popcountll(
			word & ((1ULL << bit_idx) - 1ULL));

	for (k = nr_child; k > ptr_idx; k--)
		pointers[k] = pointers[k - 1];
	pointers[ptr_idx] = child_node_flag;

	hdr->bm[word_idx] = word | (1ULL << bit_idx);
	ft_meta_nr_child_inc(metadata);
	return 0;
}


static inline_lookup
struct cds_ft_inode_flag *ft_pigeon_node_get_nth(const struct cds_ft_type __attribute__((unused)) *type,
		struct cds_ft_inode *node,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	struct cds_ft_inode_flag **child_node_flag_ptr;
	struct cds_ft_inode_flag *child_node_flag;

	assert(!type || ft_type_is_pigeon(type->type_class));
	child_node_flag_ptr = &((struct cds_ft_inode_flag **) node->data)[n];
	child_node_flag = ft_dereference_acquire_prefetch_hint(*child_node_flag_ptr, pf_hint);
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = child_node_flag_ptr;
	return child_node_flag;
}

static inline_lookup
struct cds_ft_inode_flag *ft_pigeon_node_get_direction(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		int n, uint8_t *result_key,
		enum ft_direction dir)
{
	struct cds_ft_inode_flag **child_node_flag_ptr;
	struct cds_ft_inode_flag *child_node_flag;
	struct cds_ft_bitmap *bitmap = cds_ft_item_to_bitmap(node, type->order);
	int i;

	assert(ft_type_is_pigeon(type->type_class));
	assert(dir == FT_LEFT || dir == FT_RIGHT);

retry:
	if (dir == FT_LEFT)
		i = cds_find_prev_bit(bitmap->bitmap, FT_ENTRY_PER_NODE, n - 1);
	else
		i = cds_find_next_bit(bitmap->bitmap, FT_ENTRY_PER_NODE, n + 1);
	if (i >= 0) {
		child_node_flag_ptr = &((struct cds_ft_inode_flag **) node->data)[i];
		child_node_flag = ft_dereference_acquire(*child_node_flag_ptr);
		if (!child_node_flag) {
			/*
			 * The source of truth is the pointer load.
			 * Continue the bitmap scan if the node is
			 * not found.
			 */
			n = i;
			goto retry;
		}
		/*
		 * Spatial prefetch.  Pigeon slots are byte-indexed (sparse).
		 * So unlike the dense popcount layouts, the adjacent present
		 * child is NOT slot[+-1].  One more bitmap scan in the
		 * iteration direction locates it.  We prefetch it as a
		 * near-future cds_ft_next/prev target.  The extra scan is
		 * cheap against a far-metadata/body miss.
		 */
		{
			int adj = (dir == FT_RIGHT) ?
				cds_find_next_bit(bitmap->bitmap,
					FT_ENTRY_PER_NODE, i + 1) :
				cds_find_prev_bit(bitmap->bitmap,
					FT_ENTRY_PER_NODE, i - 1);

			if (adj >= 0)
				ft_maybe_prefetch_nta(((struct cds_ft_inode_flag **)
					node->data)[adj]);
		}
		dbg_printf("ft_pigeon_node_get child_node_flag %p\n", child_node_flag);
		*result_key = (uint8_t) i;
		return child_node_flag;
	}
	return NULL;
}

static inline_lookup
struct cds_ft_inode_flag *ft_pigeon_node_get_ith_pos(const struct cds_ft_type *type,
		struct cds_ft_inode *node,
		uint8_t i)
{
	return ft_pigeon_node_get_nth(type, node, NULL, i, FT_PF_NONE);
}

/*
 * ft_node_get_nth: get nth item from a node.
 * node_flag is already rcu_dereference'd.
 */

/*
 * Compile-time bitmasks indexed by type_index: for each class,
 * the set of type_index values whose ft_types[] entry has that
 * class.  Dispatching via `(1U << type_index) & FT_MASK_X` keeps
 * the check in ALU ops only.  Each bit is derived from
 * ft_types[i].type_class directly, so an ft_types[] edit
 * (reclassification, new type) updates the masks automatically.
 */
#define FT_TC_BIT(n, tc) \
	(ft_types[(n)].type_class == (tc) ? (1U << (n)) : 0)

#if CAA_BITS_PER_LONG < 64
#define FT_MASK_CLASS(tc) \
	(FT_TC_BIT(0, tc) | FT_TC_BIT(1, tc) | FT_TC_BIT(2, tc) | \
	 FT_TC_BIT(3, tc) | FT_TC_BIT(4, tc) | FT_TC_BIT(5, tc) | \
	 FT_TC_BIT(6, tc))
#else
#define FT_MASK_CLASS(tc) \
	(FT_TC_BIT(0, tc) | FT_TC_BIT(1, tc) | FT_TC_BIT(2, tc) | \
	 FT_TC_BIT(3, tc) | FT_TC_BIT(4, tc) | FT_TC_BIT(5, tc) | \
	 FT_TC_BIT(6, tc) | FT_TC_BIT(7, tc))
#endif

#define FT_MASK_POPCOUNT    FT_MASK_CLASS(FT_POPCOUNT)

/*
 * Runtime assertions on the popcount_1l slots: both host
 * popcount_1l FT_POPCOUNT nodes.  Validated at library load so
 * any future ft_types[] regression fires before any lookup runs.
 */
static void __attribute__((constructor))
ft_check_popcount_1l_idx_assumptions(void)
{
#if (CAA_BITS_PER_LONG < 64)
	assert(ft_types[3].type_class == FT_POPCOUNT && ft_types[3].popcount_1l);
	assert(ft_types[4].type_class == FT_POPCOUNT && ft_types[4].popcount_1l);
#else
	assert(ft_types[4].type_class == FT_POPCOUNT && ft_types[4].popcount_1l);
	assert(ft_types[5].type_class == FT_POPCOUNT && ft_types[5].popcount_1l);
#endif
}

/*
 * ft_node_get_nth_skip: raw child slot access.  Returns the slot
 * value as-is, including skip-compressed pointers.  It does not resolve
 * skip pointers: a caller that needs the target resolves them itself.
 *
 * Per-type dispatch on type_index (no ft_types[] field load).  Each
 * arch maps type_index to a scanner specialized for that tier's
 * layout.
 *
 * 64-bit type-index -> scanner:
 *   0:  popcount_2l_scan_16_16_max_3 (32 B,  max_child=3)
 *   1:  popcount_2l_scan_32_8        (64 B,  max_child=6)
 *   2:  popcount_2l_scan_64_4        (128 B, max_child=14)
 *   3:  popcount_1l_scan_28          (256 B, max_child=28)
 *   4:  popcount_1l_scan_28          (512 B, max_child=60)
 *   5:  popcount_1l_scan_28         (1024 B, max_child=124)
 *   6:  pigeon                      (2048 B)
 *
 * 32-bit type-index -> scanner:
 *   0:  popcount_2l_scan_16_16_max_5 (32 B,  max_child=5)
 *   1:  popcount_2l_scan_64_4        (64 B,  max_child=12)
 *   2:  popcount_1l_scan_28          (128 B, max_child=24)
 *   3:  popcount_1l_scan_28          (256 B, max_child=56)
 *   4:  popcount_1l_scan_28          (512 B, max_child=120)
 *   5:  pigeon                      (1024 B)
 *
 * The popcount scanners are pure integer math (popcount + bitmap
 * indexing), with no SSE / SIMD dependency.  The hot caa_likely branch
 * targets the most-frequent type observed across the comprehensive
 * benchmark.  That is popcount_2l max_child=3 on 64-bit: ~46% of
 * dispatches.
 */

/*
 * FT_NODE_SUB_TAG: SUB-by-imm tag-clear.  At each dispatch case below
 * the type_index is a compile-time literal.  So the tag value to
 * subtract is a compile-time constant.  The compiler emits a single
 * SUB-by-immediate, or folds it into an LEA with displacement.
 * ft_node_ptr_internal uses a data-dependent variable-shift AND
 * instead.
 *
 * The subtraction is exact because every alignment bit covered by the
 * tag bits is zero in the raw address.  Type-T internal nodes are
 * 2^(4+T)-byte aligned.  The tag bits live in [0,3]
 * (FT_INTERNAL_MASK | FT_TYPE_MASK = 0xF).
 *
 * On FEATURE_FT_SKIP_COMPRESSED builds the skip-len high bits live
 * at [FT_SKIP_LEN_SHIFT, 63].  Mask them off with FT_ADDR_MASK so
 * the result is a canonical address suitable for memory access.
 * Builds without FEATURE_FT_SKIP_COMPRESSED skip the mask entirely.
 */
#ifdef FEATURE_FT_SKIP_COMPRESSED
# define FT_NODE_SUB_TAG(nf, type_idx)					\
	((struct cds_ft_inode *)					\
		((((unsigned long) (nf) -				\
		   (((unsigned long) (type_idx) << FT_INTERNAL_BITS) | \
		    FT_INTERNAL_MASK)) & FT_ADDR_MASK)))
#else
# define FT_NODE_SUB_TAG(nf, type_idx)					\
	((struct cds_ft_inode *)					\
		((unsigned long) (nf) -					\
		 (((unsigned long) (type_idx) << FT_INTERNAL_BITS) |	\
		  FT_INTERNAL_MASK)))
#endif

/*
 * FT_NODE_SUB_TAG_NOSKIP: SUB-by-imm tag-clear variant *without* the
 * FT_ADDR_MASK (skip_len high-bit clear).  Safe only at descent sites
 * where the caller has already established skip_len = 0.  One way is the
 * loop-top ft_node_skip_compressed handler: it either skips the entry or
 * resolves the skip through ft_skip_child_ptr.  The pure SUB folds
 * into the immediately-following load's displacement.  This avoids the
 * movabs of the 64-bit FT_ADDR_MASK constant in the hot path.
 */
#define FT_NODE_SUB_TAG_NOSKIP(nf, type_idx)				\
	((struct cds_ft_inode *)					\
		((unsigned long) (nf) -					\
		 (((unsigned long) (type_idx) << FT_INTERNAL_BITS) |	\
		  FT_INTERNAL_MASK)))

/*
 * The type index a tagged pointer carries when it names no node type:
 * NODE_INDEX_NULL, which no real node is ever allocated with.
 *
 * A parked flip PROXY is that, on purpose.  It is encoded as a synthetic
 * internal node of the maximal type index (ft-helpers.h).  So every caller
 * that reaches a descent step still holding an unresolved proxy dispatches
 * to here.
 *
 * This arm used to be __builtin_unreachable(), on the premise that such an
 * index is "never encoded in a tagged pointer".  The proxy encoding makes
 * that premise false.  Being wrong about it cost the worst failure shape
 * available.  The compiler is entitled to index past the end of the jump
 * table.  So an unresolved proxy became a wild jump to a non-executable
 * address.  It raised SIGILL with no diagnostic.  The stack held nothing
 * but a return address in the caller.  That is how the free-walk defect
 * presented.  It is also why it took a bisect and a hand-rolled probe to
 * place it, rather than one assert.
 *
 * Answer "no child" instead.  Every caller already handles NULL:
 * - an external or compressed parent returns NULL from the tag test above;
 * - the reader descent turns NULL into CDS_FT_STATUS_NOT_FOUND.
 * So a caller bug degrades to a miss it can cope with, rather than to
 * undefined control flow.  The debug build names the culprit first, with
 * the frame still intact.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_node_dispatch_untyped(
		struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag ***node_flag_ptr)
{
	ft_assert_resolved(node_flag);
	if (caa_unlikely(node_flag_ptr))
		*node_flag_ptr = NULL;
	return NULL;
}

static inline_lookup
struct cds_ft_inode_flag *ft_node_get_nth_skip(struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	unsigned long tag = (unsigned long) node_flag & 0xF;
	unsigned int type_index;

	/* External / compressed: internal flag clear. */
	if (caa_unlikely(!(tag & FT_INTERNAL_MASK))) {
		if (caa_unlikely(node_flag_ptr))
			*node_flag_ptr = NULL;
		return NULL;
	}

	type_index = (tag >> FT_INTERNAL_BITS) & 0x7;

#if CAA_BITS_PER_LONG >= 64
	/*
	 * Pure switch; expect gcc to emit a jump table for 0..6.  Each
	 * case computes FT_NODE_SUB_TAG with a compile-time-literal type.
	 * So the SUB is by-immediate and has no data dependency on
	 * @node_flag's type bits.
	 */
	switch (type_index) {
	case 0:
		return ft_popcount_2l_scan_16_16_max_3(
			FT_NODE_SUB_TAG(node_flag, 0), node_flag_ptr, n, pf_hint);
	case 1:
		return ft_popcount_2l_scan_32_8(
			FT_NODE_SUB_TAG(node_flag, 1), node_flag_ptr, n, pf_hint);
	case 2:
		return ft_popcount_2l_scan_64_4(
			FT_NODE_SUB_TAG(node_flag, 2), node_flag_ptr, n, pf_hint);
	case 3:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG(node_flag, 3), node_flag_ptr, n, pf_hint);
	case 4:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG(node_flag, 4), node_flag_ptr, n, pf_hint);
	case 5:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG(node_flag, 5), node_flag_ptr, n, pf_hint);
	case 6:
		return ft_pigeon_node_get_nth(NULL,
			FT_NODE_SUB_TAG(node_flag, 6), node_flag_ptr, n, pf_hint);
	default:
		/* NODE_INDEX_NULL: no node type. See ft_node_dispatch_untyped. */
		return ft_node_dispatch_untyped(node_flag, node_flag_ptr);
	}
#else
	switch (type_index) {
	case 0:
		return ft_popcount_2l_scan_16_16_max_5(
			FT_NODE_SUB_TAG(node_flag, 0), node_flag_ptr, n, pf_hint);
	case 1:
		return ft_popcount_2l_scan_64_4(
			FT_NODE_SUB_TAG(node_flag, 1), node_flag_ptr, n, pf_hint);
	case 2:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG(node_flag, 2), node_flag_ptr, n, pf_hint);
	case 3:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG(node_flag, 3), node_flag_ptr, n, pf_hint);
	case 4:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG(node_flag, 4), node_flag_ptr, n, pf_hint);
	case 5:
		return ft_pigeon_node_get_nth(NULL,
			FT_NODE_SUB_TAG(node_flag, 5), node_flag_ptr, n, pf_hint);
	default:
		/* NODE_INDEX_NULL: no node type. See ft_node_dispatch_untyped. */
		return ft_node_dispatch_untyped(node_flag, node_flag_ptr);
	}
#endif
}

/*
 * ft_node_get_nth_skip_pretyped: same dispatch as ft_node_get_nth_skip,
 * but the caller has already extracted @type_index from the tag bits
 * and verified the FT_INTERNAL_MASK bit was set.  The parent pointer
 * @node_flag is still tagged.  Each switch case applies
 * FT_NODE_SUB_TAG_NOSKIP with a compile-time-literal type.  So the
 * SUB-by-immediate folds into the immediately-following body load's
 * displacement (gcc emits e.g. `mov -5(%rcx), %rax`).  This avoids an
 * explicit AND step on the per-step dependency chain to the body load.
 *
 * Skip-compressed safety.  At this call site the loop-top
 * ft_node_skip_compressed handler has already run.  It either skipped
 * this entry or resolved the skip via ft_skip_child_ptr.  That function
 * masks FT_ADDR_MASK internally.  So @node_flag's high-bit skip_len
 * field is zero, and the FT_NODE_SUB_TAG_NOSKIP form is safe.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_node_get_nth_skip_pretyped(
		struct cds_ft_inode_flag *node_flag,
		unsigned int type_index,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
#if CAA_BITS_PER_LONG >= 64
	switch (type_index) {
	case 0:
		return ft_popcount_2l_scan_16_16_max_3(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 0),
			node_flag_ptr, n, pf_hint);
	case 1:
		return ft_popcount_2l_scan_32_8(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 1),
			node_flag_ptr, n, pf_hint);
	case 2:
		return ft_popcount_2l_scan_64_4(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 2),
			node_flag_ptr, n, pf_hint);
	case 3:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 3),
			node_flag_ptr, n, pf_hint);
	case 4:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 4),
			node_flag_ptr, n, pf_hint);
	case 5:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 5),
			node_flag_ptr, n, pf_hint);
	case 6:
		return ft_pigeon_node_get_nth(NULL,
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 6),
			node_flag_ptr, n, pf_hint);
	default:
		/* NODE_INDEX_NULL: no node type. See ft_node_dispatch_untyped. */
		return ft_node_dispatch_untyped(node_flag, node_flag_ptr);
	}
#else
	switch (type_index) {
	case 0:
		return ft_popcount_2l_scan_16_16_max_5(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 0),
			node_flag_ptr, n, pf_hint);
	case 1:
		return ft_popcount_2l_scan_64_4(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 1),
			node_flag_ptr, n, pf_hint);
	case 2:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 2),
			node_flag_ptr, n, pf_hint);
	case 3:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 3),
			node_flag_ptr, n, pf_hint);
	case 4:
		return ft_popcount_1l_scan_28(
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 4),
			node_flag_ptr, n, pf_hint);
	case 5:
		return ft_pigeon_node_get_nth(NULL,
			FT_NODE_SUB_TAG_NOSKIP(node_flag, 5),
			node_flag_ptr, n, pf_hint);
	default:
		/* NODE_INDEX_NULL: no node type. See ft_node_dispatch_untyped. */
		return ft_node_dispatch_untyped(node_flag, node_flag_ptr);
	}
#endif
}

/*
 * ft_node_get_nth: child slot access with skip-compressed resolution
 * for writer / debug callers.
 *
 * Resolves any skip pointer via the slot value directly (no validation,
 * no concurrency handling).  All RCU reader paths use
 * ft_node_get_nth_reanchor instead.  That one validates the skip pointer
 * against the live compressed node.  On a mismatch it re-anchors via the
 * child's parent chain (the single concurrency mechanism -- see
 * ft_skip_reanchor).  The historical validate=true spin loop here is
 * therefore gone; the @validate_lookup parameter is dropped.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_node_get_nth(const struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	struct cds_ft_inode_flag *child =
		ft_node_get_nth_skip(node_flag, node_flag_ptr, n, pf_hint);

	/*
	 * Resolve a type-7 flip proxy transiently occupying the slot BEFORE
	 * the skip handler classifies the child.  (Phase 4.3: the proxy is a
	 * peer's one-commit publish or merge flip in its install-to-settle
	 * window.)  An unresolved proxy's low nibble reads as internal type
	 * 7.  So the descent would dereference the proxy latch as node memory
	 * on its next step (a SIGILL / SIGSEGV garbage jump in
	 * ft_descent_step).  Predicted-not-taken when no flip is in flight
	 * (single-writer unchanged).  The resolved value may itself be
	 * skip-encoded, hence resolve-then-skip order.  Mirrors
	 * ft_node_get_nth_reanchor's leading resolve.
	 */
	child = ft_resolve_flip_proxy(child);
	return ft_resolve_skip_compressed(ft, child);
}

/*
 * ft_node_get_nth_reanchor_slot: the shared robust child-resolution step for
 * BOTH the read side and the MW update side.  Fetch @node_flag's child at
 * byte @n.  Resolve a skip-compressed slot via the live skip-child parent
 * chain (ft_skip_reanchor), not via the unvalidated one-hop back-pointer.
 * That chain is the single concurrency-handling mechanism.
 *
 * Under mutual exclusion the update side could descend with the simpler
 * one-hop resolve (a peer writer could not change the structure under its own
 * descent).  MW removes that exemption.  The writer descent now faces the
 * same concurrent split/merge the read side always did.  So it navigates
 * through THIS primitive instead of a bespoke writer resolver.  The update
 * side differs from a reader only in capturing the mutable publish slot via
 * @slot_ret.
 *
 * @raw_ret:    out (may be NULL) -- the raw word that slot held at THIS load,
 *              unresolved.  Callers comparing against slot contents (an
 *              expected-old) need it; the resolved return value never equals a
 *              skip-encoded word.
 * @slot_ret:   out (may be NULL) -- the RAW slot the child sits in.  Under
 *              skip-compression it is the skip slot itself, unresolved.  It
 *              is the update side's publish target, the value
 *              ft_split_compressed_insert's forward CAS matches.  Readers
 *              pass NULL.
 * @rewind_ret: out -- byte-depths the resolved node lies ABOVE the dispatched
 *              child.  It is 0 in the split / recompaction case.  It is > 0
 *              only when a concurrent chain-merge moved the encoded position
 *              shallower.  A reader then backs its descent cursor up by that
 *              much.  The update side's @slot_ret is then at the wrong level,
 *              so a mutator re-descends.
 *
 * Return the resolved child flag, or NULL for an empty slot.  The resolved
 * child is a LIVE compressed/internal/external node.  It is never
 * skip-encoded, and never a torn back-pointer's content.  ft_skip_reanchor
 * never returns NULL on a well-formed trie.  The writer wires every fresh
 * cluster's parent before the cluster becomes reachable.  So the up-walk
 * never observes a NULL parent.  The result is asserted non-NULL.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_node_get_nth_reanchor_slot(struct cds_ft *ft,
		struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag ***slot_ret,
		struct cds_ft_inode_flag **raw_ret,
		uint8_t n, enum ft_pf_target pf_hint,
		unsigned int *rewind_ret)
{
	struct cds_ft_inode_flag *child =
		ft_node_get_nth_skip(node_flag, slot_ret, n, pf_hint);

	/*
	 * The UNRESOLVED word, from this one load -- coherent with the resolved
	 * value returned below, which a second read of the slot would not be.
	 */
	if (raw_ret)
		*raw_ret = child;

	/*
	 * Resolve a type-7 flip proxy transiently occupying the slot BEFORE
	 * the skip handler classifies the child.  The proxy is a
	 * cds_ft_merge_at flip, or an ordered-list insert's one-commit
	 * publish.  An unresolved proxy's tag bits read as internal type 7
	 * and would dereference the proxy latch as node memory.
	 * Predicted-not-taken when no flip is in flight.  The resolved value
	 * may itself be skip-encoded, hence resolve-then-skip order.
	 */
	child = ft_resolve_flip_proxy(child);
	return ft_reanchor_flag(ft, child, rewind_ret);
}

/*
 * Surgical reader descent step: fetch @node_flag's child at byte @n.  Resolve
 * a skip-compressed slot mismatch via the live skip-child parent chain instead
 * of spinning on a (possibly frozen) slot like ft_node_get_nth's old validate
 * path.  It is a thin reader wrapper over ft_node_get_nth_reanchor_slot, with
 * no publish slot.
 * On a mismatch ft_skip_reanchor locates the live position.  *@rewind_ret is
 * how many byte-depths the resolved node lies ABOVE the dispatched child.  It
 * is 0 in the common split / recompaction case.  It is > 0 only when a
 * concurrent chain-merge moved the encoded position shallower.  The caller
 * then backs its descent cursor up by that much, or re-descends.
 * ft_skip_reanchor never returns NULL on a well-formed trie; the result is
 * asserted non-NULL.  Return the resolved child flag
 * (compressed/internal/external), or NULL for an empty slot.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_node_get_nth_reanchor(struct cds_ft *ft,
		struct cds_ft_inode_flag *node_flag, uint8_t n,
		unsigned int *rewind_ret)
{
	return ft_node_get_nth_reanchor_slot(ft, node_flag, NULL, NULL, n,
			FT_PF_NONE, rewind_ret);
}

/*
 * ft_node_find_child: reverse lookup -- given a parent internal node and
 * a child pointer, find the key byte and slot that lead to that child.
 *
 * Returns true if found, with *n_ret set to the key byte and *slot_ret
 * set to a pointer to the slot (cds_ft_inode_flag **) within the parent.
 * Returns false if the child is not found (should not happen on a
 * well-formed trie).
 *
 * Only handles internal node types (popcount, pigeon).
 * Compressed parents are handled separately by callers.
 * Write-side only (mutex-held).
 */
static
bool ft_node_find_child(struct cds_ft *ft, struct cds_ft_inode_flag *parent_nf,
		struct cds_ft_inode_flag *child_nf,
		uint8_t *n_ret,
		struct cds_ft_inode_flag ***slot_ret)
{
	struct cds_ft_inode *node = ft_node_ptr(parent_nf);
	unsigned int type_index = ft_node_type(parent_nf);
	const struct cds_ft_type *type = &ft_types[type_index];

	switch (type->type_class) {
	case FT_POPCOUNT:
	{
		uint8_t nr_child = ft_popcount_node_get_nr_child(type, node);
		unsigned int i;

		for (i = 0; i < nr_child; i++) {
			struct cds_ft_inode_flag *iter;
			uint8_t v;

			ft_popcount_node_get_ith_pos(type, node, i, &v, &iter);
			if (iter == child_nf) {
				if (n_ret)
					*n_ret = v;
				if (slot_ret)
					ft_node_get_nth(ft, parent_nf, slot_ret, v, FT_PF_NONE);
				return true;
			}
		}
		return false;
	}
	case FT_PIGEON:
	{
		unsigned int i;

		for (i = 0; i < FT_ENTRY_PER_NODE; i++) {
			struct cds_ft_inode_flag *iter;

			iter = ft_pigeon_node_get_ith_pos(type, node, i);
			if (iter == child_nf) {
				if (n_ret)
					*n_ret = (uint8_t) i;
				if (slot_ret)
					ft_node_get_nth(ft, parent_nf, slot_ret, i, FT_PF_NONE);
				return true;
			}
		}
		return false;
	}
	default:
		assert(0);
		return false;
	}
}

/*
 * @validate_lookup: when true, validate the skip-compressed resolution
 * against the slot's skip_len.  On mismatch the RAW (still skip-compressed)
 * pointer is returned as a re-anchor signal.  The reader-level caller resolves
 * it via ft_skip_reanchor.  A validated result is never skip-compressed, so
 * the signal is unambiguous.  RCU readers must pass true and MUST handle the
 * skip-compressed return.  Writers must pass false and get no validation.  A
 * writer reads its own in-flight state.  It is the race partner, not a victim
 * of it.  With static inlining and a constant arg, the unused branch is DCE'd
 * at each call site.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_node_get_direction(struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		int n, uint8_t *result_key,
		enum ft_direction dir, bool validate_lookup)
{
	unsigned int type_index;
	struct cds_ft_inode *node;
	const struct cds_ft_type *type;
	struct cds_ft_inode_flag *child;

	/*
	 * Compressed node: no branching at any level within the
	 * compressed path.  Return NULL to indicate no siblings,
	 * causing the going-up walk to continue ascending.
	 */
	if (ft_node_compressed(node_flag))
		return NULL;
	node = ft_node_ptr(node_flag);
	assert(node != NULL);
	type_index = ft_node_type(node_flag);
	type = &ft_types[type_index];

	for (;;) {
		switch (type->type_class) {
		case FT_POPCOUNT:
			child = ft_popcount_node_get_direction(type, node, n, result_key, dir);
			break;
		case FT_PIGEON:
			child = ft_pigeon_node_get_direction(type, node, n, result_key, dir);
			break;
		default:
			assert(0);
			return (void *) -1UL;
		}
		/*
		 * Resolve a type-7 flip proxy transiently occupying a slot (a
		 * cds_ft_merge_at flip, or a one-commit insert's parked
		 * publish).  Every ordered traversal reaching a directional
		 * child through this accessor then sees the view-appropriate
		 * old-or-new child.  Those traversals are iteration via
		 * ft_node_get_leftright / ft_node_get_minmax and the
		 * relational lookups.
		 * Predicted-not-taken when no flip is in flight.  NULL and
		 * skip-encoded children pass through unchanged (a skip child's
		 * low nibble is its tag, never the 0xF proxy).
		 *
		 * A parked one-commit insert proxy resolves to NULL (fresh key,
		 * empty old slot).  That slot is INVISIBLE, not end-of-scan.
		 * Continue the directional scan past it.  Otherwise a rank
		 * walk (lookup_nth / nth_last) or going-up sibling scan would
		 * silently drop every child beyond the parked slot.
		 */
		if (caa_unlikely(ft_node_flip_proxy(child))) {
			child = ft_resolve_flip_proxy(child);
			if (!child) {
				/*
				 * Skip the parked slot: directional scans pivot
				 * past it; a minmax entry continues INWARD from
				 * it as a pivoted scan.
				 */
				if (dir == FT_LEFTMOST)
					dir = FT_RIGHT;
				else if (dir == FT_RIGHTMOST)
					dir = FT_LEFT;
				n = *result_key;
				continue;
			}
		}
		break;
	}
	if (!validate_lookup)
		return ft_resolve_skip_compressed(ft, child);
	/*
	 * Precise (validating) lookup: return a skip-compressed child RAW as a
	 * re-anchor signal.  It is still skip-compressed, which a resolved
	 * result never is, so the reader-level caller detects it and resolves
	 * via ft_skip_reanchor (the single skip concurrency mechanism).  We must
	 * NOT resolve it here by assuming the skip child's back-pointer is a
	 * compressed node.  Mid-split, it can transiently be an INTERNAL
	 * node.  An example is a suffix_len==0 branch re-parenting the old
	 * external child before the slot is republished.  Reading an
	 * INTERNAL node as a compressed len/key_bytes yields a garbage key.
	 */
	return child;
}

static inline_lookup
struct cds_ft_inode_flag *ft_node_get_leftright(struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		unsigned int n, uint8_t *result_key,
		enum ft_direction dir, bool validate_lookup)
{
	return ft_node_get_direction(ft, node_flag, n, result_key, dir, validate_lookup);
}

static inline_lookup
struct cds_ft_inode_flag *ft_node_get_minmax(struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		uint8_t *result_key,
		enum ft_direction dir, bool validate_lookup)
{
	switch (dir) {
	case FT_LEFTMOST:
		return ft_node_get_direction(ft, node_flag,
				-1, result_key, FT_RIGHT, validate_lookup);
	case FT_RIGHTMOST:
		return ft_node_get_direction(ft, node_flag,
				FT_ENTRY_PER_NODE, result_key, FT_LEFT, validate_lookup);
	default:
		assert(0);
		return NULL;
	}
}

/*
 * The read path (point lookups and the next / prev / min / max descent)
 * force-inlines the node scanners.  Several are also called at many write-side
 * sites, where the same force-inline duplicated each scanner across the mutation
 * code.  Provide a single shared, non-inlined copy of each such scanner for the
 * mutation path to call.  Read-side callers keep calling the always_inline
 * originals.  The mutation modules are redirected to these *_shared variants via
 * #define in fractal-trie.c.
 *
 * Only these five pay off, about -9% .text together:
 * - get_nth;
 * - get_nth_skip;
 * - get_nth_reanchor;
 * - get_direction;
 * - get_minmax.
 * An A/B over every node scanner showed why the rest do not.  Some are too
 * small for sharing to beat inlining.  The others are reached only from the
 * read path.
 */
static struct cds_ft_inode_flag *ft_node_get_nth_shared(
		const struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	return ft_node_get_nth(ft, node_flag, node_flag_ptr, n, pf_hint);
}

static struct cds_ft_inode_flag *ft_node_get_nth_skip_shared(
		struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag ***node_flag_ptr,
		uint8_t n, enum ft_pf_target pf_hint)
{
	return ft_node_get_nth_skip(node_flag, node_flag_ptr, n, pf_hint);
}

static struct cds_ft_inode_flag *ft_node_get_nth_reanchor_shared(
		struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		uint8_t n, unsigned int *rewind_ret)
{
	return ft_node_get_nth_reanchor(ft, node_flag, n, rewind_ret);
}

static struct cds_ft_inode_flag *ft_node_get_direction_shared(
		struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		int n, uint8_t *result_key,
		enum ft_direction dir, bool validate_lookup)
{
	return ft_node_get_direction(ft, node_flag, n, result_key, dir,
			validate_lookup);
}

static struct cds_ft_inode_flag *ft_node_get_minmax_shared(
		struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		uint8_t *result_key,
		enum ft_direction dir, bool validate_lookup)
{
	return ft_node_get_minmax(ft, node_flag, result_key, dir, validate_lookup);
}
