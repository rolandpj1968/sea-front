# Coding Standards

sea-front is written in C11, targeting correctness and auditability over
performance. This is a bootstrap tool — clarity is the priority.

## Style

- **Explicit, verbose C.** Prefer clarity over brevity. Three assignment
  lines are better than a clever one-liner. The code will be read far
  more often than it is written.

- **Don't duplicate code.** Before writing a block that resembles
  something already in the file (or in a sibling parse/codegen file),
  search for it and extract a helper instead. Two copies is a smell;
  three copies is a bug. Common offenders: balanced-paren skipping,
  param-list emit, type-recursion boilerplate. Run
  `scripts/find_repeats.py` periodically to surface accumulated
  duplication — every block it reports is a missing helper.

- **No magic constants.** Annotate non-obvious arguments with inline
  comments: `region_push(p, REGION_BLOCK, /*name=*/NULL)`. Any numeric
  literal other than 0 or 1 in a function call should have a comment or
  a named constant.

- **Comment every unbounded loop** (`for(;;)`, `while(cond)` that isn't
  trivially bounded) with a termination argument: what decreases, what
  causes the break, why it can't spin forever.

- **Comment array/buffer bounds.** When doing lookahead or pointer
  arithmetic, state why the access is within bounds. Reference the
  buffer padding (32 bytes NUL padding in read_file) or the EOF token
  sentinel where relevant.

- **Spec references.** Every parse function, node kind, type kind, and
  disambiguation rule should cite the relevant section of N4659 (C++17),
  with C++20 (N4861) and C++23 (N4950) section numbers noted where they
  differ. Format: `N4659 §6.4.1 [basic.lookup.unqual]`.

## Naming

- **Namespace all public functions** to avoid collisions:
  - Parser operations: `parser_` prefix (`parser_peek`, `parser_advance`)
  - Lexer/unicode: `lex_` prefix (`lex_decode_utf8`, `lex_is_ident_start`)
  - Utility: `sf_` prefix (`sf_read_file`)
  - Arena/Vec: `arena_`, `vec_` prefixes (already namespaced)
  - Lookup: `region_`, `lookup_` prefixes (already namespaced)
  - Node/Type constructors: `new_node`, `new_ptr_type` etc. are specific
    enough with their signatures

- **Use the standard's terminology** for types and functions. `DeclarativeRegion`
  not `Scope`, `EntityKind` not `SymKind`, `lookup_unqualified` not
  `symbol_table_find`. The code should read like a pragmatic implementation
  of the spec.

- **Variables:** short names for local scope (`p`, `ty`, `tok`, `d`),
  descriptive names for wider scope. Avoid redundant prefixes — `ns` not
  `ns_name` (namespace name is redundant).

## Memory

- **Arena allocator for all AST/Type/Declaration allocations.** No
  individual `free()` calls. The arena survives through the entire
  pipeline (parse → sema → codegen) and is freed in one shot.

- **Vec for growable arrays** during construction, backed by the arena.
  Semi-exponential growth (double up to 4K, then +50%).

- **Source buffer padding.** `sf_read_file` allocates 32 bytes of NUL
  padding past EOF to guarantee safe lookahead for the lexer (raw string
  delimiter memcmp, operator lookahead, etc.).

## Token Stream

- **Contiguous array** (`TokenArray`), not a linked list. Index-based
  cursor (`parser_peek`, `parser_advance`).

- **TK_EOF is a real token** — always the last element. Parser loop
  termination depends on this: no operator/keyword/type pattern matches
  EOF, so every dispatch loop breaks.

- **Save/restore is an int** (`ParseState.pos`). No pointer invalidation
  risk. Used for tentative parsing (§9.8 disambiguation).

## Error Handling

- **Fail fast.** The lexer/parser calls `error_tok()` and exits on
  malformed input. No error recovery — this is a bootstrap tool
  processing source that already compiles under GCC/Clang.

- **Tentative mode.** When `p->tentative` is true, functions return
  NULL instead of calling `error_tok()`. Side effects (name registration)
  are suppressed. Used for §9.8 stmt-vs-decl disambiguation.

## Build

- **`CC=cc`, `-std=c11`.** Must compile with any C11 compiler — this
  is in the bootstrap chain.

- **`bootstrap.sh`** lists every source file explicitly. When adding a
  new `.c` file, update both `Makefile` and `bootstrap.sh`.

- **Zero warnings** under `-Wall -Wextra -Wpedantic`. No exceptions.
