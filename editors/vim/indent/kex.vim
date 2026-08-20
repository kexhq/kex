" Vim indent file for Kex.
if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

let s:cpo_save = &cpo
set cpo&vim

setlocal autoindent
setlocal indentexpr=GetKexIndent()
setlocal indentkeys==end,=else,=elif,=rescue,=after,0.,),],}

let b:undo_indent = "setlocal ai< inde< indk<"

let &cpo = s:cpo_save
unlet s:cpo_save

if exists("*GetKexIndent")
  finish
endif

" Is the START of this line inside a string, char, or regex literal? Testing
" the start rather than the end of the line matters: a closed string still
" highlights its final quote as a string, and testing there treated every
" line after one as a continuation.
function s:InLiteral(lnum) abort
  return synIDattr(synID(a:lnum, 1, 1), "name")
        \ =~# 'kex\%(String\|Char\|Raw\|Regex\|Interp\)'
endfunction

" The line as code: comments dropped, literal text blanked out to `_`. A
" comment ending in the word `do`, or a string holding an unbalanced bracket,
" must not read as a block opener. Literals are blanked rather than removed so
" that what surrounds them keeps its shape — deleting them glued
" `printLine("x" if c)` into `printLine( if c)`, which then read as an `if`
" opening a block. A literal's own delimiters are blanked with it, so
" brackets inside one never reach the bracket count. The one exception is the
" tag a literal can carry: `regex("…")` highlights `regex("` as one piece,
" and blanking that `(` would leave its closing paren looking unmatched.
function s:CodeLine(lnum) abort
  let l:line = getline(a:lnum)
  let l:code = ''
  let l:i = 1
  while l:i <= strlen(l:line)
    let l:syn = synIDattr(synID(a:lnum, l:i, 1), "name")
    if l:syn =~# '^kex\%(Comment\|Todo\|DocTag\)$'
      break
    elseif l:syn =~# '^kex\%(String\|Char\|Raw\|Regex\|Interp\)'
          \ && l:syn !~# 'Tag$'
      let l:code .= '_'
    else
      let l:code .= l:line[l:i - 1]
    endif
    let l:i += 1
  endwhile
  return l:code
endfunction

" Brackets inside a comment or a literal are text, not structure.
let s:skip = 'synIDattr(synID(line("."), col("."), 1), "name")'
      \ . ' =~# ''^kex\%(String\|Char\|Raw\|Regex\|Interp\|Comment\)'''

" The line holding the innermost bracket still open at the start of `lnum`,
" or 0 when that position is not inside a bracketed expression. The search
" stops after s:lookback lines: a bracketed expression that spans more than
" that is not worth making every keystroke in a long file pay for.
let s:lookback = 60

function s:UnclosedOpen(lnum) abort
  let l:save = getcurpos()
  call cursor(a:lnum, 1)
  let l:stop = max([1, a:lnum - s:lookback])
  let l:line = 0
  for l:pair in [['(', ')'], ['\[', '\]'], ['{', '}']]
    let l:pos = searchpairpos(l:pair[0], '', l:pair[1], 'bnW', s:skip, l:stop)
    if l:pos[0] > l:line
      let l:line = l:pos[0]
    endif
  endfor
  call setpos('.', l:save)
  return l:line
endfunction

" What opens a block, for pairing an `end` with its head:
"   - `do`, with or without block parameters, at the end of a line;
"   - a `do`-less `if` or `while` header at the start of a line — but not one
"     ending in `do` (`if @order do |o|` is already counted above), and not the
"     modifier form (`break if i > 3`), which is never line-initial. The match
"     starts at the keyword rather than at the indent, so that a line-initial
"     inline `if` counts once rather than twice;
"   - an inline `if … then … end`, whose `if` sits mid-line but does open the
"     `end` that closes it.
let s:blockOpen = '\%(\<do\>\s*\%(|[^|]*|\)\?\s*$'
      \ . '\|\%(^\s*\)\@<=\%(if\|while\)\>\%(.*\<do\>\s*\%(|[^|]*|\)\?\s*$\)\@!'
      \ . '\|\<if\>\%(.*\<then\>\)\@='
      \ . '\)'

" The heads of the branches inside a block. `after` counts only in its
" receive form: the testing DSL's `after { … }` is an ordinary call.
let s:branchHead = '\%(\<\%(else\|elif\|rescue\)\>\|\<after\s\+timeout\s*:\)'

" The line opening the block that the keyword matching `pattern` on `lnum`
" closes or continues, or 0 when no opener is found within s:endLookback
" lines.
let s:endLookback = 400

function s:MatchingOpen(lnum, pattern) abort
  let l:col = match(getline(a:lnum), a:pattern) + 1
  if l:col <= 0
    return 0
  endif
  let l:save = getcurpos()
  call cursor(a:lnum, l:col)
  let l:pos = searchpairpos(s:blockOpen, '', '\<end\>', 'bnW', s:skip,
        \ max([1, a:lnum - s:endLookback]))
  call setpos('.', l:save)
  return l:pos[0]
endfunction

" A line that continues the expression above it: a UFCS chain step
" (`.map { … }`), or a type spread over lines — `= Number(Int)` under its
" `type` head, and the `| Fizz` variants after it.
function s:IsContinuation(line) abort
  return a:line =~# '^\s*\%(\.\s*[A-Za-z_]\|[|]\|=[^=>]\)'
endfunction

" Where the statement holding `lnum` starts. If this line was inside
" brackets — the tail of a wrapped parameter list, say — the construct starts
" where the bracket opened, and that is the line levels are measured from.
" Nothing is open at the line being indented, so this one can only have been
" inside a bracket if it closed it: no closer, no search.
function s:BracketHead(lnum, code) abort
  let l:head = a:code =~# '[)}\]]' ? s:UnclosedOpen(a:lnum) : 0
  return l:head > 0 ? l:head : a:lnum
endfunction

" Does the line above `lnum` end in `=`, making `lnum` the body of a
" declaration split over two lines? `type Shape =` / `  Circle(Number)` is
" the shape of it, and the variants that follow line up with the first.
function s:ContinuesAbove(lnum) abort
  let l:above = prevnonblank(a:lnum - 1)
  return l:above > 0 && s:CodeLine(l:above) =~# '[^=<>!]=\s*$'
endfunction

function GetKexIndent() abort
  " Inside a multi-line literal: leave the line exactly as the author wrote
  " it — its indentation is string content, not code.
  if s:InLiteral(v:lnum)
    return -1
  endif

  let l:cur = getline(v:lnum)

  " Inside a bracketed expression opened on an earlier line: one level in
  " from the line that opened it, and its closer lines back up with that
  " line. Once one continuation line exists the rest keep its column, so
  " alignment under an opening paren survives a reindent.
  let l:open = s:UnclosedOpen(v:lnum)
  if l:open > 0
    if l:cur =~# '^\s*[)}\]]'
      return indent(l:open)
    endif
    let l:above = prevnonblank(v:lnum - 1)
    return l:above > l:open ? indent(l:above) : indent(l:open) + shiftwidth()
  endif

  " An `end`, or a branch head like `else`, lines up with the line that
  " opened the block — wherever that is. Nothing nearer says where that is
  " for a block hung off a chain step, or for a branch whose predecessor is a
  " one-line `if … then …`.
  let l:closerPattern = '^\s*\zs\%(\<end\>\|' . s:branchHead . '\)'
  if l:cur =~# l:closerPattern
    let l:opener = s:MatchingOpen(v:lnum, l:closerPattern)
    if l:opener > 0
      return indent(s:BracketHead(l:opener, s:CodeLine(l:opener)))
    endif
  endif

  let l:pline = prevnonblank(v:lnum - 1)
  " Skip back over the body of a multi-line literal. Its closing line is
  " indented by the string's content, so the code after it must take its
  " bearings from the line that opened the string instead.
  while l:pline > 0 && s:InLiteral(l:pline)
    let l:pline = prevnonblank(l:pline - 1)
  endwhile
  if l:pline == 0
    return 0
  endif

  let l:prev = s:CodeLine(l:pline)

  " A continuation indents one level under the line it continues; the ones
  " after it line up with the first.
  if s:IsContinuation(l:cur)
    " Line up with the step above when there is one. A closing line counts as
    " one: the `end` of `.command(…) do` and the `])` of `.rule(…, [` sit at
    " the column their own step started on, which is where the next step
    " belongs too.
    return s:IsContinuation(l:prev) || s:ContinuesAbove(l:pline)
          \ || l:prev =~# '^\s*\%(end\>\|[)}\]]\)'
          \ ? indent(l:pline) : indent(l:pline) + shiftwidth()
  endif

  " A line ending in `do` (with or without block parameters) or in `=` opens
  " the next one, and so does a bare branch head. `else` and `rescue` must be
  " the whole line to count: `c then a else b` is a one-line conditional
  " expression, not the head of a block.
  let l:opens = l:prev =~# '\%(\<do\>\s*\%(|[^|]*|\)\?\|[^=<>!]=\)\s*$'
        \ || l:prev =~# '^\s*\%(else\|rescue\)\s*$'
  " So does a header whose body starts on the next line with no `do` to say
  " so: `while cond`, and `if cond` / `elif cond` written without `then`.
  " A one-line `if … then … end` closes on the spot and opens nothing.
  " `let x = if cond` counts too; `return x if cond` — the modifier form —
  " does not, hence the anchors before the keyword.
  if !l:opens && l:prev =~# '\%(^\s*\|[=(,]\s*\)\<\%(if\|elif\)\>'
        \ && l:prev !~# '\<\%(then\|end\)\>'
    let l:opens = 1
  endif
  if !l:opens && l:prev =~# '^\s*while\>' && l:prev !~# '\<end\>\s*$'
    let l:opens = 1
  endif
  " `after timeout: …` heads the receive branch that follows it. Only that
  " form: `after : Block<Void> -> Void` is a method declaration.
  if !l:opens && l:prev =~# '^\s*after\s\+timeout\s*:'
    let l:opens = 1
  endif

  if l:opens
    " The block belongs to the line that opened it, even when that line is a
    " chain step: `.filter do |n|` holds its body one level in from itself.
    let l:ind = indent(s:BracketHead(l:pline, l:prev)) + shiftwidth()
  else
    " Otherwise, come back out of any continuation: the level is set by the
    " line the chain hangs off, not by its last step.
    let l:base = l:pline
    let l:baseCode = l:prev
    while l:base > 1
      if l:baseCode =~# '^\s*\<end\>'
        " Step over a whole block to its head. Where that block hangs off a
        " chain step, the `end` sits at the step's column, not at the level
        " the statement after it belongs to.
        let l:opener = s:MatchingOpen(l:base, '^\s*\zs\<end\>')
        if l:opener <= 0 || l:opener >= l:base
          break
        endif
        let l:base = l:opener
      elseif s:IsContinuation(l:baseCode) || s:ContinuesAbove(l:base)
        let l:base = prevnonblank(l:base - 1)
      else
        break
      endif
      let l:baseCode = s:CodeLine(l:base)
    endwhile
    let l:ind = indent(s:BracketHead(l:base, l:baseCode))
  endif

  " A line starting with a closer dedents against its opener — but only from
  " a body line. A branch head that already carries its body (`rescue return
  " -1`) sits at the opener's level, and the `end` after it belongs there too.
  let l:prevIsHead = l:prev =~# '^\s*' . s:branchHead
  if !(l:prevIsHead && !l:opens)
        \ && l:cur =~# '^\s*\%(\<end\>\|' . s:branchHead . '\|[)}\]]\)'
    let l:ind -= shiftwidth()
  endif

  return l:ind < 0 ? 0 : l:ind
endfunction

