" calligrus.vim: live Markdown/Org preview in a separate Calligrus window.
"
" Copyright (C) 2026 KαΩSS (theKAOSSphere)
" SPDX-License-Identifier: GPL-3.0-or-later
"
" This program is free software: you can redistribute it and/or modify it under
" the terms of the GNU General Public License as published by the Free Software
" Foundation, either version 3 of the License, or (at your option) any later
" version. It is distributed WITHOUT ANY WARRANTY; see the GNU General Public
" License for details: <https://www.gnu.org/licenses/>.

" Commands only; the work is in autoload/calligrus.vim (loaded on first use).
if exists('g:loaded_calligrus') || &compatible || v:version < 802
  finish
endif
let g:loaded_calligrus = 1

command! -bang -bar CalligrusToggle call calligrus#toggle(<bang>0)
command! -bar CalligrusStop call calligrus#stop()
command! -bar CalligrusOpenFile call calligrus#open_file()
