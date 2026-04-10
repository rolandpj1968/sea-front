/*
 * sea-front — C++ to C transpiler for trusted bootstrapping
 * Single shared header.
 */

#ifndef SEA_FRONT_H
#define SEA_FRONT_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

/*
 * Token kinds.
 *
 * Every keyword uses TK_KW_ prefix to avoid collision with literal kinds
 * (e.g. TK_CHAR is a character literal, TK_KW_CHAR is the 'char' keyword).
 * Alternative representations (and, or, etc.) map to their operator tokens.
 * Identifiers with special meaning (override, final) remain TK_IDENT.
 */
typedef enum {
    /* Literals */
    TK_NUM,             /* integer literal */
    TK_FNUM,            /* floating-point literal */
    TK_STR,             /* string literal */
    TK_CHAR,            /* character literal */

    /* Identifier */
    TK_IDENT,

    /* Punctuators / Operators — single character */
    TK_LPAREN,          /* ( */
    TK_RPAREN,          /* ) */
    TK_LBRACE,          /* { or <% */
    TK_RBRACE,          /* } or %> */
    TK_LBRACKET,        /* [ or <: */
    TK_RBRACKET,        /* ] or :> */
    TK_SEMI,            /* ; */
    TK_COLON,           /* : */
    TK_COMMA,           /* , */
    TK_DOT,             /* . */
    TK_QUESTION,        /* ? */
    TK_TILDE,           /* ~ or compl */
    TK_EXCL,            /* ! or not */
    TK_PLUS,            /* + */
    TK_MINUS,           /* - */
    TK_STAR,            /* * */
    TK_SLASH,           /* / */
    TK_PERCENT,         /* % */
    TK_AMP,             /* & or bitand */
    TK_PIPE,            /* | or bitor */
    TK_CARET,           /* ^ or xor */
    TK_ASSIGN,          /* = */
    TK_LT,              /* < */
    TK_GT,              /* > */
    TK_HASH,            /* # */

    /* Punctuators / Operators — multi-character */
    TK_ARROW,           /* -> */
    TK_DOTSTAR,         /* .* */
    TK_ARROWSTAR,       /* ->* */
    TK_INC,             /* ++ */
    TK_DEC,             /* -- */
    TK_SHL,             /* << */
    TK_SHR,             /* >> */
    TK_SPACESHIP,       /* <=> (C++20, included for forward-compat) */
    TK_LE,              /* <= */
    TK_GE,              /* >= */
    TK_EQ,              /* == */
    TK_NE,              /* != or not_eq */
    TK_LAND,            /* && or and */
    TK_LOR,             /* || or or */
    TK_PLUS_ASSIGN,     /* += */
    TK_MINUS_ASSIGN,    /* -= */
    TK_STAR_ASSIGN,     /* *= */
    TK_SLASH_ASSIGN,    /* /= */
    TK_PERCENT_ASSIGN,  /* %= */
    TK_AMP_ASSIGN,      /* &= or and_eq */
    TK_PIPE_ASSIGN,     /* |= or or_eq */
    TK_CARET_ASSIGN,    /* ^= or xor_eq */
    TK_SHL_ASSIGN,      /* <<= */
    TK_SHR_ASSIGN,      /* >>= */
    TK_SCOPE,           /* :: */
    TK_ELLIPSIS,        /* ... */
    TK_HASHHASH,        /* ## */

    /* Keywords — alphabetical order matches Table 5 of N4659. The
     * range is contiguous (the enum values are sequential between
     * TK_KW_ALIGNAS and TK_KW_WHILE) so a range-check predicate
     * could be added if needed; none currently is. */
    TK_KW_ALIGNAS,
    TK_KW_ALIGNOF,
    TK_KW_ASM,
    TK_KW_AUTO,
    TK_KW_BOOL,
    TK_KW_BREAK,
    TK_KW_CASE,
    TK_KW_CATCH,
    TK_KW_CHAR,
    TK_KW_CHAR16_T,
    TK_KW_CHAR32_T,
    TK_KW_CLASS,
    TK_KW_CONST,
    TK_KW_CONSTEXPR,
    TK_KW_CONST_CAST,
    TK_KW_CONTINUE,
    TK_KW_DECLTYPE,
    TK_KW_DEFAULT,
    TK_KW_DELETE,
    TK_KW_DO,
    TK_KW_DOUBLE,
    TK_KW_DYNAMIC_CAST,
    TK_KW_ELSE,
    TK_KW_ENUM,
    TK_KW_EXPLICIT,
    TK_KW_EXPORT,
    TK_KW_EXTERN,
    TK_KW_FALSE,
    TK_KW_FLOAT,
    TK_KW_FOR,
    TK_KW_FRIEND,
    TK_KW_GOTO,
    TK_KW_IF,
    TK_KW_INLINE,
    TK_KW_INT,
    TK_KW_LONG,
    TK_KW_MUTABLE,
    TK_KW_NAMESPACE,
    TK_KW_NEW,
    TK_KW_NOEXCEPT,
    TK_KW_NULLPTR,
    TK_KW_OPERATOR,
    TK_KW_PRIVATE,
    TK_KW_PROTECTED,
    TK_KW_PUBLIC,
    TK_KW_REGISTER,
    TK_KW_REINTERPRET_CAST,
    TK_KW_RETURN,
    TK_KW_SHORT,
    TK_KW_SIGNED,
    TK_KW_SIZEOF,
    TK_KW_STATIC,
    TK_KW_STATIC_ASSERT,
    TK_KW_STATIC_CAST,
    TK_KW_STRUCT,
    TK_KW_SWITCH,
    TK_KW_TEMPLATE,
    TK_KW_THIS,
    TK_KW_THREAD_LOCAL,
    TK_KW_THROW,
    TK_KW_TRUE,
    TK_KW_TRY,
    TK_KW_TYPEDEF,
    TK_KW_TYPEID,
    TK_KW_TYPENAME,
    TK_KW_UNION,
    TK_KW_UNSIGNED,
    TK_KW_USING,
    TK_KW_VIRTUAL,
    TK_KW_VOID,
    TK_KW_VOLATILE,
    TK_KW_WCHAR_T,
    TK_KW_WHILE,

    TK_UNKNOWN,         /* unrecognized character — passed through for parser */

    TK_EOF,             /* end-of-file sentinel — always the last token in a
                         * TokenArray. This is a real token (not the absence of
                         * tokens): it has kind, loc, line, col like any other.
                         * Parser loops terminate because TK_EOF matches no
                         * operator, keyword, or type-specifier pattern, so every
                         * for(;;) loop that dispatches on token kind will break
                         * when it reaches EOF. */
} TokenKind;

/* Encoding prefix for string/char literals */
enum {
    ENC_NONE = 0,
    ENC_U8,         /* u8 */
    ENC_LITTLE_U,   /* u */
    ENC_BIG_U,      /* U */
    ENC_L,          /* L */
};

/* Source file context */
typedef struct File File;
struct File {
    char *name;         /* filename */
    char *contents;     /* full file contents, NUL-terminated */
    int size;           /* file size in bytes */
};

/* Token */
typedef struct Token Token;
struct Token {
    TokenKind kind;

    char *loc;          /* pointer into source buffer */
    int len;            /* byte length of token in source */

    int line;           /* 1-based line number */
    int col;            /* 1-based column (byte offset from line start) */
    File *file;

    /* Literal values */
    int64_t ival;       /* integer / character literal value */
    double fval;        /* floating-point literal value */
    /* Declared-but-unused: a planned 'decoded string contents'
     * field. The lexer doesn't currently decode escapes into a
     * separate buffer — every consumer reads loc/len directly and
     * decodes on demand. Kept here as a hook for the eventual
     * decode-once-cache; remove if no consumer materialises.
     * TODO(seafront#token-str-cache). */
    char *str;
    int str_len;

    int enc;            /* encoding prefix: ENC_NONE, ENC_U8, etc. */
    bool is_raw;        /* true for raw string literals */

    /* User-defined literal suffix */
    char *ud_suffix;    /* pointer into source, or NULL */
    int ud_suffix_len;

    bool at_bol;        /* at beginning of line */
    bool has_space;     /* preceded by whitespace */
};

/* Contiguous array of tokens — the output of the lexer.
 * Replaces the linked-list approach: better cache locality,
 * index-based save/restore for tentative parsing.
 *
 * Invariant: tokens[len-1].kind == TK_EOF. The EOF sentinel
 * is always present and is the last element. This guarantees
 * that parser loops scanning for specific token kinds will
 * terminate — TK_EOF matches no operator, keyword, or type
 * pattern, so any dispatch loop breaks when it reaches EOF. */
typedef struct {
    Token *tokens;      /* contiguous array, malloc'd */
    int len;            /* number of tokens (including final TK_EOF) */
} TokenArray;

/* Lexer context — threaded through helpers, no globals */
typedef struct {
    File *file;
    char *p;            /* current position in source */
    int line;
    int col;
    char *line_start;   /* pointer to start of current line (for col calc) */
} LexCtx;

/*
 * tokenize.c — Lexer
 */
TokenArray tokenize(File *file);
const char *token_kind_name(TokenKind kind);
bool token_equal(Token *tok, const char *s);

/*
 * unicode.c — UTF-8 and identifier classification
 */
int lex_decode_utf8(const char *p, uint32_t *codepoint);
bool lex_is_ident_start(uint32_t cp);
bool lex_is_ident_continue(uint32_t cp);

/*
 * util.c — Helpers
 */
File *sf_read_file(const char *path);
void *xmalloc(size_t size);
void *xcalloc(size_t nmemb, size_t size);
char *xstrdup(const char *s);
_Noreturn void error(const char *fmt, ...);
_Noreturn void error_at(const char *filename, const char *source,
                        char *loc, const char *fmt, ...);
_Noreturn void error_tok(Token *tok, const char *fmt, ...);

/*
 * arena.c — Arena allocator and growable array
 */
typedef struct ArenaPage ArenaPage;
struct ArenaPage {
    ArenaPage *next;
    size_t size;
    size_t used;
    char data[];        /* C99 flexible array member */
};

typedef struct {
    ArenaPage *cur;
    size_t page_size;
} Arena;

Arena arena_new(void);
void *arena_alloc(Arena *a, size_t size);
void  arena_free_all(Arena *a);

typedef struct {
    void **data;
    int len;
    int cap;
    Arena *arena;
} Vec;

Vec   vec_new(Arena *arena);
void  vec_push(Vec *v, void *item);
void *vec_get(Vec *v, int index);

/*
 * Forward declarations for parser types (full defs in src/parse/parse.h)
 */
typedef struct Node Node;
typedef struct Type Type;

/* C++ standard version flag */
typedef enum {
    CPP03,  /* C++03 (ISO 14882:2003) — gcc 4.8 bootstrap target */
    CPP11,  /* C++11 (ISO 14882:2011) — lambdas, auto, move, constexpr */
    CPP14,  /* C++14 (ISO 14882:2014) — modern gcc build requirement */
    CPP17,  /* C++17 (ISO 14882:2017) — LLVM/Clang build requirement */
    CPP20,  /* C++20 (ISO 14882:2020) — concepts, coroutines, modules */
    CPP23,  /* C++23 (ISO 14882:2023) — deducing this, if consteval */
} CppStandard;

/*
 * parse — public API
 */
Node *parse(TokenArray tokens, Arena *arena, CppStandard std);
void  dump_ast(Node *node, int indent);

#endif /* SEA_FRONT_H */
