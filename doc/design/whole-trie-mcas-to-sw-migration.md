# Whole-trie MCAS → single-writer (sw) migration — SCOPE

Status: **SCOPING / decisions taken** (2026-07-15). No code written. This
document scopes moving the Fractal Trie's structural transaction engine:
- from the multi-writer MCAS engine (`urcu_mcas_txn`, wrapped by
  `ft_flip_txn`);
- to the single-writer proxy-flip engine (`urcu_txn_sw_txn`,
  `<urcu/rcu-txn-sw.h>`).

**Decisions taken (Mathieu, 2026-07-15):**
1. **REPLACE, not coexist.** The sw end-state is the sole engine. OPTIMISTIC /
   MCAS retires from the FT once LOCK_FINE is load-bearing (§7). No permanent
   dual-engine fork.
2. **Gate confirmed.** P1 does not start until the **LOCK_FINE per-domain
   drop** is designed. This drop is the FT-wide `writer_lock` removal,
   op-domain by op-domain. It is the immediate next design deliverable. See
   `mw-writer-lock-escalation-model.md` §11 and the drop-mechanics note that
   extends it.

Companion docs:
- `mw-writer-lock-escalation-model.md` (the lock pivot this depends on);
- `in-trie-move-seqcount.md` (the reader-coherence half).

Memory: `[[project_ft_mw_lock_escalation_pivot]]`,
`[[project_ft_mw_samekey_remove_arbitration]]` (Step B engine wall).

---

## 0. Why this exists / the question

Step B retires the bespoke duplicate-chain hlist for `urcu_txn_sw_list`. It
is **blocked**:
- The chain's freeze/del/replace/append edges FOLD into the structural MCAS
  flip-txn at ~12 sites. This gives atomicity with the trie↔head-anchor edge.
- sw records cannot fold into an MCAS txn. They are two different engines.

So a *real* chain→sw-list migration has only one way forward. Move the
structural edges to sw first. The chain edges can then share the *same* sw
txn.

So "scope the whole-trie MCAS→sw migration" = scope the lever that unblocks
Step B. Also decide whether it is worth doing on its own merits.

## 1. TL;DR / recommendation

- **Feasible and well-targeted.** `urcu_txn_sw` was purpose-built as the FT's
  migration target:
  - It takes per-record tags in the low 4 bits. This is the FT's exact `0xF`
    type code; the header names it.
  - It mirrors the MCAS `record()` frozen-set contract, for mechanical
    migration.
  - Its `init_inline`/`reserve` are the documented analog of
    `ft_flip_txn_create_bounded`.

  Tag/alignment cost = **zero**. Both engines are `aligned(16)`. The FT tag is
  `0xF` by construction: `FT_INTERNAL_BITS=1`, so
  `FT_FLIP_PROXY_TAG = 1|(7<<1) = 0xF`.

- **It is gated on the lock pivot, both ways:**
  1. **Correctness gate.** sw has *no* install CAS, *no* conflict detection
     and *no* abort. Two writers racing one slot silently corrupt it. sw is
     legal ONLY under writer mutual exclusion, i.e. `lock_mode` (COARSE or
     FINE). **OPTIMISTIC mode is fundamentally incompatible with sw.**
  2. **Performance gate.** A FINE trie still takes the FT-wide `writer_lock`
     until the per-domain drop lands. So sw-under-lock today is *fully
     serial*. sw only becomes a throughput win once LOCK_FINE drops the
     FT-wide lock per op-domain. Then disjoint keys → disjoint per-node locks
     → concurrent. Before that, sw is a simpler-engine swap with equivalent
     correctness. It is not a speedup.

- **The migration is mostly a SIMPLIFICATION.** Single-writer exclusion makes
  the entire read-set-validation apparatus provably dead:
  - guards and `load_validate`;
  - RYW same-slot chaining;
  - `expect_conflict` and conflict/aging;
  - the ABORT lane;
  - the copying-registry's abort arm.

  The hard, non-mechanical part is the **state-word composition layer**
  (§4.A). Today several edits RYW-chain onto one `&meta->state` word inside
  one MCAS record. sw forbids same-slot records. So each must be
  **pre-folded into a single computed `{old→new}`**. A single writer holding
  the lock CAN do this: it reads the word raw and knows the final value in
  one shot.

- **Recommended shape: REPLACE, not coexist.** Do not keep MCAS for
  OPTIMISTIC and add sw for locked mode. That doubles and forks every write
  path. Instead, the sw end-state *is* the lock end-state. LOCK_FINE +
  per-node locks + per-domain drop recovers disjoint-key concurrency
  *without* MCAS. So OPTIMISTIC/MCAS is retired wholesale once FINE is
  load-bearing. Until then, keep MCAS as the shipping default. Build and
  validate sw behind a build flag.

- **Do NOT start until the LOCK_FINE per-domain drop is designed.** The drop
  determines whether sw ever runs concurrently. The two share the same
  exclusion contract. Sequencing in §8.

## 2. Why migrate (the levers)

1. **Unblocks Step B** (the proximate reason). Structural = sw ⇒ chain edges
   become sw records in the *same* txn ⇒ the bespoke hlist can retire for
   `urcu_txn_sw_list`. Its `_add_after_prepare`/`_del_prepare` already record
   into a caller `urcu_txn_sw_txn *`. This is exactly how `ft-txn-hlist.h`
   records into the mcas txn today.
2. **Simpler engine to reason about.** The MW campaign has repeatedly fought
   engine-level subtleties:
   - helping loads;
   - RYW poison;
   - aging/escalation;
   - the masking-guard self-abort livelock.

   sw has none of these: one monotone `0→1` selector, no CAS, no abort. The
   correctness surface shrinks hard.
3. **Cheaper commit.** MCAS commit is a k-CAS install + status-word flip,
   with per-slot contention machinery. sw commit is one release store on a
   shared selector, then a settle sweep. Under a held lock there is no
   contention to pay for. sw drops the machinery that exists to survive it.
4. **Matches the pivot's direction.** The strategic pivot already chose locks
   over optimistic MW. sw is the engine that *assumes* the lock. MCAS is the
   engine that assumes you don't have one. Keeping MCAS under a lock pays for
   contention handling that the lock has already made impossible.

## 3. The enabling precondition — sw ⟺ `lock_mode`

`ft_writer_lock_scope_enter` (fractal-trie-internal.h:1646) takes the FT-wide
`writer_lock` whenever `ft->lock_mode` (COARSE or FINE). For OPTIMISTIC it is
a no-op. sw's own header (§"URCU_TXN_SW_EXCL_VALIDATE") states the contract
bluntly:

> This engine requires writer mutual exclusion — it has no install-time CAS, no
> conflict detection and no abort, so two writers racing on one slot simply
> corrupt it.

Therefore:

- The LOCK per-node lock (`FT_STATE_LOCK`) and/or the FT-wide `writer_lock`
  is *exactly* the exclusion sw demands. The lock pivot is not merely
  compatible with sw. It is the **precondition** that makes sw sound.
- OPTIMISTIC's whole value is the disjoint-key lock-free milestone (0/1600
  @16w). It comes from having *no* lock, and letting MCAS arbitrate disjoint
  writers wait-free. sw cannot serve that path. Disjoint-key concurrency
  under sw is recovered a *different* way. It uses LOCK_FINE per-node locks
  that disjoint keys never share, once the FT-wide lock is dropped
  per-domain.
- `URCU_TXN_SW_EXCL_VALIDATE` is the sw analog of `FEATURE_FT_EXCL_VALIDATE`.
  Turn it on in the MW oracle/unit builds. It catches a two-writer slot race
  the moment it interleaves. A clean run is evidence, not proof (same
  caveat).

## 4. The gap — what MCAS gives the FT that sw does not

| MCAS feature the FT uses | sw equivalent | Migration action |
|---|---|---|
| `urcu_txn_load` (RYW / helping read) | **none** ("no transactional loads at all") | Read the word RAW (lock held), pre-compute final value |
| same-slot RYW chaining (`{old→mid}∘{mid→new}`) | **none** (records must be slot-distinct; dup = last-wins corruption) | Fold same-slot edits into ONE computed record (§4.A) |
| `urcu_txn_validate` / `ft_flip_txn_guard_parent` ({live→live}) | **none** | **Drop** — dead under single-writer (§4.C) |
| `urcu_txn_conflict` / aging / `expect_conflict` | **none** (no contention) | Delete |
| `declare_disjoint` | **none** (records already asserted distinct) | Delete |
| ABORT status + retry loop | **never returned** (only OK / MEMORY_ERROR) | Delete the retry lane; keep OOM propagation |
| copying-registry *abort* arm | commit consumes fences; only OOM/pre-commit bail remain | Simplify registry to OOM/bail only |

sw also gives what the FT needs and MCAS also gave. These are **kept,
mechanical**:
- atomic multi-slot publish via one selector flip;
- per-record heterogeneous tags (state `0x1`, pointer `0xF`, chain `0x1` in
  one txn);
- bounded on-stack flip (`init_inline`) and pre-reserve (`reserve`), with
  sticky-OOM→`MEMORY_ERROR`;
- single-edge fast path (nr==1 → bare release store, no proxy/GP), matching
  the FT's dominant one-pointer publish;
- one `call_rcu` reclaim per committed multi-edge txn.

### 4.A The state-word composition layer (the ONE hard workstream)

Today `&meta->state` is a transacted word. It carries `nr_child`,
`FT_STATE_PROXY` (bit 0), `FT_STATE_TOMBSTONE` and `FT_STATE_LOCK`. Several
*distinct* edits land on the SAME `state` word in ONE txn. They RYW-chain into
a single MCAS record:

- `nr_child ± 1` (the child-count edge, e.g. `ft_state_edge` in
  `ft_remove_one_commit`, ft-mutation-helpers.h:2531)
- `ft_flip_txn_record_tombstone` / `_tombstone_locked` — set TOMBSTONE
- `ft_flip_txn_record_release_lock` — `{s|LOCK → s}` (drop the lock)
- `ft_flip_txn_guard_parent` — `{live → live}` validate

Their composition is *engine-mediated*. Each reads the word via
`urcu_txn_load` (RYW), so it sees the same-txn pending value. The engine then
chains the records. ft-mutation-helpers.h:1306-1474 documents this
explicitly: "a live holder's guard and decrement fuse into one record".

Note the DISTINCT-slot count fold, `ft_flip_txn_record_count_parent`
(ft-mutation-helpers.h:3138). It is the order-statistic `nr_keys += delta<<1`,
walked over every ancestor `stable_base→root`, with tag
`FT_NR_KEYS_PROXY_TAG`. It is a *separate* word per node. So it is naturally
slot-distinct. It needs NO refold, only a mechanical record-primitive swap.
The same-slot hazard is confined to the `state` word.

**sw cannot do this**: no RYW, no same-slot reconcile. The migration replaces
the *composition* with *pre-computation*. Single-writer makes this trivial:

```
/* Under the lock, the writer knows the word exactly. */
old = uatomic_load(&meta->state, RELAXED);   /* raw read, we hold the lock  */
new = old;
new = ft_state_dec_child(new);               /* if this op decrements       */
new |=  FT_STATE_TOMBSTONE;                   /* if this op retires          */
new &= ~FT_STATE_LOCK;                     /* if this op held+releases    */
urcu_txn_sw_record(txn, &meta->state, (void*)old, (void*)new, FT_STATE_PROXY);
/* NO guard record — see §4.C */
```

One record per state word, computed once. The `{LOCK|s → s}` release stays:
the fence bit is still the lock, still cleared at commit. But it is now a
*component* of the single computed `new`, not a separate chained record.

This is a rewrite of the `ft_flip_txn_record_*` state-word family. It is
bounded, and mechanical once the pattern is set. The callers keep their
shape: 18 count sites, 16+7 tombstone, 10 release. They call one folded
helper instead of two.

### 4.B Engine-handle swap (`ft_flip_txn` internals)

`struct ft_flip_txn` wraps `urcu_mcas_txn *mtxn` (+ inline `own`) + `reserved`
+ `locks[8]` registry. The swap:

- `mtxn: urcu_mcas_txn → urcu_txn_sw_txn`. `create` → `urcu_txn_sw_init`;
  `create_bounded(cap)` → `urcu_txn_sw_reserve(cap)`; the on-stack bounded flip
  → `urcu_txn_sw_init_inline(buf, cap)` with a caller `aligned(16)` latch buf.
- `record_reserved`/`record_tag`/`record_count_parent`/`record_tombstone`/
  `record_release_lock` → `urcu_txn_sw_record(txn, slot, old, new, tag)`.
  Same {slot, old, new, tag} shape; the tag is the per-record arg already.
- `commit` → `urcu_txn_sw_commit` (flavor-bound). It returns
  OK/MEMORY_ERROR, **never ABORT**. The retry loop around every mutator
  collapses to "OOM→propagate, else done".
- `destroy` (pre-commit bail) → free the record array + `copying_clear_all`.
  This is simpler. sw has no live descriptor to `urcu_mcas_destroy` distinct
  from the block. OOM is sticky on the handle.
- Drop: `reserved` gymnastics stay (sw `reserve` is the same contract).
  `expect_conflict`/`urcu_txn_conflict` are deleted.

### 4.C Guards & the read-set — DELETED under single-writer

Every `ft_flip_txn_guard_parent` (21) and `ft_flip_txn_lock_or_guard_parent`
(14) has one purpose. It detects a *concurrent* retire/relocate of a node
between descent and commit. The code already states that this is dead under
exclusion:
- ft-mutation-helpers.h:1420: "Under the retained single-writer exclusion the
  guard always passes ⇒ behaviour-identical";
- :1458: "Dead-at-guard is unreachable under a single writer".

Under sw + lock, the writer holds the holder's lock (or the FT-wide lock)
across plan→edit→commit. No peer can retire it. So:

- `guard_parent` → **removed**. Optionally, keep a debug-build raw `assert`
  on the word for defense-in-depth.
- `lock_or_guard_parent` → collapses to its LOCK arm only. The guard fallback
  was for the acquire-miss under FINE. Under a correct single-writer, that
  path is the FT-wide lock already held. The acquire cannot miss for a reason
  that matters. This interacts with the fault-injection probes: see risks §9.
- The masking-guard self-abort livelock (the Step A gotcha) **cannot exist**
  in sw: there is no guard and no abort.

### 4.D Same-slot audit (the SLOT-COINCIDENCE review, per the rcu skill)

sw forbids two records on one slot in a commit. Every *composed* FT commit
must be proven slot-distinct or refolded. The census identified the **fused
"atomic trie↔head-anchor" commits** below as the ones that fold the most
edges. Each must be proven slot-distinct, or its `state`-word coincidences
refolded per §4.A:

- `ft_remove_one_commit` (helpers:2469→2538): structural unlink + cell/run
  unsplice (≤4) + `freeze_leaf` hlist MARK + `nr_child--` on `state`.
- `ft_chain_compress_fused` (remove.h:433-733) is **the largest fold**:
  - forward publish (+SKIP_X dual);
  - back-edge (parent,offset);
  - up to **3 `record_tombstone_locked`** (boundary + parent_cn + child_cn:
    distinct nodes ⇒ distinct `state` words);
  - orphan freezes;
  - `freeze_leaf` hlist MARK;
  - nr_keys count fold;
  - cell unsplice.
- `ft_detach_node` (remove.h:1645-2013): forward republish + orphan tombstones +
  `retire_glue` free-list tombstones + `freeze_leaf` hlist MARK + nr_keys walk
  + §4.B guard.
- `ft_promote_head` (remove.h:2192-2366): forward head publish + head-cell swap
  + `next_node->prev` fold + `@node` freeze + `hold_or_lock_parent` RELEASE.
- `ft_unchain_node` head-no-succ (remove.h:2510-2534).
- `cds_ft_replace` head cases (insert.h:3690-3768).
- External-promote/recompaction publishes (remove.h:177/201/292).
- **Dual cross-trie root swaps** (`ft_root_list_swap_publish_dual`): 2 roots
  + 2×4 endpoint edges + old-root tombstone. This gives cross-trie atomicity,
  so a reader sees a key in exactly one trie.

Audit rule (per the skill): prove distinctness *by construction*. Enumerate
the same-address record pairs in each commit above. Do not trust a clean soak.
sw's `install()` pair-scan debug-asserts distinctness. But a rare-shape
coincidence would corrupt silently in a release build.

The expected hits are the `state`-word coincidences: tombstone, release,
guard and nr_child on one node. §4.A's pre-fold handles them. The
pointer/cell/nr_keys edges should fall out distinct.
`ft_chain_compress_fused`'s 3 tombstones are on distinct nodes. That is the
one to double-check.

### 4.G The §8.3 layout split becomes MANDATORY (correctness, not just contention)

The lock-escalation model's §8.3 moves the parent-owned fields
`{parent_slot_offset, incoming_byte}` out of `state`. They go into their own
word, adjacent to `parent`. This leaves `state` cleanly `C`-owned:
`{proxy, lock, nr_child, …}`. That model marks §8.3 **APPROVED but
deferred**. The reasoning: while writers stay CAS loops, a cross-lock write
to the shared word costs "a spurious abort, never a lost update". MCAS
validates its expected old, and one side loses.

**That reasoning does not survive the sw migration.** sw has no CAS and no
abort. C's `state` word is written under TWO different locks:
- `nr_child` under C's lock;
- `parent_slot_offset` under P's lock (a re-home of C is P-owned, §8.1).

With `P ≠ C`, those two ops hold *different* locks and run *concurrently*.
Under sw, each does a plain read-modify-write of C's `state`. The second
silently overwrites the first. This is **a lost update**, exactly what §8.3
warns about. It is no longer downgraded to a spurious abort. Under sw,
`state`-word sharing across lock domains is a correctness bug.

Therefore **§8.3 is a hard prerequisite of the sw migration**, not a
deferrable optimization. Conveniently, it lands where the model already
sequences it: "at the END, when OPTIMISTIC is retired". Under REPLACE, that
*is* the sw cutover. And its payoff is exactly what sw wants. `nr_child` and
the re-home fields drop from CAS-retry to plain stores under a single lock.
§4.A's "compute the word once" is the same move. So these are ONE coupled
end-state, not independent steps:
- §8.3;
- the re-home rewrite;
- the state-word fold (§4.A);
- the FT-wide-lock drop.

### 4.F Helping reads collapse (`ft_flip_txn_resolve_prio`)

`ft_flip_txn_resolve_prio` (helpers:775) resolves a live child slot to its
committed value by `urcu_mcas_read`. It does so by *helping a doomed peer to
its terminal status*, so the resolver bypasses the read-set. Under
single-writer there are no concurrent peers to help. The slot is either a
plain value or this writer's own parked proxy. So it collapses to a plain
resolve (`urcu_txn_sw_proxy_get`), with no patience/help machinery. The same
holds for the reader-side `urcu_mcas_read` sites (§5.2).

### 4.E Chain fold → the Step B unblock (the payoff)

Once the structural txn is `urcu_txn_sw_txn`, the chain edges record into it
directly. There are two ways; decide at that point:

1. **Keep `ft-txn-hlist.h`, retag to sw.** Mechanically swap its
   `urcu_txn_store(txn,…,FT_HLIST_TAG)` for `urcu_txn_sw_record(txn,…,tag)`.
   Smallest diff. The hlist stays bespoke but rides sw.
2. **Adopt `urcu_txn_sw_list`.** Its `_add_after_prepare`/`_del_prepare`/
   `_replace_prepare` already take a `urcu_txn_sw_txn *` and record into it.
   This is *designed* to fold into the FT's structural sw txn. It retires the
   bespoke chain entirely. Larger diff, less code owned.

Either way, the fold is now legal because both sides are one engine. This is
the whole reason the migration is worth doing before Step B.

## 5. Reader-side proxy resolution swap

**Blast radius: chokepointed, NOT scattered** (census-confirmed). The reader
migration surface is ~5-7 named resolver helpers. They are reached from ~60
call sites, but through those funnels. FT slots carry six distinct proxy-tag
regimes, each with its own resolver family. The migration re-points the
engine backing of each family behind its chokepoint. No reader ever drives,
settles or helps a transaction (owner-only settle). sw preserves this
exactly.

| Family | Resolver (def) | Slots | Engine call today → sw |
|---|---|---|---|
| **A** structural flip-proxy (0xF) | **`ft_resolve_flip_proxy`** (ft-helpers.h:944) | root, child, external-head, prev | `urcu_mcas_resolve_record` → `urcu_txn_sw_proxy_get` |
| **C** parent-slot 2-edge coherent snapshot | `ft_resolve_parent_slot` (ft-helpers.h:1863) | parent ptr + state-word PSO | `urcu_mcas_status/is_proxy/untag` (raw) → **shared-group coherence (§5.1)** |
| **D** scalar state/count word (bit-0) | `ft_meta_nr_child_load`, `ft_nr_keys_load`, `ft_meta_parent_slot_offset_load` | `meta->state`, `nr_keys` | `urcu_mcas_read` (bounded-spin) → `urcu_txn_sw_proxy_get` (**no spin, §5.2**) |
| **E** hlist chain next (bit-0 + bit-1 mark) | `ft_hlist_resolve` / `cds_ft_node_next_rcu` | `node->next` | `urcu_mcas_resolve` → sw (with §4.E) |
| **F** ordinal-cell list (bit-0 + bit-1 mark) | `ft_ord_cell_resolve_ord` (ft-lookup-helpers.h:104) | `ord_next`/`ord_prev` | `urcu_txn_list_resolve` → sw-list resolve |

Family B (`ft_node_ptr*`) is post-resolution address masking — **not** a
resolver, no engine backing, unchanged.

Family A is the dominant regime. Recognise the `0xF` tag (unchanged). Then
resolve via `urcu_txn_sw_proxy_get(proxy)` (pure `ptr[selector]` acquire),
instead of `urcu_mcas_resolve_record`. All 6 wrappers funnel here:
`ft_dereference_*` / `ft_root_dereference*` / `ft_cn_child_*`. So ~60
descent/query/iter sites migrate by changing this one function.

### 5.1 Family C simplifies (shared-group coherence)

`ft_resolve_parent_slot` is the ONE place touching raw engine internals. It
resolves the parent pointer AND the state-word offset. It uses a SINGLE
`urcu_mcas_status` snapshot + a coherence re-read/retry loop. So a reader
never tears a mid-commit re-home across the two edges.

Under sw this **simplifies**. Today the re-home commits both edges together in
one MCAS txn. If it records both edges in one sw txn, both proxies share ONE
group selector. Resolving each then reads the same `0→1` word. This is
coherent *by construction*, with no snapshot-and-retry. The migration must
confirm that the re-home keeps both edges in one sw txn (it should). Then the
coherence loop collapses to two `urcu_txn_sw_proxy_get` calls.

### 5.2 Family D simplifies (no bounded spin)

`urcu_mcas_read` is the state/count reader. On an undecided proxy, it spins up
to `URCU_MCAS_WAIT_PATIENCE` (8192), waiting for the packed integer word to
*stabilise*. Then it falls back to the logical value.

Under sw, the state word is transacted as a `{old_word, new_word}` pair in the
latch's `ptr[]`. The "pointer" is the cast integer word. Live words keep bit 0
clear, which satisfies the tag contract. `urcu_txn_sw_proxy_get` returns the
full old-or-new word atomically off the monotone selector: **no torn
sub-field, no spin**. The bounded-spin patience machinery on the FT read path
disappears. `ft_meta_nr_child_load` etc. become a resolve-then-extract-bits.

### 5.3 Reader-coherence notes (unchanged concerns)

- **Monotonicity scope.** sw guarantees that the selector is monotone `0→1`.
  A *dependency-chained* descent inherits the order for free, and the FT
  descent is exactly that. The one non-chained hop is the back-edge JUMP. It
  is the subject of `in-trie-move-seqcount.md`. sw does not change that
  story. But the JUMP reader must be re-confirmed against sw's §"never
  new-then-old" caveat: a cached/independently-reached pointer owes itself an
  acquire.
- **No helping on the read side.** MCAS resolution already reads the logical
  value off the status word without helping. sw is the same (single writer,
  monotone selector). Semantics are preserved, and the machinery is simpler.

The chain (E) and cell (F) reader sides swap when §4.E lands. Both alias the
engine's bit-0 tag. So the `URCU_MCAS_TAG`-derived constants must be re-based
on the sw proxy's bit-0 tag, with their static-asserts:
`FT_HLIST_TAG`, `FT_ORD_CELL_TAG` and `CDS_FT_NODE_TXN_PROXY_TAG`.

## 6. What is DELETED (the simplification dividend, census-quantified)

- The read-set/guard apparatus. All of it is dead under single-writer (§4.C):
  - **~19** `ft_flip_txn_guard_parent`;
  - **~9** `_lock_or_guard_parent` (guard arm);
  - **3** `_hold_or_lock_parent`;
  - **2** `urcu_txn_load_validate`;
  - **1** `urcu_txn_validate`.
- Escalation/aging: **5** sites, `urcu_txn_expect_conflict` (2) +
  `urcu_txn_conflict` (3). `declare_disjoint`: **0** uses (non-issue).
- The ABORT retry lane in every mutator (sw never aborts). It collapses to
  "OOM→propagate, else done".
- The copying-registry's *abort* arm. Only the OOM / pre-commit-bail clear
  survives. The drain contract at helpers:616/663-672 is load-bearing on abort
  today. Under sw, only the destroy/OOM path remains.
- The masking-guard/release ordering rule and the Step A livelock class. They
  go with the guards.
- All transactional-load / read-policy discipline in the write path:
  `urcu_txn_load` RYW, helping reads, `ft_flip_txn_resolve_prio`'s help. No
  transactional loads exist in sw (§4.F).
- Reader-side: the bounded-spin `URCU_MCAS_WAIT_PATIENCE` machinery on
  state/count reads (§5.2).

This is a large net *removal*. It removes exactly the machinery that the MW
campaign has spent the most effort getting right. That is itself an argument
for the migration, independent of the Step B unblock.

## 7. REPLACE vs COEXIST — DECIDED: REPLACE

- **COEXIST** (MCAS for OPTIMISTIC, sw for locked). Every `ft_flip_txn_*`
  needs two implementations, dispatched on `lock_mode`. There are also two
  reader resolution paths and two reclaim models. This is a permanent fork of
  the hottest code. Rejected unless OPTIMISTIC must ship indefinitely.
- **REPLACE** (sw only, lock mandatory). The sw end-state coincides with the
  lock end-state. LOCK_FINE + per-node locks + per-domain drop gives disjoint
  writers concurrency *without* MCAS. Same-key serializes on the shared lock.
  OPTIMISTIC/MCAS retires. One engine, one reader path. **Recommended**, but
  only *after* LOCK_FINE is load-bearing. Otherwise writers serialize and the
  disjoint-key milestone regresses.

The honest tension: OPTIMISTIC's disjoint-key result is a *lock-free* number.
LOCK_FINE's is a *fine-lock* number. They are not identical. The migration
makes a bet. It bets that fine-per-node-lock concurrency + a far simpler
engine is a better long-term point than lock-free + a complex engine. That
bet should be **measured** before OPTIMISTIC is deleted, not assumed. The
measure is a LOCK_FINE-post-drop vs OPTIMISTIC disjoint-key benchmark.

## 8. Sequencing (phased, each independently validated)

1. **Prereq — LOCK_FINE per-domain drop is DESIGNED.** It need not be fully
   landed, but the drop plan must exist. The drop decides whether sw ever runs
   concurrent, and it shares sw's exclusion contract. This is the lock
   pivot's own "NEXT".
2. **P1 — Build sw behind a flag, correctness-equivalent, SERIAL.** Migrate
   these, gated by a `FEATURE_FT_ENGINE_SW` build flag:
   - `ft_flip_txn` internals (§4.B);
   - state-word fold (§4.A);
   - drop guards (§4.C).

   This stays under the FT-wide lock (fully serial). Validate against the
   full oracle set + `URCU_TXN_SW_EXCL_VALIDATE`. No perf claim yet.
3. **P2 — Same-slot audit** (§4.D) across merge/graft/bulk/spine-copy. Prove
   every composed commit slot-distinct. Fold any coincidences.
4. **P3 — Reader resolution swap** (§5) behind the same flag. Re-confirm the
   back-edge JUMP vs sw monotonicity.
5. **P4 — Chain fold / Step B** (§4.E), now legal: hlist→sw (retag) or→sw-list.
6. **P5 — Land the LOCK_FINE per-domain drop under sw** → disjoint concurrency
   without MCAS. Benchmark vs OPTIMISTIC (§7). Only if it holds:
7. **P6 — Retire OPTIMISTIC/MCAS** from the FT: delete the dual path, make the
   flag the default, remove the engine-select.

P1–P4 are reversible (flag off = MCAS). P6 is the commitment. Gate it on P5's
benchmark.

## 9. Risks & open items

- **The perf bet (§7)** is the headline risk. Fine-per-node-lock disjoint
  throughput may underperform lock-free OPTIMISTIC by a wide margin. Then
  REPLACE is wrong, and we are stuck with COEXIST or with keeping MCAS.
  **Measure at P5.**
- **Fault-injection probes.** The FINE acquire-miss bails are
  `cds_ft_fault_lock_countdown` and the recompact/insert/remove fallbacks.
  Today fault injection drives them, because the FT-wide lock makes them dead.
  Under sw+§4.C the *guard* fallback is gone. The *acquire* semantics change:
  there is no MCAS abort to unwind into. The probes and
  `test_fine_lock_*_fault` tests must be re-derived for the sw acquire model.
- **Same-slot coincidence found late** (§4.D). A composed commit may coincide
  only on a rare shape. It would then corrupt silently in a release build. The
  debug assert catches it only if that shape runs. The audit must be by
  construction, per the skill's rule. Enumerate same-address record pairs. Do
  not trust a clean soak.
- **Reclaim timing.** MCAS defers descriptor reclaim engine-side. sw commit
  calls `call_rcu` on the block directly (nr≥2), or frees immediately (nr≤1).
  The GP-wait rule is unaffected, because sw commit does not synchronize.
  That rule is `ft_writer_lock_gp_wait`: never block on a GP holding the lock.
  But the reclaim accounting in the OOM/bail paths must be re-checked. It must
  match sw's "commit owns reclaim" contract.
- **Back-edge JUMP vs monotonicity** (§5). It is orthogonal to
  `in-trie-move-seqcount`. But it must be re-confirmed under sw's acquire
  requirement for non-chained hops.

## 10. Effort estimate (rough)

| Workstream | Size | Nature |
|---|---|---|
| §4.A state-word fold | **M** | Non-mechanical rewrite of the record-* family; the crux |
| §4.B handle swap | S–M | Mechanical; shape-preserving |
| §4.C drop guards | S | Deletion + a few debug asserts |
| §4.D same-slot audit | **M** | Proof work across merge/graft/bulk; find + refold coincidences |
| §5 reader swap | S | One chokepoint + JUMP re-confirm |
| §4.E chain fold (Step B) | S–M | Retag, or adopt sw-list |
| tests/probes re-derive | M | Fault model changes |
| P5 benchmark + P6 retire | M | The go/no-go gate |

No single giant step. The risk is concentrated in §4.A (correctness) and in
§7/P5 (the perf go/no-go), not in volume.

## 11. Bottom line

The migration is feasible and mostly a simplification. The sw engine was built
for it: 0xF tag, frozen-set contract, bounded-flip analog. It is **gated on
the lock pivot**. sw needs the exclusion. It only pays off once LOCK_FINE
drops the FT-wide lock per-domain. Recommended path: REPLACE (not coexist),
phased behind a flag. OPTIMISTIC retirement is gated on a P5 benchmark. The
one hard piece is folding the RYW-chained state-word edits into single
pre-computed records (§4.A). Everything else is mechanical swap or deletion.
Do not start P1 before the LOCK_FINE per-domain drop is designed.
