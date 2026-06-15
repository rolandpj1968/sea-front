// EXPECT: 0
// Non-Static Data Member Initializer (NSDMI) — N4659 §12.6.2/9
// [class.base.init]. When an aggregate-init declaration or
// functional-cast brace-init `T{a, b, ...}` omits trailing
// members, sea-front must fill in their NSDMI default expressions.
//
// Three slice-fixes meet here:
//   - parse_primary's `T{...}` functional cast now captures the
//     brace-init as an ND_INIT_LIST (was: skip-balanced + drop).
//   - emit ND_CAST + non-empty ND_INIT_LIST lowers to a stmt-expr
//     with per-member assignment, falling back to each member's
//     `var_decl.init` (the NSDMI) when no caller arg is present.
//   - emit's aggregate-init var-decl path (`Y y { 1 };` with NSDMI
//     for omitted `n`) uses the same per-member assignment shape.
//
// NSDMI refs to other members of the same class use the implicit-
// this override hook (`g_implicit_this_override`) to retarget
// `this->name` to either `__sf_il_tmp.name` (inside the cast stmt-
// expr) or `y.name` (inside the var-decl form).
//
// Critically, EXPLICIT args supplied by the caller keep the
// SURROUNDING context's override — `Y{2, i}` inside Y's NSDMI for
// `n` uses the OUTER `y.i`, not the inner temp's i.
//
// Reduced from g++.dg/cpp1y/pr79937-3.C and pr79937-4.C.

extern "C" void abort(void);

struct X {
    unsigned i;
    unsigned n = i;     // NSDMI: n = i
    unsigned m = i;     // NSDMI: m = i
};

X bar(X x) { return x; }

struct Y {
    unsigned i;
    unsigned n = bar(X{i + 1, 0, 0}).i;  // NSDMI refers to outer i
};

int main() {
    // X{1, X{2}.n} should construct outer X{i=1, n=X{2}.n=2, m=NSDMI(=1)}.
    // X{2}.n: X{i=2, n=NSDMI(=2), m=NSDMI(=2)}, then .n is 2.
    X x { 1, X{2}.n };
    if (x.i != 1) abort();
    if (x.n != 2) abort();
    if (x.m != 1) abort();  // NSDMI fill for omitted m, refers to x.i

    // Y y{5} — i=5, n=NSDMI: bar(X{i+1, 0, 0}).i where i refers to y.i = 5.
    // bar(X{6, 0, 0}).i = 6.
    Y y { 5 };
    if (y.i != 5) abort();
    if (y.n != 6) abort();
    return 0;
}
