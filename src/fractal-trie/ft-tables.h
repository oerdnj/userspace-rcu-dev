// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-tables.h
 *
 * Userspace RCU library - Fractal Trie: the node-size tier tables
 * (ft_types[]) and the tier / pointer-tag layout rationale.  A single-TU
 * data definition, so it is a #included implementation unit rather than a
 * multi-TU header.  Not standalone.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-tables.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * Node-size tiers (the ft_types[] arrays below) and the tag budget that
 * bounds them.
 *
 * Each node carries a type index naming its tier.  A tier defines:
 *   - the [min_child, max_child] child-count range it serves;
 *   - its type class;
 *   - its size order (node size == 1 << order bytes).
 *
 * Picking a tier on add/remove is NOT a search of the array.
 * find_nearest_type_index() STEPS from the node's current tier to the
 * adjacent one:
 *   - up one tier when a child add exceeds max_child;
 *   - down one tier when a remove drops below min_child.
 * So the common +/- 1-child change moves at most one tier.  It loops further
 * only when the child count jumps by more.
 *
 * Each tier's min_child overlaps the previous tier's max_child.  This is an
 * intentional hysteresis: it damps reallocation under cyclic add/remove
 * within a node.  The top tier has max_child == 256 (a full node).  The
 * bottom tier (index 0) holds the smallest nodes.  These include the 0-child
 * root that is kept alive.
 *
 * The type index is carried in 3 bits of the node pointer's low tag bits.  So
 * it has 8 possible values: the real tiers plus NODE_INDEX_NULL (the absent /
 * pruned-node sentinel).  The assignment is:
 *   - on 64-bit: indices 0..6 real + 7 == NULL;
 *   - on 32-bit: indices 0..5 real + 6 == NULL.
 *
 * The full pointer-tag scheme is below.  The canonical masks are
 * FT_INTERNAL_MASK and FT_COMPRESSED_MASK, in fractal-trie-internal.h.
 *
 *   bit0 = 0, bit1 = 0   external (leaf) pointer, or NULL
 *   bit0 = 0, bit1 = 1   compressed (path) node
 *   bit0 = 1             internal node; bits 1-3 carry the 3-bit type index
 *
 * Internal nodes spend the most tag bits: 4 low bits.  That is 1 bit for the
 * internal/external flag plus 3 bits for the type index.  So every node must
 * be 16-byte aligned to keep those bits free for the tag.  That is why the
 * smallest usable allocation order is 4 (node size 1 << 4 == 16 bytes).
 * Compressed and external pointers fit within the same 4-bit budget: bit 1
 * set for compressed, 0b00 for external.
 */

/*
 * The popcount scanners hardcode the pointer offset that matches
 * their header layout:
 *   - scan_16_16_max_3 at +8;
 *   - scan_16_16_max_5 at +12;
 *   - scan_32_8 / scan_64_4 at +16;
 *   - popcount_1l at +32.
 * They access pointers via sizeof(void *) stride.  Both offsets and
 * strides remain valid on 32-bit pointers, so the scanners
 * themselves are pointer-size agnostic.  What differs is per-node
 * capacity.  32-bit pointers leave trailing space.  Large bp_1l
 * nodes can fit substantially more children for the same node size
 * (the 32 B bitmap header is fixed).  The 32-bit ft_types[] below
 * tunes max_child accordingly.
 *
 * Two-level popcount bitmap.  A node's child-presence bitmap says which of
 * the up-to-256 child slots are occupied.  It is stored two-level: a ROOT
 * bitmap (root_bm) sits over a row of SUB-bitmaps (sub_bm).  This way,
 * mapping an occupied slot to its dense child-pointer index costs a couple
 * of popcounts rather than a scan.  Each sub_bm holds the presence bits for
 * one contiguous block of slots.  The corresponding root_bm bit is set iff
 * that sub_bm has any child.  A slot's index is then popcount(sub_bm below
 * the slot) plus the children in all lower non-empty sub-bitmaps.  The
 * root_bm summarizes which to add.
 *
 * Layout naming: scan_<root_bits>_<sub_bm_bits>(_max_<N>).  For example,
 * scan_16_16 is a 16-bit root_bm over 16-bit sub_bms (16 x 16 = 256 slots).
 * The trailing _max_<N> appears only on the generic-2L variants
 * (scan_16_16_*).  There, the ptr offset depends on the static sub_bm[]
 * tail length (max_child).
 *
 * The flat-layout scanners split the 8-bit slot index (a byte) in two.  A
 * high field selects the root_bm entry.  A low field selects the bit within
 * the sub_bm.  A "5+3 byte-split" is 5 high bits + 3 low bits, etc.
 *   - scan_32_8 uses a 5+3 byte-split with u8 nibbles;
 *   - scan_64_4 uses a 6+2 byte-split with u4 nibbles.
 * Both pack root_bm + packed_bms into a fixed 12 B or 16 B header.  So the
 * ptr offset is constant (+16 after alignment), and one function serves
 * multiple max_child values.
 *
 * 32-bit max_child tuning.  The flat scanners (scan_32_8, scan_64_4) are
 * layout-only.  Their headers do not grow with max_child.  packed_bms
 * supports up to 64 distinct (hi, lo) pairs.  On 32-bit ptrs, scan_64_4 hosts
 * max_child=12 in 64 B and max_child=16 in 128 B.  The latter is
 * bitmap-bound; its trailing 48 B is layout slack.  The layout helpers
 * accept all canonical max_child values.  These helpers are header_bytes,
 * get_nr_child, get_ith_pos and set_nth.  The dispatcher routes by
 * max_child.
 *
 * scan_16_16_max_3 stays at max_child=3 on both arches.  Its 8 B header
 * packs root_bm + 3 sub_bm exactly and cannot grow without changing
 * the load shape.  scan_16_16_max_5 (32-bit only) extends the same
 * generic 2L layout with two extra sub_bm slots.  The 12 B header
 * + 5 x 4 B ptrs then fills the 32 B order-5 node exactly.
 */

#if (CAA_BITS_PER_LONG < 64)

/*
 * 32-bit pointers.  Max-density popcount tiers (max_child inlined into
 * ft_types[] below):
 *   idx 0  scan_16_16_max_5  32 B  hdr 12 B +  5 x 4 B = 32 B exact
 *   idx 1  scan_64_4         64 B  hdr 16 B + 12 x 4 B = 64 B exact
 *                            (flat 6+2; node-size cap 12, bitmap cap 16)
 *   idx 2  popcount_1l      128 B  hdr 32 B + 24 x 4 B = 128 B exact
 *   idx 3  popcount_1l      256 B  hdr 32 B + 56 x 4 B = 256 B exact
 *   idx 4  popcount_1l      512 B  hdr 32 B + 120 x 4 B = 512 B exact
 *   idx 5  pigeon          1024 B  256 x 4 B pointers
 */
static const struct cds_ft_type ft_types[] = {
	[0] = {
		.type_class = FT_POPCOUNT,
		.popcount_2l = true,
		.min_child = 1,
		.max_child = 5, .order = 5, .bitmap = FT_NO_BITMAP,
	},
	[1] = {
		.type_class = FT_POPCOUNT,
		.popcount_2l = true,
		.min_child = 3,
		.max_child = 12, .order = 6, .bitmap = FT_NO_BITMAP,
	},
	[2] = {
		/* 32 B bitmap + 24 x 4 B ptrs = 128 B (order-7). */
		.type_class = FT_POPCOUNT,
		.popcount_1l = true,
		.min_child = 7,
		.max_child = 24, .order = 7, .bitmap = FT_NO_BITMAP,
	},

	/*
	 * Indices 2, 3, and 4 use popcount_1l (32 B bitmap + 4 B ptr
	 * table fills the node exactly).
	 */
	[3] = {
		/* 32 B bitmap + 56 * 4 B ptrs = 256 B (order-8). */
		.type_class = FT_POPCOUNT,
		.popcount_1l = true,
		.min_child = 10,
		.max_child = 56, .order = 8, .bitmap = FT_NO_BITMAP },
	[4] = {
		/* 32 B bitmap + 120 * 4 B ptrs = 512 B (order-9). */
		.type_class = FT_POPCOUNT,
		.popcount_1l = true,
		.min_child = 28,
		.bitmap = FT_NO_BITMAP,
		.max_child = 120, .order = 9 },

	/*
	 * Pigeon shrinks to the popcount_1l type directly below it once a
	 * removal brings it down to min_child.  The shrink target always has
	 * room, regardless of key distribution, for two reasons:
	 *   - that target's max_child is >= pigeon's min_child;
	 *   - popcount_1l capacity is purely count-bound.  Its 256-bit bitmap
	 *     addresses any byte value, and the pointer table is sized to
	 *     max_child.
	 * So the recompaction cannot fail for capacity and never rolls back
	 * to pigeon.
	 */
	[5] = { .type_class = FT_PIGEON,
		.min_child = 51,
		.max_child = 256, .order = 10, .bitmap = FT_BITMAP },

	[6] = { .type_class = FT_NULL, .min_child = 0, .max_child = 0 /* NULL */, .bitmap = FT_NO_BITMAP },
	/*
	 * Slot 7 padding.  FT_TYPE_BITS = 3, so the tag-encodable type_index
	 * range is 0..7.  The 32-bit tier uses only indices 0..6 (real
	 * types + FT_NULL).  Slot 7 must exist so that ft_types[] covers
	 * every tag value the encoder can produce.
	 */
	[7] = { .type_class = FT_NULL, .min_child = 0, .max_child = 0, .bitmap = FT_NO_BITMAP },
};
#else /* !(CAA_BITS_PER_LONG < 64) */
/* 64-bit pointers.  Max-child caps are inlined into ft_types[] below. */

/*
 * scan_32_8 (per-slot 5+3 byte-split, max_child=6) puts the following
 * into the 64B order-6 node:
 *   - a 12-byte popcount header (4B root + 8B packed sub_bms);
 *   - 6 x 8-byte pointers.
 *
 * scan_64_4 (flat 6+2 byte-split, max_child=14) puts the following
 * into the 128B order-7 node (16 + 14 * 8 = 128 exactly):
 *   - a 16-byte popcount header: 8B root + 8B packed_bms.  The
 *     packed_bms hold 14 x 4-bit sub_bms = 56 bits used;
 *   - 14 x 8-byte pointers.
 */

static const struct cds_ft_type ft_types[] = {
	[0] = {
		.type_class = FT_POPCOUNT,
		.popcount_2l = true,
		.min_child = 1, .max_child = 3, .order = 5, .bitmap = FT_NO_BITMAP,
	},
	[1] = {	/* scan_32_8 (per-slot 5+3) */
		.type_class = FT_POPCOUNT,
		.popcount_2l = true,
		.min_child = 3, .max_child = 6, .order = 6, .bitmap = FT_NO_BITMAP,
	},
	[2] = {	/* scan_64_4 (flat 6+2) */
		.type_class = FT_POPCOUNT,
		.popcount_2l = true,
		.min_child = 5, .max_child = 14, .order = 7, .bitmap = FT_NO_BITMAP,
	},
	[3] = {
		.type_class = FT_POPCOUNT,
		.popcount_1l = true,
		.min_child = 10, .max_child = 28, .order = 8, .bitmap = FT_NO_BITMAP,
	},

	/*
	 * Indices 4 and 5 use popcount_1l (32B bitmap + 8B ptr
	 * table fills the node).
	 */
	[4] = {
		/* 32B bitmap + 60 * 8B ptrs = 512B order-9. */
		.type_class = FT_POPCOUNT,
		.popcount_1l = true,
		.min_child = 22,
		.max_child = 60,
		.order = 9, .bitmap = FT_NO_BITMAP },
	[5] = {
		/* 32B bitmap + 124 * 8B ptrs = 1024B order-10. */
		.type_class = FT_POPCOUNT,
		.popcount_1l = true,
		.min_child = 51,
		.max_child = 124,
		.order = 10, .bitmap = FT_NO_BITMAP },

	/*
	 * Pigeon shrinks to the popcount_1l type directly below it once a
	 * removal brings it down to min_child.  The shrink target always has
	 * room, regardless of key distribution, for two reasons:
	 *   - that target's max_child is >= pigeon's min_child;
	 *   - popcount_1l capacity is purely count-bound.  Its 256-bit bitmap
	 *     addresses any byte value, and the pointer table is sized to
	 *     max_child.
	 * So the recompaction cannot fail for capacity and never rolls back
	 * to pigeon.
	 */
	[6] = { .type_class = FT_PIGEON, .min_child = 95, .max_child = 256, .order = 11, .bitmap = FT_BITMAP },

	[7] = { .type_class = FT_NULL, .min_child = 0, .max_child = 256, .bitmap = FT_NO_BITMAP },
};
#endif /* !(BITS_PER_LONG < 64) */
