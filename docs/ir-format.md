# Kex IR Format (`.kexo`)

## Context

Kex needs a binary intermediate representation to serve as the canonical distribution artifact for packages. This enables:
- Shipping compiled IR instead of source (unlike Crystal's approach)
- Fast load times (mmap, no parsing) vs source distribution
- Type mangling for cross-module linking and validation
- Install-time compilation to target (BEAM or WASM now, LLVM/native later)
- Encapsulation — only public type signatures exposed, internals opaque

The IR is a **high-level typed core calculus** — not a low-level bytecode. It preserves semantic constructs (pattern matching, spawn, receive) that backends lower to their target's idioms. This is necessary because BEAM and WASM are too different for a shared low-level representation.

## Target Architecture

```
                         ┌──→ BEAM backend   → .beam files
                         │
.kex → frontend → .kexo ─┼──→ WASM backend   → .wasm
                         │
                         └──→ LLVM backend   → native binary (future)
                              (links libkex_rt)
```

### Module Affinities

Modules have a build-system-assigned affinity:
- **Common** — data models, pure functions, validation. Compiles to all targets. No side effects, no target-specific APIs.
- **Backend** — processes, supervision, persistence, distribution. Targets BEAM.
- **Frontend** — rendering, DOM interaction, local state. Targets WASM.

Common modules are the shared type contract between frontend and backend. The build system enforces purity constraints (no `Receive`/`Spawn`, no target-specific imports in common modules). The `.kexo` format itself does not reject any IR nodes based on affinity — validation is a build system concern, not a format concern.

## IR Design

### Core IR Nodes (expressions: 17, patterns: 6, top-level: 4)

**Expressions:**
`Lit | Var | Call | TailCall | CallIndirect | TailCallIndirect | Intrinsic | Let | Lambda | Match | Construct | FieldGet | Update | Seq | Receive | Spawn | Return`

**Patterns:**
`PLit | PVar | PWild | PConstruct | PRecord | PTuple`

**Top-level:**
`FunDef | TypeDef | RecordDef | Module`

### Key Desugaring (AST → IR)

- UFCS resolved: `x.foo(y)` → `Call(foo, [x, y])`
- Field access: `x.field` (zero-arg method call on record) → `FieldGet(x, field)`
- Message send: `process.send(msg)` → `Call(send, [process, msg])` (not a special form, just UFCS)
- If/elif/trailing-if → `Match` on booleans
- Operators → `Intrinsic(op, [a, b])`
- Multi-clause functions → single function with top-level `Match`
- LoopExpr → tail-recursive call
- ShorthandLambda → full Lambda
- RangeExpr → `Construct(:range, [start, end])`
- ErrorPropagate → `Match` on Result type
- Named args → positional (resolved by typechecker)
- `compiled` blocks → expanded at compile time before lowering; generated declarations appear as normal IR nodes
- `UsingBlock` → resolved during name resolution; imports are erased, calls become fully qualified
- `MainBlock` → becomes a `FunDef` named `main`

## Binary Format

LLVM bitstream-inspired. VBR (variable-bit-rate) integers throughout. Straightforward encoding — every node fully spelled out. Compression (if needed) happens at the transport/package layer, not the format level.

### File Layout

```
┌─────────────────────────────────────────────────────────┐
│ HEADER (fixed size, 32 bytes)                           │
├─────────────────────────────────────────────────────────┤
│ SECTION DIRECTORY (variable, list of section offsets)    │
├─────────────────────────────────────────────────────────┤
│ STRING TABLE                                            │
├─────────────────────────────────────────────────────────┤
│ TYPE TABLE                                              │
├─────────────────────────────────────────────────────────┤
│ MODULE TABLE                                            │
├─────────────────────────────────────────────────────────┤
│ SYMBOL TABLE                                            │
├─────────────────────────────────────────────────────────┤
│ EXPORT TABLE                                            │
├─────────────────────────────────────────────────────────┤
│ IMPORT TABLE                                            │
├─────────────────────────────────────────────────────────┤
│ FUNCTION BODIES                                         │
├─────────────────────────────────────────────────────────┤
│ ERROR LOCATIONS (compact, not strippable)                │
├─────────────────────────────────────────────────────────┤
│ DEBUG INFO (optional, strippable)                       │
└─────────────────────────────────────────────────────────┘
```

### Header (32 bytes)

```
offset  size  field
0x00    4     magic: 0x4B455830 ("KEX0")
0x04    2     format_version: u16
0x06    2     min_reader_version: u16
0x08    4     flags: u32
                bit 0: has_debug_info
                bit 1: targets_beam
                bit 2: targets_wasm
                bit 3: targets_native
0x0C    4     section_count: u32
0x10    4     package_name: string_ref u32 (index into string table)
0x14    4     source_hash: u32 (for cache invalidation)
0x18    8     reserved
```

### Section Directory

ELF-style named sections. New sections can be added in future format versions — readers skip sections they don't recognize.

Each entry (32 bytes fixed):

```
offset  size  field
0x00    16    name: inline null-terminated ASCII (max 15 chars + null, e.g. ".strings\0...")
0x10    4     offset: u32 (byte offset from file start)
0x14    4     length: u32 (byte length of section)
0x18    4     flags: u32
                bit 0: required (reader must understand this section or reject the file)
                bit 1: strippable (can be removed without invalidating the module)
0x1C    4     reserved
```

Section names are inline (not string_refs) to avoid circular dependency — the directory must be readable before the string table is located.

Well-known section names:

```
.strings   — string table (shared across all modules in the file)
.types     — type table (shared, deduplicated)
.modules   — module directory
.symbols   — symbol table (all modules, indexed by .modules)
.exports   — export table (all modules, indexed by .modules)
.imports   — import table (external dependencies)
.bodies    — function bodies
.errors    — error locations (not strippable)
.debug     — debug info (strippable)
```

Custom/future sections (e.g. `.beam_hints`, `.wasm_mem`, `.license`) are ignored by readers that don't recognize them, unless the `required` flag is set.

### String Table

All strings in the file stored once, referenced by index.

```
[count: VBR]
[str_0_len: VBR] [str_0_bytes: utf8...]
[str_1_len: VBR] [str_1_bytes: utf8...]
...
```

### Type Table

Every type in the module, deduplicated. Referenced by index.

```
[count: VBR]
[type_entry]*
```

Type entry tags (6 bits, 64 slots — 16 used, 48 reserved for future):

```
0x00 INT
0x01 FLOAT
0x02 STRING
0x03 CHAR
0x04 BOOL
0x05 NONE
0x06 ATOM
0x07 TUPLE    [arity: VBR] [elem_type_ref: VBR]*
0x08 LIST     [elem_type_ref: VBR]
0x09 MAP      [key_type_ref: VBR] [val_type_ref: VBR]
0x0A FN       [param_count: VBR] [param_type_ref: VBR]* [return_type_ref: VBR]
0x0B NAMED    [string_ref: VBR] [type_param_count: VBR] [type_arg_ref: VBR]*
0x0C UNION    [left_type_ref: VBR] [right_type_ref: VBR]
0x0D OPTIONAL [inner_type_ref: VBR]
0x0E GENERIC  [string_ref: VBR]
0x0F PROCESS  [msg_type_ref: VBR]
0x10..0x3F    (reserved for future types)
```

### Module Table

A `.kexo` file is a package containing one or more modules. Tables (strings, types, symbols) are shared and deduplicated across all modules. Each module owns a slice of the symbol and export tables.

```
[count: VBR]
[module_entry]*
```

Each entry:

```
[name: string_ref VBR]
[has_parent: 1 bit]
[parent: VBR]?               (index into module table, only present if has_parent = 1)
[symbol_range_start: VBR]    (index into symbol table)
[symbol_range_end: VBR]
[export_range_start: VBR]    (index into export table)
[export_range_end: VBR]
[flags: u8]
  bit 0: is_pure (no side effects, no target-specific APIs, portable to all targets)
  bit 1: is_foul
```

Cross-module references within the same `.kexo` use the symbol table directly (same file, same index space). External references (to other packages) go through the import table.

### Symbol Table

```
[count: VBR]
[symbol_entry]*
```

Each entry:

```
[name: string_ref VBR]
[kind: 6 bits]  (FUNCTION=0, RECORD_CTOR=1, TYPE=2, CONSTANT=3)
[type_ref: VBR]
[arity: VBR]              (0 for TYPE/CONSTANT)
[flags: u8]
  bit 0: is_tail_recursive
  bit 1: is_pure
  bit 2: is_foul (source `foul` keyword)
  bit 3: is_predicate
[has_body: 1 bit]
[body_offset: VBR]?       (only present if has_body = 1; absent for TYPE)
```

### Export Table

Public symbols visible to consumers:

```
[count: VBR]
[symbol_index: VBR]*
```

### Import Table

```
[count: VBR]
[import_entry]*
```

Each entry:

```
[package: string_ref VBR]
[module: string_ref VBR]
[symbol: string_ref VBR]
[expected_type: type_ref VBR]
```

### Function Bodies

Tree-encoded IR nodes:

```
[function_body]*
```

Each function body:

```
[param_count: VBR]
[param_pattern: pattern_node]*
[clause_count: VBR]
[clause]*
```

Each clause:

```
[pattern_count: VBR]
[pattern: pattern_node]*
[has_guard: 1 bit]
[guard: ir_node]?
[body_count: VBR]
[body: ir_node]*
```

IR node encoding:

```
[opcode: 6 bits]  (64 slots — 17 used, 47 reserved)
    0x00 LIT                 [type_tag: 6 bits] [value: type-dependent]
    0x01 VAR                 [name: string_ref VBR]
    0x02 CALL                [is_external: 1 bit] [ref: VBR] [arg_count: VBR] [args: ir_node]*
    0x03 TAIL_CALL           [is_external: 1 bit] [ref: VBR] [arg_count: VBR] [args: ir_node]*
    0x04 CALL_INDIRECT       [callee: ir_node] [arg_count: VBR] [args: ir_node]*
    0x05 TAIL_CALL_INDIRECT  [callee: ir_node] [arg_count: VBR] [args: ir_node]*
    0x06 INTRINSIC           [op: 6 bits] [arg_count: VBR] [args: ir_node]*
    0x07 LET                 [pattern: pattern_node] [type_ref: VBR] [value: ir_node]
    0x08 LAMBDA              [param_count: VBR] [param_names: string_ref VBR]* [capture_count: VBR] [capture_names: string_ref VBR]* [body_count: VBR] [body: ir_node]*
    0x09 MATCH               [subject: ir_node] [clause_count: VBR] [clauses...]*
    0x0A CONSTRUCT           [type_ref: VBR] [field_count: VBR] [field_name: string_ref VBR, value: ir_node]*
    0x0B FIELD_GET           [record: ir_node] [field_name: string_ref VBR]
    0x0C UPDATE              [record: ir_node] [field_count: VBR] [field_name: string_ref VBR, value: ir_node]*  (future: no surface syntax yet)
    0x0D SEQ                 [count: VBR] [exprs: ir_node]*
    0x0E RECEIVE             [clause_count: VBR] [clauses...]* [has_timeout: 1 bit] [timeout: ir_node]?
    0x0F SPAWN               [body: ir_node]
    0x10 RETURN              [value: ir_node]
```

**Reference resolution for CALL/TAIL_CALL:**
- `is_external = 0`: `ref` is an index into the symbol table (same `.kexo` file)
- `is_external = 1`: `ref` is an index into the import table (cross-package call)

**Type annotations:** Every expression node has an implicit type — the reader tracks types from LET bindings and function signatures. VAR nodes reference names already bound with a type in the enclosing LET or function parameter, so no explicit type_ref on VAR is needed. Backends build a type environment as they walk the tree.

**RECEIVE clauses** reuse the same format as MATCH clauses: `[pattern_count: VBR] [patterns] [has_guard: 1 bit] [guard?] [body_count: VBR] [body: ir_node]*`

**SPAWN** takes an `ir_node` as its body — typically a LAMBDA or a VAR referencing a function.

Pattern node encoding (6 bits — 64 slots, 6 used):

```
0x00 P_LIT      [type_tag: 6 bits] [value: type-dependent]
0x01 P_VAR      [name: string_ref VBR]
0x02 P_WILD
0x03 P_CONSTRUCT [sym_ref: VBR] [arg_count: VBR] [args: pattern_node]*
0x04 P_RECORD   [field_count: VBR] [field_name: string_ref VBR, has_pattern: 1 bit, pattern: pattern_node?]*
0x05 P_TUPLE    [arity: VBR] [elements: pattern_node]*
```

### Intrinsic Operations

The `op` field in INTRINSIC nodes (6 bits, 64 slots):

```
; Arithmetic (binary)
0x00 ADD
0x01 SUB
0x02 MUL
0x03 DIV
0x04 MOD

; Arithmetic (unary)
0x05 NEG

; Comparison
0x06 EQ
0x07 NEQ
0x08 LT
0x09 GT
0x0A LTE
0x0B GTE

; Logical
0x0C AND
0x0D OR
0x0E NOT

; String/List
0x0F CONCAT

; Bitwise
0x10 BIT_AND
0x11 BIT_OR
0x12 BIT_XOR
0x13 BIT_NOT
0x14 BIT_SHL
0x15 BIT_SHR

0x16..0x3F (reserved)
```

### Error Locations (`.errors`, not strippable)

Compact source locations only for nodes that can fail at runtime. Keeps useful error messages in production without full debug info overhead.

```
[file_count: VBR]
[file_names: string_ref VBR]*   (source file name table)
[entry_count: VBR]
[error_loc_entry]*
```

Each entry:

```
[kind: 6 bits]
  0x00 MATCH_FAIL      — non-exhaustive match
  0x01 RECEIVE_TIMEOUT — receive timed out
  0x02 ERROR_PROP      — ? operator hit an error value
  0x03 GUARD_FAIL      — guard clause failed
  0x04 ASSERTION       — explicit assert
[file_index: VBR]     — index into file name table above
[body_offset: VBR]    — offset into function body where the node lives
[line: VBR]
[col: VBR]
```

Typically a few dozen entries per module. Produces messages like:
`MatchError: no clause matched in Foo.bar (foo.kex:42:5)`

### Debug Info (`.debug`, optional, strippable)

Full source mapping for every IR node. Only included in dev/debug builds.

```
[file_count: VBR]
[file_names: string_ref VBR]*   (source file name table, shared with .errors if both present)
[line_map_count: VBR]
[line_map_entry]*
  [file_index: VBR] [body_offset: VBR] [line: VBR] [col: VBR]
```

## Concrete Example

Kex source:

```kex
let factorial(0) = 1
let factorial(n) = n * factorial(n - 1)
```

Textual IR dump:

```
let factorial(n: Int) -> Int =
  match var:n
    | p_lit 0 -> lit 1
    | p_var n -> intrinsic mul [var:n, tail_call factorial [intrinsic sub [var:n, lit 1]]]
```

Binary (~35 bytes for the body):

```
01        ; param_count = 1
02        ; clause_count = 2
; Clause 0: literal 0 → 1
01 00 00  ; P_LIT INT 0
0         ; no guard
00 00 01  ; LIT INT 1
; Clause 1: bind → n * factorial(n - 1)
01 05     ; P_VAR str:"n"
0         ; no guard
06 01 02  ; INTRINSIC mul, 2 args
  01 05     ; VAR str:"n"
  03 10 01  ; TAIL_CALL factorial, 1 arg
    06 02 02  ; INTRINSIC sub, 2 args
      01 05     ; VAR str:"n"
      00 00 01  ; LIT INT 1
```

## Package Distribution

- **Publish**: ship `.kexo` files (with type info, mangled names, target-agnostic)
- **Install**: compile IR → target locally (always works, always correct)
- **Optional**: pre-built binaries for common target combinations as a cache optimization

Thin interface files (`.kexi`) = `HEADER + STRINGS + TYPES + MODULES + EXPORTS` for fast dependency resolution without loading function bodies.

## LLVM-Readiness Constraints

These rules ensure a future native backend needs no IR format changes:

- `TailCall` stays separate from `Call` (LLVM needs `musttail`)
- Numeric types stay precise (`Int`/`Float` not collapsed to bigint)
- Closure representation is abstract (captures listed, layout is backend's choice)
- `Spawn`/`Receive` are explicit nodes, not baked into call conventions
- No BEAM-specific concepts in the IR — atoms are just a data type

## Implementation File Structure

```
src/ir/
  ir.hxx         — IR node definitions
  types.hxx      — type representation
  module.hxx     — module container (symbols, imports, exports)
  lower.hxx/cxx  — AST → IR desugaring pass
  writer.hxx/cxx — IR → .kexo binary serialization
  reader.hxx/cxx — .kexo binary → IR deserialization
  dump.hxx/cxx   — textual IR printer (debug tool)
  vbr.hxx        — VBR integer encoding/decoding utilities
```

## Decisions

1. **Typechecker first**: A full type inference/checking pass is required before lowering. The IR is always fully typed.
2. **No abbreviations**: Straightforward node encoding. Compression at the transport/package layer if needed.

## Versioning

Hybrid approach: additive evolution by default, breaking changes only when unavoidable.

### Rules

- **`format_version`** (u16) — increments on any format change (for caching/tooling). Informational.
- **`min_reader_version`** (u16) — the oldest reader that can load this file. Only bumps on truly breaking changes (header layout, encoding fundamentals).

### What doesn't bump `min_reader_version`

- New named sections → old readers skip them (unless `required` flag is set)
- New IR opcodes from unused range → old readers reject that specific module, not the whole file
- New type tags from unused range → same behavior
- New flags in existing flag fields (unused bits are reserved, must be 0)

### What does bump `min_reader_version`

- Header layout changes
- VBR encoding changes
- Fundamental section format changes (e.g., string table encoding)

### Package registry interaction

- Packages pin the `format_version` they were built with
- When a new compiler ships with a higher `min_reader_version`, a rebuild is triggered for affected packages
- Source hash in header enables incremental rebuilds (only actually-changed modules recompile)

### Design principles for stability

- Reserve unused bits in all flag fields (must be written as 0, ignored on read)
- Opcode space is intentionally sparse (6-bit = 64 slots, only 17 used, leaves room for 47 future nodes)
- Type tag space is sparse (6-bit = 64 slots, only 16 used)
- Section names are strings, not enums — infinite extensibility without coordination
