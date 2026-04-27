// EXPECT: 7
// A const ref-param materialised temp inside a switch case body must
// have its initializer execute when the case label is jumped to.
// Sea-front previously hoisted the temp's declaration to the outer
// switch-body scope, BEFORE the case label — so a switch jump to
// that case skipped the initializer (C99 §6.8.6 — control transfer
// past an automatic variable's declaration leaves the variable's
// value indeterminate, even though its lifetime spans the whole
// enclosing block).
//
// Concrete: gcc 4.8 genextract's walk_rtx pushed
// 'VEC_char_to_string(acc.pathstr)' (an rvalue char*) into
// acc.duplocs inside a MATCH_DUP / MATCH_OP_DUP case. The temp
// holding the malloc'd char* was hoisted ABOVE the case label,
// got never-initialised stack storage, and the resulting vec
// stored an uninitialised pointer that happened to land in the
// .text segment. genextract aborted in print_path on
// machine-code bytes.
//
// Fix: emit_stmt for ND_CASE/ND_DEFAULT wraps the case body in a
// brace block; hoist runs *inside* that block. Every entry to the
// case label flows into the brace, so the temp's initializer
// always executes before any use.
template<typename T>
struct Box {
    T data;
    void take(const T &x) { data = x; }
};

static int caller(Box<long> &b, int which) {
    switch (which) {
    case 0:
        b.take(1);   // forces a 'int → const long &' temp materialisation
        return 1;
    case 1:
        b.take(7);   // same shape; second case to ensure jumps land here
        return 2;
    }
    return 99;
}

int main() {
    Box<long> b;
    b.data = 0;
    caller(b, 1);
    return (b.data == 7L) ? 7 : 1;
}
