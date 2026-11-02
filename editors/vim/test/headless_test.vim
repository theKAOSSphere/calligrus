" Run from editors/vim:  vim -Nu NONE -es -S test/headless_test.vim
" with CALLIGRUS_STUB_LOG set to a scratch file and `python` (or $CALLIGRUS_PYTHON) on PATH.
" Prints PASS/FAIL lines and exits non-zero on failure.
"
" Copyright (C) 2026 KαΩSS (theKAOSSphere)
" SPDX-License-Identifier: GPL-3.0-or-later

set nocompatible hidden
let s:here = fnamemodify(expand('<sfile>:p'), ':h')
execute 'set runtimepath^=' . fnameescape(fnamemodify(s:here, ':h'))
runtime plugin/calligrus.vim

let s:log = $CALLIGRUS_STUB_LOG
call delete(s:log)
" The stub writes lines we append to this file to its stdout (what the app's `goto` looks like).
let s:out = s:log . '.out'
call delete(s:out)
let $CALLIGRUS_STUB_OUT = s:out
let g:calligrus_exe = [empty($CALLIGRUS_PYTHON) ? 'python' : $CALLIGRUS_PYTHON, s:here . '/stub.py']
let g:calligrus_debounce = 50

let s:failures = 0
let s:report = []
function! s:check(name, ok) abort
  call add(s:report, (a:ok ? 'PASS ' : 'FAIL ') . a:name)
  if !a:ok | let s:failures += 1 | endif
endfunction

function! s:lines() abort
  return filereadable(s:log) ? readfile(s:log) : []
endfunction

" The JSON messages the stub has logged (ARGS and EOF lines skipped).
function! s:msgs() abort
  let l:out = []
  for l:l in s:lines()
    if l:l =~# '^{'
      try | call add(l:out, json_decode(l:l)) | catch | endtry
    endif
  endfor
  return l:out
endfunction

function! s:count(type) abort
  return len(filter(s:msgs(), 'v:val.type ==# a:type'))
endfunction

function! s:last() abort
  let l:m = s:msgs()
  return empty(l:m) ? {} : l:m[-1]
endfunction

" Poll Fn (a lambda) for up to 3 s; :sleep lets jobs and timers run.
function! s:wait_for(Fn) abort
  for l:i in range(150)
    if a:Fn() | return 1 | endif
    sleep 20m
  endfor
  return 0
endfunction

function! s:emit(s) abort
  call writefile([a:s, ''], s:out, 'ab')
endfunction

function! s:goto_msg(doc, line) abort
  return '{"type":"goto","doc":"' . a:doc . '","line":' . a:line . '}\n'
endfunction

function! s:last_is_cursor(n) abort
  let l:m = s:last()
  return get(l:m, 'type', '') ==# 'cursor' && l:m.line == a:n
endfunction

function! s:last_open_format() abort
  let l:o = filter(s:msgs(), 'v:val.type ==# "open"')
  return empty(l:o) ? '' : l:o[-1].format
endfunction

function! s:last_search_text(text) abort
  let l:o = filter(s:msgs(), 'v:val.type ==# "search"')
  return !empty(l:o) && l:o[-1].text ==# a:text && l:o[-1].case == v:true
endfunction

function! s:run() abort
  let l:file = s:here . '/sample.md'
  execute 'edit ' . fnameescape(l:file)
  setlocal filetype=markdown
  call setline(1, ['# Title', '', 'line three "quoted" é', '', 'last'])
  let l:buf = bufnr('%')

  " toggle on -> open (+ cursor)
  CalligrusToggle
  call s:check('process started', s:wait_for({-> s:count("open") == 1}))
  let l:open = get(s:msgs(), 0, {})
  call s:check('args include --editor', get(s:lines(), 0, '') ==# 'ARGS --editor')
  call s:check('open doc/path/text', get(l:open, 'doc', '') ==# string(l:buf)
        \ && get(l:open, 'path', '') =~# 'sample\.md$'
        \ && get(l:open, 'text', '') ==# "# Title\n\nline three \"quoted\" é\n\nlast")
  call s:check('open format', get(l:open, 'format', '') ==# 'markdown')
  call s:check('cursor sent on toggle', s:wait_for({-> s:count("cursor") >= 1}))

  " cursor move -> cursor with the new line
  call cursor(3, 1)
  doautocmd CursorMoved
  call s:check('cursor line 3', s:wait_for({-> s:last_is_cursor(3)}))

  " edits: three quick changes -> one coalesced text message with the final text
  let l:before = s:count('text')
  for l:i in range(1, 3)
    call setline(5, 'last ' . l:i)
    doautocmd TextChanged
  endfor
  call s:check('text sent', s:wait_for({-> s:count("text") > l:before}))
  sleep 300m
  call s:check('text debounced to one message', s:count('text') == l:before + 1)
  let l:texts = filter(s:msgs(), 'v:val.type ==# "text"')
  call s:check('text is the latest', l:texts[-1].text[-6:] ==# 'last 3')

  " BufEnter of the previewed buffer sends open again
  enew
  let l:opens = s:count('open')
  execute 'buffer ' . l:buf
  call s:check('BufEnter -> open', s:wait_for({-> s:count("open") == l:opens + 1}))

  " follow: another Markdown buffer is previewed without :CalligrusToggle
  enew
  let l:other = bufnr('%')
  call setline(1, 'other')
  setlocal filetype=markdown
  call s:check('follow: new markdown buffer opens', s:wait_for({-> s:count("open") == l:opens + 2}))
  " ... unless toggled off: then switching back to it leaves it alone
  CalligrusToggle
  execute 'buffer ' . l:buf
  execute 'buffer ' . l:other
  sleep 100m
  call s:check('follow: a muted buffer stays off', s:count('open') == l:opens + 3)
  let l:opens += 2

  " non-markdown buffer is refused without !
  enew
  setlocal filetype=text
  CalligrusToggle
  sleep 100m
  call s:check('non-markdown refused', s:count('open') == l:opens + 1)

  " org buffers are sent as org
  enew
  setlocal filetype=org
  call s:check('org format', s:wait_for({-> s:last_open_format() ==# "org"}))

  " search highlight sync
  execute 'buffer ' . l:buf
  set hlsearch
  let @/ = 'three'
  let v:hlsearch = 1
  doautocmd CursorMoved
  call s:check('search: plain text sent', s:wait_for({-> s:last_search_text("three")}))
  call s:check('search text cases', calligrus#search_text('foo.*bar') ==# ['', 0]
        \ && calligrus#search_text('\<word\>')[0] ==# 'word'
        \ && calligrus#search_text('\cWord')[1] == 0)

  " goto (Ctrl+click in the preview): lines the app writes to stdout move the cursor
  call setline(1, ['# Title', '', '    indented three', '', 'last'])
  enew
  setlocal filetype=text
  call s:emit(s:goto_msg(l:buf, 3))
  call s:check('goto: shows the buffer in the current window', s:wait_for({-> bufnr("%") == l:buf}))
  call s:check('goto: line and first non-blank', s:wait_for({-> line(".") == 3 && col(".") == 5}))
  doautocmd CursorMoved
  call s:check('goto: cursor message follows', s:wait_for({-> s:last_is_cursor(3)}))
  sleep 200m
  call s:check('goto: no message loop', s:last().type ==# 'cursor')

  " folds at the target are opened
  setlocal foldmethod=manual
  1,2fold
  normal! gg
  call s:check('fold closed before', foldclosed(2) != -1)
  call s:emit(s:goto_msg(l:buf, 2))
  call s:check('goto: opens folds', s:wait_for({-> line(".") == 2 && foldclosed(2) == -1}))

  " a window already showing the buffer is used
  split
  enew
  let l:wins = winnr('$')
  let l:other_win = win_getid()
  call s:emit(s:goto_msg(l:buf, 5))
  call s:check('goto: uses the window that shows the buffer',
        \ s:wait_for({-> bufnr("%") == l:buf && win_getid() != l:other_win}))
  call s:check('goto: no extra window', winnr('$') == l:wins)
  call s:check('goto: other window untouched', winbufnr(win_id2win(l:other_win)) != l:buf)
  only

  " past the end clamps; line 0 too
  normal! gg
  call s:emit(s:goto_msg(l:buf, 999))
  call s:check('goto: past the end clamps', s:wait_for({-> line(".") == 5}))
  call s:emit(s:goto_msg(l:buf, 0))
  call s:check('goto: line 0 clamps to 1', s:wait_for({-> line(".") == 1}))

  " a message split over two writes, then ignored input
  normal! G
  call s:emit('{"type":"goto","doc":"' . l:buf . '","li')
  sleep 150m
  call s:check('goto: partial line waits', line('.') == 5)
  call s:emit('ne":2}\n')
  call s:check('goto: split message is joined', s:wait_for({-> line(".") == 2}))
  normal! G
  call s:emit('this is not json\n')
  call s:emit('{"type":"goto","doc":"' . l:buf . '","line":\n')
  call s:emit('{"type":"goto","doc":"' . l:buf . '","line":"2"}\n')
  call s:emit('{"type":"mystery","doc":"' . l:buf . '","line":2}\n')
  call s:emit('[1,2]\n')
  call s:emit(s:goto_msg(l:buf + 100, 2))
  call s:emit(s:goto_msg(l:other, 2))
  call s:emit(s:goto_msg('abc', 2))
  sleep 400m
  call s:check('goto: malformed/unknown/wrong doc ignored', bufnr('%') == l:buf && line('.') == 5)
  call s:emit(s:goto_msg(l:buf, 1))
  call s:check('goto: works after junk', s:wait_for({-> line(".") == 1}))

  " two messages in one write
  call s:emit(s:goto_msg(l:buf, 2) . s:goto_msg(l:buf, 4))
  call s:check('goto: two messages in one chunk', s:wait_for({-> line(".") == 4}))

  " goto = 0: ignored
  let g:calligrus_goto = 0
  call s:emit(s:goto_msg(l:buf, 1))
  sleep 400m
  call s:check('goto = 0: ignored', line('.') == 4)
  let g:calligrus_goto = 1
  call s:emit(s:goto_msg(l:buf, 1))
  call s:check('goto = 1 again', s:wait_for({-> line(".") == 1}))

  " wiping the previewed buffer -> close
  let l:closes = s:count('close')
  execute 'bwipeout! ' . l:buf
  call s:check('BufWipeout -> close', s:wait_for({-> s:count("close") == l:closes + 1}))

  " stop -> process sees quit then EOF
  CalligrusStop
  call s:check('quit sent', s:wait_for({-> s:count("quit") == 1}))
  call s:check('process exited', s:wait_for({-> !calligrus#is_running()}))
  call s:check('stdin reached EOF', s:wait_for({-> get(s:lines(), -1, "") ==# "EOF"}))

  " toggling again restarts it
  let l:opens_before = s:count('open')
  enew
  setlocal filetype=markdown
  CalligrusToggle
  call s:check('restart after stop', s:wait_for({-> s:count("open") == l:opens_before + 1}))
  CalligrusStop
  sleep 300m
endfunction

try
  call s:run()
catch
  call s:check('no exception: ' . v:exception . ' @ ' . v:throwpoint, 0)
endtry

call add(s:report, s:failures == 0 ? 'ALL PASSED' : s:failures . ' FAILED')
call writefile(s:report, s:log . '.report')
for s:l in s:report
  silent execute '!echo ' . shellescape(s:l)
endfor
execute s:failures == 0 ? 'qa!' : 'cquit'
