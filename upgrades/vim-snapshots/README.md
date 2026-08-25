# vim-snapshots

`vim-snapshots` displays the latest immutable snapshot of the active buffer in a
right-hand pane. The pane follows the source cursor and refreshes after text
changes, but its rows are always rendered from a captured snapshot rather than
read directly from the live buffer.

## Install

Add this directory to Vim's runtime path, for example:

```vim
set runtimepath^=/path/to/vim/upgrades/vim-snapshots
```

Then run:

```vim
:SnapshotOpen
```

Set `g:vim_snapshots_width` before opening a pane to change its default width
from 50 columns.

## Commands

- `:SnapshotOpen` opens the snapshot pane for the current buffer.
- `:SnapshotClose` closes it.
- `:SnapshotTake` captures and publishes a new latest snapshot.
- `:SnapshotRefresh` is an alias for `:SnapshotTake`.

## Vim scripting API

```vim
var snapshot = snapshots#Snapshot(bufnr())
var published = snapshots#Take(bufnr())
var latest = snapshots#Latest(bufnr())
echo snapshots#Line(snapshot, 1)
echo snapshots#LineCount(snapshot)
echo snapshots#Tick(snapshot)
```

A snapshot is a detached dictionary with `bufnr`, `changedtick`, and `lines`
fields. It remains unchanged when its source buffer is edited (callers should
treat the returned value as read-only). This is a Vimscript-level snapshot; it
does not expose the C `buf_snapshot_T *` handle or provide worker-thread access.
