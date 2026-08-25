# Copy-on-write buffer snapshot upgrade

## Goal

Add a copy-on-write snapshot of buffer text.  A snapshot presents an immutable,
read-only view of one exact buffer revision and allows buffer queries to run
without blocking normal editing for the lifetime of the query.

The two non-negotiable goals are:

1. **Thread safety.** Snapshot queries must be safe from worker threads while
   the main thread edits, unloads, or deletes the originating buffer.  No
   snapshot reader may touch mutable `buf_T`, `memline_T`, memfile cache state,
   global editor state, or memory whose lifetime is controlled by the main
   thread.
2. **Low memory use.** Creating a snapshot must not copy the whole buffer.
   Snapshots share immutable storage with the live buffer, and editing copies
   only the smallest practical unit that is shared with a snapshot.

The snapshot interface is query-only.  It initially exposes line content, line
length, and line count.  It does not expose mutation, marks, options, undo,
windows, text properties, swap state, or callbacks.

This document is a design proposal.  The exact storage unit and synchronization
primitives must be validated with a prototype and measurements before the
public internal API is fixed.

## API shape

Names should follow the buffer interface added by the buffer upgrade:
`buf_line()`, `buf_line_len()`, and `buf_line_count()`.  The preferred API uses
an explicit, refcounted handle:

```c
typedef struct buf_snapshot buf_snapshot_T;

buf_snapshot_T *buf_snapshot_acquire(buf_T *buf);
void buf_snapshot_ref(buf_snapshot_T *snapshot);
void buf_snapshot_unref(buf_snapshot_T *snapshot);

varnumber_T buf_snapshot_tick(const buf_snapshot_T *snapshot);
const char_u *buf_snapshot_line(const buf_snapshot_T *snapshot,
                                linenr_T lnum);
colnr_T buf_snapshot_line_len(const buf_snapshot_T *snapshot,
                              linenr_T lnum);
linenr_T buf_snapshot_line_count(const buf_snapshot_T *snapshot);
```

Conceptually, a caller asking for revision `tick` performs:

```c
snapshot = buf_snapshot_acquire(buf);
tick = buf_snapshot_tick(snapshot);
line = buf_snapshot_line(snapshot, lnum);
```

This provides the intended equivalent of
`snapshot_line(buf, tick, lnum)` and `snapshot_line_count(buf, tick)`, but does
not make `(buf, tick)` the ownership token.  A bare tick is an identity, not a
lifetime guarantee.  Looking up a snapshot by `(buf, tick)` and returning a
pointer would race with buffer deletion or release of the final snapshot
reference.  It would also require a retained per-buffer revision registry,
which increases memory use.

If lookup by tick is later required, make acquisition explicit and fallible:

```c
buf_snapshot_T *buf_snapshot_acquire_tick(int fnum, varnumber_T tick);
```

That function must return a referenced handle or `NULL`; query functions still
operate on the handle.  Do not accept a raw `buf_T *` from a worker thread.
The first implementation should omit this registry until a real consumer needs
it.

### Contract

- `buf_snapshot_acquire()` is called on the main editor thread.  It captures the
  current text revision and returns one owned reference, or `NULL` on failure.
- `buf_snapshot_ref()`, `buf_snapshot_unref()`, and all query functions are
  safe on any thread.
- A snapshot remains valid after its source buffer is edited, unloaded, or
  deleted.
- `buf_snapshot_tick()` identifies the captured text revision.  The initial
  implementation may use `CHANGEDTICK(buf)`, but publication must ensure the
  tick and root describe the same committed revision.
- Query functions perform no allocation, locking, I/O, swap-file access,
  message emission, callback, or lazy cache fill.
- A line pointer is immutable and remains valid until the caller releases its
  snapshot reference.  Multiple line pointers from one snapshot may be held at
  once; unlike `buf_line()`, a later query does not invalidate an earlier one.
- Line bytes are NUL-terminated.  `buf_snapshot_line_len()` excludes the NUL.
  Embedded NUL representation must match the buffer interface contract.
- The initial API returns text only.  It must not expose packed text-property
  bytes.  Supporting properties later requires a separate typed query.
- Valid line numbers are `1` through `buf_snapshot_line_count(snapshot)`.
  Invalid input returns `NULL` for a line and `0` for its length, without using
  Vim's global error machinery.
- An unloaded buffer cannot produce a new snapshot.  Existing snapshots remain
  valid.  Vim's one-empty-line representation is preserved.
- Snapshot functions are observational only and never increment changedtick.

`const` prevents accidental writes through the API but is not the thread-safety
mechanism.  Immutability, ownership, and publication are.

## Proposed representation

Use a persistent tree whose leaves contain immutable line records.  A snapshot
handle owns a reference to one immutable root:

```c
struct buf_snapshot
{
    snapshot_node_T *bs_root;
    linenr_T         bs_line_count;
    varnumber_T      bs_tick;
    snapshot_ref_T   bs_refcount;
};
```

The exact fields remain private.  `snapshot_ref_T` represents a portable
reference-count implementation selected for Vim's supported platforms; it is
not a proposed concrete typedef.

### Immutable line records

A line record contains its byte length and NUL-terminated bytes in one
allocation where practical.  Records are immutable once published and may be
shared by the live revision and any number of snapshots.  Length is stored so
queries never call `STRLEN()`.

Do not create one allocation per line during conversion if that causes
unacceptable overhead.  Prefer immutable leaf slabs containing a bounded group
of line offsets and bytes.  The prototype should compare:

- fixed-size leaf slabs;
- size-bounded leaves, split when edited; and
- reuse of existing memline data blocks after separating mutable cache state
  from immutable payload.

The last option is likely lowest-memory, but only if block locking, swap-file
translation, encryption, and cache eviction are completely absent from the
reader path.

### Persistent index

Interior nodes store subtree line counts, allowing line lookup and total count
without mutable side tables.  The live buffer owns one root.  A snapshot is
normally O(1): increment the root reference and allocate only the small handle.

On append, replace, or delete, mutation uses path copying:

1. Build replacement leaf data without changing published storage.
2. Copy only nodes on the path from the changed leaf to the root.
3. Reuse and reference unchanged children.
4. Publish the fully initialized new root.
5. Release the live buffer's reference to the old root.

Readers that hold the old root continue to see the old revision.  Cost should
be O(log n) index nodes plus changed line/leaf bytes, not O(number of snapshots)
and not O(buffer size).

### What happens when a line is edited

The old line is not copied into a separate snapshot at edit time.  It is already
part of an immutable leaf owned by the currently published revision.  A
snapshot has retained that revision, and therefore retains the old leaf.

For example, replacing line 120 follows this sequence on the main thread:

1. Resolve line 120 to immutable leaf `A` in revision `R1`.
2. Allocate leaf `B` and copy the line table and bytes from `A`, applying the
   replacement while constructing `B`.  Neither `A` nor `R1` is modified.
3. Allocate copies of the interior nodes between `A` and the root.  Point the
   copied path at `B` and share all unaffected subtrees.
4. Construct revision `R2` with the new root, updated line count if needed, and
   the new tick.
5. After memline, undo, and other required mutation bookkeeping succeeds,
   atomically publish `R2` as the live revision.
6. Release the live reference to `R1`.  Snapshot references keep `R1`, `A`, and
   its old line 120 alive.  Without a snapshot, unreferenced old nodes are freed.

Thus "copy on write" means **copy the shared leaf before changing it**, not
"save the old line into every snapshot."  There is one old leaf regardless of
how many snapshots share that revision.  If a live leaf is proven uniquely
owned it is still preferable initially to construct a replacement leaf: never
mutating published storage makes the thread-safety invariant auditable.  A
future unique-owner optimization is allowed only before publication and only
if it cannot race with snapshot acquisition.

Insert and delete use the same rule.  They may split or merge leaves and update
subtree line counts, but published leaves and nodes remain unchanged.

### Relationship to memline blocks

Vim's buffer is already managed in memline data and pointer blocks, but those
blocks are not currently safe snapshot objects.  They participate in a mutable
cache and may be locked, translated, evicted, loaded from swap, encrypted, or
changed in place.  A worker snapshot must not invoke any of those mechanisms.

The snapshot should be block-based as well, because copying one allocation per
line or the whole buffer would waste memory.  However, snapshot **leaf slabs**
should initially be a separate immutable abstraction, not raw memline blocks.
They may use similar size targets and may eventually share an immutable payload
with memline after cache and swap metadata are separated from text storage.

Using exactly the same block boundaries is not required and may be harmful:

- memline block size is driven by swap and cache behavior;
- copy-on-write size is driven by edit amplification and retained memory;
- a one-byte edit should not preserve or copy an unnecessarily large block;
- a very long line must be represented without imposing a huge normal leaf
  size; and
- insertions should split leaves without renumbering or copying unrelated text.

The initial recommendation is a size-bounded leaf slab containing multiple
whole lines, offsets, lengths, and packed text bytes.  Target a small bounded
payload, permit an oversize leaf for one long line, and split a leaf when an
edit exceeds the target.  The prototype must determine the target size; it
should not simply inherit the memline block size without measurements.

Leaf size is the main memory/performance tuning decision.  Copying an entire
large memline block for a one-byte edit may retain too much old text; very small
leaves increase index and allocation overhead.  Measure realistic editing,
large files, long lines, and many retained snapshots before selecting it.

## Thread-safety model

Thread safety is an unbreakable invariant, not a best-effort property.

### Main-thread ownership

Only the main editor thread may:

- acquire a snapshot from `buf_T`;
- mutate or publish the live root;
- interact with memline, memfile, swap files, undo, text properties, changed
  state, autocommands, or global Vim state.

Worker threads receive only referenced `buf_snapshot_T *` handles.  They must
never receive `buf_T *` for snapshot access.

### Publication

All nodes and line storage are fully initialized before they become reachable
from a published root.  Publishing a root uses release semantics; acquiring or
referencing a published snapshot uses matching acquire semantics.  Published
nodes are never modified.

A new root and its tick form one revision.  Mutation must not expose the new
root with the old tick or vice versa.  Prefer constructing a revision object
that contains both and atomically publishing one pointer rather than publishing
fields independently.

### Reclamation

Reference counting is the initial reclamation strategy because snapshots have
explicit lifetimes and Vim already has many reference-counted objects.  A node
or slab is freed only after the live revision and every snapshot that can reach
it have released their references.

The implementation must use real atomic reference operations when references
cross threads.  `volatile`, the current `buf_T` lock fields, and assumptions
about word-sized writes are not synchronization.  If Vim cannot provide the
required atomics on a supported target, snapshot worker-thread support must be
compile-time unavailable there; it must not silently become unsafe.

Recursive destruction can overflow the C stack or create a long pause when the
last large snapshot is released.  Use iterative release or bounded tree depth,
and permit final reclamation to be queued to the main thread if platform
allocators are not safe to call from workers.  Queueing must not require the
source buffer to remain alive.

### No hidden mutable state

Snapshot queries cannot call `buf_line()`, `ml_get_buf()`, or any helper that
locks blocks, updates a cached line, reads a swap file, reports an error, or
consults `curbuf`.  Thread sanitizers and code review should treat any such call
as a correctness failure.

## Integration with the buffer interface

The live buffer interface remains the sole mutation boundary:

```c
buf_append_line(...)
buf_replace_line(...)
buf_delete_line(...)
```

Initially, these operations should update memline and the persistent snapshot
representation in one main-thread transaction.  They publish a new snapshot
revision only after the mutation succeeds.  On failure, neither the published
root nor its tick changes.

This dual representation is suitable for proving the API but may temporarily
increase memory use.  The end state should make the persistent immutable text
store the buffer's canonical payload, with memline reduced or adapted to swap,
recovery, and compatibility responsibilities.  Keeping two permanent complete
copies would violate the low-memory goal.

Every mutation path must pass through the buffer interface before snapshots
can be declared correct.  Existing internal memline mutations and writable
`buf_line(..., TRUE)` access are blockers because they can bypass copy-on-write
and revision publication.  The buffer-interface enforcement tool should gain a
snapshot mode that rejects bypasses outside explicitly reviewed internals.

Undo is not automatically a snapshot implementation.  Undo entries encode
changes and may be pruned, are tied to mutable buffer state, and are not a
thread-safe random-access text store.  Shared immutable text records may later
be reused by both systems, but their ownership contracts stay separate.

## Memory policy

- Snapshot creation is O(1) apart from the handle.
- Unchanged text and index nodes are shared across revisions.
- Mutation copies only the changed immutable leaf and its index path.
- No retained global `(buffer, tick)` registry in the initial implementation.
- No line copy on query and no per-query allocation.
- Cache values must either be immutable and stored in nodes, or thread-local;
  there is no mutable shared line cache.
- Empty leaves and underfull neighboring leaves may be merged during later
  mutations, never by changing a published node.
- Add accounting for bytes retained solely by snapshots.  This is diagnostic
  data, not an automatic eviction mechanism: a live referenced snapshot cannot
  be invalidated to save memory.
- Allocation failure while editing must preserve the old live revision and
  existing snapshots.  The mutation reports failure through the existing
  buffer API; partial trees are released.

## Phased implementation

### Phase 0: settle invariants and measure

- [x] Inventory every text mutation, including writable cached-line access and
  memline-internal paths.
- [x] Define the representative memory and edit benchmark matrix that must be
  implemented with the prototype before choosing leaf size.
- [x] Audit existing atomic/refcount facilities and define the portability
  requirement and unsupported-build behavior.
- [x] Audit changedtick and decide whether it identifies snapshot revisions.

#### Phase 0 findings

The existing `tools/check_internal_mutations.py` inventory is the baseline.
`internal_mutations.txt` currently reports 171 raw findings across nine files:
44 implicit mutations, 17 explicit/internal mutations, one writable line
access, and 109 direct memline state mutations.  It reports no line-pointer
follow-up candidates.  Most findings are memline implementation internals or
comments/prototypes, but the writable cached-line path in `memline.c` proves
that successful snapshot publication cannot yet be attached only to
`buf_append_line()`, `buf_replace_line()`, and `buf_delete_line()`.

`b:changedtick` is not the snapshot revision.  Normal text notification
increments it, but bulk low-level mutations may be grouped under one increment,
undo may increment it more than once, and `unchanged()` increments it for
modified-state and write transitions that do not change text.  Buffer load,
recovery, and deletion also affect it.  A snapshot revision therefore has its
own monotonically increasing per-buffer sequence, incremented exactly once
when a new immutable text root is published.  A snapshot may retain the source
changedtick for diagnostics or conservative invalidation, but must not expose
it as its canonical revision identity.

Vim has no reusable portable atomic or mutex abstraction.  Existing Vim object
reference counts are plain `int` and are main-thread/re-entrancy mechanisms,
not cross-thread synchronization.  Windows code provides `Interlocked*`
precedents, but core configure support does not currently detect C11 atomics,
compiler atomic builtins, or general pthread synchronization.  A plain Vim
refcount, `volatile`, or `sig_atomic_t` is forbidden for snapshots.

Before Phase 1 exposes cross-thread retain/release, add a small private
snapshot-atomic portability layer with:

- acquire/release pointer publication;
- atomic increment and decrement with safe final-release semantics;
- a Windows `Interlocked*` backend;
- configure-tested compiler/C11 backends where available; and
- compile-time disabling of worker-thread snapshots on legacy targets where no
  correct backend exists.

A main-thread-only correctness prototype may use ordinary ownership internally,
but it must not claim thread safety or publish the API to workers until this
layer exists.  A mutex fallback is acceptable only after Vim gains reliable
thread-library detection and linkage; Python-specific `-pthread` flags are not
sufficient.

#### Phase 0 status

Phase 0 design and source audits are complete.  Phase 1 begins with two
implementation prerequisites: the private atomic portability layer and a
benchmark harness capable of reporting snapshot-owned/retained bytes.  Leaf
size remains intentionally undecided until prototype measurements exist.

### Phase 1: immutable prototype

- [x] Add a private snapshot handle and immutable, size-bounded leaf slabs.
- [x] Add a sorted immutable leaf index with binary-search line lookup.  Replace
  it with the persistent tree when Phase 2 adds path-copying mutations.
- [x] Build a snapshot from a loaded buffer with a one-time full copy.  This is
  a correctness prototype only, not completion of the low-memory goal.
- [x] Add line, line-length, line-count, revision, and allocated-size queries.
- [x] Add focused integration coverage through the private `test_getvalue()`
  test hook, including empty and oversized-leaf lines.
- [x] Add atomic reference implementations for Windows `Interlocked*` and
  supported GCC/Clang `__atomic_*` builtins, with explicit capability reporting
  and a main-thread-only fallback.
- [x] Add concurrent query, reference, and worker final-release tests for the
  POSIX-thread and Windows thread backends.  Tests run only when the matching
  thread and atomic capabilities are available.
- [x] Add a real internal consumer: `buf_contents_changed()` captures the
  original buffer and compares the re-read temporary buffer against immutable
  snapshot lines.
- [x] Add allocation-failure coverage at every Phase 1 snapshot allocation.
- [x] Test stable pointers, invalid queries, non-final reference release, and a
  retained snapshot across source edits and buffer wipeout.
- [x] Verify query implementations use only immutable snapshot storage.  The
  acquisition path alone reads through the buffer interface.

#### Phase 1 implementation status

`src/buffer_snapshot.c` implements the mirror prototype using a 4 KiB
experimental leaf target.  Each leaf owns line offsets, stored lengths, and
packed NUL-terminated text.  A line larger than the target receives its own
oversized leaf.  The snapshot owns a sorted leaf-pointer array and line lookup
uses binary search.  Snapshot construction allocates an upper-bound pointer
array of one pointer per source line; Phase 2 should replace this prototype
tradeoff with incrementally allocated persistent index nodes.

The buffer carries `b_snapshot_revision`, currently initialized to zero.  It is
intentionally not derived from `b:changedtick`.  Phase 2 must increment it once
per successfully published immutable text root before the revision value is
meaningful to consumers.

The snapshot query path is immutable, allocation-free, and independent of
memline.  Snapshot references use `InterlockedIncrement()`/
`InterlockedDecrement()` on Windows and GCC/Clang `__atomic_*` operations where
supported.  Final release on those backends frees snapshot-owned allocations
without consulting Vim globals.  `buf_snapshot_threadsafe()` reports whether
cross-thread ownership is supported; it returns false for the plain-counter
fallback and `MEM_PROFILE` builds.  Snapshot acquisition remains main-thread-
only, and handle publication must occur through an independently synchronized
queue.  No worker consumer may bypass the capability check.

The atomic operations are selected conservatively from compiler/platform
macros.  Configure now detects a usable POSIX `pthread_create()` implementation
for the Unix concurrency test; Windows uses `CreateThread()`.  The stress test
runs four workers, repeatedly queries and retains/releases one snapshot, drops
the main reference before joining, and therefore exercises final release from
a worker.  Compiler-atomic configure probes remain desirable before treating
the backend list as final, especially for legacy toolchains.

### Phase 2: copy-on-write live revisions

- [x] Add a buffer-owned immutable revision and O(1) repeated acquisition.
- [x] Retain old revisions independently when the live buffer is edited,
  unloaded, or wiped.
- [x] Invalidate the buffer-owned revision on writable line access, cached-line
  replacement/length adjustment, and successful append/replace/delete façade
  operations.
- [x] Clear the buffer-owned reference during memline close and buffer teardown.
- [x] Add persistent AVL path copying for append, replace, and delete through
  the buffer mutation façade.
- [x] Publish the candidate root and tick as one revision after successful
  memline mutation.
- [x] Cross-check façade append, replace, and delete results against live buffer
  contents while retaining an older generation.
- [x] Convert direct cached-line writers from conservative invalidation to an
  idempotent logical-change commit operation.
- [x] Remove normal cached-line invalidation and its subsequent full-copy
  acquisition fallback.

#### Phase 2A status: live revision ownership

The buffer now owns `b_snapshot`, one reference to its current immutable
revision.  `buf_snapshot_acquire()` builds this revision lazily when absent and
otherwise only increments its reference count, making repeated acquisition O(1)
without duplicating text.  Existing handles keep the old revision alive after
the buffer drops its reference.

Operations that bypass the append/replace/delete façade still conservatively
call `buf_snapshot_invalidate()`.  The next acquisition rebuilds the full
immutable mirror from final memline contents.  This closes stale-snapshot
correctness holes for direct cached-line writers, but can create false revisions
for temporary writable access.  In particular, `buf_line(..., TRUE)` invalidates
before the caller writes because the current API has no later commit boundary.

The snapshot revision increments once when a previously published live revision
is invalidated.  Further writes while no live revision exists do not increment
again.  This gives stable identity to repeated acquisitions and a new identity
after rebuilding changed text, but persistent publication will become the
canonical increment point in Phase 2B.

#### Phase 2B status: persistent façade mutations

The flat leaf array has been replaced by a subtree-counted persistent AVL rope.
Branch nodes contain immutable child pointers, subtree line counts, and heights.
Leaves contain one packed slab with offsets, lengths, and NUL-terminated text.
Lookups descend by subtree line count without absolute line numbers.

When a published revision exists, `buf_append_line()`, `buf_replace_line()`, and
`buf_delete_line()` first build a complete candidate revision.  The operation
copies the affected leaf and AVL path while referencing unchanged subtrees.  If
candidate allocation fails, memline is not changed.  If memline fails, the
candidate is released.  After memline succeeds, the candidate root, line count,
and revision are published together by replacing the buffer-owned snapshot.
A candidate prepared from a stale base is rejected; after a successful memline
mutation this falls back to invalidation so a later query rebuilds correct text.

Multi-line leaves that exceed the experimental 4 KiB text target are recursively
split and balanced.  A single oversized line remains a valid oversized leaf.
Deleting the final line publishes Vim's canonical one-empty-line revision.
Buffers without a published snapshot do not pay COW allocation costs; their
first acquisition still lazily builds the current tree.

Stress coverage builds a 300-line revision, retains it, then performs repeated
append, replace, and delete operations while checking every current line against
memline and every retained line against the old generation.  Allocation-failure
coverage verifies candidate preparation does not mutate live text.

#### Phase 2C status: cached-line commit boundary

Writable line access and cached-line storage helpers no longer invalidate a
published revision.  `changed_common()` commits finalized visible text through
`buf_snapshot_sync()`, which compares the reported range, path-copies only lines
whose bytes differ, and publishes the entire logical range under one new
revision.  Façade mutations are idempotent at this boundary because their
already-published lines compare equal.

This placement intentionally ignores temporary writers such as `ex_uniq()` and
formatting probes that restore their bytes before any logical change
notification.  Property-only cached-line repacking also remains outside the text
revision stream because it uses display/property notifications rather than
`changed_common()`.

Insert-mode tab compaction can finalize bytes after its earlier insertion
notification, so `ins_tab()` performs one explicit final-line sync.  Help-buffer
filtering similarly performs a dedicated whole-buffer sync without marking the
buffer modified.  Allocation failure during a post-change sync conservatively
invalidates the live revision; the next query rebuilds correct text.

Phase 2 is complete for the audited mutation paths.  Phase 3 should enforce the
commit boundary and remove or annotate any newly discovered mutation bypasses.

### Phase 3: close mutation bypasses

- [x] Encapsulate writable `buf_line(..., TRUE)` access behind the explicit
  `buf_line_mutable()` interface.
- [x] Require all text mutations to update the canonical revision through the
  buffer API.
- [x] Extend interface enforcement and run a full-source audit.
- [x] Define behavior for load, reload, unload, wipeout, recovery, and buffer
  replacement.

#### Phase 3A status: writable access and lifecycle ownership

All audited in-place line writers now use `buf_line_mutable()`.  The accessor's
contract requires callers to report finalized logical changes through the
normal `changed_*()` interface, where `buf_snapshot_sync()` updates the
canonical revision.  Temporary writers may restore bytes before that boundary
without creating a revision.  Writable line access is now distinguishable from
read access in a full-source search, making newly introduced bypasses visible.

Loading and buffer replacement build no eager snapshot; the first acquisition
lazily mirrors the final loaded contents.  Reload, unload, wipeout, recovery
failure, and any other path that closes memline call `ml_close()`, which drops
the buffer-owned revision.  Externally retained snapshot handles continue to
own their immutable revision and remain valid.  A subsequent load or successful
recovery therefore starts with no live revision and lazily publishes one from
the replacement contents.  Lifecycle tests cover unload, reload, forced reload,
and wipeout while retaining the pre-unload generation.

#### Phase 3B status: mutation boundary enforcement

`tools/check_buffer_interface.py` now rejects legacy writable
`buf_line(..., TRUE)` calls and new `buf_line_mutable()` callers outside the
explicitly audited set.  These rules are enforced even for files otherwise
classified as buffer or memline implementation exceptions.  Existing mutable
callers either restore temporary bytes before notification or reach an audited
`changed_*()` boundary that synchronizes the finalized text.  External direct
memline mutations remain forbidden, so text mutation consumers must use the
buffer-owned mutation API.

`make check-buffer-interface` checks added source lines and
`make audit-buffer-interface` checks the full source tree.  Both pass.  The
regenerated `internal_mutations.txt` scans 451 source files and reports 13
writable-access occurrences and six pointer-write candidates.  Review found
only the mutable accessor and prototype, one internal memline cache preparation,
and the already audited callers; there are no unclassified mutation bypasses.
Phase 3 is complete.

### Phase 4: consolidate storage

- Avoid permanent duplicate text ownership between memline and snapshot
  storage.
- Reuse immutable payload in swap/recovery where safe, or make memline consume
  the persistent store through a private adapter.
- Tune leaf sizing, tree fanout, release batching, and allocator overhead from
  measurements.

### Phase 5: enable worker consumers

- Enable only after sanitizer and stress validation.
- Pass referenced snapshot handles through worker queues.
- Document cancellation and ownership at each consumer.
- Keep all result application and buffer mutation on the main thread, checking
  that the originating buffer/revision is still relevant before applying a
  result.

## Tests and validation

### Functional tests

- Snapshot empty, one-line, multiline, very-long-line, and unloaded buffers.
- Retain a snapshot across append, replace, delete, undo, redo, reload, unload,
  and wipeout; verify the old text and count never change.
- Hold pointers to several snapshot lines and verify later queries and live
  edits do not invalidate them.
- Verify invalid line numbers and allocation failures have deterministic,
  callback-free results.
- Verify text-property mutations do not corrupt text snapshots and are not
  exposed as line bytes.
- Compare every snapshot line and length against the captured live revision in
  debug tests.

### Concurrency tests

- Many readers query the same snapshot while the main thread repeatedly edits
  and publishes revisions.
- Readers retain and release different generations while the buffer is
  unloaded and deleted.
- Race final `unref` operations from multiple workers.
- Run supported configurations under ThreadSanitizer and AddressSanitizer.
- Add deterministic stress hooks around publication and final reference drops,
  rather than relying only on timing-sensitive tests.

### Memory and performance tests

The benchmark harness must run the same workloads against the pre-snapshot
baseline and candidate leaf payload targets, initially 1, 4, 16, and 64 KiB.
Use deterministic generated buffers and report wall time, allocations,
snapshot-owned bytes, bytes retained solely by old revisions, and peak resident
memory where the platform can provide it.

Required datasets are:

- 100,000 short lines of 20 bytes;
- 100,000 medium lines of 200 bytes;
- a mixed source-like distribution;
- 1,000 lines of 1 MiB each; and
- a single line larger than every candidate leaf target.

Required workloads are:

- acquire and release 1, 10, and 1,000 snapshots without edits;
- replace one byte repeatedly in the same line;
- replace one byte in uniformly random lines;
- append and delete 10,000 lines at the start, middle, and end;
- retain 1, 10, and 100 generations while sparse edits continue;
- sequentially scan every line and perform one million random line queries;
  and
- release the final snapshot retaining a large old revision.

Measure and verify:

- snapshot acquisition remains O(1) versus buffer size;
- retained bytes are proportional to changed leaves and copied index paths;
- many snapshots of one revision do not duplicate text storage;
- very long lines do not force the normal leaf target to grow;
- insert/delete throughput in hot editing paths remains acceptable;
- query functions perform no allocation; and
- final release has bounded stack usage and acceptable latency.

A full-buffer copy on every snapshot, one complete tree per snapshot, or
unbounded revision retention fails the design regardless of functional test
results.

## Completion criteria

The upgrade is complete only when:

- snapshot acquisition shares the current immutable root and does not copy all
  text;
- snapshot queries are read-only, callback-free, allocation-free, and safe on
  worker threads;
- a snapshot survives edits, unload, and deletion of its source buffer;
- all text mutations participate in copy-on-write publication;
- returned line pointers remain valid for the snapshot lifetime;
- atomic publication and reclamation are validated on every enabled platform;
- focused buffer, undo, recovery, swap, text-property, and scripting tests
  pass;
- concurrency stress plus available thread/address sanitizer runs pass; and
- measured memory retention and editing overhead are acceptable and documented.

Thread safety takes precedence over API convenience and performance.  Low
memory use takes precedence over retaining a historical revision registry.
Neither goal may be weakened silently for a supported build.
