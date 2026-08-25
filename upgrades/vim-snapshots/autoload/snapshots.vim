vim9script

var latest_by_buffer: dict<dict<any>> = {}
var pane_by_buffer: dict<number> = {}


def ResolveBuffer(buf: any): number
  var nr = type(buf) == v:t_number ? buf : bufnr(buf)
  if nr <= 0 || !bufexists(nr) || !bufloaded(nr)
    throw $'vim-snapshots: buffer is not loaded: {string(buf)}'
  endif
  return nr
enddef


def Key(bufnr: number): string
  return string(bufnr)
enddef


def Render(bufnr: number)
  var pane = get(pane_by_buffer, Key(bufnr), 0)
  if pane == 0 || win_id2win(pane) == 0
    if has_key(pane_by_buffer, Key(bufnr))
      remove(pane_by_buffer, Key(bufnr))
    endif
    return
  endif

  var snapshot = get(latest_by_buffer, Key(bufnr), {})
  if empty(snapshot)
    return
  endif

  var pane_buf = winbufnr(pane)
  setbufvar(pane_buf, '&modifiable', 1)
  setbufline(pane_buf, 1, snapshot.lines)
  var old_count = len(getbufline(pane_buf, 1, '$'))
  if old_count > len(snapshot.lines)
    deletebufline(pane_buf, len(snapshot.lines) + 1, old_count)
  endif
  setbufvar(pane_buf, '&modified', 0)
  setbufvar(pane_buf, '&modifiable', 0)
enddef


export def Snapshot(buf: any = bufnr()): dict<any>
  var nr = ResolveBuffer(buf)
  var lines = getbufline(nr, 1, '$')
  var result = {
    bufnr: nr,
    changedtick: getbufvar(nr, 'changedtick'),
    lines: lines,
  }
  return result
enddef


export def Take(buf: any = bufnr()): dict<any>
  var snapshot = Snapshot(buf)
  latest_by_buffer[Key(snapshot.bufnr)] = snapshot
  Render(snapshot.bufnr)
  return snapshot
enddef


export def Latest(buf: any = bufnr()): dict<any>
  var nr = ResolveBuffer(buf)
  return get(latest_by_buffer, Key(nr), {})
enddef


export def Line(snapshot: dict<any>, lnum: number): string
  if lnum < 1 || lnum > len(snapshot.lines)
    return ''
  endif
  return snapshot.lines[lnum - 1]
enddef


export def LineCount(snapshot: dict<any>): number
  return len(snapshot.lines)
enddef


export def Tick(snapshot: dict<any>): number
  return snapshot.changedtick
enddef


export def Open()
  if getbufvar(bufnr(), 'vim_snapshots_pane', false)
    return
  endif

  var source_buf = bufnr()
  var source_win = win_getid()
  Take(source_buf)
  var width = get(g:, 'vim_snapshots_width', 50)
  rightbelow vertical new
  execute $'vertical resize {width}'
  var pane = win_getid()
  pane_by_buffer[Key(source_buf)] = pane
  setbufvar(bufnr(), 'vim_snapshots_pane', true)
  setbufvar(bufnr(), 'vim_snapshots_source', source_buf)
  execute $'file [Snapshot:{source_buf}]'
  setlocal buftype=nofile bufhidden=wipe noswapfile nobuflisted
  setlocal nowrap nonumber norelativenumber nospell
  setlocal modifiable
  Render(source_buf)
  win_gotoid(source_win)
  Sync(source_buf)
enddef


export def Close(buf: any = bufnr())
  var nr = ResolveBuffer(buf)
  var pane = get(pane_by_buffer, Key(nr), 0)
  if pane != 0 && win_id2win(pane) != 0
    win_execute(pane, 'close')
  endif
  if has_key(pane_by_buffer, Key(nr))
    remove(pane_by_buffer, Key(nr))
  endif
enddef


export def Sync(buf: any = bufnr())
  if getbufvar(bufnr(), 'vim_snapshots_pane', false)
    return
  endif
  var nr = ResolveBuffer(buf)
  var pane = get(pane_by_buffer, Key(nr), 0)
  if pane == 0 || win_id2win(pane) == 0
    return
  endif
  var pos = getcurpos()
  win_execute(pane, $'call cursor({pos[1]}, {pos[2]})')
enddef


export def Forget(buf: any)
  var nr = type(buf) == v:t_number ? buf : bufnr(buf)
  if nr <= 0
    return
  endif
  if has_key(latest_by_buffer, Key(nr))
    remove(latest_by_buffer, Key(nr))
  endif
  if has_key(pane_by_buffer, Key(nr))
    remove(pane_by_buffer, Key(nr))
  endif
enddef
