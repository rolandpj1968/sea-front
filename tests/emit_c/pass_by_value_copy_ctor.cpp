// EXPECT: 0
// Class arg passed by value invokes the user copy ctor — N4659
// §11.4.5 [class.copy.ctor] + §16.3.1.4 [over.match.copy].
//
// Regression for the gated copy-ctor lowering (a45fe5a + cfa82ad).
// The gate fires only when the class has no destructor (because the
// copied param's dtor wouldn't run at the call's pseudo-scope-end
// and would imbalance with-dtor classes — see opt/dtor1 in g++.dg).
//
// Two cases:
//   - NoDtor: copy-ctor MUST fire on every pass-by-value.
//   - WithDtor: bitwise fall-back. Whether we fire copy or not, the
//     dtor count must MATCH so a future ungating doesn't silently
//     unbalance the ctor/dtor accounting.

int no_dtor_copies = 0;
int with_dtor_copies = 0;
int with_dtor_dtors = 0;

struct NoDtor {
    int v;
    NoDtor() : v(0) {}
    NoDtor(const NoDtor &o) : v(o.v) { ++no_dtor_copies; }
};

struct WithDtor {
    int v;
    WithDtor() : v(0) {}
    WithDtor(const WithDtor &o) : v(o.v) { ++with_dtor_copies; }
    ~WithDtor() { ++with_dtor_dtors; }
};

void take_no_dtor(NoDtor) {}
void take_with_dtor(WithDtor) {}

int main() {
    NoDtor a;
    take_no_dtor(a);
    if (no_dtor_copies != 1) return 1;

    take_no_dtor(a);
    if (no_dtor_copies != 2) return 2;

    WithDtor b;
    int before_copies = with_dtor_copies;
    int before_dtors  = with_dtor_dtors;
    take_with_dtor(b);
    int after_copies = with_dtor_copies;
    int after_dtors  = with_dtor_dtors;
    // Whether the path is bitwise (0 / 0) or full copy + dtor (1 / 1),
    // the deltas must match. A regression that emits copy-ctor without
    // a balancing dtor would land here as (1 / 0).
    if (after_copies - before_copies != after_dtors - before_dtors)
        return 3;

    return 0;
}
