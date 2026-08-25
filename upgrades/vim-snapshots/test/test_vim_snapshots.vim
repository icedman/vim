vim9script

execute 'set runtimepath^=' .. fnameescape(fnamemodify(expand('<sfile>'), ':p:h:h'))
runtime plugin/vim-snapshots.vim

var source = bufnr()
setline(1, ['one', 'two'])
var snapshot = snapshots#Take(source)
assert_equal(2, snapshots#LineCount(snapshot))
assert_equal('two', snapshots#Line(snapshot, 2))
var tick = snapshots#Tick(snapshot)
setline(1, 'changed')
assert_equal('one', snapshots#Line(snapshot, 1))
snapshots#Take(source)
assert_notequal(tick, snapshots#Tick(snapshots#Latest(source)))

SnapshotOpen
assert_equal(2, winnr('$'))
var pane = winbufnr(getwininfo()->filter((_, w) => w.winid != win_getid())[0].winid)
assert_equal(['changed', 'two'], getbufline(pane, 1, '$'))
assert_false(getbufvar(pane, '&modifiable'))
SnapshotClose
assert_equal(1, winnr('$'))

if !empty(v:errors)
  writefile(v:errors, 'test-errors.log')
  cquit
endif
quit!
