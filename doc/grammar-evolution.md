# C++ Grammar Evolution: C++17 → C++20 → C++23

## Source Documents

| Standard | Draft    | Annex A Pages |
|----------|----------|---------------|
| C++17    | N4659   | 1412–1433     |
| C++20    | N4861   | 1610–1630     |
| C++23    | N4950   | 1881–1902     |

All three drafts are available locally as PDFs in this directory.

---

## C++17 → C++20: Major Grammar Additions (~30+ new productions)

### Modules (entirely new — ~12 productions)

```
module-declaration:
    export-keyword_opt module-keyword module-name module-partition_opt
        attribute-specifier-seq_opt ;

module-name:
    module-name-qualifier_opt identifier

module-partition:
    : module-name-qualifier_opt identifier

module-name-qualifier:
    identifier .
    module-name-qualifier identifier .

export-declaration:
    export declaration
    export { declaration-seq_opt }
    export-keyword module-import-declaration

module-import-declaration:
    import-keyword module-name attribute-specifier-seq_opt ;
    import-keyword module-partition attribute-specifier-seq_opt ;
    import-keyword header-name attribute-specifier-seq_opt ;

global-module-fragment:
    module-keyword ; declaration-seq_opt

private-module-fragment:
    module-keyword : private ; declaration-seq_opt
```

### Concepts and Constraints (~15 productions)

```
concept-definition:
    concept concept-name attribute-specifier-seq_opt = constraint-expression ;

requires-clause:
    requires constraint-logical-or-expression

constraint-logical-or-expression:
    constraint-logical-and-expression
    constraint-logical-or-expression || constraint-logical-and-expression

constraint-logical-and-expression:
    primary-expression
    constraint-logical-and-expression && primary-expression

type-constraint:
    nested-name-specifier_opt concept-name
    nested-name-specifier_opt concept-name < template-argument-list_opt >

requires-expression:
    requires requirement-parameter-list_opt requirement-body

requirement-body:
    { requirement-seq }

requirement:
    simple-requirement          // expression ;
    type-requirement            // typename nested-name-specifier_opt type-name ;
    compound-requirement        // { expression } noexcept_opt return-type-requirement_opt ;
    nested-requirement          // requires constraint-expression ;

return-type-requirement:
    -> type-constraint
```

### Coroutines (3 productions)

```
yield-expression:
    co_yield assignment-expression
    co_yield braced-init-list

coroutine-return-statement:
    co_return expr-or-braced-init-list_opt ;

await-expression:
    co_await cast-expression
```

### Three-Way Comparison (1 production)

```
compare-expression:
    shift-expression
    compare-expression <=> shift-expression
```

Inserted between shift-expression and relational-expression in the
precedence chain. The `<=>` operator was added to operator-or-punctuator.

### Designated Initializers (3 productions)

```
designated-initializer-list:
    designated-initializer-clause
    designated-initializer-list , designated-initializer-clause

designated-initializer-clause:
    designator brace-or-equal-initializer

designator:
    . identifier
```

### Modified Existing Productions in C++20

**primary-expression:** added `requires-expression` alternative.

**lambda-expression:** added form with explicit template parameters:
```
lambda-introducer < template-parameter-list > requires-clause_opt
    lambda-declarator_opt compound-statement
```

**lambda-declarator:** added trailing `requires-clause_opt`.

**simple-capture / init-capture:** added pack expansion (`..._opt`).

**unary-expression:** added `await-expression` alternative.

**assignment-expression:** added `yield-expression` alternative.

**jump-statement:** added `coroutine-return-statement` alternative.

**translation-unit:** added module-based alternative:
```
global-module-fragment_opt module-declaration declaration-seq_opt
    private-module-fragment_opt
```

**declaration:** added `export-declaration`, `module-import-declaration`.

**decl-specifier:** added `consteval`, `constinit`.

**placeholder-type-specifier:** added `type-constraint_opt` prefix:
```
type-constraint_opt auto
type-constraint_opt decltype ( auto )
```

**template-head:** added trailing `requires-clause_opt`.

**type-parameter:** added constrained forms with `type-constraint`.

**init-declarator / member-declarator:** added `requires-clause` alternatives.

**braced-init-list:** added `designated-initializer-list` alternative.

**unqualified-id:** changed `~ class-name` to `~ type-name`.

**pseudo-destructor-name:** removed entirely (folded into unqualified-id).

**member-declaration:** added `using-enum-declaration`.

**operator list:** added `co_await`, `<=>`.

**Preprocessing:** added module-related directives (`pp-module`, `pp-import`),
`__has_cpp_attribute`, `__VA_OPT__`, `elifdef`/`elifndef`.

### Removed in C++20

- **pseudo-destructor-name** production (folded into unqualified-id)
- Separate **operator** / **punctuator** token categories (merged into
  **operator-or-punctuator**)

---

## C++20 → C++23: Incremental Refinement (~15 new productions)

### if consteval (2 new selection-statement forms)

```
selection-statement:
    ...existing...
    if !_opt consteval compound-statement
    if !_opt consteval compound-statement else statement
```

### Deducing this (explicit object parameter)

`this` keyword added before `decl-specifier-seq` in parameter-declaration:
```
parameter-declaration:
    attribute-specifier-seq_opt this_opt decl-specifier-seq declarator
    attribute-specifier-seq_opt this_opt decl-specifier-seq declarator
        = initializer-clause
    attribute-specifier-seq_opt this_opt decl-specifier-seq
        abstract-declarator_opt
    attribute-specifier-seq_opt this_opt decl-specifier-seq
        abstract-declarator_opt = initializer-clause
```

### Static Lambdas

Lambda declarator restructured with new `lambda-specifier` production:
```
lambda-specifier:
    consteval
    constexpr
    mutable
    static              // NEW in C++23

lambda-specifier-seq:
    lambda-specifier
    lambda-specifier lambda-specifier-seq
```

### Extended Floating-Point Literals

```
floating-point-suffix: one of
    f l f16 f32 f64 f128 bf16 F L F16 F32 F64 F128 BF16
```

### Size Literals

```
size-suffix: one of
    z Z
```

Added `size-suffix` alternatives to `integer-suffix`.

### Named and Delimited Escape Sequences (~8 productions)

```
named-universal-character:
    \N{ n-char-sequence }

// Delimited forms:
octal-escape-sequence (new alternative):
    \o{ simple-octal-digit-sequence }

hexadecimal-escape-sequence (new alternative):
    \x{ simple-hexadecimal-digit-sequence }

universal-character-name (new alternatives):
    \u{ simple-hexadecimal-digit-sequence }
    named-universal-character
```

Escape sequences reorganized with:
- `simple-escape-sequence-char` (named list)
- `numeric-escape-sequence` (grouping)
- `conditional-escape-sequence` (new)

### Unicode Identifier Model

```
// C++20:
identifier-nondigit:
    nondigit
    universal-character-name

// C++23 (replaces identifier-nondigit):
identifier-start:
    nondigit
    element with Unicode property XID_Start

identifier-continue:
    digit
    nondigit
    element with Unicode property XID_Continue
```

### Labels Refactored

```
// C++23 splits labeled-statement:
label:
    attribute-specifier-seq_opt identifier :
    attribute-specifier-seq_opt case constant-expression :
    attribute-specifier-seq_opt default :

label-seq:
    label
    label-seq label

// Labels can now appear at end of compound-statement:
compound-statement:
    { statement-seq_opt label-seq_opt }
```

### Declaration Restructuring

```
// C++23 splits declaration:
declaration:
    name-declaration
    special-declaration

name-declaration:
    block-declaration
    nodeclspec-function-declaration
    function-definition
    template-declaration
    deduction-guide
    linkage-specification
    namespace-definition
    empty-declaration
    attribute-declaration
    module-import-declaration

special-declaration:
    explicit-instantiation
    explicit-specialization
    export-declaration
```

### Other C++23 Changes

- **init-statement:** added `alias-declaration` alternative
- **using-enum-declaration:** changed from `using elaborated-enum-specifier ;`
  to `using enum using-enum-declarator ;` with new `using-enum-declarator`
  production

---

## Impact Assessment for Trusted Bootstrap Transpiler

### What we need now (GCC target = C++14, LLVM target = C++17)

The **C++17 Annex A is sufficient** for both current targets. No C++20 or
C++23 grammar additions are needed today.

### What might be needed if LLVM moves to C++20

The LLVM project has an active RFC to raise compiler requirements toward
C++20. If adopted, the transpiler would need:

| Feature | Grammar Impact | Implementation Difficulty |
|---------|---------------|--------------------------|
| Concepts/Constraints | ~15 productions, requires-clause integration | **High** — new semantic domain |
| Coroutines | 3 new expression/statement forms | **High** — complex transformation to C |
| Modules | ~12 productions | **Medium** — mostly organisational |
| Three-way comparison | 1 production, operator precedence change | **Low** |
| Designated initializers | 3 productions | **Low** |
| consteval / constinit | 2 new decl-specifiers | **Medium** |

**Concepts** are the biggest concern — they add a new constraint-checking
layer to template instantiation and overload resolution.

**Coroutines** are hard to transpile to C (state machine transformation)
but whether LLVM's own source would use them is uncertain.

**Modules** would have major build system implications but the grammar
changes are straightforward.

### C++23 features unlikely to be needed soon

`if consteval`, deducing this, static lambdas, and the lexical changes
are unlikely to appear in GCC or LLVM's own source code in the near term.
The grammar is incremental and could be added later without architectural
changes to the parser.

### Recommendation

Implement the C++17 grammar fully. Design the parser so that adding the
C++20 productions (especially concepts and coroutines) is a matter of
adding new AST node types and parse functions, not restructuring the
parser. The expression precedence chain should accommodate `<=>` from
the start, even if not immediately needed — it's a one-line addition
that avoids a precedence chain restructure later.
