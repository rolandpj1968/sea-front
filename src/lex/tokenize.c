/*
 * tokenize.c — C++17 lexer core.
 *
 * Produces a singly-linked list of tokens from preprocessed C++ source.
 * Follows chibicc's structural patterns: pointer-into-source tokens,
 * dummy-head-node list construction, two-phase keyword conversion.
 */

#include "../sea-front.h"

/* ------------------------------------------------------------------ */
/* Token construction                                                  */
/* ------------------------------------------------------------------ */

static Token *new_token(TokenKind kind, char *start, int len, LexCtx *ctx) {
    Token *tok = xcalloc(1, sizeof(Token));
    tok->kind = kind;
    tok->loc = start;
    tok->len = len;
    tok->line = ctx->line;
    tok->col = (int)(start - ctx->line_start) + 1;
    tok->file = ctx->file;
    return tok;
}

/* ------------------------------------------------------------------ */
/* Whitespace and comment skipping                                     */
/* ------------------------------------------------------------------ */

static void skip_whitespace(LexCtx *ctx) {
    char *p = ctx->p;

    /* Terminates: each iteration either advances p toward the NUL
     * terminator or breaks. NUL matches none of the whitespace/comment
     * patterns, so the loop exits. */
    for (;;) {
        if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\f' || *p == '\v') {
            p++;
            continue;
        }

        if (*p == '\n') {
            p++;
            ctx->line++;
            ctx->line_start = p;
            continue;
        }

        /* Line comment */
        if (p[0] == '/' && p[1] == '/') {
            p += 2;
            while (*p && *p != '\n')
                p++;
            continue;
        }

        /* Block comment */
        if (p[0] == '/' && p[1] == '*') {
            char *start = p;
            p += 2;
            while (*p) {
                if (p[0] == '*' && p[1] == '/') {
                    p += 2;
                    goto done_block;
                }
                if (*p == '\n') {
                    ctx->line++;
                    ctx->line_start = p + 1;
                }
                p++;
            }
            error_at(ctx->file->name, ctx->file->contents, start,
                     "unterminated block comment");
        done_block:
            continue;
        }

        break;
    }

    ctx->p = p;
}

/* ------------------------------------------------------------------ */
/* Identifier and keyword support                                      */
/* ------------------------------------------------------------------ */

/*
 * Check if p points to the start of an identifier character (ASCII fast path
 * + UTF-8 for non-ASCII).  Returns the byte length consumed, or 0.
 *
 * '$' in identifiers is a GCC/Clang extension, not standard C++.
 * Both GCC and Clang accept it by default; GCC/Clang source may use it.
 */
static int ident_start_len(const char *p) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
        c == '$' /* GCC/Clang extension */)
        return 1;
    if (c >= 0x80) {
        uint32_t cp;
        int len = lex_decode_utf8(p, &cp);
        if (len > 0 && lex_is_ident_start(cp))
            return len;
    }
    return 0;
}

static int ident_continue_len(const char *p) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' ||
        c == '$' /* GCC/Clang extension */)
        return 1;
    if (c >= 0x80) {
        uint32_t cp;
        int len = lex_decode_utf8(p, &cp);
        if (len > 0 && lex_is_ident_continue(cp))
            return len;
    }
    return 0;
}

static Token *read_ident(LexCtx *ctx) {
    char *start = ctx->p;
    int n = ident_start_len(ctx->p);
    ctx->p += n;
    while ((n = ident_continue_len(ctx->p)) > 0)
        ctx->p += n;
    return new_token(TK_IDENT, start, (int)(ctx->p - start), ctx);
}

/* ------------------------------------------------------------------ */
/* Keyword conversion (two-phase, chibicc pattern)                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    TokenKind kind;
} Keyword;

/*
 * Keyword table — sorted by strcmp order for bsearch.
 * Includes both Table 5 keywords (TK_KW_*) and Table 6 alternative
 * representations (mapped to their operator token kinds).
 */
static const Keyword kw_table[] = {
    /* C11 alternate spellings (sort first because '_' < 'a' in ASCII). */
    {"_Bool",            TK_KW_BOOL},
    {"_Static_assert",   TK_KW_STATIC_ASSERT},
    {"alignas",          TK_KW_ALIGNAS},
    {"alignof",          TK_KW_ALIGNOF},
    {"and",              TK_LAND},
    {"and_eq",           TK_AMP_ASSIGN},
    {"asm",              TK_KW_ASM},
    {"auto",             TK_KW_AUTO},
    {"bitand",           TK_AMP},
    {"bitor",            TK_PIPE},
    {"bool",             TK_KW_BOOL},
    {"break",            TK_KW_BREAK},
    {"case",             TK_KW_CASE},
    {"catch",            TK_KW_CATCH},
    {"char",             TK_KW_CHAR},
    {"char16_t",         TK_KW_CHAR16_T},
    {"char32_t",         TK_KW_CHAR32_T},
    {"class",            TK_KW_CLASS},
    {"compl",            TK_TILDE},
    {"const",            TK_KW_CONST},
    {"const_cast",       TK_KW_CONST_CAST},
    {"constexpr",        TK_KW_CONSTEXPR},
    {"continue",         TK_KW_CONTINUE},
    {"decltype",         TK_KW_DECLTYPE},
    {"default",          TK_KW_DEFAULT},
    {"delete",           TK_KW_DELETE},
    {"do",               TK_KW_DO},
    {"double",           TK_KW_DOUBLE},
    {"dynamic_cast",     TK_KW_DYNAMIC_CAST},
    {"else",             TK_KW_ELSE},
    {"enum",             TK_KW_ENUM},
    {"explicit",         TK_KW_EXPLICIT},
    {"export",           TK_KW_EXPORT},
    {"extern",           TK_KW_EXTERN},
    {"false",            TK_KW_FALSE},
    {"float",            TK_KW_FLOAT},
    {"for",              TK_KW_FOR},
    {"friend",           TK_KW_FRIEND},
    {"goto",             TK_KW_GOTO},
    {"if",               TK_KW_IF},
    {"inline",           TK_KW_INLINE},
    {"int",              TK_KW_INT},
    {"long",             TK_KW_LONG},
    {"mutable",          TK_KW_MUTABLE},
    {"namespace",        TK_KW_NAMESPACE},
    {"new",              TK_KW_NEW},
    {"noexcept",         TK_KW_NOEXCEPT},
    {"not",              TK_EXCL},
    {"not_eq",           TK_NE},
    {"nullptr",          TK_KW_NULLPTR},
    {"operator",         TK_KW_OPERATOR},
    {"or",               TK_LOR},
    {"or_eq",            TK_PIPE_ASSIGN},
    {"private",          TK_KW_PRIVATE},
    {"protected",        TK_KW_PROTECTED},
    {"public",           TK_KW_PUBLIC},
    {"register",         TK_KW_REGISTER},
    {"reinterpret_cast", TK_KW_REINTERPRET_CAST},
    {"return",           TK_KW_RETURN},
    {"short",            TK_KW_SHORT},
    {"signed",           TK_KW_SIGNED},
    {"sizeof",           TK_KW_SIZEOF},
    {"static",           TK_KW_STATIC},
    {"static_assert",    TK_KW_STATIC_ASSERT},
    {"static_cast",      TK_KW_STATIC_CAST},
    {"struct",           TK_KW_STRUCT},
    {"switch",           TK_KW_SWITCH},
    {"template",         TK_KW_TEMPLATE},
    {"this",             TK_KW_THIS},
    {"thread_local",     TK_KW_THREAD_LOCAL},
    {"throw",            TK_KW_THROW},
    {"true",             TK_KW_TRUE},
    {"try",              TK_KW_TRY},
    {"typedef",          TK_KW_TYPEDEF},
    {"typeid",           TK_KW_TYPEID},
    {"typename",         TK_KW_TYPENAME},
    {"union",            TK_KW_UNION},
    {"unsigned",         TK_KW_UNSIGNED},
    {"using",            TK_KW_USING},
    {"virtual",          TK_KW_VIRTUAL},
    {"void",             TK_KW_VOID},
    {"volatile",         TK_KW_VOLATILE},
    {"wchar_t",          TK_KW_WCHAR_T},
    {"while",            TK_KW_WHILE},
    {"xor",              TK_CARET},
    {"xor_eq",           TK_CARET_ASSIGN},
};

#define KW_COUNT ((int)(sizeof(kw_table) / sizeof(kw_table[0])))

static int kw_cmp(const void *a, const void *b) {
    const Keyword *ka = a, *kb = b;
    return strcmp(ka->name, kb->name);
}

static TokenKind lookup_keyword(const char *name, int len) {
    /*
     * We need a NUL-terminated string for bsearch/strcmp.
     * Stack buffer is fine — C++ keywords max out at 16 chars.
     */
    char buf[32];
    if (len >= (int)sizeof(buf))
        return TK_IDENT;
    memcpy(buf, name, len);
    buf[len] = '\0';

    Keyword key = { buf, 0 };
    Keyword *found = bsearch(&key, kw_table, KW_COUNT, sizeof(Keyword), kw_cmp);
    return found ? found->kind : TK_IDENT;
}

/* ------------------------------------------------------------------ */
/* Punctuator / operator reading                                       */
/* ------------------------------------------------------------------ */

static Token *read_punct(LexCtx *ctx) {
    char *p = ctx->p;

    /* 3-character operators */
    if (p[0] == '<' && p[1] == '=' && p[2] == '>') {
        ctx->p += 3;
        return new_token(TK_SPACESHIP, p, 3, ctx);
    }
    if (p[0] == '<' && p[1] == '<' && p[2] == '=') {
        ctx->p += 3;
        return new_token(TK_SHL_ASSIGN, p, 3, ctx);
    }
    if (p[0] == '>' && p[1] == '>' && p[2] == '=') {
        ctx->p += 3;
        return new_token(TK_SHR_ASSIGN, p, 3, ctx);
    }
    if (p[0] == '-' && p[1] == '>' && p[2] == '*') {
        ctx->p += 3;
        return new_token(TK_ARROWSTAR, p, 3, ctx);
    }
    if (p[0] == '.' && p[1] == '.' && p[2] == '.') {
        ctx->p += 3;
        return new_token(TK_ELLIPSIS, p, 3, ctx);
    }

    /* Digraph special case: <:: (§5.4/3) */
    if (p[0] == '<' && p[1] == ':') {
        if (p[2] == ':' && p[3] != ':' && p[3] != '>') {
            /* Exception: <:: not followed by : or > — emit < only */
            ctx->p += 1;
            return new_token(TK_LT, p, 1, ctx);
        }
        ctx->p += 2;
        return new_token(TK_LBRACKET, p, 2, ctx);
    }

    /* 2-character operators */
    struct { char c0, c1; TokenKind kind; } ops2[] = {
        {'-', '>', TK_ARROW},
        {'.', '*', TK_DOTSTAR},
        {'+', '+', TK_INC},
        {'-', '-', TK_DEC},
        {'<', '<', TK_SHL},
        {'>', '>', TK_SHR},
        {'<', '=', TK_LE},
        {'>', '=', TK_GE},
        {'=', '=', TK_EQ},
        {'!', '=', TK_NE},
        {'&', '&', TK_LAND},
        {'|', '|', TK_LOR},
        {'+', '=', TK_PLUS_ASSIGN},
        {'-', '=', TK_MINUS_ASSIGN},
        {'*', '=', TK_STAR_ASSIGN},
        {'/', '=', TK_SLASH_ASSIGN},
        {'%', '=', TK_PERCENT_ASSIGN},
        {'&', '=', TK_AMP_ASSIGN},
        {'|', '=', TK_PIPE_ASSIGN},
        {'^', '=', TK_CARET_ASSIGN},
        {':', ':', TK_SCOPE},
        {'#', '#', TK_HASHHASH},
        {':', '>', TK_RBRACKET},   /* digraph :> = ] */
        {'<', '%', TK_LBRACE},    /* digraph <% = { */
        {'%', '>', TK_RBRACE},    /* digraph %> = } */
    };
    for (int i = 0; i < (int)(sizeof(ops2) / sizeof(ops2[0])); i++) {
        if (p[0] == ops2[i].c0 && p[1] == ops2[i].c1) {
            ctx->p += 2;
            return new_token(ops2[i].kind, p, 2, ctx);
        }
    }

    /* Single character operators */
    TokenKind single = 0;
    switch (*p) {
    case '(': single = TK_LPAREN; break;
    case ')': single = TK_RPAREN; break;
    case '{': single = TK_LBRACE; break;
    case '}': single = TK_RBRACE; break;
    case '[': single = TK_LBRACKET; break;
    case ']': single = TK_RBRACKET; break;
    case ';': single = TK_SEMI; break;
    case ':': single = TK_COLON; break;
    case ',': single = TK_COMMA; break;
    case '.': single = TK_DOT; break;
    case '?': single = TK_QUESTION; break;
    case '~': single = TK_TILDE; break;
    case '!': single = TK_EXCL; break;
    case '+': single = TK_PLUS; break;
    case '-': single = TK_MINUS; break;
    case '*': single = TK_STAR; break;
    case '/': single = TK_SLASH; break;
    case '%': single = TK_PERCENT; break;
    case '&': single = TK_AMP; break;
    case '|': single = TK_PIPE; break;
    case '^': single = TK_CARET; break;
    case '=': single = TK_ASSIGN; break;
    case '<': single = TK_LT; break;
    case '>': single = TK_GT; break;
    case '#': single = TK_HASH; break;
    }

    if (single) {
        ctx->p += 1;
        return new_token(single, p, 1, ctx);
    }

    /*
     * Unknown character — emit as TK_UNKNOWN rather than fatally erroring.
     * The parser or semantic layer can reject it. This follows GCC/Clang
     * behaviour: both accept stray characters (e.g. backtick) without a
     * lexer-level fatal error, deferring diagnosis to later phases.
     */
    ctx->p += 1;
    return new_token(TK_UNKNOWN, p, 1, ctx);
}

/* ------------------------------------------------------------------ */
/* Numeric literals                                                    */
/* ------------------------------------------------------------------ */

static bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

static bool is_bin_digit(char c) {
    return c == '0' || c == '1';
}

/*
 * Consume digits for the given base, including digit separators.
 * A digit separator (') is consumed only when followed by a valid digit.
 */
static void consume_digits(LexCtx *ctx, bool (*is_digit)(char)) {
    while (is_digit(*ctx->p) ||
           (*ctx->p == '\'' && is_digit(ctx->p[1]))) {
        ctx->p++;
    }
}

static void consume_decimal_digits(LexCtx *ctx) {
    while ((*ctx->p >= '0' && *ctx->p <= '9') ||
           (*ctx->p == '\'' && ctx->p[1] >= '0' && ctx->p[1] <= '9')) {
        ctx->p++;
    }
}

/*
 * Read a numeric literal.  Handles:
 *   - decimal, octal, hex, binary integers
 *   - digit separators (')
 *   - decimal and hex floating point
 *   - integer and float suffixes
 *   - user-defined literal suffixes
 *
 * Does NOT compute the numeric value — that is deferred to a conversion
 * function that strips separators and calls strtol/strtod.
 */
static Token *read_number(LexCtx *ctx) {
    char *start = ctx->p;
    bool is_float = false;

    if (*ctx->p == '0' && (ctx->p[1] == 'x' || ctx->p[1] == 'X')) {
        /* Hex literal */
        ctx->p += 2;
        if (!is_hex_digit(*ctx->p))
            error_at(ctx->file->name, ctx->file->contents, start,
                     "expected hex digit after '0x'");
        consume_digits(ctx, is_hex_digit);

        /* Hex float: must have . or p/P exponent */
        if (*ctx->p == '.') {
            is_float = true;
            ctx->p++;
            consume_digits(ctx, is_hex_digit);
        }
        if (*ctx->p == 'p' || *ctx->p == 'P') {
            is_float = true;
            ctx->p++;
            if (*ctx->p == '+' || *ctx->p == '-')
                ctx->p++;
            if (!(*ctx->p >= '0' && *ctx->p <= '9'))
                error_at(ctx->file->name, ctx->file->contents, ctx->p,
                         "expected digit in hex float exponent");
            consume_decimal_digits(ctx);
        }
    } else if (*ctx->p == '0' && (ctx->p[1] == 'b' || ctx->p[1] == 'B')) {
        /* Binary literal (C++14) */
        ctx->p += 2;
        if (!is_bin_digit(*ctx->p))
            error_at(ctx->file->name, ctx->file->contents, start,
                     "expected binary digit after '0b'");
        consume_digits(ctx, is_bin_digit);
    } else if (*ctx->p == '.' ||
               (*ctx->p >= '0' && *ctx->p <= '9')) {
        /* Decimal or octal integer, or decimal float */
        bool starts_with_dot = (*ctx->p == '.');
        if (starts_with_dot) {
            is_float = true;
            ctx->p++;
        }
        consume_decimal_digits(ctx);

        if (!starts_with_dot && *ctx->p == '.') {
            /* Could be float or member access. Float if followed by digit,
             * e, E, or another dot (which would be an error caught later). */
            if (ctx->p[1] >= '0' && ctx->p[1] <= '9') {
                is_float = true;
                ctx->p++;
                consume_decimal_digits(ctx);
            } else if (ctx->p[1] == 'e' || ctx->p[1] == 'E' ||
                       ctx->p[1] == 'f' || ctx->p[1] == 'F' ||
                       ctx->p[1] == 'l' || ctx->p[1] == 'L') {
                /* 1.e5, 1.f, 1.L */
                is_float = true;
                ctx->p++;
            } else if (ctx->p[1] != '.') {
                /* 1. with no following digit/exponent — still a float */
                is_float = true;
                ctx->p++;
            }
            /* If ctx->p[1] == '.', don't consume — could be 1..2 range (error)
             * or some other context. Let the parser handle it. */
        }

        /* Exponent */
        if (*ctx->p == 'e' || *ctx->p == 'E') {
            is_float = true;
            ctx->p++;
            if (*ctx->p == '+' || *ctx->p == '-')
                ctx->p++;
            if (!(*ctx->p >= '0' && *ctx->p <= '9'))
                error_at(ctx->file->name, ctx->file->contents, ctx->p,
                         "expected digit in exponent");
            consume_decimal_digits(ctx);
        }
    }

    /* Float suffix: f F l L */
    if (is_float) {
        if (*ctx->p == 'f' || *ctx->p == 'F' ||
            *ctx->p == 'l' || *ctx->p == 'L')
            ctx->p++;
    } else {
        /* Integer suffix: u/U, l/L, ll/LL, and combinations.
         * Terminates: at most 3 iterations (u + ll), then breaks.
         * Each branch sets a flag preventing re-entry. */
        bool had_u = false, had_l = false;
        for (;;) {
            if (!had_u && (*ctx->p == 'u' || *ctx->p == 'U')) {
                had_u = true;
                ctx->p++;
            } else if (!had_l && (*ctx->p == 'l' || *ctx->p == 'L')) {
                had_l = true;
                ctx->p++;
                if (*ctx->p == ctx->p[-1])  /* ll or LL */
                    ctx->p++;
            } else {
                break;
            }
        }
    }

    /* User-defined literal suffix */
    Token *tok = new_token(is_float ? TK_FNUM : TK_NUM, start,
                           (int)(ctx->p - start), ctx);

    int n = ident_start_len(ctx->p);
    if (n > 0) {
        tok->ud_suffix = ctx->p;
        ctx->p += n;
        while ((n = ident_continue_len(ctx->p)) > 0)
            ctx->p += n;
        tok->ud_suffix_len = (int)(ctx->p - tok->ud_suffix);
        tok->len = (int)(ctx->p - start);
    }

    /* Compute numeric value */
    if (!tok->ud_suffix) {
        /* Strip digit separators into a temp buffer */
        int raw_len = (int)(ctx->p - start);
        char *buf = xmalloc(raw_len + 1);
        int j = 0;
        for (int i = 0; i < raw_len; i++) {
            if (start[i] != '\'')
                buf[j++] = start[i];
        }
        buf[j] = '\0';

        char *end;
        if (is_float) {
            tok->fval = strtod(buf, &end);
        } else if (buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) {
            /* C11 strtoull doesn't handle 0b prefix — parse manually */
            tok->ival = (int64_t)strtoull(buf + 2, &end, 2);
        } else {
            tok->ival = (int64_t)strtoull(buf, &end, 0);
        }
        free(buf);
    }

    return tok;
}

/* ------------------------------------------------------------------ */
/* String and character literal prefix detection                       */
/* ------------------------------------------------------------------ */

/*
 * Check if p starts a string or character literal (including encoding prefix).
 * Returns:
 *   0 — not a string/char literal
 *   positive — number of bytes in the prefix (before the quote)
 * Sets *enc, *is_raw, *quote.
 */
static int string_prefix_len(const char *p, int *enc, bool *is_raw, char *quote) {
    *enc = ENC_NONE;
    *is_raw = false;
    *quote = '\0';

    /* u8R", u8", u8' */
    if (p[0] == 'u' && p[1] == '8') {
        if (p[2] == 'R' && p[3] == '"') {
            *enc = ENC_U8; *is_raw = true; *quote = '"';
            return 3;
        }
        if (p[2] == '"') { *enc = ENC_U8; *quote = '"'; return 2; }
        if (p[2] == '\'') { *enc = ENC_U8; *quote = '\''; return 2; }
        return 0;
    }

    /* LR", uR", UR" */
    if ((p[0] == 'L' || p[0] == 'u' || p[0] == 'U') && p[1] == 'R' && p[2] == '"') {
        if (p[0] == 'L') *enc = ENC_L;
        else if (p[0] == 'u') *enc = ENC_LITTLE_U;
        else *enc = ENC_BIG_U;
        *is_raw = true; *quote = '"';
        return 2;
    }

    /* R" */
    if (p[0] == 'R' && p[1] == '"') {
        *is_raw = true; *quote = '"';
        return 1;
    }

    /* L", L', u", u', U", U' */
    if (p[0] == 'L' || p[0] == 'u' || p[0] == 'U') {
        if (p[1] == '"' || p[1] == '\'') {
            if (p[0] == 'L') *enc = ENC_L;
            else if (p[0] == 'u') *enc = ENC_LITTLE_U;
            else *enc = ENC_BIG_U;
            *quote = p[1];
            return 1;
        }
        return 0;
    }

    /* Plain " or ' */
    if (p[0] == '"') { *quote = '"'; return 0; }
    if (p[0] == '\'') { *quote = '\''; return 0; }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Escape sequence reading                                             */
/* ------------------------------------------------------------------ */

/*
 * Advance past an escape sequence starting at p (which points at the \).
 * Returns a pointer past the escape. Does not interpret the value —
 * that is the semantic layer's concern.
 */
static char *skip_escape(char *p, LexCtx *ctx) {
    if (*p == '\0')
        return p;  /* don't advance past NUL — caller handles the error */
    if (*p != '\\')
        return p + 1;
    p++; /* skip backslash */
    if (*p == '\0')
        return p;  /* backslash at end of input — caller handles */

    switch (*p) {
    case '\'': case '"': case '?': case '\\':
    case 'a': case 'b': case 'f': case 'n':
    case 'r': case 't': case 'v':
        return p + 1;

    case '0': case '1': case '2': case '3':
    case '4': case '5': case '6': case '7':
        /* Octal: up to 3 digits */
        p++;
        if (*p >= '0' && *p <= '7') p++;
        if (*p >= '0' && *p <= '7') p++;
        return p;

    case 'x':
        /* Hex: \xNN or \x{NN} (C++23 delimited) */
        p++;
        if (*p == '{') {
            p++;
            while (is_hex_digit(*p)) p++;
            if (*p == '}') p++;
            return p;
        }
        if (!is_hex_digit(*p))
            error_at(ctx->file->name, ctx->file->contents, p - 2,
                     "expected hex digit in \\x escape");
        while (is_hex_digit(*p)) p++;
        return p;

    case 'u': {
        /* \uXXXX or \u{XXXX} (C++23 delimited) */
        p++;
        if (*p == '{') {
            p++;
            while (is_hex_digit(*p)) p++;
            if (*p == '}') p++;
            return p;
        }
        for (int i = 0; i < 4; i++) {
            if (!is_hex_digit(*p))
                error_at(ctx->file->name, ctx->file->contents, p,
                         "expected 4 hex digits in \\u escape");
            p++;
        }
        return p;
    }

    case 'U': {
        /* \UXXXXXXXX or \U{XXXXXXXX} (C++23 delimited) */
        p++;
        if (*p == '{') {
            p++;
            while (is_hex_digit(*p)) p++;
            if (*p == '}') p++;
            return p;
        }
        for (int i = 0; i < 8; i++) {
            if (!is_hex_digit(*p))
                error_at(ctx->file->name, ctx->file->contents, p,
                         "expected 8 hex digits in \\U escape");
            p++;
        }
        return p;
    }

    case 'o':
        /* \o{NNN} — C++23 delimited octal escape */
        p++;
        if (*p == '{') {
            p++;
            while (*p >= '0' && *p <= '7') p++;
            if (*p == '}') p++;
        }
        return p;

    case 'N':
        /* \N{name} — C++23 named escape */
        p++;
        if (*p == '{') {
            p++;
            while (*p && *p != '}') p++;
            if (*p == '}') p++;
        }
        return p;

    default:
        /* Unknown escape — let the semantic layer diagnose */
        return *p ? p + 1 : p;  /* don't advance past NUL */
    }
}

/* ------------------------------------------------------------------ */
/* String literals                                                     */
/* ------------------------------------------------------------------ */

static Token *read_string_literal(LexCtx *ctx, int prefix_len, int enc) {
    char *start = ctx->p;
    ctx->p += prefix_len;  /* skip encoding prefix */
    ctx->p++;              /* skip opening " */

    /* Scan to closing quote, handling escapes */
    while (*ctx->p != '"') {
        if (*ctx->p == '\0' || *ctx->p == '\n')
            error_at(ctx->file->name, ctx->file->contents, start,
                     "unterminated string literal");
        ctx->p = skip_escape(ctx->p, ctx);
    }
    ctx->p++;  /* skip closing " */

    /* Check for UDL suffix */
    Token *tok = new_token(TK_STR, start, (int)(ctx->p - start), ctx);
    tok->enc = enc;

    int n = ident_start_len(ctx->p);
    if (n > 0) {
        tok->ud_suffix = ctx->p;
        ctx->p += n;
        while ((n = ident_continue_len(ctx->p)) > 0)
            ctx->p += n;
        tok->ud_suffix_len = (int)(ctx->p - tok->ud_suffix);
        tok->len = (int)(ctx->p - start);
    }

    return tok;
}

/* ------------------------------------------------------------------ */
/* Raw string literals                                                 */
/* ------------------------------------------------------------------ */

static bool is_raw_dchar(char c) {
    /* d-char is any basic-source-char except space, (, ), \, \t, \v, \f, \n */
    if (c == ' ' || c == '(' || c == ')' || c == '\\' ||
        c == '\t' || c == '\v' || c == '\f' || c == '\n' || c == '\0')
        return false;
    return true;
}

static Token *read_raw_string_literal(LexCtx *ctx, int prefix_len, int enc) {
    char *start = ctx->p;
    int start_line = ctx->line;
    char *start_line_start = ctx->line_start;

    ctx->p += prefix_len;  /* skip encoding prefix (includes R) */
    ctx->p++;              /* skip " */

    /* Extract delimiter */
    char *delim_start = ctx->p;
    while (is_raw_dchar(*ctx->p))
        ctx->p++;

    if (*ctx->p != '(')
        error_at(ctx->file->name, ctx->file->contents, start,
                 "expected '(' in raw string delimiter");

    int delim_len = (int)(ctx->p - delim_start);
    if (delim_len > 16)
        error_at(ctx->file->name, ctx->file->contents, delim_start,
                 "raw string delimiter too long (max 16 characters)");

    ctx->p++;  /* skip ( */

    /* Scan for )delim".
     * Terminates: each iteration advances ctx->p by one byte.
     * NUL terminator causes error_at() which exits the program. */
    for (;;) {
        if (*ctx->p == '\0')
            error_at(ctx->file->name, ctx->file->contents, start,
                     "unterminated raw string literal");

        if (*ctx->p == '\n') {
            ctx->line++;
            ctx->line_start = ctx->p + 1;
        }

        if (*ctx->p == ')' &&
            (delim_len == 0 || memcmp(ctx->p + 1, delim_start, delim_len) == 0) &&
            ctx->p[1 + delim_len] == '"') {
            /* Safe: sf_read_file() allocates 32 bytes of NUL padding past EOF,
             * which covers the max delimiter length (16) + 1 for '"'. */
            ctx->p += 1 + delim_len + 1;  /* skip )delim" */
            break;
        }

        ctx->p++;
    }

    /* Use saved start position for token location */
    Token *tok = xcalloc(1, sizeof(Token));
    tok->kind = TK_STR;
    tok->loc = start;
    tok->len = (int)(ctx->p - start);
    tok->line = start_line;
    tok->col = (int)(start - start_line_start) + 1;
    tok->file = ctx->file;
    tok->enc = enc;
    tok->is_raw = true;

    /* UDL suffix */
    int n = ident_start_len(ctx->p);
    if (n > 0) {
        tok->ud_suffix = ctx->p;
        ctx->p += n;
        while ((n = ident_continue_len(ctx->p)) > 0)
            ctx->p += n;
        tok->ud_suffix_len = (int)(ctx->p - tok->ud_suffix);
        tok->len = (int)(ctx->p - start);
    }

    return tok;
}

/* ------------------------------------------------------------------ */
/* Character literals                                                  */
/* ------------------------------------------------------------------ */

static Token *read_char_literal(LexCtx *ctx, int prefix_len, int enc) {
    char *start = ctx->p;
    ctx->p += prefix_len;  /* skip encoding prefix */
    ctx->p++;              /* skip opening ' */

    if (*ctx->p == '\'')
        error_at(ctx->file->name, ctx->file->contents, start,
                 "empty character literal");

    while (*ctx->p != '\'') {
        if (*ctx->p == '\0' || *ctx->p == '\n')
            error_at(ctx->file->name, ctx->file->contents, start,
                     "unterminated character literal");
        ctx->p = skip_escape(ctx->p, ctx);
    }
    ctx->p++;  /* skip closing ' */

    Token *tok = new_token(TK_CHAR, start, (int)(ctx->p - start), ctx);
    tok->enc = enc;

    /* Compute value for simple single-byte chars */
    char *content = start + prefix_len + 1;  /* past prefix and opening ' */
    int content_len = (int)(ctx->p - 1 - content);
    if (content_len == 1 && content[0] != '\\') {
        tok->ival = (unsigned char)content[0];
    } else if (content_len >= 2 && content[0] == '\\') {
        switch (content[1]) {
        case '\'': tok->ival = '\''; break;
        case '"':  tok->ival = '"'; break;
        case '?':  tok->ival = '?'; break;
        case '\\': tok->ival = '\\'; break;
        case 'a':  tok->ival = '\a'; break;
        case 'b':  tok->ival = '\b'; break;
        case 'f':  tok->ival = '\f'; break;
        case 'n':  tok->ival = '\n'; break;
        case 'r':  tok->ival = '\r'; break;
        case 't':  tok->ival = '\t'; break;
        case 'v':  tok->ival = '\v'; break;
        case '0': case '1': case '2': case '3':
        case '4': case '5': case '6': case '7': {
            int64_t val = content[1] - '0';
            int i = 2;
            if (i < content_len && content[i] >= '0' && content[i] <= '7')
                val = val * 8 + (content[i++] - '0');
            if (i < content_len && content[i] >= '0' && content[i] <= '7')
                val = val * 8 + (content[i++] - '0');
            tok->ival = val;
            break;
        }
        case 'x': {
            int64_t val = 0;
            for (int i = 2; i < content_len && is_hex_digit(content[i]); i++) {
                char c = content[i];
                int d = (c >= '0' && c <= '9') ? c - '0' :
                        (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                        c - 'A' + 10;
                val = val * 16 + d;
            }
            tok->ival = val;
            break;
        }
        default:
            /* Multi-char or complex escape — leave ival as 0,
             * semantic layer handles it */
            break;
        }
    }

    /* UDL suffix */
    int n = ident_start_len(ctx->p);
    if (n > 0) {
        tok->ud_suffix = ctx->p;
        ctx->p += n;
        while ((n = ident_continue_len(ctx->p)) > 0)
            ctx->p += n;
        tok->ud_suffix_len = (int)(ctx->p - tok->ud_suffix);
        tok->len = (int)(ctx->p - start);
    }

    return tok;
}

/* ------------------------------------------------------------------ */
/* Token kind names (for --dump-tokens)                                */
/* ------------------------------------------------------------------ */

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
    case TK_NUM:            return "NUM";
    case TK_FNUM:           return "FNUM";
    case TK_STR:            return "STR";
    case TK_CHAR:           return "CHAR";
    case TK_IDENT:          return "IDENT";
    case TK_LPAREN:         return "(";
    case TK_RPAREN:         return ")";
    case TK_LBRACE:         return "{";
    case TK_RBRACE:         return "}";
    case TK_LBRACKET:       return "[";
    case TK_RBRACKET:       return "]";
    case TK_SEMI:           return ";";
    case TK_COLON:          return ":";
    case TK_COMMA:          return ",";
    case TK_DOT:            return ".";
    case TK_QUESTION:       return "?";
    case TK_TILDE:          return "~";
    case TK_EXCL:           return "!";
    case TK_PLUS:           return "+";
    case TK_MINUS:          return "-";
    case TK_STAR:           return "*";
    case TK_SLASH:          return "/";
    case TK_PERCENT:        return "%";
    case TK_AMP:            return "&";
    case TK_PIPE:           return "|";
    case TK_CARET:          return "^";
    case TK_ASSIGN:         return "=";
    case TK_LT:             return "<";
    case TK_GT:             return ">";
    case TK_HASH:           return "#";
    case TK_ARROW:          return "->";
    case TK_DOTSTAR:        return ".*";
    case TK_ARROWSTAR:      return "->*";
    case TK_INC:            return "++";
    case TK_DEC:            return "--";
    case TK_SHL:            return "<<";
    case TK_SHR:            return ">>";
    case TK_SPACESHIP:      return "<=>";
    case TK_LE:             return "<=";
    case TK_GE:             return ">=";
    case TK_EQ:             return "==";
    case TK_NE:             return "!=";
    case TK_LAND:           return "&&";
    case TK_LOR:            return "||";
    case TK_PLUS_ASSIGN:    return "+=";
    case TK_MINUS_ASSIGN:   return "-=";
    case TK_STAR_ASSIGN:    return "*=";
    case TK_SLASH_ASSIGN:   return "/=";
    case TK_PERCENT_ASSIGN: return "%=";
    case TK_AMP_ASSIGN:     return "&=";
    case TK_PIPE_ASSIGN:    return "|=";
    case TK_CARET_ASSIGN:   return "^=";
    case TK_SHL_ASSIGN:     return "<<=";
    case TK_SHR_ASSIGN:     return ">>=";
    case TK_SCOPE:          return "::";
    case TK_ELLIPSIS:       return "...";
    case TK_HASHHASH:       return "##";
    case TK_KW_ALIGNAS:     return "KW_alignas";
    case TK_KW_ALIGNOF:     return "KW_alignof";
    case TK_KW_ASM:         return "KW_asm";
    case TK_KW_AUTO:        return "KW_auto";
    case TK_KW_BOOL:        return "KW_bool";
    case TK_KW_BREAK:       return "KW_break";
    case TK_KW_CASE:        return "KW_case";
    case TK_KW_CATCH:       return "KW_catch";
    case TK_KW_CHAR:        return "KW_char";
    case TK_KW_CHAR16_T:    return "KW_char16_t";
    case TK_KW_CHAR32_T:    return "KW_char32_t";
    case TK_KW_CLASS:       return "KW_class";
    case TK_KW_CONST:       return "KW_const";
    case TK_KW_CONSTEXPR:   return "KW_constexpr";
    case TK_KW_CONST_CAST:  return "KW_const_cast";
    case TK_KW_CONTINUE:    return "KW_continue";
    case TK_KW_DECLTYPE:    return "KW_decltype";
    case TK_KW_DEFAULT:     return "KW_default";
    case TK_KW_DELETE:       return "KW_delete";
    case TK_KW_DO:          return "KW_do";
    case TK_KW_DOUBLE:      return "KW_double";
    case TK_KW_DYNAMIC_CAST: return "KW_dynamic_cast";
    case TK_KW_ELSE:        return "KW_else";
    case TK_KW_ENUM:        return "KW_enum";
    case TK_KW_EXPLICIT:    return "KW_explicit";
    case TK_KW_EXPORT:      return "KW_export";
    case TK_KW_EXTERN:      return "KW_extern";
    case TK_KW_FALSE:       return "KW_false";
    case TK_KW_FLOAT:       return "KW_float";
    case TK_KW_FOR:         return "KW_for";
    case TK_KW_FRIEND:      return "KW_friend";
    case TK_KW_GOTO:        return "KW_goto";
    case TK_KW_IF:          return "KW_if";
    case TK_KW_INLINE:      return "KW_inline";
    case TK_KW_INT:         return "KW_int";
    case TK_KW_LONG:        return "KW_long";
    case TK_KW_MUTABLE:     return "KW_mutable";
    case TK_KW_NAMESPACE:   return "KW_namespace";
    case TK_KW_NEW:         return "KW_new";
    case TK_KW_NOEXCEPT:    return "KW_noexcept";
    case TK_KW_NULLPTR:     return "KW_nullptr";
    case TK_KW_OPERATOR:    return "KW_operator";
    case TK_KW_PRIVATE:     return "KW_private";
    case TK_KW_PROTECTED:   return "KW_protected";
    case TK_KW_PUBLIC:      return "KW_public";
    case TK_KW_REGISTER:    return "KW_register";
    case TK_KW_REINTERPRET_CAST: return "KW_reinterpret_cast";
    case TK_KW_RETURN:      return "KW_return";
    case TK_KW_SHORT:       return "KW_short";
    case TK_KW_SIGNED:      return "KW_signed";
    case TK_KW_SIZEOF:      return "KW_sizeof";
    case TK_KW_STATIC:      return "KW_static";
    case TK_KW_STATIC_ASSERT: return "KW_static_assert";
    case TK_KW_STATIC_CAST: return "KW_static_cast";
    case TK_KW_STRUCT:      return "KW_struct";
    case TK_KW_SWITCH:      return "KW_switch";
    case TK_KW_TEMPLATE:    return "KW_template";
    case TK_KW_THIS:        return "KW_this";
    case TK_KW_THREAD_LOCAL: return "KW_thread_local";
    case TK_KW_THROW:       return "KW_throw";
    case TK_KW_TRUE:        return "KW_true";
    case TK_KW_TRY:         return "KW_try";
    case TK_KW_TYPEDEF:     return "KW_typedef";
    case TK_KW_TYPEID:      return "KW_typeid";
    case TK_KW_TYPENAME:    return "KW_typename";
    case TK_KW_UNION:       return "KW_union";
    case TK_KW_UNSIGNED:    return "KW_unsigned";
    case TK_KW_USING:       return "KW_using";
    case TK_KW_VIRTUAL:     return "KW_virtual";
    case TK_KW_VOID:        return "KW_void";
    case TK_KW_VOLATILE:    return "KW_volatile";
    case TK_KW_WCHAR_T:     return "KW_wchar_t";
    case TK_KW_WHILE:       return "KW_while";
    case TK_UNKNOWN:        return "UNKNOWN";
    case TK_EOF:            return "EOF";
    }
    return "???";
}

/* ------------------------------------------------------------------ */
/* Token helpers                                                       */
/* ------------------------------------------------------------------ */

bool token_equal(Token *tok, const char *s) {
    return tok->len == (int)strlen(s) &&
           memcmp(tok->loc, s, tok->len) == 0;
}

/* ------------------------------------------------------------------ */
/* Main tokenization loop                                              */
/* ------------------------------------------------------------------ */

/*
 * Growable token list for lexer construction.
 * After lexing, flattened into a contiguous TokenArray.
 */
typedef struct TokList TokList;
struct TokList {
    Token **ptrs;
    int len;
    int cap;
};

static void toklist_push(TokList *tl, Token *tok) {
    if (tl->len >= tl->cap) {
        int new_cap = tl->cap < 4 ? 8 : tl->cap * 2;
        Token **new_ptrs = xmalloc(new_cap * sizeof(Token *));
        if (tl->ptrs) {
            memcpy(new_ptrs, tl->ptrs, tl->len * sizeof(Token *));
            free(tl->ptrs);
        }
        tl->ptrs = new_ptrs;
        tl->cap = new_cap;
    }
    tl->ptrs[tl->len++] = tok;
}

TokenArray tokenize(File *file) {
    LexCtx ctx;
    ctx.file = file;
    ctx.p = file->contents;
    ctx.line = 1;
    ctx.col = 1;
    ctx.line_start = file->contents;

    TokList tl = {0};

    /* Terminates: each iteration consumes at least one byte (via a
     * read_* function advancing ctx.p), or breaks on NUL. The source
     * buffer is NUL-terminated, so ctx.p eventually reaches '\0'. */
    for (;;) {
        /* Track whitespace state before skipping */
        char *before_ws = ctx.p;
        skip_whitespace(&ctx);
        bool has_space = (ctx.p != before_ws);
        bool at_bol = (ctx.p == ctx.line_start);

        if (*ctx.p == '\0')
            break;

        Token *tok;

        /* String/char literal with optional encoding prefix.
         * Must check BEFORE identifier, since u8"..." starts with 'u'. */
        int enc;
        bool is_raw;
        char quote;
        int prefix_len = string_prefix_len(ctx.p, &enc, &is_raw, &quote);

        if (quote) {
            if (is_raw)
                tok = read_raw_string_literal(&ctx, prefix_len, enc);
            else if (quote == '"')
                tok = read_string_literal(&ctx, prefix_len, enc);
            else
                tok = read_char_literal(&ctx, prefix_len, enc);
        }
        /* Numeric literal */
        else if ((*ctx.p >= '0' && *ctx.p <= '9') ||
                 (*ctx.p == '.' && ctx.p[1] >= '0' && ctx.p[1] <= '9')) {
            tok = read_number(&ctx);
        }
        /* Identifier */
        else if (ident_start_len(ctx.p) > 0) {
            tok = read_ident(&ctx);
        }
        /* Punctuator */
        else {
            tok = read_punct(&ctx);
        }

        tok->has_space = has_space;
        tok->at_bol = at_bol;
        toklist_push(&tl, tok);
    }

    /* EOF token */
    Token *eof = new_token(TK_EOF, ctx.p, 0, &ctx);
    eof->at_bol = (ctx.p == ctx.line_start);
    toklist_push(&tl, eof);

    /* Two-phase keyword conversion — operate on the pointer list */
    for (int i = 0; i < tl.len; i++) {
        Token *t = tl.ptrs[i];
        if (t->kind != TK_IDENT)
            continue;
        TokenKind kw = lookup_keyword(t->loc, t->len);
        if (kw != TK_IDENT)
            t->kind = kw;
    }

    /* Flatten into a contiguous Token array */
    TokenArray result;
    result.len = tl.len;
    result.tokens = xmalloc(tl.len * sizeof(Token));
    for (int i = 0; i < tl.len; i++)
        result.tokens[i] = *tl.ptrs[i];

    /* Free the temporary pointer list and individual tokens */
    for (int i = 0; i < tl.len; i++)
        free(tl.ptrs[i]);
    free(tl.ptrs);

    return result;
}
