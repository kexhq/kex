" Vim syntax file for Kex ERB-shaped templates (.ket) — kexhq/kex#171.
"
" A .ket file is plain text (conventionally Markdown, HTML, or anything
" else — the true extension sits before this one, as in README.md.ket)
" with `<% %>`-delimited holes of real Kex, scanned by Template.scan and
" lowered to a real function by Template.text/.html. This file highlights
" the tag delimiters and embeds the full Kex grammar (syntax/kex.vim) for
" what is inside them; nothing else here is Kex-specific, so the
" surrounding text is left as plain content rather than guessed at.
"
" Ported from editors/vscode/syntaxes/ket.tmLanguage.json — keep the two in
" sync when the grammar changes.

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

" Embed the Kex grammar under its own cluster — the standard way one syntax
" file borrows another's (:help :syn-include). @KEX ends up holding every
" kex* group kex.vim defines, namespaced so nothing below can collide with
" it. b:current_syntax is unset first so kex.vim's own top-of-file guard
" does not skip loading because THIS file already set it.
unlet! b:current_syntax
syn include @KEX syntax/kex.vim
unlet! b:current_syntax

" ===== Tags =====
" Each pattern excludes the others' openers with a negative lookahead
" rather than relying on definition order, so a tag is never misidentified
" regardless of the order these are declared in:
"
"   <%#  ... %>   comment — never highlighted as code
"   <%== ... %>   raw interpolation (no auto-escaping)
"   <%=  ... %>   interpolation
"   <%   ... %>   control — the only form the leading `-%>`/`<%-` trim
"                 markers apply to
"   <%%           literal `<%`, never a tag at all

syn region ketComment matchgroup=ketTagDelim start=/<%#/ end=/%>/

syn region ketTag matchgroup=ketTagDelim start=/<%==/ end=/-\=%>/ contains=@KEX
syn region ketTag matchgroup=ketTagDelim start=/<%=\%(=\)\@!/ end=/-\=%>/ contains=@KEX
syn region ketTag matchgroup=ketTagDelim start=/<%-\=\%(#\|=\|%\)\@!/ end=/-\=%>/ contains=@KEX

syn match ketLiteral /<%%/

hi def link ketTagDelim Delimiter
hi def link ketComment Comment
hi def link ketLiteral SpecialChar

let b:current_syntax = "ket"

let &cpo = s:cpo_save
unlet s:cpo_save
