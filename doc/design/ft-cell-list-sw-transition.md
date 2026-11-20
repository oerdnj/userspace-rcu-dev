# Completing the ordered-cell list's SW transition

Status (2026-09-26): the park is **MANDATORY**. `FEATURE_FT_CELL_SW` and
`NO_FEATURE_FT_ROOT_SW` are gone. A held cell edge and a held `&ft->root` are
always recorded SW. An unheld one aborts (`FT CELL UNHELD` / `FT ROOT
UNHELD`). `-DNO_FT_CELL_OWNED_STRICT` / `-DNO_FT_ROOT_OWNED_STRICT` remain, as
red controls only. Per-node is the only lock spacing (@7b842e0b), so the
coarse-spacing refusal in step 4 no longer exists.

The history below was measured 2026-09-20 at `per-node` on the same-key
contention shape, while the park was still opt-in.

**Target: 100% SW records.** Not "sound" -- sound is what turning the park off
already bought. The four steps below are what it takes to park every cell
edge. Step 3 is the one that makes the last two ownerless classes ownable.

## Why it is off

@89a1f7ec parked ordered-cell edges SW "whenever the op HOLDS the cell's
nearest ancestor lock". It left the rest MW. The reasoning was that an unheld
edge *"degrades to exactly today's behaviour"*. It does not. Today's behaviour
was **all-MW**: every writer of a cell word arbitrated by CAS and a loser
retried. A partial conversion instead leaves **one word carrying two writer
disciplines**:

| kind | what `urcu_txn_desc_commit` does to the word |
|---|---|
| `URCU_TXN_KIND_SW` | `uatomic_store(slot, new, CMM_RELEASE)` — *"caller-exclusive, so a plain release store IS the atomic commit"* |
| `URCU_TXN_KIND_MW` | `uatomic_cmpxchg(slot, old, new)` |

and settle *"stores old_ptr (abort) or new_ptr (commit) **BLIND**"*
(`urcu_txn_add`'s header). So:

* writer **X** holds the anchor → parks SW → stores **unconditionally**;
* writer **Y** does not hold it → CASes;
* they are concurrent **precisely because Y never took the lock X holds**;
* Y's CAS succeeds, X's blind store lands on top, Y's edge is gone.

A lost list link is a lost key. The reverse interleaving is safe: X stores,
then Y's CAS sees a changed word and retries. This is why the defect is rare
and load-shaped rather than deterministic.

**The engine's SW/MW kind detector cannot see this.** So the conversion's
green `--enable-rcu-debug` run is not evidence either way. The detector looks
at **one descriptor** only:
- `urcu_txn_record_chain` resolves via `urcu_txn_find(t, slot)`;
- the duplicate-slot assert loops over `t->recs[]`.

Two *concurrent* txns recording one slot with different kinds is outside what
it examines. Armed, firing, and blind to exactly this class.

### ☠ What is observed, and what the A/B actually says

The mechanism above is a **code-level hazard**, established by reading the
commit paths. It is NOT established as the cause of anything measured. One
attribution has been **refuted**.

The A/B below is matched:
- one source tree and one flag;
- arms run ALONE and alternating;
- 120 runs each of the full `ft_inv` `ioff`/`per-node` leg.

| arm | runs | key loss | proxy at rest | livelock | clean |
|---|---:|---:|---:|---:|---:|
| OFF (all-MW) | 120 | **4** | 0 | 0 | 116 |
| ON (55% SW) | 120 | **1** | 0 | 2 | 117 |

**Key loss is MORE frequent with the park OFF.** 4 vs 1 at n=120 is not
significant in either direction. So the honest reading is: *no measurable
difference, and the park is not the cause.*

So this failure is a SEPARATE, PRE-EXISTING defect:
`inv_concurrent_same_key_inserts_nolist: key K worker W: chain has 0, owner
says 1` -> `not ok 138`. It hit 5 of 240 runs, in **both arms**. The all-MW
discipline reproduces it too. It is open and unattributed.

The type-7 flip proxy AT REST in `cell->parent` --

    ft_verify: depth 8: head H cell C {parent 0x..3bf, node N}
                             != expected {owner 0x..c0b, node N}

-- was seen **once**, on a build carrying the previous session's census
probes. It did not recur in these 240 probe-free runs. Two facts support the
observation:
- `cell->node` matched. So the cell was the right cell, and the failing term
  was the parent word alone.
- All four workers were blocked in the next barrier. So every mutator had
  returned, and the verifier's premise held.

But one observation on an instrumented build is a lead, not a result. The
back-channel at-rest check added for it has **never fired**. It is armed and
lacks a positive control.

`FT REMOVE LIVELOCK` tracks load, not the park (2/120 ON, 0/120 OFF here; 33/48
in *both* arms of an earlier oversubscribed batch).

### So why is the park still off?

Not because of the evidence above -- it does not support that. Because of the
code. Take an SW blind store and an MW CAS on one word, with no lock excluding
the MW writer. That is unsound as written. And the standing rule is
*"complete the lock-set transition first; only when COMPLETE can MW become
SW"*. Off is the "locks first" half of that ordering. The work below starts
from there.

## The rule

**One discipline per word.** A cell edge may park SW only when *every* writer
of that word holds the word's nearest-ancestor lock. This is Mathieu's
standing rule: complete the lock-set transition first; only when COMPLETE can
MW become SW. No per-class conversion, no partial flip.

The duplicate chain honoured this: its UNHELD population was driven to **0**
before @80e855f5 parked it SW by default.

### Which refusals are per-WORD (safe) and which are per-OP (the gap)

The rule is per word. So a refusal that is a property of the *word* is
harmless, however large:
- every writer of that word refuses alike;
- the word stays uniformly MW;
- MW-vs-MW is exactly the discipline that shipped.

Only a refusal that varies **between ops writing the same word** creates the
mix.

| refusal | keyed on | verdict |
|---|---|---|
| `sentinel` | `ft_ord_is_end(owner_cell)` — `&c->lnode == &ft->ord_sentinel.node`, a pure identity test on the slot's OWNER | **per-word, so SOUND** (uniformly MW for every writer) — but not the end state: see step 3, it gets its own locks and converts. |
| `noctx` at a coarse spacing | `!e->ctx && lock_spacing != PER_NODE` — true for *every* cell edge there | **per-word (vacuously). SAFE:** exponential and root-only convert 0%, so they are uniformly MW. |
| `nocell` | the producer passed no `owner_cell` ("not a cell link") | **per-op in principle**, and the hazard if two producers disagree about the same word. Measured **0** at per-node; keep it at 0. |
| `nodepth`, `nometa` | the climb / metadata lookup for this op | **per-op.** Measured **0** at per-node. |
| **`notheld`** | *this op* does not hold the anchor | **PER-OP. THIS IS THE GAP.** |

So the **soundness** gap is exactly two things:
- the `notheld` population;
- the insert lane below, which never asks at all.

The sentinel's 13.1% is sound as it stands. But it is still *work*. The target
is not "sound": it is **100% SW records** (step 3).

## What blocks completion, measured

78,447 cell edges through `ft_cell_edge_owner`:

| population | count | % | what it needs |
|---|---:|---:|---|
| CONVERTED (anchor held) | 43,936 | 56.0 | — |
| **anchor NOT HELD** | 24,205 | 30.9 | **the soundness gap** — widen three acquires |
| neighbour is the sentinel | 10,306 | 13.1 | sound as-is; converts once it has a lock (step 3) |

The NOT-HELD edges, keyed by the txn's creation site — *the acquire that owes
the anchor* (`(unattributed)` is 0, so the table is complete):

| acquire | edges |
|---|---:|
| `ft-remove.h:6228` (head-promotion cell swap) | 14,642 |
| `ft-remove.h:4845` (`ft_detach_node`'s commit) | 9,488 |
| `ft-remove.h:1519` | 75 |

There is also a **fourth population the census never counted**.
`ft_txn_list_insert_between_prepare` records *both* its edges with
`urcu_txn_store_mw` unconditionally (`FT_AB_OWN_NA`). That is **19,628
stores** from `ft-insert.h:459`, on the same words this function parks SW.

## ☠ The insert lane's MW is load-bearing, not laziness

It cannot simply be re-recorded through the owner-bearing recorder. Its own
header says why it exists:

> records `@succ_expected` as the `&pos->next` expected old — so a later
> interposition **fails the commit's value CAS** instead of being adopted.

The MW kind on `&pos->next` *is* the interposition detector. A peer may land
a node between `pos` and `succ_expected` after the caller decided the key
order. The MW kind is how the insert notices it. Park it SW, and the blind
store would **adopt** the interposition silently. The peer's node is then
dropped.

That is the same conclusion from the other direction. The CAS is only
redundant once the lock makes interposition impossible. **So the transition is a locking
change, not a recording change.**

## Order of work

Two steps, not four -- the sentinel is not a blocker (see the table above).

1. **Widen the three `ft-remove.h` acquires** so the neighbour cells' anchors are
   in the lock set. `ft_promote_head` shows the shape (`ft-remove.h:6228`, the
   largest at 14,642):
   - Its caller `ft_unchain_node` takes ONE member, the chain head's holder,
     with `ft_acquire_member(ft, ctx, lock_nf, ..., &h)`.
   - It then hands it down as the `held_holder` PARAMETER.
   - The cell swap afterwards rewrites the NEIGHBOUR cells' links. That
     acquire never took their holders.

   The blocker is structural, not local. `held_holder` arrives already
   acquired. So `old_cell`, hence pred/succ, is resolved only *after* a lock
   is held. Adding anchors at that point breaks the ascending anchor-address
   order. That order is the entire deadlock argument for plain-CAS takes. So:

   * resolve `old_cell`'s pred and succ **before** the first acquire;
   * derive their holders (`pred->parent`, `succ->parent`) and anchors;
   * take the head holder, the pred anchor and the succ anchor in ONE ordered
     take (`ft_dlm_acquire_set`), sorted ascending;
   * **re-validate pred/succ after the take** and retry on change -- they were
     read with no exclusion held. `ft_unchain_node` already carries exactly
     this idiom for the holder itself ("★ RE-DERIVE AFTER THE ACQUIRE"). So the
     pattern is established rather than invented;
   * a pred/succ that is the sentinel contributes no anchor and needs none --
     its edge stays MW by the table above.

   `ft-remove.h:4845` (`ft_detach_node`'s commit, 9,488) and `:1519` (75) take
   the same treatment.

   ### ☠☠ ATTEMPTED 2026-09-21 AND IT LIVELOCKS — measured, not predicted

   The widening above was implemented for `ft_promote_head` and **reverted**.
   Patch kept at
   `fractal-trie-review-2026-06/widen-remove-neighbour-anchors-LIVELOCKS-2026-09-21.patch`.

   `inv_concurrent_same_key_removes`, filtered, per-node, list-off:

   | build | result |
   |---|---|
   | widened | **rc=124 (hang), twice** |
   | same tree, widening reverted | rc=0, census 55.6% / 14.3% / 30.1% |

   Matched control: same tree, same flags, same command, one file reverted.

   It is a **LIVELOCK, not a deadlock**. gdb on the hung process shows the
   four workers at ~62% CPU each, in state `R`:
   - three inside `cds_ft_remove` (one at `ft_txn_attempt_bail`);
   - one in `cds_ft_insert`.

   Nothing is blocked. They are all spinning in their retry loops.

   **Mechanism.** The remove now holds THREE anchors across its commit (head
   holder + pred + succ). Before, it held one. `skr_writer` states its own
   shape: *"Every writer walks the keys in the SAME order: the point is to
   have all T removers on one chain at once."* Adjacent keys share or
   neighbour their holders. So one remover's neighbour hold is the next
   remover's PRIMARY acquire. Each then misses, returns `-EAGAIN`, re-derives
   and retries. None converges.

   Two things that did **not** fix it, both tried:

   * Making the neighbour take non-fatal. An all-or-none miss then simply
     skips the conversion, with no `-EAGAIN`. The livelock is caused by the
     SUCCESSFUL takes, not the failing ones.
   * Moving the take from the holder's set down to its consumption point.
     That fixed a real bug. The first version added the members to the
     holder's set. This left SEVEN exits in `ft_unchain_node` with nothing to
     release them, and a leaked lock is permanent. But the hang survived the
     fix.

   **So the widening is not "add the anchors".** It is a contention change. It
   needs one of these:
   - the retry side handled: fairness or a bounded wait rather than an
     abort-and-respin;
   - a granularity that does not make one key's remove exclude its
     neighbours'.

   ⇒ That is an argument for doing step 3's dedicated locks FIRST. Those are
   per-FT words, touched only by the first and last key in the trie. They
   close 14.3% + 0.9% without going near the hot remove acquire.

   `ft_cell_lock_member()` is KEPT. It is the acquire-side twin of
   `ft_cell_edge_owner`, and mirrors its bail sequence word for word. It is
   correct. It is what any version of this step needs. It is inert while the
   park is off.

2. **Widen `ft-insert.h:459`'s acquire** to hold `pos` and `succ`'s anchors, then
   route `ft_txn_list_insert_between_prepare` through `ft_cell_edge_owner`.
   Order matters and is not stylistic. Its MW is the interposition detector
   (below). So the lock must make interposition impossible *before* the CAS
   is dropped.

3. **Give the ownerless words their own locks** (Mathieu, 2026-09-20). This
   note called two populations "never converts". They are only ownerless
   because nothing was ever made to own them. That is a decision, not a fact:

   * **`&ft->root`** — `MW_ALWAYS`'s `ROOT` class, measured 300 (0.9%).
     `fractal-trie-internal.h` says *"no node to lock, NEVER converts"* and
     `ft_cell_edge_owner`'s header says *"no node owns &ft->root"*. A lock
     dedicated to the root pointer retires both claims.
   * **the cell list's BEGIN and END** — `&ft->ord_sentinel.node.next` and
     `.prev`, the 10,306 sentinel edges. **One lock each, not one for both.**
     The two words are independent. A shared lock would serialise a head
     splice against a tail splice for no reason.

   These live in `struct cds_ft`, so they have addresses. They sort into the
   same ascending anchor-address order the ordered take already uses. So the
   deadlock argument composes unchanged. Contention is low by construction:
   - only the first and last key in the whole trie touch the list extremes;
   - root pointer changes are rare.

   ☠ `ft_ord_is_end()` folds `!c` (no cell at all) together with "is the
   sentinel". With a lockable sentinel, those two stop being the same answer.
   The predicate has to split.

   **What the implementation needs, from reading the DLM.** A "lock" in this
   trie is just the `FT_STATE_LOCK` bit in the `state` word of a
   `struct cds_ft_metadata`. It is taken by CAS (`ft_meta_lock_acquire`). The
   ordered take sorts it by the anchor's ADDRESS. So a dedicated lock is an
   anchor object. Embed three of them in `struct cds_ft`, beside
   `ord_sentinel`. They then get all of that for free, including a sortable
   address.

   The existing non-node locks are **not** a template. `writer_lock`
   (`cds_fair_mutex`) and `move_gate_lock` (`pthread_mutex_t`) are mutexes.
   They do not participate in the anchor-address order at all. These three
   must, because they will be taken alongside node anchors in one set.

   ☠ **`ft_dlm_member` cannot express a lock-only member today.**
   `ft_dlm_acquire_set_at` counts `if (set[i].nf) nr_present++`. It treats a
   NULL `.nf` as a HOLE to skip (that is what @7c0b6749 fixed). A dedicated
   lock has no `struct cds_ft_inode_flag`. Synthesising one is the wrong move:
   too much code dereferences `.nf` as a real node. So the change is:
   - The member grows an explicit lock-only form (an `.anchor` plus a flag).
   - `ft_lock_set_order_by_anchor()` learns to take that anchor as given,
     instead of deriving it from `.nf`/`.depth`. This function is where each
     member's anchor is derived and the order computed.
   - Such a member carries no `guard_child` / `guard_pf` and no depth. There
     is no parent edge to validate.

   Only then can `ft_cell_edge_owner` answer for these words:
   - `&ft->root_lock` for the root pointer;
   - `&ft->ord_begin_lock` when the slot is `&ord_sentinel.node.next`;
   - `&ft->ord_end_lock` when it is `.prev`.

4. ☑ (2026-09-26) Flip the default and delete `FT_CELL_SW_ENABLED`. **The
   target is 100% SW records**, not merely "no per-op refusal". That means
   all of these at 0: `notheld`, `nocell`, `nodepth`, `nometa`, `sentinel`
   and the insert lane. The coarse spacings remain a separate axis. They
   refuse uniformly today (`!e->ctx && lock_spacing != PER_NODE`), so they
   are sound. Threading `@ctx` to the cell producers is what converts them.

## ☠ Why the ablation is NOT in the gate matrix yet (historical: the opt-in is gone)

`chainmw` exists so `-DNO_FEATURE_FT_CHAIN_SW` cannot go unbuildable. An
ablation nobody compiles is worthless exactly when a bisect needs it. The same
argument applies here, with one difference:
- that ablation restores the **sound** arm, so it is green;
- `-DFEATURE_FT_CELL_SW` restores the **unsound** one.

A config that is permanently red masks every real regression behind it. A
strict control and a test of what it forbids cannot coexist.

So: keep the flag compiling (this note is its record). When it is added to
the matrix, add it with the `u` leg only. `ft_unit` has no concurrent writers,
so the SW/MW mix cannot arise there. The arm then stays green while still
being built. Add the concurrent legs at step 5, when they are supposed to
pass.

## The instrument

`ft_verify_no_proxy_at_rest` covered only **forward** edges:
root, child slot, `cn->child`, compressed `skip_slot` and `external_nodes`.
It never covered a word naming a **parent**. That is exactly the class the
conversion moved. It is now applied to the three back-channel reads in
`ft_verify_external_chain`:
- head cell link;
- head prev list-on/off;
- interior prev.

So this class reports as itself, instead of as an opaque
`!= expected {owner ...}` mismatch.
