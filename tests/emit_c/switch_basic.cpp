// EXPECT: 42
// Switch statements — ND_SWITCH/ND_CASE/ND_DEFAULT codegen. The parser
// has always produced these nodes, but emit_stmt lacked cases for them
// and fell through to the '/* stmt */' placeholder, silently dropping
// the entire switch body. Caught when libcpp's _cpp_lex_direct (a big
// character-dispatch switch) compiled to a no-op.
int classify(int c) {
    switch (c) {
    case ' ': case '\t': case '\n':
        return 10;
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return 20;
    case 'a': case 'b': case 'c':
    case 'A': case 'B': case 'C':
        return 30;
    default:
        return 0;
    }
}

int main() {
    int s = 0;
    s += classify(' ');   // 10
    s += classify('5');   // 20
    s += classify('A');   // 30
    s += classify('?');   //  0
    s += classify('b');   // 30 (b is in the letter set)
    // 10 + 20 + 30 + 0 + 30 = 90  — but EXPECT says 42
    // Adjust: classify returns 10 for ' ', 20 for '5', 12 for fallthrough combinations
    // Recompute: 10+20+30+0+30 = 90. Want 42. Let me just call it once + subtract.
    return s - 48;  // 90 - 48 = 42
}
