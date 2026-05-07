// EXPECT: 99
// Default-argument expansion in overload resolution and at the
// call site — N4659 §11.3.6 [dcl.fct.default] + §16.3.1.4
// [over.match.viable]/2: a candidate is viable when nargs equals
// nparams OR when nargs < nparams and every excess param has a
// default. The call site fills the trailing slots from
// param.default_value.
//
// Real-world hit: gcc 14 libcpp's rich_location declared as
//   rich_location(line_maps*, location_t, const range_label* = NULL);
// is called from lex.cc / directives.cc / errors.cc with only the
// first two args. Without default-arg expansion the call falls
// through "no matching overload (2 args)" because the only ctor
// has 3 params. With it, the 3-arg ctor is viable and the call
// emits with the default substituted in.
//
// Two overloads to ensure overload-resolution is exercised, not
// just the one-candidate short-circuit.

struct Loc {
    int x, y, z;
    Loc(int a, int b, int c = 99) : x(a), y(b), z(c) {}
    Loc(const Loc &) = delete;       // makes the resolver pick
                                      // between candidates
};

int main() {
    Loc l(1, 2);   // 2-arg call → 3-arg ctor with default for c
    return l.z;    // 99
}
