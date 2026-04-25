# C++17 Disambiguation Rules Audit

## Overview

Annex A (N4659, page 1412) states:

> "The grammar described here accepts a superset of valid C++ constructs.
> Disambiguation rules (9.8, 10.1, 13.2) must be applied to distinguish
> expressions from declarations."

This document catalogues **every** disambiguation rule in the C++17 standard,
including those beyond the three sections cited in Annex A. Each rule is
documented with the spec text, concrete examples, which interpretation wins,
and what a recursive descent parser needs to handle it.

---

## Rule 1: Expression-statement vs. declaration (Section 9.8 [stmt.ambig])

**The rule:** An expression-statement with a function-style explicit type
conversion as its leftmost subexpression can be indistinguishable from a
declaration where the first declarator starts with `(`. **The statement is
a declaration.**

**The standard says:** "The disambiguation is purely syntactic; that is, the
meaning of the names occurring in such a statement, beyond whether they are
*type-names* or not, is not generally used in or changed by the disambiguation."

```cpp
class T { public: T(); T(int); };

T(a);           // declaration of 'a' as type T — NOT expression T(a)
T(*b)();        // declaration of pointer-to-function b returning T
T(c) = 7;       // declaration of c as type T, initialized with 7
T(d), e, f = 3; // declaration of d, e, f as type T

// Unambiguously expressions (operators impossible in declarators):
T(a)->m = 7;    // expression (-> forces expression interpretation)
T(a)++;         // expression (++ forces expression interpretation)
T(a, 5) << c;   // expression (<< forces expression interpretation)
```

**Which wins:** Declaration always wins.

**This is the "most vexing parse":**
```cpp
T a(T());       // declares function a, NOT variable a of type T
```

**Parser needs:** Tentative parse. Try declaration first; if it fails
syntactically, fall back to expression. Requires knowing whether the
leading identifier is a type-name.

**Semantic info required:** YES — type-name lookup only.

---

## Rule 2: Declarator-level ambiguity (Section 11.2 [dcl.ambig.res])

**The rule (11.2/1):** Same ambiguity as Rule 1 but in declaration context.
Choice between function declaration with redundant parentheses around a
parameter name, and object declaration with function-style cast initializer.
**Considered a declaration.**

```cpp
void foo(double a) {
    S w(int(a));      // function declaration: S w(int a);
    S x(int());       // function declaration: S x(int (*)())
    S y((int(a)));    // object declaration (extra parens force expression)
    S z = int(a);     // object declaration (copy-init, unambiguous)
}
```

**The rule (11.2/2):** Ambiguity between type-id and expression.
**Resolved to type-id.**

```cpp
template<class T> void f();
template<int I> void f();

f<int()>();     // int() is a type-id (function type), NOT expression int()
```

**The rule (11.2/3):** In parameter-declaration-clause, when a type-name
is nested in parentheses, it is considered a simple-type-specifier
(starting a parameter type) rather than a declarator-id.

```cpp
class C {};
void f(int(C)) {}   // void f(int(*)(C)) — NOT void f(int C)
```

**Which wins:** Declaration / type-id always wins.

**Parser needs:** Tentative parse. Same type-name oracle as Rule 1.

**Semantic info required:** YES — type-name lookup.

---

## Rule 3: Type-name in decl-specifier-seq (Section 10.1/3 [dcl.spec])

**The rule:** If a type-name is encountered while parsing a decl-specifier-seq,
it is interpreted as part of the decl-specifier-seq if and only if there is
no previous defining-type-specifier other than a cv-qualifier.

```cpp
typedef char* Pc;

static Pc;            // error: Pc is the type, name is missing
void f(const Pc);     // void f(char* const) — Pc is the type
void g(const int Pc); // void g(const int)   — Pc is the parameter name
void h(unsigned Pc);  // void h(unsigned int) — Pc is the parameter name
```

**Which wins:** Type-specifier wins if no defining-type-specifier has been
seen yet; otherwise, it's the declarator name.

**Parser needs:** Greedy left-to-right scan with a boolean flag tracking
whether a defining-type-specifier has been consumed.

**Semantic info required:** YES — type-name lookup.

---

## Rule 4: The `<` template angle bracket problem (Section 17.2/3 [temp.names])

**The rule:** After name lookup finds that a name is a template-name, a
following `<` is **always** taken as the delimiter of a template-argument-list,
never as less-than. The first non-nested `>` ends the template-argument-list.
`>>` is treated as two consecutive `>` tokens.

```cpp
template<int i> class X {};

X< 1>2 > x1;      // syntax error: parsed as X<1> 2 > x1
X<(1>2)> x2;      // OK: parentheses protect the >

template<class T> class Y {};
Y<X<1>> x3;       // OK (since C++11): >> split into > >
```

**In dependent contexts,** the `template` keyword is required:

```cpp
template<class T> void f(T* p) {
    T* p1 = p->alloc<200>();            // ill-formed: < means less-than
    T* p2 = p->template alloc<200>();   // OK: < starts template arg list
}
```

**Which wins:** Template-argument-list, if the name is known to be a template.
In dependent contexts, `template` keyword is the signal.

**Parser needs:** Name lookup feedback. When parser sees `identifier <`, it
must check if the identifier is a template-name. If yes, enter balanced
delimiter mode for `<` / `>`. Must implement `>>` splitting.

**Semantic info required:** YES — template-name lookup. In dependent contexts,
the `template` keyword provides the information syntactically.

---

## Rule 5: Type-id vs. expression in template arguments (Section 17.3/2 [temp.arg])

**The rule:** In a template-argument, an ambiguity between a type-id and an
expression is resolved to a type-id, **regardless of the form of the
corresponding template-parameter.**

```cpp
template<class T> void f();
template<int I> void f();

f<int()>();    // int() is a type-id (function type), calls first f
               // NOT expression int() which evaluates to 0
```

**Which wins:** type-id always wins.

**Parser needs:** Tentative parse — try type-id first, fall back to expression.

**Semantic info required:** NO — this is purely syntactic. The standard
explicitly says the form of the template-parameter is irrelevant.

---

## Rule 6: Dependent name defaults (Section 17.6/2 [temp.res])

**The rule:** A name dependent on a template-parameter is assumed **not to
name a type** unless qualified by `typename`. Similarly, a dependent name
is assumed **not to name a template** unless preceded by `template`.

```cpp
template<class T> void f(int i) {
    T::x * i;              // multiplication (T::x assumed to be a value)
    typename T::x * i;     // pointer declaration (T::x is a type)
    p->template foo<3>();   // template member call
}
```

**Which wins:** Non-type (value/expression) by default. Programmer overrides
with `typename` or `template`.

**Parser needs:** No lookahead or tentative parsing needed. The default is
always "not a type, not a template" — the parser can proceed deterministically.
`typename` and `template` are simple keyword checks.

This is the key rule that makes C++ templates **parseable** despite
incomplete type information. The defaults are safe, and the keywords
provide explicit disambiguation when needed.

**Semantic info required:** NO for initial template parse — that's the
whole point. Semantic checking happens at instantiation time.

---

## Rule 7: Maximal munch (Section 5.4/3 [lex.pptoken])

**The rule:** The next preprocessing token is the longest sequence of
characters that could constitute a token, even if that causes further
analysis to fail.

```cpp
x+++++y     // x ++ ++ + y  (NOT x ++ + ++ y)
            // former is ill-formed but maximal munch requires it

0xe+foo     // single pp-number token, not 0xe + foo
```

Special exception: `<::` is not treated as the digraph `<:` followed by `:`
when the next character is neither `:` nor `>`.

**Which wins:** Longest possible token, unconditionally.

**Parser needs:** Greedy lexer. This is handled entirely at the lexer level.

**Semantic info required:** NO — purely lexical.

---

## Rule 8: Member name lookup in class hierarchies (Section 13.2 [class.member.lookup])

**The rule:** Member name lookup in the presence of multiple inheritance
can find the same name in different base classes. If the declarations
differ, the program is ill-formed.

```cpp
struct A { int x; };
struct B { float x; };
struct C : A, B {};

C c;
c.x;        // error: ambiguous — A::x or B::x?
c.A::x;     // OK — explicit qualification
```

**Note:** Despite being cited in Annex A alongside syntactic disambiguation
rules, this is **entirely semantic**. It does not affect parsing at all —
the parser builds `c.x` as a member access expression regardless. The
ambiguity is detected during name resolution.

**Parser needs:** Nothing special.

**Semantic info required:** YES — but post-parse, during semantic analysis.

---

## Summary

| # | Section | Ambiguity | What wins | Syntactic/Semantic | Parser technique |
|---|---------|-----------|-----------|-------------------|-----------------|
| 1 | 9.8 | expr-stmt vs. declaration | Declaration | Semantic (type-name) | Tentative parse |
| 2 | 11.2 | declarator-level same | Declaration / type-id | Semantic (type-name) | Tentative parse |
| 3 | 10.1/3 | type-name in decl-specifier-seq | Type-spec if first | Semantic (type-name) | Greedy scan + flag |
| 4 | 17.2/3 | `<` as template vs. less-than | Template (if template-name) | Semantic (template-name) | Name lookup feedback |
| 5 | 17.3/2 | type-id vs. expr in template arg | type-id | Syntactic | Tentative parse |
| 6 | 17.6/2 | dependent name: type vs. value | Non-type (default) | Syntactic (by design) | Keywords override |
| 7 | 5.4/3 | maximal munch | Longest token | Syntactic (lexical) | Greedy lexer |
| 8 | 13.2 | member in class hierarchy | Ill-formed if ambiguous | Semantic (post-parse) | None |

---

## Key Takeaways for the Transpiler Parser

### The parser needs exactly two semantic oracles:

1. **Is this identifier a type-name?** (Rules 1, 2, 3)
2. **Is this identifier a template-name?** (Rule 4)

These two queries, answered by the symbol table, resolve ALL parsing
ambiguities in non-dependent contexts.

### In dependent contexts (inside templates), no oracles are needed:

Rule 6 provides safe defaults (not a type, not a template). The `typename`
and `template` keywords are explicit syntactic markers that override the
defaults. The parser can proceed deterministically through template
definitions without looking up dependent names.

### Tentative parsing is needed in exactly three places:

1. Statement-level: is this a declaration or expression? (Rule 1)
2. Declarator-level: same class of ambiguity (Rule 2)
3. Template argument: is this a type-id or expression? (Rule 5)

In all three cases, the strategy is: try the declaration/type-id
interpretation first; if it fails syntactically, it's an expression.

### The declaration-wins principle is universal:

Whenever there is an ambiguity between a declaration interpretation and an
expression interpretation, the declaration wins. This is consistent across
all rules and simplifies the tentative parsing logic — the "try first"
branch is always the same (declaration/type-id).

### The `>>` splitting rule requires lexer-parser cooperation:

When inside a template argument list, the parser must tell the lexer (or
re-tokenize itself) to split `>>` into two `>` tokens. This is typically
implemented by the parser tracking template argument nesting depth.
