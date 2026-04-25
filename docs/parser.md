# Parser Design

## Overview

The parser is a hand-written recursive descent parser over the lexer's
token list. It produces the AST described in ast.md. Most of the
interesting design lives in three places:

1. The recursion structure (functions per grammar rule)
2. How parsing-time ambiguity is resolved (C++'s "is this a declaration
   or an expression?" problem)
3. The interaction with name lookup (the parser needs lookup to *parse*
   correctly — see "the two semantic oracles" below)

This document is grounded in `src/parse/`. For the per-node-kind data
shapes the parser produces, see ast.md.

## Why Recursive Descent

C++'s grammar is not LALR(k) for any fixed k. Some constructs require
unbounded lookahead, semantic information about prior declarations
(typedef vs identifier), or backtracking. Hand-written recursive descent
is the only proven way to parse the full language correctly — every
production C++ compiler (gcc, clang, msvc, edg) does it this way.

The structural model is `chibicc`: each grammar rule is a function, the
parser carries a `Parser *p` with a token cursor and an arena, and
backtracking is achieved by saving and restoring the cursor.

## File Layout

| File | Responsibility |
|------|---------------|
| `parse.h` | All AST and Type definitions (see ast.md), Parser state |
| `parser.c` | Top-level entry, translation-unit driver |
| `expr.c` | Expression-grammar productions (operator precedence + postfix) |
| `stmt.c` | Statement-grammar productions |
| `decl.c` | Declaration-specifier-seq, declarators, function bodies |
| `type.c` | Type specifiers, type-id parsing |
| `lookup.c` | Declarative regions, name lookup primitives |
| `ast_dump.c` | `--dump-ast` debug rendering |

## What "Done Parsing" Means

After `parse_translation_unit(p)` returns:

- A complete AST is built. Every grammar production that succeeded has
  produced a node.
- Every declarative region (namespace, class, function-local block,
  template) is populated and linked via `enclosing` pointers.
- Every name reference is captured as a token; resolution is sema's job
  EXCEPT for the resolutions the parser needed to make to disambiguate
  the grammar (see below).
- `is_type_dependent` is set on every dependent expression — see
  template-instantiation.md.
- Type system: every declarator is fully unwound into a `Type *`. The
  base type came from `parse_type_specifiers`; the declarator suffixes
  (pointer, array, function) wrap it inside-out per N4659 §11.3
  [dcl.meaning].

The parser does NOT:

- Run overload resolution
- Check typing rules (e.g. argument-to-parameter compatibility)
- Resolve qualified-id `A::B::C` chains beyond what's needed for
  disambiguation
- Substitute template parameters (templates are parsed once, generically)

## The Two Semantic Oracles

C++ requires the parser to know, at parse time, whether an identifier
names a *type* or a *non-type*, and whether a name is a *template name*
followed by `<` (template-id) or just an identifier followed by less-than.
Without these answers the grammar is genuinely ambiguous.

sea-front exposes two narrow lookup oracles for this, both in `lookup.c`:

```c
bool lookup_is_type_name(Parser *p, Token *t);
bool lookup_is_template_name(Parser *p, Token *t);
```

These are convenience wrappers around unqualified name lookup that
inspect the `EntityKind` of the result (`ENTITY_TYPE`, `ENTITY_TAG`,
`ENTITY_TEMPLATE`). Every other parsing decision should NOT need lookup
— if it does, that's a sign the grammar production is wrong.

The complete enumeration of disambiguation cases that depend on these
oracles is in [`disambiguation-rules.md`](disambiguation-rules.md).

## Disambiguation Strategy

C++ has three families of "parse this two ways" problems:

### 1. Most-vexing-parse family — declaration vs. expression

`T x(y);` — function declaration if `y` is a type, otherwise direct-init.
`T x();` — function declaration if T is a type, otherwise an expression
(less common). The standard rule (N4659 §9.8 [stmt.ambig] / §11.2/1
[dcl.ambig.res]) is: tentatively parse as a parameter-declaration-clause;
if it succeeds, that's the interpretation.

sea-front uses a **gated tentative parse** rather than the full standard
algorithm. See SHORTCUT comments in `decl.c` near the grouped-declarator
branch. The gate is a one-token lookahead: `'(' ')'`, `'(' '...')`,
`'(' type-keyword`, or `'(' qualified-name` opens a parameter list;
anything else is direct-init. Inside the gate, a real ParseState
save/restore handles mid-stream failures.

### 2. Template-id vs. less-than

`f<T>(x)` is a function template call iff `f` is a template-name. The
parser consults `lookup_is_template_name` to decide. If false, it parses
as a chain of comparisons.

There's a SHORTCUT in `decl.c` for declarator-id position: any IDENT
followed by `<` is treated as a template-id, regardless of lookup. The
standard requires lookup; we elide it because at declarator-id position
there's no other valid reading (the program is ill-formed without the
template interpretation). See the comment block in `parse_declarator`
for the full discussion.

### 3. Grouped declarator vs. function call

`int (x);` — declaration with redundant parens around the name.
`int (x)(y);` — declaration with grouped declarator.
`f(x);` — function call.

The disambiguation rule is roughly: at *declarator* position, `(` opens
a grouped declarator; at *expression* position, `(` opens a call. The
parser knows which position it's in from the recursion path, so this
isn't actually ambiguous from the parser's perspective.

## Backtracking

Backtracking is rare and narrow. The mechanism:

```c
Token *saved = parser_save(p);
... try parse ...
if (failed) {
    parser_restore(p, saved);
    /* try alternative */
}
```

The `tentative` flag on the Parser is set during a tentative parse so
that side-effecting operations (region creation, declaration insertion)
can short-circuit. This is the discipline by which "tentative parse"
remains *safe* — without it, a failed tentative parse would leave junk
declarations in the enclosing scope.

The major use sites:

- Parameter-list-vs-direct-init (decl.c, parameter parse)
- Pointer-to-member declarator detection
- A couple of declarator suffix lookaheads

## Declarator Parsing

The declarator parsing in `decl.c` is the most subtle code in the parser.
Worth knowing:

### Inside-out type construction

`int (*f[10])(int)` reads as: f is array-of-10 of pointer-to-function-
returning-int-taking-int. C++ declarators read inside-out: the
grouped declarator's contents bind first, then the outer suffixes
apply to *that* result.

Implementation: when `parse_declarator` recurses into a grouped
declarator, it lets the inner recursion build whatever it builds, then
"unwinds" any leading `*`/`&`/`[]` modifiers into a `pending_wrap[]`
stack. The outer suffix (function args, more `[]`) then applies to the
inner's BASE type. After the suffix is built, the pending wraps are
re-applied in reverse order via `apply_pending_wrap`, which preserves
cv-qualifiers and array sizes.

This is why the parser correctly handles:

- `int (*p)(int)` — pointer to function
- `int (*p[N])(int)` — array of function pointers (the `[N]` is on the
  inner `p`, but the function-suffix `(int)` is outer; outcome: `p[i]`
  is a function pointer)
- `int (*const p)(int)` — const pointer to function (cv-qualifier
  preserved through the unwind/rewrap)

### Capturing default arguments

Per-parameter default values are captured as their parsed expression
nodes and stored in a `Type.param_defaults[]` array on the function
type. The codegen later injects them at call sites that pass fewer args
than params. Defaults are merged across re-declarations (e.g. header
declares the default, definition omits it) in `region_declare_in`.

## Deferred Method Bodies

A method body inside a class definition cannot be parsed eagerly
because it may reference members declared later in the same class
(N4659 §6.4.5/3 [class.scope]: the class is treated as complete within
member bodies). The parser:

1. When it sees a `{` opening a method body, captures the token range
   `[start, end]` (matching brace via depth count) and stores it on the
   `ND_FUNC_DEF` node.
2. After the class body's closing `}`, walks back through the deferred
   methods and re-parses each body inside the now-complete class scope.

This is the only place the parser does multi-pass work over the source.

## Out-of-Class Method Definitions

`void Foo::bar() { ... }` is parsed by:

1. Recognising `Foo::` as a qualified declarator-id.
2. Looking up `Foo` to get its `class_region`.
3. Pushing the class region as the parser's enclosing scope while
   parsing the parameter list and body, so member typedefs (`value_type`)
   etc.) and other members resolve.
4. Linking the function back to the class via its `class_type` slot.

Templates add a wrinkle: `template<T> void Box<T>::set(T)` requires the
template parameter scope to be visible *inside* the class scope during
body parsing. See the template parameter scope handling in `decl.c`
(comment references N4659 §17.7/4 [temp.res]).

## Notable SHORTCUTs and TODOs

The codebase consistently labels shortcuts with `SHORTCUT (ours, not the
standard's):` followed by a description and a TODO tag. Greppable. Major
ones in the parser:

- **`decl.c`**: gated tentative param-list parse vs. full §11.2/1
  algorithm
- **`decl.c`**: declarator-id `<` always parsed as template-id without
  lookup check
- **`type.c`**: unknown ident in type-position with declarator-shaped
  follower → opaque type-name
- **`expr.c`**: `__name(...)` for any unknown leading-underscore ident
  → bool-lit (catches type-trait builtins; explicitly excludes
  `__builtin_va_start/end`)

Each has the standard's actual rule cited and a TODO tag for tracking.

## Extension Points

The parser is structured so that extending it for newer C++ standards
(C++20 modules, concepts, coroutines, C++23 deducing-this) is mostly a
matter of adding productions to the relevant file (`expr.c`, `stmt.c`,
`decl.c`) and possibly a new node kind. The existing C++17-baseline
grammar coverage is described in
[`grammar-evolution.md`](grammar-evolution.md).
