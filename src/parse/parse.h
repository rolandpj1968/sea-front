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
/* AST Node Kinds                                                      */
/* ================================================================== */

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

    /* -- Top level -- */
    ND_TRANSLATION_UNIT, /* translation-unit — N4659 §6.1 [basic.link]
                          * C++20: adds module-declaration at TU level */
} NodeKind;

/* ================================================================== */
/* AST Node                                                            */
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
        } ident;

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

        /* ND_BLOCK — N4659 §9.3 [stmt.block]
         * compound-statement: { statement-seq(opt) } */
        struct {
            Node **stmts;
            int nstmts;
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
            Node *init;     /* initializer expression, or NULL */
        } var_decl;

        /* ND_FUNC_DEF — N4659 §11.4 [dcl.fct.def]
         * function-definition: decl-specifier-seq declarator function-body */
        struct {
            Type *ret_ty;
            Token *name;
            Node **params;  /* array of ND_PARAM nodes */
            int nparams;
            Node *body;     /* ND_BLOCK (compound-statement) */
        } func;

        /* ND_PARAM — N4659 §11.3.5 [dcl.fct]
         * parameter-declaration: decl-specifier-seq declarator */
        struct {
            Type *ty;
            Token *name;    /* may be NULL for unnamed params */
        } param;

        /* ND_TRANSLATION_UNIT — N4659 §6.1 [basic.link]
         * translation-unit: declaration-seq(opt)
         * C++20: extends with module-declaration, export-declaration */
        struct {
            Node **decls;
            int ndecls;
        } tu;
    };
};

/* ================================================================== */
/* Type System                                                         */
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
typedef enum {
    /* Fundamental types — N4659 §6.9.1 [basic.fundamental] */
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
    TY_ARRAY,           /* array — N4659 §11.3.4 [dcl.array] */
    TY_FUNC,            /* function — N4659 §11.3.5 [dcl.fct] */

    /* Aggregate types (first pass: declaration only, no members) */
    TY_STRUCT,          /* struct — N4659 §12 [class] */
    TY_UNION,           /* union — N4659 §12.3 [class.union] */
    TY_ENUM,            /* enum — N4659 §10.2 [dcl.enum] */
} TypeKind;

struct Type {
    TypeKind kind;

    /* Qualifiers — N4659 §10.1.7.1 [dcl.type.cv] */
    bool is_unsigned;
    bool is_const;
    bool is_volatile;

    /* TY_PTR: pointed-to type — N4659 §11.3.1 [dcl.ptr]
     * TY_ARRAY: element type — N4659 §11.3.4 [dcl.array]
     * Future: TY_REF, TY_RVALREF for & and && (N4659 §11.3.2 [dcl.ref]) */
    Type *base;

    /* TY_ARRAY: element count, -1 for unsized [] */
    int array_len;

    /* TY_FUNC — N4659 §11.3.5 [dcl.fct]
     * C++11: trailing return types (-> Type)
     * C++20: abbreviated function templates, consteval
     * C++23: deducing this (explicit object parameter) */
    Type *ret;
    Type **params;
    int nparams;
    bool is_variadic;   /* true if param list ends with ... */

    /* TY_STRUCT, TY_UNION, TY_ENUM: tag name (for 'struct Foo' usage) */
    Token *tag;
};

/* ================================================================== */
/* Parser State                                                        */
/* ================================================================== */

typedef struct Parser Parser;
struct Parser {
    Token *tok;         /* current token (cursor into linked list) */
    Token *prev;        /* previous token (for error messages) */
    File *file;         /* source file */
    Arena *arena;       /* all AST/Type allocations come from here */
    CppStandard std;    /* C++17 baseline; 20/23 gated behind this flag */
    bool tentative;     /* when true, return NULL on error instead of aborting */
};

/* ================================================================== */
/* Parser operations — parser.c                                        */
/* ================================================================== */

/* Token stream */
Token *peek(Parser *p);
Token *advance(Parser *p);
bool   at(Parser *p, TokenKind k);
bool   consume(Parser *p, TokenKind k);
Token *expect(Parser *p, TokenKind k);
bool   at_eof(Parser *p);

/* Tentative parsing — save/restore token position */
Token *parser_save(Parser *p);
void   parser_restore(Parser *p, Token *saved);

/* Node constructors (arena-allocated) */
Node *new_node(Parser *p, NodeKind kind, Token *tok);
Node *new_num_node(Parser *p, Token *tok);
Node *new_fnum_node(Parser *p, Token *tok);
Node *new_binary_node(Parser *p, TokenKind op, Node *lhs, Node *rhs, Token *tok);
Node *new_unary_node(Parser *p, TokenKind op, Node *operand, Token *tok);

/* ================================================================== */
/* Expression parser — expr.c                                          */
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
/* Statement parser — stmt.c                                           */
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
 *       // C++23: if consteval (§9.4.2)
 */
Node *parse_stmt(Parser *p);
Node *parse_compound_stmt(Parser *p);

/* ================================================================== */
/* Declaration parser — decl.c                                         */
/* ================================================================== */

/*
 * Declaration grammar — N4659 §10 [dcl.dcl]
 *
 *   declaration:
 *       simple-declaration
 *       function-definition
 *       // and many others (template, namespace, using, etc.)
 *
 *   simple-declaration:
 *       decl-specifier-seq init-declarator-list(opt) ;
 *       // C++17: adds structured bindings (§11.5)
 *       // C++20: adds concept-definition, module-decl, export-decl
 *
 * Stmt-vs-decl disambiguation (Rule 1, N4659 §9.8 [stmt.ambig]):
 *   "An expression-statement with a function-style explicit type
 *    conversion as its leftmost subexpression can be indistinguishable
 *    from a declaration ... The disambiguation is that any statement
 *    that could be a declaration IS a declaration."
 *
 * First pass: approximate the type-name oracle with built-in type
 * keywords. User-defined type names require the symbol table (Stage 2).
 */
Node *parse_declaration(Parser *p);
Node *parse_top_level_decl(Parser *p);

/* ================================================================== */
/* Type construction — type.c                                          */
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
Type *parse_type_specifiers(Parser *p);
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
 *       * cv-qualifier-seq(opt)
 *       & (deferred — references)
 *       && (deferred — rvalue references)
 *       nested-name-specifier * cv-qualifier-seq(opt) (deferred — ptr-to-member)
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
Type *new_array_type(Parser *p, Type *base, int len);
Type *new_func_type(Parser *p, Type *ret, Type **params, int nparams, bool variadic);

/* Check if current token starts a declaration (type-specifier keyword) */
bool at_type_specifier(Parser *p);

/* ================================================================== */
/* AST dump — ast_dump.c                                               */
/* ================================================================== */

/* Print an S-expression representation of the AST for debugging.
 * Used by --dump-ast mode. */
/* dump_ast() is declared in sea-front.h (public API) */

#endif /* PARSE_H */
