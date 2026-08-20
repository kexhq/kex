" Vim filetype plugin for Kex.
if exists("b:did_ftplugin")
  finish
endif
let b:did_ftplugin = 1

let s:cpo_save = &cpo
set cpo&vim

let b:undo_ftplugin = "setlocal cms< com< sua< inc< def< fo<"
      \ . "| unlet! b:match_ignorecase b:match_words"

setlocal commentstring=#\ %s
setlocal comments=:#
setlocal suffixesadd=.kex
setlocal include=^\s*using\>
setlocal define=^\s*\%(let\|foul\|module\|type\|record\|trait\|make\)\>
setlocal formatoptions-=t
setlocal formatoptions+=croql

" With matchit loaded, % bounces between block openers, branch heads, and end.
let b:match_ignorecase = 0
let b:match_words = ''
      \ . '\<\%(do\|if\|match\|receive\|loop\|while\|try\|using\|compiled\|module\|make\|record\|main\|public\|private\)\>'
      \ . ':\%(\<else\>\|\<elif\>\|\<rescue\>\|\<after\>\)'
      \ . ':\<end\>'

let &cpo = s:cpo_save
unlet s:cpo_save
