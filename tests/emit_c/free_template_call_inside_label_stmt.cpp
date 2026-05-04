// EXPECT: 7
// A free function-template call placed inside a labeled statement.
// `goto done` jumps to a `done:` label whose statement contains the
// call; the instantiation pass's collect_from_node previously had no
// case for ND_LABEL and silently skipped its inner statement, so the
// callee never got an InstRequest, the template never instantiated,
// and codegen emitted the bare unmangled name. Pattern from gcc 4.8
// postreload.c reload_cse_simplify (and other functions): a `done:`
// label at the end of the function with a return that calls a free
// function template like vec_safe_length / EDGE_COUNT. Drove the
// remaining vec_safe_length cc1plus link errors after the chain-arg
// resolution fix.

template<typename T>
T grab(T *p) { return *p; }

int main() {
    int x = 7;
    int y = 0;
    if (x == 0) goto done;
    y = x;
done:
    return grab(&y);
}
