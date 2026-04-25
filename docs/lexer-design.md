# Lexer Design

## Overview

The lexer is the first phase of the sea-front pipeline. It takes preprocessed
C++ source text (UTF-8) and produces a linked list of tokens. The lexer is a
pure function: source buffer in, token list out. It has no callbacks, no parser
feedback, and no global state.

```
Preprocessed C++ source (UTF-8)
    → Lexer
    → Singly-linked list of Token structs
    → Parser (next phase)
```

## Structural Model: chibicc

The lexer follows the structural patterns of Rui Ueyama's
[chibicc](https://github.com/rui314/chibicc) compiler:

- **Tokens point into the source buffer.** Each token stores a `char *loc`
  (pointer into the original source) and `int len` (byte length). No string
  copying. This is correct and efficient for preprocessed input that lives in
  a single contiguous buffer.

- **Singly-linked list.** Tokens are chained via `next` pointers, constructed
  using the dummy-head-node pattern: `Token head = {}; cur = &head; ...;
  return head.next;`. The parser consumes tokens by walking the list.

- **Two-phase keyword conversion.** The main lexing loop classifies all
  word-like tokens as identifiers. A second pass walks the list and converts
  keywords (via lookup table) to their specific token kinds. This keeps the
  main loop simple and puts keyword recognition in one place.

- **Single shared header.** All types, enums, and function prototypes live in
  `src/sea-front.h`. Every `.c` file includes it. This is the simplest module
  system for a small C project.

## Token Representation

### TokenKind enum

Every keyword, operator, and literal class gets its own enum value:

- **Keywords** use a `TK_KW_` prefix (`TK_KW_IF`, `TK_KW_CLASS`, etc.) to
  avoid collision with literal token kinds (e.g., `TK_CHAR` is a character
  literal, `TK_KW_CHAR` is the `char` keyword).
- **Operators** each have a named value (`TK_PLUS`, `TK_SHL_ASSIGN`, etc.).
- **Literals** use `TK_NUM` (integer), `TK_FNUM` (floating-point), `TK_STR`
  (string), and `TK_CHAR` (character).
- **Alternative representations** (`and`, `or`, `bitand`, etc., Table 6 of
  N4659) map to the same token kind as their symbolic counterpart: `and`
  produces `TK_LAND`, `not` produces `TK_EXCL`, etc.
- **Identifiers with special meaning** (`override`, `final`) remain as
  `TK_IDENT`. The parser recognises them contextually.

Individual enum values mean the parser can do `tok->kind == TK_KW_IF`
everywhere — no string comparisons, no secondary lookups.

### Token struct

```
kind            TokenKind enum value
*next           next token in the linked list
*loc / len      pointer + length into source buffer
line / col      1-based source location
*file           source file context

ival            integer / character literal value
fval            floating-point literal value
*str / strlen   decoded string literal contents
enc             encoding prefix (none, u8, u, U, L)
*ud_suffix      pointer to user-defined literal suffix (or NULL)
at_bol          true if at beginning of line
has_space       true if preceded by whitespace
```

### LexCtx struct

A small context struct threaded through all lexer functions:

```
*file           source file being lexed
*p              current position in source buffer
line / col      current source location
```

This avoids global state while keeping function signatures clean.

## Preprocessing Assumption

The lexer receives **already-preprocessed** input. An external tool (cpp, mcpp,
or similar) handles `#include`, macro expansion, and conditional compilation
before sea-front sees the source. This means:

- No `#include` processing
- No macro expansion
- No `#if` / `#ifdef` evaluation
- `#line` directives may be present (the lexer can optionally track them for
  accurate location reporting)

Comments are handled defensively in the whitespace-skipping logic, even though
a conforming preprocessor should have already stripped them.

## Character Encoding

The lexer assumes **UTF-8 input**. String and character literal contents are
**byte-transparent** — the lexer does not validate or decode them, it only
scans for the closing delimiter.

Rationale:

- **C++17 translation phase 1** (N4659 §5.2) converts the physical source
  encoding to the basic source character set plus universal-character-names.
  This is the preprocessor's responsibility, not ours.

- **All C++ syntax is ASCII.** Keywords, operators, whitespace, and punctuation
  are pure ASCII. UTF-8 is only relevant for identifiers and literal contents.

- **Identifiers may contain non-ASCII characters** per Annex E. The lexer
  decodes UTF-8 in `unicode.c` and classifies codepoints against the Annex E
  tables. In practice, GCC and Clang sources use ASCII-only identifiers, so
  this is a correctness measure rather than a practical necessity for the
  bootstrap target.

- **String/char literal contents are opaque bytes.** The lexer's job is to find
  the closing `"` or `'` (handling escape sequences for token boundary
  detection). It does not interpret the encoding of the content — that is the
  semantic layer's concern. This means the lexer is byte-transparent within
  literals regardless of the actual encoding.

- **No BOM handling.** If a UTF-8 BOM (U+FEFF) is present, the preprocessor
  is expected to have consumed it. The lexer will reject it as an unexpected
  character if encountered.

## Lexing Rules

### Maximal Munch

The lexer greedily consumes the longest sequence of characters that forms a
valid token (N4659 §5.4). For operators, this is implemented by checking
multi-character operators before shorter ones:

1. Check 3-character operators (`<<=`, `>>=`, `<=>`, `->*`, `...`)
2. Check 2-character operators (`->`, `++`, `<<`, `==`, `::`, etc.)
3. Fall through to single-character operators

### The `<::` Digraph Exception

Per §5.4/3, `<::` is **not** lexed as the digraph `<:` (which means `[`)
followed by `:`, **unless** the character after `<::` is `:` or `>`. In the
normal case, `<` is emitted as its own token and `::` becomes `TK_SCOPE`.

This requires 4-character lookahead in the punctuator reader.

### String and Character Literal Prefixes

The lexer must detect encoding prefixes before identifiers in the main loop,
because `u8"hello"` starts with `u` — a valid identifier character. The
detection order:

1. Check for `u8R"`, `u8"`, `u8'` (3-char prefix)
2. Check for `LR"`, `uR"`, `UR"` (2-char prefix)
3. Check for `L"`, `L'`, `u"`, `u'`, `U"`, `U'`, `R"` (1-char prefix)
4. Check for plain `"`, `'` (no prefix)
5. Only then fall through to identifier lexing

### Raw String Literals

Raw string literals (`R"delim(content)delim"`) are scanned by searching forward
for the `)delim"` terminator. The delimiter is extracted from the characters
between `"` and `(`. Content is taken as-is — no escape processing. Newlines
within the raw string are counted to keep line tracking accurate.

### Numeric Literals

The lexer handles:
- **Decimal** (`42`), **octal** (`077`), **hex** (`0xFF`), **binary** (`0b101`)
- **Digit separators** (`1'000'000`): the `'` is consumed as part of the number
  only when immediately followed by a valid digit for the current base. This
  naturally distinguishes `1'000` (digit separator) from `1 'x'` (integer
  followed by character literal) without backtracking.
- **Floating-point** with decimal and hex exponents
- **Integer suffixes** (`u`, `l`, `ll`, `ul`, `ull`, etc.)
- **User-defined literal suffixes**: if the character immediately after the
  number + standard suffix is an identifier-start character, it is consumed as
  a UDL suffix and stored in `tok->ud_suffix`.

Numeric values are computed by copying digits (skipping `'` separators) into
a temporary buffer and calling `strtol` / `strtod`.

### Keyword Conversion

After the main lexing loop completes, a second pass walks the token list and
converts identifiers to keywords using a sorted lookup table with `bsearch`.
The table has ~84 entries (73 keywords from Table 5 + 11 alternative
representations from Table 6). For this size, `bsearch` (~7 comparisons) is
simpler and more auditable than a hash table.

### The `>>` Problem

The lexer always emits `>>` as a single `TK_SHR` token, following maximal
munch. Template angle-bracket splitting is the parser's responsibility: when
the parser is inside a template argument list and encounters `TK_SHR`, it
splits it into two `TK_GT` tokens by mutating the token list.

This keeps the lexer as a pure function with no parser feedback mechanism.
GCC and Clang use the same approach.

## Unicode Support

Identifier characters are classified per C++17 rules (N4659 §5.10). A separate
`unicode.c` file provides:

- UTF-8 decoding (one codepoint at a time)
- `is_ident_start(codepoint)` — letters, underscore, UCN ranges
- `is_ident_continue(codepoint)` — above plus digits, combining marks

The character tables are mechanically derived from the standard.

## Error Handling

Lexer errors (unterminated string, invalid character, malformed number) call
`error_at(loc, fmt, ...)` which prints `file:line:col: error: message` and
exits. The lexer does not attempt error recovery — for a bootstrap tool,
failing fast on bad input is the right choice.

## Testing Strategy

### Unit Tests (C test harness)

`tests/test_lex.c` calls `tokenize()` directly and asserts on token kinds,
literal values, string contents, encoding prefixes, UDL suffixes, and source
locations. Catches value-computation bugs that output diffing would miss.

### Integration Tests (shell-based)

Small `.cpp` files in `tests/lex/` with corresponding `.expected` files. A
shell script runs `sea-front --dump-tokens` on each and diffs the output.
Quick, readable, catches regressions in token sequences.

### Test Categories

- All 73 C++17 keywords + 11 alternative tokens
- All operators and punctuators, including digraphs and the `<::` exception
- Numeric literals: decimal, octal, hex, binary, digit separators, floats,
  hex floats, suffixes
- String literals: all encoding prefixes, escape sequences, raw strings with
  various delimiters
- Character literals: all prefixes, escape sequences
- User-defined literals
- Unicode identifiers
- Edge cases: `x+++++y`, `>>`, `0xe+1`, unterminated strings
- Location tracking accuracy (line and column)
