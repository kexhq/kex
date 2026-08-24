" Vim syntax file for the Kex language.
" Maintainer: Kex contributors
" Ported from editors/vscode/syntaxes/kex.tmLanguage.json — keep the two in
" sync when the grammar changes.

if exists("b:current_syntax")
  finish
endif

let s:cpo_save = &cpo
set cpo&vim

syn case match

" ===== Comments =====
syn keyword kexTodo TODO FIXME XXX HACK contained
syn match kexDocTag /@\%(example\|param\|return\)\>/ contained
syn match kexComment /#.*/ contains=kexTodo,kexDocTag

" ===== Interpolation shared by strings and regex literals =====
syn region kexInterp contained matchgroup=kexInterpDelim start=/\${/ end=/}/ contains=TOP

" ===== Regex literals: re`…`, regex`…`, Regex.re`…`, regex("…") =====
syn match kexRegexEscape /\\./ contained
syn region kexRegexCharClass start=/\[/ end=/\]/ contained contains=kexRegexEscape
syn match kexRegexQuant /\%([{][0-9]\+\%(,[0-9]*\)\=[}]\)\|[*+?]/ contained
syn match kexRegexAnchor /[$^]/ contained
syn match kexRegexOr /|/ contained
syn match kexRegexGroup /[()]/ contained
syn match kexRegexDot /\./ contained

syn region kexRegexInterp matchgroup=kexRegexTag
      \ start=/\%(\<Regex\.\)\=\<re\%(gex\)\=\$`/ end=/`/
      \ contains=kexInterp,kexRegexEscape,kexRegexCharClass,kexRegexQuant,kexRegexAnchor,kexRegexOr,kexRegexGroup,kexRegexDot
syn region kexRegex matchgroup=kexRegexTag
      \ start=/\%(\<Regex\.\)\=\<re\%(gex\)\=`/ end=/`/
      \ contains=kexRegexEscape,kexRegexCharClass,kexRegexQuant,kexRegexAnchor,kexRegexOr,kexRegexGroup,kexRegexDot
syn region kexRegexCall matchgroup=kexRegexTag
      \ start=/\<regex("/ end=/"/
      \ contains=kexRegexEscape,kexRegexCharClass,kexRegexQuant,kexRegexAnchor,kexRegexOr,kexRegexGroup,kexRegexDot

" ===== Strings =====
" `` ` `` escapes to a literal backtick; $${ escapes ${ in interpolated
" form. They are `skip=` patterns so they cannot end the region.
syn match kexRawEscape /``\|\$\$[{]/ contained
syn match kexEscape /\\\%(u[{][0-9a-fA-F]\+[}]\|[nrt0"'$\\]\)/ contained
syn region kexInterpRaw start=/\$`/ skip=/``\|\$\$[{]/ end=/`/ contains=kexInterp,kexRawEscape
syn region kexRaw start=/`/ skip=/``/ end=/`/ contains=kexRawEscape
syn region kexString start=/"/ skip=/\\./ end=/"/ contains=kexEscape,kexInterp
syn region kexChar start=/'/ skip=/\\./ end=/'/ contains=kexEscape

" ===== Block parameters: { |x, y| … } and do |x| … end =====
syn match kexBlockParam /[a-z_][A-Za-z0-9_?!]*/ contained
syn match kexBlockParamSep /,/ contained
syn region kexBlockParams matchgroup=kexPipe start=/\%({\)\@<=\s*|/ end=/|/ oneline contains=kexBlockParam,kexBlockParamSep
syn region kexBlockParams matchgroup=kexPipe start=/\%(\<do\>\)\@<=\s*|/ end=/|/ oneline contains=kexBlockParam,kexBlockParamSep

" ===== Declarations =====
" `name :> Type` — a method declaration inside make/record blocks.
syn match kexMethodDecl /^\s*\zs[a-z_][A-Za-z0-9_?!]*\ze\s*:>/
" `let name(` / `foul name(` — a function definition. A lookbehind rather
" than \zs: a match that starts at the `let` keyword loses to the keyword
" item, and the lookbehind starts the match at the name itself.
syn match kexFuncDecl /\%(^\s*\<\%(let\|foul\)\s\+\)\@<=[a-z_][A-Za-z0-9_?!]*\ze\s*(/
" A record field declaration starting a line (`size : Integer`); not `::`,
" `:>`, or `:=`.
syn match kexMemberDecl /^\s*\zs[a-z_][A-Za-z0-9_?!]*\ze\s*:\%([:>=]\)\@!/
" Keys in a { … } literal (or one after a comma): `name:` before a value.
syn match kexHashKey /\%({\)\@<=\s*\zs[a-z_][A-Za-z0-9_?!]*\ze\s*:\%([:>=]\)\@!/
syn match kexHashKey /\%(,\)\@<=\s*\zs[a-z_][A-Za-z0-9_?!]*\ze\s*:\%([:>=]\)\@!/
" `make Foo, implement: Showable do` — the label is part of the declaration.
syn match kexImplement /\<implement\>\%(\s*:\)\@=/

" ===== Keywords and types =====
syn keyword kexKeyword after break compiled do elif else end export final foul if let loop main match next private public receive rescue return spawn then timeout try trying using var when while with
syn keyword kexConstant true false None
" `new` is an ordinary, shadowable binding, but make methods supply it as a
" language-level receiver copy. Highlight it alongside `this`, not as Keyword.
syn keyword kexThis this new
syn keyword kexModifier distinct
syn keyword kexStorage module record type trait make capability
syn keyword kexBuiltinType Any Atom Bool Byte Char Float Float32 Float64 Int Integer Int8 Int16 Int32 Int64 List Map Optional Process Result Stream String UInt8 UInt16 UInt32 UInt64 Void
syn match kexType /\<[A-Z][A-Za-z0-9_]*\>/
" Contextual make-block constructors; neither spelling is globally reserved.
" Keep this after kexType so the more specific match wins.
syn match kexUpdateConstructor /\<\%(New\|This\)\>\ze\s*{/

" `-> T or! E` / `-> T or E` is type syntax, not the .or(…) method.
syn match kexOr /\%([.A-Za-z0-9_]\)\@<!or!\=\%([A-Za-z0-9_(]\)\@!/

" ===== Literals and operators =====
syn match kexAtom /:[A-Za-z_][A-Za-z0-9_?!]*/
syn match kexInstanceVar /@[a-z_][A-Za-z0-9_?!]*/
syn match kexNumber /\%([A-Za-z0-9_]\)\@<!\%(0[xX][0-9a-fA-F_]\+\|0[bB][01_]\+\|0[oO][0-7_]\+\|[0-9][0-9_]*\%(\.[0-9][0-9_]*\)\=\%([eE][+-]\=[0-9]\+\)\=\)\%([A-Za-z0-9_]\)\@!/
syn match kexOperator /->\|=>\|:>\|\.\.\.\?\|==\|!=\|<=\|>=\|&&\|||\|[+\-*/%=<>!?&|~^]/
" Keep this after kexOperator so the member wins over the preceding dot.
syn match kexFieldAssign /\.\zs[a-z_][A-Za-z0-9_?!]*\ze\s*=/

syn sync minlines=200

hi def link kexComment Comment
hi def link kexTodo Todo
hi def link kexDocTag SpecialComment
hi def link kexKeyword Statement
hi def link kexOr Statement
hi def link kexConstant Boolean
hi def link kexThis Special
hi def link kexModifier StorageClass
hi def link kexStorage StorageClass
hi def link kexImplement Label
hi def link kexFuncDecl Function
hi def link kexMethodDecl Function
hi def link kexMemberDecl Identifier
hi def link kexHashKey Constant
hi def link kexAtom Constant
hi def link kexInstanceVar Identifier
hi def link kexNumber Number
hi def link kexOperator Operator
hi def link kexString String
hi def link kexChar Character
hi def link kexRaw String
hi def link kexInterpRaw String
hi def link kexEscape SpecialChar
hi def link kexRawEscape SpecialChar
hi def link kexInterpDelim Special
hi def link kexRegex String
hi def link kexRegexInterp String
hi def link kexRegexCall String
hi def link kexRegexTag Function
hi def link kexRegexEscape SpecialChar
hi def link kexRegexCharClass SpecialChar
hi def link kexRegexQuant Special
hi def link kexRegexAnchor Special
hi def link kexRegexOr Operator
hi def link kexRegexGroup Special
hi def link kexRegexDot Special
hi def link kexBlockParam Identifier
hi def link kexBlockParamSep Special
hi def link kexPipe Special
hi def link kexBuiltinType Type
hi def link kexUpdateConstructor Function
hi def link kexType Type
hi def link kexFieldAssign Identifier

let b:current_syntax = "kex"

let &cpo = s:cpo_save
unlet s:cpo_save
