/*
 * unicode.c — UTF-8 decoding and C++ identifier character classification.
 *
 * Identifier characters per C++17 (N4659 §5.10):
 *   identifier-start: letter, underscore, or universal-character-name
 *                     from Annex E.1
 *   identifier-continue: identifier-start, digit, or UCN from Annex E.2
 *
 * For the bootstrap use case we use a pragmatic approximation: ASCII letters,
 * digits, underscore, and all codepoints >= 0x80 that fall in the Annex E
 * ranges. The tables below are derived from C++17 Annex E.
 */

#include "../sea-front.h"

/*
 * Decode one UTF-8 codepoint from p.
 * Returns the number of bytes consumed (1-4), or 0 on invalid sequence.
 * Sets *codepoint to the decoded value.
 */
int lex_decode_utf8(const char *p, uint32_t *codepoint) {
    unsigned char c = (unsigned char)*p;

    if (c < 0x80) {
        *codepoint = c;
        return 1;
    }

    int len;
    uint32_t cp;

    if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        *codepoint = 0;
        return 0;
    }

    for (int i = 1; i < len; i++) {
        unsigned char cont = (unsigned char)p[i];
        if ((cont & 0xC0) != 0x80) {
            *codepoint = 0;
            return 0;
        }
        cp = (cp << 6) | (cont & 0x3F);
    }

    /* Reject overlong encodings and surrogates */
    if ((len == 2 && cp < 0x80) ||
        (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) ||
        cp > 0x10FFFF) {
        *codepoint = 0;
        return 0;
    }

    *codepoint = cp;
    return len;
}

/*
 * C++17 Annex E identifier character ranges.
 *
 * These are {start, end} inclusive ranges. Annex E.1 defines characters
 * allowed at the start of an identifier; Annex E.2 defines additional
 * characters allowed in continuation positions.
 *
 * The ranges below cover the union of Latin, Greek, Cyrillic, CJK, and
 * other scripts used in C++17. This is a simplified but sufficient set
 * for bootstrapping GCC and Clang (whose sources use only ASCII identifiers),
 * but we include the full Annex E ranges for correctness.
 */

typedef struct { uint32_t lo, hi; } Range;

/* Annex E.1 — allowed at start of identifier (and continuation) */
static const Range annex_e1[] = {
    {0x00A8, 0x00A8}, {0x00AA, 0x00AA}, {0x00AD, 0x00AD}, {0x00AF, 0x00AF},
    {0x00B2, 0x00B5}, {0x00B7, 0x00BA}, {0x00BC, 0x00BE}, {0x00C0, 0x00D6},
    {0x00D8, 0x00F6}, {0x00F8, 0x00FF},
    {0x0100, 0x167F}, {0x1681, 0x180D}, {0x180F, 0x1FFF},
    {0x200B, 0x200D}, {0x202A, 0x202E}, {0x203F, 0x2040}, {0x2054, 0x2054},
    {0x2060, 0x206F},
    {0x2070, 0x218F}, {0x2460, 0x24FF}, {0x2776, 0x2793}, {0x2C00, 0x2DFF},
    {0x2E80, 0x2FFF},
    {0x3004, 0x3007}, {0x3021, 0x302F}, {0x3031, 0x303F},
    {0x3040, 0xD7FF},
    {0xF900, 0xFD3D}, {0xFD40, 0xFDCF}, {0xFDF0, 0xFE44},
    {0xFE47, 0xFFFD},
    {0x10000, 0x1FFFD}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
    {0x40000, 0x4FFFD}, {0x50000, 0x5FFFD}, {0x60000, 0x6FFFD},
    {0x70000, 0x7FFFD}, {0x80000, 0x8FFFD}, {0x90000, 0x9FFFD},
    {0xA0000, 0xAFFFD}, {0xB0000, 0xBFFFD}, {0xC0000, 0xCFFFD},
    {0xD0000, 0xDFFFD}, {0xE0000, 0xEFFFD},
};

/* Annex E.2 — additionally allowed in continuation positions */
static const Range annex_e2[] = {
    {0x0300, 0x036F}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0xFE20, 0xFE2F},
};

static bool in_ranges(uint32_t cp, const Range *ranges, int count) {
    /* Binary search */
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp < ranges[mid].lo)
            hi = mid - 1;
        else if (cp > ranges[mid].hi)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

bool lex_is_ident_start(uint32_t cp) {
    /* ASCII fast path */
    if (cp < 0x80)
        return (cp >= 'a' && cp <= 'z') ||
               (cp >= 'A' && cp <= 'Z') ||
               cp == '_';
    return in_ranges(cp, annex_e1,
                     (int)(sizeof(annex_e1) / sizeof(annex_e1[0])));
}

bool lex_is_ident_continue(uint32_t cp) {
    /* ASCII fast path */
    if (cp < 0x80)
        return (cp >= 'a' && cp <= 'z') ||
               (cp >= 'A' && cp <= 'Z') ||
               (cp >= '0' && cp <= '9') ||
               cp == '_';
    return in_ranges(cp, annex_e1,
                     (int)(sizeof(annex_e1) / sizeof(annex_e1[0]))) ||
           in_ranges(cp, annex_e2,
                     (int)(sizeof(annex_e2) / sizeof(annex_e2[0])));
}
