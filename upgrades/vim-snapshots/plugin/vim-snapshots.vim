vim9script

if exists('g:loaded_vim_snapshots')
  finish
endif
g:loaded_vim_snapshots = 1

command! SnapshotOpen snapshots#Open()
command! SnapshotClose snapshots#Close()
command! SnapshotTake snapshots#Take()
command! SnapshotRefresh snapshots#Take()

augroup vim_snapshots
  autocmd!
  autocmd TextChanged,TextChangedI * if !getbufvar(bufnr(), 'vim_snapshots_pane', false) | snapshots#Take() | endif
  autocmd CursorMoved,CursorMovedI * snapshots#Sync()
  autocmd BufWipeout * snapshots#Forget(str2nr(expand('<abuf>')))
augroup END
