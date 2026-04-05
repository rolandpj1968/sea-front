/*
 * test_lex.c — Unit tests for the C++17 lexer.
 *
 * Calls tokenize() directly and asserts on token kinds, values, and locations.
 * Build: cc -std=c11 -g -o test_lex tests/test_lex.c src/util.c
 *        src/lex/tokenize.c src/lex/unicode.c -I src
 */

#define _POSIX_C_SOURCE 200809L
#include "sea-front.h"

static int tests_run = 0;
static int tests_failed = 0;

#define ASSERT(cond, ...) do { \
    tests_run++; \
    if (!(cond)) { \
        tests_failed++; \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
    } \
} while (0)

/* Helper: tokenize a string, return token list.
 * Allocates 32 bytes of NUL padding to match read_file() contract
 * (covers raw string delimiter memcmp lookahead). */
static Token *lex(const char *src) {
    int len = (int)strlen(src);
    File *f = xmalloc(sizeof(File));
    f->name = xstrdup("<test>");
    f->contents = xcalloc(1, len + 32 + 1);
    memcpy(f->contents, src, len);
    f->size = len;
    return tokenize(f);
}

/* Helper: get the nth token (0-indexed) */
static Token *nth(Token *tok, int n) {
    for (int i = 0; i < n && tok->kind != TK_EOF; i++)
        tok = tok->next;
    return tok;
}

/* ------------------------------------------------------------------ */
/* Test cases                                                          */
/* ------------------------------------------------------------------ */

static void test_keywords(void) {
    Token *t;

    t = lex("if else while for return");
    ASSERT(t->kind == TK_KW_IF, "expected KW_if, got %s", token_kind_name(t->kind));
    ASSERT(nth(t,1)->kind == TK_KW_ELSE, "expected KW_else");
    ASSERT(nth(t,2)->kind == TK_KW_WHILE, "expected KW_while");
    ASSERT(nth(t,3)->kind == TK_KW_FOR, "expected KW_for");
    ASSERT(nth(t,4)->kind == TK_KW_RETURN, "expected KW_return");
    ASSERT(nth(t,5)->kind == TK_EOF, "expected EOF");

    t = lex("class struct union enum namespace template typename");
    ASSERT(t->kind == TK_KW_CLASS, "expected KW_class");
    ASSERT(nth(t,1)->kind == TK_KW_STRUCT, "expected KW_struct");
    ASSERT(nth(t,2)->kind == TK_KW_UNION, "expected KW_union");
    ASSERT(nth(t,3)->kind == TK_KW_ENUM, "expected KW_enum");
    ASSERT(nth(t,4)->kind == TK_KW_NAMESPACE, "expected KW_namespace");
    ASSERT(nth(t,5)->kind == TK_KW_TEMPLATE, "expected KW_template");
    ASSERT(nth(t,6)->kind == TK_KW_TYPENAME, "expected KW_typename");

    /* override and final should be IDENT, not keywords */
    t = lex("override final");
    ASSERT(t->kind == TK_IDENT, "override should be IDENT");
    ASSERT(nth(t,1)->kind == TK_IDENT, "final should be IDENT");
}

static void test_alternative_tokens(void) {
    Token *t;

    t = lex("and or not bitand bitor xor compl");
    ASSERT(t->kind == TK_LAND, "and -> &&");
    ASSERT(nth(t,1)->kind == TK_LOR, "or -> ||");
    ASSERT(nth(t,2)->kind == TK_EXCL, "not -> !");
    ASSERT(nth(t,3)->kind == TK_AMP, "bitand -> &");
    ASSERT(nth(t,4)->kind == TK_PIPE, "bitor -> |");
    ASSERT(nth(t,5)->kind == TK_CARET, "xor -> ^");
    ASSERT(nth(t,6)->kind == TK_TILDE, "compl -> ~");

    t = lex("and_eq or_eq xor_eq not_eq");
    ASSERT(t->kind == TK_AMP_ASSIGN, "and_eq -> &=");
    ASSERT(nth(t,1)->kind == TK_PIPE_ASSIGN, "or_eq -> |=");
    ASSERT(nth(t,2)->kind == TK_CARET_ASSIGN, "xor_eq -> ^=");
    ASSERT(nth(t,3)->kind == TK_NE, "not_eq -> !=");
}

static void test_operators(void) {
    Token *t;

    /* Multi-character operators */
    t = lex("-> .* ->* ++ -- << >> <=> <= >= == != && ||");
    ASSERT(t->kind == TK_ARROW, "->");
    ASSERT(nth(t,1)->kind == TK_DOTSTAR, ".*");
    ASSERT(nth(t,2)->kind == TK_ARROWSTAR, "->*");
    ASSERT(nth(t,3)->kind == TK_INC, "++");
    ASSERT(nth(t,4)->kind == TK_DEC, "--");
    ASSERT(nth(t,5)->kind == TK_SHL, "<<");
    ASSERT(nth(t,6)->kind == TK_SHR, ">>");
    ASSERT(nth(t,7)->kind == TK_SPACESHIP, "<=>");
    ASSERT(nth(t,8)->kind == TK_LE, "<=");
    ASSERT(nth(t,9)->kind == TK_GE, ">=");
    ASSERT(nth(t,10)->kind == TK_EQ, "==");
    ASSERT(nth(t,11)->kind == TK_NE, "!=");
    ASSERT(nth(t,12)->kind == TK_LAND, "&&");
    ASSERT(nth(t,13)->kind == TK_LOR, "||");

    /* Assignment operators */
    t = lex("+= -= *= /= %= &= |= ^= <<= >>=");
    ASSERT(t->kind == TK_PLUS_ASSIGN, "+=");
    ASSERT(nth(t,1)->kind == TK_MINUS_ASSIGN, "-=");
    ASSERT(nth(t,2)->kind == TK_STAR_ASSIGN, "*=");
    ASSERT(nth(t,3)->kind == TK_SLASH_ASSIGN, "/=");
    ASSERT(nth(t,4)->kind == TK_PERCENT_ASSIGN, "%%=");
    ASSERT(nth(t,5)->kind == TK_AMP_ASSIGN, "&=");
    ASSERT(nth(t,6)->kind == TK_PIPE_ASSIGN, "|=");
    ASSERT(nth(t,7)->kind == TK_CARET_ASSIGN, "^=");
    ASSERT(nth(t,8)->kind == TK_SHL_ASSIGN, "<<=");
    ASSERT(nth(t,9)->kind == TK_SHR_ASSIGN, ">>=");

    /* Scope and ellipsis */
    t = lex(":: ...");
    ASSERT(t->kind == TK_SCOPE, "::");
    ASSERT(nth(t,1)->kind == TK_ELLIPSIS, "...");
}

static void test_digraphs(void) {
    Token *t;

    t = lex("<% %> <: :>");
    ASSERT(t->kind == TK_LBRACE, "digraph <%%");
    ASSERT(nth(t,1)->kind == TK_RBRACE, "digraph %%>");
    ASSERT(nth(t,2)->kind == TK_LBRACKET, "digraph <:");
    ASSERT(nth(t,3)->kind == TK_RBRACKET, "digraph :>");

    /* <:: exception: <:: not followed by : or > => < :: */
    t = lex("<::foo");
    ASSERT(t->kind == TK_LT, "<:: exception: first token is <");
    ASSERT(nth(t,1)->kind == TK_SCOPE, "<:: exception: second token is ::");
    ASSERT(nth(t,2)->kind == TK_IDENT, "<:: exception: third token is foo");

    /* <:: followed by : => <: (digraph [) then : */
    t = lex("<:::");
    ASSERT(t->kind == TK_LBRACKET, "<::: first is [");
    ASSERT(nth(t,1)->kind == TK_SCOPE, "<::: then ::");

    /* <:: followed by > => <: (digraph [) then :> (digraph ]) */
    t = lex("<::>");
    ASSERT(t->kind == TK_LBRACKET, "<::> first is [");
    ASSERT(nth(t,1)->kind == TK_RBRACKET, "<::> second is ]");
}

static void test_integers(void) {
    Token *t;

    /* Decimal */
    t = lex("0 42 1000");
    ASSERT(t->kind == TK_NUM && t->ival == 0, "0");
    ASSERT(nth(t,1)->kind == TK_NUM && nth(t,1)->ival == 42, "42");
    ASSERT(nth(t,2)->kind == TK_NUM && nth(t,2)->ival == 1000, "1000");

    /* Hex */
    t = lex("0xFF 0X10");
    ASSERT(t->ival == 0xFF, "0xFF = %lld", (long long)t->ival);
    ASSERT(nth(t,1)->ival == 0x10, "0X10");

    /* Octal */
    t = lex("077");
    ASSERT(t->ival == 077, "077 = %lld", (long long)t->ival);

    /* Binary */
    t = lex("0b1010 0B11");
    ASSERT(t->ival == 10, "0b1010 = %lld", (long long)t->ival);
    ASSERT(nth(t,1)->ival == 3, "0B11 = %lld", (long long)nth(t,1)->ival);

    /* Digit separators */
    t = lex("1'000'000");
    ASSERT(t->kind == TK_NUM, "digit sep is NUM");
    ASSERT(t->ival == 1000000, "1'000'000 = %lld", (long long)t->ival);

    t = lex("0xFF'FF");
    ASSERT(t->ival == 0xFFFF, "0xFF'FF = %lld", (long long)t->ival);

    t = lex("0b1010'1100");
    ASSERT(t->ival == 0xAC, "0b1010'1100 = %lld", (long long)t->ival);

    /* Integer suffixes */
    t = lex("42u 42L 42ull 42ULL");
    ASSERT(t->kind == TK_NUM && t->ival == 42, "42u");
    ASSERT(nth(t,1)->kind == TK_NUM && nth(t,1)->ival == 42, "42L");
    ASSERT(nth(t,2)->kind == TK_NUM && nth(t,2)->ival == 42, "42ull");
    ASSERT(nth(t,3)->kind == TK_NUM && nth(t,3)->ival == 42, "42ULL");
}

static void test_floats(void) {
    Token *t;

    t = lex("3.14");
    ASSERT(t->kind == TK_FNUM, "3.14 is FNUM");
    ASSERT(t->fval > 3.13 && t->fval < 3.15, "3.14 value");

    t = lex("1.0e5");
    ASSERT(t->kind == TK_FNUM, "1.0e5 is FNUM");
    ASSERT(t->fval == 1.0e5, "1.0e5 value");

    t = lex("1.");
    ASSERT(t->kind == TK_FNUM, "1. is FNUM");
    ASSERT(t->fval == 1.0, "1. value");

    t = lex(".5");
    ASSERT(t->kind == TK_FNUM, ".5 is FNUM");
    ASSERT(t->fval == 0.5, ".5 value");

    t = lex("1.0f 1.0L");
    ASSERT(t->kind == TK_FNUM, "1.0f is FNUM");
    ASSERT(nth(t,1)->kind == TK_FNUM, "1.0L is FNUM");

    /* Hex float */
    t = lex("0x1.0p10");
    ASSERT(t->kind == TK_FNUM, "hex float is FNUM");
    ASSERT(t->fval == 1024.0, "0x1.0p10 = %g", t->fval);
}

static void test_strings(void) {
    Token *t;

    /* Plain string */
    t = lex("\"hello\"");
    ASSERT(t->kind == TK_STR, "string literal");
    ASSERT(t->enc == ENC_NONE, "no encoding prefix");

    /* Encoding prefixes */
    t = lex("u8\"x\" u\"x\" U\"x\" L\"x\"");
    ASSERT(t->enc == ENC_U8, "u8 prefix");
    ASSERT(nth(t,1)->enc == ENC_LITTLE_U, "u prefix");
    ASSERT(nth(t,2)->enc == ENC_BIG_U, "U prefix");
    ASSERT(nth(t,3)->enc == ENC_L, "L prefix");

    /* Raw string */
    t = lex("R\"(hello)\"");
    ASSERT(t->kind == TK_STR, "raw string is STR");
    ASSERT(t->is_raw, "raw flag set");

    /* Raw string with delimiter */
    t = lex("R\"delim(raw content)delim\"");
    ASSERT(t->kind == TK_STR && t->is_raw, "raw with delim");

    /* Raw string with newline */
    t = lex("R\"(line1\nline2)\"");
    ASSERT(t->kind == TK_STR && t->is_raw, "raw with newline");

    /* Prefixed raw strings */
    t = lex("u8R\"(x)\" LR\"(x)\" uR\"(x)\" UR\"(x)\"");
    ASSERT(t->enc == ENC_U8 && t->is_raw, "u8R");
    ASSERT(nth(t,1)->enc == ENC_L && nth(t,1)->is_raw, "LR");
    ASSERT(nth(t,2)->enc == ENC_LITTLE_U && nth(t,2)->is_raw, "uR");
    ASSERT(nth(t,3)->enc == ENC_BIG_U && nth(t,3)->is_raw, "UR");
}

static void test_chars(void) {
    Token *t;

    t = lex("'a'");
    ASSERT(t->kind == TK_CHAR, "char literal");
    ASSERT(t->ival == 'a', "'a' = %lld", (long long)t->ival);

    /* Escape sequences */
    t = lex("'\\n' '\\t' '\\0' '\\\\' '\\''");
    ASSERT(t->ival == '\n', "'\\n'");
    ASSERT(nth(t,1)->ival == '\t', "'\\t'");
    ASSERT(nth(t,2)->ival == '\0', "'\\0'");
    ASSERT(nth(t,3)->ival == '\\', "'\\\\'");
    ASSERT(nth(t,4)->ival == '\'', "'\\''");

    /* Hex escape */
    t = lex("'\\x41'");
    ASSERT(t->ival == 0x41, "'\\x41' = %lld", (long long)t->ival);

    /* Octal escape */
    t = lex("'\\101'");
    ASSERT(t->ival == 65, "'\\101' = %lld", (long long)t->ival);

    /* Encoding prefixes */
    t = lex("u'x' U'x' L'x' u8'x'");
    ASSERT(t->enc == ENC_LITTLE_U, "u char");
    ASSERT(nth(t,1)->enc == ENC_BIG_U, "U char");
    ASSERT(nth(t,2)->enc == ENC_L, "L char");
    ASSERT(nth(t,3)->enc == ENC_U8, "u8 char");
}

static void test_udl(void) {
    Token *t;

    /* Numeric UDL */
    t = lex("42_km");
    ASSERT(t->kind == TK_NUM, "42_km is NUM");
    ASSERT(t->ud_suffix != NULL, "has UDL suffix");
    ASSERT(t->ud_suffix_len == 3, "suffix len = 3");
    ASSERT(memcmp(t->ud_suffix, "_km", 3) == 0, "suffix = _km");

    /* String UDL */
    t = lex("\"hello\"_s");
    ASSERT(t->kind == TK_STR, "string UDL is STR");
    ASSERT(t->ud_suffix != NULL, "string has UDL");
    ASSERT(memcmp(t->ud_suffix, "_s", 2) == 0, "suffix = _s");

    /* Char UDL */
    t = lex("'x'_c");
    ASSERT(t->kind == TK_CHAR, "char UDL is CHAR");
    ASSERT(t->ud_suffix != NULL, "char has UDL");
}

static void test_edge_cases(void) {
    Token *t;

    /* x+++++y => x ++ ++ + y (maximal munch) */
    t = lex("x+++++y");
    ASSERT(t->kind == TK_IDENT, "x");
    ASSERT(nth(t,1)->kind == TK_INC, "first ++");
    ASSERT(nth(t,2)->kind == TK_INC, "second ++");
    ASSERT(nth(t,3)->kind == TK_PLUS, "+");
    ASSERT(nth(t,4)->kind == TK_IDENT, "y");

    /* >> always lexed as single TK_SHR (parser splits for templates) */
    t = lex(">>");
    ASSERT(t->kind == TK_SHR, ">> is SHR");

    /* Digit separator vs char literal: 1'x' is 1 then 'x' */
    t = lex("1 'x'");
    ASSERT(t->kind == TK_NUM && t->ival == 1, "1");
    ASSERT(nth(t,1)->kind == TK_CHAR, "'x' is CHAR");

    /* Comments (defensive — input should be preprocessed) */
    t = lex("a /* block */ b // line\nc");
    ASSERT(t->kind == TK_IDENT, "a");
    ASSERT(nth(t,1)->kind == TK_IDENT, "b after block comment");
    ASSERT(nth(t,2)->kind == TK_IDENT, "c after line comment");
}

static void test_locations(void) {
    Token *t;

    t = lex("a b\nc d");
    ASSERT(t->line == 1 && t->col == 1, "a at 1:1");
    ASSERT(nth(t,1)->line == 1 && nth(t,1)->col == 3, "b at 1:3");
    ASSERT(nth(t,2)->line == 2 && nth(t,2)->col == 1, "c at 2:1");
    ASSERT(nth(t,3)->line == 2 && nth(t,3)->col == 3, "d at 2:3");

    /* Multiline raw string: tokens after it should have correct location */
    t = lex("R\"(a\nb)\" x");
    ASSERT(t->kind == TK_STR && t->line == 1, "raw string starts at line 1");
    ASSERT(nth(t,1)->kind == TK_IDENT && nth(t,1)->line == 2,
           "x after raw string at line 2, got %d", nth(t,1)->line);
}

/* ------------------------------------------------------------------ */

int main(void) {
    test_keywords();
    test_alternative_tokens();
    test_operators();
    test_digraphs();
    test_integers();
    test_floats();
    test_strings();
    test_chars();
    test_udl();
    test_edge_cases();
    test_locations();

    printf("\n%d tests, %d failures\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
