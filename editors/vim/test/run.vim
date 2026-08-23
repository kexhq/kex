" Tests for the Kex Vim runtime files. Run from anywhere:
"
"   nvim --headless -u editors/vim/test/run.vim
"   vim -es -u NONE -N -c 'source editors/vim/test/run.vim'
"
" Exits nonzero when something fails, so it can be wired into CI.

set nocompatible
let s:here = expand('<sfile>:p:h')
execute 'set runtimepath^=' . fnameescape(fnamemodify(s:here, ':h'))
filetype plugin indent on
syntax enable

let s:failures = []

function! s:Fail(msg) abort
  call add(s:failures, a:msg)
endfunction

" --- Indent: reindenting the fixture must not move a single line. ---------
execute 'edit ' . fnameescape(s:here . '/indent.kex')
setlocal shiftwidth=2 expandtab
let s:before = getline(1, '$')
silent normal! gg=G
let s:after = getline(1, '$')
for s:i in range(len(s:before))
  if s:before[s:i] !=# s:after[s:i] && s:before[s:i] =~# '\S'
    call s:Fail(printf("indent.kex:%d\n    got  [%s]\n    want [%s]",
          \ s:i + 1, s:after[s:i], s:before[s:i]))
  endif
endfor

" --- Filetype and ftplugin settings ---------------------------------------
if &filetype !=# 'kex'
  call s:Fail('filetype is ' . &filetype . ', expected kex')
endif
if &commentstring !=# '# %s'
  call s:Fail('commentstring is ' . &commentstring)
endif
if !exists('b:match_words')
  call s:Fail('b:match_words is not set — matchit will not know do/end')
endif

" --- Syntax: the group each construct lands in. ---------------------------
" [pattern to find, group expected at the start of the match]
let s:cases = [
      \ ['# Fixture', 'kexComment'],
      \ ['@param', 'kexDocTag'],
      \ ['using', 'kexKeyword'],
      \ ['record\>', 'kexStorage'],
      \ ['distinct', 'kexModifier'],
      \ ['Integer', 'kexBuiltinType'],
      \ ['Showable', 'kexType'],
      \ ['implement', 'kexImplement'],
      \ ['show :>', 'kexMethodDecl'],
      \ ['x : Integer', 'kexMemberDecl'],
      \ ['log(msg', 'kexFuncDecl'],
      \ ['or! Error', 'kexOr'],
      \ ['"point ', 'kexString'],
      \ ['${this', 'kexInterpDelim'],
      \ ['re`', 'kexRegexTag'],
      \ ['\\\.(foo', 'kexRegexEscape'],
      \ ['0xFF_1', 'kexNumber'],
      \ ['name: "kex"', 'kexHashKey'],
      \ ['New {', 'kexUpdateConstructor'],
      \ ['This {', 'kexUpdateConstructor'],
      \ ['new.x', 'kexThis'],
      \ ['\.\zsx =', 'kexFieldAssign'],
      \ ['\.\.\.other', 'kexOperator'],
      \ ["'v'", 'kexChar'],
      \ ['capability Clock', 'kexStorage'],
      \ ['with Clock', 'kexKeyword'],
      \ ]
for s:case in s:cases
  keepjumps normal! gg
  let s:pos = searchpos(s:case[0], 'cW')
  if s:pos[0] == 0
    call s:Fail('syntax: pattern not found in fixture: ' . s:case[0])
    continue
  endif
  let s:group = synIDattr(synID(s:pos[0], s:pos[1], 1), 'name')
  if s:group !=# s:case[1]
    call s:Fail(printf('syntax: %s is %s, expected %s (line %d)',
          \ s:case[0], empty(s:group) ? 'unhighlighted' : s:group,
          \ s:case[1], s:pos[0]))
  endif
endfor

" --- Report ---------------------------------------------------------------
if empty(s:failures)
  echo 'kex vim tests: ok'
  qall!
endif
for s:failure in s:failures
  echo 'FAIL ' . s:failure
endfor
echo printf('kex vim tests: %d failure(s)', len(s:failures))
cquit
