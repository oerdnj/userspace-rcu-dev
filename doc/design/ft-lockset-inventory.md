# Fractal Trie — lock-set inventory at FINE (per-node) and EXPONENTIAL (2026-09-16)

Mathieu's shape for the work: *"we need to complete the missing lock-set
transition for fine and exponential. only when it is COMPLETE can we turn MW
into SW."* This is the inventory that transition starts from. It answers one
question: **which mutation primitives do not hold their lock set, at per-node
and at exponential spacing**. The answer is measured and attributed to the
primitive. It is not argued.

It is an inventory, not a plan. No primitive is converted here, and nothing
below is a proposal to convert one class on its own.

---

## 0. The question, and how it was asked

**"Holds its lock set"** is a property of an op at the moment it writes a word.
The op holds the lock that `ft-dlm-lock-coarseness.md` §1/§2 assign to that
word. That lock is the **anchor of the word's owner** (the owner itself at
per-node). An op can fail it in two ways, and they need different instruments:

* **Coverage** — the op writes a word and holds no lock covering it.
* **Agreement** — the op holds *a* lock, but not the one every other op
  computes for that node, so it excludes nobody (§1).

`-DFT_DEBUG_STRUCT_ANCHOR` answers both against an **independent** anchor. It
needs `-DFT_DEBUG_TXN_KIND`, and a build with `-fno-omit-frame-pointer`. It
climbs the owner's parent words to the root. It sums the spans to get the byte
depth. Then it applies §2 over that path. This is §5.3's rejected up-walk, used as
an oracle only. A parked proxy, a NULL parent word (transient re-home) or an
external on the path reads UNDATED rather than guessed.

| probe | where | question |
|---|---|---|
| A | `ft_dlm_acquire_set_at`, after `ft_anchor_meta` | does the anchor the op takes equal the trie's anchor for that member? |
| B | `__ft_flip_txn_record_tag_ctx` / `ft_flip_txn_record_tag_mw`, re-asked in `ft_flip_txn_commit` | at record time, is the owner's anchor held? If not, is it held by commit (LATE) or never (NEVER)? Keyed by (txn creation site, call site, one frame up), symbolized offline with `addr2line -i` |
| C | the chain audit arms (`-DFT_DEBUG_CHAIN_HOLD`) | the same exact-anchor verdict for the words that never reach a record helper: hlist stores, raw parent/head words, in-place body writes. Writes the caller declares `FT_EXCL_HIDDEN` are skipped **except chain words**, which carry no declaration and are always scored |

The witness is the union the owner assert uses:
- the txn registry;
- the ctx frames;
- the per-thread hold ledger (`-DFEATURE_FT_HOLD_TRACE`);
- this trie's FT-wide writer lock.

Two cases are bucketed out. COARSE and EXCLUSIVE tries have no peer to exclude.
Commits that already carry `@acquire_miss` are discarded before the engine.

**Red control, and it fires.** `FT_SA_RED=root` asks every record about the
ROOT instead of its real anchor, without touching the ops. ft_unit at
exponential goes from 0 to **2,700,761 SELF_ONLY + 41,617 UNHELD**. The 41,617
matches the normal run's 41,619 records held through a coarser anchor to within
2. So the witness can say *no* while the op holds other locks, and the zeros
below are readings, not a blind instrument.

**Rig.** `-O2 -g -fno-omit-frame-pointer -DFEATURE_FT_INSERT_IN_PLACE
-DFEATURE_FT_LOCK_SPACING_ENV -DFT_DEBUG_STRUCT_ANCHOR -DFT_DEBUG_TXN_KIND
-DFEATURE_FT_HOLD_TRACE -DFT_EXCL_REPORT_ONLY -DFT_DEBUG_CHAIN_HOLD`,
`CDS_FT_LOCK_SPACING=per-node|exponential`, ft_unit 357/357 and ft_inv 152/152
green in every leg. Numbers are per single leg (one ft_inv run covers its
list-on, list-off and MW rows).

**Limits.** Only paths the two suites reach (§4). The always-MW lanes carry
no owner, so B counts them but cannot score them. §3 says which are MW on
purpose and which are unfinished. HIDDEN declarations are
believed on the raw lanes. Record time asks the SW question on purpose: an SW
park is legal only if the lock was held when the overwritten value was read.

---

## 1. Coverage: primitives that write while holding NOTHING — both spacings

Probe B puts these in NOLOCKS: the registry and the ledger are both empty. The
commit re-check puts them in NEVER: the lock is still not held when the
descriptor goes to the engine. None of them depends on spacing, because nothing
is held.

| # | primitive | ft_inv per-node | ft_inv exponential | ft_unit | what is missing |
|---|---|---|---|---|---|
| 1 | **delete tier, pure leaf delete** — `ft_detach_node` → `ft_remove_one_commit` (holder's child slot + the fused `nr_child--`), and its chain word at `ft_detach_node:4975` (freeze_leaf) | 2,745,486 records, **2,585,871 committed never held** (159,615 aborted); chain lane 2,384,879 | 2,609,398, **2,443,766 committed** (165,632 aborted); chain lane 2,247,316 | 655,092 / chain 646,700 | the holder (its anchor). `ft_flip_txn_lock_or_guard_parent` runs only on the promote arm (`commit_txn && !pub->state_meta`); the leaf arm leans on the fused `nr_child--` CAS, the "arbitrated on the holder's state word" of `ft-reintroduce-in-place-mutations.md` §6.5 |
| 2 | **`cds_ft_remove_all` of the EMPTY key** — root's `external_nodes` (`_cds_ft_remove_all_locked`, both `key_len == 0` commits) | 57,918, all committed | 55,126, all committed | 4 | the root node's lock (the root is its own anchor at every spacing) |
| 3 | **insert over an OCCUPIED slot** — `_cds_ft_insert`'s "NULL or external node before end of key" arm → `ft_attach_node`'s reserved-slot record, when the slot already holds a displaced external head | 13,359 (13,326 committed) | 13,942 (13,917 committed) | 102 (exponential: 55 LATE, 47 NEVER) | the attach node. The hoist is gated on `!old_node_flag`; the other `ft_attach_node` call site (`_cds_ft_insert:3476`) is fully held |
| 4 | **compaction** — `ft_compact_relocate_compressed`'s raw parent and head words | — | — | 1,030 | any lock: its exclusion is the "caller's responsibility" TODO, which is not a contract |
| 5 | bulk glue writes outside the FT-wide lock — `ft_glue_apply_deferred`, `ft_glue_defer_edge_origin`, `ft_store_at_graft_point_commit` | ≤ 45 | ≤ 40 | ≤ 11 | bulk ops are excluded by design by the FT-wide lock; these few rows are writes the lock did not cover at that instant — not yet attributed |

**Everything else that holds a lock holds the right one for the words it
records.** The structural record lane, ft_inv at exponential, reads:
- HELD 29,243,068;
- held through a coarser anchor 1,207,645;
- **SELF_ONLY 0, UNHELD 0**;
- UNDATED 1,925. Row 1's commit has 1,923 of them, racing re-homes.

Per-node likewise has no UNHELD.

On the raw lanes, these writes are covered at exponential:
- the chain words of `ft_chain_compress_fused:2326` and `ft_detach_node:3902`;
- the in-place body writes (`ft_popcount_node_set_nth` /
  `ft_pigeon_node_set_nth`).

A large share of them is covered through an ancestor anchor.
(`ft_popcount_node_set_nth:516` reads UNHELD / UNDATED on an unpublished node.
It is the refused-arm false positive that
`ft-reintroduce-in-place-mutations.md` §6.3 already names.)

---

## 2. Agreement: acquires that take the WRONG word — exponential only

Probe A, ft_inv at exponential (per-node is exact by construction). For rows
6–9 the mismatch does not depend on load. Each row also shows up in
single-threaded ft_unit, or its mechanism is read directly off the anchor table
in the sample.

| # | acquire site | mismatches | mechanism (from the samples) |
|---|---|---|---|
| 6 | `ft_detach_node`'s compressed-parent free walk (`ft_detach_orphan_acquire` in the `free_detached_subtree \|\| nr_elevated` loop) | **88,059 of 199,918**; ft_unit 2 | `walk_depth` is computed once and **never advanced**, so every orphan past the first is dated at the first one's depth. Sample: depth(op) 2, depth(trie) 3, L=2 → `ft_anchor_meta`'s on-a-level early return anchors the node on ITSELF, while its anchor is the parent at byte 2 |
| 7 | `ft_detach_orphan_planlock` (both orphan walks, via `ft_walk_extend`) | 3 per run, plus ~2,190 stale (below) | `ft_walk_extend` leaves `d->nf` on the node it ENTERED and moves `d->depth` to that node's end, so the cursor is the member's PARENT. Sample: `pending=1 bound=(nil) bound_start=7 == clamp` → `bound ? bound : d->nf` returns the compressed parent instead of the member (SELF). This is the design doc's still-open *"`a->bound ? a->bound : d->nf` substitutes the CURSOR"* |
| 8 | `ft_node_recompact:1761` | 11 (1 non-stale); ft_unit 6 | the cursor is the member's **SKIP-encoded** flag (`0xa007…f00b` over a member `0x7ff…f00b`). The pending-level fallback returns it, `anchor == nf` fails on the encoding, and `ft_flag_to_metadata` resolves the skip word to the elided compressed node (ABOVE) |
| 9 | `ft_merge_lock_overlap:523` | 22 | the cursor is at depth 0 and level 2 was never crossed (`crossed=0`); a member three bytes down goes through `ft_descent_anchor_child`, whose rule is exact for ONE hop only, and anchors on itself. Bulk: the FT-wide lock excludes today |

**Live evidence that 7 breaks exclusion.** The E.2 exclusion oracle is armed
with `FEATURE_FT_HOLD_TRACE` and without `FT_EXCL_REPORT_ONLY`. ft_inv at
exponential then aborts in `inv_concurrent_insert_unique_nolist`: **2 of 80**
filtered runs, plus 1 full leg. The shape is identical each time:

    FT EXCLUSION VIOLATION: node X claimed at ft_flip_txn_lock_or_guard_parent_ex
      by tid B (anchor X=SELF) while owned by tid A (from ft_detach_orphan_planlock:984,
      anchor <ancestor>) ... ANCHORS DIFFER

Two threads cover one node through two words. Probe B cannot see this: each op
holds *a* covering lock for its own records. Only A and the oracle see an
agreement failure.

### 2.1 Rows 6-8 fixed; row 9 open (2026-09-17)

**Rows 7 and 8 were one defect in the derivation.** `ft_descent_anchor_at_level`'s
pending-level fallback, `a->bound ? a->bound : d->nf`, names the node starting at
`bound_start` through the cursor. It assumes the cursor is the not-yet-entered node
at `d->depth`. `ft_walk_extend` leaves the cursor on the node it ENTERED (row 7), and
`ft_node_recompact` left it skip-encoded (row 8). The member passed to the query
starts at `bound_start`, so it IS that node. The query now takes the member
(`self`). It answers with the member when `bound_start == clamp`.

**Row 6** gives `ft_detach_node`'s first orphan walk its own descent copy, extended
per orphan with its depth advanced, as the phase-2 walk already did. Re-entering
key-path nodes in order restores each pending level before a deeper member is
queried.

Probe A and the anchor-property probe (§7), ft_inv at exponential, before -> after:

| site | before | after |
|---|---|---|
| `ft_detach_node` orphan walk (row 6) | 65,290 mismatches of 150,365 | **0** of 171,553 (ft_unit 2 -> 0) |
| `ft_detach_orphan_planlock` (row 7) | 3,025: 1 above, 1 cursor-is-parent, rest off-path | 2,300, **all off-path** (stale plans: the op anchored on an older copy of the parent) |
| `ft_node_recompact` (row 8) | 0 (11 at 09-16f) | 0 |
| `ft_merge_lock_overlap` (row 9) | 29 | 27 -- **not fixed** |
| non-locked-word writes: op holds A and the parent shares A | 1,090,320 of 1,090,340; 2 SELF_ANCHORED | **1,160,550 of 1,160,552; 0** |

The live E.2 violation above cannot confirm the fix. It no longer reproduces at
HEAD before this change: 0 of 400 filtered runs. The rows 1-4 conversions
changed that test's interleavings. The probe rows do confirm the fix.

One on-path mismatch remains at `ft_chain_compress_fused:1614`: 1 in 2.46M.
Its descent dates the compressed parent's end at byte 18. The member starts at
byte 12. That is not the pending-boundary case, so this change does not affect
it. Not yet explained.

**Row 9 is not an agreement defect to fix. It is a bulk op that should take no
DLM lock at all.** Its 27-33 mismatches come from the cross-trie merge
(`inv_ordered_bulk_consistency`, `inv_merge_root_src_cross_view`).
`ft_merge_build` fences each dst overlap node through the glue's descent. That
descent ends at the merge point. But the fence excludes nothing that is not
already excluded, whatever word it takes:
- `cds_ft_merge_at` holds the dst's FT-wide writer lock for its whole body (its
  exclusive source skips every grace-period wait);
- while a bulk op is live, a fine trie's point ops re-take that lock (G5.25).

Mathieu: the bulk-op transformation is INCOMPLETE -- bulk ops "should not have to
take _any_ node or exp locks, because they exclude all other ops with per-ft
locking". Two facts support it:
- the FT-wide lock is dropped only at `ft_writer_lock_gp_wait` (reader drains in
  detach, graft, graft_swap and rekey on a live trie);
- the seam rule forbids holding a node lock across one.
A take still performs one duty incidentally. It refuses a TOMBSTONE/PROXY word
for a plan that was read before a drain seam and used after it. The next step
is to measure which bulk-op plans cross a seam.

**Stale plans, not defects.** The other ~2,330 mismatches are OFF PATH:
- 2,192 at the plan-lock;
- 85 at `ft_chain_compress_fused:1614`;
- 25 at `ft_flip_txn_lock_or_guard_parent_ex`;
- 13 at `_cds_ft_insert:3812`;
- 10 at `ft_node_recompact`;
- 2 at `ft_unchain_node`.

**Every one** has its anchor or its member TOMBSTONED at the check. That is the
§2.1 "planned against the pre-change node hits TOMBSTONE and replans" path working.

---

## 3. The always-MW lanes — two by design, the rest unfinished

`ft_flip_txn_record_tag_mw` names no owner, so these records carry no coverage
verdict. Mathieu's classification (2026-09-17):
- **MW on purpose:** the root pointer, and the cell sibling list.
- **Everything else should be SW under lock.** Anything still MW here is an
  unfinished transition, not a design choice.
- **`nr_keys`:** a rank-stats group is coerced to `CDS_FT_WRITER_LOCK_COARSE`
  (`ft-lifecycle.h`), so every writer holds the FT-wide writer lock. It may be
  SW under that condition only (register note 9).

Records per ft_inv leg, by class (the `-DFT_DEBUG_STRUCT_ANCHOR` legs of §7.2):

| class | exponential | per-node | what it is | classification |
|---|---|---|---|---|
| ROOT | 1,911,262 | 1,892,283 | `&ft->root` (graft swap, remove, root list swaps, graft/merge publishes) | **MW on purpose** |
| CELL | 9,405,804 | 8,837,644 | every non-structural-tag edge through `ft_ord_cell_flip_into` / `ft_ord_cell_record_into_ft` (remove 4.6M, swap publish 1.8M, remove rec 1.3M, root list swaps, graft): the key-ordered CELL LIST links and deletion marks, plus ONE state-word edge -- `ft_remove_one_commit`'s not-held `nr_child--` (`ft_state_edge`, tag `FT_STATE_PROXY`), counted here because it shares bit 0 with `URCU_TXN_TAG` | the cell list is **MW on purpose**; the `nr_child--` edge is not a cell edge (not split yet). ☠ NOT the duplicate chain: `cds_ft_node.next` / `prev` are a different list, stored through `ft_hlist_store_*` and not counted in this class |
| HEAD_BACK | 6,449,049 | 6,288,104 | head back edges: `ft_node_recompact`'s re-home sweep 4.4M, `ft_chain_compress_fused` 0.88M, `ft_park_live_parent_edge` 1.08M, detach, remove_all | **unfinished** (owner: the chain holder) |
| STATE | 1,673,695 | 984,569 | `{live -> live}` validations of re-homed children (= the PARENT_WORD count); at exponential also **599,063 MW anchored retires** (`ft_chain_compress_fused`, `ft_detach_freeze_one`, insert, recompact, glue) | the retires are **unfinished** writes; the validation is a read check, which the register keeps MW ("a park validates nothing") -- ☐ to confirm |
| PARENT_WORD | 1,074,632 | 984,569 | `ft_reparent_record_meta`, the child not held | **unfinished** (owner: the parent) |
| DUAL_UNNAMED | 463,642 | 451,030 | `_cds_ft_insert_replace`'s leaf replace: the list-off arm 437k (`ft_ord_cell_flip_into`), the list-on arm 27k (`ft_ord_cell_swap_publish_multi`). ☠ NOT cell edges: STRUCTURAL edges built by hand with no owner -- the forward edge, or a SKIP_X dual plus `cn->child` | ☑ asked at every arm (§10.3); the dual's grandparent was NOT held, now taken |
| PSO | 411,687 | 408,417 | the same re-parent sweeps | **unfinished** |
| RANK | 1,515 | 1,515 | `nr_keys` via `ft_flip_txn_record_count_parent` (coarse tries only) | **unfinished**: SW under the FT-wide lock, COARSE only |

## 4. Not reached by either suite

Neither suite reaches these:
- `_cds_ft_remove_locked`'s and `_cds_ft_remove_all_locked`'s ordered-cell
  unsplice RETRY txns;
- `ft_compact_relocate_compressed`'s own txn;
- every rekey spine / subpos site.

`cds_ft_replace` commits 3 times in ft_unit and not enough in ft_inv to score.
A primitive missing from §1 is only as clean as its coverage.

---

## 5. SETTLED BY TEST, then CLOSED: holding the old anchor did not exclude a late acquirer

§3 of the anchor doc argues that *"every change to which-node-covers-byte-L is
itself a locked mutation of those very nodes"*. Holding the OLD anchor during
the change excludes ops that hold it *concurrently*. The question was whether it
also excludes an op that PLANNED before the change and ACQUIRES the old anchor
after the restructurer released it. It does not.

**The rig** is `doc/design/ft-anchor-move-rig.c`. It runs against a library
built with `-DFT_DEBUG_INTERLEAVE`. That build has a test-only hook that parks
the delete tier's leaf hoist before and after its acquire. Exponential spacing:

    root [0,1) -'a'-> cn [1,9) "bcdefghi" -> A @9 -'J'-> X @10, children '1'..'8'
    anchor(X): depth 10, L = 8, first boundary >= 8 is A          ==> A
    split: insert "abcdefghQzz" (diverges at byte 8), boundary B @8 ==> anchor(X) = B

The VICTIM removes `abcdefghiJ8`. That leaf is directly in X's slot, so the
victim holds nothing before its hoist. The steps, one column per thread, time
going down:

```
VICTIM                        SPLIT             PEER
plan taken: anchor A
parked at PRE
                              commits
acquires A
parked at POST, holding A
                                                runs, 2 s budget
```

| mode | peer | peer completed while the victim held its anchor | reading |
|---|---|---|---|
| no split | delete `J7` | **no** | CONTROL: both anchor X on A, the peer waits |
| split | delete `J7` | **YES** | **HOLE**: the victim holds A, the peer holds B, both write X |
| split | insert `J0` | no | a false negative, see below |

Probe A records the victim's acquire as a LIVE mismatch (neither node
tombstoned): `op anchor A (start 9), trie anchor B (start 8)`. Both commits
landed and both keys are gone. Today's MW records arbitrated the two writes to
X's slot and state word. Under SW parks they would not.

☠ **The insert peer is not a counter-example.** Growing X recompacts it with
lock set {X, A, ...}. A is X's PARENT, so it is a coarsened MEMBER. The acquire
samples a coarsened member's own word, and A's own word carries the victim's
lock. So the insert waits on A as a member, not on the anchor it computed. A
peer whose lock set is {X} alone (the leaf delete) shows the gap.

**What the split does do**, observed on the way: its lock set includes the child
it re-homes (A). So a victim already HOLDING A blocks the split. The gap is only
the late acquire. The acquire validates the MEMBER's back edge, and a split
above A leaves X's edge unchanged. Once A is held, nothing re-checks that A is
still the first boundary at or after L.

### 5.1 The cure: a missing tombstone validation (Mathieu)

The plan derived A from cn, the node that covers level 8. At the victim's stale
acquire, cn was already `TOMBSTONE` (probe A: `cover state 0x6`). The split's
fenced tombstone had retired it. The anchor answer depends on that node:

- A boundary can appear in `[L, anchor)` only by splitting the node that covers
  L strictly inside its span.
- A boundary can disappear at the anchor only by fusing the anchor into that
  node.
- A compressed node's span never changes in place, so both changes RETIRE it.

`ft_anchor_coverer` names that node for a member (NULL when the answer
depended on none). `ft_dlm_acquire_set_at` loads its state after the acquire
commit. A tombstone releases what was taken and re-plans with `-EAGAIN`.
TOMBSTONE never clears, so a load after the commit answers for the commit --
**read logically**.

☠ **The first version read it raw, and the skeptic REFUTED it by running.** A
split's commit settles its guard on the old anchor A BEFORE the fenced tombstone
on cn. The guard is a plain late record. The tombstone is on a registered lock
word, and those settle last (`8c517957`).
Between the two, A is takeable while cn still holds the split's SUCCEEDED proxy.
That raw word carries `FT_STATE_PROXY` (bit 0), not `FT_STATE_TOMBSTONE` (bit 1),
so the check passed and the hole was back. The check now resolves the word
(`urcu_txn_resolve`: SUCCEEDED answers new, anything else old). An UNDECIDED
restructure cannot succeed: the lock just taken fails its guard on A.
`-DFT_DEBUG_INTERLEAVE` gained `FT_IL_SETTLE_LOCKS_LAST`.
`ft_flip_txn_late_last` fires it at the start of the last settle pass. The
rig's `splitlate` mode uses it to park the split in exactly that window.

A load after the commit is enough only if no split can land while the lock is
held. That holds. Moving the boundary re-homes (or retires) the old anchor. The
re-home records a guard on that node's CLEAN state word
(`ft_reparent_record_meta`). The victim's lock fails that guard. `splitheld`
measures that, with a red control:

| mode (peer = delete `J7`) | coverer check | split while held | peer while held | reading |
|---|---|---|---|---|
| nosplit | 18 checked, 0 retired | -- | waited | CONTROL OK |
| split | 17 checked, **1 retired** | -- | **waited** | the stale acquire re-planned onto B: **no hole** |
| splitheld | 17 checked, 0 retired | waited | waited | the held order was already closed |
| splitheld, RED: child state guard removed | 17 checked, 0 retired | **completed** | **completed** | the guard is what closes it |
| splitlate, RED: coverer read raw | 17 checked, 0 retired | -- | **completed** (3/3) | a committed tombstone read as a proxy: **HOLE** |
| splitlate | 17 checked, **1 retired** | -- | waited (3/3) | resolved: **no hole** |

A skip-encoded coverer names its node only through the child's back pointer.
The very split being guarded against re-homes that pointer. So the check
re-plans on a skip-encoded coverer rather than resolve it. The descents enter
resolved flags, so it is not expected to occur. `-DFT_DEBUG_STRUCT_ANCHOR`
counts it (`FT_SA_COVER ... skip=`).

---

## 6. Progress on §1 (ordered by Mathieu: 1, 2, 3, then 4)

| row | commit | after |
|---|---|---|
| 1 delete-tier leaf delete | `7e4e9d15` | NOLOCKS 2.7M -> 0 (per-node), 2.6M -> 0 (exponential) |
| 2 `remove_all` of the empty key | `c4089e33` | both commit rows absent |
| 3 insert over an occupied slot | `79f0b245` | row absent |
| 4 compaction (compressed + cell relocation) | `06662abe` | raw writes 1,030 -> 1 (the exclusive-trie test) |

**Found on the way, and a precondition of the flip:** the engine's late settle
pass selected the words it hands ownership over by PROXY TAG. The trie's
duplicate-chain links share tag 1 with its state words. So a registered lock's
release was settled before the chain links it protects. A peer could then take
the lock while a link was still a proxy. It surfaced as a pre-existing SIGSEGV
in `inv_concurrent_insert_replace_nolist`:
- 28/1500 filtered runs before row 1;
- 40/1200 after row 1, which put a release next to a chain mark in one more
  commit;
- 0/1200 once the registered lock words settle last (`8c517957`).

---

## 7. Audit: does every fine / exponential lock holder check TOMBSTONE with the lock held? (Mathieu)

### 7.1 Where the check lives

Every DLM take in the tree goes through `ft_dlm_acquire_set_at`:
- `ft_dlm_lock` has exactly one caller, the acquire loop.
- `ft_acquire_member` wraps a one-member set.
- `ft_flip_txn_lock_or_guard_parent_ex` locks through `ft_acquire_member`.

So the check is structural, and it is in the SAME commit that takes the lock:

| what the site's write depends on | where it is checked |
|---|---|
| the lock word (per-node: the member itself) | `ft_dlm_lock` refuses `PROXY \| TOMBSTONE \| LOCK` and records `{s -> LOCK\|s}` against that exact `s` |
| a coarsened member's own word | `ft_held_anchor_sample_node` refuses `PROXY \| TOMBSTONE \| LOCK`; `ft_held_anchor_guard_node` validates the snapshot in the acquire commit |
| the coverer the anchor was derived from (exponential) | after the commit, resolved (§5.1) |
| a member deduped onto a word the op already holds | no peer can retire a held word; the op's OWN consumed fence is the designed `TOMBSTONE` dedupe arm |

What the choke point cannot see is a node a site WRITES without naming it as a
member. That is §1's coverage question, and rows 1-4 closed the measured cases
(§6).

### 7.2 The measurement

`-DFT_DEBUG_STRUCT_ANCHOR` gains two probes.
- **DEAD MEMBER.** After `ft_dlm_acquire_set_at` returns 0, a member whose node
  reads logically TOMBSTONE is counted and sampled. The count is by arm (taken /
  shared / shared-coarsened / coarsened-taken). The sample carries the acquire's
  `fn:line`.
- **DEAD-OWNER WRITE.** This probe covers every owner-bearing record whose
  verdict was reached through the owner. It resolves that owner's state at
  commit ENTRY, while the op's locks are held. An owner already TOMBSTONE before
  a commit that lands is a lock holder writing into a retired node. It is
  counted per (txn site, pc0, pc1) as `T_DEAD_HELD_OK` / `T_DEAD_UNCOV_OK` /
  `t_dead_ab`. It is sampled.

RED CONTROL, `FT_SA_RED_DEAD=1`: the acquire takes and samples TOMBSTONED words.

| leg (probe tree, `bil`) | owner words asked at a landed commit | dead members | dead-owner writes | suite |
|---|---|---|---|---|
| ft_inv exponential | 39,899,794 | 0 | 0 | 152/152 |
| ft_inv per-node | 44,612,485 | 0 | 0 | 152/152 |
| ft_unit exponential | 5,061,857 | 0 | 0 | 358/358 |
| ft_unit per-node | 5,105,301 | 0 | 0 | 358/358 |
| ft_inv exponential, RED | -- | 5 sampled (3 taken, 1 shared, 1 coarsened-taken) | 4 sampled | key loss (`an OK insert is NOT READABLE`), hang in test 27 |
| ft_inv per-node, RED | -- | 4 sampled (taken) | 5 sampled | key loss (2 reports), hang in test 27 |

The same legs give these record verdicts:
- NOLOCKS 0, UNHELD 0, SELF_ONLY 0 at both spacings;
- HELD 38.3M + held-coarse 1.7M (exponential);
- HELD 44.7M (per-node).

### 7.3 What the measurement does NOT cover

- **The always-MW lanes carry no owner**, so nothing asks about them: 21.4M
  (exponential) / 19.8M (per-node) records per ft_inv leg.
  - The root pointer and the cell sibling list are MW on purpose and have no
    lock holder to audit.
  - The rest should be SW under lock (§3), so they ARE a gap in this audit.
    That is HEAD_BACK 6.4M, PARENT_WORD 1.1M, PSO 0.4M, the 0.6M exponential MW
    retires, and the non-sibling part of CELL and DUAL_UNNAMED (not yet split).
    Asking about them needs the owner named on each record, which is the same
    step their conversion to SW needs.
  - `nr_keys` writers all hold the FT-wide writer lock (COARSE only).
- **Raw stores outside the record layer** (`ft_hlist_store_*` chain words,
  `ft_set_parent_excl` HIDDEN) never reach a record.
- **Bulk ops (WLOCK)** are excluded by design: the FT-wide lock with the point
  mode flip. The skeptic of §5.1 notes their re-homes carry no state guard, so
  §5's held-order argument rests on the drain, not the guard.
- A zero from the suites is not a proof over interleavings the suites never
  produce.

### 7.4 Raw tombstone reads (the §5.1 lesson)

A raw `state & FT_STATE_TOMBSTONE` can only UNDER-report: a parked record is
`ptr | FT_STATE_PROXY`, and bit 1 of an aligned pointer is 0. So a raw read is
sound exactly when its "not tombstoned" branch is followed by a take or a sample
that refuses PROXY. The coverer check broke that rule: it proceeded on a node it
never locks. Every other decision read found:

| site | use | verdict |
|---|---|---|
| `ft-remove.h` `ft_rm_holder_rehomed`, `_cds_ft_remove_locked` (`ft_flag_tombstoned` x5) | routes the plan (re-descend / retry / miss) before the acquire | conservative: a missed tombstone routes as live, and the acquire refuses the PROXY |
| `ft-remove.h` `ft_detach_node` climb (`ft_meta_tombstone`) | pre-lock bail of the in-place tier | pre-lock filter; the hoist's acquire re-checks the node it writes |
| `fractal-trie.c` `_cds_ft_debug_cow_replace_root`, `ft-rekey.h` x3, `ft-remove.h` `ft_detach_node` | scrubbing released marks ("consumed fence keeps answering holds()") | after `ft_meta_lock_release_if_held` waits out PROXY; an under-report scrubs, so the op re-takes and is refused |
| `ft_glue_held_snap_one` x2 | does a consumed free-list entry answer holds() | under-report answers "not held", so the op takes and is refused |
| `ft_dlm_acquire_set_at` dedupe | covering hold is LOCK, PROXY or TOMBSTONE | PROXY-inclusive |
| `ft_flip_txn_record_anchor_release*`, `ft_remove_one_commit`, `ft_glue_tombstone_free_list` | `urcu_txn_load` | resolved through the txn |
| trace, stats, verify, and the probes themselves | diagnostics | not a decision |

---

## 8. The live validation of a re-homed child, measured (Mathieu: "I worry about the live validation")

When an op re-homes a child whose state word it does not hold,
`ft_reparent_record_meta` records that word `{live -> live}` MW. This is a read
check: the commit aborts if the child was locked, retired or changed meanwhile.
The code gives its purpose as catching "a child a PEER froze mid-recompact".
`-DFT_DEBUG_LIVE_VALIDATE` (on top of `-DFT_DEBUG_TXN_KIND -DURCU_TXN_REC_DBG
-DFT_WINNER_DBG`) records, per re-home call site:
- how many checks were recorded;
- whether the child is its own anchor, and whether the op holds the child's anchor
  (with `-DFT_DEBUG_STRUCT_ANCHOR`);
- for each check that LOSES, what beat it, read from the value the losing CAS
  observed.

ft_inv, one leg each:

| re-home site | per-node: checks / lost (cause) | exponential: checks / lost (cause) |
|---|---|---|
| `ft_reparent_record`, skip-compressed child (its callers: the `ft_node_recompact` sweep, compaction's compressed relocation, the rekey COW, the bulk glue) | 187,662 / 1,910 (LOCK held by a member claim 1,792, all `ft_detach_orphan_planlock`; LOCK unclaimed 118) | 192,304 / **0** |
| ... internal child | 533,293 / 0 | 518,790 / 0 |
| ... compressed child | 183,158 / 0 | 197,348 / 0 |
| chain-compress fuse, the surviving child (`ft_record_child_back_edge`) | 92,772 / 4,839 (LOCK unclaimed 2,545, nr_child 2,141, member claim 153) | 87,110 / 235 (LOCK unclaimed 227) |
| split's live child (`ft_park_live_parent_edge`) | 20,398 / 77 | 23,714 / 86 (LOCK unclaimed 85) |

At exponential, **every** check on a child that is not its own anchor was made
by an op that already holds that anchor. That is 220,007 of 220,007 across the
five sites, where property 2 predicts it (§7). For those children the check is
redundant.

**RED CONTROL, `FT_LV_RED_DROP=1`:** the check is dropped inside the
span-preserving re-homes only. These are `ft_node_recompact`'s sweep and
compaction's compressed relocation, bracketed by `FT_LV_SPAN`. No anchor moves
there. A peer that retires or recompacts a child must rewrite the child's slot.
So it holds the parent, which the re-homer also holds.

| leg | checks dropped | suite | exclusion violations | dead members | dead-owner writes |
|---|---|---|---|---|---|
| ft_inv per-node | 994,183 | 152/152 | 0 | 0 | 0 |
| ft_inv exponential | 1,056,364 | 152/152 | 0 | 0 | 0 |
| ft_unit (both spacings) | 18,909 | 358/358 | 0 | 0 | 0 |

The baseline legs without the drop read the same zeros.

The two boundary-moving re-homes (the split's live child, the chain-compress
fuse) keep the check. The split's is what closes §5's held order (§5.1 red
control). There the re-homed node is a compressed node's SINGLE child and the
old anchor of everything below it. Taking its lock instead of validating it
avoids the fan the reverted lock-the-sweep experiment choked on.

---

## 9. Bulk ops take no node lock; compaction follows the mode flip (Mathieu, 2026-09-17)

**The rule.** A bulk op excludes every other op through per-FT locking. That
locking has two parts: the bulk lock, and point ops flipped onto the FT-wide
writer lock while the gate is open (G5.25). So a bulk op should take no node or
anchor lock at all. In bulk mode, point ops and compaction take BOTH the FT-wide
lock and their nearest-ancestor locks. They do so because the regime is mixed at
the gate's two edges:
- while the gate opens, ops that saw it clear hold node locks only until its grace
  period ends;
- after it closes, fresh ops are back on node locks while flipped ones may still run.

Only the bulk BODY, strictly inside the gate, may drop them.

**Compaction now takes part in the flip, per step.** `cds_ft_compact_step`
enters the writer scope inside its own read-side section, as a point op does.
Two things follow:
- the gate's grace period covers a step in flight;
- a bulk op that starts between two steps makes the next step queue.

Sampling once per `cds_ft_compact` pass did not work. The pass drops the read
lock between steps, and the gate's grace period completed in that gap. The step
keeps its relocations' node locks (row 4).

**The inventory** (`-DFT_DEBUG_BULK_ACQ`): every DLM acquire made inside a bulk op.
- ft_inv makes 1,201,459 (exponential) / 1,064,667 (per-node) of them, at 12 sites:
  - `ft_flip_txn_lock_or_guard_parent_ex` 740k;
  - `ft_node_recompact` 178k;
  - `ft_root_attach_fence_empty` 89k;
  - `ft_merge_lock_overlap` 59k;
  - `ft_graft_keylen` 21k;
  - `ft_split_compressed_graft_build` 21k;
  - `ft_rekey_cow_stop` 2.7k;
  - five sites below 120.
- ft_unit reaches 26 sites, including the rekey graft attempts and the glue marks.
- **Every one** is made under the FT-wide lock of its trie, or on an exclusive trie.
- **None** follows a drain seam (`ft_writer_lock_gp_wait`) earlier in the same op.
  The arm yield confirms that seams occur inside bulk ops: 825,704 / 708,654 in
  ft_inv and 45 in ft_unit.
- So no bulk-op take provides exclusion or stale-plan detection that per-FT locking
  does not already give.

**Removing those takes is QUEUED AS AN OPTIMIZATION (Mathieu: "having bulk ops
take fine-grained locking is not a correctness issue").** A first prototype
answered every bulk-op member at the acquire choke point as already held. It did
not survive, because bulk bodies are wired to a real take at about 124
lock-lifecycle call sites:
- `ft_meta_lock_release` 52, `ft_flip_txn_lock_register` 24,
  `ft_meta_lock_release_if_held` 16, anchor/held release records 16,
  `ft_flip_txn_record_release_lock` 9, fenced tombstones that expect `LOCK` 7;
- `inv_graft_root_swap_cross_view` registered the untaken lock and the commit's
  release sweep asserted;
- the rekey fold tests chained an MW record onto an SW one;
- the rig slowed from 250+ to 1-5 bulk cycles in 20 s.

Two shapes for later:
- **A (central):** a sentinel `lock_snap` for "held by per-FT exclusion, never
  taken". Four places honour it: register, the release records, the fenced
  tombstone and a bulk-scoped `ft_meta_lock_release`.
- **B (per site):** each bulk site stops acquiring and drops its lifecycle code.

`doc/design/ft-compact-bulk-rig.c` is the harness for it.

### 8.1 The live check removed where it observed an exclusion the op already holds, and TAKEN where it did not (Mathieu: "let's try" it)

**A. Span-preserving re-homes, no check.** `ft_node_recompact`'s sweep and
compaction's compressed relocation now pass `check_child = false`. No anchor
moves. A peer that could freeze or retire the child must rewrite its slot in the
node being copied. The re-homer holds that node's lock.

**B1. The split TAKES its live child.** A compressed split ADDS a boundary above
cn's only child without retiring that child. So everything anchored on the child
re-anchors under the fresh junction. `ft_insert_dlm_acquire_split` now takes the
child (when internal) in the same lock set as {cn, P}, and re-reads `cn->child`
under cn's lock. The lock is carried on the insert commit like P's. It is
released on a pre-publish bail. It is registered with its release record in
`ft_insert_publish_or_park` before the arm. `ft_park_live_parent_edge` asks the
txn registry, so a held child records no state edge.

Rig `splitheld` (the §5 held order):

| tree | split while the victim holds A | peer while held |
|---|---|---|
| child locked | waited | waited |
| RED `-DFT_RED_SPLIT_NO_CHILD_LOCK` (child neither locked nor checked) | COMPLETED | COMPLETED -- **HOLE, 3/3** |

**B2. The fuse, no check.** The chain-compress fuse moves boundaries only by
REMOVING them. It removes the boundary node and a fused compressed child. It
retires and holds both. A node anchored on either one is excluded by the fuse, and a plan
made before it hits the tombstone. A node anchored on the surviving child or
below keeps its anchor. Its per-node losses (4,839 of 92,772) were aborts for
nothing.

Measured with `-DFT_DEBUG_LIVE_VALIDATE` on ft_inv (per-node / exponential):

| | before | after |
|---|---|---|
| live checks recorded (all sites) | ~1.02M / ~1.02M | 150 / 307 (the split's untaken children, 0 lost) |
| checks lost | 6,826 / 321 | 0 / 0 |
| always-MW STATE records | 984,569 / 1,673,695 | 150 / 646,693 (the rest: MW anchored retires) |
| exclusion violations, dead members, dead-owner writes | 0 | 0 |

**Skeptic: refuted=false.** Two hardening items, both applied:
- the re-read of `cn->child` under cn's lock is unconditional, so a child that
  became internal after the plan is re-planned, never re-homed unlocked;
- the child's release is recorded through the txn
  (`ft_flip_txn_record_anchor_release_held`, as P's is), not as a fixed
  `{LOCK|snap -> snap}`.

The skeptic's open question was LIVENESS. The split now also conflicts with ops
that hold the child but not cn. And escalation rescues a refused commit, not a
refused acquire. The measurement uses `-DFT_DEBUG_LIVE_VALIDATE` counters in
`ft_insert_dlm_acquire_split`, on ft_inv. It compares against the same tree
with the child lock compiled out:

| | split acquires | with a child member | refused | longest refusal streak (one thread) |
|---|---|---|---|---|
| per-node, child locked | 1,035,083 | 24,367 | 87,592 | 132 |
| per-node, no child lock | 1,044,292 | 0 | 101,638 | 545 |
| exponential, child locked | 923,969 | 27,903 | 123,205 | 134 |
| exponential, no child lock | 931,228 | 0 | 100,605 | 150 |

No starvation signal. Refusals move by run-to-run amounts in both directions, and
the longest streak is not worse. Only about 2.5% of splits have an internal child
to lock.

## 10. The unfinished lanes' owners, and the (parent, offset) pair stored plain (2026-09-17)

### 10.1 Who should lock each unfinished lane, and is it held

`-DFT_DEBUG_STRUCT_ANCHOR` now asks each unfinished always-MW lane (`ft_sa_lane_ask`)
against the owner the register names for it:
- a head back edge: the chain holder that its old value names (compaction's cell
  relocation: `cell->parent`);
- `parent_word` and `parent_slot_offset`: the parent being replaced;
- `_cds_ft_insert_replace`'s structural edges: the node that contains the slot.

It gives the same verdicts as an owner-bearing record, plus `O_CHILD_ONLY`: not
covered by the owner, but the op holds the child's anchor. A head back edge is
asked with the site's own lock context (`-DFT_DEBUG_BACK_EDGE_OWNER`). Some holds
live only in the context's `extra[]` frames, and without it the probe reads
93k-310k uncovered records that are in fact held.

| ft_inv | asked | uncovered | child-only |
|---|---|---|---|
| per-node | 8,666,129 | **0** | 0 |
| exponential | 8,381,861 | **4** (1 recompact head back edge, 2 fuse `parent_word`, 1 insert_replace) | 1 |

ft_unit: 0 uncovered at both spacings. The 4 were re-asked at commit, and one arm of
`_cds_ft_insert_replace` was never asked: see §10.3.

### 10.2 The pair stored plain on a reachable node

`ft_resolve_parent_slot` treats `parent_word` as a sequence word for the offset.
So the precondition is: every offset change goes through a parked `parent_word`,
or the node is invisible. `incoming_byte` is read by the key up-walk beside the
parent, with nothing parking it.

`-DFT_DEBUG_PAIR_STORE` asks at every store that CHANGES the offset or the byte:
is the node reachable through its current pair, all the way up to the trie's
root slot? Hits are counted per call site.

☠ Two earlier versions of the climb read false positives:
- one hop: a fresh cluster built bottom-up holds its own children;
- a root-position parent word taken as the root: a fresh node is stamped with its
  owning trie before it is published. The climb now requires `owner->root` to hold
  the top, and the owner not to be exclusive.

That second hole alone made up the whole incoming_byte "producer": 295 per ft_inv
leg, from the merge build and `ft_node_recompact`'s `FT_EXCL_HIDDEN` arm.

| ft_inv, per-node / exponential | result |
|---|---|
| incoming_byte changed on a reachable node | **0** / **0** |
| control: same-value byte stores on reachable nodes | 1,050,066 (per-node) -- the climb answers yes |
| offset changed on a reachable node | 36,335-56,246 / 42,491-50,947, **one site**: `ft_glue_record_back_edge` → `ft_set_parent_slot` |

ft_unit: 0 and 0.

The glue site stores a live child's offset plain before the commit parks its
`parent_word`. It is unobservable, for three reasons:
- **readers** never read the offset: the up-walk and `ft_skip_to_compressed` read
  `parent_word` only. The one iterator use is an `FT_DEBUG_PARENT_VIOLATION` dump;
- **other writers** are excluded: every caller is a bulk op. Its gate is
  published and followed by a grace period. Point ops and compaction steps
  then take the FT-wide lock (§9). ☠ So this is a DEPENDENCY on the FT-wide lock,
  the same one `ft_glue_record_back_edge`'s FT-SLOT-3 note names;
- **the op itself**: a per-thread ring notes each glue store and checks it in
  `ft_resolve_parent_slot`, cleared when the outermost FT-wide scope releases.
  0 torn reads of 59,433 (per-node) / 73,167 (exponential) noted.
  RED `-DFT_DT_GLUE_RED` resolves right after the store: 67,880 of 67,880.
  `ft_txn_parent_slot_at` reads both words through the txn, so it is coherent by
  construction.

### 10.3 Every DUAL_UNNAMED record asked, and the uncovered re-asked at commit

**Commit-time re-ask.** An uncovered lane record is kept on the txn. It is
asked again when its commit goes to the engine, this time without the site's
lock context:
- `c_LATE`: covered by then;
- `c_NEVER_OK`: still uncovered, and the commit landed;
- `c_NEVER_AB`: still uncovered, and the commit aborted;
- `c_BAILED`: the txn never reached the engine (an acquire miss, an op bail).

A LATE report prints the registry and ledger counts at the record and at the
commit. If they are unchanged, no lock arrived in between, so the owner was
covered at the record too. RED `FT_SA_RED=root` (ft_inv exponential): late
2,697,912, NEVER_OK 24,941,870, never_ab 57,301, bailed 59,932, overflow 337,714.

**The arm that was never asked.** §10.1 said the hot DUAL_UNNAMED path was
unasked. That was a misread: the ask with 2 hits per leg is the list-off leaf
arm's SKIP_X variant. Its single-edge ask read 493,072 (exponential) / 582,596
(per-node) asked, 0 uncovered. The arm that had no ask was the LIST-ON leaf arm
(`ft_ord_cell_swap_publish_multi`). DUAL_UNNAMED is filed by `_cds_ft_insert_replace`
alone, every arm of it. The per-site split (`mwa5`) names no other producer. Its
edges are built by hand and never pass `ft_pub_rec_add_at`, so the recorder that
read `unnamed = 0` never saw them.

Asked at the list-on leaf arm, per leg:

| | ft_inv per-node | ft_inv exponential | ft_unit |
|---|---|---|---|
| single forward edge (owner P) | 10,620 held | 10,313 held (4 exact, 10,309 via an ancestor) | 1,713 held |
| SKIP_X variant, `cn->child` (owner cn = P) | 15,729 held | 10,279 held | 130 held |
| SKIP_X variant, the dual (owner GP) | **15,729 UNCOVERED, all NEVER_OK** | **10,279 UNCOVERED, all NEVER_OK** | **130 UNCOVERED** |

The leaf replace under a compressed parent wrote the grandparent's dual slot
holding only {P}. §8.1 A drops the live check on a premise: a writer of a
child's slot holds the node it lives in. This writer broke that premise. The
list-off arm has the same shape, reached about once per leg. `cds_ft_replace`
already takes GP (`ft_lock_skip_dual_gp`, §9.3's third member).

**Fix.** Both leaf arms take GP with `ft_lock_skip_dual_gp` after {P}. They
derive the edges after that acquire (`ft_insert_replace_leaf_sedges`), from the
same read-your-own-writes derivation the acquire used. Each reserves one more
record. The records stay MW.

After, per leg (the `gp`/`gl` legs):

| | uncovered | re-ask at commit |
|---|---|---|
| ft_inv per-node x2 | 0 | -- |
| ft_inv root-only | 0 | -- |
| ft_inv exponential x7 | 0-4 per leg, all `UNDATED` at the record: 14 total, 1 covered through its child | the other **13: all LATE, 0 NEVER_OK**; the 8 printed with counts all read `locks 1->1 ledger 0->0` |
| ft_unit per-node, exponential | 0 | -- |

The dual asks now read held: 7,948 of 7,948 (per-node), 12,331 and 9,156
(exponential), and held_coarse at root-only. The exponential residue is head
back edges (txn sites ft-insert.h:1163 and ft-remove.h:4785) and the fuse's
`parent_word`. The climb that dates the owner reads the words without locks, and
could not date it past a peer's park. At commit the owner's anchor is held, and no
lock was taken in between. **No lane record commits uncovered.**


## 11. Readiness for the MW→SW flip, measured (2026-09-17)

The flip happens once, after the lock-set transition is complete at FINE and
EXPONENTIAL (Mathieu). This section records what the instruments say about
each class that is to become SW, and what is still open.

### 11.1 What the flip changes

Per the register (fractal-trie-internal.h), a structural slot at exponential
and root-only is MW *by construction*: `ft_flip_txn_arm_per_op` refuses any
spacing but per-node (`ft_txn_per_op_spacing_ok`, "door 2"). The refusal
exists because `ft_flip_txn_owns` is exact at per-node and anchor-blind above
it. So the flip is two changes that must land together:
- lift door 2, with an owner witness that resolves the anchor. Otherwise the
  debug owner assert would fire on every coarse leg, as it did when measured
  before;
- convert the unfinished always-MW lanes (§3, §10) to owner-dispatched records,
  the exponential anchored retires, and the not-held `nr_child--`.

`-DFT_DEBUG_STRUCT_ANCHOR`'s record-time inventory is the anchor-aware
witness that door 2 lacks.

### 11.2 Measured, per class (ft_inv, probe trees `blane4` / `bchain3`)

| class | per-node | exponential | root-only |
|---|---|---|---|
| owner-bearing records (MW_STRUCT surface) | 0 unheld | 0 unheld; UNDATED re-asked, see 11.3 | 0 unheld; 6 UNDATED, all LATE |
| unfinished lanes | 0 uncovered | 0 NEVER_OK (§10.3) | 0 |
| duplicate chain (`-DFT_DEBUG_CHAIN_HOLD`) | 4 unheld | 19 unheld | 1 unheld |
| anchored retires, P1/P2 (§7, 09-17) | -- | 1,160,550 of 1,160,552, 0 exceptions after rows 6-8 | -- |

ft_unit: 0 at both spacings on every row.

**The chain audit had no exclusive-trie bucket.** It scored an exclusive trie
(a bulk product or source, no peer) as UNHELD. At the in-place delete's fused
freeze_leaf, that was the entire residue: 57,823 per ft_inv per-node leg and 24
in ft_unit. A probe splitting the same population on `ft->exclusive` confirmed
it one for one. Bucketed now (`exclTrie`). The site then reads 0 at every
spacing (149,152 / 119,208 exclusive records per leg).

☠ A first probe at that site counted "registry and ledger both empty" and
pointed at the external promote. It was blind to the orphan plan-lock marks
in `lctx`'s extras, which the audit counts as HELD(ctx). A lock hoist written
from it closed nothing and was reverted.

### 11.3 Closed (2026-09-17)

The five open items, in order. Probe trees `blane4` (struct anchor) and
`bchain3` (chain audit, `-DFEATURE_FT_HOLD_TRACE`). All legs green.

1. **Chain, `ft_detach_node:3941`: fixed.** The freeze sat in the helper's
   retiring arm (`topmost=0`, no orphans): the leaf hangs straight under `cn`,
   which only the helper's {src_cn, pub_parent} set takes. Recording the freeze
   in the caller first was a record made holding nothing. It now happens inside
   `ft_detach_node_replace_compressed_parent`, after each arm's acquire (and on
   the bulk split re-home return). Chain audit: 0 violations at every spacing.
   The moved row reads held: 47,618 per-node (own lock), 69,923 exponential
   (17,022 own + 52,758 ancestor), 41,800 root-only (ancestor).
2. **UNDATED owner-bearing records: settled.** The record keeps the owner's held
   ancestors (`anc_rec`). At commit the report says whether the owner's anchor
   was among them. For an owner still undatable, it says whether every ancestor
   held at commit was already held at the record. 8 exponential legs: every LATE
   record reads `anchor_held_at_record 1`, and the 7 still undatable read
   `ancestor_held 1`, subset 1. No hold arrived between record and commit on
   any of them.
3. **The fuse's `parent_word` lane: settled.** Its `locks 1->2` is an orphan
   anchor registered after the record, not the owner's. The owner's anchor was
   already held at the record in every sample (`anchor_held_at_record 1`).
4. **Unscored rows: scored.**
   - Every chain-store call site now has a FULL audit row. The three remove_all
     freezes that had none (the NIL key's two, the prefix clear) read held:
     registry, or ancestor at root-only.
   - HIDDEN declarations are now tested. Is the target reachable, through its
     current (parent, offset) pairs up to a non-exclusive trie's root slot, or
     for a head through its holder's slot? Per ft_inv leg: 5,616,097 /
     4,848,829 / 5,042,418 asked (per-node / exponential / root-only), **0
     reachable**; ft_unit 1,331,448, 0.
   - Controls: LOCKED in-place body writes read reachable at ~100% (counters
     are unlocked, so within a few dozen). Live removed leaves (heads) read
     reachable at 99.95%; the rest are heads moving mid-scan.
   - ☠ The control first read 3% at two sites. The climb did not resolve a
     SKIP_X dual, which names cn's child and not cn, so every path through a
     skip-compressed node read unreachable. The head test also asked a
     compressed holder by byte. Both are fixed; without the control, the
     HIDDEN zeros were blind there.
   - `ft_popcount_node_set_nth:516` is the REFUSED arm (`-ERANGE`, no store).
     Its `lockVIOL` counts callers that declined the in-place tier, on nodes
     with no parent word yet. Not a write.
5. **The not-held `nr_child--`: split and asked.** `ft_remove_one_commit`
   records it directly, with `FT_STATE_PROXY` spelled out, under its own
   always-MW class `NR_CHILD_DEC`. It is the same MW record as before, no
   longer counted in CELL. Per ft_inv leg:

   | | per-node | exponential |
   |---|---|---|
   | fine, shared trie | 0 | 536,499, anchor held (`ft_sa_anchor_props`) |
   | coarse trie, FT-wide lock | 286,007 | 273,252 |
   | exclusive trie | 104,643 | 95,269 |
   | fine, bulk (FT-wide lock) | 0 | 25 |

   Root-only: 2,084,317 of 2,084,321 anchor held, 4 undatable.

Gate after the batch, clean rebuild: 18/18 GREEN. `bf-nsk`
(`NO_FEATURE_FT_SKIP_COMPRESSED`): ft_unit and ft_inv at per-node and
exponential, GREEN.

### 11.4 Queued for the flip (Mathieu, 2026-09-17) -- ☑ CLOSED BY MEASUREMENT

☑ **CLOSED @659bc54f, and by measurement rather than by work.** This item
assumed the not-held `nr_child--` had a FINE-trie population to convert. It does
not: NR_CHILD_DEC reads **0 on a fine trie at all three spacings** (§11.6).
Every one of its records is on a coarse or exclusive trie. §12.1.1 now parks it
there.
The text below is the original statement of the item.

**`ft_remove_one_commit`'s not-held `nr_child--` keeps its MW kind until the
flip.** `ft_flip_txn_owns` (the per-node lock registry) chooses its kind. So it
takes the hardcoded-MW branch wherever the registry is empty. That includes the
tries that door 1 arms SW trie-wide:

| per ft_inv leg | per-node | exponential |
|---|---|---|
| coarse trie (FT-wide writer lock) | 286,007 | 273,252 |
| exclusive trie (no peer) | 104,643 | 95,269 |
| fine, shared (op holds the ANCHOR, an ancestor) | 0 | 536,499 |
| fine, bulk (FT-wide lock) | 0 | 25 |

Nothing chose MW for the first two rows. The txn around them parks every other
state-word record SW (`ft_txn_content_sw_ok`). The other producers of this very
word (`nr_child++`, the tombstone) park SW there too. So the MW record is the
odd one out. It is harmless only because those tries exclude every peer. MW
stays correct meanwhile (a CAS that arbitrates against nobody).

The third row is door 2's anchor-blindness. The op holds the holder's anchor,
and `owns()` looks for the holder itself. ⇒ At the flip:
- pick the kind from the trie's mode AND an anchor-aware ownership answer;
- correct the register row (`metadata.state nr_child--`). Its own note (2) does
  not back its "MW [DESIGN]". That note is about the TAG (FT-SLOT-1), not the
  kind. The code's "self-guarded by this very CAS" comment predates the
  leaf-delete hoist.

### 11.5 The flip, spacing axis: door 2 opened for exponential and root-only

Mathieu chose both coarse spacings, and the chain class in scope (see §11.6 for
the rest). Door 2 never refused the spacing. It refused the PREDICATE.
`ft_flip_txn_owns` compared the registry's words against the node a record
names. A coarse spacing registers that node's ANCHOR instead. So it answered
"miss" for a word that IS excluded. Under an arm, a false miss is fatal.

Four changes make the answer exact, and one makes a wrong answer harmless:

1. **The acquire's answer travels with the registration.** `ft_held_anchor`
   carries the MEMBER its word was taken for. `ft_flip_txn_lock_register_held`
   files both, and `ft_flip_txn_holds` matches either. Only the acquire knows
   this (the anchor comes from the op's descent).
2. **Members whose word deduped onto another member's** file no entry at all.
   They owe no release. The acquire records them in the txn's `@covered` list.
   Three sites hand a shared member on, and they say so explicitly: the split's
   P and child, and the fuse's retire and publish parent.
3. **ROOT-ONLY needs no descent:** every node's anchor IS the trie's root. So
   `ft_flip_txn_owns` resolves it directly from the txn's trie (`@ft`, a
   production field). The debug copy it replaces was read NULL in a core.
4. **The kind is decided PER RECORD** (the former `-DFT_SW_REQUIRES_OWNER`). A
   per-op armed txn parks SW only where it owns the record's owner. Otherwise
   it records MW. So an arm can no longer convert a word the op does not own.
   That structural weakness made the arm's "registry is non-empty" gate too
   coarse. The park and the assert now share ONE predicate.
   ☠ A whole-body SW writer (the rekey / root-COW driver, `@sw_body`) is
   EXEMPT. Its edges sit on words it fenced itself. So an MW fallback aborts
   its own commit forever. Until the flag existed, this was measured as a
   livelock at ft_unit test 111, then as a 16G leak at test 117.

**Debug gate, `--enable-rcu-debug` with the owner assert armed, 6 legs GREEN:**
ft_unit 358/358 and ft_inv 152/152 at per-node, exponential and root-only.
That assert named each gap, one at a time. The last two did not reproduce
under gdb. They needed a self-diagnosing dump and a core.

**Positive control** (same tree, door 2 shut vs open, per ft_inv leg):

| | armSW | SW records | MW_STRUCT | OWN_MISS | ABORT |
|---|---|---|---|---|---|
| exponential | 3,531,046 → **9,624,073** | 5,714,002 → **11,662,220** | 39,598,068 → 35,674,503 | 1,620,759 → 1,121,390 | 654,580 → 744,820 |
| root-only | 3,224,762 → **7,323,652** | 4,435,496 → **8,060,515** | 25,881,500 → 23,443,122 | 5,242,617 → **1,928,817** | 1,782,700 → 1,581,854 |
| per-node | 10,879,803 → 10,281,676 | 13,579,840 → 12,452,716 | 39,359,409 → 38,564,634 | 1,847,956 → 1,691,751 | 859,505 → 699,931 |

Per-node loses about 8% of its SW parks: that is change 4 refusing to park
where the registry cannot confirm the owner. MW is correct there, just a CAS
that arbitrates against nobody, and its aborts fell too.

☞ USER-VISIBLE VALUE STILL WAITS ON THE API.
`cds_ft_group_attr_set_lock_spacing` still refuses both coarse spacings unless
`FEATURE_FT_ANCHOR_VALIDATE` (§12.3). The flip makes them correct. Graduating
exponential is a separate decision.

### 11.6 Still owed for the flip -- MEASURED, not listed from memory

This section used to be a list. It is now the per-class FINE-trie residue of
`ft_flip_txn_record_tag_mw`. That residue IS the remaining conversion surface.
Everything door 1 retires (§12.1.1) is gone from it by construction. So what is
left is what a FINE trie still CASes. `-DFT_DEBUG_MWA_DOOR1`, per leg, all four
legs GREEN:

| class | inv per-node | inv exp | inv root-only | ft_unit | verdict |
|---|---|---|---|---|---|
| RANK | **0** | **0** | **0** | **0** | ☑ DISCHARGED -- coarse-only, retired by §12.1.1 |
| NR_CHILD_DEC | **0** | **0** | **0** | **0** | ☑ DISCHARGED -- ditto (closes §11.4) |
| PSO | -- | -- | -- | **0** | ☑ DISCHARGED with FT-SLOT-3 (§11.7) |
| PARENT_WORD | 65 | 65 | 65 | 2,283 | ☑ at its floor: the SAME 65 as §11.7, compaction's own lockless txns |
| HEAD_BACK | 106,049 | 105,300 | 4,000 | 620,609 | ☑ converted §11.8; this is the not-owned residue |
| DUAL_UNNAMED | **0** | **0** | **0** | 0 | ☑ DISCHARGED -- the owners are named (below) |
| STATE | 0 | 678,940 | 5,144,495 | 0 | ✖ a VALIDATE -- must stay MW (§12.1.1 rule 1) |
| GUARD | -- | -- | -- | 61 | ✖ a VALIDATE -- must stay MW |
| ROOT | 1,196,702 | 1,026,182 | 1,150,475 | 638 | ✖ [DESIGN] -- a root lives in no node |
| CELL | 6,983,744 | 7,265,261 | 6,844,722 | 2,029,561 | ✖/☐ ALIASED: see below |

**What this changes.** The old list named RANK and NR_CHILD_DEC as owed. Both
read ZERO on a fine trie at every spacing. They only ever occur on
coarse/exclusive tries, and §12.1.1 retired them there. §11.4's queued item is
closed by measurement, not by work.

☑ **DUAL_UNNAMED IS DISCHARGED**, and it was plumbing in the most literal sense.
The owners were ALREADY DERIVED in `ft_insert_replace_leaf_sedges`. But that
happened only inside `FT_IR_SEDGE_LANE_ASK`, which is compiled out without
`-DFT_DEBUG_STRUCT_ANCHOR`. Production filed NULL and took the hardcoded-MW arm.
Now:
- the three edges carry their owner: {P} for the in-parent slots, and
  `ft_owner_of_parent_word(cn)` for the SKIP_X dual in the grandparent;
- both leaf arms CAPTURE `ft_lock_skip_dual_gp`'s answer, instead of discarding
  it with a `(void)` cast. So `owner_held` is the acquire's verdict rather than
  an assumption.

☞ NAMING IS SAFE BY CONSTRUCTION. That makes this change a different shape
from §11.9's. `ft_flip_txn_record_tag` routes to the per-record gate, and that
gate RE-ASKS `ft_flip_txn_owns`. A named owner the op does not hold answers MW
exactly as before. `owner_held` selects the route; it does not decide the kind.
A mis-derived owner costs a conservative MW, never a wrong park.

☞ AND IT MAKES THE SITE AGREE WITH THE SLOT'S OTHER WRITERS. The dual's other
producers already record it owner-keyed. So THIS site's unconditional MW was
the odd kind out on a slot the others park. The change is the reverse of
introducing a mixed kind. It also retires a "stay MW" branch. The fuse rule
says that branch is wrong from the day the class converts.

Measured at `_cds_ft_insert_replace`'s leaf-replace txn, ft_inv per-node:
- **MW_ALWAYS 433,125 -> 1**;
- OWN_HELD 128,267 -> 254,834;
- the class is ABSENT from the census at all three spacings.

☠ The records do NOT park yet. That txn arms SW on 125,111 of 401,153
creations, so the gate mostly answers MW_STRUCT. The records are now on the
ordinary conversion surface, asked and answered. Before, they were hardcoded
where no instrument could reach them. Parking is door 2's business, not this
change's.

☑ **THE CELL ROW IS NOT ALIASED -- I HAD CONFLATED TWO INSTRUMENTS.** This
table is the MW_ALWAYS class census. Only `ft_flip_txn_record_tag_mw` files it.
There, `tag != FT_FLIP_PROXY_TAG` inside `ft_ord_cell_flip_into` chooses
`FT_TK_MWA_CELL`. That is the ORDERED-CELL list alone, `[DESIGN]` MW. The
duplicate chain never reaches that recorder: `ft_hlist_store_*` call
`urcu_txn_store_mw/sw` DIRECTLY. ⇒ With DUAL_UNNAMED discharged, **this table
has no open conversion item left**.

☠ THE ALIAS WAS IN A DIFFERENT COUNTER, and it is now split (§11.6.1).

### 11.6.1 The duplicate chain, sized at last (the cell alias split)

`ft_hlist_store_mw_at` counted every duplicate-chain store as `cell_mw`, the
same bucket the ORDERED-CELL list writes from
`ft_ord_cell_insert_after_prepare`. Its own header asked for the split:
*"Sharing one bucket means no instrument can tell them apart, and any 'MW is
correct here' reasoning earned by the cell list reads as though it covered the
chain."* Both reach the engine handle directly. So neither is site-attributed,
and neither could be SIZED. They now count apart (`chain_mw`/`chain_sw`).

☠ AND THE TAG CANNOT DO THIS JOB. `FT_HLIST_TAG` **IS** `URCU_TXN_TAG` (1), the
ordered-cell tag. So a tag test classes a chain word as a cell. The PRODUCER
names the class ⇒ [[a tag cannot name a word class]].

| per leg | ordered-cell MW | ordered-cell SW | **duplicate chain MW** | chain SW |
|---|---|---|---|---|
| ft_inv per-node | 3,189,982 | 0 | **14,558,007** | 1,310,138 |
| ft_inv exponential | 3,561,002 | 0 | **13,312,744** | 1,361,325 |
| ft_inv root-only | 2,971,680 | 0 | **7,427,938** | 1,380,789 |
| ft_unit per-node | 1,350,026 | 0 | **682,006** | 1,514 |

⇒ **The duplicate chain is the LARGEST remaining MW population in the tree.**
It is an order of magnitude above every row of §11.6. It is roughly 4.5x the
ordered-cell list it was hidden inside. Its already-SW share is the coarse /
exclusive dispatch plus `ft_glue_record_splices`. The ordered-cell list reads SW
= 0 at every spacing, as `[DESIGN]` predicts.

This is the size of the prize for §11.9's six-item work list, and the number to
watch when it is attempted again.

### 11.7 FT-SLOT-3 settled: the back edge belongs to the PARENT

Mathieu asked what is wrong with keying the kind on the CHILD, as the code did,
and whether the model might be outdated. It is not, for a mechanical reason. A
re-home changes WHICH SLOT names the child. So every writer of that word
rewrites the slot in the parent, and therefore holds the parent. Only some of
them also hold the child. The parent is what makes those writers exclude each
other. The recompaction sweep re-homes every child of the node it copies, and
holds none of them. Keying on the child left that sweep permanently MW.

Taken literally, "lock both" would mean the sweep locks each child. That is the
fan of up to 256 acquires that was implemented in full and reverted ("a
contended child fails the acquire, and escalation cannot rescue it"). The
parent rule needs NO new acquire.

Priced first (`-DFT_DEBUG_SLOT3`, per ft_inv leg, one producer):

| | total | parent held | child held | neither |
|---|---|---|---|---|
| per-node | 1,246,659 | 1,246,594 | 25,762 | 65 |
| exponential | 1,390,475 | 1,390,410 | 24,864 | 65 |
| root-only | 1,272,917 | 1,272,852 | 1,162,228 | 65 |

Every record the child licensed, the parent licensed too. So the switch loses
nothing. The identical 65 are compaction's own txns (ft-compact.h:92 / :418,
`nr_locks` 0). The API leaves their exclusion to the caller (§12.1), and they
stay MW.

After the switch, per leg at EVERY spacing: **PARENT_WORD 65, PSO 0** (from
1.22M-1.37M and 0.42M). `@child_held` survives as a legacy branch, counted. It
fires 5-18 times per ft_unit leg, with **FINE SHARED = 0**. So it fires only on
tries whose exclusion is trie-wide. There door 1 decides the kind, not the
named owner. Both branches record the same kind, so one word can never get two.

### 11.8 A head's back edge joins the SW transition (the same rule)

`ft_back_edge_owner` already names the owner Mathieu decided on 2026-09-03: the
PARENT the head hangs from. That is FT-SLOT-3's rule for a metadata child's back
edge (§11.7). The lane probe had measured that owner held (0 uncovered, 0
NEVER_OK). So the lane converts the same way. The record names the owner, and
the per-record gate parks SW where the op holds it.

☠ THE CALLER RESOLVES THE OWNER, not the recorder. Only the caller knows what
the word's OLD VALUE IS. At compaction's cell relocation it is the CELL, not a
parent word, and a derivation inside the recorder would misparse it.
`ft_flip_txn_record_head_back_edge_owned` takes the owner. The old spelling
passes NULL and stays always-MW (compaction's site keeps it).

Ten producers converted:
- the re-parent arms;
- the recompaction back edge;
- the insert's src-use relocation;
- the child back edge;
- the glue's two arms;
- the rekey re-parent;
- the detach's compressed-parent pair and its unchain.

Per ft_inv leg:

| HEAD_BACK records | before | after |
|---|---|---|
| per-node | 6,125,292 | **146,304** |
| exponential | 5,954,727 | **157,306** |
| root-only | 5,962,536 | **38,662** |

Gate 18/18 GREEN, probe tree with the owner assert armed 6/6 at the three
spacings, NO_FEATURE_FT_SKIP_COMPRESSED 6/6.

### 11.9 The duplicate CHAIN class: CONVERTED (2026-09-19)

☑ **THE CLASS PARKS SW BY DEFAULT.** `cds_ft_node.next` / `.prev` dispatch
`urcu_txn_store_sw` unconditionally. `-DNO_FEATURE_FT_CHAIN_SW` builds the MW
arm back as an ablation, and the gate runs it (config `chainmw`, with `imw`).
`imw` is needed because the flip changes a multi-writer property. An SW abort
writes `old_ptr` back blind, where a failed MW CAS writes nothing.

Proof that the default parks, not merely that it compiles
(`-DFT_DEBUG_TXN_KIND`, ft_inv per-node): chain **MW = 0, SW = 14,600,409**.
The ablation arm reads MW = 13,629,079 / SW = 1,346,661. This is the largest
conversion in the tree.

**Gate, 20 legs, all green with matched plans**:
- default and ablation, ft_inv 153/153 + ft_unit 361/361 at all three spacings
  (12);
- the six same legs under `--enable-rcu-debug`. It arms the engine's nine
  `urcu_assert_debug` self-checks, *including* the
  SW/MW-kind-conflict-on-one-slot detector. The detector stayed silent throughout. A two-line TU verified its
  arming; it was not assumed;
- `NO_FEATURE_FT_SKIP_COMPRESSED` ft_inv 153/153 + ft_unit 361/361. In this
  config the 09-17 attempt saw its one unreproduced red.

☠ **READ THE HISTORY BELOW BEFORE TOUCHING THIS.** The class was attempted and
withdrawn TWICE, and *both attempts gated green*. On this surface, an
instrument that could not see has twice produced a green gate. So the gate is
not what licenses the conversion. The six refutations being PAID license it,
each by a measurement recorded in `ft_hlist_store_chain_at`'s header. The gate
is the last check rather than the argument.

---

#### The history: step 1 is DONE, step 2 was ATTEMPTED and REFUTED

☞ **Read the 2026-09-19 subsections at the end before the middle ones.** The
first verdict recorded there was "the exclusion does not exist". It came from a
witness that cannot see a registered hold. It has been WITHDRAWN and
re-measured. The flip is still parked, for a shorter and differently-placed
list of reasons.

Mathieu's design for this class is two steps: *"add the missing lock acquire of
nearest parent -- in some cases those may already be held.  Once this is
thorough, ONLY THEN can we switch the chain mutation ops to SW."*  Under the
governing rule: **the duplicate chain is protected by the NEAREST ANCESTOR
LOCK.  Period.**

#### ☑ Step 1 is done, and the ops' own lock sets did it

Re-measured at @d6550af9 (`-DFT_DEBUG_CHAIN_HOLD -DFEATURE_FT_HOLD_TRACE`),
every FULL row of the chain hold audit:

| | ft_unit | ft_inv |
|---|---|---|
| per-node | 0 UNHELD | 0 UNHELD |
| exponential | 0 UNHELD | 0 UNHELD |

`FT CHAIN-DATE on_descent=3,963,730 no_holder=0`; `derived(off-path)` 0 in
seven of eight audit legs and **2** in one exponential ft_inv leg. This class
was blocked on one gap: `ft_detach_node`'s fused `freeze_leaf` (2,340,730
UNHELD), plus 678 members reachable only by a back-pointer walk. That gap closed
when the delete tier converted. `ft_lock_chain_holder` was written for step 1
and never wired. It is not needed for the acquire.

☞ BUT THE AUDIT ANSWERS A WEAKER QUESTION THAN THE FLIP ASKS. Its HELD verdict
is a UNION of three witnesses:
- `ft_flip_txn_owns` (the registry);
- the per-thread hold LEDGER;
- the lock CONTEXT.

Only the first can license a park. The ledger is debug-only
(`FEATURE_FT_HOLD_TRACE`), and a record helper has no ctx. **So "0 UNHELD" does
not certify the dispatch.** Reading it as the step-2 gate was the error below.

#### ☠☠ Step 2 attempted, and refuted before landing

The attempt made a chain word's kind the ordinary ownership question
(`ft_chain_word_sw_ok`):
- coarse -> SW;
- the txn owns the chain's HOLDER -> SW;
- else MW.

It asked once per prepare. Two expected-olds are caller-DERIVED:
`ft_hlist_freeze_sole_prepare`, and the TAIL of `ft_hlist_freeze_chain_prepare`.
The attempt re-read them under the lock, so a park could only happen on a value
read there. It measured 98% converted on ft_unit and 63-77% on ft_inv. It
passed the 18-leg gate, including `--enable-rcu-debug`.

**It is still wrong, and the tree had already written the reason.** An
adversarial review found six. These are decisive:

1. **The kind became a property of the ASKING TXN, not of the SLOT.** The
   engine's rule is "a slot is SW xor MW, **globally**".
   - The three point-op sites (`ft_chain_node`, `ft_unchain_node`,
     `_cds_ft_replace_locked`) acquire the holder into a STACK
     `ft_held_anchor` BEFORE creating the content txn. So their registry is
     empty, and they can only ever answer MW.
   - The structural sites (`ft_detach_node`'s commit,
     `_cds_ft_remove_all_locked`, `cds_ft_insert_replace`) carry registered
     holder locks and answer SW.

   Same slot, two kinds, by construction.
2. **THE GATE THIS TREE SET WAS NOT MET.** `ft_lock_chain_holder`'s header:
   *"STEP 2 IS GATED ON THIS BRANCH BEING MEASURED UNREACHABLE (a counter
   reading ZERO across every spacing), never on it being 'safe'."* The
   attempt's own counter read 4.8M unowned per ft_inv leg. I argued the residue
   safe instead of zero. That is the exact move the header forbids.
3. **An SW abort WRITES.** `settle()` stores `old_ptr` (abort) or `new_ptr`
   (commit) **BLIND** (rcu-txn-mcas.h). A failed MW CAS never parks, so its
   abort writes nothing. An SW record always parks, so its abort restores
   `old_ptr` into a live slot. Three chain stores pass an expected-old that is
   an ARGUMENT, never loaded (`succ->prev = pos`, `next->prev = elem`,
   `next->prev = old`). Under SW, each of them is an abort-time WRITE of a
   derived value.
4. **`ft_hlist_append_run_prepare` was missed.** It has the same shape as
   `freeze_sole`. By its own header, its literal `NULL` expected-old is *"the
   serializing one (CAS old = NULL: a concurrent freeze of the tail fails this
   commit)"*. The attempt made it parkable without the re-read.
5. **The re-read is RYW.** `urcu_txn_load` returns this txn's own pending value
   when the slot is already in its write set. So for a FUSED commit, the
   "re-read under the lock" is not a read of the word at all. It needs
   `urcu_txn_load_committed`.
6. **Raw producers remain**, and a raw store cannot be SW-parked:
   - a bare `uatomic_cmpxchg` on `node->next`, holding NOTHING, in
     `ft_detach_node`'s standalone freeze fallback (ft-remove.h, audited with a
     NULL txn);
   - four plain `new_node->next->prev = ...` stores in
     `_cds_ft_replace_locked`.

One RED was seen with the attempt in tree. It was ft_inv per-node on
`NO_FEATURE_FT_SKIP_COMPRESSED`, on `inv_concurrent_same_key_removes`. The 16G
memcg SIGKILLed it at 395s, with a writer spinning in
`urcu_txn_install_mw_depth`. It did NOT reproduce: 3 runs with the flip and 3
with it compiled out, 6/6 GREEN. So it is recorded, not attributed.

#### ☠☠ THE FIRST VERDICT WAS A WRONG ZERO, AND IT HAS BEEN WITHDRAWN

The flip was built and measured GREEN (ft_unit 361/361, ft_inv 152/152,
per-node AND exponential). Then it was **taken back out**. That part stands.
Green was not evidence. The `UNHELD = 0` that licensed it is a
UNION-OF-WITNESSES zero (txn registry, per-thread ledger, lock ctx). A row
scores HELD if ANY of them sees a hold, while only the registry licenses a park.

What does **not** stand is the reading that replaced it. On 2026-09-19 the same
question was asked of a POSITIVE claim, `cds_ft_metadata.dbg_owner_tid`. It
read `US = 0` over ~27,000 events at both spacings. That was written up as
*"cds_ft_remove holds NOTHING; the chain SW flip is blocked on locking"*.

**That instrument cannot see a registered hold**, and the tree had already said
so in two places:

* `ft_flip_txn_record_release_lock` calls `ft_hold_trace_drop()` -- which
  **yields the stamp** -- the moment it RECORDS the release. The note beside it
  spells out the consequence: *"the ledger is not a hold count, it is an
  OUTSTANDING-RELEASE count ... a site that acquires a word and immediately
  records its {LOCK|s->s} terminal reads as ledger-EMPTY from then on, while
  the lock is still set on the word and still owned, by the commit rather than
  by the frame. Reading BARE as exposure would invert the meaning of the
  conversion this instrument exists to measure."* Every `lock_or_guard` hoist
  has exactly that shape.
* the duplicate-chain canary's own header prices it numerically. The chain hold
  audit measures **`ft_detach_node` at REG = 2,579,201 / LED = 0**, *"so a
  ledger-only verdict calls every one of those holds a violation"*.

`US = 0` beside `ft_chain_node`'s 2.6M was read as proof the instrument works.
It is the opposite. `ft_chain_node` takes the holder into a **stack**
`ft_held_anchor` and keeps it, so its stamp survives. Every converted,
registry-held site drops its stamp at the acquire. The asymmetry *is* the
blindness.

#### ☑ RE-MEASURED WITH THE WITNESSES SIDE BY SIDE (2026-09-19, later)

`-DFT_DEBUG_HLIST_TAIL_WHY` now reports two witnesses **at the same event**, in
one run:
- the REGISTRY (`ft_flip_txn_owns` on the `ft_flip_txn` whose handle the freeze
  was given);
- the LEDGER.

It does not use a second run at a second point. That is how an unmatched
control gets read as a result. The stamp plumbing is self-checked. The
classifier counts a stamped wrapper whose handle is not the handle the record
is going into (`stale = 0`, `no ftxn = 0` in every leg below).

The two sole-entry `ft_detach_node` call sites are now STAMPED, so the rows are
named rather than reasoned into a `(unstamped)` bucket.

ft_inv, `-O2 -g -DNDEBUG`, 152/152 green at both spacings:

| `len` derived at | disagree (per-node / exp) | **REGISTRY owns** | LEDGER | peer CLAIMS |
|---|---|---|---|---|
| `_cds_ft_remove_locked:8211` (internal holder, sole entry) | 10,157 / 16,219 | **100% / 100%** | 0 | 0 |
| `_cds_ft_remove_locked:7969` (compressed holder, sole entry) | 8 / 37 | **0% / 100%** | 0 | 0 |
| `_cds_ft_remove_all_locked:9301` | 347 / 523 | 37% / 62% | 0 | 0 |
| `_cds_ft_insert_replace:5160` | 13,683 / 14,337 | 22% / 22% | 0 | **834 / 1,094** |

**LEDGER = 0 on every row at both spacings**, including the rows the registry
owns outright. This shows, at matched points, what the stamp was reporting:
*"this hold was registered"*, and not *"there is no hold"*.

Reproduced on a second pair of legs: 11,111 / 23,382 disagreements at `:8211`,
again 100% / 100% registry-owned.

☠ **AND THE TWO POSITIVE WITNESSES DO NOT CONTRADICT EACH OTHER.** "Our registry
owns the holder AND a peer claims it" cannot both be true.
`ft_meta_lock_acquire` refuses a word that already carries `FT_STATE_LOCK`. So
the classifier counts the overlap. It is **0 on every row at both spacings**.
So the peer claims are DISJOINT from our own holds. They are not an artefact of
reading two witnesses at once, and they keep their weight as a finding.

The chain hold audit shows the same split from the other side. Per ft_inv
per-node leg, the `chain` word rows:

| site | total | HELD(reg) | HELD(led) | HELD(ctx) | UNHELD |
|---|---|---|---|---|---|
| `ft_detach_node:5153` | 4,086,233 | **2,584,875** | **0** | 929,773 | 0 |
| `ft_chain_node:2993` | 2,446,554 | **0** | **2,129,588** | 0 | 0 |
| `ft_promote_head:6319` | 156,788 | 156,780 | 0 | 0 | 0 |
| `ft_chain_compress_fused:2371` | 279,141 | 172,729 | 0 | 39,254 | 0 |
| `_cds_ft_remove_all_locked:9222/8858/8903` | 145,704 / 84,432 / 22,505 | all | 0 | 0 | 0 |
| `_cds_ft_replace_locked:6146` | 174,896 | 0 | 46,904 | 0 | 0 |
| `ft_detach_node_replace_compressed_parent` | 75,664 | 0 | 0 | 75,549 | 0 |

The stack-anchor shape is the one the stamp CAN see, and it is the minority.
That shape is the point ops (`ft_chain_node`, `_cds_ft_replace_locked`). They
take the holder into an `ft_held_anchor` BEFORE creating the content txn.
Everything the transition has already converted reads REG-held / LED-zero.
That is precisely the population the withdrawn verdict scored as unprotected.

☞ And it re-frames what the union objection actually forbids. "A park is
licensed by the registry alone" was written when the flip was PREDICATE-based:
`ft_flip_txn_owns` chose SW or MW per record. There, a hold the predicate
cannot see is useless. An UNCONDITIONAL dispatch has no predicate. Then
`ft_flip_txn_owns`' own header applies: *"read a miss as 'the registry cannot
see this hold', never as 'the op does not hold it'"*. So the union is not
disqualified as EVIDENCE OF EXCLUSION. It is disqualified as a RUNTIME GATE.
The next attempt needs one of the two. That choice decides whether
`ft_chain_node`'s REG = 0 / LED = 2.1M row is a gap or a bookkeeping
difference. ☐ Not decided here.

`ft_flip_txn_owns` is EXACT at per-node. Above it, it is conservative: a coarser
anchor is held while the owner itself is absent. So these percentages are
**lower bounds** on ownership. That is the right direction for licensing a
park:
- a registry HIT is a positive licence;
- a registry MISS is inconclusive. It owes the ctx witness before it is called
  a gap.

ft_unit reads DISAGREE = 0 on every row. The class is only visible under
ft_inv's concurrency, so ft_unit cannot certify it either way.

#### What this changes about the work list

* **The 09-19 "SCOPE question" for `cds_ft_remove`'s holder lock is not the
  work.** It was posed because the op appeared to hold nothing at the freeze.
  In fact it holds the holder at 100% of the sole path's disagreements. Nothing
  here argues for widening a hold across the op. That disposition was measured
  at 20/41 hangs under `FT_RM_REVALIDATE`. It stays refuted and stays unneeded.
* `cds_ft_remove`'s own acquire really is dead code (`#ifdef FT_RM_REVALIDATE`,
  defined in no build). That static fact is unchanged. It remains worth
  deciding or deleting. But it is about *that* acquire, not about whether the
  chain freeze is covered. The freeze rides `ft_detach_node`'s commit, and the
  detach's lock set covers it.
* ☐ **The sole path's remaining discriminator, and it has not been run.** We
  own the holder at the record, and a duplicate is there anyway. There are two
  cases:
  - The append COMMITTED BEFORE our acquire. That is a STALE PLAN: `@len` is
    the literal `1`, derived from an unheld read of `@succ_node`. That is
    exactly what `ft_hlist_chain_len`'s header forbids ("☠ CALL IT UNDER THE
    CHAIN HOLDER"). The cure is to re-validate the sole-entry claim under the
    lock, with a retriable bail.
  - The append landed AFTER. That is an exclusion gap on the APPENDER's side
    (`ft_hlist_insert_after_prepare:529`). The canary names that producer for
    100% of them.

  **The discriminator is a snapshot of the tail word taken at the holder
  acquire.** Both cures are local; neither is a scope change.
* ☐ `_cds_ft_insert_replace:5160` is now the strongest real gap. It is 78%
  registry-unowned. It also has 834-1,041 / 1,094-1,143 events (two legs each)
  where a **peer positively claims** the holder while this op records into that
  chain. Those claims are DISJOINT from our own holds. A stack hold is what the
  ledger CAN see, so that column does not explain away.
* ☐ `_cds_ft_remove_all_locked:9301`: 63% / 38% registry-unowned.
* ☐ The per-node events at `:7969` the registry does not own: 0 of 8 and 0 of
  12 over two legs. Exponential reads 37/37 and 25/25, and `:8211` reads 100%
  in all four. Reproducible, per-node ONLY, and small is not zero: a tolerated
  residue is not a licence.


#### What the flip costs, so the next attempt does not rediscover it

The MW expected-old is load-bearing BEYOND arbitration in three places. Each
one turns into a WRITE once the record parks. `urcu_txn_settle` stores
`old_ptr` on abort and `new_ptr` on commit, BLIND, and an SW record always
parks:

1. `ft_hlist_del_prepare` derives `&pred->next` by LOADING `@elem->prev`. A
   stale or head `@pred` points that slot at something that is not a live node.
   The CAS rejects it today. (Found the hard way: the canary's first spelling
   wrote through `caa_container_of` from the slot and corrupted the heap within
   seven tests, "corrupted size vs. prev_size".)
2. `ft_hlist_freeze_chain_prepare`'s TAIL. Its derived NULL detects a duplicate
   appended since the caller counted `@len`. That is live at ~24k/leg, above.
   ☞ On the sole-entry path, this is the arbitration that absorbs the stale
   plan today: the commit loses its install CAS and the caller re-derives.
   Parking removes it. That is why that path owes the under-lock
   re-validation.
3. Five structural expected-olds that are arguments rather than loads
   (`succ->prev <- pos`, `pred->next <- elem`, `next->prev <- elem`, and
   replace's two). MEASURED with `-DFT_DEBUG_HLIST_EXPECTED_OLD`: **0
   disagreements** over both suites and all three spacings. So they are cheap
   and safe to convert to loads whenever the class does park.

#### The instruments, all in tree and debug-gated

- `-DFT_DEBUG_CHAIN_CANARY` -- records, per chain WORD, the (fn, line) of the
  site that last stored it. The side table is keyed BY SLOT ADDRESS. Three
  witness-based attempts had failed. Then this canary named the producer of
  every surprise duplicate in one run (`ft_hlist_insert_after_prepare`).
  ☠ Keyed on the slot, never on a field inside the node. A chain store's slot
  is not always inside a live `cds_ft_node` (see (1) above).
- `-DFT_DEBUG_HLIST_TAIL_WHY` -- interns the `len` DERIVATION site and counts
  tail disagreements. It reports the holder's ownership by **all three
  witnesses at the same event**:
  - the ledger's positive claim (split by trie mode);
  - the per-thread ledger, asked directly;
  - the txn REGISTRY.

  ☠ It reports them together on purpose. A single-witness verdict on this
  question has now been wrong in both directions: the union read it HELD, and
  the stamp read it UNHELD. The registry source is stamped in
  `ft_flip_txn_handle()`. That is the one place a `*_prepare` caller converts
  its wrapper into the engine handle. The classifier does not trust the stamp.
  It VERIFIES it against the handle the record is going into (`stale`,
  `no ftxn` columns).
  ☠ The derivation stamp is CLEARED once consumed. If it were left standing,
  the next freeze on the same thread whose `@len` is a literal would be filed
  under whatever site last called `ft_hlist_chain_len`.

### 11.10 The post-flip census: where the conversion surface actually is

With the duplicate chain parked (§11.9), re-measure rather than assume what is
next. `-DFT_DEBUG_TXN_KIND`, ft_inv per-node, default build:

| population | records/leg | disposition |
|---|---|---|
| MW_STRUCT | 133,822,009 | **the conversion surface** |
| MW_LOCK | 35,441,459 | the lock take — stays MW forever |
| MW_ALWAYS | 11,469,627 | ROOT 28% (never converts), CELL 71% (`[DESIGN]`) |
| SW | 25,631,033 | — |

☠ **AND OWN_MISS IS A WRONG ZERO'S MIRROR IMAGE WITHOUT THE LEDGER.** The census
splits MW_STRUCT by whether the op holds the word's owner:
- OWN_HELD: the txn registry names it;
- OWN_LEDGER: only this thread's hold ledger does (a REGISTRY gap);
- OWN_MISS: neither does (a real exclusion gap).

`OWN_LEDGER` reads **0** unless the build carries `-DFEATURE_FT_HOLD_TRACE`.
The report says so. A census without it folds the registry gap into OWN_MISS.
It then over-reports:

| | without the ledger | with it |
|---|---|---|
| OWN_MISS, total | 3,305,319 | **2,259,139** |
| OWN_MISS, `ft-remove.h:4845` | 1,245,467 | **9,808** (LEDGER 1,266,317) |

The detach's commit txn has 58M records. It is the second-largest site in the
tree. It looks like the #2 exclusion gap. In fact it is the CLEANEST big site,
at 0.02%. Its holds live in the orphan plan-lock chain. That chain is
`FT_MAX_DEPTH` long, and the registry holds `FT_ENTRY_PER_NODE + 1`. So the
chain cannot be registered, by construction. A no-ledger census would have sent
the next piece of work straight at it.

**The real ranking** (ledger armed, ft_inv per-node):

| txn creation site | MW_STRUCT | OWN_MISS | miss % |
|---|---|---|---|
| `ft-insert.h:1183` (the insert's content txn) | 60,286,946 | **1,775,054** | 2.9% |
| `ft-insert.h:5321` | 614,283 | 235,095 | **38.3%** |
| `ft-graft.h:4137` | 1,299,312 | 89,974 | 6.9% |
| `ft-graft.h:4161` | 218,374 | 85,920 | **39.3%** |
| `ft-merge.h:1717` | 143,437 | 39,649 | **27.6%** |
| `ft-graft.h:2368` | 407,488 | 23,639 | 5.8% |
| `ft-remove.h:4845` | 58,183,980 | 9,808 | 0.0% |

⇒ **`ft-insert.h:1183` carries 79% of the entire remaining exclusion gap.** It is
the next piece of work by size. The three sites at 27-39% are the next by RATE.
They are small enough to handle one at a time. Which of the two orders to take
is a judgement. But both are now numbers rather than impressions.

☞ The report's own rule still governs: *"a site with OWN_MISS == 0 is ready for
the Phase B per-op arm; OWN_MISS is the size of its exclusion gap."*

### 11.11 The OWN_RETIRE lane is an ARM-PREDICATE GAP, and the bulk gate's writer-word fence is FINE-only (2026-09-20)

**Asked**: convert the `OWN_RETIRE` census lane from MW to SW.

**What it turned out to be.** The tombstone record already takes the SW-capable
path (`ft_flip_txn_record_state` → `record_state_kind(..., sw_ok=1)`). It does
not park because the *txn* is unarmed. The txn is unarmed because
`ft_txn_content_sw_ok(ft)` is `!ft->lock_fine || ft->exclusive`. It never asks
whether the caller is inside the FT-wide writer scope. Measured, per ft_inv leg:

| where the retire's lock-bearing word stands | records |
|---|---|
| held by US / by another thread | 0 / 0 |
| unlocked, FINE (the only class owed an answer) | **0** |
| unlocked, coarse / exclusive | 32,923 / 886,675 |
| unlocked, FINE **inside the FT-wide writer scope** | **203,006** |

All 203,006 have one shape. It is a FINE trie inside a bulk window. There,
G5.25 makes every writer re-take the FT-wide mutex. So this is not a
retire-specific change. It is the arm predicate missing a case. Widening it
would arm **every** record on those txns, not just the retire.

**Two wrong zeros closed before the number was believed.**

1. *Identity is not continuity.* The first probe re-read `ft_wlock_held == ft`
   at the commit and reported `LOST=0`. That result cannot mean anything.
   `ft_writer_lock_gp_wait` DROPS the mutex across every grace period. It then
   restores **the same pointer**. So a hold that a peer writer ran through
   reads the same as one that never broke. The probe was replaced by
   `ft_wlock_gen`, which counts RELEASES. It is stamped at CREATE and compared
   at the commit. It was also given a positive control: the total release
   count. `DROPPED=0` against zero releases is an inert probe, not a
   measurement.

2. *The retire subset is not the population.* The flag was set at the RECORD.
   That only covers retire-recording txns. But widening the arm arms every txn
   created in that scope. The flag moved to the three constructors. A new
   `FT_TXN_ARM_TRIE_WIDE` helper sets it, so the three cannot drift apart.

Measured, create→commit, all three spacings plus ft_unit:

```
kept=1,578,876   LOST(identity)=0   DROPPED(a seam in between)=0
watched txns=1,883,283   FT-wide lock releases this run=4,224,082
```

⇒ the mutex is genuinely held unbroken across those txns. **That is necessary
and not sufficient**, see below.

**☠ THE HAZARD THAT IS NOT A SEAM.** On a FINE trie the FT-wide lock is taken
only while `ft_bulk_active(ft)`. A point op may take it because a **peer's**
bulk gate was up. That op keeps holding it. But `ft_bulk_gate_exit` lowers
`bulk_state` with **no grace period and without the writer mutex**. This is
deliberate: for a READER a stale gate is "never wrong, only slower". Once the
word reads 0, the next point writer skips the mutex entirely. It runs beside
us. An SW park cannot fail; it erases. So the population splits:

- **SELF-gated** (`ft_bulk_lock_held == ft`): this thread owns a refcount on
  this trie's gate for its whole body, so `bulk_state` cannot reach 0 under it.
  The exclusion is real for the whole txn.
- **PEER-gated**: the exclusion is a refcount another thread owns and may drop
  at any instant. **Not licensed.**

**☐ PREREQUISITE, not yet done.** Three sites re-derive `ft_txn_content_sw_ok()`
to ask "did the constructor arm this txn trie-wide?" --
`ft_flip_txn_claim_per_op_armable`, `ft_flip_txn_arm_per_op_at` and
`ft_flip_txn_arm_structural`. Today the predicate and `t->trie_wide_sw` agree
exactly, so they are correct. They stop agreeing the moment the arm gains a
fourth licence. `ft_flip_txn_arm_per_op_at` would then fall through. It would
set `sw_per_op = true` on a trie-wide-armed txn. That demands a registered
owner per record, and DOWNGRADES the record to MW. This is exactly the shape of
the @f6ab074d uninitialised-`sw_per_op` defect. They must be changed to ask
`t->trie_wide_sw` **before** any widening lands.

### 11.11.1 The writer-word fence is FINE-only (Mathieu, 2026-09-20) — ☑ LANDED

> *"In external sync mode and coarse trie, there is no fine locking writers to
> contend with, so the bulk lock and fence is useless."*
> *"Fine and exponential need the bulk fence."*

`@bulk_state` has exactly one non-instrumentation consumer in the tree:
`ft_writer_lock_scope_enter`'s FINE branch. Every other reference is inside
`FT_ANC_LEDGER` / `FT_DEBUG_WIDEN_OWNER` / `FT_DEBUG_TXN_KIND`. COARSE takes the
FT-wide mutex unconditionally. EXTERNAL_SYNC and exclusive return from the scope
*before* the FINE branch. On those tries the word was published, **fenced with a
full grace period**, and read by nobody.

The fence is the expensive half. A `FT_BULK_WRITER_ONLY` op (detach / graft /
graft_swap / merge_at) publishes no reader word. So `@bulk_state` is the only
thing that can make the 0→1 opener own a grace period.

⇒ `ft_bulk_state_has_consumer()` = `FT_BULK_WIDE_LOCK && lock_fine &&
!exclusive && !external_sync`. The **store stays unconditional**: it is one
relaxed word. A trie may leave `@external_sync` mid-window. It then degrades to
"peers take the lock", the conservative side. A rekey is unaffected. It owns
its GP for `@move_active`. That is a READER word, needed in every mode, since
EXTERNAL_SYNC excludes writers and nothing else.

Measured, and it confirms Mathieu's rule rather than assuming it:

| suite | writer-word opens fenced | SAVED (no consumer) |
|---|---|---|
| ft_inv, per-node / exponential / root-only | 875,357 / 875,081 / 825,456 | **0 / 0 / 0** |
| ft_unit (mixed modes) | 753 | 123 |

All three FINE spacings keep the fence; only coarse / exclusive / external-sync
skip it. A/B against the `-DFT_RED_BULK_FENCE_ALWAYS` control (a coarse trie,
100 × detach+merge = 200 gate opens):

| readers | control | working | saved |
|---|---|---|---|
| 8 | 10,342 µs/op | 10,256 µs/op | 8.6 µs/op ≈ **4.3 µs per fence** |
| 32 | 10,373 µs/op | 10,262 µs/op | 11.1 µs/op ≈ **5.5 µs per fence** |

The direction reproduces across every alternating pair. The saving scales with
grace-period cost, as a skipped `synchronize_rcu` should. The control is a gate
config (`bulkfence`) so the claim stays refutable.

**Not changed: `ft_bulk_lock` on a COARSE trie.** It already skips exclusive and
external_sync. On coarse it still looks load-bearing to me. It is the
bulk-vs-bulk serializer *held across the drain seams*. A coarse trie's FT-wide
mutex IS dropped at every `ft_writer_lock_gp_wait`. So without it, two bulk
bodies interleave at a seam. Flagged rather than removed.


### 11.11.2 EXTERNAL_SYNC takes grace periods under the APPLICATION's lock; rekey becomes opt-in (Mathieu, 2026-09-20) -- ☑ LANDED

> *"Our coarse mode findings will apply to the external sync contract. It's
> important."* / *"I suspect that External sync will be fundamentally
> incompatible with rekey"* / *"Unless readers always do two pass with external
> sync"* / *"Maybe it should be opt in"*

**First, the coarse question, measured.** Every bulk op does scope the FT-wide
mutex around its whole body (`CDS_FT_SCOPED_WRITER`). The only holes are the
eight `ft_writer_lock_gp_wait` drain seams. Those MUST drop the mutex. A writer
parked on the mutex is an online, non-quiescent QSBR reader. So a GP taken
while holding the mutex waits for the thread it is blocking. The probe counts
at the RELEASE (`held != NULL`), not at the seam. `ft_bl_seam_under` counts
seams *reached*, and it scores on a FINE trie that holds nothing:

```
seams that ACTUALLY RELEASED the FT-wide mutex (ft_unit):
  in a BULK body  coarse=16  fine=29  |  in a POINT op  coarse=34  fine=0
  cross-trie=0
```

⇒ on COARSE the mutex is **not** held across a whole bulk op. So `ft_bulk_lock`
is load-bearing there: it is the bulk-vs-bulk serializer held *across* the
seams. It stays.

**Then the consequence for EXTERNAL_SYNC, which is the important one.** Every
seam guards on `@exclusive`, **none on `@external_sync`**. So they all fire
there. The library holds no mutex of its own to drop. What it cannot drop is
the APPLICATION's exclusion. Per-site, one op each:

| site | op | GPs under the app's lock |
|---|---|---|
| `internal.h` bulk gate | rekey ×2 | 2 |
| `ft-detach.h:513` | detach reader drain | 1 |
| `ft-lifecycle.h:485` | `make_exclusive` drain | 1 |

COARSE takes the *same four*. The only difference is who holds a lock across
them. The contract text says nothing about this. It mentions grace periods only
to say readers are unaffected.

**The cure, and its exact reach.** `cds_ft_group_attr_set_rekey()`, default OFF.
Take an EXTERNAL_SYNC group that opts in. There, `@rekey_always_coherent` makes
`ft_move_active()` answer true outright. Readers never leave the two-pass path.
So the move gate publishes no mode *transition* and owes no grace period.
Measured before / after, same program:

```
EXTERNAL_SYNC gate GPs:  2  ->  0        (COARSE unchanged at 2 -- correctly scoped)
```

☠ **It removes the rekey GPs and nothing else.** `ft-detach.h:513` and
`ft-lifecycle.h:485` are reader DRAINS, not mode transitions. External sync
excludes writers only. So these drains are exactly as necessary as under
coarse. **The contract still owes a requirement that a writer blocked on the
application's exclusion be RCU-quiescent** (`thread_offline` while waiting).
The library already does that for its own locks (`ft_bulk_lock_enter`,
`ft_writer_lock_park`). Forbidding rekey would NOT have closed this.

**The opt-in also pays for itself off external sync.** A trie that will never
rekey no longer carries the coherent lookup specializations at all.
`ft_install_lookup_ops` selects the plain ones. `ft-lifecycle.h` had traded
that away, in its note *"REKEY coherence is no longer an opt-in ... the opt-in
attr is gone with it"*. The note was right about cost. It was wrong about the
grace period always being payable.

**Wrong-green guard, because this is exactly the shape that has bitten before.**
A creator may forget the opt-in. It then keeps calling rekey and collects `-7`
every time. It reports green with zero coverage. Three things prevent that:
- `-DFT_DEBUG_REKEY_OPTIN_STRICT` turns a refusal into an abort naming the
  trie;
- gate config `rekeyoptin` arms it (**0 aborts** over ft_unit + ft_inv);
- `test_rekey_optin_default_refuses` covers the default arm. Every other rekey
  test opts out of exercising it.

Converting the suites was itself the evidence: 48 tests went RED on the flip,
not silently green.

☠ And the TAP plan has TWO arms: `NR_TESTS` is defined once per
`FEATURE_FT_FAULT_INJECT` branch. Bumping one left 363 tests running against
`1..361`. Every line was `ok` and the `^ok` grep was green. The exit status was
2.


## 12. API / design questions queued by Mathieu (2026-09-17)

Not implemented; recorded so the flip does not silently decide them.

### 12.1 An application-provided writer exclusion mode -- ☑ LANDED @f6ab074d

☑ **RESTORED as `CDS_FT_WRITER_EXTERNAL_SYNC`**, the third
`cds_ft_writer_strategy`. Named for the CONTRACT, not a lock (Mathieu). The
other two are named for the lock the LIBRARY takes. Here it takes none. The
caller meets the contract however it likes: a single mutating thread, or its
own mutex around every mutating call.

It is implemented as a SEPARATE trie flag, never `exclusive`. So all 71
consumers of that flag are untouched, and no reader-side shortcut is reachable.
`lock_fine` is false. So every kind / lock-set / door decision reads as COARSE
(door 1 arms it SW trie-wide). Only the FT-wide mutex goes away. There are two
early-outs. Both use the same argument as `exclusive`'s:
- `ft_writer_lock_scope_enter`: no mutex to take;
- `ft_bulk_lock_enter`: nothing to promote. The bulk gate and rekey promote
  FINE locking, and there is none here, nor in COARSE.

☞ RANK STATS NOW COERCE *FINE* ONLY. The coercion read "any strategy !=
COARSE". EXTERNAL_SYNC already serialises every structural writer. Coercing it
would hand back the very lock the caller asked the library not to take.

☑ THE CONTRACT CHECK NEEDED NO NEW CODE. The access validator keeps its
single-OWNER claim for any non-fine trie. So EXTERNAL_SYNC lands on the owner
CAS. We measured it with a guaranteed-overlap control: two writers, one trie,
`-DFEATURE_FT_EXCL_VALIDATE`. COARSE **survives**, because the mutex serialises
them. EXTERNAL_SYNC reports *"writer conflict -- owner 0x…, entering thread
0x…"* and aborts. This also proves that the mutex is genuinely skipped. Under
COARSE those two writers cannot overlap at all.

In contract (`test_urcu_ft -W external-sync`, 4 readers + 1 writer, 5s), the
run did 380,623,969 reads / 864,142 writes. The always-MW lanes converted
(`FT MWA DOOR1 converted=1,107,522`).

☞ THE NAME CHANGED, AND THE OTHER SIDE OF THE COLLISION MOVED. It landed as
`CDS_FT_WRITER_EXCL_CALLER`. That read too close to `cds_ft_make_exclusive`,
even though the two promise different things. The strategy is about WRITERS.
The trie flag is about writers AND readers. Mathieu asked whether the trie mode
should be renamed. The mapping says otherwise. In Rust's vocabulary an
EXCLUSIVE trie is `&mut T`, the *exclusive* (unique) reference: no concurrent
access of any kind. The default trie is `&T` where `T: Sync`, the *shared*
reference. So "exclusive trie" is already the right name, and its antonym is
SHARED. What was misnamed is the strategy. It is not about exclusive ACCESS at
all. It is about WHERE the synchronization lives. Hence:

- `CDS_FT_WRITER_EXCL_CALLER` -> **`CDS_FT_WRITER_EXTERNAL_SYNC`**. Still named
  for the contract, not a lock -- it says the synchronization is the
  application's, without claiming it is a mutex;
- `cds_ft_make_concurrent` -> **`cds_ft_make_shared`**, completing the
  exclusive/shared pair at the call site;
- `cds_ft_make_exclusive` / `cds_ft_attr_set_exclusive` / `cds_ft_is_exclusive`
  are UNCHANGED, and "exclusive" now has exactly one meaning in the API.

☠ `private` was considered and REJECTED, for two reasons:
- it is already the FT allocator's word for a range owned by one compactor or
  writer and not yet published (~85 sites in the same files);
- it is a C++ keyword. A trial rename put `bool private` in the public header.
  That would have broken every C++ consumer, including
  `test_urcu_ft_unit_cxx`.

`FEATURE_FT_EXCL_VALIDATE` and the `ft_excl_*` validator internals keep their
names on purpose. That machinery polices BOTH modes. So its "excl" means access
exclusion in general.

☑ THE CONTRACT IS NOW TESTED, not just documented. `test_urcu_ft -W
external-sync` did NOT catch a violation. Its writers rarely overlap inside the
claim window. Three matched rows in `tests/unit/test_urcu_ft_unit.c` do catch
it. They share one body, with one variable changed per row:
- EXTERNAL_SYNC + a caller mutex survives and verifies coherent;
- LOCK_COARSE with no caller mutex survives and verifies coherent. This is the
  control: the library's own mutex is what serialises it;
- EXTERNAL_SYNC with no caller mutex aborts under the validator.

The third row SKIPs, loudly, on a build without `-DFEATURE_FT_EXCL_VALIDATE`.

The original statement of the item follows.

Planned, removed from the API, to be restored. The app guarantees writer
exclusion: single-threaded, or its own mutex around every caller. It should
behave "pretty much as COARSE". The difference: the contract comes from the
caller, and the library takes no writer lock of its own. Debug builds keep the
validation macros that check the contract.

For the record kinds this costs nothing. `ft_txn_content_sw_ok` is
`!lock_fine || exclusive`. So door 1 arms such a trie SW trie-wide, like
COARSE. The requirement is that the mode be DECLARED, so the library can read
it. Nothing needs to be inferred.

Two things to pin when it returns:
- it is NOT `cds_ft_attr_set_exclusive`. That one also promises NO concurrent
  RCU readers. So it licenses skipping reader-visible publication discipline:
  the reachability climb of §11.3 treats an exclusive trie as unreachable by
  readers. This mode keeps readers, so proxies, tags and grace periods all
  stay. It is a third writer strategy;
- the existing hold-ledger oracle polices LOCK collisions. This mode has none.
  The contract to check is "no two writers inside the trie at once". That is,
  a writer-scope entry counter that must never exceed one.

☑ **THE MW RETIREMENT IS DONE** (the half that does not need the mode to exist).
`ft_flip_txn_record_tag_mw` now asks `ft_txn_content_sw_ok(t->ft)` and parks
instead of CASing. See §12.1.1 below. The rest of this section is the mode
itself, still owed.

☞ AND IT RETIRES THE REMAINING MW (Mathieu). Under app exclusion the always-MW
lanes can go SW too. The exception is the lanes that are genuinely MW by
design. These lanes are MW today on a COARSE trie as well. Nothing decided it.
The lanes call `ft_flip_txn_record_tag_mw` UNCONDITIONALLY for:
- the root pointer;
- the cell list;
- head back edges;
- parent_word;
- PSO, state, RANK, NR_CHILD_DEC.

So door 1 arms the rest of the txn SW, and these stay MW. Nothing can race them
there, and no validate can fail. So one predicate (`ft_txn_content_sw_ok`),
consulted by those recorders, converts the lot. The only record that must stay
MW is the DLM lock TAKE: the arbitration point. Such a trie takes no DLM locks
at all. This is independent of the fine-spacing flip; do it right after.

### 12.1.1 Landed: the always-MW lanes consult door 1

"Always MW" was never a decision about the WORD. It came from this recorder
being called unconditionally. On a COARSE trie every writer sits behind the
FT-wide mutex. On an EXCLUSIVE one the caller promises a single writer. So
there is no peer CAS for a park to race, **for any slot in that trie**. Door 1
already arms the rest of the txn on that same exclusion.

☞ WHY THIS IS NOT THE TRAP §11.9 FELL INTO. The chain attempt made the kind a
property of the ASKING TXN (its lock registry). So one slot got two kinds,
depending on which op wrote it. This predicate is a property of the TRIE. Every
writer of every slot in it gets the same answer. So per-slot uniformity is
automatic.

**Three things had to be true, and each one was wrong first.** An adversarial
review refuted the first version of this change. These are its findings, fixed.

#### ☠☠ 1. A record whose old == new is a VALIDATE, and can never be parked

Two lanes here are pure §4.B guards: `ft_flip_txn_guard_installed_child` and
the recompaction sweep's child validate. They record `{live_state ->
live_state}`, which rcu-txn.h calls `urcu_txn_validate`. Parking one does two
wrong things, **neither of which needs a peer writer**:

1. the check disappears, because a park cannot fail. That check is what
   catches a node ALREADY frozen or locked when the record was made. The
   dangling parent slot and the concurrent-recompaction UAF both came through
   it;
2. worse, the value parked is `live_state`: the word with TOMBSTONE and LOCK
   **masked out**. So the commit CLEARS a tombstone on a node whose grace
   period is running, or a DLM lock the op does not hold. `settle()` writes
   `old_ptr` BLIND, so even an ABORTED commit stomps those bits.

The guard's own header already said it: *"MW kind, unconditionally: an SW park
validates nothing (it cannot fail), so recording this through the
structural_sw-dispatching helper would be a guard in name only"*. This recorder
then turned into that helper. So the test is STRUCTURAL, not a list of lanes to
remember: **same value in and out means MW**. (Measured: 293 such records per
ft_unit leg. The first version parked all of them, and the 18-leg gate was
green.)

#### ☠ 2. The answer must be LATCHED, because `exclusive` is mutable

`ft->exclusive` is a plain bool. `cds_ft_make_exclusive` / `_make_shared` and
the graft_swap restore assign it. Sampled per record, it could answer
differently within ONE txn. It could also disagree with the `structural_sw`
the constructor latched from the same call. The whole argument needs the
answer fixed for the txn's life. It is now latched into `t->trie_wide_sw` at
construction.

☠☠ AND THE LATCH MUST BE INITIALISED EXPLICITLY. These txn structs are REUSED.
The constructors initialise them field by field; there is no memset. So a flag
that is only ever SET carries a previous op's trie answer into the next one.
We measured this while the init was missing. A thread that had run one coarse
op parked the always-MW lanes on **every FINE trie it touched afterwards**.
That was 1,647,382 records in one ft_unit leg instead of 8,298. And **the leg
was GREEN**. It was found only because the counter disagreed with the expected
magnitude.

#### ☠ 3. The ROOT pin must key on the SLOT, not the producer's flag

A cross-trie dual NAMES one trie and writes TWO roots. So `t->ft` being coarse
says nothing about the other trie. That trie may be fine, with live peer
writers. `ft_flip_txn_record_root` calls a PINNED twin that never converts. Its
own header gives the rule: "the rule is a property of the SLOT, not of the
txn's named trie". But that pinning depended entirely on the producer setting
`.root`. So the converting arm now also refuses any slot that IS the named
trie's root. It counts when that test is what saved it.

#### The two witnesses, and they read ZERO

| witness | what a non-zero would mean | measured |
|---|---|---|
| `foreign_ft` | a recorder carrying its own `ft` argument disagreed with `t->ft` -- a coarse name licensing a park on a fine trie's word | **0** |
| `root_slot_saved` | a root slot reached the converting arm (a producer forgot `.root`) | **0** |

The first covers the three recorders that take a trie separately from the txn
(`ft_flip_txn_record_count_parent`, `ft_ord_cell_flip_into`,
`ft_ord_cell_record_into_ft`). With both at zero, every slot these lanes record
belongs to the trie the predicate asked about. That is what makes the
trie-wide argument apply to the slot.

☠ AND THE LOCK TAKE IS NOT IN THIS RECORDER. It is MW_LOCK, a separate kind.
It IS the arbitration point and can never park. A coarse trie takes no DLM lock
at all. An EXCLUSIVE FINE trie does, and keeps its CAS.

☞ A BONUS: the guard at `ft_flip_txn_guard_installed_child` is always-MW. Its
header records a ONE SLOT, TWO KINDS livelock from landing beside an SW record
(ft_unit `test_rekey_collapse_one_slot_two_kinds`, 50,001 attempts). It stays
MW under rule 1, so that pairing is unchanged.

#### Measured

Build with `-DFT_DEBUG_MWA_DOOR1`, ft_unit per-node leg. There are 2,666,930
always-MW records. Of these, 11,804 sit on a trie door 1 says yes to. Four
classes are ENTIRELY coarse-only, so they retire completely:

| class | convertible | fine |
|---|---|---|
| RANK | 4,984 | **0** |
| NR_CHILD_DEC | 943 | **0** |
| STATE (incl. the validates) | 78 | **0** |
| PSO | 15 | **0** |
| CELL | 3,146 | 2,029,561 |
| HEAD_BACK | 2,196 | 620,609 |
| PARENT_WORD | 24 | 2,283 |
| GUARD | 36 | 61 |
| DUAL_UNNAMED | 2 | 1,974 |
| ROOT (pinned) | 380 | 638 |

☠ ft_unit is the wrong place to SIZE it: that leg is fine-dominated. The
in-tree counter is `FT MWA DOOR1`, under `-DFT_DEBUG_MWA_DOOR1`. Per leg it
reads: ft_unit 8,298 converted; ft_inv 2.0M-2.7M. Some users run a coarse
trie, or the app-provided exclusion mode of §12.1 when it returns. For them,
this is 100% of the always-MW traffic. Every record of it is a CAS that
arbitrates against nobody.

### 12.2 COARSE vs FINE + ROOT_ONLY -- redundant?

As a CONCURRENCY setting, yes: both serialize every structural writer. As
machinery, no:

| | COARSE | FINE + root-only |
|---|---|---|
| `lock_fine` | false | true |
| what serializes writers | the FT-wide writer lock (a fair mutex) | the ROOT node's state-word LOCK bit, taken by an MW CAS through the engine |
| lock sets | none derived | derived per op, every member's anchor collapsing onto one word (dedupe everywhere) |
| on contention | waits | refuses (-EAGAIN), the op re-descends; escalation for fairness |
| per-op cost | one mutex | an acquire record, a release/retire terminal, dedupe bookkeeping, guards |
| record kind | door 1, trie-wide | door 2, per record, keyed on ownership |
| bulk ops | FT-wide lock | FT-wide lock -- common |

Root-only earns its keep as the MAXIMAL-COLLAPSE TEST AXIS for the anchor
machinery, not as a configuration. This flip had to close two gaps:
- members deduping onto one word with nothing filing them;
- a record whose owner is covered only by the root.

Both were invisible at per-node and exponential.

### 12.3 Gate ROOT_ONLY as non-public (Mathieu) -- ☑ LANDED

The ENUMERATOR sat in the public header, so root-only read as public API even
though the setter refused it. Now:

- `CDS_FT_LOCK_SPACING_ROOT_ONLY` is declared **only** under
  `-DFEATURE_FT_LOCK_SPACING_ROOT_ONLY` -- its own macro, not the
  anchor-validation one it shared -- with the value 3 documented as RESERVED.
  Code that names the symbol fails to COMPILE without the gate, which is the
  strongest "not public" signal there is;
- the library's own dozen spacing branches keep compiling. They use an
  internal spelling of the reserved value in `fractal-trie-internal.h`.
  Without the gate nothing can SET the spacing. So those branches are
  unreachable, not wrong;
- `cds_ft_group_attr_set_lock_spacing` refuses the raw value 3 without the
  gate. The unit test asserts exactly that, by NUMBER, since the name does not
  exist there. That assertion IS the "not public" property.

☠ THE ENVIRONMENT KNOB WAS THE TRAP. The 18-leg gate reaches root-only through
`CDS_FT_LOCK_SPACING=root-only`, not the setter. Gated silently, every
root-only leg would have degraded to per-node and read GREEN. A trie CREATOR is
a feature gate, and a degraded creator is the wrong zero. So the knob now
ABORTS loudly: `[Fatal] Fractal Trie: CDS_FT_LOCK_SPACING=root-only requested, but this build
has no -DFEATURE_FT_LOCK_SPACING_ROOT_ONLY` (measured, rc=134). The dev trees
carry the macro. A shipping build cannot be talked into the axis by an env var.

☞ EXPONENTIAL IS UNCHANGED BY THIS. It is still refused at the setter without
`FEATURE_FT_ANCHOR_VALIDATE`. It is still reachable through the env knob under
`FEATURE_FT_LOCK_SPACING_ENV`. Whether it GRADUATES is a separate, deliberate
decision, not folded in here. The flip (§11.5) serves it, and it has been green
at 18/18 for every gate since.
