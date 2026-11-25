// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-key.h
 *
 * Userspace RCU library - Fractal Trie: key <-> ordinal conversion and key comparison (scalar + SIMD).
 *
 * Implementation unit: #included once into the fractal-trie.c translation
 * unit (preserves cross-module inlining).  Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-key.h is an implementation unit; #include it from fractal-trie.c only"
#endif

static inline void ft_key_to_ordinals(uint8_t *dst, const uint8_t *key,
		size_t len, const struct cds_ft_key_map *km)
{
	size_t i;

	if (caa_likely(km->identity)) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wrestrict"
		memcpy(dst, key, len);
#pragma GCC diagnostic pop
		return;
	}
	for (i = 0; i < len; i++)
		dst[i] = km->key_to_ordinal[key[i]];
}

/*
 * Bulk ordinal-to-key conversion.  Converts @len ordinals in @src
 * back to external key bytes in @dst.  Identity maps short-circuit
 * to memcpy.
 */
static inline void ft_ordinals_to_key(uint8_t *dst, const uint8_t *ordinals,
		size_t len, const struct cds_ft_key_map *km)
{
	size_t i;

	if (caa_likely(km->identity)) {
		memcpy(dst, ordinals, len);
		return;
	}
	for (i = 0; i < len; i++)
		dst[i] = km->ordinal_to_key[ordinals[i]];
}

/*
 * Byte-swap an unsigned long for lexicographic word comparison on
 * little-endian.  On big-endian this is a no-op: natural word order
 * already matches memory (lexicographic) order.
 */
static inline unsigned long ft_bswap_long(unsigned long v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#if __SIZEOF_LONG__ == 8
	return __builtin_bswap64(v);
#else
	return __builtin_bswap32(v);
#endif
#else
	return v;
#endif
}

/*
 * Given two mismatching words loaded from position @base, return the
 * appropriate non-zero result.
 *
 * When @signed_cmp is true, byte-swap on little-endian to get
 * lexicographic word order, then return <0 or >0.
 * When @signed_cmp is false, return 1 (unequal, sign unspecified).
 *
 * When @mismatch_pos is non-NULL, store the index of the first
 * differing byte using ctz/clz on the XOR of the two words.
 *
 * Both checks are constant-folded when the function is inlined with
 * literal arguments.
 */
static inline_lookup
int ft_word_mismatch(unsigned long va, unsigned long vb,
		unsigned int base, bool signed_cmp,
		unsigned int *mismatch_pos)
{
	if (mismatch_pos) {
		unsigned long diff = va ^ vb;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		*mismatch_pos = base + (unsigned int)__builtin_ctzl(diff) / 8;
#else
		*mismatch_pos = base + (unsigned int)__builtin_clzl(diff) / 8;
#endif
	}
	if (signed_cmp) {
		va = ft_bswap_long(va);
		vb = ft_bswap_long(vb);
		return va < vb ? -1 : 1;
	}
	return 1;
}

/*
 * ft_key_cmp_ordinals: compare @len bytes of ordinal data from two
 * sources.  Both @a and @b must be in ordinal space.
 *
 * Returns 0 when equal, non-zero when unequal.
 *
 * @signed_cmp: when true, the return value encodes lexicographic
 *   order.  <0 means a < b, and >0 means a > b.  When false, any
 *   non-zero value may be returned.  This lets the compiler
 *   eliminate the bswap.
 *
 * @mismatch_pos: when non-NULL, receives the index of the first
 *   mismatching byte (undefined on full match).  Gates the
 *   bitscan instruction.
 *
 * All three use-cases (equality, mismatch position, signed
 * cardinality) share the same comparison logic.  Since this
 * function is force-inlined, both @signed_cmp and @mismatch_pos
 * checks are constant-folded at each call site.
 *
 * Dispatch is ordered by frequency.  Short keys (< 8 bytes) are the
 * most common case in trie traversal (compressed paths).  Medium
 * keys come next.  Long keys, where SIMD helps, come last.
 */

/*
 * Helper: resolve a mismatch found at byte position @pos.  Store
 * @pos in *@mismatch_pos when it is non-NULL.  Return the byte
 * difference when @signed_cmp is true, else 1.
 */
static inline_lookup
int ft_byte_mismatch(const uint8_t *a, const uint8_t *b,
		unsigned int pos, bool signed_cmp,
		unsigned int *mismatch_pos)
{
	if (mismatch_pos)
		*mismatch_pos = pos;
	if (signed_cmp)
		return (int)a[pos] - (int)b[pos];
	return 1;
}

#if defined(__AVX2__)
#ifndef FT_IMMINTRIN_INCLUDED
#define FT_IMMINTRIN_INCLUDED
#include <immintrin.h>
#endif
#endif
#if defined(__AVX2__) && !defined(FT_NO_SIMD_CMP)
/*
 * Helper: given a non-zero 32-bit mismatch mask from an AVX2
 * comparison starting at @base, resolve the first differing byte.
 */
static inline_lookup
int ft_avx2_mismatch(const uint8_t *a, const uint8_t *b,
		unsigned int base, unsigned int mask,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	unsigned int pos = base + (unsigned int)__builtin_ctz(mask);

	return ft_byte_mismatch(a, b, pos, signed_cmp, mismatch_pos);
}
#endif /* __AVX2__ && !FT_NO_SIMD_CMP */

#if defined(__SSE2__)
#ifndef FT_IMMINTRIN_INCLUDED
#define FT_IMMINTRIN_INCLUDED
#include <immintrin.h>
#endif
#endif
#if defined(__SSE2__) && !defined(FT_NO_SIMD_CMP)
/*
 * Helper: given a non-zero 16-bit mismatch mask from an SSE2
 * comparison starting at @base, resolve the first differing byte.
 */
static inline_lookup
int ft_sse2_mismatch(const uint8_t *a, const uint8_t *b,
		unsigned int base, unsigned int mask,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	unsigned int pos = base + (unsigned int)__builtin_ctz(mask);

	return ft_byte_mismatch(a, b, pos, signed_cmp, mismatch_pos);
}
#endif /* __SSE2__ */

/*
 * Building blocks for key comparison.  Each is self-contained and
 * handles its key length range completely, including tail.
 */

/* Compare len < 8 bytes.  Uses masked word or byte-by-byte. */
static inline_lookup
int ft_cmp_tiny(const uint8_t *a, const uint8_t *b,
		unsigned int len, unsigned int remaining_key,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	if (remaining_key >= sizeof(unsigned long)) {
		unsigned long va, vb, mask;

		__builtin_memcpy(&va, a, sizeof(unsigned long));
		__builtin_memcpy(&vb, b, sizeof(unsigned long));
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		mask = (1UL << (len * 8)) - 1;
#else
		mask = ~((1UL << ((sizeof(unsigned long) - len) * 8)) - 1);
#endif
		va &= mask;
		vb &= mask;
		if (va != vb)
			return ft_word_mismatch(va, vb, 0,
						signed_cmp, mismatch_pos);
	} else {
		unsigned int j;

		for (j = 0; j < len; j++) {
			if (a[j] != b[j])
				return ft_byte_mismatch(a, b, j,
						signed_cmp, mismatch_pos);
		}
	}
	return 0;
}

/* Compare len >= 8 bytes using word-at-a-time + overlapping tail. */
static inline_lookup
int ft_cmp_word(const uint8_t *a, const uint8_t *b,
		unsigned int len,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	unsigned int j = 0;

	while (j + sizeof(unsigned long) <= len) {
		unsigned long va, vb;

		__builtin_memcpy(&va, a + j, sizeof(unsigned long));
		__builtin_memcpy(&vb, b + j, sizeof(unsigned long));
		if (va != vb)
			return ft_word_mismatch(va, vb, j,
						signed_cmp, mismatch_pos);
		j += sizeof(unsigned long);
	}
	if (j < len) {
		unsigned long va, vb;
		unsigned int tail = len - sizeof(unsigned long);

		__builtin_memcpy(&va, a + tail, sizeof(unsigned long));
		__builtin_memcpy(&vb, b + tail, sizeof(unsigned long));
		if (va != vb)
			return ft_word_mismatch(va, vb, tail,
						signed_cmp, mismatch_pos);
	}
	return 0;
}

#if defined(__SSE2__) && !defined(FT_NO_SIMD_CMP)
/* Compare len >= 16 bytes using SSE2 + overlapping 16-byte tail. */
static inline_lookup
int ft_cmp_sse2(const uint8_t *a, const uint8_t *b,
		unsigned int len,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	unsigned int j = 0;

	while (j + 16 <= len) {
		__m128i va = _mm_loadu_si128((const __m128i *)(a + j));
		__m128i vb = _mm_loadu_si128((const __m128i *)(b + j));
		__m128i eq = _mm_cmpeq_epi8(va, vb);
		unsigned int mask = (unsigned int)_mm_movemask_epi8(eq);

		if (mask != 0xFFFFU)
			return ft_sse2_mismatch(a, b, j,
						~mask & 0xFFFF,
						signed_cmp, mismatch_pos);
		j += 16;
	}
	if (j < len) {
		unsigned int tail = len - 16;
		__m128i va = _mm_loadu_si128((const __m128i *)(a + tail));
		__m128i vb = _mm_loadu_si128((const __m128i *)(b + tail));
		__m128i eq = _mm_cmpeq_epi8(va, vb);
		unsigned int mask = (unsigned int)_mm_movemask_epi8(eq);

		if (mask != 0xFFFFU)
			return ft_sse2_mismatch(a, b, tail,
						~mask & 0xFFFF,
						signed_cmp, mismatch_pos);
	}
	return 0;
}
#endif /* __SSE2__ && !FT_NO_SIMD_CMP */

#if defined(__AVX2__) && !defined(FT_NO_SIMD_CMP)
/*
 * Unmasked 32-byte AVX2 short-key compare for 1 <= len <= 32.
 *
 * Caller guarantees @a and @b each have at least 32 readable bytes.
 * Load is unmasked; mask is applied only to the comparison result
 * so bytes past @len don't influence the outcome.
 *
 * Cheapest short-key path when the contract permits it: one
 * vmovdqu pair + vpcmpeqb + vpmovmskb + bzhi + andn + jne.  No
 * page-cross check (caller-promised safe), no EVEX kmask setup,
 * no overlapping tail load.  ~10 cycles on Zen 4 for the matching
 * case.
 */
static inline_lookup
int ft_cmp_short_unmasked_avx2(const uint8_t *a, const uint8_t *b,
		unsigned int len,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	__m256i va = _mm256_loadu_si256((const __m256i *) a);
	__m256i vb = _mm256_loadu_si256((const __m256i *) b);
	__m256i eq = _mm256_cmpeq_epi8(va, vb);
	uint32_t mask = (uint32_t) _mm256_movemask_epi8(eq);
	uint32_t want = (uint32_t) _bzhi_u32(0xFFFFFFFFU, len);

	if ((mask & want) == want)
		return 0;
	return ft_avx2_mismatch(a, b, 0, (~mask) & want,
				signed_cmp, mismatch_pos);
}
#endif

#if defined(__SSE2__) && !defined(FT_NO_SIMD_CMP)
/*
 * Unmasked 16-byte SSE2 short-key compare for 1 <= len <= 16.
 *
 * Same idea as ft_cmp_short_unmasked_avx2 but 16-byte load width.
 * Caller guarantees @a and @b each have at least 16 readable bytes.
 */
static inline_lookup
int ft_cmp_short_unmasked_sse2(const uint8_t *a, const uint8_t *b,
		unsigned int len,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	__m128i va = _mm_loadu_si128((const __m128i *) a);
	__m128i vb = _mm_loadu_si128((const __m128i *) b);
	__m128i eq = _mm_cmpeq_epi8(va, vb);
	unsigned int mask = (unsigned int) _mm_movemask_epi8(eq);
	unsigned int want = (1U << len) - 1U;

	if ((mask & want) == want)
		return 0;
	return ft_sse2_mismatch(a, b, 0, (~mask) & want,
				signed_cmp, mismatch_pos);
}
#endif

#if defined(__AVX512VL__) && defined(__AVX512BW__) && !defined(FT_NO_SIMD_CMP)
/*
 * AVX-512 short-key compare for 1 <= len <= 32 (BW + VL).
 *
 * Predicated load on BOTH sides + predicated compare.  The k-mask
 * gates which byte lanes each load fetches.  Bytes outside the mask
 * are NOT read from memory.  The Intel SDM and the AMD APM guarantee
 * this for AVX-512 masked memory operands.  So both pointers are
 * page-cross safe by construction, with no runtime check.  The
 * @readable_bytes contract needs nothing beyond @readable_bytes >= @len.
 *
 * This is cheaper than the AVX2 fallback.  It has no page-cross branch
 * and no separate mask-and-result step.  It matches what glibc's
 * __memcmp_evex_movbe emits for len <= 32.
 */
static inline_lookup
int ft_cmp_short_avx512(const uint8_t *a, const uint8_t *b,
		unsigned int len,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	__mmask32 k = (__mmask32) _bzhi_u32(0xFFFFFFFFU, len);
	__m256i va = _mm256_maskz_loadu_epi8(k, (const void *) a);
	__m256i vb = _mm256_maskz_loadu_epi8(k, (const void *) b);
	__mmask32 ne = _mm256_mask_cmpneq_epu8_mask(k, va, vb);

	if (ne == 0)
		return 0;
	return ft_avx2_mismatch(a, b, 0, (uint32_t) ne,
				signed_cmp, mismatch_pos);
}
#endif

#if defined(__AVX2__) && !defined(FT_NO_SIMD_CMP)
/*
 * Page-cross-safe single-vector compare for 16 <= len <= 31.
 *
 * Load 32 bytes from both pointers and compare all 32.  Then mask
 * the result to the first @len lanes.  Reading 1-16 bytes past the
 * requested length is safe iff neither pointer is in the last 32
 * bytes of its 4 KB page.  The page-cross check below tests that.
 * If the check fails, fall back to the overlapping-pair SSE2 path.
 * That path never reads past byte @len-1.
 *
 * The branch is highly predictable.  In practice both @a and @b
 * are typically in the first ~4000 bytes of their pages.  Slot
 * bodies start CL-aligned and never span pages for our slot sizes.
 * So the fast path is taken essentially 100% of the time.
 *
 * Used only when AVX-512 BW+VL is not available.  The AVX-512 path
 * above does the same job without the page-cross check.
 */
static inline_lookup
int ft_cmp_short_avx2(const uint8_t *a, const uint8_t *b,
		unsigned int len,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	/*
	 * Page-cross check: a 32-byte load at @p is safe (cannot cross
	 * a page boundary) iff (p & 0xFFF) <= 0xFE0.  Both pointers
	 * must be safe; OR-ing the low 12 bits picks up the worst of
	 * the two with a single AND.
	 */
	if (caa_likely((((uintptr_t)a | (uintptr_t)b) & 0xFFFu) <= 0xFE0u)) {
		__m256i va = _mm256_loadu_si256((const __m256i *)a);
		__m256i vb = _mm256_loadu_si256((const __m256i *)b);
		__m256i eq = _mm256_cmpeq_epi8(va, vb);
		uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);
		uint32_t want = (uint32_t)((1ULL << len) - 1ULL);

		if ((mask & want) != want)
			return ft_avx2_mismatch(a, b, 0,
						(~mask) & want,
						signed_cmp, mismatch_pos);
		return 0;
	}
	return ft_cmp_sse2(a, b, len, signed_cmp, mismatch_pos);
}

/* Compare len >= 32 bytes using AVX2 + overlapping 32-byte tail. */
static inline_lookup
int ft_cmp_avx2(const uint8_t *a, const uint8_t *b,
		unsigned int len,
		bool signed_cmp, unsigned int *mismatch_pos)
{
	unsigned int j = 0;

	while (j + 32 <= len) {
		__m256i va = _mm256_loadu_si256((const __m256i *)(a + j));
		__m256i vb = _mm256_loadu_si256((const __m256i *)(b + j));
		__m256i eq = _mm256_cmpeq_epi8(va, vb);
		uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);

		if (mask != 0xFFFFFFFFU)
			return ft_avx2_mismatch(a, b, j,
						~mask, signed_cmp,
						mismatch_pos);
		j += 32;
	}
	if (j < len) {
		unsigned int tail = len - 32;
		__m256i va = _mm256_loadu_si256((const __m256i *)(a + tail));
		__m256i vb = _mm256_loadu_si256((const __m256i *)(b + tail));
		__m256i eq = _mm256_cmpeq_epi8(va, vb);
		uint32_t mask = (uint32_t)_mm256_movemask_epi8(eq);

		if (mask != 0xFFFFFFFFU)
			return ft_avx2_mismatch(a, b, tail,
						~mask, signed_cmp,
						mismatch_pos);
	}
	return 0;
}
#endif /* __AVX2__ && !FT_NO_SIMD_CMP */

/*
 * ft_key_cmp_ordinals: compare @len bytes in ordinal space.
 *
 * @readable_bytes: contract from the caller -- @a and @b each have
 *                  at least this many bytes safely readable (no
 *                  fault on load).  Conservative callers pass
 *                  @readable_bytes = @len (no over-read promised).
 *                  Callers that know their buffers have trailing
 *                  padding pass a larger value, unlocking the
 *                  widest unmasked single-load fast path.
 *
 * Dispatch ordered by call-site frequency (hottest first):
 *
 *   1. len <= 32 && readable >= 32:    ft_cmp_short_unmasked_avx2
 *                                      (1 vmovdqu pair + mask to @len)
 *                                      -- HOT, single likely branch.
 *
 *   AVX-512 BW+VL build:
 *   2. len <= 32 (any readable):       ft_cmp_short_avx512
 *                                      (predicated load on both sides,
 *                                       handles 1..32 contract-free)
 *
 *   Non-AVX-512 build (SWAR ladder):
 *   2. len <= 16 && readable >= 16:    ft_cmp_short_unmasked_sse2
 *   3. len < 8:                        tiny (masked 8-B word if
 *                                       readable >= 8, else byte-by-byte)
 *   4. 8 <= len < 16:                  word-overlap-pair
 *   5. 16 <= len <= 32:                ft_cmp_short_avx2 (page-cross check)
 *                                      / SSE2 overlapping pair
 *
 *   Final (any build):
 *   6. len > 32:                       loop + overlapping tail
 *                                      (contract-independent; rare).
 *
 * Key insight: when @readable_bytes >= 32, every short key
 * (regardless of @len: 1..32) takes the same fast path.  The same
 * code is emitted for len=5 and len=25 -- only the mask differs.
 * When AVX-512 BW+VL is available, the predicated short path
 * covers ALL of 1..32 without any contract, so the SWAR ladder is
 * elided entirely.
 */
static inline_lookup
int ft_key_cmp_ordinals(const uint8_t *a, const uint8_t *b,
		unsigned int len, unsigned int readable_bytes,
		bool signed_cmp, unsigned int *mismatch_pos)
{
#ifdef CMM_SANITIZE_THREAD
	/*
	 * The wide loads below read up to @readable_bytes, past @len.  The
	 * compare then masks out the extra bytes.  Those bytes may belong to
	 * the next item of the arena, which a writer may be initializing.
	 * ThreadSanitizer reports that as a race.  Load only @len bytes, so
	 * that each report is about a byte the compare uses.  For the same
	 * reason, skip ft_cmp_short_avx2() below: it always loads 32 bytes.
	 */
	readable_bytes = len;
#endif
	if (caa_likely(len <= 32)) {
		/*
		 * Hot path: caller-promised 32-B readable horizon on
		 * both sides -- single unmasked AVX2 load each side.
		 */
#if defined(__AVX2__) && !defined(FT_NO_SIMD_CMP)
		if (caa_likely(readable_bytes >= 32))
			return ft_cmp_short_unmasked_avx2(a, b, len, signed_cmp, mismatch_pos);
#endif
#if defined(__AVX512VL__) && defined(__AVX512BW__) && !defined(FT_NO_SIMD_CMP)
		/*
		 * AVX-512 BW+VL: predicated loads on both sides are
		 * page-cross safe and cover all 1..32 without further
		 * dispatch.  Wins over the SWAR ladder for the common
		 * short-key range (16..32) and matches glibc's
		 * __memcmp_evex_movbe for shorter keys too.
		 */
		return ft_cmp_short_avx512(a, b, len, signed_cmp, mismatch_pos);
#else
		/* Short key, no 32-B contract.  Try 16-B contract. */
# if defined(__SSE2__) && !defined(FT_NO_SIMD_CMP)
		if (len <= 16 && readable_bytes >= 16)
			return ft_cmp_short_unmasked_sse2(a, b, len, signed_cmp, mismatch_pos);
# endif
		/* len < 8: tiny (byte/masked-word). */
		if (len < sizeof(unsigned long))
			return ft_cmp_tiny(a, b, len, readable_bytes,
					   signed_cmp, mismatch_pos);
		/* 8 <= len < 16: word-overlap-pair. */
		if (len < 16)
			return ft_cmp_word(a, b, len, signed_cmp, mismatch_pos);
		/* 16 <= len <= 32 without 32-B contract: safe fallback. */
# if defined(__AVX2__) && !defined(FT_NO_SIMD_CMP) && \
	!defined(CMM_SANITIZE_THREAD)
		return ft_cmp_short_avx2(a, b, len, signed_cmp, mismatch_pos);
# elif defined(__SSE2__) && !defined(FT_NO_SIMD_CMP)
		return ft_cmp_sse2(a, b, len, signed_cmp, mismatch_pos);
# else
		return ft_cmp_word(a, b, len, signed_cmp, mismatch_pos);
# endif
#endif
	}

	/* Long key (>32): contract-independent loop. */
#if defined(__AVX2__) && !defined(FT_NO_SIMD_CMP)
	return ft_cmp_avx2(a, b, len, signed_cmp, mismatch_pos);
#elif defined(__SSE2__) && !defined(FT_NO_SIMD_CMP)
	return ft_cmp_sse2(a, b, len, signed_cmp, mismatch_pos);
#else
	return ft_cmp_word(a, b, len, signed_cmp, mismatch_pos);
#endif
}
