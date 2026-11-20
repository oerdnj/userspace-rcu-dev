// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-trace-helpers.h
 *
 * Userspace RCU library - Fractal Trie: LTTng tracepoint argument helpers
 * (compiled only under FT_ENABLE_TRACING).  Map a tagged cds_ft_inode_flag
 * pointer to the symbolic node-kind / skip-length values the tracepoints in
 * cds_ft_tp.h record.  The helpers are exported (non-static).  The
 * tracepoint-provider unit cds_ft_tp.c calls them through the declarations
 * in cds_ft_tp.h.  There are no in-translation-unit callers.  Compiles to
 * nothing when tracing is off.
 *
 * Implementation unit: #included once into the fractal-trie.c translation
 * unit, after ft-helpers.h (whose node accessors it uses).  Not standalone.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-trace-helpers.h is an implementation unit; #include it from fractal-trie.c only"
#endif

#ifdef FT_ENABLE_TRACING
/*
 * Map a tagged cds_ft_inode_flag pointer to a symbolic node-kind
 * value (enum ft_tp_node_kind, defined in fractal-trie-internal.h
 * and exposed as the LTTng enum ft_tp_node_kind in
 * src/fractal-trie/cds_ft_tp.h).
 * Uses a single 16-entry compile-time dispatch table.  The
 * type-selecting bits of the pointer index it.  So the runtime helper
 * reduces to a NULL/skip check plus one table load.
 */

/*
 * Single dispatch table indexed by the low 4 bits of a tagged
 * cds_ft_inode_flag pointer -- exactly the bits that select the node
 * type:
 *   - bit 0      = FT_INTERNAL_MASK (1 = internal node)
 *   - bits 1..3  = type index (when internal) or class selector
 *                  (when not: 00=external, 01=compressed)
 *
 * Compressed nodes are 16-byte aligned, so bit 3 is guaranteed zero
 * for them.  External nodes need only 8-byte alignment (low 3 bits
 * = 000), so bit 3 may be either value.  Both [0b0000] and [0b1000]
 * map to EXTERNAL.
 *
 * Skip-compressed pointers are special-cased before the table lookup.
 * This is the only exception.  The table itself is a pure pointer-bits
 * dispatch.
 *
 * Slot value 0 (FT_TP_NODE_NULL) doubles as a "no entry" sentinel
 * that resolves to FT_TP_NODE_UNKNOWN.  NULL pointers are caught by
 * the explicit nf != NULL check before any table access.
 */
#define FT_TP_KIND_TABLE_MASK	0xFU

/* Pointer-bits encoding for internal-node type index `idx` (0..7). */
#define FT_TP_INTERNAL_TAG(idx)	\
	(((unsigned int) (idx) << FT_INTERNAL_BITS) | FT_INTERNAL_MASK)

/*
 * Compile-time pickers that map a single ft_types[] entry's sizing
 * parameters to an FT_TP_NODE_* constant.  All arguments are integer
 * constant expressions (sizing enums and order constants), so each
 * conditional collapses to one constant during compilation.
 */
#define FT_TP_KIND_P2L(ord) (					\
	(ord) == 5 ? FT_TP_NODE_P2L_32 :			\
	(ord) == 6 ? FT_TP_NODE_P2L_64 :			\
	(ord) == 7 ? FT_TP_NODE_P2L_128 :			\
	FT_TP_NODE_UNKNOWN)
#define FT_TP_KIND_P1L(ord) (					\
	(ord) == 7  ? FT_TP_NODE_P1L_128 :			\
	(ord) == 8  ? FT_TP_NODE_P1L_256 :			\
	(ord) == 9  ? FT_TP_NODE_P1L_512 :			\
	(ord) == 10 ? FT_TP_NODE_P1L_1024 :			\
	FT_TP_NODE_UNKNOWN)
#define FT_TP_KIND_PIGEON(ord) (				\
	(ord) == 10 ? FT_TP_NODE_PIGEON_1024 :			\
	(ord) == 11 ? FT_TP_NODE_PIGEON_2048 :			\
	FT_TP_NODE_UNKNOWN)

static const uint8_t ft_tp_kind_table[FT_TP_KIND_TABLE_MASK + 1] = {
	/* External: low 3 bits = 000; bit 3 unconstrained. */
	[0x0]				= FT_TP_NODE_EXTERNAL,
	[0x8]				= FT_TP_NODE_EXTERNAL,
	/* Compressed: low 3 bits = 010, bit 3 = 0 (16-byte aligned). */
	[FT_COMPRESSED_MASK]		= FT_TP_NODE_COMPRESSED,
	/*
	 * Internal nodes: bit 0 set, bits 1..3 = type index.  Each
	 * arch-specific ft_types[] is mapped via FT_TP_KIND_*().
	 */
#if (CAA_BITS_PER_LONG < 64)
	[FT_TP_INTERNAL_TAG(0)]		= FT_TP_KIND_P2L(5),
	[FT_TP_INTERNAL_TAG(1)]		= FT_TP_KIND_P2L(6),
	[FT_TP_INTERNAL_TAG(2)]		= FT_TP_KIND_P1L(7),
	[FT_TP_INTERNAL_TAG(3)]		= FT_TP_KIND_P1L(8),
	[FT_TP_INTERNAL_TAG(4)]		= FT_TP_KIND_P1L(9),
	[FT_TP_INTERNAL_TAG(5)]		= FT_TP_KIND_PIGEON(10),
	/* idx 6 = NODE_INDEX_NULL: never encoded in a pointer. */
#else
	[FT_TP_INTERNAL_TAG(0)]		= FT_TP_KIND_P2L(5),
	[FT_TP_INTERNAL_TAG(1)]		= FT_TP_KIND_P2L(6),
	[FT_TP_INTERNAL_TAG(2)]		= FT_TP_KIND_P2L(7),
	[FT_TP_INTERNAL_TAG(3)]		= FT_TP_KIND_P1L(8),
	[FT_TP_INTERNAL_TAG(4)]		= FT_TP_KIND_P1L(9),
	[FT_TP_INTERNAL_TAG(5)]		= FT_TP_KIND_P1L(10),
	[FT_TP_INTERNAL_TAG(6)]		= FT_TP_KIND_PIGEON(11),
#endif
};

uint16_t ft_tp_node_kind(struct cds_ft_inode_flag *nf)
{
	uint8_t kind;

	if (!nf)
		return FT_TP_NODE_NULL;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	/*
	 * Skip-compression is orthogonal to the underlying node type: a
	 * skip pointer still points to a real child (external or
	 * internal).  Strip the skip-length bits so the dispatch table
	 * sees the underlying child's tag bits.  The companion
	 * ft_tp_node_skip_len() field exposes the skip length separately.
	 */
	if (ft_node_skip_compressed(nf))
		nf = ft_skip_child_ptr(nf);
#endif
	kind = ft_tp_kind_table[(unsigned long) nf & FT_TP_KIND_TABLE_MASK];
	return kind ? kind : FT_TP_NODE_UNKNOWN;
}

/*
 * Return the number of key bytes the skip pointer covers (i.e. the
 * length of the skipped compressed path).  Zero means "not a skip
 * pointer".  The value fits in a uint16_t since FT_SKIP_LEN_MAX is at
 * most 255 on any supported architecture.
 */
uint16_t ft_tp_node_skip_len(struct cds_ft_inode_flag *nf)
{
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (!nf || !ft_node_skip_compressed(nf))
		return 0;
	return (uint16_t) ft_skip_len(nf);
#else
	(void) nf;
	return 0;
#endif
}
#endif /* FT_ENABLE_TRACING */
