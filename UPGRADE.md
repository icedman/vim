# Buffer interface upgrade

## Goal

Make buffer text access go through a buffer-owned interface instead of reaching
through `buf_T` into its `memline_T` implementation. In practical terms, code
should ask a buffer for a line (conceptually `buf->line(lnum)`) rather than
read `buf->b_ml` or rely on the current buffer through `ml_get()`.

This is an internal C refactoring. It must preserve Vim's existing behavior,
including swap-file handling, undo, text properties, autocommands, recovery,
error messages, and the lifetime rules of returned line pointers.

## Current state

- `buf_T` embeds `memline_T b_ml` in `src/structs.h`.
- `ml_get_buf(buf, lnum, will_change)` and `ml_get_buf_len(buf, lnum)` are
  already the main explicit-buffer read entry points.
- `ml_get()`, `ml_get_len()`, `ml_replace()`, `ml_append()`, and
  `ml_delete()` operate on `curbuf` and therefore encode hidden context.
- Many callers inspect `buf->b_ml.ml_line_count` and
  `buf->b_ml.ml_mfp` directly. `BUFEMPTY()` also combines direct field access
  with an implicit current-buffer line lookup.
- Line pointers returned by `ml_get_*()` are temporary: another line access
  can invalidate the previous pointer. Some callers also request a writable
  line with `will_change = TRUE`.
- The first inventory found roughly 1,279 relevant matches across about 70
  source files when searching for `b_ml`, `ml_get*`, and `BUFEMPTY`; this is a
  baseline, not a promise that every match belongs to this migration.

## Proposed interface

Use names that fit Vim's existing C conventions rather than trying to make C
look like C++. The desired call shape is conceptually `buf->line(lnum)`, but
initially the implementation can be ordinary functions:

```c
char_u *buf_line(buf_T *buf, linenr_T lnum, int will_change);
colnr_T buf_line_len(buf_T *buf, linenr_T lnum);
linenr_T buf_line_count(buf_T *buf);
int buf_is_loaded(buf_T *buf);
int buf_is_empty(buf_T *buf);
int buf_append_line(buf_T *buf, linenr_T lnum, char_u *line,
                    colnr_T len, int flags);
int buf_replace_line(buf_T *buf, linenr_T lnum, char_u *line,
                     colnr_T len, int has_props, int copy);
int buf_delete_line(buf_T *buf, linenr_T lnum, int flags);
```

The spelling and exact signatures should be settled in the first
implementation patch. The important properties are:

1. Every operation receives the target buffer explicitly.
2. The interface owns bounds checking, loaded/unloaded behavior, cache
   invalidation, and forwarding to memline.
3. Callers do not need to know that the implementation uses a memfile,
   cached line, locked block, or `memline_T`.
4. Read-only and writable access are visibly different. Prefer separate
   functions if that makes accidental mutation harder.
5. Pointer lifetime and whether a returned line includes text properties are
   documented at the interface boundary.

During the transition, current `ml_*` functions can remain as compatibility
wrappers. New code should not add direct `b_ml` access.

## Migration plan

### Phase 0: establish rules and inventory

- Keep this document as the migration checklist.
- Establish the rule: new code must use the buffer interface for line
  content, line count, loaded state, and empty-buffer checks. Direct `b_ml`
  access is permitted only inside the memline implementation until Phase 3.
- Generate a categorized inventory with searches for:
  - `->b_ml` and `curbuf->b_ml`;
  - `ml_get`, `ml_get_buf`, `ml_get_len`, and related accessors;
  - `ml_append`, `ml_replace`, `ml_delete`, and related mutation calls;
  - direct uses of `ml_line_ptr`, `ml_locked`, and memline flags.
- Classify each use as read, mutation, cache/state management, recovery/swap
  implementation, or test/support code. Do not mechanically replace the
  memline implementation itself.
- Run `python3 tools/check_internal_access.py`. Its report is grouped by source
  file in `internal_access.txt`, includes per-file statistics, and ends with
  totals. Treat its mutation section as a review inventory: it heuristically
  identifies line variables obtained from `ml_get*()` and subsequently written;
  verify each candidate against C control flow before changing it.
- Review `ml_get_buf(..., TRUE)` occurrences separately, since they grant
  writable cached-line access even when the returned pointer is not locally
  captured.
#### Phase 0 status

- [x] Access rules established: new consumers must not add direct `b_ml`
  access or implicit current-buffer line operations.
- [x] Inventory tool created at `tools/check_internal_access.py`.
- [x] Inventory generated at `internal_access.txt`, grouped by source file,
  with per-file statistics and totals.
- [x] Mutation follow-up inventory added for line variables returned by
  `ml_get*()` and subsequently written.
- [x] Baseline `make -j2` completed successfully in `src/`.
- [x] Focused `make test_buffer.res` completed successfully in
  `src/testdir/`.

Baseline inventory totals: 449 source files scanned, 81 files with findings,
1,650 access findings, 11 mutation follow-up candidates, and 10 writable-line
accesses. Phase 0 is complete; the next step is Phase 1, adding the
read-only explicit-buffer façade.

### Phase 1: create the read-only façade

- Add explicit-buffer accessors for line, line length, line count, loaded state,
  and empty state near the existing buffer API.
- Implement them as thin wrappers over the existing memline operations, so
  this phase has no semantic change.
- Add the declarations to the appropriate `.pro` file and regenerate
  prototypes with `make proto` when required by the source change.
- Convert a small, representative group first: display, syntax, search, and
  one integration (Python/Lua/Ruby) caller.
- Add assertions or debug-only checks where useful to catch invalid buffers or
  line numbers at the new boundary.

#### Phase 1 status

- [x] Added explicit-buffer read-only accessors: `buf_line()`,
  `buf_line_len()`, `buf_line_count()`, `buf_is_loaded()`, and
  `buf_is_empty()`.
- [x] Kept the accessors as thin wrappers over existing memline behavior.
- [x] Added declarations to `src/proto/buffer.pro`.
- [x] Migrated representative reads in balloon evaluation, channel output,
  quickfix, Python integration, syntax handling, and search.
- [ ] Add dedicated API tests and debug assertions for invalid buffers and
  line numbers.
- [x] Regenerate prototypes with `make proto` after installing the required
  Python `clang.cindex` dependency; only `src/proto/buffer.pro` changed.

The source build and focused buffer test pass with the façade and migrations.
Phase 1 remains open until its API checks and debug assertions are added.

### Phase 2: migrate all consumers

#### Phase 2 progress

- [x] Migrated explicit-buffer `ml_get_buf()` and `ml_get_buf_len()` reads
  outside `memline.c` and the façade implementation to `buf_line()` and
  `buf_line_len()` across 42 source files.
- [x] Regenerated the internal-access inventory after the migration.
- [x] Rebuilt Vim and reran `test_buffer.res` successfully.
- [x] Replace direct line-count field reads and `BUFEMPTY()` uses with
  `buf_line_count()` and `buf_is_empty()` in consumer code.
- [x] Replace loaded-state null checks with `buf_is_loaded()` in consumer
  code, preserving inverse checks and compound conditions.
- [ ] Migrate implicit-current-buffer reads.
- [x] Added buffer-owned mutation APIs: `buf_append_line()`,
  `buf_replace_line()`, and `buf_delete_line()`.
- [x] Migrated mutation consumers, including language integrations and
  `if_py_both.h`, away from `ml_append()`, `ml_replace()`, and `ml_delete()`.
- [x] Added `tools/check_internal_mutations.py` and generated
  `internal_mutations.txt` as the mutation inventory.

The access inventory reports 1,075 findings, including 514 direct `b_ml`
accesses, 460 implicit-current-buffer line accesses, and one writable line
access. The mutation inventory reports 186 findings after the consumer
migration: 44 implicit mutation references, 17 explicit/internal mutation
references, one writable line access, 124 direct memline state mutations, and
two line-pointer follow-up candidates. Remaining mutation findings are
primarily implementation-side operations in `memline.c` and direct cache/state
management that still require deliberate review.

- Convert reads before mutations, grouping changes by subsystem to keep each
  patch buildable.
- Replace line-count and loaded-state field reads with accessors.
- Replace `BUFEMPTY()` with an explicit-buffer operation; retain a current-
buffer convenience wrapper only where the caller truly means `curbuf`.
- [x] Convert consumer mutation calls to buffer-owned API operations. The
  façade carries the target `buf_T *`; replace/delete compatibility entry
  points still rely on current-buffer context inside the memline implementation
  and return `FAIL` for a non-current buffer until that layer is refactored.
- Continue migrating implicit-current-buffer reads and remove the remaining
  memline context dependency from mutation implementations.
- Update language integrations and any code in headers/macros, not just `.c`
  files. Pay special attention to `if_py_both.h`, where buffer access is
  exposed through several collection operations.
- Leave memline internals (`memline.c` and its private helpers) allowed to use
  `memline_T`; the goal is to stop unrelated subsystems from depending on it.

### Phase 3: enforce the boundary

- Make `memline_T` private to the memline implementation if the include/layout
  structure permits it. If a complete `buf_T` definition must remain public,
  at least remove external direct access to `b_ml` and document the remaining
  implementation-only exceptions.
- Rename or relocate compatibility wrappers once all callers have migrated.
- Add a presubmit/review grep or static check that rejects new external
  `->b_ml`, `ml_line_ptr`, and implicit current-buffer text access.
- Re-run the inventory and require zero unclassified consumer accesses.

### Phase 4: harden and optimize

- Run the full test suite and feature-matrix builds, including minimal builds
  and language interfaces where available.
- Compare performance of hot paths such as screen drawing, insert/delete,
  syntax, and writing files. Avoid introducing allocations or buffer switches
  merely to obtain a line.
- Consider a future opaque buffer representation only after the accessor
  boundary is complete and measurable.

## Main challenges and decisions

### 1. `buf_T` is not currently opaque

The structure is defined in `src/structs.h` and many subsystems use fields
other than text. Hiding only `b_ml` may require include-order changes and
accessors for related state. Hiding the entire structure is a larger project;
do not couple it to the first line-access migration.

### 2. Implicit `curbuf` is deeply embedded

The existing mutation and convenience APIs often operate on `curbuf`. A
wrapper that silently switches buffers would be fragile: switching can run
autocommands, change windows, invalidate pointers, and alter error context.
The target API must carry `buf_T *` explicitly through the call chain.

### 3. Cached line pointer lifetime

A line pointer is valid only until a subsequent line access can flush or replace
the cache. The interface must preserve this contract or introduce an explicit
copy/borrow type. Do not “improve” lifetime accidentally by allocating every
line; that would affect hot paths and ownership assumptions.

### 4. Writable access and mutation bookkeeping

`ml_get_buf(..., TRUE)` marks cached state for modification, while
`ml_replace()` and friends handle undo, text properties, NetBeans, changed
state, and display updates through surrounding code. Reads and writes need
separate review and tests; a generic `line()` accessor must not make writes
look like harmless reads.

### 5. Unloaded, empty, invalid, and special buffers

A buffer can be never loaded, unloaded, empty, quickfix, terminal, prompt, or
otherwise special. Existing APIs return an empty string, an error marker, or
emit an error depending on the operation. The new interface must specify each
case and preserve the existing result and diagnostics.

### 6. Swap files, recovery, and memline cache state

`memline.c` manages block locking, translation, flushing, encryption, and
recovery. Direct field users may be coupled to these details in subtle ways.
Keep implementation-only operations in memline until their callers have a
well-defined replacement; do not replace state checks with a simplistic
“line count > 0” test.

### 7. Pointer invalidation across callbacks

Autocommands, scripting callbacks, redraw, and NetBeans can run while work is
in progress and may delete or unload a buffer. Existing `bufref_T` patterns
must remain in place. Accessors should not retain raw pointers across calls
that can invoke callbacks without revalidating the buffer.

### 8. Portability and generated declarations

Vim supports old and diverse C compilers. Keep signatures compatible with the
project's C dialect and use Vim types/wrappers. Changes to source functions
may require generated prototype updates; do not hand-edit generated files
unless the repository's normal generation step requires it.

### 9. Performance-sensitive call sites

Screen rendering, cursor movement, insert mode, syntax, and file writing can
access lines in tight loops. Thin inline wrappers may be appropriate after
correctness is established, but first measure rather than expose internals for
speed.

## Validation and completion criteria

A phase is complete only when:

- the relevant build succeeds without new warnings;
- focused buffer, editing, undo, recovery, swap-file, display, and scripting
  tests pass;
- behavior for unloaded and special buffers is covered;
- no newly migrated caller relies on a stale line pointer;
- the inventory is updated and the remaining direct accesses are explicitly
  justified as memline implementation code; and
- the full migration has a reviewable diff, not a broad formatting rewrite.

The overall upgrade is complete when external buffer consumers no longer read
`b_ml` or call implicit-current-buffer line operations, and the memline layout
can be changed without changing those consumers' code.

## Suggested patch sequence

1. Define the accessor contract and add read-only explicit-buffer wrappers.
2. Migrate line reads in one subsystem at a time, starting with low-risk
   consumers and including regression tests.
3. Migrate line count, loaded, and empty checks.
4. Add and migrate explicit-buffer mutation APIs.
5. Migrate integrations and macros; audit callback and `bufref_T` lifetimes.
6. Restrict direct memline access to implementation code and add enforcement.
7. Run full validation and measure hot paths before considering further
   encapsulation.
