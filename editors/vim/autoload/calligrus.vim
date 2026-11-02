" calligrus: live Markdown/Org preview in a separate calligrus window (spec 15).
"
" Copyright (C) 2026 KαΩSS (theKAOSSphere)
" SPDX-License-Identifier: GPL-3.0-or-later
"
" This program is free software: you can redistribute it and/or modify it under
" the terms of the GNU General Public License as published by the Free Software
" Foundation, either version 3 of the License, or (at your option) any later
" version. It is distributed WITHOUT ANY WARRANTY; see the GNU General Public
" License for details: <https://www.gnu.org/licenses/>.

" One shared `calligrus --editor` process per Vim session. We talk to it on its
" stdin, one JSON object per line:
"   open   {doc, path, format, text}   show this buffer now
"   text   {doc, text}                 the buffer changed
"   cursor {doc, line}                 scroll the preview to this source line
"   search {doc, text, case}           highlight this plain text
"   close  {doc}                       the buffer is gone
"   quit                               exit
" calligrus talks back with one message on its stdout (spec 23):
"   goto   {doc, line}                 Ctrl+click in the preview: move the cursor
" Everything else is "send messages when Vim events happen".

let s:job = v:null
" Bumped whenever the job is stopped or replaced, so a late exit callback of an
" old job is recognised and ignored.
let s:gen = 0
" s:previewed[bufnr] = 1 for buffers whose preview is on; s:muted: toggled off.
let s:previewed = {}
let s:muted = {}
let s:timers = {'text': {}, 'cursor': {}}
let s:last_search = {}

" ---- options (read each time, so :let g:calligrus_... takes effect at once)

function! s:opt(name, default) abort
  return get(g:, 'calligrus_' . a:name, a:default)
endfunction

function! s:warn(msg) abort
  echohl WarningMsg | echomsg 'calligrus: ' . a:msg | echohl None
endfunction

function! s:info(msg) abort
  echomsg 'calligrus: ' . a:msg
endfunction

" exe is a program name, or a list: [program, leading args...].
function! s:command(extra) abort
  let l:exe = s:opt('exe', 'calligrus')
  let l:cmd = type(l:exe) == v:t_list ? copy(l:exe) : [l:exe]
  return l:cmd + a:extra
endfunction

" ---- sending

function! s:send(msg) abort
  if s:job is v:null
    return 0
  endif
  try
    call ch_sendraw(s:job, json_encode(a:msg) . "\n")
  catch
    return 0
  endtry
  return 1
endfunction

function! s:text(buf) abort
  return join(getbufline(a:buf, 1, '$'), "\n")
endfunction

function! s:send_open(buf) abort
  let l:path = bufname(a:buf)
  if l:path !=# ''
    let l:path = fnamemodify(l:path, ':p')
  endif
  let l:format = getbufvar(a:buf, '&filetype') ==# 'org' ? 'org' : 'markdown'
  call s:send({'type': 'open', 'doc': string(a:buf), 'path': l:path,
        \ 'format': l:format, 'text': s:text(a:buf)})
endfunction

function! s:send_text(buf) abort
  call s:send({'type': 'text', 'doc': string(a:buf), 'text': s:text(a:buf)})
endfunction

" The cursor line of the window showing buf (the current window if it shows it), or 0.
function! s:cursor_line(buf) abort
  if bufnr('%') == a:buf
    return line('.')
  endif
  let l:win = bufwinid(a:buf)
  if l:win == -1
    return 0
  endif
  let g:calligrus_tmp_line = 0
  call win_execute(l:win, "let g:calligrus_tmp_line = line('.')")
  let l:n = g:calligrus_tmp_line
  unlet g:calligrus_tmp_line
  return l:n
endfunction

function! s:send_cursor(buf) abort
  let l:n = s:cursor_line(a:buf)
  if l:n > 0
    call s:send({'type': 'cursor', 'doc': string(a:buf), 'line': l:n})
  endif
endfunction

" ---- search highlight

" Vim's search pattern -> [plain text, case sensitive]. Real regexes give "":
" the preview matches plain text only, and a wrong highlight is worse than none.
function! calligrus#search_text(pat) abort
  let l:pat = a:pat
  let l:case = -1
  if stridx(l:pat, '\C') >= 0 | let l:case = 1 | endif
  if stridx(l:pat, '\c') >= 0 | let l:case = 0 | endif
  let l:pat = substitute(l:pat, '\\[cC]', '', 'g')
  let l:vnomagic = stridx(l:pat, '\V') >= 0
  let l:vmagic = stridx(l:pat, '\v') >= 0
  let l:pat = substitute(l:pat, '\\[vV]', '', 'g')
  let l:pat = substitute(l:pat, '\\[<>]', '', 'g')
  if l:vmagic
    let l:pat = substitute(l:pat, '[<>]', '', 'g')
    if l:pat =~# '[.*\[\]~^$\\{}+=?|()@%]'
      return ['', 0]
    endif
  elseif l:vnomagic
    if stridx(l:pat, '\') >= 0
      return ['', 0]
    endif
  else
    if l:pat =~# '[.*\[~\\]' || l:pat[0] ==# '^' || l:pat[-1:] ==# '$'
      return ['', 0]
    endif
  endif
  if l:case == -1
    let l:case = !&ignorecase || (&smartcase && l:pat =~# '\u')
  endif
  return [l:pat, l:case ? 1 : 0]
endfunction

function! s:current_search() abort
  if !&hlsearch || !v:hlsearch
    return ['', 0]
  endif
  return calligrus#search_text(getreg('/'))
endfunction

function! s:send_search(buf, force) abort
  if !has_key(s:previewed, a:buf)
    return
  endif
  let [l:text, l:case] = s:current_search()
  let l:key = l:case . '|' . l:text
  if a:force || get(s:last_search, a:buf, '') !=# l:key
    let s:last_search[a:buf] = l:key
    call s:send({'type': 'search', 'doc': string(a:buf), 'text': l:text,
          \ 'case': l:case ? v:true : v:false})
  endif
endfunction

" ---- timers

function! s:stop_timer(kind, buf) abort
  if has_key(s:timers[a:kind], a:buf)
    call timer_stop(remove(s:timers[a:kind], a:buf))
  endif
endfunction

function! s:stop_buf_timers(buf) abort
  call s:stop_timer('text', a:buf)
  call s:stop_timer('cursor', a:buf)
endfunction

function! s:stop_all_timers() abort
  for l:kind in keys(s:timers)
    for l:buf in keys(s:timers[l:kind])
      call s:stop_timer(l:kind, l:buf)
    endfor
  endfor
endfunction

function! s:fire(kind, buf, Fn, timer) abort
  if get(s:timers[a:kind], a:buf, -1) != a:timer
    return
  endif
  call remove(s:timers[a:kind], a:buf)
  if has_key(s:previewed, a:buf) && bufexists(a:buf)
    call call(a:Fn, [a:buf])
  endif
endfunction

" Run Fn once, ms after the LAST call for (kind, buf).
function! s:debounced(kind, buf, ms, Fn) abort
  call s:stop_timer(a:kind, a:buf)
  let s:timers[a:kind][a:buf] = timer_start(a:ms, function('s:fire', [a:kind, a:buf, a:Fn]))
endfunction

" Run Fn ms after the FIRST call; calls in between ride along. While you type
" non-stop the preview still updates every ms, and the last edit is always
" sent because Fn reads the buffer when the timer fires.
function! s:throttled(kind, buf, ms, Fn) abort
  if has_key(s:timers[a:kind], a:buf)
    return
  endif
  let s:timers[a:kind][a:buf] = timer_start(a:ms, function('s:fire', [a:kind, a:buf, a:Fn]))
endfunction

" ---- goto (Ctrl+click in the preview)

function! s:goto_line(buf, line) abort
  if bufnr('%') != a:buf
    let l:win = 0
    for l:w in range(1, winnr('$'))
      if winbufnr(l:w) == a:buf
        let l:win = l:w
        break
      endif
    endfor
    if l:win > 0
      execute l:win . 'wincmd w'
    else
      execute 'hide buffer ' . a:buf
    endif
  endif
  let l:n = max([1, min([float2nr(a:line), line('$')])])
  call cursor(l:n, 1)
  normal! ^
  normal! zv
endfunction

" One line from calligrus's stdout. Unknown or malformed: ignored.
function! s:on_out(ch, line) abort
  if !s:opt('goto', 1)
    return
  endif
  try
    let l:msg = json_decode(a:line)
  catch
    return
  endtry
  if type(l:msg) != v:t_dict || get(l:msg, 'type', '') !=# 'goto'
    return
  endif
  let l:doc = get(l:msg, 'doc', 0)
  let l:line = get(l:msg, 'line', '')
  if type(l:doc) != v:t_string || l:doc !~# '^\d\+$'
        \ || (type(l:line) != v:t_number && type(l:line) != v:t_float)
    return
  endif
  let l:buf = str2nr(l:doc)
  if !has_key(s:previewed, l:buf) || !bufexists(l:buf)
    return
  endif
  try
    call s:goto_line(l:buf, l:line)
  catch
    call s:warn('cannot go to line ' . string(l:line) . ': ' . v:exception)
  endtry
endfunction

" ---- the process

function! s:on_exit(gen, job, status) abort
  " A late exit callback of a job we stopped (or replaced) is ignored.
  if a:gen != s:gen
    return
  endif
  call s:reset()
  call s:warn('the preview window closed (exit code ' . a:status . '); previews are off')
endfunction

function! s:reset() abort
  let s:job = v:null
  let s:gen += 1
  let s:previewed = {}
  let s:last_search = {}
  call s:stop_all_timers()
  call s:remove_autocmds()
endfunction

function! s:ensure_job() abort
  if s:job isnot v:null
    return 1
  endif
  let s:gen += 1
  let l:cmd = s:command(['--editor'])
  try
    let l:job = job_start(l:cmd, {
          \ 'in_mode': 'nl', 'out_mode': 'nl', 'err_io': 'null',
          \ 'out_cb': function('s:on_out'),
          \ 'exit_cb': function('s:on_exit', [s:gen]),
          \ })
  catch
    call s:warn('cannot run `' . join(l:cmd, ' ') . '` (not found? see g:calligrus_exe)')
    return 0
  endtry
  if job_status(l:job) ==# 'fail'
    call s:warn('cannot run `' . join(l:cmd, ' ') . '` (not found? see g:calligrus_exe)')
    return 0
  endif
  let s:job = l:job
  call s:add_autocmds()
  return 1
endfunction

function! calligrus#is_running() abort
  return s:job isnot v:null && job_status(s:job) ==# 'run'
endfunction

" ---- autocmds (only while the process runs)

function! s:add_autocmds() abort
  augroup Calligrus
    autocmd!
    autocmd BufEnter,FileType * call s:on_enter(str2nr(expand('<abuf>')))
    autocmd CmdlineLeave * call timer_start(0, {-> s:send_search(bufnr('%'), 0)})
    autocmd OptionSet hlsearch call s:send_search(bufnr('%'), 0)
    autocmd TextChanged,TextChangedI * call s:on_text(str2nr(expand('<abuf>')))
    if exists('##TextChangedP')
      autocmd TextChangedP * call s:on_text(str2nr(expand('<abuf>')))
    endif
    autocmd CursorMoved,CursorMovedI * call s:on_cursor(str2nr(expand('<abuf>')))
    autocmd BufDelete,BufWipeout * call s:on_delete(str2nr(expand('<abuf>')))
    autocmd VimLeavePre * call calligrus#stop()
  augroup END
endfunction

function! s:remove_autocmds() abort
  augroup Calligrus
    autocmd!
  augroup END
endfunction

function! s:previewable(buf) abort
  return index(s:opt('filetypes', ['markdown', 'org']), getbufvar(a:buf, '&filetype')) >= 0
endfunction

" Switching to a previewed buffer shows it; with follow, any previewable buffer
" joins in while the process runs. FileType too: a new file gets its filetype
" only after BufEnter.
function! s:on_enter(buf) abort
  if a:buf != bufnr('%')
    return
  endif
  if s:opt('follow', 1) && s:job isnot v:null && !has_key(s:previewed, a:buf)
        \ && !has_key(s:muted, a:buf) && s:previewable(a:buf)
    let s:previewed[a:buf] = 1
  endif
  if has_key(s:previewed, a:buf)
    call s:send_open(a:buf)
    call s:send_cursor(a:buf)
    call s:send_search(a:buf, 1)
  endif
endfunction

function! s:on_text(buf) abort
  if has_key(s:previewed, a:buf)
    call s:throttled('text', a:buf, s:opt('debounce', 150), function('s:send_text'))
  endif
endfunction

function! s:on_cursor(buf) abort
  if has_key(s:previewed, a:buf)
    call s:debounced('cursor', a:buf, 50, function('s:send_cursor'))
    call s:send_search(a:buf, 0)
  endif
endfunction

function! s:on_delete(buf) abort
  silent! call remove(s:muted, a:buf)
  if has_key(s:previewed, a:buf)
    call s:send({'type': 'close', 'doc': string(a:buf)})
    call remove(s:previewed, a:buf)
    call s:stop_buf_timers(a:buf)
  endif
endfunction

" ---- commands

" :CalligrusToggle[!] -- preview the current buffer on/off.
function! calligrus#toggle(force) abort
  let l:buf = bufnr('%')
  if has_key(s:previewed, l:buf)
    call s:send({'type': 'close', 'doc': string(l:buf)})
    call remove(s:previewed, l:buf)
    let s:muted[l:buf] = 1
    call s:stop_buf_timers(l:buf)
    call s:info('preview off')
    return
  endif
  if !a:force && !s:previewable(l:buf)
    call s:warn("'filetype' is not one of " . join(s:opt('filetypes', ['markdown', 'org']), ', ')
          \ . ' (use :CalligrusToggle! to preview anyway)')
    return
  endif
  if !s:ensure_job()
    return
  endif
  silent! call remove(s:muted, l:buf)
  let s:previewed[l:buf] = 1
  call s:send_open(l:buf)
  call s:send_cursor(l:buf)
  call s:send_search(l:buf, 1)
endfunction

" :CalligrusStop -- quit the process; all previews turn off.
function! calligrus#stop() abort
  if s:job is v:null
    return
  endif
  let l:job = s:job
  call s:send({'type': 'quit'})
  " Closing stdin also makes it exit, whether or not it read the message.
  try
    call ch_close_in(l:job)
  catch
  endtry
  call s:reset()
endfunction

" :CalligrusOpenFile -- just open the file in a new, unconnected calligrus window.
function! calligrus#open_file() abort
  let l:path = bufname('%')
  if l:path ==# ''
    call s:warn('this buffer has no file')
    return
  endif
  let l:cmd = s:command([fnamemodify(l:path, ':p')])
  try
    let l:job = job_start(l:cmd, {'stoponexit': '', 'in_io': 'null', 'out_io': 'null', 'err_io': 'null'})
    if job_status(l:job) ==# 'fail'
      throw 'fail'
    endif
  catch
    call s:warn('cannot run `' . join(l:cmd, ' ') . '`')
  endtry
endfunction
