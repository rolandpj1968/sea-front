// EXPECT: 0
// Two more pack-expansion sites that need pack-aware cloning:
//
//   - brace-init-list `{args..., -1}` inside a variadic template
//     body. With an empty pack, expands to `{-1}` — a one-element
//     array initializer. With a non-empty pack, expands as
//     expected: `{a, b, c, -1}`.
//
//   - mem-initializer `: m(args...)` in a templated constructor.
//     With a one-element pack, expands to `: m(arg)`.
//
// Mirrors the earlier new-expression and direct-init declarator
// slices in commits fb8c7c3 + e9fcdeb. N4659 §17.5.3.4
// [temp.variadic.expand] for the pack expansion. N4659 §11.6.4
// [dcl.init.list] + §15.6.2 [class.base.init] for the underlying
// init forms.

extern "C" void abort(void);

struct S {
    int a;
    template <class... Args>
    S(Args... args) : a(args...) {}
};

template <class... Args>
int sum(Args... args) {
    int x[] = { args..., -1 };
    int s = 0;
    for (int i = 0; x[i] != -1; i++) s += x[i];
    return s;
}

int main() {
    if (sum() != 0) abort();
    if (sum(1, 2, 3) != 6) abort();
    S s5(5);
    if (s5.a != 5) abort();
    return 0;
}
