/*
 * parse.h — Internal header for the sea-front parser.
 *
 * Defines AST node types, the Type system, Parser state, and all
 * parse function prototypes. Included by all src/parse/ source files.
 *
 * Grammar references are to the C++17 standard draft N4659 (Annex A).
 * Where C++20 (N4861) or C++23 (N4950) change a production, the change
 * is noted. Productions are identified by section number and grammar
 * rule name.
 */

#ifndef PARSE_H
#define PARSE_H

#include "../sea-front.h"

/* ================================================================== */
/* AST Node Kinds                                                     */
/* ================================================================== */

/* Forward declarations — Node fields reference DeclarativeRegion and
 * Declaration via pointer, so we need the typedefs visible before
 * struct Node is defined further down. */
typedef struct DeclarativeRegion DeclarativeRegion;
typedef struct Declaration       Declaration;
typedef struct Node              Node;

/* Constructor mem-initializer-list entry — N4659 §15.6.2 [class.base.init].
 * Each member-init in the source like 'a(1)' or 'b(make_b(), 7)' becomes
 * one MemInit entry on the func node. Bases not yet handled. */
typedef struct MemInit {
    struct Token *name;  /* member identifier */
    Node        **args;
    int           nargs;
} MemInit;

typedef enum {
    /* -- Expressions --
     * N4659 §8 [expr]
     * C++20 adds: co_await (§8.3.8), co_yield (in assignment-expr),
     *             three-way comparison <=> (§8.10), requires-expr (§8.1.7.3)
     * C++23 adds: static operator(), if consteval (stmt only)
     */
    ND_NUM,             /* integer literal — N4659 §5.13.2 [lex.icon] */
    ND_FNUM,            /* floating literal — N4659 §5.13.4 [lex.fcon] */
    ND_STR,             /* string literal — N4659 §5.13.5 [lex.string] */
    ND_CHAR,            /* character literal — N4659 §5.13.3 [lex.ccon] */
    ND_IDENT,           /* unresolved identifier (pre-sema) — N4659 §8.1.4 [expr.prim.id] */
    ND_QUALIFIED,       /* qualified-id — N4659 §8.1.4.3 [expr.prim.id.qual]
                         *   nested-name-specifier template(opt) unqualified-id
                         * E.g.: std::cout, Foo::bar, ::global
                         * Parts stored as an array of tokens (the name chain).
                         * Sema resolves the qualified lookup. */
    ND_BOOL_LIT,        /* true/false — N4659 §5.13.6 [lex.bool] */
    ND_NULLPTR,         /* nullptr — N4659 §5.13.7 [lex.nullptr]
                         * Type is std::nullptr_t (§21.2.4), distinct from int 0.
                         * C++20/23: unchanged. */

    ND_BINARY,          /* binary op — N4659 §8.5-§8.15 [expr.mul through expr.log.or]
                         * C++20: adds <=> at §8.10 [expr.spaceship] */
    ND_UNARY,           /* prefix unary — N4659 §8.3 [expr.unary]
                         * Includes: + - ! ~ * & ++ --
                         * C++20: adds co_await (§8.3.8 [expr.await]) */
    ND_POSTFIX,         /* postfix ++ -- — N4659 §8.2.6 [expr.post.incr] */
    ND_ASSIGN,          /* = += -= etc — N4659 §8.18 [expr.ass]
                         * C++20: adds co_yield as assignment-expr alternative */
    ND_TERNARY,         /* ?: — N4659 §8.16 [expr.cond] */
    ND_COMMA,           /* comma — N4659 §8.19 [expr.comma] */

    ND_CALL,            /* function call — N4659 §8.2.2 [expr.call] */
    ND_MEMBER,          /* . and -> — N4659 §8.2.5 [expr.ref] */
    ND_SUBSCRIPT,       /* [] — N4659 §8.2.1 [expr.sub] */
    ND_CAST,            /* C-style cast (type)expr — N4659 §8.4 [expr.cast]
                         * Note: also covers functional casts T(expr) in later stages */
    ND_SIZEOF,          /* sizeof — N4659 §8.3.3 [expr.sizeof]
                         * C++11 adds sizeof...(pack) */
    ND_ALIGNOF,         /* alignof — N4659 §8.3.6 [expr.alignof] */
    ND_OFFSETOF,        /* GCC __builtin_offsetof(type, member)
                         * Parsed into a type + raw member-token range.
                         * Codegen re-emits as '__builtin_offsetof(T, m)'
                         * so gcc computes the real offset. Used by
                         * stddef.h's offsetof macro and libcpp's
                         * DEFAULT_ALIGNMENT. Not in the C++ standard
                         * (§20.2.5 exposes 'offsetof' via <cstddef>
                         * without specifying the implementation). */
    ND_VA_ARG,          /* GCC __builtin_va_arg(ap, type)
                         * Variadic-argument extraction. Second arg is
                         * a type (not an expression). Parsed into an
                         * (ap, type) pair; codegen re-emits verbatim
                         * for gcc. Used by gcc 4.8 tree-data-ref.c
                         * conflict_fn. Not in standard C++. */
    ND_STMT_EXPR,       /* GCC statement-expression ({ stmts; expr; })
                         * Not in the standard — gcc extension used
                         * heavily in glibc/libiberty macros (obstack,
                         * __extension__). The value is the last
                         * expression-statement in the block. Captured
                         * as a raw token range so codegen can re-emit
                         * the extension verbatim for a gcc compiler. */
    ND_INIT_LIST,       /* braced-init-list — N4659 §11.6.4 [dcl.init.list]
                         *   { initializer-list(opt) ,(opt) }
                         * Used as an initializer for arrays and aggregates,
                         * and (via list-initialization) for class types. */

    /* -- Statements --
     * N4659 §9 [stmt.stmt]
     * C++20 adds: coroutine-return-statement (co_return, §9.6.3.1)
     * C++23 adds: if consteval (§9.4.2)
     */
    ND_BLOCK,           /* compound-statement — N4659 §9.3 [stmt.block] */
    ND_RETURN,          /* return — N4659 §9.6.3 [stmt.return] */
    ND_IF,              /* if — N4659 §9.4.1 [stmt.if]
                         * C++17 adds: if constexpr, init-statement
                         * C++23 adds: if consteval (N4950 §9.4.2) */
    ND_WHILE,           /* while — N4659 §9.5.1 [stmt.while] */
    ND_DO,              /* do-while — N4659 §9.5.2 [stmt.do] */
    ND_FOR,             /* for — N4659 §9.5.3 [stmt.for]
                         * C++11 adds: range-based for (§9.5.4 [stmt.ranged]) */
    ND_SWITCH,          /* switch — N4659 §9.4.2 [stmt.switch]
                         * C++17 adds: init-statement */
    ND_CASE,            /* case — N4659 §9.1 [stmt.label] */
    ND_DEFAULT,         /* default — N4659 §9.1 [stmt.label] */
    ND_BREAK,           /* break — N4659 §9.6.1 [stmt.break] */
    ND_CONTINUE,        /* continue — N4659 §9.6.2 [stmt.cont] */
    ND_GOTO,            /* goto — N4659 §9.6.4 [stmt.goto] */
    ND_LABEL,           /* labeled-statement — N4659 §9.1 [stmt.label] */
    ND_EXPR_STMT,       /* expression-statement — N4659 §9.2 [stmt.expr] */
    ND_NULL_STMT,       /* empty statement (bare ;) — N4659 §9.2 [stmt.expr] */

    /* -- Declarations --
     * N4659 §10 [dcl.dcl]
     * C++20 adds: module-declaration, concept-definition,
     *             explicit(bool), constinit, consteval
     * C++23 adds: static operator[], deducing this
     */
    ND_VAR_DECL,        /* simple-declaration (variable) — N4659 §10 [dcl.dcl] */
    ND_FUNC_DEF,        /* function-definition — N4659 §11.4 [dcl.fct.def] */
    ND_FUNC_DECL,       /* function declaration (prototype) — N4659 §11.3.5 [dcl.fct] */
    ND_PARAM,           /* parameter-declaration — N4659 §11.3.5 [dcl.fct] */
    ND_TYPEDEF,         /* typedef — N4659 §10.1.3 [dcl.typedef] */

    ND_FRIEND,          /* friend declaration — N4659 §14.3 [class.friend]
                         * Wraps the inner declaration (class, function, template).
                         * Sema uses this to grant access to private members. */

    /* -- Classes --
     * N4659 §12 [class] (Annex A.8 [gram.class])
     * C++20: no structural grammar changes to class definitions
     * C++23: deducing this (explicit object parameter in methods)
     */
    ND_CLASS_DEF,       /* class-specifier — N4659 §12.1 [class.name]
                         *   class-key identifier(opt) base-clause(opt)
                         *       { member-specification(opt) }
                         * Produced by type.c when parsing struct/class body */
    ND_ACCESS_SPEC,     /* access-specifier — N4659 §12.2 [class.access.spec]
                         *   public: | protected: | private: */

    /* -- Templates --
     * N4659 §17 [temp] (Annex A.12 [gram.temp])
     * C++20: adds concepts (requires-clause on template-declaration),
     *        abbreviated function templates (auto params)
     * C++23: no changes to template grammar
     */
    ND_TEMPLATE_DECL,   /* template-declaration — N4659 §17.1 [temp]
                         *   template < template-parameter-list > declaration */
    ND_TEMPLATE_ID,     /* simple-template-id — N4659 §17.2 [temp.names]
                         *   template-name < template-argument-list(opt) > */

    /* -- Top level -- */
    ND_TRANSLATION_UNIT, /* translation-unit — N4659 §A.5 (grammar) /
                          * §5.2/2 [lex.phases]
                          * C++20: adds module-declaration at TU level */
} NodeKind;

/* ================================================================== */
/* AST Node                                                           */
/* ================================================================== */

/*
 * Tagged union with embedded structs. Each node kind has a dedicated
 * anonymous struct in the union, documenting exactly which fields are
 * valid. All allocations come from the Parser's arena.
 *
 * Arrays (params, stmts, args, decls) use Node** + int count, built
 * via Vec during parsing, then "frozen" by copying the Vec's data pointer.
 */
struct Node {
    NodeKind kind;
    Token *tok;         /* anchoring token — for error messages and source location */

    /* Sema-resolved type of this node, when applicable.
     *
     * For expression nodes, this is the type of the value the
     * expression evaluates to (e.g. 'int' for '1 + 2', 'T*' for '&x').
     * Filled in by the sema phase; NULL until then. NULL also for
     * non-expression nodes (statements, declarations themselves) where
     * "type of the node" doesn't apply. */
    Type *resolved_type;

    /* Two-phase lookup (N4659 §17.7 [temp.res]): true when this
     * expression or sub-expression depends on a template parameter.
     * Set during parsing (Phase 1); used by clone.c and sema.c
     * to decide which names need Phase-2 lookup at instantiation.
     *
     * Base case: ND_IDENT whose resolved type is TY_DEPENDENT.
     * Propagation: any compound expression/statement inherits
     * dependence from its children (bottom-up). */
    bool is_type_dependent;

    /* Codegen-only: when emit_c hoists a class-typed expression
     * into a synthesized local (Slice D temp materialization), the
     * original expression node is tagged with the local's name.
     * emit_expr then substitutes the name verbatim instead of
     * re-emitting the expression. NULL when no hoisting happened. */
    const char *codegen_temp_name;

    union {
        /* ND_NUM — N4659 §5.13.2 [lex.icon]
         * 128-bit binary representation with sign flag.
         * Covers all standard integer types including __int128 extension.
         * The token's ud_suffix field carries any UDL suffix. */
        struct {
            uint64_t lo;
            uint64_t hi;
            bool is_signed;
        } num;

        /* ND_FNUM — N4659 §5.13.4 [lex.fcon] */
        struct {
            double fval;
        } fnum;

        /* ND_STR — N4659 §5.13.5 [lex.string]
         * Adjacent string literals are concatenated in translation phase 6.
         * We store the token directly; the semantic layer handles concat. */
        struct {
            Token *tok;     /* points to the string token (loc/len/enc/is_raw) */
        } str;

        /* ND_CHAR — N4659 §5.13.3 [lex.ccon] */
        struct {
            Token *tok;
        } chr;

        /* ND_IDENT — N4659 §8.1.4 [expr.prim.id]
         * Before semantic analysis, this is an unresolved name.
         * Sema resolves it to a variable, function, enumerator, etc. */
        struct {
            Token *name;
            /* Sema-set: true when the name resolved to a class member
             * accessed via the implicit 'this' (i.e. unqualified
             * reference inside a method body). Codegen rewrites these
             * to 'this->name' (data members) or 'Class_name(this,...)'
             * (member functions). */
            bool implicit_this;
            /* Sema-set: the resolved declaration. NULL until sema runs.
             * Used by codegen to recover the home class for method-
             * call mangling.
             *
             * For overloaded names this is ONE member of overload_set
             * (historically the first-found, kept as-is so existing
             * consumers keep working). Proper per-call overload
             * resolution (N4659 §16.3 [over.match]) will pick the
             * winner from overload_set at the call site. */
            Declaration *resolved_decl;
            /* Sema-set: the full overload set (all declarations of
             * this name in the scope where lookup landed). NULL +
             * n_overloads=0 when not yet populated or the name
             * resolved to a non-overloadable entity (type, namespace,
             * template, variable). Populated for function names.
             *
             * Owned by the parser arena; callers read only. */
            Declaration **overload_set;
            int n_overloads;
        } ident;

        /* ND_QUALIFIED — N4659 §8.1.4.3 [expr.prim.id.qual]
         *   qualified-id: nested-name-specifier template(opt) unqualified-id
         * The name chain is stored as an array of tokens: each element is
         * an identifier or operator-function-id from the nested-name-specifier
         * and the final unqualified-id. E.g., std::vector::size_type
         * → parts = ["std", "vector", "size_type"], nparts = 3.
         * global_scope is true for ::foo (starts with ::). */
        struct {
            Token **parts;
            int     nparts;
            bool    global_scope;   /* true if starts with :: */
            Node   *lead_tid;      /* ND_TEMPLATE_ID for leading part's
                                    * template-args (e.g. Box<int>::test
                                    * → lead_tid carries <int>). NULL if
                                    * the leading part has no template args. */
        } qualified;

        /* ND_BINARY, ND_ASSIGN — N4659 §8.5-§8.18
         * op is the TokenKind of the operator (TK_PLUS, TK_STAR, etc.)
         * For assignment: TK_ASSIGN, TK_PLUS_ASSIGN, etc. */
        struct {
            TokenKind op;
            Node *lhs;
            Node *rhs;
        } binary;

        /* ND_UNARY, ND_POSTFIX — N4659 §8.3, §8.2.6 */
        struct {
            TokenKind op;
            Node *operand;
        } unary;

        /* ND_TERNARY — N4659 §8.16 [expr.cond] */
        struct {
            Node *cond;
            Node *then_;
            Node *else_;
        } ternary;

        /* ND_COMMA — N4659 §8.19 [expr.comma] */
        struct {
            Node *lhs;
            Node *rhs;
        } comma;

        /* ND_CALL — N4659 §8.2.2 [expr.call]
         * callee is the function expression (usually ND_IDENT).
         * args/nargs: the argument-expression-list. */
        struct {
            Node *callee;
            Node **args;
            int nargs;
        } call;

        /* ND_MEMBER — N4659 §8.2.5 [expr.ref]
         * op: TK_DOT or TK_ARROW
         * C++20: unchanged. C++23: unchanged. */
        struct {
            Node *obj;
            Token *member;
            TokenKind op;
        } member;

        /* ND_SUBSCRIPT — N4659 §8.2.1 [expr.sub]
         * C++23: adds multidimensional subscript a[i, j] */
        struct {
            Node *base;
            Node *index;
        } subscript;

        /* ND_CAST — N4659 §8.4 [expr.cast]
         * C-style cast: (type)expr.
         * Also used for functional casts T(expr) in later stages. */
        struct {
            Type *ty;
            Node *operand;
        } cast;

        /* ND_SIZEOF — N4659 §8.3.3 [expr.sizeof]
         * Two forms: sizeof(type-id) and sizeof unary-expression.
         * is_type distinguishes them. */
        struct {
            Node *expr;     /* non-NULL when sizeof(expr) */
            Type *ty;       /* non-NULL when sizeof(type) */
            bool is_type;
        } sizeof_;

        /* ND_ALIGNOF — N4659 §8.3.6 [expr.alignof]
         * alignof(type-id) only — always a type, never an expression. */
        struct {
            Type *ty;
        } alignof_;

        /* ND_OFFSETOF — __builtin_offsetof. Emit '__builtin_offsetof(T, mem)'. */
        struct {
            Type  *ty;
            Token *mem_toks;  /* member designator tokens (starting after ',') */
            int    n_mem_toks;
        } offsetof_;

        /* ND_VA_ARG — __builtin_va_arg(ap, type). Codegen re-emits
         * verbatim for gcc to handle. */
        struct {
            Node *ap;
            Type *ty;
        } va_arg_;

        /* ND_STMT_EXPR — GCC statement-expression.
         * Parsed as a normal compound statement so type-names get
         * mangled and member accesses resolve against our renamed
         * structs. Codegen re-wraps the emitted block in '({ ... })'
         * for gcc. N4659 does NOT include this — it's a gcc extension. */
        struct {
            Node *block;   /* ND_BLOCK holding the inner statements */
        } stmt_expr;

        /* ND_INIT_LIST — N4659 §11.6.4 [dcl.init.list]
         * Braced initializer: { e1, e2, ... }. Each element is itself
         * an expression — possibly another ND_INIT_LIST for nested
         * aggregate init like 'int m[2][3] = { { 1,2,3 }, { 4,5,6 } }'. */
        struct {
            Node **elems;
            int    nelems;
        } init_list;

        /* ND_BLOCK — N4659 §9.3 [stmt.block]
         * compound-statement: { statement-seq(opt) } */
        struct {
            Node **stmts;
            int nstmts;
            DeclarativeRegion *scope;  /* sema-side; the block's region */
            bool is_flat;  /* true for comma-separated decls, namespace/extern "C" bodies —
                            * emit statements without wrapping { } braces */
        } block;

        /* ND_RETURN — N4659 §9.6.3 [stmt.return]
         * C++20: adds co_return (separate ND in future) */
        struct {
            Node *expr;     /* NULL for bare 'return;' */
        } ret;

        /* ND_EXPR_STMT — N4659 §9.2 [stmt.expr] */
        struct {
            Node *expr;
        } expr_stmt;

        /* ND_IF — N4659 §9.4.1 [stmt.if]
         * C++17: adds init-statement and constexpr form.
         * init is NULL for plain 'if (cond)'. */
        struct {
            Node *init;     /* C++17 init-statement, or NULL */
            Node *cond;
            Node *then_;
            Node *else_;    /* NULL if no else */
            bool is_constexpr;  /* C++17 if constexpr */
            /* Scope containing the if-init declaration (when cond
             * is an ND_VAR_DECL per C++ if-with-init). Sema pushes
             * this before walking then/else so the declared name
             * resolves in both branches. N4659 §9.4.1/2. */
            DeclarativeRegion *scope;
        } if_;

        /* ND_WHILE — N4659 §9.5.1 [stmt.while] */
        struct {
            Node *cond;
            Node *body;
        } while_;

        /* ND_DO — N4659 §9.5.2 [stmt.do] */
        struct {
            Node *cond;
            Node *body;
        } do_;

        /* ND_FOR — N4659 §9.5.3 [stmt.for]
         * C++11: range-based for (§9.5.4) deferred to later stage.
         * C++17: init-statement in if/switch is similar pattern. */
        struct {
            Node *init;     /* init-statement or declaration */
            Node *cond;     /* condition, or NULL */
            Node *inc;      /* expression, or NULL */
            Node *body;
        } for_;

        /* ND_SWITCH — N4659 §9.4.2 [stmt.switch]
         * C++17: adds init-statement. */
        struct {
            Node *init;     /* C++17 init-statement, or NULL */
            Node *expr;
            Node *body;
        } switch_;

        /* ND_CASE — N4659 §9.1 [stmt.label] */
        struct {
            Node *expr;
            Node *stmt;
        } case_;

        /* ND_DEFAULT — N4659 §9.1 [stmt.label] */
        struct {
            Node *stmt;
        } default_;

        /* ND_GOTO — N4659 §9.6.4 [stmt.goto] */
        struct {
            Token *label;
        } goto_;

        /* ND_LABEL — N4659 §9.1 [stmt.label] */
        struct {
            Token *label;
            Node *stmt;
        } label;

        /* ND_VAR_DECL — N4659 §10 [dcl.dcl], §11 [dcl.decl]
         * A simple-declaration: decl-specifier-seq declarator(opt) = init(opt)
         * C++17: adds structured bindings (§11.5, deferred)
         * C++20: adds constinit, consteval specifiers */
        struct {
            Type *ty;
            Token *name;
            Node *init;             /* '= expr' initializer, or NULL */
            /* Direct-initialization 'T x(args)' — N4659 §11.6/16.
             * Filled when the parser sees '(' after the declarator.
             * Codegen lowers this as 'struct T x; T_ctor(&x, args);'.
             * has_ctor_init distinguishes 'T x()' (zero args, default
             * ctor) from no parens at all. */
            Node **ctor_args;
            int    ctor_nargs;
            bool   has_ctor_init;
            /* True for class-member function declarations (ND_VAR_DECL
             * with TY_FUNC) that are constructors / destructors —
             * used by emit_class_def's forward-decl loop to mangle
             * the right way. Mirrors the same flags on ND_FUNC_DEF. */
            bool   is_constructor;
            bool   is_destructor;
            bool   is_virtual;
            /* Bit-field width — N4659 §12.2.4 [class.bit]
             *   member-declarator: identifier(opt) : constant-expression
             * NULL for non-bitfield members. */
            Node  *bitfield_width;
            /* Storage-class / function-specifier flags — N4659
             * §10.1.1 [dcl.stc] / §10.1.6 [dcl.inline]. Copied from
             * the DeclSpec at parse time so codegen can re-emit
             * 'extern', 'static', 'inline' qualifiers. Without this,
             * system-header patterns like 'extern FILE *stdin;' lose
             * the extern and become TU-local definitions, causing
             * multiple-definition link errors. */
            int    storage_flags;  /* DECL_STATIC | DECL_EXTERN | DECL_INLINE | ... */
        } var_decl;

        /* ND_FUNC_DEF — N4659 §11.4 [dcl.fct.def]
         * function-definition: decl-specifier-seq declarator function-body */
        struct {
            Type *ret_ty;
            Token *name;
            Node **params;  /* array of ND_PARAM nodes */
            int nparams;
            bool is_variadic;  /* param list ends with '...' — N4659 §11.3.5/4 [dcl.fct] */
            /* Storage-class / function-specifier flags — N4659
             * §10.1.1 [dcl.stc] / §10.1.6 [dcl.inline]. 'static' on
             * a definition gives internal linkage; 'inline' gates
             * multi-TU header-inline behavior. Without this, every
             * inline in a preprocessed header becomes an external
             * definition and links collide. */
            int    storage_flags;
            Node *body;     /* ND_BLOCK (compound-statement) */
            DeclarativeRegion *param_scope;  /* sema; prototype-scope region */
            /* Constructor mem-initializer-list — N4659 §15.6.2 [class.base.init]
             * 'mem_inits' is an array of MemInit (defined below) entries.
             * As-written order; codegen reorders to declaration order. */
            struct MemInit *mem_inits;
            int n_mem_inits;
            /* For an out-of-class method definition 'int Foo::bar() {}',
             * this is the resolved class type (Foo). NULL for free
             * functions and in-class method definitions. Codegen uses
             * the tag for name mangling and the type for the 'this'
             * parameter. */
            Type *class_type;
            /* For OOL methods whose qualifier is a template-id —
             * 'vec<T, A, vl_embed>::last()' — this is the parsed
             * ND_TEMPLATE_ID node carrying the qualifier args. Used by
             * the template instantiation pass to bind the method to
             * the matching specialization (primary vs partial) instead
             * of attaching it to every instantiation with the same
             * class tag. NULL for non-templated qualifiers.
             *
             * N4659 §17.8.2 [temp.mem]/5 — a member of a class
             * template may be defined outside of the template
             * definition; the definition of a member of a partial
             * specialization is written using the form of the partial
             * specialization's template-id. */
            Node *qual_tid;
            /* True for destructors (parsed from '~ClassName'). The
             * declared name token still points at 'ClassName' (no tilde),
             * so codegen needs this flag to distinguish a dtor from a
             * same-named ctor. Mangled via mangle_class_dtor — see
             * docs/mangling.md and src/codegen/mangle.c. */
            bool is_destructor;
            /* True for constructors (parsed from 'ClassName(...)' inside
             * class ClassName). Like dtors, the declared name token is
             * the class name. Mangled via mangle_class_ctor. */
            bool is_constructor;
            /* Virtual method — N4659 §13.3 [class.virtual]. The
             * 'virtual' specifier was seen in the decl-specifier-seq.
             * Codegen emits a vtable for the class, places a vptr
             * field at offset 0, and dispatches calls through it. */
            bool is_virtual;
            /* N4659 §10.1.7.1 [dcl.type.cv] / §16.3.1/4: 'const'
             * after the parameter list qualifies the implicit object
             * parameter. Used by mangling to distinguish const from
             * non-const overloads (e.g. operator[] const). */
            bool is_const_method;
            /* Deferred body parsing — N4659 §6.4.7/1 [class.mem]/6.
             * For an inline member function defined inside a class
             * body, the function body (and ctor-initializer list) are
             * in "complete-class context": members declared later in
             * the class are visible. We capture the body's token
             * range here during the eager pass and replay it after
             * the closing '}' of the class with the class scope on
             * the lookup chain. body_start_pos == -1 means not
             * deferred (already parsed eagerly, or no body). */
            int body_start_pos;
            int body_end_pos;
            DeclarativeRegion *deferred_class_region;  /* class scope to push at replay */
        } func;

        /* ND_PARAM — N4659 §11.3.5 [dcl.fct]
         * parameter-declaration: decl-specifier-seq declarator
         *
         * For template type parameters (§17.1 [temp.param]),
         * default_type holds the default type-id from '= type-id'
         * (e.g. 'typename T = int'). NULL when no default. The
         * instantiation pass consults this when fewer template
         * arguments are provided than parameters. */
        struct {
            Type *ty;
            Token *name;    /* may be NULL for unnamed params */
            Type *default_type;  /* template param default, or NULL */
            /* Default argument for an ordinary function parameter:
             *   void f(int x, int y = 42);
             * Captured at parse time; injected at call sites that
             * pass fewer args than the function has params.
             * N4659 §11.3.6 [dcl.fct.default]. */
            Node *default_value;
        } param;

        /* ND_TEMPLATE_DECL — N4659 §17.1 [temp] (Annex A.12)
         *   template < template-parameter-list > declaration
         * The inner declaration can be a class, function, variable,
         * alias, or another template (nested templates).
         * C++20: adds requires-clause after template-parameter-list */
        struct {
            Node **params;      /* template-parameter-list (type-params + non-type) */
            int nparams;
            Node *decl;         /* the templated declaration */
        } template_decl;

        /* ND_CLASS_DEF — N4659 §12 [class] (Annex A.8 [gram.class])
         *   class-specifier:
         *       class-head { member-specification(opt) }
         *   class-head:
         *       class-key identifier(opt) base-clause(opt)
         *
         * Members include data members, member functions, access specifiers,
         * nested types, static_assert, using-declarations, etc.
         * C++20: no structural changes. C++23: deducing this. */
        struct {
            Token  *tag;        /* class/struct name (may be NULL for anonymous) */
            Type   *ty;         /* class type, with class_region; may be NULL */
            Node  **members;    /* member-specification as array of nodes */
            int     nmembers;
            /* Base-class types from the base-clause. Stored here
             * (in addition to class_region->bases) so the template
             * instantiation pass can recover template base types
             * (whose class_region is NULL at parse time). */
            Type  **base_types;
            int     nbase_types;
            /* Note: base-class metadata is ALSO stored on the class_def
             * node — it lives on Type.class_region->bases (an array
             * of base-class regions). Codegen reaches it via
             * class_type->class_region->bases[i]->owner_type. The
             * struct-vs-class distinction (class-key) is also not
             * tracked yet — we treat both as TY_STRUCT. */
        } class_def;

        /* ND_FRIEND — N4659 §14.3 [class.friend]
         * Wraps the friended declaration so sema can track access grants. */
        struct {
            Node *decl;     /* the friended declaration */
        } friend_decl;

        /* ND_ACCESS_SPEC — N4659 §12.2 [class.access.spec]
         *   access-specifier : */
        struct {
            TokenKind access;   /* TK_KW_PUBLIC, TK_KW_PROTECTED, TK_KW_PRIVATE */
        } access_spec;

        /* ND_TEMPLATE_ID — N4659 §17.2 [temp.names]
         *   simple-template-id: template-name < template-argument-list(opt) >
         * Used in expressions and type-specifiers: vector<int>, pair<A,B> */
        struct {
            Token *name;        /* the template-name */
            Node **args;        /* template-argument-list */
            int nargs;
        } template_id;

        /* ND_TRANSLATION_UNIT — N4659 §A.5 (grammar) / §5.2/2
         * [lex.phases]:
         *   translation-unit: declaration-seq(opt)
         * C++20: extends with module-declaration, export-declaration */
        struct {
            Node **decls;
            int ndecls;
        } tu;
    };
};

/* ================================================================== */
/* Declaration specifier flags — N4659 §10.1 [dcl.spec]              */
/* C++20: adds consteval (§9.2.6), constinit (§9.2.7)               */
/* ================================================================== */

/*
 * Flags from the decl-specifier-seq that are NOT part of the type
 * but affect the declaration's storage, linkage, or semantics.
 * Parsed by parse_type_specifiers, stored on declaration nodes.
 */
enum {
    DECL_STATIC    = 1 << 0,  /* §10.1.1 [dcl.stc] */
    DECL_EXTERN    = 1 << 1,  /* §10.1.1 */
    DECL_INLINE    = 1 << 2,  /* §10.1.6 [dcl.inline] */
    DECL_CONSTEXPR = 1 << 3,  /* §10.1.5 [dcl.constexpr] */
    DECL_VIRTUAL   = 1 << 4,  /* N4659 §10.1.2 [dcl.fct.spec] (grammar);
                               *        §13.3   [class.virtual] (semantics) */
    DECL_EXPLICIT  = 1 << 5,  /* §10.1.1 [dcl.stc] — constructors */
    DECL_MUTABLE   = 1 << 6,  /* §10.1.1 [dcl.stc] — data members */
    DECL_REGISTER  = 1 << 7,  /* §10.1.1 [dcl.stc] — deprecated in C++17 */
};

/* ================================================================== */
/* Type System                                                        */
/* ================================================================== */

/*
 * N4659 §10.1.7 [dcl.type] — type-specifiers
 * N4659 §11.3 [dcl.meaning] — declarators
 *
 * Types are built in two phases:
 * 1. parse_type_specifiers() produces the base type from the
 *    decl-specifier-seq (e.g., 'unsigned long long' → TY_LLONG).
 * 2. parse_declarator() wraps the base with pointer, array, and
 *    function types from the declarator syntax.
 *
 * C++20 adds: constinit, consteval, concept auto, abbreviated templates.
 * C++23 adds: deducing this, static operator[].
 */
/* Forward-declared so Type can hold a DeclarativeRegion* for class
 * member lookup. The struct itself is defined further down. */
typedef struct DeclarativeRegion DeclarativeRegion;

typedef enum {
    /* Fundamental types — N4659 §6.9.1 [basic.fundamental]
     * C++20: adds char8_t (N4861 §7.3.1) */
    TY_VOID,
    TY_BOOL,            /* bool — N4659 §6.9.1/6 */
    TY_CHAR,            /* char, signed char, unsigned char */
    TY_CHAR16,          /* char16_t — N4659 §6.9.1/5 */
    TY_CHAR32,          /* char32_t — N4659 §6.9.1/5 */
    TY_WCHAR,           /* wchar_t — N4659 §6.9.1/5 */
    TY_SHORT,           /* short int */
    TY_INT,             /* int */
    TY_LONG,            /* long int */
    TY_LLONG,           /* long long int — guaranteed >= 64 bits */
    TY_FLOAT,           /* float */
    TY_DOUBLE,          /* double */
    TY_LDOUBLE,         /* long double */

    /* Compound types — N4659 §6.9.2 [basic.compound] */
    TY_PTR,             /* pointer — N4659 §11.3.1 [dcl.ptr] */
    TY_REF,             /* lvalue reference — N4659 §11.3.2 [dcl.ref] */
    TY_RVALREF,         /* rvalue reference — N4659 §11.3.2 [dcl.ref] (C++11) */
    TY_ARRAY,           /* array — N4659 §11.3.4 [dcl.array] */
    TY_FUNC,            /* function — N4659 §11.3.5 [dcl.fct] */

    /* Aggregate types (first pass: declaration only, no members) */
    TY_STRUCT,          /* struct — N4659 §12 [class] */
    TY_UNION,           /* union — N4659 §12.3 [class.union] */
    TY_ENUM,            /* enum — N4659 §10.2 [dcl.enum] */

    /* Template type parameter — placeholder for substitution during
     * template instantiation. Created when a template type parameter
     * (typename T / class T) is used as a type-specifier inside the
     * template body. The tag field carries the parameter name token.
     * During instantiation, subst_type replaces TY_DEPENDENT nodes
     * with concrete types from the substitution map. */
    TY_DEPENDENT,
} TypeKind;

struct Type {
    TypeKind kind;

    /* Qualifiers — N4659 §10.1.7.1 [dcl.type.cv] */
    bool is_unsigned;
    bool is_const;
    bool is_volatile;

    /* Pointed-to / element / referenced type. Used by:
     *   TY_PTR     — N4659 §11.3.1 [dcl.ptr]   (T*)
     *   TY_REF     — N4659 §11.3.2 [dcl.ref]   (T&)
     *   TY_RVALREF — N4659 §11.3.2 [dcl.ref]   (T&&) (C++11)
     *   TY_ARRAY   — N4659 §11.3.4 [dcl.array] (T[N]) */
    Type *base;

    /* TY_ARRAY: element count, -1 for unsized [] or expression-sized */
    int array_len;

    /* TY_ARRAY: original size expression when it isn't an integer
     * literal — e.g. '[N]', '[sizeof(x)]', '[f()]'. Codegen falls
     * back to emitting this when array_len < 0 so constant folding
     * and sizeof expressions survive into the C output. NULL when
     * the size was a literal (already captured in array_len) or
     * omitted ('[]'). N4659 §11.3.4/1 [dcl.array]: size is a
     * constant-expression, but we keep the full expr so the C
     * compiler re-evaluates it. */
    Node *array_size_expr;

    /* TY_FUNC — N4659 §11.3.5 [dcl.fct]
     * C++11: trailing return types (-> Type)
     * C++20: abbreviated function templates, consteval
     * C++23: deducing this (explicit object parameter) */
    Type *ret;
    Type **params;
    int nparams;
    bool is_variadic;   /* true if param list ends with ... */
    /* Per-parameter default values for default-argument injection.
     * NULL if the function has no defaults at all; otherwise, a
     * nparams-sized array where entry i is the ND_EXPR for param i's
     * default value (or NULL if that param has none). N4659 §11.3.6
     * [dcl.fct.default]. */
    Node **param_defaults;

    /* TY_STRUCT, TY_UNION, TY_ENUM: tag name (for 'struct Foo' usage)
     * TY_DEPENDENT: the template parameter name (e.g. 'T') or, for a
     *   qualified dependent name 'typename T::member', the leading
     *   template parameter ('T'); the nested member lives in
     *   dep_member below. */
    Token *tag;

    /* TY_DEPENDENT: nested-name suffix of a dependent qualified type.
     * For 'typename T::default_layout', tag is 'T' and dep_member is
     * 'default_layout'. During substitution, once T resolves to a
     * concrete type with a class_region, we look up dep_member in
     * that region to get the final concrete type. NULL for plain
     * dependent names ('T' alone).
     *
     * N4659 §13.8.3 [temp.dep.type] — dependent types produced by a
     * nested-name-specifier. N4659 §17.6/2 [temp.res]: 'typename'
     * disambiguates a dependent qualified name as naming a type. */
    Token *dep_member;

    /* TY_STRUCT, TY_UNION: the declarative region holding the members
     * of the class body (NULL for forward-declared / opaque types).
     * Set when the class definition '{ ... }' is parsed.
     * Used for qualified-name lookup: 'Foo::bar' resolves 'bar' in
     * Foo's class region (and walks its base-class chain). */
    DeclarativeRegion *class_region;

    /* TY_STRUCT, TY_UNION: true if the class needs a non-trivial
     * destructor. Set when the class body is parsed by scanning the
     * member list for a non-empty user-declared dtor; ALSO inherited
     * from any direct base whose has_dtor is true (so a derived
     * class with no own dtor still gets the wrapper synthesized to
     * chain into the base dtor — see emit_class_def). Codegen
     * consults this to decide whether to emit a sf__Class__dtor
     * call at end of scope. */
    bool has_dtor;

    /* TY_STRUCT, TY_UNION: true if the class has a user-declared
     * zero-argument constructor. Codegen consults this to decide
     * whether 'Foo a;' (default-init) should emit a sf__Foo__ctor(&a)
     * call. Three sources set it:
     *   - user-declared zero-arg ctor in the class body
     *   - synthesized: classes with non-trivially-default-
     *     constructible members and no user ctor
     *   - synthesized: polymorphic classes (need ctor to install
     *     vptr) and any derived class that inherits has_default_ctor
     *     from a direct base. */
    bool has_default_ctor;

    /* TY_STRUCT, TY_UNION: true if any member function (own or
     * inherited) is virtual. When set, codegen emits a per-class
     * vtable struct, places a vptr at offset 0 of the layout, and
     * dispatches matching method calls through it. Pay-only-when-
     * used: classes without virtual methods get no vptr. */
    bool has_virtual_methods;

    /* TY_STRUCT, TY_UNION: back-pointer to the ND_CLASS_DEF node
     * that defined this type. Set when the class body is parsed.
     * Codegen uses it to walk member declarations IN ORDER (the
     * hash-bucketed class_region doesn't preserve order) when
     * emitting out-of-class ctor/dtor definitions whose body needs
     * to chain into member ctors. NULL for forward-declared types. */
    Node *class_def;

    /* Template instantiation metadata.
     *
     * template_id_node: back-pointer to the ND_TEMPLATE_ID node that
     *   produced this type (e.g. when 'vec<int>' is parsed in type-
     *   specifier position). NULL for non-template types. The
     *   instantiation pass reads the template name and arguments from
     *   this node to decide what to instantiate.
     *
     * template_args / n_template_args: concrete type arguments for an
     *   instantiated template type. Set by the instantiation pass after
     *   cloning. The mangler reads these to produce distinct names for
     *   each instantiation (e.g. sf__vec_t_int_te_). */
    Node *template_id_node;
    Type **template_args;
    int    n_template_args;

    /* TY_ENUM: enumerator body tokens for C emission.
     * enum_tokens points to the first token INSIDE the braces,
     * enum_ntokens is the count (NOT including '}'). Set during
     * parsing; -1/NULL for forward-declared enums. */
    Token *enum_tokens;   /* first token of enum body */
    int    enum_ntokens;  /* number of tokens in body */

    /* Codegen flag: set when this struct's definition has been
     * emitted to prevent duplicate/out-of-order definitions. */
    bool codegen_emitted;

    /* Stable id for anonymous struct/union types (no tag). Lazily
     * assigned on first reference so the definition and every use
     * agree on the name. Zero means 'unassigned'. */
    int  anon_id;
};

/* ================================================================== */
/* Name Lookup — N4659 §6.3-§6.4 [basic.scope, basic.lookup]          */
/*               N4861 §6.4-§6.5 (C++20, renumbered)                  */
/*               N4950 §6.4-§6.5 (C++23, unchanged)                   */
/*                                                                    */
/* "The name lookup rules apply uniformly to all names (including     */
/*  typedef-names, namespace-names, and class-names) wherever the     */
/*  grammar allows such names." — N4659 §6.4/1                        */
/* ================================================================== */

/*
 * EntityKind — N4659 §6.1 [basic]
 * What kind of entity a declared name refers to.
 * The parser's disambiguation rules inspect this after lookup.
 */
typedef enum {
    ENTITY_VARIABLE,    /* object (§6.6.2 [basic.stc]) or function (§6.6.3)
                         * — "not a type" is all the disambiguation oracle cares about */
    ENTITY_TYPE,        /* type-name (§10.1.7.1 [dcl.type.simple]):
                         *   typedef-name (§10.1.3 [dcl.typedef])
                         * | class-name (§12.1 [class.name])
                         * | enum-name (§10.2 [dcl.enum])
                         * C++ class name injection (§6.3.10/2, §12.1/2):
                         *   'struct Foo {}' makes bare 'Foo' a type-name */
    ENTITY_TAG,         /* struct/union/enum tag reached via elaborated-type-specifier
                         * (§10.1.7.3 [dcl.type.elab])
                         * Separate from ENTITY_TYPE per §6.3.10/2: a variable
                         * can hide a class name, but 'struct Foo' still works */
    ENTITY_NAMESPACE,   /* namespace-name (§10.3.1 [namespace.def]) */
    ENTITY_TEMPLATE,    /* template-name (§17.1 [temp]) */
    ENTITY_ENUMERATOR,  /* enumerator (§10.2 [dcl.enum]) — a named constant */
} EntityKind;

/* Forward declaration — Declaration references DeclarativeRegion */
typedef struct DeclarativeRegion DeclarativeRegion;

/*
 * Declaration — N4659 §6.1 [basic]
 * "An entity is a value, object, reference, function, enumerator, type,
 *  class member, bit-field, template, template specialization, namespace,
 *  or parameter pack."
 *
 * A Declaration records that a name was introduced into a declarative region.
 * Arena-allocated, never freed individually.
 */
/* Declaration typedef forward-declared above (before Node). */
struct Declaration {
    const char  *name;      /* pointer into source buffer (Token.loc) — no copy */
    int          name_len;  /* byte length of the name */
    EntityKind   entity;    /* what kind of entity this name refers to */
    Type        *type;      /* associated type, or NULL */
    DeclarativeRegion *ns_region; /* ENTITY_NAMESPACE: the namespace's region.
                                  * Survives after the region is popped — used
                                  * by 'using namespace' to find the region. */
    DeclarativeRegion *home; /* the region this Declaration was registered in.
                              * Sema uses this to detect that an unqualified
                              * lookup landed on a class member (region kind
                              * REGION_CLASS) so 'x' inside a method body can
                              * be rewritten to 'this->x'. */
    Node        *tmpl_node; /* ENTITY_TEMPLATE: the ND_TEMPLATE_DECL node this
                             * declaration names. Used by overload resolution
                             * to reach the inner function decl for template
                             * argument deduction (N4659 §17.8.2). NULL on
                             * non-template entries. */
    Declaration *next;      /* hash chain within the declarative region */
};

/*
 * RegionKind — N4659 §6.3 [basic.scope]
 * The standard defines these kinds of declarative regions:
 */
typedef enum {
    REGION_BLOCK,       /* §6.3.3 [basic.scope.block] — compound-statement { ... } */
    REGION_PROTOTYPE,   /* §6.3.4 [basic.scope.proto] — function parameter names */
    REGION_NAMESPACE,   /* §6.3.6 [basic.scope.namespace] — namespace or global */
    REGION_CLASS,       /* §6.3.7 [basic.scope.class] — class body */
    REGION_ENUM,        /* §6.3.8 [basic.scope.enum] — scoped enum (declared
                         * but unused: enumerator scope is not yet implemented) */
    REGION_TEMPLATE,    /* §6.3.9 [basic.scope.temp] — template parameters */
} RegionKind;

/*
 * DeclarativeRegion — N4659 §6.3/1 [basic.scope.declarative]
 * "Every name is introduced in some portion of the program text
 *  called a declarative region, which is the largest part of the
 *  program in which that name is valid"
 *
 * Regions nest: each has an enclosing region (except the global namespace).
 * Arena-allocated with a fixed-size hash table for name declarations.
 */
#define REGION_HASH_SIZE 32

/* DeclarativeRegion typedef is forward-declared above (before Declaration) */
struct DeclarativeRegion {
    RegionKind      kind;
    DeclarativeRegion *enclosing;   /* §6.3/1: "declarative regions can nest" */
    Declaration    *buckets[REGION_HASH_SIZE]; /* hash table, separate chaining */

    /* Named region — for namespaces (§6.3.6) and classes (§6.3.7).
     * NULL for anonymous regions (blocks, prototypes). */
    Token          *name;

    /* using-directives — N4659 §10.3.4 [namespace.udir]
     *   "using namespace foo;" makes foo's declarations visible here.
     * N4659 §6.4.1/2: "declarations from the namespace nominated by a
     *   using-directive become visible in a namespace enclosing the
     *   using-directive."
     *
     * Arena-allocated array of pointers to nominated regions.
     * Scoping is automatic: when this region is popped, the using
     * list goes away with it. No explicit clearing needed. */
    DeclarativeRegion **using_regions;
    int                 nusing;
    int                 using_cap;

    /* Base-class regions (REGION_CLASS only) — N4659 §13 [class.derived].
     * For 'struct Derived : public Base1, private Base2 { ... }', this
     * holds the class regions of Base1 and Base2 in declaration order.
     * Lookup of an unqualified name in a derived-class scope walks
     * these after the class's own buckets (§6.4.2 [class.member.lookup]).
     * Arena-allocated, scoped with the region. */
    DeclarativeRegion **bases;
    int                 nbases;
    int                 bases_cap;

    /* For REGION_CLASS: back-pointer to the class Type that owns this
     * region. Lets sema/codegen recover the class name (via type->tag)
     * starting from a Declaration found inside the region — used to
     * mangle method calls 'doubled()' inside a method body to
     * 'sf__Box__doubled(this)' via mangle_class_method (see
     * src/codegen/mangle.c). */
    Type *owner_type;
};

/*
 * ParseState — save/restore for tentative parsing.
 * Token position is an index into the contiguous TokenArray.
 * Must include the declarative region since tentative parsing may
 * push/pop regions that need to be unwound on failure.
 */
typedef struct {
    int                pos;     /* index into TokenArray */
    DeclarativeRegion *region;
    int                template_depth;
    bool               split_shr;
} ParseState;

/* ================================================================== */
/* Parser State                                                       */
/* ================================================================== */

typedef struct Parser Parser;
struct Parser {
    Token *tokens;             /* contiguous token array (from TokenArray) */
    int    ntokens;            /* total token count (including EOF) */
    int    pos;                /* current position (index into tokens[]) */
    File *file;                /* source file */
    Arena *arena;              /* all AST/Type allocations come from here */
    CppStandard std;           /* C++17 baseline; 20/23 gated behind this flag */
    bool tentative;            /* when true, return NULL on error instead of aborting */
    bool tentative_failed;     /* set when a silenced error occurred during tentative parse */

    /* Side channel from parse_declarator → parse_declaration:
     * when the declarator-id is qualified ('void Foo::bar() { ... }'),
     * parse_declarator sets this to Foo's class_region. The function-def
     * branch in parse_declaration pushes it as an enclosing scope so
     * the method body can resolve Foo's members via lookup. Cleared
     * after each top-level declaration. */
    DeclarativeRegion *qualified_decl_scope;
    /* Side channel from parse_declarator → parse_declaration: when the
     * declarator-id is qualified via a template-id ('C<T,A,L>::f'),
     * this is the parsed ND_TEMPLATE_ID node. The function-def branch
     * copies it onto func.qual_tid so the template instantiation pass
     * can bind OOL methods to the matching specialization. Cleared
     * after each top-level declaration. */
    Node *qualified_decl_tid;
    /* Side channel from parse_declarator → parse_declaration: set true
     * when the declarator-id is '~Name' (destructor). Read by the
     * function-def branch and cleared after each declaration. */
    bool pending_is_destructor;
    /* Side channel from parse_type_specifiers → parse_declaration:
     * set true when we recognize a constructor pattern at class body
     * scope (class-name followed by '('). The class name is then
     * consumed as the declarator-id. */
    bool pending_is_constructor;
    DeclarativeRegion *region; /* current innermost declarative region (§6.3) */
    int template_depth;        /* nesting depth of template-argument-lists being parsed.
                                * When > 0, TK_SHR (>>) is treated as two '>' tokens
                                * (N4659 §17.2/3 [temp.names]). */
    bool split_shr;            /* true when a >> (TK_SHR) has been "split": the first >
                                * was consumed, the second > is virtual. parser_peek()/
                                * parser_at() return a synthetic TK_GT; parser_advance()
                                * clears the flag. N4659 §17.2/3 [temp.names].
                                *
                                * A bool suffices because the lexer's maximal munch
                                * produces >> as one token — at most one pending > results
                                * from a split. E.g., A<B<C<int>>> lexes as >> then >,
                                * so we only ever split one >> at a time.
                                *
                                * Note: >>= (TK_SHR_ASSIGN) inside template args
                                * (e.g., A<B<x>>=y) would need a different split
                                * (> + >=, or > + > + =). This is not yet handled — it
                                * would require a small pending-token queue instead of
                                * a boolean. Rare in practice (both GCC and Clang source
                                * avoid this pattern). */
};

/* ================================================================== */
/* Parser operations — parser.c                                       */
/* ================================================================== */

/* Token stream — index-based cursor into contiguous array */
Token *parser_peek(Parser *p);                 /* current token (no advance) */
Token *parser_peek_ahead(Parser *p, int n);    /* lookahead by n tokens */
Token *parser_advance(Parser *p);             /* return current, advance position */
bool   parser_at(Parser *p, TokenKind k);
bool   parser_consume(Parser *p, TokenKind k);
Token *parser_expect(Parser *p, TokenKind k);
bool   parser_at_eof(Parser *p);

/* Tentative parsing — save/restore parser state (position + region) */
ParseState parser_save(Parser *p);
void       parser_restore(Parser *p, ParseState saved);

/* GCC extension: __attribute__((...)) — skip any sequence of these.
 * Lexer treats __attribute__ as a plain identifier. */
void parser_skip_gnu_attributes(Parser *p);
void parser_skip_gnu_attributes_with_mode(Parser *p, Token **out_mode);
void parser_skip_cxx_attributes(Parser *p);

/* Node constructors (arena-allocated) */
Node *new_node(Parser *p, NodeKind kind, Token *tok);
Node *new_num_node(Parser *p, Token *tok);
Node *new_fnum_node(Parser *p, Token *tok);
Node *new_binary_node(Parser *p, TokenKind op, Node *lhs, Node *rhs, Token *tok);
Node *new_unary_node(Parser *p, TokenKind op, Node *operand, Token *tok);
Node *new_ternary_node(Parser *p, Node *cond, Node *then_, Node *else_, Token *tok);
Node *new_cast_node(Parser *p, Type *ty, Node *operand, Token *tok);
Node *new_call_node(Parser *p, Node *callee, Node **args, int nargs, Token *tok);
Node *new_subscript_node(Parser *p, Node *base, Node *index, Token *tok);
Node *new_member_node(Parser *p, Node *obj, Token *member, TokenKind op, Token *tok);
Node *new_qualified_node(Parser *p, Token **parts, int nparts, bool global_scope, Token *tok);
Node *new_block_node(Parser *p, Node **stmts, int nstmts, Token *tok);
Node *new_for_node(Parser *p, Node *init, Node *cond, Node *inc, Node *body, Token *tok);
Node *new_var_decl_node(Parser *p, Type *ty, Token *name, Token *tok);
Node *new_typedef_node(Parser *p, Type *ty, Token *name, Token *tok);
Node *new_param_node(Parser *p, Type *ty, Token *name, Token *tok);
Node *new_class_def_node(Parser *p, Token *tag, Node **members, int nmembers, Token *tok);
Node *new_template_decl_node(Parser *p, Node **params, int nparams, Node *decl, Token *tok);
Node *new_template_id_node(Parser *p, Token *name, Node **args, int nargs, Token *tok);

/* ================================================================== */
/* Expression parser — expr.c                                         */
/* ================================================================== */

/*
 * Expression grammar — N4659 §8 [expr]
 *
 * Implemented via precedence climbing. The call chain maps to
 * the grammar's precedence hierarchy:
 *
 *   expr()           → comma-expression      (§8.19)
 *   assign_expr()    → assignment-expression  (§8.18)
 *   ternary_expr()   → conditional-expression (§8.16)
 *   binary_expr()    → handles §8.5-§8.15 via precedence table
 *   unary_expr()     → unary-expression       (§8.3)
 *   postfix_expr()   → postfix-expression     (§8.2)
 *   primary_expr()   → primary-expression     (§8.1)
 *
 * C++20 changes: adds <=> (§8.10 [expr.spaceship]) between
 *   shift-expression and relational-expression; adds co_await
 *   to unary-expression; adds co_yield to assignment-expression;
 *   adds requires-expression to primary-expression.
 * C++23 changes: adds static operator() and operator[] (affects
 *   postfix and subscript, not precedence).
 */
Node *parse_expr(Parser *p);
Node *parse_assign_expr(Parser *p);

/* ================================================================== */
/* Statement parser — stmt.c                                          */
/* ================================================================== */

/*
 * Statement grammar — N4659 §9 [stmt.stmt]
 *
 *   statement:
 *       labeled-statement       (§9.1)
 *       expression-statement    (§9.2)
 *       compound-statement      (§9.3)
 *       selection-statement     (§9.4)
 *       iteration-statement     (§9.5)
 *       jump-statement          (§9.6)
 *       declaration-statement   (§9.7)
 *       // C++11: attribute-specifier-seq(opt) before each
 *       // C++17: init-statement in if/switch
 *       // C++20: co_return-statement (§9.6.3.1)
 *       // C++23: if consteval (§9.4.2), label refactoring
 *       //        (labels at end of compound-statement)
 */
Node *parse_stmt(Parser *p);
Node *parse_compound_stmt(Parser *p);

/* ================================================================== */
/* Declaration parser — decl.c                                        */
/* ================================================================== */

/*
 * Declaration grammar — N4659 §10 [dcl.dcl]
 *
 *   declaration:
 *       simple-declaration
 *       function-definition
 *       template-declaration           (§17.1)
 *       explicit-instantiation         (§17.7.2 — skip-parsed)
 *       explicit-specialization        (§17.7.3 — via template<>)
 *       linkage-specification          (extern "C")
 *       namespace-definition           (§10.3.1)
 *       using-declaration              (§10.3.3)
 *       using-directive                (§10.3.4)
 *       static_assert-declaration      (§10.1.4)
 *       // C++20: adds export-declaration, module-import-declaration,
 *       //        concept-definition
 *       // C++23: refactors into name-declaration / special-declaration
 *
 *   template-declaration:              (§17.1, Annex A.12)
 *       template < template-parameter-list > declaration
 *       // C++20: adds trailing requires-clause
 *
 * Stmt-vs-decl disambiguation (Rule 1, N4659 §9.8 [stmt.ambig]):
 *   "any statement that could be a declaration IS a declaration."
 */
Node *parse_declaration(Parser *p);
Node *parse_top_level_decl(Parser *p);

/*
 * Template declaration — N4659 §17.1 [temp] (Annex A.12 [gram.temp])
 *   template < template-parameter-list > declaration
 *
 * Parses the template parameter list, pushes a REGION_TEMPLATE scope
 * (§6.3.9 [basic.scope.temp]) for the template parameters, then
 * parses the inner declaration. Registers the declared name as
 * ENTITY_TEMPLATE in the enclosing scope.
 *
 * C++20: adds requires-clause after template-parameter-list
 * C++23: no changes
 */
Node *parse_template_declaration(Parser *p);

/*
 * parse_template_id — N4659 §17.2 [temp.names]
 *   simple-template-id:
 *       template-name < template-argument-list(opt) >
 *   template-argument-list:
 *       template-argument ...(opt)
 *       template-argument-list , template-argument ...(opt)
 *   template-argument:
 *       constant-expression | type-id | id-expression
 *
 * Called when '<' begins a template-argument-list. Two paths reach
 * this function:
 *   - Confirmed: lookup_is_template_name returned true for the
 *     leading name (the standard rule per §17.2/2).
 *   - Speculative: in declarator-id position, in qualified-name
 *     chains, and in a few other places where '<' after an IDENT
 *     is unambiguously template-args by grammar but our lookup
 *     can't see the name yet (inline-namespace members, deferred
 *     friend declarations, etc.). See parse_declarator and the
 *     'SHORTCUT' comments there.
 *
 * Handles >> splitting (§17.2/3): when inside template args
 * (template_depth > 0), TK_SHR is treated as two '>' tokens via
 * the parser_save / split_shr machinery in parser.c.
 */
Node *parse_template_id(Parser *p, Token *name);
void parse_deferred_func_body(Parser *p, Node *func);

/* ================================================================== */
/* Type construction — type.c                                         */
/* ================================================================== */

/*
 * Type specifier parsing — N4659 §10.1.7 [dcl.type]
 *
 *   type-specifier:
 *       simple-type-specifier     (§10.1.7.2)
 *       elaborated-type-specifier (§10.1.7.3)
 *       cv-qualifier              (§10.1.7.1)
 *
 *   simple-type-specifier:
 *       char | char16_t | char32_t | wchar_t | bool | short | int |
 *       long | signed | unsigned | float | double | void | auto |
 *       decltype-specifier
 *
 * The specifiers combine per §10.1.7.2/Table 10 to form a type.
 * E.g., 'unsigned long long int' → TY_LLONG + is_unsigned.
 *
 * C++20: adds char8_t, constinit, consteval
 * C++23: adds deducing this
 */
/*
 * Result of parsing a decl-specifier-seq (§10.1).
 * Bundles the type, optional class definition, and specifier flags.
 */
typedef struct {
    Type *type;         /* the parsed type (NULL on failure) */
    Node *class_def;    /* non-NULL if a class body was parsed */
    int   flags;        /* DECL_STATIC | DECL_EXTERN | ... */
} DeclSpec;

DeclSpec parse_type_specifiers(Parser *p);
void parse_absorb_trailing_cv(Parser *p, DeclSpec *spec);
Type *parse_type_name(Parser *p);

/*
 * Declarator parsing — N4659 §11.3 [dcl.meaning]
 *
 *   declarator:
 *       ptr-declarator
 *       noptr-declarator parameters-and-qualifiers trailing-return-type
 *
 *   ptr-declarator:
 *       noptr-declarator
 *       ptr-operator ptr-declarator
 *
 *   ptr-operator:
 *       * cv-qualifier-seq(opt)                          (pointer)
 *       & attribute-specifier-seq(opt)                    (lvalue reference)
 *       && attribute-specifier-seq(opt)                   (rvalue reference, C++11)
 *       nested-name-specifier * cv-qualifier-seq(opt)     (pointer-to-member)
 *
 *   noptr-declarator:
 *       declarator-id
 *       noptr-declarator parameters-and-qualifiers   (function)
 *       noptr-declarator [ constant-expression(opt) ] (array)
 *       ( ptr-declarator )                            (grouping)
 */
Node *parse_declarator(Parser *p, Type *base_ty);

/* Type construction helpers (arena-allocated) */
Type *new_type(Parser *p, TypeKind kind);
Type *new_ptr_type(Parser *p, Type *base);
Type *new_ref_type(Parser *p, Type *base);
Type *new_rvalref_type(Parser *p, Type *base);
Type *new_array_type(Parser *p, Type *base, int len);
Type *new_func_type(Parser *p, Type *ret, Type **params, int nparams, bool variadic);

/* Structural type equivalence — two Types match iff same kind + same
 * sub-structure. See type.c for the rules and caller list. Used by
 * codegen dedup and sema overload resolution. */
bool types_equivalent(Type *a, Type *b);

/* Check if current token starts a declaration (type-specifier keyword
 * or, with name lookup, a user-defined type-name) */
bool parser_at_type_specifier(Parser *p);

/* ================================================================== */
/* Name lookup — lookup.c                                             */
/* ================================================================== */

/*
 * N4659 §6.3 [basic.scope] — Declarative region management
 */
void region_push(Parser *p, RegionKind kind, Token *name);
void region_pop(Parser *p);

/*
 * N4659 §6.3.2 [basic.scope.pdecl] — Point of declaration
 * Introduce a name into the current declarative region.
 * No-op when p->tentative is true (speculative parse).
 */
Declaration *region_declare(Parser *p, const char *name, int name_len,
                            EntityKind entity, Type *type);

/* Declare a name in a specific region (not necessarily the current one).
 * Used for friend declarations that introduce names into the enclosing
 * namespace (N4659 §14.3/11). */
Declaration *region_declare_in(Parser *p, DeclarativeRegion *r,
                               const char *name, int name_len,
                               EntityKind entity, Type *type);

/*
 * N4659 §6.4 [basic.lookup] — Name lookup
 *
 * §6.4.1 [basic.lookup.unqual]: "the scopes are searched for a
 * declaration in the order listed ... name lookup ends as soon as
 * a declaration is found for the name."
 */
Declaration *lookup_unqualified(Parser *p, const char *name, int name_len);
Declaration *lookup_unqualified_from(DeclarativeRegion *start,
                                     const char *name, int name_len);

/* Collect ALL declarations reachable by unqualified lookup for the
 * given name, stopping at the innermost scope that contains any
 * match (per N4659 §6.4.1/1 — inner scopes hide outer). Writes up
 * to 'cap' pointers into 'out' and returns the count. If there are
 * more matches than cap, truncates silently (caller's responsibility
 * to pick a reasonable cap; 16 is plenty for gcc 4.8 overload sets).
 *
 * Used to build the overload set carried on ND_IDENT so per-call
 * resolution (N4659 §16.3 [over.match]) can pick among candidates
 * without the parser/sema having to collapse to a single winner. */
int lookup_overload_set_from(DeclarativeRegion *start,
                              const char *name, int name_len,
                              Declaration **out, int cap);

/* Look up by entity kind — needed for elaborated-type-specifier
 * (§10.1.7.3): 'struct Foo' must find ENTITY_TAG even if a variable
 * 'Foo' hides the class name (§6.3.10/2 [basic.scope.hiding]). */
Declaration *lookup_unqualified_kind(Parser *p, const char *name,
                                     int name_len, EntityKind kind);
Declaration *lookup_kind_from(DeclarativeRegion *start, const char *name,
                               int name_len, EntityKind kind);

/*
 * Disambiguation oracles — convenience wrappers around lookup.
 *
 * §10.1.7.1: type-name = class-name | enum-name | typedef-name
 * §17.1:     template-name = name of a template
 *
 * These are the "two semantic oracles" from doc/disambiguation-rules.md.
 */
bool lookup_is_type_name(Parser *p, Token *tok);
bool lookup_is_template_name(Parser *p, Token *tok);

/*
 * N4659 §10.3.4 [namespace.udir] — Using directives
 * Add a namespace's declarative region to the current region's
 * "also search" list. Lookup will search these after own declarations.
 */
void region_add_using(Parser *p, DeclarativeRegion *ns);
void region_add_base(Parser *p, DeclarativeRegion *base);
Declaration *lookup_in_scope(DeclarativeRegion *scope,
                             const char *name, int name_len);

/*
 * Find a named namespace region by name, searching outward.
 * Returns NULL if not found.
 */
DeclarativeRegion *region_find_namespace(Parser *p, const char *name, int name_len);

/*
 * Shared helpers for declaration + region building (no Parser needed).
 * Used by the template instantiation pass and codegen for building
 * class_regions and param_scopes on cloned AST nodes.
 */
uint32_t hash_name(const char *name, int len);
Declaration *region_declare_raw(DeclarativeRegion *r, Arena *arena,
                                const char *name, int name_len,
                                EntityKind entity, Type *type);
Declaration *region_lookup_own(DeclarativeRegion *r,
                                const char *name, int name_len);
void region_add_base_raw(DeclarativeRegion *r, DeclarativeRegion *base,
                          Arena *arena);
DeclarativeRegion *region_build_class(Node *class_def, Type *owner,
                                       Arena *arena);
DeclarativeRegion *region_build_prototype(Node *func,
                                           DeclarativeRegion *enclosing,
                                           Arena *arena);

/* ================================================================== */
/* AST dump — ast_dump.c                                              */
/* ================================================================== */

/* Print an S-expression representation of the AST for debugging.
 * Used by --dump-ast mode. */
/* dump_ast() is declared in sea-front.h (public API) */

#endif /* PARSE_H */
